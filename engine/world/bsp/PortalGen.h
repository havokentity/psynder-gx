// SPDX-License-Identifier: MIT
// Psynder — runtime BSP tree portalizer. Lane 12 (world-bsp) owns.
//
// The frozen .psybsp format (BspFormat.h) carries no portals chunk and the
// frozen BspMap ABI (Bsp.h) has no portal field, so build_portal_set(const
// BspMap&) in Portal.h returns an empty set (callers fall back to PVS-only
// visibility). This header adds a runtime portalizer that derives the portal
// graph directly from the BSP node planes + leaf cells — the classic Quake
// qbsp "MakeTreePortals" flood — so portal culling has a real graph to
// traverse today. When lm_qbsp (lane 24) lands a portals chunk in a future
// format bump, build_portal_set() will read it directly and this runtime
// fallback becomes optional.

#pragma once

#include <vector>  // Bsp.h uses std::vector without including <vector>; see Bsp.cpp.
#include "Bsp.h"
#include "Portal.h"

namespace psynder::world::bsp {

// Derive a portal set from a compiled BSP tree. Each portal is the convex
// polygon where two non-solid leaf cells meet on a node split plane, oriented
// per BspPortal's front->back convention (normal points front_leaf ->
// back_leaf). Portals touching the exterior void or a solid leaf are dropped.
//
// Cost is O(nodes * portals); intended to run once at level load, not per
// frame. Returns an empty set for a degenerate tree (no nodes, no leaves, or
// zero-volume world bounds).
BspPortalSet build_portal_set_from_tree(const BspMap& map);

}  // namespace psynder::world::bsp
