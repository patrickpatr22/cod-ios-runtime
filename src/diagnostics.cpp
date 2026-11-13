// BO2 iOS - on-device performance diagnostics. See diagnostics.h.

#include "diagnostics.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <dlfcn.h>

#include <imgui.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system.h>
#include <rex/system/xam/content_manager.h>
#include <rex/thread.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/windowed_app_context.h>

#include <SDL3/SDL_events.h>
#include <rex/exception_handler.h>

#include <execinfo.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <unordered_map>
#if TARGET_OS_IOS
#include <os/proc.h>
#endif
#endif

#include <rex/cvar.h>

namespace bo2 {

namespace {

std::mutex g_cpu_summary_mutex;
std::string g_cpu_summary;

#if defined(__APPLE__)

// NSProcessInfo.thermalState (0 nominal, 1 fair, 2 serious, 3 critical; -1
// unknown) and whether the low power mode is on, through the Objective-C
// runtime (this file is C++).
int ThermalState(bool* low_power_out) {
  *low_power_out = false;
  Class process_info_class = objc_getClass("NSProcessInfo");
  if (!process_info_class) {
    return -1;
  }
  id process_info = reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend)(
      process_info_class, sel_registerName("processInfo"));
  if (!process_info) {
    return -1;
  }
  const long state = reinterpret_cast<long (*)(id, SEL)>(objc_msgSend)(
      process_info, sel_registerName("thermalState"));
  *low_power_out = reinterpret_cast<bool (*)(id, SEL)>(objc_msgSend)(
      process_info, sel_registerName("isLowPowerModeEnabled"));
  return int(state);
}

const char* ThermalStateName(int state) {
  switch (state) {
    case 0:
      return "normal";
    case 1:
      return "erhoeht";
    case 2:
      return "hoch (iOS drosselt)";
    case 3:
      return "kritisch";
    default:
      return "unbekannt";
  }
}

// Scheduling of a thread: policy, current and base priority, QoS class.
std::string ThreadScheduling(mach_port_t thread) {
  thread_extended_info_data_t info = {};
  mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
  std::string text = "?";
  if (thread_info(thread, THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info), &count) ==
      KERN_SUCCESS) {
    const char* policy = info.pth_policy == POLICY_TIMESHARE ? "TS"
                         : info.pth_policy == POLICY_RR      ? "RR"
                         : info.pth_policy == POLICY_FIFO    ? "FIFO"
                                                             : "?";
    text = fmt::format("{} pri {}/{}", policy, info.pth_curpri, info.pth_priority);
  }
  if (pthread_t pthread = pthread_from_mach_thread_np(thread)) {
    qos_class_t qos = QOS_CLASS_UNSPECIFIED;
    int relative = 0;
    if (pthread_get_qos_class_np(pthread, &qos, &relative) == 0) {
      const char* name = qos == QOS_CLASS_USER_INTERACTIVE ? "interaktiv"
                         : qos == QOS_CLASS_USER_INITIATED ? "initiiert"
                         : qos == QOS_CLASS_DEFAULT        ? "default"
                         : qos == QOS_CLASS_UTILITY        ? "utility"
                         : qos == QOS_CLASS_BACKGROUND     ? "hintergrund"
                                                           : "ohne";
      text += fmt::format(" QoS {}", name);
    }
  }
  return text;
}

