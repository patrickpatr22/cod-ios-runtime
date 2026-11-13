// Call of Duty iPad - see ui_kit.h.

#include "ui_kit.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>

#include "launcher.h"

namespace cod::ui {

namespace {
constexpr float kPi = 3.14159265358979f;
using Clock = std::chrono::steady_clock;
}  // namespace

ImU32 WithAlpha(ImU32 color, float alpha) {
  const int a =
      static_cast<int>(((color >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  ImU32 out = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const float ca = float((a >> shift) & 0xFF);
    const float cb = float((b >> shift) & 0xFF);
    out |= ImU32(ca + (cb - ca) * t + 0.5f) << shift;
  }
  return out;
}

ImFont* FontOr(ImFont* font) { return font ? font : ImGui::GetFont(); }
float SizeOf(ImFont* font) { return font ? font->LegacySize : ImGui::GetFontSize(); }

ImVec2 Measure(ImFont* font, const char* text, float wrap_width) {
  return FontOr(font)->CalcTextSizeA(SizeOf(font), FLT_MAX, wrap_width, text);
}

void Text(ImDrawList* dl, ImFont* font, ImVec2 pos, ImU32 color, const char* text,
          float wrap_width) {
  dl->AddText(FontOr(font), SizeOf(font), pos, color, text, nullptr, wrap_width);
}

void DrawCrosshair(ImDrawList* dl, ImVec2 c, float r, ImU32 color) {
  const float t = std::max(2.0f, r * 0.11f);
  dl->AddCircle(c, r * 0.72f, color, 48, t);
  dl->AddLine(ImVec2(c.x - r * 1.15f, c.y), ImVec2(c.x - r * 0.35f, c.y), color, t);
  dl->AddLine(ImVec2(c.x + r * 0.35f, c.y), ImVec2(c.x + r * 1.15f, c.y), color, t);
  dl->AddLine(ImVec2(c.x, c.y - r * 1.15f), ImVec2(c.x, c.y - r * 0.35f), color, t);
  dl->AddLine(ImVec2(c.x, c.y + r * 0.35f), ImVec2(c.x, c.y + r * 1.15f), color, t);
  dl->AddCircleFilled(c, t * 0.9f, color);
}

void DrawSkull(ImDrawList* dl, ImVec2 c, float r, ImU32 color, ImU32 hole) {
  dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.18f), r * 0.82f, color, 48);
  dl->AddRectFilled(ImVec2(c.x - r * 0.48f, c.y + r * 0.3f), ImVec2(c.x + r * 0.48f, c.y + r * 0.98f),
                    color, r * 0.14f);
  dl->AddCircleFilled(ImVec2(c.x - r * 0.32f, c.y - r * 0.08f), r * 0.21f, hole, 24);
  dl->AddCircleFilled(ImVec2(c.x + r * 0.32f, c.y - r * 0.08f), r * 0.21f, hole, 24);
  dl->AddTriangleFilled(ImVec2(c.x, c.y + r * 0.16f), ImVec2(c.x - r * 0.1f, c.y + r * 0.36f),
                        ImVec2(c.x + r * 0.1f, c.y + r * 0.36f), hole);
  for (int i = -1; i <= 1; ++i) {
    const float x = c.x + i * r * 0.17f;
    dl->AddLine(ImVec2(x, c.y + r * 0.6f), ImVec2(x, c.y + r * 0.98f), hole,
                std::max(1.5f, r * 0.06f));
  }
}

void DrawFlag(ImDrawList* dl, ImVec2 c, float r, ImU32 color) {
  const float t = std::max(2.0f, r * 0.11f);
  dl->AddLine(ImVec2(c.x - r * 0.65f, c.y - r), ImVec2(c.x - r * 0.65f, c.y + r), color, t);
  dl->AddQuadFilled(ImVec2(c.x - r * 0.6f, c.y - r * 0.95f), ImVec2(c.x + r * 0.85f, c.y - r * 0.7f),
                    ImVec2(c.x + r * 0.85f, c.y + r * 0.1f), ImVec2(c.x - r * 0.6f, c.y - r * 0.15f),
                    color);
}

void DrawGear(ImDrawList* dl, ImVec2 c, float r, ImU32 color, ImU32 hole) {
  for (int i = 0; i < 8; ++i) {
    const float a = i * kPi / 4.0f;
    const ImVec2 d(std::cos(a), std::sin(a));
    const ImVec2 n(-d.y, d.x);
    const float w = r * 0.2f;
    const ImVec2 base(c.x + d.x * r * 0.6f, c.y + d.y * r * 0.6f);
    const ImVec2 tip(c.x + d.x * r, c.y + d.y * r);
    dl->AddQuadFilled(ImVec2(base.x - n.x * w, base.y - n.y * w),
                      ImVec2(tip.x - n.x * w * 0.75f, tip.y - n.y * w * 0.75f),
                      ImVec2(tip.x + n.x * w * 0.75f, tip.y + n.y * w * 0.75f),
                      ImVec2(base.x + n.x * w, base.y + n.y * w), color);
  }
  dl->AddCircleFilled(c, r * 0.72f, color, 32);
  dl->AddCircleFilled(c, r * 0.3f, hole, 24);
}

void DrawLock(ImDrawList* dl, ImVec2 c, float r, ImU32 color) {
  const float t = std::max(2.0f, r * 0.14f);
  dl->PathArcTo(ImVec2(c.x, c.y - r * 0.15f), r * 0.42f, kPi, 2.0f * kPi, 16);
  dl->PathStroke(color, ImDrawFlags_None, t);
  dl->AddLine(ImVec2(c.x - r * 0.42f, c.y - r * 0.15f), ImVec2(c.x - r * 0.42f, c.y + r * 0.1f),
              color, t);
  dl->AddLine(ImVec2(c.x + r * 0.42f, c.y - r * 0.15f), ImVec2(c.x + r * 0.42f, c.y + r * 0.1f),
              color, t);
  dl->AddRectFilled(ImVec2(c.x - r * 0.65f, c.y + r * 0.05f), ImVec2(c.x + r * 0.65f, c.y + r * 0.9f),
                    color, r * 0.12f);
}

void DrawPlay(ImDrawList* dl, ImVec2 c, float r, ImU32 color) {
  dl->AddTriangleFilled(ImVec2(c.x - r * 0.45f, c.y - r * 0.6f), ImVec2(c.x - r * 0.45f, c.y + r * 0.6f),
                        ImVec2(c.x + r * 0.6f, c.y), color);
}

void DrawCross(ImDrawList* dl, ImVec2 c, float r, ImU32 color, float thickness) {
  dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), color, thickness);
  dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), color, thickness);
}

