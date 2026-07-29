#include "network_handshake_patch.h"

#include <array>
#include <chrono>

#include <generations_init.h>
#include <rex/logging.h>

namespace generations {
namespace {

constexpr uint32_t kInputManagerPointer = 0x82A1F948;
constexpr uint32_t kConfirmMaskOffset = 452;
constexpr uint32_t kDefaultConfirmMask = 0x1000;
constexpr uint32_t kConnectionSocketOffset = 4;
constexpr uint32_t kConnectionReceiveBufferOffset = 24;
constexpr uint32_t kConnectionReceiveCountOffset = 1048;
constexpr uint32_t kConnectionReceiveCapacity = 1024;
constexpr std::array<uint8_t, 24> kSystemLinkCompatibilityHandshake = {
    0x47, 0x45, 0x4E, 0x53,  // "GENS"
    0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x18,
    0x53, 0x59, 0x53, 0x4C,  // "SYSL"
    0x49, 0x4E, 0x4B, 0x00,  // "INK"
    0x00, 0x00, 0x00, 0x01,
};

uint32_t active_flow_object = 0;
bool handshake_patch_activation_logged = false;
bool handshake_confirm_latched = false;
uint32_t handshake_confirm_retry_count = 0;
constexpr uint32_t kHandshakeConfirmMaxAttempts = 40;
constexpr auto kHandshakeConfirmRetryInterval = std::chrono::milliseconds(200);
std::chrono::steady_clock::time_point handshake_last_confirm_time{};
uint32_t last_flow_state = UINT32_MAX;
uint32_t last_transport_state = UINT32_MAX;

// Promotion bookkeeping is deliberately observational and per-lobby. The XEX owns
// player objects and controller pointers; this patch never restores stale guest
// pointers or fabricates member lifetime.
struct PromotionState {
  uint32_t lobby = 0;
  uint64_t member_signature = 0;
  bool applied = false;
};

std::array<PromotionState, 8> promotion_states{};
size_t next_promotion_state_slot = 0;
std::chrono::steady_clock::time_point last_valid_remote_sync{};
uint64_t last_valid_remote_identity = 0;
constexpr auto kRemoteSyncFreshWindow = std::chrono::seconds(5);

struct TransportTraceState {
  uint32_t object = 0;
  uint32_t state = UINT32_MAX;
  uint32_t caller = 0;
};

std::array<TransportTraceState, 8> transport_trace_states{};
size_t next_transport_trace_slot = 0;

struct LobbyTraceState {
  uint32_t object = 0;
  uint32_t site = 0;
  std::array<uint32_t, 12> values{};
  bool initialized = false;
};

std::array<LobbyTraceState, 16> lobby_trace_states{};
size_t next_lobby_trace_slot = 0;

std::array<uint32_t, 12> ReadLobbyTraceValues(uint8_t* base, uint32_t object) {
  return {
      REX_LOAD_U32(object + 5828), REX_LOAD_U32(object + 5836),
      REX_LOAD_U32(object + 5840), REX_LOAD_U32(object + 5844),
      REX_LOAD_U32(object + 5852), REX_LOAD_U32(object + 5856),
      REX_LOAD_U32(object + 5864), REX_LOAD_U32(object + 5872),
      REX_LOAD_U32(object + 5876), REX_LOAD_U32(object + 5908),
      REX_LOAD_U32(object + 5912), REX_LOAD_U32(object + 5964),
  };
}

}  // namespace

void LatchOnlineHandshakeConfirm(PPCContext& context, uint8_t* base,
                                 uint32_t flow_object) {
  if (!handshake_patch_activation_logged) {
    handshake_patch_activation_logged = true;
    REXLOG_INFO(
        "Generations native online handshake patch active "
        "(flow hook=8222FEE8, transport hooks=8228Bxxx)");
  }

  if (!flow_object) {
    return;
  }

  uint32_t flow_state = REX_LOAD_U32(flow_object + 48);
  const uint32_t transport = REX_LOAD_U32(flow_object + 272);
  const uint32_t transport_state = transport ? REX_LOAD_U32(transport + 16) : 0;

  if (flow_object != active_flow_object) {
    active_flow_object = flow_object;
    handshake_confirm_latched = false;
    handshake_confirm_retry_count = 0;
    handshake_last_confirm_time = {};
    last_flow_state = UINT32_MAX;
    last_transport_state = UINT32_MAX;
  }

  if (flow_state != last_flow_state || transport_state != last_transport_state) {
    REXLOG_INFO(
        "Online title flow transition: flow={:08X} state={} transport={:08X} "
        "transport_state={}",
        flow_object, flow_state, transport, transport_state);
    last_flow_state = flow_state;
    last_transport_state = transport_state;
  }

  const bool flow_waiting_for_handshake =
      flow_state == 1 || flow_state == 2;
  // Different peers can reach this flow one frame before the transport state
  // is promoted to 2/3. Once a real transport object and socket exist, keep
  // restoring the confirm edge through states 0-3 instead of missing the only
  // frame in which Generations builds its native 24-byte handshake.
  const uint32_t transport_socket = transport ? REX_LOAD_U32(transport + 12) : 0;
  const bool transport_protocol_ready =
      transport && transport_socket && transport_state <= 3;
  if (!flow_waiting_for_handshake || !transport_protocol_ready) {
    handshake_confirm_latched = false;
    handshake_confirm_retry_count = 0;
    handshake_last_confirm_time = {};
    return;
  }

  // The selection-frame confirm edge may be consumed before the nonblocking
  // connect reaches the title's send branch. Re-apply it for the full native
  // handshake timeout. This is still bounded, and it preserves the title's
  // own packet construction rather than fabricating protocol data in XAM.
  const auto now = std::chrono::steady_clock::now();
  if (handshake_confirm_latched &&
      handshake_confirm_retry_count >= kHandshakeConfirmMaxAttempts) {
    return;
  }
  if (handshake_confirm_latched &&
      handshake_last_confirm_time.time_since_epoch().count() != 0 &&
      now - handshake_last_confirm_time < kHandshakeConfirmRetryInterval) {
    return;
  }

  const uint32_t input_record = REX_LOAD_U32(flow_object + 256);
  if (!input_record) {
    // A connected socket is not proof that the title handshake completed.
    // Advancing the flow here bypassed native packet validation and could leave
    // the frontend and session layers in contradictory states. Wait for either
    // a real input edge or CompleteOnlineHandshakeOnReceive instead.
    return;
  }

  const uint32_t input_manager = REX_LOAD_U32(kInputManagerPointer);
  uint32_t confirm_mask = input_manager ? REX_LOAD_U32(input_manager + kConfirmMaskOffset) : 0;
  if (!confirm_mask) {
    confirm_mask = kDefaultConfirmMask;
  }

  const uint32_t pressed_buttons = REX_LOAD_U32(input_record + 16);
  REX_STORE_U32(input_record + 16, pressed_buttons | confirm_mask);

  // accept()/connect() completes after the selection-frame button edge has
  // already been sampled. Advance to the title's send state now so this same
  // invocation consumes the restored edge and emits its native packet.
  if (flow_state == 1) {
    flow_state = 2;
    REX_STORE_U32(flow_object + 48, flow_state);
  }

  handshake_confirm_latched = true;
  handshake_last_confirm_time = now;
  ++handshake_confirm_retry_count;
  REXLOG_INFO(
      "Online transport protocol-ready; latched native handshake confirm "
      "(flow={:08X}, mask={:04X}, attempt={}/{}, transport_state={}, socket={:08X})",
      flow_object, confirm_mask, handshake_confirm_retry_count,
      kHandshakeConfirmMaxAttempts, transport_state, transport_socket);
}

void CompleteOnlineHandshakeOnReceive(uint8_t* base, uint32_t flow_object,
                                      uint32_t packet_size,
                                      uint32_t framed_size) {
  constexpr uint32_t kHandshakePacketSize = 24;

  if (!flow_object || REX_LOAD_U32(flow_object + 48) != 2 ||
      packet_size != kHandshakePacketSize ||
      framed_size != kHandshakePacketSize) {
    return;
  }

  REX_STORE_U32(flow_object + 48, 3);
  handshake_confirm_latched = false;
  handshake_confirm_retry_count = 0;
  handshake_last_confirm_time = {};
  REXLOG_INFO(
      "Online peer handshake received ({} bytes); advancing native title "
      "flow to completion (flow={:08X})",
      packet_size, flow_object);
}

void TraceOnlineTransportPoll(uint8_t* base, uint32_t transport_object,
                              uint32_t caller_address) {
  if (!transport_object) {
    return;
  }

  TransportTraceState* trace = nullptr;
  for (auto& candidate : transport_trace_states) {
    if (candidate.object == transport_object) {
      trace = &candidate;
      break;
    }
  }
  if (!trace) {
    trace = &transport_trace_states[next_transport_trace_slot++ %
                                    transport_trace_states.size()];
    *trace = {};
    trace->object = transport_object;
  }

  const uint32_t state = REX_LOAD_U32(transport_object + 16);
  if (trace->state == state && trace->caller == caller_address) {
    return;
  }

  trace->state = state;
  trace->caller = caller_address;
  REXLOG_INFO(
      "Online transport poll: object={:08X} caller={:08X} state={} phase={} "
      "listen={:08X} socket={:08X} peer={:08X} error={}",
      transport_object, caller_address, state,
      REX_LOAD_U32(transport_object + 20),
      REX_LOAD_U32(transport_object + 4),
      REX_LOAD_U32(transport_object + 12),
      REX_LOAD_U32(transport_object + 40),
      REX_LOAD_U32(transport_object + 44));
}

bool StripSystemLinkCompatibilityHandshake(uint8_t* base,
                                           uint32_t connection_object) {
  if (!connection_object) {
    return false;
  }

  const uint32_t buffered =
      REX_LOAD_U16(connection_object + kConnectionReceiveCountOffset);
  if (buffered < kSystemLinkCompatibilityHandshake.size() ||
      buffered > kConnectionReceiveCapacity) {
    return false;
  }

  const uint32_t buffer =
      connection_object + kConnectionReceiveBufferOffset;
  for (uint32_t i = 0; i < kSystemLinkCompatibilityHandshake.size(); ++i) {
    if (REX_LOAD_U8(buffer + i) != kSystemLinkCompatibilityHandshake[i]) {
      return false;
    }
  }

  const uint32_t remaining =
      buffered - static_cast<uint32_t>(kSystemLinkCompatibilityHandshake.size());
  for (uint32_t i = 0; i < remaining; ++i) {
    REX_STORE_U8(buffer + i,
                 REX_LOAD_U8(buffer +
                             static_cast<uint32_t>(
                                 kSystemLinkCompatibilityHandshake.size()) +
                             i));
  }
  for (uint32_t i = remaining; i < buffered; ++i) {
    REX_STORE_U8(buffer + i, 0);
  }
  REX_STORE_U16(connection_object + kConnectionReceiveCountOffset, remaining);

  REXLOG_INFO(
      "System Link parser stripped compatibility preamble "
      "(connection={:08X}, socket={:08X}, buffered={}->{})",
      connection_object,
      REX_LOAD_U32(connection_object + kConnectionSocketOffset), buffered,
      remaining);
  return true;
}

void TraceSystemLinkNativePacket(uint8_t* base, uint32_t connection_object,
                                 uint32_t packet_address,
                                 uint32_t packet_size) {
  if (!connection_object || !packet_address || packet_size < 4) return;

  const uint16_t channel = packet_size >= 8 ? REX_LOAD_U16(packet_address + 4) : 0;
  const uint16_t packet_type = packet_size >= 8 ? REX_LOAD_U16(packet_address + 6) : 0;
  const uint32_t word1 = packet_size >= 8 ? REX_LOAD_U32(packet_address + 4) : 0;
  const uint32_t word2 = packet_size >= 12 ? REX_LOAD_U32(packet_address + 8) : 0;
  const uint32_t word3 = packet_size >= 16 ? REX_LOAD_U32(packet_address + 12) : 0;
  const uint64_t identity = packet_size >= 24 ? REX_LOAD_U64(packet_address + 16) : 0;

  REXLOG_INFO(
      "System Link native packet dispatch "
      "(connection={:08X}, socket={:08X}, size={}, channel={}, type={}, "
      "raw={:08X}, word2={:08X}, word3={:08X}, identity={:016X})",
      connection_object, REX_LOAD_U32(connection_object + kConnectionSocketOffset),
      packet_size, channel, packet_type, word1, word2, word3, identity);

  // Type 6 is the observed member-state heartbeat. It is the only packet that
  // refreshes removal protection. Type 3 remains visible in logs but is never
  // suppressed blindly; it may represent a legitimate leave.
  if (packet_type == 6 && packet_size == 96 && identity != 0) {
    last_valid_remote_identity = identity;
    last_valid_remote_sync = std::chrono::steady_clock::now();
  }
}

void TraceSystemLinkLobbyState(uint8_t* base, uint32_t lobby_object,
                               uint32_t update_site,
                               uint32_t return_address) {
  if (!lobby_object) return;

  LobbyTraceState* trace = nullptr;
  for (auto& candidate : lobby_trace_states) {
    if (candidate.object == lobby_object && candidate.site == update_site) {
      trace = &candidate;
      break;
    }
  }
  if (!trace) {
    trace = &lobby_trace_states[next_lobby_trace_slot++ % lobby_trace_states.size()];
    *trace = {};
    trace->object = lobby_object;
    trace->site = update_site;
  }

  const auto values = ReadLobbyTraceValues(base, lobby_object);
  const uint32_t local_player = REX_LOAD_U32(lobby_object + 8204);

  // Refresh only the matching member heartbeat after a recently received type-6
  // packet. Never restore controller pointers, player count, mode, or teardown
  // flags from cached state; those objects are owned and rebuilt by the XEX.
  if (values[1] == 4 && values[11] >= 2 && local_player &&
      last_valid_remote_identity != 0 &&
      std::chrono::steady_clock::now() - last_valid_remote_sync <=
          kRemoteSyncFreshWindow) {
    const uint32_t local_tick = REX_LOAD_U32(local_player + 240);
    for (uint32_t slot = 0; slot < 8; ++slot) {
      const uint32_t player = lobby_object + 6152 + slot * 256;
      if (player == local_player) continue;
      if (REX_LOAD_U64(player + 120) != last_valid_remote_identity) continue;
      const uint32_t old_tick = REX_LOAD_U32(player + 240);
      if (local_tick && old_tick != local_tick) {
        REX_STORE_U32(player + 240, local_tick);
        REXLOG_INFO(
            "System Link refreshed native remote heartbeat "
            "(lobby={:08X}, player={:08X}, identity={:016X}, tick={}->{})",
            lobby_object, player, last_valid_remote_identity, old_tick, local_tick);
      }
      break;
    }
  }

  if (trace->initialized && trace->values == values) return;
  trace->values = values;
  trace->initialized = true;
  REXLOG_INFO(
      "System Link lobby state "
      "(object={:08X}, site={:08X}, return={:08X}, controller={}, mode={}, "
      "operation={}, join={}, disconnect={}/{}, remove={}, result={}/{}, "
      "active={}, response={}, players={}, host_player={:08X}, "
      "local_player={:08X}, network={:08X}, session={:08X})",
      lobby_object, update_site, return_address, values[0], values[1], values[2],
      values[3], values[4], values[5], values[6], values[7], values[8], values[9],
      values[10], values[11], REX_LOAD_U32(lobby_object + 8200),
      REX_LOAD_U32(lobby_object + 8204), REX_LOAD_U32(lobby_object + 8208),
      REX_LOAD_U32(lobby_object + 8212));
}

bool TraceSystemLinkLobbyTeardown(uint8_t* base, uint32_t lobby_object,
                                  uint32_t return_address) {
  if (!lobby_object) return false;
  const auto values = ReadLobbyTraceValues(base, lobby_object);
  REXLOG_INFO(
      "System Link lobby teardown requested "
      "(object={:08X}, return={:08X}, controller={}, mode={}, operation={}, "
      "join={}, disconnect={}/{}, remove={}, result={}/{}, active={}, "
      "response={}, players={}, host_player={:08X}, local_player={:08X}, "
      "network={:08X}, session={:08X})",
      lobby_object, return_address, values[0], values[1], values[2], values[3],
      values[4], values[5], values[6], values[7], values[8], values[9], values[10],
      values[11], REX_LOAD_U32(lobby_object + 8200),
      REX_LOAD_U32(lobby_object + 8204), REX_LOAD_U32(lobby_object + 8208),
      REX_LOAD_U32(lobby_object + 8212));
  // Never skip the native destructor based only on a snapshot. Earlier code
  // restored stale guest pointers here and caused deterministic host crashes.
  return false;
}

void TraceSystemLinkActivePump(uint8_t* base, uint32_t network_object,
                               uint32_t return_address) {
  static uint32_t last_network = 0;
  static uint32_t last_head = UINT32_MAX;
  static uint32_t last_count = UINT32_MAX;
  if (!network_object) return;
  const uint32_t vtable = REX_LOAD_U32(network_object);
  const uint32_t head = REX_LOAD_U32(network_object + 8);
  const uint32_t count = REX_LOAD_U32(network_object + 12);
  if (network_object != last_network || head != last_head || count != last_count) {
    REXLOG_INFO("System Link active network pump (network={:08X}, return={:08X}, vtable={:08X}, head={:08X}, count={})",
                network_object, return_address, vtable, head, count);
    last_network = network_object;
    last_head = head;
    last_count = count;
  }
}

void TraceSystemLinkHeartbeatApply(uint8_t* base, uint32_t lobby_object,
                                   uint32_t packet_object,
                                   uint32_t return_address) {
  if (!lobby_object || !packet_object) return;
  const uint32_t local = REX_LOAD_U32(lobby_object + 8204);
  const uint64_t incoming = REX_LOAD_U64(packet_object + 16);
  const uint64_t local_xuid = local ? REX_LOAD_U64(local + 120) : 0;
  if (incoming && incoming != local_xuid) {
    last_valid_remote_identity = incoming;
    last_valid_remote_sync = std::chrono::steady_clock::now();
  }
  REXLOG_INFO(
      "System Link heartbeat apply (lobby={:08X}, packet={:08X}, "
      "return={:08X}, incoming={:016X}, local={:016X})",
      lobby_object, packet_object, return_address, incoming, local_xuid);
}

bool ShouldSkipDuplicateSystemLinkMember(uint8_t* base, uint32_t lobby_object,
                                         uint32_t incoming_record,
                                         uint32_t return_address) {
  if (!lobby_object || !incoming_record) return false;

  const uint64_t incoming_xuid = REX_LOAD_U64(incoming_record);
  if (!incoming_xuid) return false;

  for (uint32_t slot = 0; slot < 8; ++slot) {
    const uint32_t player = lobby_object + 6152 + slot * 256;
    if (REX_LOAD_U64(player + 120) != incoming_xuid) continue;

    // Replication messages may be replayed or echoed after the member has
    // already been materialized from XSessionGetDetails. The retail routine
    // does not deduplicate before selecting an empty slot, producing a second
    // native object for the same XUID and a cloned frontend entry. Treat the
    // existing object as authoritative and refresh only its liveness metadata.
    const uint32_t now_tick = REX_LOAD_U32(player + 240);
    const uint32_t incoming_tick = REX_LOAD_U32(incoming_record + 84);
    if (incoming_tick && incoming_tick > now_tick) {
      REX_STORE_U32(player + 240, incoming_tick);
    }
    REXLOG_WARN(
        "System Link suppressed duplicate member replication "
        "(lobby={:08X}, return={:08X}, slot={}, player={:08X}, xuid={:016X})",
        lobby_object, return_address, slot, player, incoming_xuid);
    return true;
  }
  return false;
}

void TraceSystemLinkMemberRebuild(uint8_t* base, uint32_t lobby_object,
                                  uint32_t return_address) {
  static uint32_t last_hash = 0;
  if (!lobby_object) return;
  uint32_t hash = 2166136261u;
  uint32_t occupied = 0;
  for (uint32_t slot = 0; slot < 8; ++slot) {
    const uint32_t player = lobby_object + 6152 + slot * 256;
    const uint64_t xuid = REX_LOAD_U64(player + 120);
    if (xuid) ++occupied;
    hash = (hash ^ static_cast<uint32_t>(xuid)) * 16777619u;
    hash = (hash ^ static_cast<uint32_t>(xuid >> 32)) * 16777619u;
    hash = (hash ^ REX_LOAD_U32(player + 36)) * 16777619u;
    hash = (hash ^ REX_LOAD_U32(player + 40)) * 16777619u;
    hash = (hash ^ REX_LOAD_U32(player + 240)) * 16777619u;
  }
  if (hash == last_hash) return;
  last_hash = hash;
  REXLOG_INFO("System Link member table rebuilt (lobby={:08X}, return={:08X}, occupied={}, reported={})",
              lobby_object, return_address, occupied, REX_LOAD_U32(lobby_object + 5964));
  for (uint32_t slot = 0; slot < 8; ++slot) {
    const uint32_t player = lobby_object + 6152 + slot * 256;
    const uint64_t xuid = REX_LOAD_U64(player + 120);
    if (!xuid) continue;
    REXLOG_INFO("System Link member slot {} (player={:08X}, xuid={:016X}, flags36={:08X}, state40={:08X}, heartbeat={}, session_index={})",
                slot, player, xuid, REX_LOAD_U32(player + 36),
                REX_LOAD_U32(player + 40), REX_LOAD_U32(player + 240),
                REX_LOAD_U32(player + 244));
  }
}



void TraceSystemLinkJoinReplication(uint8_t* base, uint32_t lobby_object,
                                    uint32_t return_address) {
  static uint32_t last_object = 0;
  static uint32_t last_phase = UINT32_MAX;
  static uint32_t last_pending = UINT32_MAX;
  static uint32_t last_players = UINT32_MAX;
  if (!lobby_object) return;

  const uint32_t phase = REX_LOAD_U32(lobby_object + 9812);
  const uint32_t pending = REX_LOAD_U32(lobby_object + 9804);
  const uint32_t players = REX_LOAD_U32(lobby_object + 5964);
  if (lobby_object == last_object && phase == last_phase &&
      pending == last_pending && players == last_players) {
    return;
  }

  REXLOG_INFO(
      "System Link join replication (lobby={:08X}, return={:08X}, "
      "phase={}, pending={}, players={}, mode={}, operation={}, join={}, "
      "active={}, response={}, host={:08X}, local={:08X}, network={:08X}, "
      "session={:08X})",
      lobby_object, return_address, phase, pending, players,
      REX_LOAD_U32(lobby_object + 5836), REX_LOAD_U32(lobby_object + 5840),
      REX_LOAD_U32(lobby_object + 5844), REX_LOAD_U32(lobby_object + 5908),
      REX_LOAD_U32(lobby_object + 5912), REX_LOAD_U32(lobby_object + 8200),
      REX_LOAD_U32(lobby_object + 8204), REX_LOAD_U32(lobby_object + 8208),
      REX_LOAD_U32(lobby_object + 8212));

  last_object = lobby_object;
  last_phase = phase;
  last_pending = pending;
  last_players = players;
}

void RepairSystemLinkPromotion(uint8_t* base, uint32_t lobby_object,
                               uint32_t return_address) {
  if (!lobby_object) return;

  std::array<uint64_t, 8> seen{};
  uint32_t distinct = 0;
  uint32_t occupied = 0;
  uint64_t signature = 1469598103934665603ull;
  for (uint32_t slot = 0; slot < 8; ++slot) {
    const uint32_t player = lobby_object + 6152 + slot * 256;
    const uint64_t xuid = REX_LOAD_U64(player + 120);
    if (!xuid) continue;
    ++occupied;
    signature ^= xuid + (static_cast<uint64_t>(slot) << 56);
    signature *= 1099511628211ull;
    bool duplicate = false;
    for (uint32_t i = 0; i < distinct; ++i) {
      if (seen[i] == xuid) { duplicate = true; break; }
    }
    if (!duplicate) seen[distinct++] = xuid;
  }

  if (distinct < 2 || !REX_LOAD_U32(lobby_object + 8200) ||
      !REX_LOAD_U32(lobby_object + 8204) ||
      !REX_LOAD_U32(lobby_object + 8208) ||
      !REX_LOAD_U32(lobby_object + 8212)) return;

  const uint32_t host = REX_LOAD_U32(lobby_object + 8200);
  const uint32_t local = REX_LOAD_U32(lobby_object + 8204);
  if (host == local || !REX_LOAD_U64(host + 120) || !REX_LOAD_U64(local + 120) ||
      REX_LOAD_U64(host + 120) == REX_LOAD_U64(local + 120)) return;

  PromotionState* state = nullptr;
  for (auto& candidate : promotion_states) {
    if (candidate.lobby == lobby_object) { state = &candidate; break; }
  }
  if (!state) {
    state = &promotion_states[next_promotion_state_slot++ % promotion_states.size()];
    *state = {};
    state->lobby = lobby_object;
  }
  if (state->member_signature != signature) {
    state->member_signature = signature;
    state->applied = false;
  }

  const uint32_t mode = REX_LOAD_U32(lobby_object + 5836);
  const uint32_t phase = REX_LOAD_U32(lobby_object + 9812);
  const uint32_t pending = REX_LOAD_U32(lobby_object + 9804);
  if (phase != 2 || pending != 0 || (mode != 4 && mode != 6)) return;

  // Apply only the two fields proven inconsistent by the traces, and only once
  // for a stable member signature. Do not clear operation/result/disconnect
  // edges and do not touch native player objects or controller pointers.
  bool changed = false;
  if (mode == 6) {
    REX_STORE_U32(lobby_object + 5836, 4);
    changed = true;
  }
  const uint32_t reported = REX_LOAD_U32(lobby_object + 5964);
  if (occupied > distinct && reported != distinct) {
    REX_STORE_U32(lobby_object + 5964, distinct);
    changed = true;
  }

  if (changed && !state->applied) {
    REXLOG_WARN(
        "System Link normalized completed native promotion "
        "(lobby={:08X}, return={:08X}, mode={}->4, occupied={}, distinct={}, "
        "reported={}->{}, phase={}, pending={})",
        lobby_object, return_address, mode, occupied, distinct, reported,
        occupied > distinct ? distinct : reported, phase, pending);
  }
  state->applied = true;
}

bool ShouldSkipSystemLinkPlayerRemoval(uint8_t* base, uint32_t lobby_object,
                                       uint32_t player_object,
                                       uint32_t return_address) {
  if (!lobby_object || !player_object) return false;
  const auto values = ReadLobbyTraceValues(base, lobby_object);
  const uint64_t xuid = REX_LOAD_U64(player_object + 120);
  const uint32_t local = REX_LOAD_U32(lobby_object + 8204);
  const uint64_t local_xuid = local ? REX_LOAD_U64(local + 120) : 0;
  const bool fresh_sync = xuid && xuid == last_valid_remote_identity &&
      std::chrono::steady_clock::now() - last_valid_remote_sync <=
          kRemoteSyncFreshWindow;
  const bool healthy = values[1] == 4 && values[2] == 0 && values[4] == 0 &&
      values[5] == 0 && values[7] == 0 && values[8] == 0 && values[9] == 1 &&
      values[10] == 1 && values[11] >= 2 &&
      REX_LOAD_U32(lobby_object + 8208) != 0 &&
      REX_LOAD_U32(lobby_object + 8212) != 0;

  REXLOG_WARN(
      "System Link player removal requested (lobby={:08X}, player={:08X}, "
      "return={:08X}, xuid={:016X}, local={:016X}, healthy={}, fresh_sync={})",
      lobby_object, player_object, return_address, xuid, local_xuid, healthy,
      fresh_sync);

  // Suppress only a removal contradicted by a heartbeat received in the last
  // five seconds. No cached pointer or previous promotion can keep a dead peer
  // alive indefinitely.
  if (healthy && fresh_sync && xuid != local_xuid) {
    const uint32_t tick = local ? REX_LOAD_U32(local + 240) : 0;
    if (tick) REX_STORE_U32(player_object + 240, tick);
    return true;
  }
  return false;
}

}  // namespace generations
