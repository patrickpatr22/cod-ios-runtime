// Call of Duty iPad - user settings (Daten/einstellungen.txt) and the
// settings screen: tabs (Allgemein, Grafik, Spiel, Fehlersuche) with cards in
// two columns, drawn like the start screen. Touch and controller (LB/RB
// tabs, up/down choose, left/right change, A switch, B close).

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rex/ui/imgui_dialog.h>

namespace bo2 {

inline constexpr int kFovConsole = 65;
inline constexpr int kFovMax = 110;

struct UserSettings {
  // Internal resolution (next start): 1 as on the Xbox, 2 renders everything
  // at twice the width and height (test).
  int resolution_scale = 1;
  bool overlay = true;
  // Field of view in degrees as the game counts cg_fov (65 = console).
  // Black Ops II only.
  int fov = kFovConsole;
  // Troubleshooting, from the next start: exact (NoContraction) shader math
  // as before build 13, and reusing textures whose bytes didn't change.
  bool exact_shaders = false;
  // Off by default since build 16 (suspected of the colored texture noise).
  bool texture_hash_reuse = false;
  // Frame pacing, applied at once: 0 automatic (120 Hz while the frames keep
  // up, otherwise an even 60), 60 (every frame shown for two refreshes of
  // the 120 Hz display) or 120.
  int frame_rate = 0;
  // Gamertag shown in the game (next start), up to 15 characters.
  std::string player_name = "User";
  // Troubleshooting: guest "interrupts off" sections take the global lock
  // again (as before build 18, much slower).
  bool guest_global_lock = false;
  // Performance test (next start): every frame is recorded to
  // Daten/Leistung, and in a match a few settings are compared automatically.
  bool performance_test = true;
  // Black Ops II: every weapon camo and reticle selectable (applies at once).
  bool unlock_all_camos = true;
  // Renderer (applies at once): memory uploads that nothing drawn earlier
  // depends on happen before the frame's first render pass instead of
  // interrupting one. Off = as before build 30.
  bool hoist_uploads = true;
  // Black Ops (applies at once): the two job threads sleep while only
  // waiting jobs are queued instead of spinning. Off = as before build 30.
  bool relax_job_threads = true;
};

UserSettings LoadUserSettings(const std::filesystem::path& data_dir);
void SaveUserSettings(const std::filesystem::path& data_dir, const UserSettings& settings);
// Sets the SDK frame pacing (cvar frame_pacing) for UserSettings::frame_rate.
void ApplyFramePacing(int frame_rate);
// The picture: the game's 16:9 frame as on the Xbox (bars above and below
// on the iPad), upscaled to the display with FSR 1, no FXAA. Fixed since
// build 23 (the picture shape and quality options are gone).
void ApplyStandardPicture();
// Sets the renderer switches of the troubleshooting tab that apply at once.
void ApplyRendererSwitches(const UserSettings& settings);

// What the settings screen shows besides the settings themselves.
struct SettingsContext {
  std::filesystem::path data_dir;
  std::string build_label;
  // Empty on the start screen; in a game its name and the running mode.
  std::string game_id;
  std::string game_name;
  std::string mode_name;
  uint32_t accent = 0xFF1F8AFF;  // ImU32
  // Other modes of the running game it can switch to: (mode id, label).
  std::vector<std::pair<std::string, std::string>> switch_targets;
  std::function<void(const std::string& mode)> switch_mode;
  std::function<void()> to_launcher;
  // Called when a setting that applies at once changed (field of view,
  // frame pacing, camos).
  std::function<void(const UserSettings&)> apply_live;
};

// The settings screen. In the game it opens from a small button in the top
// right corner (shown for a few seconds after touching the screen) or with
// both sticks pressed; on the start screen the launcher opens it.
class SettingsPanel : public rex::ui::ImGuiDialog {
 public:
  using rex::ui::ImGuiDialog::ImGuiDialog;
  virtual void Open() = 0;
  virtual void Close() = 0;
  // Safe to call from any thread (the input system asks while the game runs).
  virtual bool IsOpen() const = 0;
  virtual const UserSettings& Current() const = 0;
};

std::unique_ptr<SettingsPanel> CreateSettingsDialog(rex::ui::ImGuiDrawer* drawer,
                                                    SettingsContext context,
                                                    UserSettings settings);

}  // namespace bo2
