#pragma once

/* Principia co-op (LAN/IP multiplayer)
 *
 * A very light-weight authoritative-host sync layer for the sandbox
 * (paused/build) mode:
 *
 *   - host opens a TCP port, clients connect by IP
 *   - on join the host streams the whole room to the new player
 *   - object spawns, movements/rotations, layer changes, deletions and
 *     connections are broadcast to everyone
 *   - each player gets a robot avatar spawned next to the host
 *   - join/leave/chat messages are printed in the lower left corner
 */

#include <cmath>
#include "mp_net.hh"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

class entity;
class connection;
class widget_manager;
class principia_wdg;

namespace tms { class surface; }

#define MP_PROTOCOL_VERSION   23   /* 23: connections to the terrain (ground) */
#define MP_DEFAULT_PORT       7777
#define MP_MAX_PLAYERS        8
#define MP_ID_BLOCK           2000000u   /* entity id range per player */
#define MP_HUD_LINES          6
#define MP_HUD_LIFETIME       12.0       /* seconds a hud line stays visible */
#define MP_PING_INTERVAL      2.0        /* keepalive interval */
#define MP_TIMEOUT            20.0       /* drop silent peers after this */
#define MP_SNAP_DISTANCE      2.5f       /* above this error we snap instead of blend */
#define MP_SMOOTHING          0.45f      /* unused: clients no longer blend */
#define MP_HANDSHAKE_TIMEOUT  10.0       /* drop sockets that never say HELLO */
#define MP_MAX_OPS_PER_FRAME  512        /* remote messages applied per frame */

namespace mp {

enum {
    MODE_OFF = 0,
    MODE_HOST,
    MODE_CLIENT,
};

enum {
    MSG_HELLO = 1,      /* c->s: version, name */
    MSG_WELCOME,        /* s->c: peer id, id base, room info */
    MSG_PEER_JOIN,      /* s->c: peer id, name */
    MSG_PEER_LEAVE,     /* s->c: peer id, name */
    MSG_CHAT,           /* both: peer id, text */
    MSG_SNAPSHOT_BEGIN, /* s->c */
    MSG_SNAPSHOT_END,   /* s->c */
    MSG_SPAWN,          /* both: serialized entity */
    MSG_XFORM,          /* both: id, x, y, angle, layer */
    MSG_DELETE,         /* both: id */
    MSG_CONNECT,        /* both: connection data */
    MSG_DISCONNECT,     /* both: entity id pair */
    MSG_AVATAR,         /* s->c: peer id -> entity id */
    MSG_REQUEST_SNAPSHOT, /* c->s */
    MSG_PING,
    MSG_READY,          /* c->s: this player pressed play */
    MSG_START_PLAY,     /* s->c: everyone is ready, start the round */
    MSG_STOP_PLAY,      /* both: back to build mode */
    MSG_STATE,          /* s->c: physics/animation state batch */
    MSG_INPUT,          /* c->s: player input */
    MSG_ENTITY_UPDATE,  /* both: full re-serialized entity (settings/size/color) */
    MSG_EVENT,          /* both: one-shot event (shot fired, damage, death, ...) */
    MSG_STOP_READY,     /* c->s: this player wants to go back to build mode */
    MSG_GROUP_XFORM,    /* both: transform of a welded group (keyed by member id) */
    MSG_ANIM,           /* both: animation start/stop */
    MSG_PONG,           /* both: reply to MSG_PING, used to measure the ping */
    MSG_PLAYERS,        /* s->c: real player count */
    MSG_REQUEST_AVATARS,/* c->s: 'I do not know which robot is mine' */
    MSG_TOOL,           /* s->c: robot id, equipped tool, equipped weapon */
    MSG_TERRAIN,        /* s->c: chunk x/y, layer, pixel x/y destroyed */
    MSG_CABLE,          /* both: cable id, type, both plug ends (build mode) */
    MSG_CABLE_DEL,      /* both: cable id removed (build mode) */
    MSG_PANEL,          /* c->s: RC panel id + widget values (round) */
    MSG_ROBOT_CFG,      /* both: robot head/feet/back/front/bolts/faction/items (build mode) */
    MSG_MOVEABLE,       /* both: object id + "moveable when playing" flag (build mode) */
};

#define MP_SYNC_PANEL_INTERVAL 0.05  /* client -> host RC panel values */
#define MP_STATE_INTERVAL     0.033   /* seconds between state batches */
#define MP_INPUT_INTERVAL     0.033   /* seconds between input updates */
/* Player characters are what the eye follows, so they get their own, much
 * faster stream: everything else may lag a frame behind, a character must
 * not. */
#define MP_AVATAR_INTERVAL    0.016   /* avatars: ~60 updates per second */
#define MP_LAYERMOVE_ATTEMPTS 30      /* frames a layer switch is retried */
#define MP_STATE_BATCH        160     /* max objects per state message */
#define MP_STATE_MAX_BODIES   8
#define MP_PROP_SCAN_INTERVAL 0.2   /* seconds between object-settings scans */
#define MP_PROP_SCAN_BUDGET   96    /* objects checked per scan pass */
#define MP_MAX_SPAWNS_PER_SEC 120   /* runtime spawn flood protection */
#define MP_DEADZONE           0.012f /* position error we simply ignore */
#define MP_ERROR_FULL         0.30f  /* error at which we correct at full rate */
/* how long an object stays 'owned' by the peer that last moved/edited it */
#define MP_REMOTE_HOLD          1.5

#define MP_RESYNC_INTERVAL    2.0    /* seconds between resync requests */

/* one-shot events broadcast to everyone */
enum {
    MP_EV_SHOOT = 1,    /* entity fired its weapon */
    MP_EV_DAMAGE,       /* entity took damage */
    MP_EV_DEATH,        /* entity died */
    MP_EV_RESPAWN,      /* entity came back to life */
};

/* Animations use two channels: the state stream keeps blend/timer values in
 * sync, these events mark the exact frame an animation starts and ends. */
enum {
    MP_ANIM_ATTACK = 1,
    MP_ANIM_JUMP,
    MP_ANIM_LAYERMOVE,
    MP_ANIM_ACTION,
};

enum {
    MP_PHASE_STOP = 0,
    MP_PHASE_START = 1,
};

/* one frame of player input */
struct input_state {
    int8_t move_dir;   /* -1 left, 0 none, 1 right */
    int8_t look_dir;   /* -1 or 1 */
    int8_t layer_req;  /* -1/+1: player asked to switch layer, 0 = nothing */
    bool   jump;
    bool   attack;     /* shoot */
    bool   action;     /* use / special action */
    float  aim;        /* weapon arm angle, so remote players can aim */
    bool   mining;     /* zapper beam held on the terrain */
    float  mine_x, mine_y;
    bool   up, down;   /* held up/down: ladders */

