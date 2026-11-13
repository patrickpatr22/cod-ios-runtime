// Call of Duty iPad - start screen. See launcher.h.

#include "launcher.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#include "embedded_fonts.h"
#include "games.h"
#include "ui_kit.h"

namespace cod {

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace cod::ui;

constexpr float kPi = 3.14159265358979f;

LauncherFonts g_fonts;

// Latin-1 plus the punctuation the German texts use („“ – … • ·).
constexpr ImWchar kGlyphRanges[] = {0x0020, 0x007E, 0x00A0, 0x00FF, 0x2013, 0x2014,
                                    0x2018, 0x201E, 0x2022, 0x2022, 0x2026, 0x2026,
                                    0x2190, 0x2193, 0x20AC, 0x20AC, 0};

ImFont* AddFont(ImFontAtlas* atlas, const char* file, float size, float density) {
  const EmbeddedFont* font = FindEmbeddedFont(file);
  if (!font) {
    REXLOG_WARN("Launcher: Schrift {} fehlt", file);
    return nullptr;
  }
  ImFontConfig config;
  // The data is a static array; the atlas must not free it.
  config.FontDataOwnedByAtlas = false;
  config.RasterizerDensity = density;
  config.PixelSnapH = true;
  std::snprintf(config.Name, sizeof(config.Name), "%s %.0f", file, size);
  return atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(font->data),
                                     static_cast<int>(font->size), size, &config, kGlyphRanges);
}

// --- Games -------------------------------------------------------------------

struct ModeEntry {
  const ModeDef* def;
  bool available = false;    // recompiled code is in the app
  bool files_found = false;  // executable in the game folder
};

struct GameEntry {
  const GameDef* def;
  std::vector<ModeEntry> modes;
  bool built_in = false;      // at least one mode is in the app
  bool experimental = false;  // every mode in the app is still a test
  bool files_found = false;
  bool title_update_found = false;
  int dlc_count = 0;
  std::vector<std::string> dlc_names;  // display names of the DLC packages
};

std::string Utf16BeToUtf8(const unsigned char* data, size_t max_chars) {
  std::string out;
  for (size_t i = 0; i < max_chars; ++i) {
    const uint32_t c = (uint32_t(data[i * 2]) << 8) | data[i * 2 + 1];
    if (c == 0) {
      break;
    }
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

// "Call of Duty®: Black Ops II Season Pass" -> "Season Pass".
std::string ShortDlcName(std::string name) {
  for (const char* prefix :
       {"Call of Duty® Black Ops II ", "Call of Duty®: Black Ops II ", "Call of Duty: Black Ops II ",
        "COD: Black Ops II ", "Black Ops II ", "Call of Duty®: Black Ops ", "Call of Duty: Black Ops ",
        "Call of Duty® Black Ops ", "Black Ops ", "Call of Duty®: ", "Call of Duty: "}) {
    if (name.rfind(prefix, 0) == 0 && name.size() > std::strlen(prefix)) {
      name.erase(0, std::strlen(prefix));
      break;
    }
  }
  if (!name.empty() && (name[0] == '-' || name[0] == ':')) {
    name.erase(0, 1);
  }
  while (!name.empty() && name[0] == ' ') {
    name.erase(0, 1);
  }
  return name;
}

// Title updates and DLC are kept as the console's package files (any
// subfolder of <game>/DLC); only the header is read here.
void ScanPackages(const fs::path& folder, GameEntry& game) {
  game.title_update_found = false;
  game.dlc_count = 0;
  game.dlc_names.clear();
  std::error_code ec;
  if (game.def->title_id == 0 || !fs::is_directory(folder, ec)) {
    return;
  }
  for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied,
                                            ec),
       end;
       it != end; it.increment(ec)) {
    if (ec) {
      break;
    }
    if (!it->is_regular_file(ec) || it->file_size(ec) < 0x1000) {
      continue;
    }
    std::ifstream in(it->path(), std::ios::binary);
    std::array<unsigned char, 0x500> header{};
    if (!in.read(reinterpret_cast<char*>(header.data()), header.size())) {
      continue;
    }
    const bool is_stfs = std::memcmp(header.data(), "CON ", 4) == 0 ||
                         std::memcmp(header.data(), "LIVE", 4) == 0 ||
                         std::memcmp(header.data(), "PIRS", 4) == 0;
    if (!is_stfs) {
      continue;
    }
    auto be32 = [&](size_t offset) {
      return (uint32_t(header[offset]) << 24) | (uint32_t(header[offset + 1]) << 16) |
             (uint32_t(header[offset + 2]) << 8) | uint32_t(header[offset + 3]);
    };
    if (be32(0x360) != game.def->title_id) {
      continue;
    }
    const uint32_t content_type = be32(0x344);
    if (content_type == 0x000B0000) {
      game.title_update_found = true;
    } else if (content_type == 0x00000002) {
      ++game.dlc_count;
      std::string name = ShortDlcName(Utf16BeToUtf8(header.data() + 0x411, 0x40));
      if (!name.empty() &&
          std::find(game.dlc_names.begin(), game.dlc_names.end(), name) == game.dlc_names.end()) {
        game.dlc_names.push_back(std::move(name));
      }
    }
  }
  std::sort(game.dlc_names.begin(), game.dlc_names.end());
}