// CPU seconds per thread between two calls of Sample().
class CpuSampler {
 public:
  // Returns "CPU 180% (42 Threads): Name 98%, Name 60%, ...", with
  // with_scheduling also policy, priorities and QoS of the listed threads.
  std::string Sample(size_t max_threads, bool with_scheduling = false) {
    const auto now = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(now - last_time_).count();
    last_time_ = now;

    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t thread_count = 0;
    if (task_threads(mach_task_self(), &threads, &thread_count) != KERN_SUCCESS) {
      return {};
    }
    std::map<mach_port_t, double> seconds_now;
    std::vector<std::pair<double, std::string>> usage;
    std::vector<std::pair<double, std::string>> scheduling;
    double total = 0.0;
    for (mach_msg_type_number_t i = 0; i < thread_count; ++i) {
      thread_basic_info_data_t info;
      mach_msg_type_number_t info_count = THREAD_BASIC_INFO_COUNT;
      if (thread_info(threads[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info),
                      &info_count) == KERN_SUCCESS) {
        const double seconds = info.user_time.seconds + info.user_time.microseconds * 1e-6 +
                               info.system_time.seconds + info.system_time.microseconds * 1e-6;
        seconds_now[threads[i]] = seconds;
        auto previous = last_seconds_.find(threads[i]);
        if (previous != last_seconds_.end() && wall > 0.0) {
          const double percent = 100.0 * (seconds - previous->second) / wall;
          total += percent;
          usage.emplace_back(percent, ThreadName(threads[i]));
          if (with_scheduling && percent >= 5.0) {
            scheduling.emplace_back(percent, usage.back().second + " " +
                                                 ThreadScheduling(threads[i]));
          }
        }
      }
      mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                  thread_count * sizeof(thread_act_t));
    last_seconds_.swap(seconds_now);
    if (usage.empty()) {
      return {};
    }

    std::sort(usage.begin(), usage.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string text = fmt::format("CPU {:.0f}% ({} Threads):", total, thread_count);
    for (size_t i = 0; i < usage.size() && i < max_threads && usage[i].first >= 2.0; ++i) {
      text += fmt::format("{} {} {:.0f}%", i ? "," : "", usage[i].second, usage[i].first);
    }
    if (!scheduling.empty()) {
      std::sort(scheduling.begin(), scheduling.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
      text += " | Planung:";
      for (size_t i = 0; i < scheduling.size() && i < 12; ++i) {
        text += fmt::format("{} {}", i ? "," : "", scheduling[i].second);
      }
    }
    return text;
  }

 private:
  static std::string ThreadName(mach_port_t thread) {
    if (pthread_t pthread = pthread_from_mach_thread_np(thread)) {
      std::string name;
      if (rex::thread::LookupThreadName(reinterpret_cast<uintptr_t>(pthread), &name) &&
          !name.empty()) {
        return name;
      }
      char buffer[64] = {};
      if (pthread_getname_np(pthread, buffer, sizeof(buffer)) == 0 && buffer[0]) {
        return buffer;
      }
    }
    return fmt::format("Thread {:X}", thread);
  }

  std::map<mach_port_t, double> last_seconds_;
  std::chrono::steady_clock::time_point last_time_ = std::chrono::steady_clock::now();
};

void CpuMonitorMain();

#if defined(__aarch64__)

// Statistical profiler for the busiest threads: about 100 times per second it
// briefly suspends each of them, reads the program counter and walks the
// frame pointer chain (no allocation or locks while a thread is suspended),
// and every 10 seconds logs which code they spent their time in. Guest code
// is attributed to the first frame in the app binary (the recompiled
// functions); the leaf shows where the time was actually spent (for example a
// kernel wait). Costs well under 1% of one core.
class PcProfiler {
 public:
  void Run() {
    pthread_setname_np("COD Profiler");
    self_ = mach_thread_self();
    Dl_info self_info = {};
    if (dladdr(reinterpret_cast<const void*>(&CpuMonitorMain), &self_info)) {
      app_base_ = reinterpret_cast<uintptr_t>(self_info.dli_fbase);
    }
    auto next_refresh = std::chrono::steady_clock::now();
    auto next_report = next_refresh + std::chrono::seconds(10);
    for (;;) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_refresh) {
        RefreshTargets();
        next_refresh = now + std::chrono::seconds(1);
      }
      for (Target& target : targets_) {
        SampleThread(target);
      }
      if (now >= next_report) {
        Report();
        next_report = now + std::chrono::seconds(10);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

 private:
  static constexpr size_t kMaxTargets = 6;
  static constexpr size_t kMaxFrames = 24;

  struct Target {
    mach_port_t port = MACH_PORT_NULL;
    std::string name;
    double cpu_percent = 0.0;
  };

  struct ThreadProfile {
    std::string name;
    double cpu_percent = 0.0;
    uint32_t samples = 0;
    std::unordered_map<uintptr_t, uint32_t> code;  // first frame in the app binary
    std::unordered_map<uintptr_t, uint32_t> leaf;  // sampled program counter
    // First two frames in the SDK libraries (GPU plugin, runtime, MoltenVK),
    // the innermost one and its caller, e.g. which code waits on a mutex.
    std::unordered_map<uintptr_t, uint32_t> engine;
  };

  struct Symbol {
    uintptr_t start = 0;  // symbol (or the address itself) - aggregation key
    std::string text;
    bool in_app = false;
    bool in_system = false;
    bool in_engine = false;
  };

  static uintptr_t StripPointer(uint64_t address) {
    // Return addresses of arm64e system libraries carry pointer
    // authentication bits above the 47-bit address space.
    return uintptr_t(address & 0x00007FFFFFFFFFFFull);
  }

  void RefreshTargets() {
    for (Target& target : targets_) {
      mach_port_deallocate(mach_task_self(), target.port);
    }
    targets_.clear();

    const auto now = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(now - last_refresh_).count();
    last_refresh_ = now;

    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t thread_count = 0;
    if (task_threads(mach_task_self(), &threads, &thread_count) != KERN_SUCCESS) {
      return;
    }
    std::vector<Target> candidates;
    std::map<uint64_t, double> seconds_now;
    for (mach_msg_type_number_t i = 0; i < thread_count; ++i) {
      thread_identifier_info_data_t id_info;
      mach_msg_type_number_t id_count = THREAD_IDENTIFIER_INFO_COUNT;
      thread_basic_info_data_t info;
      mach_msg_type_number_t info_count = THREAD_BASIC_INFO_COUNT;
      bool keep = false;
      if (threads[i] != self_ &&
          thread_info(threads[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&id_info),
                      &id_count) == KERN_SUCCESS &&
          thread_info(threads[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info),
                      &info_count) == KERN_SUCCESS) {
        const double seconds = info.user_time.seconds + info.user_time.microseconds * 1e-6 +
                               info.system_time.seconds + info.system_time.microseconds * 1e-6;
        seconds_now[id_info.thread_id] = seconds;
        auto previous = last_seconds_.find(id_info.thread_id);
        if (previous != last_seconds_.end() && wall > 0.0) {
          const double percent = 100.0 * (seconds - previous->second) / wall;
          if (percent >= 10.0) {
            Target& target = candidates.emplace_back();
            target.port = threads[i];
            target.cpu_percent = percent;
            keep = true;
          }
        }
      }
      if (!keep) {
        mach_port_deallocate(mach_task_self(), threads[i]);
      }
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                  thread_count * sizeof(thread_act_t));
    last_seconds_.swap(seconds_now);

    std::sort(candidates.begin(), candidates.end(),
              [](const Target& a, const Target& b) { return a.cpu_percent > b.cpu_percent; });
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (i < kMaxTargets) {
        candidates[i].name = ThreadName(candidates[i].port);
        ThreadProfile& profile = profiles_[candidates[i].name];
        profile.name = candidates[i].name;
        profile.cpu_percent = std::max(profile.cpu_percent, candidates[i].cpu_percent);
        targets_.push_back(std::move(candidates[i]));
      } else {
        mach_port_deallocate(mach_task_self(), candidates[i].port);
      }
    }
  }

  static std::string ThreadName(mach_port_t thread) {
    if (pthread_t pthread = pthread_from_mach_thread_np(thread)) {
      std::string name;
      if (rex::thread::LookupThreadName(reinterpret_cast<uintptr_t>(pthread), &name) &&
          !name.empty()) {
        return name;
      }
      char buffer[64] = {};
      if (pthread_getname_np(pthread, buffer, sizeof(buffer)) == 0 && buffer[0]) {
        return buffer;
      }
    }
    return fmt::format("Thread {:X}", thread);
  }

  void SampleThread(const Target& target) {
    uintptr_t frames[kMaxFrames];
    size_t frame_count = 0;
    uintptr_t link = 0;
    pthread_t pthread = pthread_from_mach_thread_np(target.port);
    uintptr_t stack_high = 0, stack_low = 0;
    if (pthread) {
      stack_high = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread));
      stack_low = stack_high - pthread_get_stacksize_np(pthread);
    }
    // Nothing between suspend and resume may allocate or take a lock.
    if (thread_suspend(target.port) != KERN_SUCCESS) {
      return;
    }
    arm_thread_state64_t state;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(target.port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state),
                         &count) == KERN_SUCCESS) {
      frames[frame_count++] = StripPointer(arm_thread_state64_get_pc(state));
      link = StripPointer(arm_thread_state64_get_lr(state));
      uintptr_t fp = StripPointer(arm_thread_state64_get_fp(state));
      while (frame_count < kMaxFrames && stack_high && fp >= stack_low &&
             fp + 16 <= stack_high && !(fp & 0xF)) {
        const uintptr_t* record = reinterpret_cast<const uintptr_t*>(fp);
        const uintptr_t next_fp = StripPointer(record[0]);
        const uintptr_t return_address = StripPointer(record[1]);
        if (!return_address) {
          break;
        }
        frames[frame_count++] = return_address;
        if (next_fp <= fp) {
          break;
        }
        fp = next_fp;
      }
    }
    thread_resume(target.port);
    if (!frame_count) {
      return;
    }

    ThreadProfile& profile = profiles_[target.name];
    ++profile.samples;
    const Symbol& leaf = Lookup(frames[0]);
    ++profile.leaf[leaf.start];
    {
      uintptr_t engine_frames[2] = {};
      size_t engine_count = 0;
      if (!leaf.in_engine && link) {
        const Symbol& caller = Lookup(link);
        if (caller.in_engine) {
          engine_frames[engine_count++] = caller.start;
        }
      }
      for (size_t i = 0; i < frame_count && engine_count < 2; ++i) {
        const Symbol& symbol = Lookup(i ? frames[i] - 1 : frames[i]);
        if (symbol.in_engine &&
            (engine_count == 0 || engine_frames[engine_count - 1] != symbol.start)) {
          engine_frames[engine_count++] = symbol.start;
        }
      }
      if (engine_count) {
        const uintptr_t key =
            engine_frames[0] ^ (engine_frames[1] * UINT64_C(0x9E3779B97F4A7C15));
        ++profile.engine[key];
        if (engine_keys_.find(key) == engine_keys_.end() && engine_keys_.size() < 20000) {
          const auto first = symbols_.find(engine_frames[0]);
          const auto second = symbols_.find(engine_frames[1]);
          std::string text = first != symbols_.end() ? first->second.text : "?";
          if (engine_count > 1 && second != symbols_.end()) {
            text += " < " + second->second.text;
          }
          engine_keys_.emplace(key, std::move(text));
        }
      }
    }
    // The leaf of a system call or a C library routine has no frame record
    // of its own, its caller is only in the link register.
    if (!leaf.in_app && link) {
      const Symbol& caller = Lookup(link);
      if (caller.in_app) {
        ++profile.code[caller.start];
        return;
      }
    }
    for (size_t i = 0; i < frame_count; ++i) {
      const Symbol& symbol = Lookup(i ? frames[i] - 1 : frames[i]);
      if (symbol.in_app) {
        ++profile.code[symbol.start];
        return;
      }
    }
    ++profile.code[0];
  }

  const Symbol& Lookup(uintptr_t address) {
    auto it = symbols_by_address_.find(address);
    if (it != symbols_by_address_.end()) {
      return symbols_.at(it->second);
    }
    Symbol symbol;
    symbol.start = address;
    Dl_info info = {};
    if (dladdr(reinterpret_cast<const void*>(address), &info) && info.dli_fbase) {
      const uintptr_t base = reinterpret_cast<uintptr_t>(info.dli_fbase);
      std::string module = info.dli_fname ? info.dli_fname : "?";
      module = module.substr(module.find_last_of('/') + 1);
      symbol.in_app = base == app_base_;
      symbol.in_system = module.rfind("libsystem_", 0) == 0 || module.rfind("libdyld", 0) == 0;
      symbol.in_engine = module.rfind("librexgpu", 0) == 0 || module.rfind("librexruntime", 0) == 0 ||
                         module.find("MoltenVK") != std::string::npos;
      if (info.dli_saddr) {
        symbol.start = reinterpret_cast<uintptr_t>(info.dli_saddr);
      }
      std::string name = info.dli_sname ? info.dli_sname : "";
      if (name.size() > 70) {
        name.resize(70);
      }
      symbol.text = fmt::format("{}+0x{:X}{}{}", module, symbol.start - base,
                                name.empty() ? "" : " ", name);
    } else {
      symbol.text = fmt::format("0x{:X}", address);
    }
    auto known = symbols_.find(symbol.start);
    if (known == symbols_.end()) {
      known = symbols_.emplace(symbol.start, std::move(symbol)).first;
    }
    symbols_by_address_.emplace(address, known->first);
    return known->second;
  }

  std::string Top(const std::unordered_map<uintptr_t, uint32_t>& counts, uint32_t total,
                  bool use_engine_keys = false) {
    std::vector<std::pair<uint32_t, uintptr_t>> sorted;
    sorted.reserve(counts.size());
    for (const auto& [key, count] : counts) {
      sorted.emplace_back(count, key);
    }
    std::sort(sorted.begin(), sorted.end(), std::greater<>());
    std::string text;
    for (size_t i = 0; i < sorted.size() && i < 6; ++i) {
      const double percent = 100.0 * sorted[i].first / std::max(total, 1u);
      if (percent < 3.0) {
        break;
      }
      const auto symbol = symbols_.find(sorted[i].second);
      const auto engine = engine_keys_.find(sorted[i].second);
      text += fmt::format("{}{:.0f}% {}", i ? ", " : "", percent,
                          sorted[i].second == 0         ? std::string("(ausserhalb App)")
                          : use_engine_keys && engine != engine_keys_.end() ? engine->second
                          : symbol != symbols_.end() ? symbol->second.text
                                                     : fmt::format("0x{:X}", sorted[i].second));
    }
    return text;
  }

  void Report() {
    std::vector<ThreadProfile*> sorted;
    for (auto& [name, profile] : profiles_) {
      if (profile.samples >= 50) {
        sorted.push_back(&profile);
      }
    }
    std::sort(sorted.begin(), sorted.end(), [](const ThreadProfile* a, const ThreadProfile* b) {
      return a->cpu_percent > b->cpu_percent;
    });
    for (const ThreadProfile* profile : sorted) {
      REXLOG_WARN("Profil {} (CPU bis {:.0f}%, {} Proben) | Code: {} | SDK: {} | Blatt: {}",
                  profile->name, profile->cpu_percent, profile->samples,
                  Top(profile->code, profile->samples), Top(profile->engine, profile->samples, true),
                  Top(profile->leaf, profile->samples));
    }
    profiles_.clear();
    // Keep the symbol cache bounded.
    if (symbols_by_address_.size() > 200000) {
      symbols_by_address_.clear();
      symbols_.clear();
      engine_keys_.clear();
    }
  }

  mach_port_t self_ = MACH_PORT_NULL;
  uintptr_t app_base_ = 0;
  std::vector<Target> targets_;
  std::map<uint64_t, double> last_seconds_;
  std::chrono::steady_clock::time_point last_refresh_ = std::chrono::steady_clock::now();
  std::map<std::string, ThreadProfile> profiles_;
  std::unordered_map<uintptr_t, uintptr_t> symbols_by_address_;
  std::unordered_map<uintptr_t, Symbol> symbols_;
  std::unordered_map<uintptr_t, std::string> engine_keys_;
};

