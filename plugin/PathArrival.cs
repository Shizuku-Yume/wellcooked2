using System;

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
    }
}
