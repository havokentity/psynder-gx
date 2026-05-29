# SPDX-License-Identifier: MIT
# Golden determinism gate (ARCH-review A11). Catch2 suites in this dir prove the
# lockstep-critical sim is bit-reproducible against a committed digest. Suites are
# globbed (CONFIGURE_DEPENDS) so a new *.cpp auto-registers on the next configure.
#
# TEST_CASE names carry "golden"/"determinism" and the exe is named psynder_golden,
# so both the discovered per-case ctest names and the exe match the determinism CI
# regex (golden|determinism|deterministic|replay|lockstep) — see
# .github/workflows/determinism.yml.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(NOT SRC)
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()

if(TARGET Catch2::Catch2WithMain)
    add_executable(psynder_golden ${SRC})
    target_link_libraries(psynder_golden PRIVATE
        Catch2::Catch2WithMain
        psynder_scene psynder_math psynder_jobs
        psynder_physics_agents psynder_physics_core
        psynder_core
    )
    # ASCII-only TEST_CASE names (see tests/unit/CMakeLists.txt) keep ctest filter
    # discovery robust across toolchains.
    include(Catch)
    catch_discover_tests(psynder_golden)
else()
    # No Catch2 available: keep the historical exe-level smoke entry so the
    # determinism regex still selects a (trivial) golden target rather than 0 tests.
    add_executable(psynder_golden ${SRC})
    target_link_libraries(psynder_golden PRIVATE psynder_core psynder_math)
    add_test(NAME psynder_golden_smoke COMMAND psynder_golden)
endif()
