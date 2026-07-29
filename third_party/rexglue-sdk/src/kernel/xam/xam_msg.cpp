/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xio.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#include <atomic>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(xam_message_trace, false, "Kernel",
                    "Trace XAM message dispatch and overlapped state")
    .debug_only();

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

namespace {
std::atomic<uint64_t> g_xmsg_sequence{0};
std::mutex g_overlap_trace_mutex;
std::unordered_map<uint32_t, uint32_t> g_last_overlap_result;
std::unordered_map<uint32_t, uint64_t> g_overlap_poll_count;

struct XMsgTraceContext {
  uint32_t thread_id = 0;
  uint32_t lr = 0;
  uint32_t ctr = 0;
  uint32_t r1 = 0;
  uint32_t r3 = 0;
};

XMsgTraceContext CaptureXMsgTraceContext() {
  XMsgTraceContext result;
  auto* thread_state = rex::runtime::ThreadState::Get();
  if (!thread_state || !thread_state->context()) {
    return result;
  }

  auto* ctx = thread_state->context();
  result.thread_id = thread_state->thread_id();
  result.lr = static_cast<uint32_t>(ctx->lr);
  result.ctr = ctx->ctr.u32;
  result.r1 = ctx->r1.u32;
  result.r3 = ctx->r3.u32;
  return result;
}

void TraceOverlappedState(const char* phase, uint64_t sequence, uint32_t overlapped_ptr) {
  if (!REXCVAR_GET(xam_message_trace)) {
    return;
  }
  if (!overlapped_ptr) {
    REXKRNL_INFO("[OVERLAPTRACE] {} seq={} ptr=00000000", phase, sequence);
    return;
  }
  auto* overlapped = REX_KERNEL_MEMORY()->TranslateVirtual<XAM_OVERLAPPED*>(overlapped_ptr);
  if (!overlapped) {
    REXKRNL_INFO("[OVERLAPTRACE] {} seq={} ptr={:08X} invalid=1", phase, sequence,
                 overlapped_ptr);
    return;
  }
  const uint32_t result = static_cast<uint32_t>(overlapped->result);
  const uint32_t length = static_cast<uint32_t>(overlapped->length);
  const uint32_t extended_error = static_cast<uint32_t>(overlapped->extended_error);
  const uint32_t event = static_cast<uint32_t>(overlapped->event);
  const uint32_t completion = static_cast<uint32_t>(overlapped->completion_routine);
  const uint32_t context = static_cast<uint32_t>(overlapped->completion_context);
  REXKRNL_INFO(
      "[OVERLAPTRACE] {} seq={} ptr={:08X} result={:08X} extended={:08X} length={} event={:08X} completion={:08X} context={:08X}",
      phase, sequence, overlapped_ptr, result, extended_error, length, event, completion,
      context);
}
}  // namespace

u32 XMsgInProcessCall_entry(u32 app, u32 message, u32 arg1, u32 arg2) {
  const bool trace = REXCVAR_GET(xam_message_trace);
  const uint64_t sequence = trace ? ++g_xmsg_sequence : 0;
  if (trace) {
    const auto guest_ctx = CaptureXMsgTraceContext();
    REXKRNL_INFO(
        "[XMSGTRACE] ENTER sync seq={} thid={} lr={:08X} ctr={:08X} r1={:08X} r3={:08X} app={:08X} message={:08X} arg1={:08X} arg2={}",
        sequence, guest_ctx.thread_id, guest_ctx.lr, guest_ctx.ctr, guest_ctx.r1, guest_ctx.r3,
        static_cast<uint32_t>(app), static_cast<uint32_t>(message), static_cast<uint32_t>(arg1),
        static_cast<uint32_t>(arg2));
  }
  auto result = REX_KERNEL_STATE()->app_manager()->DispatchMessageSync(app, message, arg1, arg2);
  if (trace) {
    REXKRNL_INFO("[XMSGTRACE] EXIT  sync seq={} app={:08X} message={:08X} result={:08X}",
                 sequence, static_cast<uint32_t>(app), static_cast<uint32_t>(message),
                 static_cast<uint32_t>(result));
  }
  if (result == X_ERROR_NOT_FOUND) {
    REXKRNL_ERROR("XMsgInProcessCall: app {:08X} undefined", app);
  }
  return result;
}

