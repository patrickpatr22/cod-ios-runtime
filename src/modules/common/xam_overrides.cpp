// BO2 iOS - XAM / XNet overrides for default_mp.xex
//
// Behaviour follows officialmelon/bo2-recompiled (Windows). That project
// patched the recompiled import thunks with x86 machine code at runtime, which
// is impossible on iOS (code pages are signed and never writable). Here the
// overrides are plain definitions of the __imp__ symbols instead: the
// recompiled game code and its function table live in this executable, so the
// static linker binds every reference to these versions and the runtime's own
// implementations in librexruntime are simply not used by the game.

#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/memory/utils.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>
#include <rex/system/xevent.h>
#include <rex/system/xio.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

namespace {

using namespace rex;
using namespace rex::system;

constexpr uint32_t kAppXgi = 0xFB;
constexpr uint32_t kAppXLiveBase = 0xFC;

constexpr uint32_t kMsgXgiSessionCreate = 0x000B0010;
constexpr uint32_t kMsgXgiSessionDelete = 0x000B0011;
constexpr uint32_t kMsgXgiSessionJoinLocal = 0x000B0012;
constexpr uint32_t kMsgXgiSessionLeaveLocal = 0x000B0013;
constexpr uint32_t kMsgXLiveGetServiceInfo = 0x00058007;

constexpr uint32_t kWsaeWouldBlock = 0x2733;
constexpr uint32_t kWsaeNotSock = 0x2736;

constexpr uint32_t kXNetGetXnAddrEthernet = 0x00000002;
constexpr uint32_t kXNetGetXnAddrStatic = 0x00000004;

constexpr uint32_t kXNetEthernetLinkActive = 0x01;
constexpr uint32_t kXNetEthernetLink100Mbps = 0x02;
constexpr uint32_t kXNetEthernetLinkFullDuplex = 0x08;

// Fixed local identity: 192.168.1.100
constexpr uint32_t kLocalAddress = 0xC0A80164;

struct XMSGSTARTIOREQUEST_UNKNOWNARG {
  be<uint32_t> unk_0;
  be<uint32_t> unk_1;
};

struct XNADDR {
  be<uint32_t> ina;
  be<uint32_t> ina_online;
  be<uint16_t> port_online;
  uint8_t ab_enet[6];
  uint8_t ab_online[20];
};

struct XNQOSINFO {
  uint8_t flags;
  uint8_t reserved;
  be<uint16_t> probes_xmit;
  be<uint16_t> probes_recv;
  be<uint16_t> data_len;
  be<uint32_t> data_ptr;
  be<uint16_t> rtt_min_in_msecs;
  be<uint16_t> rtt_med_in_msecs;
  be<uint32_t> up_bits_per_sec;
  be<uint32_t> down_bits_per_sec;
};

struct XNQOS {
  be<uint32_t> count;
  be<uint32_t> count_pending;
  XNQOSINFO info[1];
};

void FillLocalXnAddr(XNADDR* addr, uint32_t ina) {
  addr->ina = ina;
  addr->ina_online = 0;
  addr->port_online = 0;
  std::memset(addr->ab_enet, 0xCC, sizeof(addr->ab_enet));
  std::memset(addr->ab_online, 0, sizeof(addr->ab_online));
}

// --- XGI / XLiveBase messages the game needs answered offline ------------

bool HandleXgiMessage(uint32_t message, uint32_t buffer_ptr, uint32_t buffer_length,
                      X_HRESULT& out_result) {
  auto* memory = REX_KERNEL_MEMORY();
  auto* buffer = buffer_ptr ? memory->TranslateVirtual(buffer_ptr) : nullptr;

  switch (message) {
    case kMsgXgiSessionCreate: {
      if (buffer_length && buffer_length != 28) {
        out_result = X_E_INVALIDARG;
        return true;
      }
      if (!buffer) {
        out_result = X_E_SUCCESS;
        return true;
      }
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 0x14);
      uint32_t nonce_ptr = memory::load_and_swap<uint32_t>(buffer + 0x18);
      if (session_info_ptr) {
        auto* info = memory->TranslateVirtual(session_info_ptr);
        memory::store_and_swap<uint32_t>(info + 0x00, 0xAB1234CD);
        memory::store_and_swap<uint32_t>(info + 0x04, 0xEF5678AB);
        memory::store_and_swap<uint32_t>(info + 0x08, kLocalAddress);
        memory::store_and_swap<uint32_t>(info + 0x0C, 0);
        memory::store_and_swap<uint16_t>(info + 0x10, 0);
        std::memset(info + 0x12, 0xCC, 6);
        std::memset(info + 0x18, 0, 20);
        std::memset(info + 0x2C, 0xAB, 16);
      }
      if (nonce_ptr) {
        auto* nonce = memory->TranslateVirtual(nonce_ptr);
        memory::store_and_swap<uint32_t>(nonce + 0, 0x12345678);
        memory::store_and_swap<uint32_t>(nonce + 4, 0x9ABCDEF0);
      }
      out_result = X_E_SUCCESS;
      return true;
    }
    case kMsgXgiSessionDelete:
    case kMsgXgiSessionJoinLocal:
    case kMsgXgiSessionLeaveLocal:
      out_result = X_E_SUCCESS;
      return true;
    default:
      return false;
  }
}

