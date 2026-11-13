// Call of Duty iPad - loads the recompiled code of a game executable (see
// game_module.h).

#pragma once

#include <filesystem>
#include <string>

#include "game_module.h"

namespace cod {

// Where the library of a module is (Frameworks/ in the app bundle, next to
// the executable elsewhere).
std::filesystem::path GameModulePath(const std::string& id);
bool GameModuleFileExists(const std::string& id);

// Loads the module (once; it stays loaded) and returns its table, or nullptr
// with a reason in *error.
const GameModule* LoadGameModule(const std::string& id, std::string* error);

}  // namespace cod