void DrawModeIcon(ImDrawList* dl, ModeIcon icon, ImVec2 c, float r, ImU32 color, ImU32 background) {
  switch (icon) {
    case ModeIcon::kCrosshair:
      DrawCrosshair(dl, c, r, color);
      break;
    case ModeIcon::kSkull:
      DrawSkull(dl, c, r, color, background);
      break;
    case ModeIcon::kFlag:
      DrawFlag(dl, c, r, color);
      break;
  }
}

std::string TitleCase(const char* upper) {
  std::string text(upper);
  bool word_start = true;
  for (char& c : text) {
    if (c == ' ' || c == '-') {
      word_start = true;
      continue;
    }
    if (!word_start && c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    word_start = false;
  }
  return text;
}

}  // namespace

// --- The launcher --------------------------------------------------------------

class LauncherView {
 public:
  explicit LauncherView(LauncherOptions options) : options_(std::move(options)) {
    for (const GameDef& def : Games()) {
      GameEntry game;
      game.def = &def;
      const fs::path folder = options_.documents / def.folder;
      for (const ModeDef& mode_def : def.modes) {
        ModeEntry mode;
        mode.def = &mode_def;
        mode.available = options_.module_available ? options_.module_available(mode_def)
                                                   : ModuleAvailable(mode_def);
        mode.files_found = GameFilesPresent(options_.documents, def, mode_def);
        game.built_in |= mode.available;
        game.files_found |= mode.files_found;
        game.modes.push_back(mode);
      }
      game.experimental = game.built_in;
      for (const ModeEntry& mode : game.modes) {
        if (mode.available && !mode.def->experimental) {
          game.experimental = false;
        }
      }
      ScanPackages(folder / "DLC", game);
      if (def.id == options_.last_game) {
        selected_game_ = static_cast<int>(games_.size());
      }
      REXLOG_WARN("Launcher: {} enthalten {}, Dateien {}, Title Update {}, {} DLC-Pakete", def.name,
                  game.built_in, game.files_found ? "da" : "fehlen",
                  game.title_update_found ? "da" : "fehlt", game.dlc_count);
      games_.push_back(std::move(game));
    }
    const GameEntry& game = games_[selected_game_];
    for (size_t i = 0; i < game.modes.size(); ++i) {
      if (game.modes[i].def->id == options_.last_mode) {
        focus_mode_ = static_cast<int>(i);
      }
    }
  }

