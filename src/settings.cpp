// Call of Duty iPad - user settings and the settings screen. See settings.h.

#include "settings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

#include <SDL3/SDL_gamepad.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/presenter.h>

#include "launcher.h"
#include "ui_kit.h"

namespace bo2 {

namespace {

namespace fs = std::filesystem;
using namespace cod::ui;
using Clock = std::chrono::steady_clock;

constexpr const char* kSettingsFile = "einstellungen.txt";
constexpr int kSettingsVersion = 25;

std::string Trim(std::string text) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
  text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
  return text;
}

// Gamertags are up to 15 letters, digits and spaces.
std::string SanitizeName(const std::string& name) {
  std::string clean;
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-') {
      clean += c;
    }
    if (clean.size() >= 15) {
      break;
    }
  }
  clean = Trim(clean);
  return clean.empty() ? "User" : clean;
}

const char* FramePacingName(int frame_rate) {
  return frame_rate == 60 ? "60" : frame_rate == 120 ? "120" : "auto";
}

bool IsOn(const std::string& value) { return value == "an" || value == "true" || value == "1"; }
bool IsOff(const std::string& value) { return value == "aus" || value == "false" || value == "0"; }

// --- Layout of the settings screen -------------------------------------------------

// One card on a tab. Built every frame from the current settings.
struct Card {
  enum class Kind { kToggle, kSegments, kSlider, kText, kButtons, kInfo };
  Kind kind = Kind::kInfo;
  int column = 0;
  std::string title;
  std::string text;   // description
  std::string badge;  // small mark next to the title ("Sofort", "Test", ...)
  ImU32 badge_color = kTextSecondary;
  // kToggle
  bool* value = nullptr;
  // kSegments: labels, the selected index
  std::vector<std::string> options;
  int selected = 0;
  // kSlider
  int* number = nullptr;
  int min = 0, max = 0, reset = 0;
  const char* format = "%d";
  // kButtons: labels
  std::vector<std::string> buttons;
  // What happens: toggled / segment chosen / slider moved / button pressed.
  std::function<void(int index)> on_change;
};

constexpr float kCardPadding = 20.0f;
constexpr float kControlHeight = 44.0f;
constexpr float kToggleWidth = 54.0f;
constexpr float kToggleHeight = 30.0f;

class SettingsDialog : public SettingsPanel {
 public:
  SettingsDialog(rex::ui::ImGuiDrawer* drawer, SettingsContext context, UserSettings settings)
      : SettingsPanel(drawer),
        ctx_(std::move(context)),
        settings_(std::move(settings)),
        start_settings_(settings_),
        last_touch_(Clock::now()) {
    std::snprintf(name_buffer_, sizeof(name_buffer_), "%s", settings_.player_name.c_str());
    if (ctx_.accent == 0) {
      ctx_.accent = kAccentDefault;
    }
  }

  // While the panel is open it follows touches and sliders at once; closed,
  // the button only needs a redraw with each new frame of the game.
  bool WantsContinuousRepaint() const override { return open_.load(std::memory_order_relaxed); }

  void Open() override {
    open_.store(true, std::memory_order_relaxed);
    just_opened_ = true;
  }
  void Close() override { open_.store(false, std::memory_order_relaxed); }
  bool IsOpen() const override { return open_.load(std::memory_order_relaxed); }
  const UserSettings& Current() const override { return settings_; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (in_game()) {
      PollControllerShortcut();
    }
    // The corner button only shows for a few seconds after the screen was
    // touched, so it doesn't sit over the picture while playing with a
    // controller.
    if (io.MouseDown[0] || io.MouseDown[1]) {
      last_touch_ = Clock::now();
    }
    if (!IsOpen()) {
      if (in_game() && Clock::now() - last_touch_ < kButtonVisibleTime) {
        DrawCornerButton(io);
      }
      return;
    }
    ImFont* font = cod::Fonts().body;
    if (font) {
      ImGui::PushFont(font, font->LegacySize);
    }
    DrawPanel(io);
    if (font) {
      ImGui::PopFont();
    }
  }

 private:
  bool in_game() const { return !ctx_.game_name.empty(); }
  const cod::LauncherFonts& fonts() const { return cod::Fonts(); }

  // --- The small button in the game ---

