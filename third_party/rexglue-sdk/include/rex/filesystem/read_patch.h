/**
 ******************************************************************************
 * Runtime read-only file patch support.
 ******************************************************************************
 *
 * Allows a recomp project to replace verified byte ranges as a file is read.
 * The underlying game archive is never written or copied.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace rex::filesystem {

struct ReadPatchRange {
  uint64_t offset = 0;
  std::vector<uint8_t> expected;
  std::vector<uint8_t> replacement;
};

class ReadPatchSet {
 public:
  ReadPatchSet(std::string label, std::vector<ReadPatchRange> ranges);

  const std::string& label() const { return label_; }
  const std::vector<ReadPatchRange>& ranges() const { return ranges_; }

 private:
  friend bool ApplyReadPatchSet(const ReadPatchSet& patch_set, uint64_t file_offset,
                                std::span<uint8_t> buffer);

  std::string label_;
  std::vector<ReadPatchRange> ranges_;
  mutable std::atomic<bool> applied_logged_{false};
  mutable std::atomic<bool> mismatch_logged_{false};
};

// Registers a verified, in-memory patch set for one host file. Registration
// replaces any earlier patch set for the same path and never mutates the file.
bool RegisterReadPatchSet(const std::filesystem::path& host_path, std::string label,
                          std::vector<ReadPatchRange> ranges);
void UnregisterReadPatchSet(const std::filesystem::path& host_path);

// HostPathFile resolves this once when it is opened, avoiding a registry lookup
// for every guest file read.
std::shared_ptr<const ReadPatchSet> FindReadPatchSet(const std::filesystem::path& host_path);

// Applies overlapping verified ranges to a buffer read from the original file.
// A mismatched expected byte sequence leaves that range untouched.
bool ApplyReadPatchSet(const ReadPatchSet& patch_set, uint64_t file_offset,
                       std::span<uint8_t> buffer);

}  // namespace rex::filesystem