void ProfilerMain() {
  static PcProfiler profiler;
  profiler.Run();
}

#endif  // defined(__aarch64__)

// "Speicher 2900 MB (noch 1800 MB frei)": the app's footprint as iOS counts it
// for the memory limit, and how much it may still grow before iOS ends it.
std::string MemoryText() {
  task_vm_info_data_t info = {};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
      KERN_SUCCESS) {
    return {};
  }
  std::string text = fmt::format("Speicher {} MB", info.phys_footprint >> 20);
#if TARGET_OS_IOS
  if (__builtin_available(iOS 13.0, *)) {
    text += fmt::format(" (noch {} MB frei)", os_proc_available_memory() >> 20);
  }
#endif
  return text;
}

void CpuMonitorMain() {
  pthread_setname_np("COD CPU-Monitor");
  CpuSampler overlay_sampler;
  CpuSampler log_sampler;
  int last_thermal = -2;
  bool last_low_power = false;
  for (uint32_t tick = 1;; ++tick) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    // Heat: iOS lowers CPU and GPU clocks from "serious" on.
    bool low_power = false;
    const int thermal = ThermalState(&low_power);
    if (thermal != last_thermal || low_power != last_low_power) {
      rex::cvar::SetFlagByName("perf_thermal_state", std::to_string(thermal));
      rex::cvar::SetFlagByName("perf_low_power_mode", low_power ? "true" : "false");
      REXLOG_WARN("Temperatur: {}{}", ThermalStateName(thermal),
                  low_power ? ", Stromsparmodus an" : "");
      last_thermal = thermal;
      last_low_power = low_power;
    }
    const std::string memory = MemoryText();
    std::string overlay_text = memory + " | Temp " + ThermalStateName(thermal) + " | " +
                               overlay_sampler.Sample(4);
    {
      std::lock_guard<std::mutex> lock(g_cpu_summary_mutex);
      g_cpu_summary = std::move(overlay_text);
    }
    if (tick % 5 == 0) {
      REXLOG_WARN("Leistung: {} | {}", memory, log_sampler.Sample(10, tick % 15 == 0));
    }
  }
}

