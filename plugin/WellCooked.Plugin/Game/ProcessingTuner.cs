using System;
using System.IO;
using System.Reflection;
using BepInEx;
using HarmonyLib;
using UnityEngine;

namespace WellCooked.Game
{
    /// <summary>
    /// Applies the layout editor's processing-time overrides to the running game.
    ///
    /// The editor writes BepInEx\wellcooked-processing.json through server.py. This class
    /// reads the same file and applies:
    ///   cook_time       -> CookingHandler.m_cookingtime   (pots / pans; burn at 2x)
    ///   chop_stages     -> WorkableItem.m_stages          (new ingredients)
    ///   chop_multiplier -> WorkableItem.GetChopTimeMultiplier() (single player only)
    ///
    /// The three ranges MUST match layout-editor/server.py PROCESSING_RANGES:
    ///   cook_time 1..30 s (step 0.5), chop_stages 2..8, chop_multiplier 1..10.
    /// </summary>
    public static class ProcessingTuner
    {
        public const float CookMin = 1f;
        public const float CookMax = 30f;
        public const int StageMin = 2;
        public const int StageMax = 8;
        public const int MultMin = 1;
        public const int MultMax = 10;

        [Serializable]
        public sealed class ProcessingConfig
        {
            public int version = 1;
            public float cook_time = 10f;
            public int chop_stages = 8;
            public int chop_multiplier = 5;
            public string updated_at;
            public string source;
        }

        private static Action<string> _log;
        private static ProcessingConfig _cfg = new ProcessingConfig();
        private static long _lastWriteTicks = -1;
        private static float _nextTick;

        private static Type _cookingHandlerType;
        private static FieldInfo _cookingTimeField;
        private static Type _workableType;
        private static FieldInfo _stagesField;
        private static bool _patched;

        private static int _lastLogPots = -1;
        private static float _lastLogCook = -1f;
        private static int _lastLogStages = -1;
        private static int _lastLogMult = -1;

        // wellcooked_demo_3 is the "fast chopping" level: fewer stages per ingredient and a
        // larger single-player time multiplier, so a dish can be prepared inside a 2 minute
        // round. Other levels keep whatever the editor's processing config says.
        private const int TunedChopStages = 2;
        private const int TunedChopMultiplier = 8;

        private static bool InTunedLevel()
        {
            try
            {
                UnityEngine.SceneManagement.Scene scene =
                    UnityEngine.SceneManagement.SceneManager.GetActiveScene();
                return scene.IsValid()
                    && scene.name.IndexOf("wellcooked_demo_3", StringComparison.OrdinalIgnoreCase) >= 0;
            }
            catch
            {
                return false;
            }
        }

        public static string ConfigPath
        {
            get { return Path.Combine(Paths.BepInExRootPath, "wellcooked-processing.json"); }
        }

        public static void Init(Harmony harmony, Action<string> log)
        {
            _log = log;
            ResolveTypes();
            LoadConfig(true);
            Patch(harmony);
            Log("[Processing] config=" + ConfigPath);
        }

        public static void Shutdown()
        {
            _log = null;
        }

        public static void Tick()
        {
            if (Time.realtimeSinceStartup < _nextTick) return;
            _nextTick = Time.realtimeSinceStartup + 2f;
            bool reloaded = ReloadIfChanged();
            ApplyCookTime(reloaded);
        }

