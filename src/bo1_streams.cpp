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

std::atomic<const uint8_t*> g_guest_base{nullptr};
std::atomic<const uint8_t*> g_record{nullptr};

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
  g_record = record;
  REXLOG_WARN("Black Ops: Stream-Schutz {}", record ? "aktiv (Spielcode mit Build-36-Korrektur)"
                                                     : "nicht im Spielcode");
  if (!record) {
    return;
  }
  std::thread([guest_base, record]() {
    uint32_t reported = 0;
    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      uint32_t count = 0;
      std::memcpy(&count, record, sizeof(count));
      if (count == reported) {
        continue;
      }
      // Several in a row: the first, then at most one line a second.
      LogRecord(guest_base, record, count, reported == 0);
      reported = count;
      rex::FlushLogging();
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
}

}  // namespace cod
