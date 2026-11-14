// Call of Duty iPad - Black Ops stream requests (see bo1_streams.h).

#include "bo1_streams.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <dlfcn.h>
#include <unistd.h>

#include <fmt/format.h>

#include <rex/logging.h>

namespace cod {
namespace {

// default.xex of title update 11.
constexpr uint32_t kRequestTable = 0x83282A88;  // 64 requests of 336 bytes
constexpr uint32_t kRequestCount = 64;
constexpr uint32_t kRequestSize = 336;

// Requester callbacks (+288 of a request).
struct Requester {
  uint32_t callback;
  const char* name;
};
constexpr Requester kRequesters[] = {
    {0x82446288, "Sound-Stream"},
    {0x82588BE0, "Textur-Streaming"},
    {0x825ADC90, "Modell-Streaming (xmodel)"},
    {0x8239DAC0, "Datei-Stream (Film/Sonstiges)"},
};

// The patched libcod_bo1_sp.dylib of build 36: the guard in the __TEXT
// header padding, the branch to it at the start of __imp__sub_8231EA20, and
// its record in the __DATA padding (count as a host word, then the five words
// of the job and the 336 bytes of the request as the guest has them).
constexpr uintptr_t kGuardOffset = 0x7000;
constexpr uint32_t kGuardFirstWord = 0xB9400008;  // ldr w8, [x0]
constexpr uintptr_t kHookOffset = 0x880A20;
constexpr uint32_t kHookWord = 0x17DE1978;        // b 0x7000
constexpr uintptr_t kRecordOffset = 0x1B8F000;

// Build 37 (scripts/module_patches/bo1_sp_safe_lzo.py): the stream path calls
// a bounded LZO decompressor (stub at 0x1000) instead of the game's own,
// which ignores the size of the target. Record at 0x1B8F400 (host words):
// errors, last code, source, its length, target, capacity, written,
// request index, pack entry, pack offset, calls, -, 16 bytes of the input.
constexpr uintptr_t kLzoStubOffset = 0x1000;
constexpr uint32_t kLzoStubWord = 0xD000DC62;     // adrp x2, record page
constexpr uintptr_t kLzoCallOffset = 0x163D094;
constexpr uint32_t kLzoCallWord = 0x97A70FDB;     // bl stub
constexpr uintptr_t kLzoRecordOffset = 0x1B8F400;

std::atomic<const uint8_t*> g_guest_base{nullptr};
std::atomic<const uint8_t*> g_record{nullptr};
std::atomic<const uint8_t*> g_lzo_record{nullptr};

inline uint32_t Host32(const uint8_t* bytes, size_t offset) {
  uint32_t value;
  std::memcpy(&value, bytes + offset, sizeof(value));
  return value;
}

const char* LzoError(int32_t code) {
  switch (code) {
    case -4: return "Eingabe zu kurz";
    case -5: return "Ziel zu klein";
    case -6: return "Rueckverweis ausserhalb";
    case -8: return "Eingabe nicht aufgebraucht";
    case -9: return "Adressen ungueltig";
    default: return "Fehler";
  }
}

std::string PackPath(const uint8_t* guest, uint32_t entry);

void LogLzoRecord(const uint8_t* guest, const uint8_t* record, uint32_t errors) {
  const uint32_t pack_entry = Host32(record, 32);
  std::string head;
  for (int i = 0; i < 16; ++i) {
    head += fmt::format("{:02X}", record[48 + i]);
  }
  const uint32_t request = Host32(record, 28);
  REXLOG_WARN(
      "Stream-Block abgewiesen (insgesamt {} von {} entpackten): {} ({}), Paket {} Versatz {:X}, "
      "Anfrage {}, Quelle {:08X} Laenge {:X}, Ziel {:08X} Platz {:X}, geschrieben {:X}, "
      "Anfang {}",
      errors, Host32(record, 40), LzoError(int32_t(Host32(record, 4))), int32_t(Host32(record, 4)),
      PackPath(guest, pack_entry), Host32(record, 36),
      request == 0xFFFFFFFFu ? std::string("?") : std::to_string(request), Host32(record, 8),
      Host32(record, 12), Host32(record, 16), Host32(record, 20), Host32(record, 24), head);
}

inline uint32_t Guest32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

inline uint32_t Raw32(const uint8_t* bytes, size_t offset) {
  uint32_t value;
  std::memcpy(&value, bytes + offset, sizeof(value));
  return __builtin_bswap32(value);
}

const char* RequesterName(uint32_t callback) {
  for (const Requester& requester : kRequesters) {
    if (requester.callback == callback) {
      return requester.name;
    }
  }
  return "unbekannt";
}

// Pack table entry: path at +4.
std::string PackPath(const uint8_t* guest, uint32_t entry) {
  std::string path;
  if (entry < 0x80000000u || entry >= 0x90000000u) {
    return path;
  }
  for (uint32_t i = 0; i < 128; ++i) {
    const char c = char(guest[entry + 4 + i]);
    if (!c) {
      break;
    }
    path += (c >= 32 && c < 127) ? c : '?';
  }
  return path;
}

void LogRecord(const uint8_t* guest, const uint8_t* record, uint32_t count, bool first) {
  const uint8_t* job = record + 8;
  const uint8_t* request = record + 32;
  const uint32_t callback = Raw32(request, 288);
  REXLOG_WARN(
      "Stream ohne Zielpuffer abgefangen (insgesamt {}): {} (Rueckruf {:08X}), Paket {}, "
      "Ziel {:08X} Quelle {:08X} Laenge {:X} gepackt {} | Anfrage {:08X} Zustand {} Versatz {:X} "
      "Gesamt {:X} Groesse {:X} gelesen {:X} Puffer {:08X} Entpackt {:X} Block {}",
      count, RequesterName(callback), callback, PackPath(guest, Raw32(request, 324)),
      Raw32(job, 4), Raw32(job, 0), Raw32(job, 8), Raw32(job, 12), Raw32(job, 16),
      Raw32(request, 296), Raw32(request, 260), Raw32(request, 264), Raw32(request, 268),
      Raw32(request, 272), Raw32(request, 276), Raw32(request, 308), Raw32(request, 328));
  if (first) {
    std::string words;
    for (uint32_t offset = 0; offset < kRequestSize; offset += 4) {
      words += fmt::format("{}{:08X}", offset ? (offset % 64 ? " " : " | ") : "",
                           Raw32(request, offset));
    }
    REXLOG_WARN("Anfrage komplett: {}", words);
  }
}

void CrashWrite(int fd, const char* text) {
  (void)!write(fd, text, std::strlen(text));
}

void CrashHex(int fd, uint32_t value) {
  char text[9];
  for (int i = 7; i >= 0; --i) {
    text[i] = "0123456789ABCDEF"[value & 15];
    value >>= 4;
  }
  text[8] = 0;
  CrashWrite(fd, text);
}

}  // namespace

void StartBo1StreamWatch(const uint8_t* guest_base, const void* module_base) {
  if (!guest_base || g_guest_base.exchange(guest_base)) {
    return;
  }
  const auto* module = static_cast<const uint8_t*>(module_base);
  const uint8_t* record = nullptr;
  if (module) {
    uint32_t guard_word = 0;
    uint32_t hook_word = 0;
    std::memcpy(&guard_word, module + kGuardOffset, 4);
    std::memcpy(&hook_word, module + kHookOffset, 4);
    Dl_info info = {};
    if (guard_word == kGuardFirstWord && hook_word == kHookWord &&
        dladdr(module + kRecordOffset, &info) && info.dli_fbase == module_base) {
      record = module + kRecordOffset;
    }
  }
  const uint8_t* lzo_record = nullptr;
  if (module && record) {
    uint32_t stub_word = 0;
    uint32_t call_word = 0;
    std::memcpy(&stub_word, module + kLzoStubOffset, 4);
    std::memcpy(&call_word, module + kLzoCallOffset, 4);
    if (stub_word == kLzoStubWord && call_word == kLzoCallWord) {
      lzo_record = module + kLzoRecordOffset;
    }
  }
  g_record = record;
  g_lzo_record = lzo_record;
  REXLOG_WARN("Black Ops: Stream-Schutz {}, begrenztes Entpacken {}",
              record ? "aktiv" : "nicht im Spielcode", lzo_record ? "aktiv" : "nicht im Spielcode");
  if (!record) {
    return;
  }
  std::thread([guest_base, record, lzo_record]() {
    uint32_t reported = 0;
    uint32_t lzo_reported = 0;
    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      bool flush = false;
      uint32_t count = 0;
      std::memcpy(&count, record, sizeof(count));
      if (count != reported) {
        // Several in a row: the first, then at most a few lines a second.
        LogRecord(guest_base, record, count, reported == 0);
        reported = count;
        flush = true;
      }
      if (lzo_record) {
        const uint32_t errors = Host32(lzo_record, 0);
        if (errors != lzo_reported) {
          LogLzoRecord(guest_base, lzo_record, errors);
          lzo_reported = errors;
          flush = true;
        }
      }
      if (flush) {
        rex::FlushLogging();
      }
    }
  }).detach();
}

