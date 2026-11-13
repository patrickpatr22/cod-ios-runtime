// Black Ops - job worker threads that sleep instead of spinning.
//
// Included once per Black Ops module with the addresses of its executable:
//   BO1_JQ_POP          jqPop(worker, group, out batch, flag): takes the next
//                       batch from the queues of a thread
//   BO1_JQ_PUSH         pushes a batch (124 bytes, copied) to a queue; used
//                       by the add function and by every "put it back"
//   BO1_JQ_WORKER_LR1/2 return addresses of the two jqPop calls in the loop
//                       of the "JQ Worker" threads
//   BO1_JQ_REQUEUE_LR   return address of the push in that loop (the job
//                       asked to be run again)
//   BO1_JQ_ADD_LR       return address of the push in the add function (new
//                       work)
// (default.xex and default_mp.xex share this code instruction for
// instruction, the multiplayer addresses are 0xB0668 higher.)
//
// A job function that returns non-zero is put back into the queue and tried
// again: that is how the engine lets a job wait for another kind of job
// (r_add_bsp returns 1 while r_dpvs_static still has batches, and so on),
// and a batch can carry a condition that is handled the same way. While
// such a job is queued, the workers take it, call it and put it back
// hundreds of thousands of times per second and never reach their own idle
// wait, which needs an empty queue. On the Xbox they have hardware threads
// of their own. On the iPad the log of build 30 shows both workers at 92 to
// 95 % all the time - two cores that the threads with real work don't get.
//
// So: a worker that has done nothing but put waiting jobs back for
// kIdleAfter, with no new job added in that time, sleeps before it looks
// again - until a job is added (the add wakes the sleepers), at most
// kSleepTime. Jobs that work in slices (they run for a while before they
// ask to be run again) keep it awake. It holds no
// batch while it sleeps, so a thread that waits for a job and helps out can
// still take it. Short waits inside a frame's burst of jobs stay as they
// are.
//
// Every 10 seconds each worker logs what it did and which jobs were put
// back most often.

#include "jobs.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

namespace bo1 {
namespace {

std::atomic<bool> g_relax_job_threads{true};
// Workers asleep right now, and a count of the jobs added so far: a worker
// only falls asleep if nothing was added since it last looked, and an add
// wakes whoever is asleep.
std::atomic<uint32_t> g_sleepers{0};
std::atomic<uint32_t> g_adds{0};
std::mutex g_sleep_mutex;
std::condition_variable g_sleep_condition;

constexpr uint64_t kIdleAfterNs = 2'000'000;
// A job that ran longer than this before it asked to be run again did a
// piece of its work - only a job that comes back at once is waiting for
// something. (A worker that was interrupted in between looks the same,
// hence the share of time below rather than a single measurement.)
constexpr uint64_t kWaitingJobNs = 5'000;
constexpr std::chrono::microseconds kSleepTime{250};
constexpr uint64_t kReportEveryNs = 10'000'000'000ull;

inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

struct WorkerState {
  // The last pop handed out a batch / it was put back since / how long the
  // job ran before that (long: it worked, see kWaitingJobNs).
  bool holding = false;
  bool requeued = false;
  bool last_job_worked = false;
  uint64_t popped_at_ns = 0;
  // Since when this worker has only put jobs back (0: its last job ran to
  // the end), the add count at that moment and the time jobs worked since.
  uint64_t only_requeues_since_ns = 0;
  uint32_t adds_seen = 0;
  uint64_t worked_ns = 0;
  // Nothing but waiting jobs for kIdleAfter: sleeping between looks.
  bool idle = false;

  // Report.
  uint64_t report_at_ns = 0;
  uint32_t pops = 0;
  uint32_t empty = 0;
  uint32_t done = 0;
  uint32_t requeues = 0;
  uint32_t slices = 0;
  uint32_t sleeps = 0;
  struct Module {
    uint32_t address;
    uint32_t requeues;
  };
  Module modules[8] = {};
};
thread_local WorkerState t_worker;

void Report(WorkerState& state, const uint8_t* base, uint64_t now) {
  if (!state.report_at_ns) {
    state.report_at_ns = now + kReportEveryNs;
    return;
  }
  if (now < state.report_at_ns) {
    return;
  }
  std::string modules;
  for (const WorkerState::Module& module : state.modules) {
    if (!module.address || !module.requeues) {
      continue;
    }
    // jqModule: name at +0.
    char name[40] = {};
    const uint32_t name_address = Load32(base, module.address);
    if (name_address) {
      std::memcpy(name, base + name_address, sizeof(name) - 1);
      for (char& c : name) {
        if (c && (c < 32 || c > 126)) {
          c = '?';
        }
      }
    }
    if (!modules.empty()) {
      modules += ", ";
    }
    modules += name;
    modules += ' ';
    modules += std::to_string(module.requeues);
  }
  REXLOG_WARN("JQ-Worker (10 s): {} geholt, {} leer, {} erledigt, davon {} in Scheiben, {} "
              "wartend zurueckgelegt, {} mal geschlafen | wartend: {}",
              state.pops, state.empty, state.done, state.slices, state.requeues, state.sleeps,
              modules.empty() ? "-" : modules);
  state.report_at_ns = now + kReportEveryNs;
  state.pops = state.empty = state.done = state.requeues = state.slices = state.sleeps = 0;
  for (WorkerState::Module& module : state.modules) {
    module = {};
  }
}

}  // namespace

void SetRelaxJobThreads(bool relax) {
  if (g_relax_job_threads.exchange(relax, std::memory_order_relaxed) != relax) {
    REXLOG_WARN("Black Ops: Job-Threads schonen {}", relax ? "an" : "aus");
    if (!relax) {
      { std::lock_guard<std::mutex> lock(g_sleep_mutex); }
      g_sleep_condition.notify_all();
    }
  }
}

}  // namespace bo1