void DrawCheck(ImDrawList* dl, ImVec2 c, float r, ImU32 color, float thickness) {
  const ImVec2 points[3] = {ImVec2(c.x - r, c.y), ImVec2(c.x - r * 0.3f, c.y + r * 0.7f),
                            ImVec2(c.x + r, c.y - r * 0.7f)};
  dl->AddPolyline(points, 3, color, ImDrawFlags_None, thickness);
}

void DrawRadialGlow(ImDrawList* dl, ImVec2 center, float radius, ImU32 inner) {
  constexpr int kSegments = 96;
  constexpr int kRings = 6;
  const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
  // Rings with falling alpha (smooth, close to a quadratic fall-off).
  dl->PrimReserve(kSegments * 6 * kRings, kSegments * (kRings + 1));
  const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
  for (int ring = 0; ring <= kRings; ++ring) {
    const float t = static_cast<float>(ring) / kRings;
    const float alpha = (1.0f - t) * (1.0f - t);
    const ImU32 color = WithAlpha(inner, alpha);
    const float r = radius * t;
    for (int i = 0; i < kSegments; ++i) {
      const float a = 2.0f * kPi * i / kSegments;
      dl->PrimWriteVtx(ImVec2(center.x + std::cos(a) * r, center.y + std::sin(a) * r), white, color);
    }
  }
  for (int ring = 0; ring < kRings; ++ring) {
    for (int i = 0; i < kSegments; ++i) {
      const int j = (i + 1) % kSegments;
      const ImDrawIdx a0 = static_cast<ImDrawIdx>(base + ring * kSegments + i);
      const ImDrawIdx a1 = static_cast<ImDrawIdx>(base + ring * kSegments + j);
      const ImDrawIdx b0 = static_cast<ImDrawIdx>(base + (ring + 1) * kSegments + i);
      const ImDrawIdx b1 = static_cast<ImDrawIdx>(base + (ring + 1) * kSegments + j);
      dl->PrimWriteIdx(a0);
      dl->PrimWriteIdx(b0);
      dl->PrimWriteIdx(b1);
      dl->PrimWriteIdx(a0);
      dl->PrimWriteIdx(b1);
      dl->PrimWriteIdx(a1);
    }
  }
}

void DrawBackdrop(ImDrawList* dl, ImVec2 origin, ImVec2 size, ImU32 accent) {
  const ImVec2 end(origin.x + size.x, origin.y + size.y);
  dl->AddRectFilledMultiColor(origin, end, kBackgroundTop, kBackgroundTop, kBackgroundBottom,
                              kBackgroundBottom);
  DrawRadialGlow(dl, ImVec2(origin.x + size.x * 0.84f, origin.y + size.y * 0.06f), size.y * 1.05f,
                 WithAlpha(accent, 0.30f));
  // Fine diagonal lines, as on the Xbox dashboards.
  const ImU32 line = IM_COL32(255, 255, 255, 6);
  dl->PushClipRect(origin, end, true);
  for (float x = origin.x - size.y; x < end.x; x += 46.0f) {
    dl->AddLine(ImVec2(x, end.y), ImVec2(x + size.y, origin.y), line, 1.0f);
  }
  dl->PopClipRect();
}

