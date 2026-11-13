// BO2 iOS - field of view, see fov.h.
//
// Addresses are Title Update 18 (disc version in brackets).
//
// sub_821AFC90 (sub_8215C188) returns the angle a client's view is drawn
// with: cg_fov, or the weapon's while aiming, a killstreak or kill camera's
// angle, times cg_fovScale and held above cg_fovMin. It is the only reader of
// cg_fovScale. Scaling its result widens every view the same way a larger
// cg_fovScale would, without touching the dvars the scripts keep resetting.
//
// sub_821AFF70 (sub_8215C468)(client, ?, float* fov, float* tan_half,
// float* tan_half_x, float* tan_half_y) turns that angle into the
// projection: tan_half = tan(fov / 2), and with the view's aspect a (cgDC +
// 16, 4:3 or 16:9 from the video mode) the vertical half-angle tangent stays
// the 4:3 one (Hor+): tan_half_y = 0.75 * tan_half, tan_half_x = a *
// tan_half_y. Called by sub_821B00F8 (sub_8215C5F0) for the world and the
// weapon view, whose results end up in the refdef (projection, culling,
// 3D-to-screen positions of the HUD markers).

#include "fov.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

namespace bo2 {
namespace {

std::atomic<double> g_fov_scale{1.0};
std::atomic<double> g_vert_plus_aspect{0.0};
std::atomic<uint64_t> g_vert_plus_calls{0};

// The game's own upper bound for cg_fov.
constexpr double kFovLimit = 160.0;

float LoadGuestFloat(uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(rex::memory::load_and_swap<uint32_t>(base + address));
}

void StoreGuestFloat(uint8_t* base, uint32_t address, float value) {
  rex::memory::store_and_swap<uint32_t>(base + address, std::bit_cast<uint32_t>(value));
}

}  // namespace

void SetFieldOfView(int degrees) {
  degrees = std::clamp(degrees, kFovGame, kFovMax);
  g_fov_scale.store(double(degrees) / double(kFovGame), std::memory_order_relaxed);
  REXLOG_INFO("BO2: Sichtfeld {} Grad", degrees);
}

void SetVertPlusAspect(double display_aspect) {
  if (!(display_aspect > 0.5 && display_aspect < 4.0)) {
    display_aspect = 0.0;
  }
  g_vert_plus_aspect.store(display_aspect, std::memory_order_relaxed);
  REXLOG_WARN("BO2: iPad-Vollbild {}", display_aspect > 0.0 ? "an" : "aus");
}

}  // namespace bo2

extern "C" void __imp__sub_821AFC90(PPCContext& __restrict ctx, uint8_t* base);

extern "C" REX_FUNC(sub_821AFC90) {
  __imp__sub_821AFC90(ctx, base);
  const double scale = bo2::g_fov_scale.load(std::memory_order_relaxed);
  if (scale != 1.0) {
    ctx.f1.f64 = double(float(std::min(ctx.f1.f64 * scale, bo2::kFovLimit)));
  }
}

extern "C" void __imp__sub_821AFF70(PPCContext& __restrict ctx, uint8_t* base);

extern "C" REX_FUNC(sub_821AFF70) {
  const uint32_t client = ctx.r3.u32;
  const uint32_t tan_half_address = ctx.r6.u32;
  const uint32_t tan_half_x_address = ctx.r7.u32;
  const uint32_t tan_half_y_address = ctx.r8.u32;
  __imp__sub_821AFF70(ctx, base);
  const double display_aspect = bo2::g_vert_plus_aspect.load(std::memory_order_relaxed);
  if (display_aspect <= 0.0 || !tan_half_address || !tan_half_x_address || !tan_half_y_address) {
    return;
  }
  const double tan_half = bo2::LoadGuestFloat(base, tan_half_address);
  const double tan_half_x = bo2::LoadGuestFloat(base, tan_half_x_address);
  const double tan_half_y = bo2::LoadGuestFloat(base, tan_half_y_address);
  if (!(tan_half > 0.0 && tan_half_x > 0.0 && tan_half_y > 0.0)) {
    return;
  }
  // Only full-screen views (4:3 or 16:9); split screen has other shapes.
  const double game_aspect = tan_half_x / tan_half_y;
  const bool full_screen = game_aspect > 1.25 && game_aspect < 1.85;
  // The 16:9 view's width (0.75 * 16/9 * tan_half, as the game computes it
  // in 16:9) and the display's height for it: the picture keeps everything
  // the 16:9 one shows left and right and adds more above and below.
  const double new_tan_half_x = tan_half * (0.75 * 16.0 / 9.0);
  const double new_tan_half_y = new_tan_half_x / display_aspect;
  const uint64_t call = bo2::g_vert_plus_calls.fetch_add(1, std::memory_order_relaxed);
  if (call < 4 || call % 5000 == 0) {
    REXLOG_WARN(
        "BO2: iPad-Sicht #{} Spieler {} Seitenverhaeltnis Spiel {:.3f} Anzeige {:.3f}: "
        "tan x {:.4f} y {:.4f} -> {}",
        call, client, game_aspect, display_aspect, tan_half_x, tan_half_y,
        full_screen ? fmt::format("x {:.4f} y {:.4f}", new_tan_half_x, new_tan_half_y)
                    : std::string("unveraendert"));
  }
  if (!full_screen) {
    return;
  }
  bo2::StoreGuestFloat(base, tan_half_x_address, float(new_tan_half_x));
  bo2::StoreGuestFloat(base, tan_half_y_address, float(new_tan_half_y));
}