#endif  // defined(__APPLE__)

#if !defined(__APPLE__)
std::string MemoryText() { return {}; }
#endif

using GpuSummaryFunction = size_t (*)(char* out, size_t out_size);

// The Xenos plugin exports rexgpu_perf_summary. Look in the global symbol
// scope first, then ask the already loaded plugin directly (RTLD_NOLOAD never
// loads anything).
GpuSummaryFunction FindGpuSummaryFunction() {
  constexpr const char* kSymbol = "rexgpu_perf_summary";
  if (void* symbol = dlsym(RTLD_DEFAULT, kSymbol)) {
    return reinterpret_cast<GpuSummaryFunction>(symbol);
  }
  const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
  const std::filesystem::path candidates[] = {
      exe_dir / "Frameworks" / "librexgpu-xenos.dylib",
      exe_dir / "librexgpu-xenos.dylib",
      exe_dir / "librexgpu-xenos.so",
  };
  for (const std::filesystem::path& candidate : candidates) {
    if (void* handle = dlopen(candidate.c_str(), RTLD_LAZY | RTLD_NOLOAD)) {
      void* symbol = dlsym(handle, kSymbol);
      dlclose(handle);  // drops only the reference RTLD_NOLOAD added
      if (symbol) {
        return reinterpret_cast<GpuSummaryFunction>(symbol);
      }
    }
  }
  return nullptr;
}

