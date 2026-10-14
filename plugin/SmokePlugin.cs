using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using BepInEx;
using BepInEx.Logging;
using HarmonyLib;

namespace WellCooked
{
    /// <summary>
    /// 阶段A1 的冒烟插件：证明 BepInEx 在 Proton 下加载了插件、Harmony 补丁真的执行。
    /// 顺带把游戏程序集的类型清单落盘，供后面读世界状态的任务使用。
    /// </summary>
    [BepInPlugin(Guid, "WellCooked.Smoke", "0.1.0")]
    public class SmokePlugin : BaseUnityPlugin
    {
        public const string Guid = "wellcooked.smoke";

        private static ManualLogSource _log;

        private void Awake()
        {
            _log = Logger;
            _log.LogInfo("WellCooked.Smoke loaded; Unity " + UnityEngine.Application.unityVersion
                         + ", BepInEx " + typeof(BaseUnityPlugin).Assembly.GetName().Version);
            // 游戏失焦就暂停：调试时人在终端、答辩时 Supervisor 在终端，游戏都在后台，
            // 不打开这个开关厨师就一步不走（实测：窗口激活后立刻开始移动，失焦则完全冻结）。
            UnityEngine.Application.runInBackground = true;
            ApplySmokePatches();
            ApplyFocusPausePatch();
            WorldState.Install(_log);
            LogicalInput.Install(_log);
            Navigation.Install(_log);
            Commands.Install(_log);
            Takeover.Install(_log);
            DumpGameInventory();
        }

        /// <summary>
        /// 补丁只用来证明链路通，同时证明我们打得上游戏自己的程序集。
        /// 首选游戏每帧都会走的 GamepadEngagementManager.Update()，它没找到时退回
        /// UnityEngine.Input.GetKey（InControl 读键盘时才会被调用，所以只能当备选）。
        /// </summary>
        private static void ApplySmokePatches()
        {
            var postfix = new HarmonyMethod(typeof(SmokePlugin).GetMethod(
                nameof(AfterPatchedMethod), BindingFlags.Static | BindingFlags.NonPublic));
            var harmony = new Harmony(Guid);

            var targets = new List<MethodInfo>();
            var engagement = AccessTools.TypeByName("GamepadEngagementManager");
            if (engagement != null)
            {
                var update = AccessTools.Method(engagement, "Update", Type.EmptyTypes);
                if (update != null)
                {
                    targets.Add(update);
                }
            }

            var input = AccessTools.TypeByName("UnityEngine.Input");
            if (input != null)
            {
                var getKey = input.GetMethods(BindingFlags.Public | BindingFlags.Static)
                                  .FirstOrDefault(m => m.Name == "GetKey"
                                                       && m.GetParameters().Length == 1
                                                       && !m.IsAbstract);
                if (getKey != null)
                {
                    targets.Add(getKey);
                }
            }

            if (targets.Count == 0)
            {
                _log.LogError("smoke patch: 没有找到可打补丁的方法");
                return;
            }

            foreach (var target in targets)
            {
                try
                {
                    harmony.Patch(target, postfix: postfix);
                    _log.LogInfo("smoke patch applied: " + Describe(target));
                }
                catch (Exception e)
                {
                    _log.LogWarning("smoke patch failed on " + Describe(target) + ": " + e.Message);
                }
            }
        }

        /// <summary>
        /// 游戏自己在失焦时暂停：`InGamePauseMenu.OnApplicationFocus(false)`。
        /// 补丁只在"失去焦点"时跳过原方法（获得焦点照常走），这样后台运行不会把游戏按停。
        /// </summary>
        private static void ApplyFocusPausePatch()
        {
            var type = AccessTools.TypeByName("InGamePauseMenu");
            if (type == null)
            {
                _log.LogWarning("失焦暂停: 找不到 InGamePauseMenu");
                return;
            }

            var target = AccessTools.Method(type, "OnApplicationFocus", new[] { typeof(bool) });
            if (target == null)
            {
                _log.LogWarning("失焦暂停: InGamePauseMenu 上没有 OnApplicationFocus(bool)");
                return;
            }

            var prefix = new HarmonyMethod(typeof(SmokePlugin).GetMethod(
                nameof(SkipPauseOnFocusLost), BindingFlags.Static | BindingFlags.NonPublic));
            new Harmony("wellcooked.focus").Patch(target, prefix: prefix);
            _log.LogInfo("失焦暂停: 已让 InGamePauseMenu.OnApplicationFocus(false) 不再暂停游戏");
        }

        /// <summary>失去焦点时跳过原方法（返回 true 时才执行原方法）。</summary>
        private static bool SkipPauseOnFocusLost(bool focus)
        {
            return focus;
        }

        private static string Describe(MethodBase method)
        {
            return method.DeclaringType.Name + "." + method.Name;
        }

        private static readonly Dictionary<MethodBase, int> PatchCounts = new Dictionary<MethodBase, int>();

