using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using BepInEx;
using BepInEx.Logging;
using HarmonyLib;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 读厨房世界状态（调度算法的输入面）：厨师坐标与朝向、手持物、锅的进度与烧焦、
    /// 订单与剩余时间、得分、回合计时。每 0.5 秒写一份 `BepInEx/wellcooked-state.json`，
    /// 每 2 秒往日志打一行摘要。
    ///
    /// 取值路径来自对游戏程序集的导出（`BepInEx/welcooked-inventory.txt`）：
    /// `ServerKitchenFlowControllerBase.GetMonitorForTeam/GetPoints/RoundTimer` →
    /// `ServerTeamMonitor.Score/OrdersController` → `ServerOrderControllerBase.ActiveRecipes`，
    /// 锅走静态的 `ServerCookingHandler.GetCookingHandlers()`。
    /// 取不到的字段进 `missing`，不假装拿到了。
    /// </summary>
    public static class WorldState
    {
        private static ManualLogSource _log;
        private static float _nextSnapshot;
        private static float _nextSummary;
        private static string _lastSummary = "";
        private static object _teamId;

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

            int chefCount = AppendChefs(json);
            AppendKitchen(json, missing);
            AppendCooking(json, missing);

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
                var summary = "state: 厨师=" + chefCount
                              + (missing.Count == 0 ? "" : " 缺=" + string.Join(",", missing.ToArray()));
                if (summary != _lastSummary)
                {
                    _lastSummary = summary;
                    _log.LogInfo(summary);
                }
            }
        }

        private static int AppendChefs(StringBuilder json)
        {
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
            return chefs.Length;
        }

        /// <summary>得分、订单、回合计时：都在厨房流程控制器上。</summary>
        private static void AppendKitchen(StringBuilder json, List<string> missing)
        {
            var flow = FindAll("ServerKitchenFlowControllerBase").FirstOrDefault();
            if (flow == null)
            {
                missing.Add("ServerKitchenFlowControllerBase");
                json.Append(",\"score\":null,\"orders\":[],\"timer\":null");
                return;
            }

            object team = TeamId();
            if (team == null)
            {
                missing.Add("TeamID");
            }

            json.Append(",\"score\":");
            var points = team == null ? null : Invoke(flow, "GetPoints", team);
            json.Append(points == null ? "null" : points.ToString());

            json.Append(",\"orders\":[");
            var monitor = team == null ? null : Invoke(flow, "GetMonitorForTeam", team);
            if (monitor == null)
            {
                missing.Add("ServerTeamMonitor");
            }
            else
            {
                var controller = Get(monitor, "OrdersController");
                var recipes = controller == null ? null : AsEnumerable(Get(controller, "ActiveRecipes"));
                if (recipes == null)
                {
                    missing.Add("ActiveRecipes");
                }
                else
                {
                    bool first = true;
                    foreach (var recipe in recipes)
                    {
                        if (!first)
                        {
                            json.Append(',');
                        }

                        first = false;
                        json.Append(Describe(recipe));
                    }
                }
            }

            json.Append(']');

            json.Append(",\"timer\":");
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

        /// <summary>锅、加热、搅拌：静态枚举拿全部烹饪工位。</summary>
        private static void AppendCooking(StringBuilder json, List<string> missing)
        {
            json.Append(",\"cooking\":[");
            var handlers = AsEnumerable(InvokeStatic("ServerCookingHandler", "GetCookingHandlers"));
            if (handlers == null)
            {
                missing.Add("ServerCookingHandler.GetCookingHandlers");
                json.Append(']');
                return;
            }

            bool first = true;
            foreach (var handler in handlers)
            {
                var component = handler as Component;
                if (component == null)
                {
                    continue;
                }

                if (!first)
                {
                    json.Append(',');
                }

                first = false;
                var p = component.transform.position;
                json.Append("{\"pos\":[").Append(p.x.ToString("F2")).Append(',').Append(p.y.ToString("F2")).Append(']')
                    .Append(",\"progress\":").Append(Num(Invoke(handler, "GetCookingProgress")))
                    .Append(",\"cooked\":").Append(Bool(Invoke(handler, "IsCooked")))
                    .Append(",\"burning\":").Append(Bool(Invoke(handler, "IsBurning")))
                    .Append('}');
            }

            json.Append(']');
        }

        private static object TeamId()
        {
            if (_teamId != null)
            {
                return _teamId;
            }

            var type = AccessTools.TypeByName("TeamID");
            if (type == null || !type.IsEnum)
            {
                return null;
            }

            // 合作模式只有一个队，取第一个非 None 的值
            foreach (var value in Enum.GetValues(type))
            {
                if (value.ToString() != "None")
                {
                    _teamId = value;
                    return _teamId;
                }
            }

            return null;
        }

        // ---- 反射小工具：拿不到返回 null，由调用方记进 missing ----

        private static List<object> FindAll(string typeName)
        {
            var result = new List<object>();
            var type = AccessTools.TypeByName(typeName);
            if (type == null || !typeof(UnityEngine.Object).IsAssignableFrom(type))
            {
                return result; // 不是组件，FindObjectsOfType 用不了
            }

            foreach (var found in UnityEngine.Object.FindObjectsOfType(type))
            {
                if (found != null)
                {
                    result.Add(found);
                }
            }

            return result;
        }

        private static object Get(object target, string name)
        {
            return Invoke(target, name, null);
        }

        private static object Invoke(object target, string name, params object[] args)
        {
            if (target == null)
            {
                return null;
            }

            try
            {
                var type = target.GetType();
                var arguments = args ?? new object[0];
                if (arguments.Length == 0)
                {
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
                }

                var method = AccessTools.Method(type, name,
                    arguments.Select(a => a.GetType()).ToArray());
                return method == null ? null : method.Invoke(target, arguments);
            }
            catch (Exception)
            {
                return null;
            }
        }

        private static object InvokeStatic(string typeName, string methodName)
        {
            var type = AccessTools.TypeByName(typeName);
            if (type == null)
            {
                return null;
            }

            var method = AccessTools.Method(type, methodName, Type.EmptyTypes);
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
            return enumerable == null ? null : enumerable.Cast<object>();
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

            return parts.Count == 0 ? "\"" + Clean(item.ToString()) + "\"" : "{" + string.Join(",", parts.ToArray()) + "}";
        }

        private static string Clean(string value)
        {
            return value.Replace("\\", "").Replace("\"", "'").Replace("\n", " ").Replace("\r", " ");
        }
    }
}
