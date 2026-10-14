using System;
using System.Collections.Generic;
using System.Reflection;
using BepInEx.Logging;
using HarmonyLib;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 逻辑层注入：补 `PlayerInputLookup.GetValue/GetButton`，让指定玩家读到我们给的轴和按键。
    ///
    /// 为什么需要它：设备层只能控制"我们自己的虚拟手柄占住的那几个玩家位"。
    /// 键盘一被使用就会抢走 1 号位（截图实证），那一号厨师的输入来自键盘，我们的手柄写不进去。
    /// 逻辑层在设备之上，不管玩家位绑的是键盘还是手柄都能接管——supercharged 在关卡内
    /// 控制小人走的就是这条路（补同一个入口）。
    ///
    /// 边界：spike game-bridge/06 实测逻辑层驱动不了**前端**（标题画面按 A 不响应），
    /// 所以前端仍然走设备层；这里只用于关卡内的动作。
    /// </summary>
    public static class LogicalInput
    {
        private static ManualLogSource _log;
        private static readonly Dictionary<PlayerInputLookup.Player, Drive> Drives =
            new Dictionary<PlayerInputLookup.Player, Drive>();

        private sealed class Drive
        {
            public bool Active;
            public float MoveX;
            public bool Action;
        }

        public static void Install(ManualLogSource log)
        {
            _log = log;

            // 关键：从插件加载起就把四个玩家的包装准备好。
            // 游戏在关卡加载时会把 GetValue/GetButton 返回的对象缓存进玩家的控制映射，
            // 那一次调用如果发生在"我们开始驱动"之前，缓存到的就是原对象，之后再补也没用。
            // 未开始驱动时包装原样透传，所以不影响正常游玩。
            foreach (PlayerInputLookup.Player player in new[]
                     {
                         PlayerInputLookup.Player.One, PlayerInputLookup.Player.Two,
                         PlayerInputLookup.Player.Three, PlayerInputLookup.Player.Four
                     })
            {
                Drives[player] = new Drive();
            }
            var harmony = new Harmony("wellcooked.logical-input");

            var valuePostfix = new HarmonyMethod(typeof(LogicalInput).GetMethod(
                nameof(AfterGetValue), BindingFlags.Static | BindingFlags.NonPublic));
            var buttonPostfix = new HarmonyMethod(typeof(LogicalInput).GetMethod(
                nameof(AfterGetButton), BindingFlags.Static | BindingFlags.NonPublic));

            int patched = 0;
            foreach (var method in typeof(PlayerInputLookup).GetMethods(BindingFlags.Public | BindingFlags.Static))
            {
                if (method.GetParameters().Length != 2)
                {
                    continue;
                }

                var first = method.GetParameters()[0].ParameterType;
                var second = method.GetParameters()[1].ParameterType;
                if (second != typeof(PlayerInputLookup.Player))
                {
                    continue;
                }

                try
                {
                    if (method.ReturnType == typeof(ILogicalValue) && first == typeof(PlayerInputLookup.LogicalValueID))
                    {
                        harmony.Patch(method, postfix: valuePostfix);
                        patched++;
                    }
                    else if (method.ReturnType == typeof(ILogicalButton) && first == typeof(PlayerInputLookup.LogicalButtonID))
                    {
                        harmony.Patch(method, postfix: buttonPostfix);
                        patched++;
                    }
                }
                catch (Exception e)
                {
                    _log.LogWarning("logical: 补不上 " + method.Name + ": " + e.Message);
                }
            }

            _log.LogInfo("logical: 逻辑层注入入口 " + patched + " 个");
        }

        /// <summary>开始/停止驱动某个玩家：轴取 [-1,1]，按键按一次给一帧。</summary>
        public static void Set(PlayerInputLookup.Player player, float moveX, bool action)
        {
            Drive drive;
            if (!Drives.TryGetValue(player, out drive))
            {
                drive = new Drive();
                Drives[player] = drive;
            }

            drive.Active = true;
            drive.MoveX = moveX;
            drive.Action = action;
        }

        public static void Clear()
        {
            foreach (var drive in Drives.Values)
            {
                drive.Active = false;
                drive.MoveX = 0f;
                drive.Action = false;
            }
        }

        public static int Driving
        {
            get
            {
                int count = 0;
                foreach (var drive in Drives.Values)
                {
                    if (drive.Active)
                    {
                        count++;
                    }
                }

                return count;
            }
        }

        // 补丁签名按 spike 里验证过的那种写法：用 __args 取参数，避免 Harmony 生成包装时踩类型坑
        private static void AfterGetValue(object[] __args, ref ILogicalValue __result)
        {
            if (__result == null || __args == null || __args.Length < 2)
            {
                return;
            }

            if (__args[0] == null || __args[0].ToString() != "MovementX")
            {
                return;
            }

            var player = (PlayerInputLookup.Player)__args[1];
            Drive drive;
            if (!Drives.TryGetValue(player, out drive))
            {
                return;
            }

            __result = new DrivenValue(__result, drive);
        }

        private static void AfterGetButton(object[] __args, ref ILogicalButton __result)
        {
            if (__result == null || __args == null || __args.Length < 2)
            {
                return;
            }

            if (__args[0] == null || __args[0].ToString() != "WorkstationInteract")
            {
                return;
            }

            var player = (PlayerInputLookup.Player)__args[1];
            Drive drive;
            if (!Drives.TryGetValue(player, out drive))
            {
                return;
            }

            __result = new DrivenButton(__result, drive);
        }

        /// <summary>移动轴：直接给策略算出来的值，其余成员转给原对象。</summary>
        private sealed class DrivenValue : ILogicalValue
        {
            private readonly ILogicalValue _inner;
            private readonly Drive _drive;

            public DrivenValue(ILogicalValue inner, Drive drive)
            {
                _inner = inner;
                _drive = drive;
            }

            public float GetValue()
            {
                return _drive.Active ? _drive.MoveX : _inner.GetValue();
            }

            public void GetLogicTreeData(out AcyclicGraph<ILogicalElement, LogicalLinkInfo> graph,
                                         out AcyclicGraph<ILogicalElement, LogicalLinkInfo>.Node node)
            {
                _inner.GetLogicTreeData(out graph, out node);
            }
        }

        /// <summary>交互键：按下 → 被领取 → 释放，和真按键一样。</summary>
        private sealed class DrivenButton : ILogicalButton
        {
            private readonly ILogicalButton _inner;
            private readonly Drive _drive;
            private bool _claimed;

            public DrivenButton(ILogicalButton inner, Drive drive)
            {
                _inner = inner;
                _drive = drive;
            }

            public void GetLogicTreeData(out AcyclicGraph<ILogicalElement, LogicalLinkInfo> graph,
                                         out AcyclicGraph<ILogicalElement, LogicalLinkInfo>.Node node)
            {
                _inner.GetLogicTreeData(out graph, out node);
            }

            public bool JustPressed()
            {
                return _drive.Active ? _drive.Action && !_claimed : _inner.JustPressed();
            }

            public bool JustReleased()
            {
                return _drive.Active ? !_drive.Action && _claimed : _inner.JustReleased();
            }

            public bool HasUnclaimedPressEvent()
            {
                return _drive.Active ? _drive.Action && !_claimed : _inner.HasUnclaimedPressEvent();
            }

            public void ClaimPressEvent()
            {
                _claimed = true;
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
                return _drive.Active ? (_drive.Action ? 1f : 0f) : _inner.GetHeldTimeLength();
            }

            public bool IsDown()
            {
                return _drive.Active ? _drive.Action : _inner.IsDown();
            }
        }
    }
}
