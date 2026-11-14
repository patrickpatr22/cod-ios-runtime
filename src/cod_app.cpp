// Call of Duty iPad - application glue. See cod_app.h.

#include "cod_app.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>

#include <dlfcn.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/system.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xex_module.h>
#include <rex/thread/mutex.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window.h>

#include "bo1_streams.h"
#include "diagnostics.h"
#include "game_loader.h"
#include "launcher.h"
#include "settings.h"

REXCVAR_DEFINE_STRING(cod_mode, "", "COD",
                      "Start directly: '<game>:<mode>' (e.g. 'bo2:zombies', 'bo1:mp') or a "
                      "Black Ops II mode ('mp', 'zombies'). Empty = start screen");
REXCVAR_DEFINE_BOOL(cod_overlay, true, "COD",
                    "Show the performance overlay (frame rate, GPU and CPU load)");
REXCVAR_DEFINE_BOOL(cod_relax_job_threads, true, "COD",
                    "Black Ops: the job worker threads sleep while only waiting jobs are queued "
                    "instead of spinning (applies at once)");

namespace {

namespace fs = std::filesystem;

// One-shot request written when the game (or the settings) switch modes: the
// next start boots straight into that mode instead of showing the start
// screen. modus.txt keeps the last played game and mode for the start screen.
constexpr const char* kNextModeFile = "naechster_start.txt";
constexpr const char* kDefaultModeFile = "modus.txt";
constexpr const char* kLaunchDataFile = "startdaten.bin";

// Settings for the iPad live in Daten/callofduty.toml. While the graphics setup is
// being tuned, every start rotates through a few profiles so they can be
// compared without new builds; the log records which one was active.
// Removing the first line of callofduty.toml hands the file over to the user.
constexpr const char* kManagedMarker = "# cod-ios-defaults";
// The file of builds before 26 (Black Ops II only).
constexpr const char* kLegacyManagedMarker = "# bo2-ios-defaults";
// Counts app starts for the rotation; renamed per test round so every round
// starts with profile A.
constexpr const char* kTestCounterFile = "grafiktest-runde12.txt";
constexpr const char* kBuildLabel = "Build 38";

struct GraphicsProfile {
  const char* letter;
  const char* name;
  const char* settings;     // lines for callofduty.toml
  bool async_metal_submit;  // MoltenVK encodes Metal commands on its own thread
};

// Round 10: handle lookups no longer take the global lock, so guest threads
// polling events don't stall the GPU command processor; texture reuse by
// checksum is off by default (suspected of the colored texture noise); two
// pipeline threads normally, all of them while a burst is queued (new map or
// mode). No in-game comparison.
#define BO2_BASE_SETTINGS                                                                        \
  "render_target_path_vulkan = \"fbo\"\n"                                                        \
  "memexport_skip_unknown_ranges = true\n"                                                       \
  "readback_memexport = false\n"                                                                 \
  "clear_memory_page_state = false\n"                                                            \
  "vulkan_render_area_tracking = true\n"                                                         \
  "shared_memory_invalidation_widening = false\n"                                                \
  "vulkan_submit_on_primary_buffer_end = false\n"                                                \
  "spirv_no_contraction = false\n"                                                               \
  "vulkan_persistent_pipeline_cache = false\n"                                                   \
  "execute_unclipped_draw_vs_on_cpu = true\n"                                                    \
  "vulkan_pipeline_creation_threads_ingame = 2\n"
constexpr GraphicsProfile kGraphicsProfiles[] = {
    {"A", "Standard", BO2_BASE_SETTINGS, true},
};
#undef BO2_BASE_SETTINGS
constexpr int kGraphicsProfileCount =
    static_cast<int>(sizeof(kGraphicsProfiles) / sizeof(kGraphicsProfiles[0]));

// Writes callofduty.toml for this start and returns the profile in use, or -1 when
// the user owns the file.
int WriteManagedConfig(const fs::path& config_path, const fs::path& data_dir) {
  {
    std::ifstream in(config_path);
    if (in) {
      std::string first_line;
      std::getline(in, first_line);
      if (first_line.rfind(kManagedMarker, 0) != 0) {
        return -1;
      }
    }
  }

  int run = 0;
  {
    std::ifstream in(data_dir / kTestCounterFile);
    if (in) {
      in >> run;
    }
  }
  if (run < 0) {
    run = 0;
  }
  const int profile = run % kGraphicsProfileCount;
  {
    std::ofstream counter(data_dir / kTestCounterFile, std::ios::trunc);
    counter << (run + 1) << "\n";
  }

  std::ofstream out(config_path, std::ios::trunc);
  out << kManagedMarker << " (Grafiktest, wird bei jedem Start neu geschrieben)\n"
      << "# Zeile 1 loeschen, um die Datei selbst zu verwalten.\n"
      << "\n"
      << "log_level = \"warn\"\n"
      << "vulkan_log_debug_messages = false\n"
      << "occlusion_query_enable = false\n"
      << "# Fehlende Shader kurz weglassen statt das Bild anzuhalten.\n"
      << "vulkan_async_skip_incomplete_frames = false\n"
      << "# Messwerte oben links (false = aus).\n"
      << "cod_overlay = true\n"
      << "\n"
      << "# Profil " << kGraphicsProfiles[profile].letter << " - " << kGraphicsProfiles[profile].name
      << "\n"
      << kGraphicsProfiles[profile].settings;
  return profile;
}

std::string ReadFileText(const fs::path& path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// "KAMPAGNE & ZOMBIES" -> "Kampagne & Zombies".
std::string ModeLabel(const cod::ModeDef& mode) {
  std::string text(mode.title);
  bool word_start = true;
  for (char& c : text) {
    if (c == ' ' || c == '-') {
      word_start = true;
      continue;
    }
    if (!word_start && c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    word_start = false;
  }
  return text;
}

// Media ID of an executable (XEX2 execution info), 0 if unreadable. Title
// updates are made per media ID.
uint32_t ReadXexMediaId(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::string header(0x10000, '\0');
  in.read(header.data(), static_cast<std::streamsize>(header.size()));
  header.resize(static_cast<size_t>(in.gcount()));
  auto be32 = [&](size_t offset) -> uint32_t {
    if (offset + 4 > header.size()) {
      return 0;
    }
    const auto* p = reinterpret_cast<const unsigned char*>(header.data() + offset);
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  };
  if (header.size() < 0x18 || header.compare(0, 4, "XEX2") != 0) {
    return 0;
  }
  const uint32_t count = be32(0x14);
  for (uint32_t i = 0; i < count && i < 64; ++i) {
    if (be32(0x18 + i * 8) == 0x00040006) {
      return be32(be32(0x18 + i * 8 + 4));
    }
  }
  return 0;
}

#if REX_PLATFORM_IOS
fs::path HomeDir() {
  const char* home = std::getenv("HOME");
  return home ? fs::path(home) : fs::temp_directory_path();
}
#endif

}  // namespace

std::unique_ptr<rex::ui::WindowedApp> CodApp::Create(rex::ui::WindowedAppContext& ctx) {
  // The recompiled image comes from the game module chosen on the start
  // screen (SetPPCImageInfo in PrepareGame).
  return std::unique_ptr<CodApp>(new CodApp(ctx, "callofduty", rex::PPCImageInfo{}));
}

void CodApp::OnConfigurePaths(rex::PathConfig& paths) {
#if REX_PLATFORM_IOS
  // Everything the user touches lives in Documents so it is visible in the
  // Files app ("Auf meinem iPad" > app folder):
  //   BO2/, BO1/, ...  the extracted game files (contents of the disc), with
  //                    DLC/ for title updates and DLC packages
  //   Daten/           saves, settings, logs and mode files
  documents_ = HomeDir() / "Documents";
  data_dir_ = documents_ / "Daten";

  std::error_code ec;
  for (const cod::GameDef& game : cod::Games()) {
    fs::create_directories(documents_ / game.folder / "DLC", ec);
  }
  fs::create_directories(data_dir_, ec);
  bo2::InstallCrashReporter(data_dir_, kBuildLabel);

  // Game folder and cache are set for the chosen game (PrepareGame).
  paths.game_data_root = documents_ / "BO2";
  paths.user_data_root = data_dir_;
  paths.cache_root = HomeDir() / "Library" / "Caches" / "bo2";
  paths.config_path = data_dir_ / "callofduty.toml";
  {
    // bo2.toml of builds before 26: taken over when the user edited it,
    // otherwise removed.
    const fs::path legacy = data_dir_ / "bo2.toml";
    std::string first_line;
    if (std::ifstream in(legacy); in && std::getline(in, first_line)) {
      in.close();
      if (first_line.rfind(kLegacyManagedMarker, 0) == 0) {
        fs::remove(legacy, ec);
      } else if (!fs::exists(paths.config_path, ec)) {
        fs::rename(legacy, paths.config_path, ec);
      }
    }
  }
  user_settings_ = bo2::LoadUserSettings(data_dir_);
  // Rewrite the file every start so it always lists every option.
  bo2::SaveUserSettings(data_dir_, user_settings_);
  // Settings the kernel and the game read while starting up.
  rex::cvar::SetFlagByName("user_profile_name", user_settings_.player_name);
  // The game swaps its frames on the guest vblank. It sees a 120 Hz display,
  // and the SDK frame pacing puts the vblanks on the iPad's own vsync, at
  // 120 Hz or (automatically, or as chosen) every other refresh for an even
  // 60. The game's own frame limit stays above that (module settings).
  rex::cvar::SetFlagByName("video_mode_refresh_rate", "120");
  bo2::ApplyFramePacing(user_settings_.frame_rate);
  graphics_profile_ = WriteManagedConfig(paths.config_path, data_dir_);

  // MoltenVK settings: set through its API when Vulkan starts (the
  // environment alone may be read too early), and logged.
  if (graphics_profile_ >= 0) {
    const bool async_submit = kGraphicsProfiles[graphics_profile_].async_metal_submit;
    ::setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", async_submit ? "0" : "1", 1);
    rex::cvar::SetFlagByName("mvk_synchronous_queue_submits", async_submit ? "false" : "true");
  }
  rex::cvar::SetFlagByName("mvk_fast_math", "0");
  rex::cvar::SetFlagByName("mvk_log_level", "1");
  rex::cvar::SetFlagByName("mvk_maximize_concurrent_compilation", "false");
  rex::cvar::SetFlagByName("mvk_performance_tracking",
                           user_settings_.performance_test ? "true" : "false");
  // The Xenos plugin captures one in-game frame (all draws and resolves)
  // into Daten/bilddaten_<profile>_1.txt.
  ::setenv("BO2_FRAME_DUMP",
           (data_dir_ / fmt::format("bilddaten_{}", graphics_profile_ >= 0
                                                         ? kGraphicsProfiles[graphics_profile_].letter
                                                         : "eigen"))
               .string()
               .c_str(),
           1);
#else
  data_dir_ = paths.user_data_root;
  documents_ = paths.game_data_root.parent_path();
#endif
  // Title updates are read from the game folder, as in bo2-recompiled.
  paths.update_data_root = paths.game_data_root;

  direct_ = TakeDirectBoot();
  last_ = data_dir_.empty() ? cod::GameAndMode{}
                            : cod::ParseGameAndMode(ReadFileText(data_dir_ / kDefaultModeFile));
}

cod::GameAndMode CodApp::TakeDirectBoot() {
  const std::string requested = REXCVAR_GET(cod_mode);
  if (!requested.empty() || data_dir_.empty()) {
    return cod::ParseGameAndMode(requested);
  }
  const fs::path next_file = data_dir_ / kNextModeFile;
  const cod::GameAndMode choice = cod::ParseGameAndMode(ReadFileText(next_file));
  std::error_code ec;
  fs::remove(next_file, ec);
  return choice;
}

void CodApp::OnConfigureFonts(ImFontAtlas* atlas) {
  // Text on the start screen and in the settings is rasterised at the
  // display's pixel density (2 on the iPad), so it stays sharp.
  float density = 1.0f;
  if (rex::ui::Window* main_window = window()) {
    density = float(main_window->GetDpi()) / float(main_window->GetMediumDpi());
  }
  cod::AddLauncherFonts(atlas, density);
}

std::optional<rex::PathConfig> CodApp::OnFinalizePaths(
    const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) {
  if (!direct_.empty()) {
    const cod::GameDef* game = cod::FindGame(direct_.game);
    const cod::ModeDef* mode = game ? cod::FindMode(*game, direct_.mode) : nullptr;
    if (mode && cod::ModuleAvailable(*mode) && cod::GameFilesPresent(documents_, *game, *mode)) {
      REXLOG_WARN("Direktstart in {} / {} (Moduswechsel)", direct_.game, direct_.mode);
      return PrepareGame(direct_, defaults);
    }
  }
  ShowLauncher(defaults, std::move(resume));
  return std::nullopt;
}

void CodApp::ShowLauncher(rex::PathConfig defaults, std::function<void(rex::PathConfig)> resume) {
  rex::ui::ImGuiDrawer* drawer = imgui_drawer();
  bo2::SettingsContext context;
  context.data_dir = data_dir_;
  context.build_label = kBuildLabel;
  context.apply_live = [](const bo2::UserSettings& settings) {
    bo2::ApplyFramePacing(settings.frame_rate);
  };
  launcher_settings_ = bo2::CreateSettingsDialog(drawer, std::move(context), user_settings_);
  cod::LauncherOptions options;
  options.documents = documents_;
  options.build_label = kBuildLabel;
  options.last_game = last_.empty() ? std::string("bo2") : last_.game;
  options.last_mode = last_.empty() ? std::string("mp") : last_.mode;
  options.player_name = [this]() {
    return launcher_settings_ ? launcher_settings_->Current().player_name
                              : user_settings_.player_name;
  };
  options.open_settings = [this]() {
    if (launcher_settings_) {
      launcher_settings_->Open();
    }
  };
  options.settings_open = [this]() { return launcher_settings_ && launcher_settings_->IsOpen(); };
  options.close_settings = [this]() {
    if (launcher_settings_) {
      launcher_settings_->Close();
    }
  };
  options.start = [this, defaults = std::move(defaults),
                   resume = std::move(resume)](const cod::LaunchRequest& request) {
    // Not from inside the launcher's own drawing: it is removed here.
    app_context().CallInUIThreadDeferred([this, defaults, resume, request]() {
      launcher_.reset();
      launcher_settings_.reset();
      resume(PrepareGame(cod::GameAndMode{request.game, request.mode}, defaults));
    });
  };
  launcher_ = cod::CreateLauncher(drawer, std::move(options));
  REXLOG_WARN("Hauptmenue (zuletzt gespielt: {})",
              last_.empty() ? std::string("-") : cod::FormatGameAndMode(last_));
}

rex::PathConfig CodApp::PrepareGame(const cod::GameAndMode& choice, rex::PathConfig paths) {
  game_ = cod::FindGame(choice.game);
  mode_ = game_ ? cod::FindMode(*game_, choice.mode) : nullptr;
  if (!mode_ || !mode_->module) {
    Fail("Dieser Modus ist in der App nicht enthalten.");
  }
  std::string error;
  module_ = cod::LoadGameModule(mode_->module, &error);
  if (!module_) {
    Fail(std::string("Der Spielcode von ") + game_->name + " konnte nicht geladen werden.\n\n" +
         error);
  }
  SetPPCImageInfo(*module_->image);
  REXLOG_WARN("Spiel {} / {} (Spielcode {})", game_->id, mode_->id, module_->id);

  if (!data_dir_.empty()) {
    std::ofstream(data_dir_ / kDefaultModeFile, std::ios::trunc)
        << cod::FormatGameAndMode(choice) << "\n";
  }
#if REX_PLATFORM_IOS
  paths.game_data_root = documents_ / game_->folder;
  // Shader caches per game (Black Ops II keeps its folder from before).
  paths.cache_root = HomeDir() / "Library" / "Caches" / game_->id;
  std::error_code ec;
  fs::create_directories(paths.cache_root, ec);
#endif
  paths.update_data_root = paths.game_data_root;
  ApplyGameSettings();
  CreateGameDialogs();
  return paths;
}

void CodApp::ApplyGameSettings() {
  // The start screen may have changed them.
  if (!data_dir_.empty()) {
    user_settings_ = bo2::LoadUserSettings(data_dir_);
  }
  rex::cvar::SetFlagByName("user_profile_name", user_settings_.player_name);
  bo2::ApplyFramePacing(user_settings_.frame_rate);
  ApplyModuleSettings(user_settings_);
  ConfigureResolutionScale();
}

void CodApp::ApplyModuleSettings(const bo2::UserSettings& settings) {
  if (!module_ || !module_->apply_settings) {
    return;
  }
  std::lock_guard<std::mutex> lock(module_settings_mutex_);
  module_settings_.fov = settings.fov;
  module_settings_.frame_rate_limit = 120;
  module_settings_.unlock_all_camos = settings.unlock_all_camos;
  module_settings_.relax_job_threads = REXCVAR_GET(cod_relax_job_threads);
  module_->apply_settings(module_settings_);
}

void CodApp::WatchModuleSwitches() {
  // Runs under the cvar registry lock, on the thread that set the value: no
  // cvar calls in here.
  rex::cvar::RegisterChangeCallback(
      "cod_relax_job_threads", [this](std::string_view, std::string_view value) {
        if (!module_ || !module_->apply_settings) {
          return;
        }
        std::lock_guard<std::mutex> lock(module_settings_mutex_);
        module_settings_.relax_job_threads = value == "true" || value == "1";
        module_->apply_settings(module_settings_);
      });
}

void CodApp::CreateGameDialogs() {
  rex::ui::ImGuiDrawer* drawer = imgui_drawer();
  bo2::SettingsContext context;
  context.data_dir = data_dir_;
  context.build_label = kBuildLabel;
  context.game_id = game_->id;
  context.game_name = game_->name;
  context.mode_name = ModeLabel(*mode_);
  context.accent = game_->accent;
  for (const cod::ModeDef& other : game_->modes) {
    if (&other != mode_ && cod::ModuleAvailable(other) &&
        cod::GameFilesPresent(documents_, *game_, other)) {
      context.switch_targets.emplace_back(other.id, ModeLabel(other));
    }
  }
  context.switch_mode = [this](const std::string& mode) { SwitchMode(mode, false); };
  context.to_launcher = [this]() { ReturnToLauncher(); };
  context.apply_live = [this](const bo2::UserSettings& settings) {
    bo2::ApplyFramePacing(settings.frame_rate);
    bo2::ApplyRendererSwitches(settings);
    ApplyModuleSettings(settings);
  };
  settings_dialog_ = bo2::CreateSettingsDialog(drawer, std::move(context), user_settings_);
  const bool full_overlay = REXCVAR_GET(cod_overlay) && user_settings_.overlay;
  std::string header = graphics_profile_ >= 0
                           ? fmt::format("{} iPad ({}) | {}", game_->name, kBuildLabel,
                                         kGraphicsProfiles[graphics_profile_].name)
                           : fmt::format("{} iPad ({}) | eigene callofduty.toml", game_->name, kBuildLabel);
  perf_overlay_ = bo2::CreatePerfOverlay(drawer, std::move(header), full_overlay);
}

void CodApp::ReturnToLauncher() {
  if (data_dir_.empty() || mode_switch_written_.exchange(true)) {
    return;
  }
  std::error_code ec;
  fs::remove(data_dir_ / kNextModeFile, ec);
  REXLOG_WARN("Zurueck zum Hauptmenue");
  rex::FlushLogging();
  app_context().CallInUIThreadDeferred([]() {
    rex::ShowSimpleMessageBox(
        rex::SimpleMessageBoxType::Help,
        "Das Spiel wird beendet.\n\nÖffne die App einfach wieder – dann erscheint das Hauptmenü.");
    rex::FlushLogging();
    std::_Exit(0);
  });
}

void CodApp::OnPreSetup(rex::RuntimeConfig& config) {
  // The Xenos GPU plugin renders through Vulkan (MoltenVK on Apple).
  if (config.gpu_plugin.empty()) {
    config.gpu_plugin = "xenos";
  }
}

void CodApp::OnLoadXexImage(std::string& xex_image) {
  // Guest path in Xbox form; the runtime maps game:\ to the game folder.
  xex_image = std::string("game:\\") + mode_->xex;

  // DLC and the title update stay the package files the console keeps
  // (<game>/DLC, in any subfolders): the DLC are listed and opened like
  // installed content, the title update is mounted at update:\ and patches
  // the executable as it loads (<name>.xexp).
  auto* content = runtime()->kernel_state()->content_manager();
  content->SetPackageFolder(game_data_root() / "DLC");
  fs::path title_update;
  uint32_t media_id = 0;
  if (game_->title_update > 0) {
    media_id = ReadXexMediaId(game_data_root() / mode_->xex);
    title_update = content->MountTitleUpdate(game_->title_id, media_id);
  }
  bo2::WriteContentList(content->PackageFiles(), title_update, game_->title_id, data_dir_);
  if (game_->title_update > 0 && title_update.empty()) {
    Fail(fmt::format(
        "Title Update {} fehlt.\n\n"
        "Diese Version der App braucht das Title Update {} von {} (eine Datei, die mit "
        "„tu“ beginnt; bei mehreren das zu deiner Disc passende, Media ID {:08X}).\n\n"
        "Kopiere es in der Dateien-App in den Ordner\n"
        "Auf meinem iPad > Call of Duty > {} > DLC\n"
        "(gerne zusammen mit den DLC-Ordnern, Unterordner sind egal) und starte die App neu.",
        game_->title_update, game_->title_update, game_->name, media_id, game_->folder));
  }
}

void CodApp::OnPostLoadXexImage() {
  // The recompiled code only fits the image patched to its title update.
  auto module = runtime()->kernel_state()->GetExecutableModule();
  auto* xex = module ? module->xex_module() : nullptr;
  const auto* exec = xex ? xex->opt_execution_info() : nullptr;
  const auto version = exec ? exec->version() : rex::xex2_version{};
  REXLOG_WARN("{} {} Version {}.{}.{}.{}", game_->name, mode_->xex, uint32_t(version.major),
              uint32_t(version.minor), uint32_t(version.build), uint32_t(version.qfe));
  if (!exec || version.build != game_->title_update) {
    Fail(fmt::format(
        "Das Title Update passt nicht.\n\n"
        "{} hat nach dem Laden die Version {}.{}.{}.{}, gebraucht wird Title Update {} "
        "(0.0.{}.x).\n\n"
        "Lege im Ordner {} > DLC das Title Update {} ab, das zu deiner Disc passt, und entferne "
        "andere Title Updates.",
        mode_->xex, uint32_t(version.major), uint32_t(version.minor), uint32_t(version.build),
        uint32_t(version.qfe), game_->title_update, game_->title_update, game_->folder,
        game_->title_update));
  }
}

void CodApp::Fail(const std::string& message) {
  REXLOG_ERROR("{}", message);
  rex::FlushLogging();
  rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, message);
  std::exit(0);
}

void CodApp::OnPostSetup() {
  bo2::CaptureLastError();
  REXLOG_INFO("Spiel {} / {}, Datenordner {}", game_->id, mode_->id, data_dir_.string());
  if (std::string_view(game_->id) == "bo2") {
    // Which image packs (.ipak, the textures streamed in during play) are
    // there.
    bo2::CheckGameFiles(game_data_root(), data_dir_, mode_->id, kBuildLabel, &app_context());
  }
  if (graphics_profile_ >= 0) {
    REXLOG_WARN("Grafiktest-Profil {} - {}", kGraphicsProfiles[graphics_profile_].letter,
                kGraphicsProfiles[graphics_profile_].name);
  }
  // The game's 16:9 frame as on the Xbox, FSR to the display, no FXAA.
  bo2::ApplyStandardPicture();
  WatchModuleSwitches();
  bo2::ApplyRendererSwitches(user_settings_);
  ApplyModuleSettings(user_settings_);
  REXLOG_WARN("Uploads vorziehen {}, Job-Threads schonen {}",
              user_settings_.hoist_uploads ? "an" : "aus",
              user_settings_.relax_job_threads ? "an" : "aus");
  // Before any guest code runs; must not change afterwards.
  rex::thread::g_guest_global_lock = user_settings_.guest_global_lock;
  rex::cvar::SetFlagByName("spirv_no_contraction",
                           user_settings_.exact_shaders ? "true" : "false");
  rex::cvar::SetFlagByName("texture_cache_revalidate_unchanged",
                           user_settings_.texture_hash_reuse ? "true" : "false");
  if (user_settings_.exact_shaders || user_settings_.texture_hash_reuse ||
      user_settings_.guest_global_lock) {
    REXLOG_WARN("Fehlersuche: genaue Shader {}, Texturen weiterverwenden {}, globale Sperre {}",
                user_settings_.exact_shaders, user_settings_.texture_hash_reuse,
                user_settings_.guest_global_lock);
  }
  REXLOG_WARN("Bild 16:9 mit FSR, Aufloesung {}x, Sichtfeld {}, Bildtakt {}, Name {}, Tarnungen {}",
              user_settings_.resolution_scale, user_settings_.fov,
              user_settings_.frame_rate ? std::to_string(user_settings_.frame_rate) : "auto",
              user_settings_.player_name, user_settings_.unlock_all_camos ? "frei" : "normal");
  ConfigurePerformanceTest();

  // While the settings are open the game gets no controller input (they are
  // operated with it).
  if (auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system())) {
    input->SetActiveCallback([this]() { return !(settings_dialog_ && settings_dialog_->IsOpen()); });
  }

