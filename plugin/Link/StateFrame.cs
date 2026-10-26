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

        /// <summary>一个容器递归展开的内容名条数上限；C 侧 shm 只留得下 4 条。</summary>
        private const int MaxContents = 8;

        /// <summary>协议 §3.1 的资源键上限；超出后这一帧只报前 N 个并记一次日志。</summary>
        private const int MaxResources = 96;

        private static bool _orderIdMissingLogged;
        private static bool _resourceOverflowLogged;
        private static int _resourceCount;

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

            double sec = GameTime();
            b.FieldNum("game_time", sec);
            b.FieldInt("game_time_ms", (long)Math.Round(sec * 1000.0));

            AppendRoundClock(b, flow);

            var points = flow == null || team == null ? null : WorldState.Invoke(flow, "GetPoints", team);
            if (points != null)
            {
                b.FieldNum("score", ToDouble(points));
            }

            AppendChefs(b);
            AppendOrders(b, flow, team);
            OrderResults.AppendTo(b);

            b.Key("resources").Raw(Resources(now));

            b.EndObj();
            return b.ToString();
        }

        /// <summary>
        /// 本局游戏时钟（秒）。与 `game_time_ms` 同源：`RoundTimer.TimeElapsed`，
        /// 暂停时 `TimeManager` 把该层的 delta 乘成 0，所以它不会走。
        /// 插件的动作超时也用它，暂停时不会把正在执行的动作判超时。
        /// </summary>
        public static float GameTime()
        {
            object flow = WorldState.Flow();
            object timer = flow == null ? null : WorldState.Get(flow, "RoundTimer");
            object elapsed = timer == null ? null : WorldState.Get(timer, "TimeElapsed");
            return elapsed == null ? 0f : (float)ToDouble(elapsed);
        }

        /// <summary>
        /// 回合剩余与上限（G8）：`ServerRoundTimer.m_timeLeft/m_timeLimit`。两个数一起给，
        /// 缺一个就当没有——只报剩余会让调度侧以为"局时未知"。
        /// </summary>
        private static void AppendRoundClock(JsonBuilder b, object flow)
        {
            var timer = flow == null ? null : WorldState.Get(flow, "RoundTimer");
            var left = timer == null ? null : WorldState.Get(timer, "m_timeLeft");
            var limit = timer == null ? null : WorldState.Get(timer, "m_timeLimit");
            if (limit == null && flow != null)
            {
                limit = WorldState.Get(flow, "m_timeLimit");
            }
            if (left == null || limit == null)
            {
                return;
            }
            b.FieldNum("round_time_left", ToDouble(left));
            b.FieldNum("round_time_limit", ToDouble(limit));
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
                var forward = chef.transform.forward;
                b.FieldNum("facing", Math.Atan2(forward.x, forward.z));
                if (held == null)
                {
                    b.FieldNull("holding");
                }
                else
                {
                    b.Field("holding", held.name);
                    // 只报名字分不清"拿着生鱼"和"拿着装了鱼的盘子"（任务 09 G4）
                    var contents = Kitchen.ItemContents(AsGameObject(held));
                    WriteContents(b, "holding_contents", contents);
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

            // 每单分值（G7）：调度按 80/20 的菜价排序时有据可依，不再一律 fallback 10
            var score = entry == null ? null : WorldState.Get(entry, "m_scoreForMeal");
            if (score != null)
            {
                b.FieldInt("score", (long)ToDouble(score));
            }

            b.EndObj();
        }

        // ---------------- 资源（锅 / 砧板 / 盘子 / 台面 / 容器） ----------------

        private static string Resources(float now)
        {
            if (_resources != null && now - _resourcesAt < ResourceTtl)
            {
                return _resources;
            }

            _resourcesAt = now;
            _resourceCount = 0;

            var b = new JsonBuilder(4096);
            b.Obj();
            // 先报"工位"，再报"工位上/手里的东西"；covered 里是已经报过的物体，
            // 后面的 container_<N> 只捡剩下的（掉在地上、没人拿的容器）。
            var covered = new HashSet<int>();
            AppendCookers(b, covered);
            AppendCuttingBoards(b, covered);
            AppendPlateStack(b);
            AppendDirtyPlates(b);
            AppendSinks(b);
            AppendCounters(b, covered);
            AppendCarried(b, covered);
            AppendContainers(b, covered);
            b.EndObj();
            _resources = b.ToString();
            return _resources;
        }

        /// <summary>
        /// 灶台：`pot_&lt;N&gt;` 是**槽位**不是"当前枚举到的锅"。枚举 `ClientCookingStation`
        /// 而不是 `ServerCookingHandler` 的静态表，空灶台也在（G1 的直接原因：
        /// `ServerCookingHandler` 没料时不 enabled，静态表里根本没有它）。
        /// 编号与 `kitchen.json` 的 `cooker-N` 一样按 x、z 排。
        /// </summary>
        private static void AppendCookers(JsonBuilder b, HashSet<int> covered)
        {
            var stations = Kitchen.SortedCookingStations();
            for (int i = 0; i < stations.Count; i++)
            {
                if (!BeginResource(b, "pot_" + i))
                {
                    return;
                }

                var station = stations[i].Component;
                var item = WorldState.Get(station, "m_itemPot");
                var itemComponent = item as Component;
                var itemGo = itemComponent == null ? null : itemComponent.gameObject;
                var contents = itemGo == null ? new List<string>() : Kitchen.ItemContents(itemGo);

                b.Field("kind", "cooker");
                WritePos(b, stations[i].Position);
                b.Field("state", CookerState(itemGo, contents.Count > 0));
                if (itemGo == null)
                {
                    // 空灶台：明确报空数组，调度侧据此知道"这只锅现在不在灶上"
                    WriteContents(b, "contents", contents);
                    b.EndObj();
                    continue;
                }

                b.Field("item", itemGo.name);
                WriteCooking(b, itemGo);
                WriteContents(b, "contents", contents);
                WriteContentCompat(b, contents);
                covered.Add(itemGo.GetInstanceID());
                b.EndObj();
            }
        }

        /// <summary>灶台状态：烧焦 &gt; 熟 &gt; 有料/在加热 &gt; 空。</summary>
        private static string CookerState(GameObject item, bool hasContents)
        {
            if (item == null)
            {
                return "empty";
            }

            var handler = item.GetComponent("ClientCookingHandler");
            if (handler == null)
            {
                return hasContents ? "occupied" : "free";
            }
            if (IsTrue(WorldState.Invoke(handler, "IsBurning")))
            {
                return "burning";
            }

            // 客户端也是秒（`CookingStateMessage.m_cookingProgress` 来自 server 的
            // `GetCookingProgress()`，实测同一口锅到熟是 12.1 秒），除总时长才归一化。
            double raw = ToDouble(WorldState.Invoke(handler, "GetCookingProgress"));
            double total = ToDouble(WorldState.Get(handler, "AccessCookingTime"));
            if (hasContents && total > 0.0001 && raw >= total - 0.001)
            {
                return "cooked";
            }
            if (hasContents || raw > 0.001)
            {
                return "cooking";
            }
            return "free";
        }

        /// <summary>锅的进度/剩余/类型（G6）。没有 `ClientCookingHandler` 就什么都不写。</summary>
        private static void WriteCooking(JsonBuilder b, GameObject item)
        {
            if (item == null)
            {
                return;
            }

            var handler = item.GetComponent("ClientCookingHandler");
            if (handler == null)
            {
                return;
            }

            double raw = ToDouble(WorldState.Invoke(handler, "GetCookingProgress"));
            double total = ToDouble(WorldState.Get(handler, "AccessCookingTime"));
            double progress = total > 0.0001 ? raw / total : raw;
            b.FieldNum("progress", Clamp01(progress));
            if (total > raw + 0.001)
            {
                b.FieldNum("time_left", total - raw);
            }

            var step = StepName(WorldState.Get(handler, "AccessCookingType"));
            if (step != null)
            {
                b.Field("step", step);
            }
        }

        private static bool BeginResource(JsonBuilder b, string key)
        {
            if (_resourceCount >= MaxResources)
            {
                if (!_resourceOverflowLogged)
                {
                    _resourceOverflowLogged = true;
                    WorldState.WarnOnce("上报: 资源数超过 " + MaxResources + "，多余的这一帧不报");
                }
                return false;
            }

            _resourceCount++;
            b.Key(key).Obj();
            return true;
        }

        private static void WritePos(JsonBuilder b, Vector2 position)
        {
            b.Key("pos").Obj().FieldNum("x", position.x).FieldNum("y", position.y).EndObj();
        }

        private static void WriteContents(JsonBuilder b, string key, List<string> names)
        {
            b.Key(key).Arr();
            if (names != null)
            {
                int n = Math.Min(names.Count, MaxContents);
                for (int i = 0; i < n; i++)
                {
                    b.Item(names[i]);
                }
            }
            b.EndArr();
        }

        /// <summary>`content` 只是 `contents` 的兼容拼串形式，C 侧只有 32 字节。</summary>
        private static void WriteContentCompat(JsonBuilder b, List<string> names)
        {
            if (names == null || names.Count == 0)
            {
                return;
            }
            b.Field("content", string.Join("+", names.ToArray()));
        }

        /// <summary>砧板：`cutting_board_&lt;N&gt;`，有没有料看它自己挂的 `ClientAttachStation.m_item`。</summary>
        private static void AppendCuttingBoards(JsonBuilder b, HashSet<int> covered)
        {
            int index = 0;
            foreach (var board in Kitchen.ChoppingBoards())
            {
                if (!BeginResource(b, "cutting_board_" + index))
                {
                    return;
                }

                var unity = board.Component;
                var attach = unity.gameObject.GetComponent("ClientAttachStation");
                b.Field("kind", "board");
                WritePos(b, board.Position);
                if (attach == null)
                {
                    // 读不到"上面有没有料"就别写 state：写 free 会让人以为它能放东西
                    WorldState.WarnOnce("上报: 有砧板没有 ClientAttachStation，这一帧只报它的编号");
                }
                else
                {
                    var item = WorldState.Get(attach, "m_item") as Component;
                    b.Field("state", item != null ? "occupied" : "free");
                    if (item != null)
                    {
                        var itemGo = item.gameObject;
                        var contents = Kitchen.ItemContents(itemGo);
                        b.Field("item", itemGo.name);
                        var workableItem = itemGo.GetComponent("ClientWorkableItem");
                        if (workableItem != null)
                        {
                            // 游戏自己的 `GetProgress()` 就是 `m_progress / (m_stages - 1)`
                            // （ClientWorkableItem.GetProgress），不必再猜字段类型。
                            var progressValue = WorldState.Invoke(workableItem, "GetProgress");
                            if (progressValue != null)
                            {
                                b.FieldNum("progress", Clamp01(ToDouble(progressValue)));
                            }
                        }
                        WriteContents(b, "contents", contents);
                        WriteContentCompat(b, contents);
                        covered.Add(itemGo.GetInstanceID());
                    }
                }
                b.EndObj();
                index++;
            }
        }

        /// <summary>
        /// 盘子堆：`plate_stack` 的 `count`（干净盘子有多少）。
        /// `ClientCleanPlateStack.GetSize()` 不存在（任务 09 G2 实测三连未找到）；
        /// 计数在基类 `ClientPlateStackBase.GetCount()`。
        /// </summary>
        private static void AppendPlateStack(JsonBuilder b)
        {
            long count = 0;
            bool found = false;
            var position = Vector2.zero;
            foreach (var stack in WorldState.FindAll("ClientCleanPlateStack"))
            {
                var size = WorldState.Invoke(stack, "GetCount");
                if (size == null)
                {
                    continue;
                }

                found = true;
                count += (long)ToDouble(size);
                var unity = stack as Component;
                if (unity != null)
                {
                    var p = unity.transform.position;
                    position = new Vector2(p.x, p.z);
                }
            }

            if (!found)
            {
                return;
            }

            if (!BeginResource(b, "plate_stack"))
            {
                return;
            }
            b.Field("kind", "plate_stack");
            WritePos(b, position);
            b.FieldInt("count", count);
            b.EndObj();
        }

        /// <summary>脏盘台：`dirty_plates` 上的盘子数。</summary>
        private static void AppendDirtyPlates(JsonBuilder b)
        {
            long count = 0;
            bool found = false;
            var position = Vector2.zero;
            foreach (var stack in WorldState.FindAll("ClientDirtyPlateStack"))
            {
                var cVal = WorldState.Invoke(stack, "GetCount");
                if (cVal == null)
                {
                    cVal = WorldState.Get(stack, "m_plateCount");
                }
                if (cVal != null)
                {
                    found = true;
                    count += (long)ToDouble(cVal);
                    var unity = stack as Component;
                    if (unity != null)
                    {
                        var p = unity.transform.position;
                        position = new Vector2(p.x, p.z);
                    }
                }
            }
            if (!found)
            {
                // 读不到脏盘台就不写：写 free/0 会让调度侧以为"没有脏盘要洗"。
                WorldState.WarnOnce("上报: 读不到脏盘台数量，这一帧不写 dirty_plates");
                return;
            }
            if (!BeginResource(b, "dirty_plates"))
            {
                return;
            }
            b.Field("kind", "dirty_plates");
            WritePos(b, position);
            b.Field("state", count > 0 ? "occupied" : "free");
            b.FieldInt("count", count);
            b.EndObj();
        }

        /// <summary>水槽：`sink_&lt;N&gt;` 的进度与盘数。</summary>
        private static void AppendSinks(JsonBuilder b)
        {
            int index = 0;
            foreach (var found in WorldState.FindAll("ClientWashingStation"))
            {
                var unity = found as Component;
                if (unity == null)
                {
                    continue;
                }
                if (!BeginResource(b, "sink_" + index))
                {
                    return;
                }

                var isWashingVal = WorldState.Get(unity, "m_isWashing");
                bool isWashing = isWashingVal is bool bw && bw;
                var plateCountVal = WorldState.Get(unity, "m_plateCount");
                int plateCount = plateCountVal is int pc ? pc : 0;
                var timerVal = WorldState.Get(unity, "m_cleaningTimer");
                float timer = timerVal is float tf ? tf : 0f;
                float total = 3f;
                var ws = WorldState.Get(unity, "m_washingStation");
                if (ws != null)
                {
                    var totVal = WorldState.Get(ws, "m_cleanPlateTime");
                    if (totVal is float tff && tff > 0f)
                    {
                        total = tff;
                    }
                }
                double prog = total > 0f ? Math.Min(1.0, Math.Max(0.0, (double)timer / total)) : 0.0;
                var p = unity.transform.position;
                b.Field("kind", "sink");
                WritePos(b, new Vector2(p.x, p.z));
                b.Field("state", isWashing ? "washing" : (plateCount > 0 ? "occupied" : "free"));
                b.FieldInt("count", plateCount);
                b.FieldNum("progress", prog);
                b.EndObj();
                index++;
            }
        }

        /// <summary>
        /// 台面（G5）：只报**上面有东西**的普通工位。空台面是静态的，位置在
        /// `kitchen.json` 里；运行时帧里没有 `counter_N` 就等于空闲。
        /// 灶台/砧板/水槽/盘堆上的东西已经由各自的键报过，这里跳过。
        /// </summary>
        private static void AppendCounters(JsonBuilder b, HashSet<int> covered)
        {
            var list = new List<KeyValuePair<Vector2, Component>>();
            foreach (var found in WorldState.FindAll("ClientAttachStation"))
            {
                var unity = found as Component;
                if (unity == null || IsDedicatedStation(unity.gameObject))
                {
                    continue;
                }

                var item = WorldState.Get(unity, "m_item") as Component;
                if (item == null)
                {
                    continue;
                }

                var p = unity.transform.position;
                list.Add(new KeyValuePair<Vector2, Component>(new Vector2(p.x, p.z), unity));
            }
            list.Sort(CompareStation);

            int index = 0;
            foreach (var entry in list)
            {
                var item = WorldState.Get(entry.Value, "m_item") as Component;
                if (item == null)
                {
                    continue;
                }
                if (!BeginResource(b, "counter_" + index))
                {
                    return;
                }

                var itemGo = item.gameObject;
                var contents = Kitchen.ItemContents(itemGo);
                b.Field("kind", "counter");
                WritePos(b, entry.Key);
                b.Field("state", "occupied");
                b.Field("item", itemGo.name);
                WriteContents(b, "contents", contents);
                WriteContentCompat(b, contents);
                covered.Add(itemGo.GetInstanceID());
                b.EndObj();
                index++;
            }
        }

        /// <summary>
        /// 厨师手里的容器/锅（G1 "carried pots as carried by chef id" / G4）：
        /// 锅被端走时灶台只剩 `state=empty`，这只锅本身在这里、`chef_id` 必填。
        /// 手上是普通料（不是容器）时不报——`chefs[].holding_contents` 已经说了。
        /// </summary>
        private static void AppendCarried(JsonBuilder b, HashSet<int> covered)
        {
            foreach (var chef in UnityEngine.Object.FindObjectsOfType<PlayerControls>())
            {
                var provider = chef.GetComponent<PlayerIDProvider>();
                if (provider == null)
                {
                    continue;
                }

                var carrier = chef.GetComponent<ServerPlayerAttachmentCarrier>();
                var held = carrier == null ? null : carrier.InspectCarriedItem() as UnityEngine.Object;
                var itemGo = AsGameObject(held);
                if (itemGo == null)
                {
                    continue;
                }
                if (itemGo.GetComponent("ClientIngredientContainer") == null
                    && itemGo.GetComponent("ClientCookableContainer") == null)
                {
                    continue;
                }

                int chefId = (int)provider.GetID();
                if (!BeginResource(b, "carried_" + chefId))
                {
                    return;
                }

                var contents = Kitchen.ItemContents(itemGo);
                var p = chef.transform.position;
                b.Field("kind", "carried");
                b.FieldInt("chef_id", chefId);
                WritePos(b, new Vector2(p.x, p.z));
                b.Field("state", "carried");
                b.Field("item", itemGo.name);
                WriteCooking(b, itemGo);
                WriteContents(b, "contents", contents);
                WriteContentCompat(b, contents);
                covered.Add(itemGo.GetInstanceID());
                b.EndObj();
            }
        }

        /// <summary>
        /// 剩下的容器（G5，掉在地上/没挂到工位的）：前面各段已经报过的物体不再重复，
        /// 按位置排序后编号。位置是它们唯一稳定的对照键，调度侧别按序号认。
        /// </summary>
        private static void AppendContainers(JsonBuilder b, HashSet<int> covered)
        {
            var list = new List<KeyValuePair<Vector2, Component>>();
            foreach (var found in WorldState.FindAll("ClientIngredientContainer"))
            {
                var unity = found as Component;
                if (unity == null)
                {
                    continue;
                }
                var go = unity.gameObject;
                if (covered.Contains(go.GetInstanceID()) || IsDedicatedStation(go))
                {
                    continue;
                }

                var p = go.transform.position;
                list.Add(new KeyValuePair<Vector2, Component>(new Vector2(p.x, p.z), unity));
            }
            list.Sort(CompareStation);

            int index = 0;
            foreach (var entry in list)
            {
                var go = entry.Value.gameObject;
                if (!BeginResource(b, "container_" + index))
                {
                    return;
                }

                var contents = Kitchen.ItemContents(go);
                b.Field("kind", "container");
                WritePos(b, entry.Key);
                b.Field("state", contents.Count > 0 ? "occupied" : "empty");
                b.Field("item", go.name);
                WriteContents(b, "contents", contents);
                WriteContentCompat(b, contents);
                b.EndObj();
                index++;
            }
        }

        /// <summary>这个物体自己的状态由别的键报（灶台/砧板/水槽/盘堆/料箱），别当成普通台面。</summary>
        private static bool IsDedicatedStation(GameObject gameObject)
        {
            return gameObject.GetComponent("ClientCookingStation") != null
                   || gameObject.GetComponent("ClientWashingStation") != null
                   || gameObject.GetComponent("ClientCleanPlateStack") != null
                   || gameObject.GetComponent("ClientDirtyPlateStack") != null
                   || gameObject.GetComponent("ClientPickupItemSpawner") != null
                   || (gameObject.GetComponent("ClientWorkstation") != null
                       && Kitchen.IsChoppingBoard(gameObject.name));
        }

        private static int CompareStation(KeyValuePair<Vector2, Component> a,
                                          KeyValuePair<Vector2, Component> b)
        {
            int byX = a.Key.x.CompareTo(b.Key.x);
            if (byX != 0)
            {
                return byX;
            }
            int byZ = a.Key.y.CompareTo(b.Key.y);
            if (byZ != 0)
            {
                return byZ;
            }
            string an = a.Value == null ? "" : a.Value.gameObject.name;
            string bn = b.Value == null ? "" : b.Value.gameObject.name;
            return string.CompareOrdinal(an, bn);
        }

        // ---------------- 小工具 ----------------

        private static GameObject AsGameObject(UnityEngine.Object item)
        {
            if (item == null)
            {
                return null;
            }
            var gameObject = item as GameObject;
            if (gameObject != null)
            {
                return gameObject;
            }
            var component = item as Component;
            return component == null ? null : component.gameObject;
        }

        private static string StepName(object step)
        {
            var unity = step as UnityEngine.Object;
            return unity != null && !string.IsNullOrEmpty(unity.name) ? unity.name : null;
        }

        private static double Clamp01(double value)
        {
            if (value < 0.0)
            {
                return 0.0;
            }
            return value > 1.0 ? 1.0 : value;
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