class PerfOverlay : public rex::ui::ImGuiDialog {
 public:
  PerfOverlay(rex::ui::ImGuiDrawer* drawer, std::string header, bool full)
      : rex::ui::ImGuiDialog(drawer), header_(std::move(header)), full_(full) {}

  // Redrawn with every frame of the game. Only the progress of the shader
  // preparation at startup (no game frames meanwhile) needs its own redraws.
  bool WantsContinuousRepaint() const override { return loading_ || !gpu_summary_; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    // The Xenos plugin is loaded after the overlay exists; look its summary
    // function up until it is there (cheap, at most once per 60 frames).
    if (!gpu_summary_ && (frames_++ % 60) == 0) {
      gpu_summary_ = FindGpuSummaryFunction();
    }
    char gpu_text[4096] = {};
    if (gpu_summary_) {
      gpu_summary_(gpu_text, sizeof(gpu_text));
    }
    // Without the full overlay, only the progress of the shader preparation
    // at startup is shown (the screen stays black meanwhile).
    const bool loading = std::strncmp(gpu_text, "Shader werden", 13) == 0 ||
                         std::strncmp(gpu_text, "Grafik-Pipelines", 16) == 0;
    loading_ = loading;
    if (!full_ && !loading) {
      return;
    }
    std::string cpu_text;
    if (full_) {
      std::lock_guard<std::mutex> lock(g_cpu_summary_mutex);
      cpu_text = g_cpu_summary;
    }

    ImGui::SetNextWindowPos(ImVec2(6.0f, 6.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.5f);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##bo2_perf_overlay", nullptr, kFlags)) {
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.8f);
      ImGui::TextUnformatted(header_.c_str());
      ImGui::TextUnformatted(gpu_text[0] ? gpu_text : "GPU: noch keine Bilder");
      if (!cpu_text.empty()) {
        ImGui::TextUnformatted(cpu_text.c_str());
      }
      ImGui::PopFont();
    }
    ImGui::End();
  }

 private:
  std::string header_;
  bool full_;
  bool loading_ = true;
  GpuSummaryFunction gpu_summary_ = nullptr;
  uint32_t frames_ = 0;
};

}  // namespace

