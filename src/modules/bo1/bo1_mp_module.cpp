// Call of Duty iPad - Black Ops module of default_mp.xex (Multiplayer, Title
// Update 11): the settings its hooks read.

#include "game_module.h"

// Title Update 11 addresses, see jobs_impl.inc.
#define BO1_JQ_POP sub_825E38A8
#define BO1_JQ_POP_ORIGINAL __imp__sub_825E38A8
#define BO1_JQ_PUSH sub_825E4CA0
#define BO1_JQ_PUSH_ORIGINAL __imp__sub_825E4CA0
#define BO1_JQ_WORKER_LR1 0x825E3BA4u
#define BO1_JQ_WORKER_LR2 0x825E3DE8u
#define BO1_JQ_REQUEUE_LR 0x825E3D3Cu
#define BO1_JQ_ADD_LR 0x825E35F8u
#include "jobs_impl.inc"

// Title Update 11 addresses, see sound_impl.inc.
#define BO1_SND_LOAD sub_82353D60
#define BO1_SND_LOAD_ORIGINAL __imp__sub_82353D60
#define BO1_SND_CHECK sub_824A3588
#define BO1_SND_CHECK_ORIGINAL __imp__sub_824A3588
#define BO1_SND_TABLE 0x834B96A0u
#define BO1_SND_REMOVE sub_824A37A0
#define BO1_SND_REMOVE_ORIGINAL __imp__sub_824A37A0
#include "sound_impl.inc"

void CodApplyModuleSettings(const cod::GameModuleSettings& settings) {
  bo1::SetRelaxJobThreads(settings.relax_job_threads);
}
