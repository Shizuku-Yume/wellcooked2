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
            if (points == null)
            {
                // 拿不到就进 missing，不要只写个 null 让人以为是"没得分"
                missing.Add("score");
            }

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
                missing.Add("timer");
            }
            else
            {
                if (Get(timer, "TimeElapsed") == null || Get(timer, "m_timeLeft") == null)
                {
                    missing.Add("timer 字段");
                }

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
                "ClientCookablePreparationContainer", "ClientTrayIngredientContainer",
                // 料箱：DispenserCrate 上的这个组件给出它发的是哪种料
                "ClientPickupItemSpawner"
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
                        .Append("\",\"name\":\"").Append(Clean(unity.gameObject.name))
                        .Append("\",\"pos\":[").Append(p.x.ToString("F2")).Append(',').Append(p.y.ToString("F2"))
                        .Append(']');

                    if (typeName == "ClientPickupItemSpawner")
                    {
                        // 料箱给什么：PickupItemSpawner.m_itemPrefab 的名字就是料名（如 SushiFish）
                        var prefab = Invoke(component, "GetItemPrefab") as UnityEngine.Object;
                        json.Append(",\"gives\":\"").Append(Clean(prefab == null ? "" : prefab.name)).Append('"');
                    }

                    json.Append(",\"contents\":[");

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

        /// <summary>
        /// 关卡实例标识（用第一名厨师的实例号）：重进同一关时场景名一样，
        /// 只看名字会复用上一关的旧网格（Unity 2017 的 Scene 没有 handle）。
        /// </summary>
        private static int _terrainChefId;

        // 可走格：直接来自游戏的 GridManager（occupant 为空的格子），不是自己打射线猜的
        private static bool[,] _walkable;
        private static float _originX;
        private static float _originZ;
        private static float _step = 1f;
        private static int _cellsX;
        private static int _cellsZ;
        private static int _cellOffsetX;
        private static int _cellOffsetZ;

        // 格子空着不等于走得过去：台面占了格心，它的碰撞体可能伸到两格之间的空隙里。
        // 所以相邻两格之间还要单独判一次"边通不通"（实测厨师卡在两格正中间）。
        private static bool[,] _passableX;
        private static bool[,] _passableZ;

        public static bool HasTerrain
        {
            get { return _walkable != null; }
        }

        /// <summary>游戏网格的格距（米）。路径跟随按它决定"算到了这一格"的半径。</summary>
        public static float Step
        {
            get { return _step; }
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

        /// <summary>从 (ix,iz) 朝 (dx,dz)（只允许单步正交）走不走得过去。</summary>
        public static bool CanStep(int ix, int iz, int dx, int dz)
        {
            if (!IsWalkableCell(ix, iz))
            {
                return false;
            }

            // 边表存在小下标那一侧：_passableX[i,j] 表示 (i,j)↔(i+1,j)。
            // 往 -x 走要查 ix-1，查 ix 就成了"另一侧那条边"——之前就是这么写的，
            // 于是 A* 只能可靠地往 +x/+z 扩，需要往左/往下就报不可达。
            if (dx > 0)
            {
                return ix >= 0 && ix + 1 < _cellsX && iz >= 0 && iz < _cellsZ
                       && _passableX != null && _passableX[ix, iz] && IsWalkableCell(ix + 1, iz);
            }

            if (dx < 0)
            {
                return ix - 1 >= 0 && iz >= 0 && iz < _cellsZ
                       && _passableX != null && _passableX[ix - 1, iz] && IsWalkableCell(ix - 1, iz);
            }

            if (dz > 0)
            {
                return ix >= 0 && ix < _cellsX && iz >= 0 && iz + 1 < _cellsZ
                       && _passableZ != null && _passableZ[ix, iz] && IsWalkableCell(ix, iz + 1);
            }

            return ix >= 0 && ix < _cellsX && iz - 1 >= 0
                   && _passableZ != null && _passableZ[ix, iz - 1] && IsWalkableCell(ix, iz - 1);
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
            if (scene == "StartScreen" || scene == "Loading")
            {
                return;
            }

            var found = UnityEngine.Object.FindObjectsOfType<PlayerControls>();
            if (found.Length == 0)
            {
                return;
            }

            if (scene == _terrainScene && found[0].GetInstanceID() == _terrainChefId)
            {
                return;
            }

            var chefs = found;

            _terrainScene = scene;
            _terrainChefId = found[0].GetInstanceID();
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
        /// 可走格：自己按 0.6 米格做物理采样。
        ///
        /// 走过的两条弯路：
        /// 1. 拿 `PlayerControls.GroundLayer` 向下打射线——台面碰撞体不在这一层，
        ///    射线穿到台面底下的地板，台面被算成可走；
        /// 2. 改用游戏自己的 `GridNavSpace.m_nodeMap`——它的格子是 1.2 米，
        ///    而台面/工作台摆在 0.6 米的格子上（`probe` 实测：worktop 在 19.8/20.4/21.0），
        ///    粗一半的网格表示不出中间的障碍，于是厨师卡在两格正中间。
        ///
        /// 现在按场景里的物体实际尺寸来：0.6 米一格，每格要求脚下有地面、
        /// 且以厨师碰撞体半径在腰间查一圈没有别的东西；相邻两格之间的中点也要能过。
        /// </summary>
        private static bool SampleGameGrid()
        {
            float chefRadius = MeasureChefRadius();
            float step = ObjectStep;

            float minX = float.MaxValue, maxX = float.MinValue, minZ = float.MaxValue, maxZ = float.MinValue;
            var anchors = new List<Vector3>();
            foreach (var chef in UnityEngine.Object.FindObjectsOfType<PlayerControls>())
            {
                anchors.Add(chef.transform.position);
            }

            foreach (var station in FindAll("ClientIngredientContainer"))
            {
                var component = station as Component;
                if (component != null)
                {
                    anchors.Add(component.transform.position);
                }
            }

            foreach (var p in anchors)
            {
                minX = Math.Min(minX, p.x);
                maxX = Math.Max(maxX, p.x);
                minZ = Math.Min(minZ, p.z);
                maxZ = Math.Max(maxZ, p.z);
            }

            if (anchors.Count == 0)
            {
                return false;
            }

            minX -= SampleMargin;
            maxX += SampleMargin;
            minZ -= SampleMargin;
            maxZ += SampleMargin;

            _originX = minX;
            _originZ = minZ;
            _step = step;
            _cellOffsetX = 0;
            _cellOffsetZ = 0;
            _cellsX = (int)Math.Floor((maxX - minX) / step) + 1;
            _cellsZ = (int)Math.Floor((maxZ - minZ) / step) + 1;
            _walkable = new bool[_cellsX, _cellsZ];
            _passableX = new bool[_cellsX, _cellsZ];
            _passableZ = new bool[_cellsX, _cellsZ];

            var cells = new StringBuilder();
            var free = new List<Vector2>();
            int count = 0;

            // 探针半径比厨师实际半径略小：贴着台面走时厨师能蹭过去，
            // 用足半径会把贴着台面的一整圈都判死，路就断了。
            // 节点和边都用**厨师真实半径**：格子小一点会让路点落在厨师站不进去的地方，
            // 厨师走到跟前被台面顶住、又够不到路点，就在那儿来回蹭（实测 20.4,-1.8 附近）。
            // 一致性比"多留几格"重要：能站才算可走，能过才算一条边。
            // 节点与边都用厨师真实半径：0.8× 会放行厨师过不去的缝，
            // 于是路点落在缝对面、厨师顶着台面推（实测 x=13.03 处推不动）。
            // 之前"用足半径就碎片化"是边表索引写错放大出来的，那个 bug 已修。
            float nodeRadius = chefRadius;
            float edgeRadius = chefRadius;

            for (int i = 0; i < _cellsX; i++)
            {
                for (int j = 0; j < _cellsZ; j++)
                {
                    var world = new Vector3(minX + i * step, 0f, minZ + j * step);
                    if (!Standable(world, nodeRadius))
                    {
                        continue;
                    }

                    _walkable[i, j] = true;
                    free.Add(new Vector2(world.x, world.z));
                    count++;
                }
            }

            if (count == 0)
            {
                _walkable = null;
                return false;
            }

            int edges = 0;
            for (int i = 0; i < _cellsX - 1; i++)
            {
                for (int j = 0; j < _cellsZ; j++)
                {
                    if (!_walkable[i, j] || !_walkable[i + 1, j])
                    {
                        continue;
                    }

                    if (StandableBetween(CellCenter(i, j), CellCenter(i + 1, j), edgeRadius))
                    {
                        _passableX[i, j] = true;
                        edges++;
                    }
                }
            }

            for (int i = 0; i < _cellsX; i++)
            {
                for (int j = 0; j < _cellsZ - 1; j++)
                {
                    if (!_walkable[i, j] || !_walkable[i, j + 1])
                    {
                        continue;
                    }

                    if (StandableBetween(CellCenter(i, j), CellCenter(i, j + 1), edgeRadius))
                    {
                        _passableZ[i, j] = true;
                        edges++;
                    }
                }
            }

            // 建完边再筛连通块：放在建边之前的话，边表还是全 false，
            // 每个格子都成了独立块，最大块只剩 1 格（实测 可走格 1/308）。
            int dropped = KeepLargestComponent();

            foreach (var p in free)
            {
                if (cells.Length > 0)
                {
                    cells.Append(',');
                }

                cells.Append('[').Append(p.x.ToString("F2")).Append(',').Append(p.y.ToString("F2")).Append(']');
            }

            var text = new StringBuilder();
            text.Append("{\"scene\":\"").Append(ActiveScene()).Append('"');
            text.Append(",\"source\":\"physics-0.6\"");
            text.Append(",\"step\":").Append(_step.ToString("F2"));
            text.Append(",\"map\":[").Append(_cellsX).Append(',').Append(_cellsZ).Append(']');
            text.Append(",\"cells\":[").Append(cells.ToString()).Append("]}");
            File.WriteAllText(Path.Combine(Paths.BepInExRootPath, "wellcooked-terrain.json"), text.ToString());
            count -= dropped;
            _log.LogInfo("state: 可走格 " + count + "/" + (_cellsX * _cellsZ) + "，可走边 " + edges
                         + "（物理采样，格距 " + _step.ToString("F2")
                         + "，厨师半径 " + chefRadius.ToString("F2")
                         + "，厨师层 " + UnityEngine.LayerMask.LayerToName(PlayerLayer)
                         + "，场景 " + ActiveScene() + "）");
            return true;
        }

        /// <summary>场景里的台面/工作台按 0.6 米一格摆放（probe 实测），采样就按这个来。</summary>
        private const float ObjectStep = 0.6f;

        private const float SampleMargin = 3f;

        /// <summary>厨师所在的层：物理检查必须排掉它，否则"厨师站着的格子"自己就被判成不可走。</summary>
        private static int PlayerLayer
        {
            get
            {
                foreach (var chef in UnityEngine.Object.FindObjectsOfType<PlayerControls>())
                {
                    return chef.gameObject.layer;
                }

                return 0;
            }
        }

        /// <summary>厨师体积半径：直接量他的碰撞体，别猜。</summary>
        private static float MeasureChefRadius()
        {
            foreach (var chef in UnityEngine.Object.FindObjectsOfType<PlayerControls>())
            {
                var collider = chef.GetComponent<Collider>();
                if (collider != null)
                {
                    var extents = collider.bounds.extents;
                    float radius = Math.Max(extents.x, extents.z);
                    if (radius > 0.05f)
                    {
                        return radius;
                    }
                }
            }

            return 0.35f;
        }

        /// <summary>
        /// 只保留最大的连通块。物理采样难免留下"口袋"（一小片孤立可走格），
        /// 厨师走进去就再也出不来：起点格在口袋里，A* 到哪都报不可达（实测卡死在 20.56,-3.13）。
        /// 把口袋从可走表里去掉，起点就会就近跳到主区。
        /// </summary>
        private static int KeepLargestComponent()
        {
            var component = new int[_cellsX, _cellsZ];
            var queue = new Queue<int>();
            int best = 0;
            int bestSize = 0;
            int label = 0;

            for (int i = 0; i < _cellsX; i++)
            {
                for (int j = 0; j < _cellsZ; j++)
                {
                    if (!_walkable[i, j] || component[i, j] != 0)
                    {
                        continue;
                    }

                    label++;
                    int size = 0;
                    component[i, j] = label;
                    queue.Enqueue(i * _cellsZ + j);
                    while (queue.Count > 0)
                    {
                        int current = queue.Dequeue();
                        int cx = current / _cellsZ;
                        int cz = current % _cellsZ;
                        size++;
                        for (int d = 0; d < 4; d++)
                        {
                            int nx = cx + (d == 0 ? 1 : d == 1 ? -1 : 0);
                            int nz = cz + (d == 2 ? 1 : d == 3 ? -1 : 0);
                            if (nx < 0 || nz < 0 || nx >= _cellsX || nz >= _cellsZ)
                            {
                                continue;
                            }

                            if (!_walkable[nx, nz] || component[nx, nz] != 0)
                            {
                                continue;
                            }

                            if (!CanStep(cx, cz, nx - cx, nz - cz))
                            {
                                continue;
                            }

                            component[nx, nz] = label;
                            queue.Enqueue(nx * _cellsZ + nz);
                        }
                    }

                    if (size > bestSize)
                    {
                        bestSize = size;
                        best = label;
                    }
                }
            }

            int dropped = 0;
            for (int i = 0; i < _cellsX; i++)
            {
                for (int j = 0; j < _cellsZ; j++)
                {
                    if (_walkable[i, j] && component[i, j] != best)
                    {
                        _walkable[i, j] = false;
                        dropped++;
                    }
                }
            }

            return dropped;
        }

        /// <summary>两格之间的空隙也要能过：在中点按厨师半径查一圈。</summary>
        private static bool StandableBetween(Vector2 a, Vector2 b, float radius)
        {
            var middle = new Vector3((a.x + b.x) * 0.5f, 0f, (a.y + b.y) * 0.5f);
            return Standable(middle, radius);
        }

        /// <summary>探针用的层掩码：排掉厨师层与 Ignore Raycast 层。</summary>
        private static int ProbeMask
        {
            get
            {
                int mask = ~(1 << PlayerLayer);
                int ignoreRaycast = UnityEngine.LayerMask.NameToLayer("Ignore Raycast");
                if (ignoreRaycast >= 0)
                {
                    mask &= ~(1 << ignoreRaycast);
                }

                return mask;
            }
        }

        /// <summary>
        /// 脚下有地面、且以厨师半径在腰间查一圈没有别的东西，才算站得住。
        ///
        /// 地面射线**必须**用和探针一样的掩码：否则射线先打到厨师自己，
        /// 把厨师头顶当成地面，探针抬到 2.5 米撞上天花板，厨师站的那一格
        /// 就被判成不可走（实测 A* 起点不可走、四邻全 False、一律报不可达）。
        /// </summary>
        private static bool Standable(Vector3 world, float radius)
        {
            int mask = ProbeMask;
            RaycastHit ground;
            if (!Physics.Raycast(world + Vector3.up * 2f, Vector3.down, out ground, 4f, mask))
            {
                return false;
            }

            var probe = new Vector3(world.x, ground.point.y + 0.5f, world.z);
            return !Physics.CheckSphere(probe, radius, mask, QueryTriggerInteraction.Ignore);
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

            // 工序状态机要知道"这单要不要切、要不要煮"，这些在菜谱节点的子树里
            var steps = new List<string>();
            CollectOrderSteps(order, steps, 0);
            parts.Add("\"steps\":[" + string.Join(",", steps.ToArray()) + "]");

            return parts.Count == 0 ? "\"" + Clean(entry.ToString()) + "\"" : "{" + string.Join(",", parts.ToArray()) + "}";
        }

        /// <summary>
        /// 递归展开菜谱节点：CompositeOrderNode 有 m_composition 子节点，
        /// 叶子节点的类型名（Ingredient/CookedComposite/…）就是"这一步要什么"。
        /// </summary>
        private static void CollectOrderSteps(object node, List<string> steps, int depth)
        {
            if (node == null || depth > 4)
            {
                return;
            }

            var type = node.GetType();
            var unity = node as UnityEngine.Object;
            string name = unity != null && !string.IsNullOrEmpty(unity.name) ? Clean(unity.name) : type.Name;
            var raw = AsEnumerable(Get(node, "m_composition"));
            var children = raw == null ? null : new List<object>(raw);
            string plating = null;
            var platingStep = Get(node, "m_platingStep");
            if (platingStep != null)
            {
                var platingUnity = platingStep as UnityEngine.Object;
                plating = platingUnity != null && !string.IsNullOrEmpty(platingUnity.name)
                    ? Clean(platingUnity.name)
                    : null;
            }

            if (children == null || children.Count == 0)
            {
                steps.Add("{\"node\":\"" + name + "\",\"type\":\"" + type.Name + "\""
                          + (plating == null ? "" : ",\"plate\":\"" + plating + "\"") + "}");
                return;
            }

            var parts = new List<string>();
            foreach (var child in children)
            {
                CollectOrderSteps(child, parts, depth + 1);
            }

            steps.Add("{\"node\":\"" + name + "\",\"type\":\"" + type.Name + "\",\"children\":["
                      + string.Join(",", parts.ToArray()) + "]}");
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