void StartCpuMonitor() {
#if defined(__APPLE__)
  std::thread(CpuMonitorMain).detach();
#if defined(__aarch64__)
  std::thread(ProfilerMain).detach();
#endif
#endif
}

namespace {

char g_crash_path[1024];
char g_crash_label[64];

void CrashWrite(int fd, const char* text) {
  ssize_t ignored = write(fd, text, std::strlen(text));
  (void)ignored;
}

void CrashWriteHex(int fd, uint64_t value) {
  char buffer[19] = "0x";
  for (int i = 0; i < 16; ++i) {
    buffer[2 + i] = "0123456789ABCDEF"[(value >> (60 - 4 * i)) & 0xF];
  }
  buffer[18] = '\0';
  CrashWrite(fd, buffer);
}

void CrashWriteDec(int fd, uint64_t value) {
  char buffer[24];
  int length = 0;
  do {
    buffer[length++] = char('0' + value % 10);
    value /= 10;
  } while (value && length < 23);
  char out[24];
  for (int i = 0; i < length; ++i) {
    out[i] = buffer[length - 1 - i];
  }
  out[length] = '\0';
  CrashWrite(fd, out);
}

// The last error the runtime logged (e.g. "Call to invalid or unregistered
// function at guest address ..." before it aborts), for the crash report.
char g_last_error[512] = {};

class LastErrorSink : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    if (msg.level < spdlog::level::err) {
      return;
    }
    const size_t length = std::min(msg.payload.size(), sizeof(g_last_error) - 1);
    std::memcpy(g_last_error, msg.payload.data(), length);
    g_last_error[length] = '\0';
  }
  void flush_() override {}
};

