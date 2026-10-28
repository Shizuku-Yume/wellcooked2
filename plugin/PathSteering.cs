using System;

namespace WellCooked
{
    internal static class PathSteering
    {
        private const float HysteresisDegrees = 30f;
        private const float DeadZoneSquared = 0.0004f;

        internal static int Quantize(float x, float y, int current, float remaining)
        {
            if (x * x + y * y <= DeadZoneSquared) return -1;
            float angle = (float)(Math.Atan2(y, x) * 180.0 / Math.PI);
            int best = ((int)Math.Floor(angle / 45f + 0.5f) % 8 + 8) % 8;
            if (current >= 0 && best != current && remaining > 0.4f)
            {
                float currentError = Math.Abs(DeltaAngle(current * 45f, angle));
                float bestError = Math.Abs(DeltaAngle(best * 45f, angle));
                if (currentError - bestError <= HysteresisDegrees) return current;
            }
            return best;
        }

        internal static int Quantize(float x, float y, int current)
        {
            return Quantize(x, y, current, (float)Math.Sqrt(x * x + y * y));
        }

        private static float DeltaAngle(float from, float to)
        {
            float delta = (to - from) % 360f;
            if (delta > 180f) delta -= 360f;
            if (delta < -180f) delta += 360f;
            return delta;
        }
    }
}
