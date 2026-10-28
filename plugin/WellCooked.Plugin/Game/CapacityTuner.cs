using System;
using System.Reflection;
using HarmonyLib;
using UnityEngine;

namespace WellCooked.Game
{
    /// <summary>
    /// Clamps every cooking utensil to a single ingredient in wellcooked_demo_3.
    ///
    /// The level asks for "one ingredient at a time" per pot / pan / fryer: the second
    /// ingredient has to wait until the first one has been taken out. The capacity lives on
    /// the runtime ingredient container (`IngredientContainer.m_capacity`, default 3), not on
    /// the level layout, so it is clamped here every time the level is entered.
    ///
    /// Only containers that belong to a cooking object are touched; plates, crates and the
    /// serving window keep their normal capacity. Any other level is left alone.
    /// </summary>
    public static class CapacityTuner
    {
        private const int TunedCapacity = 1;

        private static Action<string> _log;
        private static Type _containerType;
        private static Type _loadingType;
        private static FieldInfo _capacityField;
        private static string _lastScene = "";
        private static float _nextTick;
        private static bool _reported;

        public static void Init(Harmony harmony, Action<string> log)
        {
            _log = log;
            _containerType = AccessTools.TypeByName("IngredientContainer");
            _loadingType = AccessTools.TypeByName("LoadingScreenUIController");
            if (_containerType != null)
            {
                _capacityField = _containerType.GetField("m_capacity",
                    BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            }
            Log("[Capacity] container=" + (_containerType != null)
                + " capacityField=" + (_capacityField != null));
        }

        public static void Shutdown()
        {
            _log = null;
        }

        public static void Tick()
        {
            string scene = CurrentScene();
            bool sceneChanged = scene != _lastScene;
            if (!sceneChanged && Time.realtimeSinceStartup < _nextTick) return;
            _nextTick = Time.realtimeSinceStartup + 2f;
            _lastScene = scene;

            if (!InTunedLevel(scene)) return;
            if (_containerType == null || _capacityField == null) return;

            // Never poke scene objects while the level is still being loaded: the container
            // components appear one by one and touching them mid-construction can wedge the
            // loading flow.
            if (IsLoading()) return;

            try
            {
                UnityEngine.Object[] all = UnityEngine.Object.FindObjectsOfType(_containerType);
                if (all == null || all.Length == 0) return;
                int clamped = 0;
                for (int i = 0; i < all.Length; i++)
                {
                    Component comp = all[i] as Component;
                    if (comp == null || !IsCookware(comp)) continue;
                    try
                    {
                        int capacity = (int)_capacityField.GetValue(comp);
                        if (capacity != TunedCapacity)
                        {
                            _capacityField.SetValue(comp, TunedCapacity);
                        }
                        clamped++;
                    }
                    catch { }
                }
                if (!_reported || sceneChanged)
                {
                    _reported = true;
                    Log("[Capacity] wellcooked_demo_3: clamped " + clamped + " cooking containers to "
                        + TunedCapacity + " ingredient(s)");
                }
            }
            catch (Exception e)
            {
                Log("[Capacity] apply failed: " + e.Message);
            }
        }

        private static bool IsCookware(Component container)
        {
            try
            {
                Component[] parents = container.GetComponentsInParent<Component>(true);
                for (int i = 0; i < parents.Length; i++)
                {
                    if (parents[i] == null) continue;
                    string name = parents[i].GetType().Name;
                    if (name.IndexOf("Cooking", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("Utensil", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("FryingPan", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("Cooker", StringComparison.OrdinalIgnoreCase) >= 0)
                    {
                        return true;
                    }
                }
            }
            catch { }
            return false;
        }

        private static bool IsLoading()
        {
            try
            {
                if (_loadingType == null) return false;
                return Resources.FindObjectsOfTypeAll(_loadingType).Length > 0;
            }
            catch
            {
                return false;
            }
        }

        private static bool InTunedLevel(string scene)
        {
            return !string.IsNullOrEmpty(scene)
                && scene.IndexOf("wellcooked_demo_3", StringComparison.OrdinalIgnoreCase) >= 0;
        }

        private static string CurrentScene()
        {
            try
            {
                UnityEngine.SceneManagement.Scene scene =
                    UnityEngine.SceneManagement.SceneManager.GetActiveScene();
                return scene.IsValid() ? scene.name : "";
            }
            catch
            {
                return "";
            }
        }

        private static void Log(string message)
        {
            if (_log != null) _log(message);
        }
    }
}