// Only async-signal-safe calls (open/write/close, backtrace on Darwin in
// practice), the process is about to end.
void ReportCrash(int signal_number, uint64_t fault_address, uint64_t pc, uint64_t lr) {
  const int fd = open(g_crash_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    return;
  }
  CrashWrite(fd, "\n=== Absturz (");
  CrashWrite(fd, g_crash_label);
  CrashWrite(fd, "), Zeit ");
  CrashWriteDec(fd, uint64_t(time(nullptr)));
  CrashWrite(fd, ", Signal ");
  CrashWriteDec(fd, uint64_t(signal_number));
  CrashWrite(fd, ", Adresse ");
  CrashWriteHex(fd, fault_address);
  if (g_last_error[0]) {
    CrashWrite(fd, "\nLetzte Fehlermeldung: ");
    CrashWrite(fd, g_last_error);
  }
  CrashWrite(fd, "\nPC/LR:\n");
  void* registers[2] = {reinterpret_cast<void*>(pc), reinterpret_cast<void*>(lr)};
  backtrace_symbols_fd(registers, lr ? 2 : 1, fd);
  CrashWrite(fd, "Aufrufkette:\n");
  void* frames[64];
  const int frame_count = backtrace(frames, 64);
  backtrace_symbols_fd(frames, frame_count, fd);
  close(fd);
}

void FatalSignalHandler(int signal_number, siginfo_t* info, void* context) {
  uint64_t pc = 0;
  uint64_t lr = 0;
#if defined(__APPLE__) && defined(__aarch64__)
  const auto* ucontext = static_cast<ucontext_t*>(context);
  pc = __darwin_arm_thread_state64_get_pc(ucontext->uc_mcontext->__ss);
  lr = __darwin_arm_thread_state64_get_lr(ucontext->uc_mcontext->__ss);
#else
  (void)context;
#endif
  ReportCrash(signal_number, reinterpret_cast<uint64_t>(info ? info->si_addr : nullptr), pc, lr);
  signal(signal_number, SIG_DFL);
  raise(signal_number);
}

bool SDLCALL MemoryWarningWatch(void*, SDL_Event* event) {
  if (event->type == SDL_EVENT_LOW_MEMORY) {
    REXLOG_WARN("iOS meldet knappen Speicher! {}", MemoryText());
    rex::FlushLogging();
  }
  return true;
}

}  // namespace

void CaptureLastError() {
  auto sink = std::make_shared<LastErrorSink>();
  spdlog::apply_all([&](std::shared_ptr<spdlog::logger> logger) { logger->sinks().push_back(sink); });
}

void InstallCrashReporter(const std::filesystem::path& data_dir, const char* build_label) {
  std::snprintf(g_crash_path, sizeof(g_crash_path), "%s",
                (data_dir / "absturz.txt").string().c_str());
  std::snprintf(g_crash_label, sizeof(g_crash_label), "%s", build_label);
  rex::arch::ExceptionHandler::SetUnhandledReporter(ReportCrash);
  struct sigaction action = {};
  action.sa_sigaction = FatalSignalHandler;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  for (int signal_number : {SIGABRT, SIGTRAP, SIGFPE}) {
    sigaction(signal_number, &action, nullptr);
  }
  SDL_AddEventWatch(MemoryWarningWatch, nullptr);
}

void WriteContentList(const std::vector<rex::system::xam::ContentPackageFile>& packages,
                      const std::filesystem::path& title_update, uint32_t title_id,
                      const std::filesystem::path& data_dir) {
  using rex::system::XContentType;
  auto kind = [](XContentType type) -> std::string {
    switch (type) {
      case XContentType::kMarketplaceContent:
        return "DLC";
      case XContentType::kInstaller:
        return "Title Update";
      case XContentType::kAvatarItem:
        return "Avatar-Gegenstand";
      default:
        return fmt::format("Typ {:08X}", uint32_t(type));
    }
  };
  std::string text;
  size_t dlc_count = 0;
  for (const auto& package : packages) {
    const XContentType type = package.data.content_type;
    const bool other_game = uint32_t(package.data.title_id) != title_id;
    if (type == XContentType::kMarketplaceContent && !other_game) {
      ++dlc_count;
    }
    std::string line = fmt::format(
        "{:<18} {:<40} {:>6} MB  {}{}{}", kind(type),
        rex::string::to_utf8(package.data.display_name()), package.content_size >> 20,
        package.path.filename().string(), other_game ? "  (anderes Spiel, ignoriert)" : "",
        package.path == title_update ? "  (wird verwendet)" : "");
    REXLOG_WARN("Paket {}", line);
    text += line + "\n";
  }
  REXLOG_WARN("{} DLC-Pakete, Title Update: {}", dlc_count,
              title_update.empty() ? std::string("fehlt") : title_update.filename().string());
  if (data_dir.empty()) {
    return;
  }
  std::ofstream out(data_dir / "dlc.txt", std::ios::trunc);
  out << "Gefundene Pakete im DLC-Ordner des Spiels (beim letzten Start)\n\n";
  out << (text.empty() ? std::string("keine\n") : text);
  out << "\nDLC: " << dlc_count << "\n";
  out << "Title Update: "
      << (title_update.empty() ? std::string("fehlt (oder fuer dieses Spiel nicht noetig) - Title Update in den DLC-Ordner des Spiels kopieren")
                               : title_update.filename().string())
      << "\n";
  out << "\nDie Pakete bleiben so, wie sie von der Konsole kommen (Dateinamen wie "
         "4F8383F7...41). Unterordner sind egal.\n";
}

