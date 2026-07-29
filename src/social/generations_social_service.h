#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace generations::social {

enum class PresenceState : uint8_t { offline, online, menus, lobby, match };
enum class ToastKind : uint8_t { friend_online, friend_offline, request, request_accepted, invite, joined };

struct Profile {
  std::string xuid;
  std::string gamertag;
  std::string radmin_ip;
  uint16_t game_port = 3074;
  PresenceState presence = PresenceState::offline;
  std::string rich_presence;
  int battle_points = 0;
  int wins = 0;
  int losses = 0;
  bool favorite = false;
  bool joinable = false;
};

struct FriendRecord {
  Profile profile;
  bool incoming_request = false;
  bool outgoing_request = false;
  std::chrono::system_clock::time_point last_seen{};
};

struct GameInvite {
  std::string id;
  std::string from_xuid;
  std::string to_xuid;
  std::string session_id;
  std::string radmin_ip;
  uint16_t port = 3074;
  std::chrono::system_clock::time_point created{};
  bool consumed = false;
};

struct ToastEvent {
  ToastKind kind = ToastKind::friend_online;
  std::string title;
  std::string message;
  std::string source_xuid;
};

// Local-first social model used by both the launcher and future XAM bridge.
// It intentionally performs no sockets, hooks or XAM exports yet. All changes
// are committed to an atomic TOML store so networking can be attached later
// without replacing the UI/data model.
class Service {
 public:
  explicit Service(std::filesystem::path root);

  bool Initialize();
  bool Reload();
  bool Save();

  void SetLocalProfile(Profile profile);
  Profile local_profile() const;

  std::vector<FriendRecord> friends() const;
  std::optional<FriendRecord> FindFriend(std::string_view xuid) const;
  bool AddFriendRequest(Profile profile, bool incoming);
  bool AcceptFriend(std::string_view xuid);
  bool RemoveFriend(std::string_view xuid);
  bool SetFavorite(std::string_view xuid, bool favorite);

  void UpdatePresence(std::string_view xuid, PresenceState state,
                      std::string rich_presence, bool joinable,
                      std::string radmin_ip = {});

  bool QueueInvite(GameInvite invite);
  std::vector<GameInvite> PendingInvites() const;
  bool ConsumeInvite(std::string_view invite_id);

  std::vector<ToastEvent> DrainToasts();

 private:
  void PushToastLocked(ToastEvent event);
  bool SaveLocked();

  std::filesystem::path root_;
  std::filesystem::path database_path_;
  mutable std::mutex mutex_;
  Profile local_profile_;
  std::unordered_map<std::string, FriendRecord> friends_;
  std::unordered_map<std::string, GameInvite> invites_;
  std::vector<ToastEvent> toasts_;
};

}  // namespace generations::social
