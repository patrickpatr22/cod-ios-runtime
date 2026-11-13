// Call of Duty iPad - Black Ops II module (default_mp.xex, Title Update 18):
// the settings its hooks read.

#include "game_module.h"

#include "fov.h"
#include "framerate.h"
#include "unlocks.h"

void CodApplyModuleSettings(const cod::GameModuleSettings& settings) {
  bo2::SetFieldOfView(settings.fov);
  bo2::SetFrameRateLimit(settings.frame_rate_limit);
  bo2::SetUnlockAllCamos(settings.unlock_all_camos);
}