  void Draw(ImGuiIO& io) {
    const bool settings_open = options_.settings_open && options_.settings_open();
    HandleController(settings_open);
    if (io.MouseDown[0]) {
      controller_active_ = false;
    }

    const ImVec2 display = io.DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##cod_launcher", nullptr, kFlags)) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      ImGui::BeginDisabled(settings_open || starting_);
      DrawBackdrop(dl, ImVec2(0.0f, 0.0f), display, Selected().def->accent);
      const float sidebar_width = std::clamp(display.x * 0.25f, 250.0f, 340.0f);
      DrawSidebar(dl, display, sidebar_width);
      DrawMain(dl, display, sidebar_width);
      ImGui::EndDisabled();
      if (settings_open) {
        dl->AddRectFilled(ImVec2(0.0f, 0.0f), display, IM_COL32(0, 0, 0, 120));
      }
      if (starting_) {
        DrawStarting(dl, display);
      }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    if (start_pending_ && options_.start) {
      // The request goes out after the frame with the start screen was drawn.
      start_pending_ = false;
      options_.start(request_);
    }
  }

 private:
  const GameEntry& Selected() const { return games_[selected_game_]; }

  static bool ModeStartable(const ModeEntry& mode) { return mode.available && mode.files_found; }

  void Start(const GameEntry& game, const ModeEntry& mode) {
    if (starting_ || !ModeStartable(mode)) {
      return;
    }
    starting_ = true;
    start_pending_ = true;
    start_time_ = Clock::now();
    request_ = LaunchRequest{game.def->id, mode.def->id};
    start_label_ = game.def->title;
    start_mode_label_ = mode.def->title;
    REXLOG_WARN("Launcher: Start {} / {}", game.def->id, mode.def->id);
  }

  // --- Controller ---

  void HandleController(bool settings_open) {
    const PadInput input = pad_.Poll();
    if (!input.any || starting_) {
      return;
    }
    controller_active_ = true;
    if (settings_open) {
      // The settings panel reads the controller itself.
      return;
    }
    if (input.settings) {
      OpenSettings();
      return;
    }
    const GameEntry& game = Selected();
    const int game_count = static_cast<int>(games_.size());
    const int mode_count = static_cast<int>(game.modes.size());
    switch (focus_zone_) {
      case Zone::kGames:
        if (input.up && selected_game_ > 0) {
          SelectGame(selected_game_ - 1);
        } else if (input.down) {
          if (selected_game_ + 1 < game_count) {
            SelectGame(selected_game_ + 1);
          } else {
            focus_zone_ = Zone::kSettings;
          }
        } else if ((input.right || input.accept) && mode_count > 0) {
          focus_zone_ = Zone::kModes;
          focus_mode_ = std::clamp(focus_mode_, 0, mode_count - 1);
        }
        break;
      case Zone::kModes:
        if (input.left) {
          if (focus_mode_ > 0) {
            --focus_mode_;
          } else {
            focus_zone_ = Zone::kGames;
          }
        } else if (input.right && focus_mode_ + 1 < mode_count) {
          ++focus_mode_;
        } else if (input.back) {
          focus_zone_ = Zone::kGames;
        } else if (input.accept && focus_mode_ < mode_count) {
          Start(game, game.modes[focus_mode_]);
        }
        break;
      case Zone::kSettings:
        if (input.up) {
          focus_zone_ = Zone::kGames;
        } else if (input.accept) {
          OpenSettings();
        } else if (input.right && mode_count > 0) {
          focus_zone_ = Zone::kModes;
        }
        break;
    }
  }

  void SelectGame(int index) {
    if (index == selected_game_) {
      return;
    }
    selected_game_ = index;
    focus_mode_ = 0;
  }

  void OpenSettings() {
    if (options_.open_settings) {
      options_.open_settings();
    }
  }

  bool Focused(int zone, int index) const {
    return controller_active_ && static_cast<int>(focus_zone_) == zone &&
           (zone == static_cast<int>(Zone::kGames) ? selected_game_ == index : focus_mode_ == index);
  }

  // --- Drawing ---