  // Switching modes starts another executable (or the same one with other
  // launch data) on the Xbox. iOS apps cannot restart themselves, so note the
  // request, tell the player and close the app; the next start boots into
  // the requested mode. Black Ops II stores the launch data (which mode)
  // before it shuts itself down for the switch, so react to that already -
  // the shutdown itself is where it crashed in build 14.
  setup_time_ = std::chrono::steady_clock::now();
  if (game_->zombies_flag_address != 0) {
    rex::kernel::xam::XamModule::SetLaunchDataHandler(
        [this](const rex::kernel::xam::XamModule::LoaderData& loader) {
          const bool zombies = loader.launch_data_present && loader.launch_data.size() > 4 &&
                               std::any_of(loader.launch_data.begin() + 4, loader.launch_data.end(),
                                           [](uint8_t b) { return b != 0; });
          const std::string next_mode = zombies ? "zombies" : "mp";
          // Ignore launch data stored while the game is still starting up.
          const bool in_menu =
              std::chrono::steady_clock::now() - setup_time_ > std::chrono::seconds(30);
          if (in_menu && loader.launch_data.size() > 4 && next_mode != mode_->id) {
            SwitchMode(next_mode, true);
          }
        });
  }
  rex::kernel::xam::XamModule::SetLaunchTitleHandler(
      [this](const rex::kernel::xam::XamModule::LoaderData& loader) {
        const bool zombies_data = loader.launch_data_present && loader.launch_data.size() > 4 &&
                                  std::any_of(loader.launch_data.begin() + 4,
                                              loader.launch_data.end(),
                                              [](uint8_t b) { return b != 0; });
        std::string next_mode = RequestedMode(loader.launch_path, zombies_data);
        if (next_mode.empty() && game_->zombies_flag_address == 0 && loader.launch_data_present) {
          // Unknown name, but launch data: the game restarts itself.
          next_mode = mode_->id;
        }
        if (!next_mode.empty()) {
          REXLOG_WARN("Spiel moechte '{}' starten", loader.launch_path);
          if (game_->zombies_flag_address == 0 && loader.launch_data_present) {
            SaveLaunchData(loader.launch_data, next_mode);
          }
          SwitchMode(next_mode, true);
        }
      });
  bo2::StartCpuMonitor();
}

