// Call of Duty iPad - Black Ops (single player / zombies, title update 11):
// a look at the file streams of the game.
//
// Build 36 found the map pack zombie maps crashing in the stream_decompress
// job: it unpacks a block read from a pack into the buffer of the request,
// and that buffer was address 0. The module of build 36 (patched) lets such
// a job fail like a damaged block and keeps a copy of the request; this
// reports it, and the crash report lists the open requests.
//
// Build 37: the game's LZO decompressor ignores the size of the target, so a
// block that is not what the request expects overwrites whatever follows the
// buffer. The module of build 37 decompresses stream blocks with a bounded
// decoder instead (same bytes for valid blocks); a block that would overflow
// fails like a damaged one and is reported here with its pack and offset.

#pragma once

#include <cstdint>

namespace cod {

// Starts a watcher thread (once) for the module "bo1_sp": guest_base is the
// virtual memory base, module_base the Mach-O header of libcod_bo1_sp.dylib.
void StartBo1StreamWatch(const uint8_t* guest_base, const void* module_base);

// For the crash report (only write(), no allocation): the open stream
// requests and the last intercepted one. Nothing when the watch never started.
void Bo1StreamCrashDump(int fd);

}  // namespace cod
