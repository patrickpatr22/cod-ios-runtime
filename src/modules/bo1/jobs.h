// Black Ops - job worker threads that sleep instead of spinning, see
// jobs_impl.inc.

#pragma once

namespace bo1 {

// On: a job worker that only finds jobs waiting for their condition sleeps
// until new work arrives (at most a quarter of a millisecond) instead of
// spinning on a core. Applies at once.
void SetRelaxJobThreads(bool relax);

}  // namespace bo1
