using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using BepInEx;
using BepInEx.Logging;
using HarmonyLib;

namespace WellCooked
{
    /// <summary>
    /// 输入注入试验（spike game-bridge/06）。
    ///
    /// 第一阶段记录游戏查询了哪些逻辑按钮/轴（每个组合只记一次）；第二阶段按
    /// `BepInEx/wellcooked-force.txt` 里列出的按钮名强制按下，用来验证插件能不能控制游戏。
    /// 强制窗口在包装对象第一次被查询时开始：游戏通常在启动时取一次按钮对象然后每帧轮询它，
    /// 所以包装对象按查询入口缓存，保持同一个实例。
    ///
    /// 结论落盘在任务文件的 ## Findings，试验代码本身可能丢弃。
    /// </summary>
    public static class ControlSpike
    {
        private const float PressSeconds = 0.4f;

        private static ManualLogSource _log;
        private static readonly HashSet<string> Seen = new HashSet<string>();
        private static readonly Dictionary<string, ILogicalButton> Wrapped = new Dictionary<string, ILogicalButton>();
        private static readonly List<string> Forced = new List<string>();
        private static float _nextFileCheck;

        public static void Install(ManualLogSource log)
        {
            _log = log;
            AttachVirtualPadWhenRunning();
            var lookup = AccessTools.TypeByName("PlayerInputLookup");
            if (lookup == null)
            {
                _log.LogError("control-spike: 找不到 PlayerInputLookup");
                return;
            }

            var buttonPostfix = new HarmonyMethod(typeof(ControlSpike).GetMethod(
                nameof(AfterButton), BindingFlags.Static | BindingFlags.NonPublic));
            var valuePostfix = new HarmonyMethod(typeof(ControlSpike).GetMethod(
                nameof(AfterValue), BindingFlags.Static | BindingFlags.NonPublic));
            var harmony = new Harmony("wellcooked.control-spike");

            int patched = 0;
            foreach (var method in lookup.GetMethods(BindingFlags.Public | BindingFlags.Static))
            {
                var returned = method.ReturnType.Name;
                if (returned != "ILogicalButton" && returned != "ILogicalValue")
                {
                    continue;
                }

                try
                {
                    harmony.Patch(method, postfix: returned == "ILogicalButton" ? buttonPostfix : valuePostfix);
                    patched++;
                }
                catch (Exception e)
                {
                    _log.LogWarning("control-spike: 补不上 " + method.Name + ": " + e.Message);
                }
            }

            _log.LogInfo("control-spike: 已记录 " + patched + " 个逻辑输入查询入口");
        }

        private static void AfterButton(MethodBase __originalMethod, object[] __args, ref ILogicalButton __result)
        {
            var key = Describe(__originalMethod, __args);
            if (Seen.Add(key))
            {
                _log.LogInfo("control-spike query: " + key);
            }

            var argName = ButtonName(__args);
            var methodName = __originalMethod.Name;
            var wanted = IsForced(argName) ? argName : (IsForced(methodName) ? methodName : null);
            if (wanted == null || __result == null)
            {
                return;
            }

            ILogicalButton forced;
            if (!Wrapped.TryGetValue(key, out forced))
            {
                forced = new ForcedButton(wanted, __result);
                Wrapped[key] = forced;
                _log.LogInfo("control-spike force: " + key + " 从下一次查询起强制按下 " + PressSeconds + " 秒");
            }

            __result = forced;
        }

        private static void AfterValue(MethodBase __originalMethod, object[] __args, ref ILogicalValue __result)
        {
            var key = Describe(__originalMethod, __args);
            if (Seen.Add(key))
            {
                _log.LogInfo("control-spike query: " + key);
            }
        }

        /// <summary>
        /// 虚拟手柄要等 InControl 初始化完再挂：借游戏每帧都会走的 GamepadEngagementManager.Update
        /// 做延时挂载，挂上后这个补丁就不再做任何事。
        /// </summary>
        private static void AttachVirtualPadWhenRunning()
        {
            var engagement = AccessTools.TypeByName("GamepadEngagementManager");
            var update = engagement == null ? null : AccessTools.Method(engagement, "Update", Type.EmptyTypes);
            if (update == null)
            {
                _log.LogError("control-spike: 找不到 GamepadEngagementManager.Update，虚拟手柄挂不上");
                return;
            }

            var postfix = new HarmonyMethod(typeof(ControlSpike).GetMethod(
                nameof(AttachOnce), BindingFlags.Static | BindingFlags.NonPublic));
            new Harmony("wellcooked.control-spike-pad").Patch(update, postfix: postfix);
            _log.LogInfo("control-spike: 等主循环起来后挂虚拟手柄");
        }

