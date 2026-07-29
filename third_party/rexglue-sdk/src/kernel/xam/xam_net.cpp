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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <optional>
#include <thread>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/xam/system_link.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#include <ws2tcpip.h>
#include <iphlpapi.h>
#elif REX_PLATFORM_LINUX
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

namespace rex {
namespace kernel {
namespace xam {
namespace apps {
void SystemLinkNotifyGamePort(uint16_t game_port);
void SystemLinkNotifyAcceptedPeer(uint16_t listener_port);
}
using namespace rex::system;
using namespace rex::system::xam;

// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

// https://github.com/pmrowla/hl2sdk-csgo/blob/master/common/xbox/xboxstubs.h
typedef struct {
  // FYI: IN_ADDR should be in network-byte order.
  in_addr ina;                    // IP address (zero if not static/DHCP)
  in_addr inaOnline;              // Online IP address (zero if not online)
  rex::be<uint16_t> wPortOnline;  // Online port
  uint8_t abEnet[6];              // Ethernet MAC address
  uint8_t abOnline[20];           // Online identification
} XNADDR;

typedef struct {
  rex::be<int32_t> status;
  rex::be<uint32_t> cina;
  in_addr aina[8];
} XNDNS;

typedef struct {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min_in_msecs;
  rex::be<uint16_t> rtt_med_in_msecs;
  rex::be<uint32_t> up_bits_per_sec;
  rex::be<uint32_t> down_bits_per_sec;
} XNQOSINFO;

typedef struct {
  rex::be<uint32_t> count;
  rex::be<uint32_t> count_pending;
  XNQOSINFO info[1];
} XNQOS;

struct Xsockaddr_t {
  rex::be<uint16_t> sa_family;
  char sa_data[14];
};

struct X_WSADATA {
  rex::be<uint16_t> version;
  rex::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  rex::be<uint16_t> max_sockets;
  rex::be<uint16_t> max_udpdg;
  rex::be<uint32_t> vendor_info_ptr;
};

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  union {
    struct {
      rex::be<uint32_t> low;
      rex::be<uint32_t> high;
    } offset;  // must be named to avoid GCC error
    rex::be<uint32_t> pointer;
  };
  rex::be<uint32_t> event_handle;
};

void LoadSockaddr(const uint8_t* ptr, sockaddr* out_addr) {
  out_addr->sa_family = memory::load_and_swap<uint16_t>(ptr + 0);
  switch (out_addr->sa_family) {
    case AF_INET: {
      auto in_addr = reinterpret_cast<sockaddr_in*>(out_addr);
      in_addr->sin_port = memory::load_and_swap<uint16_t>(ptr + 2);
      // Maybe? Depends on type.
      in_addr->sin_addr.s_addr = *(uint32_t*)(ptr + 4);
      break;
    }
    default:
      assert_unhandled_case(out_addr->sa_family);
      break;
  }
}

void StoreSockaddr(const sockaddr& addr, uint8_t* ptr) {
  switch (addr.sa_family) {
    case AF_UNSPEC:
      std::memset(ptr, 0, sizeof(addr));
      break;
    case AF_INET: {
      auto& in_addr = reinterpret_cast<const sockaddr_in&>(addr);
      memory::store_and_swap<uint16_t>(ptr + 0, in_addr.sin_family);
      memory::store_and_swap<uint16_t>(ptr + 2, in_addr.sin_port);
      // Maybe? Depends on type.
      memory::store_and_swap<uint32_t>(ptr + 4, in_addr.sin_addr.s_addr);
      break;
    }
    default:
      assert_unhandled_case(addr.sa_family);
      break;
  }
}

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags;
  uint8_t cfgSockMaxDgramSockets;
  uint8_t cfgSockMaxStreamSockets;
  uint8_t cfgSockDefaultRecvBufsizeInK;
  uint8_t cfgSockDefaultSendBufsizeInK;
  uint8_t cfgKeyRegMax;
  uint8_t cfgSecRegMax;
  uint8_t cfgQosDataLimitDiv4;
  uint8_t cfgQosProbeTimeoutInSeconds;
  uint8_t cfgQosProbeRetries;
  uint8_t cfgQosSrvMaxSimultaneousResponses;
  uint8_t cfgQosPairWaitTimeInSeconds;
};

XNetStartupParams xnet_startup_params = {};

REXCVAR_DEFINE_STRING(system_link_adapter, "Radmin", "Network",
                      "Preferred IPv4 adapter for system-link traffic");
REXCVAR_DEFINE_UINT32(system_link_discovery_port, 32137, "Network",
                      "UDP port used by the local system-link lobby discovery service");
REXCVAR_DEFINE_STRING(system_link_peers, "", "Network",
                      "Comma-separated Radmin IPv4 peer addresses used in addition to broadcast");

namespace {

struct SystemLinkEndpoint {
  in_addr address = {};
  in_addr broadcast = {};
  std::array<uint8_t, 6> mac = {};
  std::string adapter_name;
};

struct QosListenState {
  std::array<uint8_t, 8> session_id = {};
  std::vector<uint8_t> data;
  uint32_t flags = 0;
};

std::mutex system_link_mutex;
uint16_t system_link_port = 0;
QosListenState qos_listen_state;
std::mutex qos_allocations_mutex;
std::unordered_map<uint32_t, std::vector<uint32_t>> qos_child_allocations;
std::unordered_set<uint32_t> qos_root_allocations;
std::atomic<uint32_t> stream_select_trace_count = 0;
std::atomic<uint32_t> stream_send_trace_count = 0;
std::atomic<uint32_t> stream_send_attempt_trace_count = 0;
std::atomic<uint32_t> stream_recv_trace_count = 0;
std::atomic<uint32_t> stream_recv_attempt_trace_count = 0;
std::atomic<uint32_t> stream_sendto_trace_count = 0;
std::atomic<uint32_t> stream_recvfrom_trace_count = 0;
std::atomic<uint32_t> stream_fd_isset_trace_count = 0;
std::atomic<uint32_t> xnaddr_translation_trace_count = 0;


#if REX_PLATFORM_WIN32
std::string FormatNativeSocketEndpoints(const object_ref<XSocket>& socket) {
  if (!socket || !socket->native_handle()) {
    return "native=invalid";
  }

  sockaddr_in local = {};
  sockaddr_in peer = {};
  int local_len = sizeof(local);
  int peer_len = sizeof(peer);
  const bool has_local = getsockname(socket->native_handle(),
                                     reinterpret_cast<sockaddr*>(&local),
                                     &local_len) == 0;
  const bool has_peer = getpeername(socket->native_handle(),
                                    reinterpret_cast<sockaddr*>(&peer),
                                    &peer_len) == 0;

  char local_ip[INET_ADDRSTRLEN] = {};
  char peer_ip[INET_ADDRSTRLEN] = {};
  if (has_local) {
    inet_ntop(AF_INET, &local.sin_addr, local_ip, sizeof(local_ip));
  }
  if (has_peer) {
    inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
  }

  return fmt::format("local={}:{} peer={}:{}",
                     has_local ? local_ip : "?",
                     has_local ? ntohs(local.sin_port) : 0,
                     has_peer ? peer_ip : "?",
                     has_peer ? ntohs(peer.sin_port) : 0);
}
#else
std::string FormatNativeSocketEndpoints(const object_ref<XSocket>& socket) {
  return "endpoints=unavailable";
}
#endif

std::string FormatPacketPrefix(const uint8_t* data, size_t length) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  const size_t prefix_length = std::min<size_t>(length, 24);
  std::string result;
  result.reserve(prefix_length * 3);
  for (size_t i = 0; i < prefix_length; ++i) {
    if (i) {
      result.push_back(' ');
    }
    result.push_back(kHex[data[i] >> 4]);
    result.push_back(kHex[data[i] & 0x0F]);
  }
  return result;
}

bool ContainsInsensitive(std::wstring_view value, std::wstring_view needle) {
  if (needle.empty()) {
    return true;
  }
  if (needle.size() > value.size()) {
    return false;
  }

  for (size_t start = 0; start <= value.size() - needle.size(); ++start) {
    bool matches = true;
    for (size_t i = 0; i < needle.size(); ++i) {
      if (std::towlower(value[start + i]) != std::towlower(needle[i])) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return true;
    }
  }
  return false;
}

#if REX_PLATFORM_WIN32
std::wstring WidenAdapterHint(std::string_view value) {
  return std::wstring(value.begin(), value.end());
}

std::optional<SystemLinkEndpoint> ResolveSystemLinkEndpoint() {
  ULONG buffer_size = 0;
  DWORD result =
      GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &buffer_size);
  if (result != ERROR_BUFFER_OVERFLOW || !buffer_size) {
    return std::nullopt;
  }

  std::vector<uint8_t> buffer(buffer_size);
  auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &buffer_size);
  if (result != NO_ERROR) {
    return std::nullopt;
  }

  const std::wstring preferred_adapter = WidenAdapterHint(REXCVAR_GET(system_link_adapter));
  std::optional<SystemLinkEndpoint> fallback;
  for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp || !adapter->FirstUnicastAddress) {
      continue;
    }

    const std::wstring_view friendly_name =
        adapter->FriendlyName ? std::wstring_view(adapter->FriendlyName) : std::wstring_view();
    const std::wstring_view description =
        adapter->Description ? std::wstring_view(adapter->Description) : std::wstring_view();
    const bool is_preferred = ContainsInsensitive(friendly_name, preferred_adapter) ||
                              ContainsInsensitive(description, preferred_adapter);

    for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
      if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET) {
        continue;
      }

      const auto* address = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
      if (address->sin_addr.s_addr == INADDR_ANY ||
          address->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
        continue;
      }

      SystemLinkEndpoint endpoint;
      endpoint.address = address->sin_addr;
      const uint8_t prefix_length = std::min<uint8_t>(unicast->OnLinkPrefixLength, 32);
      const uint32_t address_host_order = ntohl(endpoint.address.s_addr);
      const uint32_t subnet_mask = prefix_length == 0    ? 0
                                   : prefix_length == 32 ? UINT32_MAX
                                                         : UINT32_MAX << (32 - prefix_length);
      endpoint.broadcast.s_addr = htonl(address_host_order | ~subnet_mask);
      if (adapter->PhysicalAddressLength >= endpoint.mac.size()) {
        std::memcpy(endpoint.mac.data(), adapter->PhysicalAddress, endpoint.mac.size());
      }
      if (std::all_of(endpoint.mac.begin(), endpoint.mac.end(),
                      [](uint8_t value) { return value == 0; })) {
        // VPN adapters do not always expose a physical address. Derive a
        // locally administered one from the selected IPv4 address instead.
        endpoint.mac = {0x02,
                        static_cast<uint8_t>(address_host_order >> 24),
                        static_cast<uint8_t>(address_host_order >> 16),
                        static_cast<uint8_t>(address_host_order >> 8),
                        static_cast<uint8_t>(address_host_order),
                        0x01};
      }
      endpoint.adapter_name.assign(friendly_name.begin(), friendly_name.end());

      if (is_preferred) {
        return endpoint;
      }
      if (!fallback) {
        fallback = endpoint;
      }
      break;
    }
  }

  return fallback;
}
#else
std::optional<SystemLinkEndpoint> ResolveSystemLinkEndpoint() {
  return std::nullopt;
}
#endif

void FillXnAddr(XNADDR* xn_addr, const SystemLinkEndpoint& endpoint) {
  std::memset(xn_addr, 0, sizeof(*xn_addr));
  xn_addr->ina = endpoint.address;
  std::memcpy(xn_addr->abEnet, endpoint.mac.data(), endpoint.mac.size());
}

std::string FormatAddress(const in_addr& address) {
  const uint32_t value = ntohl(address.s_addr);
  return std::to_string((value >> 24) & 0xFF) + "." + std::to_string((value >> 16) & 0xFF) + "." +
         std::to_string((value >> 8) & 0xFF) + "." + std::to_string(value & 0xFF);
}

uint32_t GetHostSocketLastError() {
#if REX_PLATFORM_WIN32
  return static_cast<uint32_t>(WSAGetLastError());
#else
  return static_cast<uint32_t>(errno);
#endif
}

void SetHostSocketLastError() {
  XThread::SetLastError(GetHostSocketLastError());
}

bool IsHostSocketWouldBlock(uint32_t error) {
#if REX_PLATFORM_WIN32
  return error == WSAEWOULDBLOCK;
#else
  return error == EWOULDBLOCK || error == EAGAIN;
#endif
}

std::string FormatSockaddrIn(const N_XSOCKADDR_IN& address) {
  in_addr addr = {};
  addr.s_addr = htonl(static_cast<uint32_t>(address.sin_addr));
  return FormatAddress(addr) + ":" + std::to_string(static_cast<uint16_t>(address.sin_port));
}

constexpr uint32_t kSystemLinkDiscoveryMagic = 0x52474C53;  // "RGLS"
constexpr uint16_t kSystemLinkDiscoveryVersion = 3;
constexpr size_t kSystemLinkQosDataCapacity = 256;
constexpr size_t kSystemLinkContextCapacity = 8;
constexpr size_t kSystemLinkPropertyCapacity = 32;
constexpr auto kSystemLinkDiscoveryCacheLifetime = std::chrono::seconds(15);
constexpr auto kSystemLinkAdvertisementHeartbeat = std::chrono::seconds(2);

enum class SystemLinkDiscoveryMessage : uint16_t {
  kQuery = 1,
  kAdvertisement = 2,
};

#pragma pack(push, 1)
struct SystemLinkWireContext {
  uint32_t id;
  uint32_t value;
};

struct SystemLinkWireProperty {
  uint32_t id;
  uint8_t type;
  uint8_t value_size;
  uint16_t reserved;
  uint8_t value[8];
};

