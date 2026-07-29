#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace generations::launcher::cpk {
namespace fs = std::filesystem;

struct Progress {
  uint64_t files_done = 0;
  uint64_t files_total = 0;
  uint64_t bytes_done = 0;
  uint64_t bytes_total = 0;
  fs::path current_file;
};

using ProgressCallback = std::function<void(const Progress&)>;

struct Result {
  bool ok = false;
  uint64_t files_extracted = 0;
  uint64_t bytes_written = 0;
  std::wstring error;
};

namespace detail {

inline uint16_t ReadBe16(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
inline uint32_t ReadBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
inline uint64_t ReadBe64(const uint8_t* p) {
  return (static_cast<uint64_t>(ReadBe32(p)) << 32) | ReadBe32(p + 4);
}
inline uint32_t ReadLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t ReadLe64(const uint8_t* p) {
  return static_cast<uint64_t>(ReadLe32(p)) |
         (static_cast<uint64_t>(ReadLe32(p + 4)) << 32);
}

inline bool ReadAt(std::ifstream& file, uint64_t offset, void* data, size_t size) {
  if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) return false;
  file.clear();
  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file) return false;
  file.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
  return static_cast<size_t>(file.gcount()) == size;
}

inline uint64_t FileSize(std::ifstream& file) {
  file.clear();
  file.seekg(0, std::ios::end);
  const auto pos = file.tellg();
  return pos < 0 ? 0 : static_cast<uint64_t>(pos);
}

inline void DecryptUtf(std::vector<uint8_t>* bytes) {
  if (!bytes || bytes->size() < 4 || std::memcmp(bytes->data(), "@UTF", 4) == 0) return;
  uint32_t key = 0x0000655f;
  for (auto& value : *bytes) {
    value ^= static_cast<uint8_t>(key & 0xFF);
    key *= 0x00004115;
  }
}

using UtfValue = std::variant<std::monostate, uint64_t, int64_t, double,
                              std::string, std::vector<uint8_t>>;
using UtfRow = std::unordered_map<std::string, UtfValue>;

struct UtfTable {
  std::string name;
  std::vector<UtfRow> rows;
};

struct UtfColumn {
  uint8_t flags = 0;
  std::string name;
  UtfValue constant;
};

inline size_t UtfTypeSize(uint8_t type) {
  switch (type & 0x0F) {
    case 0: case 1: return 1;
    case 2: case 3: return 2;
    case 4: case 5: case 8: case 0xA: return 4;
    case 6: case 7: case 0xB: return 8;
    default: return 0;
  }
}

inline std::optional<std::string> UtfString(const std::vector<uint8_t>& data,
                                            size_t strings_base, uint32_t offset) {
  const size_t start = strings_base + offset;
  if (start >= data.size()) return std::nullopt;
  size_t end = start;
  while (end < data.size() && data[end] != 0) ++end;
  if (end == data.size()) return std::nullopt;
  return std::string(reinterpret_cast<const char*>(data.data() + start), end - start);
}

