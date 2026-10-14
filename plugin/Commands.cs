using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using BepInEx;
using BepInEx.Logging;
using HarmonyLib;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 调试指令通道：往游戏目录写 `BepInEx/wellcooked-command.txt`，插件 1 秒内执行并删掉文件。
    ///
    /// 为什么要它：调试时人在终端、游戏在后台，不可能每次都手动点菜单进关卡，
    /// 也不该为了看一眼状态去截图。有了这个通道，"重进关""看某类型有哪些方法"
    /// "看暂停状态"都能一条命令做完，日志里留证据。
    ///
    /// 支持的指令（一行一条，`#` 开头是注释）：
    ///   load=&lt;场景名&gt;   直接加载关卡场景（如 s_sushi_1_1）
    ///   restart=1        重新加载当前场景（相当于原地重进这一关）
    ///   continue=1       标题画面的"继续游戏"：加载存档档位，之后 load 才是正常进关
    ///   engage=1         标题画面：直接替我们的占位完成"加入"（等价于按 A）
    ///   dump=&lt;类型名&gt;    把这个类型的所有方法签名打到日志
    ///   pause=1          打当前暂停状态（TimeManager 各层）
    /// </summary>
    public static class Commands
    {
        private static ManualLogSource _log;
        private static float _nextPoll;

        public static void Install(ManualLogSource log)
        {
            _log = log;
        }

        public static void Tick()
        {
            var now = Time.realtimeSinceStartup;
            if (now < _nextPoll)
            {
                return;
            }

            _nextPoll = now + 1f;

            string path = Path.Combine(Paths.BepInExRootPath, "wellcooked-command.txt");
            if (!File.Exists(path))
            {
                return;
            }

            string text;
            try
            {
                text = File.ReadAllText(path);
            }
            catch (Exception e)
            {
                _log.LogWarning("指令: 读不到 " + path + " " + e.Message);
                return;
            }

            try
            {
                File.Delete(path);
            }
            catch (Exception e)
            {
                _log.LogWarning("指令: 删不掉 " + path + " " + e.Message);
            }

            foreach (var raw in text.Split('\n'))
            {
                var line = raw.Trim();
                if (line.Length == 0 || line[0] == '#')
                {
                    continue;
                }

                int eq = line.IndexOf('=');
                string key = (eq < 0 ? line : line.Substring(0, eq)).Trim();
                string value = eq < 0 ? "" : line.Substring(eq + 1).Trim();
                try
                {
                    Run(key, value);
                }
                catch (Exception e)
                {
                    _log.LogError("指令 " + line + " 失败: " + e);
                }
            }
        }

        private static void Run(string key, string value)
        {
            switch (key)
            {
                case "load":
                    LoadLevel(value);
                    break;
                case "restart":
                    LoadLevel(UnityEngine.SceneManagement.SceneManager.GetActiveScene().name);
                    break;
                case "engage":
                    Engage();
                    break;
                case "continue":
                    ContinueFromTitle();
                    break;
                case "session":
                    StartSession(value);
                    break;
                case "dump":
                    DumpType(value);
                    break;
                case "pause":
                    DumpPause();
                    break;
                default:
                    _log.LogWarning("指令: 不认识 " + key);
                    break;
            }
        }

        private static void LoadLevel(string scene)
        {
            if (scene.Length == 0)
            {
                _log.LogWarning("指令: load 少了场景名");
                return;
            }

            var type = AccessTools.TypeByName("GameUtils");
            if (type == null)
            {
                _log.LogWarning("指令: 找不到 GameUtils");
                return;
            }

            foreach (var method in type.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static))
            {
                if (method.Name != "LoadLevel")
                {
                    continue;
                }

                var parameters = method.GetParameters();
                _log.LogInfo("指令: GameUtils.LoadLevel(" + Describe(parameters) + ")");
                if (parameters.Length != 1)
                {
                    continue;
                }

                object argument = Convert(scene, parameters[0].ParameterType);
                if (argument == null)
                {
                    continue;
                }

                _log.LogInfo("指令: 加载 " + scene + " → " + argument);
                method.Invoke(null, new[] { argument });
                _log.LogInfo("指令: 加载调用返回（当前场景 " + UnityEngine.SceneManagement.SceneManager.GetActiveScene().name + "）");
                return;
            }

            _log.LogWarning("指令: GameUtils 上没有单参数的 LoadLevel");
        }

        /// <summary>
        /// 标题画面按 A 的效果：游戏内部是"某个手柄完成加入" → StartScreenFlow.OnEngagementFinished。
        /// 我们的虚拟手柄已经占住了玩家位，所以直接把这个加入结果告诉标题画面。
        /// 只调 CheckForEngagement 不够：它在等一次新的按键加入，而我们早就占好位了。
        /// </summary>
        private static void Engage()
        {
            var type = AccessTools.TypeByName("StartScreenFlow");
            if (type == null)
            {
                _log.LogWarning("指令: 找不到 StartScreenFlow");
                return;
            }

            var instance = type.GetProperty("Instance", BindingFlags.Public | BindingFlags.Static);
            object flow = instance != null ? instance.GetValue(null, null) : null;
            if (flow == null)
            {
                _log.LogWarning("指令: StartScreenFlow 没有实例（现在不在标题画面？）");
                return;
            }

            object user = FirstEngagedUser();
            if (user == null)
            {
                _log.LogWarning("指令: 还没有任何已加入的手柄用户，没法完成加入");
                return;
            }

            var finished = AccessTools.Method(type, "OnEngagementFinished");
            if (finished == null)
            {
                _log.LogWarning("指令: 找不到 OnEngagementFinished");
                return;
            }

            _log.LogInfo("指令: 用 " + user.GetType().Name + " 完成加入（等价按 A）");
            finished.Invoke(flow, new[] { user });
        }

        private static object FindPlayerManager()
        {
            var type = AccessTools.TypeByName("PCPlayerManager");
            if (type == null)
            {
                return null;
            }

            var found = UnityEngine.Object.FindObjectsOfType(type);
            return found.Length > 0 ? found[0] : null;
        }

        private static object FirstEngagedUser()
        {
            object manager = FindPlayerManager();
            if (manager == null)
            {
                return null;
            }

            var slotType = AccessTools.TypeByName("EngagementSlot");
            var get = AccessTools.Method(manager.GetType(), "GetUser", new[] { slotType });
            if (get == null)
            {
                _log.LogWarning("指令: PlayerManager 上没有 GetUser(EngagementSlot)");
                return null;
            }

            for (int slot = 0; slot < 4; slot++)
            {
                object user = get.Invoke(manager, new[] { Enum.ToObject(slotType, slot) });
                if (user != null)
                {
                    _log.LogInfo("指令: 玩家位 " + (slot + 1) + " 已加入");
                    return user;
                }
            }

            return null;
        }

        /// <summary>开一个空会话（不带存档）：T17FrontendFlow.StartEmptySession(GameType, int)。</summary>
        private static void StartSession(string value)
        {
            var type = AccessTools.TypeByName("T17FrontendFlow");
            if (type == null)
            {
                _log.LogWarning("指令: 找不到 T17FrontendFlow");
                return;
            }

            var instance = type.GetProperty("Instance", BindingFlags.Public | BindingFlags.Static);
            object flow = instance != null ? instance.GetValue(null, null) : null;
            if (flow == null)
            {
                _log.LogWarning("指令: T17FrontendFlow 没有实例");
                return;
            }

            var method = AccessTools.Method(type, "StartEmptySession");
            if (method == null)
            {
                _log.LogWarning("指令: 找不到 StartEmptySession");
                return;
            }

            var parameters = method.GetParameters();
            var args = new object[parameters.Length];
            for (int i = 0; i < parameters.Length; i++)
            {
                if (parameters[i].ParameterType.IsEnum)
                {
                    var names = Enum.GetNames(parameters[i].ParameterType);
                    args[i] = Enum.Parse(parameters[i].ParameterType,
                        value.Length > 0 && Array.IndexOf(names, value) >= 0 ? value : names[0]);
                }
                else
                {
                    args[i] = System.Convert.ChangeType("0", parameters[i].ParameterType);
                }
            }

            _log.LogInfo("指令: StartEmptySession(" + args[0] + ", " + args[1] + ")");
            var session = method.Invoke(flow, args);
            _log.LogInfo("指令: 会话=" + (session == null ? "null" : session.GetType().Name));
        }

        /// <summary>
        /// 标题画面的"继续游戏"。必须走这一步：直接用 GameUtils.LoadLevel 加载关卡场景，
        /// 关卡流程不会起来（实测订单为空、得分 None、计时全 0，厨师也不受控）。
        /// StartScreenFlow.StartLoadProfile 是个协程，得拿 IEnumerator 交给 StartCoroutine 跑。
        /// </summary>
        private static void ContinueFromTitle()
        {
            var type = AccessTools.TypeByName("StartScreenFlow");
            if (type == null)
            {
                _log.LogWarning("继续游戏: 找不到 StartScreenFlow");
                return;
            }

            var property = type.GetProperty("Instance", BindingFlags.Public | BindingFlags.Static);
            object flow = property != null ? property.GetValue(null, null) : null;
            if (flow == null)
            {
                _log.LogWarning("继续游戏: 没有 StartScreenFlow 实例（现在不在标题画面）");
                return;
            }

            _log.LogInfo("继续游戏: 当前场景 " + UnityEngine.SceneManagement.SceneManager.GetActiveScene().name);
            var check = AccessTools.Method(type, "CheckForEngagement", Type.EmptyTypes);
            if (check != null)
            {
                check.Invoke(flow, null);
                _log.LogInfo("继续游戏: 已触发加入检查");
            }

            var loadProfile = AccessTools.Method(type, "StartLoadProfile");
            if (loadProfile == null)
            {
                _log.LogWarning("继续游戏: 找不到 StartLoadProfile");
                return;
            }

            object callback = MakeCallback(loadProfile.GetParameters()[0].ParameterType);
            object result = loadProfile.Invoke(flow, new[] { callback });
            var routine = result as System.Collections.IEnumerator;
            if (routine == null)
            {
                _log.LogWarning("继续游戏: StartLoadProfile 没返回协程，拿到 " + (result == null ? "null" : result.GetType().Name));
                return;
            }

            var behaviour = flow as MonoBehaviour;
            if (behaviour == null)
            {
                _log.LogWarning("继续游戏: StartScreenFlow 不是 MonoBehaviour，协程起不来");
                return;
            }

            behaviour.StartCoroutine(routine);
            _log.LogInfo("继续游戏: 协程已启动（等场景切换）");
        }

        /// <summary>回调类型未知，按它的委托签名现场造一个空实现。</summary>
        private static object MakeCallback(Type callbackType)
        {
            try
            {
                var method = typeof(Commands).GetMethod(nameof(Noop), BindingFlags.Static | BindingFlags.NonPublic);
                return Delegate.CreateDelegate(callbackType, method);
            }
            catch (Exception e)
            {
                _log.LogWarning("继续游戏: 造不出回调 " + callbackType.Name + ": " + e.Message);
                return null;
            }
        }

        private static void Noop()
        {
        }

        private static void DumpType(string name)
        {
            var type = AccessTools.TypeByName(name);
            if (type == null)
            {
                _log.LogWarning("指令: 找不到类型 " + name);
                return;
            }

            if (type.IsEnum)
            {
                _log.LogInfo("指令: 枚举 " + type.FullName + " = " + string.Join(", ", Enum.GetNames(type)));
                return;
            }

            _log.LogInfo("指令: 类型 " + type.FullName + " 的方法：");
            foreach (var method in type.GetMethods(BindingFlags.Public | BindingFlags.NonPublic
                                                   | BindingFlags.Static | BindingFlags.Instance
                                                   | BindingFlags.DeclaredOnly))
            {
                _log.LogInfo("     " + method.Name + "(" + Describe(method.GetParameters()) + ") → "
                             + (method.ReturnType == null ? "?" : method.ReturnType.Name));
            }
        }

        private static void DumpPause()
        {
            var type = AccessTools.TypeByName("TimeManager");
            if (type == null)
            {
                _log.LogWarning("指令: 找不到 TimeManager");
                return;
            }

            var field = type.GetField("s_timeManager", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
            object manager = field != null ? field.GetValue(null) : null;
            _log.LogInfo("指令: TimeManager 实例=" + (manager != null)
                         + " Time.timeScale=" + Time.timeScale.ToString("F2"));
            if (manager == null)
            {
                return;
            }

            foreach (var f in type.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance))
            {
                object v = null;
                try
                {
                    v = f.GetValue(manager);
                }
                catch
                {
                    continue;
                }

                _log.LogInfo("     " + f.Name + " = " + (v == null ? "null" : v.ToString()));
            }
        }

        private static string Describe(ParameterInfo[] parameters)
        {
            var parts = new List<string>();
            foreach (var p in parameters)
            {
                parts.Add(p.ParameterType.Name + " " + p.Name);
            }

            return string.Join(", ", parts.ToArray());
        }

        private static object Convert(string value, Type target)
        {
            try
            {
                if (target == typeof(string))
                {
                    return value;
                }

                if (target.IsEnum)
                {
                    return Enum.Parse(target, value, true);
                }

                return System.Convert.ChangeType(value, target);
            }
            catch (Exception e)
            {
                _log.LogWarning("指令: 参数 " + value + " 转不成 " + target.Name + ": " + e.Message);
                return null;
            }
        }
    }
}