  void DrawSidebar(ImDrawList* dl, ImVec2 display, float width) {
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(width, display.y), kSidebar);
    dl->AddLine(ImVec2(width, 0.0f), ImVec2(width, display.y), kSidebarLine, 1.0f);

    const float pad = 26.0f;
    float y = 30.0f;
    // App title.
    Text(dl, g_fonts.heading, ImVec2(pad, y), kTextPrimary, "CALL OF DUTY");
    y += SizeOf(g_fonts.heading) + 2.0f;
    Text(dl, g_fonts.small, ImVec2(pad, y), kTextSecondary, "Xbox 360 · iPad");
    y += SizeOf(g_fonts.small) + 26.0f;
    Text(dl, g_fonts.small, ImVec2(pad, y), kTextMuted, "SPIELE");
    y += SizeOf(g_fonts.small) + 8.0f;

    // Game list.
    const float item_height = 60.0f;
    for (size_t i = 0; i < games_.size(); ++i) {
      const GameEntry& game = games_[i];
      const ImVec2 a(10.0f, y);
      const ImVec2 b(width - 10.0f, y + item_height);
      ImGui::SetCursorScreenPos(a);
      ImGui::PushID(static_cast<int>(i));
      const bool clicked = ImGui::InvisibleButton("##game", ImVec2(b.x - a.x, b.y - a.y));
      const bool hovered = ImGui::IsItemHovered();
      ImGui::PopID();
      if (clicked) {
        SelectGame(static_cast<int>(i));
        focus_zone_ = Zone::kGames;
      }
      const bool selected = static_cast<int>(i) == selected_game_;
      if (selected || hovered) {
        dl->AddRectFilled(a, b, selected ? IM_COL32(255, 255, 255, 14) : IM_COL32(255, 255, 255, 8),
                          8.0f);
      }
      if (selected) {
        dl->AddRectFilled(ImVec2(a.x, a.y + 11.0f), ImVec2(a.x + 4.0f, b.y - 11.0f), game.def->accent,
                          2.0f);
      }
      if (Focused(static_cast<int>(Zone::kGames), static_cast<int>(i))) {
        dl->AddRect(a, b, game.def->accent, 8.0f, 0, 2.0f);
      }
      Text(dl, g_fonts.label, ImVec2(a.x + 18.0f, a.y + 7.0f),
           selected ? kTextPrimary : kTextSecondary, game.def->name);
      const char* status;
      ImU32 status_color;
      if (!game.built_in) {
        status = "Noch nicht enthalten";
        status_color = kTextMuted;
      } else if (!game.files_found) {
        status = "Spieldateien fehlen";
        status_color = kWarn;
      } else if (game.experimental) {
        status = "Bereit · Test";
        status_color = kGood;
      } else {
        status = "Bereit";
        status_color = kGood;
      }
      const float status_y = a.y + 10.0f + SizeOf(g_fonts.label);
      dl->AddCircleFilled(ImVec2(a.x + 22.0f, status_y + SizeOf(g_fonts.small) * 0.55f), 3.5f,
                          status_color);
      Text(dl, g_fonts.small, ImVec2(a.x + 32.0f, status_y), kTextSecondary, status);
      y += item_height + 5.0f;
    }

    // Settings and player at the bottom.
    const float bottom = display.y - 26.0f;
    const float button_height = 52.0f;
    const ImVec2 sa(10.0f, bottom - button_height - SizeOf(g_fonts.small) * 2.0f - 20.0f);
    const ImVec2 sb(width - 10.0f, sa.y + button_height);
    ImGui::SetCursorScreenPos(sa);
    const bool settings_clicked = ImGui::InvisibleButton("##settings", ImVec2(sb.x - sa.x, sb.y - sa.y));
    const bool settings_hovered = ImGui::IsItemHovered();
    if (settings_clicked) {
      focus_zone_ = Zone::kSettings;
      OpenSettings();
    }
    const bool settings_focused = controller_active_ && focus_zone_ == Zone::kSettings;
    dl->AddRectFilled(sa, sb, settings_hovered ? kPanelHover : kPanel, 8.0f);
    dl->AddRect(sa, sb, settings_focused ? Selected().def->accent : kPanelLine, 8.0f, 0,
                settings_focused ? 2.0f : 1.0f);
    DrawGear(dl, ImVec2(sa.x + 26.0f, (sa.y + sb.y) * 0.5f), 11.0f, kTextSecondary, kPanel);
    Text(dl, g_fonts.button, ImVec2(sa.x + 48.0f, (sa.y + sb.y) * 0.5f - SizeOf(g_fonts.button) * 0.55f),
         kTextPrimary, "Einstellungen");