inline std::optional<UtfValue> ReadUtfValue(const std::vector<uint8_t>& data,
                                            size_t* cursor, uint8_t type,
                                            size_t strings_base, size_t data_base) {
  if (!cursor) return std::nullopt;
  const size_t size = UtfTypeSize(type);
  if (!size || *cursor > data.size() || size > data.size() - *cursor) return std::nullopt;
  const uint8_t* p = data.data() + *cursor;
  *cursor += size;
  switch (type & 0x0F) {
    case 0: return UtfValue{static_cast<uint64_t>(p[0])};
    case 1: return UtfValue{static_cast<int64_t>(static_cast<int8_t>(p[0]))};
    case 2: return UtfValue{static_cast<uint64_t>(ReadBe16(p))};
    case 3: return UtfValue{static_cast<int64_t>(static_cast<int16_t>(ReadBe16(p)))};
    case 4: return UtfValue{static_cast<uint64_t>(ReadBe32(p))};
    case 5: return UtfValue{static_cast<int64_t>(static_cast<int32_t>(ReadBe32(p)))};
    case 6: return UtfValue{ReadBe64(p)};
    case 7: return UtfValue{static_cast<int64_t>(ReadBe64(p))};
    case 8: {
      const uint32_t bits = ReadBe32(p);
      float value = 0.0f;
      std::memcpy(&value, &bits, sizeof(value));
      return UtfValue{static_cast<double>(value)};
    }
    case 0xA: {
      auto text = UtfString(data, strings_base, ReadBe32(p));
      return text ? std::optional<UtfValue>(UtfValue{std::move(*text)}) : std::nullopt;
    }
    case 0xB: {
      const uint32_t offset = ReadBe32(p);
      const uint32_t length = ReadBe32(p + 4);
      const size_t start = data_base + offset;
      if (start > data.size() || length > data.size() - start) return std::nullopt;
      return UtfValue{std::vector<uint8_t>(data.begin() + start, data.begin() + start + length)};
    }
    default: return std::nullopt;
  }
}

inline std::optional<UtfTable> ParseUtf(std::vector<uint8_t> bytes) {
  DecryptUtf(&bytes);
  if (bytes.size() < 0x20 || std::memcmp(bytes.data(), "@UTF", 4) != 0) return std::nullopt;
  const uint32_t table_size = ReadBe32(bytes.data() + 4);
  if (table_size + 8ULL > bytes.size()) return std::nullopt;
  const size_t base = 8;
  const size_t rows_base = base + ReadBe32(bytes.data() + 8);
  const size_t strings_base = base + ReadBe32(bytes.data() + 12);
  const size_t data_base = base + ReadBe32(bytes.data() + 16);
  const uint32_t table_name_offset = ReadBe32(bytes.data() + 20);
  const uint16_t column_count = ReadBe16(bytes.data() + 24);
  const uint16_t row_length = ReadBe16(bytes.data() + 26);
  const uint32_t row_count = ReadBe32(bytes.data() + 28);
  if (rows_base > bytes.size() || strings_base > bytes.size() || data_base > bytes.size())
    return std::nullopt;

  UtfTable result;
  if (auto name = UtfString(bytes, strings_base, table_name_offset)) result.name = *name;
  size_t cursor = 0x20;
  std::vector<UtfColumn> columns;
  columns.reserve(column_count);
  for (uint16_t i = 0; i < column_count; ++i) {
    if (cursor + 5 > bytes.size()) return std::nullopt;
    UtfColumn column;
    column.flags = bytes[cursor++];
    const uint32_t name_offset = ReadBe32(bytes.data() + cursor);
    cursor += 4;
    auto name = UtfString(bytes, strings_base, name_offset);
    if (!name) return std::nullopt;
    column.name = std::move(*name);
    const uint8_t storage = column.flags & 0xF0;
    if (storage == 0x30) {
      auto value = ReadUtfValue(bytes, &cursor, column.flags, strings_base, data_base);
      if (!value) return std::nullopt;
      column.constant = std::move(*value);
    }
    columns.push_back(std::move(column));
  }

  if (row_count && row_length && rows_base + static_cast<uint64_t>(row_count) * row_length > bytes.size())
    return std::nullopt;
  result.rows.reserve(row_count);
  for (uint32_t row_index = 0; row_index < row_count; ++row_index) {
    size_t row_cursor = rows_base + static_cast<size_t>(row_index) * row_length;
    UtfRow row;
    for (const auto& column : columns) {
      const uint8_t storage = column.flags & 0xF0;
      if (storage == 0x10) {
        row.emplace(column.name, UtfValue{static_cast<uint64_t>(0)});
      } else if (storage == 0x30) {
        row.emplace(column.name, column.constant);
      } else if (storage == 0x50) {
        auto value = ReadUtfValue(bytes, &row_cursor, column.flags, strings_base, data_base);
        if (!value) return std::nullopt;
        row.emplace(column.name, std::move(*value));
      }
    }
    result.rows.push_back(std::move(row));
  }
  return result;
}

