// Call of Duty iPad - the games of the app: folders, executables, which
// recompiled code (game module) runs which mode. Shared by the start screen
// and the app.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cod {

enum class ModeIcon { kCrosshair, kSkull, kFlag };

struct ModeDef {
  const char* id;           // "mp", "zombies", "sp"
  const char* title;        // tile title (upper case)
  const char* description;  // tile text
  ModeIcon icon;
  // Recompiled code that runs this mode (library libcod_<module>), nullptr
  // when the mode is not part of the app.
  const char* module;
  const char* xex;  // executable in the game folder
  // New in the app, not tested much yet: shown with a "Test" mark.
  bool experimental;
};

struct GameDef {
  const char* id;        // "bo2"
  const char* name;      // game list
  const char* title;     // big title (upper case)
  const char* subtitle;  // studio, year
  const char* folder;    // folder in Documents
  uint32_t title_id;
  uint32_t accent;  // ImU32 (IM_COL32)
  // Shown on the start screen instead of the mode tiles' state when no mode
  // of the game is in the app yet.
  const char* note;
  std::vector<ModeDef> modes;
  // Title update the recompiled code was made from (its build number), 0 for
  // the disc version. A different one would not match the code.
  uint32_t title_update;
  // Black Ops II: Multiplayer and Zombies are one executable; this guest
  // flag (address, bit) is set before the start to boot into Zombies.
  uint32_t zombies_flag_address;
  uint32_t zombies_flag_bit;
};

const std::vector<GameDef>& Games();
const GameDef* FindGame(std::string_view id);
const ModeDef* FindMode(const GameDef& game, std::string_view mode);

// Whether the app contains the recompiled code of the mode.
bool ModuleAvailable(const ModeDef& mode);
// Whether the game folder holds the mode's executable.
bool GameFilesPresent(const std::filesystem::path& documents, const GameDef& game,
                      const ModeDef& mode);

// "<game>:<mode>" as kept in the mode files; a plain mode ("mp", "zombies")
// is Black Ops II (files from before build 25).
struct GameAndMode {
  std::string game;
  std::string mode;
  bool empty() const { return game.empty() || mode.empty(); }
};
GameAndMode ParseGameAndMode(std::string text);
std::string FormatGameAndMode(const GameAndMode& choice);

}  // namespace cod