    const std::string player = options_.player_name ? options_.player_name() : std::string();
    const std::string player_line = "Spieler: " + player;
    Text(dl, g_fonts.small, ImVec2(pad, sb.y + 14.0f), kTextSecondary, player_line.c_str());
    Text(dl, g_fonts.small, ImVec2(pad, sb.y + 16.0f + SizeOf(g_fonts.small)), kTextMuted,
         options_.build_label.c_str());
  }

  void DrawMain(ImDrawList* dl, ImVec2 display, float sidebar_width) {
    const GameEntry& game = Selected();
    const GameDef& def = *game.def;
    const float left = sidebar_width + 44.0f;
    const float right = display.x - 44.0f;
    const float width = right - left;
    float y = std::max(40.0f, display.y * 0.075f);

    // Title with an accent bar.
    Text(dl, g_fonts.title, ImVec2(left, y), kTextPrimary, def.title);
    if (game.experimental) {
      // "TEST" mark next to the title.
      const ImVec2 title_size = Measure(g_fonts.title, def.title);
      const ImVec2 mark_size = Measure(g_fonts.small, "TEST");
      const ImVec2 a(left + title_size.x + 16.0f, y + SizeOf(g_fonts.title) * 0.32f);
      const ImVec2 b(a.x + mark_size.x + 18.0f, a.y + mark_size.y + 8.0f);
      dl->AddRectFilled(a, b, WithAlpha(kWarn, 0.16f), 5.0f);
      dl->AddRect(a, b, WithAlpha(kWarn, 0.7f), 5.0f);
      Text(dl, g_fonts.small, ImVec2(a.x + 9.0f, a.y + 4.0f), kWarn, "TEST");
    }
    y += SizeOf(g_fonts.title) + 2.0f;
    dl->AddRectFilled(ImVec2(left, y), ImVec2(left + 84.0f, y + 4.0f), def.accent, 2.0f);
    y += 16.0f;
    Text(dl, g_fonts.body, ImVec2(left, y), kTextSecondary, def.subtitle);
    y += SizeOf(g_fonts.body) + 20.0f;

    if (game.built_in) {
      // Status chips.
      float x = left;
      x += DrawChip(dl, ImVec2(x, y), game.files_found ? kGood : kWarn,
                    game.files_found ? "Spieldateien gefunden" : "Spieldateien fehlen");
      x += 10.0f;
      if (def.title_update > 0) {
        char tu[48];
        std::snprintf(tu, sizeof(tu), game.title_update_found ? "Title Update %u da"
                                                              : "Title Update %u fehlt",
                      def.title_update);
        x += DrawChip(dl, ImVec2(x, y), game.title_update_found ? kGood : kWarn, tu);
        x += 10.0f;
      }
      char dlc[48];
      std::snprintf(dlc, sizeof(dlc), "%d DLC-Pakete", game.dlc_count);
      DrawChip(dl, ImVec2(x, y), game.dlc_count > 0 ? kGood : kTextMuted, dlc);
      y += SizeOf(g_fonts.small) + 16.0f + 22.0f;
      std::string hint;
      if (!game.files_found) {
        hint = std::string("Kopiere den Inhalt deiner entpackten Disc in der Dateien-App in den "
                           "Ordner „") +
               def.folder + "“ dieser App.";
      } else if (def.title_update > 0 && !game.title_update_found) {
        hint = std::string("Lege das Title Update ") + std::to_string(def.title_update) +
               " (Datei tu…) in den Ordner „" + def.folder + "/DLC“.";
      } else if (game.experimental) {
        hint = std::string(def.name) +
               " ist neu in der App und noch ein Test: Läuft etwas nicht, schick mir die Logs aus "
               "„Daten/logs“.";
      }
      if (!hint.empty()) {
        const ImU32 color = game.experimental && game.files_found ? kTextSecondary : kWarn;
        Text(dl, g_fonts.body, ImVec2(left, y), color, hint.c_str(), width);
        y += Measure(g_fonts.body, hint.c_str(), width).y + 18.0f;
      }
    } else {
      // Not built in yet: an info panel.
      const char* note = def.note ? def.note : "Noch nicht enthalten.";
      const ImVec2 a(left, y);
      const float text_width = width - 96.0f;
      const ImVec2 text_size = Measure(g_fonts.body, note, text_width);
      const ImVec2 b(right, y + std::max(96.0f, text_size.y + SizeOf(g_fonts.button) + 52.0f));
      dl->AddRectFilled(a, b, kPanel, 10.0f);
      dl->AddRect(a, b, kPanelLine, 10.0f);
      DrawLock(dl, ImVec2(a.x + 44.0f, a.y + 46.0f), 18.0f, WithAlpha(def.accent, 0.9f));
      Text(dl, g_fonts.button, ImVec2(a.x + 80.0f, a.y + 20.0f), kTextPrimary, "Noch nicht enthalten");
      Text(dl, g_fonts.body, ImVec2(a.x + 80.0f, a.y + 28.0f + SizeOf(g_fonts.button)), kTextSecondary,
           note, text_width);
      y = b.y + 26.0f;
    }

    // Mode tiles.
    const int count = static_cast<int>(game.modes.size());
    const float gap = 20.0f;
    const float tile_width = (width - gap * (count - 1)) / std::max(1, count);
    const float tile_height = std::clamp(display.y - y - 90.0f, 170.0f, 300.0f);
    for (int i = 0; i < count; ++i) {
      const ModeEntry& mode = game.modes[i];
      const ModeDef& mode_def = *mode.def;
      const ImVec2 a(left + i * (tile_width + gap), y);
      const ImVec2 b(a.x + tile_width, a.y + tile_height);
      const bool startable = ModeStartable(mode);
      ImGui::SetCursorScreenPos(a);
      ImGui::PushID(1000 + i);
      const bool clicked = ImGui::InvisibleButton("##mode", ImVec2(tile_width, tile_height));
      const bool hovered = ImGui::IsItemHovered() && startable;
      const bool active = ImGui::IsItemActive() && startable;
      ImGui::PopID();
      if (clicked) {
        focus_zone_ = Zone::kModes;
        focus_mode_ = i;
        Start(game, mode);
      }
      const bool focused = Focused(static_cast<int>(Zone::kModes), i);
      const float alpha = startable ? 1.0f : 0.45f;
      dl->AddRectFilled(a, b, WithAlpha(hovered || focused ? kPanelHover : kPanel, alpha), 12.0f);
      if (startable) {
        // Accent strip on top.
        dl->AddRectFilled(a, ImVec2(b.x, a.y + 5.0f), WithAlpha(def.accent, active ? 1.0f : 0.85f),
                          12.0f, ImDrawFlags_RoundCornersTop);
      }
      dl->AddRect(a, b, hovered || focused ? def.accent : WithAlpha(kPanelLine, alpha), 12.0f, 0,
                  hovered || focused ? 2.0f : 1.0f);

      const float inner = 24.0f;
      const float icon_radius = std::min(30.0f, tile_height * 0.12f);
      DrawModeIcon(dl, mode_def.icon, ImVec2(a.x + inner + icon_radius, a.y + inner + 12.0f + icon_radius),
                   icon_radius, WithAlpha(startable ? def.accent : kTextMuted, alpha),
                   hovered || focused ? kPanelHover : kPanel);
      if (mode.available && mode_def.experimental) {
        const ImVec2 mark_size = Measure(g_fonts.small, "TEST");
        const ImVec2 ma(b.x - inner - mark_size.x - 18.0f, a.y + inner + 4.0f);
        const ImVec2 mb(b.x - inner, ma.y + mark_size.y + 8.0f);
        dl->AddRect(ma, mb, WithAlpha(kWarn, 0.7f * alpha), 5.0f);
        Text(dl, g_fonts.small, ImVec2(ma.x + 9.0f, ma.y + 4.0f), WithAlpha(kWarn, alpha), "TEST");
      }
      float ty = a.y + inner + 12.0f + icon_radius * 2.0f + 22.0f;
      Text(dl, g_fonts.heading, ImVec2(a.x + inner, ty), WithAlpha(kTextPrimary, alpha), mode_def.title,
           tile_width - inner * 2.0f);
      ty += Measure(g_fonts.heading, mode_def.title, tile_width - inner * 2.0f).y + 6.0f;
      Text(dl, g_fonts.body, ImVec2(a.x + inner, ty), WithAlpha(kTextSecondary, alpha),
           mode_def.description, tile_width - inner * 2.0f);

      // Bottom line: play prompt, or why it can't start.
      const float line_y = b.y - inner - SizeOf(g_fonts.button);
      if (startable) {
        DrawPlay(dl, ImVec2(a.x + inner + 7.0f, line_y + SizeOf(g_fonts.button) * 0.55f), 11.0f,
                 def.accent);
        Text(dl, g_fonts.button, ImVec2(a.x + inner + 22.0f, line_y), def.accent, "SPIELEN");
      } else {
        const char* why = !mode.available ? "Nicht verfügbar" : "Spieldateien fehlen";
        Text(dl, g_fonts.button, ImVec2(a.x + inner, line_y), kTextMuted, why);
      }
    }

    // What was found in the DLC folder.
    const float hint_y = display.y - 26.0f - SizeOf(g_fonts.small) - 6.0f;
    if (game.built_in && !game.dlc_names.empty()) {
      float list_y = y + tile_height + 26.0f;
      if (list_y + SizeOf(g_fonts.small) * 2.0f + 40.0f < hint_y) {
        Text(dl, g_fonts.small, ImVec2(left, list_y), kTextMuted, "INHALTE AUS DEM DLC-ORDNER");
        list_y += SizeOf(g_fonts.small) + 10.0f;
        float x = left;
        for (const std::string& name : game.dlc_names) {
          const float chip_width = Measure(g_fonts.small, name.c_str()).x + 40.0f;
          if (x + chip_width > right) {
            x = left;
            list_y += SizeOf(g_fonts.small) + 26.0f;
            if (list_y + SizeOf(g_fonts.small) + 16.0f > hint_y - 10.0f) {
              break;
            }
          }
          x += DrawChip(dl, ImVec2(x, list_y), def.accent, name.c_str()) + 10.0f;
        }
      }
    }

    // Hints at the bottom.
    float x = left;
    Text(dl, g_fonts.small, ImVec2(x, hint_y + 3.0f), kTextMuted, "Antippen zum Starten");
    x += Measure(g_fonts.small, "Antippen zum Starten").x + 28.0f;
    x = DrawHint(dl, x, hint_y, "A", "Starten", kTextMuted);
    x = DrawHint(dl, x, hint_y, "Y", "Einstellungen", kTextMuted);
    DrawHint(dl, x, hint_y, "B", "Zurück", kTextMuted);

    if (!options_.last_game.empty()) {
      for (const GameEntry& last : games_) {
        if (last.def->id != options_.last_game) {
          continue;
        }
        for (const ModeEntry& mode : last.modes) {
          if (mode.def->id != options_.last_mode) {
            continue;
          }
          std::string text = std::string("Zuletzt gespielt: ") + last.def->name + " – " +
                             TitleCase(mode.def->title);
          const ImVec2 size = Measure(g_fonts.small, text.c_str());
          Text(dl, g_fonts.small, ImVec2(right - size.x, hint_y + 3.0f), kTextMuted, text.c_str());
        }
      }
    }
  }

  void DrawStarting(ImDrawList* dl, ImVec2 display) {
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), display, IM_COL32(4, 5, 8, 230));
    const ImVec2 center(display.x * 0.5f, display.y * 0.5f);
    const ImVec2 title_size = Measure(g_fonts.title, start_label_.c_str());
    Text(dl, g_fonts.title, ImVec2(center.x - title_size.x * 0.5f, center.y - title_size.y - 20.0f),
         kTextPrimary, start_label_.c_str());
    const std::string line = TitleCase(start_mode_label_.c_str()) + " wird gestartet …";
    const ImVec2 line_size = Measure(g_fonts.body, line.c_str());
    Text(dl, g_fonts.body, ImVec2(center.x - line_size.x * 0.5f, center.y + 4.0f), kTextSecondary,
         line.c_str());
    const float t = std::chrono::duration<float>(Clock::now() - start_time_).count();
    const float r = 16.0f;
    const ImVec2 spinner(center.x, center.y + 70.0f);
    dl->PathArcTo(spinner, r, t * 6.0f, t * 6.0f + kPi * 1.4f, 24);
    dl->PathStroke(Selected().def->accent, ImDrawFlags_None, 3.0f);
  }

  enum class Zone { kGames = 0, kModes = 1, kSettings = 2 };

  LauncherOptions options_;
  std::vector<GameEntry> games_;
  PadReader pad_;
  int selected_game_ = 0;
  int focus_mode_ = 0;
  Zone focus_zone_ = Zone::kGames;
  bool controller_active_ = false;
  bool starting_ = false;
  bool start_pending_ = false;
  Clock::time_point start_time_;
  LaunchRequest request_;
  std::string start_label_;
  std::string start_mode_label_;
};

