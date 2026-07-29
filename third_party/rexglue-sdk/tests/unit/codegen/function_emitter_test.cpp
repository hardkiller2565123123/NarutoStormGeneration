/**
 * @file        tests/unit/codegen/function_emitter_test.cpp
 * @brief       Regression tests for graph-backed branch emission
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <cstdint>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/config.h>
#include <rex/codegen/function_graph.h>
#include <rex/codegen/test_support.h>

namespace rex::codegen {
namespace {

constexpr uint32_t kBase = 0x82000000;
constexpr uint32_t kTarget = kBase + 8;

std::string EmitBranchWithoutCallEdge(const std::array<uint8_t, 12>& code) {
  TestModule module;
  module.Load(kBase, code.data(), code.size());

  auto binary = BinaryView::fromModule(module);
  FunctionGraph graph;

  auto* source = graph.addFunction(kBase, 4, FunctionAuthority::CONFIG, true);
  auto* target = graph.addFunction(kTarget, 4, FunctionAuthority::CONFIG, true);
  REQUIRE(source != nullptr);
  REQUIRE(target != nullptr);

  source->discover({{kBase, 4}}, {}, {});
  target->discover({{kTarget, 4}}, {}, {});
  source->seal();
  target->seal();

  RecompilerConfig config;
  return source->emitCpp({binary, config, graph, 0, nullptr});
}

}  // namespace

TEST_CASE("Registered tail-call target does not require a duplicate CallEdge",
          "[codegen][function_emitter]") {
  // b 0x82000008; blr; blr
  constexpr std::array<uint8_t, 12> code = {
      0x48, 0x00, 0x00, 0x08, 0x4E, 0x80, 0x00, 0x20, 0x4E, 0x80, 0x00, 0x20,
  };

  const auto output = EmitBranchWithoutCallEdge(code);
  CHECK(output.find("sub_82000008(ctx, base);") != std::string::npos);
  CHECK(output.find("REX_FATAL") == std::string::npos);
}

TEST_CASE("Registered conditional tail target does not require a duplicate CallEdge",
          "[codegen][function_emitter]") {
  // beq 0x82000008; blr; blr
  constexpr std::array<uint8_t, 12> code = {
      0x41, 0x82, 0x00, 0x08, 0x4E, 0x80, 0x00, 0x20, 0x4E, 0x80, 0x00, 0x20,
  };

  const auto output = EmitBranchWithoutCallEdge(code);
  CHECK(output.find("sub_82000008(ctx, base);") != std::string::npos);
  CHECK(output.find("REX_FATAL") == std::string::npos);
}

}  // namespace rex::codegen
