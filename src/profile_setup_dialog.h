#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace rex::ui {
class ImGuiDrawer;
}

namespace generations {

struct ProfileIdentity {
  std::string name;
  uint64_t xuid = 0;
  std::string machine_key;
};

std::optional<ProfileIdentity>
LoadProfileIdentity(const std::filesystem::path &saves_root);
ProfileIdentity CreateProfileIdentity(const std::filesystem::path &saves_root,
                                      std::string preferred_name = {});
bool SaveAndActivateProfileIdentity(const std::filesystem::path &saves_root,
                                    const ProfileIdentity &identity,
                                    std::string *error_message = nullptr);

void ShowProfileSetupDialog(
    rex::ui::ImGuiDrawer *drawer, std::filesystem::path saves_root,
    ProfileIdentity identity,
    std::function<void(const ProfileIdentity &)> completion);

} // namespace generations
