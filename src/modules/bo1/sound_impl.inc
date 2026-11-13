// Black Ops - sound packs of map packs bought in another language.
//
// Included once per Black Ops module with the addresses of its executable:
//   BO1_SND_LOAD    loads a sound pack (path, a, b, -1) into the pack table
//                   slot b * 4 + (b != 2 ? a : 0)
//   BO1_SND_CHECK   registers a sound bank of a zone (fastfile) and checks
//                   every registered bank against the pack it plays from:
//                   "Sound zone %s does not match pack file. Please rebuild
//                   fastfiles." (fatal; on the Xbox it restarts the title)
//   BO1_SND_REMOVE  removes a registered bank again (zone unload)
//   BO1_SND_TABLE   the pack table (entries of 296 bytes: +4 path, +276 the
//                   hash from the pack's header)
//
// A sound bank carries the hash of its pack (+16) and the pack's location
// (+20, the slot b; the check uses a = 3). Build 33 showed what goes wrong
// with English map packs in the German game: the map's language bank
// ("zmb_prototype.english", "zmb_coast.english") plays from slot 1, the
// base game's language pack - it expects snd.english.pak (hash 2E69), and
// the German disc only has snd.german.pak (280A). Taking over the German
// hash (build 33) let the game read the German pack at English offsets and
// the loading screen hung.
//
// Now such a bank is left out: it is not registered (and not removed again
// when the map unloads), so the map loads without the voice lines that only
// exist in the English base pack. Everything else of the map (the .all bank,
// the map pack's own sound packs) is unchanged.

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include <rex/hook.h>
#include <rex/logging.h>

namespace bo1 {
namespace {

inline uint32_t SndLoad32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

inline void SndStore32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

std::string SndString(const uint8_t* base, uint32_t address, size_t max_length = 96) {
  std::string text;
  if (address < 0x10000 || address >= 0xF0000000u) {
    return text;
  }
  for (size_t i = 0; i < max_length; ++i) {
    const char c = char(base[address + i]);
    if (!c) {
      break;
    }
    text += (c >= 32 && c < 127) ? c : '?';
  }
  return text;
}

// Banks left out, so their removal is left out as well.
std::mutex g_skipped_banks_mutex;
std::unordered_set<uint32_t> g_skipped_banks;

inline uint32_t SndSlotEntry(uint32_t a, uint32_t b) {
  return BO1_SND_TABLE + (b * 4 + (b != 2 ? a : 0)) * 296;
}

}  // namespace
}  // namespace bo1

extern "C" void BO1_SND_LOAD_ORIGINAL(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void BO1_SND_CHECK_ORIGINAL(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void BO1_SND_REMOVE_ORIGINAL(PPCContext& __restrict ctx, uint8_t* base);

extern "C" REX_FUNC(BO1_SND_LOAD) {
  const std::string path = bo1::SndString(base, ctx.r3.u32, 256);
  const uint32_t a = ctx.r4.u32;
  const uint32_t b = ctx.r5.u32;
  BO1_SND_LOAD_ORIGINAL(ctx, base);
  if (b < 64) {
    const uint32_t entry = bo1::SndSlotEntry(a, b);
    REXLOG_WARN("Soundpaket {} -> Platz {}/{}, Kennung {:08X}", path, a, b,
                bo1::SndLoad32(base, entry + 276));
  }
}

extern "C" REX_FUNC(BO1_SND_CHECK) {
  const uint32_t bank = ctx.r3.u32;
  if (bank) {
    const std::string name = bo1::SndString(base, bo1::SndLoad32(base, bank));
    const uint32_t expected = bo1::SndLoad32(base, bank + 16);
    const uint32_t location = bo1::SndLoad32(base, bank + 20);
    if (location < 64) {
      const uint32_t entry = bo1::SndSlotEntry(3, location);
      const uint32_t loaded = bo1::SndLoad32(base, entry + 276);
      const std::string pack = bo1::SndString(base, entry + 4, 256);
      if (expected && loaded && expected != loaded) {
        REXLOG_WARN("Soundbank {}: braucht Paket mit Kennung {:08X}, auf Platz {} liegt {} "
                    "({:08X}) - ausgelassen (Sprachaufnahmen fehlen)",
                    name, expected, location, pack, loaded);
        {
          std::lock_guard<std::mutex> lock(bo1::g_skipped_banks_mutex);
          bo1::g_skipped_banks.insert(bank);
        }
        return;
      }
      REXLOG_WARN("Soundbank {}: Platz {}, Paket {} ({:08X})", name, location, pack, loaded);
    }
  }
  BO1_SND_CHECK_ORIGINAL(ctx, base);
}

extern "C" REX_FUNC(BO1_SND_REMOVE) {
  {
    std::lock_guard<std::mutex> lock(bo1::g_skipped_banks_mutex);
    if (bo1::g_skipped_banks.erase(ctx.r3.u32)) {
      return;
    }
  }
  BO1_SND_REMOVE_ORIGINAL(ctx, base);
}