u32 XMsgSystemProcessCall_entry(u32 app, u32 message, u32 buffer, u32 buffer_length) {
  const bool trace = REXCVAR_GET(xam_message_trace);
  const uint64_t sequence = trace ? ++g_xmsg_sequence : 0;
  if (trace) {
    const auto guest_ctx = CaptureXMsgTraceContext();
    REXKRNL_INFO(
        "[XMSGTRACE] ENTER system seq={} thid={} lr={:08X} ctr={:08X} r1={:08X} r3={:08X} app={:08X} message={:08X} buffer={:08X} length={}",
        sequence, guest_ctx.thread_id, guest_ctx.lr, guest_ctx.ctr, guest_ctx.r1, guest_ctx.r3,
        static_cast<uint32_t>(app), static_cast<uint32_t>(message), static_cast<uint32_t>(buffer),
        static_cast<uint32_t>(buffer_length));
  }
  auto result =
      REX_KERNEL_STATE()->app_manager()->DispatchMessageAsync(app, message, buffer, buffer_length);
  if (trace) {
    REXKRNL_INFO("[XMSGTRACE] EXIT  system seq={} app={:08X} message={:08X} result={:08X}",
                 sequence, static_cast<uint32_t>(app), static_cast<uint32_t>(message),
                 static_cast<uint32_t>(result));
  }
  if (result == X_ERROR_NOT_FOUND) {
    REXKRNL_ERROR("XMsgSystemProcessCall: app {:08X} undefined", app);
  }
  return result;
}

struct XMSGSTARTIOREQUEST_UNKNOWNARG {
  be<uint32_t> unk_0;
  be<uint32_t> unk_1;
};

X_HRESULT xeXMsgStartIORequestEx(uint32_t app, uint32_t message, uint32_t overlapped_ptr,
                                 uint32_t buffer_ptr, uint32_t buffer_length,
                                 XMSGSTARTIOREQUEST_UNKNOWNARG* unknown) {
  const bool trace = REXCVAR_GET(xam_message_trace);
  const uint64_t sequence = trace ? ++g_xmsg_sequence : 0;
  if (trace) {
    const auto guest_ctx = CaptureXMsgTraceContext();
    REXKRNL_INFO(
        "[XMSGTRACE] ENTER io seq={} thid={} lr={:08X} ctr={:08X} r1={:08X} r3={:08X} app={:08X} message={:08X} overlapped={:08X} buffer={:08X} length={} unknown={:08X}",
        sequence, guest_ctx.thread_id, guest_ctx.lr, guest_ctx.ctr, guest_ctx.r1, guest_ctx.r3,
        app, message, overlapped_ptr, buffer_ptr, buffer_length,
        unknown ? REX_KERNEL_MEMORY()->HostToGuestVirtual(unknown) : 0u);
  }
  TraceOverlappedState("BEFORE", sequence, overlapped_ptr);
  auto result = REX_KERNEL_STATE()->app_manager()->DispatchMessageAsync(app, message, buffer_ptr,
                                                                        buffer_length);
  if (trace) {
    REXKRNL_INFO("[XMSGTRACE] DISPATCH io seq={} app={:08X} message={:08X} result={:08X}",
                 sequence, app, message, static_cast<uint32_t>(result));
  }
  if (result == X_E_NOTFOUND) {
    REXKRNL_ERROR("XMsgStartIORequestEx: app {:08X} undefined", app);
    result = X_E_INVALIDARG;
    XThread::SetLastError(X_ERROR_NOT_FOUND);
  }
  if (overlapped_ptr) {
    if (trace) {
      REXKRNL_INFO("[OVERLAPTRACE] COMPLETE-IMMEDIATE seq={} ptr={:08X} completion_result={:08X}",
                   sequence, overlapped_ptr, static_cast<uint32_t>(result));
    }
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, result);
    TraceOverlappedState("AFTER-COMPLETE", sequence, overlapped_ptr);
    result = X_ERROR_IO_PENDING;
  }
  if (result == X_ERROR_SUCCESS || result == X_ERROR_IO_PENDING) {
    XThread::SetLastError(0);
  }
  if (trace) {
    REXKRNL_INFO("[XMSGTRACE] EXIT  io seq={} app={:08X} message={:08X} return={:08X}",
                 sequence, app, message, static_cast<uint32_t>(result));
  }
  return result;
}

u32 XMsgStartIORequestEx_entry(u32 app, u32 message, ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr,
                               u32 buffer_ptr, u32 buffer_length,
                               ppc_ptr_t<XMSGSTARTIOREQUEST_UNKNOWNARG> unknown_ptr) {
  return xeXMsgStartIORequestEx(app, message, overlapped_ptr.guest_address(), buffer_ptr,
                                buffer_length, unknown_ptr);
}

u32 XMsgStartIORequest_entry(u32 app, u32 message, ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr,
                             u32 buffer_ptr, u32 buffer_length) {
  return xeXMsgStartIORequestEx(app, message, overlapped_ptr.guest_address(), buffer_ptr,
                                buffer_length, nullptr);
}

