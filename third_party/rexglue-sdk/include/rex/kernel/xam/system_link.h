#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rex::kernel::xam {

// IPv4 values in this interface are stored in native in_addr byte order.
// Keeping the representation identical to XNADDR avoids a second conversion
// when the session result is written to guest memory.
struct SystemLinkIdentity {
  uint32_t ipv4_address = 0;
  uint32_t ipv4_broadcast = 0;
  std::array<uint8_t, 6> mac = {};
  std::string adapter_name;
};

struct SystemLinkContext {
  uint32_t id = 0;
  uint32_t value = 0;
};

// XSession search properties used by Generations are scalar X_USER_DATA
// values. Preserve their guest-endian payload bytes so they can be written
// directly into an XUSER_PROPERTY on the discovering machine.
struct SystemLinkProperty {
  uint32_t id = 0;
  uint8_t type = 0xFF;
  uint8_t value_size = 0;
  std::array<uint8_t, 8> value = {};
};

struct SystemLinkSessionAdvertisement {
  uint32_t title_id = 0;
  std::array<uint8_t, 8> session_id = {};
  std::array<uint8_t, 16> key_exchange_key = {};
  std::array<uint8_t, 6> mac = {};
  uint32_t host_ipv4 = 0;
  uint16_t game_port = 0;
  uint32_t flags = 0;
  uint32_t public_slots = 0;
  uint32_t private_slots = 0;
  uint32_t open_public_slots = 0;
  uint32_t open_private_slots = 0;
  uint32_t filled_public_slots = 0;
  uint32_t filled_private_slots = 0;
  uint64_t host_xuid = 0;
  std::vector<uint8_t> qos_data;
  std::vector<SystemLinkContext> contexts;
  std::vector<SystemLinkProperty> properties;
};

bool SystemLinkGetLocalIdentity(SystemLinkIdentity* identity);
void SystemLinkPublishSession(uint32_t owner, const SystemLinkSessionAdvertisement& session);
void SystemLinkRemoveSession(uint32_t owner);
// Re-advertises active lobbies after XNet reserves the title's system-link port.
// A number of titles create the session before making that XNet call.
void SystemLinkUpdateGamePort(uint16_t port);
std::vector<SystemLinkSessionAdvertisement> SystemLinkDiscoverSessions(uint32_t title_id,
                                                                         uint32_t max_results);
void SystemLinkUpdateQosData(const std::array<uint8_t, 8>& session_id,
                             const std::vector<uint8_t>& data);
std::optional<SystemLinkSessionAdvertisement> SystemLinkFindSessionById(
    const std::array<uint8_t, 8>& session_id);
void SystemLinkStopDiscovery();

}  // namespace rex::kernel::xam
