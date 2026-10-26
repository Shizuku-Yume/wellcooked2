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
