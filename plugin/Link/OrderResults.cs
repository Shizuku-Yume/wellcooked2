using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using HarmonyLib;

namespace WellCooked
{
    /// <summary>
    /// 订单结果：交付成功 / 过期 / 送错被拒（任务 09 G9）。
    ///
    /// `m_activeOrders` 里订单一消失，光看列表分不清"送对了"还是"超时了"——
    /// 前者加分、后者扣分，调度侧把过期记成完成是闭环错误。这里挂游戏自己的
    /// `ClientOrderControllerBase.OnFoodDelivered(bool, OrderID)` 与
    /// `OnOrderExpired(OrderID)`（`Assembly-CSharp.dll` 元数据确认存在），
    /// 记下最近 16 条结果，每帧随 `state_update.order_results` 整份重发。
    /// 桥侧按 `seq` 去重，所以丢一帧不会丢事件，重复帧也不会重复计数。
    ///
    /// 补丁在第一次 `Build()` 时装（`StateFrame` 调 `Install()`）：入口 `SmokePlugin.Awake`
    /// 不归本工作流管，状态字段的补丁跟着状态上报自己装，少一个必须同步改的文件。
    /// 运行期在 Unity 主线程被回调，`AppendTo` 也在主线程，锁只是防御。
    /// </summary>
    public static class OrderResults
    {
        private const int Capacity = 16;

        private struct Result
        {
            public long Seq;
            public string Id;
            public string Recipe;
            public string Outcome;
            public int Score;
            public bool HasScore;
            public long GameTimeMs;
        }

        private static readonly Result[] Ring = new Result[Capacity];
        private static readonly object Gate = new object();
        private static int _count;
        private static int _next;
        private static long _seq;
        private static bool _installed;

        /// <summary>装 Harmony 补丁；失败只记一次，状态上报照常进行。</summary>
        public static void Install()
        {
            if (_installed)
            {
                return;
            }
            _installed = true;

            try
            {
                var controller = WorldState.StaticType("ClientOrderControllerBase");
                if (controller == null)
                {
                    WorldState.WarnOnce("订单结果: 没找到 ClientOrderControllerBase，交付/过期结果不上报");
                    return;
                }

                var delivered = new HarmonyMethod(typeof(OrderResults).GetMethod(
                    "AfterFoodDelivered", BindingFlags.Static | BindingFlags.NonPublic));
                var expired = new HarmonyMethod(typeof(OrderResults).GetMethod(
                    "AfterOrderExpired", BindingFlags.Static | BindingFlags.NonPublic));
                var harmony = new Harmony("wellcooked.order-results");
                int patched = 0;

                foreach (var method in controller.GetMethods(BindingFlags.Instance
                                                             | BindingFlags.Public
                                                             | BindingFlags.NonPublic))
                {
                    if (method.Name == "OnFoodDelivered" && method.GetParameters().Length == 2)
                    {
                        harmony.Patch(method, postfix: delivered);
                        patched++;
                    }
                    else if (method.Name == "OnOrderExpired" && method.GetParameters().Length == 1)
                    {
                        harmony.Patch(method, postfix: expired);
                        patched++;
                    }
                }

                if (patched == 0)
                {
                    WorldState.WarnOnce("订单结果: OnFoodDelivered/OnOrderExpired 没找到，交付/过期结果不上报");
                }
            }
            catch (Exception e)
            {
                WorldState.WarnOnce("订单结果: 打补丁失败: " + e.Message);
            }
        }
        /// <summary>
        /// 重开后订单 ID 会复用；保留上一局的 delivered 结果会让 Bridge 把同 ID 新订单误判为已交付。
        /// 保留递增序号，只清掉上一局的结果记录。
        /// </summary>
        public static void ResetLevel()
        {
            lock (Gate)
            {
                Array.Clear(Ring, 0, Ring.Length);
                _count = 0;
                _next = 0;
            }
        }

