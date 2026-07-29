#include <rex/kernel/xam/achievements.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xam/user_profile.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/windowed_app_context.h>

namespace rex::kernel::xam {
namespace {

using AchievementKey = std::tuple<uint64_t, uint32_t>;

constexpr uint64_t kWindowsEpochOffsetSeconds = 11644473600ULL;
constexpr uint64_t kFileTimeTicksPerSecond = 10000000ULL;

std::mutex achievement_mutex;
std::map<AchievementKey, std::map<uint32_t, AchievementUnlockRecord>>
    achievement_cache;
std::map<AchievementKey, bool> achievement_cache_loaded;

uint64_t CurrentFileTime() {
  const auto unix_ticks = std::chrono::system_clock::now().time_since_epoch();
  const uint64_t ticks = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(unix_ticks).count() /
      100);
  return ticks + kWindowsEpochOffsetSeconds * kFileTimeTicksPerSecond;
}

std::filesystem::path AchievementPath(system::KernelState* kernel_state,
                                      uint64_t xuid, uint32_t title_id) {
  auto root = kernel_state->emulator()->user_data_root();
  if (!root.empty()) {
    root = root.parent_path();
  }
  return root / "achievements" / fmt::format("{:016X}", xuid) /
         fmt::format("{:08X}.toml", title_id);
}

void LoadAchievementsLocked(system::KernelState* kernel_state, uint64_t xuid,
                            uint32_t title_id) {
  const AchievementKey key{xuid, title_id};
  if (achievement_cache_loaded[key]) {
    return;
  }
  achievement_cache_loaded[key] = true;

  const auto path = AchievementPath(kernel_state, xuid, title_id);
  if (!std::filesystem::is_regular_file(path)) {
    return;
  }
  try {
    const toml::table document = toml::parse_file(path.string());
    const toml::array* unlocks = document["unlocks"].as_array();
    if (!unlocks) {
      return;
    }
    for (const auto& node : *unlocks) {
      const toml::table* table = node.as_table();
      if (!table) {
        continue;
      }
      const auto id = (*table)["id"].value<int64_t>();
      const auto unlocked_at = (*table)["unlocked_at_filetime"].value<int64_t>();
      if (!id || !unlocked_at || *id < 0 || *id > UINT32_MAX ||
          *unlocked_at <= 0) {
        continue;
      }
      const uint32_t achievement_id = static_cast<uint32_t>(*id);
      achievement_cache[key][achievement_id] =
          AchievementUnlockRecord{achievement_id,
                                  static_cast<uint64_t>(*unlocked_at)};
    }
  } catch (const std::exception& error) {
    REXKRNL_WARN("Achievement store '{}' could not be parsed: {}",
                 path.string(), error.what());
  }
}

bool SaveAchievementsLocked(system::KernelState* kernel_state, uint64_t xuid,
                            uint32_t title_id) {
  const auto path = AchievementPath(kernel_state, xuid, title_id);
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    REXKRNL_ERROR("Could not create achievement directory '{}': {}",
                  path.parent_path().string(), error.message());
    return false;
  }

  toml::table document;
  document.insert("version", 1);
  document.insert("xuid", fmt::format("{:016X}", xuid));
  document.insert("title_id", fmt::format("{:08X}", title_id));
  toml::array unlocks;
  for (const auto& [id, record] :
       achievement_cache[AchievementKey{xuid, title_id}]) {
    toml::table unlock;
    unlock.insert("id", static_cast<int64_t>(id));
    unlock.insert("unlocked_at_filetime",
                  static_cast<int64_t>(record.unlocked_at_filetime));
    unlocks.push_back(std::move(unlock));
  }
  document.insert("unlocks", std::move(unlocks));

  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
      return false;
    }
    output << "# Local Xbox 360 achievement state\n" << document << '\n';
    if (!output) {
      return false;
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
    REXKRNL_ERROR("Could not commit achievement store '{}': {}", path.string(),
                  error.message());
    return false;
  }
  return true;
}

struct AchievementMetadata {
  std::string label;
  uint32_t gamerscore = 0;
};

std::optional<AchievementMetadata> FindMetadata(system::KernelState* kernel_state,
                                                uint32_t achievement_id) {
  const auto database = kernel_state->title_xdbf();
  if (!database.is_valid()) {
    return std::nullopt;
  }
  const auto language =
      database.GetExistingLanguage(system::XLanguage::kEnglish);
  for (const auto& entry : database.GetAchievements()) {
    if (static_cast<uint16_t>(entry.id) != achievement_id) {
      continue;
    }
    AchievementMetadata metadata;
    metadata.label =
        database.GetStringTableEntry(language, entry.label_id);
    metadata.gamerscore = entry.gamerscore;
    return metadata;
  }
  return std::nullopt;
}

