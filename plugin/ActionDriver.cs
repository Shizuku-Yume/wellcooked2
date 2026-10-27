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
        private const float PathEndpointTolerance = 0.3f; // nav waypoint must be reached within 0.3 m
        private const float PathCornerTolerance = 0.15f; // turn corners may be skipped within 0.15 m
        private const float FaceAngleTolerance = 15f;    // 朝向误差 < 15°
        private const float FaceStickMagnitude = 0.35f;  // 转身脉冲幅度：避免平移冲出 0.3m 站位
        private const float FacePulseDuration = 0.06f;   // 每次转身脉冲时长
        private const float FacePulseInterval = 0.25f;   // 转身脉冲间隔
        private const float PulseSeconds = 0.25f;     // 按键按下去多久（游戏只认边沿）
        private const float PulseInterval = 0.6f;     // 一次动作没做出效果时，隔多久再按一次
        private const int KeyPulses = 2;              // 按键类按几次算"做过了"（游戏只认边沿，按一次可能被吃掉）
        // 桥对一步按键会连发 KOS_WORKER_INTERACT_WINDOW_MS（bridge/src/worker_timing.h，1 秒）。
        // 这段时间里上一条按键已经做完再收到同类按键，是同一步的重发，不是新的一步。
        private const float ResendWindow = 1.0f;

        private sealed class Command
        {
            public string Kind;
            public Vector2 Target;
            public string TargetObject;
            public float Deadline;
            public int Pulses;   // 按键类按了几次
            public float PulseUntil;
            public float NextPulseAt;
            public float StartedAt;
            public bool Done;   // 走完/超时了：留着这条，但不再驱动
            public string HeldAtFirstPulse; // 按键类：第一下按下时手上是什么
            public float DoneAt;            // 按键类：哪一刻做完的（游戏时钟）

            // path 专用
            public long Seq;
            public List<Vector2> Waypoints;
            public int WaypointIndex;
            public int PathDirection = -1;
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

        public static int OwnedCount
        {
            get { return Owned.Count; }
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

            float wall = Time.realtimeSinceStartup;
            float now = GameClock(wall);
            string kind = m.action.Trim().ToLowerInvariant();

            Command active;
            if (Commands.TryGetValue(m.chef_id, out active) && active.Kind == "work"
                && kind != "work")
            {
                // work 的结束条件只有"收到下一条动作"；把这句话写进日志，实机验收才看得见。
                _log.LogInfo("action: 厨师位 " + (m.chef_id + 1) + " work 松开（收到 " + kind + "）");
            }

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
                if (!IsFinite(m.target.x) || !IsFinite(m.target.y))
                {
                    return "move 的 target 不是有限数";
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
                if (Commands.TryGetValue(m.chef_id, out command) && command.Kind == kind
                    && !command.Done)
                {
                    // 同一条按键动作还在（桥每帧都会重发）：不重复登记 —— 重复登记会把
                    // 脉冲间隔和超时都重置，变成"一直按着、永远不结束"。
                    return null;
                }

                if (command != null && command.Kind == kind && command.Done && now - command.DoneAt < ResendWindow)
                {
                    // 刚做完又收到同类按键：是桥同一步的重发。再登记就是第 3 下，
                    // 会把刚拿起的东西放回去（实测 `_3`：拿起→放回→再拿起）。
                    return null;
                }

                command = new Command { Kind = kind, StartedAt = now };
                Commands[m.chef_id] = command;
                command.NextPulseAt = 0f; // 立刻按第一下
                command.Deadline = now + MoveTimeout;
                return null;
            }

            if (kind == "path")
            {
                if (m.waypoints == null || m.waypoints.Length == 0)
                {
                    return "path 缺 waypoints";
                }
                if (m.waypoints.Length > 8)
                {
                    return "path 路点数超过 8（收到 " + m.waypoints.Length + "）";
                }
                if (m.seq < 0)
                {
                    return "path 的 seq 不能为负";
                }
                for (int i = 0; i < m.waypoints.Length; i++)
                {
                    if (!IsFinite(m.waypoints[i].x) || !IsFinite(m.waypoints[i].y))
                    {
                        return "path 的路点不是有限数";
                    }
                }

                Owned.Add(m.chef_id);
                var newWps = new List<Vector2>(m.waypoints.Length);
                for (int i = 0; i < m.waypoints.Length; i++)
                {
                    newWps.Add(new Vector2(m.waypoints[i].x, m.waypoints[i].y));
                }

                PlayerControls chef;
                Vector2 pos = Vector2.zero;
                bool hasPos = Takeover.TryChefOf(m.chef_id, out chef) && chef != null;
                if (hasPos)
                {
                    pos = new Vector2(chef.transform.position.x, chef.transform.position.z);
                }

                Command previous;
                if (Commands.TryGetValue(m.chef_id, out previous) && previous.Kind == "path" && previous.Seq == m.seq)
                {
                    if (previous.Waypoints != null)
                    {
                        int existingCount = previous.Waypoints.Count;
                        int matchIndex = -1;
                        if (previous.WaypointIndex >= 0 && previous.WaypointIndex < previous.Waypoints.Count)
                        {
                            Vector2 prevTarget = previous.Waypoints[previous.WaypointIndex];
                            for (int i = 0; i < newWps.Count; i++)
                            {
                                if (Vector2.Distance(newWps[i], prevTarget) <= 0.01f)
                                {
                                    matchIndex = i;
                                    break;
                                }
                            }
                        }

                        if (matchIndex < 0)
                        {
                            matchIndex = 0;
                            if (hasPos)
                            {
                                while (matchIndex < newWps.Count - 1 &&
                                       Vector2.Distance(pos, newWps[matchIndex]) <= PathCornerTolerance)
                                {
                                    matchIndex++;
                                }
                            }
                        }
                        else if (hasPos)
                        {
                            while (matchIndex < newWps.Count - 1 &&
                                   Vector2.Distance(pos, newWps[matchIndex]) <= PathCornerTolerance)
                            {
                                matchIndex++;
                            }
                        }

                        previous.Waypoints = newWps;
                        previous.WaypointIndex = matchIndex;
                        previous.PathDirection = -1;
                        previous.Done = false;
                        int remaining = previous.Waypoints.Count - previous.WaypointIndex;
                        previous.Deadline = now + Math.Max(MoveTimeout, remaining * 2.0f);
                        if (newWps.Count != existingCount)
                        {
                            _log.LogInfo("action: 厨师位 " + (m.chef_id + 1) + " path seq="
                                         + m.seq + " 更新路点 " + existingCount + "→"
                                         + newWps.Count + " idx=" + matchIndex);
                        }
                    }
                    return null;
                }

                int startIdx = 0;
                if (hasPos)
                {
                    while (startIdx < newWps.Count - 1 &&
                           Vector2.Distance(pos, newWps[startIdx]) <= PathCornerTolerance)
                    {
                        startIdx++;
                    }
                }

                Commands[m.chef_id] = new Command
                {
                    Kind = "path",
                    Seq = m.seq,
                    Waypoints = newWps,
                    WaypointIndex = startIdx,
                    StartedAt = now,
                    Deadline = now + Math.Max(MoveTimeout, newWps.Count * 2.0f)
                };
                _log.LogInfo("action: 厨师位 " + (m.chef_id + 1) + " path seq=" + m.seq + " 路点数=" + newWps.Count);
                return null;
            }

            if (kind == "face")
            {
                Command previous;
                bool hasPrevious = Commands.TryGetValue(m.chef_id, out previous);
                if (!string.IsNullOrEmpty(m.target_object)
                    && hasPrevious && previous.Kind == "face"
                    && previous.TargetObject == m.target_object)
                {
                    if (previous.Done)
                    {
                        previous.Done = false;
                        previous.StartedAt = now;
                        previous.Deadline = now + 4f;
                        previous.NextPulseAt = 0f;
                    }
                    return null;
                }

                Vector2 target = Vector2.zero;
                bool hasTarget = false;
                if (!string.IsNullOrEmpty(m.target_object))
                {
                    Vector2 station;
                    if (Kitchen.TryStationPosition(m.target_object, out station))
                    {
                        target = station;
                        hasTarget = true;
                    }
                }
                if (!hasTarget && m.target != null)
                {
                    target = new Vector2(m.target.x, m.target.y);
                    hasTarget = true;
                }

                if (!hasTarget)
                {
                    return "face 缺 target 或找不到 target_object";
                }
                if (!IsFinite(target.x) || !IsFinite(target.y))
                {
                    return "face 的目标不是有限数";
                }

                Owned.Add(m.chef_id);
                if (hasPrevious && previous.Kind == "face"
                    && Vector2.Distance(previous.Target, target) <= 0.01f)
                {
                    if (previous.Done)
                    {
                        previous.Done = false;
                        previous.StartedAt = now;
                        previous.Deadline = now + 4f;
                        previous.NextPulseAt = 0f;
                    }
                    return null;
                }

                Commands[m.chef_id] = new Command
                {
                    Kind = "face",
                    Target = target,
                    TargetObject = m.target_object,
                    StartedAt = now,
                    Deadline = now + 4f
                };
                _log.LogInfo("action: 厨师位 " + (m.chef_id + 1) + " face 转向 ("
                             + target.x.ToString("F1") + "," + target.y.ToString("F1") + ")"
                             + (string.IsNullOrEmpty(m.target_object) ? "" : " " + m.target_object));
                return null;
            }

            if (kind == "work")
            {
                if (string.IsNullOrEmpty(m.target_object))
                {
                    return "work 缺 target_object";
                }

                Owned.Add(m.chef_id);
                Command previous;
                if (Commands.TryGetValue(m.chef_id, out previous) && previous.Kind == "work")
                {
                    if (previous.TargetObject == m.target_object)
                    {
                        return null; // 桥每帧重发同一条
                    }
                    _log.LogInfo("action: 厨师位 " + (m.chef_id + 1) + " work 换目标 "
                                 + (previous.TargetObject ?? "") + "→" + m.target_object);
                }

                Commands[m.chef_id] = new Command
                {
                    Kind = "work",
                    TargetObject = m.target_object,
                    StartedAt = now
                };
                _log.LogInfo("action: 厨师位 " + (m.chef_id + 1) + " work 按住 "
                             + (m.target_object ?? "") + "（直到下一条动作）");
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
            float clock = GameClock(now);
            if (!Owned.Contains(slot))
            {
                return false;
            }

            Command command;
            if (Commands.TryGetValue(slot, out command) && command.Kind == "face" && command.Done)
            {
                PlayerControls chefObj;
                if (Takeover.TryChefOf(slot, out chefObj) && chefObj != null)
                {
                    Vector2 d = command.Target - position;
                    if (d.sqrMagnitude > 0.0001f)
                    {
                        Vector2 fwd = new Vector2(chefObj.transform.forward.x, chefObj.transform.forward.z).normalized;
                        float curAngle = Vector2.Angle(fwd, d.normalized);
                        if (curAngle >= FaceAngleTolerance)
                        {
                            command.Done = false;
                            command.StartedAt = now;
                            command.Deadline = now + 4f;
                            command.NextPulseAt = 0f;
                        }
                    }
                }
            }

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
                                     + (clock - command.StartedAt).ToString("F1") + " 秒";
                    _log.LogInfo("action: 厨师位 " + (slot + 1) + " " + arrived);
                    Report(slot, "move", true, arrived);
                    command.Done = true;
                    Feed(slot, pad, Vector2.zero, false, false, false);
                    return true;
                }

                if (clock > command.Deadline)
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

            if (command.Kind == "path")
            {
                if (command.Waypoints == null || command.Waypoints.Count == 0)
                {
                    command.Done = true;
                    Feed(slot, pad, Vector2.zero, false, false, false);
                    return true;
                }

                int idx = command.WaypointIndex;
                if (idx >= command.Waypoints.Count)
                {
                    idx = command.Waypoints.Count - 1;
                }
                int startedAt = idx;
                Vector2 currentTarget = command.Waypoints[idx];
                float dist = Vector2.Distance(position, currentTarget);

                while (idx < command.Waypoints.Count - 1 && dist <= PathCornerTolerance)
                {
                    idx++;
                    command.WaypointIndex = idx;
                    command.PathDirection = -1;
                    currentTarget = command.Waypoints[idx];
                    dist = Vector2.Distance(position, currentTarget);
                }

                if (idx != startedAt)
                {
                    // 实机验收看"连续走、没有中途停顿"就靠这些带时间戳的切换行。
                    Diag.Trace("action: path 路点切换 厨师位 " + (slot + 1) + " "
                               + (startedAt + 1) + "→" + (idx + 1) + "/"
                               + command.Waypoints.Count + " seq=" + command.Seq
                               + " t=" + now.ToString("F2"));
                }

                if (idx == command.Waypoints.Count - 1 &&
                    PathArrival.IsWithinTolerance(position.x, position.y,
                                                  currentTarget.x, currentTarget.y,
                                                  PathEndpointTolerance))
                {
                    string arrived = "path 到达终点 (" + currentTarget.x.ToString("F1") + "," + currentTarget.y.ToString("F1")
                                     + ") seq=" + command.Seq + "，用时 " + (clock - command.StartedAt).ToString("F1") + " 秒";
                    _log.LogInfo("action: 厨师位 " + (slot + 1) + " " + arrived);
                    Report(slot, "path", true, arrived, command.Seq);
                    command.Done = true;
                    Feed(slot, pad, Vector2.zero, false, false, false);
                    return true;
                }

                if (clock > command.Deadline)
                {
                    string late = "path 超时，距第 " + (idx + 1) + " 个路点还差 " + dist.ToString("F1") + " 米 seq=" + command.Seq;
                    _log.LogWarning("action: 厨师位 " + (slot + 1) + " " + late);
                    Report(slot, "path", false, late, command.Seq);
                    command.Done = true;
                    Feed(slot, pad, Vector2.zero, false, false, false);
                    return true;
                }

                Vector2 dir = PathDirection(currentTarget - position, command);
                Feed(slot, pad, dir, false, false, false);
                return true;
            }

            if (command.Kind == "face")
            {
                Vector2 dir = command.Target - position;
                if (dir.sqrMagnitude > 0.0001f)
                {
                    dir = dir.normalized;
                }
                else
                {
                    dir = Vector2.zero;
                }

                PlayerControls chef;
                float angle = 180f;
                bool hasChef = Takeover.TryChefOf(slot, out chef) && chef != null;
                if (hasChef)
                {
                    if (dir == Vector2.zero)
                    {
                        angle = 0f; // 目标就在脚下：没有"朝哪"的问题
                    }
                    else
                    {
                        Vector2 currentForward = new Vector2(chef.transform.forward.x,
                                                             chef.transform.forward.z).normalized;
                        angle = Vector2.Angle(currentForward, dir);
                    }
                }

                if (angle < FaceAngleTolerance)
                {
                    string done = "face 对准 误差=" + angle.ToString("F1") + "°";
                    _log.LogInfo("action: 厨师位 " + (slot + 1) + " " + done);
                    Report(slot, "face", true, done);
                    command.Done = true;
                    Feed(slot, pad, Vector2.zero, false, false, false);
                    return true;
                }

                if (clock > command.Deadline)
                {
                    string late = "face 超时 误差=" + angle.ToString("F1") + "°";
                    _log.LogWarning("action: 厨师位 " + (slot + 1) + " " + late);
                    Report(slot, "face", false, late);
                    command.Done = true;
                    Feed(slot, pad, Vector2.zero, false, false, false);
                    return true;
                }

                // 转身要短促：用较小摇杆幅度与短脉冲，避免平移冲出 0.3m 站位。
                if (now >= command.NextPulseAt)
                {
                    command.NextPulseAt = now + FacePulseInterval;
                    command.PulseUntil = now + FacePulseDuration;
                }
                bool pulsing = hasChef && now < command.PulseUntil;
                Feed(slot, pad, pulsing ? (dir * FaceStickMagnitude) : Vector2.zero, false, false, false);
                return true;
            }

            if (command.Kind == "work")
            {
                // 按住加工键直到下一条动作
                Feed(slot, pad, Vector2.zero, false, false, true);
                return true;
            }

            // 按键类：按下 PulseSeconds，之后松开；再隔 PulseInterval 按一次（游戏只认边沿，
            // 一次按键可能被帧边界吃掉）。按满 KeyPulses 次并松开就算做完，回一条结局 ack ——
            // 按键没有"到没到位"这种可观测的结局，能报的就是"按了几次"，效果看后续 state_update。
            bool pressed = now < command.PulseUntil;
            if (!pressed && command.Pulses < KeyPulses && now >= command.NextPulseAt)
            {
                string held = HeldOf(slot);
                if (command.Pulses == 0)
                {
                    command.HeldAtFirstPulse = held;
                }
                else if (held != command.HeldAtFirstPulse)
                {
                    // 第一下已经生效（手上的东西变了）：补按的第二下会把它撤回（拿起又放下），不按。
                    _log.LogInfo("action: 厨师位 " + (slot + 1) + " " + command.Kind + " 第一下已生效（手上「"
                                 + command.HeldAtFirstPulse + "」→「" + held + "」），不再补按");
                    command.Pulses = KeyPulses;
                }
            }

            if (!pressed && command.Pulses < KeyPulses && now >= command.NextPulseAt)
            {
                command.PulseUntil = now + PulseSeconds;
                command.NextPulseAt = now + PulseInterval;
                command.Pulses++;
                pressed = true;
                // 按键有没有效果要看"面前是谁、手上是什么"：游戏按厨师所在格的邻格里朝向最对齐的
                // 那一格判定交互对象，按错格子时 ack 照样 ok，只有这一行能看出来。
                _log.LogInfo("action: 厨师位 " + (slot + 1) + " 按 " + command.Kind
                             + (command.Pulses > 1 ? "（再按一次）" : "") + " " + PoseSummary(slot, position));
            }

            if (clock > command.Deadline)
            {
                string late = "超时 " + MoveTimeout.ToString("F0") + " 秒，按键没走完";
                _log.LogWarning("action: 厨师位 " + (slot + 1) + " " + command.Kind + " " + late);
                Report(slot, command.Kind, false, late);
                command.Done = true;
                command.DoneAt = clock;
                Feed(slot, pad, Vector2.zero, false, false, false);
                return true;
            }

            if (command.Pulses >= KeyPulses && !pressed)
            {
                string done = "已按键 " + command.Pulses + " 次";
                _log.LogInfo("action: 厨师位 " + (slot + 1) + " " + command.Kind + " 按完 "
                             + PoseSummary(slot, position));
                Report(slot, command.Kind, true, done);
                command.Done = true;
                command.DoneAt = clock;
                Feed(slot, pad, Vector2.zero, false, false, false);
                return true;
            }

            bool pickup = command.Kind == "pickup" || command.Kind == "interact";
            bool dash = command.Kind == "dash";
            Feed(slot, pad, Vector2.zero, pickup && pressed, dash && pressed, false);
            return true;
        }

        /// <summary>厨师手上是什么（拿不到厨师时返回空串）。</summary>
        private static string HeldOf(int slot)
        {
            PlayerControls chef;
            return Takeover.TryChefOf(slot, out chef) ? Kitchen.HeldSummary(chef) : "";
        }

        /// <summary>厨师位置、朝向、面前的占位物体、手上的东西（按键诊断用）。</summary>
        private static string PoseSummary(int slot, Vector2 position)
        {
            PlayerControls chef;
            if (!Takeover.TryChefOf(slot, out chef))
            {
                return "位置=(" + position.x.ToString("F2") + "," + position.y.ToString("F2") + ") 厨师=缺";
            }
            var forward = chef.transform.forward;
            return "位置=(" + position.x.ToString("F2") + "," + position.y.ToString("F2")
                   + ") 朝向=(" + forward.x.ToString("F2") + "," + forward.z.ToString("F2")
                   + ") 面前=「" + Kitchen.FacingObjectName(chef) + "」 手上=「" + Kitchen.HeldSummary(chef) + "」 "
                   + GameCellSummary(position);
        }

        /// <summary>
        /// 游戏 1.2 米网格上：厨师所在格、本格占位物体、四个邻格的占位物体。
        /// `GetFacingGridOccupant` 在本格有占位物体时直接跳过，只看邻格 —— 这一段能看出是哪种情况。
        /// </summary>
        private static string GameCellSummary(Vector2 position)
        {
            int ix, iz;
            if (!WorldState.GameCellOf(position, out ix, out iz))
            {
                return "游戏格=?";
            }
            return "游戏格=(" + ix + "," + iz + ") 本格=「" + OccupantName(ix, iz)
                   + "」 +x=「" + OccupantName(ix + 1, iz) + "」 -x=「" + OccupantName(ix - 1, iz)
                   + "」 +z=「" + OccupantName(ix, iz + 1) + "」 -z=「" + OccupantName(ix, iz - 1) + "」";
        }

        private static string OccupantName(int ix, int iz)
        {
            var occupant = WorldState.GameCellOccupant(ix, iz);
            return occupant == null ? "" : occupant.name;
        }

        private static bool IsFinite(float value)
        {
            return !float.IsNaN(value) && !float.IsInfinity(value);
        }

        /// <summary>游戏时钟（秒）；关卡计时还没初始化时退回墙钟，避免 0 秒永远不超时。</summary>
        private static float GameClock(float wallNow)
        {
            float game = StateFrame.GameTime();
            return game > 0f ? game : wallNow;
        }

        /// <summary>
        /// 当前登记的动作摘要（`wellcooked-command.txt` 的 `action_test=1` 用它回读
        /// "同 seq 是否只追加"与"work 是否还按着"）。不是调度路径的一部分。
        /// </summary>
        public static string DebugState(int slot)
        {
            Command command;
            if (!Commands.TryGetValue(slot, out command))
            {
                return "none";
            }
            if (command.Kind == "path")
            {
                return "path seq=" + command.Seq + " waypoints="
                       + (command.Waypoints == null ? 0 : command.Waypoints.Count)
                       + " index=" + command.WaypointIndex + " done=" + command.Done;
            }
            return command.Kind + " done=" + command.Done;
        }

        /// <summary>把一条动作的结局回给 Bridge（第二次 ack）。`seq` 只对 path 有意义。</summary>
        private static void Report(int slot, string action, bool ok, string detail, long seq = -1)
        {
            LinkRunner.ReportAction(action, slot, ok, detail, seq);
        }

        /// <summary>
        /// 两手同时写：我们占的虚拟手柄（设备层）+ 逻辑层（按厨师自己的游戏玩家号）。
        /// 逻辑层激活时覆盖设备层的三个逻辑键与移动轴，所以两处必须是同一组值。
        /// </summary>
        private static Vector2 PathDirection(Vector2 offset, Command command)
        {
            if (offset.sqrMagnitude <= 0.16f) return offset.normalized;
            int direction = PathSteering.Quantize(offset.x, offset.y, command.PathDirection, offset.magnitude);
            command.PathDirection = direction;
            if (direction < 0) return Vector2.zero;
            float angle = direction * 45f * Mathf.Deg2Rad;
            return new Vector2(Mathf.Cos(angle), Mathf.Sin(angle));
        }



        private static void Feed(int slot, VirtualPad pad, Vector2 direction,
                                 bool pickup, bool dash, bool work)
        {
            // 设备层和逻辑层对竖轴的方向相反（2026-10-25 s_city_h5：逻辑层喂 +y 时厨师
            // 向 -z 走）。设备层与 Takeover 的本地驱动走同一条路，必须喂世界方向；
            // 逻辑层才翻转。
            if (pad != null)
            {
                // 只写设备层：逻辑层对"已经由我们手柄占住的玩家位"会把设备层的轴覆盖成
                // 零/反方向（实测厨师坐标完全不动）。这也与 Takeover 本地驱动一致。
                pad.Want(pickup, dash, work, direction);
                return;
            }

            PlayerInputLookup.Player player;
            if (Takeover.TryPlayerOf(slot, out player))
            {
                var stick = new Vector2(direction.x, -direction.y);
                LogicalInput.SetButtons(player, stick, pickup, dash, work);
            }
        }
    }
}
