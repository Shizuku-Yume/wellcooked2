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
        }

        private static readonly Dictionary<object, Agent> Agents = new Dictionary<object, Agent>();

        /// <summary>给某个执行体（手柄槽位或玩家）指定目标点；重复调用同一个目标不重算。</summary>
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
        }

        public static void Release(object owner)
        {
            Agents.Remove(owner);
        }

        public static void Clear()
        {
            Agents.Clear();
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
                return Vector2.zero;
            }

            if (!WorldState.HasTerrain)
            {
                status = "没有地形";
                return Vector2.zero;
            }

            float toTarget = Vector2.Distance(position, agent.Target);
            if (toTarget <= ArriveRadius)
            {
                agent.Arrived = true;
                status = "已到达";
                return Vector2.zero;
            }

            if (agent.Path == null || now >= agent.NextRepath)
            {
                agent.NextRepath = now + 1f;
                agent.Path = FindPath(position, agent.Target);
                agent.Waypoint = 0;
                if (agent.Path == null)
                {
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
                status = "路径走完";
                return Vector2.zero;
            }

            var next = agent.Path[agent.Waypoint];
            var direction = next - position;
            if (direction.sqrMagnitude < 0.0001f)
            {
                return Vector2.zero;
            }

            status = "路点 " + agent.Waypoint + "/" + agent.Path.Count + " 剩余 " + toTarget.ToString("F1");
            return direction.normalized;
        }

        private const float ArriveRadius = 0.45f;
        private const float WaypointRadius = 0.3f;

        /// <summary>A*：找不到路返回 null（调用方要明确报"不可达"，不能原地打转）。</summary>
        public static List<Vector2> FindPath(Vector2 from, Vector2 to)
        {
            int sx, sz, tx, tz;
            WorldState.WorldToCell(from.x, from.y, out sx, out sz);
            WorldState.WorldToCell(to.x, to.y, out tx, out tz);

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
                    if (!WorldState.IsWalkableCell(nx, nz))
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

        private static bool FindNearestWalkable(int cx, int cz, out int foundX, out int foundZ)
        {
            for (int radius = 1; radius <= 4; radius++)
            {
                for (int dx = -radius; dx <= radius; dx++)
                {
                    for (int dz = -radius; dz <= radius; dz++)
                    {
                        if (WorldState.IsWalkableCell(cx + dx, cz + dz))
                        {
                            foundX = cx + dx;
                            foundZ = cz + dz;
                            return true;
                        }
                    }
                }
            }

            foundX = cx;
            foundZ = cz;
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
            return path;
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