extern "C" void BO1_JQ_POP_ORIGINAL(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void BO1_JQ_PUSH_ORIGINAL(PPCContext& __restrict ctx, uint8_t* base);

extern "C" REX_FUNC(BO1_JQ_POP) {
  const uint32_t caller = uint32_t(ctx.lr);
  if (caller != BO1_JQ_WORKER_LR1 && caller != BO1_JQ_WORKER_LR2) {
    BO1_JQ_POP_ORIGINAL(ctx, base);
    return;
  }
  bo1::WorkerState& state = bo1::t_worker;
  uint64_t now = bo1::NowNs();
  // What became of the batch handed out last time.
  const bool last_job_worked = state.last_job_worked;
  state.last_job_worked = false;
  if (state.holding) {
    state.holding = false;
    if (!state.requeued) {
      ++state.done;
      state.only_requeues_since_ns = 0;
      state.idle = false;
    } else if (!state.only_requeues_since_ns) {
      state.only_requeues_since_ns = now;
      state.adds_seen = bo1::g_adds.load(std::memory_order_seq_cst);
      state.worked_ns = 0;
    }
  }
  if (state.only_requeues_since_ns && now - state.only_requeues_since_ns >= bo1::kIdleAfterNs &&
      bo1::g_relax_job_threads.load(std::memory_order_relaxed)) {
    if (!state.idle) {
      if (state.worked_ns * 4 > now - state.only_requeues_since_ns) {
        // Jobs that work in slices kept it busy: look again in a while.
        state.only_requeues_since_ns = now;
        state.worked_ns = 0;
      } else {
        state.idle = true;
      }
    }
    // Only host state in here (the registers of the guest stay untouched).
    if (state.idle && !last_job_worked) {
      bool added = bo1::g_adds.load(std::memory_order_seq_cst) != state.adds_seen;
      if (!added) {
        std::unique_lock<std::mutex> lock(bo1::g_sleep_mutex);
        bo1::g_sleepers.fetch_add(1, std::memory_order_seq_cst);
        if (bo1::g_adds.load(std::memory_order_seq_cst) == state.adds_seen) {
          bo1::g_sleep_condition.wait_for(lock, bo1::kSleepTime);
        }
        bo1::g_sleepers.fetch_sub(1, std::memory_order_seq_cst);
        lock.unlock();
        ++state.sleeps;
        now = bo1::NowNs();
        added = bo1::g_adds.load(std::memory_order_seq_cst) != state.adds_seen;
      }
      if (added) {
        // New work: awake for the next kIdleAfter at least.
        state.adds_seen = bo1::g_adds.load(std::memory_order_seq_cst);
        state.only_requeues_since_ns = now;
        state.worked_ns = 0;
        state.idle = false;
      }
    }
  }

  BO1_JQ_POP_ORIGINAL(ctx, base);
  ++state.pops;
  if (ctx.r3.u32 & 0xFF) {
    state.holding = true;
    state.requeued = false;
    state.popped_at_ns = bo1::NowNs();
  } else {
    // Nothing queued for this worker. Its own idle wait follows, but it only
    // blocks while no job at all is pending (on the loading screen it spun
    // through 2 million empty looks per second): idle like waiting jobs.
    ++state.empty;
    if (!state.only_requeues_since_ns) {
      state.only_requeues_since_ns = now;
      state.adds_seen = bo1::g_adds.load(std::memory_order_seq_cst);
      state.worked_ns = 0;
    }
  }
  bo1::Report(state, base, now);
}

extern "C" REX_FUNC(BO1_JQ_PUSH) {
  const uint32_t caller = uint32_t(ctx.lr);
  const uint32_t batch = ctx.r4.u32;
  BO1_JQ_PUSH_ORIGINAL(ctx, base);
  if (caller == BO1_JQ_REQUEUE_LR) {
    bo1::WorkerState& state = bo1::t_worker;
    const uint64_t ran_ns = bo1::NowNs() - state.popped_at_ns;
    if (ran_ns > bo1::kWaitingJobNs) {
      ++state.slices;
      state.worked_ns += ran_ns;
      state.last_job_worked = true;
    }
    state.requeued = true;
    ++state.requeues;
    // jqBatch: module at +12 (the caller's copy; the queue took its own).
    const uint32_t module = bo1::Load32(base, batch + 12);
    for (bo1::WorkerState::Module& slot : state.modules) {
      if (slot.address == module || !slot.address) {
        slot.address = module;
        ++slot.requeues;
        break;
      }
    }
    return;
  }
  if (caller == BO1_JQ_ADD_LR) {
    bo1::g_adds.fetch_add(1, std::memory_order_seq_cst);
    if (bo1::g_sleepers.load(std::memory_order_seq_cst)) {
      { std::lock_guard<std::mutex> lock(bo1::g_sleep_mutex); }
      bo1::g_sleep_condition.notify_all();
    }
  }
}
