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
    ///   engage=1         跳过标题画面（直接走 StartScreenFlow 的加入流程）
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

            _log.LogInfo("指令: 标题画面 检查加入状态");
            var check = AccessTools.Method(type, "CheckForEngagement", Type.EmptyTypes);
            if (check != null)
            {
                check.Invoke(flow, null);
            }
        }

        private static void DumpType(string name)
        {
            var type = AccessTools.TypeByName(name);
            if (type == null)
            {
                _log.LogWarning("指令: 找不到类型 " + name);
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
