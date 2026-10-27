using System;
using WellCooked;

internal static class Program
{
    private static int Main()
    {
        const float tolerance = 0.3f;
        Check(PathArrival.IsWithinTolerance(0.18f, 0.24f, 0f, 0f, tolerance),
              "3-4-5 endpoint exactly at 0.3 m is accepted");
        Check(PathArrival.IsWithinTolerance(0f, 0.299f, 0f, 0f, tolerance),
              "endpoint just inside 0.3 m is accepted");
        Check(!PathArrival.IsWithinTolerance(0f, 0.301f, 0f, 0f, tolerance),
              "endpoint just outside 0.3 m is rejected");
        var wobble = new[] { 22f, 23f, 22f, 23f, 22f, 23f, 22f, 23f };
        int beforeFlips = 0;
        int afterFlips = 0;
        int previousBefore = -1;
        int previousAfter = -1;
        foreach (float degrees in wobble)
        {
            int before = ((int)Math.Round(degrees / 45f) % 8 + 8) % 8;
            int after = PathSteering.Quantize(
                (float)Math.Cos(degrees * Math.PI / 180.0),
                (float)Math.Sin(degrees * Math.PI / 180.0), previousAfter);
            if (previousBefore >= 0 && before != previousBefore) beforeFlips++;
            if (previousAfter >= 0 && after != previousAfter) afterFlips++;
            previousBefore = before;
            previousAfter = after;
        }
        Console.WriteLine("heading replay: before=" + beforeFlips + " after=" + afterFlips
                          + " flips/sequence (8 samples)");
        Check(beforeFlips == 7, "raw steering flips on alternating 22°/23° replay");
        Check(afterFlips == 0, "hysteretic steering holds heading on alternating replay");
        return _failures == 0 ? 0 : 1;
    }

    private static int _failures;

    private static void Check(bool condition, string description)
    {
        Console.WriteLine((condition ? "PASS " : "FAIL ") + description);
        if (!condition)
        {
            _failures++;
        }
    }
}