bool HandleXLiveBaseMessage(uint32_t message, X_HRESULT& out_result) {
  if (message != kMsgXLiveGetServiceInfo) {
    return false;
  }
  out_result = X_E_SUCCESS;
  return true;
}

X_HRESULT DispatchWithOverrides(uint32_t app, uint32_t message, uint32_t buffer_ptr,
                                uint32_t buffer_length) {
  X_HRESULT result = X_ERROR_NOT_FOUND;
  if (app == kAppXgi && HandleXgiMessage(message, buffer_ptr, buffer_length, result)) {
    return result;
  }
  if (app == kAppXLiveBase && HandleXLiveBaseMessage(message, result)) {
    return result;
  }
  return REX_KERNEL_STATE()->app_manager()->DispatchMessageSync(app, message, buffer_ptr,
                                                                buffer_length);
}

X_HRESULT StartIoRequestWithOverrides(uint32_t app, uint32_t message, uint32_t overlapped_ptr,
                                      uint32_t buffer_ptr, uint32_t buffer_length) {
  X_HRESULT result = X_ERROR_NOT_FOUND;
  bool handled = false;
  if (app == kAppXgi) {
    handled = HandleXgiMessage(message, buffer_ptr, buffer_length, result);
  } else if (app == kAppXLiveBase) {
    handled = HandleXLiveBaseMessage(message, result);
  }

  if (!handled) {
    result = REX_KERNEL_STATE()->app_manager()->DispatchMessageAsync(app, message, buffer_ptr,
                                                                     buffer_length);
    if (result == X_E_NOTFOUND) {
      result = X_E_INVALIDARG;
      XThread::SetLastError(X_ERROR_NOT_FOUND);
    }
  }

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, result);
    result = X_ERROR_IO_PENDING;
  }

  if (result == X_ERROR_SUCCESS || result == X_ERROR_IO_PENDING) {
    XThread::SetLastError(0);
  }
  return result;
}

// --- XAM ------------------------------------------------------------------

u32 XamUserCheckPrivilegeOverride(u32 user_index, u32 mask, mapped_u32 out_value) {
  (void)mask;
  if (user_index != 0xFF) {
    if (user_index >= 4) {
      return X_ERROR_INVALID_PARAMETER;
    }
    if (user_index) {
      return X_ERROR_NO_SUCH_USER;
    }
  }
  if (out_value) {
    *out_value = 1;
  }
  return X_ERROR_SUCCESS;
}

u32 XMsgInProcessCallOverride(u32 app, u32 message, u32 arg1, u32 arg2) {
  return DispatchWithOverrides(app, message, arg1, arg2);
}

u32 XMsgStartIORequestExOverride(u32 app, u32 message, ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr,
                                 u32 buffer_ptr, u32 buffer_length,
                                 ppc_ptr_t<XMSGSTARTIOREQUEST_UNKNOWNARG> unknown_ptr) {
  (void)unknown_ptr;
  return StartIoRequestWithOverrides(app, message, overlapped_ptr.guest_address(), buffer_ptr,
                                     buffer_length);
}

