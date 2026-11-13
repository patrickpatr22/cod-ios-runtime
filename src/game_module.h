// Call of Duty iPad - interface between the app and the recompiled code of a
// game executable ("game module").
//
// Every executable (Black Ops II default_mp.xex, Black Ops default_mp.xex and
// default.xex, ...) is recompiled into functions named after their guest
// address (sub_82160000, ...), so two games cannot be linked into one binary.
// Each one is a shared library (libcod_<id>) with its symbols hidden; the app
// loads the one of the chosen mode before the runtime is built and only asks
// it for this table.

#pragma once

#include <cstdint>

namespace rex {
struct PPCImageInfo;
}  // namespace rex

namespace cod {

inline constexpr uint32_t kGameModuleAbi = 2;

// Settings the game-specific hooks of a module read. Modules without such
// hooks ignore them.
struct GameModuleSettings {
  int fov = 65;                 // degrees as the game counts cg_fov
  int frame_rate_limit = 120;   // value com_maxfps is registered with
  bool unlock_all_camos = true; // all weapon camos and reticles selectable
  // Black Ops: job worker threads sleep instead of spinning while only
  // waiting jobs are queued.
  bool relax_job_threads = true;
};

struct GameModule {
  uint32_t abi;
  const char* id;                    // "bo2_mp"
  const rex::PPCImageInfo* image;    // recompiled image (PPCImageConfig)
  void (*apply_settings)(const GameModuleSettings& settings);
};

}  // namespace cod

// Exported by every game module.
extern "C" const cod::GameModule* cod_game_module();