namespace {

class LauncherDialog : public rex::ui::ImGuiDialog {
 public:
  LauncherDialog(rex::ui::ImGuiDrawer* drawer, LauncherOptions options)
      : rex::ui::ImGuiDialog(drawer), view_(std::move(options)) {}

  bool WantsContinuousRepaint() const override { return true; }

 protected:
  void OnDraw(ImGuiIO& io) override { view_.Draw(io); }

 private:
  LauncherView view_;
};

}  // namespace

LauncherPreview::LauncherPreview(LauncherOptions options)
    : view_(std::make_unique<LauncherView>(std::move(options))) {}
LauncherPreview::~LauncherPreview() = default;
void LauncherPreview::Draw(ImGuiIO& io) { view_->Draw(io); }

const LauncherFonts& Fonts() { return g_fonts; }

void AddLauncherFonts(ImFontAtlas* atlas, float pixel_density) {
  const float density = std::clamp(pixel_density, 1.0f, 3.0f);
  g_fonts.title = AddFont(atlas, "Rajdhani-Bold.ttf", 64.0f, density);
  g_fonts.heading = AddFont(atlas, "Rajdhani-Bold.ttf", 32.0f, density);
  g_fonts.label = AddFont(atlas, "Rajdhani-SemiBold.ttf", 25.0f, density);
  g_fonts.button = AddFont(atlas, "Barlow-SemiBold.ttf", 20.0f, density);
  g_fonts.body = AddFont(atlas, "Barlow-Regular.ttf", 18.0f, density);
  g_fonts.small = AddFont(atlas, "Barlow-Regular.ttf", 15.0f, density);
}

std::unique_ptr<rex::ui::ImGuiDialog> CreateLauncher(rex::ui::ImGuiDrawer* drawer,
                                                     LauncherOptions options) {
  return std::make_unique<LauncherDialog>(drawer, std::move(options));
}

}  // namespace cod
