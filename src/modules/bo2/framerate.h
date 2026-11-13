// BO2 iOS - frame rate limit. On the Xbox the game caps itself with the
// com_maxfps dvar (registered with 60) and waits for the 60 Hz vertical
// blank; for 120 Hz both are raised (the vertical blank through the guest
// video mode's refresh rate).

#pragma once

namespace bo2 {

// 60 or 120, read when the game registers its dvars (next start).
void SetFrameRateLimit(int fps);
int FrameRateLimit();

}  // namespace bo2
