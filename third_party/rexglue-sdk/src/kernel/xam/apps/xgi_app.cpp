/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <charconv>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/kernel/xam/achievements.h>
#include <rex/kernel/xam/system_link.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/thread.h>
#include <rex/types.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xmemory.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

namespace {

// Forward declarations used by the LAN leaderboard helpers below.
std::string FormatUtcTime(std::chrono::system_clock::time_point value,
                          const char* format);
uint64_t MixSessionValue(uint64_t value);

#pragma pack(push, 1)
struct XNKID {
  uint8_t ab[8];
};
static_assert_size(XNKID, 0x8);

struct XNKEY {
  uint8_t ab[16];
};
static_assert_size(XNKEY, 0x10);

struct XNADDR {
  // IPv4 fields are already network-ordered byte sequences in an XNADDR.
  uint32_t ina;
  uint32_t ina_online;
  be<uint16_t> online_port;
  uint8_t ethernet[6];
  uint8_t online_id[20];
};
static_assert_size(XNADDR, 0x24);

struct XSESSION_INFO {
  XNKID session_id;
  XNADDR host_address;
  XNKEY key_exchange_key;
};
static_assert_size(XSESSION_INFO, 0x3C);

struct XSESSION_SEARCHRESULT {
  XSESSION_INFO info;
  be<uint32_t> open_public_slots;
  be<uint32_t> open_private_slots;
  be<uint32_t> filled_public_slots;
  be<uint32_t> filled_private_slots;
  be<uint32_t> properties_count;
  be<uint32_t> contexts_count;
  be<uint32_t> properties_ptr;
  be<uint32_t> contexts_ptr;
};
static_assert_size(XSESSION_SEARCHRESULT, 0x5C);

struct XSESSION_SEARCHRESULT_HEADER {
  be<uint32_t> search_results_count;
  be<uint32_t> search_results_ptr;
};
static_assert_size(XSESSION_SEARCHRESULT_HEADER, 0x8);

struct XUSER_CONTEXT {
  be<uint32_t> context_id;
  be<uint32_t> value;
};
static_assert_size(XUSER_CONTEXT, 0x8);

// XUSER_PROPERTY contains an alignas(8) X_USER_DATA. Keep the explicit
// padding here because this file's guest structures are packed.
struct XUSER_PROPERTY {
  be<uint32_t> property_id;
  uint8_t property_padding[4];
  uint8_t type;
  uint8_t type_padding[7];
  uint8_t value[8];
};
static_assert_size(XUSER_PROPERTY, 0x18);

struct XSESSION_MEMBER {
  be<uint64_t> online_xuid;
  be<uint32_t> user_index;
  be<uint32_t> flags;
};
static_assert_size(XSESSION_MEMBER, 0x10);

struct X_USER_ACHIEVEMENT {
  be<uint32_t> user_index;
  be<uint32_t> achievement_id;
};
static_assert_size(X_USER_ACHIEVEMENT, 0x8);

struct XSESSION_LOCAL_DETAILS {
  be<uint32_t> user_index_host;
  be<uint32_t> game_type;
  be<uint32_t> game_mode;
  be<uint32_t> flags;
  be<uint32_t> max_public_slots;
  be<uint32_t> max_private_slots;
  be<uint32_t> available_public_slots;
  be<uint32_t> available_private_slots;
  be<uint32_t> actual_member_count;
  be<uint32_t> returned_member_count;
  be<uint32_t> state;
  uint8_t state_padding[4];
  be<uint64_t> nonce;
  XSESSION_INFO session_info;
  XNKID arbitration_session_id;
  be<uint32_t> session_members_ptr;
};
static_assert_size(XSESSION_LOCAL_DETAILS, 0x80);
#pragma pack(pop)

constexpr uint32_t kSessionDetailsMinSize = sizeof(XSESSION_LOCAL_DETAILS);
constexpr uint32_t kSessionDetailsMemberOffset = sizeof(XSESSION_LOCAL_DETAILS);
constexpr uint32_t kSessionStateLobby = 0;
constexpr uint32_t kSessionStateInGame = 2;
constexpr uint32_t kSessionMemberPrivateSlot = 0x1;
constexpr uint32_t kSessionHostFlag = 0x1;
constexpr uint32_t kNoUserIndex = UINT32_MAX;

struct SystemLinkSession {
  uint32_t flags = 0;
  uint32_t public_slots = 1;
  uint32_t private_slots = 0;
  uint32_t user_index = 0;
  uint32_t local_public_members = 0;
  uint32_t local_private_members = 0;
  uint32_t remote_public_members = 0;
  uint32_t remote_private_members = 0;
  uint64_t owner_xuid = 0;
  uint64_t host_xuid = 0;
  uint64_t peer_xuid = 0;
  uint64_t nonce = 0;
  std::array<uint8_t, 8> session_id = {};
  std::array<uint8_t, 8> arbitration_session_id = {};
  std::array<uint8_t, 16> key_exchange_key = {};
  std::array<uint8_t, 6> host_mac = {};
  uint32_t host_ipv4 = 0;
  uint16_t game_port = 0;
  bool is_host = false;
  bool uses_private_slot = false;
  bool started = false;
  uint32_t lifecycle_phase = 0;
  uint64_t lifecycle_sequence = 0;
};

std::mutex system_link_sessions_mutex;
std::unordered_map<uint32_t, SystemLinkSession> system_link_sessions;
std::unordered_map<std::string, SystemLinkSessionAdvertisement> discovered_system_link_sessions;

const char* SessionLifecyclePhaseName(uint32_t phase) {
  switch (phase) {
    case 0: return "new";
    case 1: return "created";
    case 2: return "members-ready";
    case 3: return "in-game";
    case 4: return "ended";
    case 5: return "deleting";
    case 6: return "migrated";
    default: return "unknown";
  }
}


uint32_t ActualMemberCount(const SystemLinkSession& session);

void NormalizeSessionInvariants(uint32_t session_ptr, SystemLinkSession* session,
                                const char* operation) {
  if (!session) return;

  const auto before = *session;
  session->public_slots = std::max(session->public_slots,
                                   session->local_public_members +
                                       session->remote_public_members);
  session->private_slots = std::max(session->private_slots,
                                    session->local_private_members +
                                        session->remote_private_members);

  if (session->is_host) {
    session->flags |= kSessionHostFlag;
    if (!session->host_xuid) session->host_xuid = session->owner_xuid;
    if (!session->peer_xuid &&
        (session->remote_public_members || session->remote_private_members)) {
      REXKRNL_WARN(
          "[XEXONLINE][INVARIANT] {} host session {:08X} has remote members "
          "without peer XUID",
          operation, session_ptr);
    }
  } else {
    session->flags &= ~kSessionHostFlag;
    if (!session->peer_xuid) session->peer_xuid = session->host_xuid;
  }

  if (session->started) {
    session->lifecycle_phase = 3;
  } else if (ActualMemberCount(*session) >= 2 &&
             session->lifecycle_phase < 2) {
    session->lifecycle_phase = 2;
  }

  const bool changed =
      before.flags != session->flags ||
      before.public_slots != session->public_slots ||
      before.private_slots != session->private_slots ||
      before.host_xuid != session->host_xuid ||
      before.peer_xuid != session->peer_xuid ||
      before.lifecycle_phase != session->lifecycle_phase;
  if (changed) {
    REXKRNL_WARN(
        "[XEXONLINE][INVARIANT] repaired {} session={:08X} flags={:08X}->{:08X} "
        "slots={}/{}->{}/{} host={:016X}->{:016X} peer={:016X}->{:016X} "
        "phase={}->{}",
        operation, session_ptr, before.flags, session->flags, before.public_slots,
        before.private_slots, session->public_slots, session->private_slots,
        before.host_xuid, session->host_xuid, before.peer_xuid, session->peer_xuid,
        before.lifecycle_phase, session->lifecycle_phase);
  }
}
void LogSessionLifecycle(uint32_t session_ptr, const SystemLinkSession& session,
                         const char* operation) {
  REXKRNL_INFO(
      "[XEXONLINE][LIFECYCLE] {} session={:08X} phase={}({}) seq={} host={} "
      "flags={:08X} started={} members={} local={}/{} remote={}/{} "
      "owner={:016X} host_xuid={:016X} peer={:016X} endpoint={:08X}:{}",
      operation, session_ptr, session.lifecycle_phase,
      SessionLifecyclePhaseName(session.lifecycle_phase), session.lifecycle_sequence,
      session.is_host, session.flags, session.started, session.local_public_members + session.local_private_members + session.remote_public_members + session.remote_private_members,
      session.local_public_members, session.local_private_members,
      session.remote_public_members, session.remote_private_members,
      session.owner_xuid, session.host_xuid, session.peer_xuid,
      session.host_ipv4, session.game_port);
}

// XGI user contexts are title-side state, not fire-and-forget notifications.
// Generations writes its online/menu contexts during boot and later reads them
// back while deciding whether to advance into create/search/lobby states.
std::mutex xgi_user_state_mutex;
std::unordered_map<uint64_t, uint32_t> xgi_user_contexts;
std::unordered_map<uint64_t, SystemLinkProperty> xgi_user_properties;

struct MatchStatProperty {
  uint32_t id = 0;
  uint8_t type = 0xFF;
  std::string type_name;
  std::string value;
};

struct MatchStatView {
  uint32_t id = 0;
  std::vector<MatchStatProperty> properties;
};

struct MatchStatsWrite {
  uint64_t xuid = 0;
  std::vector<MatchStatView> views;
};

struct LocalMatchRecord {
  std::chrono::system_clock::time_point started_at;
  std::vector<MatchStatsWrite> writes;
};

std::mutex local_match_records_mutex;
std::unordered_map<uint32_t, LocalMatchRecord> local_match_records;

// LAN-shared leaderboard/profile database. It is transported only through the
// existing System Link discovery socket and requires no external service.
constexpr uint32_t kLanProfileMarkerProperty = 0x7F470001;
constexpr uint32_t kLanProfileVersionProperty = 0x7F470002;
constexpr uint32_t kLanProfileGamesProperty = 0x7F470003;
constexpr uint32_t kLanProfileWinsProperty = 0x7F470004;
constexpr uint32_t kLanProfileLossesProperty = 0x7F470005;
constexpr uint32_t kLanProfileDrawsProperty = 0x7F470006;
constexpr uint32_t kLanProfileDisconnectsProperty = 0x7F470007;
constexpr uint32_t kLanProfileBattlePointsProperty = 0x7F470008;
constexpr uint32_t kLanProfileName0Property = 0x7F470010;
constexpr uint32_t kLanProfileName1Property = 0x7F470011;
constexpr uint32_t kLanProfileName2Property = 0x7F470012;
constexpr uint32_t kLanProfileName3Property = 0x7F470013;
constexpr uint32_t kLanProfileName4Property = 0x7F470014;
constexpr uint32_t kLanProfileName5Property = 0x7F470015;
constexpr uint32_t kLanProfileName6Property = 0x7F470016;
constexpr uint32_t kLanProfileName7Property = 0x7F470017;
constexpr uint32_t kLanProfileMarkerValue = 0x4C425031;
constexpr uint32_t kLanProfileOwnerMask = 0x80000000u;

struct LanPlayerRecord {
  uint64_t xuid = 0;
  std::string gamertag;
  uint64_t version = 1;
  uint64_t games = 0;
  uint64_t wins = 0;
  uint64_t losses = 0;
  uint64_t draws = 0;
  uint64_t disconnects = 0;
  int64_t battle_points = 0;
  std::string last_seen_utc;
  std::unordered_map<uint64_t, std::string> raw_stats;
  std::unordered_map<uint64_t, uint8_t> raw_stat_types;
};

std::mutex lan_player_database_mutex;
std::unordered_map<uint64_t, LanPlayerRecord> lan_player_database;
bool lan_player_database_loaded = false;

std::filesystem::path LanLeaderboardPath(system::KernelState* kernel_state) {
  auto root = kernel_state->emulator()->user_data_root();
  if (!root.empty()) {
    root = root.parent_path();
  }
  return root / "network" / "generations_lan_leaderboard.toml";
}

uint32_t LanProfileOwner(uint32_t session_ptr) {
  return session_ptr ^ kLanProfileOwnerMask;
}

uint64_t ReadLanUnsigned(const SystemLinkProperty& property) {
  uint64_t value = 0;
  const size_t count = std::min<size_t>(property.value_size, sizeof(value));
  for (size_t i = 0; i < count; ++i) {
    value |= static_cast<uint64_t>(property.value[i]) << (i * 8);
  }
  return value;
}

void WriteLanUnsigned(SystemLinkProperty* property, uint32_t id, uint64_t value) {
  property->id = id;
  property->type = 2;
  property->value_size = 8;
  property->value.fill(0);
  for (size_t i = 0; i < 8; ++i) {
    property->value[i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

const SystemLinkProperty* FindLanProperty(
    const SystemLinkSessionAdvertisement& advertisement, uint32_t id) {
  const auto found = std::find_if(
      advertisement.properties.begin(), advertisement.properties.end(),
      [id](const SystemLinkProperty& property) { return property.id == id; });
  return found == advertisement.properties.end() ? nullptr : &*found;
}

bool IsLanProfileAdvertisement(
    const SystemLinkSessionAdvertisement& advertisement) {
  const auto* marker = FindLanProperty(advertisement, kLanProfileMarkerProperty);
  return marker && ReadLanUnsigned(*marker) == kLanProfileMarkerValue;
}

std::string DecodeLanName(
    const SystemLinkSessionAdvertisement& advertisement) {
  constexpr std::array<uint32_t, 8> ids = {
      kLanProfileName0Property, kLanProfileName1Property,
      kLanProfileName2Property, kLanProfileName3Property,
      kLanProfileName4Property, kLanProfileName5Property,
      kLanProfileName6Property, kLanProfileName7Property};
  std::array<char, 33> name = {};
  for (size_t chunk = 0; chunk < ids.size(); ++chunk) {
    const auto* property = FindLanProperty(advertisement, ids[chunk]);
    if (property) {
      std::memcpy(name.data() + chunk * 4, property->value.data(),
                  std::min<size_t>(4, property->value_size));
    }
  }
  return std::string(name.data());
}

uint64_t ParseLanHex(std::string_view text) {
  uint64_t value = 0;
  std::from_chars(text.data(), text.data() + text.size(), value, 16);
  return value;
}

void SaveLanDatabase(system::KernelState* kernel_state) {
  std::unordered_map<uint64_t, LanPlayerRecord> snapshot;
  {
    std::scoped_lock lock(lan_player_database_mutex);
    snapshot = lan_player_database;
  }

  const auto path = LanLeaderboardPath(kernel_state);
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    REXKRNL_ERROR("[LANBOARD] Directory creation failed '{}': {}",
                  path.parent_path().string(), error.message());
    return;
  }

  std::vector<LanPlayerRecord> ordered;
  ordered.reserve(snapshot.size());
  for (const auto& [xuid, player] : snapshot) {
    ordered.push_back(player);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const LanPlayerRecord& left, const LanPlayerRecord& right) {
              if (left.battle_points != right.battle_points) {
                return left.battle_points > right.battle_points;
              }
              if (left.wins != right.wins) {
                return left.wins > right.wins;
              }
              return left.gamertag < right.gamertag;
            });

  toml::table document;
  document.insert("version", 1);
  document.insert("title_id", fmt::format("{:08X}", kernel_state->title_id()));
  document.insert("updated_at_utc",
                  FormatUtcTime(std::chrono::system_clock::now(),
                                "%Y-%m-%dT%H:%M:%SZ"));
  toml::array rows;
  uint64_t position = 1;
  for (const auto& player : ordered) {
    toml::table row;
    row.insert("position", static_cast<int64_t>(position++));
    row.insert("xuid", fmt::format("{:016X}", player.xuid));
    row.insert("gamertag", player.gamertag);
    row.insert("record_version", static_cast<int64_t>(player.version));
    row.insert("games", static_cast<int64_t>(player.games));
    row.insert("wins", static_cast<int64_t>(player.wins));
    row.insert("losses", static_cast<int64_t>(player.losses));
    row.insert("draws", static_cast<int64_t>(player.draws));
    row.insert("disconnects", static_cast<int64_t>(player.disconnects));
    row.insert("battle_points", player.battle_points);
    row.insert("last_seen_utc", player.last_seen_utc);
    toml::table raw;
    for (const auto& [key, value] : player.raw_stats) {
      raw.insert(fmt::format("{:016X}", key), value);
    }
    row.insert("raw_stats", std::move(raw));
    toml::table raw_types;
    for (const auto& [key, value] : player.raw_stat_types) {
      raw_types.insert(fmt::format("{:016X}", key),
                       static_cast<int64_t>(value));
    }
    row.insert("raw_stat_types", std::move(raw_types));
    rows.push_back(std::move(row));
  }
  document.insert("players", std::move(rows));

  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
      return;
    }
    output << document << '\n';
  }
  std::filesystem::rename(temporary, path, error);
  if (error) {
    error.clear();
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
  }
  if (error) {
    REXKRNL_ERROR("[LANBOARD] Save failed '{}': {}", path.string(),
                  error.message());
  } else {
    REXKRNL_INFO("[LANBOARD] Saved {} player record{}", snapshot.size(),
                 snapshot.size() == 1 ? "" : "s");
  }
}