        private static void AfterPatchedMethod(MethodBase __originalMethod)
        {
            // 键用 MethodBase：这个补丁每帧都会进，不能为了计数每次都拼字符串
            int count;
            PatchCounts.TryGetValue(__originalMethod, out count);
            count++;
            PatchCounts[__originalMethod] = count;
            if (count == 1 || count % 600 == 0)
            {
                _log.LogInfo("smoke patch alive: " + Describe(__originalMethod) + " call #" + count);
            }
        }

        /// <summary>
        /// 导出 Assembly-CSharp 里与订单、厨具、厨师、得分相关的类型和方法名。
        /// 阶段A2 要读世界状态，得先知道游戏里有哪些类型，这份清单是那次任务的起点。
        /// </summary>
        private static void DumpGameInventory()
        {
            try
            {
                var game = AppDomain.CurrentDomain.GetAssemblies()
                                      .FirstOrDefault(a => a.GetName().Name == "Assembly-CSharp");
                if (game == null)
                {
                    _log.LogWarning("inventory: Assembly-CSharp 还没加载，跳过导出");
                    return;
                }

                var keywords = new[] { "chef", "order", "recipe", "cook", "station", "plate",
                                       "kitchen", "player", "score", "ingredient", "carry",
                                       "delivery", "input", "gamepad", "workstation" };
                var text = new StringBuilder();
                text.AppendLine("# WellCooked A1 导出的 Assembly-CSharp 类型清单");
                text.AppendLine("# Unity " + UnityEngine.Application.unityVersion);
                text.AppendLine("# 关键字: " + string.Join(", ", keywords));
                text.AppendLine();

                int types = 0, lines = 0;
                foreach (var type in SafeTypes(game).OrderBy(t => t.FullName))
                {
                    var lower = type.Name.ToLowerInvariant();
                    if (!keywords.Any(k => lower.Contains(k)))
                    {
                        continue;
                    }

                    types++;
                    text.AppendLine(type.FullName);
                    lines++;
                    foreach (var m in type.GetMethods(BindingFlags.Public | BindingFlags.NonPublic
                                                      | BindingFlags.Instance | BindingFlags.Static
                                                      | BindingFlags.DeclaredOnly))
                    {
                        text.Append("    ").Append(m.IsStatic ? "static " : "")
                            .Append(m.ReturnType.Name).Append(' ').Append(m.Name).Append('(')
                            .Append(string.Join(", ", m.GetParameters().Select(p => p.ParameterType.Name).ToArray()))
                            .AppendLine(")");
                        if (++lines > 40000)
                        {
                            text.AppendLine("# 截断：超过 40000 行");
                            break;
                        }
                    }

                    if (lines > 40000)
                    {
                        break;
                    }
                }

                text.AppendLine();
                text.AppendLine("# 关注类型（世界状态读取要用到的完整成员）");
                foreach (var wanted in new[]
                         {
                             "ServerKitchenFlowControllerBase", "ClientKitchenFlowControllerBase",
                             "ServerTeamMonitor", "ServerOrderControllerBase", "ServerOrderData",
                             "ServerCookingHandler", "ServerWorkstation", "ServerWorkableItem",
                             "ServerPlayerAttachmentCarrier", "ServerWashingStation", "ServerHeatedStation",
                             "PlateReturnController", "ServerPlateStation", "EntitySerialisationRegistry",
                             "RoundTimer", "ServerRoundTimer", "OrderDefinitionNode", "RecipeListEntry",
                             "PlayerIDProvider", "PlayerControls", "GameUtils", "MultiplayerController",
                             "RecipeList", "RecipeList+Entry", "CookingHandler", "IServerRoundTimer",
                             "ServerRoundTimer", "TeamMonitor", "ServerKitchenFlowController",
                             "OrderController", "OrderController+OrderID", "PCPadInputProvider", "TeamID"
                         })
                {
                    var focus = AccessTools.TypeByName(wanted)
                                ?? SafeTypes(game).FirstOrDefault(t => t.FullName == wanted || t.Name == wanted);
                    if (focus == null)
                    {
                        text.AppendLine(wanted + " : 没找到");
                        continue;
                    }

                    text.AppendLine(focus.FullName + (focus.IsEnum ? " (enum)" : ""));
                    foreach (var member in focus.GetMembers(BindingFlags.Public | BindingFlags.NonPublic
                                                            | BindingFlags.Instance | BindingFlags.Static
                                                            | BindingFlags.DeclaredOnly))
                    {
                        text.AppendLine("    " + member);
                    }
                }

                var path = Path.Combine(Paths.BepInExRootPath, "welcooked-inventory.txt");
                File.WriteAllText(path, text.ToString());
                _log.LogInfo("inventory: " + types + " 个类型写入 " + path);
            }
            catch (Exception e)
            {
                _log.LogError("inventory 导出失败: " + e);
            }
        }

        private static IEnumerable<Type> SafeTypes(Assembly assembly)
        {
            try
            {
                return assembly.GetTypes();
            }
            catch (ReflectionTypeLoadException e)
            {
                return e.Types.Where(t => t != null);
            }
        }
    }
}
