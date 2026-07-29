/**
 * @file        tests/unit/codegen/function_scanner_test.cpp
 * @brief       Regression tests for PowerPC basic-block discovery
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/function_scanner.h>
#include <rex/codegen/test_support.h>

#include "codegen/decoded_binary.h"

namespace rex::codegen {
namespace {

constexpr uint32_t kBase = 0x82000000;

template <typename BlockType>
bool HasBlock(const std::vector<BlockType>& blocks, uint32_t base, uint32_t end) {
  return std::any_of(blocks.begin(), blocks.end(), [=](const BlockType& block) {
    if constexpr (requires { block.end(); }) {
      return block.base == base && block.end() == end;
    } else {
      return block.base == base && block.end == end;
    }
  });
}

struct DiscoveryResults {
  BlockDiscoveryResult active;
  FunctionBlocks legacy;
};

template <size_t N>
DiscoveryResults Discover(const std::array<uint8_t, N>& code) {
  TestModule module;
  module.Load(kBase, code.data(), code.size());

  auto binary = BinaryView::fromModule(module);
  DecodedBinary decoded(binary);
  decoded.decode();

  const auto* region = decoded.regionContaining(kBase);
  REQUIRE(region != nullptr);

  std::unordered_set<uint32_t> knownFunctions;
  auto active = discoverBlocks(decoded, kBase, *region, knownFunctions, code.size());

  FunctionScanner scanner(binary);
  auto legacy = scanner.discover_blocks(kBase, code.size());
  return {std::move(active), std::move(legacy)};
}

}  // namespace

TEST_CASE("Conditional bcctr discovers its fallthrough", "[codegen][function_scanner]") {
  // bnectr; li r3, 0; blr
  constexpr std::array<uint8_t, 12> code = {
      0x4C, 0x82, 0x04, 0x20, 0x38, 0x60, 0x00, 0x00, 0x4E, 0x80, 0x00, 0x20,
  };

  auto result = Discover(code);

  CHECK(HasBlock(result.active.blocks, kBase, kBase + 4));
  CHECK(HasBlock(result.active.blocks, kBase + 4, kBase + 12));
  CHECK(result.active.labels.contains(kBase + 4));
  CHECK(result.active.instructions.size() == 3);

  CHECK(HasBlock(result.legacy.blocks, kBase, kBase + 4));
  CHECK(HasBlock(result.legacy.blocks, kBase + 4, kBase + 12));
}

TEST_CASE("Unconditional bctr remains a terminator", "[codegen][function_scanner]") {
  // bctr; li r3, 0; blr
  constexpr std::array<uint8_t, 12> code = {
      0x4E, 0x80, 0x04, 0x20, 0x38, 0x60, 0x00, 0x00, 0x4E, 0x80, 0x00, 0x20,
  };

  auto result = Discover(code);

  CHECK(HasBlock(result.active.blocks, kBase, kBase + 4));
  CHECK_FALSE(HasBlock(result.active.blocks, kBase + 4, kBase + 12));
  CHECK(result.active.instructions.size() == 1);

  CHECK(HasBlock(result.legacy.blocks, kBase, kBase + 4));
  CHECK_FALSE(HasBlock(result.legacy.blocks, kBase + 4, kBase + 12));
}

}  // namespace rex::codegen