        private static void AttachOnce()
        {
            if (_padAttached || UnityEngine.Time.realtimeSinceStartup < 5f)
            {
                return;
            }

            _padAttached = true;
            try
            {
                InControl.InputManager.AttachDevice(new VirtualPad("WellCooked Pad 0"));
                _log.LogInfo("control-spike: 虚拟手柄已挂上，接下来 30 秒每 3 秒按一次 A"
                             + (VirtualPad.Skipped.Count == 0 ? "" : "（跳过不存在的控制: " + string.Join(", ", VirtualPad.Skipped.ToArray()) + "）"));

                // 摇杆/方向键的名字在这个 InControl 版本里和文档不一致，把可用的名字打出来给下一个任务用
                var names = Enum.GetNames(typeof(InControl.InputControlType));
                _log.LogInfo("control-spike: InputControlType 共 " + names.Length + " 项: "
                             + string.Join(", ", names));
            }
            catch (Exception e)
            {
                _log.LogError("control-spike: 挂虚拟手柄失败: " + e);
            }
        }

        private static bool _padAttached;

        private static string Describe(MethodBase method, object[] args)
        {
            return method.Name + "("
                   + string.Join(", ", args.Select(a => a == null ? "null" : a.ToString()).ToArray())
                   + ")";
        }

        /// <summary>按钮查询的第一个参数是逻辑按钮 ID（枚举），值查询的第一个参数是轴 ID。</summary>
        private static string ButtonName(object[] args)
        {
            if (args.Length == 0 || args[0] == null)
            {
                return null;
            }

            var first = args[0].ToString();
            return first == "0" || first == "None" ? null : first;
        }

        /// <summary>强制列表来自文件，改文件就能换目标，不用重新编译插件。</summary>
        private static bool IsForced(string name)
        {
            var now = UnityEngine.Time.realtimeSinceStartup;
            if (now >= _nextFileCheck)
            {
                _nextFileCheck = now + 0.5f;
                Forced.Clear();
                try
                {
                    var path = Path.Combine(Paths.BepInExRootPath, "wellcooked-force.txt");
                    if (File.Exists(path))
                    {
                        foreach (var line in File.ReadAllLines(path))
                        {
                            var trimmed = line.Trim();
                            if (trimmed.Length > 0 && !trimmed.StartsWith("#"))
                            {
                                Forced.Add(trimmed);
                            }
                        }
                    }
                }
                catch (Exception e)
                {
                    _log.LogWarning("control-spike: 读强制列表失败: " + e.Message);
                }
            }

            return Forced.Contains(name);
        }

        /// <summary>
        /// 把一次真实的按键时序压缩成几百毫秒：按下 → 被领取 → 释放。
        /// 逻辑树那一项不参与按键判定，直接转给被包装的真实按钮。
        /// </summary>
        private sealed class ForcedButton : ILogicalButton
        {
            private readonly string _name;
            private readonly ILogicalButton _inner;
            private readonly float _pressedAt;
            private bool _pressClaimed;
            private bool _releaseReported;

            public ForcedButton(string name, ILogicalButton inner)
            {
                _name = name;
                _inner = inner;
                _pressedAt = UnityEngine.Time.realtimeSinceStartup;
                _log.LogInfo("control-spike: 强制按钮 " + _name + " 已按下");
            }

            public void GetLogicTreeData(out AcyclicGraph<ILogicalElement, LogicalLinkInfo> graph,
                                         out AcyclicGraph<ILogicalElement, LogicalLinkInfo>.Node node)
            {
                _inner.GetLogicTreeData(out graph, out node);
            }

            public bool JustPressed()
            {
                Poll("JustPressed");
                return !_pressClaimed && HeldSeconds() < PressSeconds;
            }

            public bool JustReleased()
            {
                Poll("JustReleased");
                if (_releaseReported || HeldSeconds() < PressSeconds)
                {
                    return false;
                }

                _releaseReported = true;
                _log.LogInfo("control-spike: 强制按钮 " + _name + " 已释放");
                return true;
            }

            public bool HasUnclaimedPressEvent()
            {
                Poll("HasUnclaimedPressEvent");
                return !_pressClaimed;
            }

            public void ClaimPressEvent()
            {
                _pressClaimed = true;
            }

            public bool HasUnclaimedReleaseEvent()
            {
                return false;
            }

            public void ClaimReleaseEvent()
            {
            }

            public float GetHeldTimeLength()
            {
                return HeldSeconds();
            }

            public bool IsDown()
            {
                Poll("IsDown");
                return HeldSeconds() < PressSeconds;
            }

            /// <summary>游戏是否真的在轮询我们替换进去的对象：没轮询说明包装的入口不是它用的那个。</summary>
            private void Poll(string member)
            {
                _polls++;
                if (_polls == 1 || _polls % 300 == 0)
                {
                    _log.LogInfo("control-spike: 强制按钮 " + _name + " 被轮询 " + _polls + " 次（最近一次 " + member + "）");
                }
            }

            private int _polls;

            private float HeldSeconds()
            {
                return UnityEngine.Time.realtimeSinceStartup - _pressedAt;
            }
        }
    }
}
