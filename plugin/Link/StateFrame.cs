using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 一帧世界状态的**上报形态**（`state_update`，任务 03）。
    ///
    /// 与 `WorldState` 落盘的那份 `wellcooked-state.json` 是同一批游戏对象、两种形状：
    /// 落盘那份是给人看与给 `scripts/export-kitchen.py` 用的，这一份必须严格对齐
    /// `protocol/protocol.md` §3.1 与 `bridge/src/protocol.c` 的字段名。
    /// 游戏事实的读取一律走 `WorldState.Get/Invoke/StaticMethod/FindAll`（一套反射），
    /// 不在这里另写一套取字段的办法。
    ///
    /// 这一轮**没映射**的字段（协议里都是可选项，缺省即"没有这个信息"）：
    /// - `chefs[].state`：游戏没有现成的 moving/idle 字段，硬猜不如不填；
    /// - `orders[].ingredients`：原料表在菜谱 DAG 里（`config/recipes.json`，任务 16 的产物），
    ///   调度侧从那里读，不必每帧重算一遍；
    /// - `resources[].chef_id`：客户端的工作台上没有 `m_interacters`（那是服务端字段）；
    /// - 锅的 `content`：取得到就填，取不到就省略。
    /// </summary>
    public static class StateFrame
    {
        /// <summary>资源（锅/砧板/盘子）变化慢，按这个间隔缓存，别每帧全场景找一遍。</summary>
        private const float ResourceTtl = 0.25f;

        /// <summary>协议 §3.1 的容量上限（对端也按这个截断，报多了只是浪费带宽）。</summary>
        private const int MaxOrders = 32;

        private static bool _orderIdMissingLogged;

        private static bool TryInt64(object value, out long result)
        {
            result = 0;
            try
            {
                result = Convert.ToInt64(value, CultureInfo.InvariantCulture);
                return true;
            }
            catch (Exception)
            {
                return false;
            }
        }

        private static string _resources;
        private static float _resourcesAt;

        public static string Build(float now)
        {
            object flow = WorldState.Flow();
            object team = WorldState.Team();

            var b = new JsonBuilder(2048);
            b.Obj();
            b.Field("type", MsgType.StateUpdate);
            b.FieldInt("timestamp", TcpLink.UnixSeconds());

            var timer = WorldState.Get(flow, "RoundTimer");
            var elapsed = timer == null ? null : WorldState.Get(timer, "TimeElapsed");
            if (elapsed != null)
            {
                b.FieldNum("game_time", ToDouble(elapsed));
            }

            var points = flow == null || team == null ? null : WorldState.Invoke(flow, "GetPoints", team);
            if (points != null)
            {
                b.FieldNum("score", ToDouble(points));
            }

            AppendChefs(b);
            AppendOrders(b, flow, team);

            b.Key("resources").Raw(Resources(now));

            b.EndObj();
            return b.ToString();
        }

        // ---------------- 厨师 ----------------

        private static void AppendChefs(JsonBuilder b)
        {
            b.Key("chefs").Arr();
            foreach (var chef in UnityEngine.Object.FindObjectsOfType<PlayerControls>())
            {
                var provider = chef.GetComponent<PlayerIDProvider>();
                if (provider == null)
                {
                    continue; // 协议里 id 必填；没有编号的厨师不属于我们的输入槽
                }

                var p = chef.transform.position;
                var carrier = chef.GetComponent<ServerPlayerAttachmentCarrier>();
                var held = carrier == null ? null : carrier.InspectCarriedItem();

                b.Obj();
                // 协议里的 `id` 是"厨师编号，同时也是输入槽编号"：游戏的玩家号就是这套编号
                // （实测 'One' → 0、'Two' → 1），与 `Takeover` 的槽号、Bridge 的 slot 一致。
                b.FieldInt("id", (int)provider.GetID());
                b.Key("position").Obj().FieldNum("x", p.x).FieldNum("y", p.z).EndObj();
                if (held == null)
                {
                    b.FieldNull("holding");
                }
                else
                {
                    b.Field("holding", held.name);
                }

                b.EndObj();
            }

            b.EndArr();
        }

        // ---------------- 订单 ----------------

        private static void AppendOrders(JsonBuilder b, object flow, object team)
        {
            b.Key("orders").Arr();

            var monitor = flow == null || team == null ? null : WorldState.Invoke(flow, "GetMonitorForTeam", team);
            var controller = monitor == null ? null : WorldState.Get(monitor, "OrdersController");
            var active = controller == null ? null : AsEnumerable(WorldState.Get(controller, "m_activeOrders"));
            if (active != null)
            {
                int written = 0;
                foreach (var order in active)
                {
                    if (order == null)
                    {
                        continue;
                    }

                    if (written >= MaxOrders)
                    {
                        break; // 协议 §3.1 的上限：对端也只留得下这么多
                    }

                    AppendOrder(b, order);
                    written++;
                }
            }

            b.EndArr();
        }

        private static void AppendOrder(JsonBuilder b, object order)
        {
            var idObject = WorldState.Get(order, "ID");
            var idValue = idObject == null ? null : WorldState.Get(idObject, "m_id");
            long idNumber = 0;
            bool hasId = idValue != null && TryInt64(idValue, out idNumber);

            b.Obj();
            if (hasId)
            {
                b.Field("id", "order_" + idNumber.ToString("D3", CultureInfo.InvariantCulture));
            }
            else if (!_orderIdMissingLogged)
            {
                // 读不到就别编：合成 order_000 会让两张单撞成一个 id，调度侧分不清
                _orderIdMissingLogged = true;
                WorldState.WarnOnce("上报: 有订单读不到 id，这一帧省略该字段");
            }

            // 菜名：订单自己带的菜谱条目（`RecipeListEntry.m_order`），与工序读的是同一处
            var entry = WorldState.Get(order, "RecipeListEntry");
            var node = entry == null ? null : WorldState.Get(entry, "m_order") as UnityEngine.Object;
            if (node == null)
            {
                b.FieldNull("recipe");
            }
            else
            {
                b.Field("recipe", node.name);
            }

            var remaining = WorldState.Get(order, "Remaining");
            if (remaining != null)
            {
                b.FieldNum("time_remaining", Math.Ceiling(ToDouble(remaining)));
            }

            b.EndObj();
        }

        // ---------------- 资源（锅 / 砧板 / 盘子） ----------------

        private static string Resources(float now)
        {
            if (_resources != null && now - _resourcesAt < ResourceTtl)
            {
                return _resources;
            }

            _resourcesAt = now;
            var b = new JsonBuilder(1024);
            b.Obj();
            AppendPots(b);
            AppendCuttingBoards(b);
            AppendPlateStack(b);
            b.EndObj();
            _resources = b.ToString();
            return _resources;
        }

        /// <summary>锅：`pot_&lt;N&gt;` 按位置排序编号，键名与 C 侧的前缀分类一致。</summary>
        private static void AppendPots(JsonBuilder b)
        {
            var getHandlers = WorldState.StaticMethod("ServerCookingHandler", "GetCookingHandlers");
            var handlers = getHandlers == null ? null : AsEnumerable(getHandlers.Invoke(null, null));
            if (handlers == null)
            {
                return;
            }

            var sorted = new List<KeyValuePair<Vector2, object>>();
            foreach (var handler in handlers)
            {
                if (handler == null)
                {
                    continue;
                }

                // ServerCookingHandler 不是 Component：位置从它交出的 CookingHandler 上取
                var component = handler as Component ?? WorldState.Get(handler, "GetCookingHandler") as Component;
                if (component == null)
                {
                    continue;
                }

                var p = component.transform.position;
                sorted.Add(new KeyValuePair<Vector2, object>(new Vector2(p.x, p.z), handler));
            }

            sorted.Sort(delegate(KeyValuePair<Vector2, object> a, KeyValuePair<Vector2, object> c)
            {
                int byX = a.Key.x.CompareTo(c.Key.x);
                return byX != 0 ? byX : a.Key.y.CompareTo(c.Key.y);
            });

            for (int i = 0; i < sorted.Count; i++)
            {
                var handler = sorted[i].Value;
                var component = handler as Component ?? WorldState.Get(handler, "GetCookingHandler") as Component;

                var progressValue = WorldState.Invoke(handler, "GetCookingProgress");
                double progress = progressValue == null ? 0.0 : ToDouble(progressValue);
                var cookedValue = WorldState.Invoke(handler, "IsCooked");
                var burningValue = WorldState.Invoke(handler, "IsBurning");
                bool cooked = IsTrue(cookedValue);
                bool burning = IsTrue(burningValue);

                var contents = AsEnumerable(WorldState.Invoke(component, "GetContents"));
                var names = new List<string>();
                if (contents != null)
                {
                    foreach (var item in contents)
                    {
                        var unity = item as UnityEngine.Object;
                        names.Add(unity != null ? unity.name : item == null ? "?" : item.ToString());
                    }
                }

                // 三个读数一个都没拿到时**不写 state**：推断成 free 会让调度侧以为锅空着
                bool known = progressValue != null || cookedValue != null || burningValue != null
                             || contents != null;
                string state = null;
                if (burning)
                {
                    state = "burning";
                }
                else if (cooked)
                {
                    state = "cooked";
                }
                else if (names.Count > 0 || progress > 0.001)
                {
                    state = "cooking";
                }
                else if (known)
                {
                    state = "free";
                }

                b.Key("pot_" + i).Obj();
                if (state != null)
                {
                    b.Field("state", state);
                }
                else
                {
                    WorldState.WarnOnce("上报: 有锅读不到状态，这一帧只报它的编号");
                }

                if (names.Count > 0)
                {
                    b.Field("content", string.Join("+", names.ToArray()));
                }

                if (state == "cooking")
                {
                    var totalValue = WorldState.Get(handler, "AccessCookingTime");
                    if (totalValue != null)
                    {
                        double total = ToDouble(totalValue);
                        if (total > 0)
                        {
                            b.FieldNum("time_left", total * (1.0 - Math.Min(1.0, Math.Max(0.0, progress))));
                        }
                    }
                }

                b.EndObj();
            }
        }

        /// <summary>砧板：`cutting_board_&lt;N&gt;`，有没有料看它自己挂的 `ClientAttachStation.m_item`。</summary>
        private static void AppendCuttingBoards(JsonBuilder b)
        {
            int index = 0;
            foreach (var found in WorldState.FindAll("ClientWorkstation"))
            {
                var unity = found as Component;
                if (unity == null || !IsChoppingBoard(unity.gameObject.name))
                {
                    continue;
                }

                var attach = unity.gameObject.GetComponent("ClientAttachStation");
                b.Key("cutting_board_" + index).Obj();
                if (attach == null)
                {
                    // 读不到"上面有没有料"就别写 state：写 free 会让人以为它能放东西
                    WorldState.WarnOnce("上报: 有砧板没有 ClientAttachStation，这一帧只报它的编号");
                }
                else
                {
                    b.Field("state", WorldState.Get(attach, "m_item") != null ? "occupied" : "free");
                }

                b.EndObj();
                index++;
            }
        }

        /// <summary>盘子堆：`plate_stack` 的 `count`（干净盘子有多少）。</summary>
        private static void AppendPlateStack(JsonBuilder b)
        {
            long count = 0;
            bool found = false;
            foreach (var stack in WorldState.FindAll("ClientCleanPlateStack"))
            {
                var size = WorldState.Invoke(stack, "GetSize");
                if (size == null)
                {
                    continue;
                }

                found = true;
                count += (long)ToDouble(size);
            }

            if (!found)
            {
                return;
            }

            b.Key("plate_stack").Obj().FieldInt("count", count).EndObj();
        }

        // ---------------- 小工具 ----------------

        private static bool IsChoppingBoard(string name)
        {
            return name.IndexOf("Chop", StringComparison.OrdinalIgnoreCase) >= 0
                   || name.IndexOf("Cut", StringComparison.OrdinalIgnoreCase) >= 0;
        }

        private static bool IsTrue(object value)
        {
            return value is bool && (bool)value;
        }

        private static double ToDouble(object value)
        {
            if (value == null)
            {
                return 0.0;
            }

            try
            {
                return Convert.ToDouble(value, CultureInfo.InvariantCulture);
            }
            catch (Exception)
            {
                return 0.0;
            }
        }

        private static IEnumerable AsEnumerable(object value)
        {
            return value as IEnumerable;
        }
    }
}