inline std::optional<uint64_t> AsUnsigned(const UtfRow& row, std::string_view key) {
  const auto found = row.find(std::string(key));
  if (found == row.end()) return std::nullopt;
  if (const auto* value = std::get_if<uint64_t>(&found->second)) return *value;
  if (const auto* value = std::get_if<int64_t>(&found->second))
    return *value >= 0 ? std::optional<uint64_t>(static_cast<uint64_t>(*value)) : std::nullopt;
  return std::nullopt;
}
inline std::optional<std::string> AsString(const UtfRow& row, std::string_view key) {
  const auto found = row.find(std::string(key));
  if (found == row.end()) return std::nullopt;
  if (const auto* value = std::get_if<std::string>(&found->second)) return *value;
  return std::nullopt;
}

inline std::optional<std::vector<uint8_t>> ReadChunkUtf(std::ifstream& file,
                                                        uint64_t offset,
                                                        uint64_t archive_size,
                                                        std::array<char, 4>* tag = nullptr) {
  std::array<uint8_t, 16> header{};
  if (!ReadAt(file, offset, header.data(), header.size())) return std::nullopt;
  if (tag) std::memcpy(tag->data(), header.data(), 4);
  uint64_t packet_size = ReadLe64(header.data() + 8);
  if (!packet_size || packet_size > archive_size || offset + 16 > archive_size ||
      packet_size > archive_size - offset - 16) {
    packet_size = ReadBe64(header.data() + 8);
  }
  if (!packet_size || packet_size > 256ULL * 1024 * 1024 ||
      offset + 16 > archive_size || packet_size > archive_size - offset - 16)
    return std::nullopt;
  std::vector<uint8_t> utf(static_cast<size_t>(packet_size));
  if (!ReadAt(file, offset + 16, utf.data(), utf.size())) return std::nullopt;
  return utf;
}

class ReverseBitReader {
 public:
  ReverseBitReader(const uint8_t* begin, const uint8_t* end)
      : begin_(begin), current_(end) {}
  std::optional<uint32_t> Get(unsigned count) {
    uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) {
      if (!bits_left_) {
        if (current_ <= begin_) return std::nullopt;
        bit_pool_ = *--current_;
        bits_left_ = 8;
      }
      value = (value << 1) | (bit_pool_ & 1);
      bit_pool_ >>= 1;
      --bits_left_;
    }
    return value;
  }
 private:
  const uint8_t* begin_ = nullptr;
  const uint8_t* current_ = nullptr;
  uint8_t bit_pool_ = 0;
  unsigned bits_left_ = 0;
};

