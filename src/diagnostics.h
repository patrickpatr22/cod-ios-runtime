// BO2 iOS - on-device performance diagnostics.
//
// A small on-screen overlay (frame rate, GPU numbers from the Xenos plugin,
// CPU load per thread) so screenshots and screen recordings from the iPad
// carry the measurements, plus the same numbers in the log every 10 seconds.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace rex::system::xam {
struct ContentPackageFile;
}  // namespace rex::system::xam

namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
class WindowedAppContext;
}  // namespace rex::ui

namespace bo2 {

// Samples the CPU time of every thread of the process (Apple platforms only).
void StartCpuMonitor();

// Writes crashes (unhandled faults, abort, trap) with a backtrace to
// <data_dir>/absturz.txt and logs iOS memory warnings.
void InstallCrashReporter(const std::filesystem::path& data_dir, const char* build_label);
// From now on the last logged error goes into the crash report (call once
// logging is set up).
void CaptureLastError();

// Logs the image packs (.ipak) in the game folder. The game streams its
// detailed textures from them; if the ones it asks for are missing, a hint is
// shown once per build (app_context: to show it on the UI thread).
void CheckGameFiles(const std::filesystem::path& game_dir, const std::filesystem::path& data_dir,
                    const std::string& mode, const char* build_label,
                    rex::ui::WindowedAppContext* app_context);

// Lists the content packages found in the DLC folder (DLC, title update) in
// the log and in <data_dir>/dlc.txt, so their state can be checked in the
// Files app.
void WriteContentList(const std::vector<rex::system::xam::ContentPackageFile>& packages,
                      const std::filesystem::path& title_update, uint32_t title_id,
                      const std::filesystem::path& data_dir);

// full = false shows only the shader preparation progress at startup.
std::unique_ptr<rex::ui::ImGuiDialog> CreatePerfOverlay(rex::ui::ImGuiDrawer* drawer,
                                                        std::string header, bool full = true);

}  // namespace bo2