u32 XMsgStartIORequestOverride(u32 app, u32 message, ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr,
                               u32 buffer_ptr, u32 buffer_length) {
  return StartIoRequestWithOverrides(app, message, overlapped_ptr.guest_address(), buffer_ptr,
                                     buffer_length);
}

u32 XMsgCancelIORequestOverride(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr, u32 wait) {
  X_HANDLE event_handle = XOverlappedGetEvent(overlapped_ptr);
  if (event_handle && wait) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    if (ev) {
      ev->Wait(0, 0, true, nullptr);
    }
  }
  return 0;
}

u32 XMsgCompleteIORequestOverride(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr, u32 result,
                                  u32 extended_error, u32 length) {
  REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(overlapped_ptr.guest_address(), result,
                                                    extended_error, length);
  return X_ERROR_SUCCESS;
}

u32 XamGetOverlappedResultOverride(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr,
                                   mapped_u32 length_ptr, u32 unknown) {
  (void)unknown;
  uint32_t result = 0;
  if (overlapped_ptr->result != X_ERROR_IO_PENDING) {
    result = overlapped_ptr->result;
  } else if (!overlapped_ptr->event) {
    result = X_ERROR_IO_INCOMPLETE;
  } else {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(overlapped_ptr->event);
    result = ev ? ev->Wait(3, 1, 0, nullptr) : X_STATUS_INVALID_HANDLE;
    if (XSUCCEEDED(result)) {
      result = overlapped_ptr->result;
    } else {
      result = kernel::xboxkrnl::xeRtlNtStatusToDosError(result);
    }
  }
  if (XSUCCEEDED(result) && length_ptr) {
    *length_ptr = overlapped_ptr->length;
  }
  return result;
}

// --- XNet: pretend to be a single console on a local network --------------

u32 NetDllXNetGetTitleXnAddrOverride(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  (void)caller;
  if (addr_ptr) {
    FillLocalXnAddr(addr_ptr, kLocalAddress);
  }
  return kXNetGetXnAddrEthernet | kXNetGetXnAddrStatic;
}

u32 NetDllXNetXnAddrToMachineIdOverride(u32 caller, ppc_ptr_t<XNADDR> addr_ptr,
                                        mapped_u32 id_ptr) {
  (void)caller;
  (void)addr_ptr;
  if (id_ptr) {
    *id_ptr = 1;
  }
  return 0;
}

void NetDllXNetInAddrToStringOverride(u32 caller, u32 in_addr_val, mapped_string string_out,
                                      u32 string_size) {
  (void)caller;
  (void)in_addr_val;
  if (string_out && string_size) {
    rex::string::copy_truncating(string_out.host_address(), "127.0.0.1", string_size);
  }
}

u32 NetDllXNetXnAddrToInAddrOverride(u32 caller, ppc_ptr_t<XNADDR> xn_addr, mapped_void xid,
                                     mapped_void in_addr) {
  (void)caller;
  (void)xid;
  if (in_addr && xn_addr) {
    auto* out = REX_KERNEL_MEMORY()->TranslateVirtual(in_addr.guest_address());
    memory::store<uint32_t>(out, xn_addr->ina);
  }
  return 0;
}

u32 NetDllXNetInAddrToXnAddrOverride(u32 caller, mapped_void in_addr, ppc_ptr_t<XNADDR> xn_addr,
                                     mapped_void xid) {
  (void)caller;
  (void)xid;
  if (xn_addr && in_addr) {
    auto* src = REX_KERNEL_MEMORY()->TranslateVirtual(in_addr.guest_address());
    FillLocalXnAddr(xn_addr, memory::load<uint32_t>(src));
  }
  return 0;
}

u32 NetDllXNetQosServiceLookupOverride(u32 caller, u32 flags, u32 event_handle, mapped_u32 pqos) {
  (void)caller;
  (void)flags;
  if (pqos) {
    uint32_t qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNQOS));
    auto* qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = 1;
    qos->count_pending = 0;
    std::memset(&qos->info[0], 0, sizeof(qos->info[0]));
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    if (ev) {
      ev->Set(0, false);
    }
  }
  return 0;
}

