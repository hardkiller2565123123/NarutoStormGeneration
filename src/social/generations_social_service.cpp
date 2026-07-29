#include "generations_social_service.h"

#include <algorithm>
#include <fstream>
#include <system_error>

#include <toml++/toml.hpp>

namespace generations::social {
namespace fs = std::filesystem;

namespace {
std::string PresenceKey(PresenceState state) {
  switch (state) {
    case PresenceState::online: return "online";
    case PresenceState::menus: return "menus";
    case PresenceState::lobby: return "lobby";
    case PresenceState::match: return "match";
    default: return "offline";
  }
}

PresenceState ParsePresence(std::string_view state) {
  if (state == "online") return PresenceState::online;
  if (state == "menus") return PresenceState::menus;
  if (state == "lobby") return PresenceState::lobby;
  if (state == "match") return PresenceState::match;
  return PresenceState::offline;
}

template <typename T>
T ReadValue(const toml::table& table, std::string_view key, T fallback) {
  if (auto value = table[key].value<T>()) return *value;
  return fallback;
}

bool SaveAtomic(const fs::path& path, const toml::table& data) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) return false;
    output << data << '\n';
    if (!output) return false;
  }
  fs::rename(temporary, path, ec);
  if (!ec) return true;
  ec.clear();
  fs::remove(path, ec);
  ec.clear();
  fs::rename(temporary, path, ec);
  return !ec;
}
}  // namespace

Service::Service(fs::path root)
    : root_(std::move(root)), database_path_(root_ / "social_state.toml") {}

bool Service::Initialize() {
  std::error_code ec;
  fs::create_directories(root_ / "invites" / "inbox", ec);
  fs::create_directories(root_ / "invites" / "outbox", ec);
  if (!fs::is_regular_file(database_path_)) return Save();
  return Reload();
}