  void DrawCornerButton(ImGuiIO& io) {
    ImFont* font = fonts().button;
    const ImVec2 text_size = Measure(font, "Einstellungen");
    const ImVec2 size(text_size.x + 62.0f, 44.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - size.x - 12.0f, 12.0f));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##cod_settings_button", nullptr, kFlags)) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 a = ImGui::GetCursorScreenPos();
      const ImVec2 b(a.x + size.x, a.y + size.y);
      if (ImGui::InvisibleButton("##open", size)) {
        Open();
      }
      const bool hovered = ImGui::IsItemHovered();
      dl->AddRectFilled(a, b, WithAlpha(hovered ? kPanelHover : kPanel, 0.88f), size.y * 0.5f);
      dl->AddRect(a, b, WithAlpha(ctx_.accent, 0.8f), size.y * 0.5f, 0, 1.5f);
      DrawGear(dl, ImVec2(a.x + 24.0f, (a.y + b.y) * 0.5f), 10.0f, kTextPrimary, kPanel);
      Text(dl, font, ImVec2(a.x + 42.0f, (a.y + b.y) * 0.5f - text_size.y * 0.5f), kTextPrimary,
           "Einstellungen");
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
  }

  // --- Cards per tab ---

  static constexpr const char* kTabNames[] = {"ALLGEMEIN", "GRAFIK", "SPIEL", "FEHLERSUCHE"};
  static constexpr int kTabCount = 4;

  std::vector<Card> BuildCards(int tab) {
    std::vector<Card> cards;
    const char* next_start = in_game() ? "Ab nächstem Start" : "";
    switch (tab) {
      case 0: {
        Card name;
        name.kind = Card::Kind::kText;
        name.title = "Spielername";
        name.text = "So heißt du im Spiel – bis zu 15 Buchstaben, Ziffern und Leerzeichen.";
        name.badge = next_start;
        cards.push_back(std::move(name));

        Card overlay;
        overlay.kind = Card::Kind::kToggle;
        overlay.title = "Leistungsanzeige";
        overlay.text = "Bildrate, GPU- und CPU-Last klein oben links im Bild.";
        overlay.badge = next_start;
        overlay.value = &settings_.overlay;
        cards.push_back(std::move(overlay));

        if (in_game()) {
          Card mode;
          mode.kind = Card::Kind::kButtons;
          mode.column = 1;
          mode.title = "Spielmodus";
          mode.text = "Gerade: " + ctx_.game_name + " – " + ctx_.mode_name +
                      ". Beim Wechsel schließt sich die App; danach einfach wieder öffnen.";
          for (const auto& target : ctx_.switch_targets) {
            mode.buttons.push_back("Zu " + target.second + " wechseln");
          }
          mode.buttons.push_back("Zum Hauptmenü");
          mode.on_change = [this](int index) {
            if (index < static_cast<int>(ctx_.switch_targets.size())) {
              if (ctx_.switch_mode) {
                ctx_.switch_mode(ctx_.switch_targets[index].first);
              }
            } else if (ctx_.to_launcher) {
              ctx_.to_launcher();
            }
          };
          cards.push_back(std::move(mode));
        } else {
          Card info;
          info.kind = Card::Kind::kInfo;
          info.column = 1;
          info.title = "Im Spiel öffnen";
          info.text =
              "Bildschirm antippen und oben rechts auf „Einstellungen“ – oder mit dem Controller "
              "beide Sticks gleichzeitig drücken. Dort kannst du auch den Modus wechseln oder "
              "zurück ins Hauptmenü.";
          cards.push_back(std::move(info));
        }

        Card about;
        about.kind = Card::Kind::kInfo;
        about.column = 1;
        about.title = "Über die App";
        about.badge = ctx_.build_label;
        about.text =
            "Spieldateien liegen in der Dateien-App unter „Auf meinem iPad > Call of Duty“, je "
            "Spiel ein Ordner (BO2, BO1, …) mit einem Unterordner „DLC“ für Title Update und "
            "Map-Packs. Logs und Spielstände: Ordner „Daten“.";
        cards.push_back(std::move(about));
        break;
      }
      case 1: {
        Card pacing;
        pacing.kind = Card::Kind::kSegments;
        pacing.title = "Bildtakt";
        pacing.badge = "Sofort";
        pacing.badge_color = kGood;
        pacing.options = {"Automatisch", "60 FPS", "120 FPS"};
        pacing.selected = settings_.frame_rate == 60 ? 1 : settings_.frame_rate == 120 ? 2 : 0;
        pacing.text = pacing.selected == 0
                          ? "120 FPS, wenn es flüssig läuft, sonst gleichmäßige 60 FPS. Empfohlen."
                      : pacing.selected == 1
                          ? "Jedes Bild steht zwei Bildschirm-Aktualisierungen lang: ruhig und "
                            "gleichmäßig."
                          : "Volle 120 FPS. Kann in Partien schwanken.";
        pacing.on_change = [this](int index) {
          settings_.frame_rate = index == 1 ? 60 : index == 2 ? 120 : 0;
          ApplyFramePacing(settings_.frame_rate);
        };
        cards.push_back(std::move(pacing));

        Card fov;
        fov.kind = Card::Kind::kSlider;
        fov.title = "Sichtfeld";
        fov.badge = "Black Ops II";
        fov.text = "65 Grad wie auf der Xbox. Mehr zeigt mehr vom Rand des Bildes.";
        fov.number = &settings_.fov;
        fov.min = kFovConsole;
        fov.max = kFovMax;
        fov.reset = kFovConsole;
        fov.format = "%d°";
        fov.buttons = {"Zurücksetzen"};
        cards.push_back(std::move(fov));

        Card resolution;
        resolution.kind = Card::Kind::kToggle;
        resolution.column = 1;
        resolution.title = "Doppelte Auflösung";
        resolution.badge = in_game() ? "Test · ab nächstem Start" : "Test";
        resolution.badge_color = kWarn;
        resolution.text =
            "Rendert in doppelter Breite und Höhe: viel schärfer, braucht aber deutlich mehr "
            "Grafikleistung und Speicher. Startet das Spiel damit nicht, schaltet die App beim "
            "nächsten Start von selbst zurück.";
        resolution_toggle_ = settings_.resolution_scale == 2;
        resolution.value = &resolution_toggle_;
        resolution.on_change = [this](int) {
          settings_.resolution_scale = resolution_toggle_ ? 2 : 1;
        };
        cards.push_back(std::move(resolution));

        if (in_game()) {
          Card reload;
          reload.kind = Card::Kind::kButtons;
          reload.column = 1;
          reload.title = "Grafik neu laden";
          reload.text =
              "Hilft bei bunten oder falschen Texturen: Alle Texturen werden neu aufgebaut. Für "
              "einen Moment kann das Bild flackern.";
          reload.buttons = {"Jetzt neu laden"};
          reload.on_change = [](int) {
            static int reload_requests = 0;
            rex::cvar::SetFlagByName("gpu_clear_caches_request", std::to_string(++reload_requests));
            REXLOG_WARN("Grafik neu laden (Einstellungen)");
          };
          cards.push_back(std::move(reload));
        }
        break;
      }
      case 2: {
        Card camos;
        camos.kind = Card::Kind::kToggle;
        camos.title = "Alle Tarnungen freischalten";
        camos.badge = "Black Ops II · sofort";
        camos.badge_color = kGood;
        camos.text =
            "Gold, Diamant und alle Herausforderungs-Tarnungen sowie alle Visiere sind sofort "
            "wählbar – im Mehrspieler-Menü „Klasse erstellen“. Tarnungen aus DLC-Paketen, die "
            "nicht im DLC-Ordner liegen, bleiben gesperrt.";
        camos.value = &settings_.unlock_all_camos;
        cards.push_back(std::move(camos));

        Card info;
        info.kind = Card::Kind::kInfo;
        info.column = 1;
        info.title = "Dein Fortschritt";
        info.text =
            "Die Freischaltung verändert keine Spielstände: Deine echten Herausforderungen zählen "
            "weiter. Schaltest du sie aus, gilt wieder der gespeicherte Stand.";
        cards.push_back(std::move(info));
        break;
      }
      case 3: {
        const std::string badge = in_game() ? "Ab nächstem Start" : "";
        Card shaders;
        shaders.kind = Card::Kind::kToggle;
        shaders.title = "Genaue Shader-Mathematik";
        shaders.badge = badge;
        shaders.text =
            settings_.exact_shaders != start_settings_.exact_shaders
                ? "Beim nächsten Start werden alle Shader neu gebaut (1 bis 3 Minuten)."
                : "Rechnet die Grafik exakt wie vor Build 13 – deutlich langsamer.";
        shaders.value = &settings_.exact_shaders;
        cards.push_back(std::move(shaders));

        Card hash;
        hash.kind = Card::Kind::kToggle;
        hash.title = "Texturen per Prüfsumme weiterverwenden";
        hash.badge = badge;
        hash.text = "Spart Grafikarbeit, stand aber im Verdacht, bunte Texturen zu verursachen.";
        hash.value = &settings_.texture_hash_reuse;
        cards.push_back(std::move(hash));

        Card lock;
        lock.kind = Card::Kind::kToggle;
        lock.column = 1;
        lock.title = "Alte globale Sperre";
        lock.badge = badge;
        lock.text = "Die Sperre im Spielcode von vor Build 18. Viel langsamer, nur zum Vergleichen.";
        lock.value = &settings_.guest_global_lock;
        cards.push_back(std::move(lock));

        Card perf;
        perf.kind = Card::Kind::kToggle;
        perf.column = 1;
        perf.title = "Leistungstest";
        perf.badge = badge;
        perf.text =
            "Zeichnet jedes Bild im Ordner „Daten/Leistung“ auf und vergleicht kurz nach "
            "Spielbeginn etwa eine Minute lang die beiden Schalter darunter (an gegen aus).";
        perf.value = &settings_.performance_test;
        cards.push_back(std::move(perf));

        Card hoist;
        hoist.kind = Card::Kind::kToggle;
        hoist.title = "Speicher-Uploads vorziehen";
        hoist.badge = "Sofort";
        hoist.badge_color = kGood;
        hoist.text =
            "Entlastet die Grafik: Daten werden vor dem Bild hochgeladen, statt das Zeichnen "
            "dutzende Male zu unterbrechen. Ausschalten, falls Figuren, Effekte oder Schrift "
            "flackern oder falsch aussehen.";
        hoist.value = &settings_.hoist_uploads;
        cards.push_back(std::move(hoist));

        Card jobs;
        jobs.kind = Card::Kind::kToggle;
        jobs.column = 1;
        jobs.title = "Job-Threads schonen";
        jobs.badge = "Black Ops · sofort";
        jobs.badge_color = kGood;
        jobs.text =
            "Zwei Hilfs-Threads des Spiels schlafen, solange nichts zu tun ist, statt zwei "
            "Prozessorkerne dauerhaft voll zu belasten. Ausschalten, falls das Spiel damit "
            "hängt oder langsamer läuft.";
        jobs.value = &settings_.relax_job_threads;
        cards.push_back(std::move(jobs));

        if (in_game()) {
          Card capture;
          capture.kind = Card::Kind::kButtons;
          capture.title = "Bild für die Fehlersuche aufzeichnen";
          capture.text =
              capture_count_ > 0
                  ? "Aufgezeichnet: Daten/bilddaten_…_manuell_" + std::to_string(capture_count_) +
                        ".txt. Diese Datei bitte zusammen mit einem Bildschirmfoto schicken."
                  : "Bei einem Grafikfehler: so hinstellen, dass der Fehler zu sehen ist, dann "
                    "hier drücken. Alle Zeichenbefehle des nächsten Bildes landen in "
                    "Daten/bilddaten_…_manuell_1.txt (kurzer Ruckler).";
          capture.buttons = {"Jetzt aufzeichnen"};
          capture.on_change = [this](int) {
            ++capture_count_;
            rex::cvar::SetFlagByName("gpu_frame_dump_request", "true");
            REXLOG_WARN("Bilddaten aufzeichnen (Einstellungen), Nr. {}", capture_count_);
          };
          cards.push_back(std::move(capture));
        }
        break;
      }
    }
    return cards;
  }

  // --- The panel ---

  struct Layout {
    ImVec2 pos;
    float width = 0.0f;
    float height = 0.0f;
    float description_width = 0.0f;
    std::vector<int> focus_ids;  // focus indices of the card's controls
  };

  float DescriptionWidth(const Card& card, float card_width) const {
    return card_width - 2.0f * kCardPadding -
           (card.kind == Card::Kind::kToggle ? kToggleWidth + 16.0f : 0.0f);
  }

  float ButtonsHeight(const Card& card, float inner_width) const {
    if (card.buttons.empty()) {
      return 0.0f;
    }
    // Side by side when they fit, else one per row.
    float total = 0.0f;
    for (const std::string& label : card.buttons) {
      total += Measure(fonts().button, label.c_str()).x + 40.0f + 10.0f;
    }
    const int rows = total <= inner_width ? 1 : static_cast<int>(card.buttons.size());
    return rows * kControlHeight + (rows - 1) * 10.0f;
  }

  float CardHeight(const Card& card, float width) const {
    const float inner = width - 2.0f * kCardPadding;
    float h = kCardPadding + SizeOf(fonts().button) + 8.0f;
    if (!card.text.empty()) {
      h += Measure(fonts().small, card.text.c_str(), DescriptionWidth(card, width)).y;
    }
    switch (card.kind) {
      case Card::Kind::kSegments:
      case Card::Kind::kText:
        h += 14.0f + kControlHeight;
        break;
      case Card::Kind::kSlider:
        h += 14.0f + kControlHeight;
        break;
      case Card::Kind::kButtons:
        h += 14.0f + ButtonsHeight(card, inner);
        break;
      default:
        break;
    }
    return h + kCardPadding;
  }

  void DrawPanel(ImGuiIO& io) {
    const ImVec2 display = io.DisplaySize;
    HandleController();

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoFocusOnAppearing;
    bool changed = false;
    if (ImGui::Begin("##cod_settings", nullptr, kFlags)) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      // Dim whatever is behind (the game or the start screen).
      dl->AddRectFilled(ImVec2(0.0f, 0.0f), display, IM_COL32(0, 0, 0, in_game() ? 150 : 90));
      const float margin = std::clamp(display.x * 0.025f, 16.0f, 34.0f);
      const ImVec2 a(margin, margin);
      const ImVec2 b(display.x - margin, display.y - margin);
      dl->AddRectFilled(a, b, Rgb(13, 16, 22), 18.0f);
      dl->PushClipRect(a, b, true);
      DrawRadialGlow(dl, ImVec2(b.x - (b.x - a.x) * 0.12f, a.y), (b.y - a.y) * 0.9f,
                     WithAlpha(ctx_.accent, 0.20f));
      dl->PopClipRect();
      dl->AddRect(a, b, kPanelLine, 18.0f, 0, 1.0f);

      changed |= DrawHeader(dl, a, b);
      const float tabs_y = a.y + 108.0f;
      DrawTabs(dl, a, b, tabs_y);
      const float content_top = tabs_y + 64.0f;
      const float footer_y = b.y - 52.0f;
      changed |= DrawCards(dl, ImVec2(a.x + 30.0f, content_top), ImVec2(b.x - 30.0f, footer_y - 12.0f));
      DrawFooter(dl, a, b, footer_y);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (changed) {
      Save();
    }
  }

  bool DrawHeader(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const float x = a.x + 32.0f;
    float y = a.y + 24.0f;
    Text(dl, fonts().heading, ImVec2(x, y), kTextPrimary, "EINSTELLUNGEN");
    y += SizeOf(fonts().heading) + 2.0f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 64.0f, y + 4.0f), ctx_.accent, 2.0f);
    y += 12.0f;
    const std::string where = in_game() ? ctx_.game_name + " · " + ctx_.mode_name
                                        : std::string("Hauptmenü · gilt für alle Spiele");
    Text(dl, fonts().small, ImVec2(x, y), kTextSecondary, where.c_str());

    // Close button.
    const float r = 22.0f;
    const ImVec2 c(b.x - 32.0f - r, a.y + 24.0f + r);
    ImGui::SetCursorScreenPos(ImVec2(c.x - r - 6.0f, c.y - r - 6.0f));
    const bool clicked = ImGui::InvisibleButton("##close", ImVec2(2.0f * r + 12.0f, 2.0f * r + 12.0f));
    const bool hovered = ImGui::IsItemHovered();
    dl->AddCircleFilled(c, r, hovered ? kPanelHover : Rgb(24, 29, 40), 32);
    dl->AddCircle(c, r, kPanelLine, 32, 1.0f);
    DrawCross(dl, c, 7.0f, kTextPrimary, 2.2f);
    DrawBadge(dl, ImVec2(c.x - r - 40.0f, c.y - (SizeOf(fonts().small) + 6.0f) * 0.5f), "B", kTextMuted);
    if (clicked) {
      Close();
    }
    return false;
  }

  void DrawTabs(ImDrawList* dl, ImVec2 a, ImVec2 b, float y) {
    ImFont* font = fonts().label;
    float x = a.x + 32.0f;
    const float badge_width = DrawBadge(dl, ImVec2(x, y + 11.0f), "LB", kTextMuted);
    x += badge_width + 14.0f;
    for (int i = 0; i < kTabCount; ++i) {
      const ImVec2 size = Measure(font, kTabNames[i]);
      const ImVec2 ta(x, y);
      const ImVec2 tb(x + size.x + 36.0f, y + 46.0f);
      ImGui::SetCursorScreenPos(ta);
      ImGui::PushID(i);
      if (ImGui::InvisibleButton("##tab", ImVec2(tb.x - ta.x, tb.y - ta.y))) {
        SelectTab(i);
        controller_ = false;
      }
      const bool hovered = ImGui::IsItemHovered();
      ImGui::PopID();
      const bool selected = i == tab_;
      if (selected) {
        dl->AddRectFilled(ta, tb, IM_COL32(255, 255, 255, 14), 10.0f);
        dl->AddRectFilled(ImVec2(ta.x + 14.0f, tb.y - 4.0f), ImVec2(tb.x - 14.0f, tb.y), ctx_.accent,
                          2.0f);
      } else if (hovered) {
        dl->AddRectFilled(ta, tb, IM_COL32(255, 255, 255, 8), 10.0f);
      }
      Text(dl, font, ImVec2(ta.x + 18.0f, ta.y + (46.0f - size.y) * 0.5f - 1.0f),
           selected ? kTextPrimary : kTextSecondary, kTabNames[i]);
      x = tb.x + 6.0f;
    }
    DrawBadge(dl, ImVec2(x + 8.0f, y + 11.0f), "RB", kTextMuted);
    dl->AddLine(ImVec2(a.x + 30.0f, y + 56.0f), ImVec2(b.x - 30.0f, y + 56.0f), kPanelLine, 1.0f);
  }

  bool DrawCards(ImDrawList* dl, ImVec2 top_left, ImVec2 bottom_right) {
    std::vector<Card> cards = BuildCards(tab_);
    const float gap = 20.0f;
    const float column_width = (bottom_right.x - top_left.x - gap) * 0.5f;
    // Layout: each card in its column, top to bottom; focus order is column
    // by column, as read.
    std::vector<Layout> layouts(cards.size());
    float column_y[2] = {top_left.y, top_left.y};
    int focus_count = 0;
    for (int column = 0; column < 2; ++column) {
      for (size_t i = 0; i < cards.size(); ++i) {
        if (cards[i].column != column) {
          continue;
        }
        Layout& layout = layouts[i];
        layout.width = column_width;
        layout.pos = ImVec2(top_left.x + column * (column_width + gap), column_y[column]);
        layout.height = CardHeight(cards[i], column_width);
        layout.description_width = DescriptionWidth(cards[i], column_width);
        column_y[column] += layout.height + 16.0f;
        int controls = 0;
        switch (cards[i].kind) {
          case Card::Kind::kInfo:
            break;
          case Card::Kind::kButtons:
            controls = static_cast<int>(cards[i].buttons.size());
            break;
          case Card::Kind::kSlider:
            controls = 2;  // slider, reset
            break;
          default:
            controls = 1;
        }
        for (int c = 0; c < controls; ++c) {
          layout.focus_ids.push_back(focus_count++);
        }
      }
    }
    focus_count_ = focus_count;
    focus_ = focus_count > 0 ? std::clamp(focus_, 0, focus_count - 1) : 0;

    bool changed = false;
    for (size_t i = 0; i < cards.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      changed |= DrawCard(dl, cards[i], layouts[i]);
      ImGui::PopID();
    }
    pending_action_ = PendingAction::kNone;
    pending_step_ = 0;
    return changed;
  }

  bool Focused(int id) const { return controller_ && id == focus_; }

  bool DrawCard(ImDrawList* dl, Card& card, const Layout& layout) {
    const ImVec2 a = layout.pos;
    const ImVec2 b(a.x + layout.width, a.y + layout.height);
    bool focused_card = false;
    for (int id : layout.focus_ids) {
      focused_card |= Focused(id);
    }
    bool changed = false;
    // Toggles: the whole card switches.
    bool card_hovered = false;
    if (card.kind == Card::Kind::kToggle) {
      ImGui::SetCursorScreenPos(a);
      if (ImGui::InvisibleButton("##card", ImVec2(layout.width, layout.height))) {
        *card.value = !*card.value;
        changed = true;
        controller_ = false;
      }
      card_hovered = ImGui::IsItemHovered();
      if (Focused(layout.focus_ids[0]) && pending_action_ == PendingAction::kAccept) {
        *card.value = !*card.value;
        changed = true;
      }
    }
    dl->AddRectFilled(a, b, card_hovered ? Rgb(30, 36, 49, 240) : Rgb(22, 27, 37, 235), 14.0f);
    dl->AddRect(a, b, focused_card ? ctx_.accent : kPanelLine, 14.0f, 0, focused_card ? 2.0f : 1.0f);

    const float x = a.x + kCardPadding;
    float y = a.y + kCardPadding;
    const float inner = layout.width - 2.0f * kCardPadding;
    Text(dl, fonts().button, ImVec2(x, y), kTextPrimary, card.title.c_str());
    if (!card.badge.empty()) {
      const float title_width = Measure(fonts().button, card.title.c_str()).x;
      const ImVec2 size = Measure(fonts().small, card.badge.c_str());
      const float right_limit =
          b.x - kCardPadding - (card.kind == Card::Kind::kToggle ? kToggleWidth + 16.0f : 0.0f);
      const float bx = x + title_width + 12.0f;
      if (bx + size.x + 16.0f <= right_limit) {
        const ImVec2 ba(bx, y + (SizeOf(fonts().button) - size.y - 6.0f) * 0.5f + 1.0f);
        const ImVec2 bb(ba.x + size.x + 16.0f, ba.y + size.y + 6.0f);
        dl->AddRectFilled(ba, bb, WithAlpha(card.badge_color, 0.12f), 5.0f);
        Text(dl, fonts().small, ImVec2(ba.x + 8.0f, ba.y + 3.0f), card.badge_color,
             card.badge.c_str());
      }
    }
    if (card.kind == Card::Kind::kToggle) {
      DrawToggle(dl, ImVec2(b.x - kCardPadding - kToggleWidth, y - 3.0f), *card.value);
    }
    y += SizeOf(fonts().button) + 8.0f;
    if (!card.text.empty()) {
      Text(dl, fonts().small, ImVec2(x, y), kTextSecondary, card.text.c_str(),
           layout.description_width);
      y += Measure(fonts().small, card.text.c_str(), layout.description_width).y;
    }
    y += 14.0f;

    switch (card.kind) {
      case Card::Kind::kToggle:
        if (changed && card.on_change) {
          card.on_change(*card.value ? 1 : 0);
        }
        if (changed && (card.value == &settings_.unlock_all_camos ||
                        card.value == &settings_.hoist_uploads ||
                        card.value == &settings_.relax_job_threads)) {
          ApplyLive();
        }
        break;
      case Card::Kind::kSegments:
        changed |= DrawSegments(dl, card, ImVec2(x, y), inner, layout.focus_ids[0]);
        break;
      case Card::Kind::kSlider:
        changed |= DrawSlider(dl, card, ImVec2(x, y), inner, layout.focus_ids[0], layout.focus_ids[1]);
        break;
      case Card::Kind::kText:
        changed |= DrawNameField(dl, ImVec2(x, y), inner, layout.focus_ids[0]);
        break;
      case Card::Kind::kButtons:
        DrawButtons(dl, card, ImVec2(x, y), inner, layout.focus_ids);
        break;
      case Card::Kind::kInfo:
        break;
    }
    return changed;
  }

  void DrawToggle(ImDrawList* dl, ImVec2 pos, bool on) {
    const ImVec2 b(pos.x + kToggleWidth, pos.y + kToggleHeight);
    const float r = kToggleHeight * 0.5f;
    dl->AddRectFilled(pos, b, on ? ctx_.accent : Rgb(52, 59, 74), r);
    const float knob_x = on ? b.x - r : pos.x + r;
    dl->AddCircleFilled(ImVec2(knob_x, pos.y + r), r - 4.0f, kTextPrimary, 24);
    if (on) {
      DrawCheck(dl, ImVec2(knob_x, pos.y + r), 4.5f, ctx_.accent, 2.0f);
    }
  }

  bool DrawSegments(ImDrawList* dl, Card& card, ImVec2 pos, float width, int focus_id) {
    const int count = static_cast<int>(card.options.size());
    const ImVec2 b(pos.x + width, pos.y + kControlHeight);
    dl->AddRectFilled(pos, b, Rgb(12, 15, 21), 10.0f);
    dl->AddRect(pos, b, Focused(focus_id) ? ctx_.accent : kPanelLine, 10.0f, 0,
                Focused(focus_id) ? 2.0f : 1.0f);
    const float segment = width / count;
    int chosen = -1;
    for (int i = 0; i < count; ++i) {
      const ImVec2 sa(pos.x + i * segment, pos.y);
      const ImVec2 sb(sa.x + segment, b.y);
      ImGui::SetCursorScreenPos(sa);
      ImGui::PushID(i);
      if (ImGui::InvisibleButton("##segment", ImVec2(segment, kControlHeight))) {
        chosen = i;
        controller_ = false;
      }
      ImGui::PopID();
      const bool selected = i == card.selected;
      if (selected) {
        dl->AddRectFilled(ImVec2(sa.x + 4.0f, sa.y + 4.0f), ImVec2(sb.x - 4.0f, sb.y - 4.0f),
                          WithAlpha(ctx_.accent, 0.9f), 7.0f);
      }
      const ImVec2 size = Measure(fonts().button, card.options[i].c_str());
      Text(dl, fonts().button,
           ImVec2(sa.x + (segment - size.x) * 0.5f, sa.y + (kControlHeight - size.y) * 0.5f),
           selected ? Rgb(16, 18, 24) : kTextSecondary, card.options[i].c_str());
    }
    if (Focused(focus_id)) {
      if (pending_action_ == PendingAction::kStep) {
        chosen = std::clamp(card.selected + pending_step_, 0, count - 1);
      } else if (pending_action_ == PendingAction::kAccept) {
        chosen = (card.selected + 1) % count;
      }
    }
    if (chosen >= 0 && chosen != card.selected) {
      card.selected = chosen;
      if (card.on_change) {
        card.on_change(chosen);
      }
      return true;
    }
    return false;
  }

  bool DrawSlider(ImDrawList* dl, Card& card, ImVec2 pos, float width, int focus_slider,
                  int focus_reset) {
    // Value, track, reset button in one row.
    const std::string reset_label = card.buttons.empty() ? std::string() : card.buttons[0];
    const float reset_width =
        reset_label.empty() ? 0.0f : Measure(fonts().button, reset_label.c_str()).x + 36.0f;
    char value_text[32];
    std::snprintf(value_text, sizeof(value_text), card.format, *card.number);
    const float value_width = 64.0f;
    const float track_x0 = pos.x + value_width + 8.0f;
    const float track_x1 = pos.x + width - (reset_width > 0.0f ? reset_width + 14.0f : 0.0f) - 12.0f;
    const float cy = pos.y + kControlHeight * 0.5f;
    const ImVec2 value_size = Measure(fonts().heading, value_text);
    Text(dl, fonts().heading, ImVec2(pos.x, cy - value_size.y * 0.5f),
         Focused(focus_slider) ? ctx_.accent : kTextPrimary, value_text);

    int value = *card.number;
    ImGui::SetCursorScreenPos(ImVec2(track_x0 - 12.0f, pos.y));
    ImGui::InvisibleButton("##slider", ImVec2(track_x1 - track_x0 + 24.0f, kControlHeight));
    if (ImGui::IsItemActive()) {
      const float t =
          std::clamp((ImGui::GetIO().MousePos.x - track_x0) / std::max(1.0f, track_x1 - track_x0),
                     0.0f, 1.0f);
      value = card.min + static_cast<int>(std::lround(t * (card.max - card.min)));
      controller_ = false;
    }
    if (Focused(focus_slider) && pending_action_ == PendingAction::kStep) {
      value = std::clamp(value + pending_step_, card.min, card.max);
    }
    const float t = float(value - card.min) / float(std::max(1, card.max - card.min));
    const float knob_x = track_x0 + t * (track_x1 - track_x0);
    dl->AddRectFilled(ImVec2(track_x0, cy - 3.0f), ImVec2(track_x1, cy + 3.0f), Rgb(52, 59, 74), 3.0f);
    dl->AddRectFilled(ImVec2(track_x0, cy - 3.0f), ImVec2(knob_x, cy + 3.0f), ctx_.accent, 3.0f);
    dl->AddCircleFilled(ImVec2(knob_x, cy), 11.0f, kTextPrimary, 24);
    if (Focused(focus_slider)) {
      dl->AddCircle(ImVec2(knob_x, cy), 15.0f, ctx_.accent, 24, 2.0f);
    }

    bool reset = false;
    if (!reset_label.empty()) {
      const ImVec2 ra(pos.x + width - reset_width, pos.y);
      reset = DrawButton(dl, ra, ImVec2(reset_width, kControlHeight), reset_label.c_str(),
                         Focused(focus_reset), false);
      if (Focused(focus_reset) && pending_action_ == PendingAction::kAccept) {
        reset = true;
      }
    }
    if (reset) {
      value = card.reset;
    }
    const bool released = ImGui::IsItemDeactivated();
    (void)released;
    if (value != *card.number) {
      *card.number = value;
      ApplyLive();
      // Saved when the finger lets go (and for controller steps at once).
      slider_dirty_ = true;
    }
    if (slider_dirty_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      slider_dirty_ = false;
      return true;
    }
    return false;
  }

  bool DrawNameField(ImDrawList* dl, ImVec2 pos, float width, int focus_id) {
    (void)dl;
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(14.0f, (kControlHeight - SizeOf(fonts().body)) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, Focused(focus_id) ? 2.0f : 1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Rgb(12, 15, 21));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Rgb(16, 20, 28));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Rgb(16, 20, 28));
    ImGui::PushStyleColor(ImGuiCol_Border, Focused(focus_id) ? ctx_.accent : kPanelLine);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextPrimary);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, WithAlpha(ctx_.accent, 0.4f));
    if (Focused(focus_id) && pending_action_ == PendingAction::kAccept) {
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(width);
    ImGui::InputText("##name", name_buffer_, sizeof(name_buffer_));
    bool changed = false;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      settings_.player_name = SanitizeName(name_buffer_);
      std::snprintf(name_buffer_, sizeof(name_buffer_), "%s", settings_.player_name.c_str());
      changed = true;
    }
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(3);
    return changed;
  }

  void DrawButtons(ImDrawList* dl, Card& card, ImVec2 pos, float width,
                   const std::vector<int>& focus_ids) {
    float total = 0.0f;
    std::vector<float> widths;
    for (const std::string& label : card.buttons) {
      widths.push_back(Measure(fonts().button, label.c_str()).x + 40.0f);
      total += widths.back() + 10.0f;
    }
    const bool one_row = total <= width;
    float x = pos.x;
    float y = pos.y;
    for (size_t i = 0; i < card.buttons.size(); ++i) {
      const float w = one_row ? widths[i] : width;
      ImGui::PushID(static_cast<int>(i));
      bool pressed = DrawButton(dl, ImVec2(x, y), ImVec2(w, kControlHeight), card.buttons[i].c_str(),
                                Focused(focus_ids[i]), i == 0);
      ImGui::PopID();
      if (Focused(focus_ids[i]) && pending_action_ == PendingAction::kAccept) {
        pressed = true;
      }
      if (pressed && card.on_change) {
        card.on_change(static_cast<int>(i));
      }
      if (one_row) {
        x += w + 10.0f;
      } else {
        y += kControlHeight + 10.0f;
      }
    }
  }

  bool DrawButton(ImDrawList* dl, ImVec2 pos, ImVec2 size, const char* label, bool focused,
                  bool primary) {
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton("##button", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    const ImU32 fill = primary ? WithAlpha(ctx_.accent, active ? 1.0f : hovered ? 0.95f : 0.85f)
                               : (hovered || active ? kPanelHover : Rgb(30, 36, 49));
    dl->AddRectFilled(pos, b, fill, 10.0f);
    dl->AddRect(pos, b, focused ? kTextPrimary : (primary ? ctx_.accent : kPanelLine), 10.0f, 0,
                focused ? 2.0f : 1.0f);
    const ImVec2 text_size = Measure(fonts().button, label);
    Text(dl, fonts().button,
         ImVec2(pos.x + (size.x - text_size.x) * 0.5f, pos.y + (size.y - text_size.y) * 0.5f),
         primary ? Rgb(16, 18, 24) : kTextPrimary, label);
    if (clicked) {
      controller_ = false;
    }
    return clicked;
  }

  void DrawFooter(ImDrawList* dl, ImVec2 a, ImVec2 b, float y) {
    dl->AddLine(ImVec2(a.x + 30.0f, y), ImVec2(b.x - 30.0f, y), kPanelLine, 1.0f);
    const float hy = y + 14.0f;
    float x = a.x + 32.0f;
    x = DrawHint(dl, x, hy, "LB", "", kTextMuted) - 22.0f + 4.0f;
    x = DrawHint(dl, x, hy, "RB", "Reiter", kTextMuted);
    x = DrawHint(dl, x, hy, "A", "Ändern", kTextMuted);
    DrawHint(dl, x, hy, "B", "Schließen", kTextMuted);
    const char* note = "Änderungen werden sofort gespeichert";
    const ImVec2 size = Measure(fonts().small, note);
    Text(dl, fonts().small, ImVec2(b.x - 32.0f - size.x, hy + 3.0f), kTextMuted, note);
  }

  // --- Controller ---

  enum class PendingAction { kNone, kAccept, kStep };

  void HandleController() {
    if (!pad_) {
      // Made when the panel is first shown: a reader made with the panel
      // (before the game's input driver starts) would keep the controller
      // hidden from the game.
      pad_ = std::make_unique<PadReader>();
    }
    const PadInput input = pad_->Poll();
    if (just_opened_) {
      // The press that opened the panel is not meant for it.
      just_opened_ = false;
      return;
    }
    if (!input.any) {
      return;
    }
    controller_ = true;
    if (input.back) {
      Close();
      return;
    }
    if (input.prev_tab) {
      SelectTab((tab_ + kTabCount - 1) % kTabCount);
    } else if (input.next_tab) {
      SelectTab((tab_ + 1) % kTabCount);
    } else if (input.up) {
      focus_ = std::max(0, focus_ - 1);
    } else if (input.down) {
      focus_ = std::min(std::max(0, focus_count_ - 1), focus_ + 1);
    } else if (input.left || input.right) {
      pending_action_ = PendingAction::kStep;
      pending_step_ = input.left ? -1 : 1;
    } else if (input.accept) {
      pending_action_ = PendingAction::kAccept;
    }
  }

  void SelectTab(int tab) {
    if (tab != tab_) {
      tab_ = tab;
      focus_ = 0;
    }
  }

  void PollControllerShortcut() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    bool both_sticks = false;
    for (int i = 0; ids && i < count; ++i) {
      SDL_Gamepad* gamepad = SDL_GetGamepadFromID(ids[i]);
      if (gamepad && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK) &&
          SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
        both_sticks = true;
      }
    }
    SDL_free(ids);
    if (both_sticks && !both_sticks_last_) {
      if (IsOpen()) {
        Close();
      } else {
        Open();
      }
    }
    both_sticks_last_ = both_sticks;
  }

  void ApplyLive() {
    if (ctx_.apply_live) {
      ctx_.apply_live(settings_);
    }
  }

  void Save() { SaveUserSettings(ctx_.data_dir, settings_); }

  SettingsContext ctx_;
  UserSettings settings_;
  UserSettings start_settings_;
  char name_buffer_[16] = {};
  bool resolution_toggle_ = false;
  int capture_count_ = 0;
  static constexpr std::chrono::seconds kButtonVisibleTime{3};

  std::atomic<bool> open_{false};
  bool just_opened_ = false;
  bool both_sticks_last_ = false;
  Clock::time_point last_touch_;
  std::unique_ptr<PadReader> pad_;
  int tab_ = 0;
  int focus_ = 0;
  int focus_count_ = 0;
  bool controller_ = false;
  PendingAction pending_action_ = PendingAction::kNone;
  int pending_step_ = 0;
  bool slider_dirty_ = false;
};

}  // namespace