inline std::optional<std::vector<uint8_t>> DecompressCriLayla(const std::vector<uint8_t>& input) {
  if (input.size() < 0x110 || std::memcmp(input.data(), "CRILAYLA", 8) != 0)
    return std::nullopt;
  const uint32_t body_size = ReadLe32(input.data() + 8);
  const uint32_t header_offset = ReadLe32(input.data() + 12);
  const uint64_t output_size = static_cast<uint64_t>(body_size) + 0x100;
  const uint64_t header_source = static_cast<uint64_t>(header_offset) + 0x10;
  if (output_size > 2ULL * 1024 * 1024 * 1024 ||
      header_source + 0x100 > input.size() || header_source < 0x10)
    return std::nullopt;
  std::vector<uint8_t> output(static_cast<size_t>(output_size));
  std::memcpy(output.data(), input.data() + header_source, 0x100);
  ReverseBitReader bits(input.data() + 0x10, input.data() + header_source);
  size_t dst = output.size();
  while (dst > 0x100) {
    auto compressed = bits.Get(1);
    if (!compressed) return std::nullopt;
    if (*compressed == 0) {
      auto literal = bits.Get(8);
      if (!literal) return std::nullopt;
      output[--dst] = static_cast<uint8_t>(*literal);
      continue;
    }
    auto distance_bits = bits.Get(13);
    if (!distance_bits) return std::nullopt;
    const size_t distance = static_cast<size_t>(*distance_bits) + 3;
    size_t length = 3;
    constexpr std::array<unsigned, 4> widths = {2, 3, 5, 8};
    for (unsigned width : widths) {
      auto part = bits.Get(width);
      if (!part) return std::nullopt;
      length += *part;
      if (*part != ((1u << width) - 1u)) break;
      if (width == 8) {
        for (;;) {
          auto extra = bits.Get(8);
          if (!extra) return std::nullopt;
          length += *extra;
          if (*extra != 0xFF) break;
        }
      }
    }
    while (length--) {
      if (dst <= 0x100 || dst - 1 + distance >= output.size()) return std::nullopt;
      --dst;
      output[dst] = output[dst + distance];
    }
  }
  return output;
}

inline bool IsSafeRelativePath(const fs::path& path) {
  if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
    return false;
  for (const auto& part : path) {
    if (part == L".." || part == L".") return false;
  }
  return true;
}

inline std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
  if (count > 0) {
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), count);
    return result;
  }
  const int ansi_count = MultiByteToWideChar(932, 0, text.data(),
                                              static_cast<int>(text.size()), nullptr, 0);
  if (ansi_count <= 0) return {};
  std::wstring result(static_cast<size_t>(ansi_count), L'\0');
  MultiByteToWideChar(932, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), ansi_count);
  return result;
}

struct Entry {
  fs::path relative_path;
  uint64_t offset = 0;
  uint64_t stored_size = 0;
  uint64_t extracted_size = 0;
};

inline uint64_t ResolveDataOffset(uint64_t file_offset, uint64_t content_offset,
                                  uint64_t toc_offset, uint64_t archive_size,
                                  uint64_t stored_size) {
  const std::array<uint64_t, 3> bases = {content_offset, toc_offset, 0};
  for (uint64_t base : bases) {
    if (base <= archive_size && file_offset <= archive_size - base) {
      const uint64_t absolute = base + file_offset;
      if (stored_size <= archive_size - absolute) return absolute;
    }
  }
  return std::numeric_limits<uint64_t>::max();
}

}  // namespace detail

