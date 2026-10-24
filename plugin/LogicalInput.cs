using System;
using System.Collections.Generic;
using BepInEx.Logging;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 逻辑层注入：替换 `PlayerControls.ControlScheme` 里的按键与摇杆对象
    /// （`m_pickupButton` / `m_worksurfaceUseButton` / `m_dashButton` / `m_moveX` / `m_moveY`）。
    ///
    /// 为什么在逻辑层而不是设备层：设备层只能写进我们自己的虚拟手柄，而且要看 InControl
    /// 的控制项枚举（`Action1` 是不是 A、`Action3` 是不是 X）——任务 09 就在这里丢过一次交互。
    /// 逻辑层直接对应游戏的 `ControlSchemeData` 字段，天然 per-player，按键边沿也由
    /// `LogicalButtonBase` 的领取/释放语义给出。
    ///
    /// 边界：前端“按 A 加入”仍然走设备层虚拟手柄（`VirtualPad`）。逻辑层在前端没有
    /// `PlayerControls` 可替换；spike game-bridge/06 实测过这条分界。
    ///
    /// 门控：每个 `IsDown/GetValue` 都问一次 `PlayerControls.CanButtonBePressed()`，
    /// 和游戏自己的 `GateLogicalButton` / `GateLogicalValue` 一致——暂停、对话框、
    /// 菜单打开时输入自动归零。
    /// </summary>
    public static class LogicalInput
    {
        private static ManualLogSource _log;
        private static readonly Dictionary<PlayerInputLookup.Player, Drive> Drives =
            new Dictionary<PlayerInputLookup.Player, Drive>();
        private static readonly Dictionary<PlayerInputLookup.Player, Attachment> Attached =
            new Dictionary<PlayerInputLookup.Player, Attachment>();

        private sealed class Drive
        {
            public bool Active;
            public float MoveX;
            public float MoveY;
            public bool Pickup;
            public bool Dash;
            public bool Work;
        }

        /// <summary>某个玩家替换前的 control scheme 字段，退出关卡时原样装回去。</summary>
        private sealed class Attachment
        {
            public PlayerControls Controls;
            public ILogicalButton Pickup;
            public ILogicalButton Use;
            public ILogicalButton Dash;
            public ILogicalValue MoveX;
            public ILogicalValue MoveY;
            public DrivenButton PickupDriver;
            public DrivenButton UseDriver;
            public DrivenButton DashDriver;
            public DrivenValue MoveXDriver;
            public DrivenValue MoveYDriver;
        }

        public static void Install(ManualLogSource log)
        {
            _log = log;
        }

        /// <summary>
        /// 给每个本地厨师的 control scheme 换上我们的包装。可重复调用：已经装过的跳过。
        /// 在 `PlayerControls` 就绪后调用（`Takeover` 每次扫描厨师时都会调一次，防止
        /// 关卡刚加载时 control scheme 还没建好）。
        /// </summary>
        public static void Attach(IEnumerable<PlayerControls> chefs)
        {
            if (chefs == null)
            {
                return;
            }

            foreach (var chef in chefs)
            {
                if (chef == null)
                {
                    continue;
                }

                var provider = chef.GetComponent<PlayerIDProvider>();
                if (provider == null || !provider.IsLocallyControlled())
                {
                    continue;
                }

                var player = provider.GetID();
                Attachment existing;
                if (Attached.TryGetValue(player, out existing))
                {
                    if (existing.Controls == chef)
                    {
                        continue;
                    }

                    // 同一玩家换了一个 PlayerControls（换关卡/重开）：先把旧对象还原，
                    // 否则新对象上的字段会叠第二层包装。
                    Restore(existing);
                    Attached.Remove(player);
                }

                var scheme = chef.ControlScheme;
                if (scheme == null)
                {
                    continue;
                }

                var drive = DriveFor(player);
                var attachment = new Attachment { Controls = chef };
                attachment.Pickup = scheme.m_pickupButton;
                attachment.Use = scheme.m_worksurfaceUseButton;
                attachment.Dash = scheme.m_dashButton;
                attachment.MoveX = scheme.m_moveX;
                attachment.MoveY = scheme.m_moveY;

                attachment.PickupDriver = new DrivenButton(chef, drive, DrivenButton.Which.Pickup, attachment.Pickup);
                attachment.UseDriver = new DrivenButton(chef, drive, DrivenButton.Which.Use, attachment.Use);
                attachment.DashDriver = new DrivenButton(chef, drive, DrivenButton.Which.Dash, attachment.Dash);
                attachment.MoveXDriver = new DrivenValue(chef, drive, true, attachment.MoveX);
                attachment.MoveYDriver = new DrivenValue(chef, drive, false, attachment.MoveY);

                scheme.m_pickupButton = attachment.PickupDriver;
                scheme.m_worksurfaceUseButton = attachment.UseDriver;
                scheme.m_dashButton = attachment.DashDriver;
                scheme.m_moveX = attachment.MoveXDriver;
                scheme.m_moveY = attachment.MoveYDriver;

                Attached[player] = attachment;
                _log.LogInfo("logical: 玩家 " + player + " 的 control scheme 已接管");
            }
        }

        /// <summary>把 control scheme 还原成游戏原来的对象，并停止所有驱动。</summary>
        public static void Detach()
        {
            foreach (var attachment in Attached.Values)
            {
                Restore(attachment);
            }

            if (Attached.Count > 0)
            {
                _log.LogInfo("logical: 还原 " + Attached.Count + " 名玩家的 control scheme");
            }

            Attached.Clear();
            Clear();
        }

        private static void Restore(Attachment attachment)
        {
            if (attachment == null || attachment.Controls == null)
            {
                return;
            }

            var scheme = attachment.Controls.ControlScheme;
            if (scheme == null)
            {
                return;
            }

            // 只还原仍然是我们装上去的那个对象：期间游戏可能自己重建过 control scheme,
            // 那时候硬写回旧对象会把游戏的新输入映射覆盖掉。
            if (ReferenceEquals(scheme.m_pickupButton, attachment.PickupDriver))
            {
                scheme.m_pickupButton = attachment.Pickup;
            }

            if (ReferenceEquals(scheme.m_worksurfaceUseButton, attachment.UseDriver))
            {
                scheme.m_worksurfaceUseButton = attachment.Use;
            }

            if (ReferenceEquals(scheme.m_dashButton, attachment.DashDriver))
            {
                scheme.m_dashButton = attachment.Dash;
            }

            if (ReferenceEquals(scheme.m_moveX, attachment.MoveXDriver))
            {
                scheme.m_moveX = attachment.MoveX;
            }

            if (ReferenceEquals(scheme.m_moveY, attachment.MoveYDriver))
            {
                scheme.m_moveY = attachment.MoveY;
            }
        }

        private static Drive DriveFor(PlayerInputLookup.Player player)
        {
            Drive drive;
            if (!Drives.TryGetValue(player, out drive))
            {
                drive = new Drive();
                Drives[player] = drive;
            }

            return drive;
        }

        /// <summary>
        /// 开始/停止驱动某个玩家：轴取 [-1,1]，`action` 是交互键（A）。
        /// </summary>
        public static void Set(PlayerInputLookup.Player player, Vector2 move, bool action)
        {
            SetButtons(player, move, action, false, false);
        }

        /// <summary>按三个逻辑键分别驱动（A / 冲刺 / X 用台面）。</summary>
        public static void SetButtons(PlayerInputLookup.Player player, Vector2 move,
                                      bool pickup, bool dash, bool work)
        {
            var drive = DriveFor(player);
            drive.Active = true;
            drive.MoveX = move.x;
            drive.MoveY = move.y;
            drive.Pickup = pickup;
            drive.Dash = dash;
            drive.Work = work;
        }

        /// <summary>这一帧没人驱动：所有轴与键归零（包装对象仍留在 control scheme 上）。</summary>
        public static void Clear()
        {
            foreach (var drive in Drives.Values)
            {
                drive.Active = false;
                drive.MoveX = 0f;
                drive.MoveY = 0f;
                drive.Pickup = false;
                drive.Dash = false;
                drive.Work = false;
            }
        }

        public static bool IsAttached(PlayerInputLookup.Player player)
        {
            return Attached.ContainsKey(player);
        }

        public static int AttachedCount
        {
            get { return Attached.Count; }
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

        /// <summary>
        /// 逻辑按键。继承 `LogicalButtonBase` 而不是自己实现 ILogicalButton：领取/释放语义
        /// （`JustPressed` 只给一次、松开后重新可领取）都由游戏基类保证，和真按键一致。
        /// </summary>
        private sealed class DrivenButton : LogicalButtonBase
        {
            public enum Which
            {
                Pickup,
                Use,
                Dash
            }

            private readonly PlayerControls _controls;
            private readonly Drive _drive;
            private readonly Which _which;
            private readonly ILogicalButton _original;

            public DrivenButton(PlayerControls controls, Drive drive, Which which, ILogicalButton original)
            {
                _controls = controls;
                _drive = drive;
                _which = which;
                _original = original;
            }

            public override bool IsDown()
            {
                if (!_drive.Active || _controls == null || !_controls.CanButtonBePressed())
                {
                    return false;
                }

                return _which == Which.Pickup ? _drive.Pickup
                    : _which == Which.Use ? _drive.Work
                    : _drive.Dash;
            }

            public override void GetLogicTreeData(
                out AcyclicGraph<ILogicalElement, LogicalLinkInfo> graph,
                out AcyclicGraph<ILogicalElement, LogicalLinkInfo>.Node node)
            {
                if (_original != null)
                {
                    _original.GetLogicTreeData(out graph, out node);
                    return;
                }

                graph = new AcyclicGraph<ILogicalElement, LogicalLinkInfo>();
                node = graph.GetNode(this);
            }
        }

        /// <summary>逻辑摇杆：门控与按键相同，未驱动时给 0。</summary>
        private sealed class DrivenValue : ILogicalValue
        {
            private readonly PlayerControls _controls;
            private readonly Drive _drive;
            private readonly bool _horizontal;
            private readonly ILogicalValue _original;

            public DrivenValue(PlayerControls controls, Drive drive, bool horizontal, ILogicalValue original)
            {
                _controls = controls;
                _drive = drive;
                _horizontal = horizontal;
                _original = original;
            }

            public float GetValue()
            {
                if (!_drive.Active || _controls == null || !_controls.CanButtonBePressed())
                {
                    return 0f;
                }

                // 游戏把逻辑 Y 变成世界 z 时会取反（PlayerControlsHelper.GetControlAxis:
                // z = -MoveY）。调用方给的是世界方向 (x, z)，所以这里把 Y 翻回来，
                // 否则“往南走”会让厨师往北走（实测卡在料箱的反方向）。
                return _horizontal ? _drive.MoveX : -_drive.MoveY;
            }

            public void GetLogicTreeData(
                out AcyclicGraph<ILogicalElement, LogicalLinkInfo> graph,
                out AcyclicGraph<ILogicalElement, LogicalLinkInfo>.Node node)
            {
                if (_original != null)
                {
                    _original.GetLogicTreeData(out graph, out node);
                    return;
                }

                graph = new AcyclicGraph<ILogicalElement, LogicalLinkInfo>();
                node = graph.GetNode(this);
            }
        }
    }
}