void LoadLanDatabase(system::KernelState* kernel_state) {
  {
    std::scoped_lock lock(lan_player_database_mutex);
    if (lan_player_database_loaded) {
      return;
    }
    lan_player_database_loaded = true;
  }
  const auto path = LanLeaderboardPath(kernel_state);
  if (!std::filesystem::exists(path)) {
    return;
  }
  try {
    const auto document = toml::parse_file(path.string());
    const auto* players = document["players"].as_array();
    if (!players) {
      return;
    }
    std::scoped_lock lock(lan_player_database_mutex);
    for (const auto& node : *players) {
      const auto* row = node.as_table();
      if (!row) {
        continue;
      }
      LanPlayerRecord player;
      player.xuid = ParseLanHex(row->get("xuid")->value_or(std::string()));
      if (!player.xuid) {
        continue;
      }
      player.gamertag = row->get("gamertag")->value_or(std::string("Unknown"));
      player.version = static_cast<uint64_t>(
          row->get("record_version")->value_or<int64_t>(1));
      player.games =
          static_cast<uint64_t>(row->get("games")->value_or<int64_t>(0));
      player.wins =
          static_cast<uint64_t>(row->get("wins")->value_or<int64_t>(0));
      player.losses =
          static_cast<uint64_t>(row->get("losses")->value_or<int64_t>(0));
      player.draws =
          static_cast<uint64_t>(row->get("draws")->value_or<int64_t>(0));
      player.disconnects = static_cast<uint64_t>(
          row->get("disconnects")->value_or<int64_t>(0));
      player.battle_points =
          row->get("battle_points")->value_or<int64_t>(0);
      player.last_seen_utc =
          row->get("last_seen_utc")->value_or(std::string());
      if (const auto* raw_node = row->get("raw_stats");
          raw_node && raw_node->is_table()) {
        for (const auto& [key, value] : *raw_node->as_table()) {
          if (const auto raw_value = value.value<std::string>()) {
            player.raw_stats[ParseLanHex(std::string(key.str()))] = *raw_value;
          }
        }
      }
      if (const auto* type_node = row->get("raw_stat_types");
          type_node && type_node->is_table()) {
        for (const auto& [key, value] : *type_node->as_table()) {
          if (const auto raw_type = value.value<int64_t>()) {
            player.raw_stat_types[ParseLanHex(std::string(key.str()))] =
                static_cast<uint8_t>(*raw_type);
          }
        }
      }
      lan_player_database[player.xuid] = std::move(player);
    }
    REXKRNL_INFO("[LANBOARD] Loaded {} persistent player record{}",
                 lan_player_database.size(),
                 lan_player_database.size() == 1 ? "" : "s");
  } catch (const std::exception& exception) {
    REXKRNL_ERROR("[LANBOARD] Parse failed '{}': {}", path.string(),
                  exception.what());
  }
}

LanPlayerRecord EnsureLanPlayer(system::KernelState* kernel_state, uint64_t xuid,
                                std::string_view name) {
  LoadLanDatabase(kernel_state);
  bool changed = false;
  LanPlayerRecord result;
  {
    std::scoped_lock lock(lan_player_database_mutex);
    auto [it, inserted] = lan_player_database.try_emplace(xuid);
    auto& player = it->second;
    if (inserted) {
      player.xuid = xuid;
      player.version = 1;
      changed = true;
      REXKRNL_INFO("[LANBOARD] Added {:016X} with zero statistics", xuid);
    }
    if (!name.empty() && player.gamertag != name) {
      player.gamertag = std::string(name);
      if (!inserted) {
        ++player.version;
      }
      changed = true;
    }
    player.last_seen_utc =
        FormatUtcTime(std::chrono::system_clock::now(), "%Y-%m-%dT%H:%M:%SZ");
    result = player;
  }
  if (changed) {
    SaveLanDatabase(kernel_state);
  }
  return result;
}

void MergeLanProfile(system::KernelState* kernel_state,
                     const SystemLinkSessionAdvertisement& advertisement) {
  if (!IsLanProfileAdvertisement(advertisement) || !advertisement.host_xuid) {
    return;
  }
  const auto read = [&](uint32_t id) -> uint64_t {
    const auto* property = FindLanProperty(advertisement, id);
    return property ? ReadLanUnsigned(*property) : 0;
  };
  LanPlayerRecord incoming;
  incoming.xuid = advertisement.host_xuid;
  incoming.gamertag = DecodeLanName(advertisement);
  incoming.version = std::max<uint64_t>(1, read(kLanProfileVersionProperty));
  incoming.games = read(kLanProfileGamesProperty);
  incoming.wins = read(kLanProfileWinsProperty);
  incoming.losses = read(kLanProfileLossesProperty);
  incoming.draws = read(kLanProfileDrawsProperty);
  incoming.disconnects = read(kLanProfileDisconnectsProperty);
  incoming.battle_points =
      static_cast<int64_t>(read(kLanProfileBattlePointsProperty));
  incoming.last_seen_utc =
      FormatUtcTime(std::chrono::system_clock::now(), "%Y-%m-%dT%H:%M:%SZ");

  LoadLanDatabase(kernel_state);
  bool changed = false;
  {
    std::scoped_lock lock(lan_player_database_mutex);
    auto found = lan_player_database.find(incoming.xuid);
    if (found == lan_player_database.end() ||
        incoming.version > found->second.version) {
      lan_player_database[incoming.xuid] = incoming;
      changed = true;
      REXKRNL_INFO(
          "[LANBOARD] Merged '{}' {:016X} version={} games={} wins={} "
          "losses={} bp={}",
          incoming.gamertag, incoming.xuid, incoming.version, incoming.games,
          incoming.wins, incoming.losses, incoming.battle_points);
    }
  }
  if (changed) {
    SaveLanDatabase(kernel_state);
  }
}

SystemLinkSessionAdvertisement MakeLanProfileAdvertisement(
    system::KernelState* kernel_state, const SystemLinkSession& session) {
  const auto player =
      EnsureLanPlayer(kernel_state, kernel_state->user_profile()->xuid(),
                      kernel_state->user_profile()->name());
  SystemLinkSessionAdvertisement advertisement;
  advertisement.title_id = kernel_state->title_id();
  advertisement.host_xuid = player.xuid;
  advertisement.host_ipv4 = session.host_ipv4;
  advertisement.game_port = session.game_port ? session.game_port : 1;
  advertisement.mac = session.host_mac;
  advertisement.flags = 0x80000000u;

  uint64_t seed = MixSessionValue(player.xuid ^ 0x4C414E424F415244ull);
  for (size_t i = 0; i < advertisement.session_id.size(); ++i) {
    seed = MixSessionValue(seed + i);
    advertisement.session_id[i] = static_cast<uint8_t>(seed >> 56);
  }
  advertisement.session_id[0] = 0x7F;
  for (size_t i = 0; i < advertisement.key_exchange_key.size(); ++i) {
    seed = MixSessionValue(seed + i + 0x40);
    advertisement.key_exchange_key[i] = static_cast<uint8_t>(seed >> 56);
  }

  const auto append = [&](uint32_t id, uint64_t value) {
    advertisement.properties.emplace_back();
    WriteLanUnsigned(&advertisement.properties.back(), id, value);
  };
  append(kLanProfileMarkerProperty, kLanProfileMarkerValue);
  append(kLanProfileVersionProperty, player.version);
  append(kLanProfileGamesProperty, player.games);
  append(kLanProfileWinsProperty, player.wins);
  append(kLanProfileLossesProperty, player.losses);
  append(kLanProfileDrawsProperty, player.draws);
  append(kLanProfileDisconnectsProperty, player.disconnects);
  append(kLanProfileBattlePointsProperty,
         static_cast<uint64_t>(player.battle_points));

  std::array<uint8_t, 32> name = {};
  std::memcpy(name.data(), player.gamertag.data(),
              std::min(name.size() - 1, player.gamertag.size()));
  constexpr std::array<uint32_t, 8> name_ids = {
      kLanProfileName0Property, kLanProfileName1Property,
      kLanProfileName2Property, kLanProfileName3Property,
      kLanProfileName4Property, kLanProfileName5Property,
      kLanProfileName6Property, kLanProfileName7Property};
  for (size_t chunk = 0; chunk < name_ids.size(); ++chunk) {
    advertisement.properties.emplace_back();
    auto& property = advertisement.properties.back();
    property.id = name_ids[chunk];
    property.type = 1;
    property.value_size = 4;
    property.value.fill(0);
    std::memcpy(property.value.data(), name.data() + chunk * 4, 4);
  }
  return advertisement;
}

void PublishLanProfile(system::KernelState* kernel_state, uint32_t session_ptr,
                       const SystemLinkSession& session) {
  SystemLinkPublishSession(LanProfileOwner(session_ptr),
                           MakeLanProfileAdvertisement(kernel_state, session));
}

void RemoveLanProfile(uint32_t session_ptr) {
  SystemLinkRemoveSession(LanProfileOwner(session_ptr));
}

