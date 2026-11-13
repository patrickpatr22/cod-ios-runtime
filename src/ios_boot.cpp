// BO2 iOS - very early start-up diagnostics.
//
// Runs as a static initializer, i.e. before main()/SDL. It creates the
// folders the user needs in the Files app right away and sends everything the
// app prints (runtime log, SDL messages, uncaught exceptions) to
// Documents/Daten/konsole.txt, so a start that fails before the real log
// exists can still be diagnosed without a Mac.

#include <rex/platform.h>

#if REX_PLATFORM_IOS

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <string>
#include <typeinfo>

#include <sys/stat.h>

#include <SDL3/SDL_log.h>

namespace {

void SdlLogToStderr(void*, int category, SDL_LogPriority priority, const char* message) {
  std::fprintf(stderr, "[SDL %d/%d] %s\n", category, static_cast<int>(priority),
               message ? message : "");
}

[[noreturn]] void TerminateWithReport() {
  std::fprintf(stderr, "*** std::terminate");
  if (auto ex = std::current_exception()) {
    try {
      std::rethrow_exception(ex);
    } catch (const std::exception& e) {
      std::fprintf(stderr, ": %s: %s", typeid(e).name(), e.what());
    } catch (...) {
      std::fprintf(stderr, ": unknown exception");
    }
  }
  std::fprintf(stderr, "\n");
  std::fflush(stderr);
  std::abort();
}

struct BootDiagnostics {
  BootDiagnostics() {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) {
      return;
    }
    const std::string documents = std::string(home) + "/Documents";
    const std::string data_dir = documents + "/Daten";
    ::mkdir(documents.c_str(), 0755);
    ::mkdir((documents + "/BO2").c_str(), 0755);  // game files go here
    ::mkdir(data_dir.c_str(), 0755);

    const std::string console = data_dir + "/konsole.txt";
    if (std::freopen(console.c_str(), "w", stdout)) {
      std::setvbuf(stdout, nullptr, _IOLBF, 0);
    }
    if (std::freopen(console.c_str(), "a", stderr)) {
      std::setvbuf(stderr, nullptr, _IONBF, 0);
    }

    std::time_t now = std::time(nullptr);
    char stamp[64] = {};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(stderr, "Call of Duty start %s\nHOME=%s\n", stamp, home);

    // MoltenVK reads its settings from the environment when it is loaded
    // later on: only report real errors (warnings arrive per draw call).
    ::setenv("MVK_CONFIG_LOG_LEVEL", "1", 1);
    ::setenv("MVK_CONFIG_DEBUG", "0", 1);
    // Xbox 360 shaders depend on exact NaN/Inf behaviour (Xenos multiply
    // rules, log/rcp of 0); Metal fast math breaks that and lets single bad
    // pixels spread over the whole image through blur/bloom passes.
    ::setenv("MVK_CONFIG_FAST_MATH_ENABLED", "0", 1);
    // Pipelines needed during play are compiled in the background; spreading
    // each compile over many Metal threads took the cores the game needs.
    ::setenv("MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION", "0", 1);

    std::set_terminate(TerminateWithReport);
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
    SDL_SetLogOutputFunction(SdlLogToStderr, nullptr);
  }
};

BootDiagnostics g_boot_diagnostics;

}  // namespace

#endif  // REX_PLATFORM_IOS
