using System;
using System.IO;
using System.Reflection;
using BepInEx;
using HarmonyLib;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// Loads a scene without going through the arcade lobby (custom level debugging).
    ///
    /// Two entry points:
    ///   1. The F9 overlay menu (LevelMenu) calls LoadScene() with the highlighted name.
    ///   2. File channel: one scene name in BepInEx\wellcooked-load.txt, applied within
    ///      one second, even while the game is in the background.
    ///
    /// Why ServerMessenger.LoadLevel:
    ///   The pause menu's restart uses it with (scene, LoadKitchen, true, RunKitchen); that
    ///   state machine brings up the kitchen flow (orders, timer, chef control). Calling
    ///   SceneManager.LoadScene would only swap the scene. OC2DIYLevel also patches this
    ///   method, which is what lets custom scene names resolve to its own AssetBundle.
    /// </summary>
    public static class HotkeyLoad
    {
        private static Action<string> _log;
        private static float _nextPoll;

        public static void Init(Action<string> log)
        {
            _log = log;
            _log("[Hotkey] file channel ready: BepInEx\\wellcooked-load.txt (F9 opens the level menu)");
        }

        public static void Tick()
        {
            try
            {
                float now = Time.realtimeSinceStartup;
                if (now < _nextPoll)
                {
                    return;
                }

                _nextPoll = now + 1f;

                string path = Path.Combine(Paths.BepInExRootPath, "wellcooked-load.txt");
                if (!File.Exists(path))
                {
                    return;
                }

                string text;
                try
                {
                    text = File.ReadAllText(path).Trim();
                }
                catch (Exception e)
                {
                    Log("[Hotkey] cannot read " + path + ": " + e.Message);
                    return;
                }

                try
                {
                    File.Delete(path);
                }
                catch
                {
                    // Failure to delete does not matter for this load.
                }

                if (text.Length > 0)
                {
                    LoadScene(text, "file wellcooked-load.txt");
                }
            }
            catch (Exception e)
            {
                Log("[Hotkey] Tick failed: " + e.Message);
            }
        }

        /// <summary>Load one scene through the game's own kitchen flow. Safe to call from Update.</summary>
        public static void LoadScene(string scene, string source)
        {
            if (string.IsNullOrEmpty(scene))
            {
                return;
            }

            Type messenger = AccessTools.TypeByName("ServerMessenger");
            Type stateType = AccessTools.TypeByName("GameState");
            if (messenger == null || stateType == null)
            {
                Log("[Hotkey] ServerMessenger / GameState not found");
                return;
            }

            MethodInfo method = AccessTools.Method(messenger, "LoadLevel",
                new[] { typeof(string), stateType, typeof(bool), stateType });
            if (method == null)
            {
                Log("[Hotkey] ServerMessenger.LoadLevel(string, GameState, bool, GameState) not found");
                return;
            }

            try
            {
                StopSynchronisation();

                object begin = Enum.Parse(stateType, "LoadKitchen");
                object run = Enum.Parse(stateType, "RunKitchen");

                Log("[Hotkey] (" + source + ") loading " + scene);
                method.Invoke(null, new[] { scene, begin, (object)true, run });
            }
            catch (Exception e)
            {
                Log("[Hotkey] load failed: " + e);
            }
        }

        /// <summary>The pause menu stops synchronisation before restarting; without it the old level follows.</summary>
        private static void StopSynchronisation()
        {
            try
            {
                Type type = AccessTools.TypeByName("MultiplayerController");
                Type utils = AccessTools.TypeByName("GameUtils");
                if (type == null || utils == null)
                {
                    return;
                }

                MethodInfo require = AccessTools.Method(utils, "RequireManager");
                object controller = null;
                if (require != null && require.IsGenericMethodDefinition)
                {
                    controller = require.MakeGenericMethod(type).Invoke(null, null);
                }

                if (controller == null)
                {
                    return;
                }

                MethodInfo stop = AccessTools.Method(type, "StopSynchronisation", Type.EmptyTypes);
                stop?.Invoke(controller, null);
            }
            catch
            {
                // Failure to stop synchronisation must not block the load.
            }
        }

        private static void Log(string message)
        {
            if (_log != null)
            {
                _log(message);
            }
        }
    }
}