void ApplyMatchToLanDatabase(system::KernelState* kernel_state,
                             const LocalMatchRecord& record,
                             const SystemLinkSession& session) {
  LoadLanDatabase(kernel_state);
  std::unordered_map<uint64_t, std::vector<MatchStatView>> writes_by_player;
  for (const auto& write : record.writes) {
    if (write.xuid) {
      auto& destination = writes_by_player[write.xuid];
      destination.insert(destination.end(), write.views.begin(),
                         write.views.end());
    }
  }
  writes_by_player.try_emplace(kernel_state->user_profile()->xuid());
  if (session.host_xuid) {
    writes_by_player.try_emplace(session.host_xuid);
  }
  if (session.peer_xuid) {
    writes_by_player.try_emplace(session.peer_xuid);
  }

  {
    std::scoped_lock lock(lan_player_database_mutex);
    for (const auto& [xuid, views] : writes_by_player) {
      auto& player = lan_player_database[xuid];
      if (!player.xuid) {
        player.xuid = xuid;
        player.gamertag =
            xuid == kernel_state->user_profile()->xuid()
                ? kernel_state->user_profile()->name()
                : fmt::format("Player_{:08X}", static_cast<uint32_t>(xuid));
        player.version = 1;
      }
      const bool title_wrote_games = std::any_of(
          views.begin(), views.end(), [](const MatchStatView& view) {
            return view.id == 3 &&
                   std::any_of(view.properties.begin(), view.properties.end(),
                               [](const MatchStatProperty& property) {
                                 return property.id == 4;
                               });
          });
      if (!title_wrote_games) {
        ++player.games;
      }
      ++player.version;
      player.last_seen_utc =
          FormatUtcTime(std::chrono::system_clock::now(), "%Y-%m-%dT%H:%M:%SZ");
      for (const auto& view : views) {
        for (const auto& property : view.properties) {
          const uint64_t key =
              (static_cast<uint64_t>(view.id) << 32) | property.id;
          player.raw_stats[key] = property.value;
          player.raw_stat_types[key] = property.type;
          if (view.id == 3) {
            int64_t numeric_value = 0;
            const auto [number_end, number_error] = std::from_chars(
                property.value.data(),
                property.value.data() + property.value.size(), numeric_value);
            if (number_error == std::errc() &&
                number_end == property.value.data() + property.value.size()) {
              switch (property.id) {
                case 1: player.battle_points = numeric_value; break;
                case 2: player.wins = std::max<int64_t>(0, numeric_value); break;
                case 3: player.losses = std::max<int64_t>(0, numeric_value); break;
                case 4: player.games = std::max<int64_t>(0, numeric_value); break;
                case 5: player.draws = std::max<int64_t>(0, numeric_value); break;
                case 6:
                  player.disconnects = std::max<int64_t>(0, numeric_value);
                  break;
                default: break;
              }
            }
          }
        }
      }
      REXKRNL_INFO(
          "[LANBOARD] Match updated '{}' {:016X}: version={} games={} "
          "raw_stats={}",
          player.gamertag, player.xuid, player.version, player.games,
          player.raw_stats.size());
    }
  }
  SaveLanDatabase(kernel_state);
}

bool MatchRecordsEnabled() {
  const auto* flag = cvar::GetFlagInfo("generations_replay_autosave");
  return !flag || cvar::Query<bool>("generations_replay_autosave");
}

std::string HexBytes(const uint8_t* data, size_t size) {
  std::string value;
  value.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    fmt::format_to(std::back_inserter(value), "{:02X}", data[i]);
  }
  return value;
}

std::string FormatUtcTime(std::chrono::system_clock::time_point value,
                          const char* format) {
  const std::time_t time = std::chrono::system_clock::to_time_t(value);
  std::tm utc = {};
#ifdef _WIN32
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif
  std::ostringstream stream;
  stream << std::put_time(&utc, format);
  return stream.str();
}

MatchStatProperty DecodeMatchProperty(memory::Memory* memory,
                                      const uint8_t* property) {
  MatchStatProperty decoded;
  decoded.id = memory::load_and_swap<uint32_t>(property + 0);
  decoded.type = property[8];
  const uint8_t* payload = property + 16;
  switch (decoded.type) {
    case 0:
      decoded.type_name = "context";
      decoded.value = fmt::format("{}", memory::load_and_swap<uint32_t>(payload));
      break;
    case 1:
      decoded.type_name = "int32";
      decoded.value = fmt::format("{}", memory::load_and_swap<int32_t>(payload));
      break;
    case 2:
      decoded.type_name = "int64";
      decoded.value = fmt::format("{}", memory::load_and_swap<int64_t>(payload));
      break;
    case 3: {
      decoded.type_name = "double";
      const uint64_t bits = memory::load_and_swap<uint64_t>(payload);
      decoded.value = fmt::format("{:.17g}", std::bit_cast<double>(bits));
      break;
    }
    case 4: {
      decoded.type_name = "wstring_utf16be";
      const uint32_t byte_count = memory::load_and_swap<uint32_t>(payload + 0);
      const uint32_t value_ptr = memory::load_and_swap<uint32_t>(payload + 4);
      const size_t bounded_size = std::min<size_t>(byte_count, 512);
      decoded.value = value_ptr && bounded_size
                          ? HexBytes(memory->TranslateVirtual(value_ptr), bounded_size)
                          : std::string();
      break;
    }
    case 5: {
      decoded.type_name = "float";
      const uint32_t bits = memory::load_and_swap<uint32_t>(payload);
      decoded.value = fmt::format("{:.9g}", std::bit_cast<float>(bits));
      break;
    }
    case 6: {
      decoded.type_name = "binary";
      const uint32_t byte_count = memory::load_and_swap<uint32_t>(payload + 0);
      const uint32_t value_ptr = memory::load_and_swap<uint32_t>(payload + 4);
      const size_t bounded_size = std::min<size_t>(byte_count, 256);
      decoded.value = value_ptr && bounded_size
                          ? HexBytes(memory->TranslateVirtual(value_ptr), bounded_size)
                          : std::string();
      break;
    }
    case 7:
      decoded.type_name = "datetime_filetime";
      decoded.value = fmt::format("{}", memory::load_and_swap<uint64_t>(payload));
      break;
    default:
      decoded.type_name = "unknown";
      decoded.value = HexBytes(property + 8, 16);
      break;
  }
  return decoded;
}

std::vector<MatchStatView> DecodeMatchViews(memory::Memory* memory,
                                            uint32_t num_views,
                                            uint32_t views_ptr) {
  std::vector<MatchStatView> decoded;
  if (!views_ptr || !num_views) {
    return decoded;
  }
  const uint32_t bounded_views = std::min(num_views, 64u);
  const uint8_t* views = memory->TranslateVirtual(views_ptr);
  decoded.reserve(bounded_views);
  for (uint32_t i = 0; i < bounded_views; ++i) {
    const uint8_t* view = views + i * 12;
    MatchStatView decoded_view;
    decoded_view.id = memory::load_and_swap<uint32_t>(view + 0);
    const uint32_t property_count =
        std::min(memory::load_and_swap<uint32_t>(view + 4), 128u);
    const uint32_t properties_ptr = memory::load_and_swap<uint32_t>(view + 8);
    if (properties_ptr && property_count) {
      const uint8_t* properties = memory->TranslateVirtual(properties_ptr);
      decoded_view.properties.reserve(property_count);
      for (uint32_t property_index = 0; property_index < property_count;
           ++property_index) {
        decoded_view.properties.push_back(
            DecodeMatchProperty(memory, properties + property_index * 24));
      }
    }
    decoded.push_back(std::move(decoded_view));
  }
  return decoded;
}

void BeginLocalMatchRecord(uint32_t session_ptr) {
  if (!MatchRecordsEnabled()) {
    return;
  }
  std::scoped_lock lock(local_match_records_mutex);
  local_match_records[session_ptr] =
      LocalMatchRecord{std::chrono::system_clock::now(), {}};
}

void AppendLocalMatchStats(uint32_t session_ptr, uint64_t xuid,
                           std::vector<MatchStatView> views) {
  if (!MatchRecordsEnabled()) {
    return;
  }
  std::scoped_lock lock(local_match_records_mutex);
  auto it = local_match_records
                .try_emplace(session_ptr,
                             LocalMatchRecord{
                                 std::chrono::system_clock::now(), {}})
                .first;
  it->second.writes.push_back(MatchStatsWrite{xuid, std::move(views)});
}

void CommitLocalMatchRecord(system::KernelState* kernel_state,
                            uint32_t session_ptr,
                            const SystemLinkSession& session) {
  if (!MatchRecordsEnabled()) {
    return;
  }

  LocalMatchRecord record;
  {
    std::scoped_lock lock(local_match_records_mutex);
    const auto found = local_match_records.find(session_ptr);
    if (found == local_match_records.end()) {
      return;
    }
    record = std::move(found->second);
    local_match_records.erase(found);
  }

  const auto ended_at = std::chrono::system_clock::now();
  ApplyMatchToLanDatabase(kernel_state, record, session);
  PublishLanProfile(kernel_state, session_ptr, session);

  auto root = kernel_state->emulator()->user_data_root();
  if (!root.empty()) {
    root = root.parent_path();
  }
  const std::filesystem::path directory = root / "replays" / "local";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    REXKRNL_ERROR("Could not create local match-record directory '{}': {}",
                  directory.string(), error.message());
    return;
  }

  toml::table document;
  document.insert("version", 1);
  document.insert("kind", "xsession_stat_record");
  document.insert("playable_replay", false);
  document.insert("title_id", fmt::format("{:08X}", kernel_state->title_id()));
  document.insert("session_object", fmt::format("{:08X}", session_ptr));
  document.insert("session_id", HexBytes(session.session_id.data(),
                                          session.session_id.size()));
  document.insert("owner_xuid", fmt::format("{:016X}", session.owner_xuid));
  document.insert("host_xuid", fmt::format("{:016X}", session.host_xuid));
  document.insert("local_xuid",
                  fmt::format("{:016X}", kernel_state->user_profile()->xuid()));
  document.insert("local_name", kernel_state->user_profile()->name());
  document.insert("is_host", session.is_host);
  document.insert("public_slots", static_cast<int64_t>(session.public_slots));
  document.insert("private_slots", static_cast<int64_t>(session.private_slots));
  document.insert(
      "member_count",
      static_cast<int64_t>(session.local_public_members +
                           session.local_private_members +
                           session.remote_public_members +
                           session.remote_private_members));
  document.insert("started_at_utc",
                  FormatUtcTime(record.started_at, "%Y-%m-%dT%H:%M:%SZ"));
  document.insert("ended_at_utc",
                  FormatUtcTime(ended_at, "%Y-%m-%dT%H:%M:%SZ"));
  document.insert(
      "duration_ms",
      std::chrono::duration_cast<std::chrono::milliseconds>(
          ended_at - record.started_at)
          .count());

  toml::array writes;
  for (const auto& write : record.writes) {
    toml::table write_table;
    write_table.insert("xuid", fmt::format("{:016X}", write.xuid));
    toml::array views;
    for (const auto& view : write.views) {
      toml::table view_table;
      view_table.insert("id", fmt::format("{:08X}", view.id));
      toml::array properties;
      for (const auto& property : view.properties) {
        toml::table property_table;
        property_table.insert("id", fmt::format("{:08X}", property.id));
        property_table.insert("type", property.type_name);
        property_table.insert("value", property.value);
        properties.push_back(std::move(property_table));
      }
      view_table.insert("properties", std::move(properties));
      views.push_back(std::move(view_table));
    }
    write_table.insert("views", std::move(views));
    writes.push_back(std::move(write_table));
  }
  document.insert("stat_writes", std::move(writes));

  const std::string stem = fmt::format(
      "match_{}_{:08X}", FormatUtcTime(ended_at, "%Y%m%d_%H%M%S"),
      session_ptr);
  const std::filesystem::path path = directory / (stem + ".toml");
  const std::filesystem::path temporary = directory / (stem + ".tmp");
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
      return;
    }
    output << "# Generations local match metadata; not a playable replay\n"
           << document << '\n';
    if (!output) {
      std::filesystem::remove(temporary, error);
      return;
    }
  }
  std::filesystem::rename(temporary, path, error);
  if (error) {
    error.clear();
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
  }
  if (error) {
    REXKRNL_ERROR("Could not commit local match record '{}': {}",
                  path.string(), error.message());
    return;
  }
  REXKRNL_INFO("Saved local match record '{}' (stat writes={})",
               path.string(), record.writes.size());
}

uint64_t MakeUserStateKey(uint32_t user_index, uint32_t id) {
  return (static_cast<uint64_t>(user_index) << 32) | id;
}

std::optional<SystemLinkSession> FindSystemLinkSession(uint32_t session_ptr) {
  std::scoped_lock lock(system_link_sessions_mutex);
  const auto it = system_link_sessions.find(session_ptr);
  return it != system_link_sessions.end() ? std::optional<SystemLinkSession>(it->second)
                                           : std::nullopt;
}

std::string SessionIdKey(const std::array<uint8_t, 8>& session_id) {
  return std::string(reinterpret_cast<const char*>(session_id.data()), session_id.size());
}

