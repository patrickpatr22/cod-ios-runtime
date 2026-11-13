// Call of Duty iPad - look of the start screen and the settings: colours,
// text, small drawn icons, controller input. Everything is drawn with ImGui
// draw lists in logical points (1210 x 834 on an iPad Pro 11").

#pragma once

#include <array>
#include <chrono>
#include <vector>

#include <imgui.h>

struct SDL_Gamepad;

namespace cod::ui {

// --- Colours ---------------------------------------------------------------

constexpr ImU32 Rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
ImU32 WithAlpha(ImU32 color, float alpha);
ImU32 Mix(ImU32 a, ImU32 b, float t);

inline constexpr ImU32 kBackgroundTop = Rgb(17, 20, 28);
inline constexpr ImU32 kBackgroundBottom = Rgb(7, 8, 12);
inline constexpr ImU32 kSidebar = Rgb(10, 12, 17, 235);
inline constexpr ImU32 kSidebarLine = Rgb(32, 38, 50);
inline constexpr ImU32 kPanel = Rgb(21, 26, 36, 240);
inline constexpr ImU32 kPanelHover = Rgb(28, 35, 48, 245);
inline constexpr ImU32 kPanelLine = Rgb(40, 48, 63);
inline constexpr ImU32 kTextPrimary = Rgb(242, 244, 248);
inline constexpr ImU32 kTextSecondary = Rgb(156, 165, 181);
inline constexpr ImU32 kTextMuted = Rgb(96, 104, 119);
inline constexpr ImU32 kGood = Rgb(76, 208, 125);
inline constexpr ImU32 kWarn = Rgb(242, 179, 61);
inline constexpr ImU32 kAccentDefault = Rgb(255, 138, 31);

// --- Text --------------------------------------------------------------------

ImFont* FontOr(ImFont* font);
float SizeOf(ImFont* font);
ImVec2 Measure(ImFont* font, const char* text, float wrap_width = 0.0f);
void Text(ImDrawList* dl, ImFont* font, ImVec2 pos, ImU32 color, const char* text,
          float wrap_width = 0.0f);

// --- Shapes ------------------------------------------------------------------

void DrawCrosshair(ImDrawList* dl, ImVec2 c, float r, ImU32 color);
void DrawSkull(ImDrawList* dl, ImVec2 c, float r, ImU32 color, ImU32 hole);
void DrawFlag(ImDrawList* dl, ImVec2 c, float r, ImU32 color);
void DrawGear(ImDrawList* dl, ImVec2 c, float r, ImU32 color, ImU32 hole);
void DrawLock(ImDrawList* dl, ImVec2 c, float r, ImU32 color);
void DrawPlay(ImDrawList* dl, ImVec2 c, float r, ImU32 color);
void DrawCross(ImDrawList* dl, ImVec2 c, float r, ImU32 color, float thickness);
void DrawCheck(ImDrawList* dl, ImVec2 c, float r, ImU32 color, float thickness);
// Soft radial glow (a radial gradient made of rings).
void DrawRadialGlow(ImDrawList* dl, ImVec2 center, float radius, ImU32 inner);
// Background of the start screen: gradient, glow in the accent colour, fine
// diagonal lines.
void DrawBackdrop(ImDrawList* dl, ImVec2 origin, ImVec2 size, ImU32 accent);
// A controller button as a small round badge with its label; returns its
// width.
float DrawBadge(ImDrawList* dl, ImVec2 pos, const char* label, ImU32 color);
// Rounded status chip with a coloured dot; returns its width.
float DrawChip(ImDrawList* dl, ImVec2 pos, ImU32 dot, const char* text);
// "Badge + text" hints in a row; returns the x after the last one.
float DrawHint(ImDrawList* dl, float x, float y, const char* badge, const char* text,
               ImU32 color);

// --- Controller ----------------------------------------------------------------

struct PadInput {
  bool up = false, down = false, left = false, right = false;
  bool accept = false, back = false, settings = false;
  bool prev_tab = false, next_tab = false;  // LB / RB
  bool any = false;
};

// Reads every connected gamepad through SDL. Presses are edge-triggered,
// held directions repeat.
class PadReader {
 public:
  PadReader();
  ~PadReader();
  PadReader(const PadReader&) = delete;
  PadReader& operator=(const PadReader&) = delete;

  PadInput Poll();

 private:
  bool Repeat(int direction, bool held, std::chrono::steady_clock::time_point now);

  bool initialized_ = false;
  std::vector<SDL_Gamepad*> opened_;
  PadInput last_;
  std::array<std::chrono::steady_clock::time_point, 4> held_since_{};
  std::array<std::chrono::steady_clock::time_point, 4> next_repeat_{};
};

}  // namespace cod::ui
