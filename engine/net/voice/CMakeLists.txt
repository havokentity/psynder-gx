# SPDX-License-Identifier: MIT OR Apache-2.0
# psynder_net_voice — Lane 19.
# Opus codec, server-side mixing per recipient, VAD, push-to-talk.
# Same UDP transport as gameplay netcode; voice on its own unreliable
# channel with sequence numbers, no retransmission. See DESIGN §10.9.
psynder_add_lane(net_voice
    LINKS
        psynder_core psynder_jobs psynder_net psynder_audio
)