uint64_t MixSessionValue(uint64_t value) {
  value += 0x9E3779B97F4A7C15ull;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

uint64_t MakeSessionNonce(uint32_t session_ptr, uint64_t xuid) {
  const uint64_t ticks = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  return MixSessionValue(ticks ^ (static_cast<uint64_t>(session_ptr) << 32) ^ xuid);
}

std::array<uint8_t, 8> MakeSystemLinkSessionId(uint64_t seed) {
  std::array<uint8_t, 8> session_id = {};
  seed = MixSessionValue(seed);
  // System-link XNKIDs are distinguished from Xbox Live sessions by their
  // leading zero word.  The remaining six bytes form the session identity.
  for (size_t i = 2; i < session_id.size(); ++i) {
    seed = MixSessionValue(seed + i);
    session_id[i] = static_cast<uint8_t>(seed >> 56);
  }
  session_id[0] = 0;
  session_id[1] = 0;
  return session_id;
}

std::array<uint8_t, 16> MakeSystemLinkKey(uint64_t seed) {
  // The old implementation returned the same 00..0F key for every lobby.
  // Generations carries this key from discovery through secure-address setup,
  // so reusing it made stale and unrelated sessions indistinguishable.
  std::array<uint8_t, 16> key = {};
  uint64_t left = MixSessionValue(seed ^ 0x584E4B45594C4546ull);   // "XNKEYLEF"
  uint64_t right = MixSessionValue(seed ^ 0x584E4B4559524947ull);  // "XNKEYRIG"
  for (size_t i = 0; i < 8; ++i) {
    key[i] = static_cast<uint8_t>(left >> ((7 - i) * 8));
    key[8 + i] = static_cast<uint8_t>(right >> ((7 - i) * 8));
  }
  if (std::all_of(key.begin(), key.end(), [](uint8_t value) { return value == 0; })) {
    key[15] = 1;
  }
  return key;
}

uint32_t FilledPublicSlots(const SystemLinkSession& session) {
  return std::min(session.public_slots,
                  session.remote_public_members + session.local_public_members);
}

uint32_t FilledPrivateSlots(const SystemLinkSession& session) {
  return std::min(session.private_slots,
                  session.remote_private_members + session.local_private_members);
}

uint32_t AvailablePublicSlots(const SystemLinkSession& session) {
  return session.public_slots - FilledPublicSlots(session);
}

uint32_t AvailablePrivateSlots(const SystemLinkSession& session) {
  return session.private_slots - FilledPrivateSlots(session);
}

uint32_t ActualMemberCount(const SystemLinkSession& session) {
  return FilledPublicSlots(session) + FilledPrivateSlots(session);
}

void AddSessionMembers(SystemLinkSession* session, uint32_t count, bool remote,
                       const uint8_t* private_slots_array, memory::Memory* memory) {
  for (uint32_t i = 0; i < count; ++i) {
    const bool wants_private =
        private_slots_array &&
        memory::load_and_swap<uint32_t>(private_slots_array + (i * sizeof(uint32_t))) != 0;
    if (wants_private && AvailablePrivateSlots(*session) != 0) {
      if (remote) {
        ++session->remote_private_members;
      } else {
        ++session->local_private_members;
        session->uses_private_slot = true;
      }
    } else if (AvailablePublicSlots(*session) != 0) {
      if (remote) {
        ++session->remote_public_members;
      } else {
        ++session->local_public_members;
        session->uses_private_slot = false;
      }
    }
  }
}

void RemoveSessionMembers(SystemLinkSession* session, uint32_t count, bool remote,
                          const uint8_t* private_slots_array, memory::Memory* memory) {
  for (uint32_t i = 0; i < count; ++i) {
    const bool was_private =
        private_slots_array &&
        memory::load_and_swap<uint32_t>(private_slots_array + (i * sizeof(uint32_t))) != 0;
    auto& private_members =
        remote ? session->remote_private_members : session->local_private_members;
    auto& public_members =
        remote ? session->remote_public_members : session->local_public_members;

    if (was_private && private_members) {
      --private_members;
    } else if (public_members) {
      --public_members;
    } else if (private_members) {
      --private_members;
    }
  }
}

uint32_t RemoveMemberCount(uint32_t current, uint32_t requested) {
  return requested >= current ? 0 : current - requested;
}

XSESSION_INFO MakeSessionInfo(const SystemLinkSession& session) {
  XSESSION_INFO info = {};
  std::memcpy(info.session_id.ab, session.session_id.data(), session.session_id.size());
  info.host_address.ina = session.host_ipv4;
  // System-link titles are inconsistent about which XNADDR IPv4 field they
  // consume. Keep both fields coherent so the search result can be handed
  // directly to XNetXnAddrToInAddr / XSessionCreate on the joining machine.
  info.host_address.ina_online = session.host_ipv4;
  info.host_address.online_port = session.game_port;
  std::memcpy(info.host_address.ethernet, session.host_mac.data(), session.host_mac.size());
  // Give the address a stable non-zero identity. Some titles reject an
  // otherwise valid LAN result when the online-id bytes are all zero.
  std::memcpy(info.host_address.online_id, session.session_id.data(),
              session.session_id.size());
  std::memcpy(info.host_address.online_id + 8, &session.host_xuid,
              std::min(sizeof(session.host_xuid), sizeof(info.host_address.online_id) - 8));
  std::memcpy(info.key_exchange_key.ab, session.key_exchange_key.data(),
              session.key_exchange_key.size());
  return info;
}

void ApplySessionInfo(const XSESSION_INFO& info, SystemLinkSession* session) {
  std::memcpy(session->session_id.data(), info.session_id.ab, session->session_id.size());
  std::memcpy(session->key_exchange_key.data(), info.key_exchange_key.ab,
              session->key_exchange_key.size());
  std::memcpy(session->host_mac.data(), info.host_address.ethernet, session->host_mac.size());
  session->host_ipv4 = info.host_address.ina != 0 ? info.host_address.ina
                                                    : info.host_address.ina_online;
  session->game_port = info.host_address.online_port;
}

SystemLinkSessionAdvertisement MakeAdvertisement(uint32_t title_id,
                                                 const SystemLinkSession& session) {
  SystemLinkSessionAdvertisement advertisement;
  advertisement.title_id = title_id;
  advertisement.session_id = session.session_id;
  advertisement.key_exchange_key = session.key_exchange_key;
  advertisement.mac = session.host_mac;
  advertisement.host_ipv4 = session.host_ipv4;
  advertisement.game_port = session.game_port;
  advertisement.flags = session.flags;
  advertisement.public_slots = session.public_slots;
  advertisement.private_slots = session.private_slots;
  advertisement.open_public_slots = session.started ? 0 : AvailablePublicSlots(session);
  advertisement.open_private_slots = session.started ? 0 : AvailablePrivateSlots(session);
  advertisement.filled_public_slots = FilledPublicSlots(session);
  advertisement.filled_private_slots = FilledPrivateSlots(session);
  advertisement.host_xuid = session.owner_xuid;
  {
    std::scoped_lock lock(xgi_user_state_mutex);
    for (const auto& [key, value] : xgi_user_contexts) {
      if (static_cast<uint32_t>(key >> 32) == session.user_index) {
        advertisement.contexts.push_back(
            {static_cast<uint32_t>(key), value});
      }
    }
    for (const auto& [key, property] : xgi_user_properties) {
      if (static_cast<uint32_t>(key >> 32) == session.user_index) {
        advertisement.properties.push_back(property);
      }
    }
  }

  // Generations serializes the lobby/card display name across property
  // 0x13, 0x31-0x35, and the final 0x37/0x38 chunks. The retail title initializes these chunks to
  // "NoName". Replace all six chunks with the active profile name in explicit
  // guest byte order so the search-result UI can reconstruct the full name.
  {
    std::array<uint8_t, 32> name_bytes = {};
    const std::string& profile_name = REX_KERNEL_STATE()->user_profile()->name();
    std::memcpy(name_bytes.data(), profile_name.data(),
                std::min(profile_name.size(), name_bytes.size() - 1));

    constexpr std::array<uint32_t, 8> kNamePropertyIds = {
        0x10000013, 0x10000031, 0x10000032, 0x10000033,
        0x10000034, 0x10000035, 0x10000037, 0x10000038};
    for (size_t chunk = 0; chunk < kNamePropertyIds.size(); ++chunk) {
      const uint32_t property_id = kNamePropertyIds[chunk];
      auto found = std::find_if(
          advertisement.properties.begin(), advertisement.properties.end(),
          [property_id](const SystemLinkProperty& property) {
            return property.id == property_id;
          });
      if (found == advertisement.properties.end()) {
        advertisement.properties.emplace_back();
        found = std::prev(advertisement.properties.end());
      }
      found->id = property_id;
      found->type = 1;
      found->value_size = 4;
      found->value.fill(0);
      for (size_t byte = 0; byte < 4; ++byte) {
        found->value[byte] = name_bytes[chunk * 4 + byte];
      }
    }
    REXKRNL_INFO("System-link advertised profile name '{}'", profile_name);
  }

  // Keep the lobby owner identity coherent with the visible card name. The
  // title reads property 0x2000000F while converting a search result into its
  // internal lobby entry, so stale title-initialized data here produces a
  // duplicate local player or a nameless result.
  {
    constexpr uint32_t kOwnerXuidProperty = 0x2000000F;
    auto found = std::find_if(
        advertisement.properties.begin(), advertisement.properties.end(),
        [](const SystemLinkProperty& property) {
          return property.id == kOwnerXuidProperty;
        });
    if (found == advertisement.properties.end()) {
      advertisement.properties.emplace_back();
      found = std::prev(advertisement.properties.end());
    }
    found->id = kOwnerXuidProperty;
    found->type = 2;
    found->value_size = 8;
    found->value.fill(0);
    const uint64_t owner_xuid = session.host_xuid ? session.host_xuid : session.owner_xuid;
    for (size_t byte = 0; byte < 8; ++byte) {
      found->value[byte] = static_cast<uint8_t>(owner_xuid >> ((7 - byte) * 8));
    }
    advertisement.host_xuid = owner_xuid;
  }

  // Private diagnostic metadata used to verify that search, create and join all
  // refer to the same lifecycle generation. Unknown properties are ignored by
  // the retail title but remain visible in discovery traces.
  {
    SystemLinkProperty phase_property;
    phase_property.id = 0x7F470020;
    phase_property.type = 1;
    phase_property.value_size = 4;
    phase_property.value.fill(0);
    phase_property.value[0] = static_cast<uint8_t>(session.lifecycle_phase >> 24);
    phase_property.value[1] = static_cast<uint8_t>(session.lifecycle_phase >> 16);
    phase_property.value[2] = static_cast<uint8_t>(session.lifecycle_phase >> 8);
    phase_property.value[3] = static_cast<uint8_t>(session.lifecycle_phase);
    advertisement.properties.push_back(phase_property);
  }

  std::sort(advertisement.contexts.begin(), advertisement.contexts.end(),
            [](const auto& left, const auto& right) { return left.id < right.id; });
  std::sort(advertisement.properties.begin(), advertisement.properties.end(),
            [](const auto& left, const auto& right) { return left.id < right.id; });
  return advertisement;
}

void PublishSystemLinkSession(uint32_t session_ptr, uint32_t title_id) {
  std::optional<SystemLinkSession> session;
  {
    std::scoped_lock lock(system_link_sessions_mutex);
    const auto it = system_link_sessions.find(session_ptr);
    if (it == system_link_sessions.end()) return;
    NormalizeSessionInvariants(session_ptr, &it->second, "publish");
    session = it->second;
  }
  if (session->is_host) {
    SystemLinkPublishSession(session_ptr, MakeAdvertisement(title_id, *session));
  }
  PublishLanProfile(REX_KERNEL_STATE(), session_ptr, *session);
}

void RememberSearchResults(const std::vector<SystemLinkSessionAdvertisement>& sessions) {
  // A search response is a complete snapshot, not an append-only cache. Keeping
  // advertisements that vanished from the latest discovery pass allows the UI
  // to select a stale session and then hands XSessionCreate the wrong host.
  {
    std::scoped_lock lock(system_link_sessions_mutex);
    discovered_system_link_sessions.clear();
    for (const auto& session : sessions) {
      discovered_system_link_sessions[SessionIdKey(session.session_id)] = session;
    }
  }
  for (const auto& session : sessions) {
    if (session.host_xuid) {
      EnsureLanPlayer(REX_KERNEL_STATE(), session.host_xuid,
                      DecodeLanName(session));
    }
  }
  REXKRNL_INFO("[XEXONLINE][SEARCH] replaced discovery snapshot with {} sessions",
               sessions.size());
}


std::string DecodeGenerationsLobbyName(
    const SystemLinkSessionAdvertisement& advertisement) {
  constexpr std::array<uint32_t, 8> ids = {
      0x10000013, 0x10000031, 0x10000032, 0x10000033,
      0x10000034, 0x10000035, 0x10000037, 0x10000038};
  std::array<char, 33> name = {};
  for (size_t chunk = 0; chunk < ids.size(); ++chunk) {
    const auto found = std::find_if(
        advertisement.properties.begin(), advertisement.properties.end(),
        [id = ids[chunk]](const SystemLinkProperty& property) {
          return property.id == id;
        });
    if (found != advertisement.properties.end()) {
      std::memcpy(name.data() + chunk * 4, found->value.data(),
                  std::min<size_t>(4, found->value_size));
    }
  }
  return std::string(name.data());
}

void PrioritizeGenerationsSearchMetadata(
    std::vector<SystemLinkSessionAdvertisement>* sessions, memory::Memory* memory,
    uint16_t num_props, uint32_t props_ptr, uint16_t num_ctx, uint32_t ctx_ptr) {
  if (!sessions) {
    return;
  }

  std::vector<uint32_t> requested_property_ids;
  if (props_ptr && num_props) {
    const auto* requested =
        memory->TranslateVirtual<const XUSER_PROPERTY*>(props_ptr);
    requested_property_ids.reserve(num_props);
    for (uint16_t i = 0; i < num_props; ++i) {
      requested_property_ids.push_back(requested[i].property_id);
    }
  }

  // Generations reconstructs the visible lobby owner from these chunks and
  // also expects the owner's XUID near the front of the returned property list.
  constexpr std::array<uint32_t, 9> identity_ids = {
      0x2000000F, 0x10000013, 0x10000031, 0x10000032, 0x10000033,
      0x10000034, 0x10000035, 0x10000037, 0x10000038};

  std::vector<uint32_t> requested_context_ids;
  if (ctx_ptr && num_ctx) {
    const auto* requested =
        memory->TranslateVirtual<const XUSER_CONTEXT*>(ctx_ptr);
    requested_context_ids.reserve(num_ctx);
    for (uint16_t i = 0; i < num_ctx; ++i) {
      requested_context_ids.push_back(requested[i].context_id);
    }
  }

  for (auto& session : *sessions) {
    std::stable_sort(session.properties.begin(), session.properties.end(),
                     [&](const SystemLinkProperty& left,
                         const SystemLinkProperty& right) {
      auto rank = [&](uint32_t id) {
        const auto requested = std::find(requested_property_ids.begin(),
                                         requested_property_ids.end(), id);
        if (requested != requested_property_ids.end()) {
          return static_cast<size_t>(requested - requested_property_ids.begin());
        }
        const auto identity = std::find(identity_ids.begin(), identity_ids.end(), id);
        if (identity != identity_ids.end()) {
          return requested_property_ids.size() +
                 static_cast<size_t>(identity - identity_ids.begin());
        }
        return requested_property_ids.size() + identity_ids.size() +
               static_cast<size_t>(id);
      };
      return rank(left.id) < rank(right.id);
    });

    std::stable_sort(session.contexts.begin(), session.contexts.end(),
                     [&](const auto& left, const auto& right) {
      auto rank = [&](uint32_t id) {
        const auto requested = std::find(requested_context_ids.begin(),
                                         requested_context_ids.end(), id);
        if (requested != requested_context_ids.end()) {
          return static_cast<size_t>(requested - requested_context_ids.begin());
        }
        return requested_context_ids.size() + static_cast<size_t>(id);
      };
      return rank(left.id) < rank(right.id);
    });

    REXKRNL_INFO(
        "[XEXONLINE][SEARCH] prepared lobby card name='{}' host_xuid={:016X} "
        "properties={} contexts={}",
        DecodeGenerationsLobbyName(session), session.host_xuid,
        session.properties.size(), session.contexts.size());
  }
}

std::optional<SystemLinkSessionAdvertisement> FindDiscoveredSession(
    const std::array<uint8_t, 8>& session_id) {
  std::scoped_lock lock(system_link_sessions_mutex);
  const auto it = discovered_system_link_sessions.find(SessionIdKey(session_id));
  return it != discovered_system_link_sessions.end()
             ? std::optional<SystemLinkSessionAdvertisement>(it->second)
             : std::nullopt;
}

void FillSearchResult(memory::Memory* memory, XSESSION_SEARCHRESULT* result,
                      const SystemLinkSessionAdvertisement& session) {
  std::memset(result, 0, sizeof(*result));
  std::memcpy(result->info.session_id.ab, session.session_id.data(), session.session_id.size());
  result->info.host_address.ina = session.host_ipv4;
  result->info.host_address.ina_online = session.host_ipv4;
  result->info.host_address.online_port = session.game_port;
  std::memcpy(result->info.host_address.ethernet, session.mac.data(), session.mac.size());
  std::memcpy(result->info.host_address.online_id, session.session_id.data(),
              session.session_id.size());
  std::memcpy(result->info.host_address.online_id + 8, &session.host_xuid,
              std::min(sizeof(session.host_xuid),
                       sizeof(result->info.host_address.online_id) - 8));
  std::memcpy(result->info.key_exchange_key.ab, session.key_exchange_key.data(),
              session.key_exchange_key.size());
  result->open_public_slots = session.open_public_slots;
  result->open_private_slots = session.open_private_slots;
  result->filled_public_slots = session.filled_public_slots;
  result->filled_private_slots = session.filled_private_slots;

  if (!session.contexts.empty()) {
    const uint32_t context_count = static_cast<uint32_t>(session.contexts.size());
    const uint32_t contexts_ptr = memory->SystemHeapAlloc(context_count * sizeof(XUSER_CONTEXT));
    auto* contexts = memory->TranslateVirtual<XUSER_CONTEXT*>(contexts_ptr);
    std::memset(contexts, 0, context_count * sizeof(XUSER_CONTEXT));
    for (uint32_t i = 0; i < context_count; ++i) {
      contexts[i].context_id = session.contexts[i].id;
      contexts[i].value = session.contexts[i].value;
    }
    result->contexts_count = context_count;
    result->contexts_ptr = contexts_ptr;
  }

  if (!session.properties.empty()) {
    const uint32_t property_count = static_cast<uint32_t>(session.properties.size());
    const uint32_t properties_ptr =
        memory->SystemHeapAlloc(property_count * sizeof(XUSER_PROPERTY));
    auto* properties = memory->TranslateVirtual<XUSER_PROPERTY*>(properties_ptr);
    std::memset(properties, 0, property_count * sizeof(XUSER_PROPERTY));
    for (uint32_t i = 0; i < property_count; ++i) {
      const auto& source = session.properties[i];
      properties[i].property_id = source.id;
      properties[i].type = source.type;
      std::memcpy(properties[i].value, source.value.data(),
                  std::min<size_t>(source.value_size, sizeof(properties[i].value)));
    }
    result->properties_count = property_count;
    result->properties_ptr = properties_ptr;
  }
}

void WriteSearchResults(memory::Memory* memory, uint32_t results_buffer_size,
                        uint32_t search_results_ptr,
                        const std::vector<SystemLinkSessionAdvertisement>& sessions) {
  if (!search_results_ptr || results_buffer_size < sizeof(XSESSION_SEARCHRESULT_HEADER)) {
    REXKRNL_WARN("System-link search rejected a missing or undersized results buffer");
    return;
  }

  auto results_buffer = memory->TranslateVirtual(search_results_ptr);
  std::memset(results_buffer, 0, results_buffer_size);

  auto* header = reinterpret_cast<XSESSION_SEARCHRESULT_HEADER*>(results_buffer);
  const uint32_t result_capacity =
      (results_buffer_size - sizeof(XSESSION_SEARCHRESULT_HEADER)) / sizeof(XSESSION_SEARCHRESULT);
  const uint32_t result_count =
      std::min<uint32_t>(result_capacity, static_cast<uint32_t>(sessions.size()));
  const uint32_t guest_results_ptr = search_results_ptr + sizeof(XSESSION_SEARCHRESULT_HEADER);

  header->search_results_count = result_count;
  header->search_results_ptr = guest_results_ptr;

  auto* results = reinterpret_cast<XSESSION_SEARCHRESULT*>(
      results_buffer + sizeof(XSESSION_SEARCHRESULT_HEADER));
  for (uint32_t i = 0; i < result_count; ++i) {
    FillSearchResult(memory, &results[i], sessions[i]);
    REXKRNL_INFO(
        "System-link search result {}: name='{}' xuid={:016X} host={:08X}:{} "
        "public_open={} private_open={} public_filled={} private_filled={} "
        "properties={} contexts={}",
        i, DecodeGenerationsLobbyName(sessions[i]), sessions[i].host_xuid,
        sessions[i].host_ipv4, sessions[i].game_port, sessions[i].open_public_slots,
        sessions[i].open_private_slots, sessions[i].filled_public_slots,
        sessions[i].filled_private_slots, sessions[i].properties.size(),
        sessions[i].contexts.size());
  }
  REXKRNL_INFO("System-link search completed: discovered={} returned={} capacity={} bytes={}",
               sessions.size(), result_count, result_capacity, results_buffer_size);
}

void WriteSearchResultsFromSizePointer(memory::Memory* memory, uint32_t results_buffer_size_ptr,
                                       uint32_t search_results_ptr,
                                       const std::vector<SystemLinkSessionAdvertisement>& sessions) {
  const uint32_t required =
      sizeof(XSESSION_SEARCHRESULT_HEADER) +
      static_cast<uint32_t>(sessions.size() * sizeof(XSESSION_SEARCHRESULT));
  uint32_t results_buffer_size =
      results_buffer_size_ptr >= 0x10000
          ? memory::load_and_swap<uint32_t>(memory->TranslateVirtual(results_buffer_size_ptr))
          : results_buffer_size_ptr;
  if (!search_results_ptr) {
    if (results_buffer_size_ptr >= 0x10000) {
      memory::store_and_swap<uint32_t>(memory->TranslateVirtual(results_buffer_size_ptr), required);
    }
    REXKRNL_INFO("System-link search size query: discovered={} required={} current={}",
                 sessions.size(), required, results_buffer_size);
    return;
  }
  if (results_buffer_size < required && results_buffer_size_ptr >= 0x10000) {
    memory::store_and_swap<uint32_t>(memory->TranslateVirtual(results_buffer_size_ptr), required);
  }
  WriteSearchResults(memory, results_buffer_size, search_results_ptr, sessions);
}

std::vector<SystemLinkSessionAdvertisement> DiscoverSystemLinkSessions(
    uint32_t title_id, uint32_t result_limit) {
  auto sessions = SystemLinkDiscoverSessions(
      title_id, std::max<uint32_t>(result_limit + 32, 32));
  for (const auto& session : sessions) {
    if (IsLanProfileAdvertisement(session)) {
      MergeLanProfile(REX_KERNEL_STATE(), session);
    }
  }
  sessions.erase(
      std::remove_if(sessions.begin(), sessions.end(),
                     [](const SystemLinkSessionAdvertisement& session) {
                       return IsLanProfileAdvertisement(session);
                     }),
      sessions.end());

  const uint64_t local_xuid = REX_KERNEL_STATE()->user_profile()->xuid();
  std::unordered_set<std::string> seen_session_ids;
  sessions.erase(
      std::remove_if(sessions.begin(), sessions.end(), [&](const auto& session) {
        const std::string key = SessionIdKey(session.session_id);
        const bool invalid_identity =
            !session.host_xuid || !session.host_ipv4 || !session.game_port;
        const bool duplicate = !seen_session_ids.insert(key).second;
        const bool own_advertisement = session.host_xuid == local_xuid;
        if (invalid_identity || duplicate || own_advertisement) {
          REXKRNL_INFO(
              "[XEXONLINE][SEARCH] dropped result xuid={:016X} host={:08X}:{} invalid={} duplicate={} self={}",
              session.host_xuid, session.host_ipv4, session.game_port,
              invalid_identity, duplicate, own_advertisement);
        }
        return invalid_identity || duplicate || own_advertisement;
      }),
      sessions.end());

  // A full lobby must not be presented as joinable. Doing this before the
  // title-side filters also prevents a zero-slot card from being selected and
  // later failing during Transmitting.
  sessions.erase(
      std::remove_if(sessions.begin(), sessions.end(), [](const auto& session) {
        return session.open_public_slots + session.open_private_slots == 0;
      }),
      sessions.end());

  std::stable_sort(sessions.begin(), sessions.end(), [](const auto& left,
                                                        const auto& right) {
    const uint32_t left_open = left.open_public_slots + left.open_private_slots;
    const uint32_t right_open = right.open_public_slots + right.open_private_slots;
    if (left_open != right_open) {
      return left_open > right_open;
    }
    return left.host_xuid < right.host_xuid;
  });

  if (sessions.size() > result_limit) {
    sessions.resize(result_limit);
  }
  RememberSearchResults(sessions);
  return sessions;
}

bool IsGenerationsPlaceholderSearchProperty(const XUSER_PROPERTY& property) {
  const uint32_t id = property.property_id;
  constexpr std::array<uint32_t, 9> identity_ids = {
      0x2000000F, 0x10000013, 0x10000031, 0x10000032, 0x10000033,
      0x10000034, 0x10000035, 0x10000037, 0x10000038};
  if (std::find(identity_ids.begin(), identity_ids.end(), id) ==
      identity_ids.end()) {
    return false;
  }

  bool all_zero = true;
  for (uint8_t byte : property.value) {
    all_zero &= byte == 0;
  }
  if (all_zero) {
    return true;
  }

  // Retail initializes the output name chunks to "NoName" before invoking
  // search. These fields describe the requested result layout; they are not a
  // request to find a host literally named NoName.
  const char* bytes = reinterpret_cast<const char*>(property.value);
  return std::memcmp(bytes, "NoNa", 4) == 0 ||
         std::memcmp(bytes, "me", 2) == 0;
}

bool SessionMatchesSearchFilters(const SystemLinkSessionAdvertisement& session,
                                 memory::Memory* memory, uint16_t num_props,
                                 uint32_t props_ptr, uint16_t num_ctx,
                                 uint32_t ctx_ptr, uint32_t required_users) {
  if (required_users && session.open_public_slots + session.open_private_slots < required_users) {
    return false;
  }

  if (ctx_ptr && num_ctx) {
    const auto* requested = memory->TranslateVirtual<const XUSER_CONTEXT*>(ctx_ptr);
    for (uint16_t i = 0; i < num_ctx; ++i) {
      const uint32_t id = requested[i].context_id;
      const uint32_t value = requested[i].value;
      const auto found = std::find_if(session.contexts.begin(), session.contexts.end(),
                                      [&](const auto& item) {
                                        return item.id == id && item.value == value;
                                      });
      if (found == session.contexts.end()) {
        return false;
      }
    }
  }

  if (props_ptr && num_props) {
    const auto* requested = memory->TranslateVirtual<const XUSER_PROPERTY*>(props_ptr);
    for (uint16_t i = 0; i < num_props; ++i) {
      if (IsGenerationsPlaceholderSearchProperty(requested[i])) {
        REXKRNL_DEBUG(
            "[XEXONLINE][SEARCH] treating placeholder property {:08X} as output metadata",
            static_cast<uint32_t>(requested[i].property_id));
        continue;
      }
      const uint32_t id = requested[i].property_id;
      const uint8_t type = requested[i].type;
      const auto found = std::find_if(session.properties.begin(), session.properties.end(),
                                      [&](const auto& item) {
                                        if (item.id != id || item.type != type) {
                                          return false;
                                        }
                                        const size_t compare_size =
                                            std::min<size_t>(item.value_size, sizeof(requested[i].value));
                                        return compare_size == 0 ||
                                               std::memcmp(item.value.data(), requested[i].value,
                                                           compare_size) == 0;
                                      });
      if (found == session.properties.end()) {
        return false;
      }
    }
  }
  return true;
}

std::vector<SystemLinkSessionAdvertisement> FilterSessionsBySearch(
    std::vector<SystemLinkSessionAdvertisement> sessions, memory::Memory* memory,
    uint16_t num_props, uint32_t props_ptr, uint16_t num_ctx, uint32_t ctx_ptr,
    uint32_t required_users) {
  sessions.erase(
      std::remove_if(sessions.begin(), sessions.end(), [&](const auto& session) {
        return !SessionMatchesSearchFilters(session, memory, num_props, props_ptr,
                                            num_ctx, ctx_ptr, required_users);
      }),
      sessions.end());
  return sessions;
}

std::vector<SystemLinkSessionAdvertisement> FilterSessionsById(
    std::vector<SystemLinkSessionAdvertisement> sessions, const uint8_t* session_ids,
    uint32_t session_id_count) {
  if (!session_ids || !session_id_count) {
    return sessions;
  }

  sessions.erase(std::remove_if(sessions.begin(), sessions.end(), [&](const auto& session) {
                   for (uint32_t i = 0; i < session_id_count; ++i) {
                     if (std::memcmp(session.session_id.data(), session_ids + (i * 8), 8) == 0) {
                       return false;
                     }
                   }
                   return true;
                 }),
                 sessions.end());
  return sessions;
}

}  // namespace

