#include "profile_setup_dialog.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string_view>
#include <unordered_set>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

namespace generations {
namespace {

constexpr uint64_t kXuidPrefix = 0xB13E000000000000ull;
constexpr uint64_t kXuidRandomMask = 0x0000FFFFFFFFFFFFull;
constexpr uint64_t kLegacySharedXuid = 0xB13EBABEBABEBABEull;
constexpr uint64_t kRejectedXuidMask = 0x00C0000000000000ull;
constexpr size_t kMaximumNameLength = 15;

std::string CurrentMachineKey() {
  std::string source;
  const std::array<const char *, 5> variable_names = {
      "COMPUTERNAME", "USERNAME", "USERDOMAIN", "PROCESSOR_IDENTIFIER",
      "NUMBER_OF_PROCESSORS"};
  for (const char *variable_name : variable_names) {
    if (const char *value = std::getenv(variable_name); value && *value) {
      source.append(variable_name);
      source.push_back('=');
      source.append(value);
      source.push_back('|');
    }
  }
  if (source.empty()) {
    source = "unknown-machine";
  }

  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char value : source) {
    hash ^= value;
    hash *= 1099511628211ull;
  }
  return fmt::format("{:016X}", hash);
}

bool IsAllowedNameCharacter(char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
         (value >= '0' && value <= '9') || value == ' ' || value == '_' ||
         value == '-';
}

std::string SanitizeSuggestedName(std::string value) {
  for (char &character : value) {
    if (!IsAllowedNameCharacter(character)) {
      character = '_';
    }
  }
  while (!value.empty() && value.front() == ' ') {
    value.erase(value.begin());
  }
  while (!value.empty() && value.back() == ' ') {
    value.pop_back();
  }
  if (value.size() > kMaximumNameLength) {
    value.resize(kMaximumNameLength);
  }
  return value.empty() ? "Player" : value;
}

std::string DefaultProfileName() {
  if (const char *username = std::getenv("USERNAME"); username && *username) {
    return SanitizeSuggestedName(username);
  }
  if (const char *username = std::getenv("USER"); username && *username) {
    return SanitizeSuggestedName(username);
  }
  return "Player";
}

bool IsValidProfileName(std::string_view name) {
  if (name.empty() || name.size() > kMaximumNameLength || name.front() == ' ' ||
      name.back() == ' ') {
    return false;
  }
  return std::all_of(name.begin(), name.end(), IsAllowedNameCharacter);
}

bool IsValidProfileXuid(uint64_t xuid) {
  return xuid && xuid != kLegacySharedXuid && (xuid & kRejectedXuidMask) == 0;
}

bool ParseHexXuid(std::string_view value, uint64_t *result) {
  if (!result || value.empty()) {
    return false;
  }
  if (value.starts_with("0x") || value.starts_with("0X")) {
    value.remove_prefix(2);
  }
  uint64_t parsed = 0;
  const auto conversion =
      std::from_chars(value.data(), value.data() + value.size(), parsed, 16);
  if (conversion.ec != std::errc() ||
      conversion.ptr != value.data() + value.size()) {
    return false;
  }
  *result = parsed;
  return true;
}

std::unordered_set<uint64_t>
CollectUsedXuids(const std::filesystem::path &saves_root) {
  std::unordered_set<uint64_t> used = {kLegacySharedXuid};
  std::error_code error;
  if (std::filesystem::is_directory(saves_root, error)) {
    for (const auto &entry :
         std::filesystem::directory_iterator(saves_root, error)) {
      if (error) {
        break;
      }
      if (!entry.is_directory(error)) {
        error.clear();
        continue;
      }
      uint64_t xuid = 0;
      const std::string name = entry.path().filename().string();
      if (name.size() == 16 && ParseHexXuid(name, &xuid)) {
        used.insert(xuid);
      }
      error.clear();
    }
  }

  std::ifstream history(saves_root / "used_xuids.txt");
  std::string line;
  while (std::getline(history, line)) {
    uint64_t xuid = 0;
    if (ParseHexXuid(line, &xuid)) {
      used.insert(xuid);
    }
  }
  return used;
}

uint64_t GenerateUniqueXuid(const std::filesystem::path &saves_root) {
  const auto used = CollectUsedXuids(saves_root);
  std::random_device random_device;
  const uint64_t clock_seed = static_cast<uint64_t>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
  std::seed_seq seed = {random_device(),
                        random_device(),
                        random_device(),
                        random_device(),
                        static_cast<uint32_t>(clock_seed),
                        static_cast<uint32_t>(clock_seed >> 32)};
  std::mt19937_64 generator(seed);

  for (size_t attempt = 0; attempt < 1024; ++attempt) {
    const uint64_t candidate = kXuidPrefix | (generator() & kXuidRandomMask);
    if (IsValidProfileXuid(candidate) && !used.contains(candidate)) {
      return candidate;
    }
  }

  // A collision loop is extraordinarily unlikely, but keep a deterministic
  // last resort so profile creation can never fall back to the shared XUID.
  uint64_t candidate = kXuidPrefix | (clock_seed & kXuidRandomMask);
  while (!IsValidProfileXuid(candidate) || used.contains(candidate)) {
    candidate = kXuidPrefix | ((candidate + 1) & kXuidRandomMask);
  }
  return candidate;
}

void RecordUsedXuid(const std::filesystem::path &saves_root, uint64_t xuid) {
  const auto used = CollectUsedXuids(saves_root);
  if (used.contains(xuid)) {
    return;
  }
  std::ofstream history(saves_root / "used_xuids.txt", std::ios::app);
  if (history) {
    history << fmt::format("{:016X}\n", xuid);
  }
}

bool ActivateProfile(const ProfileIdentity &identity) {
  return rex::cvar::SetFlagByName("user_profile_name", identity.name) &&
         rex::cvar::SetFlagByName("user_profile_xuid",
                                  fmt::format("{:016X}", identity.xuid)) &&
         rex::cvar::SetFlagByName("user_profile_machine", identity.machine_key);
}

class ProfileSetupDialog final : public rex::ui::ImGuiDialog {
public:
  ProfileSetupDialog(rex::ui::ImGuiDrawer *drawer,
                     std::filesystem::path saves_root, ProfileIdentity identity,
                     std::function<void(const ProfileIdentity &)> completion)
      : ImGuiDialog(drawer), saves_root_(std::move(saves_root)),
        identity_(std::move(identity)), completion_(std::move(completion)) {
    const std::string name = SanitizeSuggestedName(identity_.name);
    std::copy_n(name.c_str(), std::min(name.size(), name_buffer_.size() - 1),
                name_buffer_.data());
  }

private:
  void OnDraw(ImGuiIO &io) override {
    if (!opened_) {
      ImGui::OpenPopup("Player Profile");
      opened_ = true;
    }

    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoCollapse;
    if (!ImGui::BeginPopupModal("Player Profile", nullptr, flags)) {
      return;
    }

    ImGui::TextUnformatted("Naruto Generations Recomp");
    ImGui::Separator();
    ImGui::TextWrapped(
        "This profile is used for saves and system-link identity. Each PC "
        "must use a different XUID.");
    ImGui::Spacing();

    ImGui::TextUnformatted("Player name");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##profile_name", name_buffer_.data(),
                     name_buffer_.size());

