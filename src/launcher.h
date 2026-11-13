// Call of Duty iPad - start screen (launcher, in the style of the Plutonium
// launcher): the games of the app on the left (games.h), the modes of the
// selected game as large tiles on the right. Touch and controller (D-pad /
// left stick to move, A to start, Y for the settings, B back).

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

struct ImFont;
struct ImFontAtlas;
struct ImGuiIO;

namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
}  // namespace rex::ui

namespace cod {

// Fonts of the launcher and the settings panel. Null until AddLauncherFonts
// ran (or if a font is missing): then the ImGui default font is used.
struct LauncherFonts {
  ImFont* title = nullptr;    // game title
  ImFont* heading = nullptr;  // tile titles
  ImFont* label = nullptr;    // game list
  ImFont* button = nullptr;   // buttons, emphasised text
  ImFont* body = nullptr;     // text
  ImFont* small = nullptr;    // hints, status
};
const LauncherFonts& Fonts();

// Adds the fonts to the ImGui atlas, rasterised at the display's pixel
// density (2 on the iPad) so text stays sharp.
void AddLauncherFonts(ImFontAtlas* atlas, float pixel_density);

struct ModeDef;

struct LaunchRequest {
  std::string game;  // "bo2", "bo1"
  std::string mode;  // "mp", "zombies", "sp"
};

struct LauncherOptions {
  std::filesystem::path documents;  // the app's Documents folder (game folders)
  std::string build_label;          // shown in the corner
  std::string last_game;            // preselected (last played)
  std::string last_mode;
  // Current player name (it can change in the settings while the launcher
  // shows).
  std::function<std::string()> player_name;
  // Settings panel: open it, ask whether it is open, close it.
  std::function<void()> open_settings;
  std::function<bool()> settings_open;
  std::function<void()> close_settings;
  // Called once when a mode was chosen; the launcher then shows "starting"
  // until it is removed.
  std::function<void(const LaunchRequest&)> start;
  // Whether the app has the code of a mode (default: the module file exists;
  // previews on a PC replace it).
  std::function<bool(const ModeDef&)> module_available;
};

std::unique_ptr<rex::ui::ImGuiDialog> CreateLauncher(rex::ui::ImGuiDrawer* drawer,
                                                     LauncherOptions options);

// One frame of the start screen into the current ImGui frame, without the app
// around it (previews on a PC).
class LauncherView;
class LauncherPreview {
 public:
  explicit LauncherPreview(LauncherOptions options);
  ~LauncherPreview();
  void Draw(ImGuiIO& io);

 private:
  std::unique_ptr<LauncherView> view_;
};

}  // namespace cod
