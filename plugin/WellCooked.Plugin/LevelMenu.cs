using System;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// In-game level picker drawn as an OnGUI overlay.
    ///
    /// F9 toggles it, Up/Down moves the highlight, Enter loads through HotkeyLoad.LoadScene
    /// (so the normal kitchen state machine still runs), Esc closes it. While the overlay is
    /// open it consumes those keys in OnGUI so they do not also drive the GUI/character.
    ///
    /// The load itself is deferred to the next Update: invoking the game's load path from
    /// inside OnGUI can run while IMGUI is iterating events.
    /// </summary>
    public static class LevelMenu
    {
        private const KeyCode ToggleKey = KeyCode.F9;

        // Scene names are AssetBundle names under BepInEx\plugins\OC2DIYLevel\levels.
        private static readonly string[] Scenes =
        {
            "s_wellcooked_demo_1",
            "s_wellcooked_demo_2",
            "s_wellcooked_demo_3",
            "s_wellcooked_demo_4",
            "s_wellcooked_show_sushi",
            "s_wellcooked_show_beach",
            "s_wellcooked_show_summer",
            "s_wellcooked_show_city",
            "s_test_level",
        };

        private static Action<string> _log;
        private static bool _open;
        private static bool _drawLogged;
        private static int _selected;
        private static string _pendingLoad;
        private static string _message = "";
        private static float _messageUntil;

        private static GUIStyle _titleStyle;
        private static GUIStyle _itemStyle;
        private static GUIStyle _selectedStyle;
        private static GUIStyle _hintStyle;

        public static bool IsOpen
        {
            get { return _open; }
        }

        public static void Init(Action<string> log)
        {
            _log = log;
        }

        /// <summary>Called from Update: F9 toggles, and a confirmed selection is loaded here.</summary>
        public static void Tick()
        {
            try
            {
                if (Input.GetKeyDown(ToggleKey))
                {
                    if (_open)
                    {
                        Close("F9");
                    }
                    else
                    {
                        Open();
                    }
                }

                if (_pendingLoad != null)
                {
                    string scene = _pendingLoad;
                    _pendingLoad = null;
                    HotkeyLoad.LoadScene(scene, "menu");
                }
            }
            catch (Exception e)
            {
                Log("[Menu] Tick failed: " + e.Message);
            }
        }

        /// <summary>Called from the plugin's OnGUI; draws nothing while the menu is closed.</summary>
        public static void Draw()
        {
            if (!_open)
            {
                return;
            }

            try
            {
                HandleKeys();
                if (!_open)
                {
                    return;
                }

                DrawOverlay();

                if (!_drawLogged)
                {
                    _drawLogged = true;
                    Log("[Menu] overlay drawn (selected=" + Scenes[_selected] + ")");
                }
            }
            catch (Exception e)
            {
                Log("[Menu] Draw failed: " + e.Message);
            }
        }

        private static void Open()
        {
            _message = "";
            _open = true;
            _drawLogged = false;
            Log("[Menu] F9 open; scenes: " + string.Join(", ", Scenes));
        }

        private static void Close(string source)
        {
            _message = "";
            _messageUntil = 0f;
            if (!_open)
            {
                return;
            }

            _open = false;
            Log("[Menu] closed by " + source);
        }

        private static void HandleKeys()
        {
            Event e = Event.current;
            if (e == null || e.type != EventType.KeyDown)
            {
                return;
            }

            switch (e.keyCode)
            {
                case KeyCode.UpArrow:
                    _selected = (_selected + Scenes.Length - 1) % Scenes.Length;
                    e.Use();
                    Log("[Menu] selected " + Scenes[_selected]);
                    break;

                case KeyCode.DownArrow:
                    _selected = (_selected + 1) % Scenes.Length;
                    e.Use();
                    Log("[Menu] selected " + Scenes[_selected]);
                    break;

                case KeyCode.Return:
                case KeyCode.KeypadEnter:
                    e.Use();
                    if (!HotkeyLoad.InLevel())
                    {
                        _message = "主菜单里不能直接进关：先进入任意关卡，再按 F9 选关";
                        _messageUntil = float.MaxValue;
                        Log("[Menu] refused: frontend state");
                        break;
                    }
                    _pendingLoad = Scenes[_selected];
                    Log("[Menu] Enter: queued " + _pendingLoad);
                    Close("Enter");
                    break;

                case KeyCode.Escape:
                    e.Use();
                    Close("Escape");
                    break;
            }
        }

        private static void DrawOverlay()
        {
            EnsureStyles();

            float scale = Mathf.Clamp(Screen.height / 1080f, 0.75f, 2f);
            float panelWidth = 640f * scale;
            float rowHeight = 34f * scale;
            float headerHeight = 78f * scale;
            float footerHeight = 46f * scale;
            float padding = 18f * scale;
            float panelHeight = headerHeight + (Scenes.Length * rowHeight) + footerHeight + (padding * 2f);

            Rect panel = new Rect(
                (Screen.width - panelWidth) * 0.5f,
                (Screen.height - panelHeight) * 0.5f,
                panelWidth,
                panelHeight);

            Color previous = GUI.color;
            GUI.color = new Color(0f, 0f, 0f, 0.82f);
            GUI.DrawTexture(panel, Texture2D.whiteTexture);
            GUI.color = previous;

            GUI.Label(
                new Rect(panel.x + padding, panel.y + padding * 0.6f, panel.width - padding * 2f, 34f * scale),
                "WellCooked - Level Picker",
                _titleStyle);

            GUI.Label(
                new Rect(panel.x + padding, panel.y + padding * 0.6f + 34f * scale, panel.width - padding * 2f, 22f * scale),
                "Custom levels from BepInEx\\plugins\\OC2DIYLevel\\levels",
                _hintStyle);

            float rowTop = panel.y + headerHeight;

            for (int i = 0; i < Scenes.Length; i++)
            {
                bool selected = i == _selected;
                Rect row = new Rect(panel.x + padding, rowTop + (i * rowHeight), panel.width - (padding * 2f), rowHeight);

                if (selected)
                {
                    GUI.color = new Color(1f, 0.62f, 0.12f, 0.45f);
                    GUI.DrawTexture(row, Texture2D.whiteTexture);
                    GUI.color = previous;
                }

                GUI.Label(
                    new Rect(row.x + 12f * scale, row.y, row.width - 12f * scale, row.height),
                    (selected ? "> " : "   ") + Scenes[i],
                    selected ? _selectedStyle : _itemStyle);
            }

            GUI.Label(
                new Rect(panel.x + padding, panel.y + panel.height - footerHeight, panel.width - padding * 2f, footerHeight),
                "Up/Down: select    Enter: load    Esc: close    F9: close",
                _hintStyle);

            if (Time.realtimeSinceStartup < _messageUntil && _message.Length > 0)
            {
                GUI.color = new Color(1f, 0.55f, 0.2f, 1f);
                GUI.Label(
                    new Rect(panel.x + padding, panel.y + panel.height - footerHeight - 30f * scale, panel.width - padding * 2f, 28f * scale),
                    _message,
                    _hintStyle);
                GUI.color = previous;
            }
        }

        private static void EnsureStyles()
        {
            float scale = Mathf.Clamp(Screen.height / 1080f, 0.75f, 2f);
            int titleSize = Mathf.RoundToInt(28f * scale);
            int itemSize = Mathf.RoundToInt(22f * scale);
            int hintSize = Mathf.RoundToInt(16f * scale);

            if (_titleStyle == null)
            {
                _titleStyle = new GUIStyle(GUI.skin.label);
                _titleStyle.fontSize = titleSize;
                _titleStyle.fontStyle = FontStyle.Bold;
                _titleStyle.normal.textColor = Color.white;

                _itemStyle = new GUIStyle(GUI.skin.label);
                _itemStyle.fontSize = itemSize;
                _itemStyle.normal.textColor = new Color(0.92f, 0.92f, 0.92f, 1f);

                _selectedStyle = new GUIStyle(_itemStyle);
                _selectedStyle.fontStyle = FontStyle.Bold;
                _selectedStyle.normal.textColor = Color.white;

                _hintStyle = new GUIStyle(GUI.skin.label);
                _hintStyle.fontSize = hintSize;
                _hintStyle.normal.textColor = new Color(0.75f, 0.75f, 0.75f, 1f);
            }
            else
            {
                _titleStyle.fontSize = titleSize;
                _itemStyle.fontSize = itemSize;
                _hintStyle.fontSize = hintSize;
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