float DrawBadge(ImDrawList* dl, ImVec2 pos, const char* label, ImU32 color) {
  ImFont* font = Fonts().small;
  const float h = SizeOf(font) + 6.0f;
  const ImVec2 size = Measure(font, label);
  const float w = std::max(h, size.x + 12.0f);
  dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), color, h * 0.5f, 0, 1.6f);
  Text(dl, font, ImVec2(pos.x + (w - size.x) * 0.5f, pos.y + (h - size.y) * 0.5f), color, label);
  return w;
}

float DrawChip(ImDrawList* dl, ImVec2 pos, ImU32 dot, const char* text) {
  ImFont* font = Fonts().small;
  const ImVec2 size = Measure(font, text);
  const ImVec2 a = pos;
  const ImVec2 b(pos.x + size.x + 40.0f, pos.y + size.y + 16.0f);
  dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 12), (b.y - a.y) * 0.5f);
  dl->AddRect(a, b, IM_COL32(255, 255, 255, 22), (b.y - a.y) * 0.5f);
  dl->AddCircleFilled(ImVec2(a.x + 17.0f, (a.y + b.y) * 0.5f), 4.0f, dot);
  Text(dl, font, ImVec2(a.x + 28.0f, a.y + 8.0f), kTextPrimary, text);
  return b.x - a.x;
}

float DrawHint(ImDrawList* dl, float x, float y, const char* badge, const char* text,
               ImU32 color) {
  ImFont* font = Fonts().small;
  x += DrawBadge(dl, ImVec2(x, y), badge, color) + 8.0f;
  Text(dl, font, ImVec2(x, y + 3.0f), color, text);
  return x + Measure(font, text).x + 22.0f;
}

// --- Controller --------------------------------------------------------------

PadReader::PadReader() {
  // Start the gamepad subsystem only if nobody else did (the game's input
  // driver keeps it running while a game runs); stopped again with this
  // reader, so the driver later finds the connected pads.
  if (!SDL_WasInit(SDL_INIT_GAMEPAD)) {
    initialized_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  }
}

PadReader::~PadReader() {
  for (SDL_Gamepad* gamepad : opened_) {
    SDL_CloseGamepad(gamepad);
  }
  if (initialized_) {
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }
}

PadInput PadReader::Poll() {
  PadInput held;
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  for (int i = 0; ids && i < count; ++i) {
    SDL_Gamepad* gamepad = SDL_GetGamepadFromID(ids[i]);
    if (!gamepad) {
      gamepad = SDL_OpenGamepad(ids[i]);
      if (gamepad) {
        opened_.push_back(gamepad);
      }
    }
    if (!gamepad) {
      continue;
    }
    constexpr int kDeadZone = 16000;
    const int lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    const int ly = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
    held.up |= SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -kDeadZone;
    held.down |= SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > kDeadZone;
    held.left |= SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -kDeadZone;
    held.right |= SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > kDeadZone;
    held.accept |= SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH);
    held.back |= SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST);
    held.settings |= SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH) ||
                     SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START);
    held.prev_tab |= SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    held.next_tab |= SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
  }
  SDL_free(ids);

  const auto now = Clock::now();
  PadInput pressed;
  pressed.up = Repeat(0, held.up, now);
  pressed.down = Repeat(1, held.down, now);
  pressed.left = Repeat(2, held.left, now);
  pressed.right = Repeat(3, held.right, now);
  pressed.accept = held.accept && !last_.accept;
  pressed.back = held.back && !last_.back;
  pressed.settings = held.settings && !last_.settings;
  pressed.prev_tab = held.prev_tab && !last_.prev_tab;
  pressed.next_tab = held.next_tab && !last_.next_tab;
  pressed.any = pressed.up || pressed.down || pressed.left || pressed.right || pressed.accept ||
                pressed.back || pressed.settings || pressed.prev_tab || pressed.next_tab;
  last_ = held;
  return pressed;
}

bool PadReader::Repeat(int direction, bool held, Clock::time_point now) {
  if (!held) {
    held_since_[direction] = {};
    return false;
  }
  if (held_since_[direction] == Clock::time_point{}) {
    held_since_[direction] = now;
    next_repeat_[direction] = now + std::chrono::milliseconds(420);
    return true;
  }
  if (now >= next_repeat_[direction]) {
    next_repeat_[direction] = now + std::chrono::milliseconds(140);
    return true;
  }
  return false;
}

}  // namespace cod::ui