struct SystemLinkWirePacket {
  uint32_t magic;
  uint16_t version;
  uint16_t message;
  uint32_t title_id;
  uint8_t session_id[8];
  uint8_t key_exchange_key[16];
  uint8_t mac[6];
  uint16_t game_port;
  uint32_t flags;
  uint32_t public_slots;
  uint32_t private_slots;
  uint32_t open_public_slots;
  uint32_t open_private_slots;
  uint32_t filled_public_slots;
  uint32_t filled_private_slots;
  uint64_t host_xuid;
  uint16_t qos_data_size;
  uint8_t qos_data[kSystemLinkQosDataCapacity];
  uint16_t context_count;
  uint16_t property_count;
  SystemLinkWireContext contexts[kSystemLinkContextCapacity];
  SystemLinkWireProperty properties[kSystemLinkPropertyCapacity];
};
#pragma pack(pop)
static_assert(sizeof(SystemLinkWirePacket) == 918);

uint64_t HostToNetwork64(uint64_t value) {
  const uint32_t high = htonl(static_cast<uint32_t>(value >> 32));
  const uint32_t low = htonl(static_cast<uint32_t>(value));
  return (static_cast<uint64_t>(low) << 32) | high;
}

uint64_t NetworkToHost64(uint64_t value) {
  const uint32_t high = ntohl(static_cast<uint32_t>(value));
  const uint32_t low = ntohl(static_cast<uint32_t>(value >> 32));
  return (static_cast<uint64_t>(high) << 32) | low;
}

std::string MakeSystemLinkSessionKey(uint32_t title_id, const std::array<uint8_t, 8>& session_id) {
  std::string key = std::to_string(title_id);
  key.append(reinterpret_cast<const char*>(session_id.data()), session_id.size());
  return key;
}

std::vector<in_addr> ParseSystemLinkPeers(std::string_view peers) {
  std::vector<in_addr> addresses;
  size_t start = 0;
  while (start < peers.size()) {
    const size_t end = peers.find(',', start);
    std::string_view part =
        peers.substr(start, end == std::string_view::npos ? peers.size() - start : end - start);
    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) {
      part.remove_prefix(1);
    }
    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back()))) {
      part.remove_suffix(1);
    }

    if (!part.empty()) {
      in_addr address = {};
      const std::string address_string(part);
      if (inet_pton(AF_INET, address_string.c_str(), &address) == 1) {
        addresses.push_back(address);
      } else {
        REXKRNL_WARN("Ignoring invalid system_link_peers entry '{}'", address_string);
      }
    }

    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return addresses;
}

#if REX_PLATFORM_WIN32
class SystemLinkDiscoveryService {
 public:
  ~SystemLinkDiscoveryService() { Stop(); }

  bool EnsureStarted() {
    std::scoped_lock lock(mutex_);
    if (running_) {
      return true;
    }

    const auto endpoint = ResolveSystemLinkEndpoint();
    if (!endpoint) {
      REXKRNL_WARN("System-link discovery is unavailable because no IPv4 adapter is active");
      return false;
    }

    WSADATA wsa_data = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
      REXKRNL_ERROR("System-link discovery could not start Winsock");
      return false;
    }
    owns_winsock_ = true;

    SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
      REXKRNL_ERROR("System-link discovery could not create a UDP socket ({})", WSAGetLastError());
      WSACleanup();
      owns_winsock_ = false;
      return false;
    }

    const BOOL enabled = TRUE;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled),
               sizeof(enabled));
    setsockopt(socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled),
               sizeof(enabled));
    u_long non_blocking = 1;
    ioctlsocket(socket, FIONBIO, &non_blocking);

    sockaddr_in bind_address = {};
    bind_address.sin_family = AF_INET;
    bind_address.sin_addr = endpoint->address;
    bind_address.sin_port = htons(static_cast<uint16_t>(REXCVAR_GET(system_link_discovery_port)));
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&bind_address), sizeof(bind_address)) ==
        SOCKET_ERROR) {
      const int bind_error = WSAGetLastError();
      closesocket(socket);
      WSACleanup();
      owns_winsock_ = false;
      REXKRNL_ERROR("System-link discovery could not bind {}:{} ({})",
                    FormatAddress(endpoint->address), REXCVAR_GET(system_link_discovery_port),
                    bind_error);
      return false;
    }

    endpoint_ = *endpoint;
    socket_ = socket;
    running_ = true;
    thread_ = std::thread(&SystemLinkDiscoveryService::Run, this);
    REXKRNL_INFO("System-link discovery listening on {}:{} via '{}'",
                 FormatAddress(endpoint_.address), REXCVAR_GET(system_link_discovery_port),
                 endpoint_.adapter_name);
    return true;
  }

  void Stop() {
    SOCKET socket = INVALID_SOCKET;
    {
      std::scoped_lock lock(mutex_);
      if (!running_ && !thread_.joinable()) {
        return;
      }
      running_ = false;
      socket = socket_;
      socket_ = INVALID_SOCKET;
      local_sessions_.clear();
      discovered_sessions_.clear();
    }

    if (socket != INVALID_SOCKET) {
      closesocket(socket);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    if (owns_winsock_) {
      WSACleanup();
      owns_winsock_ = false;
    }
  }

  void Publish(uint32_t owner, SystemLinkSessionAdvertisement session) {
    if (!EnsureStarted()) {
      return;
    }

    {
      std::scoped_lock lock(mutex_);
      if (!session.host_ipv4) {
        session.host_ipv4 = endpoint_.address.s_addr;
      }
      if (std::all_of(session.mac.begin(), session.mac.end(),
                      [](uint8_t value) { return value == 0; })) {
        session.mac = endpoint_.mac;
      }
      if (!session.game_port) {
        std::scoped_lock system_link_lock(system_link_mutex);
        session.game_port = system_link_port;
      }
      local_sessions_[owner] = session;
    }

    SendAdvertisement(session);
    REXKRNL_INFO("System-link lobby published: title={:08X}, public={}/{}, private={}/{}",
                 session.title_id, session.filled_public_slots, session.public_slots,
                 session.filled_private_slots, session.private_slots);
  }

  void Remove(uint32_t owner) {
    std::scoped_lock lock(mutex_);
    local_sessions_.erase(owner);
  }

  void UpdateGamePort(uint16_t port) {
    std::vector<SystemLinkSessionAdvertisement> sessions;
    {
      std::scoped_lock lock(mutex_);
      if (!running_) {
        return;
      }

      sessions.reserve(local_sessions_.size());
      for (auto& entry : local_sessions_) {
        auto& session = entry.second;
        session.game_port = port;
        sessions.push_back(session);
      }
    }

    for (const auto& session : sessions) {
      SendAdvertisement(session);
    }
    if (!sessions.empty()) {
      REXKRNL_INFO("System-link lobby advertisement updated for game port {}", port);
    }
  }

  void UpdateQosData(const std::array<uint8_t, 8>& session_id, const std::vector<uint8_t>& data) {
    std::vector<SystemLinkSessionAdvertisement> sessions;
    {
      std::scoped_lock lock(mutex_);
      for (auto& [owner, session] : local_sessions_) {
        if (session.session_id == session_id) {
          session.qos_data = data;
          sessions.push_back(session);
        }
      }
    }
    for (const auto& session : sessions) {
      SendAdvertisement(session);
    }
    if (!sessions.empty()) {
      REXKRNL_INFO("System-link lobby advertisement updated with {} QoS bytes", data.size());
    }
  }

  std::optional<SystemLinkSessionAdvertisement> FindBySessionId(
      const std::array<uint8_t, 8>& session_id) {
    std::scoped_lock lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [key, cached] : discovered_sessions_) {
      if (cached.session.session_id == session_id &&
          now - cached.last_seen <= kSystemLinkDiscoveryCacheLifetime) {
        return cached.session;
      }
    }
    for (const auto& [owner, session] : local_sessions_) {
      if (session.session_id == session_id) {
        return session;
      }
    }
    return std::nullopt;
  }

  std::vector<SystemLinkSessionAdvertisement> Discover(uint32_t title_id, uint32_t max_results) {
    if (!max_results || !EnsureStarted()) {
      return {};
    }

    SendQuery(title_id);
    constexpr auto kSearchWindow = std::chrono::milliseconds(250);
    constexpr auto kSearchStep = std::chrono::milliseconds(25);
    const auto deadline = std::chrono::steady_clock::now() + kSearchWindow;
    while (running_ && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(kSearchStep);
    }

    const auto now = std::chrono::steady_clock::now();
    std::vector<SystemLinkSessionAdvertisement> sessions;
    std::scoped_lock lock(mutex_);
    // Do not reject every advertisement from our own IPv4 address. Two local
    // game processes (and multiple PCs behind some VPN/NAT setups) may share
    // that address. Exclude only session IDs actually published by this
    // process, which still prevents a host from finding its own lobby.
    const auto is_local_session = [&](const SystemLinkSessionAdvertisement& candidate) {
      return std::any_of(local_sessions_.begin(), local_sessions_.end(), [&](const auto& entry) {
        return entry.second.session_id == candidate.session_id;
      });
    };
    for (auto it = discovered_sessions_.begin(); it != discovered_sessions_.end();) {
      if (now - it->second.last_seen > kSystemLinkDiscoveryCacheLifetime) {
        it = discovered_sessions_.erase(it);
        continue;
      }
      const auto& session = it->second.session;
      if (session.title_id == title_id && !is_local_session(session)) {
        sessions.push_back(session);
        if (sessions.size() >= max_results) {
          break;
        }
      }
      ++it;
    }
    REXKRNL_INFO("System-link search found {} compatible peer lobby{}", sessions.size(),
                 sessions.size() == 1 ? "" : " lobbies");
    return sessions;
  }

 private:
  struct CachedSession {
    SystemLinkSessionAdvertisement session;
    std::chrono::steady_clock::time_point last_seen;
  };

  SystemLinkWirePacket MakePacket(SystemLinkDiscoveryMessage message,
                                  const SystemLinkSessionAdvertisement& session) const {
    SystemLinkWirePacket packet = {};
    packet.magic = htonl(kSystemLinkDiscoveryMagic);
    packet.version = htons(kSystemLinkDiscoveryVersion);
    packet.message = htons(static_cast<uint16_t>(message));
    packet.title_id = htonl(session.title_id);
    std::memcpy(packet.session_id, session.session_id.data(), session.session_id.size());
    std::memcpy(packet.key_exchange_key, session.key_exchange_key.data(),
                session.key_exchange_key.size());
    std::memcpy(packet.mac, session.mac.data(), session.mac.size());
    packet.game_port = htons(session.game_port);
    packet.flags = htonl(session.flags);
    packet.public_slots = htonl(session.public_slots);
    packet.private_slots = htonl(session.private_slots);
    packet.open_public_slots = htonl(session.open_public_slots);
    packet.open_private_slots = htonl(session.open_private_slots);
    packet.filled_public_slots = htonl(session.filled_public_slots);
    packet.filled_private_slots = htonl(session.filled_private_slots);
    packet.host_xuid = HostToNetwork64(session.host_xuid);
    const uint16_t qos_size =
        static_cast<uint16_t>(std::min(session.qos_data.size(), kSystemLinkQosDataCapacity));
    packet.qos_data_size = htons(qos_size);
    if (qos_size) {
      std::memcpy(packet.qos_data, session.qos_data.data(), qos_size);
    }
    const uint16_t context_count = static_cast<uint16_t>(
        std::min(session.contexts.size(), kSystemLinkContextCapacity));
    packet.context_count = htons(context_count);
    for (uint16_t i = 0; i < context_count; ++i) {
      packet.contexts[i].id = htonl(session.contexts[i].id);
      packet.contexts[i].value = htonl(session.contexts[i].value);
    }
    const uint16_t property_count = static_cast<uint16_t>(
        std::min(session.properties.size(), kSystemLinkPropertyCapacity));
    packet.property_count = htons(property_count);
    for (uint16_t i = 0; i < property_count; ++i) {
      const auto& source = session.properties[i];
      auto& destination = packet.properties[i];
      destination.id = htonl(source.id);
      destination.type = source.type;
      destination.value_size =
          static_cast<uint8_t>(std::min<size_t>(source.value_size, sizeof(destination.value)));
      std::memcpy(destination.value, source.value.data(), destination.value_size);
    }
    return packet;
  }

  std::optional<SystemLinkSessionAdvertisement> ParseAdvertisement(
      const SystemLinkWirePacket& packet, const sockaddr_in& sender) const {
    if (ntohl(packet.magic) != kSystemLinkDiscoveryMagic ||
        ntohs(packet.version) != kSystemLinkDiscoveryVersion ||
        ntohs(packet.message) !=
            static_cast<uint16_t>(SystemLinkDiscoveryMessage::kAdvertisement)) {
      return std::nullopt;
    }

    SystemLinkSessionAdvertisement session;
    session.title_id = ntohl(packet.title_id);
    if (!session.title_id) {
      return std::nullopt;
    }
    std::memcpy(session.session_id.data(), packet.session_id, session.session_id.size());
    std::memcpy(session.key_exchange_key.data(), packet.key_exchange_key,
                session.key_exchange_key.size());
    std::memcpy(session.mac.data(), packet.mac, session.mac.size());
    session.host_ipv4 = sender.sin_addr.s_addr;
    session.game_port = ntohs(packet.game_port);
    if (!session.game_port ||
        std::all_of(session.session_id.begin(), session.session_id.end(),
                    [](uint8_t value) { return value == 0; })) {
      return std::nullopt;
    }
    session.flags = ntohl(packet.flags);
    session.public_slots = ntohl(packet.public_slots);
    session.private_slots = ntohl(packet.private_slots);
    session.open_public_slots = ntohl(packet.open_public_slots);
    session.open_private_slots = ntohl(packet.open_private_slots);
    session.filled_public_slots = ntohl(packet.filled_public_slots);
    session.filled_private_slots = ntohl(packet.filled_private_slots);
    session.host_xuid = NetworkToHost64(packet.host_xuid);
    const uint16_t qos_size =
        std::min<uint16_t>(ntohs(packet.qos_data_size), kSystemLinkQosDataCapacity);
    session.qos_data.assign(packet.qos_data, packet.qos_data + qos_size);
    const uint16_t context_count =
        std::min<uint16_t>(ntohs(packet.context_count), kSystemLinkContextCapacity);
    session.contexts.reserve(context_count);
    for (uint16_t i = 0; i < context_count; ++i) {
      session.contexts.push_back(
          {ntohl(packet.contexts[i].id), ntohl(packet.contexts[i].value)});
    }
    const uint16_t property_count =
        std::min<uint16_t>(ntohs(packet.property_count), kSystemLinkPropertyCapacity);
    session.properties.reserve(property_count);
    for (uint16_t i = 0; i < property_count; ++i) {
      const auto& source = packet.properties[i];
      SystemLinkProperty property;
      property.id = ntohl(source.id);
      property.type = source.type;
      property.value_size =
          static_cast<uint8_t>(std::min<size_t>(source.value_size, property.value.size()));
      std::memcpy(property.value.data(), source.value, property.value_size);
      session.properties.push_back(property);
    }
    return session;
  }

  void SendPacket(const SystemLinkWirePacket& packet, const sockaddr_in& target) const {
    SOCKET socket = INVALID_SOCKET;
    {
      std::scoped_lock lock(mutex_);
      socket = socket_;
    }
    if (socket != INVALID_SOCKET) {
      sendto(socket, reinterpret_cast<const char*>(&packet), sizeof(packet), 0,
             reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    }
  }

  void SendQuery(uint32_t title_id) {
    SystemLinkSessionAdvertisement query = {};
    query.title_id = title_id;
    const auto packet = MakePacket(SystemLinkDiscoveryMessage::kQuery, query);

    std::vector<in_addr> targets;
    {
      std::scoped_lock lock(mutex_);
      if (endpoint_.broadcast.s_addr && endpoint_.broadcast.s_addr != endpoint_.address.s_addr) {
        targets.push_back(endpoint_.broadcast);
      }
    }
    in_addr global_broadcast = {};
    global_broadcast.s_addr = INADDR_BROADCAST;
    targets.push_back(global_broadcast);
    for (const auto& peer : ParseSystemLinkPeers(REXCVAR_GET(system_link_peers))) {
      targets.push_back(peer);
    }

    for (const auto& target_address : targets) {
      sockaddr_in target = {};
      target.sin_family = AF_INET;
      target.sin_addr = target_address;
      target.sin_port = htons(static_cast<uint16_t>(REXCVAR_GET(system_link_discovery_port)));
      SendPacket(packet, target);
    }
  }

  void SendAdvertisement(const SystemLinkSessionAdvertisement& session,
                         const sockaddr_in* target = nullptr) {
    const auto packet = MakePacket(SystemLinkDiscoveryMessage::kAdvertisement, session);
    if (target) {
      SendPacket(packet, *target);
      return;
    }

    std::vector<in_addr> targets;
    {
      std::scoped_lock lock(mutex_);
      if (endpoint_.broadcast.s_addr && endpoint_.broadcast.s_addr != endpoint_.address.s_addr) {
        targets.push_back(endpoint_.broadcast);
      }
    }
    in_addr global_broadcast = {};
    global_broadcast.s_addr = INADDR_BROADCAST;
    if (std::none_of(targets.begin(), targets.end(),
                     [&](const in_addr& value) {
                       return value.s_addr == global_broadcast.s_addr;
                     })) {
      targets.push_back(global_broadcast);
    }
    for (const auto& peer : ParseSystemLinkPeers(REXCVAR_GET(system_link_peers))) {
      targets.push_back(peer);
    }
    for (const auto& target_address : targets) {
      sockaddr_in destination = {};
      destination.sin_family = AF_INET;
      destination.sin_addr = target_address;
      destination.sin_port = htons(static_cast<uint16_t>(REXCVAR_GET(system_link_discovery_port)));
      SendPacket(packet, destination);
    }
  }

  void HandlePacket(const SystemLinkWirePacket& packet, const sockaddr_in& sender) {
    if (ntohl(packet.magic) != kSystemLinkDiscoveryMagic ||
        ntohs(packet.version) != kSystemLinkDiscoveryVersion) {
      return;
    }

    const auto message = static_cast<SystemLinkDiscoveryMessage>(ntohs(packet.message));
    if (message == SystemLinkDiscoveryMessage::kQuery) {
      const uint32_t title_id = ntohl(packet.title_id);
      std::vector<SystemLinkSessionAdvertisement> matching_sessions;
      {
        std::scoped_lock lock(mutex_);
        for (const auto& [owner, session] : local_sessions_) {
          if (session.title_id == title_id) {
            matching_sessions.push_back(session);
          }
        }
      }
      for (const auto& session : matching_sessions) {
        SendAdvertisement(session, &sender);
      }
      return;
    }

    const auto session = ParseAdvertisement(packet, sender);
    if (!session) {
      return;
    }
    std::scoped_lock lock(mutex_);
    // Same-machine clients share the same adapter IPv4 address. Do not drop
    // their advertisements here; Discover() already removes only sessions
    // published by this exact process by comparing session IDs.
    discovered_sessions_[MakeSystemLinkSessionKey(session->title_id, session->session_id)] = {
        *session, std::chrono::steady_clock::now()};
  }

  void Run() {
    while (running_) {
      SOCKET socket = INVALID_SOCKET;
      {
        std::scoped_lock lock(mutex_);
        socket = socket_;
      }
      if (socket == INVALID_SOCKET) {
        break;
      }

      fd_set read_set;
      FD_ZERO(&read_set);
      FD_SET(socket, &read_set);
      timeval timeout = {0, 100000};
      const int selected = select(0, &read_set, nullptr, nullptr, &timeout);
      const auto now = std::chrono::steady_clock::now();
      if (now - last_advertisement_heartbeat_ >= kSystemLinkAdvertisementHeartbeat) {
        std::vector<SystemLinkSessionAdvertisement> heartbeat_sessions;
        {
          std::scoped_lock lock(mutex_);
          for (const auto& [owner, session] : local_sessions_) {
            heartbeat_sessions.push_back(session);
          }
        }
        for (const auto& session : heartbeat_sessions) {
          SendAdvertisement(session);
        }
        last_advertisement_heartbeat_ = now;
      }
      if (selected <= 0 || !FD_ISSET(socket, &read_set)) {
        continue;
      }

      while (running_) {
        SystemLinkWirePacket packet = {};
        sockaddr_in sender = {};
        int sender_size = sizeof(sender);
        const int received = recvfrom(socket, reinterpret_cast<char*>(&packet), sizeof(packet), 0,
                                      reinterpret_cast<sockaddr*>(&sender), &sender_size);
        if (received == SOCKET_ERROR) {
          if (WSAGetLastError() != WSAEWOULDBLOCK) {
            REXKRNL_DEBUG("System-link discovery receive failed ({})", WSAGetLastError());
          }
          break;
        }
        if (received == sizeof(packet)) {
          HandlePacket(packet, sender);
        }
      }
    }
  }

  mutable std::mutex mutex_;
  std::atomic_bool running_ = false;
  SOCKET socket_ = INVALID_SOCKET;
  bool owns_winsock_ = false;
  SystemLinkEndpoint endpoint_;
  std::thread thread_;
  std::unordered_map<uint32_t, SystemLinkSessionAdvertisement> local_sessions_;
  std::unordered_map<std::string, CachedSession> discovered_sessions_;
  std::chrono::steady_clock::time_point last_advertisement_heartbeat_{};
};

