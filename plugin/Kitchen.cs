using System;
using System.Collections.Generic;
using System.Reflection;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 工序要用到的世界查询：料箱给什么料、盘子堆在哪、出菜台在哪、厨师手上拿着什么。
    ///
    /// 这些和 `WorldState` 落盘用的是同一批游戏对象，只是这里直接返回对象（不是 JSON），
    /// 供状态机每帧判断"这一步现在能不能做"。
    /// </summary>
    public static class Kitchen
    {
        // 反射查一次就存下来。下面三个函数每帧都被工序调用（`FacingObjectName`/`FacingObjectPosition`/
        // `WorkstationDebug`），每次重新 TypeByName + Method 是白花的开销；查不到时更糟 ——
        // AccessTools 每失败一次就写一行日志，于是"每帧一行警告"。`WorldState.StaticType/StaticMethod`
        // 自带缓存，也只记一次未命中。
        private static bool _lookupsDone;
        private static MethodInfo _facingLookup;
        private static Type _buttonIdType;
        private static Type _playerType;
        private static MethodInfo _getButtonLookup;

        private static void ResolveLookups()
        {
            if (_lookupsDone)
            {
                return;
            }

            _lookupsDone = true;
            _facingLookup = WorldState.StaticMethod("InteractWithItemHelper", "GetFacingGridOccupant",
                typeof(Transform), typeof(int));
            _buttonIdType = WorldState.StaticType("PlayerInputLookup+LogicalButtonID");
            _playerType = WorldState.StaticType("PlayerInputLookup+Player");
            _getButtonLookup = _buttonIdType == null || _playerType == null
                ? null
                : WorldState.StaticMethod("PlayerInputLookup", "GetButton", _buttonIdType, _playerType);
        }

        /// <summary>厨师面前那个占位物体（`InteractWithItemHelper.GetFacingGridOccupant`），没查到返回 null。</summary>
        private static Component FacingOccupant(PlayerControls chef)
        {
            ResolveLookups();
            return _facingLookup == null
                ? null
                : _facingLookup.Invoke(null, new object[] { chef.transform, -1 }) as Component;
        }

        /// <summary>
        /// 站在工位旁边该去哪：工位所在游戏格的四个邻居里，挑一个我们网格里可走、
        /// 且离当前位置最近的。
        ///
        /// 为什么不能直接走到工位本身：交互判定（`InteractWithItemHelper.GetFacingGridOccupant`）
        /// 取的是"厨师所在格的邻居格里、朝向最对齐的那一格的占位物体"，**不看距离**；
        /// 厨师站在工位格里或者隔两格都交互不到，必须站到相邻格并朝着它。
        /// </summary>
        public static bool ApproachPosition(Vector2 station, Vector2 from, out Vector2 approach)
        {
            approach = station;
            int cx, cz;
            if (!WorldState.GameCellOf(station, out cx, out cz))
            {
                return false;
            }

            var offsets = new[]
            {
                new[] { 1, 0 }, new[] { -1, 0 }, new[] { 0, 1 }, new[] { 0, -1 }
            };

            float best = float.MaxValue;
            bool found = false;
            foreach (var offset in offsets)
            {
                Vector2 candidate;
                if (!WorldState.GameCellPosition(cx + offset[0], cz + offset[1], out candidate))
                {
                    continue;
                }

                if (!WorldState.IsWalkablePoint(candidate.x, candidate.y))
                {
                    continue;
                }

                float distance = Vector2.Distance(candidate, from);
                if (distance < best)
                {
                    best = distance;
                    approach = candidate;
                    found = true;
                }
            }

            return found;
        }

        /// <summary>一个可用的工位：类型、物体名、水平面位置、以及（料箱才有）它给的料。</summary>
        public sealed class Station
        {
            public string Type;
            public string Name;
            public string Gives;
            public Vector2 Position;
            public Component Component;
        }

        /// <summary>场上所有料箱（DispenserCrate 上的 ClientPickupItemSpawner）。</summary>
        public static List<Station> Crates()
        {
            var result = new List<Station>();
            foreach (var component in WorldState.FindAll("ClientPickupItemSpawner"))
            {
                var unity = component as Component;
                if (unity == null)
                {
                    continue;
                }

                var prefab = WorldState.Invoke(component, "GetItemPrefab") as UnityEngine.Object;
                var p = unity.transform.position;
                result.Add(new Station
                {
                    Type = "ClientPickupItemSpawner",
                    Name = unity.gameObject.name,
                    Gives = prefab == null ? "" : prefab.name,
                    Position = new Vector2(p.x, p.z),
                    Component = unity
                });
            }

            return result;
        }

        /// <summary>盘子堆（干净的盘子，拿一个就少一个）。</summary>
        public static Station PlateStack()
        {
            // 挑**最近的有盘子的干净盘子堆**：原来的写法取第一个 ClientCleanPlateStack，
            // 结果挑到回收口（workstation_plate_return）或者空的堆，站在那儿按 A 什么也拿不到
            // （实测 4/6 卡死在 equipment_plate_01 / workstation_plate_return）。
            var from = LastKnownPosition;
            Station best = null;
            float bestDistance = float.MaxValue;

            foreach (var component in WorldState.FindAll("ClientCleanPlateStack"))
            {
                var unity = component as Component;
                if (unity == null)
                {
                    continue;
                }

                var name = unity.gameObject.name;
                if (name.IndexOf("return", StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    continue; // 洗盘回收口，不是拿盘子的地方
                }

                var count = WorldState.Get(component, "GetCount");
                if (count is int plates && plates <= 0)
                {
                    continue; // 空的
                }


                var p = unity.transform.position;
                var position = new Vector2(p.x, p.z);
                float distance = Vector2.Distance(position, from);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best = new Station
                    {
                        Type = "ClientCleanPlateStack",
                        Name = name,
                        Position = position,
                        Component = unity
                    };
                }
            }

            if (best != null)
            {
                return best;
            }

            // 兜底：这关可能没有"盘子堆"，盘子是散放在台面上的单个 ClientPlate。
            // 排除被厨师拿着的那些（挂在厨师下面的）。
            foreach (var component in WorldState.FindAll("ClientPlate"))
            {
                var unity = component as Component;
                if (unity == null || HeldByChef(unity))
                {
                    continue;
                }

                var name = unity.gameObject.name;
                if (name.IndexOf("return", StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    continue;
                }

                var p = unity.transform.position;
                var position = new Vector2(p.x, p.z);
                float distance = Vector2.Distance(position, from);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best = new Station
                    {
                        Type = "ClientPlate",
                        Name = name,
                        Position = position,
                        Component = unity
                    };
                }
            }

            return best;
        }

        /// <summary>这个东西是不是被厨师拿在手里（沿父级找 PlayerControls）。</summary>
        private static bool HeldByChef(Component component)
        {
            var current = component.transform;
            for (int i = 0; i < 6 && current != null; i++)
            {
                if (current.GetComponent("PlayerControls") != null)
                {
                    return true;
                }

                current = current.parent;
            }

            return false;
        }

        /// <summary>最近一次已知的厨师位置，用来挑"离我最近的工位"。</summary>
        public static Vector2 LastKnownPosition = new Vector2(0f, 0f);

        /// <summary>出菜台（交付口）。</summary>
        public static Station DeliveryStation()
        {
            foreach (var component in WorldState.FindAll("ClientPlateStation"))
            {
                var unity = component as Component;
                if (unity == null)
                {
                    continue;
                }

                var p = unity.transform.position;
                return new Station
                {
                    Type = "ClientPlateStation",
                    Name = unity.gameObject.name,
                    Position = new Vector2(p.x, p.z),
                    Component = unity
                };
            }

            return null;
        }

        /// <summary>
        /// 厨师"面前"是什么（游戏自己判的，`InteractWithItemHelper.GetFacingGridOccupant`）：
        /// 取厨师所在格的邻居格里、朝向最对齐的那一格的占位物体。按 A 之前用它确认。
        /// </summary>
        public static string FacingObjectName(PlayerControls chef)
        {
            ResolveLookups();
            if (_facingLookup == null)
            {
                return "?";
            }

            var collider = FacingOccupant(chef);
            return collider == null ? "（空）" : collider.gameObject.name;
        }

        /// <summary>
        /// 砧板（挂着 `ClientWorkstation` 的台面）。鱼要在这上面切——生鱼不能直接装盘。
        /// 优先挑上面已经有料的那块（多半就是自己刚放上去的）。
        /// </summary>
        public static Station ChoppingBoard()
        {
            Station fallback = null;
            foreach (var component in WorldState.FindAll("ClientWorkstation"))
            {
                var unity = component as Component;
                if (unity == null)
                {
                    continue;
                }

                var attach = unity.GetComponent("ClientAttachStation");
                var item = attach == null ? null : WorldState.Get(attach, "m_item") as Component;
                var p = unity.transform.position;
                var station = new Station
                {
                    Type = "ChoppingBoard",
                    Name = unity.gameObject.name,
                    Position = new Vector2(p.x, p.z),
                    Component = unity
                };

                if (item != null)
                {
                    return station; // 上面有东西，就是它了
                }

                if (fallback == null)
                {
                    fallback = station;
                }
            }

            return fallback;
        }

        /// <summary>
        /// 砧板上那块料的切割状态。`m_progress` 是**整数阶段**（不是 0..1 的浮点），
        /// `HasFinished()` = `m_progress == m_stages - 1`；`m_subProgress` 是当前阶段里切了几刀。
        /// </summary>
        public static bool WorkState(Vector2 station, out bool finished, out string description)
        {
            finished = false;
            description = "?";
            int ix, iz;
            if (!WorldState.GameCellOf(station, out ix, out iz))
            {
                return false;
            }

            var occupant = WorldState.GameCellOccupant(ix, iz);
            var workstation = occupant == null ? null : occupant.GetComponent("ClientWorkstation");
            if (workstation == null)
            {
                return false;
            }

            // 料在 attach station 上（workstation.m_item 只在**有厨师正在用**的时候才非空，
            // 平时是 null，所以不能拿它判"板上有没有料"）；进度在同一个物体上的
            // ClientWorkableItem 组件里（attach station 的 m_item 只是 ClientPhysicalAttachment，
            // 直接读 m_progress 只会得到一堆 AccessTools 警告）。
            var attach = occupant.GetComponent("ClientAttachStation");
            var attached = attach == null ? null : WorldState.Get(attach, "m_item") as Component;
            if (attached == null)
            {
                return false;
            }

            var workableItem = attached.gameObject.GetComponent("ClientWorkableItem");
            if (workableItem == null)
            {
                return false;
            }

            var progressValue = WorldState.Get(workableItem, "m_progress");
            var subValue = WorldState.Get(workableItem, "m_subProgress");
            int progress = progressValue is int p0 ? p0 : 0;
            int sub = subValue is int s0 ? s0 : 0;

            int stages = 0;
            var workable = WorldState.Get(workableItem, "m_workable");
            if (workable != null)
            {
                var stagesValue = WorldState.Get(workable, "m_stages");
                if (stagesValue is int stagesInt)
                {
                    stages = stagesInt;
                }
            }

            finished = WorldState.Get(workableItem, "HasFinished") is bool done && done;
            description = "阶段 " + progress + "/" + Mathf.Max(stages - 1, 1)
                          + " 子进度 " + sub + (finished ? " 已切好" : "");
            return true;
        }

        /// <summary>
        /// 游戏自己怎么看这个工位：`ClientWorkstation.IsBeingUsed()`（有没有厨师正在用）
        /// 和交互键状态。用来区分"我们的按键没到游戏"和"到了但没开始工作"。
        /// </summary>
        public static string WorkstationDebug(Vector2 station, int slot)
        {
            var parts = new List<string>();
            int ix, iz;
            if (WorldState.GameCellOf(station, out ix, out iz))
            {
                var occupant = WorldState.GameCellOccupant(ix, iz);
                if (occupant != null)
                {
                    var workstation = occupant.GetComponent("ClientWorkstation");
                    if (workstation != null)
                    {
                        var beingUsed = WorldState.Get(workstation, "IsBeingUsed");
                        parts.Add("在用=" + (beingUsed ?? "?"));
                    }
                }
            }

            ResolveLookups();
            if (_getButtonLookup != null)
            {
                var id = Enum.Parse(_buttonIdType, "WorkstationInteract");
                var player = Enum.ToObject(_playerType, slot);
                var button = _getButtonLookup.Invoke(null, new[] { id, player });
                if (button != null)
                {
                    var isDown = WorldState.Get(button, "IsDown");
                    var justPressed = WorldState.Get(button, "JustPressed");
                    parts.Add("游戏看到 X=" + (isDown ?? "?") + "/刚按=" + (justPressed ?? "?"));
                }
            }

            return parts.Count == 0 ? "?" : string.Join(" ", parts.ToArray());
        }

        /// <summary>工位那一格上放着什么（`ClientAttachStation.m_item` 的名字）。</summary>
        public static string StationItemName(Vector2 station)
        {
            int ix, iz;
            if (!WorldState.GameCellOf(station, out ix, out iz))
            {
                return "?";
            }

            var occupant = WorldState.GameCellOccupant(ix, iz);
            if (occupant == null)
            {
                return "（没东西）";
            }

            var stationComponent = occupant.GetComponent("ClientAttachStation");
            if (stationComponent == null)
            {
                return "（不是台面）";
            }

            var item = WorldState.Get(stationComponent, "m_item") as Component;
            return item == null ? "（空台面）" : item.gameObject.name;
        }

        /// <summary>厨师面前那个东西的位置（判"是不是工位"用它比格子，比名字靠谱）。</summary>
        public static bool FacingObjectPosition(PlayerControls chef, out Vector2 position)
        {
            position = Vector2.zero;
            ResolveLookups();
            if (_facingLookup == null)
            {
                return false;
            }

            var collider = FacingOccupant(chef);
            if (collider == null)
            {
                return false;
            }

            var p = collider.transform.position;
            position = new Vector2(p.x, p.z);
            return true;
        }

        /// <summary>
        /// 厨师手上东西的摘要：盘子会把里面的东西带出来，如 `Plate 1 (4)[SushiFish]`。
        /// 只报名字分不清"拿着生鱼"和"拿着装了鱼的盘子"（实测装盘那步就这么误判成成功）。
        /// </summary>
        public static string HeldSummary(PlayerControls chef)
        {
            var carrier = chef.GetComponent<ServerPlayerAttachmentCarrier>();
            if (carrier == null)
            {
                return "";
            }

            var item = carrier.InspectCarriedItem() as UnityEngine.Object;
            if (item == null)
            {
                return "";
            }

            var gameObject = item as GameObject;
            if (gameObject == null)
            {
                var component = item as Component;
                gameObject = component == null ? null : component.gameObject;
            }

            if (gameObject == null)
            {
                return "";
            }

            string name = gameObject.name;
            var container = gameObject.GetComponent("ClientIngredientContainer");
            if (container == null)
            {
                return name;
            }

            var contents = WorldState.Invoke(container, "GetContents") as Array;
            if (contents == null || contents.Length == 0)
            {
                return name;
            }

            var first = contents.GetValue(0) as UnityEngine.Object;
            return name + "[" + (first == null ? "?" : first.name) + "]";
        }

        /// <summary>当前订单要的那道菜（取第一个订单的菜名；没有订单返回空串）。</summary>
        public static string FirstOrderDish()
        {
            var controller = WorldState.FirstOrderController();
            if (controller == null)
            {
                return "";
            }

            var recipes = WorldState.ActiveRecipes(controller);
            return recipes.Count == 0 ? "" : recipes[0];
        }
    }
}