UserSettings LoadUserSettings(const fs::path& data_dir) {
  UserSettings settings;
  std::ifstream in(data_dir / kSettingsFile);
  std::string line;
  while (std::getline(in, line)) {
    const size_t hash = line.find('#');
    if (hash != std::string::npos) {
      line.resize(hash);
    }
    const size_t equals = line.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    const std::string key = Trim(line.substr(0, equals));
    std::string value = Trim(line.substr(equals + 1));
    value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
    // bildformat, kantenglaettung and hochrechnen (builds 17 to 22) are gone:
    // the picture is always the standard one.
    if (key == "aufloesung") {
      settings.resolution_scale = value == "2" ? 2 : 1;
    } else if (key == "leistungsanzeige") {
      settings.overlay = !IsOff(value);
    } else if (key == "sichtfeld") {
      settings.fov = std::clamp(std::atoi(value.c_str()), kFovConsole, kFovMax);
    } else if (key == "genaue_shader") {
      settings.exact_shaders = IsOn(value);
    } else if (key == "bildtakt") {
      settings.frame_rate = value == "60" ? 60 : value == "120" ? 120 : 0;
    } else if (key == "spielername") {
      settings.player_name = SanitizeName(value);
    } else if (key == "globale_sperre") {
      settings.guest_global_lock = IsOn(value);
    } else if (key == "textur_pruefsumme") {
      settings.texture_hash_reuse = IsOn(value);
    } else if (key == "leistungstest") {
      settings.performance_test = !IsOff(value);
    } else if (key == "tarnungen_freischalten") {
      settings.unlock_all_camos = !IsOff(value);
    } else if (key == "uploads_vorziehen") {
      settings.hoist_uploads = !IsOff(value);
    } else if (key == "job_threads_schonen") {
      settings.relax_job_threads = !IsOff(value);
    }
  }
  return settings;
}

