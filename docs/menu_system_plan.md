# Menu System Implementation Plan

## Overview
Add a native Win32 menu bar to the game window using the existing `rex::ui::MenuItem` + `Window::SetMainMenu()` API. Config dialogs use `rex::ui::ImGuiDialog`. User settings persist to a TOML file (`vig8_settings.toml`) in the game directory.

## Architecture

### Key SDK APIs (already available)
- `Window::SetMainMenu(std::unique_ptr<MenuItem>)` — native menu bar
- `MenuItem::Create(Type, text, hotkey, callback)` — menu items (kPopup, kNormal, kSeparator)
- `MenuItem::AddChild()` — nested menus
- `rex::ui::ImGuiDialog` — base class for modal config dialogs (`OnDraw(ImGuiIO&)`)
- `ImGuiDrawer::AddDialog() / RemoveDialog()` — show/hide dialogs
- `ImGuiDialog::ShowMessageBox()` — simple info dialogs

### Settings Persistence
- File: `vig8_settings.toml` in game directory (next to extracted/)
- Load on startup in `Vig8App::OnInitialize()`
- Save on change from any config dialog
- Gitignored (user-specific)
- Format example:
```toml
[gfx]
render_path = "rov"       # "rov" or "rtv"

[game]
full_game = true          # unlock all content

[controls]
input_device = "controller"  # "controller" or "keyboard"
# keyboard bindings (only used when input_device = "keyboard")
key_accelerate = "W"
key_brake = "S"
key_steer_left = "A"
key_steer_right = "D"
key_fire = "Space"
key_special = "E"
key_machinegun = "Q"
key_rear_view = "R"
key_pause = "Escape"

[debug]
show_fps = true
show_console = false
```

### Files to Create/Modify
- `project/src/settings.h` — Settings struct + load/save (TOML)
- `project/src/settings.cpp` — Implementation
- `project/src/menu.h` — Menu bar builder + dialog classes
- `project/src/menu.cpp` — Implementation
- `project/src/main.cpp` — Wire up menu bar in OnInitialize(), pass settings
- `project/CMakeLists.txt` — Add new source files
- `.gitignore` — Add `vig8_settings.toml`

---

## Menu Structure

```
File
├── Load Save State          (opens file picker → load .v8save)
├── Save Save State          (opens file picker → save .v8save)
└── Exit

Config
├── Controls...              (opens ControlsDialog)
├── GFX Options...           (opens GfxDialog)
├── Game...                  (opens GameDialog)
└── Debug...                 (opens DebugDialog)

Help
└── About...                 (opens AboutDialog)
```

---

## Dialog Specifications

### 1. ControlsDialog (Config → Controls)
- **Input Device**: Radio buttons — "Controller" / "Keyboard"
- **Keyboard Bindings** (only visible when Keyboard selected):
  - Each action shows current key + "Rebind" button
  - On click "Rebind": captures next keypress as new binding
  - Actions: Accelerate, Brake, Steer Left, Steer Right, Fire Weapon, Special Weapon, Machine Gun, Rear View, Pause
- **Controller**: shows "Using XInput controller" info text (no config needed, handled by SDK)
- **Buttons**: Apply, Cancel

### 2. GfxDialog (Config → GFX Options)
- **Render Path**: Dropdown — "ROV (Recommended)" / "RTV (Default)"
  - Shows info text: "ROV fixes white screen in 3D rendering. Requires pixel shader interlock support."
- **Future slots**: Resolution scale, VSync, fullscreen toggle (placeholders for now)
- **Buttons**: Apply (requires restart), Cancel

### 3. GameDialog (Config → Game)
- **Full Game**: Checkbox — "Unlock full game"
  - Info text: "Skips trial mode restrictions. Persists across sessions."
- **Buttons**: Apply, Cancel

### 4. DebugDialog (Config → Debug)
- **Show FPS Overlay**: Checkbox (toggles DebugOverlayDialog visibility)
- **Show Debug Console**: Checkbox (toggles AllocConsole / FreeConsole)
- **Buttons**: Apply, Cancel

### 5. AboutDialog (Help → About)
- Title: "Vigilante 8 Arcade — Static Recompilation"
- Body: Version info, GitHub link (https://github.com/sp00nznet/vig8)
- Single "OK" button (or use ShowMessageBox)

---

## Implementation Steps

### Step 1: Settings System (`settings.h/cpp`)
1. Define `Vig8Settings` struct with all fields (gfx, game, controls, debug)
2. `LoadSettings(const std::string& path)` — read TOML, populate struct, use defaults for missing keys
3. `SaveSettings(const std::string& path, const Vig8Settings&)` — write TOML
4. Need a minimal TOML writer (or use the SDK's TOML support if available, otherwise hand-roll for simple flat keys)

### Step 2: Menu Bar (`menu.h/cpp`)
1. `BuildMenuBar(Vig8App*)` — creates MenuItem tree, returns root MenuItem
2. Each menu callback opens the corresponding ImGuiDialog via `imgui_drawer_->AddDialog()`

### Step 3: Config Dialogs (`menu.cpp`)
1. Each dialog class extends `rex::ui::ImGuiDialog`
2. Override `OnDraw(ImGuiIO&)` with ImGui widgets
3. On "Apply": update `Vig8Settings`, call `SaveSettings()`, apply runtime changes
4. On "Cancel": discard changes, remove dialog

### Step 4: Wire Up in main.cpp
1. Add `Vig8Settings settings_` member to Vig8App
2. In `OnInitialize()`:
   - Load settings from `<game_dir>/vig8_settings.toml`
   - Apply settings (set cvars, toggle debug overlay, etc.)
   - Build menu bar via `BuildMenuBar()`
   - Call `window_->SetMainMenu(menu)`
3. Apply `render_target_path_d3d12` cvar from settings before runtime init

### Step 5: Save States (Future / Stub for now)
- Save state = snapshot of PPC memory + PPCContext + handle table state
- `.v8save` files — binary format
- File picker via Win32 `GetOpenFileName` / `GetSaveFileName`
- Initially stub with "Not yet implemented" message box

### Step 6: Full Game Unlock
- Need to find the trial mode check in the recompiled code
- Likely a flag read from XConfig or a content license check
- Stub the check to return "full game" when settings.full_game = true
- Investigate: XamContentGetLicenseMask, XContentGetLicenseMask, or similar

---

## Integration Notes

- Menu bar is native Win32 (via rex::ui::MenuItem), not ImGui — it lives outside the game render area
- Dialogs are ImGui overlays drawn on top of the game via ImGuiDrawer
- Settings file is per-installation (lives next to the game, not in AppData)
- GFX render path change requires restart (lifecycle: kRequiresRestart)
- Keyboard input for gameplay requires intercepting rex::ui input events and translating to XInput state in the kernel stubs
- The `--render_target_path_d3d12=rov` CLI flag should be auto-set from settings on startup (before runtime init)
