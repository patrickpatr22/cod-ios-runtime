// BO2 iOS - unlock all weapon camos and reticles, see unlocks.h.
//
// Addresses are Title Update 18.
//
// sub_826A3A90 is BG_UnlockablesItemOptionLocked(controller, item, option):
// true when a weapon option (camo, reticle, ...) can't be chosen. It asks
//   1. sub_826A3930 = BG_UnlockablesItemOptionLockedByChallenge: the camo
//      challenges (Gold needs all other camos of the weapon, Diamond Gold on
//      all weapons of the class), and the "elite" mask;
//   2. sub_826BF160(option) / sub_825A0E70(controller, 0): options sold as
//      DLC (personalization packs) and whether the content is there;
//   3. the kill-switch dvars tu7_itemOptionLocked(Result);
//   4. the weapon's level against the option's unlock level.
// Its callers are the Create-a-Class menu (LUI GetItemOptionLocked,
// sub_82723600) and the loadout checks, so answering "unlocked" here makes the
// option selectable and keeps it on the weapon in the match.
//
// With the setting on, only check 2 stays: a camo of a personalization pack
// that isn't installed would have no textures.

#include "unlocks.h"

#include <atomic>
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>

namespace bo2 {
namespace {
std::atomic<bool> g_unlock_all_camos{true};
}  // namespace

void SetUnlockAllCamos(bool unlock) {
  if (g_unlock_all_camos.exchange(unlock, std::memory_order_relaxed) != unlock) {
    REXLOG_WARN("BO2: Tarnungen freischalten {}", unlock ? "an" : "aus");
  }
}

}  // namespace bo2

extern "C" void __imp__sub_826A3A90(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_826BF160);
extern "C" REX_FUNC(sub_825A0E70);

extern "C" REX_FUNC(sub_826A3A90) {
  if (!bo2::g_unlock_all_camos.load(std::memory_order_relaxed)) {
    __imp__sub_826A3A90(ctx, base);
    return;
  }
  const uint64_t controller = ctx.r3.u64;
  const uint64_t option = ctx.r5.u64;
  // Options sold as DLC stay locked when the content isn't there.
  ctx.r3.u64 = option;
  sub_826BF160(ctx, base);
  if ((ctx.r3.u32 & 0xFF) != 0) {
    ctx.r3.u64 = controller;
    ctx.r4.u64 = 0;
    sub_825A0E70(ctx, base);
    if ((ctx.r3.u32 & 0xFF) == 0) {
      ctx.r3.u64 = 1;
      return;
    }
  }
  ctx.r3.u64 = 0;
}