    input_state() : move_dir(0), look_dir(1), layer_req(0), jump(false),
                    attack(false), action(false), aim(0.f),
                    mining(false), mine_x(0.f), mine_y(0.f),
                    up(false), down(false) {}

    bool differs(const input_state &o) const {
        float da = this->aim - o.aim;
        if (da < 0.f) da = -da;

        return this->move_dir != o.move_dir
            || this->look_dir != o.look_dir
            || this->layer_req != o.layer_req
            || this->jump != o.jump
            || this->attack != o.attack
            || this->action != o.action
            || this->mining != o.mining
            || this->up != o.up || this->down != o.down
            || (this->mining && (std::fabs(this->mine_x - o.mine_x) > 0.05f
                              || std::fabs(this->mine_y - o.mine_y) > 0.05f))
            || da > 0.01f;
    }
};

struct host_config {
    char server_name[64];
    char player_name[32];
    int  port;
    int  max_players;
    int  room_width;
    int  room_height;

    host_config();
};

struct join_config {
    char ip[128];
    char player_name[32];
    int  port;

    join_config();
};

struct peer {
    int          id;
    std::string  name;
    std::string  ip;
    conn        *c;
    bool         ready;      /* handshake done */
    uint32_t     avatar_id;
    bool         wants_play; /* pressed play, waiting for the others */
    bool         wants_stop; /* pressed back/B, waiting for the others */
    input_state  input;      /* last received input */
    input_state  prev_input;
    double       last_recv;    /* last time we heard from this peer */
    double       connected_at; /* socket accept time (handshake timeout) */
    double       ping_sent;    /* time the last MSG_PING went out */
    int          rtt_ms;       /* measured round trip time */

