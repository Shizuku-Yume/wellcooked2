using System;
using System.Collections.Generic;
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

                var p = unity.transform.position;
                var position = new Vector2(p.x, p.z);
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
