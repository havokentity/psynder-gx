// SPDX-License-Identifier: MIT
// Psynder — script-lane component-id determinism (lockstep pillar).
//
// Script-defined component ids must be a STABLE function of the component NAME,
// not of registration / call order, so they are bit-reproducible across runs,
// builds, and peers (required if script components ever ride the snapshot path).
// The fix replaced the old `kScriptIdBase + count` call-order counter with a
// name hash (scene::fnv1a32 | top-bit). These tests pin that property through
// the public Lua API: `world:component(name)` returns the minted id.

#include "script/Script.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace {

// Mint/look up a script component id via the public world API and parse the
// integer the REPL prints back.
std::uint64_t component_id(psynder::script::Vm& vm, const char* name) {
    std::string out;
    const std::string expr = std::string("world:component('") + name + "')";
    REQUIRE(vm.execute_repl(expr, out));
    return std::stoull(out);
}

}  // namespace

TEST_CASE("script: component ids are a stable name hash, order-independent",
          "[script][determinism]") {
    auto& vm = psynder::script::Vm::Get();

    // Phase 1: a fresh VM registers Alpha THEN Bravo.
    REQUIRE(vm.start());
    const std::uint64_t a1 = component_id(vm, "Alpha");
    const std::uint64_t b1 = component_id(vm, "Bravo");
    vm.shutdown();

    // Phase 2: a fresh VM registers them in the OPPOSITE order.
    REQUIRE(vm.start());
    const std::uint64_t b2 = component_id(vm, "Bravo");
    const std::uint64_t a2 = component_id(vm, "Alpha");
    vm.shutdown();

    // Same name -> same id regardless of registration order. Under the old
    // call-order scheme Alpha would be base+1 in phase 1 but base+2 in phase 2,
    // so this would FAIL — it is the regression guard.
    REQUIRE(a1 == a2);
    REQUIRE(b1 == b2);
    REQUIRE(a1 != b1);

    // Script-space invariant: top bit set, never the invalid 0 id.
    REQUIRE((a1 & 0x80000000ull) != 0u);
    REQUIRE((b1 & 0x80000000ull) != 0u);
    REQUIRE(a1 != 0u);
}

TEST_CASE("script: component id lookup is idempotent within a VM",
          "[script][determinism]") {
    auto& vm = psynder::script::Vm::Get();
    REQUIRE(vm.start());
    const std::uint64_t first = component_id(vm, "Position");
    REQUIRE(component_id(vm, "Position") == first);
    REQUIRE(component_id(vm, "Position") == first);
    // A different name yields a different id.
    REQUIRE(component_id(vm, "Velocity") != first);
    vm.shutdown();
}
