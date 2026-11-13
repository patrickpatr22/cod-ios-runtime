// BO2 iOS - imports the game touches that have no meaning on iPad.
// (Kinect/USB camera and Live QoS statistics; same results as bo2-recompiled.)

#include <rex/hook.h>

#define BO2_STUB_RESULT(name, value) \
  REX_HOOK_RAW(name) {               \
    (void)base;                      \
    ctx.r3.u64 = (value);            \
  }

BO2_STUB_RESULT(__imp__XUsbcamCreate, 0)
BO2_STUB_RESULT(__imp__XUsbcamGetState, 0)
BO2_STUB_RESULT(__imp__XUsbcamDestroy, 0x80004005)
BO2_STUB_RESULT(__imp__XUsbcamSetConfig, 0x80004005)
BO2_STUB_RESULT(__imp__XUsbcamSetCaptureMode, 0x80004005)
BO2_STUB_RESULT(__imp__XUsbcamReadFrame, 0x80004005)
BO2_STUB_RESULT(__imp__NetDll_XNetQosGetListenStats, 0)