std::string CodApp::RequestedMode(const std::string& launch_path, bool zombies_data) const {
  if (launch_path.empty() || !game_) {
    return {};
  }
  std::string lower = launch_path;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const bool is_mp = lower.find("default_mp.xex") != std::string::npos;
  const bool is_sp = !is_mp && lower.find("default.xex") != std::string::npos;
  if (!is_mp && !is_sp) {
    return {};
  }
  if (is_mp && game_->zombies_flag_address != 0) {
    return zombies_data ? "zombies" : "mp";
  }
  for (const cod::ModeDef& mode : game_->modes) {
    if (std::string_view(mode.xex) == (is_mp ? "default_mp.xex" : "default.xex")) {
      return mode.id;
    }
  }
  return {};
}

// Doubled internal resolution (setting "aufloesung = 2", test): the emulated
// GPU renders every render target at twice the width and height (resolution
// scaling of the Xenos emulation). It needs a 2 GB buffer for the scaled
// copies and much more GPU time, so a start that doesn't get far with it
// (crash, or iOS ending the app for its memory) switches it off again: a
// marker file stays behind until the game has run for two minutes.
void CodApp::ConfigureResolutionScale() {
  const fs::path marker = data_dir_ / "aufloesung_test_laeuft.txt";
  std::error_code ec;
  if (user_settings_.resolution_scale == 2 && fs::exists(marker, ec)) {
    REXLOG_WARN("Letzter Start mit doppelter Aufloesung kam nicht weit - zurueck auf 1x");
    user_settings_.resolution_scale = 1;
    bo2::SaveUserSettings(data_dir_, user_settings_);
    std::ofstream(data_dir_ / "aufloesung_hinweis.txt", std::ios::trunc)
        << "Die doppelte Aufloesung wurde ausgeschaltet: Der letzte Start damit\n"
           "ist abgebrochen (zu wenig Speicher oder Absturz).\n";
  }
  fs::remove(marker, ec);
  const std::string scale = user_settings_.resolution_scale == 2 ? "2" : "1";
  rex::cvar::SetFlagByName("draw_resolution_scale_x", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_y", scale);
  if (user_settings_.resolution_scale != 2) {
    return;
  }
  std::ofstream(marker, std::ios::trunc) << "Start mit doppelter Aufloesung laeuft\n";
  std::thread([marker]() {
    std::this_thread::sleep_for(std::chrono::minutes(2));
    std::error_code remove_ec;
    fs::remove(marker, remove_ec);
    REXLOG_WARN("Doppelte Aufloesung laeuft seit 2 Minuten");
  }).detach();
}

// Performance test (setting "leistungstest", on by default): every frame and
// every presentation is written to Daten/Leistung. The per-frame telemetry
// itself always runs (overlay and log); this only adds the files.
void CodApp::ConfigurePerformanceTest() {
  if (!user_settings_.performance_test || data_dir_.empty()) {
    REXLOG_WARN("Leistungstest aus (kein Vergleich der Schalter)");
    return;
  }
  const fs::path folder = data_dir_ / "Leistung";
  std::error_code ec;
  fs::create_directories(folder, ec);
  char stamp[32] = {};
  const std::time_t now = std::time(nullptr);
  std::tm local_time = {};
  localtime_r(&now, &local_time);
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M", &local_time);
  const std::string prefix = fmt::format("{}_{}_{}", stamp, game_->id, mode_->id);
  rex::cvar::SetFlagByName("perf_csv_frames", (folder / (prefix + "_bilder.csv")).string());
  rex::cvar::SetFlagByName("perf_csv_display", (folder / (prefix + "_anzeige.csv")).string());
  rex::cvar::SetFlagByName("perf_report_path", (folder / (prefix + "_vergleich.txt")).string());
  // Build 30: measures what its two new switches are worth, a minute long
  // from 25 s into the game (three 10 s phases, twice). "Standard" is what
  // the settings say; the other phases turn one switch off.
  std::string comparison = "Standard";
  if (user_settings_.hoist_uploads) {
    comparison += ";Ohne Vorziehen:vulkan_hoist_uploads=false";
  }
  if (user_settings_.relax_job_threads && std::string_view(game_->id) == "bo1") {
    comparison += ";Ohne Job-Pause:cod_relax_job_threads=false";
  }
  if (comparison != "Standard") {
    rex::cvar::SetFlagByName("perf_ab_test", comparison);
  }
  REXLOG_WARN("Leistungstest an, Vergleich: {}", comparison);
  REXLOG_WARN("Leistungstest an, Dateien in {} ({}_...)", folder.string(), prefix);
}

void CodApp::SwitchMode(const std::string& mode, bool from_guest_thread) {
  if (data_dir_.empty() || mode_switch_written_.exchange(true)) {
    return;
  }
  const cod::ModeDef* target = cod::FindMode(*game_, mode);
  const bool available = target && cod::ModuleAvailable(*target);
  const bool restart = available && std::string_view(target->id) == mode_->id;
  const cod::GameAndMode next{game_->id, available ? mode : std::string(mode_->id)};
  // One-shot request plus the permanent choice, so the app keeps starting in
  // the mode that was played last.
  for (const char* file : {kNextModeFile, kDefaultModeFile}) {
    std::ofstream out(data_dir_ / file, std::ios::trunc);
    out << cod::FormatGameAndMode(next) << "\n";
  }
  REXLOG_WARN("Moduswechsel, naechster Start: {}", cod::FormatGameAndMode(next));
  rex::FlushLogging();
  std::string message =
      restart
          ? std::string("Das Spiel muss für diese Karte neu starten (wie auf der Xbox).\n\n"
                        "Die App wird jetzt geschlossen. Öffne sie einfach wieder – die Karte "
                        "lädt dann direkt.")
      : !available
          ? fmt::format("{} ist in dieser Version nicht enthalten.\n\n"
                        "Die App wird jetzt geschlossen und startet beim nächsten Öffnen wieder im "
                        "Modus {}.",
                        target ? ModeLabel(*target) : std::string("Dieser Modus"),
                        ModeLabel(*mode_))
          : fmt::format("Wechsel zu {}.\n\nDie App wird jetzt geschlossen. Öffne sie einfach "
                        "wieder – sie startet dann direkt dort.",
                        ModeLabel(*target));
  app_context().CallInUIThreadDeferred([message = std::move(message)]() {
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Help, message);
    rex::FlushLogging();
    std::_Exit(0);
  });
  if (from_guest_thread) {
    // Keep the game from running into its own shutdown (which crashed) while
    // the message is shown.
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
}

void CodApp::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  (void)drawer;
  if (rex::ui::Window* main_window = window()) {
    REXLOG_WARN("Display {}x{}", main_window->GetActualPhysicalWidth(),
                main_window->GetActualPhysicalHeight());
  }
  // The start screen and the in-game panels are created once it is clear
  // whether the start screen shows (OnFinalizePaths).
}