inline Result ExtractArchive(const fs::path& archive, const fs::path& output,
                             const ProgressCallback& callback = {}) {
  using namespace detail;
  Result result;
  std::ifstream file(archive, std::ios::binary);
  if (!file) { result.error = L"Could not open the CPK archive"; return result; }
  const uint64_t archive_size = FileSize(file);
  if (archive_size < 0x20) { result.error = L"The CPK archive is too small"; return result; }
  std::array<char, 4> root_tag{};
  auto root_utf_bytes = ReadChunkUtf(file, 0, archive_size, &root_tag);
  if (!root_utf_bytes || std::memcmp(root_tag.data(), "CPK ", 4) != 0) {
    result.error = L"The selected archive does not contain a valid CPK header";
    return result;
  }
  auto root_table = ParseUtf(std::move(*root_utf_bytes));
  if (!root_table || root_table->rows.empty()) {
    result.error = L"The CPK metadata table could not be decoded";
    return result;
  }
  const UtfRow& root = root_table->rows.front();
  const uint64_t toc_offset = AsUnsigned(root, "TocOffset").value_or(0);
  const uint64_t content_offset = AsUnsigned(root, "ContentOffset").value_or(0);
  if (!toc_offset || toc_offset >= archive_size) {
    result.error = L"This CPK uses an unsupported ID-only table (no TOC filenames)";
    return result;
  }
  std::array<char, 4> toc_tag{};
  auto toc_utf_bytes = ReadChunkUtf(file, toc_offset, archive_size, &toc_tag);
  if (!toc_utf_bytes || std::memcmp(toc_tag.data(), "TOC ", 4) != 0) {
    result.error = L"The CPK TOC could not be read";
    return result;
  }
  auto toc_table = ParseUtf(std::move(*toc_utf_bytes));
  if (!toc_table) { result.error = L"The CPK TOC metadata could not be decoded"; return result; }

  std::vector<Entry> entries;
  entries.reserve(toc_table->rows.size());
  uint64_t total_bytes = 0;
  for (const auto& row : toc_table->rows) {
    const auto file_name = AsString(row, "FileName");
    if (!file_name || file_name->empty()) continue;
    const std::string directory = AsString(row, "DirName").value_or("");
    fs::path relative;
    if (!directory.empty() && directory != "<NULL>") relative /= Utf8ToWide(directory);
    relative /= Utf8ToWide(*file_name);
    relative = relative.lexically_normal();
    if (!IsSafeRelativePath(relative)) continue;
    const uint64_t stored = AsUnsigned(row, "FileSize").value_or(0);
    const uint64_t extracted = AsUnsigned(row, "ExtractSize").value_or(stored);
    const uint64_t file_offset = AsUnsigned(row, "FileOffset").value_or(0);
    const uint64_t absolute = ResolveDataOffset(file_offset, content_offset, toc_offset,
                                                archive_size, stored);
    if (!stored || absolute == std::numeric_limits<uint64_t>::max()) continue;
    entries.push_back({std::move(relative), absolute, stored, extracted});
    total_bytes += extracted;
  }
  if (entries.empty()) { result.error = L"The CPK did not contain extractable named files"; return result; }

  std::error_code ec;
  fs::create_directories(output, ec);
  if (ec) { result.error = L"Could not create the extraction folder"; return result; }
  Progress progress;
  progress.files_total = entries.size();
  progress.bytes_total = total_bytes;
  for (const auto& entry : entries) {
    progress.current_file = entry.relative_path;
    if (callback) callback(progress);
    if (entry.stored_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      result.error = L"A CPK entry is too large for this build"; return result;
    }
    std::vector<uint8_t> stored(static_cast<size_t>(entry.stored_size));
    if (!ReadAt(file, entry.offset, stored.data(), stored.size())) {
      result.error = L"Failed reading " + entry.relative_path.wstring(); return result;
    }
    std::vector<uint8_t> unpacked;
    const std::vector<uint8_t>* bytes = &stored;
    if (stored.size() >= 8 && std::memcmp(stored.data(), "CRILAYLA", 8) == 0) {
      auto decoded = DecompressCriLayla(stored);
      if (!decoded) { result.error = L"CRILAYLA decompression failed for " + entry.relative_path.wstring(); return result; }
      unpacked = std::move(*decoded);
      bytes = &unpacked;
    }
    const fs::path target = output / entry.relative_path;
    fs::create_directories(target.parent_path(), ec);
    if (ec) { result.error = L"Could not create a folder for " + entry.relative_path.wstring(); return result; }
    const fs::path temporary = target.wstring() + L".extracting";
    {
      std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
      if (!out) { result.error = L"Could not create " + entry.relative_path.wstring(); return result; }
      out.write(reinterpret_cast<const char*>(bytes->data()),
                static_cast<std::streamsize>(bytes->size()));
      if (!out) { result.error = L"Failed writing " + entry.relative_path.wstring(); return result; }
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      fs::remove(temporary, ec);
      result.error = L"Could not finalize " + entry.relative_path.wstring(); return result;
    }
    ++progress.files_done;
    progress.bytes_done += bytes->size();
    result.files_extracted = progress.files_done;
    result.bytes_written = progress.bytes_done;
    if (callback) callback(progress);
  }
  result.ok = true;
  return result;
}

}  // namespace generations::launcher::cpk
