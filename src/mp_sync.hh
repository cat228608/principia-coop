#pragma once

/* Principia co-op: world synchronization (host as authority).
 *
 * Split out of multiplayer.cc so the streaming rules live in one place:
 *
 * Only the HOST sends any of these. Clients send MSG_INPUT and nothing else.
 *
 *   players   - batched, ~20 Hz, unreliable   (position, angle, layer, velocity)
 *   bodies    - batched, ~20 Hz, unreliable   (dynamic b2Body only, moving only)
 *   creature  - reliable, ON STATE CHANGE ONLY (_state, creature_flags,
 *               look_dir, dir). Animations keep playing locally between two
 *               of these messages, which is what removes the walk lag of the
 *               previous per-frame animation stream.
 *   layer     - reliable, on prio change
 *   shots     - reliable, one shot event (origin + direction), no projectile
 *               physics on the wire
 *   conns     - reliable, connection create/destroy
 *
 * The client runs its own Box2D simulation for looks and is corrected
 * softly (lerp) or snapped when it drifted more than MP_SYNC_SNAP_DIST.
 */

#include "mp_net.hh"

#include <cstdint>
#include <vector>

class entity;
class connection;
class creature;

/* streaming rates */
#define MP_SYNC_INTERVAL     0.05    /* 20 Hz for position streams */
#define MP_SYNC_SNAP_DIST    2.0f    /* above this error: snap, below: lerp */
#define MP_SYNC_LERP         0.30f   /* correction applied per tick */
#define MP_SYNC_DEADZONE     0.02f   /* error we do not bother correcting */
/* own, locally predicted character: the host picture is one RTT old, so a
 * small error is expected and must not be pulled back (rubber banding) */
#define MP_SYNC_OWN_TOLERANCE 0.35f
#define MP_SYNC_OWN_LERP      0.10f   /* gentler pull for our own character */
#define MP_SYNC_VEL_EPS      0.04f   /* (m/s)^2 below which a body is "still" */
#define MP_SYNC_STILL_HOLD   0.75    /* keep streaming this long after it stops */
#define MP_SYNC_BATCH        128     /* objects per packet */

namespace mp {

/* creature_flags that actually change how a character is drawn. Everything
 * else (damage bookkeeping, lost limbs, ...) is either derived or arrives
 * through its own event, so it must not trigger a state message. */
#define MP_ANIM_FLAG_MASK  (CREATURE_MOVING_LEFT | CREATURE_MOVING_RIGHT \
                          | CREATURE_MOVING_UP | CREATURE_MOVING_DOWN \
                          | CREATURE_CLIMBING_LADDER | CREATURE_FROZEN \
                          | CREATURE_LOST_BALANCE)

namespace sync {

/* message ids, continue the enum in multiplayer.hh */
enum {
    MSG_PLAYER_POS = 64,   /* unreliable: batched player transforms */
    MSG_BODY_POS,          /* unreliable: batched dynamic body transforms */
    MSG_CREATURE_STATE,    /* reliable: _state / creature_flags / look_dir / dir */
    MSG_LAYER,             /* reliable: prio change */
    MSG_SHOT,              /* reliable: origin + direction of a shot */

    MSG__LAST,
};

/** Forget every cached transform and state (session start/stop, level load). */
void reset();

/** Sample the characters/objects this machine owns and send what changed.
 *  Called once per frame on BOTH sides. */
void send_tick();

/** Interpolate/correct everything we do not own towards the last received
 *  transforms. Called once per frame on BOTH sides. */
void recv_tick();

/** Returns true if the message belonged to this module. */
bool handle(uint8_t type, buf *b);

/* --- owner side hooks --- */
void on_shot(creature *shooter);

}

/* Glue implemented in multiplayer.cc: the session state the sync module
 * needs, without exposing its internals. */
namespace glue {

void     send_all(uint8_t type, const buf &b, bool reliable, int except_peer = -1);
void     avatar_ids(std::vector<uint32_t> *out);
uint32_t local_avatar();
bool     is_avatar(uint32_t id);
bool     playing();
bool     is_host();
bool     is_client();
double   now();
void     set_applying(bool v);

}

}
