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

#include <atomic>
#include <array>
#include <cstring>
#include <mutex>

#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

#pragma GCC diagnostic ignored "-Wunused-parameter"

namespace rex {
namespace kernel {
namespace xam {

// This translation unit primarily contains exports that Generations resolves
// dynamically with XexGetProcedureAddress. Because rexruntime_static is a static
// library, the linker may otherwise discard this object file before the REX_EXPORT
// registration objects run. XamModule calls this no-op function to force the
// complete party export translation unit into the final executable.
void ForceLinkPartyExports() {}

namespace {
constexpr u32 kPartyUnavailable = 0x807D0003u;
std::mutex g_party_mutex;
std::array<uint8_t, 256> g_party_custom_data{};
u32 g_party_custom_data_size = 0;

void CompletePartyCall(const char* function_name) {
  static std::atomic_uint32_t call_count{0};
  const uint32_t index = call_count.fetch_add(1, std::memory_order_relaxed);
  if (index < 16 || (index % 600) == 0) {
    REXKRNL_INFO("[XEXONLINE][PARTY] {} -> local System Link service (call={})",
                 function_name, index + 1);
  }
}

void LogPartyOfflineOnce(const char* function_name) {
  // Generations may poll these functions every frame. Keep diagnostics useful
  // without turning an unsupported offline path into a logging stall.
  static std::atomic_uint32_t warning_count{0};
  const uint32_t index = warning_count.fetch_add(1, std::memory_order_relaxed);
  if (index < 32) {
    REXKRNL_WARN("{}: Xbox Live party service unavailable; returning safe offline state",
                 function_name);
  } else if (index == 32) {
    REXKRNL_WARN("Further Xbox Live party offline warnings are suppressed");
  }
}
}  // namespace

u32 XamPartyGetUserList_entry(u32 player_count, mapped_u32 party_list) {
  // Generations only needs a stable local-party state before a network session
  // has populated its own XSession member list. Report no dashboard-party
  // members rather than an Xbox Live service failure.
  if (party_list) {
    *party_list = 0;
  }
  CompletePartyCall("XamPartyGetUserList");
  return X_ERROR_SUCCESS;
}

u32 XamPartySendGameInvites_entry(u32 user_index, u32 xuids_ptr, u32 xuid_count) {
  // Generations imports ordinal 0xB00 even for System Link. Returning
  // NOT_LOGGED_ON makes the title treat the local session as an unavailable
  // Xbox Live lobby. Keep the call local-only, but report successful dispatch
  // so the invite/menu state machine can continue without an external service.
  REXKRNL_INFO(
      "[XEXONLINE][PARTY] XamPartySendGameInvites user={} xuids={:08X} count={} -> local System Link success",
      user_index, xuids_ptr, xuid_count);
  return X_ERROR_SUCCESS;
}

u32 XamPartySetCustomData_entry(u32 user_index, u32 data_ptr, u32 data_size) {
  if (user_index >= 4 || data_size > g_party_custom_data.size() ||
      (data_size && !data_ptr)) {
    return X_ERROR_INVALID_PARAMETER;
  }
  std::scoped_lock lock(g_party_mutex);
  g_party_custom_data.fill(0);
  g_party_custom_data_size = data_size;
  if (data_size) {
    auto* source = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(data_ptr);
    if (!source) {
      g_party_custom_data_size = 0;
      return X_ERROR_INVALID_PARAMETER;
    }
    std::memcpy(g_party_custom_data.data(), source, data_size);
  }
  REXKRNL_INFO("[XEXONLINE][PARTY] custom data stored user={} size={}",
               user_index, data_size);
  return X_ERROR_SUCCESS;
}

u32 XamPartyGetBandwidth_entry(u32 user_index, mapped_u32 bandwidth_out) {
  if (user_index >= 4 || !bandwidth_out) {
    return X_ERROR_INVALID_PARAMETER;
  }
  // A conservative 10 Mbit/s value is enough for the title's lobby gating and
  // avoids pretending that the unavailable Xbox Live bandwidth probe failed.
  *bandwidth_out = 10u * 1000u * 1000u;
  CompletePartyCall("XamPartyGetBandwidth");
  return X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamPartyGetUserList, rex::kernel::xam::XamPartyGetUserList_entry)
REX_EXPORT(__imp__XamPartySendGameInvites, rex::kernel::xam::XamPartySendGameInvites_entry)
REX_EXPORT(__imp__XamPartySetCustomData, rex::kernel::xam::XamPartySetCustomData_entry)
REX_EXPORT(__imp__XamPartyGetBandwidth, rex::kernel::xam::XamPartyGetBandwidth_entry)

// Never leave r3 unchanged for unsupported party calls. Many games immediately
// branch on the return value, and an inherited guest register can look like
// success and send execution into an unimplemented Xbox Live path.
REX_EXPORT_STUB_RETURN(__imp__XamPartyAddLocalUsers, 0x000004DDu);
REX_EXPORT_STUB_RETURN(__imp__XamPartyAutomationInprocCall, 0x000004DDu);
REX_EXPORT_STUB_RETURN(__imp__XamPartyCreate, 0x000004DDu);
REX_EXPORT_STUB_RETURN(__imp__XamPartyGetAccessLevel, 0x807D0003u);
REX_EXPORT_STUB_RETURN(__imp__XamPartyGetFormation, 0x807D0003u);
REX_EXPORT_STUB_RETURN(__imp__XamPartyGetInfo, 0x807D0003u);
REX_EXPORT_STUB_RETURN(__imp__XamPartyGetInfoEx, 0x807D0003u);
REX_EXPORT_STUB_RETURN(__imp__XamPartyGetJoinable, 0x807D0003u);
REX_EXPORT_STUB_RETURN(__imp__XamPartyGetNetworkCounters, 0x807D0003u);
REX_EXPORT_STUB_RETURN(__imp__XamPartyGetRoutingTable, 0x807D0003u);
REX_EXPORT_STUB_RETURN(__imp__XamPartyGetState, 0x807D0003u);
REX_EXPORT_STUB_RETURN(__imp__XamPartyGetUserListInternal, 0x807D0003u);
REX_EXPORT_STUB_RETURN(__imp__XamPartyIsCoordinator, 0u);
REX_EXPORT_STUB_RETURN(__imp__XamPartyJoin, 0x000004DDu);
REX_EXPORT_STUB_RETURN(__imp__XamPartyJoinEx, 0x000004DDu);
REX_EXPORT_STUB_RETURN(__imp__XamPartyKickUser, 0x000004DDu);
REX_EXPORT_STUB_RETURN(__imp__XamPartyLeave, 0x00000000u);
REX_EXPORT_STUB_RETURN(__imp__XamPartyOverrideNatType, 0x000004DDu);
REX_EXPORT_STUB_RETURN(__imp__XamPartyRemoveLocalUsers, 0x00000000u);
REX_EXPORT_STUB_RETURN(__imp__XamPartySendInvite, 0x000004DDu);
REX_EXPORT_STUB_RETURN(__imp__XamPartySendInviteDeprecated, 0x000004DDu);
REX_EXPORT_STUB_RETURN(__imp__XamPartySetConnectivityGraph, 0x000004DDu);
REX_EXPORT_STUB_RETURN(__imp__XamPartySetJoinable, 0x000004DDu);
REX_EXPORT_STUB_RETURN(__imp__XamPartySetTestDelay, 0x00000000u);
REX_EXPORT_STUB_RETURN(__imp__XamPartySetTestFlags, 0x00000000u);