SystemLinkDiscoveryService system_link_discovery_service;
#endif

}  // namespace

bool SystemLinkGetLocalIdentity(SystemLinkIdentity* identity) {
  if (!identity) {
    return false;
  }
  const auto endpoint = ResolveSystemLinkEndpoint();
  if (!endpoint) {
    *identity = {};
    return false;
  }
  identity->ipv4_address = endpoint->address.s_addr;
  identity->ipv4_broadcast = endpoint->broadcast.s_addr;
  identity->mac = endpoint->mac;
  identity->adapter_name = endpoint->adapter_name;
  return true;
}

void SystemLinkPublishSession(uint32_t owner, const SystemLinkSessionAdvertisement& session) {
#if REX_PLATFORM_WIN32
  system_link_discovery_service.Publish(owner, session);
#else
  (void)owner;
  (void)session;
#endif
}

void SystemLinkRemoveSession(uint32_t owner) {
#if REX_PLATFORM_WIN32
  system_link_discovery_service.Remove(owner);
#else
  (void)owner;
#endif
}

void SystemLinkUpdateGamePort(uint16_t port) {
#if REX_PLATFORM_WIN32
  system_link_discovery_service.UpdateGamePort(port);
#else
  (void)port;
#endif
}

std::vector<SystemLinkSessionAdvertisement> SystemLinkDiscoverSessions(uint32_t title_id,
                                                                       uint32_t max_results) {
#if REX_PLATFORM_WIN32
  return system_link_discovery_service.Discover(title_id, max_results);
#else
  (void)title_id;
  (void)max_results;
  return {};
#endif
}

void SystemLinkUpdateQosData(const std::array<uint8_t, 8>& session_id,
                             const std::vector<uint8_t>& data) {
#if REX_PLATFORM_WIN32
  system_link_discovery_service.UpdateQosData(session_id, data);
#else
  (void)session_id;
  (void)data;
#endif
}

std::optional<SystemLinkSessionAdvertisement> SystemLinkFindSessionById(
    const std::array<uint8_t, 8>& session_id) {
#if REX_PLATFORM_WIN32
  return system_link_discovery_service.FindBySessionId(session_id);
#else
  (void)session_id;
  return std::nullopt;
#endif
}

void SystemLinkStopDiscovery() {
#if REX_PLATFORM_WIN32
  system_link_discovery_service.Stop();
#endif
}

namespace { void ClearXNetSecureState(); }

u32 NetDll_XNetStartup_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params) {
  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  if (auto endpoint = ResolveSystemLinkEndpoint()) {
    REXKRNL_INFO("System link adapter '{}' is active at {}", endpoint->adapter_name,
                 FormatAddress(endpoint->address));
  } else {
    REXKRNL_WARN("No active IPv4 adapter is available for system link");
  }

  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetStartupEx_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params, u32 config_ptr) {
  REXKRNL_INFO("NetDll_XNetStartupEx caller={} params={:08X} config={:08X}",
               caller, params.guest_address(), config_ptr);
  return NetDll_XNetStartup_entry(caller, params);
}

u32 NetDll_XNetCleanup_entry(u32 caller, mapped_void params) {
  SystemLinkStopDiscovery();
  {
    std::scoped_lock lock(system_link_mutex);
    system_link_port = 0;
  }
  ClearXNetSecureState();
  std::memset(&xnet_startup_params, 0, sizeof(xnet_startup_params));
  REXKRNL_INFO("[XEXONLINE][XNET] cleanup caller={} complete", caller);
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, mapped_void buffer_ptr,
                            mapped_u32 buffer_size) {
  if (one != 1 || !buffer_size) {
    return 0x2726;  // WSAEINVAL
  }
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return 0x2738;  // WSAEMSGSIZE
      }
      if (!buffer_ptr) {
        return 0x2726;  // WSAEINVAL
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      *buffer_size = sizeof(XNetStartupParams);
      return 0;
    default:
      REXKRNL_WARN("NetDll_XNetGetOpt: unsupported option {}", option_id);
      return 0x2726;  // WSAEINVAL
  }
}


namespace {
std::mutex xnet_secure_state_mutex;
std::unordered_map<uint64_t, std::array<uint8_t, 16>> xnet_registered_keys;
std::unordered_set<uint32_t> xnet_connected_addresses;
std::unordered_map<uint32_t, std::array<uint8_t, 8>> xnet_translated_session_ids;
std::atomic<uint64_t> xnet_key_sequence{1};

void ClearXNetSecureState() {
  std::scoped_lock lock(xnet_secure_state_mutex);
  xnet_registered_keys.clear();
  xnet_connected_addresses.clear();
  xnet_translated_session_ids.clear();
  xnet_key_sequence.store(1, std::memory_order_relaxed);
}

uint64_t ReadXNetKeyId(mapped_void key_id) {
  if (!key_id) return 0;
  uint64_t value = 0;
  std::memcpy(&value, key_id, sizeof(value));
  return value;
}
}  // namespace

