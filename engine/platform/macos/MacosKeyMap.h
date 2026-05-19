// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/platform/macos/MacosKeyMap.h
//
// Lane 25 — macOS virtual keycode → psynder::platform::KeyCode translation.
//
// macOS virtual keycodes are defined in <Carbon/Carbon.h> (kVK_* constants)
// but Carbon.h drags in the whole Carbon framework. We use the raw numeric
// values here (documented in HIToolbox/Events.h and stable since OS X 10.0)
// to avoid adding Carbon as a dependency for a pure-input TU.
//
// The mapping is intentionally a plain constexpr array indexed by the raw
// virtual keycode (0..127; macOS only defines 0x00–0x7F). Any keycode not
// listed resolves to KeyCode::Unknown.

#pragma once

#include "platform/Platform.h"  // psynder::platform::KeyCode

#include <array>
#include <cstdint>

namespace psynder::platform::macos {

// macOS virtual keycodes (raw numeric, HIToolbox/Events.h)
// Only the ones we care about are listed; the rest stay at their zero-init
// KeyCode::Unknown value.
namespace vk {
    // Letters
    static constexpr uint8_t kA = 0x00;
    static constexpr uint8_t kS = 0x01;
    static constexpr uint8_t kD = 0x02;
    static constexpr uint8_t kF = 0x03;
    static constexpr uint8_t kH = 0x04;
    static constexpr uint8_t kG = 0x05;
    static constexpr uint8_t kZ = 0x06;
    static constexpr uint8_t kX = 0x07;
    static constexpr uint8_t kC = 0x08;
    static constexpr uint8_t kV = 0x09;
    static constexpr uint8_t kB = 0x0B;
    static constexpr uint8_t kQ = 0x0C;
    static constexpr uint8_t kW = 0x0D;
    static constexpr uint8_t kE = 0x0E;
    static constexpr uint8_t kR = 0x0F;
    static constexpr uint8_t kY = 0x10;
    static constexpr uint8_t kT = 0x11;
    static constexpr uint8_t k1 = 0x12;
    static constexpr uint8_t k2 = 0x13;
    static constexpr uint8_t k3 = 0x14;
    static constexpr uint8_t k4 = 0x15;
    static constexpr uint8_t k6 = 0x16;
    static constexpr uint8_t k5 = 0x17;
    static constexpr uint8_t kEqual = 0x18;
    static constexpr uint8_t k9 = 0x19;
    static constexpr uint8_t k7 = 0x1A;
    static constexpr uint8_t kMinus = 0x1B;
    static constexpr uint8_t k8 = 0x1C;
    static constexpr uint8_t k0 = 0x1D;
    static constexpr uint8_t kO = 0x1F;
    static constexpr uint8_t kU = 0x20;
    static constexpr uint8_t kI = 0x22;
    static constexpr uint8_t kP = 0x23;
    static constexpr uint8_t kL = 0x25;
    static constexpr uint8_t kJ = 0x26;
    static constexpr uint8_t kK = 0x28;
    static constexpr uint8_t kN = 0x2D;
    static constexpr uint8_t kM = 0x2E;
    // Special keys
    static constexpr uint8_t kReturn    = 0x24;
    static constexpr uint8_t kTab       = 0x30;
    static constexpr uint8_t kSpace     = 0x31;
    static constexpr uint8_t kBackspace = 0x33;
    static constexpr uint8_t kEscape    = 0x35;
    static constexpr uint8_t kGrave     = 0x32;   // ` / ~ (tilde)
    // Modifiers (reported by FlagsChanged events)
    static constexpr uint8_t kLeftShift  = 0x38;
    static constexpr uint8_t kRightShift = 0x3C;
    static constexpr uint8_t kLeftCtrl   = 0x3B;
    static constexpr uint8_t kRightCtrl  = 0x3E;
    static constexpr uint8_t kLeftAlt    = 0x3A;  // Option
    static constexpr uint8_t kRightAlt   = 0x3D;
    // Function keys
    static constexpr uint8_t kF1  = 0x7A;
    static constexpr uint8_t kF2  = 0x78;
    static constexpr uint8_t kF3  = 0x63;
    static constexpr uint8_t kF4  = 0x76;
    static constexpr uint8_t kF5  = 0x60;
    static constexpr uint8_t kF6  = 0x61;
    static constexpr uint8_t kF7  = 0x62;
    static constexpr uint8_t kF8  = 0x64;
    static constexpr uint8_t kF9  = 0x65;
    static constexpr uint8_t kF10 = 0x6D;
    static constexpr uint8_t kF11 = 0x67;
    static constexpr uint8_t kF12 = 0x6F;
    // Arrow keys
    static constexpr uint8_t kLeft  = 0x7B;
    static constexpr uint8_t kRight = 0x7C;
    static constexpr uint8_t kDown  = 0x7D;
    static constexpr uint8_t kUp    = 0x7E;
} // namespace vk

// Table size: macOS virtual keycodes fit in 7 bits (0x00–0x7F).
static constexpr std::size_t kVKTableSize = 128;

// Build the lookup table at compile time.
// clang-format off
inline constexpr std::array<KeyCode, kVKTableSize> make_vk_table() {
    std::array<KeyCode, kVKTableSize> t{};  // zero-init → KeyCode(0) == Unknown
    using K = KeyCode;

    t[vk::kA] = K::A;  t[vk::kB] = K::B;  t[vk::kC] = K::C;
    t[vk::kD] = K::D;  t[vk::kE] = K::E;  t[vk::kF] = K::F;
    t[vk::kG] = K::G;  t[vk::kH] = K::H;  t[vk::kI] = K::I;
    t[vk::kJ] = K::J;  t[vk::kK] = K::K;  t[vk::kL] = K::L;
    t[vk::kM] = K::M;  t[vk::kN] = K::N;  t[vk::kO] = K::O;
    t[vk::kP] = K::P;  t[vk::kQ] = K::Q;  t[vk::kR] = K::R;
    t[vk::kS] = K::S;  t[vk::kT] = K::T;  t[vk::kU] = K::U;
    t[vk::kV] = K::V;  t[vk::kW] = K::W;  t[vk::kX] = K::X;
    t[vk::kY] = K::Y;  t[vk::kZ] = K::Z;

    t[vk::kReturn]    = K::Enter;
    t[vk::kTab]       = K::Tab;
    t[vk::kSpace]     = K::Space;
    t[vk::kBackspace] = K::Backspace;
    t[vk::kEscape]    = K::Escape;
    t[vk::kGrave]     = K::Tilde;

    t[vk::kLeftShift]  = K::LeftShift;
    t[vk::kRightShift] = K::RightShift;
    t[vk::kLeftCtrl]   = K::LeftCtrl;
    t[vk::kRightCtrl]  = K::RightCtrl;
    t[vk::kLeftAlt]    = K::LeftAlt;
    t[vk::kRightAlt]   = K::RightAlt;

    t[vk::kF1]  = K::F1;  t[vk::kF2]  = K::F2;  t[vk::kF3]  = K::F3;
    t[vk::kF4]  = K::F4;  t[vk::kF5]  = K::F5;  t[vk::kF6]  = K::F6;
    t[vk::kF7]  = K::F7;  t[vk::kF8]  = K::F8;  t[vk::kF9]  = K::F9;
    t[vk::kF10] = K::F10; t[vk::kF11] = K::F11; t[vk::kF12] = K::F12;

    t[vk::kLeft]  = K::Left;  t[vk::kRight] = K::Right;
    t[vk::kDown]  = K::Down;  t[vk::kUp]    = K::Up;

    return t;
}
// clang-format on

inline constexpr auto kVKTable = make_vk_table();

// Translate a raw macOS virtual keycode to the cross-platform KeyCode.
// Returns KeyCode::Unknown for anything not in the table (> 0x7F or unmapped).
inline constexpr KeyCode vk_to_keycode(uint16_t vk) noexcept {
    if (vk >= kVKTableSize) return KeyCode::Unknown;
    return kVKTable[static_cast<std::size_t>(vk)];
}

} // namespace psynder::platform::macos