bool Service::Reload() {
  std::lock_guard lock(mutex_);
  try {
    const toml::table root = toml::parse_file(database_path_.string());
    if (const auto* profile = root["profile"].as_table()) {
      local_profile_.xuid = ReadValue(*profile, "xuid", std::string{});
      local_profile_.gamertag = ReadValue(*profile, "gamertag", std::string{"LocalPlayer"});
      local_profile_.radmin_ip = ReadValue(*profile, "radmin_ip", std::string{});
      local_profile_.game_port = static_cast<uint16_t>(ReadValue<int64_t>(*profile, "game_port", 3074));
      local_profile_.presence = ParsePresence(ReadValue(*profile, "presence", std::string{"offline"}));
      local_profile_.rich_presence = ReadValue(*profile, "rich_presence", std::string{});
      local_profile_.battle_points = static_cast<int>(ReadValue<int64_t>(*profile, "battle_points", 0));
      local_profile_.wins = static_cast<int>(ReadValue<int64_t>(*profile, "wins", 0));
      local_profile_.losses = static_cast<int>(ReadValue<int64_t>(*profile, "losses", 0));
      local_profile_.joinable = ReadValue(*profile, "joinable", false);
    }
    friends_.clear();
    if (const auto* array = root["friends"].as_array()) {
      for (const auto& node : *array) {
        const auto* item = node.as_table();
        if (!item) continue;
        FriendRecord record;
        record.profile.xuid = ReadValue(*item, "xuid", std::string{});
        if (record.profile.xuid.empty()) continue;
        record.profile.gamertag = ReadValue(*item, "gamertag", std::string{"Unknown"});
        record.profile.radmin_ip = ReadValue(*item, "radmin_ip", std::string{});
        record.profile.game_port = static_cast<uint16_t>(ReadValue<int64_t>(*item, "game_port", 3074));
        record.profile.presence = ParsePresence(ReadValue(*item, "presence", std::string{"offline"}));
        record.profile.rich_presence = ReadValue(*item, "rich_presence", std::string{});
        record.profile.battle_points = static_cast<int>(ReadValue<int64_t>(*item, "battle_points", 0));
        record.profile.wins = static_cast<int>(ReadValue<int64_t>(*item, "wins", 0));
        record.profile.losses = static_cast<int>(ReadValue<int64_t>(*item, "losses", 0));
        record.profile.favorite = ReadValue(*item, "favorite", false);
        record.profile.joinable = ReadValue(*item, "joinable", false);
        record.incoming_request = ReadValue(*item, "incoming_request", false);
        record.outgoing_request = ReadValue(*item, "outgoing_request", false);
        friends_.emplace(record.profile.xuid, std::move(record));
      }
    }
    invites_.clear();
    if (const auto* array = root["invites"].as_array()) {
      for (const auto& node : *array) {
        const auto* item = node.as_table();
        if (!item) continue;
        GameInvite invite;
        invite.id = ReadValue(*item, "id", std::string{});
        if (invite.id.empty()) continue;
        invite.from_xuid = ReadValue(*item, "from_xuid", std::string{});
        invite.to_xuid = ReadValue(*item, "to_xuid", std::string{});
        invite.session_id = ReadValue(*item, "session_id", std::string{});
        invite.radmin_ip = ReadValue(*item, "radmin_ip", std::string{});
        invite.port = static_cast<uint16_t>(ReadValue<int64_t>(*item, "port", 3074));
        invite.consumed = ReadValue(*item, "consumed", false);
        invites_.emplace(invite.id, std::move(invite));
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool Service::Save() {
  std::lock_guard lock(mutex_);
  return SaveLocked();
}

void Service::SetLocalProfile(Profile profile) {
  std::lock_guard lock(mutex_);
  local_profile_ = std::move(profile);
  SaveLocked();
}

Profile Service::local_profile() const {
  std::lock_guard lock(mutex_);
  return local_profile_;
}

std::vector<FriendRecord> Service::friends() const {
  std::lock_guard lock(mutex_);
  std::vector<FriendRecord> result;
  result.reserve(friends_.size());
  for (const auto& [_, friend_record] : friends_) result.push_back(friend_record);
  std::stable_sort(result.begin(), result.end(), [](const FriendRecord& a, const FriendRecord& b) {
    if (a.profile.favorite != b.profile.favorite) return a.profile.favorite;
    if (a.profile.presence != b.profile.presence)
      return a.profile.presence != PresenceState::offline;
    return a.profile.gamertag < b.profile.gamertag;
  });
  return result;
}

std::optional<FriendRecord> Service::FindFriend(std::string_view xuid) const {
  std::lock_guard lock(mutex_);
  const auto found = friends_.find(std::string(xuid));
  return found == friends_.end() ? std::nullopt : std::optional(found->second);
}

bool Service::AddFriendRequest(Profile profile, bool incoming) {
  if (profile.xuid.empty()) return false;
  std::lock_guard lock(mutex_);
  auto& record = friends_[profile.xuid];
  record.profile = std::move(profile);
  record.incoming_request = incoming;
  record.outgoing_request = !incoming;
  PushToastLocked({ToastKind::request, "Friend request", record.profile.gamertag, record.profile.xuid});
  return SaveLocked();
}

bool Service::AcceptFriend(std::string_view xuid) {
  std::lock_guard lock(mutex_);
  const auto found = friends_.find(std::string(xuid));
  if (found == friends_.end()) return false;
  found->second.incoming_request = false;
  found->second.outgoing_request = false;
  PushToastLocked({ToastKind::request_accepted, "Friend added",
                   found->second.profile.gamertag + " is now your friend", found->second.profile.xuid});
  return SaveLocked();
}

bool Service::RemoveFriend(std::string_view xuid) {
  std::lock_guard lock(mutex_);
  if (!friends_.erase(std::string(xuid))) return false;
  return SaveLocked();
}

bool Service::SetFavorite(std::string_view xuid, bool favorite) {
  std::lock_guard lock(mutex_);
  const auto found = friends_.find(std::string(xuid));
  if (found == friends_.end()) return false;
  found->second.profile.favorite = favorite;
  return SaveLocked();
}

void Service::UpdatePresence(std::string_view xuid, PresenceState state,
                             std::string rich_presence, bool joinable,
                             std::string radmin_ip) {
  std::lock_guard lock(mutex_);
  const auto found = friends_.find(std::string(xuid));
  if (found == friends_.end()) return;
  const PresenceState old = found->second.profile.presence;
  found->second.profile.presence = state;
  found->second.profile.rich_presence = std::move(rich_presence);
  found->second.profile.joinable = joinable;
  if (!radmin_ip.empty()) found->second.profile.radmin_ip = std::move(radmin_ip);
  if (old == PresenceState::offline && state != PresenceState::offline) {
    PushToastLocked({ToastKind::friend_online, "Friend online",
                     found->second.profile.gamertag + " is now online", found->second.profile.xuid});
  } else if (old != PresenceState::offline && state == PresenceState::offline) {
    PushToastLocked({ToastKind::friend_offline, "Friend offline",
                     found->second.profile.gamertag + " went offline", found->second.profile.xuid});
  }
  SaveLocked();
}

bool Service::QueueInvite(GameInvite invite) {
  if (invite.id.empty()) return false;
  std::lock_guard lock(mutex_);
  const std::string source = invite.from_xuid;
  invites_[invite.id] = std::move(invite);
  PushToastLocked({ToastKind::invite, "Game invite", "A Generations game invite is waiting", source});
  return SaveLocked();
}

std::vector<GameInvite> Service::PendingInvites() const {
  std::lock_guard lock(mutex_);
  std::vector<GameInvite> result;
  for (const auto& [_, invite] : invites_) if (!invite.consumed) result.push_back(invite);
  return result;
}

bool Service::ConsumeInvite(std::string_view invite_id) {
  std::lock_guard lock(mutex_);
  const auto found = invites_.find(std::string(invite_id));
  if (found == invites_.end()) return false;
  found->second.consumed = true;
  return SaveLocked();
}

std::vector<ToastEvent> Service::DrainToasts() {
  std::lock_guard lock(mutex_);
  std::vector<ToastEvent> result;
  result.swap(toasts_);
  return result;
}

void Service::PushToastLocked(ToastEvent event) {
  toasts_.push_back(std::move(event));
  if (toasts_.size() > 64) toasts_.erase(toasts_.begin(), toasts_.begin() + 32);
}

bool Service::SaveLocked() {
  toml::table profile{{"xuid", local_profile_.xuid}, {"gamertag", local_profile_.gamertag},
                      {"radmin_ip", local_profile_.radmin_ip},
                      {"game_port", static_cast<int64_t>(local_profile_.game_port)},
                      {"presence", PresenceKey(local_profile_.presence)},
                      {"rich_presence", local_profile_.rich_presence},
                      {"battle_points", local_profile_.battle_points}, {"wins", local_profile_.wins},
                      {"losses", local_profile_.losses}, {"joinable", local_profile_.joinable}};
  toml::array friends;
  for (const auto& [_, record] : friends_) {
    const auto& p = record.profile;
    friends.push_back(toml::table{{"xuid", p.xuid}, {"gamertag", p.gamertag},
                                  {"radmin_ip", p.radmin_ip},
                                  {"game_port", static_cast<int64_t>(p.game_port)},
                                  {"presence", PresenceKey(p.presence)},
                                  {"rich_presence", p.rich_presence},
                                  {"battle_points", p.battle_points}, {"wins", p.wins},
                                  {"losses", p.losses}, {"favorite", p.favorite},
                                  {"joinable", p.joinable},
                                  {"incoming_request", record.incoming_request},
                                  {"outgoing_request", record.outgoing_request}});
  }
  toml::array invites;
  for (const auto& [_, invite] : invites_) {
    invites.push_back(toml::table{{"id", invite.id}, {"from_xuid", invite.from_xuid},
                                  {"to_xuid", invite.to_xuid}, {"session_id", invite.session_id},
                                  {"radmin_ip", invite.radmin_ip},
                                  {"port", static_cast<int64_t>(invite.port)},
                                  {"consumed", invite.consumed}});
  }
  return SaveAtomic(database_path_, toml::table{{"version", 1}, {"profile", std::move(profile)},
                                                {"friends", std::move(friends)},
                                                {"invites", std::move(invites)}});
}

}  // namespace generations::social