void SystemLinkNotifyGamePort(uint16_t game_port) {
  if (!game_port) {
    return;
  }

  std::vector<uint32_t> hosts_to_publish;
  {
    std::scoped_lock lock(system_link_sessions_mutex);
    for (auto& [session_ptr, session] : system_link_sessions) {
      if (!session.is_host) {
        continue;
      }
      if (session.game_port != game_port) {
        REXKRNL_INFO(
            "System-link XGI game-port sync: session={:08X} {} -> {}",
            session_ptr, session.game_port, game_port);
        session.game_port = game_port;
      }
      hosts_to_publish.push_back(session_ptr);
    }
  }
  for (uint32_t session_ptr : hosts_to_publish) {
    PublishSystemLinkSession(session_ptr, REX_KERNEL_STATE()->title_id());
  }
}

void SystemLinkNotifyAcceptedPeer(uint16_t listener_port) {
  std::vector<uint32_t> hosts_to_publish;
  {
    std::scoped_lock lock(system_link_sessions_mutex);
    for (auto& [session_ptr, session] : system_link_sessions) {
      if (!session.is_host) {
        continue;
      }
      if (listener_port && session.game_port && session.game_port != listener_port) {
        continue;
      }
      if (listener_port && !session.game_port) {
        session.game_port = listener_port;
      }
      hosts_to_publish.push_back(session_ptr);
      REXKRNL_INFO(
          "System-link host accepted transport peer; membership awaits "
          "XSessionJoinRemote (session={:08X}, port={})",
          session_ptr, session.game_port);
    }
  }
  for (uint32_t session_ptr : hosts_to_publish) {
    PublishSystemLinkSession(session_ptr, REX_KERNEL_STATE()->title_id());
  }
}

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  const uint32_t trace_size = std::min(buffer_length, 64u);
  REXKRNL_INFO(
      "[XEXONLINE][XGI] dispatch message={:08X} buffer={:08X} length={} data={}",
      message, buffer_ptr, buffer_length,
      buffer_ptr && trace_size ? HexBytes(buffer, trace_size) : std::string());
  switch (message) {
    case 0x000B0006: {
      assert_true(!buffer_length || buffer_length == 24);
      // dword r3 user index
      // dword (unwritten?)
      // qword 0
      // dword r4 context enum
      // dword r5 value
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t context_value = memory::load_and_swap<uint32_t>(buffer + 20);
      {
        std::scoped_lock lock(xgi_user_state_mutex);
        xgi_user_contexts[MakeUserStateKey(user_index, context_id)] = context_value;
      }
      REXKRNL_INFO("[XEXONLINE] XGIUserSetContextEx user={} context={:08X} value={:08X}",
                   user_index, context_id, context_value);
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t property_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t value_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t value_ptr = memory::load_and_swap<uint32_t>(buffer + 24);
      SystemLinkProperty property;
      property.id = property_id;
      property.type = static_cast<uint8_t>(property_id >> 28);
      property.value_size = static_cast<uint8_t>(std::min(value_size, 8u));
      if (value_ptr && property.value_size) {
        std::memcpy(property.value.data(), memory_->TranslateVirtual(value_ptr),
                    property.value_size);
      }
      {
        std::scoped_lock lock(xgi_user_state_mutex);
        xgi_user_properties[MakeUserStateKey(user_index, property_id)] = property;
      }
      REXKRNL_INFO(
          "[XEXONLINE] XGIUserSetPropertyEx user={} property={:08X} type={} size={} "
          "ptr={:08X} value={}",
          user_index, property_id, property.type, value_size, value_ptr,
          HexBytes(property.value.data(), property.value_size));
      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t achievement_count = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t achievements_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      REXKRNL_DEBUG("XGIUserWriteAchievements({}, {:08X})", achievement_count,
                    achievements_ptr);
      if (achievement_count && !achievements_ptr) {
        return X_E_INVALIDARG;
      }
      const auto* achievements =
          memory_->TranslateVirtual<const X_USER_ACHIEVEMENT*>(
              achievements_ptr);
      for (uint32_t i = 0; i < achievement_count; ++i) {
        UnlockAchievement(kernel_state_, achievements[i].user_index,
                          achievements[i].achievement_id);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      assert_true(!buffer_length || buffer_length == 28);
      // Sequence:
      // - XamSessionCreateHandle
      // - XamSessionRefObjByHandle
      // - [this]
      // - CloseHandle
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_slots_public = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t num_slots_private = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t nonce_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      SystemLinkSession session;
      session.flags = flags;
      session.public_slots = std::max(num_slots_public, 1u);
      session.private_slots = num_slots_private;
      session.user_index = user_index;
      session.owner_xuid = kernel_state_->user_profile()->xuid();
            EnsureLanPlayer(kernel_state_, session.owner_xuid,
                      kernel_state_->user_profile()->name());
session.is_host = (flags & kSessionHostFlag) != 0;
      session.nonce = MakeSessionNonce(session_ptr, session.owner_xuid);
      session.lifecycle_phase = 1;
      session.lifecycle_sequence = 1;

      if (session.is_host) {
        SystemLinkIdentity local_identity;
        if (SystemLinkGetLocalIdentity(&local_identity)) {
          session.host_ipv4 = local_identity.ipv4_address;
          session.host_mac = local_identity.mac;
        }
        session.session_id = MakeSystemLinkSessionId(session.nonce);
        session.key_exchange_key = MakeSystemLinkKey(session.nonce ^ session.owner_xuid);
        session.host_xuid = session.owner_xuid;
      } else if (session_info_ptr) {
        XSESSION_INFO info = {};
        std::memcpy(&info, memory_->TranslateVirtual(session_info_ptr), sizeof(info));
        ApplySessionInfo(info, &session);
        if (const auto remote_session = FindDiscoveredSession(session.session_id)) {
          session.host_xuid = remote_session->host_xuid;
          session.peer_xuid = remote_session->host_xuid;
          session.game_port = remote_session->game_port;
          session.host_ipv4 = remote_session->host_ipv4;
          session.host_mac = remote_session->mac;
          session.key_exchange_key = remote_session->key_exchange_key;
          if (session.public_slots > 0) {
            session.remote_public_members = 1;
          } else if (session.private_slots > 0) {
            session.remote_private_members = 1;
          }
          REXKRNL_INFO(
              "System-link join prepared with discovered host member "
              "(host_xuid={:016X}, name='{}', host={:08X}:{}, public_remote={}, private_remote={})",
              session.host_xuid, DecodeGenerationsLobbyName(*remote_session),
              session.host_ipv4, session.game_port, session.remote_public_members,
              session.remote_private_members);
        }
      }

      {
        std::scoped_lock lock(system_link_sessions_mutex);
        system_link_sessions[session_ptr] = session;
      }

      if (session_info_ptr) {
        const XSESSION_INFO info = MakeSessionInfo(session);
        std::memcpy(memory_->TranslateVirtual(session_info_ptr), &info, sizeof(info));
      }
      if (nonce_ptr) {
        memory::store_and_swap<uint64_t>(memory_->TranslateVirtual(nonce_ptr), session.nonce);
      }
      if (session.is_host) {
        PublishSystemLinkSession(session_ptr, kernel_state_->title_id());
      }

      LogSessionLifecycle(session_ptr, session, session.is_host ? "create-host" : "create-join");
      REXKRNL_INFO("System-link session {} ({:08X}, public={}, private={}, user={})",
                   session.is_host ? "host created" : "join prepared", session_ptr,
                   num_slots_public, num_slots_private, user_index);
      return X_E_SUCCESS;
    }
    case 0x000B0011: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      bool was_host = false;
      std::optional<SystemLinkSession> deleted_session;
      {
        std::scoped_lock lock(system_link_sessions_mutex);
        const auto it = system_link_sessions.find(obj_ptr);
        if (it == system_link_sessions.end()) {
          REXKRNL_ERROR("[XEXONLINE][SESSION] delete rejected unknown session={:08X}", obj_ptr);
          return X_E_INVALIDARG;
        }
        if (session_nonce && it->second.nonce && session_nonce != it->second.nonce) {
          REXKRNL_ERROR(
              "[XEXONLINE][SESSION] delete nonce mismatch session={:08X} expected={:016X} got={:016X}",
              obj_ptr, it->second.nonce, session_nonce);
          return X_E_INVALIDARG;
        }
        was_host = it->second.is_host;
        it->second.lifecycle_phase = 5;
        ++it->second.lifecycle_sequence;
        LogSessionLifecycle(obj_ptr, it->second, "delete");
        deleted_session = it->second;
        system_link_sessions.erase(it);
      }
      if (deleted_session) {
        CommitLocalMatchRecord(kernel_state_, obj_ptr, *deleted_session);
      }
      if (was_host) {
        SystemLinkRemoveSession(obj_ptr);
        RemoveLanProfile(obj_ptr);
      }

      REXKRNL_DEBUG("XGISessionDelete({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return X_E_SUCCESS;
    }
    case 0x000B0012: {
      assert_true(!buffer_length || buffer_length == 20);
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t user_index_array = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t private_slots_array = memory::load_and_swap<uint32_t>(buffer + 16);

      const bool join_local = xuid_array_ptr == 0;
      bool found_session = false;
      {
        std::scoped_lock lock(system_link_sessions_mutex);
        const auto it = system_link_sessions.find(session_ptr);
        if (it != system_link_sessions.end()) {
          found_session = true;
          auto& session = it->second;
          const uint32_t members_before = ActualMemberCount(session);
          if (join_local && user_index_array) {
            session.user_index =
                memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(user_index_array));
          } else if (!join_local && user_count) {
            session.peer_xuid = memory::load_and_swap<uint64_t>(
                memory_->TranslateVirtual(xuid_array_ptr));
          }
          AddSessionMembers(&session, user_count, !join_local,
                            private_slots_array ? memory_->TranslateVirtual(private_slots_array)
                                                : nullptr,
                            memory_);
          session.lifecycle_phase = 2;
          ++session.lifecycle_sequence;
          LogSessionLifecycle(session_ptr, session,
                              join_local ? "join-local" : "join-remote");
          REXKRNL_INFO(
              "System-link XSessionJoin{} applied (session={:08X}, "
              "peer_xuid={:016X}, members={}->{})",
              join_local ? "Local" : "Remote", session_ptr,
              session.peer_xuid, members_before, ActualMemberCount(session));
        }
      }

      if (!found_session) {
        REXKRNL_ERROR("XSessionJoin rejected unknown session ({:08X})", session_ptr);
        return X_E_INVALIDARG;
      }

      PublishSystemLinkSession(session_ptr, kernel_state_->title_id());

      REXKRNL_INFO("System-link session joined {} ({:08X}, users={}, xuids={:08X}, indexes={:08X})",
                   join_local ? "locally" : "remotely", session_ptr, user_count,
                   xuid_array_ptr, user_index_array);
      return X_E_SUCCESS;
    }
    case 0x000B0013: {
      assert_true(!buffer_length || buffer_length == 20);
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t user_index_array = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t private_slots_array = memory::load_and_swap<uint32_t>(buffer + 16);

      const bool leave_local = xuid_array_ptr == 0;
      bool found_session = false;
      {
        std::scoped_lock lock(system_link_sessions_mutex);
        const auto it = system_link_sessions.find(session_ptr);
        if (it != system_link_sessions.end()) {
          found_session = true;
          auto& session = it->second;
          RemoveSessionMembers(
              &session, user_count, !leave_local,
              private_slots_array ? memory_->TranslateVirtual(private_slots_array) : nullptr,
              memory_);
          if (!leave_local && !session.remote_public_members &&
              !session.remote_private_members) {
            session.peer_xuid = 0;
          }
          ++session.lifecycle_sequence;
          LogSessionLifecycle(session_ptr, session,
                              leave_local ? "leave-local" : "leave-remote");
        }
      }
      if (!found_session) {
        REXKRNL_ERROR("XSessionLeave rejected unknown session ({:08X})", session_ptr);
        return X_E_INVALIDARG;
      }
      PublishSystemLinkSession(session_ptr, kernel_state_->title_id());
      REXKRNL_INFO("System-link session left {} ({:08X}, users={}, xuids={:08X}, indexes={:08X})",
                   leave_local ? "locally" : "remotely", session_ptr, user_count,
                   xuid_array_ptr, user_index_array);
      return X_E_SUCCESS;
    }
    case 0x000B0014: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      bool found_session = false;
      {
        std::scoped_lock lock(system_link_sessions_mutex);
        const auto it = system_link_sessions.find(obj_ptr);
        if (it != system_link_sessions.end()) {
          if (session_nonce && it->second.nonce && session_nonce != it->second.nonce) {
            REXKRNL_ERROR(
                "[XEXONLINE][SESSION] start nonce mismatch session={:08X} expected={:016X} got={:016X}",
                obj_ptr, it->second.nonce, session_nonce);
            return X_E_INVALIDARG;
          }
          if (ActualMemberCount(it->second) < 2) {
            REXKRNL_WARN(
                "[XEXONLINE][SESSION] start rejected before remote membership session={:08X} members={}",
                obj_ptr, ActualMemberCount(it->second));
            return X_E_FAIL;
          }
          it->second.started = true;
          it->second.lifecycle_phase = 3;
          ++it->second.lifecycle_sequence;
          LogSessionLifecycle(obj_ptr, it->second, "start");
          found_session = true;
        }
      }
      if (!found_session) {
        REXKRNL_ERROR("XSessionStart rejected unknown session ({:08X})", obj_ptr);
        return X_E_INVALIDARG;
      }
      BeginLocalMatchRecord(obj_ptr);
      PublishSystemLinkSession(obj_ptr, kernel_state_->title_id());
      REXKRNL_DEBUG("XSessionStart({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      // send high scores?
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      bool found_session = false;
      SystemLinkSession finished_session;
      {
        std::scoped_lock lock(system_link_sessions_mutex);
        const auto it = system_link_sessions.find(obj_ptr);
        if (it != system_link_sessions.end()) {
          if (session_nonce && it->second.nonce && session_nonce != it->second.nonce) {
            REXKRNL_ERROR(
                "[XEXONLINE][SESSION] end nonce mismatch session={:08X} expected={:016X} got={:016X}",
                obj_ptr, it->second.nonce, session_nonce);
            return X_E_INVALIDARG;
          }
          if (!it->second.started) {
            REXKRNL_WARN("[XEXONLINE][SESSION] duplicate end session={:08X}", obj_ptr);
          }
          it->second.started = false;
          it->second.lifecycle_phase = 4;
          ++it->second.lifecycle_sequence;
          LogSessionLifecycle(obj_ptr, it->second, "end");
          finished_session = it->second;
          found_session = true;
        }
      }
      if (!found_session) {
        REXKRNL_ERROR("XSessionEnd rejected unknown session ({:08X})", obj_ptr);
        return X_E_INVALIDARG;
      }
      PublishSystemLinkSession(obj_ptr, kernel_state_->title_id());
      CommitLocalMatchRecord(kernel_state_, obj_ptr, finished_session);
      REXKRNL_DEBUG("XSessionEnd({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return X_E_SUCCESS;
    }
    case 0x000B0016: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t props_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearch({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X})", proc_index,
                    user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr);

      const uint32_t search_limit = std::clamp(num_results, 1u, 64u);
      auto sessions = DiscoverSystemLinkSessions(kernel_state_->title_id(), search_limit);
      sessions = FilterSessionsBySearch(std::move(sessions), memory_, num_props,
                                       props_ptr, num_ctx, ctx_ptr, 1);
      PrioritizeGenerationsSearchMetadata(&sessions, memory_, num_props, props_ptr,
                                          num_ctx, ctx_ptr);
      REXKRNL_INFO("[XEXONLINE][SEARCH] filtered result count={}", sessions.size());
      WriteSearchResultsFromSizePointer(memory_, results_buffer_size, search_results_ptr, sessions);
      return X_E_SUCCESS;
    }
    case 0x000B0018: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t maxPublicSlots = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t maxPrivateSlots = memory::load_and_swap<uint16_t>(buffer + 12);

      bool found_session = false;
      uint32_t effective_flags = flags;
      bool session_is_host = false;
      {
        std::scoped_lock lock(system_link_sessions_mutex);
        const auto it = system_link_sessions.find(obj_ptr);
        if (it != system_link_sessions.end()) {
          auto& session = it->second;
          session_is_host = session.is_host;
          effective_flags =
              session.is_host ? (flags | kSessionHostFlag)
                              : (flags & ~kSessionHostFlag);
          session.flags = effective_flags;
          session.public_slots = std::max({maxPublicSlots, 1u, FilledPublicSlots(session)});
          session.private_slots = std::max<uint32_t>(maxPrivateSlots, FilledPrivateSlots(session));
          ++session.lifecycle_sequence;
          LogSessionLifecycle(obj_ptr, session, "modify");
          found_session = true;
        }
      }
      if (!found_session) {
        REXKRNL_ERROR("XSessionModify rejected unknown session ({:08X})", obj_ptr);
        return X_E_INVALIDARG;
      }
      PublishSystemLinkSession(obj_ptr, kernel_state_->title_id());
      REXKRNL_INFO(
          "XSessionModify({:08X}) requested_flags={:08X} "
          "effective_flags={:08X} host={} public={} private={}",
          obj_ptr, flags, effective_flags, session_is_host, maxPublicSlots,
          maxPrivateSlots);

      return X_E_SUCCESS;
    }
    case 0x000B001C: {
      assert_true(!buffer_length || buffer_length == 36);

      // session_search
      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t props_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      //
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 32);

      REXKRNL_INFO("[XEXONLINE][SEARCH] XSessionSearchEx({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X}, {})",
                    proc_index, user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr, num_users);

      const uint32_t search_limit = std::clamp(num_results, 1u, 64u);
      auto sessions = DiscoverSystemLinkSessions(kernel_state_->title_id(), search_limit);
      sessions = FilterSessionsBySearch(std::move(sessions), memory_, num_props,
                                       props_ptr, num_ctx, ctx_ptr,
                                       std::max(num_users, 1u));
      PrioritizeGenerationsSearchMetadata(&sessions, memory_, num_props, props_ptr,
                                          num_ctx, ctx_ptr);
      REXKRNL_INFO("[XEXONLINE][SEARCH] extended filtered result count={}",
                   sessions.size());
      WriteSearchResultsFromSizePointer(memory_, results_buffer_size, search_results_ptr, sessions);

      return X_E_SUCCESS;
    }
    case 0x000B001D: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      // XSessionGetDetails passes pcbDetails, not the byte count directly. The
      // title supplies a guest pointer here; treating that address as a length
      // zeroed its stack and left the local member lookup with no valid data.
      uint32_t details_buffer_size_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t details_buffer_size =
          details_buffer_size_ptr
              ? memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(details_buffer_size_ptr))
              : 0;
      uint32_t session_details_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_INFO("XSessionGetDetails({:08X}, size={:08X}->{}, {:08X}, {}, {}, {})", obj_ptr,
                   details_buffer_size_ptr, details_buffer_size, session_details_ptr, reserved1,
                   reserved2, reserved3);

      const auto session = FindSystemLinkSession(obj_ptr);
      if (!session) {
        REXKRNL_ERROR("XSessionGetDetails rejected unknown session ({:08X})", obj_ptr);
        return X_E_INVALIDARG;
      }
      const uint32_t required_details_size =
          kSessionDetailsMemberOffset +
          (ActualMemberCount(*session) * sizeof(XSESSION_MEMBER));
      if (details_buffer_size_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(details_buffer_size_ptr),
                                         required_details_size);
      }
      if (!session_details_ptr) {
        REXKRNL_INFO("XSessionGetDetails size query ({:08X}, required={}, members={})", obj_ptr,
                     required_details_size, ActualMemberCount(*session));
        return X_E_SUCCESS;
      }
      if (details_buffer_size < required_details_size ||
          details_buffer_size < kSessionDetailsMinSize) {
        REXKRNL_ERROR(
            "XSessionGetDetails buffer too small (size={}, required={}, ptr={:08X}, members={})",
            details_buffer_size, required_details_size, session_details_ptr,
            ActualMemberCount(*session));
        return X_STATUS_BUFFER_TOO_SMALL;
      }
      auto details_buffer = memory_->TranslateVirtual(session_details_ptr);
      std::memset(details_buffer, 0, details_buffer_size);

      XSESSION_LOCAL_DETAILS details = {};
      details.user_index_host = session->is_host ? session->user_index : kNoUserIndex;
      details.flags = session->flags;
      details.max_public_slots = session->public_slots;
      details.max_private_slots = session->private_slots;
      details.available_public_slots = AvailablePublicSlots(*session);
      details.available_private_slots = AvailablePrivateSlots(*session);
      details.actual_member_count = ActualMemberCount(*session);
      details.state = session->started ? kSessionStateInGame : kSessionStateLobby;
      details.nonce = session->nonce;
      details.session_info = MakeSessionInfo(*session);
      std::memcpy(details.arbitration_session_id.ab, session->arbitration_session_id.data(),
                  session->arbitration_session_id.size());
      const uint32_t member_capacity =
          (details_buffer_size - kSessionDetailsMemberOffset) / sizeof(XSESSION_MEMBER);
      const uint32_t member_ptr = session_details_ptr + kSessionDetailsMemberOffset;
      details.session_members_ptr = member_ptr;

      std::vector<XSESSION_MEMBER> members;
      const auto append_local_member = [&]() {
        if (!session->local_public_members && !session->local_private_members) return;
        XSESSION_MEMBER local_member = {};
        local_member.online_xuid = kernel_state_->user_profile()->xuid();
        local_member.user_index = session->user_index;
        local_member.flags =
            session->local_private_members ? kSessionMemberPrivateSlot : 0;
        members.push_back(local_member);
      };
      const auto append_remote_member = [&]() {
        if (!session->remote_public_members && !session->remote_private_members) return;
        XSESSION_MEMBER remote_member = {};
        remote_member.online_xuid =
            session->peer_xuid ? session->peer_xuid : session->host_xuid;
        remote_member.user_index = kNoUserIndex;
        remote_member.flags =
            session->remote_private_members ? kSessionMemberPrivateSlot : 0;
        members.push_back(remote_member);
      };

      // Generations assigns native lobby ownership from the ordering returned
      // by XSessionGetDetails. A host owns the first/local entry, while a joiner
      // must see the remote host first and itself second. Returning local-first
      // on both machines made the client promote itself as host and remain on
      // "Transmitting" even though XSessionJoinRemote had succeeded.
      if (session->is_host) {
        append_local_member();
        append_remote_member();
      } else {
        append_remote_member();
        append_local_member();
      }
      details.returned_member_count =
          std::min<uint32_t>(member_capacity, static_cast<uint32_t>(members.size()));
      if (members.size() >= 2) {
        const uint64_t local_xuid = kernel_state_->user_profile()->xuid();
        const uint64_t remote_xuid =
            session->peer_xuid ? session->peer_xuid : session->host_xuid;
        REXKRNL_INFO(
            "[XEXONLINE][SESSION] ordered details session={:08X} host={} first={:016X} second={:016X}",
            obj_ptr, session->is_host,
            session->is_host ? local_xuid : remote_xuid,
            session->is_host ? remote_xuid : local_xuid);
      }

      std::memcpy(details_buffer, &details, sizeof(details));
      auto* member_buffer = reinterpret_cast<XSESSION_MEMBER*>(
          details_buffer + kSessionDetailsMemberOffset);
      for (uint32_t i = 0; i < details.returned_member_count; ++i) {
        member_buffer[i] = members[i];
      }

      LogSessionLifecycle(obj_ptr, *session, "get-details");
      REXKRNL_INFO("System-link session details ({:08X}, members={}, public={}/{}, private={}/{})",
                   obj_ptr, ActualMemberCount(*session), FilledPublicSlots(*session),
                   session->public_slots, FilledPrivateSlots(*session), session->private_slots);

      return X_E_SUCCESS;
    }
    case 0x000B001E: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      if (!session_info_ptr) {
        REXKRNL_ERROR("XSessionMigrateHost rejected missing session-info output ({:08X})", obj_ptr);
        return X_E_FAIL;
      }

      SystemLinkSession migrated_session;
      bool found_session = false;
      {
        std::scoped_lock lock(system_link_sessions_mutex);
        const auto it = system_link_sessions.find(obj_ptr);
        if (it != system_link_sessions.end()) {
          found_session = true;
          auto& session = it->second;

          if (user_index != kNoUserIndex) {
            session.is_host = true;
            session.flags |= kSessionHostFlag;
            session.user_index = user_index;
            session.owner_xuid = kernel_state_->user_profile()->xuid();
            session.host_xuid = session.owner_xuid;

            SystemLinkIdentity local_identity;
            if (SystemLinkGetLocalIdentity(&local_identity)) {
              session.host_ipv4 = local_identity.ipv4_address;
              session.host_mac = local_identity.mac;
            }

            // The peer already connected using this identity. Host migration
            // refreshes the owning address but must not silently replace the
            // XNKID or exchange key underneath that live connection.
            if (std::all_of(session.session_id.begin(), session.session_id.end(),
                            [](uint8_t value) { return value == 0; })) {
              session.session_id = MakeSystemLinkSessionId(session.nonce ^ session.owner_xuid);
            }
            if (std::all_of(session.key_exchange_key.begin(), session.key_exchange_key.end(),
                            [](uint8_t value) { return value == 0; })) {
              session.key_exchange_key = MakeSystemLinkKey(session.nonce ^ session.owner_xuid);
            }
          } else {
            session.is_host = false;
            session.flags &= ~kSessionHostFlag;
          }

          session.arbitration_session_id = session.session_id;
          session.lifecycle_phase = 6;
          ++session.lifecycle_sequence;
          LogSessionLifecycle(obj_ptr, session, "migrate-host");
          migrated_session = session;
        }
      }

      if (!found_session) {
        REXKRNL_ERROR("XSessionMigrateHost rejected unknown session ({:08X})", obj_ptr);
        return X_E_INVALIDARG;
      }

      const XSESSION_INFO info = MakeSessionInfo(migrated_session);
      std::memcpy(memory_->TranslateVirtual(session_info_ptr), &info, sizeof(info));
      if (migrated_session.is_host) {
        PublishSystemLinkSession(obj_ptr, kernel_state_->title_id());
      }

      REXKRNL_INFO(
          "XSessionMigrateHost completed ({:08X}, user={}, host={}, address={:08X}:{}, "
          "xnkid={:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}, reserved={:08X}/"
          "{:08X}/{:08X})",
          obj_ptr, user_index, migrated_session.is_host, migrated_session.host_ipv4,
          migrated_session.game_port, migrated_session.session_id[0],
          migrated_session.session_id[1], migrated_session.session_id[2],
          migrated_session.session_id[3], migrated_session.session_id[4],
          migrated_session.session_id[5], migrated_session.session_id[6],
          migrated_session.session_id[7], reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0019: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);

      if (!session_info_ptr) {
        return X_E_INVALIDARG;
      }
      XSESSION_INFO invitation = {};
      const auto discovered = DiscoverSystemLinkSessions(kernel_state_->title_id(), 1);
      if (!discovered.empty()) {
        SystemLinkSession temp = {};
        temp.session_id = discovered.front().session_id;
        temp.key_exchange_key = discovered.front().key_exchange_key;
        temp.host_mac = discovered.front().mac;
        temp.host_ipv4 = discovered.front().host_ipv4;
        temp.game_port = discovered.front().game_port;
        invitation = MakeSessionInfo(temp);
      }
      std::memcpy(memory_->TranslateVirtual(session_info_ptr), &invitation, sizeof(invitation));
      REXKRNL_INFO("XSessionGetInvitationData user={} ptr={:08X} found={}",
                   user_index, session_info_ptr, !discovered.empty());
      return X_E_SUCCESS;
    }
    case 0x000B001A: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);
      uint32_t session_duration_sec = memory::load_and_swap<uint32_t>(buffer + 16);  // 300
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      constexpr uint32_t kArbitrationResultsSize = 3592;
      const auto session = FindSystemLinkSession(obj_ptr);
      if (!session) {
        REXKRNL_ERROR(
            "[XEXONLINE][ARBITRATION] rejected unknown session={:08X} nonce={:016X}",
            obj_ptr, session_nonce);
        return X_E_INVALIDARG;
      }
      if (session_nonce && session->nonce && session_nonce != session->nonce) {
        REXKRNL_ERROR(
            "[XEXONLINE][ARBITRATION] nonce mismatch session={:08X} expected={:016X} got={:016X}",
            obj_ptr, session->nonce, session_nonce);
        return X_E_INVALIDARG;
      }
      if (!results_ptr || results_buffer_size < kArbitrationResultsSize) {
        REXKRNL_INFO(
            "[XEXONLINE][ARBITRATION] size query session={:08X} supplied={} required={}",
            obj_ptr, results_buffer_size, kArbitrationResultsSize);
        return X_ERROR_INSUFFICIENT_BUFFER;
      }
      std::memset(memory_->TranslateVirtual(results_ptr), 0, kArbitrationResultsSize);
      // Preserve the authoritative nonce and current member count in the
      // otherwise title-private result header. This keeps repeated calls
      // deterministic and makes lifecycle disagreements visible in captures.
      memory::store_and_swap<uint64_t>(memory_->TranslateVirtual(results_ptr + 0),
                                       session->nonce);
      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(results_ptr + 8),
                                       ActualMemberCount(*session));
      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(results_ptr + 12),
                                       session->is_host ? 1u : 0u);
      REXKRNL_INFO(
          "[XEXONLINE][ARBITRATION] registered session={:08X} nonce={:016X} duration={} "
          "members={} host={} flags={:08X}",
          obj_ptr, session->nonce, session_duration_sec, ActualMemberCount(*session),
          session->is_host, flags);
      return X_E_SUCCESS;
    }
    case 0x000B001B: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByID({}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);

      const uint32_t search_limit = std::clamp(num_session_ids, 1u, 64u);
      auto sessions = DiscoverSystemLinkSessions(kernel_state_->title_id(), search_limit);
      const uint8_t* session_ids =
          session_ids_ptr && num_session_ids <= 64 ? memory_->TranslateVirtual(session_ids_ptr)
                                                    : nullptr;
      sessions = FilterSessionsById(std::move(sessions), session_ids, num_session_ids);
      WriteSearchResultsFromSizePointer(memory_, results_buffer_size, search_results_ptr, sessions);

      return X_E_SUCCESS;
    }
    case 0x000B001F: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t array_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionModifySkill({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr, array_count,
                    xuid_array_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0020: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t view_id = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XUserResetStatsView({:08X}, {})", user_index, view_id);

      return X_E_SUCCESS;
    }
    case 0x000B0021: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t title_id = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t xuids_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t specs_count = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t specs_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t results_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_INFO("[XEXONLINE][STATS] XUserReadStats({}, {}, {:08X}, {}, {:08X}, {}, {:08X})", title_id, xuids_count,
                    xuids_ptr, specs_count, specs_ptr, results_size, results_ptr);

      if (results_ptr && results_size) {
        uint8_t* results = memory_->TranslateVirtual(results_ptr);
        std::memset(results, 0, results_size);
        REXKRNL_INFO(
            "XUserReadStats direct request currently initialized safely; leaderboard UI uses populated XamUserCreateStatsEnumerator rows (title={:08X}, xuids={}, specs={}, size={})",
            title_id, xuids_count, specs_count, results_size);
      }

      return X_E_SUCCESS;
    }
    case 0x000B0025: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_INFO("[XEXONLINE][STATS] XSessionWriteStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);

      AppendLocalMatchStats(obj_ptr, xuid,
                            DecodeMatchViews(memory_, num_views, views_ptr));

      return X_E_SUCCESS;
    }
    case 0x000B0026: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_INFO("[XEXONLINE][STATS] XSessionFlushStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);

      if (num_views && views_ptr) {
        AppendLocalMatchStats(obj_ptr, xuid,
                              DecodeMatchViews(memory_, num_views, views_ptr));
      }
      if (const auto active_session = FindSystemLinkSession(obj_ptr)) {
        CommitLocalMatchRecord(kernel_state_, obj_ptr, *active_session);
        BeginLocalMatchRecord(obj_ptr);
        PublishSystemLinkSession(obj_ptr, kernel_state_->title_id());
        REXKRNL_INFO(
            "[XEXONLINE][STATS] flushed and replay-published leaderboard "
            "record for session {:08X}",
            obj_ptr);
      }

      return X_E_SUCCESS;
    }
    case 0x000B0036: {
      // Called after opening xbox live arcade and clicking on xbox live v5759
      // to 5787 and called after clicking xbox live in the game library from
      // v6683 to v6717
      // Does not get sent a buffer
      REXKRNL_INFO("[XEXONLINE][PROFILE] XInvalidateGamerTileCache -> local cache invalidated");
      // The recomp uses local profile imagery and has no Xbox Live tile CDN.
      // Treat invalidation as complete so the title does not enter its error path.
      return X_E_SUCCESS;
    }
    case 0x000B003D: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t AnId_buffer_size = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t AnId_buffer_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t block = memory::load_and_swap<uint32_t>(buffer + 12);

      REXKRNL_DEBUG("XUserGetANID({:08X}, {:08X}, {:08X}, {:08X})", user_index, AnId_buffer_size,
                    AnId_buffer_ptr, block);

      return X_E_SUCCESS;
    }
    case 0x000B0041: {
      assert_true(!buffer_length || buffer_length == 32);
      // 00000000 2789fecc 00000000 00000000 200491e0 00000000 200491f0 20049340
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      auto context = context_ptr ? memory_->TranslateVirtual(context_ptr) : nullptr;
      uint32_t context_id = context ? memory::load_and_swap<uint32_t>(context + 0) : 0;
      uint32_t value = 0;
      bool found = false;
      {
        std::scoped_lock lock(xgi_user_state_mutex);
        const auto it = xgi_user_contexts.find(MakeUserStateKey(user_index, context_id));
        if (it != xgi_user_contexts.end()) {
          value = it->second;
          found = true;
        }
      }
      if (context) {
        memory::store_and_swap<uint32_t>(context + 4, value);
      }
      REXKRNL_INFO(
          "[XEXONLINE] XGIUserGetContext user={} context={:08X} value={:08X} found={}",
          user_index, context_id, value, found);
      // A missing context is represented by the default value. Returning
      // failure here causes the title's asynchronous online state machine to
      // take its error path before it can invoke XSessionCreate/Search.
      return X_E_SUCCESS;
    }
    case 0x000B0060: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByIds({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);

      const uint32_t search_limit = std::clamp(num_session_ids, 1u, 64u);
      auto sessions = DiscoverSystemLinkSessions(kernel_state_->title_id(), search_limit);
      const uint8_t* session_ids =
          session_ids_ptr && num_session_ids <= 64 ? memory_->TranslateVirtual(session_ids_ptr)
                                                    : nullptr;
      sessions = FilterSessionsById(std::move(sessions), session_ids, num_session_ids);
      WriteSearchResultsFromSizePointer(memory_, results_buffer_size, search_results_ptr, sessions);
      return X_E_SUCCESS;
    }
    case 0x000B0065: {
      assert_true(!buffer_length || buffer_length == 52);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_weighted_properties = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_weighted_contexts = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 24);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 26);
      uint32_t non_weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      uint32_t non_weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 32);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 36);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 40);
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 44);
      uint32_t weighted_search = memory::load_and_swap<uint32_t>(buffer + 48);

      REXKRNL_INFO(
          "[XEXONLINE][SEARCH] XSessionSearchWeighted({:08X}, {:08X}, {:08X}, {}, {}, {:08X}, {:08X}, {}, {}, {:08X}, "
          "{:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
          proc_index, user_index, num_results, num_weighted_properties, num_weighted_contexts,
          weighted_search_properties_ptr, weighted_search_contexts_ptr, num_props, num_ctx,
          non_weighted_search_properties_ptr, non_weighted_search_contexts_ptr, results_buffer_size,
          search_results_ptr, num_users, weighted_search);

      const uint32_t search_limit = std::clamp(num_results, 1u, 64u);
      auto sessions = DiscoverSystemLinkSessions(kernel_state_->title_id(), search_limit);
      sessions = FilterSessionsBySearch(
          std::move(sessions), memory_, num_props,
          non_weighted_search_properties_ptr, num_ctx,
          non_weighted_search_contexts_ptr, std::max(num_users, 1u));
      REXKRNL_INFO("[XEXONLINE][SEARCH] weighted filtered result count={} "
                   "(weighted criteria retained for ranking diagnostics)",
                   sessions.size());
      WriteSearchResultsFromSizePointer(memory_, results_buffer_size, search_results_ptr, sessions);
      return X_E_SUCCESS;
    }
    case 0x000B0071: {
      // This title probes this private XGI message as an optional capability.
      // Return a deterministic empty result instead of leaving guest memory
      // unchanged, which could be interpreted as a supported online service.
      if (buffer_ptr && buffer_length) {
        auto* output = memory_->TranslateVirtual<uint8_t*>(buffer_ptr);
        if (output) {
          std::memset(output, 0, buffer_length);
        }
      }
      REXKRNL_INFO("[XEXONLINE][XGI] capability probe 0x000B0071 -> empty local result ({} bytes)",
                   buffer_length);
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
