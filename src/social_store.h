#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <toml++/toml.hpp>

namespace generations::social {
namespace fs = std::filesystem;

enum class PresenceState { kOffline, kOnline, kInMenus, kInLobby, kInMatch };

struct FriendEntry {
  std::string xuid;
  std::string gamertag;
  std::string radmin_ip;
  PresenceState presence = PresenceState::kOffline;
  std::string rich_presence = "Offline";
  std::string avatar = "default";
  int battle_points = 0;
  int wins = 0;
  int losses = 0;
  bool favorite = false;
  bool joinable = false;
  bool pending_incoming = false;
  bool pending_outgoing = false;
};

struct InviteEntry {
  std::string id;
  std::string from_xuid;
  std::string from_gamertag;
  std::string to_xuid;
  std::string session_id;
  std::string radmin_ip;
  uint16_t port = 3074;
  std::string created_utc;
  bool consumed = false;
};

inline const char* PresenceName(PresenceState state) {
  switch (state) {
    case PresenceState::kOnline: return "Online";
    case PresenceState::kInMenus: return "In Menus";
    case PresenceState::kInLobby: return "In Online Lobby";
    case PresenceState::kInMatch: return "In Match";
    default: return "Offline";
  }
}

inline PresenceState ParsePresence(std::string_view value) {
  if (value == "online") return PresenceState::kOnline;
  if (value == "menus") return PresenceState::kInMenus;
  if (value == "lobby") return PresenceState::kInLobby;
  if (value == "match") return PresenceState::kInMatch;
  return PresenceState::kOffline;
}

inline std::string PresenceKey(PresenceState value) {
  switch (value) {
    case PresenceState::kOnline: return "online";
    case PresenceState::kInMenus: return "menus";
    case PresenceState::kInLobby: return "lobby";
    case PresenceState::kInMatch: return "match";
    default: return "offline";
  }
}

class Store {
 public:
  explicit Store(fs::path root) : root_(std::move(root)) {}

  const fs::path& root() const { return root_; }
  fs::path FriendsPath() const { return root_ / "friends.toml"; }
  fs::path PresencePath() const { return root_ / "presence.toml"; }
  fs::path InviteOutbox() const { return root_ / "invites" / "outbox"; }
  fs::path InviteInbox() const { return root_ / "invites" / "inbox"; }

  void EnsureLayout() const {
    std::error_code ec;
    fs::create_directories(root_, ec);
    fs::create_directories(InviteOutbox(), ec);
    fs::create_directories(InviteInbox(), ec);
  }

  std::vector<FriendEntry> LoadFriends() const {
    EnsureLayout();
    std::vector<FriendEntry> result;
    if (!fs::is_regular_file(FriendsPath())) return result;
    try {
      const toml::table file = toml::parse_file(FriendsPath().string());
      const auto* friends = file["friends"].as_array();
      if (!friends) return result;
      for (const auto& node : *friends) {
        const auto* item = node.as_table();
        if (!item) continue;
        FriendEntry friend_entry;
        friend_entry.xuid = item->get_as<std::string>("xuid") ? item->get_as<std::string>("xuid")->get() : "";
        friend_entry.gamertag = item->get_as<std::string>("gamertag") ? item->get_as<std::string>("gamertag")->get() : "Unknown";
        friend_entry.radmin_ip = item->get_as<std::string>("radmin_ip") ? item->get_as<std::string>("radmin_ip")->get() : "";
        friend_entry.presence = ParsePresence(item->get_as<std::string>("presence") ? item->get_as<std::string>("presence")->get() : "offline");
        friend_entry.rich_presence = item->get_as<std::string>("rich_presence") ? item->get_as<std::string>("rich_presence")->get() : PresenceName(friend_entry.presence);
        friend_entry.avatar = item->get_as<std::string>("avatar") ? item->get_as<std::string>("avatar")->get() : "default";
        friend_entry.battle_points = item->get_as<int64_t>("battle_points") ? static_cast<int>(item->get_as<int64_t>("battle_points")->get()) : 0;
        friend_entry.wins = item->get_as<int64_t>("wins") ? static_cast<int>(item->get_as<int64_t>("wins")->get()) : 0;
        friend_entry.losses = item->get_as<int64_t>("losses") ? static_cast<int>(item->get_as<int64_t>("losses")->get()) : 0;
        friend_entry.favorite = item->get_as<bool>("favorite") && item->get_as<bool>("favorite")->get();
        friend_entry.joinable = item->get_as<bool>("joinable") && item->get_as<bool>("joinable")->get();
        friend_entry.pending_incoming = item->get_as<bool>("pending_incoming") && item->get_as<bool>("pending_incoming")->get();
        friend_entry.pending_outgoing = item->get_as<bool>("pending_outgoing") && item->get_as<bool>("pending_outgoing")->get();
        result.push_back(std::move(friend_entry));
      }
    } catch (...) {
    }
    std::stable_sort(result.begin(), result.end(), [](const FriendEntry& a, const FriendEntry& b) {
      const auto score = [](const FriendEntry& f) {
        return (f.favorite ? 100 : 0) + (f.presence != PresenceState::kOffline ? 10 : 0) + (f.joinable ? 1 : 0);
      };
      return score(a) > score(b);
    });
    return result;
  }

