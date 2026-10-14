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
            int stationCount = AppendMap(json, missing);
            DumpTerrainOnce();

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
                var summary = "state: 厨师=" + chefCount + " 工位=" + stationCount
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
                    .Append(",\"pos\":[").Append(position.x.ToString("F2")).Append(',').Append(position.z.ToString("F2")).Append(']')
                    .Append(",\"height\":").Append(position.y.ToString("F2"))
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
            var controller = monitor == null ? null : Get(monitor, "OrdersController");
            if (monitor == null)
            {
                missing.Add("ServerTeamMonitor");
            }
            else if (controller == null)
            {
                missing.Add("OrdersController");
            }
            else
            {
                // 活跃订单：控制器私有 m_activeOrders 上有剩余时间与寿命
                var active = AsEnumerable(Get(controller, "m_activeOrders"));
                if (active == null)
                {
                    missing.Add("m_activeOrders");
                }
                else
                {
                    bool first = true;
                    foreach (var order in active)
                    {
                        if (!first)
                        {
                            json.Append(',');
                        }

                        first = false;
                        json.Append(DescribeOrder(order));
                    }
                }
            }

            json.Append(']');

            // 菜谱条目单独一个键：能拿到菜名与单份分数
            json.Append(",\"recipes\":[");
            var recipes = controller == null ? null : AsEnumerable(Get(controller, "ActiveRecipes"));
            if (controller != null && recipes == null)
            {
                missing.Add("ActiveRecipes");
            }
            else if (recipes != null)
            {
                bool firstRecipe = true;
                foreach (var recipe in recipes)
                {
                    if (!firstRecipe)
                    {
                        json.Append(',');
                    }

                    firstRecipe = false;
                    json.Append(DescribeRecipe(recipe));
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
                    .Append(",\"round\":").Append(Num(Get(timer, "m_roundTimer")))
                    .Append(",\"left\":").Append(Num(Get(timer, "m_timeLeft")))
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
                // ServerCookingHandler 不是 UnityEngine.Component，位置从它交出的 CookingHandler 上取
                var component = handler as Component ?? Get(handler, "GetCookingHandler") as Component;
                if (component == null)
                {
                    missing.Add("CookingHandler 位置");
                    continue;
                }

                if (!first)
                {
                    json.Append(',');
                }

                first = false;
                var p = component.transform.position;
                json.Append("{\"pos\":[").Append(p.x.ToString("F2")).Append(',').Append(p.z.ToString("F2")).Append(']')
                    .Append(",\"progress\":").Append(Num(Invoke(handler, "GetCookingProgress")))
                    .Append(",\"cooked\":").Append(Bool(Invoke(handler, "IsCooked")))
                    .Append(",\"burning\":").Append(Bool(Invoke(handler, "IsBurning")))
                    .Append('}');
            }

            json.Append(']');
        }

        /// <summary>
        /// 地图上的工位与物品：砧板/锅/盘子/水槽/加热台/传送带各自的位置和内容物。
        /// 内容是 `ClientIngredientContainer.GetContents()` 给出的物品节点，位置取组件自己的 transform。
        /// 调度要用它做资源预订（谁占了哪口锅、哪块砧板上有东西）。
        /// </summary>
        private static int AppendMap(StringBuilder json, List<string> missing)
        {
            var stationTypes = new[]
            {
                "ClientIngredientContainer", "ClientPlateStation", "ClientWashingStation",
                "ClientHeatedStation", "ClientMixingStation", "ClientCleanPlateStack",
                "ClientDirtyPlateStack", "ClientConveyorStation", "ClientSwitchStation",
                "ClientCookingStation", "ClientPlate", "ClientCookableContainer",
                "ClientCookablePreparationContainer", "ClientTrayIngredientContainer"
            };

            json.Append(",\"stations\":[");
            bool first = true;
            int count = 0;
            foreach (var typeName in stationTypes)
            {
                var found = FindAll(typeName);
                if (found.Count == 0)
                {
                    continue;
                }

                foreach (var component in found)
                {
                    var unity = component as Component;
                    if (unity == null)
                    {
                        continue;
                    }

                    if (!first)
                    {
                        json.Append(',');
                    }

                    first = false;
                    count++;
                    var p = unity.transform.position;
                    json.Append("{\"type\":\"").Append(typeName)
                        .Append("\",\"pos\":[").Append(p.x.ToString("F2")).Append(',').Append(p.y.ToString("F2"))
                        .Append("],\"contents\":[");

                    var contents = AsEnumerable(Invoke(component, "GetContents"));
                    bool firstItem = true;
                    if (contents != null)
                    {
                        foreach (var item in contents)
                        {
                            if (!firstItem)
                            {
                                json.Append(',');
                            }

                            firstItem = false;
                            json.Append(Describe(item));
                        }
                    }

                    json.Append(']');

                    var heat = Invoke(component, "HeatValue");
                    if (heat != null)
                    {
                        json.Append(",\"heat\":").Append(Num(heat));
                    }

                    json.Append('}');
                }
            }

            json.Append(']');
            if (count == 0)
            {
                missing.Add("地图工位");
            }

            return count;
        }

        /// <summary>
        /// 地形：这个游戏没有可用的 Unity 导航网格（实测 0 个顶点），
        /// 它自己是靠"地面层 + 向下打射线"判断能不能站的（`PlayerControls.GroundLayer`）。
        /// 所以按同样的办法采样：在地图范围内按 0.5 格打射线，打到地面层的格子就是可走格。
        /// 每个场景导一次，写 `wellcooked-terrain.json`。
        /// </summary>
        private static string _terrainScene;

        public const float SampleStep = 0.5f;
        private const float SampleMargin = 6f;

        // 采样出来的可走格，寻路直接查这张表
        private static bool[,] _walkable;
        private static float _originX;
        private static float _originZ;
        private static int _cellsX;
        private static int _cellsZ;

        public static bool HasTerrain
        {
            get { return _walkable != null; }
        }

        public static void WorldToCell(float x, float z, out int ix, out int iz)
        {
            ix = (int)Math.Round((x - _originX) / SampleStep);
            iz = (int)Math.Round((z - _originZ) / SampleStep);
        }

        public static Vector2 CellCenter(int ix, int iz)
        {
            return new Vector2(_originX + ix * SampleStep, _originZ + iz * SampleStep);
        }

        public static bool IsWalkableCell(int ix, int iz)
        {
            return _walkable != null && ix >= 0 && iz >= 0 && ix < _cellsX && iz < _cellsZ && _walkable[ix, iz];
        }

        public static bool IsWalkablePoint(float x, float z)
        {
            int ix, iz;
            WorldToCell(x, z, out ix, out iz);
            return IsWalkableCell(ix, iz);
        }

        public static int WalkableCellCount
        {
            get
            {
                if (_walkable == null)
                {
                    return 0;
                }

                int count = 0;
                for (int ix = 0; ix < _cellsX; ix++)
                {
                    for (int iz = 0; iz < _cellsZ; iz++)
                    {
                        if (_walkable[ix, iz])
                        {
                            count++;
                        }
                    }
                }

                return count;
            }
        }

        private static void DumpTerrainOnce()
        {
            var scene = UnityEngine.SceneManagement.SceneManager.GetActiveScene().name;
            if (scene == _terrainScene || scene == "StartScreen" || scene == "Loading")
            {
                return;
            }

            var chefs = UnityEngine.Object.FindObjectsOfType<PlayerControls>();
            if (chefs.Length == 0)
            {
                return;
            }

            _terrainScene = scene;
            try
            {
                int mask = chefs[0].GroundLayer.value;

                // 采样范围：厨师与工位的外接矩形再加一圈余量
                float minX = float.MaxValue, maxX = float.MinValue, minZ = float.MaxValue, maxZ = float.MinValue;
                var points = new List<Vector3>();
                foreach (var chef in chefs)
                {
                    points.Add(chef.transform.position);
                }

                foreach (var station in FindAll("ClientIngredientContainer"))
                {
                    var component = station as Component;
                    if (component != null)
                    {
                        points.Add(component.transform.position);
                    }
                }

                foreach (var p in points)
                {
                    minX = Math.Min(minX, p.x);
                    maxX = Math.Max(maxX, p.x);
                    minZ = Math.Min(minZ, p.z);
                    maxZ = Math.Max(maxZ, p.z);
                }

                minX -= SampleMargin;
                maxX += SampleMargin;
                minZ -= SampleMargin;
                maxZ += SampleMargin;

                _originX = minX;
                _originZ = minZ;
                _cellsX = (int)Math.Floor((maxX - minX) / SampleStep) + 1;
                _cellsZ = (int)Math.Floor((maxZ - minZ) / SampleStep) + 1;
                _walkable = new bool[_cellsX, _cellsZ];

                var cells = new StringBuilder();
                int count = 0;
                float top = 5f;
                float distance = 10f;
                for (int ix = 0; ix < _cellsX; ix++)
                {
                    for (int iz = 0; iz < _cellsZ; iz++)
                    {
                        float x = _originX + ix * SampleStep;
                        float z = _originZ + iz * SampleStep;
                        RaycastHit hit;
                        if (!Physics.Raycast(new Vector3(x, top, z), Vector3.down, out hit, distance, mask))
                        {
                            continue;
                        }

                        _walkable[ix, iz] = true;
                        if (count > 0)
                        {
                            cells.Append(',');
                        }

                        count++;
                        cells.Append('[').Append(x.ToString("F2")).Append(',').Append(hit.point.z.ToString("F2")).Append(']');
                    }
                }

                var text = new StringBuilder();
                text.Append("{\"scene\":\"").Append(scene).Append('"');
                text.Append(",\"step\":").Append(SampleStep.ToString("F1"));
                text.Append(",\"groundLayer\":").Append(mask);
                text.Append(",\"bounds\":[").Append(minX.ToString("F2")).Append(',').Append(minZ.ToString("F2"))
                    .Append(',').Append(maxX.ToString("F2")).Append(',').Append(maxZ.ToString("F2")).Append(']');
                text.Append(",\"cells\":[").Append(cells).Append("]}");
                File.WriteAllText(Path.Combine(Paths.BepInExRootPath, "wellcooked-terrain.json"), text.ToString());
                _log.LogInfo("state: 地形采样 " + count + " 个可走格（场景 " + scene + "，地面层 " + mask + "）");
            }
            catch (Exception e)
            {
                _log.LogWarning("state: 地形采样失败: " + e.Message);
            }
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

        /// <summary>订单：ID、剩余时间、寿命。</summary>
        private static string DescribeOrder(object order)
        {
            if (order == null)
            {
                return "null";
            }

            var parts = new List<string>();
            var id = Get(order, "ID");
            if (id != null)
            {
                parts.Add("\"id\":\"" + Clean(Get(id, "m_id").ToString()) + "\"");
            }

            var remaining = Get(order, "Remaining");
            if (remaining != null)
            {
                parts.Add("\"remaining\":" + Num(remaining));
            }

            var lifetime = Get(order, "Lifetime");
            if (lifetime != null)
            {
                parts.Add("\"lifetime\":" + Num(lifetime));
            }

            return parts.Count == 0 ? "\"" + Clean(order.ToString()) + "\"" : "{" + string.Join(",", parts.ToArray()) + "}";
        }

        /// <summary>菜谱条目：RecipeList+Entry 里是 m_order（菜谱节点）与 m_scoreForMeal。</summary>
        private static string DescribeRecipe(object entry)
        {
            if (entry == null)
            {
                return "null";
            }

            var order = Get(entry, "m_order");
            var parts = new List<string>();
            if (order != null)
            {
                var unity = order as UnityEngine.Object;
                parts.Add("\"dish\":\"" + Clean(unity != null ? unity.name : order.ToString()) + "\"");
            }

            var score = Get(entry, "m_scoreForMeal");
            if (score != null)
            {
                parts.Add("\"score\":" + score);
            }

            return parts.Count == 0 ? "\"" + Clean(entry.ToString()) + "\"" : "{" + string.Join(",", parts.ToArray()) + "}";
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
