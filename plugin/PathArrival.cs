using System.Collections.Generic;
using UnityEngine;

namespace WellCooked
{
    /// <summary>Geometric arrival check for a navd path endpoint, not an interaction radius.</summary>
    public static class PathArrival
    {
        public static bool IsWithinTolerance(float x, float y, float targetX, float targetY,
                                             float tolerance)
        {
            float dx = x - targetX;
            float dy = y - targetY;
            return dx * dx + dy * dy <= tolerance * tolerance;
        }
        public static int ForwardWaypointIndex(IList<Vector2> waypoints, Vector2 position,
                                               int index, float cornerTolerance)
        {
            while (index < waypoints.Count - 1)
            {
                Vector2 segment = waypoints[index + 1] - waypoints[index];
                Vector2 past = position - waypoints[index];
                float lengthSquared = segment.sqrMagnitude;
                if (past.sqrMagnitude <= cornerTolerance * cornerTolerance)
                {
                    index++;
                    continue;
                }
                if (lengthSquared < 0.0001f) break;
                float projection = Vector2.Dot(past, segment) / lengthSquared;
                Vector2 lateral = past - segment * projection;
                if (projection > 0f && lateral.sqrMagnitude < 0.16f)
                {
                    index++;
                    continue;
                }
                break;
            }
            return index;
        }
    }
}