u32 NetDllXNetQosListenOverride(u32 caller, mapped_void id, mapped_void data, u32 data_size,
                                u32 r7, u32 flags) {
  (void)caller;
  (void)id;
  (void)data;
  (void)data_size;
  (void)r7;
  (void)flags;
  return 0;
}

u32 NetDllXNetGetEthernetLinkStatusOverride(u32 caller) {
  (void)caller;
  return kXNetEthernetLinkActive | kXNetEthernetLink100Mbps | kXNetEthernetLinkFullDuplex;
}

u32 NetDllRecvFromOverride(u32 caller, u32 socket_handle, mapped_u8 buf_ptr, u32 buf_len,
                           u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr, mapped_u32 fromlen_ptr) {
  (void)caller;
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(kWsaeNotSock);
    return static_cast<uint32_t>(-1);
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  int ret = socket->RecvFrom(buf_ptr, buf_len, flags, &native_from,
                             fromlen_ptr ? &native_fromlen : nullptr);

  if (from_ptr) {
    from_ptr->sin_family = native_from.sin_family;
    from_ptr->sin_port = native_from.sin_port;
    from_ptr->sin_addr = native_from.sin_addr;
    std::memset(from_ptr->x_sin_zero, 0, sizeof(from_ptr->x_sin_zero));
  }
  if (fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  if (ret == -1) {
    XThread::SetLastError(kWsaeWouldBlock);
    return static_cast<uint32_t>(-1);
  }
  XThread::SetLastError(0);
  return static_cast<uint32_t>(ret);
}

u32 NetDllSendToOverride(u32 caller, u32 socket_handle, mapped_u8 buf_ptr, u32 buf_len, u32 flags,
                         ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len) {
  (void)caller;
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(kWsaeNotSock);
    return static_cast<uint32_t>(-1);
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  int ret = socket->SendTo(buf_ptr, buf_len, flags, &native_to, to_len);
  XThread::SetLastError(0);
  if (ret == -1) {
    return static_cast<uint32_t>(-1);
  }
  return static_cast<uint32_t>(ret);
}

}  // namespace

// Link-time replacements of the import thunks the recompiled game calls.
REX_HOOK(__imp__XamUserCheckPrivilege, XamUserCheckPrivilegeOverride)
REX_HOOK(__imp__XMsgInProcessCall, XMsgInProcessCallOverride)
REX_HOOK(__imp__XMsgStartIORequestEx, XMsgStartIORequestExOverride)
REX_HOOK(__imp__XMsgStartIORequest, XMsgStartIORequestOverride)
REX_HOOK(__imp__XMsgCancelIORequest, XMsgCancelIORequestOverride)
REX_HOOK(__imp__XMsgCompleteIORequest, XMsgCompleteIORequestOverride)
REX_HOOK(__imp__XamGetOverlappedResult, XamGetOverlappedResultOverride)

REX_HOOK(__imp__NetDll_XNetGetTitleXnAddr, NetDllXNetGetTitleXnAddrOverride)
REX_HOOK(__imp__NetDll_XNetXnAddrToMachineId, NetDllXNetXnAddrToMachineIdOverride)
REX_HOOK(__imp__NetDll_XNetInAddrToString, NetDllXNetInAddrToStringOverride)
REX_HOOK(__imp__NetDll_XNetXnAddrToInAddr, NetDllXNetXnAddrToInAddrOverride)
REX_HOOK(__imp__NetDll_XNetInAddrToXnAddr, NetDllXNetInAddrToXnAddrOverride)
REX_HOOK(__imp__NetDll_XNetQosServiceLookup, NetDllXNetQosServiceLookupOverride)
REX_HOOK(__imp__NetDll_XNetQosListen, NetDllXNetQosListenOverride)
REX_HOOK(__imp__NetDll_XNetGetEthernetLinkStatus, NetDllXNetGetEthernetLinkStatusOverride)
REX_HOOK(__imp__NetDll_recvfrom, NetDllRecvFromOverride)
REX_HOOK(__imp__NetDll_sendto, NetDllSendToOverride)
