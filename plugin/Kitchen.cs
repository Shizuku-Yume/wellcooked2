using System;
using System.Collections.Generic;
using HarmonyLib;
using HarmonyLib;
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
            foreach (var typeName in new[] { "ClientCleanPlateStack", "ClientPlate" })
            {
                foreach (var component in WorldState.FindAll(typeName))
                {
                    var unity = component as Component;
                    if (unity == null)
                    {
                        continue;
                    }

                    var p = unity.transform.position;
                    return new Station
                    {
                        Type = typeName,
                        Name = unity.gameObject.name,
                        Position = new Vector2(p.x, p.z),
                        Component = unity
                    };
                }
            }

            return null;
        }

        /// <summary>
        /// 空台面（可以放东西的普通台面）。
        ///
        /// 为什么需要它：OC2 里**不能拿着盘子直接取料**——得先把料放在台面上，
        /// 再拿盘子走过去"装盘"（实测：拿着盘子去料箱按 A，盘子被放下、只拿到生鱼，
        /// 于是交付口不认，得分一直 0）。
        /// </summary>
        public static Station FreeCounter(Vector2 near)
        {
            Station best = null;
            float bestDistance = float.MaxValue;
            foreach (var component in WorldState.FindAll("ClientAttachStation"))
            {
                var unity = component as Component;
                if (unity == null)
                {
                    continue;
                }

                var item = WorldState.Get(component, "m_item");
                if (item != null)
                {
                    continue; // 台面上有东西
                }

                // 料箱、盘子堆、出菜台身上也挂着 ClientAttachStation：
                // 挑到它们会把料放回料箱（实测"空台面"选中的就是料箱本身）。
                if (unity.GetComponent("ClientPickupItemSpawner") != null
                    || unity.GetComponent("ClientCleanPlateStack") != null
                    || unity.GetComponent("ClientPlateStation") != null
                    || unity.GetComponent("ClientDirtyPlateStack") != null)
                {
                    continue;
                }

                // 垃圾桶也挂着 ClientAttachStation，而且离料箱往往很近——实测"空台面"
                // 挑中的就是它，结果把鱼扔进了垃圾桶（ClientRubbishBin）。
                if (unity.GetComponent("ClientRubbishBin") != null
                    || unity.name.IndexOf("Bin", System.StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    continue;
                }

                var p = unity.transform.position;
                var position = new Vector2(p.x, p.z);

                // 离料箱/盘子堆太近的台面不能用：站在那儿按 A，游戏会在"料箱取料"和
                // "台面拿放"之间乱选，手上东西在鱼和盘子之间跳（实测就是这么卡住的）。
                bool tooCloseToCrate = false;
                foreach (var crate in Crates())
                {
                    if (Vector2.Distance(position, crate.Position) < 2.4f)
                    {
                        tooCloseToCrate = true;
                        break;
                    }
                }

                if (tooCloseToCrate)
                {
                    continue;
                }

                var stack = PlateStack();
                if (stack != null && Vector2.Distance(position, stack.Position) < 2.4f)
                {
                    continue;
                }

                float distance = Vector2.Distance(position, near);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best = new Station
                    {
                        Type = "ClientAttachStation",
                        Name = unity.gameObject.name,
                        Position = position,
                        Component = unity
                    };
                }
            }

            return best;
        }

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
            var helper = AccessTools.TypeByName("InteractWithItemHelper");
            var method = helper == null
                ? null
                : AccessTools.Method(helper, "GetFacingGridOccupant", new[] { typeof(Transform), typeof(int) });
            if (method == null)
            {
                return "?";
            }

            var collider = method.Invoke(null, new object[] { chef.transform, -1 }) as Component;
            return collider == null ? "（空）" : collider.gameObject.name;
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
            var helper = AccessTools.TypeByName("InteractWithItemHelper");
            var method = helper == null
                ? null
                : AccessTools.Method(helper, "GetFacingGridOccupant", new[] { typeof(Transform), typeof(int) });
            if (method == null)
            {
                return false;
            }

            var collider = method.Invoke(null, new object[] { chef.transform, -1 }) as Component;
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

        /// <summary>厨师手上拿的东西的名字（空手返回空串）。</summary>
        public static string HeldName(PlayerControls chef)
        {
            var carrier = chef.GetComponent<ServerPlayerAttachmentCarrier>();
            if (carrier == null)
            {
                return "";
            }

            var item = carrier.InspectCarriedItem();
            return item == null ? "" : item.name;
        }

        public static Vector2 PositionOf(PlayerControls chef)
        {
            var p = chef.transform.position;
            return new Vector2(p.x, p.z);
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
