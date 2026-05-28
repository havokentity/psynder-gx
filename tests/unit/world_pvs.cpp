// SPDX-License-Identifier: MIT
// Psynder — Lane 12 (world-bsp) cluster-PVS unit test.
//
// Tiny 3-cluster PVS feeding the render-extract cull:
//   cluster 0 sees {0, 1}  (cluster 2 is occluded from the view)
//   cluster 1 sees {1, 2}
//   cluster 2 sees {2}
//
// Entities live in clusters {0, 1, 2}. From view cluster 0, cull() must emit
// exactly the entities in clusters 0 and 1 (in ascending entity order) and
// exclude the cluster-2 entity.

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "world/bsp/Pvs.h"

using namespace psynder;
using namespace psynder::world::bsp;

namespace {

// 0 sees {0,1}; 1 sees {1,2}; 2 sees {2}.
PvsTable make_three_cluster_pvs() {
    PvsTable pvs(3);
    pvs.set_visible(0, 0);
    pvs.set_visible(0, 1);
    pvs.set_visible(1, 1);
    pvs.set_visible(1, 2);
    pvs.set_visible(2, 2);
    return pvs;
}

}  // namespace

TEST_CASE("world_pvs/visible reflects the set bits", "[world_pvs]") {
    const PvsTable pvs = make_three_cluster_pvs();

    REQUIRE(pvs.cluster_count() == 3u);

    REQUIRE(pvs.visible(0, 0));
    REQUIRE(pvs.visible(0, 1));
    REQUIRE_FALSE(pvs.visible(0, 2));  // cluster 2 occluded from cluster 0

    REQUIRE(pvs.visible(1, 1));
    REQUIRE(pvs.visible(1, 2));
    REQUIRE_FALSE(pvs.visible(1, 0));

    // Out-of-range queries are safe and return false.
    REQUIRE_FALSE(pvs.visible(3, 0));
    REQUIRE_FALSE(pvs.visible(0, 7));
}

TEST_CASE("world_pvs/cull emits visible entity indices in order",
          "[world_pvs]") {
    const PvsTable pvs = make_three_cluster_pvs();

    // entity:   0    1    2    3    4
    // cluster:  0    2    1    2    0
    const std::vector<u32> entity_clusters = {0u, 2u, 1u, 2u, 0u};

    std::vector<u32> out_visible;
    pvs.cull(/*view_cluster*/ 0u, entity_clusters, out_visible);

    // From cluster 0 only clusters {0,1} are visible → entities 0, 2, 4
    // (entities 1 and 3 are in occluded cluster 2). Order is ascending entity
    // index, matching the input layout.
    REQUIRE(out_visible == std::vector<u32>{0u, 2u, 4u});
}

TEST_CASE("world_pvs/cull appends rather than clears the output",
          "[world_pvs]") {
    const PvsTable pvs = make_three_cluster_pvs();
    const std::vector<u32> entity_clusters = {2u, 1u};  // entity 0 occluded

    std::vector<u32> out_visible = {99u};  // pre-existing content is preserved
    pvs.cull(0u, entity_clusters, out_visible);

    REQUIRE(out_visible == std::vector<u32>{99u, 1u});
}
