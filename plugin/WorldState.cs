using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using BepInEx;
using BepInEx.Logging;
using HarmonyLib;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 读厨房世界状态（任务 game-bridge/07 的数据面）：厨师坐标与朝向、手持物、锅的进度、
    /// 订单与剩余时间、得分、计时。每 0.5 秒写一份 `BepInEx/wellcooked-state.json`，
    /// 每 2 秒往日志打一行摘要。
    ///
    /// 这是调度算法的输入面：寻路要用厨师坐标，派单要用订单的截止时间，资源锁要用厨具状态。
    /// 字段名来自对游戏程序集的导出（`BepInEx/welcooked-inventory.txt`）和参考项目的读取点；
    /// 拿不到的东西不打日志假装拿到了，而是写进 `missing` 列表。
    /// </summary>
    public static class WorldState
    {
        private static ManualLogSource _log;
        private static float _nextSnapshot;
        private static float _nextSummary;
        private static string _lastSummary = "";

        public static void Install(ManualLogSource log)
        {
            _log = log;
            _log.LogInfo("state: 世界状态读取已启用");
        }

        public static void Tick(float now)
        {
            if (now >= _nextSnapshot)
            {
                _nextSnapshot = now + 0.5f;
                try
                {
                    Snapshot(now);
                }
                catch (Exception e)
                {
                    _log.LogError("state: 采集失败 " + e);
                }
            }
        }

        private static void Snapshot(float now)
        {
            var json = new StringBuilder();
            var missing = new List<string>();

            json.Append("{\"t\":").Append(now.ToString("F2"));
            json.Append(",\"scene\":\"").Append(UnityEngine.SceneManagement.SceneManager.GetActiveScene().name).Append('"');

            json.Append(",\"chefs\":[");
            var chefs = UnityEngine.Object.FindObjectsOfType<PlayerControls>();
            for (int i = 0; i < chefs.Length; i++)
            {
                var chef = chefs[i];
                var provider = chef.GetComponent<PlayerIDProvider>();
                var position = chef.transform.position;
                var forward = chef.transform.forward;
                var carrier = chef.GetComponent<ServerPlayerAttachmentCarrier>();
                var held = carrier == null ? null : carrier.InspectCarriedItem();

                if (i > 0)
                {
                    json.Append(',');
                }

                json.Append("{\"player\":\"").Append(provider == null ? "?" : provider.GetID().ToString())
                    .Append("\",\"local\":").Append(provider != null && provider.IsLocallyControlled() ? "true" : "false")
                    .Append(",\"pos\":[").Append(position.x.ToString("F2")).Append(',').Append(position.y.ToString("F2")).Append(']')
                    .Append(",\"facing\":[").Append(forward.x.ToString("F2")).Append(',').Append(forward.z.ToString("F2")).Append(']')
                    .Append(",\"held\":\"").Append(held == null ? "" : Clean(held.name)).Append("\"}");
            }

            json.Append(']');

            // 得分与订单：ServerTeamMonitor 是每队的监视器，Score 与 OrdersController 都在它上面
            json.Append(",\"score\":");
            var monitors = FindAll("ServerTeamMonitor");
            if (monitors.Count == 0)
            {
                missing.Add("ServerTeamMonitor");
                json.Append("null");
            }
            else
            {
                var scoreStats = Get(monitors[0], "Score");
                var total = Get(scoreStats, "GetTotalScore");
                json.Append(total == null ? "null" : total.ToString());
            }

            json.Append(",\"orders\":[");
            bool firstOrder = true;
            foreach (var monitor in monitors)
            {
                var controller = Get(monitor, "OrdersController");
                var recipes = AsEnumerable(Get(controller, "ActiveRecipes"));
                if (recipes == null)
                {
                    continue;
                }

                foreach (var recipe in recipes)
                {
                    if (!firstOrder)
                    {
                        json.Append(',');
                    }

                    firstOrder = false;
                    json.Append(Describe(recipe));
                }
            }

            json.Append(']');

            // 锅/加热/搅拌：ServerCookingHandler 提供静态枚举，进度和烧焦状态都在它上面
            json.Append(",\"cooking\":[");
            var handlers = AsEnumerable(InvokeStatic("ServerCookingHandler", "GetCookingHandlers"));
            bool firstCook = true;
            int cookingCount = 0;
            if (handlers != null)
            {
                foreach (var handler in handlers)
                {
                    var component = handler as Component;
                    if (component == null)
                    {
                        continue;
                    }

                    if (!firstCook)
                    {
                        json.Append(',');
                    }

                    firstCook = false;
                    cookingCount++;
                    var p = component.transform.position;
                    json.Append("{\"pos\":[").Append(p.x.ToString("F2")).Append(',').Append(p.y.ToString("F2")).Append(']')
                        .Append(",\"progress\":").Append(Num(Get(handler, "GetCookingProgress")))
                        .Append(",\"cooked\":").Append(Bool(Get(handler, "IsCooked")))
                        .Append(",\"burning\":").Append(Bool(Get(handler, "IsBurning")))
                        .Append('}');
                }
            }
            else
            {
                missing.Add("ServerCookingHandler.GetCookingHandlers");
            }

            json.Append(']');

            // 计时：厨房流程控制器上挂着回合计时器
            json.Append(",\"timer\":");
            var flow = FindAll("ServerKitchenFlowControllerBase").FirstOrDefault()
                       ?? FindAll("ClientKitchenFlowControllerBase").FirstOrDefault();
            if (flow == null)
            {
                missing.Add("KitchenFlowController");
                json.Append("null");
            }
            else
            {
                var timer = Get(flow, "RoundTimer");
                if (timer == null)
                {
                    missing.Add("RoundTimer");
                    json.Append("null");
                }
                else
                {
                    json.Append("{\"elapsed\":").Append(Num(Get(timer, "TimeElapsed")))
                        .Append(",\"limit\":").Append(Num(Get(flow, "m_timeLimit")))
                        .Append('}');
                }
            }

            json.Append(",\"missing\":[");
            for (int i = 0; i < missing.Count; i++)
            {
                if (i > 0)
                {
                    json.Append(',');
                }

                json.Append('"').Append(missing[i]).Append('"');
            }

            json.Append("]}");

            File.WriteAllText(Path.Combine(Paths.BepInExRootPath, "wellcooked-state.json"), json.ToString());

            if (now >= _nextSummary)
            {
                _nextSummary = now + 2f;
                var summary = "state: 厨师=" + chefs.Length + " 订单=" + CountOrders(monitors)
                              + " 灶=" + cookingCount + " " + (flow == null ? "无计时" : "计时有")
                              + (missing.Count == 0 ? "" : " 缺=" + string.Join(",", missing.ToArray()));
                if (summary != _lastSummary)
                {
                    _lastSummary = summary;
                    _log.LogInfo(summary);
                }
            }
        }

        private static int CountOrders(List<object> monitors)
        {
            int count = 0;
            foreach (var monitor in monitors)
            {
                var recipes = AsEnumerable(Get(Get(monitor, "OrdersController"), "ActiveRecipes"));
                if (recipes != null)
                {
                    count += recipes.Count();
                }
            }

            return count;
        }

        // ---- 取值的反射小工具：拿不到就返回 null，由调用方记进 missing ----

        private static List<object> FindAll(string typeName)
        {
            var result = new List<object>();
            var type = AccessTools.TypeByName(typeName);
            if (type == null)
            {
                return result;
            }

            foreach (var found in UnityEngine.Object.FindObjectsOfType(type))
            {
                result.Add(found);
            }

            return result;
        }

        private static object Get(object target, string name)
        {
            if (target == null)
            {
                return null;
            }

            try
            {
                var type = target.GetType();
                var property = AccessTools.Property(type, name);
                if (property != null)
                {
                    return property.GetValue(target, null);
                }

                var field = AccessTools.Field(type, name);
                if (field != null)
                {
                    return field.GetValue(target);
                }

                var method = AccessTools.Method(type, name, Type.EmptyTypes);
                return method == null ? null : method.Invoke(target, null);
            }
            catch (Exception)
            {
                return null;
            }
        }

        private static object InvokeStatic(string typeName, string methodName)
        {
            var type = AccessTools.TypeByName(typeName);
            var method = type == null ? null : AccessTools.Method(type, methodName, Type.EmptyTypes);
            try
            {
                return method == null ? null : method.Invoke(null, null);
            }
            catch (Exception)
            {
                return null;
            }
        }

        private static IEnumerable<object> AsEnumerable(object value)
        {
            var enumerable = value as IEnumerable;
            if (enumerable == null)
            {
                return null;
            }

            return enumerable.Cast<object>();
        }

        private static string Num(object value)
        {
            if (value == null)
            {
                return "null";
            }

            float f;
            return float.TryParse(value.ToString(), out f) ? f.ToString("F3") : "null";
        }

        private static string Bool(object value)
        {
            return value == null ? "null" : value.ToString().ToLowerInvariant();
        }

        /// <summary>把订单/菜谱这类对象压成一行：有名字用名字，有剩余时间就用它。</summary>
        private static string Describe(object item)
        {
            if (item == null)
            {
                return "null";
            }

            var unity = item as UnityEngine.Object;
            if (unity != null)
            {
                return "\"" + Clean(unity.name) + "\"";
            }

            var parts = new List<string>();
            var name = Get(item, "name") ?? Get(item, "Name");
            if (name != null)
            {
                parts.Add("\"name\":\"" + Clean(name.ToString()) + "\"");
            }

            var remaining = Get(item, "Remaining");
            if (remaining != null)
            {
                parts.Add("\"remaining\":" + Num(remaining));
            }

            var lifetime = Get(item, "Lifetime");
            if (lifetime != null)
            {
                parts.Add("\"lifetime\":" + Num(lifetime));
            }

            if (parts.Count == 0)
            {
                return "\"" + Clean(item.ToString()) + "\"";
            }

            return "{" + string.Join(",", parts.ToArray()) + "}";
        }

        private static string Clean(string value)
        {
            return value.Replace("\\", "").Replace("\"", "'").Replace("\n", " ").Replace("\r", " ");
        }
    }
}
