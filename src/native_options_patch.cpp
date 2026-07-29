#include "native_options_patch.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/filesystem/read_patch.h>
#include <rex/logging.h>

#include "generated_native_options_patch.h"

namespace generations {
namespace {

constexpr std::array<uint8_t, 8> kPatchMagic = {'G', 'R', 'O', 'P', 'T', '0', '0', '1'};

bool ReadU32(std::span<const uint8_t> data, size_t* cursor, uint32_t* value) {
  if (*cursor > data.size() || data.size() - *cursor < 4) {
    return false;
  }
  const auto offset = *cursor;
  *value = uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8) |
           (uint32_t(data[offset + 2]) << 16) | (uint32_t(data[offset + 3]) << 24);
  *cursor += 4;
  return true;
}

bool ReadU64(std::span<const uint8_t> data, size_t* cursor, uint64_t* value) {
  if (*cursor > data.size() || data.size() - *cursor < 8) {
    return false;
  }
  const auto offset = *cursor;
  uint64_t result = 0;
  for (size_t index = 0; index < 8; ++index) {
    result |= uint64_t(data[offset + index]) << (index * 8);
  }
  *cursor += 8;
  *value = result;
  return true;
}

bool ReadBytes(std::span<const uint8_t> data, size_t* cursor, size_t count,
               std::vector<uint8_t>* output) {
  if (*cursor > data.size() || count > data.size() - *cursor) {
    return false;
  }
  output->assign(data.begin() + *cursor, data.begin() + *cursor + count);
  *cursor += count;
  return true;
}

std::filesystem::path FindDataArchive(const std::filesystem::path& game_data_root) {
  const auto preferred = game_data_root / "data" / "cpk" / "EU" / "data.cpk";
  if (std::filesystem::is_regular_file(preferred)) {
    return preferred;
  }

  const auto cpk_root = game_data_root / "data" / "cpk";
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator iterator(
           cpk_root, std::filesystem::directory_options::skip_permission_denied, error),
       end;
       iterator != end && !error; iterator.increment(error)) {
    if (iterator->is_regular_file(error) && iterator->path().filename() == "data.cpk") {
      return iterator->path();
    }
  }
  return {};
}

}  // namespace

bool InstallNativeOptionsPatch(const std::filesystem::path& game_data_root) {
  constexpr auto& patch = native_options_patch_data::kPatch;
  const std::span<const uint8_t> bytes(patch.data(), patch.size());
  if (bytes.size() < kPatchMagic.size() + 4 ||
      !std::equal(kPatchMagic.begin(), kPatchMagic.end(), bytes.begin())) {
    REXLOG_ERROR("Native Options patch data is invalid");
    return false;
  }

  size_t cursor = kPatchMagic.size();
  uint32_t range_count = 0;
  if (!ReadU32(bytes, &cursor, &range_count) || range_count == 0 || range_count > 128) {
    REXLOG_ERROR("Native Options patch has an invalid range count");
    return false;
  }

  std::vector<rex::filesystem::ReadPatchRange> ranges;
  ranges.reserve(range_count);
  for (uint32_t index = 0; index < range_count; ++index) {
    uint64_t offset = 0;
    uint32_t size = 0;
    rex::filesystem::ReadPatchRange range;
    if (!ReadU64(bytes, &cursor, &offset) || !ReadU32(bytes, &cursor, &size) || size == 0 ||
        !ReadBytes(bytes, &cursor, size, &range.expected) ||
        !ReadBytes(bytes, &cursor, size, &range.replacement)) {
      REXLOG_ERROR("Native Options patch range {} is truncated", index);
      return false;
    }
    range.offset = offset;
    ranges.emplace_back(std::move(range));
  }
  if (cursor != bytes.size()) {
    REXLOG_ERROR("Native Options patch has trailing data");
    return false;
  }

  const auto data_archive = FindDataArchive(game_data_root);
  if (data_archive.empty()) {
    REXLOG_WARN("Native Options patch skipped: data.cpk was not found");
    return false;
  }

  return rex::filesystem::RegisterReadPatchSet(data_archive, "Generations native Options",
                                                std::move(ranges));
}

}  // namespace generations