        /// <summary>把最近的结果按"旧 → 新"写进 `order_results`。</summary>
        public static void AppendTo(JsonBuilder b)
        {
            Install();

            b.Key("order_results").Arr();
            lock (Gate)
            {
                int start = _count < Capacity ? 0 : _next;
                for (int i = 0; i < _count; i++)
                {
                    var result = Ring[(start + i) % Capacity];
                    b.Obj();
                    b.FieldInt("seq", result.Seq);
                    b.Field("id", result.Id);
                    b.Field("outcome", result.Outcome);
                    if (!string.IsNullOrEmpty(result.Recipe))
                    {
                        b.Field("recipe", result.Recipe);
                    }
                    if (result.HasScore)
                    {
                        b.FieldInt("score", result.Score);
                    }
                    b.FieldInt("game_time_ms", result.GameTimeMs);
                    b.EndObj();
                }
            }
            b.EndArr();
        }

        // 补丁签名用 __args 取参数（与 LogicalInput 一致）：不写 OrderID 类型，
        // 免得把嵌套类型的 C# 名字硬编进插件。
        private static void AfterFoodDelivered(object[] __args)
        {
            if (__args == null || __args.Length < 2)
            {
                return;
            }
            bool success = __args[0] is bool value && value;
            Note(success ? "delivered" : "failed", __args[1]);
        }

        private static void AfterOrderExpired(object[] __args)
        {
            if (__args == null || __args.Length < 1)
            {
                return;
            }
            Note("expired", __args[0]);
        }

        private static void Note(string outcome, object orderIdArg)
        {
            if (orderIdArg == null)
            {
                return;
            }

            // OrderController.OrderID 是结构体，里面就一个 m_id。
            var rawId = WorldState.Get(orderIdArg, "m_id");
            long id;
            if (rawId == null || !TryInt64(rawId, out id))
            {
                return;
            }

            string orderId = "order_" + id.ToString("D3", CultureInfo.InvariantCulture);
            string recipe;
            int score;
            bool hasScore;
            LookupOrder(orderId, out recipe, out score, out hasScore);

            var result = new Result();
            result.Id = orderId;
            result.Recipe = recipe;
            result.Outcome = outcome;
            result.Score = score;
            result.HasScore = hasScore;
            result.GameTimeMs = (long)Math.Round(StateFrame.GameTime() * 1000.0);

            lock (Gate)
            {
                _seq++;
                result.Seq = _seq;
                Ring[_next] = result;
                _next = (_next + 1) % Capacity;
                if (_count < Capacity)
                {
                    _count++;
                }
            }
        }

        /// <summary>结果发生时订单可能还在 `m_activeOrders` 里；顺手把菜名与分值带上。</summary>
        private static void LookupOrder(string orderId, out string recipe, out int score, out bool hasScore)
        {
            recipe = "";
            score = 0;
            hasScore = false;

            var flow = WorldState.Flow();
            var team = WorldState.Team();
            var monitor = flow == null || team == null ? null : WorldState.Invoke(flow, "GetMonitorForTeam", team);
            var controller = monitor == null ? null : WorldState.Get(monitor, "OrdersController");
            var active = controller == null ? null : WorldState.Get(controller, "m_activeOrders") as IEnumerable;
            if (active == null)
            {
                return;
            }

            foreach (var order in active)
            {
                if (order == null)
                {
                    continue;
                }
                var idObject = WorldState.Get(order, "ID");
                var idValue = idObject == null ? null : WorldState.Get(idObject, "m_id");
                long id;
                if (idValue == null || !TryInt64(idValue, out id))
                {
                    continue;
                }
                if (orderId != "order_" + id.ToString("D3", CultureInfo.InvariantCulture))
                {
                    continue;
                }

                var entry = WorldState.Get(order, "RecipeListEntry");
                var node = entry == null ? null : WorldState.Get(entry, "m_order") as UnityEngine.Object;
                if (node != null)
                {
                    recipe = node.name;
                }
                var scoreValue = entry == null ? null : WorldState.Get(entry, "m_scoreForMeal");
                if (scoreValue != null)
                {
                    score = (int)Convert.ToInt64(scoreValue, CultureInfo.InvariantCulture);
                    hasScore = true;
                }
                return;
            }
        }

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
    }
}