    ImGui::Spacing();
    ImGui::TextUnformatted("XUID");
    ImGui::SameLine();
    ImGui::Text("%016llX", static_cast<unsigned long long>(identity_.xuid));
    ImGui::TextDisabled("Save root: %s", saves_root_.string().c_str());

    if (!status_.empty()) {
      ImGui::Spacing();
      const ImVec4 color = status_is_error_ ? ImVec4(1.0f, 0.35f, 0.30f, 1.0f)
                                            : ImVec4(0.35f, 0.85f, 0.55f, 1.0f);
      ImGui::TextColored(color, "%s", status_.c_str());
    }

    ImGui::Spacing();
    if (ImGui::Button("New XUID", ImVec2(130.0f, 0.0f))) {
      identity_.xuid = GenerateUniqueXuid(saves_root_);
      status_ = "New identity ready. The game will create a fresh save.";
      status_is_error_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Start Game", ImVec2(150.0f, 0.0f))) {
      identity_.name = name_buffer_.data();
      if (!IsValidProfileName(identity_.name)) {
        status_ = "Use 1-15 letters, numbers, spaces, underscores, or hyphens.";
        status_is_error_ = true;
      } else {
        std::string error_message;
        if (!SaveAndActivateProfileIdentity(saves_root_, identity_,
                                            &error_message)) {
          status_ = std::move(error_message);
          status_is_error_ = true;
        } else {
          auto completion = completion_;
          const auto identity = identity_;
          ImGui::CloseCurrentPopup();
          Close();
          completion(identity);
        }
      }
    }

