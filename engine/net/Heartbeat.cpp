// SPDX-License-Identifier: MIT
//
// engine/net/Heartbeat.cpp — the heartbeat tracker is header-only (all logic is
// inline in Heartbeat.h). This TU anchors the lane's source glob and pins the
// POD/triviality contract so a Heartbeat can be snapshotted/memcpy'd if needed.

#include "net/Heartbeat.h"

#include <type_traits>

namespace psynder::net {

static_assert(std::is_trivially_copyable_v<Heartbeat>,
              "Heartbeat must stay trivially copyable (plain timestamp state)");
static_assert(std::is_trivially_copyable_v<HeartbeatConfig>,
              "HeartbeatConfig must stay a POD policy struct");

}  // namespace psynder::net