        private static void ResolveTypes()
        {
            _cookingHandlerType = AccessTools.TypeByName("CookingHandler");
            if (_cookingHandlerType != null)
            {
                _cookingTimeField = _cookingHandlerType.GetField("m_cookingtime",
                    BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            }
            _workableType = AccessTools.TypeByName("WorkableItem");
            if (_workableType != null)
            {
                _stagesField = _workableType.GetField("m_stages",
                    BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            }
        }

        private static void Patch(Harmony harmony)
        {
            if (_patched) return;
            try
            {
                if (_workableType != null)
                {
                    MethodInfo awake = _workableType.GetMethod("Awake",
                        BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                    if (awake != null)
                    {
                        harmony.Patch(awake, postfix: new HarmonyMethod(
                            typeof(ProcessingTuner).GetMethod("WorkableAwakePostfix",
                                BindingFlags.Static | BindingFlags.NonPublic)));
                    }
                    MethodInfo mult = _workableType.GetMethod("GetChopTimeMultiplier",
                        BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                        null, new[] { typeof(int) }, null);
                    if (mult != null)
                    {
                        harmony.Patch(mult, postfix: new HarmonyMethod(
                            typeof(ProcessingTuner).GetMethod("ChopMultiplierPostfix",
                                BindingFlags.Static | BindingFlags.NonPublic)));
                    }
                }
                _patched = true;
                Log("[Processing] patched WorkableItem=" + (_workableType != null)
                    + " CookingHandler=" + (_cookingHandlerType != null));
            }
            catch (Exception e)
            {
                Log("[Processing] patch failed: " + e.Message);
            }
        }

        private static void WorkableAwakePostfix(object __instance)
        {
            if (__instance == null || _stagesField == null) return;
            try
            {
                _stagesField.SetValue(__instance, InTunedLevel() ? TunedChopStages : _cfg.chop_stages);
            }
            catch { }
        }

        private static void ChopMultiplierPostfix(ref int __result)
        {
            // The game returns 1 for 2P+; only override the single-player multiplier (>1)
            // so a 1P setting can never make a 2P game slower than the game's own rule.
            if (__result > 1) __result = InTunedLevel() ? TunedChopMultiplier : _cfg.chop_multiplier;
        }

        private static void ApplyCookTime(bool log)
        {
            if (_cookingHandlerType == null || _cookingTimeField == null) return;
            try
            {
                UnityEngine.Object[] all = UnityEngine.Object.FindObjectsOfType(_cookingHandlerType);
                if (all == null) return;
                for (int i = 0; i < all.Length; i++)
                {
                    _cookingTimeField.SetValue(all[i], _cfg.cook_time);
                }
                if (log || all.Length != _lastLogPots)
                {
                    Log("[Processing] applied cook_time=" + _cfg.cook_time
                        + "s to " + all.Length + " cooking handlers");
                }
                _lastLogPots = all.Length;
            }
            catch (Exception e)
            {
                if (log) Log("[Processing] apply failed: " + e.Message);
            }
        }

        private static bool ReloadIfChanged()
        {
            try
            {
                string path = ConfigPath;
                if (!File.Exists(path)) return false;
                long ticks = File.GetLastWriteTimeUtc(path).Ticks;
                if (ticks == _lastWriteTicks) return false;
                _lastWriteTicks = ticks;
                LoadConfig(false);
                return true;
            }
            catch (Exception e)
            {
                Log("[Processing] reload failed: " + e.Message);
                return false;
            }
        }

        private static void LoadConfig(bool initial)
        {
            ProcessingConfig cfg = new ProcessingConfig();
            string path = ConfigPath;
            try
            {
                if (File.Exists(path))
                {
                    string text = File.ReadAllText(path);
                    ProcessingConfig parsed = JsonUtility.FromJson<ProcessingConfig>(text);
                    if (parsed != null) cfg = parsed;
                }
            }
            catch (Exception e)
            {
                Log("[Processing] read failed (" + path + "): " + e.Message);
            }

            cfg.cook_time = ClampCook(cfg.cook_time);
            cfg.chop_stages = ClampStages(cfg.chop_stages);
            cfg.chop_multiplier = ClampMult(cfg.chop_multiplier);
            _cfg = cfg;

            if (initial || cfg.cook_time != _lastLogCook || cfg.chop_stages != _lastLogStages
                || cfg.chop_multiplier != _lastLogMult)
            {
                Log("[Processing] loaded cook_time=" + cfg.cook_time + "s chop_stages="
                    + cfg.chop_stages + " chop_multiplier=" + cfg.chop_multiplier
                    + (File.Exists(path) ? "" : " (defaults, file missing)"));
                _lastLogCook = cfg.cook_time;
                _lastLogStages = cfg.chop_stages;
                _lastLogMult = cfg.chop_multiplier;
            }
        }

        private static float ClampCook(float v)
        {
            if (float.IsNaN(v) || float.IsInfinity(v) || v <= 0f) return 10f;
            v = Mathf.Clamp(v, CookMin, CookMax);
            return Mathf.Round(v / 0.5f) * 0.5f;
        }

        private static int ClampStages(int v)
        {
            if (v <= 0) return 8;
            return Mathf.Clamp(v, StageMin, StageMax);
        }

        private static int ClampMult(int v)
        {
            if (v <= 0) return 5;
            return Mathf.Clamp(v, MultMin, MultMax);
        }

        private static void Log(string message)
        {
            if (_log != null) _log(message);
        }
    }
}
