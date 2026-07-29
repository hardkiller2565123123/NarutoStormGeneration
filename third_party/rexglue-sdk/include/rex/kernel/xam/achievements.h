#pragma once

#include <cstdint>
#include <optional>

namespace rex::system {
class KernelState;
}

namespace rex::kernel::xam {

struct AchievementUnlockRecord {
  uint32_t id = 0;
  uint64_t unlocked_at_filetime = 0;
};

std::optional<AchievementUnlockRecord> GetAchievementUnlock(
    system::KernelState* kernel_state, uint64_t xuid, uint32_t title_id,
    uint32_t achievement_id);

// Returns true only when this call changes the achievement from locked to
// unlocked. Invalid IDs and already-unlocked achievements return false.
bool UnlockAchievement(system::KernelState* kernel_state, uint32_t user_index,
                       uint32_t achievement_id);

}  // namespace rex::kernel::xam
