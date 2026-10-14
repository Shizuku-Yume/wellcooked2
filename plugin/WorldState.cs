using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Reflection;
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

        // 可走格：直接来自游戏的 GridManager（occupant 为空的格子），不是自己打射线猜的
        private static bool[,] _walkable;
        private static float _originX;
        private static float _originZ;
        private static float _step = 1f;
        private static int _cellsX;
        private static int _cellsZ;
        private static int _cellOffsetX;
        private static int _cellOffsetZ;

        public static bool HasTerrain
        {
            get { return _walkable != null; }
        }

        public static void WorldToCell(float x, float z, out int ix, out int iz)
        {
            ix = (int)Math.Round((x - _originX) / _step) - _cellOffsetX;
            iz = (int)Math.Round((z - _originZ) / _step) - _cellOffsetZ;
        }

        public static Vector2 CellCenter(int ix, int iz)
        {
            return new Vector2(_originX + (ix + _cellOffsetX) * _step, _originZ + (iz + _cellOffsetZ) * _step);
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
                if (!SampleGameGrid())
                {
                    _log.LogWarning("state: 拿不到游戏网格，可走格为空");
                }
            }
            catch (Exception e)
            {
                _log.LogWarning("state: 地形采样失败: " + e);
            }
        }

        /// <summary>
        /// 可走格直接取游戏自己的导航节点图：`GridNavSpace.m_nodeMap`。
        ///
        /// 走过的弯路：自己拿 `PlayerControls.GroundLayer` 向下打射线采样——台面的碰撞体
        /// 不在这一层，射线从台面旁穿到台面底下的地板，台面被算成可走，厨师贴着台面推墙
        /// （实测 x=20.6 一动不动）。改用 `GridManager.GetActive(0)` 的 occupant 判据也不行：
        /// 拿到的不是厨房那张网格，191 个点互相不连通，A* 全报"不可达"。
        /// `GridNavSpace` 是游戏自己的导航空间（`GridNavigator`/它自带的 A* 都用它），
        /// 它的 `m_nodeMap` 就是答案。
        /// </summary>
        private static bool SampleGameGrid()
        {
            var navSpaceType = AccessTools.TypeByName("GridNavSpace");
            var gridManagerType = AccessTools.TypeByName("GridManager");
            var indexType = AccessTools.TypeByName("GridIndex");
            if (navSpaceType == null || gridManagerType == null || indexType == null)
            {
                _log.LogWarning("state: 找不到 GridNavSpace/GridManager/GridIndex");
                return false;
            }

            var spaces = UnityEngine.Object.FindObjectsOfType(navSpaceType);
            if (spaces.Length == 0)
            {
                _log.LogWarning("state: 场上没有 GridNavSpace");
                return false;
            }

            object space = spaces[0];
            var flags = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
            var nodeMapField = navSpaceType.GetField("m_nodeMap", flags);
            var managerField = navSpaceType.GetField("m_gridManager", flags);
            var offsetField = navSpaceType.GetField("m_mapOffset", flags);
            if (nodeMapField == null || managerField == null || offsetField == null)
            {
                _log.LogWarning("state: GridNavSpace 字段名对不上（m_nodeMap/m_gridManager/m_mapOffset）");
                return false;
            }

            var nodeMap = (bool[,])nodeMapField.GetValue(space);
            object manager = managerField.GetValue(space);
            object offset = offsetField.GetValue(space);
            if (nodeMap == null || manager == null || offset == null)
            {
                _log.LogWarning("state: GridNavSpace 还没初始化好");
                return false;
            }

            var offsetType = offset.GetType();
            int offX = System.Convert.ToInt32(offsetType.GetField("X").GetValue(offset));
            int offY = System.Convert.ToInt32(offsetType.GetField("Y").GetValue(offset));

            var getPos = AccessTools.Method(gridManagerType, "GetPosFromGridLocation", new[] { indexType });
            var indexCtor = indexType.GetConstructor(new[] { typeof(int), typeof(int), typeof(int) });
            if (getPos == null || indexCtor == null)
            {
                _log.LogWarning("state: GridManager.GetPosFromGridLocation 找不到");
                return false;
            }

            _cellsX = nodeMap.GetLength(0);
            _cellsZ = nodeMap.GetLength(1);
            _walkable = new bool[_cellsX, _cellsZ];

            var cells = new StringBuilder();
            var free = new List<Vector3>();
            float minX = float.MaxValue, minZ = float.MaxValue;
            int minI = int.MaxValue, minJ = int.MaxValue;
            int count = 0;

            for (int i = 0; i < _cellsX; i++)
            {
                for (int j = 0; j < _cellsZ; j++)
                {
                    if (!nodeMap[i, j])
                    {
                        continue;
                    }

                    object index = indexCtor.Invoke(new object[] { i - offX, 0, j - offY });
                    var world = (Vector3)getPos.Invoke(manager, new[] { index });
                    _walkable[i, j] = true;
                    free.Add(world);
                    minX = Math.Min(minX, world.x);
                    minZ = Math.Min(minZ, world.z);
                    minI = Math.Min(minI, i);
                    minJ = Math.Min(minJ, j);
                    count++;
                }
            }

            if (count == 0)
            {
                _walkable = null;
                return false;
            }

            float step = 0f;
            foreach (var p in free)
            {
                float d = Math.Abs(p.x - minX);
                if (d > 0.01f && (step == 0f || d < step))
                {
                    step = d;
                }
            }

            _originX = minX;
            _originZ = minZ;
            _step = step > 0.01f ? step : 1f;
            // 数组下标是游戏网格的绝对下标，而 origin 取的是"可走格"的左上角：
            // 左上角被台面占掉几格，两者就差几格。之前忘了这一步，整张表错位，
            // 厨师所在格查出来全不可走，A* 一律报"不可达"（实测 178 次）。
            _cellOffsetX = minI;
            _cellOffsetZ = minJ;

            foreach (var p in free)
            {
                if (cells.Length > 0)
                {
                    cells.Append(',');
                }

                cells.Append('[').Append(p.x.ToString("F2")).Append(',').Append(p.z.ToString("F2")).Append(']');
            }

            var text = new StringBuilder();
            text.Append("{\"scene\":\"").Append(ActiveScene()).Append('"');
            text.Append(",\"source\":\"game-navspace\"");
            text.Append(",\"step\":").Append(_step.ToString("F2"));
            text.Append(",\"map\":[").Append(_cellsX).Append(',').Append(_cellsZ).Append(']');
            text.Append(",\"offset\":[").Append(offX).Append(',').Append(offY).Append(']');
            text.Append(",\"cells\":[").Append(cells.ToString()).Append("]}");
            File.WriteAllText(Path.Combine(Paths.BepInExRootPath, "wellcooked-terrain.json"), text.ToString());
            _log.LogInfo("state: 游戏导航节点图 " + count + "/" + (_cellsX * _cellsZ) + " 个可走格（格距 "
                         + _step.ToString("F2") + "，左上角下标 " + _cellOffsetX + "," + _cellOffsetZ
                         + "，场景 " + ActiveScene() + "）");
            return true;
        }

        private static string ActiveScene()
        {
            return UnityEngine.SceneManagement.SceneManager.GetActiveScene().name;
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