void CodApp::SaveLaunchData(const std::vector<uint8_t>& data, const std::string& mode) {
  if (data_dir_.empty() || data.empty()) {
    return;
  }
  const std::string header =
      cod::FormatGameAndMode(cod::GameAndMode{game_->id, mode}) + "\n";
  std::ofstream out(data_dir_ / kLaunchDataFile, std::ios::binary | std::ios::trunc);
  out.write(header.data(), std::streamsize(header.size()));
  out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  REXLOG_WARN("Startdaten fuer den naechsten Start gespeichert ({} Bytes, {})", data.size(),
              cod::FormatGameAndMode(cod::GameAndMode{game_->id, mode}));
}

void CodApp::RestoreLaunchData() {
  if (data_dir_.empty()) {
    return;
  }
  const fs::path path = data_dir_ / kLaunchDataFile;
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return;
  }
  std::ifstream in(path, std::ios::binary);
  std::string header;
  std::getline(in, header);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  // Used once, whatever happens next.
  fs::remove(path, ec);
  const std::string expected = cod::FormatGameAndMode(cod::GameAndMode{game_->id, mode_->id});
  if (header != expected || data.empty()) {
    REXLOG_WARN("Startdaten fuer '{}' verworfen (Start: {})", header, expected);
    return;
  }
  auto xam = runtime()->kernel_state()->GetKernelModule<rex::kernel::xam::XamModule>("xam.xex");
  if (!xam) {
    return;
  }
  auto& loader = xam->loader_data();
  loader.launch_data_present = true;
  loader.launch_data = std::move(data);
  REXLOG_WARN("Startdaten vom letzten Start uebergeben ({} Bytes)", loader.launch_data.size());
}