    ImGui::EndPopup();
  }

  std::filesystem::path saves_root_;
  ProfileIdentity identity_;
  std::function<void(const ProfileIdentity &)> completion_;
  std::array<char, kMaximumNameLength + 1> name_buffer_ = {};
  std::string status_;
  bool status_is_error_ = false;
  bool opened_ = false;
};

} // namespace

std::optional<ProfileIdentity>
LoadProfileIdentity(const std::filesystem::path &saves_root) {
  const auto profile_path = saves_root / "profile.toml";
  if (!std::filesystem::is_regular_file(profile_path)) {
    return std::nullopt;
  }

  rex::cvar::LoadConfig(profile_path);
  ProfileIdentity identity;
  identity.name = rex::cvar::GetFlagByName("user_profile_name");
  identity.machine_key = rex::cvar::GetFlagByName("user_profile_machine");

  const std::string xuid_value = rex::cvar::GetFlagByName("user_profile_xuid");
  if (!ParseHexXuid(xuid_value, &identity.xuid) ||
      !IsValidProfileName(identity.name) ||
      !IsValidProfileXuid(identity.xuid)) {
    REXLOG_WARN("Ignoring invalid player profile at {}", profile_path.string());
    return std::nullopt;
  }

  const std::string current_machine = CurrentMachineKey();
  if (identity.machine_key != current_machine) {
    REXLOG_WARN(
        "Player profile belongs to another PC; a new XUID will be created");
    return ProfileIdentity{identity.name, GenerateUniqueXuid(saves_root),
                           current_machine};
  }

  return identity;
}

ProfileIdentity CreateProfileIdentity(const std::filesystem::path &saves_root,
                                      std::string preferred_name) {
  if (preferred_name.empty()) {
    preferred_name = DefaultProfileName();
  }
  return ProfileIdentity{SanitizeSuggestedName(std::move(preferred_name)),
                         GenerateUniqueXuid(saves_root), CurrentMachineKey()};
}

bool SaveAndActivateProfileIdentity(const std::filesystem::path &saves_root,
                                    const ProfileIdentity &identity,
                                    std::string *error_message) {
  if (!IsValidProfileName(identity.name) ||
      !IsValidProfileXuid(identity.xuid) ||
      identity.machine_key != CurrentMachineKey()) {
    if (error_message) {
      *error_message = "The selected profile identity is invalid.";
    }
    return false;
  }

  std::error_code error;
  std::filesystem::create_directories(saves_root, error);
  if (error) {
    if (error_message) {
      *error_message =
          fmt::format("Could not create the saves folder: {}", error.message());
    }
    return false;
  }

  const auto profile_path = saves_root / "profile.toml";
  std::ofstream profile(profile_path, std::ios::trunc);
  if (!profile) {
    if (error_message) {
      *error_message = "Could not write saves/profile.toml.";
    }
    return false;
  }
  profile << "# Naruto Generations Recomp player profile\n";
  profile << "user_profile_name = \"" << identity.name << "\"\n";
  profile << "user_profile_xuid = \"" << fmt::format("{:016X}", identity.xuid)
          << "\"\n";
  profile << "user_profile_machine = \"" << identity.machine_key << "\"\n";
  profile.flush();
  if (!profile) {
    if (error_message) {
      *error_message = "Could not finish writing saves/profile.toml.";
    }
    return false;
  }
  profile.close();

  if (!ActivateProfile(identity)) {
    if (error_message) {
      *error_message = "The runtime rejected the selected profile.";
    }
    return false;
  }

  RecordUsedXuid(saves_root, identity.xuid);
  REXLOG_INFO("Active player profile: name='{}' xuid=0x{:016X} saves='{}'",
              identity.name, identity.xuid, saves_root.string());
  return true;
}

void ShowProfileSetupDialog(
    rex::ui::ImGuiDrawer *drawer, std::filesystem::path saves_root,
    ProfileIdentity identity,
    std::function<void(const ProfileIdentity &)> completion) {
  new ProfileSetupDialog(drawer, std::move(saves_root), std::move(identity),
                         std::move(completion));
}

} // namespace generations
