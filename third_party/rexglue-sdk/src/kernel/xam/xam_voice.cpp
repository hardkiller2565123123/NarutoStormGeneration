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
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

#include <atomic>

namespace rex {
namespace kernel {
namespace xam {

namespace {

// Voice is intentionally exposed as unavailable in the offline runtime.  The
// game still polls a few microphone-array functions during startup, so those
// calls must complete deterministically without the generic STUB logger
// flooding the log or leaving r3 unchanged.
void LogVoiceOfflineOnce(const char* function_name, std::atomic_bool& logged) {
  bool expected = false;
  if (logged.compare_exchange_strong(expected, true,
                                     std::memory_order_relaxed)) {
    REXKRNL_INFO("{}: offline voice emulation active", function_name);
  }
}

std::atomic_bool g_logged_idle_users{false};
std::atomic_bool g_logged_submit_packet{false};
std::atomic_bool g_logged_capture_routine{false};

constexpr u32 kSuccess = 0x00000000u;
constexpr u32 kAccessDenied = 0x00000005u;

}  // namespace

u32 XamVoiceIsActiveProcess_entry() {
  // Returning false short-circuits the normal Xbox Live voice pipeline.
  return 0;
}

u32 XamVoiceCreate_entry(u32 unk1,  // 0
                         u32 unk2,  // 0xF
                         mapped_u32 out_voice_ptr) {
  // Never return an uninitialized guest voice object.
  if (out_voice_ptr) {
    out_voice_ptr.Zero();
  }
  return kAccessDenied;
}

u32 XamVoiceClose_entry(mapped_void voice_ptr) { return kSuccess; }

u32 XamVoiceHeadsetPresent_entry(mapped_void voice_ptr) {
  // No emulated headset/microphone is attached.
  return 0;
}

// Microphone-array control calls are harmless no-ops while voice is disabled.
u32 XamVoiceDisableMicArray_entry(u32 user_index) { return kSuccess; }
u32 XamVoiceMuteMicArray_entry(u32 mute) { return kSuccess; }
u32 XamVoiceSetMicArrayBeamAngle_entry(u32 angle) { return kSuccess; }

u32 XamVoiceSetMicArrayIdleUsers_entry(u32 idle_user_mask) {
  LogVoiceOfflineOnce("XamVoiceSetMicArrayIdleUsers", g_logged_idle_users);
  return kSuccess;
}

// Getters consistently report no hardware/data. The first parameter is kept
// generic because titles use multiple dashboard-era signatures for these APIs.
u32 XamVoiceGetBatteryStatus_entry(u32 r3, u32 r4, u32 r5, u32 r6) {
  return 0;
}

u32 XamVoiceGetDirectionalData_entry(u32 r3, u32 r4, u32 r5, u32 r6) {
  return 0;
}

u32 XamVoiceGetMicArrayAudio_entry(u32 r3, u32 r4, u32 r5, u32 r6) {
  return 0;
}

u32 XamVoiceGetMicArrayAudioEx_entry(u32 r3, u32 r4, u32 r5, u32 r6) {
  return 0;
}

u32 XamVoiceGetMicArrayFilenameDesc_entry(u32 r3, u32 r4, u32 r5,
                                          u32 r6) {
  return 0;
}

u32 XamVoiceGetMicArrayStatus_entry(u32 r3, u32 r4, u32 r5, u32 r6) {
  return 0;
}

u32 XamVoiceGetMicArrayUnderrunStatus_entry(u32 r3, u32 r4, u32 r5,
                                            u32 r6) {
  return 0;
}

u32 XamVoiceRecordUserPrivileges_entry(u32 r3, u32 r4, u32 r5, u32 r6) {
  return kSuccess;
}

u32 XamVoiceSetAudioCaptureRoutine_entry(u32 r3, u32 r4, u32 r5, u32 r6) {
  LogVoiceOfflineOnce("XamVoiceSetAudioCaptureRoutine",
                      g_logged_capture_routine);
  return kSuccess;
}

u32 XamVoiceSubmitPacket_entry(u32 r3, u32 r4, u32 r5, u32 r6) {
  LogVoiceOfflineOnce("XamVoiceSubmitPacket", g_logged_submit_packet);
  return kSuccess;
}

u32 XamMuteSound_entry(u32 r3, u32 r4, u32 r5, u32 r6) {
  return kSuccess;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamVoiceIsActiveProcess,
           rex::kernel::xam::XamVoiceIsActiveProcess_entry)
REX_EXPORT(__imp__XamVoiceCreate, rex::kernel::xam::XamVoiceCreate_entry)
REX_EXPORT(__imp__XamVoiceClose, rex::kernel::xam::XamVoiceClose_entry)
REX_EXPORT(__imp__XamVoiceHeadsetPresent,
           rex::kernel::xam::XamVoiceHeadsetPresent_entry)
REX_EXPORT(__imp__XamMuteSound, rex::kernel::xam::XamMuteSound_entry)
REX_EXPORT(__imp__XamVoiceDisableMicArray,
           rex::kernel::xam::XamVoiceDisableMicArray_entry)
REX_EXPORT(__imp__XamVoiceGetBatteryStatus,
           rex::kernel::xam::XamVoiceGetBatteryStatus_entry)
REX_EXPORT(__imp__XamVoiceGetDirectionalData,
           rex::kernel::xam::XamVoiceGetDirectionalData_entry)
REX_EXPORT(__imp__XamVoiceGetMicArrayAudio,
           rex::kernel::xam::XamVoiceGetMicArrayAudio_entry)
REX_EXPORT(__imp__XamVoiceGetMicArrayAudioEx,
           rex::kernel::xam::XamVoiceGetMicArrayAudioEx_entry)
REX_EXPORT(__imp__XamVoiceGetMicArrayFilenameDesc,
           rex::kernel::xam::XamVoiceGetMicArrayFilenameDesc_entry)
REX_EXPORT(__imp__XamVoiceGetMicArrayStatus,
           rex::kernel::xam::XamVoiceGetMicArrayStatus_entry)
REX_EXPORT(__imp__XamVoiceGetMicArrayUnderrunStatus,
           rex::kernel::xam::XamVoiceGetMicArrayUnderrunStatus_entry)
REX_EXPORT(__imp__XamVoiceMuteMicArray,
           rex::kernel::xam::XamVoiceMuteMicArray_entry)
REX_EXPORT(__imp__XamVoiceRecordUserPrivileges,
           rex::kernel::xam::XamVoiceRecordUserPrivileges_entry)
REX_EXPORT(__imp__XamVoiceSetAudioCaptureRoutine,
           rex::kernel::xam::XamVoiceSetAudioCaptureRoutine_entry)
REX_EXPORT(__imp__XamVoiceSetMicArrayBeamAngle,
           rex::kernel::xam::XamVoiceSetMicArrayBeamAngle_entry)
REX_EXPORT(__imp__XamVoiceSetMicArrayIdleUsers,
           rex::kernel::xam::XamVoiceSetMicArrayIdleUsers_entry)
REX_EXPORT(__imp__XamVoiceSubmitPacket,
           rex::kernel::xam::XamVoiceSubmitPacket_entry)