    peer() : id(0), c(0), ready(false), avatar_id(0), wants_play(false),
             wants_stop(false), last_recv(0.0), connected_at(0.0),
             ping_sent(0.0), rtt_ms(0) {}
};

struct xform_cache {
    float x, y, angle;
    int layer;
};

/* --- state --- */
extern int          mode;

/* True as soon as a co-op session was started in this run, and it stays true
 * after the session ends. The world keeps objects that arrived over the
 * network, which the final teardown on exit can not handle. */
extern bool         had_session;
extern bool         applying;         /* true while we apply a remote event */
extern std::string  local_name;
extern int          local_peer_id;
extern host_config  cfg_host;
extern join_config  cfg_join;

bool is_active();
bool is_host();
bool is_client();
int  num_players();
const char *status_line();

/* --- lifecycle --- */
/* make sure the local player controls a robot in the current level.
 * returns the entity id of the local robot, or 0 */
uint32_t attach_local_player();

bool host_start(const host_config &cfg);
bool client_start(const join_config &cfg);
void shutdown(const char *reason);

/* Called every frame from game::step() */
void step();

/* --- local event hooks (called by the game) --- */
void on_local_spawn(entity *e);
void on_local_delete(uint32_t entity_id);
void on_local_connection(connection *c, int option);
void on_local_disconnect(uint32_t a, uint32_t b);
void on_local_disconnect_ents(entity *a, entity *b);
bool is_syncing();
void send_chat(const char *text);

/* --- co-op play mode --- */

/**
 * The local player pressed "play". The round only starts once every player
 * in the session has pressed it.
 *
 * @return true if the co-op layer handled the press (the caller must not
 *         start the simulation on its own).
 */
bool request_play();

/**
 * The local player pressed "pause"/B to go back to build mode. Just like
 * starting the round, this only happens once every player asked for it.
 *
 * @return true if the co-op layer handled the press (the caller must not
 *         stop the simulation on its own).
 */
bool request_stop();

/** true while the co-op round is running. */
bool session_playing();

/**
 * true when the local world must not simulate itself (clients replicate the
 * host's physics instead of running their own simulation).
 */
bool suppress_local_sim();

/**
 * true while the local Box2D world must not be stepped at all. During a co-op
 * round a client is a pure viewer: it renders exactly what the host sends and
 * never runs a simulation of its own, which is what keeps both worlds equal.
 *
 * world::step() must skip its b2World::Step() call when this returns true.
 */
bool suppress_local_physics();

/** Measured round trip time in milliseconds (worst peer on the host). */
int ping_ms();

/** An entity was created/removed while the round is running. */
void on_world_reset();
void on_world_teardown();
void on_runtime_spawn(entity *e);
void on_runtime_remove(entity *e);
/* host: a terrain pixel of a chunk was destroyed (chunk terrain is not made
 * of entities, so it never goes through on_runtime_remove) */
void on_terrain_pixel(int cx, int cy, int layer, int x, int y);

/**
 * The chunk window is about to load/unload procedural terrain, or is done
 * doing so. Objects that appear while this is true are generated from the
 * level seed and exist on every machine already - streaming them would
 * create a second copy of each with the same id, which is what crashed a
 * client that walked away from the host.
 *
 * world::step() must wrap its cwindow->step() call in these.
 */
void set_chunk_loading(bool loading);
/* set by level_chunk while it breaks the joints of dug-away pixels */
void set_ground_dig(bool on);

/** true while a co-op client must not apply a layer switch locally. */
bool suppress_local_layermove();

/** Client: our own character changed layer, tell the host. */
void on_local_layermove(int dir);

/**
 * The local player changed the settings/size/appearance of an object (for
 * example the width of a plank or the frequency of a receiver). The whole
 * object is re-sent so every detail stays in sync.
 */
void on_local_entity_changed(entity *e);

/** Broadcast a one-shot event (shot, damage, death, respawn). */
void send_event(uint8_t ev, uint32_t entity_id, float a = 0.f, float b = 0.f);

/** Client in a round: local projectiles are visual only, no damage. */
bool suppress_local_damage();

/** The entity id of the local player's character (0 if none). */
uint32_t local_avatar_id();

/* --- lower-left message log --- */
void hud_init(widget_manager *wm, tms::surface *surface);
void hud_add(const char *fmt, ...);
void hud_step();
void hud_clear();

}
