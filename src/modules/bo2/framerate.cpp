// BO2 iOS - frame rate limit, see framerate.h.
//
// sub_824985D0 (Title Update 18; disc version: sub_82423990) is
// Dvar_RegisterInt(name, default, min, max, flags, description); com_maxfps
// is registered with 60 (range 0..1000).

#include "framerate.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>

namespace bo2 {
namespace {
std::atomic<int> g_frame_rate_limit{60};
}  // namespace

void SetFrameRateLimit(int fps) {
  g_frame_rate_limit.store(std::clamp(fps, 30, 240), std::memory_order_relaxed);
}

int FrameRateLimit() {
  return g_frame_rate_limit.load(std::memory_order_relaxed);
}

}  // namespace bo2

extern "C" void __imp__sub_824985D0(PPCContext& __restrict ctx, uint8_t* base);

extern "C" REX_FUNC(sub_824985D0) {
  const int fps = bo2::FrameRateLimit();
  if (fps != 60 && ctx.r3.u32) {
    const char* name = reinterpret_cast<const char*>(base + ctx.r3.u32);
    if (std::strcmp(name, "com_maxfps") == 0) {
      REXLOG_WARN("BO2: com_maxfps {} statt {}", fps, ctx.r4.s32);
      ctx.r4.u64 = uint64_t(fps);
    }
  }
  __imp__sub_824985D0(ctx, base);
}
