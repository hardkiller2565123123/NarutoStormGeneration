/**
 ******************************************************************************
 * Runtime read-only file patch support.
 ******************************************************************************
 */

#include <rex/filesystem/read_patch.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <limits>
#include <mutex>
#include <unordered_map>

#include <rex/logging.h>

namespace rex::filesystem {
namespace {

std::mutex read_patch_mutex;
std::unordered_map<std::string, std::shared_ptr<const ReadPatchSet>> read_patch_sets;
std::atomic_uint64_t read_patch_generation{1};

std::string NormalizePatchPath(const std::filesystem::path& path) {
  std::error_code error;
  auto normalized = std::filesystem::weakly_canonical(path, error);
  if (error) {
    normalized = std::filesystem::absolute(path, error);
  }
  if (error) {
    normalized = path;
  }

  auto key = normalized.lexically_normal().generic_string();
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return key;
}

bool IsRangeValid(const ReadPatchRange& range) {
  if (range.expected.empty() || range.expected.size() != range.replacement.size()) {
    return false;
  }
  return range.offset <= std::numeric_limits<uint64_t>::max() - range.expected.size();
}

}  // namespace

ReadPatchSet::ReadPatchSet(std::string label, std::vector<ReadPatchRange> ranges)
    : label_(std::move(label)), ranges_(std::move(ranges)) {}

bool RegisterReadPatchSet(const std::filesystem::path& host_path, std::string label,
                          std::vector<ReadPatchRange> ranges) {
  if (host_path.empty() || label.empty() || ranges.empty()) {
    REXLOG_ERROR("Read patch registration requires a path, label, and at least one range");
    return false;
  }

  std::sort(ranges.begin(), ranges.end(), [](const ReadPatchRange& left,
                                              const ReadPatchRange& right) {
    return left.offset < right.offset;
  });
  uint64_t previous_end = 0;
  for (const auto& range : ranges) {
    if (!IsRangeValid(range) || range.offset < previous_end) {
      REXLOG_ERROR("Read patch '{}' has invalid or overlapping ranges", label);
      return false;
    }
    previous_end = range.offset + range.expected.size();
  }

  const auto key = NormalizePatchPath(host_path);
  auto patch_set = std::make_shared<ReadPatchSet>(std::move(label), std::move(ranges));
  {
    std::scoped_lock lock(read_patch_mutex);
    read_patch_sets.insert_or_assign(key, std::move(patch_set));
    read_patch_generation.fetch_add(1, std::memory_order_release);
  }
  REXLOG_INFO("Registered runtime read patch for '{}'", host_path.string());
  return true;
}

void UnregisterReadPatchSet(const std::filesystem::path& host_path) {
  const auto key = NormalizePatchPath(host_path);
  std::scoped_lock lock(read_patch_mutex);
  if (read_patch_sets.erase(key) != 0) {
    read_patch_generation.fetch_add(1, std::memory_order_release);
  }
}

std::shared_ptr<const ReadPatchSet> FindReadPatchSet(const std::filesystem::path& host_path) {
  // File reads call this for every chunk. Canonicalizing a Windows path and
  // taking the global registry mutex on every small data.cpk read caused a
  // severe title/menu stall on systems with slower storage or real-time AV.
  // The patch registry is populated during startup, so keep a per-thread hot
  // entry for the overwhelmingly common repeated-read case.
  struct ThreadPatchCache {
    std::filesystem::path path;
    std::shared_ptr<const ReadPatchSet> patch_set;
    uint64_t generation = 0;
    bool initialized = false;
  };
  thread_local ThreadPatchCache cache;

  const uint64_t generation = read_patch_generation.load(std::memory_order_acquire);
  if (cache.initialized && cache.generation == generation && cache.path == host_path) {
    return cache.patch_set;
  }

  const auto key = NormalizePatchPath(host_path);
  std::shared_ptr<const ReadPatchSet> result;
  {
    std::scoped_lock lock(read_patch_mutex);
    const auto it = read_patch_sets.find(key);
    if (it != read_patch_sets.end()) {
      result = it->second;
    }
  }

  cache.path = host_path;
  cache.patch_set = result;
  cache.generation = generation;
  cache.initialized = true;
  return result;
}

bool ApplyReadPatchSet(const ReadPatchSet& patch_set, uint64_t file_offset,
                       std::span<uint8_t> buffer) {
  if (buffer.empty() || file_offset > std::numeric_limits<uint64_t>::max() - buffer.size()) {
    return false;
  }

  const uint64_t read_end = file_offset + buffer.size();
  bool applied = false;

  // data.cpk is read in many small chunks during stage and character loading.
  // The ranges are sorted at registration, so don't scan every patch range on
  // every archive read. Start at the first range whose end may overlap the
  // requested bytes, then stop as soon as offsets pass the read window.
  auto range_it = std::lower_bound(
      patch_set.ranges_.begin(), patch_set.ranges_.end(), file_offset,
      [](const ReadPatchRange& range, uint64_t offset) {
        return range.offset + range.expected.size() <= offset;
      });
  for (; range_it != patch_set.ranges_.end() && range_it->offset < read_end;
       ++range_it) {
    const auto& range = *range_it;
    const uint64_t range_end = range.offset + range.expected.size();
    const uint64_t overlap_begin = std::max(file_offset, range.offset);
    const uint64_t overlap_end = std::min(read_end, range_end);
    if (overlap_begin >= overlap_end) {
      continue;
    }

    const size_t range_index = static_cast<size_t>(overlap_begin - range.offset);
    const size_t buffer_index = static_cast<size_t>(overlap_begin - file_offset);
    const size_t count = static_cast<size_t>(overlap_end - overlap_begin);
    auto expected_begin = range.expected.begin() + range_index;
    if (!std::equal(expected_begin, expected_begin + count, buffer.begin() + buffer_index)) {
      if (!patch_set.mismatch_logged_.exchange(true)) {
        REXLOG_WARN("Runtime read patch '{}' did not match its expected source bytes; skipping it",
                    patch_set.label_);
      }
      continue;
    }

    std::copy_n(range.replacement.begin() + range_index, count, buffer.begin() + buffer_index);
    applied = true;
  }

  if (applied && !patch_set.applied_logged_.exchange(true)) {
    REXLOG_INFO("Applied runtime read patch '{}'", patch_set.label_);
  }
  return applied;
}

}  // namespace rex::filesystem
