using System;
using System.Collections.Generic;
using BepInEx.Logging;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 寻路与路径跟随（任务 game-bridge/08）。
    ///
    /// 输入是 `WorldState` 采样出来的可走格（这个游戏没有可用导航网格，可走格是用
    /// `PlayerControls.GroundLayer` 向下打射线得到的），输出是"这一帧往哪个方向走"。
    /// 调度器只负责决定"谁去哪"，走位由这里负责——两层分开，调度不用关心格子。
    ///
    /// 网格约定：**4 邻域**（只走上下左右）。实测 8 邻域不行：胡闹厨房的厨师走位只认四个方向，
    /// 喂对角方向（0.7,0.7）他们会原地卡住不动——日志里 4 名厨师 12 秒坐标不变。
    /// 代价 1；启发式用曼哈顿距离。
    /// </summary>
    public static class Navigation
    {
        private static ManualLogSource _log;

        public static void Install(ManualLogSource log)
        {
            _log = log;
        }

        private sealed class Agent
        {
            public Vector2 Target;
            public List<Vector2> Path;
            public int Waypoint;
            public float NextRepath;
            public bool Arrived;
            public Vector2 LastPosition;
            public float LastMovedAt;
            public bool StuckReported;
            public float SkipAllowed;
            public int GiveUps;
            public float StartDistance = -1f;
            public float MinDistance = float.MaxValue;
            public string Reason = "";
            public int LastAxis = -1;
            public int SlideAxis = -1;
            public float SlideSign = 1f;
            public float SlideUntil;
            public int SlideCount;
        }

        private static readonly Dictionary<object, Agent> Agents = new Dictionary<object, Agent>();

        /// <summary>给某个执行体（手柄槽位或玩家）指定目标点；重复调用同一个目标不重算。</summary>
        /// <summary>上一条目标的结果（走完/放弃时打日志用）。</summary>
        public static string LastResult(object owner)
        {
            Agent agent;
            if (!Agents.TryGetValue(owner, out agent) || agent.StartDistance < 0f)
            {
                return "";
            }

            return "目标=(" + agent.Target.x.ToString("F1") + "," + agent.Target.y.ToString("F1") + ")"
                   + " 起始距离=" + agent.StartDistance.ToString("F1")
                   + " 最近=" + agent.MinDistance.ToString("F1")
                   + " 结束原因=" + (agent.Reason.Length > 0 ? agent.Reason : "换目标");
        }

        public static void Assign(object owner, Vector2 target)
        {
            Agent agent;
            if (!Agents.TryGetValue(owner, out agent))
            {
                agent = new Agent();
                Agents[owner] = agent;
            }

            if (!agent.Arrived && (agent.Target - target).sqrMagnitude < 0.01f)
            {
                return;
            }

            agent.Target = target;
            agent.Path = null;
            agent.Waypoint = 0;
            agent.NextRepath = 0f;
            agent.Arrived = false;
            agent.GiveUps = 0;
            agent.StuckReported = false;
            agent.StartDistance = -1f;
            agent.MinDistance = float.MaxValue;
            agent.Reason = "";
        }

        public static void Release(object owner)
        {
            Agents.Remove(owner);
        }

        public static void Clear()
        {
            Agents.Clear();
        }

        /// <summary>当前目标（没登记过返回 false）。</summary>
        public static bool TryTarget(object owner, out Vector2 target)
        {
            Agent agent;
            if (Agents.TryGetValue(owner, out agent))
            {
                target = agent.Target;
                return true;
            }

            target = Vector2.zero;
            return false;
        }

        /// <summary>有没有登记过目标。</summary>
        public static bool HasTarget(object owner)
        {
            return Agents.ContainsKey(owner);
        }

        public static bool HasArrived(object owner)
        {
            Agent agent;
            return Agents.TryGetValue(owner, out agent) && agent.Arrived;
        }

        /// <summary>本帧该往哪走；到达或没有路径时返回零向量。</summary>
        public static Vector2 Steer(object owner, Vector2 position, float now, out string status)
        {
            status = "";
            Agent agent;
            if (!Agents.TryGetValue(owner, out agent))
            {
                status = "没登记目标";
                return Vector2.zero;
            }

            if (!WorldState.HasTerrain)
            {
                status = "没有地形";
                return Vector2.zero;
            }

            // 卡住判定：位置 1.5 秒没动过就是卡住了（不是"慢慢走"）
            if (Vector2.Distance(position, agent.LastPosition) > 0.05f)
            {
                agent.LastPosition = position;
                agent.LastMovedAt = now;
                agent.StuckReported = false;
                agent.SlideCount = 0;
            }
            else if (now - agent.LastMovedAt > 0.4f && now >= agent.SlideUntil
                     && Vector2.Distance(position, agent.Target) < 3f)
            {
                // 侧向错身：两个厨师面对面互推时（路点正好在对穿位置），谁都不让、双双卡死
                // （实测两人坐标完全不动）。按玩家位奇偶决定往哪边让，两边正好相反。
                //
                // 只在**离目标很近**时才错身：那是"两个厨师对着同一个点互推"的场景。
                // 离得远时还错身就成了乱走——错身期间厨师一直在小幅移动，卡住计时攒不够，
                // 永远不重算路径，20 秒只挪半米（实测）。
                // 另外错身本身有界：连续 2 次还推不动就丢路径让上层重算。
                agent.SlideCount++;
                if (agent.SlideCount > 2)
                {
                    agent.SlideCount = 0;
                    agent.Path = null;
                    agent.SlideUntil = 0f;
                }
                else
                {
                    agent.SlideAxis = agent.LastAxis == 0 ? 1 : 0;
                    agent.SlideSign = SideOf(owner);
                    agent.SlideUntil = now + 0.6f;
                }
            }

            if (now < agent.SlideUntil)
            {
                status = "侧向错身 " + status;
                return agent.SlideAxis == 0
                    ? new Vector2(agent.SlideSign, 0f)
                    : new Vector2(0f, agent.SlideSign);
            }

            if (agent.Path != null && now - agent.LastMovedAt > 0.4f && now >= agent.SkipAllowed)
            {
                // 局部容错：网格是物理采样出来的，永远不可能和游戏碰撞完全对齐。
                // 喂了方向 0.4 秒还推不动，就跳过这个路点去下一个——比顶着台面推墙强。
                // （1.5 秒那个分支是"整条路都不通"，会放弃目标换下一个。）
                agent.SkipAllowed = now + 0.4f;
                agent.Waypoint++;
                status = "跳过被挡的路点";
                if (agent.Waypoint >= agent.Path.Count)
                {
                    // 不置 Arrived：置了上层就换目标、把路点指针拨回 0，
                    // 厨师永远走不完一条长路（实测换目标 148 次/150 秒、到达 0）。
                    // 改成丢掉这条路径、下一帧从当前位置重算。
                    agent.Path = null;
                    status = "路点都被挡住，重算路径";
                    return Vector2.zero;
                }
            }
            else if (!agent.StuckReported && now - agent.LastMovedAt > 1.5f)
            {
                // 报一次卡住，同时把路径清掉让上层换目标。只报不清的话，
                // 下一帧又会朝同一面墙推（实测贴墙站着不动、日志里只剩不可达）。
                agent.StuckReported = true;
                agent.Path = null;
                agent.GiveUps++;
                if (agent.GiveUps >= 3)
                {
                    // 同一个目标连续三次推不动，才算真做不到，换目标
                    agent.Arrived = true;
                    agent.Reason = "卡住";
                    status = "卡住（" + (now - agent.LastMovedAt).ToString("F1") + " 秒没动），放弃这个目标";
                }
                else
                {
                    status = "卡住（" + (now - agent.LastMovedAt).ToString("F1") + " 秒没动），重算路径";
                }

                return Vector2.zero;
            }

            float toTarget = Vector2.Distance(position, agent.Target);
            if (agent.StartDistance < 0f)
            {
                agent.StartDistance = toTarget;
            }

            if (toTarget < agent.MinDistance)
            {
                agent.MinDistance = toTarget;
            }

            if (toTarget <= ArriveRadius)
            {
                agent.Arrived = true;
                agent.Reason = "到达";
                status = "已到达";
                return Vector2.zero;
            }

            if (agent.Path == null)
            {
                // 只在"没有路"时重算。周期性重算会把路点指针拨回 0，
                // 而重算起点是厨师当前所在格，路一变方向就变，厨师就在原地来回蹭
                // （实测 x 在 20.0~20.4、13.0~13.5 之间反复）。卡住时由卡住分支清掉路径。
                agent.NextRepath = now + 2f;
                agent.Path = FindPath(position, agent.Target);
                agent.Waypoint = 0;
                if (agent.Path == null)
                {
                    agent.Arrived = true; // 别每帧重算，让上层换目标
                    agent.Reason = "不可达";
                    status = "不可达（目标 " + agent.Target.x.ToString("F1") + "," + agent.Target.y.ToString("F1") + "）";
                    return Vector2.zero;
                }
            }

            // 跳过已经走过的路点
            while (agent.Waypoint < agent.Path.Count
                   && Vector2.Distance(position, agent.Path[agent.Waypoint]) < WaypointRadius)
            {
                agent.Waypoint++;
            }

            if (agent.Waypoint >= agent.Path.Count)
            {
                // 走到路径终点就算到了。目标点是台面中心（锅/出菜台/食材箱），
                // 厨师根本站不上去，用"离目标 ≤0.45 格"判到达永远不成立——
                // 实测厨师在目标旁边绕圈、剩余距离不降、0 次到达。
                agent.Arrived = true;
                agent.Reason = "路径终点";
                status = "已到达（路径终点，距目标 " + toTarget.ToString("F1") + "）";
                return Vector2.zero;
            }

            var next = agent.Path[agent.Waypoint];
            var direction = next - position;

            // 游戏里的厨师只走四个方向，喂对角方向他会走出锯齿、贴着台面蹭。
            // 取分量大的那个轴，给纯轴向。
            direction = Math.Abs(direction.x) >= Math.Abs(direction.y)
                ? new Vector2(Math.Sign(direction.x), 0f)
                : new Vector2(0f, Math.Sign(direction.y));
            agent.LastAxis = Math.Abs(next.x - position.x) >= Math.Abs(next.y - position.y) ? 0 : 1;
            status = "路点 " + agent.Waypoint + "/" + agent.Path.Count + " 剩余 " + toTarget.ToString("F1")
                     + " 路点坐标=(" + next.x.ToString("F2") + "," + next.y.ToString("F2") + ")"
                     + " 目标=(" + agent.Target.x.ToString("F2") + "," + agent.Target.y.ToString("F2") + ")";
            if (direction.sqrMagnitude < 0.0001f)
            {
                status = "路点与当前位置重合 " + status;
                return Vector2.zero;
            }

            return direction.normalized;
        }

        /// <summary>
        /// "到了"的判定半径。目标点是工位中心（台面里），厨师站不上去，
        /// 用 0.45 格永远不成立。一格（0.6 米）出头的距离就是"站在工位旁边"，
        /// 之后能按交互键即可——具体阈值要跟任务 09 的工序一起定。
        /// </summary>
        private const float ArriveRadius = 1.2f;

        /// <summary>
        /// "算到了这个路点"的半径。取格距的一半左右：厨师是连续移动的，
        /// 很少正好站在格心上，半径太小就会永远够不到那个格心。
        /// </summary>
        private static float WaypointRadius
        {
            get { return WorldState.Step * 0.45f; }
        }

        /// <summary>A*：找不到路返回 null（调用方要明确报"不可达"，不能原地打转）。</summary>
        public static List<Vector2> FindPath(Vector2 from, Vector2 to)
        {
            int sx, sz, tx, tz;
            WorldState.WorldToCell(from.x, from.y, out sx, out sz);
            WorldState.WorldToCell(to.x, to.y, out tx, out tz);

            // 厨师是连续移动的，站位常常落在"格心不可站"的地方（贴着台面走、两格之间），
            // 这时起点格查不到，A* 一步都扩不出去、一律报不可达（实测厨师走进死角后再也不动）。
            // 起点也就近取一个可走格。
            if (!WorldState.IsWalkableCell(sx, sz))
            {
                if (!FindNearestWalkable(sx, sz, out sx, out sz))
                {
                    return null;
                }
            }

            if (!WorldState.IsWalkableCell(tx, tz))
            {
                // 目标格不可走时，就近找一个可走格（台面旁边站不住）
                if (!FindNearestWalkable(tx, tz, out tx, out tz))
                {
                    return null;
                }
            }

            if (sx == tx && sz == tz)
            {
                return new List<Vector2> { WorldState.CellCenter(tx, tz) };
            }

            var open = new BinaryHeap();
            var cameFrom = new Dictionary<int, int>();
            var gScore = new Dictionary<int, float>();
            var closed = new HashSet<int>();

            int start = Key(sx, sz);
            int goal = Key(tx, tz);
            gScore[start] = 0f;
            open.Push(start, Heuristic(sx, sz, tx, tz));

            int guard = 0;
            while (open.Count > 0)
            {
                if (++guard > 200000)
                {
                    return null;
                }

                int current = open.Pop();
                if (current == goal)
                {
                    return Reconstruct(cameFrom, current);
                }

                if (!closed.Add(current))
                {
                    continue;
                }

                int cx = current >> 16;
                int cz = current & 0xffff;
                foreach (var offset in Steps)
                {
                    int nx = cx + offset[0];
                    int nz = cz + offset[1];
                    if (!WorldState.CanStep(cx, cz, offset[0], offset[1]))
                    {
                        continue;
                    }

                    {
                        int neighbor = Key(nx, nz);
                        if (closed.Contains(neighbor))
                        {
                            continue;
                        }

                        const float step = 1f;
                        float tentative = gScore[current] + step;
                        float known;
                        if (gScore.TryGetValue(neighbor, out known) && tentative >= known)
                        {
                            continue;
                        }

                        gScore[neighbor] = tentative;
                        cameFrom[neighbor] = current;
                        open.Push(neighbor, tentative + Heuristic(nx, nz, tx, tz));
                    }
                }
            }

            return null;
        }

        /// <summary>就近找可走格：按距离取最近的，不是按扫描顺序取第一个。</summary>
        private static bool FindNearestWalkable(int cx, int cz, out int foundX, out int foundZ)
        {
            foundX = cx;
            foundZ = cz;
            int best = int.MaxValue;
            bool found = false;
            for (int radius = 1; radius <= 20; radius++)
            {
                for (int dx = -radius; dx <= radius; dx++)
                {
                    for (int dz = -radius; dz <= radius; dz++)
                    {
                        if (Math.Abs(dx) != radius && Math.Abs(dz) != radius)
                        {
                            continue; // 只看这一圈的边
                        }

                        if (!WorldState.IsWalkableCell(cx + dx, cz + dz))
                        {
                            continue;
                        }

                        int distance = dx * dx + dz * dz;
                        if (distance < best)
                        {
                            best = distance;
                            foundX = cx + dx;
                            foundZ = cz + dz;
                            found = true;
                        }
                    }
                }

                if (found)
                {
                    return true;
                }
            }

            return false;
        }

        private static List<Vector2> Reconstruct(Dictionary<int, int> cameFrom, int current)
        {
            var path = new List<Vector2> { WorldState.CellCenter(current >> 16, current & 0xffff) };
            while (cameFrom.ContainsKey(current))
            {
                current = cameFrom[current];
                path.Add(WorldState.CellCenter(current >> 16, current & 0xffff));
            }

            path.Reverse();
            var simplified = Simplify(path);

            // 第一个路点是"厨师自己所在格的中心"，走过去是 0.6 格回头路；
            // 而且每次重算路径都会把它拨回来，厨师就在两个格心之间来回（实测方向反复翻转）。
            // 起点格直接丢掉：已经在里面了，下一个格才是要去的。
            if (simplified.Count > 1)
            {
                simplified.RemoveAt(0);
            }

            return simplified;
        }

        /// <summary>
        /// 只留拐点：0.5 格一个路点太密，厨师一步就跨过去、在路点附近来回摆，
        /// 永远推进不到下一个（实测"路点 1/N 剩余不变、0 次到达"）。留拐点后是长直线段。
        /// </summary>
        private static List<Vector2> Simplify(List<Vector2> path)
        {
            if (path.Count <= 2)
            {
                return path;
            }

            var simplified = new List<Vector2> { path[0] };
            for (int i = 1; i < path.Count - 1; i++)
            {
                var previous = path[i - 1];
                var current = path[i];
                var next = path[i + 1];
                var a = current - previous;
                var b = next - current;
                if (Math.Abs(a.x - b.x) > 0.01f || Math.Abs(a.y - b.y) > 0.01f)
                {
                    simplified.Add(current);
                }
            }

            simplified.Add(path[path.Count - 1]);
            return simplified;
        }

        /// <summary>让路的朝向：按玩家位奇偶决定，两个互相顶住的厨师会往相反方向让。</summary>
        private static float SideOf(object owner)
        {
            int index;
            if (owner is int)
            {
                index = (int)owner;
            }
            else
            {
                index = System.Convert.ToInt32(owner);
            }

            return index % 2 == 0 ? 1f : -1f;
        }

        private static int Key(int x, int z)
        {
            return (x << 16) | (z & 0xffff);
        }

        private static float Heuristic(int x, int z, int gx, int gz)
        {
            return Math.Abs(x - gx) + Math.Abs(z - gz);
        }

        /// <summary>只走四个方向：游戏里的厨师走位只认上下左右。</summary>
        private static readonly int[][] Steps = new[]
        {
            new[] { 1, 0 }, new[] { -1, 0 }, new[] { 0, 1 }, new[] { 0, -1 }
        };

        /// <summary>net35 没有优先队列，自己写一个二叉堆。</summary>
        private sealed class BinaryHeap
        {
            private readonly List<int> _items = new List<int>();
            private readonly List<float> _priorities = new List<float>();

            public int Count
            {
                get { return _items.Count; }
            }

            public void Push(int item, float priority)
            {
                _items.Add(item);
                _priorities.Add(priority);
                int i = _items.Count - 1;
                while (i > 0)
                {
                    int parent = (i - 1) / 2;
                    if (_priorities[parent] <= _priorities[i])
                    {
                        break;
                    }

                    Swap(parent, i);
                    i = parent;
                }
            }

            public int Pop()
            {
                int result = _items[0];
                int last = _items.Count - 1;
                _items[0] = _items[last];
                _priorities[0] = _priorities[last];
                _items.RemoveAt(last);
                _priorities.RemoveAt(last);

                int i = 0;
                while (true)
                {
                    int left = 2 * i + 1;
                    int right = left + 1;
                    int smallest = i;
                    if (left < _items.Count && _priorities[left] < _priorities[smallest])
                    {
                        smallest = left;
                    }

                    if (right < _items.Count && _priorities[right] < _priorities[smallest])
                    {
                        smallest = right;
                    }

                    if (smallest == i)
                    {
                        break;
                    }

                    Swap(i, smallest);
                    i = smallest;
                }

                return result;
            }

            private void Swap(int a, int b)
            {
                int item = _items[a];
                _items[a] = _items[b];
                _items[b] = item;
                float priority = _priorities[a];
                _priorities[a] = _priorities[b];
                _priorities[b] = priority;
            }
        }
    }
}