void CodApp::OnPreLaunchModule() {
  RestoreLaunchData();
  if (module_ && std::string_view(module_->id) == "bo1_sp") {
    Dl_info info = {};
    const void* module_base = dladdr(module_, &info) ? info.dli_fbase : nullptr;
    cod::StartBo1StreamWatch(runtime()->memory()->virtual_membase(), module_base);
  }
  if (game_->zombies_flag_address == 0 || std::string_view(mode_->id) != "zombies") {
    return;
  }
  auto* flags = runtime()->memory()->TranslateVirtual(game_->zombies_flag_address);
  uint32_t value = rex::memory::load_and_swap<uint32_t>(flags);
  value |= game_->zombies_flag_bit;
  rex::memory::store_and_swap<uint32_t>(flags, value);
  REXLOG_INFO("Start direkt in Zombies");
}

void CodApp::OnShutdown() {
  launcher_.reset();
  launcher_settings_.reset();
  perf_overlay_.reset();
  settings_dialog_.reset();
  if (mode_switch_written_ || !game_) {
    return;
  }
  // The game switches modes by asking the dashboard to launch another
  // executable. A process cannot start a new copy of itself on iOS, so
  // remember the request; the next app start boots into the requested mode.
  auto* kernel_state = runtime() ? runtime()->kernel_state() : nullptr;
  if (!kernel_state || data_dir_.empty()) {
    return;
  }
  auto xam = kernel_state->GetKernelModule<rex::kernel::xam::XamModule>("xam.xex");
  if (!xam) {
    return;
  }
  const auto& loader = xam->loader_data();
  const bool zombies_data = loader.launch_data_present && loader.launch_data.size() > 4 &&
                            std::any_of(loader.launch_data.begin() + 4, loader.launch_data.end(),
                                        [](uint8_t b) { return b != 0; });
  const std::string next_mode = RequestedMode(loader.launch_path, zombies_data);
  const cod::ModeDef* target = next_mode.empty() ? nullptr : cod::FindMode(*game_, next_mode);
  if (!target || !cod::ModuleAvailable(*target)) {
    return;
  }
  const cod::GameAndMode next{game_->id, next_mode};
  for (const char* file : {kNextModeFile, kDefaultModeFile}) {
    std::ofstream out(data_dir_ / file, std::ios::trunc);
    out << cod::FormatGameAndMode(next) << "\n";
  }
  REXLOG_WARN("Spiel wollte '{}' starten, naechster Start: {}", loader.launch_path,
              cod::FormatGameAndMode(next));
}