void Bo1StreamCrashDump(int fd) {
  const uint8_t* guest = g_guest_base.load();
  if (!guest) {
    return;
  }
  CrashWrite(fd, "Black Ops Stream-Anfragen (Handle Zustand Versatz Gesamt Groesse gelesen "
                 "Puffer Rueckruf Datei Entpackt Ziel Paket):\n");
  for (uint32_t i = 0; i < kRequestCount; ++i) {
    const uint32_t entry = kRequestTable + i * kRequestSize;
    const uint32_t handle = Guest32(guest, entry);
    if (handle == 0xFFFFFFFFu) {
      continue;
    }
    CrashWrite(fd, "  ");
    for (uint32_t offset : {0u, 296u, 260u, 264u, 268u, 272u, 276u, 288u, 304u, 308u, 312u}) {
      CrashHex(fd, Guest32(guest, entry + offset));
      CrashWrite(fd, " ");
    }
    const uint32_t pack = Guest32(guest, entry + 324);
    if (pack >= 0x80000000u && pack < 0x90000000u) {
      char path[129] = {};
      std::memcpy(path, guest + pack + 4, 128);
      for (char& c : path) {
        if (c && (c < 32 || c > 126)) {
          c = '?';
        }
      }
      CrashWrite(fd, path);
    }
    CrashWrite(fd, "\n");
  }
  if (const uint8_t* record = g_record.load()) {
    uint32_t count = 0;
    std::memcpy(&count, record, sizeof(count));
    CrashWrite(fd, "Abgefangene Streams ohne Zielpuffer: ");
    CrashHex(fd, count);
    CrashWrite(fd, "\n");
  }
  if (const uint8_t* lzo = g_lzo_record.load()) {
    CrashWrite(fd, "Begrenztes Entpacken: Aufrufe ");
    CrashHex(fd, Host32(lzo, 40));
    CrashWrite(fd, ", abgewiesen ");
    CrashHex(fd, Host32(lzo, 0));
    CrashWrite(fd, ", zuletzt Code/Quelle/Laenge/Ziel/Platz/geschrieben/Anfrage/Paket/Versatz ");
    for (size_t offset = 4; offset <= 36; offset += 4) {
      CrashHex(fd, Host32(lzo, offset));
      CrashWrite(fd, " ");
    }
    CrashWrite(fd, "\n");
  }
}

}  // namespace cod
