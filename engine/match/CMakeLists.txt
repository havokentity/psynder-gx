# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_match — the server-authoritative match loop that bridges the netcode
# (engine/net ReplicationSession: movement prediction/reconciliation + lag-comp
# hit detection) to the real gameplay ECS (engine/gameplay: Health/Score/Damage/
# respawn). Headless + deterministic (no GPU/audio/display) so it builds and
# runs under the dedicated-server preset. On the authoritative lockstep tick, so
# it compiles with the determinism FP flags and is covered by perf_guardrails.
psynder_add_lane(match
    LINKS
        psynder_core psynder_math psynder_scene psynder_net psynder_gameplay
)
psynder_determinism_fp(psynder_match)
