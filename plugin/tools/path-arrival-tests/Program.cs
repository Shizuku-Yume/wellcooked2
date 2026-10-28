using System;
using WellCooked;

internal static class Program
{
    private static int _failures;

    private static int Main()
    {
        const float tolerance = 0.3f;
        Check(PathArrival.IsWithinTolerance(0.18f, 0.24f, 0f, 0f, tolerance), "3-4-5 endpoint exactly at 0.3 m is accepted");
        Check(PathArrival.IsWithinTolerance(0f, 0.299f, 0f, 0f, tolerance), "endpoint just inside 0.3 m is accepted");
        Check(!PathArrival.IsWithinTolerance(0f, 0.301f, 0f, 0f, tolerance), "endpoint just outside 0.3 m is rejected");
        var wobble = new[] { 22f, 23f, 22f, 23f, 22f, 23f, 22f, 23f };
        int beforeFlips = 0, afterFlips = 0, previousBefore = -1, previousAfter = -1;
        foreach (float degrees in wobble)
        {
            int before = ((int)Math.Round(degrees / 45f) % 8 + 8) % 8;
            int after = PathSteering.Quantize((float)Math.Cos(degrees * Math.PI / 180.0),
                (float)Math.Sin(degrees * Math.PI / 180.0), previousAfter);
            if (previousBefore >= 0 && before != previousBefore) beforeFlips++;
            if (previousAfter >= 0 && after != previousAfter) afterFlips++;
            previousBefore = before;
            previousAfter = after;
        }
        Console.WriteLine("heading replay: before=" + beforeFlips + " after=" + afterFlips + " flips/sequence (8 samples)");
        Check(beforeFlips == 7, "raw steering flips on alternating 22°/23° replay");
        Check(afterFlips == 0, "hysteretic steering holds heading on alternating replay");
        CheckReplay("straight offset 60fps", new[] { (4f, 0f) }, 0f, 0.3f, 60);
        CheckReplay("straight offset 30fps", new[] { (4f, 0f) }, 0f, 0.3f, 30);
        CheckReplay("L turn 60fps", new[] { (2f, 0f), (2f, 2f) }, 0f, 0f, 60);
        CheckReplay("L turn 30fps", new[] { (2f, 0f), (2f, 2f) }, 0f, 0f, 30);
        CheckReplay("22 degree waypoint 60fps", new[] { (4f * (float)Math.Cos(22 * Math.PI / 180), 4f * (float)Math.Sin(22 * Math.PI / 180)) }, 0f, 0f, 60, 0);
        CheckReplay("22 degree waypoint 30fps", new[] { (4f * (float)Math.Cos(22 * Math.PI / 180), 4f * (float)Math.Sin(22 * Math.PI / 180)) }, 0f, 0f, 30, 0);
        var selectionPath = new System.Collections.Generic.List<UnityEngine.Vector2>
        {
            new UnityEngine.Vector2(0.6f, 0f), new UnityEngine.Vector2(1.8f, 0f), new UnityEngine.Vector2(3.0f, 0f)
        };
        int selected = PathArrival.ForwardWaypointIndex(selectionPath, new UnityEngine.Vector2(1.0f, 0f), 0, 0.15f);
        int baseIndex = LegacyStartIndex(selectionPath, new UnityEngine.Vector2(1.0f, 0f));
        Check(selected == 1, "forward projection selects waypoint 1 for chef at (1,0) facing +x");
        Check(baseIndex == 0, "base ActionDriver selector fails: starts at behind waypoint 0");
        return _failures == 0 ? 0 : 1;
    }
    private static int LegacyStartIndex(System.Collections.Generic.List<UnityEngine.Vector2> path, UnityEngine.Vector2 position)
    {
        int index = 0;
        while (index < path.Count - 1)
        {
            UnityEngine.Vector2 segment = path[index + 1] - path[index];
            UnityEngine.Vector2 offset = position - path[index];
            if (offset.sqrMagnitude <= 0.15f * 0.15f) { index++; continue; }
            float lengthSquared = segment.sqrMagnitude;
            float projection = UnityEngine.Vector2.Dot(offset, segment) / lengthSquared;
            UnityEngine.Vector2 lateral = offset - segment * projection;
            if (projection >= 1f && lateral.sqrMagnitude <= 0.09f) { index++; continue; }
            break;
        }
        return index;
    }
    private static void CheckReplay(string name, (float x, float y)[] waypoints, float startX, float startY, int fps, int initialHeading = -1)
    {
        float x = startX, y = startY;
        int heading = initialHeading;
        foreach (var waypoint in waypoints)
        {
            float directDistance = Distance(x, y, waypoint.x, waypoint.y);
            int maxFrames = (int)Math.Ceiling((directDistance / 4f) * 1.5f * fps);
            int flips = 0;
            float oldAngle = float.NaN;
            bool reached = false;
            for (int frame = 0; frame <= maxFrames; frame++)
            {
                float dx = waypoint.x - x, dy = waypoint.y - y;
                float remaining = (float)Math.Sqrt(dx * dx + dy * dy);
                if (remaining <= 0.15f) { reached = true; break; }
                float moveX, moveY;
                if (remaining <= 0.4f) { moveX = dx / remaining; moveY = dy / remaining; }
                else
                {
                    heading = PathSteering.Quantize(dx, dy, heading, remaining);
                    float angle = heading * 45f * (float)Math.PI / 180f;
                    moveX = (float)Math.Cos(angle); moveY = (float)Math.Sin(angle);
                }
                float moveAngle = (float)(Math.Atan2(moveY, moveX) * 180f / Math.PI);
                if (!float.IsNaN(oldAngle) && Math.Abs(DeltaAngle(oldAngle, moveAngle)) > 30f) flips++;
                oldAngle = moveAngle;
                x += moveX * 4f / fps; y += moveY * 4f / fps;
            }
            Check(reached, name + " reaches waypoint within 1.5x direct time");
            Check(flips <= 2, name + " has <=2 heading flips per segment");
            heading = -1;
        }
    }


    private static float DeltaAngle(float from, float to)
    {
        float delta = (to - from) % 360f;
        if (delta > 180f) delta -= 360f;
        if (delta < -180f) delta += 360f;
        return delta;
    }

    private static float Distance(float x, float y, float tx, float ty)
    {
        float dx = tx - x, dy = ty - y;
        return (float)Math.Sqrt(dx * dx + dy * dy);
    }

    private static void Check(bool condition, string description)
    {
        Console.WriteLine((condition ? "PASS " : "FAIL ") + description);
        if (!condition) _failures++;
    }
}
