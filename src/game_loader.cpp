// Call of Duty iPad - game module loader. See game_loader.h.

#include "game_loader.h"

#include <climits>
#include <cstdint>
#include <system_error>
#include <vector>

#include <dlfcn.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <rex/logging.h>

namespace cod {

namespace {

namespace fs = std::filesystem;

fs::path ExecutableDir() {
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size + 1, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
    std::error_code ec;
    fs::path path = fs::canonical(buffer.data(), ec);
    return (ec ? fs::path(buffer.data()) : path).parent_path();
  }
  return {};
#else
  std::error_code ec;
  return fs::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

std::string LibraryName(const std::string& id) {
#if defined(__APPLE__)
  return "libcod_" + id + ".dylib";
#else
  return "libcod_" + id + ".so";
#endif
}

}  // namespace

fs::path GameModulePath(const std::string& id) {
  const fs::path dir = ExecutableDir();
  const std::string name = LibraryName(id);
  std::error_code ec;
  for (const fs::path& candidate : {dir / "Frameworks" / name, dir / name}) {
    if (fs::exists(candidate, ec)) {
      return candidate;
    }
  }
  return dir / "Frameworks" / name;
}

bool GameModuleFileExists(const std::string& id) {
  std::error_code ec;
  return fs::exists(GameModulePath(id), ec);
}

const GameModule* LoadGameModule(const std::string& id, std::string* error) {
  const fs::path path = GameModulePath(id);
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char* reason = dlerror();
    *error = "Spielcode " + path.filename().string() + " nicht ladbar: " +
             (reason ? reason : "unbekannter Fehler");
    return nullptr;
  }
  using GetModule = const GameModule* (*)();
  auto get = reinterpret_cast<GetModule>(dlsym(handle, "cod_game_module"));
  const GameModule* module = get ? get() : nullptr;
  if (!module || module->abi != kGameModuleAbi || !module->image) {
    *error = "Spielcode " + path.filename().string() + " passt nicht zu dieser App.";
    return nullptr;
  }
  REXLOG_WARN("Spielcode {} geladen ({})", module->id, path.string());
  return module;
}

}  // namespace cod
