using System;
using System.Collections;
using System.Collections.Generic;
using System.Text;
using BepInEx.Logging;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 人手动打一局时的观察记录（任务 09 的 P0 第 1 步）。
    ///
    /// 干什么：每帧看每个 `PlayerControls` 的 A（`PickupAndDrop`）与 X
    /// （`WorkstationInteract`）逻辑键，按下/松开各打一行，带当时的手持物（含盘子里的内容）、
    /// 面前的占位物体与游戏格四邻；另外每 0.25 秒给工位内容拍一次快照，变化才打一行
    /// （砧板上的料与进度、锅里的料与进度、盘子上/盘子堆里的内容）。
    ///
    /// 为什么不用 `ActionDriver`：那是 Bridge 驱动的日志，手动局没有动作下发。这里读的是
    /// 游戏自己的逻辑键（`PlayerControls.ControlScheme`），设备层键盘/手柄都算，只调
    /// `IsDown()`（不像 `JustPressed()` 那样吃掉按键事件），不影响游戏输入。
    ///
    /// 开关：`BepInEx/wellcooked-takeover.txt` 的 `manual_log=true`。默认关；手动局才需要。
    /// 前缀固定为 `manual:`，日志里按这个 grep。
    /// </summary>
    internal static class ManualLog
    {
        private const string Prefix = "manual: ";
        private const float SnapshotInterval = 0.25f;

        private sealed class Keys
        {
            public bool A;
            public bool X;
        }

        private static ManualLogSource _log;
        private static bool _enabled;
        private static float _nextSnapshot;
        private static string _lastStations;
        private static readonly Dictionary<int, Keys> Pressed = new Dictionary<int, Keys>();

        public static bool Enabled
        {
            get { return _enabled; }
        }

        public static void Install(ManualLogSource log)
        {
            _log = log;
        }

        public static void Set(bool on)
        {
            _enabled = on;
            Pressed.Clear();
            _lastStations = null;
            _nextSnapshot = 0f;
        }

        public static void Tick(float now)
        {
            if (!_enabled || _log == null)
            {
                return;
            }

            try
            {
                ObserveKeys(now);
                if (now >= _nextSnapshot)
                {
                    _nextSnapshot = now + SnapshotInterval;
                    ObserveStations(now);
                }
            }
            catch (Exception e)
            {
                // 记录器不能把游戏打崩；同一条错误只报一次。
                _enabled = false;
                _log.LogWarning(Prefix + "观察失败，已关闭: " + e.Message);
            }
        }

        // ---------------- 按键 ----------------

        private static void ObserveKeys(float now)
        {
            var seen = new HashSet<int>();
            foreach (var chef in UnityEngine.Object.FindObjectsOfType<PlayerControls>())
            {
                if (chef == null)
                {
                    continue;
                }
                var provider = chef.GetComponent<PlayerIDProvider>();
                if (provider == null)
                {
                    continue;
                }

                int slot = (int)provider.GetID();
                seen.Add(slot);

                Keys state;
                if (!Pressed.TryGetValue(slot, out state))
                {
                    state = new Keys();
                    Pressed[slot] = state;
                }

                var scheme = chef.ControlScheme;
                if (scheme == null)
                {
                    continue;
                }

                bool a = scheme.m_pickupButton != null && scheme.m_pickupButton.IsDown();
                bool x = scheme.m_worksurfaceUseButton != null && scheme.m_worksurfaceUseButton.IsDown();

                if (a != state.A)
                {
                    state.A = a;
                    LogKey(now, slot, "A", a ? "按下" : "松开", chef);
                }
                if (x != state.X)
                {
                    state.X = x;
                    LogKey(now, slot, "X", x ? "按下" : "松开", chef);
                }
            }

            if (Pressed.Count != seen.Count)
            {
                var stale = new List<int>();
                foreach (var slot in Pressed.Keys)
                {
                    if (!seen.Contains(slot))
                    {
                        stale.Add(slot);
                    }
                }
                foreach (var slot in stale)
                {
                    Pressed.Remove(slot);
                }
            }
        }

        private static void LogKey(float now, int slot, string key, string edge, PlayerControls chef)
        {
            _log.LogInfo(Prefix + "按键 玩家位=" + (slot + 1) + " 键=" + key + " " + edge
                         + " t=" + now.ToString("F2") + " 手=" + HeldDetail(chef)
                         + " " + FacingDetail(chef) + " " + PoseDetail(chef));
        }

        // ---------------- 工位内容 ----------------

        private static void ObserveStations(float now)
        {
            var parts = new List<string>();

            var boards = Kitchen.ChoppingBoards();
            for (int i = 0; i < boards.Count; i++)
            {
                var board = boards[i];
                var attach = board.Component == null ? null
                    : board.Component.gameObject.GetComponent("ClientAttachStation");
                var item = attach == null ? null : WorldState.Get(attach, "m_item") as Component;
                bool finished;
                string progress;
                Kitchen.WorkState(board.Position, out finished, out progress);
                parts.Add("砧板" + i + "@" + Pos(board.Position)
                          + "=「" + (item == null ? "空" : item.gameObject.name) + "」"
                          + (item == null ? "" : " " + progress));
            }

            var pots = Kitchen.SortedCookingHandlers();
            for (int i = 0; i < pots.Count; i++)
            {
                var handler = pots[i].Value;
                var component = handler as Component
                                ?? WorldState.Get(handler, "GetCookingHandler") as Component;
                if (component == null)
                {
                    continue;
                }

                var contents = ContentsOf(component.gameObject);
                bool cooked = IsTrue(WorldState.Invoke(handler, "IsCooked"));
                bool burning = IsTrue(WorldState.Invoke(handler, "IsBurning"));
                var progress = WorldState.Invoke(handler, "GetCookingProgress");
                // 不写编号：`GetCookingHandlers()` 只列有料的锅，编号会随锅空/满变化，
                // 位置才是稳定的对照键（kitchen.json 的 cooker-N 也按位置排）。
                parts.Add("锅@" + Pos(pots[i].Key) + "=[" + string.Join("+", contents.ToArray())
                          + "] 烹饪秒=" + (progress == null ? "?" : Convert.ToDouble(progress).ToString("F2"))
                          + (cooked ? " 已熟" : "") + (burning ? " 烧焦" : ""));
            }

            foreach (var found in WorldState.FindAll("ClientIngredientContainer"))
            {
                var container = found as Component;
                if (container == null)
                {
                    continue;
                }
                var contents = ContentsOf(container.gameObject);
                if (contents.Count == 0)
                {
                    continue;
                }
                var p = container.transform.position;
                parts.Add("容器「" + container.gameObject.name + "」@"
                          + Pos(new Vector2(p.x, p.z)) + "=[" + string.Join("+", contents.ToArray()) + "]");
            }

            int stackCount = 0;
            bool stackFound = false;
            var stackPos = Vector2.zero;
            foreach (var stack in WorldState.FindAll("ClientCleanPlateStack"))
            {
                var component = stack as Component;
                if (component == null)
                {
                    continue;
                }
                var size = WorldState.Invoke(stack, "GetSize");
                if (size == null)
                {
                    continue;
                }
                stackFound = true;
                stackCount += (int)Convert.ToDouble(size);
                var p = component.transform.position;
                stackPos = new Vector2(p.x, p.z);
            }
            if (stackFound)
            {
                parts.Add("干净盘堆@「" + Pos(stackPos) + "」=" + stackCount);
            }

            string snapshot = string.Join("；", parts.ToArray());
            if (snapshot != _lastStations)
            {
                _lastStations = snapshot;
                _log.LogInfo(Prefix + "工位 t=" + now.ToString("F2") + " " + snapshot);
            }
        }

        // ---------------- 摘要 ----------------

        /// <summary>手上是什么，带全部内容：`「Plate」[SushiFish+ChoppedFish]`；空手 `（空）`。</summary>
        private static string HeldDetail(PlayerControls chef)
        {
            var carrier = chef.GetComponent<ServerPlayerAttachmentCarrier>();
            var item = carrier == null ? null : carrier.InspectCarriedItem() as UnityEngine.Object;
            if (item == null)
            {
                return "（空）";
            }
            var gameObject = AsGameObject(item);
            if (gameObject == null)
            {
                return "「?」";
            }
            var contents = ContentsOf(gameObject);
            return "「" + gameObject.name + "」"
                   + (contents.Count == 0 ? "" : "[" + string.Join("+", contents.ToArray()) + "]");
        }

        private static string FacingDetail(PlayerControls chef)
        {
            var name = Kitchen.FacingObjectName(chef);
            Vector2 position;
            if (Kitchen.FacingObjectPosition(chef, out position))
            {
                return "面前=「" + name + "」@" + Pos(position);
            }
            return "面前=「" + name + "」";
        }

        private static string PoseDetail(PlayerControls chef)
        {
            var p = chef.transform.position;
            var forward = chef.transform.forward;
            var sb = new StringBuilder();
            sb.Append("位置=(").Append(p.x.ToString("F2")).Append(',').Append(p.z.ToString("F2"))
              .Append(") 朝向=(").Append(forward.x.ToString("F2")).Append(',')
              .Append(forward.z.ToString("F2")).Append(')');
            int ix, iz;
            var position = new Vector2(p.x, p.z);
            if (WorldState.GameCellOf(position, out ix, out iz))
            {
                sb.Append(" 游戏格=(").Append(ix).Append(',').Append(iz).Append(") 本格=「")
                  .Append(Occupant(ix, iz)).Append("」 +x=「").Append(Occupant(ix + 1, iz))
                  .Append("」 -x=「").Append(Occupant(ix - 1, iz))
                  .Append("」 +z=「").Append(Occupant(ix, iz + 1))
                  .Append("」 -z=「").Append(Occupant(ix, iz - 1)).Append('」');
            }
            return sb.ToString();
        }

        private static string Occupant(int ix, int iz)
        {
            var occupant = WorldState.GameCellOccupant(ix, iz);
            return occupant == null ? "" : occupant.name;
        }

        /// <summary>`ClientIngredientContainer.GetContents()` 里每个节点的名字（盘子装几样就报几样）。</summary>
        private static List<string> ContentsOf(GameObject gameObject)
        {
            var result = new List<string>();
            if (gameObject == null)
            {
                return result;
            }
            var container = gameObject.GetComponent("ClientIngredientContainer");
            if (container == null)
            {
                return result;
            }
            var contents = WorldState.Invoke(container, "GetContents") as Array;
            if (contents == null)
            {
                return result;
            }
            foreach (var node in contents)
            {
                result.Add(NodeName(node));
            }
            return result;
        }

        /// <summary>`AssembledDefinitionNode` 不是 Unity 对象；名字在它包着的订单节点上。</summary>
        private static string NodeName(object node)
        {
            if (node == null)
            {
                return "?";
            }
            var unity = node as UnityEngine.Object;
            if (unity != null && !string.IsNullOrEmpty(unity.name))
            {
                return unity.name;
            }
            var orderNode = WorldState.Get(node, "m_ingriedientOrderNode") as UnityEngine.Object;
            if (orderNode != null && !string.IsNullOrEmpty(orderNode.name))
            {
                return orderNode.name;
            }
            var name = WorldState.Get(node, "name") ?? WorldState.Get(node, "Name");
            if (name != null && !string.IsNullOrEmpty(name.ToString()))
            {
                return name.ToString();
            }
            return node.GetType().Name;
        }

        private static GameObject AsGameObject(UnityEngine.Object item)
        {
            var gameObject = item as GameObject;
            if (gameObject != null)
            {
                return gameObject;
            }
            var component = item as Component;
            return component == null ? null : component.gameObject;
        }

        private static bool IsTrue(object value)
        {
            return value is bool && (bool)value;
        }

        private static string Pos(Vector2 position)
        {
            return "(" + position.x.ToString("F2") + "," + position.y.ToString("F2") + ")";
        }
    }
}
