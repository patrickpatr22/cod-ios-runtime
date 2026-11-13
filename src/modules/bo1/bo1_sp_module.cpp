// Call of Duty iPad - Black Ops module of default.xex (Campaign + Zombies,
// Title Update 11): the settings its hooks read.

#include "game_module.h"

// Title Update 11 addresses, see jobs_impl.inc.
#define BO1_JQ_POP sub_82533240
#define BO1_JQ_POP_ORIGINAL __imp__sub_82533240
#define BO1_JQ_PUSH sub_82534638
#define BO1_JQ_PUSH_ORIGINAL __imp__sub_82534638
#define BO1_JQ_WORKER_LR1 0x8253353Cu
#define BO1_JQ_WORKER_LR2 0x82533780u
#define BO1_JQ_REQUEUE_LR 0x825336D4u
#define BO1_JQ_ADD_LR 0x82532F90u
#include "jobs_impl.inc"

// Title Update 11 addresses, see sound_impl.inc.
#define BO1_SND_LOAD sub_8231E858
#define BO1_SND_LOAD_ORIGINAL __imp__sub_8231E858
#define BO1_SND_CHECK sub_824452C8
#define BO1_SND_CHECK_ORIGINAL __imp__sub_824452C8
#define BO1_SND_TABLE 0x83287E88u
#define BO1_SND_REMOVE sub_824454E0
#define BO1_SND_REMOVE_ORIGINAL __imp__sub_824454E0
#include "sound_impl.inc"

void CodApplyModuleSettings(const cod::GameModuleSettings& settings) {
  bo1::SetRelaxJobThreads(settings.relax_job_threads);
}
