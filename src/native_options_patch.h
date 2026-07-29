#pragma once

#include <filesystem>

namespace generations {

// Installs the native Options archive override for the currently mounted game
// data. The override is applied only to verified bytes read from data.cpk.
bool InstallNativeOptionsPatch(const std::filesystem::path& game_data_root);

}  // namespace generations
