// Call of Duty iPad - application glue: start screen, then the recompiled
// code of the chosen game and mode (game modules, see game_module.h) on the
// ReXGlue runtime. Black Ops II based on officialmelon/bo2-recompiled.

#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rex/rex_app.h>

#include "games.h"
#include "game_module.h"
#include "settings.h"

class CodApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override;
  // The start screen: shown before the runtime exists, the game starts when
  // a mode was chosen (skipped after a mode switch, which boots directly).
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override;
  void OnConfigureFonts(ImFontAtlas* atlas) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnLoadXexImage(std::string& xex_image) override;
  void OnPostLoadXexImage() override;
  void OnPostSetup() override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;
  void OnPreLaunchModule() override;
  void OnShutdown() override;

 private:
  // Game and mode for this start when the start screen is skipped (empty:
  // show it): the cod_mode setting or the one-shot request of a mode switch.
  cod::GameAndMode TakeDirectBoot();
  void ShowLauncher(rex::PathConfig defaults, std::function<void(rex::PathConfig)> resume);
  // Everything that depends on the chosen game and mode, right before the
  // runtime is built: its code, folders, settings, in-game panels. Returns
  // the paths to continue with.
  rex::PathConfig PrepareGame(const cod::GameAndMode& choice, rex::PathConfig paths);
  void ApplyGameSettings();
  void ApplyModuleSettings(const bo2::UserSettings& settings);
  void WatchModuleSwitches();
  void CreateGameDialogs();
  // Closes the app; the next start shows the start screen.
  void ReturnToLauncher();
  // Notes the mode for the next start, tells the player and closes the app.
  void SwitchMode(const std::string& mode, bool from_guest_thread);
  // Black Ops restarts itself with launch data (DLC zombie maps: the sound
  // zone and the map to load). iOS can't restart the app, so the data waits
  // in Daten/startdaten.bin for the next start of the same game and mode.
  void SaveLaunchData(const std::vector<uint8_t>& data, const std::string& mode);
  void RestoreLaunchData();
  // The mode the game asks for when it wants another executable started.
  std::string RequestedMode(const std::string& launch_path, bool zombies_data) const;
  // Files and automatic comparison of the performance test (setting).
  void ConfigurePerformanceTest();
  // Doubled internal resolution (setting, test) with a fallback after a
  // start that didn't get far.
  void ConfigureResolutionScale();
  // Shows a message and closes the app (before the game starts).
  [[noreturn]] void Fail(const std::string& message);

  std::filesystem::path documents_;  // game folders (BO2, BO1, ...)
  std::filesystem::path data_dir_;   // saves, config, logs, mode files
  const cod::GameDef* game_ = nullptr;
  const cod::ModeDef* mode_ = nullptr;
  const cod::GameModule* module_ = nullptr;
  // What the module was told last; the job thread switch also changes
  // through its cvar (automatic comparison), on another thread.
  std::mutex module_settings_mutex_;
  cod::GameModuleSettings module_settings_;
  cod::GameAndMode direct_;  // see TakeDirectBoot
  cod::GameAndMode last_;    // last played, preselected on the start screen
  std::unique_ptr<rex::ui::ImGuiDialog> launcher_;
  std::unique_ptr<bo2::SettingsPanel> launcher_settings_;
  int graphics_profile_ = -1;  // index of the active graphics test profile
  std::unique_ptr<rex::ui::ImGuiDialog> perf_overlay_;
  std::unique_ptr<bo2::SettingsPanel> settings_dialog_;
  bo2::UserSettings user_settings_;
  std::atomic<bool> mode_switch_written_{false};
  std::chrono::steady_clock::time_point setup_time_;
};