  bool SaveFriends(const std::vector<FriendEntry>& friends) const {
    EnsureLayout();
    toml::array array;
    for (const auto& f : friends) {
      array.push_back(toml::table{{"xuid", f.xuid}, {"gamertag", f.gamertag},
                                  {"radmin_ip", f.radmin_ip}, {"presence", PresenceKey(f.presence)},
                                  {"rich_presence", f.rich_presence}, {"avatar", f.avatar},
                                  {"battle_points", f.battle_points}, {"wins", f.wins},
                                  {"losses", f.losses}, {"favorite", f.favorite},
                                  {"joinable", f.joinable}, {"pending_incoming", f.pending_incoming},
                                  {"pending_outgoing", f.pending_outgoing}});
    }
    toml::table root{{"version", 1}, {"friends", std::move(array)}};
    return SaveAtomic(FriendsPath(), root);
  }

  void SeedPreviewData() const {
    if (fs::is_regular_file(FriendsPath())) return;
    std::vector<FriendEntry> friends = {
        {"0009000000000001", "Fuijin", "26.18.32.14", PresenceState::kInLobby,
         "Waiting in Player Match", "naruto", 18420, 128, 47, true, true, false, false},
        {"0009000000000002", "ShinobiStorm", "26.18.32.21", PresenceState::kInMatch,
         "Ranked Match - Round 2", "sasuke", 12110, 86, 52, false, false, false, false},
        {"0009000000000003", "LeafShadow", "", PresenceState::kOnline,
         "Naruto Generations Recomp", "kakashi", 7650, 44, 31, false, false, false, false},
        {"0009000000000004", "MaskedNinja", "", PresenceState::kOffline,
         "Last seen 2 hours ago", "tobi", 22130, 192, 80, false, false, false, false},
        {"0009000000000005", "AkatsukiAce", "26.18.32.33", PresenceState::kOnline,
         "Friend request received", "itachi", 9400, 61, 35, false, false, true, false},
    };
    SaveFriends(friends);
  }

  bool QueueInvite(const InviteEntry& invite) const {
    EnsureLayout();
    toml::table data{{"id", invite.id}, {"from_xuid", invite.from_xuid},
                     {"from_gamertag", invite.from_gamertag}, {"to_xuid", invite.to_xuid},
                     {"session_id", invite.session_id}, {"radmin_ip", invite.radmin_ip},
                     {"port", static_cast<int64_t>(invite.port)}, {"created_utc", invite.created_utc},
                     {"consumed", invite.consumed}};
    return SaveAtomic(InviteOutbox() / (invite.id + ".toml"), data);
  }

 private:
  static bool SaveAtomic(const fs::path& path, const toml::table& table) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = path.string() + ".tmp";
    {
      std::ofstream stream(temp, std::ios::trunc);
      if (!stream) return false;
      stream << table << '\n';
      if (!stream) return false;
    }
#ifdef _WIN32
    return ::MoveFileExW(temp.wstring().c_str(), path.wstring().c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    fs::rename(temp, path, ec);
    return !ec;
#endif
  }

  fs::path root_;
};

}  // namespace generations::social
