using System;
using System.Reflection;
using HarmonyLib;
using UnityEngine;

namespace WellCooked.Game
{
    /// <summary>
    /// Level 3 only: every cooking vessel accepts one ingredient at a time.
    ///
    /// The scene carries the capacity on PseudoPrefabCookingUtensilStub, but the runtime
    /// pseudo-prefab swap drops that value, so a pot still behaves like the stock prefab
    /// (IngredientContainer.m_capacity = 3..4). Clamping the two CanAddIngredient
    /// implementations is the only reliable lever:
    ///   ClientIngredientContainer.CanAddIngredient -> client prediction / UI highlight
    ///   ServerIngredientContainer.CanAddIngredient -> authority (needs both, otherwise the
    ///   second ingredient is predicted locally and then rolled back by the host).
    ///
    /// Only cookware is clamped: the check requires a CookingHandler / CookingStation on
    /// the container or an ancestor, so ingredient trays (料箱, capacity 3) keep working.
    /// </summary>
    public static class IngredientCapacityTuner
    {
        private static Action<string> _log;
        private static Type _cookingHandlerType;
        private static Type _cookingStationType;
        private static bool _patched;
        private static int _blockedLogs;

        public static void Init(Harmony harmony, Action<string> log)
        {
            _log = log;
            _cookingHandlerType = AccessTools.TypeByName("CookingHandler");
            _cookingStationType = AccessTools.TypeByName("CookingStation");
            Patch(harmony);
            Log("[Capacity] init cookingHandler=" + (_cookingHandlerType != null)
                + " cookingStation=" + (_cookingStationType != null));
        }

        public static void Shutdown()
        {
            _log = null;
        }

        private static void Log(string message)
        {
            if (_log != null) _log(message);
        }

        private static bool IsLevel3()
        {
            try
            {
                string scene = UnityEngine.SceneManagement.SceneManager.GetActiveScene().name;
                return !string.IsNullOrEmpty(scene) && scene.Contains("wellcooked_demo_3");
            }
            catch
            {
                return false;
            }
        }

        private static void Patch(Harmony harmony)
        {
            if (_patched) return;
            _patched = true;
            MethodInfo postfix = typeof(IngredientCapacityTuner).GetMethod(
                "CanAddPostfix", BindingFlags.Static | BindingFlags.NonPublic);
            if (postfix == null) return;
            PatchOne(harmony, "ClientIngredientContainer", postfix);
            PatchOne(harmony, "ServerIngredientContainer", postfix);
        }

        private static void PatchOne(Harmony harmony, string typeName, MethodInfo postfix)
        {
            Type type = AccessTools.TypeByName(typeName);
            if (type == null)
            {
                Log("[Capacity] type missing: " + typeName);
                return;
            }
            MethodInfo target = AccessTools.Method(type, "CanAddIngredient");
            if (target == null)
            {
                Log("[Capacity] method missing: " + typeName + ".CanAddIngredient");
                return;
            }
            try
            {
                harmony.Patch(target, postfix: new HarmonyMethod(postfix));
                Log("[Capacity] patched " + typeName + ".CanAddIngredient");
            }
            catch (Exception e)
            {
                Log("[Capacity] patch failed " + typeName + ": " + e.Message);
            }
        }

        private static bool IsCookware(Component container)
        {
            // Only the container that actually sits on a cooking vessel counts. Deliberately no
            // ancestor scan: ingredient trays (料箱) and dispensers live on counters, and a wide
            // ancestor check risked vetoing them.
            GameObject go = container.gameObject;
            if (_cookingHandlerType != null && go.GetComponent(_cookingHandlerType) != null) return true;
            if (_cookingStationType != null && go.GetComponent(_cookingStationType) != null) return true;
            return false;
        }

        private static bool HasContents(object instance)
        {
            try
            {
                MethodInfo has = AccessTools.Method(instance.GetType(), "HasContents");
                if (has == null) return false;
                object value = has.Invoke(instance, null);
                return value is bool && (bool)value;
            }
            catch
            {
                return false;
            }
        }

        // Harmony postfix: CanAddIngredient already returned true -> veto it when the
        // cookware is not empty on level 3.
        private static void CanAddPostfix(object __instance, ref bool __result)
        {
            if (!__result) return;
            if (!IsLevel3()) return;
            Component container = __instance as Component;
            if (container == null || !IsCookware(container)) return;
            if (!HasContents(__instance)) return;
            __result = false;
            if (_blockedLogs < 20)
            {
                _blockedLogs++;
                Log("[Capacity] level3 second ingredient rejected on " + container.gameObject.name);
            }
        }
    }
}