void SaveUserSettings(const fs::path& data_dir, const UserSettings& settings) {
  std::ofstream out(data_dir / kSettingsFile, std::ios::trunc);
  out << "# Call of Duty iPad - Einstellungen (werden in der App unter \"Einstellungen\" geaendert)\n"
      << "version = " << kSettingsVersion << "\n"
      << "# aufloesung: 1 (wie Xbox) oder 2 (doppelt, Test)\n"
      << "aufloesung = " << settings.resolution_scale << "\n"
      << "leistungsanzeige = " << (settings.overlay ? "an" : "aus") << "\n"
      << "# sichtfeld: 65 (wie Xbox) bis 110 (Black Ops II)\n"
      << "sichtfeld = " << settings.fov << "\n"
      << "# bildtakt: auto (120 FPS, wenn es fluessig geht, sonst gleichmaessige 60),\n"
      << "# 60 oder 120\n"
      << "bildtakt = " << FramePacingName(settings.frame_rate) << "\n"
      << "spielername = " << settings.player_name << "\n"
      << "# Black Ops II: alle Tarnungen und Visiere waehlbar\n"
      << "tarnungen_freischalten = " << (settings.unlock_all_camos ? "an" : "aus") << "\n"
      << "# Fehlersuche\n"
      << "genaue_shader = " << (settings.exact_shaders ? "an" : "aus") << "\n"
      << "textur_pruefsumme = " << (settings.texture_hash_reuse ? "an" : "aus") << "\n"
      << "globale_sperre = " << (settings.guest_global_lock ? "an" : "aus") << "\n"
      << "uploads_vorziehen = " << (settings.hoist_uploads ? "an" : "aus") << "\n"
      << "job_threads_schonen = " << (settings.relax_job_threads ? "an" : "aus") << "\n"
      << "# leistungstest: jedes Bild nach Daten/Leistung aufzeichnen (ab naechstem Start)\n"
      << "leistungstest = " << (settings.performance_test ? "an" : "aus") << "\n";
}