class AchievementToast final : public ui::ImGuiDialog {
 public:
  AchievementToast(ui::ImGuiDrawer* drawer, std::string label,
                   uint32_t gamerscore)
      : ImGuiDialog(drawer), label_(std::move(label)),
        gamerscore_(gamerscore), shown_at_(std::chrono::steady_clock::now()) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (std::chrono::steady_clock::now() - shown_at_ >
        std::chrono::seconds(5)) {
      Close();
      return;
    }

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f,
                                  io.DisplaySize.y - 34.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(430.0f, 74.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##achievement_toast", nullptr, flags)) {
      ImDrawList* draw = ImGui::GetWindowDrawList();
      const ImVec2 origin = ImGui::GetWindowPos();
      draw->AddCircleFilled(ImVec2(origin.x + 38.0f, origin.y + 37.0f), 21.0f,
                            IM_COL32(78, 166, 58, 255));
      draw->AddCircle(ImVec2(origin.x + 38.0f, origin.y + 37.0f), 12.0f,
                      IM_COL32(245, 248, 245, 255), 0, 3.0f);
      ImGui::SetCursorPos(ImVec2(74.0f, 12.0f));
      ImGui::TextUnformatted("Achievement unlocked");
      ImGui::SetCursorPos(ImVec2(74.0f, 38.0f));
      ImGui::Text("%uG  %s", gamerscore_, label_.c_str());
    }
    ImGui::End();
  }

 private:
  std::string label_;
  uint32_t gamerscore_ = 0;
  std::chrono::steady_clock::time_point shown_at_;
};

void ShowAchievementToast(system::KernelState* kernel_state,
                          const AchievementMetadata& metadata) {
  if (rex::cvar::GetFlagByName("generations_achievement_toasts") == "false") {
    return;
  }
  Runtime* runtime = kernel_state->emulator();
  auto* context = runtime->app_context();
  auto* drawer = runtime->imgui_drawer();
  if (!context || !drawer) {
    return;
  }
  const std::string label = metadata.label.empty() ? "Achievement" : metadata.label;
  context->CallInUIThreadDeferred(
      [drawer, label, gamerscore = metadata.gamerscore]() {
        new AchievementToast(drawer, label, gamerscore);
      });
}

}  // namespace

std::optional<AchievementUnlockRecord> GetAchievementUnlock(
    system::KernelState* kernel_state, uint64_t xuid, uint32_t title_id,
    uint32_t achievement_id) {
  if (!kernel_state || !xuid || !title_id) {
    return std::nullopt;
  }
  std::scoped_lock lock(achievement_mutex);
  LoadAchievementsLocked(kernel_state, xuid, title_id);
  const auto& records = achievement_cache[AchievementKey{xuid, title_id}];
  const auto found = records.find(achievement_id);
  return found == records.end()
             ? std::nullopt
             : std::optional<AchievementUnlockRecord>(found->second);
}

bool UnlockAchievement(system::KernelState* kernel_state, uint32_t user_index,
                       uint32_t achievement_id) {
  if (!kernel_state || user_index != 0) {
    return false;
  }
  const auto metadata = FindMetadata(kernel_state, achievement_id);
  if (!metadata) {
    REXKRNL_WARN("Ignoring unknown achievement ID {} for title {:08X}",
                 achievement_id, kernel_state->title_id());
    return false;
  }

  const uint64_t xuid = kernel_state->user_profile()->xuid();
  const uint32_t title_id = kernel_state->title_id();
  {
    std::scoped_lock lock(achievement_mutex);
    LoadAchievementsLocked(kernel_state, xuid, title_id);
    auto& records = achievement_cache[AchievementKey{xuid, title_id}];
    if (records.contains(achievement_id)) {
      return false;
    }
    records.emplace(achievement_id,
                    AchievementUnlockRecord{achievement_id, CurrentFileTime()});
    if (!SaveAchievementsLocked(kernel_state, xuid, title_id)) {
      records.erase(achievement_id);
      return false;
    }
  }

  REXKRNL_INFO("Achievement unlocked: id={} score={} name='{}'",
               achievement_id, metadata->gamerscore, metadata->label);
  ShowAchievementToast(kernel_state, *metadata);
  return true;
}

}  // namespace rex::kernel::xam
