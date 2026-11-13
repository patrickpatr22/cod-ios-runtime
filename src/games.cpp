// Call of Duty iPad - the games of the app. See games.h.

#include "games.h"

#include <algorithm>
#include <cctype>
#include <system_error>

#include "game_loader.h"

namespace cod {

namespace {

namespace fs = std::filesystem;

constexpr uint32_t Rgb(uint32_t r, uint32_t g, uint32_t b) {
  return (0xFFu << 24) | (b << 16) | (g << 8) | r;
}

std::vector<GameDef> MakeGames() {
  std::vector<GameDef> games;
  games.push_back(GameDef{
      "bo2",
      "Black Ops II",
      "BLACK OPS II",
      "Treyarch · 2012 · Title Update 18",
      "BO2",
      0x415608C3,
      Rgb(255, 138, 31),
      nullptr,
      {
          {"mp", "MEHRSPIELER",
           "Eigene Partien gegen Bots – alle Karten und Modi, auch die DLC-Karten.",
           ModeIcon::kCrosshair, "bo2_mp", "default_mp.xex", false},
          {"zombies", "ZOMBIES",
           "TranZit, Nuketown Zombies und die Karten der Map-Packs – allein gegen die Untoten.",
           ModeIcon::kSkull, "bo2_mp", "default_mp.xex", false},
          {"campaign", "KAMPAGNE", "Noch nicht enthalten.", ModeIcon::kFlag, nullptr,
           "default.xex", false},
      },
      18,
      // Guest boot flags read by the MP executable; bit 4 boots straight
      // into Zombies (Title Update 18; disc version: 0x83663530, from
      // bo2-recompiled).
      0x837F8CB4,
      1u << 4,
  });
  games.push_back(GameDef{
      "bo1",
      "Black Ops",
      "BLACK OPS",
      "Treyarch · 2010 · Title Update 11",
      "BO1",
      0x41560855,
      Rgb(214, 178, 92),
      nullptr,
      {
          {"mp", "MEHRSPIELER",
           "Das Mehrspieler-Menü des Spiels mit allen Karten, auch denen der Map-Packs.",
           ModeIcon::kCrosshair, "bo1_mp", "default_mp.xex", true},
          {"sp", "KAMPAGNE & ZOMBIES",
           "Die Kampagne und im Hauptmenü des Spiels „Zombies“: Kino der Toten, Five und die "
           "Karten der Map-Packs.",
           ModeIcon::kSkull, "bo1_sp", "default.xex", true},
      },
      11,
      0,
      0,
  });
  games.push_back(GameDef{
      "mw3",
      "Modern Warfare 3",
      "MODERN WARFARE 3",
      "Infinity Ward · Sledgehammer · 2011",
      "MW3",
      0x415608CB,
      Rgb(120, 200, 90),
      "Modern Warfare 3 ist als Nächstes dran: Die Disc ist entpackt, der Spielcode wird "
      "noch übersetzt. Kopiere die Spieldateien erst aufs iPad, wenn es hier als spielbar "
      "erscheint.",
      {
          {"mp", "MEHRSPIELER", "Kommt mit einem der nächsten Builds.", ModeIcon::kCrosshair,
           nullptr, "default_mp.xex", false},
          {"sp", "KAMPAGNE & SPEC OPS", "Kommt mit einem der nächsten Builds.", ModeIcon::kFlag,
           nullptr, "default.xex", false},
      },
      0,
      0,
      0,
  });
  games.push_back(GameDef{
      "ghosts",
      "Ghosts",
      "GHOSTS",
      "Infinity Ward · 2013",
      "Ghosts",
      0x415608FC,
      Rgb(160, 175, 190),
      "Geplant. Die erste Disc ist entpackt; Ghosts wird nach Modern Warfare 3 übersetzt.",
      {
          {"mp", "MEHRSPIELER & EXTINCTION", "Geplant.", ModeIcon::kCrosshair, nullptr,
           "default_mp.xex", false},
          {"sp", "KAMPAGNE", "Geplant.", ModeIcon::kFlag, nullptr, "default.xex", false},
      },
      0,
      0,
      0,
  });
  games.push_back(GameDef{
      "aw",
      "Advanced Warfare",
      "ADVANCED WARFARE",
      "Sledgehammer Games · 2014",
      "AW",
      0x41560914,
      Rgb(55, 160, 255),
      "Geplant. Die Spieldisc ist entpackt; Advanced Warfare kommt nach Ghosts.",
      {
          {"mp", "MEHRSPIELER & EXO ZOMBIES", "Geplant.", ModeIcon::kCrosshair, nullptr,
           "default_mp.xex", false},
          {"sp", "KAMPAGNE", "Geplant.", ModeIcon::kFlag, nullptr, "default.xex", false},
      },
      0,
      0,
      0,
  });
  return games;
}

std::string Normalize(std::string text) {
  text.erase(std::remove_if(text.begin(), text.end(),
                            [](unsigned char c) { return std::isspace(c) != 0; }),
             text.end());
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

}  // namespace

const std::vector<GameDef>& Games() {
  static const std::vector<GameDef> games = MakeGames();
  return games;
}

const GameDef* FindGame(std::string_view id) {
  for (const GameDef& game : Games()) {
    if (id == game.id) {
      return &game;
    }
  }
  return nullptr;
}

const ModeDef* FindMode(const GameDef& game, std::string_view mode) {
  for (const ModeDef& entry : game.modes) {
    if (mode == entry.id) {
      return &entry;
    }
  }
  return nullptr;
}

bool ModuleAvailable(const ModeDef& mode) {
  return mode.module && GameModuleFileExists(mode.module);
}

bool GameFilesPresent(const fs::path& documents, const GameDef& game, const ModeDef& mode) {
  std::error_code ec;
  return fs::exists(documents / game.folder / mode.xex, ec);
}

GameAndMode ParseGameAndMode(std::string text) {
  text = Normalize(std::move(text));
  GameAndMode result;
  const size_t colon = text.find(':');
  if (colon == std::string::npos) {
    // Mode files of builds 20 to 24: Black Ops II only.
    result.game = "bo2";
    result.mode = text;
  } else {
    result.game = text.substr(0, colon);
    result.mode = text.substr(colon + 1);
  }
  if (result.mode == "zombie" || result.mode == "zm") {
    result.mode = "zombies";
  } else if (result.mode == "multiplayer") {
    result.mode = "mp";
  }
  const GameDef* game = FindGame(result.game);
  if (!game || !FindMode(*game, result.mode)) {
    return {};
  }
  return result;
}

std::string FormatGameAndMode(const GameAndMode& choice) {
  return choice.game + ":" + choice.mode;
}

}  // namespace cod