void CheckGameFiles(const std::filesystem::path& game_dir, const std::filesystem::path& data_dir,
                    const std::string& mode, const char* build_label,
                    rex::ui::WindowedAppContext* app_context) {
  namespace fs = std::filesystem;
  auto lower = [](std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return text;
  };
  // All .ipak files up to three folder levels deep.
  std::map<std::string, std::pair<fs::path, uint64_t>> found;
  std::error_code ec;
  fs::recursive_directory_iterator it(game_dir, fs::directory_options::skip_permission_denied, ec);
  for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it.depth() > 3) {
      it.disable_recursion_pending();
      continue;
    }
    std::error_code file_ec;
    if (!it->is_regular_file(file_ec)) {
      continue;
    }
    const std::string name = lower(it->path().filename().string());
    if (name.size() < 5 || name.compare(name.size() - 5, 5, ".ipak") != 0) {
      continue;
    }
    found[name] = {fs::relative(it->path(), game_dir, file_ec), uint64_t(it->file_size(file_ec))};
  }
  uint64_t total_mb = 0;
  std::string list;
  for (const auto& [name, info] : found) {
    total_mb += info.second >> 20;
    if (!list.empty()) {
      list += ", ";
    }
    list += fmt::format("{} ({} MB)", info.first.generic_string(), info.second >> 20);
  }
  REXLOG_WARN("BO2: Bilddateien (.ipak) im Spielordner: {} Dateien, {} MB: {}", found.size(),
              total_mb, list.empty() ? "keine" : list);
  // The Xbox 360 disc has the image packs base.ipak and lowmip.ipak (all maps
  // and modes in one). The game also tries code_post_gfx_mp, common_mp,
  // ui_mp, mp_<map> and faction_* packs - those only exist on other
  // platforms or in DLC, so their absence is normal (build 20 hinted
  // otherwise, wrongly).
  (void)mode;
  std::vector<std::string> wanted = {"base.ipak", "lowmip.ipak"};
  std::string missing;
  std::string misplaced;
  for (const std::string& name : wanted) {
    auto entry = found.find(name);
    if (entry == found.end()) {
      missing += (missing.empty() ? "" : ", ") + name;
    } else if (entry->second.first.has_parent_path()) {
      misplaced += (misplaced.empty() ? "" : ", ") + entry->second.first.generic_string();
    }
  }
  if (missing.empty() && misplaced.empty()) {
    return;
  }
  REXLOG_WARN("BO2: Bilddateien fehlen im Spielordner: {}{}{}", missing.empty() ? "-" : missing,
              misplaced.empty() ? "" : " | in Unterordnern statt direkt im Spielordner: ",
              misplaced);
  // The hint once per build.
  const fs::path marker = data_dir / fmt::format("ipak_hinweis2_{}.txt", build_label);
  if (data_dir.empty() || fs::exists(marker, ec) || !app_context) {
    return;
  }
  std::ofstream(marker) << "gezeigt\n";
  std::string message =
      "Hinweis: Bilddateien fehlen\n\n"
      "Das Spiel laedt seine detaillierten Texturen waehrend des Spiels aus den Bilddateien "
      "base.ipak und lowmip.ipak. Im Ordner BO2 fehlt:\n" +
      (missing.empty() ? std::string("-") : missing) +
      (misplaced.empty() ? std::string()
                         : "\n\nIn einem Unterordner statt direkt bei default_mp.xex:\n" +
                               misplaced) +
      "\n\nKopiere sie von deiner Disc direkt in den Ordner BO2, dorthin wo default_mp.xex "
      "liegt. Die App laeuft trotzdem "
      "weiter.\n\nDieser Hinweis erscheint nur einmal.";
  app_context->CallInUIThreadDeferred([message = std::move(message)]() {
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Warning, message);
  });
}

std::unique_ptr<rex::ui::ImGuiDialog> CreatePerfOverlay(rex::ui::ImGuiDrawer* drawer,
                                                        std::string header, bool full) {
  return std::make_unique<PerfOverlay>(drawer, std::move(header), full);
}

}  // namespace bo2