u32 NetDll_XNetCreateKey_entry(u32 caller, mapped_void key_id, mapped_void key) {
  if (!key_id || !key) return X_ERROR_INVALID_PARAMETER;
  const uint64_t sequence = xnet_key_sequence.fetch_add(1, std::memory_order_relaxed);
  const uint64_t local_xuid = REX_KERNEL_STATE()->user_profile()->xuid();
  std::array<uint8_t, 8> id_bytes{};
  std::array<uint8_t, 16> key_bytes{};
  for (size_t i = 0; i < id_bytes.size(); ++i) {
    id_bytes[i] = static_cast<uint8_t>((local_xuid >> ((7 - i) * 8)) ^
                                       (sequence >> ((i % 8) * 8)));
  }
  for (size_t i = 0; i < key_bytes.size(); ++i) {
    const uint64_t mixed = local_xuid ^ (sequence * 0x9E3779B97F4A7C15ull) ^
                           (0xA5A5A5A5A5A5A5A5ull + i * 0x11ull);
    key_bytes[i] = static_cast<uint8_t>(mixed >> ((i % 8) * 8));
  }
  std::memcpy(key_id, id_bytes.data(), id_bytes.size());
  std::memcpy(key, key_bytes.data(), key_bytes.size());
  REXKRNL_INFO("[XEXONLINE][XNET] XNetCreateKey caller={} id={}", caller,
               FormatPacketPrefix(id_bytes.data(), id_bytes.size()));
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetRegisterKey_entry(u32 caller, mapped_void key_id, mapped_void key) {
  if (!key_id || !key) return X_ERROR_INVALID_PARAMETER;
  std::array<uint8_t, 16> key_bytes{};
  std::memcpy(key_bytes.data(), key, key_bytes.size());
  const uint64_t id = ReadXNetKeyId(key_id);
  if (!id || std::all_of(key_bytes.begin(), key_bytes.end(),
                         [](uint8_t value) { return value == 0; })) {
    REXKRNL_ERROR(
        "[XEXONLINE][XNET] XNetRegisterKey rejected invalid key caller={} id={:016X}",
        caller, id);
    return X_ERROR_INVALID_PARAMETER;
  }
  bool replaced_different_key = false;
  {
    std::scoped_lock lock(xnet_secure_state_mutex);
    const auto existing = xnet_registered_keys.find(id);
    replaced_different_key = existing != xnet_registered_keys.end() &&
                             existing->second != key_bytes;
    xnet_registered_keys[id] = key_bytes;
  }
  REXKRNL_INFO(
      "[XEXONLINE][XNET] XNetRegisterKey caller={} id={:016X} replaced_different={}",
      caller, id, replaced_different_key);
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetReplaceKey_entry(u32 caller, mapped_void key_id, mapped_void key) {
  return NetDll_XNetRegisterKey_entry(caller, key_id, key);
}

u32 NetDll_XNetUnregisterKey_entry(u32 caller, mapped_void key_id) {
  if (!key_id) return X_ERROR_INVALID_PARAMETER;
  const uint64_t id = ReadXNetKeyId(key_id);
  uint32_t removed_addresses = 0;
  {
    std::scoped_lock lock(xnet_secure_state_mutex);
    xnet_registered_keys.erase(id);
    for (auto it = xnet_translated_session_ids.begin();
         it != xnet_translated_session_ids.end();) {
      uint64_t mapped_id = 0;
      std::memcpy(&mapped_id, it->second.data(), sizeof(mapped_id));
      if (mapped_id == id) {
        xnet_connected_addresses.erase(it->first);
        it = xnet_translated_session_ids.erase(it);
        ++removed_addresses;
      } else {
        ++it;
      }
    }
  }
  REXKRNL_INFO(
      "[XEXONLINE][XNET] XNetUnregisterKey caller={} id={:016X} removed_addresses={}",
      caller, id, removed_addresses);
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetConnect_entry(u32 caller, u32 in_addr) {
  if (!in_addr) return X_ERROR_INVALID_PARAMETER;
  bool translated = false;
  {
    std::scoped_lock lock(xnet_secure_state_mutex);
    translated = xnet_translated_session_ids.contains(in_addr);
    if (!translated) {
      REXKRNL_WARN(
          "[XEXONLINE][XNET] XNetConnect rejected unmapped address caller={} address={:08X}",
          caller, in_addr);
      return X_ERROR_INVALID_PARAMETER;
    }
    xnet_connected_addresses.insert(in_addr);
  }
  REXKRNL_INFO("[XEXONLINE][XNET] XNetConnect caller={} address={:08X} mapped=1", caller,
               in_addr);
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetGetConnectStatus_entry(u32 caller, u32 in_addr) {
  if (!in_addr) return 0;  // XNET_CONNECT_STATUS_IDLE
  std::scoped_lock lock(xnet_secure_state_mutex);
  const bool translated = xnet_translated_session_ids.contains(in_addr);
  const bool connected = xnet_connected_addresses.contains(in_addr);
  const uint32_t status = (translated || connected) ? 2u : 0u;
  REXKRNL_INFO(
      "[XEXONLINE][XNET] XNetGetConnectStatus caller={} address={:08X} "
      "translated={} explicit={} status={}",
      caller, in_addr, translated, connected, status);
  return status;
}

u32 NetDll_XNetUnregisterInAddr_entry(u32 caller, u32 in_addr) {
  std::scoped_lock lock(xnet_secure_state_mutex);
  xnet_connected_addresses.erase(in_addr);
  xnet_translated_session_ids.erase(in_addr);
  REXKRNL_INFO("[XEXONLINE][XNET] XNetUnregisterInAddr caller={} address={:08X}",
               caller, in_addr);
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetSetOpt_entry(u32 caller, u32 option_id, mapped_void buffer_ptr,
                            u32 buffer_size) {
  if (option_id == 1) {
    if (!buffer_ptr || buffer_size < sizeof(XNetStartupParams)) {
      return 0x2738;  // WSAEMSGSIZE
    }
    std::memcpy(&xnet_startup_params, buffer_ptr, sizeof(XNetStartupParams));
    return X_ERROR_SUCCESS;
  }
  // Unknown XNet tuning options are advisory for System Link. Accepting them
  // is safer than aborting the title's online initialization path.
  REXKRNL_INFO("[XEXONLINE][XNET] XNetSetOpt caller={} option={} size={} accepted",
               caller, option_id, buffer_size);
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetRandom_entry(u32 caller, mapped_void buffer_ptr, u32 length) {
  if (!buffer_ptr && length) return X_ERROR_INVALID_PARAMETER;

  // Titles use XNetRandom for session nonces, arbitration material and other
  // values that must not repeat between creates. The previous constant 0xBB
  // stream made unrelated lobbies share identical supposedly-random state.
  static std::atomic<uint64_t> random_sequence{1};
  uint64_t state = random_sequence.fetch_add(1, std::memory_order_relaxed);
  state ^= static_cast<uint64_t>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
  state ^= REX_KERNEL_STATE()->user_profile()->xuid();
  state ^= static_cast<uint64_t>(caller) << 32;

  auto* output = static_cast<uint8_t*>(buffer_ptr);
  for (uint32_t i = 0; i < length; ++i) {
    // SplitMix64 step. This is not exposed as cryptographic security, but it
    // provides unique, well-distributed title-side random material.
    state += 0x9E3779B97F4A7C15ull;
    uint64_t mixed = state;
    mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
    mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
    mixed ^= mixed >> 31;
    output[i] = static_cast<uint8_t>(mixed >> ((i & 7u) * 8));
  }

  REXKRNL_INFO("[XEXONLINE][XNET] XNetRandom caller={} length={} prefix={}",
               caller, length,
               FormatPacketPrefix(output, std::min<uint32_t>(length, 8)));
  return X_ERROR_SUCCESS;
}

u32 NetDll_WSAStartup_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr) {
// TODO(benvanik): abstraction layer needed.
#if REX_PLATFORM_WIN32
  WSADATA wsaData;
  ZeroMemory(&wsaData, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsaData);

  auto data_out = REX_KERNEL_MEMORY()->TranslateVirtual(data_ptr.guest_address());

  if (data_ptr) {
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
    std::memcpy(&data_ptr->description, wsaData.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsaData.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsaData.iMaxSockets;
    data_ptr->max_udpdg = wsaData.iMaxUdpDg;

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    uint32_t vendor_ptr = memory::load_and_swap<uint32_t>(data_out + 0x190);
    memory::store_and_swap<uint32_t>(data_out + 0x190, vendor_ptr);
  }
#else
  int ret = 0;
  if (data_ptr) {
    // Guess these values!
    data_ptr->version = version;
    data_ptr->description[0] = '\0';
    data_ptr->system_status[0] = '\0';
    data_ptr->max_sockets = 100;
    data_ptr->max_udpdg = 1024;
  }
#endif

  // DEBUG
  /*
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return ret;
}

u32 NetDll_WSAStartupEx_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr,
                              u32 extended_version) {
  // The extended entry point is used by Generations during its online-service
  // bootstrap. It has the normal WSADATA contract plus an XDK compatibility
  // version, so the network state and output must be initialized exactly as
  // they are for NetDll_WSAStartup.
  REXKRNL_INFO("NetDll_WSAStartupEx(caller={}, version={:04X}, extended={})", caller, version,
               extended_version);
  return NetDll_WSAStartup_entry(caller, version, data_ptr);
}

u32 NetDll_WSACleanup_entry(u32 caller) {
  // This does nothing. Xenia needs WSA running.
  return 0;
}

u32 NetDll_WSAGetLastError_entry() {
  return XThread::GetLastError();
}

namespace {

void SetLastSocketError() {
#if REX_PLATFORM_WIN32
  XThread::SetLastError(WSAGetLastError());
#else
  XThread::SetLastError(0);
#endif
}

void CompleteWsaOverlapped(ppc_ptr_t<XWSAOVERLAPPED> overlapped, uint32_t transferred) {
  if (!overlapped) {
    return;
  }

  overlapped->internal = 0;
  overlapped->internal_high = transferred;
  if (overlapped->event_handle) {
    if (auto event = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(overlapped->event_handle)) {
      event->Set(0, false);
    }
  }
}

bool GetWsaBufferSize(ppc_ptr_t<XWSABUF> buffers, uint32_t buffer_count, uint32_t* size_out) {
  if (!size_out || (!buffers && buffer_count)) {
    return false;
  }

  uint32_t size = 0;
  for (uint32_t i = 0; i < buffer_count; ++i) {
    const uint32_t buffer_length = buffers[i].len.value;
    if (buffer_length && !buffers[i].buf_ptr) {
      return false;
    }
    if (buffer_length > UINT32_MAX - size) {
      return false;
    }
    size += buffer_length;
  }

  *size_out = size;
  return true;
}

}  // namespace

u32 NetDll_WSARecvFrom_entry(u32 caller, u32 socket, ppc_ptr_t<XWSABUF> buffers_ptr,
                             u32 buffer_count, mapped_u32 num_bytes_recv, mapped_u32 flags_ptr,
                             ppc_ptr_t<XSOCKADDR_IN> from_addr,
                             ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                             mapped_void completion_routine_ptr) {
  auto xsocket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket);
  if (!xsocket) {
    XThread::SetLastError(0x2736);  // WSAENOTSOCK
    return -1;
  }

  uint32_t buffer_size = 0;
  if (!GetWsaBufferSize(buffers_ptr, buffer_count, &buffer_size)) {
    XThread::SetLastError(0x271E);  // WSAEFAULT
    return -1;
  }

  std::vector<uint8_t> combined_buffer(buffer_size);
  N_XSOCKADDR_IN native_from = {};
  uint32_t native_from_len = from_addr ? sizeof(native_from) : 0;
  const uint32_t flags = flags_ptr ? flags_ptr.value() : 0;
  const int received =
      xsocket->RecvFrom(combined_buffer.data(), buffer_size, flags,
                        from_addr ? &native_from : nullptr, from_addr ? &native_from_len : nullptr);
  if (received < 0) {
    if (num_bytes_recv) {
      *num_bytes_recv = 0;
    }
    SetLastSocketError();
    return -1;
  }

  uint32_t copied = 0;
  uint32_t remaining = static_cast<uint32_t>(received);
  for (uint32_t i = 0; i < buffer_count && remaining; ++i) {
    const uint32_t copy_size = std::min(buffers_ptr[i].len.value, remaining);
    std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual(buffers_ptr[i].buf_ptr),
                combined_buffer.data() + copied, copy_size);
    copied += copy_size;
    remaining -= copy_size;
  }

  if (num_bytes_recv) {
    *num_bytes_recv = copied;
  }
  if (from_addr) {
    from_addr->sin_family = native_from.sin_family;
    from_addr->sin_port = native_from.sin_port;
    from_addr->sin_addr = native_from.sin_addr;
    std::memset(from_addr->x_sin_zero, 0, sizeof(from_addr->x_sin_zero));
  }
  if (flags_ptr) {
    *flags_ptr = flags;
  }

  CompleteWsaOverlapped(overlapped_ptr, copied);
  return X_ERROR_SUCCESS;
}

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
u32 NetDll_WSASendTo_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers,
                           u32 num_buffers, mapped_u32 num_bytes_sent, u32 flags,
                           ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len,
                           ppc_ptr_t<XWSAOVERLAPPED> overlapped, mapped_void completion_routine) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  uint32_t combined_buffer_size = 0;
  if (!GetWsaBufferSize(buffers, num_buffers, &combined_buffer_size)) {
    XThread::SetLastError(0x271E);  // WSAEFAULT
    return -1;
  }

  // Host sockets use contiguous buffers while the guest API accepts an array.
  std::vector<uint8_t> combined_buffer_mem(combined_buffer_size);
  uint32_t combined_buffer_offset = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    const uint32_t buffer_length = buffers[i].len.value;
    std::memcpy(combined_buffer_mem.data() + combined_buffer_offset,
                REX_KERNEL_MEMORY()->TranslateVirtual(buffers[i].buf_ptr), buffer_length);
    combined_buffer_offset += buffer_length;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  const int sent = socket->SendTo(combined_buffer_mem.data(), combined_buffer_size, flags,
                                  to_ptr ? &native_to : nullptr, to_len);
  if (sent < 0) {
    if (num_bytes_sent) {
      *num_bytes_sent = 0;
    }
    SetLastSocketError();
    return -1;
  }

  if (num_bytes_sent) {
    *num_bytes_sent = static_cast<uint32_t>(sent);
  }
  CompleteWsaOverlapped(overlapped, static_cast<uint32_t>(sent));
  return X_ERROR_SUCCESS;
}

u32 NetDll_WSARecv_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers_ptr,
                         u32 buffer_count, mapped_u32 num_bytes_recv, mapped_u32 flags_ptr,
                         ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                         mapped_void completion_routine_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);  // WSAENOTSOCK
    return -1;
  }

  uint32_t buffer_size = 0;
  if (!GetWsaBufferSize(buffers_ptr, buffer_count, &buffer_size)) {
    XThread::SetLastError(0x271E);  // WSAEFAULT
    return -1;
  }

  const uint32_t flags = flags_ptr ? flags_ptr.value() : 0;
  std::vector<uint8_t> combined_buffer(buffer_size);
  const int received = socket->Recv(combined_buffer.data(), buffer_size, flags);
  if (received < 0) {
    if (num_bytes_recv) {
      *num_bytes_recv = 0;
    }
    SetLastSocketError();
    return -1;
  }

  uint32_t copied = 0;
  uint32_t remaining = static_cast<uint32_t>(received);
  for (uint32_t i = 0; i < buffer_count && remaining; ++i) {
    const uint32_t copy_size = std::min(buffers_ptr[i].len.value, remaining);
    std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual(buffers_ptr[i].buf_ptr),
                combined_buffer.data() + copied, copy_size);
    copied += copy_size;
    remaining -= copy_size;
  }

  if (num_bytes_recv) {
    *num_bytes_recv = copied;
  }
  if (flags_ptr) {
    *flags_ptr = flags;
  }
  CompleteWsaOverlapped(overlapped_ptr, copied);
  return X_ERROR_SUCCESS;
}

u32 NetDll_WSASend_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers, u32 num_buffers,
                         mapped_u32 num_bytes_sent, u32 flags, ppc_ptr_t<XWSAOVERLAPPED> overlapped,
                         mapped_void completion_routine) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);  // WSAENOTSOCK
    return -1;
  }

  uint32_t combined_buffer_size = 0;
  if (!GetWsaBufferSize(buffers, num_buffers, &combined_buffer_size)) {
    XThread::SetLastError(0x271E);  // WSAEFAULT
    return -1;
  }

  std::vector<uint8_t> combined_buffer_mem(combined_buffer_size);
  uint32_t combined_buffer_offset = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    const uint32_t buffer_length = buffers[i].len.value;
    std::memcpy(combined_buffer_mem.data() + combined_buffer_offset,
                REX_KERNEL_MEMORY()->TranslateVirtual(buffers[i].buf_ptr), buffer_length);
    combined_buffer_offset += buffer_length;
  }

  const int sent = socket->Send(combined_buffer_mem.data(), combined_buffer_size, flags);
  if (sent < 0) {
    if (num_bytes_sent) {
      *num_bytes_sent = 0;
    }
    SetLastSocketError();
    return -1;
  }

  if (num_bytes_sent) {
    *num_bytes_sent = static_cast<uint32_t>(sent);
  }

  CompleteWsaOverlapped(overlapped, static_cast<uint32_t>(sent));
  return X_ERROR_SUCCESS;
}

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 num_events, mapped_u32 events, u32 wait_all,
                                          u32 timeout, u32 alertable) {
  if (num_events > 64) {
    XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return ~0u;
  }

  uint64_t timeout_wait = (uint64_t)timeout;

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(num_events, events, wait_all, 1, alertable,
                                                    timeout != -1 ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return ~0u;
  }
  return 0;
}

u32 NetDll_WSACreateEvent_entry() {
  XEvent* ev = new XEvent(REX_KERNEL_STATE());
  ev->Initialize(true, false);
  return ev->handle();
}

u32 NetDll_WSACloseEvent_entry(u32 event_handle) {
  X_STATUS result = REX_KERNEL_OBJECTS()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAResetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAGetOverlappedResult_entry(u32 socket_handle, ppc_ptr_t<XWSAOVERLAPPED> overlapped,
                                        mapped_u32 transferred, u32 wait, mapped_u32 flags_ptr) {
  if (!overlapped) {
    XThread::SetLastError(0x271E);  // WSAEFAULT
    return 0;
  }

  if (wait && overlapped->event_handle) {
    if (auto event = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(overlapped->event_handle)) {
      event->Wait(0, 0, true, nullptr);
    }
  }
  if (transferred) {
    *transferred = overlapped->internal_high;
  }
  if (flags_ptr) {
    *flags_ptr = 0;
  }
  XThread::SetLastError(0);
  return 1;
}

u32 NetDll_WSACancelOverlappedIO_entry(u32 socket_handle) {
  XThread::SetLastError(0);
  return X_ERROR_SUCCESS;
}

u32 NetDll_WSAEventSelect_entry(u32 socket_handle, u32 event_handle, u32 network_events) {
  XThread::SetLastError(0);
  return X_ERROR_SUCCESS;
}

struct XnAddrStatus {
  // Address acquisition is not yet complete
  static const uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
  // XNet is uninitialized or no debugger found
  static const uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
  // Host has ethernet address (no IP address)
  static const uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
  // Host has statically assigned IP address
  static const uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
  // Host has DHCP assigned IP address
  static const uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
  // Host has PPPoE assigned IP address
  static const uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
  // Host has one or more gateways configured
  static const uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
  // Host has one or more DNS servers configured
  static const uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
  // Host is currently connected to online service
  static const uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  // Network configuration requires troubleshooting
  static const uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

u32 NetDll_XNetGetTitleXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  if (!addr_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto endpoint = ResolveSystemLinkEndpoint();
  if (!endpoint) {
    addr_ptr.Zero();
    return XnAddrStatus::XNET_GET_XNADDR_NONE;
  }

  FillXnAddr(addr_ptr, *endpoint);
  return XnAddrStatus::XNET_GET_XNADDR_STATIC | XnAddrStatus::XNET_GET_XNADDR_GATEWAY |
         XnAddrStatus::XNET_GET_XNADDR_DNS | XnAddrStatus::XNET_GET_XNADDR_ONLINE;
}

u32 NetDll_XNetGetDebugXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

u32 NetDll_XNetXnAddrToMachineId_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr, mapped_u32 id_ptr) {
  if (!addr_ptr || !id_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  uint32_t machine_id = ntohl(addr_ptr->ina.s_addr);
  for (uint8_t byte : addr_ptr->abEnet) {
    machine_id = (machine_id * 33u) ^ byte;
  }
  *id_ptr = machine_id ? machine_id : 1;
  return X_ERROR_SUCCESS;
}

void NetDll_XNetInAddrToString_entry(u32 caller, u32 in_addr, mapped_string string_out,
                                     u32 string_size) {
  ::in_addr address = {};
  address.s_addr = htonl(in_addr);
  rex::string::rex_strcpy(string_out, string_size, FormatAddress(address).c_str());
}

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address)
u32 NetDll_XNetXnAddrToInAddr_entry(u32 caller, ppc_ptr_t<XNADDR> xn_addr, mapped_void xid,
                                    mapped_void in_addr) {
  if (!xn_addr || !in_addr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  std::memcpy(in_addr, &xn_addr->ina, sizeof(xn_addr->ina));
  const uint32_t translated_address = xn_addr->ina.s_addr;
  {
    std::scoped_lock lock(xnet_secure_state_mutex);
    auto& session_id = xnet_translated_session_ids[translated_address];
    session_id.fill(0);
    if (xid) {
      std::memcpy(session_id.data(), xid, session_id.size());
    }
    // The System Link socket path uses the translated native address directly;
    // registration therefore represents a connected secure-address mapping.
    xnet_connected_addresses.insert(translated_address);
  }
  if (xnaddr_translation_trace_count.fetch_add(1, std::memory_order_relaxed) < 32) {
    REXKRNL_INFO("XNetXnAddrToInAddr: address={} session_id=[{}]",
                 FormatAddress(xn_addr->ina),
                 xid ? FormatPacketPrefix(static_cast<uint8_t*>(xid), 8) : "none");
  }
  return X_ERROR_SUCCESS;
}

// Does the reverse of the above.
// FIXME: Arguments may not be correct.
u32 NetDll_XNetInAddrToXnAddr_entry(u32 caller, mapped_void in_addr, ppc_ptr_t<XNADDR> xn_addr,
                                    mapped_void xid) {
  if (!in_addr || !xn_addr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto endpoint = ResolveSystemLinkEndpoint();
  if (!endpoint) {
    xn_addr.Zero();
    return X_ERROR_FUNCTION_FAILED;
  }

  FillXnAddr(xn_addr, *endpoint);
  std::memcpy(&xn_addr->ina, in_addr, sizeof(xn_addr->ina));
  uint32_t translated_address = 0;
  std::memcpy(&translated_address, in_addr, sizeof(translated_address));
  std::array<uint8_t, 8> session_id{};
  bool found_mapping = false;
  {
    std::scoped_lock lock(xnet_secure_state_mutex);
    const auto it = xnet_translated_session_ids.find(translated_address);
    if (it != xnet_translated_session_ids.end()) {
      session_id = it->second;
      found_mapping = true;
    }
  }
  if (xid) {
    std::memcpy(xid, session_id.data(), session_id.size());
  }
  // XNADDR in this ReXGlue revision has no embedded online/session ID.
  // Keep the association in xnet_translated_session_ids and return it through
  // the explicit XNKID output only.
  REXKRNL_INFO(
      "[XEXONLINE][XNET] XNetInAddrToXnAddr caller={} address={:08X} "
      "mapping={} session_id=[{}]",
      caller, translated_address, found_mapping,
      FormatPacketPrefix(session_id.data(), session_id.size()));
  return X_ERROR_SUCCESS;
}

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
u32 NetDll_XNetSetSystemLinkPort_entry(u32 caller, u32 port) {
  if (port > UINT16_MAX) {
    return X_ERROR_INVALID_PARAMETER;
  }

  const uint16_t system_link_game_port = static_cast<uint16_t>(port);
  {
    std::scoped_lock lock(system_link_mutex);
    system_link_port = system_link_game_port;
  }
  SystemLinkUpdateGamePort(system_link_game_port);
  REXKRNL_INFO("System link port set to {}", system_link_game_port);
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetGetSystemLinkPort_entry(u32 caller) {
  std::scoped_lock lock(system_link_mutex);
  return system_link_port;
}

// https://github.com/ILOVEPIE/Cxbx-Reloaded/blob/master/src/CxbxKrnl/EmuXOnline.h#L39
struct XEthernetStatus {
  static const uint32_t XNET_ETHERNET_LINK_ACTIVE = 0x01;
  static const uint32_t XNET_ETHERNET_LINK_100MBPS = 0x02;
  static const uint32_t XNET_ETHERNET_LINK_10MBPS = 0x04;
  static const uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
  static const uint32_t XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;
};

u32 NetDll_XNetGetEthernetLinkStatus_entry(u32 caller) {
  if (!ResolveSystemLinkEndpoint()) {
    return 0;
  }
  return XEthernetStatus::XNET_ETHERNET_LINK_ACTIVE | XEthernetStatus::XNET_ETHERNET_LINK_100MBPS |
         XEthernetStatus::XNET_ETHERNET_LINK_FULL_DUPLEX;
}

u32 NetDll_XNetDnsLookup_entry(u32 caller, mapped_string host, u32 event_handle, mapped_u32 pdns) {
  if (!pdns || !host || host.value().empty()) {
    return X_ERROR_INVALID_PARAMETER;
  }

  const auto dns_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNDNS));
  if (!dns_guest) {
    return 8;  // ERROR_NOT_ENOUGH_MEMORY
  }
  auto dns = REX_KERNEL_MEMORY()->TranslateVirtual<XNDNS*>(dns_guest);
  std::memset(dns, 0, sizeof(XNDNS));

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* results = nullptr;
  const std::string host_name(host.value());
  const int lookup_result = getaddrinfo(host_name.c_str(), nullptr, &hints, &results);
  if (lookup_result == 0 && results) {
    uint32_t count = 0;
    for (auto* current = results; current && count < 8; current = current->ai_next) {
      if (current->ai_family != AF_INET || !current->ai_addr) continue;
      const auto* address = reinterpret_cast<const sockaddr_in*>(current->ai_addr);
      bool duplicate = false;
      for (uint32_t i = 0; i < count; ++i) {
        if (dns->aina[i].s_addr == address->sin_addr.s_addr) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) dns->aina[count++] = address->sin_addr;
    }
    dns->cina = count;
    dns->status = count ? 0 : WSAHOST_NOT_FOUND;
    freeaddrinfo(results);
  } else {
    dns->status = lookup_result ? lookup_result : WSAHOST_NOT_FOUND;
    dns->cina = 0;
    if (results) freeaddrinfo(results);
  }

  *pdns = dns_guest;
  if (event_handle) {
    if (auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle)) {
      ev->Set(0, false);
    }
  }
  REXKRNL_INFO("[XEXONLINE][XNET] DNS '{}' status={} count={}", host.value(),
               static_cast<int32_t>(dns->status), static_cast<uint32_t>(dns->cina));
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetDnsRelease_entry(u32 caller, ppc_ptr_t<XNDNS> dns) {
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(dns.guest_address());
  return 0;
}

u32 NetDll_XNetQosServiceLookup_entry(u32 caller, u32 flags, u32 event_handle, mapped_u32 pqos) {
  // Set pqos as some games will try accessing it despite a zero-result service lookup.
  // Do not publish a null/invalid guest pointer when the system heap is exhausted.
  if (!pqos) {
    return X_ERROR_INVALID_PARAMETER;
  }
  const auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNQOS));
  if (!qos_guest) {
    *pqos = 0;
    return X_STATUS_NO_MEMORY;
  }
  auto* qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
  if (!qos) {
    REX_KERNEL_MEMORY()->SystemHeapFree(qos_guest);
    *pqos = 0;
    return X_STATUS_NO_MEMORY;
  }
  std::memset(qos, 0, sizeof(XNQOS));
  qos->count = qos->count_pending = 0;
  {
    std::scoped_lock allocation_lock(qos_allocations_mutex);
    qos_root_allocations.insert(qos_guest);
  }
  *pqos = qos_guest;
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

// The title's XAM wrapper forwards twelve arguments for this variant of
// XNetQosLookup. The final pointer is the XNQOS out parameter. Keeping the
// complete signature preserves the guest PPC ABI, even though local peer
// discovery is not yet populated by the transport.
u32 NetDll_XNetQosLookup_entry(u32 caller, u32 cxna, u32 apxna, u32 apxnkid, u32 apxnkey, u32 cina,
                               u32 aina, u32 adw_service_id, u32 probes, u32 bits_per_sec,
                               u32 flags, u32 event_handle, mapped_u32 pqos) {
  if (!pqos) {
    return X_ERROR_INVALID_PARAMETER;
  }

  const uint32_t target_count = cxna + cina;
  const size_t qos_size =
      sizeof(XNQOS) + (target_count > 0 ? (target_count - 1) * sizeof(XNQOSINFO) : 0);
  auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(qos_size);
  if (!qos_guest) {
    return X_STATUS_NO_MEMORY;
  }

  auto* qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
  if (!qos) {
    REX_KERNEL_MEMORY()->SystemHeapFree(qos_guest);
    return X_STATUS_NO_MEMORY;
  }
  std::memset(qos, 0, qos_size);
  {
    std::scoped_lock allocation_lock(qos_allocations_mutex);
    qos_root_allocations.insert(qos_guest);
  }
  qos->count = target_count;
  qos->count_pending = 0;
  *pqos = qos_guest;

  constexpr uint8_t kQosComplete = 0x01;
  constexpr uint8_t kQosTargetContacted = 0x02;
  constexpr uint8_t kQosDataReceived = 0x08;

  auto* session_id_pointer_array =
      apxnkid ? REX_KERNEL_MEMORY()->TranslateVirtual<be<uint32_t>*>(apxnkid) : nullptr;
  auto* session_id_direct_array =
      apxnkid ? REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(apxnkid) : nullptr;

  for (uint32_t i = 0; i < target_count; ++i) {
    auto& info = qos->info[i];
    // Complete means the lookup finished; contacted is set only after the
    // target is proven to exist. Previously every stale/unknown target looked
    // healthy and could be selected by matchmaking or retained by the lobby.
    info.flags = kQosComplete;
    info.probes_xmit = static_cast<uint16_t>(std::max(probes, 1u));
    info.probes_recv = 0;
    info.rtt_min_in_msecs = 0;
    info.rtt_med_in_msecs = 0;
    info.up_bits_per_sec = 0;
    info.down_bits_per_sec = 0;

    bool target_contacted = false;
    if (i < cxna && (session_id_pointer_array || session_id_direct_array)) {
      std::array<uint8_t, 8> session_id = {};
      const uint32_t session_id_ptr =
          session_id_pointer_array ? static_cast<uint32_t>(session_id_pointer_array[i]) : 0;
      if (session_id_ptr >= 0x10000) {
        std::memcpy(session_id.data(), REX_KERNEL_MEMORY()->TranslateVirtual(session_id_ptr),
                    session_id.size());
      } else if (session_id_direct_array) {
        std::memcpy(session_id.data(), session_id_direct_array + (i * session_id.size()),
                    session_id.size());
      }
      if (std::any_of(session_id.begin(), session_id.end(),
                      [](uint8_t byte) { return byte != 0; })) {
        const auto session = SystemLinkFindSessionById(session_id);
        if (session) {
          target_contacted = true;
          info.flags |= kQosTargetContacted;
          info.probes_recv = static_cast<uint16_t>(std::max(probes, 1u));
          info.rtt_min_in_msecs = 1;
          info.rtt_med_in_msecs = 1;
          info.up_bits_per_sec = bits_per_sec ? bits_per_sec : 100000000;
          info.down_bits_per_sec = bits_per_sec ? bits_per_sec : 100000000;
        }
        if (session && !session->qos_data.empty()) {
          const uint32_t data_size = static_cast<uint32_t>(session->qos_data.size());
          const uint32_t data_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(data_size);
          if (data_guest) {
            std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual(data_guest), session->qos_data.data(),
                        data_size);
            info.data_len = static_cast<uint16_t>(std::min<uint32_t>(data_size, UINT16_MAX));
            info.data_ptr = data_guest;
            info.flags |= kQosDataReceived;
            std::scoped_lock allocation_lock(qos_allocations_mutex);
            qos_child_allocations[qos_guest].push_back(data_guest);
          }
        }
      }
    } else if (i >= cxna) {
      // Raw-address QoS targets are an array of IN_ADDR values. The previous
      // implementation treated the array pointer itself as proof that every
      // element was reachable, so zero, stale and unrelated addresses all
      // received perfect QoS. Validate the individual address against the
      // same secure translation/connect registry used by the join path.
      const uint32_t raw_index = i - cxna;
      uint32_t raw_address = 0;
      if (aina && raw_index < cina) {
        auto* raw_addresses =
            REX_KERNEL_MEMORY()->TranslateVirtual<be<uint32_t>*>(aina);
        if (raw_addresses) {
          raw_address = static_cast<uint32_t>(raw_addresses[raw_index]);
        }
      }
      if (raw_address) {
        std::scoped_lock secure_lock(xnet_secure_state_mutex);
        target_contacted =
            xnet_translated_session_ids.contains(raw_address) ||
            xnet_connected_addresses.contains(raw_address);
      }
      if (target_contacted) {
        info.flags |= kQosTargetContacted;
        info.probes_recv = static_cast<uint16_t>(std::max(probes, 1u));
        info.rtt_min_in_msecs = 1;
        info.rtt_med_in_msecs = 1;
        info.up_bits_per_sec = bits_per_sec ? bits_per_sec : 100000000;
        info.down_bits_per_sec = bits_per_sec ? bits_per_sec : 100000000;
      }
    }

    REXKRNL_INFO(
        "[XEXONLINE][QOS] result={} contacted={} flags={:02X} probes={}/{} rtt={}",
        i, target_contacted, static_cast<uint32_t>(info.flags),
        static_cast<uint16_t>(info.probes_recv),
        static_cast<uint16_t>(info.probes_xmit),
        static_cast<uint16_t>(info.rtt_med_in_msecs));
  }

  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    if (ev) {
      ev->Set(0, false);
    }
  }

  REXKRNL_INFO(
      "XNetQosLookup completed: secure_targets={}, raw_targets={}, results={}, probes={}, "
      "flags={:08X}, xnkid={:08X}",
      cxna, cina, target_count, probes, flags, apxnkid);
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetQosRelease_entry(u32 caller, ppc_ptr_t<XNQOS> qos) {
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  std::vector<uint32_t> child_allocations;
  {
    std::scoped_lock lock(qos_allocations_mutex);
    if (!qos_root_allocations.erase(qos.guest_address())) {
      REXKRNL_WARN("[XEXONLINE][QOS] release rejected unknown/already-freed root={:08X}",
                   qos.guest_address());
      return X_ERROR_INVALID_PARAMETER;
    }
    const auto it = qos_child_allocations.find(qos.guest_address());
    if (it != qos_child_allocations.end()) {
      child_allocations = std::move(it->second);
      qos_child_allocations.erase(it);
    }
  }
  for (const uint32_t allocation : child_allocations) {
    REX_KERNEL_MEMORY()->SystemHeapFree(allocation);
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(qos.guest_address());
  REXKRNL_INFO("[XEXONLINE][QOS] released root={:08X} children={}",
               qos.guest_address(), child_allocations.size());
  return 0;
}

u32 NetDll_XNetQosListen_entry(u32 caller, mapped_void id, mapped_void data, u32 data_size, u32 r7,
                               u32 flags) {
  // XNET_QOS_LISTEN_DISABLE. Generations uses this while leaving/deleting a
  // lobby. Previously the call reset the state and then synthesized/published
  // a fresh payload, making a dead lobby continue to pass QoS validation.
  constexpr uint32_t kQosListenDisable = 0x00000002u;
  if (flags & kQosListenDisable) {
    std::array<uint8_t, 8> disabled_session_id{};
    {
      std::scoped_lock lock(system_link_mutex);
      disabled_session_id = qos_listen_state.session_id;
      if (id) {
        std::memcpy(disabled_session_id.data(), id, disabled_session_id.size());
      }
      qos_listen_state = {};
    }
    if (std::any_of(disabled_session_id.begin(), disabled_session_id.end(),
                    [](uint8_t value) { return value != 0; })) {
      SystemLinkUpdateQosData(disabled_session_id, {});
    }
    REXKRNL_INFO(
        "[XEXONLINE][QOS] XNetQosListen disabled caller={} flags={:08X} session={}",
        caller, flags,
        FormatPacketPrefix(disabled_session_id.data(), disabled_session_id.size()));
    return X_ERROR_SUCCESS;
  }

  std::scoped_lock lock(system_link_mutex);
  qos_listen_state = {};
  if (id) {
    std::memcpy(qos_listen_state.session_id.data(), id, qos_listen_state.session_id.size());
  }
  if (data && data_size) {
    qos_listen_state.data.resize(std::min<u32>(data_size, kSystemLinkQosDataCapacity));
    std::memcpy(qos_listen_state.data.data(), data, qos_listen_state.data.size());
  } else if (id) {
    // Generations arms QoS with a null/zero-length payload. Real XAM still
    // returns a contacted result with title/session identity. Publish a small,
    // deterministic compatibility payload so the searching instance receives
    // XNET_XNQOSINFO_DATA_RECEIVED instead of remaining in its validation loop.
    // Layout: 'RGLQ', version, payload size, 8-byte XNKID, listen flags.
    qos_listen_state.data.resize(20);
    const uint32_t magic = 0x514C4752u;  // "RGLQ" in guest byte order.
    const uint16_t version = 1;
    const uint16_t payload_size = static_cast<uint16_t>(qos_listen_state.data.size());
    std::memcpy(qos_listen_state.data.data() + 0, &magic, sizeof(magic));
    std::memcpy(qos_listen_state.data.data() + 4, &version, sizeof(version));
    std::memcpy(qos_listen_state.data.data() + 6, &payload_size, sizeof(payload_size));
    std::memcpy(qos_listen_state.data.data() + 8, qos_listen_state.session_id.data(),
                qos_listen_state.session_id.size());
    std::memcpy(qos_listen_state.data.data() + 16, &flags, sizeof(flags));
    REXKRNL_INFO("XNetQosListen synthesized {}-byte System Link compatibility payload",
                 qos_listen_state.data.size());
  }
  qos_listen_state.flags = flags;
  const auto session_id = qos_listen_state.session_id;
  const auto qos_data = qos_listen_state.data;
  // Publish outside the state update path so clients receive the title's QoS
  // payload, which Generations uses while validating a search result.
  SystemLinkUpdateQosData(session_id, qos_data);
  REXKRNL_INFO("XNetQosListen armed (guest={} bytes, published={} bytes, flags={:08X})",
               data_size, qos_data.size(), flags);
  return X_ERROR_SUCCESS;
}

u32 NetDll_inet_addr_entry(mapped_string addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  uint32_t addr = inet_addr(addr_ptr);
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (addr == -1 && !addr_ptr.value().length()) {
    return 0;
  }

  return rex::byte_swap(addr);
}

u32 NetDll_socket_entry(u32 caller, u32 af, u32 type, u32 protocol) {
  XSocket* socket = new XSocket(REX_KERNEL_STATE());
  X_STATUS result =
      socket->Initialize(XSocket::AddressFamily((uint32_t)af), XSocket::Type((uint32_t)type),
                         XSocket::Protocol((uint32_t)protocol));

  if (XFAILED(result)) {
    socket->Release();

    SetHostSocketLastError();
    REXKRNL_WARN("NetDll_socket failed: af={} type={} proto={} error={}", af, type, protocol,
                 GetHostSocketLastError());
    return -1;
  }

  REXKRNL_INFO("NetDll_socket: af={} type={} proto={} -> handle={:08X}", af, type, protocol,
               socket->handle());
  return socket->handle();
}

u32 NetDll_closesocket_entry(u32 caller, u32 socket_handle) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  socket->Close();
  socket->ReleaseHandle();
  REXKRNL_INFO("NetDll_closesocket: handle={:08X}", socket_handle);
  return 0;
}

i32 NetDll_shutdown_entry(u32 caller, u32 socket_handle, i32 how) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
    SetHostSocketLastError();
    REXKRNL_WARN("NetDll_shutdown failed: handle={:08X} how={} error={}", socket_handle, how,
                 GetHostSocketLastError());
  }
  return ret;
}

u32 NetDll_setsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, u32 optlen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  if (XFAILED(status)) {
    SetHostSocketLastError();
    REXKRNL_WARN("NetDll_setsockopt failed: handle={:08X} level={:08X} opt={:08X} len={} error={}",
                 socket_handle, level, optname, optlen, GetHostSocketLastError());
    return -1;
  }
  REXKRNL_INFO("NetDll_setsockopt: handle={:08X} level={:08X} opt={:08X} len={}", socket_handle,
               level, optname, optlen);
  return 0;
}

u32 NetDll_ioctlsocket_entry(u32 caller, u32 socket_handle, u32 cmd, mapped_void arg_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (XFAILED(status)) {
    SetHostSocketLastError();
    REXKRNL_WARN("NetDll_ioctlsocket failed: handle={:08X} cmd={:08X} error={}", socket_handle, cmd,
                 GetHostSocketLastError());
    return -1;
  }

  REXKRNL_INFO("NetDll_ioctlsocket: handle={:08X} cmd={:08X}", socket_handle, cmd);
  return 0;
}

u32 NetDll_bind_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
  X_STATUS status = socket->Bind(&native_name, namelen);
  if (XFAILED(status)) {
    SetHostSocketLastError();
    REXKRNL_WARN("NetDll_bind failed: handle={:08X} addr={} len={} error={}", socket_handle,
                 FormatSockaddrIn(native_name), namelen, GetHostSocketLastError());
    return -1;
  }

  REXKRNL_INFO("NetDll_bind: handle={:08X} addr={} len={}", socket_handle,
               FormatSockaddrIn(native_name), namelen);
  return 0;
}


namespace {

constexpr std::array<uint8_t, 24> kGenerationsSystemLinkHandshake = {
    0x47, 0x45, 0x4E, 0x53,  // "GENS"
    0x00, 0x00, 0x00, 0x01,  // compatibility protocol version
    0x00, 0x00, 0x00, 0x18,  // packet size
    0x53, 0x59, 0x53, 0x4C,  // "SYSL"
    0x49, 0x4E, 0x4B, 0x00,  // "INK"
    0x00, 0x00, 0x00, 0x01,
};

void SendGenerationsSystemLinkHandshake(const object_ref<XSocket>& socket,
                                        uint32_t socket_handle,
                                        std::string_view direction) {
  if (!socket || !socket->native_handle()) {
    REXKRNL_WARN(
        "Generations System Link compatibility handshake skipped {}: "
        "handle={:08X} native socket unavailable",
        direction, socket_handle);
    return;
  }

  // Both Generations peers use nonblocking TCP sockets. Immediately after a
  // successful connect/accept, send can still transiently report
  // WSAEWOULDBLOCK. The previous one-shot helper silently dropped the only
  // handshake packet in that case, leaving the client at "Transmitting" while
  // the host saw an accepted transport with no bytes. Retry for a short,
  // bounded interval and preserve partial-send progress.
  size_t offset = 0;
  uint32_t attempts = 0;
  constexpr uint32_t kMaxAttempts = 40;
  while (offset < kGenerationsSystemLinkHandshake.size() &&
         attempts++ < kMaxAttempts) {
    const int sent = socket->Send(
        const_cast<uint8_t*>(kGenerationsSystemLinkHandshake.data()) + offset,
        static_cast<uint32_t>(kGenerationsSystemLinkHandshake.size() - offset),
        0);
    if (sent > 0) {
      offset += static_cast<size_t>(sent);
      continue;
    }

    if (sent == 0) {
      break;
    }

    SetHostSocketLastError();
    const uint32_t error = GetHostSocketLastError();
    if (!IsHostSocketWouldBlock(error)) {
      REXKRNL_WARN(
          "Generations System Link compatibility handshake failed {}: "
          "handle={:08X} error={} sent={}/{}",
          direction, socket_handle, error, offset,
          kGenerationsSystemLinkHandshake.size());
      return;
    }

#if REX_PLATFORM_WIN32
    fd_set write_set = {};
    FD_ZERO(&write_set);
    FD_SET(socket->native_handle(), &write_set);
    timeval wait_time = {};
    wait_time.tv_sec = 0;
    wait_time.tv_usec = 50000;
    select(0, nullptr, &write_set, nullptr, &wait_time);
#endif
  }

  if (offset == kGenerationsSystemLinkHandshake.size()) {
    XThread::SetLastError(0);
    REXKRNL_INFO(
        "Generations System Link compatibility handshake sent {}: "
        "handle={:08X} bytes={} attempts={} data=[{}]",
        direction, socket_handle, offset, attempts,
        FormatPacketPrefix(kGenerationsSystemLinkHandshake.data(),
                           kGenerationsSystemLinkHandshake.size()));
  } else {
    REXKRNL_WARN(
        "Generations System Link compatibility handshake timed out {}: "
        "handle={:08X} sent={}/{} attempts={}",
        direction, socket_handle, offset,
        kGenerationsSystemLinkHandshake.size(), attempts);
  }
}

}  // namespace

u32 NetDll_connect_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_name(name);
  const auto* native_name_in = native_name.address_family == 2 && namelen >= sizeof(N_XSOCKADDR_IN)
                                   ? reinterpret_cast<const N_XSOCKADDR_IN*>(&native_name)
                                   : nullptr;
  X_STATUS status = socket->Connect(&native_name, namelen);
  if (XFAILED(status)) {
    SetHostSocketLastError();
    const uint32_t connect_error = GetHostSocketLastError();

#if REX_PLATFORM_WIN32
    // Generations creates a nonblocking TCP socket, calls connect, then waits
    // for writability. On the original XAM path the completion is observed as
    // a successful connect transition. Returning WSAEWOULDBLOCK here leaves
    // the title in its transmitting state even though select later reports the
    // socket writable, and no native handshake packet is ever sent.
    //
    // Complete this specific System Link TCP connect in-place for a bounded
    // interval. This preserves the title's own packet construction and only
    // changes how the already-established host connection is reported.
    if (native_name_in &&
        (connect_error == WSAEWOULDBLOCK || connect_error == WSAEINPROGRESS ||
         connect_error == WSAEALREADY)) {
      fd_set write_set = {};
      fd_set except_set = {};
      FD_ZERO(&write_set);
      FD_ZERO(&except_set);
      FD_SET(socket->native_handle(), &write_set);
      FD_SET(socket->native_handle(), &except_set);
      timeval wait_time = {};
      wait_time.tv_sec = 2;
      wait_time.tv_usec = 0;
      const int ready = select(0, nullptr, &write_set, &except_set, &wait_time);
      if (ready > 0 && FD_ISSET(socket->native_handle(), &write_set)) {
        int socket_error = 0;
        int socket_error_size = sizeof(socket_error);
        if (getsockopt(socket->native_handle(), SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&socket_error),
                       &socket_error_size) == 0 && socket_error == 0) {
          XThread::SetLastError(0);
          REXKRNL_INFO(
              "NetDll_connect completed System Link connection inline: "
              "handle={:08X} addr={} len={}",
              socket_handle, FormatSockaddrIn(*native_name_in), namelen);
          // The accepted host socket sends the compatibility preamble. Sending
          // a second copy from the client puts "GENS" in front of the title's
          // native length-prefixed type-0 packet on the host and permanently
          // wedges its frame parser.
          return 0;
        }
        if (socket_error) {
          XThread::SetLastError(socket_error);
        }
      }
    }
#endif

    if (native_name_in) {
      REXKRNL_INFO("NetDll_connect pending/failed: handle={:08X} addr={} len={} error={}",
                   socket_handle, FormatSockaddrIn(*native_name_in), namelen,
                   GetHostSocketLastError());
    } else {
      REXKRNL_INFO("NetDll_connect pending/failed: handle={:08X} family={} len={} error={}",
                   socket_handle, native_name.address_family, namelen,
                   GetHostSocketLastError());
    }
    return -1;
  }
  if (native_name_in) {
    REXKRNL_INFO("NetDll_connect: handle={:08X} addr={} len={}", socket_handle,
                 FormatSockaddrIn(*native_name_in), namelen);
  } else {
    REXKRNL_INFO("NetDll_connect: handle={:08X} family={} len={}", socket_handle,
                 native_name.address_family, namelen);
  }
  // The host sends the one compatibility preamble from accept(). The client
  // must keep its outbound stream clean for the title's native protocol.
  return 0;
}

u32 NetDll_listen_entry(u32 caller, u32 socket_handle, i32 backlog) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    SetHostSocketLastError();
    REXKRNL_WARN("NetDll_listen failed: handle={:08X} backlog={} error={}", socket_handle, backlog,
                 GetHostSocketLastError());
    return -1;
  }

  const uint16_t bound_port = socket->bound_port();
  bool inferred_system_link_port = false;
  if (bound_port) {
    std::scoped_lock lock(system_link_mutex);
    if (!system_link_port) {
      system_link_port = bound_port;
      inferred_system_link_port = true;
    }
  }
  if (inferred_system_link_port) {
    SystemLinkUpdateGamePort(bound_port);
    apps::SystemLinkNotifyGamePort(bound_port);
    REXKRNL_INFO("System link game port inferred from listening socket: {}", bound_port);
  } else if (bound_port) {
    apps::SystemLinkNotifyGamePort(bound_port);
  }

  REXKRNL_INFO("NetDll_listen: handle={:08X} port={} backlog={}", socket_handle, bound_port,
               backlog);
  return 0;
}

u32 NetDll_accept_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> addr_ptr,
                        mapped_u32 addrlen_ptr) {
  if (!addr_ptr) {
    // WSAEFAULT
    XThread::SetLastError(0x271E);
    return -1;
  }

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = *addrlen_ptr;
  auto new_socket = socket->Accept(&native_addr, &native_len);
  if (new_socket) {
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data, *addrlen_ptr - 2);
    *addrlen_ptr = native_len;

    if (native_addr.address_family == 2 && native_len >= static_cast<int>(sizeof(N_XSOCKADDR_IN))) {
      const auto& native_addr_in = reinterpret_cast<const N_XSOCKADDR_IN&>(native_addr);
      REXKRNL_INFO("NetDll_accept: listener={:08X} peer={} -> handle={:08X}", socket_handle,
                   FormatSockaddrIn(native_addr_in), new_socket->handle());
      apps::SystemLinkNotifyAcceptedPeer(socket->bound_port());
      SendGenerationsSystemLinkHandshake(new_socket, new_socket->handle(),
                                         "after accept");
    } else {
      REXKRNL_INFO("NetDll_accept: listener={:08X} family={} -> handle={:08X}", socket_handle,
                   native_addr.address_family, new_socket->handle());
    }

    return new_socket->handle();
  } else {
    SetHostSocketLastError();
    const uint32_t error = GetHostSocketLastError();
    if (!IsHostSocketWouldBlock(error)) {
      REXKRNL_WARN("NetDll_accept failed: handle={:08X} error={}", socket_handle, error);
    }
    return -1;
  }
}

struct x_fd_set {
  rex::be<uint32_t> fd_count;
  rex::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[64];

  void Load(const x_fd_set* guest_set) {
    assert_true(guest_set->fd_count < 64);
    this->count = 0;
    const uint32_t guest_count = guest_set->fd_count;
    for (uint32_t i = 0; i < guest_count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        break;
      }
      // Convert from Xenia -> native
      auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
      if (!socket) {
        REXKRNL_WARN("NetDll_select skipped invalid fd_set handle {:08X}", socket_handle);
        continue;
      }
      this->sockets[this->count++] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = socket->handle();
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle(), native_set);
    }
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      if (FD_ISSET(socket->native_handle(), native_set)) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
  }
};

i32 NetDll_select_entry(i32 caller, i32 nfds, ppc_ptr_t<x_fd_set> readfds,
                        ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                        mapped_void timeout_ptr) {
  host_set host_readfds = {};
  fd_set native_readfds = {};
  if (readfds) {
    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
  }
  host_set host_writefds = {};
  fd_set native_writefds = {};
  if (writefds) {
    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
  }
  host_set host_exceptfds = {};
  fd_set native_exceptfds = {};
  if (exceptfds) {
    host_exceptfds.Load(exceptfds);
    host_exceptfds.Store(&native_exceptfds);
  }
  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    timeout = {static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[0]),
               static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[1])};
    chrono::Clock::ScaleGuestDurationTimeval(reinterpret_cast<int32_t*>(&timeout.tv_sec),
                                             reinterpret_cast<int32_t*>(&timeout.tv_usec));
    timeout_in = &timeout;
  }
  int ret = select(nfds, readfds ? &native_readfds : nullptr, writefds ? &native_writefds : nullptr,
                   exceptfds ? &native_exceptfds : nullptr, timeout_in);
  if (ret == -1) {
    SetHostSocketLastError();
    REXKRNL_WARN("NetDll_select failed: nfds={} read={} write={} except={} error={}", nfds,
                 readfds ? static_cast<uint32_t>(readfds->fd_count) : 0,
                 writefds ? static_cast<uint32_t>(writefds->fd_count) : 0,
                 exceptfds ? static_cast<uint32_t>(exceptfds->fd_count) : 0,
                 GetHostSocketLastError());
  }
  if (readfds) {
    host_readfds.UpdateFrom(&native_readfds);
    host_readfds.Store(readfds);
  }
  bool completed_connect = false;
  if (writefds) {
    host_writefds.UpdateFrom(&native_writefds);

#if REX_PLATFORM_WIN32
    // A nonblocking connect is reported as writable when it has completed,
    // whether successfully or with an error. Validate SO_ERROR for each
    // writable socket. Successful connects must clear the stale
    // WSAEWOULDBLOCK left by NetDll_connect, otherwise Generations remains in
    // its "Transmitting" state even though FD_ISSET reports the socket ready.
    uint32_t successful_write_count = 0;
    for (uint32_t i = 0; i < host_writefds.count; ++i) {
      auto socket = host_writefds.sockets[i];
      int socket_error = 0;
      int socket_error_size = sizeof(socket_error);
      if (getsockopt(socket->native_handle(), SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&socket_error),
                     &socket_error_size) == 0 &&
          socket_error == 0) {
        host_writefds.sockets[successful_write_count++] = socket;
        completed_connect = true;
        REXKRNL_INFO(
            "NetDll_select completed nonblocking connect: handle={:08X}",
            socket->handle());
      } else {
        if (socket_error) {
          REXKRNL_WARN(
              "NetDll_select nonblocking connect failed: handle={:08X} "
              "error={}",
              socket->handle(), socket_error);
          if (exceptfds && host_exceptfds.count < 64) {
            host_exceptfds.sockets[host_exceptfds.count++] = socket;
          }
        }
      }
    }
    host_writefds.count = successful_write_count;
#endif

    host_writefds.Store(writefds);
  }
  if (exceptfds) {
    host_exceptfds.UpdateFrom(&native_exceptfds);
    host_exceptfds.Store(exceptfds);
  }

  if (completed_connect) {
    XThread::SetLastError(0);
  }

  // Return the number of descriptors actually copied back to the guest, not
  // the raw host select count before SO_ERROR validation.
  if (ret >= 0) {
    ret = static_cast<int>(host_readfds.count + host_writefds.count +
                           host_exceptfds.count);
  }

  if (ret > 0 && stream_select_trace_count.fetch_add(1, std::memory_order_relaxed) < 256) {
    const uint32_t read_handle = host_readfds.count ? host_readfds.sockets[0]->handle() : 0;
    const uint32_t write_handle = host_writefds.count ? host_writefds.sockets[0]->handle() : 0;
    const uint32_t except_handle = host_exceptfds.count ? host_exceptfds.sockets[0]->handle() : 0;
    REXKRNL_INFO("NetDll_select ready: result={} read={}/{} write={}/{} except={}/{}", ret,
                 host_readfds.count, read_handle, host_writefds.count, write_handle,
                 host_exceptfds.count, except_handle);
  }

  // TODO(gibbed): modify ret to be what's actually copied to the guest fd_sets?
  return ret;
}

u32 NetDll_recv_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);
    return -1;
  }

  const int ret = socket->Recv(buf_ptr, buf_len, flags);
  uint32_t error = 0;
  if (ret == -1) {
    SetHostSocketLastError();
    error = GetHostSocketLastError();
    const uint32_t trace_index =
        stream_recv_attempt_trace_count.fetch_add(1, std::memory_order_relaxed);
    // WSAEWOULDBLOCK is the expected result while a non-blocking stream has no
    // packet available. Generations polls these sockets every frame. Logging
    // every miss performs expensive endpoint formatting and file I/O on the
    // guest thread, causing machine-dependent loading stalls and enough timing
    // pressure to destabilize the peer. Keep the first few samples for
    // diagnostics and then only report sparse milestones. Real socket errors
    // are always logged.
    const bool would_block = IsHostSocketWouldBlock(error);
    const bool sparse_sample = trace_index < 4 || ((trace_index + 1) % 2048) == 0;
    if (!would_block || sparse_sample) {
      REXKRNL_INFO(
          "[NETTRACE] recv caller={} handle={:08X} requested={} flags={:08X} result={} error={} attempts={} {}",
          caller, socket_handle, buf_len, flags, ret, error, trace_index + 1,
          FormatNativeSocketEndpoints(socket));
    }
  } else if (ret == 0) {
    REXKRNL_INFO("[NETTRACE] recv closed caller={} handle={:08X} requested={} flags={:08X} {}",
                 caller, socket_handle, buf_len, flags,
                 FormatNativeSocketEndpoints(socket));
  } else {
    const uint32_t trace_index = stream_recv_trace_count.fetch_add(1, std::memory_order_relaxed);
    if (trace_index < 64) {
      REXKRNL_INFO("[NETTRACE] recv caller={} handle={:08X} requested={} flags={:08X} result={} data=[{}] {}",
                   caller, socket_handle, buf_len, flags, ret,
                   FormatPacketPrefix(static_cast<uint8_t*>(buf_ptr), ret),
                   FormatNativeSocketEndpoints(socket));
    }
  }
  return ret;
}

u32 NetDll_recvfrom_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len,
                          u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr, mapped_u32 fromlen_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  const int ret = socket->RecvFrom(buf_ptr, buf_len, flags, &native_from,
                                   fromlen_ptr ? &native_fromlen : 0);

  if (from_ptr) {
    from_ptr->sin_family = native_from.sin_family;
    from_ptr->sin_port = native_from.sin_port;
    from_ptr->sin_addr = native_from.sin_addr;
    std::memset(from_ptr->x_sin_zero, 0, sizeof(from_ptr->x_sin_zero));
  }
  if (fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  uint32_t error = 0;
  if (ret == -1) {
    SetHostSocketLastError();
    error = GetHostSocketLastError();
    const uint32_t trace_index =
        stream_recvfrom_trace_count.fetch_add(1, std::memory_order_relaxed);
    const bool would_block = IsHostSocketWouldBlock(error);
    const bool sparse_sample = trace_index < 4 || ((trace_index + 1) % 2048) == 0;
    if (!would_block || sparse_sample) {
      REXKRNL_INFO(
          "[NETTRACE] recvfrom caller={} handle={:08X} requested={} flags={:08X} result={} error={} attempts={}",
          caller, socket_handle, buf_len, flags, ret, error, trace_index + 1);
    }
  } else if (ret > 0) {
    const uint32_t trace_index = stream_recvfrom_trace_count.fetch_add(1, std::memory_order_relaxed);
    if (trace_index < 64) {
      REXKRNL_INFO("[NETTRACE] recvfrom caller={} handle={:08X} from={} requested={} flags={:08X} result={} data=[{}]",
                   caller, socket_handle, FormatSockaddrIn(native_from), buf_len,
                   flags, ret, FormatPacketPrefix(static_cast<uint8_t*>(buf_ptr), ret));
    }
  }
  return ret;
}

u32 NetDll_send_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);
    return -1;
  }

  const std::string packet_prefix =
      buf_ptr && buf_len ? FormatPacketPrefix(static_cast<uint8_t*>(buf_ptr), buf_len) : "";
  const int ret = socket->Send(buf_ptr, buf_len, flags);
  uint32_t error = 0;
  if (ret == -1) {
    SetHostSocketLastError();
    error = GetHostSocketLastError();
  }

  const uint32_t trace_index = stream_send_attempt_trace_count.fetch_add(1, std::memory_order_relaxed);
  if (trace_index < 64 || (ret == -1 && !IsHostSocketWouldBlock(error))) {
    REXKRNL_INFO("[NETTRACE] send caller={} handle={:08X} requested={} flags={:08X} result={} error={} data=[{}] {}",
                 caller, socket_handle, buf_len, flags, ret, error, packet_prefix,
                 FormatNativeSocketEndpoints(socket));
  }
  return ret;
}

u32 NetDll_sendto_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags,
                        ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  const std::string packet_prefix =
      buf_ptr && buf_len ? FormatPacketPrefix(static_cast<uint8_t*>(buf_ptr), buf_len) : "";
  const int ret = socket->SendTo(buf_ptr, buf_len, flags, &native_to, to_len);
  uint32_t error = 0;
  if (ret == -1) {
    SetHostSocketLastError();
    error = GetHostSocketLastError();
  }

  const uint32_t trace_index = stream_sendto_trace_count.fetch_add(1, std::memory_order_relaxed);
  if (trace_index < 64 || (ret == -1 && !IsHostSocketWouldBlock(error))) {
    REXKRNL_INFO("[NETTRACE] sendto caller={} handle={:08X} to={} to_len={} requested={} flags={:08X} result={} error={} data=[{}]",
                 caller, socket_handle, FormatSockaddrIn(native_to), to_len,
                 buf_len, flags, ret, error, packet_prefix);
  }
  return ret;
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  bool found = false;
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      found = true;
      break;
    }
  }
  if (stream_fd_isset_trace_count.fetch_add(1, std::memory_order_relaxed) < 64) {
    REXKRNL_INFO("NetDll___WSAFDIsSet: handle={:08X} count={} result={}", socket_handle,
                 max_fd_count, found ? 1 : 0);
  }
  return found ? 1 : 0;
}

void NetDll_WSASetLastError_entry(u32 error_code) {
  XThread::SetLastError(error_code);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__NetDll_XNetStartup, rex::kernel::xam::NetDll_XNetStartup_entry)
REX_EXPORT(__imp__NetDll_XNetStartupEx, rex::kernel::xam::NetDll_XNetStartupEx_entry)
REX_EXPORT(__imp__NetDll_XNetCleanup, rex::kernel::xam::NetDll_XNetCleanup_entry)
REX_EXPORT(__imp__NetDll_XNetGetOpt, rex::kernel::xam::NetDll_XNetGetOpt_entry)
REX_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
REX_EXPORT(__imp__NetDll_WSAStartup, rex::kernel::xam::NetDll_WSAStartup_entry)
REX_EXPORT(__imp__NetDll_WSAStartupEx, rex::kernel::xam::NetDll_WSAStartupEx_entry)
REX_EXPORT(__imp__NetDll_WSACleanup, rex::kernel::xam::NetDll_WSACleanup_entry)
REX_EXPORT(__imp__NetDll_WSAGetLastError, rex::kernel::xam::NetDll_WSAGetLastError_entry)
REX_EXPORT(__imp__NetDll_WSARecv, rex::kernel::xam::NetDll_WSARecv_entry)
REX_EXPORT(__imp__NetDll_WSARecvFrom, rex::kernel::xam::NetDll_WSARecvFrom_entry)
REX_EXPORT(__imp__NetDll_WSASend, rex::kernel::xam::NetDll_WSASend_entry)
REX_EXPORT(__imp__NetDll_WSASendTo, rex::kernel::xam::NetDll_WSASendTo_entry)
REX_EXPORT(__imp__NetDll_WSAWaitForMultipleEvents,
           rex::kernel::xam::NetDll_WSAWaitForMultipleEvents_entry)
REX_EXPORT(__imp__NetDll_WSACreateEvent, rex::kernel::xam::NetDll_WSACreateEvent_entry)
REX_EXPORT(__imp__NetDll_WSACloseEvent, rex::kernel::xam::NetDll_WSACloseEvent_entry)
REX_EXPORT(__imp__NetDll_WSAResetEvent, rex::kernel::xam::NetDll_WSAResetEvent_entry)
REX_EXPORT(__imp__NetDll_WSASetEvent, rex::kernel::xam::NetDll_WSASetEvent_entry)
REX_EXPORT(__imp__NetDll_WSAGetOverlappedResult,
           rex::kernel::xam::NetDll_WSAGetOverlappedResult_entry)
REX_EXPORT(__imp__NetDll_WSACancelOverlappedIO,
           rex::kernel::xam::NetDll_WSACancelOverlappedIO_entry)
REX_EXPORT(__imp__NetDll_WSAEventSelect, rex::kernel::xam::NetDll_WSAEventSelect_entry)
REX_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, rex::kernel::xam::NetDll_XNetGetTitleXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetGetDebugXnAddr, rex::kernel::xam::NetDll_XNetGetDebugXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToMachineId,
           rex::kernel::xam::NetDll_XNetXnAddrToMachineId_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToString, rex::kernel::xam::NetDll_XNetInAddrToString_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToInAddr, rex::kernel::xam::NetDll_XNetXnAddrToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToXnAddr, rex::kernel::xam::NetDll_XNetInAddrToXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetSetSystemLinkPort,
           rex::kernel::xam::NetDll_XNetSetSystemLinkPort_entry)
REX_EXPORT(__imp__NetDll_XNetGetSystemLinkPort,
           rex::kernel::xam::NetDll_XNetGetSystemLinkPort_entry)
REX_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus,
           rex::kernel::xam::NetDll_XNetGetEthernetLinkStatus_entry)
REX_EXPORT(__imp__NetDll_XNetDnsLookup, rex::kernel::xam::NetDll_XNetDnsLookup_entry)
REX_EXPORT(__imp__NetDll_XNetDnsRelease, rex::kernel::xam::NetDll_XNetDnsRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosServiceLookup, rex::kernel::xam::NetDll_XNetQosServiceLookup_entry)
REX_EXPORT(__imp__NetDll_XNetQosLookup, rex::kernel::xam::NetDll_XNetQosLookup_entry)
REX_EXPORT(__imp__NetDll_XNetQosRelease, rex::kernel::xam::NetDll_XNetQosRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosListen, rex::kernel::xam::NetDll_XNetQosListen_entry)
REX_EXPORT(__imp__NetDll_inet_addr, rex::kernel::xam::NetDll_inet_addr_entry)
REX_EXPORT(__imp__NetDll_socket, rex::kernel::xam::NetDll_socket_entry)
REX_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
REX_EXPORT(__imp__NetDll_shutdown, rex::kernel::xam::NetDll_shutdown_entry)
REX_EXPORT(__imp__NetDll_setsockopt, rex::kernel::xam::NetDll_setsockopt_entry)
REX_EXPORT(__imp__NetDll_ioctlsocket, rex::kernel::xam::NetDll_ioctlsocket_entry)
REX_EXPORT(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry)
REX_EXPORT(__imp__NetDll_connect, rex::kernel::xam::NetDll_connect_entry)
REX_EXPORT(__imp__NetDll_listen, rex::kernel::xam::NetDll_listen_entry)
REX_EXPORT(__imp__NetDll_accept, rex::kernel::xam::NetDll_accept_entry)
REX_EXPORT(__imp__NetDll_select, rex::kernel::xam::NetDll_select_entry)
REX_EXPORT(__imp__NetDll_recv, rex::kernel::xam::NetDll_recv_entry)
REX_EXPORT(__imp__NetDll_recvfrom, rex::kernel::xam::NetDll_recvfrom_entry)
REX_EXPORT(__imp__NetDll_send, rex::kernel::xam::NetDll_send_entry)
REX_EXPORT(__imp__NetDll_sendto, rex::kernel::xam::NetDll_sendto_entry)
REX_EXPORT(__imp__NetDll___WSAFDIsSet, rex::kernel::xam::NetDll___WSAFDIsSet_entry)
REX_EXPORT(__imp__NetDll_WSASetLastError, rex::kernel::xam::NetDll_WSASetLastError_entry)

REX_EXPORT_STUB(__imp__NetDll_UpnpActionCalculateWorkBufferSize);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpCleanup);
REX_EXPORT_STUB(__imp__NetDll_UpnpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpDoWork);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventGetCurrentState);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventUnsubscribe);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchGetDevices);
REX_EXPORT_STUB(__imp__NetDll_UpnpStartup);
REX_EXPORT_STUB(__imp__NetDll_XHttpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_XHttpConnect);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpDoWork);
REX_EXPORT_STUB(__imp__NetDll_XHttpGetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpen);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequestUsingMemory);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryAuthSchemes);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryHeaders);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpReadData);
REX_EXPORT_STUB(__imp__NetDll_XHttpReceiveResponse);
REX_EXPORT_STUB(__imp__NetDll_XHttpResetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpSendRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetCredentials);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetStatusCallback);
REX_EXPORT_STUB(__imp__NetDll_XHttpShutdown);
REX_EXPORT_STUB(__imp__NetDll_XHttpStartup);
REX_EXPORT_STUB(__imp__NetDll_XHttpWriteData);
REX_EXPORT(__imp__NetDll_XNetConnect, rex::kernel::xam::NetDll_XNetConnect_entry)
REX_EXPORT(__imp__NetDll_XNetCreateKey, rex::kernel::xam::NetDll_XNetCreateKey_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
REX_EXPORT_STUB(__imp__NetDll_XNetGetBroadcastVersionStatus);
REX_EXPORT(__imp__NetDll_XNetGetConnectStatus, rex::kernel::xam::NetDll_XNetGetConnectStatus_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetGetXnAddrPlatform);
REX_EXPORT_STUB(__imp__NetDll_XNetInAddrToServer);
REX_EXPORT_STUB(__imp__NetDll_XNetQosGetListenStats);
REX_EXPORT(__imp__NetDll_XNetRegisterKey, rex::kernel::xam::NetDll_XNetRegisterKey_entry)
REX_EXPORT(__imp__NetDll_XNetReplaceKey, rex::kernel::xam::NetDll_XNetReplaceKey_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetServerToInAddr);
REX_EXPORT(__imp__NetDll_XNetSetOpt, rex::kernel::xam::NetDll_XNetSetOpt_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetTsAddrToInAddr);
REX_EXPORT(__imp__NetDll_XNetUnregisterInAddr, rex::kernel::xam::NetDll_XNetUnregisterInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetUnregisterKey, rex::kernel::xam::NetDll_XNetUnregisterKey_entry)
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadContinue);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetParseTime);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetReceivedDataSize);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStart);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStop);
REX_EXPORT_STUB(__imp__NetDll_XnpCapture);
REX_EXPORT_STUB(__imp__NetDll_XnpConfig);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnP);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnPPortAndExternalAddr);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptRecv);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetExtendedReceiveCallback);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmit);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmitAsIp);
REX_EXPORT_STUB(__imp__NetDll_XnpGetActiveSocketList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetConfigStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpGetKeyList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetQosLookupList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetSecAssocList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetChallengeResponse);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetPState);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpNoteSystemTime);
REX_EXPORT_STUB(__imp__NetDll_XnpPersistTitleState);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetAggregateMeasurement);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetEntries);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryLoad);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistorySaveMeasurements);
REX_EXPORT_STUB(__imp__NetDll_XnpRegisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpReplaceKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpSetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpToolIpProxyInject);
REX_EXPORT_STUB(__imp__NetDll_XnpToolSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpUnregisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpUpdateConfigParams);
REX_EXPORT_STUB(__imp__NetDll_getpeername);
REX_EXPORT_STUB(__imp__NetDll_getsockname);
REX_EXPORT_STUB(__imp__NetDll_getsockopt);