void ApplyRendererSwitches(const UserSettings& settings) {
  rex::cvar::SetFlagByName("vulkan_hoist_uploads", settings.hoist_uploads ? "true" : "false");
  rex::cvar::SetFlagByName("cod_relax_job_threads", settings.relax_job_threads ? "true" : "false");
}

void ApplyStandardPicture() {
  rex::cvar::SetFlagByName("video_mode_widescreen", "-1");
  rex::cvar::SetFlagByName("present_display_aspect", "");
  rex::cvar::SetFlagByName("present_stretch", "false");
  rex::cvar::SetFlagByName("swap_post_effect", "none");
  rex::cvar::SetFlagByName("present_effect", "fsr");
  rex::ui::Presenter::RequestGuestOutputPaintConfigReload();
}

void ApplyFramePacing(int frame_rate) {
  rex::cvar::SetFlagByName("frame_pacing",
                           frame_rate == 60 ? "60" : frame_rate == 120 ? "120" : "1");
}

std::unique_ptr<SettingsPanel> CreateSettingsDialog(rex::ui::ImGuiDrawer* drawer,
                                                    SettingsContext context,
                                                    UserSettings settings) {
  return std::make_unique<SettingsDialog>(drawer, std::move(context), std::move(settings));
}

}  // namespace bo2
