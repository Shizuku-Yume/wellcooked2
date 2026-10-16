using System;
using System.Collections.Generic;
using BepInEx.Logging;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 动作执行器（任务 04）：把 Bridge 下发的 `action` 变成手柄/逻辑层的输入。
    ///
    /// 边界：**不做任何决策**。走哪、按什么、什么时候按，全部照收到的消息做；
    /// "该做什么"是 Scheduler 的事（`specs/game-bridge.md` 的「What it must not do」）。
    ///
    /// 归属规则（谁能驱动某个厨师）：
    /// - 某个厨师**收到过动作**之后，这个玩家位就归它，直到链路断开（`ReleaseAll`）——
    ///   期间本地那套驱动（`WorkOrder` 工序 / 来回走）不再碰它，否则两边会抢同一个手柄。
    /// - `stop`、`move` 到达/超时只会清掉"当前命令"（厨师原地站住），**不交还归属**：
    ///   Bridge 每帧都在发消息，交还归属会让本地驱动在手势间隙里插进来。
    ///
    /// 回执（`ack`）分两次，协议 §3.5 的字段够用：
    /// 1. **收到就回一次**（`ok=true` = 指令已登记并开始执行）——上层据此知道消息没丢；
    /// 2. **走完再回一次**（到达 `ok=true` / 超时 `ok=false`）——上层据此知道这条指令的结局，
    ///    不必靠猜（任务清单要求"超时回一条失败事件，避免上层永远等"）。
    /// 没实现的动作名当场回 `ok=false`。
    ///
    /// 输入层：两手都写，且写同样的值 —— 设备层（我们占的虚拟手柄）与逻辑层
    /// （`LogicalInput`，按厨师的游戏玩家号）正好互为备份；逻辑层一旦激活会覆盖设备层的
    /// 三个逻辑键与移动轴，所以两边的值必须一致。
    /// </summary>
    public static class ActionDriver
    {
        private const float ArriveRadius = 0.5f;      // 一格（0.6 米）以内算到了
        private const float MoveTimeout = 10f;        // 一次 move 最多走这么久（任务清单定的默认）
        private const float PulseSeconds = 0.25f;     // 按键按下去多久（游戏只认边沿）
        private const float PulseInterval = 0.6f;     // 一次动作没做出效果时，隔多久再按一次

        private sealed class Command
        {
            public string Kind;
            public Vector2 Target;
            public float Deadline;
            public bool Pulsed;
            public float PulseUntil;
            public float NextPulseAt;
            public float StartedAt;
            public bool Done;   // move 走完/超时了：留着这条（同一条指令别重复登记），但不再驱动
        }

        private static ManualLogSource _log;
        private static readonly Dictionary<int, Command> Commands = new Dictionary<int, Command>();
        private static readonly HashSet<int> Owned = new HashSet<int>();

        /// <summary>有厨师被动作指令接管了（`Takeover` 据此跳过本地驱动）。</summary>
        public static bool AnyOwned
        {
            get { return Owned.Count > 0; }
        }

        public static bool Owns(int slot)
        {
            return Owned.Contains(slot);
        }

        public static void Install(ManualLogSource log)
        {
            _log = log;
            LinkRunner.ActionHandler = Handle;
        }

        /// <summary>链路断了：不再有任何归属，本地驱动可以收回这些厨师。</summary>
        public static void ReleaseAll()
        {
            if (Owned.Count == 0)
            {
                return;
            }

            _log.LogInfo("action: 链路断开，交还 " + Owned.Count + " 个玩家位给本地驱动");
            Owned.Clear();
            Commands.Clear();
        }

        // ---------------- 入站消息 ----------------

        /// <summary>处理一条 `action`；返回 null 表示接受，非 null 是失败原因（会回进 ack）。</summary>
        public static string Handle(InboundMessage m)
        {
            if (m == null || string.IsNullOrEmpty(m.action))
            {
                return "空动作";
            }

            if (!Takeover.IsOwnedSlot(m.chef_id))
            {
                // 不是我们占的玩家位：直说，别假装做了（ack 会带这句话）
                return "玩家位 " + m.chef_id + " 不在我们占的位里";
            }

            float now = Time.realtimeSinceStartup;
            string kind = m.action.Trim().ToLowerInvariant();

            if (kind == "stop")
            {
                Owned.Add(m.chef_id);
                Commands.Remove(m.chef_id);
                return null;
            }

            if (kind == "move")
            {
                if (m.target == null)
                {
                    return "move 缺 target";
                }

                Owned.Add(m.chef_id);
                var target = new Vector2(m.target.x, m.target.y);
                Command previous;
                if (Commands.TryGetValue(m.chef_id, out previous) && previous.Kind == "move"
                    && Vector2.Distance(previous.Target, target) <= 0.01f)
                {
                    // 同一条指令还在（可能已经走完）：不重复登记、不重复打日志。
                    // 桥每帧都会把输入槽里的同一条动作发过来，重复登记会变成每帧一行日志。
                    return null;
                }

                Commands[m.chef_id] = new Command
                {
                    Kind = "move",
                    Target = target,
                    Deadline = now + MoveTimeout,
                    StartedAt = now
                };
                Navigation.Assign(m.chef_id, target);
                _log.LogInfo("action: 厨师位 " + (m.chef_id + 1) + " 去 ("
                             + target.x.ToString("F1") + "," + target.y.ToString("F1") + ")");
                return null;
            }

            if (kind == "interact" || kind == "pickup" || kind == "dash")
            {
                // 语义捷径：pickup/interact 都按交互键（A），dash 按冲刺键 —— 与协议 §3.6 一致
                Owned.Add(m.chef_id);
                Command command;
                if (!Commands.TryGetValue(m.chef_id, out command) || command.Kind != kind)
                {
                    command = new Command { Kind = kind, StartedAt = now };
                    Commands[m.chef_id] = command;
                }

                command.NextPulseAt = 0f; // 立刻按第一下
                command.Deadline = now + MoveTimeout;
                return null;
            }

            return "不认识的动作 " + m.action;
        }

        // ---------------- 每帧驱动 ----------------

        /// <summary>
        /// 驱动这个玩家位（返回 true 表示这一帧归动作执行器管）。`takeover` 在本地驱动之前调用它。
        /// </summary>
        public static bool Drive(int slot, VirtualPad pad, Vector2 position, float now)
        {
            if (!Owned.Contains(slot))
            {
                return false;
            }

            Command command;
            if (!Commands.TryGetValue(slot, out command) || command.Done)
            {
                // 归属还在、当前没有活着的命令：站住不动（stop/到达/超时之后就是这个状态）
                Feed(slot, pad, Vector2.zero, false, false, false);
                return true;
            }

            if (command.Kind == "move")
            {
                float distance = Vector2.Distance(position, command.Target);
                if (distance <= ArriveRadius)
                {
                    string arrived = "到达 (" + command.Target.x.ToString("F1") + ","
                                     + command.Target.y.ToString("F1") + ")，用时 "
                                     + (now - command.StartedAt).ToString("F1") + " 秒";
                    _log.LogInfo("action: 厨师位 " + (slot + 1) + " " + arrived);
                    Report(slot, "move", true, arrived);
                    command.Done = true;
                    Feed(slot, pad, Vector2.zero, false, false, false);
                    return true;
                }

                if (now > command.Deadline)
                {
                    string failed = "超时 " + MoveTimeout.ToString("F0") + " 秒，还差 "
                                    + distance.ToString("F1") + " 米";
                    _log.LogWarning("action: 厨师位 " + (slot + 1) + " 走不到 ("
                                    + command.Target.x.ToString("F1") + "," + command.Target.y.ToString("F1")
                                    + ")，" + failed);
                    Report(slot, "move", false, failed);
                    command.Done = true;
                    Feed(slot, pad, Vector2.zero, false, false, false);
                    return true;
                }

                Navigation.Assign(slot, command.Target);
                string status;
                var direction = Navigation.Steer(slot, position, now, out status);
                // 过程行：走得慢/不动时看这句（`寻路=不可达` / `卡住` 一眼能看出是路的问题还是输入没到）
                Diag.Trace("action: 移动中 厨师位 " + (slot + 1) + " 位置=("
                           + position.x.ToString("F1") + "," + position.y.ToString("F1")
                           + ") 距目标=" + distance.ToString("F1") + " 寻路=" + status);
                Feed(slot, pad, direction, false, false, false);
                return true;
            }

            // 按键类：按下 PulseSeconds，之后松开；没做出效果就隔 PulseInterval 再按一次
            bool pressed = now < command.PulseUntil;
            if (!pressed && now >= command.NextPulseAt)
            {
                command.PulseUntil = now + PulseSeconds;
                command.NextPulseAt = now + PulseInterval;
                pressed = true;
                _log.LogInfo("action: 厨师位 " + (slot + 1) + " 按 " + command.Kind
                             + (command.Pulsed ? "（再按一次）" : ""));
                command.Pulsed = true;
            }

            if (now > command.Deadline)
            {
                command.Done = true;
            }

            bool pickup = command.Kind == "pickup" || command.Kind == "interact";
            bool dash = command.Kind == "dash";
            Feed(slot, pad, Vector2.zero, pickup && pressed, dash && pressed, false);
            return true;
        }

        /// <summary>把一条动作的结局回给 Bridge（第二次 ack）。</summary>
        private static void Report(int slot, string action, bool ok, string detail)
        {
            LinkRunner.ReportAction(action, slot, ok, detail);
        }

        /// <summary>
        /// 两手同时写：我们占的虚拟手柄（设备层）+ 逻辑层（按厨师自己的游戏玩家号）。
        /// 逻辑层激活时覆盖设备层的三个逻辑键与移动轴，所以两处必须是同一组值。
        /// </summary>
        private static void Feed(int slot, VirtualPad pad, Vector2 direction,
                                 bool pickup, bool dash, bool work)
        {
            if (pad != null)
            {
                pad.Want(pickup, dash, work, direction);
            }

            PlayerInputLookup.Player player;
            if (Takeover.TryPlayerOf(slot, out player))
            {
                LogicalInput.SetButtons(player, direction, pickup, dash, work);
            }
        }
    }
}
