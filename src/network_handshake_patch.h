#pragma once

#include <cstdint>

struct PPCContext;

namespace generations {

void LatchOnlineHandshakeConfirm(PPCContext& context, uint8_t* base,
                                 uint32_t flow_object);
void CompleteOnlineHandshakeOnReceive(uint8_t* base, uint32_t flow_object,
                                      uint32_t packet_size,
                                      uint32_t framed_size);
void TraceOnlineTransportPoll(uint8_t* base, uint32_t transport_object,
                              uint32_t caller_address);
bool StripSystemLinkCompatibilityHandshake(uint8_t* base,
                                           uint32_t connection_object);
void TraceSystemLinkNativePacket(uint8_t* base, uint32_t connection_object,
                                 uint32_t packet_address,
                                 uint32_t packet_size);
void TraceSystemLinkLobbyState(uint8_t* base, uint32_t lobby_object,
                               uint32_t update_site,
                               uint32_t return_address);
bool TraceSystemLinkLobbyTeardown(uint8_t* base, uint32_t lobby_object,
                                  uint32_t return_address);

void TraceSystemLinkActivePump(uint8_t* base, uint32_t network_object,
                               uint32_t return_address);
void TraceSystemLinkHeartbeatApply(uint8_t* base, uint32_t lobby_object,
                                   uint32_t packet_object,
                                   uint32_t return_address);
void TraceSystemLinkMemberRebuild(uint8_t* base, uint32_t lobby_object,
                                  uint32_t return_address);
bool ShouldSkipDuplicateSystemLinkMember(uint8_t* base, uint32_t lobby_object,
                                         uint32_t incoming_record,
                                         uint32_t return_address);
void TraceSystemLinkJoinReplication(uint8_t* base, uint32_t lobby_object,
                                    uint32_t return_address);
void RepairSystemLinkPromotion(uint8_t* base, uint32_t lobby_object,
                               uint32_t return_address);
bool ShouldSkipSystemLinkPlayerRemoval(uint8_t* base, uint32_t lobby_object,
                                       uint32_t player_object,
                                       uint32_t return_address);

}  // namespace generations
