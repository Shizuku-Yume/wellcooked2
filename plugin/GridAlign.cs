using System.Collections.Generic;
using BepInEx.Logging;
using HarmonyLib;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 把游戏的交互网格（`QuadGridManager`，1.2 米一格）对齐到工位实际摆放的位置。
    ///
    /// 为什么要它：`_2/_3/_4` 的布局把工位摆在 0.6 米的奇数倍上，而游戏网格原点是 0，
    /// 格心在 1.2 米的整数倍上——相邻两个工位四舍五入进同一格。`GridManager.OccupyGrid`
    /// 只留先到的那个，后一个永远选不中（实测 `_3` 丢了 dispenser-2/6、cooker-3/5，
    /// 厨师站在它面前按键，拿起的是隔壁那箱）。关卡包只能在 Windows 的 Unity 里重打，
    /// 所以在插件里改：原点只在"不挪 / 挪半格"四种里选重叠最少的，然后让每个
    /// `StaticGridLocation` 按新原点重新占格。
    ///
    /// 时机：工位是陆续加载出来的，后加载的会在 `Awake` 里按当时的原点占格，所以
    /// 先挪原点也不会错；工位数变了就再评一次，直到不再变好。
    /// </summary>
    internal static class GridAlign
    {
        private static ManualLogSource _log;

        /// <summary>每个网格实例最初的原点（按实例号），挪过以后仍以它为基准选候选。</summary>
        private static readonly Dictionary<int, Vector3> BaseOrigin = new Dictionary<int, Vector3>();

        /// <summary>上次评估时这张网格上的定点物体数：没变就不重算。</summary>
        private static readonly Dictionary<int, int> EvaluatedCount = new Dictionary<int, int>();

        /// <summary>这些组件把自己的格号缓存在 `m_gridIndex` 里，挪原点后要跟着改。</summary>
        private static readonly string[] CachedIndexTypes =
        {
            "ServerConveyorStation", "Travelator", "ServerCookingRegion", "ClientCookingRegion"
        };

        public static void Install(ManualLogSource log)
        {
            _log = log;
        }

        /// <summary>评估并（需要时）挪动所有活动的方格网格。返回是否挪了。</summary>
        public static bool Apply()
        {
            // 只动自己做的关卡：原版关卡的工位本来就落在格心，不碰
            if (!UnityEngine.SceneManagement.SceneManager.GetActiveScene().name.StartsWith("s_wellcooked"))
            {
                return false;
            }

            bool moved = false;
            var locations = Object.FindObjectsOfType<StaticGridLocation>();
            for (int i = 0; i < GridManager.GetActiveCount(); i++)
            {
                var grid = GridManager.GetActive(i) as QuadGridManager;
                if (grid != null && Align(grid, locations))
                {
                    moved = true;
                }
            }

            return moved;
        }

        private static bool Align(QuadGridManager grid, StaticGridLocation[] all)
        {
            var mine = new List<StaticGridLocation>();
            foreach (var location in all)
            {
                if (location != null && location.AccessGridManager == grid)
                {
                    mine.Add(location);
                }
            }

            int id = grid.GetInstanceID();
            int seen;
            if (mine.Count == 0 || (EvaluatedCount.TryGetValue(id, out seen) && seen == mine.Count))
            {
                return false;
            }

            EvaluatedCount[id] = mine.Count;
            Vector3 baseOrigin;
            if (!BaseOrigin.TryGetValue(id, out baseOrigin))
            {
                baseOrigin = grid.AccessOrigin;
                BaseOrigin[id] = baseOrigin;
            }

            var size = Traverse.Create(grid).Field("m_size").GetValue<Vector3>();
            Vector3 current = grid.AccessOrigin;
            int currentOverlap = Overlap(grid, mine, current, size);
            Vector3 best = current;
            int bestOverlap = currentOverlap;
            // 候选按"挪得少"排在前面：重叠一样多就不挪
            var candidates = new[]
            {
                baseOrigin,
                baseOrigin + new Vector3(size.x * 0.5f, 0f, 0f),
                baseOrigin + new Vector3(0f, 0f, size.z * 0.5f),
                baseOrigin + new Vector3(size.x * 0.5f, 0f, size.z * 0.5f)
            };
            foreach (var origin in candidates)
            {
                int overlap = Overlap(grid, mine, origin, size);
                if (overlap < bestOverlap)
                {
                    best = origin;
                    bestOverlap = overlap;
                }
            }

            if (best == current)
            {
                Diag.Trace("grid: 原点 " + Fmt(current) + " 不动，重叠 " + currentOverlap + "（定点物体 " + mine.Count + " 个）");
                return false;
            }

            grid.AccessOrigin = best;
            Reoccupy(grid, mine);
            RefreshCachedIndices(grid);
            _log.LogInfo("grid: 场景 " + UnityEngine.SceneManagement.SceneManager.GetActiveScene().name
                         + " 网格原点 " + Fmt(current) + "→" + Fmt(best) + "，同格重叠 " + currentOverlap + "→"
                         + bestOverlap + "（定点物体 " + mine.Count + " 个）");
            return true;
        }

        /// <summary>按某个原点算：有多少个定点物体会和别人挤进同一格。</summary>
        private static int Overlap(QuadGridManager grid, List<StaticGridLocation> mine, Vector3 origin, Vector3 size)
        {
            var cells = new HashSet<GridIndex>(default(GridIndex));
            int overlap = 0;
            foreach (var location in mine)
            {
                Vector3 local = grid.transform.InverseTransformPoint(location.transform.position) - origin;
                var index = new GridIndex(Mathf.RoundToInt(local.x / size.x), Mathf.RoundToInt(local.y / size.y),
                                          Mathf.RoundToInt(local.z / size.z));
                if (!cells.Add(index))
                {
                    overlap++;
                }
            }

            return overlap;
        }

        /// <summary>
        /// 先把这些物体自己占的格全放掉，再按新原点重新占。原来占着格的先占，
        /// 保住"台面先、台面上的东西后"这类本来就靠先后顺序定下来的归属。
        /// </summary>
        private static void Reoccupy(QuadGridManager grid, List<StaticGridLocation> mine)
        {
            var first = new List<StaticGridLocation>();
            var rest = new List<StaticGridLocation>();
            foreach (var location in mine)
            {
                (location.IsGridOccupant() ? first : rest).Add(location);
            }

            foreach (var location in first)
            {
                grid.DeoccupyGrid(location.GridIndex);
            }

            foreach (var location in first)
            {
                location.Initialise();
            }

            foreach (var location in rest)
            {
                location.Initialise();
            }
        }

        private static void RefreshCachedIndices(QuadGridManager grid)
        {
            foreach (var typeName in CachedIndexTypes)
            {
                var type = AccessTools.TypeByName(typeName);
                if (type == null)
                {
                    continue;
                }

                foreach (var found in Object.FindObjectsOfType(type))
                {
                    var component = found as Component;
                    if (component == null || GameUtils.GetGridManager(component.transform) != grid)
                    {
                        continue;
                    }

                    Traverse.Create(component).Field("m_gridIndex")
                            .SetValue(grid.GetGridLocationFromPos(component.transform.position));
                    // 传送带的"下一格接收者"是按旧格号算的
                    var update = AccessTools.Method(type, "UpdateAdjacentReceiver");
                    if (update != null && update.GetParameters().Length == 0)
                    {
                        update.Invoke(component, null);
                    }
                }
            }
        }

        private static string Fmt(Vector3 v)
        {
            return "(" + v.x.ToString("F2") + "," + v.z.ToString("F2") + ")";
        }
    }
}