u32 XMsgCancelIORequest_entry(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr, u32 wait) {
  X_HANDLE event_handle = XOverlappedGetEvent(overlapped_ptr);
  if (event_handle && wait) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    if (ev) {
      ev->Wait(0, 0, true, nullptr);
    }
  }

  return 0;
}

u32 XMsgCompleteIORequest_entry(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr, u32 result,
                                u32 extended_error, u32 length) {
  const bool trace = REXCVAR_GET(xam_message_trace);
  const uint64_t sequence = trace ? ++g_xmsg_sequence : 0;
  if (trace) {
    REXKRNL_INFO("[OVERLAPTRACE] XMsgCompleteIORequest seq={} ptr={:08X} result={:08X} extended={:08X} length={}",
                 sequence, overlapped_ptr.guest_address(), static_cast<uint32_t>(result),
                 static_cast<uint32_t>(extended_error), static_cast<uint32_t>(length));
  }
  TraceOverlappedState("BEFORE-EXPLICIT-COMPLETE", sequence, overlapped_ptr.guest_address());
  REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(overlapped_ptr.guest_address(), result,
                                                    extended_error, length);
  TraceOverlappedState("AFTER-EXPLICIT-COMPLETE", sequence, overlapped_ptr.guest_address());
  return X_ERROR_SUCCESS;
}

u32 XamGetOverlappedResult_entry(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr, mapped_u32 length_ptr,
                                 u32 unknown) {
  const uint32_t guest_ptr = overlapped_ptr.guest_address();
  uint32_t result;
  if (overlapped_ptr->result != X_ERROR_IO_PENDING) {
    result = overlapped_ptr->result;
  } else if (!overlapped_ptr->event) {
    result = X_ERROR_IO_INCOMPLETE;
  } else {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(overlapped_ptr->event);
    result = ev->Wait(3, 1, 0, nullptr);
    if (XSUCCEEDED(result)) {
      result = overlapped_ptr->result;
    } else {
      result = xboxkrnl::xeRtlNtStatusToDosError(result);
    }
  }
  if (XSUCCEEDED(result) && length_ptr) {
    *length_ptr = overlapped_ptr->length;
  }

  if (REXCVAR_GET(xam_message_trace)) {
    bool should_log = false;
    uint64_t poll = 0;
    {
      std::scoped_lock lock(g_overlap_trace_mutex);
      poll = ++g_overlap_poll_count[guest_ptr];
      const auto previous = g_last_overlap_result.find(guest_ptr);
      if (previous == g_last_overlap_result.end() || previous->second != result || poll <= 8 ||
          (poll % 300) == 0) {
        should_log = true;
        g_last_overlap_result[guest_ptr] = result;
      }
    }
    if (should_log) {
      const uint32_t stored_result = static_cast<uint32_t>(overlapped_ptr->result);
      const uint32_t stored_length = static_cast<uint32_t>(overlapped_ptr->length);
      const uint32_t stored_event = static_cast<uint32_t>(overlapped_ptr->event);
      REXKRNL_INFO(
          "[OVERLAPTRACE] GET poll={} ptr={:08X} return={:08X} stored={:08X} length={} event={:08X} wait={} length_ptr={:08X}",
          poll, guest_ptr, result, stored_result, stored_length, stored_event,
          static_cast<uint32_t>(unknown), length_ptr.guest_address());
    }
  }
  return result;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XMsgInProcessCall, rex::kernel::xam::XMsgInProcessCall_entry)
REX_EXPORT(__imp__XMsgSystemProcessCall, rex::kernel::xam::XMsgSystemProcessCall_entry)
REX_EXPORT(__imp__XMsgStartIORequestEx, rex::kernel::xam::XMsgStartIORequestEx_entry)
REX_EXPORT(__imp__XMsgStartIORequest, rex::kernel::xam::XMsgStartIORequest_entry)
REX_EXPORT(__imp__XMsgCancelIORequest, rex::kernel::xam::XMsgCancelIORequest_entry)
REX_EXPORT(__imp__XMsgCompleteIORequest, rex::kernel::xam::XMsgCompleteIORequest_entry)
REX_EXPORT(__imp__XamGetOverlappedResult, rex::kernel::xam::XamGetOverlappedResult_entry)

REX_EXPORT_STUB(__imp__XMsgAcquireAsyncMessageFromOverlapped);
REX_EXPORT_STUB(__imp__XMsgProcessRequest);
REX_EXPORT_STUB(__imp__XMsgReleaseAsyncMessageToOverlapped);
