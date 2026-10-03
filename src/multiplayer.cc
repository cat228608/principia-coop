#include "multiplayer.hh"
#include "mp_sync.hh"

#include "adventure.hh"
#include "faction.hh"
#include "creature.hh"
#include "entity.hh"
#include "font.hh"
#include "game.hh"
#include "main.hh"
#include "object_factory.hh"
#include "pkgman.hh"
#include "robot_base.hh"
#include "robot_parts.hh"
#include "text.hh"
#include "ui.hh"
#include "widget_manager.hh"
#include "world.hh"
#include "chunk.hh"

#include <SDL3/SDL.h>
#include <tms/math/misc.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <set>
#include <map>
#include <vector>

namespace mp {

int         mode = MODE_OFF;
bool        applying = false;
std::string local_name = "Player";
bool had_session = false;
int         local_peer_id = 0;
host_config cfg_host;
join_config cfg_join;

host_config::host_config() {
    snprintf(this->server_name, sizeof(this->server_name), "Principia co-op");
    snprintf(this->player_name, sizeof(this->player_name), "Host");
    this->port = MP_DEFAULT_PORT;
    this->max_players = 4;
    this->room_width = 60;
    this->room_height = 40;
}

join_config::join_config() {
    snprintf(this->ip, sizeof(this->ip), "127.0.0.1");
    snprintf(this->player_name, sizeof(this->player_name), "Player");
    this->port = MP_DEFAULT_PORT;
}

/* ------------------------------------------------------------ internals */

static listener             _listener;
static std::vector<peer*>   _peers;      /* host only */
static conn                 _server;     /* client only */
static int                  _next_peer_id = 1;
static double               _last_xform_sync = 0.0;
static std::map<uint32_t, xform_cache> _xf;

/* transforms of welded groups, keyed by the lowest member id */
static std::map<uint32_t, xform_cache> _gxf;

/* incoming messages, applied once per frame from step() */
struct pending_msg {
    uint8_t type;
    buf     b;
    int     peer_id;   /* -1 = from the server */
};

static std::deque<pending_msg> _inq;

static std::map<uint32_t, uint32_t> _sig;
static std::map<uint32_t, char> _awake;
static std::map<uint32_t, double> _remote_touch;
static uint32_t _refollow = 0;            /* re-point the camera after a re-create */
static bool     _need_wipe = false;       /* drop our locally generated world once */
static uint32_t _local_avatar = 0;        /* our own character in the room */

/* The character the session gave us. _local_avatar is wiped every time the
 * level is rebuilt (world::create -> on_world_reset), every time a round
 * starts and every time the client drops its locally generated world - and
 * nothing ever restored it. The host then streamed nobody (its own robot was
 * not even in the avatar list) and the client controlled nothing. This id
 * survives all of that and is used to find the character again. */
static uint32_t _assigned_avatar = 0;

/* Every character that belongs to a player, on BOTH sides: entity id -> peer
 * id of its owner. A client used to know only its own, so it could not tell
 * a player apart from a crate. */
static std::map<uint32_t, int> _avatars;
static uint32_t _id_base = 0;
/* client: objects its own (visual only) simulation produced during a round.
 * The host is the authority and streams its copy, the local one is removed. */
static std::set<uint32_t> _client_doomed;
/* ids of every object of the level when the round started (both sides) */
static std::set<uint32_t> _level_ids;
/* host: last tool/weapon sent per player robot */
static std::map<uint32_t, int> _tool_sent;
static double _tool_resend = 0.0;             /* client: object id block the host gave us */
static char                 _status[192] = {0};
static double               _last_recv = 0.0;      /* client: last packet from host */
static double               _last_ping = 0.0;
static double               _last_resync_req = 0.0;
static int                  _resync_requests = 0;
#define MP_MAX_RESYNC_REQUESTS 4
static bool                 _loading = false;      /* level (re)load in progress */

/* hud */
struct hud_line {
    std::string text;
    double      time;
};

static principia_wdg   *_hud_wdg[MP_HUD_LINES] = {0};
static std::deque<hud_line> _hud;
static bool             _hud_dirty = false;

static double now_sec() {
    return (double)SDL_GetTicks() / 1000.0;
}

bool is_active() { return mode != MODE_OFF; }
bool is_host()   { return mode == MODE_HOST; }
bool is_client() { return mode == MODE_CLIENT; }

/* Clients used to guess "2": they had no roster at all. */
static int    _roster_count = 0;
static double _local_ping_sent = 0.0;
static int    _local_rtt_ms = 0;

int num_players() {
    if (mode == MODE_HOST) {
        int n = 1;
        for (size_t x = 0; x < _peers.size(); ++x)
            if (_peers[x]->ready) ++n;
        return n;
    }

    if (mode == MODE_CLIENT) return _roster_count > 0 ? _roster_count : 2;

    return 0;
}

int ping_ms() {
    if (mode == MODE_CLIENT) return _local_rtt_ms;

    int worst = 0;

    for (size_t x = 0; x < _peers.size(); ++x)
        if (_peers[x] && _peers[x]->rtt_ms > worst) worst = _peers[x]->rtt_ms;

    return worst;
}

const char *status_line() { return _status; }

static void update_status() {
    if (mode == MODE_HOST)
        snprintf(_status, sizeof(_status), "Hosting \"%s\" on port %d - %d player(s)",
                 cfg_host.server_name, cfg_host.port, num_players());
    else if (mode == MODE_CLIENT)
        snprintf(_status, sizeof(_status), "Connected to %s:%d as %s",
                 cfg_join.ip, cfg_join.port, local_name.c_str());
    else
        _status[0] = 0;
}

/* --------------------------------------------------------------- sending */

static void broadcast(uint8_t type, const buf &b, int except_peer = -1) {
    if (mode == MODE_HOST) {
        for (size_t x = 0; x < _peers.size(); ++x) {
            peer *p = _peers[x];
            if (!p || !p->c || !p->c->valid()) continue;
            if (p->id == except_peer) continue;
            p->c->send(type, b);
        }
    } else if (mode == MODE_CLIENT) {
        if (_server.valid()) _server.send(type, b);
    }
}

/* ------------------------------------------------------------------- hud */

void hud_init(widget_manager *wm, tms::surface *surface) {
    if (!wm) return;
    if (_hud_wdg[0]) return;

    for (int x = 0; x < MP_HUD_LINES; ++x) {
        _hud_wdg[x] = wm->create_widget(surface, TMS_WDG_LABEL,
                                        GW_IGNORE, AREA_BOTTOM_LEFT);
        _hud_wdg[x]->priority = 5000 - x;
        _hud_wdg[x]->set_label(" ", font::small);
    }

    _hud_dirty = true;
}

void hud_clear() {
    _hud.clear();
    _hud_dirty = true;
}

void hud_add(const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    tms_infof("co-op: %s", tmp);

    hud_line l;
    l.text = tmp;
    l.time = now_sec();

    _hud.push_back(l);

    while (_hud.size() > MP_HUD_LINES)
        _hud.pop_front();

    _hud_dirty = true;
}

void hud_step() {
    double t = now_sec();

    while (!_hud.empty() && t - _hud.front().time > MP_HUD_LIFETIME) {
        _hud.pop_front();
        _hud_dirty = true;
    }

    if (!_hud_dirty || !_hud_wdg[0]) return;

    _hud_dirty = false;

    /* newest line at the bottom */
    for (int x = 0; x < MP_HUD_LINES; ++x) {
        principia_wdg *w = _hud_wdg[x];
        if (!w) continue;

        int idx = (int)_hud.size() - MP_HUD_LINES + x;

        if (idx < 0 || idx >= (int)_hud.size()) {
            w->remove();
            continue;
        }

        w->set_label(_hud[idx].text.c_str(), font::small);
        w->add();
    }

    if (_hud_wdg[0] && _hud_wdg[0]->parent)
        _hud_wdg[0]->parent->rearrange();
}

/* ------------------------------------------------- entity (de)serializing */

static bool is_streamable(entity *e);
static bool is_player_avatar(uint32_t id);
static void write_conn_xforms(buf *b, entity *e, entity *o);

static bool write_entity(buf *b, entity *e) {
    if (!e || !W) return false;

    e->pre_write();

    lvlbuf lb;
    lb.ensure(4096);
    lb.size = 0;
    lb.rp = 0;

    of::write(&lb, (uint8_t)W->level.version, e, 0, b2Vec2(0.f, 0.f), false);

    if (lb.size == 0) return false;

    b->w_u8((uint8_t)W->level.version);
    b->w_blob(lb.buf, (uint32_t)lb.size);

    return true;
}

static entity *read_entity(buf *b) {
    uint8_t version = b->r_u8();

    std::vector<uint8_t> data;
    if (!b->r_blob(&data) || data.empty()) return 0;

    lvlbuf lb;
    lb.ensure(data.size() + 16);
    memcpy(lb.buf, &data[0], data.size());
    lb.size = data.size();
    lb.rp = 0;

    entity *e = of::read(&lb, version);

    return e;
}

static void add_remote_entity(entity *e) {
    if (!e) return;

    applying = true;

    e->on_load(true, false);

    W->add(e);
    G->add_entity(e);

    e->construct();
    if (!W || W->is_paused()) e->on_pause();

    xform_cache c;
    b2Vec2 p = e->get_position();
    c.x = p.x;
    c.y = p.y;
    c.angle = e->get_angle();
    c.layer = e->get_layer();
    _xf[e->id] = c;

    applying = false;
}

/* ------------------------------------------------------------- avatars */

/* ------------------------------------------------------------- loadout */

/* The weapons of a character live in its *state*, and the entity data we
 * stream (of::write without write_states) does not carry states. A spawned
 * avatar therefore joined the round completely unarmed: the host called
 * creature::attack() on it and there was simply nothing to fire with, which
 * is why the second player could not shoot at all. The loadout is sent
 * explicitly with MSG_AVATAR instead and applied the same way on every
 * machine. */

static bool is_armable(entity *e) {
    return e && e->is_creature() && e->g_id == O_ROBOT;
}

static void read_loadout(entity *e, std::vector<uint8_t> *out, uint8_t *active) {
    out->clear();
    *active = (uint8_t)WEAPON_NULL;

    if (!is_armable(e)) return;

    creature *c = static_cast<creature*>(e);

    for (int x = 0; x < (int)c->num_weapons; ++x)
        if (c->weapons[x])
            out->push_back((uint8_t)c->weapons[x]->get_weapon_type());

    if (c->get_weapon())
        *active = (uint8_t)c->get_weapon()->get_weapon_type();
}

static void apply_loadout(entity *e, const std::vector<uint8_t> &w, uint8_t active) {
    if (!is_armable(e)) return;

    robot_base *r = static_cast<robot_base*>(static_cast<creature*>(e));

    /* add_weapon() ignores everything we already carry */
    for (size_t x = 0; x < w.size(); ++x)
        r->add_weapon(w[x]);

    if (active != (uint8_t)WEAPON_NULL)
        r->equip_weapon(active, false);
    else if (!w.empty())
        r->equip_weapon(w[0], false);
}

/* MSG_AVATAR can arrive before the entity it talks about (the spawn is a
 * separate message and the snapshot is applied in chunks), so an unresolved
 * loadout waits here instead of being dropped. */
struct pending_loadout {
    std::vector<uint8_t> weapons;
    uint8_t              active;
};

static std::map<uint32_t, pending_loadout> _loadout_wait;

static void flush_pending_loadouts() {
    if (_loadout_wait.empty() || !W) return;

    for (std::map<uint32_t, pending_loadout>::iterator it = _loadout_wait.begin();
         it != _loadout_wait.end(); ) {
        entity *e = W->get_entity_by_id(it->first);

        if (!e) { ++it; continue; }

        apply_loadout(e, it->second.weapons, it->second.active);
        _loadout_wait.erase(it++);
    }
}

/* What a joining player gets when there is nothing to copy from. The plasma
 * gun is a plain projectile weapon on purpose: a rocket launcher would let
 * the client side copy of a shot tear holes into the terrain of one machine
 * only. */
static void default_loadout(entity *e) {
    if (!is_armable(e) || !W) return;

    entity *ref = 0;

    if (_local_avatar) ref = W->get_entity_by_id(_local_avatar);

    if ((!ref || !is_armable(ref)) && W->is_adventure())
        ref = W->get_entity_by_id(W->level.get_adventure_id());

    std::vector<uint8_t> w;
    uint8_t active = (uint8_t)WEAPON_NULL;

    if (ref && ref != e) read_loadout(ref, &w, &active);

    if (w.empty()) {
        w.push_back((uint8_t)WEAPON_PLASMAGUN);
        active = (uint8_t)WEAPON_PLASMAGUN;
    }

    apply_loadout(e, w, active);
}

static void write_avatar_msg(buf *b, int peer_id, uint32_t ent, const char *name) {
    b->w_i32(peer_id);
    b->w_u32(ent);
    b->w_str(name);

    std::vector<uint8_t> w;
    uint8_t active = (uint8_t)WEAPON_NULL;

    read_loadout(W ? W->get_entity_by_id(ent) : 0, &w, &active);

    b->w_u8((uint8_t)w.size());

    for (size_t x = 0; x < w.size(); ++x)
        b->w_u8(w[x]);

    b->w_u8(active);
}

static entity *spawn_avatar(const char *name, b2Vec2 pos, int layer) {
    entity *e = of::create(O_ROBOT);

    if (!e) return 0;

    e->_angle = 0.f;
    e->_pos = pos;
    e->set_layer(layer);
    e->prio = 0;

    if (e->is_creature())
        static_cast<robot_base*>(e)->set_faction(FACTION_FRIENDLY);

    /* The level is streamed in chunks around the player, and everything in a
     * chunk that gets unloaded is removed from the world. When the host ran
     * far away, the chunk holding the other player was unloaded and the
     * second player simply vanished. Player characters must stay loaded no
     * matter where anyone else is. */
    e->set_flag(ENTITY_DISABLE_UNLOADING, true);

    e->ghost_update();
    e->on_load(true, false);

    W->add(e);
    G->add_entity(e);

    e->construct();
    if (!W || W->is_paused()) e->on_pause();

    /* an unarmed avatar can not shoot, no matter what its player presses */
    default_loadout(e);

    tms_infof("co-op: spawned avatar for %s (entity %u)", name, e->id);

    return e;
}

/* (Re)announce who controls which robot.
 *
 * MSG_AVATAR used to be sent exactly once, right after the welcome. But the
 * client rebuilds its level from the host's seed while those messages are
 * still sitting in its queue, and throwing a level away also throws that
 * queue away (on_world_teardown) - so the announcement was lost and the
 * client never learned which robot was its own: local_avatar=0, no camera,
 * no controls, and to the other player it looked like a robot walking off on
 * its own. The mapping is a handful of bytes, so it is simply repeated. */
static void send_avatars(peer *to) {
    if (mode != MODE_HOST) return;

    if (!_local_avatar) attach_local_player();

    std::vector<buf> msgs;

    if (_local_avatar) {
        buf a;
        write_avatar_msg(&a, local_peer_id, _local_avatar, local_name.c_str());
        msgs.push_back(a);
    }

    for (size_t x = 0; x < _peers.size(); ++x) {
        peer *o = _peers[x];

        if (!o || !o->ready || !o->avatar_id) continue;

        buf a;
        write_avatar_msg(&a, o->id, o->avatar_id, o->name.c_str());
        msgs.push_back(a);
    }

    for (size_t x = 0; x < msgs.size(); ++x) {
        if (to) {
            if (to->c && to->c->valid()) to->c->send(MSG_AVATAR, msgs[x]);
        } else
            broadcast(MSG_AVATAR, msgs[x]);
    }
}

/* ------------------------------------------------------------ snapshots */

static void send_snapshot(peer *p) {
    if (!p || !p->c || !W) return;

    p->c->send_empty(MSG_SNAPSHOT_BEGIN);

    int count = 0;

    for (std::map<uint32_t, entity*>::iterator it = W->all_entities.begin();
         it != W->all_entities.end(); ++it) {
        entity *e = it->second;
        if (!e || !is_streamable(e)) continue;

        buf b;
        if (write_entity(&b, e)) {
            p->c->send(MSG_SPAWN, b);
            ++count;
        }
    }

    /* connections */
    for (std::set<connection*>::iterator it = W->connections.begin();
         it != W->connections.end(); ++it) {
        connection *c = *it;
        if (!c || !c->e || !c->o) continue;

        buf b;
        b.w_u32(c->e->id);
        b.w_u32(c->o->id);
        b.w_u8(c->type);
        b.w_u8(c->f[0]);
        b.w_u8(c->f[1]);
        b.w_f(c->p.x);
        b.w_f(c->p.y);
        b.w_i32(c->layer);
        b.w_f(c->max_force);
        b.w_f(c->damping);
        b.w_f(c->angle);
        b.w_i32(c->option);
        b.w_u8(c->render_type);
        write_conn_xforms(&b, c->e, c->o);

        p->c->send(MSG_CONNECT, b);
    }

    p->c->send_empty(MSG_SNAPSHOT_END);

    tms_infof("co-op: sent snapshot with %d objects to peer %d", count, p->id);
}

/* The anchor point of a connection is stored in the local frame of the
 * first object, so both objects have to sit exactly where the player who
 * made the connection saw them. A few centimetres of drift (the transform
 * sync runs at its own rate) is what put the wheels on the side wall
 * instead of under the platform on the other side. */
static void write_conn_xforms(buf *b, entity *e, entity *o) {
    for (int x = 0; x < 2; ++x) {
        entity *t = (x == 0 ? e : o);
        b2Vec2 p = t->get_position();

        b->w_f(p.x);
        b->w_f(p.y);
        b->w_f(t->get_angle());
        b->w_i32(t->get_layer());
    }
}

static void snap_before_connect(entity *e, float x, float y, float a, int layer) {
    if (!e || !G) return;

    /* the group owns the transform of its members */
    if (e->gr) return;

    /* do not yank an object out of the other player's hands */
    if (G->selection.e == e) return;

    if (e->get_layer() != layer)
        e->set_layer(layer);

    e->set_position(x, y);
    e->set_angle(a);
    e->update();

    xform_cache c;
    c.x = x;
    c.y = y;
    c.angle = a;
    c.layer = layer;
    _xf[e->id] = c;
}

static void apply_connect(buf *b) {
    uint32_t e_id = b->r_u32();
    uint32_t o_id = b->r_u32();
    uint8_t  type = b->r_u8();
    uint8_t  f0   = b->r_u8();
    uint8_t  f1   = b->r_u8();
    float    px   = b->r_f();
    float    py   = b->r_f();
    float    psx  = b->r_f();
    float    psy  = b->r_f();
    int32_t  layer = b->r_i32();
    float    max_force = b->r_f();
    float    damping = b->r_f();
    float    angle = b->r_f();
    int32_t  option = b->r_i32();
    uint8_t  render_type = b->r_u8();

    float    exf[2][3];
    int32_t  elayer[2];

    for (int x = 0; x < 2; ++x) {
        exf[x][0] = b->r_f();
        exf[x][1] = b->r_f();
        exf[x][2] = b->r_f();
        elayer[x] = b->r_i32();
    }

    if (b->err || !W || !G) return;

    entity *e = W->get_entity_by_id(e_id);
    entity *o = W->get_entity_by_id(o_id);

    if (e && o) {
        applying = true;
        snap_before_connect(e, exf[0][0], exf[0][1], exf[0][2], (int)elayer[0]);
        snap_before_connect(o, exf[1][0], exf[1][1], exf[1][2], (int)elayer[1]);
        applying = false;
    }

    if (!e || !o) {
        tms_debugf("co-op: could not apply connection %u<->%u, missing object", e_id, o_id);
        return;
    }

    if (e->connected_to(o)) return;

    /* Both objects already belong to the same welded group: applying the
     * connection again makes the group rebuild itself with nothing left in
     * it ("REMOVING SELF" in the log) and the game crashed right there. */
    if (e->gr && e->gr == o->gr && !W->is_paused()) {
        tms_debugf("co-op: %u and %u are already in the same group", e_id, o_id);
        return;
    }

    /* Welding two *existing* groups together merges them, and the group that
     * loses frees itself while the engine is still walking it. That is the
     * same "REMOVING SELF" crash, so we leave the local structures alone
     * instead of taking the game down. */
    if (e->gr && o->gr && !W->is_paused()) {
        tms_debugf("co-op: not merging the groups of %u and %u", e_id, o_id);
        return;
    }

    connection *c = G->get_tmp_conn();
    if (!c) return;

    c->reset();
    c->owned = false;
    c->e = e;
    c->o = o;
    c->type = type;
    c->f[0] = f0;
    c->f[1] = f1;
    c->p = b2Vec2(px, py);
    c->p_s = b2Vec2(psx, psy);
    c->layer = layer;
    c->max_force = max_force;
    c->damping = damping;
    c->angle = angle;
    c->render_type = render_type;

    applying = true;
    G->apply_connection(c, option);
    applying = false;
}

/* game::delete_entity does NOT clear the camera target, the current
 * selection or the multi-selection set, because in single player the only
 * caller already took care of that. Co-op deletes objects behind the
 * player's back (a peer leaving, the host removing an object, a settings
 * update re-creating one), so every dangling pointer has to be cut here or
 * the next frame dereferences freed memory -> segfault. */
static void safe_delete_entity(entity *e) {
    if (!e || !W || !G) return;

    uint32_t id = e->id;

    bool selected = (G->selection.e == e)
            || (G->selection.m && G->selection.m->count(e));

    if (selected) {
        /* selection_handler::disable() deletes the multi-selection set but
         * leaves m_saved dangling, so drop the saved state as well - never
         * dereference it after the set is gone. */
        G->selection.m_saved = 0;
        G->selection.e_saved = 0;
        G->selection.c_saved = 0;

        G->selection.disable(false);

        G->selection.e = 0;
        G->selection.b = 0;
    }

    if (G->selection.e_saved == e) {
        G->selection.e_saved = 0;
        G->selection.m_saved = 0;
    }

    /* Deleting a welded member can take the whole group with it (a group with
     * less than two members removes itself). Anything still pointing at that
     * group has to be cleared as well. */
    if (e->gr) {
        entity *g = (entity*)e->gr;

        if (G->selection.e == g || (G->selection.m && G->selection.m->count(g))) {
            G->selection.m_saved = 0;
            G->selection.e_saved = 0;
            G->selection.c_saved = 0;
            G->selection.disable(false);
            G->selection.e = 0;
            G->selection.b = 0;
        }

        if (G->selection.e_saved == g) {
            G->selection.e_saved = 0;
            G->selection.m_saved = 0;
        }

        if (G->follow_object == g)
            G->follow_object = 0;
    }

    if (adventure::player == e)
        adventure::player = 0;

    bool was_followed = (G->follow_object == e);

    if (was_followed)
        G->follow_object = 0;

    applying = true;
    G->delete_entity(e);
    applying = false;

    _xf.erase(id);
    _sig.erase(id);
    _awake.erase(id);
    _remote_touch.erase(id);

    if (id == _local_avatar)
        _local_avatar = 0;

    _avatars.erase(id);

    /* the camera lost its target: fall back to our own character */
    if (was_followed) {
        entity *own = _local_avatar ? W->get_entity_by_id(_local_avatar) : 0;

        if (own) G->set_follow_object(own, true);
    }
}

static void apply_xform(buf *b) {
    uint32_t id = b->r_u32();
    float x = b->r_f();
    float y = b->r_f();
    float a = b->r_f();
    int32_t layer = b->r_i32();

    if (b->err || !W) return;

    entity *e = W->get_entity_by_id(id);
    if (!e) return;

    /* The transform of an object welded into a group belongs to the group.
     * Moving the member itself (or changing its layer) makes the engine
     * rebuild the group in the middle of our message handling, and a group
     * that ends up with a single member frees itself right there
     * ("WARNING: REMOVING SELF") -> segfault. */
    if (e->gr) {
        _remote_touch[id] = now_sec();
        return;
    }

    applying = true;

    if (e->get_layer() != (int)layer)
        e->set_layer((int)layer);

    e->set_position(x, y);
    e->set_angle(a);

    if (!W->is_paused()) {
        for (int bi = 0; bi < (int)e->get_num_bodies(); ++bi) {
            b2Body *bd = e->get_body(bi);
            if (!bd) continue;
            bd->SetLinearVelocity(b2Vec2(0.f, 0.f));
            bd->SetAngularVelocity(0.f);
            bd->SetAwake(true);
        }
    }

    e->update();

    xform_cache c;
    c.x = x;
    c.y = y;
    c.angle = a;
    c.layer = (int)layer;
    _xf[id] = c;

    /* another player is holding this object: do not fight them with our own
     * settings scanner (that used to bounce the object back and steal the
     * selection from whoever was dragging it) */
    _remote_touch[id] = now_sec();

    applying = false;
}

/* MSG_DISCONNECT used to be sent but never handled, so breaking a joint was
 * only visible on the side that did it: one player saw a loose plank while
 * the other still had it welded on. */
static void apply_disconnect(buf *b) {
    uint32_t a_id = b->r_u32();
    uint32_t b_id = b->r_u32();

    if (b->err || !W || !G) return;

    entity *e = W->get_entity_by_id(a_id);
    entity *o = W->get_entity_by_id(b_id);

    if (!e || !o || !e->conn_ll) return;

    applying = true;

    connection *c = e->conn_ll;

    while (c) {
        connection *next = c->next[(c->e == e) ? 0 : 1];
        entity *other = (c->e == e) ? c->o : c->e;

        if (other == o && !c->fixed) {
            /* the editor keeps a pointer to the selected connection */
            if (G->selection.c == c) {
                G->selection.c_saved = 0;
                G->selection.m_saved = 0;
                G->selection.e_saved = 0;
                G->selection.disable(false);
            }

            if (G->selection.c_saved == c) G->selection.c_saved = 0;

            c->e->destroy_connection(c);
            break;
        }

        c = next;
    }

    applying = false;
}

static void apply_delete(buf *b) {
    uint32_t id = b->r_u32();

    if (b->err || !W || !G) return;

    entity *e = W->get_entity_by_id(id);
    if (!e) return;

    /* A welded member is only removed in build mode, from the deferred
     * queue - exactly like the editor's own delete. In a running round the
     * group is still being simulated, so it is left alone there. */
    if (e->gr && !W->is_paused()) {
        tms_debugf("co-op: not deleting welded object %u during the round", id);
        return;
    }

    safe_delete_entity(e);
}


/* ======================================================================== */
/*                     co-op play mode (synced round)                        */
/* ======================================================================== */

static bool        _session_playing = false;
static bool        _local_ready = false;
static bool        _local_stop_ready = false;  /* we asked to go back to build mode */
static int         _pending_play = 0;      /* 1 = start round, 2 = stop round */
static double      _last_state_sync = 0.0;
static double      _last_avatar_sync = 0.0;
static double      _last_input_sync = 0.0;

/* Layer switching: the keys belong to the game (it turns them into
 * adventure::pending_layermove). A client latches the request here, forwards
 * it to the host and never performs the move itself. */
static int8_t      _layer_req = 0;

/* One pending layer switch per character on the host. A switch can fail for
 * a few frames (something is standing in the target layer, the character is
 * mid-air, ...), exactly like in single player, so it is retried instead of
 * being applied twice. */
struct layer_request {
    int dir;
    int attempts;
};

static std::map<uint32_t, layer_request> _lreq;

/* true while the chunk window generates/frees terrain */
static bool        _chunk_loading = false;
static input_state _local_input;
static input_state _local_input_prev;
static input_state _local_input_sent;

bool session_playing() { return _session_playing && mode != MODE_OFF; }
uint32_t local_avatar_id() { return _local_avatar; }

/* The client runs the game logic itself and is corrected by the host
 * (mp::sync). A pure viewer could only move as often as a packet arrived,
 * which is exactly the lag that made walking feel stuttery. */
bool suppress_local_sim() {
    return false;
}

/* A client that also stepped its own physics produced a second, diverging
 * world (objects resting in different places, robots falling through floors
 * the host still had). It is a pure viewer now. */
bool suppress_local_physics() {
    /* local Box2D keeps running for looks; mp::sync::recv_tick() blends
     * it back towards the authoritative state of the host */
    return false;
}

static void announce(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    buf b;
    b.w_str("[co-op]");
    b.w_str(line);
    broadcast(MSG_CHAT, b);

    hud_add("%s", line);
}

/* ------------------------------------------------------------ ready gate */

static void host_check_ready() {
    if (mode != MODE_HOST) return;

    int total = 1;
    int ready = _local_ready ? 1 : 0;

    for (size_t x = 0; x < _peers.size(); ++x) {
        peer *p = _peers[x];
        if (!p->ready) continue;
        ++total;
        if (p->wants_play) ++ready;
    }

    if (ready >= total) {
        /* a round start reloads the level on every machine, so make sure
         * everybody knows its own character before that happens */
        send_avatars(0);

        buf b;
        broadcast(MSG_START_PLAY, b);

        announce("All players are ready - starting the level!");

        for (size_t x = 0; x < _peers.size(); ++x) _peers[x]->wants_play = false;

        _local_ready = false;
        _pending_play = 1;
    } else {
        announce("%d of %d players are ready to start", ready, total);
    }
}

bool request_play() {
    if (mode == MODE_OFF) return false;
    if (_session_playing) return false;   /* already running */

    if (mode == MODE_HOST) {
        if (_peers.empty()) {
            /* nobody else connected yet, just start */
            _pending_play = 1;
            return true;
        }

        if (!_local_ready) {
            _local_ready = true;
            announce("%s is ready to start", local_name.c_str());
        }

        host_check_ready();
        return true;
    }

    if (!_local_ready) {
        _local_ready = true;

        buf b;
        broadcast(MSG_READY, b);

        announce("%s is ready to start", local_name.c_str());
        hud_add("Waiting for the other players...");
    }

    return true;
}

/* Everyone has to agree before the round ends, otherwise one player tears
 * the running level down under the other player's feet - which used to take
 * the whole game down with it. */
static void host_check_stop() {
    if (mode != MODE_HOST) return;

    int total = 1;
    int ready = _local_stop_ready ? 1 : 0;

    for (size_t x = 0; x < _peers.size(); ++x) {
        peer *p = _peers[x];
        if (!p || !p->ready) continue;
        ++total;
        if (p->wants_stop) ++ready;
    }

    if (ready >= total) {
        buf b;
        broadcast(MSG_STOP_PLAY, b);

        announce("Everyone is done - back to build mode!");

        for (size_t x = 0; x < _peers.size(); ++x) {
            if (_peers[x]) _peers[x]->wants_stop = false;
        }

        _local_stop_ready = false;
        _pending_play = 2;
    } else {
        announce("%d of %d players want to go back to build mode", ready, total);
    }
}

bool request_stop() {
    if (mode == MODE_OFF) return false;

    /* the round is being torn down right now (process_pending_play) - let the
     * game do its work */
    if (_loading || applying) return false;

    if (!_session_playing) return false;  /* not playing, nothing to stop */

    if (mode == MODE_HOST) {
        if (_peers.empty()) {
            _pending_play = 2;
            return true;
        }

        if (!_local_stop_ready) {
            _local_stop_ready = true;
            announce("%s wants to go back to build mode", local_name.c_str());
        }

        host_check_stop();
        return true;
    }

    if (!_local_stop_ready) {
        _local_stop_ready = true;

        buf b;
        broadcast(MSG_STOP_READY, b);

        announce("%s wants to go back to build mode", local_name.c_str());
        hud_add("Waiting for the other players...");
    }

    return true;
}

static void process_pending_play() {
    if (!_pending_play || !G || !W) return;

    int action = _pending_play;
    _pending_play = 0;

    _resync_requests = 0;

    _local_ready = false;
    _local_stop_ready = false;

    for (size_t x = 0; x < _peers.size(); ++x) {
        if (!_peers[x]) continue;

        _peers[x]->wants_play = false;
        _peers[x]->wants_stop = false;
    }

    if (action == 1) {
        _session_playing = true;
        _local_input = input_state();
        _local_input_prev = input_state();
        _local_input_sent = input_state();

        /* a key that was held down when the previous round ended must not
         * make a character jump or shoot the moment the next one starts */
        for (size_t x = 0; x < _peers.size(); ++x) {
            if (!_peers[x]) continue;

            _peers[x]->input = input_state();
            _peers[x]->prev_input = input_state();
        }

        if (W->is_paused()) {
            _level_ids.clear();
            for (std::map<uint32_t, entity*>::iterator it = W->all_entities.begin();
                    it != W->all_entities.end(); ++it)
                _level_ids.insert(it->first);

            _loading = true;
            G->do_play();
            _loading = false;
        }

        /* follow our own robot during the round */
        if (_local_avatar) {
            entity *av = W->get_entity_by_id(_local_avatar);

            if (av && av->is_creature()) {
                if (W->is_adventure()) {
                    G->state.adventure_id = av->id;
                    adventure::set_player(static_cast<creature*>(av), true);
                } else {
                    G->set_follow_object(av, true);
                }
            }
        }
    } else {
        _session_playing = false;

        _local_input = input_state();
        _local_input_prev = input_state();
        _local_input_sent = input_state();

        for (size_t x = 0; x < _peers.size(); ++x) {
            if (!_peers[x]) continue;

            _peers[x]->input = input_state();
            _peers[x]->prev_input = input_state();
            _peers[x]->wants_play = false;
        }

        if (!W->is_paused()) {
            _loading = true;
            G->do_pause();
            _loading = false;
        }

        /* everything may have moved during the round, resync the room */
        _xf.clear();
        _gxf.clear();
        _awake.clear();
        _remote_touch.clear();
        _sig.clear();

        /* a layer switch that was still queued belongs to the round that
         * just ended */
        _lreq.clear();
        _layer_req = 0;

        if (mode == MODE_CLIENT && _server.valid()) {
            buf r;
            _server.send(MSG_REQUEST_SNAPSHOT, r);
        }
    }
}

/* -------------------------------------------------------------- input */

static input_state read_local_input() {
    input_state in;
    in.look_dir = _local_input.look_dir;

    if (!G || ui::is_blocking()) return in;

    const auto *ks = SDL_GetKeyboardState(0);
    if (!ks) return in;

    bool left  = ks[SDL_SCANCODE_A] || ks[SDL_SCANCODE_LEFT];
    bool right = ks[SDL_SCANCODE_D] || ks[SDL_SCANCODE_RIGHT];

    if (left && !right) in.move_dir = -1;
    else if (right && !left) in.move_dir = 1;
    else in.move_dir = 0;

    if (in.move_dir) in.look_dir = in.move_dir;

    in.jump   = ks[SDL_SCANCODE_SPACE];
    in.up     = ks[SDL_SCANCODE_W] || ks[SDL_SCANCODE_UP];
    in.down   = ks[SDL_SCANCODE_S] || ks[SDL_SCANCODE_DOWN];
    in.attack = ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL] || ks[SDL_SCANCODE_F];
    in.action = ks[SDL_SCANCODE_E];

    /* Layer switching deliberately has no keys of its own: the game turns
     * whatever the player has bound (keyboard or touch) into
     * adventure::pending_layermove, so we read that. A client must not
     * perform the switch locally (it does not simulate during a round), it
     * only forwards the request to the host. */
    in.layer_req = _layer_req;

    /* Principia shoots with the pointer, not with a key, so the mouse button
     * has to count as "attack" as well - otherwise a joining player simply
     * can not fire. */
    {
        float mx = 0.f, my = 0.f;

        if ((SDL_GetMouseState(&mx, &my) & SDL_BUTTON_LMASK) && !adventure::mining)
            in.attack = true;
    }

    /* the zapper (mining tool) is driven by the game's own pointer handling;
     * it is forwarded so the host can mine for this player */
    in.mining = adventure::mining;
    in.mine_x = adventure::mining_pos.x;
    in.mine_y = adventure::mining_pos.y;

    /* The local aiming (mouse movement, aim keys) is handled by the game
     * itself and ends up in our own robot's weapon angle. Read it back and
     * ship it with the input so the host shoots in the direction we are
     * actually pointing at. */
    if (W && _local_avatar) {
        entity *ae = W->get_entity_by_id(_local_avatar);

        if (ae && ae->is_creature()) {
            creature *ac = static_cast<creature*>(ae);

            if (ac->get_weapon()) in.aim = ac->get_weapon()->get_arm_angle();
            if (ac->look_dir) in.look_dir = (int8_t)ac->look_dir;
        }
    }

    return in;
}

/* ------------------------------------------------- animation start/stop */

/* The state stream carries the *values* of an animation (timers, blend), but
 * it is sampled, so a short animation could start on one machine and never
 * start on another. These events carry the transitions themselves. */
static void send_anim(uint8_t code, uint32_t entity_id, uint8_t phase, float param = 0.f) {
    if (mode == MODE_OFF || applying || _loading || !entity_id) return;

    buf b;
    b.w_u8(code);
    b.w_u8(phase);
    b.w_u32(entity_id);
    b.w_f(param);

    broadcast(MSG_ANIM, b);
}

static void apply_anim(buf *b) {
    uint8_t  code  = b->r_u8();
    uint8_t  phase = b->r_u8();
    uint32_t id    = b->r_u32();
    float    param = b->r_f();

    if (b->err || !W || !G || !id) return;

    /* our own character is driven by our own input (predicted locally);
     * only the layer switch is decided by the host */
    if (id == _local_avatar && (mode == MODE_HOST || code != MP_ANIM_LAYERMOVE))
        return;

    entity *e = W->get_entity_by_id(id);
    if (!e || !e->is_creature()) return;

    creature *cr = static_cast<creature*>(e);

    applying = true;

    switch (code) {
        case MP_ANIM_ATTACK:
            /* the shot itself arrives as sync::MSG_SHOT, this event only
             * lowers the arm again when the trigger is released */
            if (phase == MP_PHASE_STOP)
                cr->attack_stop();
            break;

        case MP_ANIM_JUMP:
            if (phase == MP_PHASE_START) cr->jump(false);
            break;

        case MP_ANIM_LAYERMOVE:
            /* Start the transition in the same frame as the host. The host
             * already validated the move (ladders, blocked layers, feet), so
             * the target layer is adopted as is; the blend values that follow
             * in the state stream animate it. */
            if (phase == MP_PHASE_START) {
                int target = (int)(param + .5f);

                if (target < 0) target = 0;
                if (target > 2) target = 2;

                if (cr->get_layer() != target) {
                    int d = target - cr->get_layer();

                    if ((d != 1 && d != -1) || !cr->layermove(d)) {
                        cr->layer_old = cr->layer_new;
                        cr->layer_blend = 0.f;
                        cr->set_layer(target);
                    }
                }
            }
            break;

        case MP_ANIM_ACTION:
            if (phase == MP_PHASE_START) {
                if (!cr->is_action_active()) cr->action_on();
            } else {
                if (cr->is_action_active()) cr->action_off();
            }
            break;
    }

    applying = false;
}

/* Remember a layer switch instead of performing it right away. */
static void request_layermove(uint32_t avatar_id, int dir) {
    if (mode != MODE_HOST || !avatar_id || !dir) return;

    layer_request r;
    r.dir = dir > 0 ? 1 : -1;
    r.attempts = 0;

    /* one request per character: holding the key must not queue up three
     * switches and throw the player through the whole level */
    _lreq[avatar_id] = r;
}

/* Runs once per frame on the host. */
static void process_layer_requests() {
    if (mode != MODE_HOST || _lreq.empty() || !W) return;

    std::vector<uint32_t> done;

    for (std::map<uint32_t, layer_request>::iterator it = _lreq.begin();
         it != _lreq.end(); ++it) {

        entity *e = W->get_entity_by_id(it->first);

        if (!e || !e->is_creature()) {
            done.push_back(it->first);
            continue;
        }

        creature *cr = static_cast<creature*>(e);

        /* wait for the previous transition to finish, otherwise two
         * switches collapse into one jump of two layers */
        if (cr->motion != MOTION_DEFAULT) {
            if (++it->second.attempts > MP_LAYERMOVE_ATTEMPTS * 4)
                done.push_back(it->first);
            continue;
        }

        if (cr->layermove(it->second.dir)) {
            send_anim(MP_ANIM_LAYERMOVE, it->first, MP_PHASE_START,
                      (float)cr->layer_new);
            done.push_back(it->first);
        } else if (++it->second.attempts > MP_LAYERMOVE_ATTEMPTS) {
            /* blocked for half a second - drop it, just like the game does */
            done.push_back(it->first);
        }
    }

    for (size_t x = 0; x < done.size(); ++x)
        _lreq.erase(done[x]);
}

class mp_mine_cb : public b2RayCastCallback {
  public:
    mp_mine_cb(entity *self, int layer) : self(self), layer(layer), result(0), fx(0), udata2(0) {}

    entity    *self;
    int        layer;
    entity    *result;
    b2Fixture *fx;
    void      *udata2;
    b2Vec2     pt;

    float32 ReportFixture(b2Fixture *f, const b2Vec2 &p, const b2Vec2 &nor, float32 fraction) {
        if (f->IsSensor()) return -1.f;

        entity *r = static_cast<entity*>(f->GetUserData());
        if (!r || r == self) return -1.f;
        if (!world::fixture_in_layer(f, layer)) return -1.f;
        if (r->g_id == O_RESOURCE) return -1.f;

        bool ok = (r->g_id == O_TPIXEL || r->g_id == O_CHUNK || r->is_creature()
                   || (r->is_zappable() && r->g_id != O_PLANT && r->g_id != O_ITEM));
        if (!ok) return -1.f;

        result = r;
        fx = f;
        udata2 = f->GetUserData2();
        pt = p;
        return fraction;
    }
};

static std::map<uint32_t, double> _mine_t;

static void host_remote_mine(creature *c, const input_state &in) {
    if (mode != MODE_HOST || !_session_playing || !in.mining || !c || !W || !G) return;

    robot_parts::tool *t = c->get_tool();
    if (!t || t->get_arm_type() != TOOL_ZAPPER) return;

    double now = now_sec();
    double &last = _mine_t[c->id];
    if (now - last < 0.075) return;
    last = now;

    float dmg = static_cast<robot_parts::miner*>(t)->damage;

    b2Vec2 start = c->get_position();
    b2Vec2 end(in.mine_x, in.mine_y);
    b2Vec2 d = end - start;
    float len = d.Length();
    if (len < 0.01f) return;
    if (len > 2.5f) end = start + (2.5f / len) * d;

    mp_mine_cb cb(c, c->get_layer());
    W->raycast(&cb, start, end);
    if (!cb.result) return;

    if (cb.result->is_creature()) {
        static_cast<creature*>(cb.result)->damage(dmg, cb.fx, DAMAGE_TYPE_ELECTRICITY,
                                                   DAMAGE_SOURCE_BULLET, c->id);
    } else if (cb.result->g_id == O_TPIXEL || cb.result->g_id == O_CHUNK) {
        G->damage_tpixel(cb.result, cb.fx, cb.udata2, dmg, cb.pt, DAMAGE_TYPE_ELECTRICITY);
    } else {
        cb.result->entity_damage(dmg);
    }
}

struct terrain_px { int32_t cx, cy; uint8_t layer, x, y; };
static std::vector<terrain_px> _terrain_pending;

static bool apply_terrain_px(const terrain_px &t) {
    if (!W || !W->cwindow) return false;
    if (t.layer > 2 || t.x > 15 || t.y > 15) return true; /* garbage: drop */

    level_chunk *c = W->cwindow->get_chunk(t.cx, t.cy, true);
    if (!c) return false; /* not loaded here yet: retry later */

    if (!c->pixels[t.layer][t.y][t.x]) return true; /* already gone */

    int z = t.layer;
    int lx = t.x, ly = t.y, sz = 1;
    int limit = std::min(16*16, (int)c->num_merged[z]);

    for (int m = 0; m < limit; ++m) {
        tpixel_desc &d = c->merged[z][m];
        if (d.hp <= 0.f) continue;

        int dx = d.get_local_x(), dy = d.get_local_y(), ds = 1 << d.size;
        if (t.x >= dx && t.x < dx + ds && t.y >= dy && t.y < dy + ds) {
            d.hp = -1.f;
            if (m < c->min_merged[z]) c->min_merged[z] = m;
            lx = dx; ly = dy; sz = ds;
            break;
        }
    }

    c->pixels[z][t.y][t.x] = 0;
    c->merge(lx, ly, z, lx + sz, ly + sz, z + 1);
    W->to_be_reloaded.insert(c);
    return true;
}

static void process_terrain_pending() {
    if (_terrain_pending.empty() || mode != MODE_CLIENT) return;

    std::vector<terrain_px> keep;
    for (size_t x = 0; x < _terrain_pending.size(); ++x)
        if (!apply_terrain_px(_terrain_pending[x]))
            keep.push_back(_terrain_pending[x]);

    if (keep.size() > 20000) keep.erase(keep.begin(), keep.begin() + (keep.size() - 20000));
    _terrain_pending.swap(keep);
}

void on_terrain_pixel(int cx, int cy, int layer, int x, int y) {
    if (mode != MODE_HOST || applying || !_session_playing) return;

    buf b;
    b.w_i32(cx);
    b.w_i32(cy);
    b.w_u8((uint8_t)layer);
    b.w_u8((uint8_t)x);
    b.w_u8((uint8_t)y);
    broadcast(MSG_TERRAIN, b);
}

static void apply_input(uint32_t avatar_id, const input_state &in, input_state *prev) {
    if (!W || !avatar_id) return;

    entity *e = W->get_entity_by_id(avatar_id);
    if (!e || !e->is_creature()) return;

    creature *c = static_cast<creature*>(e);

    /* the host's own robot mines through adventure.cc */
    if (avatar_id != _local_avatar) host_remote_mine(c, in);

    /* remote players aim on their own machine; our own robot is aimed by the
     * game itself, so it must not be overridden here */
    bool remote = (avatar_id != _local_avatar);

    /* in.aim is the raw arm angle of the owner's weapon - the old code mixed
     * it with get_aim(), a different value, so the arm jumped between two
     * angles every frame */
    if (remote && c->get_weapon()) c->get_weapon()->set_arm_angle_raw(in.aim);

    if (in.move_dir) {
        c->move((int)in.move_dir);
        if (!remote) c->look((int)in.move_dir);
    } else {
        c->stop();
    }

    /* the owner's robot faces the mouse, not only the walking direction */
    if (remote) {
        int want = in.look_dir ? (int)in.look_dir : (int)in.move_dir;
        if (want && c->look_dir != want) c->look(want, true);
    }

    /* ladders: the game itself does move(DIR_UP/DOWN) for the host's own
     * robot; a remote robot used to get a jump instead, which threw it off
     * the ladder every time the client pressed up */
    if (avatar_id != _local_avatar) {
        bool pu = prev && prev->up, pd = prev && prev->down;
        if (in.up && !pu) c->move(DIR_UP);
        else if (!in.up && pu) c->stop_moving(DIR_UP);
        if (in.down && !pd) c->move(DIR_DOWN);
        else if (!in.down && pd) c->stop_moving(DIR_DOWN);
    }

    if (in.jump && (!prev || !prev->jump)) {
        c->jump(false);

        if (mode == MODE_HOST)
            send_anim(MP_ANIM_JUMP, avatar_id, MP_PHASE_START);
    }

    /* --- layer switching (the level has three of them) ---
     *
     * Only the host performs the switch: it owns the physics and a layer
     * change rebuilds every fixture of the character. Everyone else receives
     * the result, so the transition starts and ends in the same frame on
     * every machine. */
    if (in.layer_req && (!prev || !prev->layer_req) && mode == MODE_HOST) {
        /* our own character is moved by the game itself
         * (adventure::pending_layermove); doing it twice here is exactly
         * what used to teleport the player to the first or the last layer */
        /* Every player performs its own layer switch now (both sides
         * simulate), tells the others with MSG_LAYER and gets the real
         * layermove() animation locally. Doing it a second time here threw
         * the character across two layers at once. */
        /* only the HOST's own character is moved by adventure.cc; every
         * remote player's switch is performed right here */
        bool handled_by_game = (W->is_adventure() && avatar_id == _local_avatar);

        if (!handled_by_game)
            request_layermove(avatar_id, (int)in.layer_req);
    }

    if (in.attack && !in.mining) {
        /* If this ever fires the player is holding nothing, and no amount of
         * pressing the trigger will produce a shot (see default_loadout()).
         * Throttled, because the key is held down. */
        /* The shot itself is not announced from here any more: only the
         * weapon knows whether the trigger actually produced a bullet, so
         * sync::watch_shots() picks it up from the cooldown. This also
         * covers the host's own character, which the game drives itself. */
        if (!c->get_weapon()) {
            static double last_warn = 0.0;
            double t = now_sec();

            if (t - last_warn > 2.0) {
                last_warn = t;
                tms_infof("co-op: entity %u pressed attack but carries no weapon",
                          avatar_id);
            }
        }

        /* Only the owner of a character pulls its trigger. A remote shot
         * arrives as MSG_SHOT and is replayed there - firing here as well
         * produced two bullets per click on the host. */
        /* HOST AS AUTHORITY: apply_input() only runs on the host, so this is
         * the real trigger pull for every player it drives. The shot is then
         * announced by sync::watch_shots() (cooldown edge -> MSG_SHOT). */
        c->attack();

        (void)prev;
    } else if (prev && prev->attack) {
        c->attack_stop();

        /* the end of an animation matters as much as its start: without this
         * a remote robot kept its arm raised forever */
        if (mode == MODE_HOST)
            send_anim(MP_ANIM_ATTACK, avatar_id, MP_PHASE_STOP);
    }

    if (in.action && (!prev || !prev->action)) {
        bool was_active = c->is_action_active();

        if (was_active) c->action_off();
        else c->action_on();

        if (mode == MODE_HOST)
            send_anim(MP_ANIM_ACTION, avatar_id,
                      was_active ? MP_PHASE_STOP : MP_PHASE_START);
    }
}

/* Put the given tool/weapon in a robot's hands. Only tools the robot really
 * owns can be equipped, the engine refuses anything else. */
static void apply_tools(uint32_t id, int tool, int weapon) {
    if (!W) return;

    entity *e = W->get_entity_by_id(id);
    if (!e || !e->is_robot()) return;

    robot_base *r = static_cast<robot_base*>(e);

    bool was = applying;
    applying = true;

    if (tool >= 0 && r->get_tool_type() != tool)
        if (!r->equip_tool((uint8_t)tool, false))
            tms_debugf("co-op: robot %u can not equip tool %d", id, tool);

    if (weapon >= 0 && r->get_weapon_type() != weapon)
        if (!r->equip_weapon((uint8_t)weapon, false))
            tms_debugf("co-op: robot %u can not equip weapon %d", id, weapon);

    applying = was;
}

static void send_tool(uint32_t id, entity *e) {
    creature *c = static_cast<creature*>(e);

    buf b;
    b.w_u32(id);
    b.w_u8((uint8_t)(int8_t)c->get_tool_type());
    b.w_u8((uint8_t)(int8_t)c->get_weapon_type());
    broadcast(MSG_TOOL, b);
}

/* host: tell everyone what every player robot holds */
static void sync_tools() {
    if (mode != MODE_HOST || !W || !_session_playing || W->is_paused()) return;

    double t = now_sec();
    bool resend = t - _tool_resend > 1.0;
    if (resend) _tool_resend = t;

    std::vector<entity*> robots;
    if (adventure::player) robots.push_back((entity*)adventure::player);

    for (size_t x = 0; x < _peers.size(); ++x) {
        if (!_peers[x] || !_peers[x]->avatar_id) continue;
        entity *e = W->get_entity_by_id(_peers[x]->avatar_id);
        if (e) robots.push_back(e);
    }

    for (size_t x = 0; x < robots.size(); ++x) {
        entity *e = robots[x];
        if (!e->is_creature()) continue;

        creature *c = static_cast<creature*>(e);
        int key = ((c->get_tool_type() & 0xff) << 8) | (c->get_weapon_type() & 0xff);

        std::map<uint32_t, int>::iterator it = _tool_sent.find(e->id);
        if (!resend && it != _tool_sent.end() && it->second == key) continue;

        _tool_sent[e->id] = key;
        send_tool(e->id, e);
    }
}

/* client: remove what our own simulation produced (see on_runtime_spawn) */
static void process_client_doomed() {
    if (mode != MODE_CLIENT || !W || _client_doomed.empty()) return;

    std::set<uint32_t> ids;
    ids.swap(_client_doomed);

    if (!_session_playing || W->is_paused() || _loading) return;

    int removed = 0;

    for (std::set<uint32_t>::iterator it = ids.begin(); it != ids.end(); ++it) {
        entity *e = W->get_entity_by_id(*it);
        if (!e) continue;
        if (e->gr) continue;                         /* welded: unsafe */
        if (is_player_avatar(e->id)) continue;
        if (e == (entity*)adventure::player) continue;
        if (G && G->interacting_with(e)) continue;   /* in the player's hand */

        applying = true;
        safe_delete_entity(e);
        applying = false;
        ++removed;
    }

    if (removed)
        tms_debugf("co-op: removed %d locally simulated objects, the host owns them", removed);
}

static void send_local_input() {
    if (mode != MODE_CLIENT || !_session_playing) return;

    input_state in = read_local_input();

    double t = now_sec();
    bool changed = in.differs(_local_input_sent);

    /* changes go out immediately (key presses must not wait up to 33 ms),
     * unchanged input is resent as a slow heartbeat */
    if (!changed && t - _last_input_sync < 0.25) return;

    _last_input_sync = t;
    _local_input = in;
    _local_input_sent = in;

    buf b;
    b.w_u8((uint8_t)(int8_t)in.move_dir);
    b.w_u8((uint8_t)(int8_t)in.look_dir);
    b.w_bool(in.jump);
    b.w_bool(in.attack);
    b.w_bool(in.action);
    b.w_f(in.aim);
    b.w_u8((uint8_t)(int8_t)in.layer_req);

    /* the tool/weapon this player picked: the host fires the real one */
    {
        creature *pl = adventure::player;
        b.w_u8((uint8_t)(int8_t)(pl ? pl->get_tool_type() : -1));
        b.w_u8((uint8_t)(int8_t)(pl ? pl->get_weapon_type() : -1));
    }

    b.w_bool(in.mining);
    b.w_f(in.mine_x);
    b.w_f(in.mine_y);
    b.w_bool(in.up);
    b.w_bool(in.down);

    broadcast(MSG_INPUT, b);

    /* the request is an edge, not a state: it is sent exactly once */
    if (in.layer_req) {
        _layer_req = 0;
        _local_input.layer_req = 0;
        _local_input_sent.layer_req = 0;
    }
}

/* ------------------------------------------------- physics/animation state */

static void send_state_batch(std::vector<entity*> &v) {
    if (v.empty()) return;

    buf b;
    b.w_u16((uint16_t)v.size());

    for (size_t x = 0; x < v.size(); ++x) {
        entity *e = v[x];

        b.w_u32(e->id);
        b.w_i32(e->get_layer());

        uint32_t nb = e->get_num_bodies();
        if (nb > MP_STATE_MAX_BODIES) nb = MP_STATE_MAX_BODIES;
        b.w_u8((uint8_t)nb);

        for (uint32_t f = 0; f < nb; ++f) {
            b2Body *bd = e->get_body((uint8_t)f);

            if (!bd) {
                b.w_u8(0);
                continue;
            }

            b2Vec2 p = bd->GetPosition();
            b2Vec2 lv = bd->GetLinearVelocity();

            b.w_u8(1);
            b.w_f(p.x);
            b.w_f(p.y);
            b.w_f(bd->GetAngle());
            b.w_f(lv.x);
            b.w_f(lv.y);
            b.w_f(bd->GetAngularVelocity());
        }

        if (e->is_creature()) {
            creature *c = static_cast<creature*>(e);

            b.w_u8(1);
            b.w_f(c->i_dir);
            b.w_i32(c->look_dir);
            b.w_i32(c->jumping);
            b.w_i32(c->get_state());
            b.w_f(c->get_hp());

            /* animation details */
            b.w_f(c->get_max_hp());
            b.w_f(c->get_weapon() ? c->get_weapon()->get_arm_angle() : 0.f);
            b.w_i32(c->motion);
            b.w_bool(c->is_dead());

            /* walking / turning / jumping animation state */
            b.w_f(c->on_ground);
            b.w_f(c->last_i_dir);
            b.w_i32(c->dir);
            b.w_i32(c->new_dir);
            b.w_i32(c->dir_timer);
            b.w_i32(c->jump_time);
            b.w_i32(c->jump_action);
            b.w_i32(c->jump_action_time);
            b.w_i32(c->ladder_time);

            /* layer switch animation */
            b.w_i32(c->layer_dir);
            b.w_i32(c->layermove_timer);
            b.w_i32(c->layermove_pretimer);
            b.w_f(c->layer_new);
            b.w_f(c->layer_blend);
            b.w_f(c->layer_old);

            /* feet step cycle */
            if (c->feet) {
                b.w_u8(1);
                b.w_f(c->feet->stepcount);
                b.w_bool(c->feet->do_step);
                b.w_bool(c->feet->on);
            } else {
                b.w_u8(0);
            }

            /* arms: weapon and tool angles */
            robot_parts::weapon *wp = c->get_weapon();
            robot_parts::tool *tl = c->get_tool();

            if (wp) {
                b.w_u8(1);
                b.w_f(wp->get_arm_angle());
                b.w_f(wp->get_arm_fold());
                b.w_i32(wp->cooldown_timer);
                b.w_bool(wp->fired);
            } else {
                b.w_u8(0);
            }

            if (tl) {
                b.w_u8(1);
                b.w_f(tl->get_arm_angle());
                b.w_f(tl->get_arm_fold());
            } else {
                b.w_u8(0);
            }
        } else {
            b.w_u8(0);
        }
    }

    broadcast(MSG_STATE, b);

    v.clear();
}

/* The world is streamed 30 times per second, which is plenty for crates and
 * machines but visibly choppy for a character you are steering yourself: the
 * client is a pure viewer, so every frame it does not get is a frame in which
 * its player does not move. Characters are therefore streamed separately, at
 * the full frame rate - there are at most MP_MAX_PLAYERS of them. */
static bool is_player_avatar(uint32_t id) {
    if (!id) return false;
    if (id == _local_avatar) return true;
    if (_avatars.find(id) != _avatars.end()) return true;

    for (size_t x = 0; x < _peers.size(); ++x)
        if (_peers[x] && _peers[x]->avatar_id == id) return true;

    return false;
}

static void sync_avatars() {
    if (mode != MODE_HOST || !W || !G) return;
    if (!_session_playing || W->is_paused()) return;

    double t = now_sec();
    if (t - _last_avatar_sync < MP_AVATAR_INTERVAL) return;
    _last_avatar_sync = t;

    std::vector<entity*> batch;

    if (_local_avatar) {
        entity *e = W->get_entity_by_id(_local_avatar);
        if (e && e->get_body(0)) batch.push_back(e);
    }

    for (size_t x = 0; x < _peers.size(); ++x) {
        peer *p = _peers[x];
        if (!p || !p->ready || !p->avatar_id) continue;

        entity *e = W->get_entity_by_id(p->avatar_id);
        if (e && e->get_body(0)) batch.push_back(e);
    }

    send_state_batch(batch);
}

static void sync_state() {
    if (mode != MODE_HOST || !W || !G) return;
    if (!_session_playing || W->is_paused()) return;

    double t = now_sec();
    if (t - _last_state_sync < MP_STATE_INTERVAL) return;
    _last_state_sync = t;

    std::vector<entity*> batch;

    for (std::map<uint32_t, entity*>::iterator it = W->all_entities.begin();
         it != W->all_entities.end(); ++it) {
        entity *e = it->second;
        if (!e) continue;

        b2Body *bd = e->get_body(0);
        if (!bd) continue;

        if (bd->GetType() == b2_staticBody && !e->is_creature()) continue;

        /* player characters are already streamed every frame by
         * sync_avatars(); sending them again here only doubled the traffic
         * and could deliver two different positions for the same frame */
        if (is_player_avatar(e->id)) continue;

        /* A sleeping object is not moving. Sending it 30 times per second
         * wastes bandwidth and keeps the client simulating it. One final
         * update is still sent after it falls asleep so both sides agree on
         * where it came to rest. */
        if (!e->is_creature()) {
            bool awake = bd->IsAwake();
            bool was_awake = _awake.find(e->id) != _awake.end();

            if (!awake && !was_awake) continue;

            if (awake) _awake[e->id] = 1;
            else _awake.erase(e->id);
        }

        batch.push_back(e);

        if (batch.size() >= MP_STATE_BATCH) send_state_batch(batch);
    }

    send_state_batch(batch);
}

static void apply_state(buf *b) {
    if (!W) return;

    uint16_t count = b->r_u16();
    int missing = 0;

    applying = true;

    for (uint16_t x = 0; x < count && !b->err; ++x) {
        uint32_t id = b->r_u32();
        int32_t layer = b->r_i32();
        uint8_t nb = b->r_u8();

        entity *e = W->get_entity_by_id(id);

        if (!e) ++missing;

        for (uint8_t f = 0; f < nb && !b->err; ++f) {
            uint8_t present = b->r_u8();
            if (!present) continue;

            float px = b->r_f();
            float py = b->r_f();
            float a  = b->r_f();
            float vx = b->r_f();
            float vy = b->r_f();
            float av = b->r_f();

            if (!e) continue;

            b2Body *bd = e->get_body(f);
            if (!bd) continue;

            b2Vec2 cur = bd->GetPosition();
            float cur_a = bd->GetAngle();

            float dx = px - cur.x;
            float dy = py - cur.y;

            /* Characters must not be smoothed lazily: a client does not run
             * the game logic during a round, so its robot moves *only*
             * because of these updates. The old gentle correction (which
             * assumed the client also simulated the robot itself) is exactly
             * why the joining player felt slow and sluggish. */
            bool is_char = e->is_creature();

            float err = sqrtf(dx*dx + dy*dy);

            (void)is_char;

            /* Stability over smoothness: the client does not simulate at all
             * during a round, so there is no local prediction to blend with.
             * Every body is placed exactly where the host says it is - the
             * two worlds can not drift apart that way. The deadzone only
             * avoids waking up bodies that did not really move. */
            float da = a - cur_a;
            const float mp_pi = 3.14159265358979f;
            while (da > mp_pi) da -= 2.f*mp_pi;
            while (da < -mp_pi) da += 2.f*mp_pi;

            if (err > MP_DEADZONE || fabsf(da) > 0.0015f)
                bd->SetTransform(b2Vec2(px, py), a);

            bd->SetLinearVelocity(b2Vec2(vx, vy));
            bd->SetAngularVelocity(av);

            /* only wake up what is actually moving, otherwise the client
             * simulates the whole room forever and loses fps */
            if (err > MP_DEADZONE
                    || fabsf(vx) > 0.02f || fabsf(vy) > 0.02f
                    || fabsf(av) > 0.02f)
                bd->SetAwake(true);
        }

        uint8_t is_c = b->r_u8();

        if (is_c) {
            float i_dir = b->r_f();
            int32_t look_dir = b->r_i32();
            int32_t jumping = b->r_i32();
            int32_t state = b->r_i32();
            float hp = b->r_f();
            float max_hp = b->r_f();
            float aim = b->r_f();
            int32_t motion = b->r_i32();
            uint8_t dead = b->r_u8();

            float on_ground_f = b->r_f();
            float last_i_dir = b->r_f();
            int32_t dir = b->r_i32();
            int32_t new_dir = b->r_i32();
            int32_t dir_timer = b->r_i32();
            int32_t jump_time = b->r_i32();
            int32_t jump_action = b->r_i32();
            int32_t jump_action_time = b->r_i32();
            int32_t ladder_time = b->r_i32();

            int32_t layer_dir = b->r_i32();
            int32_t layermove_timer = b->r_i32();
            int32_t layermove_pretimer = b->r_i32();
            float layer_new = b->r_f();
            float layer_blend = b->r_f();
            float layer_old = b->r_f();

            uint8_t has_feet = b->r_u8();
            float feet_stepcount = 0.f;
            uint8_t feet_do_step = 0;
            uint8_t feet_on = 0;

            if (has_feet) {
                feet_stepcount = b->r_f();
                feet_do_step = b->r_u8();
                feet_on = b->r_u8();
            }

            uint8_t has_weapon = b->r_u8();
            float wp_angle = 0.f, wp_fold = 0.f;
            int32_t wp_cooldown = 0;
            uint8_t wp_fired = 0;

            if (has_weapon) {
                wp_angle = b->r_f();
                wp_fold = b->r_f();
                wp_cooldown = b->r_i32();
                wp_fired = b->r_u8();
            }

            uint8_t has_tool = b->r_u8();
            float tl_angle = 0.f, tl_fold = 0.f;

            if (has_tool) {
                tl_angle = b->r_f();
                tl_fold = b->r_f();
            }

            (void)max_hp;

            if (e && e->is_creature()) {
                creature *c = static_cast<creature*>(e);

                /* our own robot faces and aims with our own mouse - the echo
                 * from the host is one round trip old and made it twitch */
                bool own = (mode == MODE_CLIENT && e->id == _local_avatar);
                bool avatar = is_player_avatar(e->id);

                if (!own) {
                    c->i_dir = i_dir;
                    c->last_i_dir = i_dir;
                    if (!avatar) c->look_dir = (int)look_dir;
                }
                c->jumping = (int)jumping;
                c->set_hp(hp);
                c->motion = (int)motion;

                if (c->get_state() != (int)state) c->set_state((int)state);

                /* the arm angle comes below as the raw weapon angle; the old
                 * get_aim() value here was in another unit and fought it */
                (void)aim;

                if (dead && !c->is_dead()) c->set_state(CREATURE_DEAD);

                /* walking / turning / jumping animation state */
                c->on_ground = on_ground_f;
                if (!own) c->last_i_dir = last_i_dir;
                c->dir_timer = (int)dir_timer;
                c->jump_time = (int)jump_time;
                c->jump_action = (int)jump_action;
                c->jump_action_time = (int)jump_action_time;
                c->ladder_time = (int)ladder_time;

                /* a turn has to go through the creature so the mesh and the
                 * head are rebuilt, otherwise the robot slides backwards */
                if (!own && (c->dir != (int)dir || c->new_dir != (int)new_dir)) {
                    c->new_dir = (int)new_dir;

                    if (c->dir != (int)dir) {
                        c->dir = (int)dir;
                        c->on_dir_change();
                    }
                }

                /* layer switch animation */
                c->layer_dir = (int)layer_dir;
                c->layermove_timer = (int)layermove_timer;
                c->layermove_pretimer = (int)layermove_pretimer;
                c->layer_new = layer_new;
                c->layer_blend = layer_blend;
                c->layer_old = layer_old;

                if (has_feet && c->feet) {
                    c->feet->stepcount = feet_stepcount;
                    c->feet->do_step = feet_do_step != 0;
                    c->feet->on = feet_on != 0;
                }

                if (has_weapon && c->get_weapon()) {
                    robot_parts::weapon *wp = c->get_weapon();

                    if (!own && !avatar) {
                        wp->set_arm_angle_raw(wp_angle);
                        wp->set_arm_fold(wp_fold);
                    }
                    wp->cooldown_timer = (int)wp_cooldown;
                    wp->fired = wp_fired != 0;
                }

                if (has_tool && c->get_tool()) {
                    robot_parts::tool *tl = c->get_tool();

                    if (!own) {
                        tl->set_arm_angle_raw(tl_angle);
                        tl->set_arm_fold(tl_fold);
                    }
                }
            }
        }

        if (e) {
            if (e->get_layer() != (int)layer) e->set_layer((int)layer);
            e->update();
        }
    }

    applying = false;

    /* The host is simulating objects we do not know about: ask for a resync.
     *
     * Some objects can simply never be re-created on our side (objects that
     * are owned by another object, for example), and asking again every two
     * seconds made the host re-send the entire room over and over during a
     * round, which is a big stall on both sides for no gain. Unknown objects
     * are harmless (their state is skipped), so the requests are capped. */
    if (missing > 0 && mode == MODE_CLIENT && !_session_playing) {
        double t = now_sec();
        double interval = MP_RESYNC_INTERVAL;

        if (t - _last_resync_req > interval
                && _resync_requests < MP_MAX_RESYNC_REQUESTS) {
            _last_resync_req = t;
            ++_resync_requests;

            buf r;
            _server.send(MSG_REQUEST_SNAPSHOT, r);

            tms_infof("co-op: %d unknown objects in state, requesting resync (%d/%d)",
                    missing, _resync_requests, MP_MAX_RESYNC_REQUESTS);
        }
    }
}

/* ------------------------------------------- object settings (properties) */

static double   _last_prop_scan = 0.0;
static uint32_t _prop_scan_cursor = 0;

static uint32_t entity_signature(entity *e) {
    if (!e || !W) return 0;

    lvlbuf lb;
    lb.ensure(4096);
    lb.size = 0;
    lb.rp = 0;

    /* hide the transform from the hash: dragging an object around must not
     * look like a settings change */
    b2Vec2 saved_pos = e->_pos;
    float saved_angle = e->_angle;
    int saved_prio = e->prio;   /* layer travels via MSG_XFORM as well */

    e->_pos = b2Vec2(0.f, 0.f);
    e->_angle = 0.f;
    e->prio = 0;

    of::write(&lb, (uint8_t)W->level.version, e, 0, b2Vec2(0.f, 0.f), false);

    e->_pos = saved_pos;
    e->_angle = saved_angle;
    e->prio = saved_prio;

    if (lb.size == 0) return 0;

    uint32_t hash = 2166136261u;

    for (uint32_t x = 0; x < (uint32_t)lb.size; ++x) {
        hash ^= (uint32_t)lb.buf[x];
        hash *= 16777619u;
    }

    return hash ? hash : 1;
}

/* A settings update is applied by deleting the object and building it again
 * from the received data. That is fine for a loose plank, but not for:
 *   - characters: they are the players themselves, the camera follows them
 *     and the adventure logic points at them, and their signature changes
 *     constantly (items, weapon, animation) - they were being re-created
 *     over and over;
 *   - objects with connections: deleting one cascades into every neighbour
 *     ("Disconnect all called on Plank" ... -> segfault);
 *   - welded group members (see is_streamable).
 * Their positions and animations still sync through the transform and state
 * channels; only the "re-serialize the whole object" path is off limits. */
static bool can_stream_settings(entity *e) {
    if (!e) return false;
    if (!is_streamable(e)) return false;
    if (e->is_creature()) return false;

    /* Connected objects used to be excluded here, which is why resizing a
     * plank that had a nail or an axle in it never reached the other player.
     * They are supported now: the update carries the object *and* all of its
     * connections and the receiver rebuilds both (apply_entity_update).
     * Welded group members stay excluded - re-creating one frees the group
     * from inside the group's own code. */
    if (e->gr) return false;

    return true;
}

/* Write every connection of `e` into `b`, in exactly the field order
 * apply_connect() reads, so the receiver can simply call it in a loop. */
static uint8_t write_entity_connections(buf *b, entity *e) {
    std::vector<connection*> list;

    for (connection *cc = e->conn_ll; cc; cc = cc->next[(cc->e == e) ? 0 : 1]) {
        if (!cc->e || !cc->o) continue;
        if (list.size() >= 255) break;

        list.push_back(cc);
    }

    b->w_u8((uint8_t)list.size());

    for (size_t x = 0; x < list.size(); ++x) {
        connection *cc = list[x];

        b->w_u32(cc->e->id);
        b->w_u32(cc->o->id);
        b->w_u8(cc->type);
        b->w_u8(cc->f[0]);
        b->w_u8(cc->f[1]);
        b->w_f(cc->p.x);
        b->w_f(cc->p.y);
        b->w_f(cc->p_s.x);   /* apply_connect() reads p_s right after p */
        b->w_f(cc->p_s.y);
        b->w_i32(cc->layer);
        b->w_f(cc->max_force);
        b->w_f(cc->damping);
        b->w_f(cc->angle);
        b->w_i32(0); /* option: plain re-connect */
        b->w_u8(cc->render_type);
        write_conn_xforms(b, cc->e, cc->o);
    }

    return (uint8_t)list.size();
}

static void send_entity_update(entity *e) {
    if (!e) return;
    if (!can_stream_settings(e)) return;

    buf b;
    if (!write_entity(&b, e)) return;

    write_entity_connections(&b, e);

    broadcast(MSG_ENTITY_UPDATE, b);
}

static void apply_entity_update(buf *b) {
    if (!W || !G) return;

    entity *e = read_entity(b);
    if (!e) return;

    uint32_t id = e->id;
    entity *old = W->get_entity_by_id(id);

    /* The connection list always follows the object, so it has to be read on
     * every path out of this function - otherwise the rest of the message is
     * parsed as garbage. */
    uint8_t num_conns = b->r_u8();

    /* The object is re-created to apply its settings, which would invalidate
     * the local selection pointer. If we are currently holding/editing this
     * very object, drop the remote update instead of yanking it out of our
     * hands. */
    if (old && G->selection.e == old) {
        _remote_touch[id] = now_sec();
        delete e;
        return;
    }

    /* the object is unknown here (never streamed, or already gone) */
    if (!old) {
        delete e;
        return;
    }

    /* welded, connected to something, or a character: re-creating it would
     * free its group, cascade through its neighbours or yank a player's own
     * robot away (see can_stream_settings) */
    if (old && !can_stream_settings(old)) {
        _remote_touch[id] = now_sec();
        delete e;
        return;
    }

    if (old && G->selection.m && G->selection.m->count(old)) {
        _remote_touch[id] = now_sec();
        delete e;
        return;
    }

    /* keep the object where it currently is, only the settings changed */
    if (old) {
        e->_pos = old->get_position();
        e->_angle = old->get_angle();
        e->set_layer(old->get_layer());

        bool was_followed = (G->follow_object == old);

        safe_delete_entity(old);

        if (was_followed) _refollow = id;
    }

    add_remote_entity(e);

    if (_refollow == id) {
        _refollow = 0;
        G->set_follow_object(e, true);
    }

    /* Re-create the connections the object had. Deleting it above destroyed
     * them (that is what silently un-nailed everything when a size changed),
     * so the sender ships the full list with every update. */
    for (uint8_t x = 0; x < num_conns; ++x) {
        if (b->err) break;

        apply_connect(b);
    }

    _sig[id] = entity_signature(e);
    _remote_touch[id] = now_sec();
}

static void sync_entity_settings() {
    if (!W || !G || mode == MODE_OFF) return;
    if (!W->is_paused() || _loading) return;

    double t = now_sec();
    if (t - _last_prop_scan < MP_PROP_SCAN_INTERVAL) return;
    _last_prop_scan = t;

    if (W->all_entities.empty()) return;

    std::map<uint32_t, entity*>::iterator it =
        W->all_entities.lower_bound(_prop_scan_cursor);

    int budget = MP_PROP_SCAN_BUDGET;

    while (budget-- > 0) {
        if (it == W->all_entities.end()) {
            _prop_scan_cursor = 0;
            break;
        }

        entity *e = it->second;
        uint32_t id = it->first;
        ++it;
        _prop_scan_cursor = (it == W->all_entities.end() ? 0 : it->first);

        if (!e) continue;

        /* skip objects a peer is currently dragging or just edited: their
         * dynamic state keeps changing and re-sending it would bounce the
         * object (and kill their selection) */
        std::map<uint32_t, double>::iterator rt = _remote_touch.find(id);

        if (rt != _remote_touch.end()) {
            if (t - rt->second < MP_REMOTE_HOLD) {
                _sig[id] = entity_signature(e);
                continue;
            }

            _remote_touch.erase(rt);
        }

        if (!can_stream_settings(e)) {
            _sig.erase(id);
            continue;
        }

        uint32_t sig = entity_signature(e);
        if (!sig) continue;

        std::map<uint32_t, uint32_t>::iterator s = _sig.find(id);

        if (s == _sig.end()) {
            _sig[id] = sig;
            continue;
        }

        if (s->second == sig) continue;

        s->second = sig;
        send_entity_update(e);
    }
}

void on_local_entity_changed(entity *e) {
    if (mode == MODE_OFF || applying || _loading || !e) return;
    if (mode == MODE_CLIENT && _session_playing) return;

    _sig[e->id] = entity_signature(e);
    send_entity_update(e);
}

/* ------------------------------------------------------------ one-shot events */

bool suppress_local_damage() {
    /* hits are decided by the host only */
    return mode == MODE_CLIENT && _session_playing && !applying;
}

void send_event(uint8_t ev, uint32_t entity_id, float a, float b_) {
    if (mode == MODE_OFF || applying || _loading) return;

    buf b;
    b.w_u8(ev);
    b.w_u32(entity_id);
    b.w_f(a);
    b.w_f(b_);

    broadcast(MSG_EVENT, b);
}

static void apply_event(buf *b) {
    uint8_t ev = b->r_u8();
    uint32_t id = b->r_u32();
    float a = b->r_f();
    float b2v = b->r_f();

    (void)b2v;

    if (b->err || !W || !G) return;

    entity *e = W->get_entity_by_id(id);
    if (!e || !e->is_creature()) return;

    creature *c = static_cast<creature*>(e);

    applying = true;

    switch (ev) {
        case MP_EV_SHOOT:
            /* legacy: shots travel as sync::MSG_SHOT now, this only keeps
             * the arm pointing the right way */
            if (c->get_weapon())
                c->get_weapon()->set_arm_angle_raw(a);
            else
                c->aim(a);
            break;

        case MP_EV_DAMAGE:
            c->set_hp(a);
            break;

        case MP_EV_DEATH:
            c->set_hp(0.f);
            if (!c->is_dead()) c->set_state(CREATURE_DEAD);
            break;

        case MP_EV_RESPAWN:
            c->set_hp(a > 0.f ? a : c->get_max_hp());
            c->set_state(CREATURE_IDLE);
            break;
    }

    applying = false;
}

/* Every object in the level is about to be destroyed: a level load, a
 * restart, starting a round (the game saves and re-opens the level), or a
 * rebuild from the host's seed. Object ids survive a load, but every pointer
 * does not, so all pointers we or the game kept into the old level have to be
 * dropped here. Otherwise the first frame after the load renders through a
 * freed camera target -> segfault. */
static std::vector<entity*> _spawn_out; /* spawns waiting for the next frame */

void on_world_teardown() {
    _spawn_out.clear();
    _terrain_pending.clear();
    if (mode == MODE_OFF) return;
    if (!W || !G) return;

    _xf.clear();
    _gxf.clear();
    _awake.clear();
    _remote_touch.clear();
    _refollow = 0;

    /* queued messages refer to objects of the level that is being thrown
     * away: applying them after the load would hit freed memory or, worse,
     * a different object that reused the id */
    _inq.clear();
    _lreq.clear();
    _layer_req = 0;

    if (!G) return;

    G->selection.m_saved = 0;
    G->selection.e_saved = 0;
    G->selection.c_saved = 0;
    G->selection.disable(false);
    G->selection.b = 0;

    G->follow_object = 0;
    adventure::player = 0;
}

/* A level was thrown away and rebuilt (new game, restart, leaving to the
 * menu, our own seed rebuild). Every object pointer and every cached object
 * id now belongs to freed memory or to a completely different object, so the
 * whole per-level sync state has to go. Object ids restart from 1 after a
 * rebuild, so keeping the caches would make us apply a peer's robot state to
 * a random plank. */
void on_world_reset() {
    _spawn_out.clear();
    _terrain_pending.clear();
    sync::reset();

    if (mode == MODE_OFF) return;

    on_world_teardown();

    /* a brand new level restarts object ids from 1, so even the ids are
     * worthless now */
    _sig.clear();

    /* ... but only the ids *we* generated. A client owns none of the avatar
     * ids: every one of them was handed out by the host and stays valid
     * across our own rebuilds, round starts and autosave reloads. Clearing
     * the registry here is what left the client with local_avatar=0 - the
     * host had already announced the mapping and never repeated it. */
    if (mode != MODE_CLIENT) {
        _local_avatar = 0;
        _avatars.clear();
        _assigned_avatar = 0;

        for (size_t x = 0; x < _peers.size(); ++x)
            if (_peers[x]) _peers[x]->avatar_id = 0;
    }
}

/* ------------------------------------------------- runtime (in-round) spawns */

/* Only the procedurally generated terrain chunks themselves are skipped:
 * every side generates those from the same seed. Everything the players
 * build (including blocks/pixels) is always streamed. */
static bool is_streamable(entity *e) {
    if (!e) return false;

    /* terrain is generated from the seed on both sides */
    if (e->g_id == O_CHUNK) return false;

    /* A group is not a real object, it is the welded-together result of other
     * objects. The level format stores groups separately from entities for
     * exactly that reason. Sending one as an object produced garbage on the
     * other side; groups are rebuilt locally from the connections instead. */
    if (e->type == ENTITY_GROUP) return false;

    /* An object welded into a group is owned by that group. Deleting or
     * re-creating a member (which is how a settings change is applied) makes
     * the engine rebuild the group from inside that delete, and a group that
     * ends up with a single member frees itself right there
     * ("WARNING: REMOVING SELF") -> segfault. Taking the object out of the
     * group first does not help, the rebuild happens there as well. Welded
     * objects are therefore left alone. */
    if (e->gr) return false;

    return true;
}

/* A client re-creates the level from the host's seed, so it generates its own
 * copy of every procedurally placed cow, plant, chest and robot. The host then
 * streams the authoritative room on top of that, which used to double every
 * creature on the level. Everything except the terrain chunks is therefore
 * thrown away right before the snapshot is applied. */
static void wipe_local_world() {
    if (!W || !G) return;

    /* only right after joining/rebuilding, and never in the middle of a
     * running round or a level load: deleting half of a simulated world
     * while the physics step or the loader is walking it is fatal */
    if (mode != MODE_CLIENT) return;
    if (!_need_wipe || _loading || _session_playing || !W->is_paused()) return;

    _need_wipe = false;

    /* Collect ids, not pointers. Deleting one object can take others with it
     * (pivot/damper pairs, groups, cascading disconnects), so cached
     * pointers go stale mid-loop and freeing them again crashes. */
    std::vector<uint32_t> doomed;

    for (std::map<uint32_t, entity*>::iterator it = W->all_entities.begin();
            it != W->all_entities.end(); ++it) {
        entity *e = it->second;

        if (!e) continue;
        if (!is_streamable(e)) continue; /* keep the generated terrain */

        /* Never throw away a character that belongs to a player. Deleting
         * our own robot here is what left the client with
         * adventure::player=0 and nothing to control. */
        if (is_player_avatar(it->first)) continue;

        doomed.push_back(it->first);
    }

    if (doomed.empty()) return;

    tms_infof("co-op: dropping %d locally generated objects before the host snapshot",
            (int)doomed.size());

    _loading = true;

    G->selection.m_saved = 0;
    G->selection.e_saved = 0;
    G->selection.c_saved = 0;
    G->selection.disable(false);
    G->selection.b = 0;

    adventure::player = 0;
    G->state.adventure_id = 0;
    G->follow_object = 0;
    _refollow = 0;

    applying = true;

    int killed = 0;

    for (size_t x = 0; x < doomed.size(); ++x) {
        entity *e = W->get_entity_by_id(doomed[x]);

        if (!e) continue; /* already taken down by a cascade */

        G->delete_entity(e);
        ++killed;
    }

    applying = false;
    _loading = false;

    tms_infof("co-op: %d objects removed", killed);

    _xf.clear();
    _sig.clear();
    _awake.clear();
    _remote_touch.clear();

    /* _local_avatar deliberately survives: the host already told us which
     * character is ours and resolve_local_avatar() re-binds it */

    G->refresh_widgets();
}

static double   _spawn_window = 0.0;
static int      _spawn_in_window = 0;


void on_runtime_spawn(entity *e) {
    /* Both players simulate and both own what they create, so a spawn is
     * announced by whoever made it - an object taken out of the inventory in
     * the middle of a round used to exist on that one screen only. */
    if (mode == MODE_OFF || applying || _loading || !e) return;
    if (!is_streamable(e)) return;

    /* Projectiles are the one thing that stays local: a shot travels as
     * MSG_SHOT and every machine builds its own bullet out of it, so sending
     * the bullet as an object would give every shot a second, ghost
     * projectile. */
    if (e->is_bullet()) return;

    /* terrain and what the generator places is built from the seed on
     * every machine with identical ids */
    if (_chunk_loading) return;

    /* re-loaded from the level by the preloader: everyone has it already */
    if (_session_playing && _level_ids.count(e->id)) return;

    /* HOST AS AUTHORITY: whatever the client's own simulation produces in a
     * round (machine output, loot, wandering NPCs) is thrown away; the host
     * streams its authoritative copy. Objects the player places himself are
     * taken off this list again by on_local_spawn(). */
    if (mode == MODE_CLIENT && _session_playing) {
        if (!is_player_avatar(e->id) && e != (entity*)adventure::player)
            _client_doomed.insert(e->id);
        return;
    }

    /* While the round runs, every machine in the room runs on both machines
     * as well: a robot factory, an emitter or a breaking crate produces its
     * output on each side by itself, and streaming that output on top of it
     * is what used to fill the level with duplicated robots and planks.
     *
     * An item is the exception, and it is the case that matters here: items
     * are what a player takes out of its inventory, drops or throws, and
     * those only ever existed on the screen of whoever did it. */
    /* host: everything spawned during a round is streamed (planks, loot,
     * NPCs, machine output) - the client no longer creates its own copies */

    /* Terrain and everything the terrain generator places is built from the
     * level seed, so the client already has an identical copy with an
     * identical id. Streaming it created a second object with the same id -
     * the world map then held two entities under one key and the client died
     * as soon as the host walked into fresh chunks. */
    if (_chunk_loading) return;

    /* hard rate limit: a runaway emitter must not kill everyone's fps */
    double t = now_sec();

    if (t - _spawn_window > 1.0) {
        _spawn_window = t;
        _spawn_in_window = 0;
    }

    if (++_spawn_in_window > MP_MAX_SPAWNS_PER_SEC) {
        if (_spawn_in_window == MP_MAX_SPAWNS_PER_SEC + 1)
            tms_infof("co-op: spawn rate limit hit, dropping extra spawns");

        return;
    }

    _spawn_out.push_back(e);
}

/* send the spawns collected during the last frame; an object that is gone
 * again (or never got a valid id) is skipped */
static void flush_spawns() {
    if (_spawn_out.empty()) return;

    std::vector<entity*> list;
    list.swap(_spawn_out);

    if (mode == MODE_OFF || !W) return;

    for (size_t x = 0; x < list.size(); ++x) {
        entity *e = list[x];

        if (!e || e->id == 0) continue;
        if (W->get_entity_by_id(e->id) != e) continue;
        if (!is_streamable(e)) continue;

        buf b;
        if (write_entity(&b, e))
            broadcast(MSG_SPAWN, b);
    }
}

void set_chunk_loading(bool loading) {
    _chunk_loading = loading;
}

bool suppress_local_layermove() {
    /* HOST AS AUTHORITY: during a round the client only *requests* a layer
     * switch (MSG_INPUT.layer_req). The host validates it with a real
     * creature::layermove() and answers with sync::MSG_LAYER, which the
     * client applies via layermove()/set_layer(). */
    return mode == MODE_CLIENT && _session_playing;
}

/* Client: our own character just changed layer locally. Forward it so the
 * host performs the same switch and relays it to everyone else. */
void on_local_layermove(int dir) {
    if (mode != MODE_CLIENT || !_session_playing || !dir) return;

    _layer_req = (int8_t)(dir < 0 ? -1 : 1);
}

void on_runtime_remove(entity *e) {
    if (e) {
        for (size_t x = 0; x < _spawn_out.size(); ++x)
            if (_spawn_out[x] == e) _spawn_out[x] = 0;
    }

    if (mode == MODE_OFF || applying || _loading || !e) return;

    /* counterpart of on_runtime_spawn: whatever is streamed in must also be
     * streamed out, otherwise objects pile up on the other side forever */
    if (!is_streamable(e)) return;
    if (e->is_bullet()) return;
    if (mode == MODE_CLIENT && _session_playing) {
        _client_doomed.erase(e->id);
        return;
    }
    /* the counterpart of the guard in on_runtime_spawn: unloading a chunk is
     * a local operation, the object still exists for everyone else */
    if (_chunk_loading) return;

    uint32_t id = e->id;

    _xf.erase(id);
    _awake.erase(id);
    _sig.erase(id);

    buf b;
    b.w_u32(id);
    broadcast(MSG_DELETE, b);
}

/* --------------------------------------------------------- message loop */

/* ------------------------------------------------------------ welded groups
 *
 * A group is a local object: its id is generated on every machine
 * separately, so it can not be addressed over the network. Its *members*
 * however have synchronized ids, so a group is identified by the lowest id
 * among its members - a value both sides compute the same way.
 *
 * Members are never moved individually (that makes the engine rebuild the
 * group while it is being walked -> "REMOVING SELF" crash). The whole group
 * is moved instead, by the delta of its reference member. */
struct group_key {
    uint32_t ref_id;   /* lowest member id */
    entity  *ref;      /* that member */
    entity  *gr;
};

static void collect_groups(std::map<void*, group_key> &out) {
    if (!W) return;

    for (std::map<uint32_t, entity*>::iterator it = W->all_entities.begin();
         it != W->all_entities.end(); ++it) {
        entity *e = it->second;

        if (!e || !e->gr) continue;

        std::map<void*, group_key>::iterator g = out.find((void*)e->gr);

        if (g == out.end()) {
            group_key k;
            k.ref_id = e->id;
            k.ref = e;
            k.gr = (entity*)e->gr;
            out[(void*)e->gr] = k;
        } else if (e->id < g->second.ref_id) {
            g->second.ref_id = e->id;
            g->second.ref = e;
        }
    }
}

static void sync_group_xforms() {
    if (!W || !G || mode == MODE_OFF) return;
    if (!W->is_paused() || _loading) return;

    std::map<void*, group_key> groups;
    collect_groups(groups);

    for (std::map<void*, group_key>::iterator it = groups.begin();
         it != groups.end(); ++it) {
        entity *ref = it->second.ref;

        b2Vec2 p = ref->get_position();
        float a = ref->get_angle();
        int l = ref->get_layer();

        std::map<uint32_t, xform_cache>::iterator ce = _gxf.find(it->second.ref_id);

        bool changed = true;

        if (ce != _gxf.end()) {
            changed = (fabsf(ce->second.x - p.x) > 0.0015f)
                   || (fabsf(ce->second.y - p.y) > 0.0015f)
                   || (fabsf(ce->second.angle - a) > 0.0015f)
                   || (ce->second.layer != l);
        }

        bool was_known = (ce != _gxf.end());

        xform_cache nc;
        nc.x = p.x;
        nc.y = p.y;
        nc.angle = a;
        nc.layer = l;
        _gxf[it->second.ref_id] = nc;

        /* a group we see for the first time was just welded together on this
         * machine: the connections themselves are already on the wire */
        if (!changed || !was_known) continue;

        /* somebody else is dragging it right now */
        std::map<uint32_t, double>::iterator rt = _remote_touch.find(it->second.ref_id);

        if (rt != _remote_touch.end() && now_sec() - rt->second < MP_REMOTE_HOLD)
            continue;

        buf b;
        b.w_u32(it->second.ref_id);
        b.w_f(p.x);
        b.w_f(p.y);
        b.w_f(a);
        b.w_i32(l);

        broadcast(MSG_GROUP_XFORM, b);
    }
}

static void apply_group_xform(buf *b) {
    uint32_t ref_id = b->r_u32();
    float x = b->r_f();
    float y = b->r_f();
    float a = b->r_f();
    int32_t layer = b->r_i32();

    if (b->err || !W || !G) return;

    entity *ref = W->get_entity_by_id(ref_id);
    if (!ref || !ref->gr) return;

    entity *gr = (entity*)ref->gr;

    /* do not yank a group out of the hands of the local player */
    if (G->selection.e == ref || G->selection.e == gr) {
        _remote_touch[ref_id] = now_sec();
        return;
    }

    applying = true;

    /* rotate first, then correct the remaining position error of the
     * reference member: a group pivots around its own centre of mass, which
     * is not the member's origin */
    float da = a - ref->get_angle();
    const float mp_pi = 3.14159265358979f;
    while (da > mp_pi) da -= 2.f*mp_pi;
    while (da < -mp_pi) da += 2.f*mp_pi;

    if (fabsf(da) > 0.0015f) {
        gr->set_angle(gr->get_angle() + da);
        gr->update();
    }

    b2Vec2 cur = ref->get_position();
    b2Vec2 gp = gr->get_position();

    gr->set_position(gp.x + (x - cur.x), gp.y + (y - cur.y));

    if (gr->get_layer() != (int)layer) gr->set_layer((int)layer);

    gr->update();

    xform_cache nc;
    nc.x = x;
    nc.y = y;
    nc.angle = a;
    nc.layer = (int)layer;
    _gxf[ref_id] = nc;

    _remote_touch[ref_id] = now_sec();

    applying = false;
}

/* ------------------------------------------------------------ player roster */

static void send_roster() {
    if (mode != MODE_HOST) return;

    buf b;
    b.w_u32((uint32_t)num_players());

    broadcast(MSG_PLAYERS, b);
}

static void handle_message(uint8_t type, buf *b, peer *from) {
    /* The host owns the world: its streams are only ever accepted from the
     * host, never from a client. */
    /* Both players simulate and stream their own character, so these are
     * accepted from either side. The host additionally relays them to the
     * other clients (it is the only machine everybody is connected to). */
    if (type >= sync::MSG_PLAYER_POS && type < sync::MSG__LAST) {
        /* HOST AS AUTHORITY: world streams come from the host only. A client
         * that sends them is ignored - it is not allowed to move anything. */
        if (mode == MODE_CLIENT && !from)
            sync::handle(type, b);
        else
            tms_debugf("co-op: dropped sync message %d from a client", (int)type);

        return;
    }

    /* During a round the world belongs to the host. A client may not spawn,
     * delete, connect or re-configure anything with ids of its own. */
    if (mode == MODE_HOST && from && _session_playing) {
        switch (type) {
            /* physics, animation and damage belong to the host */
            case MSG_STATE: case MSG_ANIM: case MSG_EVENT:
                tms_debugf("co-op: peer %d sent host-only message %d, ignored",
                           from->id, (int)type);
                return;

            /* build requests are accepted, but nobody may delete or teleport
             * a player's robot */
            case MSG_DELETE: case MSG_XFORM: {
                buf peek = *b;
                uint32_t target_id = peek.r_u32();

                if (!peek.err && is_player_avatar(target_id)) {
                    tms_infof("co-op: peer %d tried to %s a player robot, ignored",
                              from->id, type == MSG_DELETE ? "delete" : "move");
                    return;
                }
            } break;
        }
    }

    switch (type) {
        case MSG_HELLO: {
            if (mode != MODE_HOST || !from) break;

            uint32_t ver = b->r_u32();
            std::string name = b->r_str();

            /* a second HELLO from the same socket would spawn a second
             * character for that player and leak the first one */
            if (from->ready) {
                tms_infof("co-op: ignoring a duplicate handshake from peer %d", from->id);
                break;
            }

            if (ver != MP_PROTOCOL_VERSION) {
                hud_add("Rejected a player: incompatible version (%u)", ver);
                from->c->close();
                break;
            }

            if (name.empty()) name = "Player";
            from->name = name;
            from->ready = true;

            /* welcome */
            {
                buf w;
                w.w_u32(MP_PROTOCOL_VERSION);
                w.w_i32(from->id);
                w.w_u32(MP_ID_BLOCK * (uint32_t)(from->id + 1));
                w.w_str(cfg_host.server_name);
                w.w_str(local_name.c_str());
                w.w_i32(cfg_host.room_width);
                w.w_i32(cfg_host.room_height);
                w.w_u64(W ? W->level.seed : 0ull);
                from->c->send(MSG_WELCOME, w);
            }

            send_snapshot(from);

            /* tell everyone (including the new player) who joined */
            {
                buf j;
                j.w_i32(from->id);
                j.w_str(from->name.c_str());
                broadcast(MSG_PEER_JOIN, j);
            }

            hud_add("%s joined the game", from->name.c_str());

            /* spawn a character for the new player next to us */
            b2Vec2 pos(0.f, 0.f);
            if (G) {
                pos.x = G->cam->_position.x + 2.f;
                pos.y = G->cam->_position.y + 1.f;
            }

            entity *av = spawn_avatar(from->name.c_str(), pos, G ? G->state.edit_layer : 1);

            if (av) {
                from->avatar_id = av->id;
                _avatars[av->id] = from->id;

                buf s;
                if (write_entity(&s, av))
                    broadcast(MSG_SPAWN, s);

                buf a;
                write_avatar_msg(&a, from->id, av->id, from->name.c_str());
                broadcast(MSG_AVATAR, a);
            }

            /* Tell the new player which characters belong to whom (its own
             * included). It is repeated later, see send_avatars(). */
            send_avatars(from);

            /* if a round is already running, put the new player into it */
            if (_session_playing && from->c) {
                buf sp;
                from->c->send(MSG_START_PLAY, sp);
            }

            send_roster();
            update_status();
        } break;

        case MSG_WELCOME: {
            uint32_t ver = b->r_u32();
            local_peer_id = b->r_i32();
            uint32_t id_base = b->r_u32();
            std::string server_name = b->r_str();
            std::string host_name = b->r_str();
            int room_w = b->r_i32();
            int room_h = b->r_i32();
            uint64_t seed = b->r_u64();

            (void)room_w; (void)room_h;

            if (ver != MP_PROTOCOL_VERSION) {
                shutdown("Server runs an incompatible version");
                break;
            }

            /* make sure our locally created objects can not collide with
             * ids created by other players */
            _id_base = id_base;
            of::_id = id_base;

            /* Adopt the host's terrain seed. Adventure levels generate their
             * biome procedurally, so a different seed means a different
             * world: one player stands on the ground while the other sees
             * them buried or floating in the air.
             *
             * Patching the seed of a live chunk window is not enough (and
             * leaves dangling chunks), so the whole level is re-created from
             * the host's seed before the room snapshot arrives. */
            if (W && G && seed != W->level.seed) {
                tms_infof("co-op: rebuilding the world with host seed 0x%016llx",
                        (unsigned long long)seed);

                applying = true;
                _loading = true;

                adventure::player = 0;
                G->state.adventure_id = 0;

                G->coop_create_level(seed);

                /* the host streams us our own robot, drop the one the fresh
                 * level was created with */
                entity *own = W->get_entity_by_id(W->level.get_adventure_id());

                if (own) {
                    adventure::player = 0;
                    G->state.adventure_id = 0;

                    if (G->follow_object == own) G->follow_object = 0;

                    if (G->selection.e == own) {
                        G->selection.m_saved = 0;
                        G->selection.e_saved = 0;
                        G->selection.c_saved = 0;
                        G->selection.disable(false);
                        G->selection.b = 0;
                    }

                    G->delete_entity(own);
                }

                G->refresh_widgets();

                _xf.clear();
                _sig.clear();
                _awake.clear();
                _remote_touch.clear();
                _local_avatar = 0;
                _assigned_avatar = 0;
                _avatars.clear();

                /* world::create resets the object id counter, restore the
                 * block the host reserved for us */
                of::_id = id_base;

                _loading = false;
                applying = false;

                _need_wipe = true;

                /* the snapshot was built for the old world, ask again */
                buf r;
                _server.send(MSG_REQUEST_SNAPSHOT, r);

                hud_add("Synced the world with the host");
            }

            hud_add("Connected to \"%s\" (host: %s)", server_name.c_str(), host_name.c_str());
            update_status();
        } break;

        case MSG_PEER_JOIN: {
            int id = b->r_i32();
            std::string name = b->r_str();
            (void)id;
            hud_add("%s joined the game", name.c_str());
        } break;

        case MSG_PEER_LEAVE: {
            int id = b->r_i32();
            std::string name = b->r_str();
            (void)id;
            hud_add("%s left the game", name.c_str());
        } break;

        case MSG_CHAT: {
            std::string name = b->r_str();
            std::string text = b->r_str();
            hud_add("%s: %s", name.c_str(), text.c_str());

            if (mode == MODE_HOST) {
                buf f;
                f.w_str(name.c_str());
                f.w_str(text.c_str());
                broadcast(MSG_CHAT, f, from ? from->id : -1);
            }
        } break;

        case MSG_SNAPSHOT_BEGIN:
            hud_add("Receiving the room from the server...");

            if (mode == MODE_CLIENT)
                wipe_local_world();

            break;

        case MSG_SNAPSHOT_END:
            hud_add("Room received, happy building!");
            break;

        case MSG_SPAWN: {
            /* Mid-round spawns (bullets, debris, anything a machine
             * produces) are accepted now. They used to create phantom
             * objects because the client simulated as well and spawned its
             * own copies; it is a pure viewer today, so the only objects it
             * sees are the ones the host sends. */

            entity *e = read_entity(b);

            if (!e) break;

            if (e->id == 0) {
                tms_infof("co-op: dropped a spawn without an id (g_id %d)", (int)e->g_id);
                delete e;
                break;
            }

            if (W->get_entity_by_id(e->id)) {
                /* we already have it */
                delete e;
                break;
            }

            /* part of the level: our own preloader will load it, creating it
             * here as well would put two objects under one id (crash) */
            if (_session_playing && _level_ids.count(e->id)) {
                delete e;
                break;
            }

            add_remote_entity(e);

            tms_infof("co-op: object %u (g_id %d) spawned by %s", e->id, (int)e->g_id,
                      from ? "a client" : "the host");

            if (mode == MODE_HOST) {
                buf f;
                if (write_entity(&f, e))
                    broadcast(MSG_SPAWN, f, from ? from->id : -1);
            }
        } break;

        case MSG_XFORM: {
            buf copy = *b;
            apply_xform(b);

            if (mode == MODE_HOST)
                broadcast(MSG_XFORM, copy, from ? from->id : -1);
        } break;

        case MSG_DELETE: {
            buf copy = *b;
            apply_delete(b);

            if (mode == MODE_HOST)
                broadcast(MSG_DELETE, copy, from ? from->id : -1);
        } break;

        case MSG_CONNECT: {
            buf copy = *b;
            apply_connect(b);

            if (mode == MODE_HOST)
                broadcast(MSG_CONNECT, copy, from ? from->id : -1);
        } break;

        case MSG_DISCONNECT: {
            buf copy = *b;
            apply_disconnect(b);

            if (mode == MODE_HOST)
                broadcast(MSG_DISCONNECT, copy, from ? from->id : -1);
        } break;

        case MSG_ENTITY_UPDATE: {
            buf copy = *b;
            apply_entity_update(b);

            if (mode == MODE_HOST)
                broadcast(MSG_ENTITY_UPDATE, copy, from ? from->id : -1);
        } break;

        case MSG_EVENT: {
            buf copy = *b;
            apply_event(b);

            if (mode == MODE_HOST)
                broadcast(MSG_EVENT, copy, from ? from->id : -1);
        } break;

        case MSG_AVATAR: {
            int id = b->r_i32();
            uint32_t ent = b->r_u32();
            std::string name = b->r_str();

            /* the weapons of that character (see write_avatar_msg) */
            pending_loadout pl;
            pl.active = (uint8_t)WEAPON_NULL;

            uint8_t nw = b->r_u8();

            for (uint8_t x = 0; x < nw && !b->err; ++x)
                pl.weapons.push_back(b->r_u8());

            pl.active = b->r_u8();

            if (b->err) break;

            tms_debugf("co-op: avatar of %s is entity %u (%u weapons)",
                       name.c_str(), ent, (unsigned)pl.weapons.size());

            entity *ae = W ? W->get_entity_by_id(ent) : 0;

            if (ae) apply_loadout(ae, pl.weapons, pl.active);
            else    _loadout_wait[ent] = pl;

            /* remember who owns which robot - this is what lets a client
             * stream and animate the host's character as well */
            if (ent) _avatars[ent] = id;

            if (id == local_peer_id) {
                _assigned_avatar = ent;
                _local_avatar = ent;
                attach_local_player();
            }
        } break;

        case MSG_REQUEST_SNAPSHOT:
            if (mode == MODE_HOST && from) {
                send_snapshot(from);

                /* the client dropped its message queue together with the
                 * level it just threw away, so the avatar mapping has to
                 * be sent again */
                send_avatars(from);
            }
            break;

        case MSG_REQUEST_AVATARS:
            if (mode == MODE_HOST && from) send_avatars(from);
            break;

        case MSG_PING: {
            /* reply so the sender can measure the round trip time */
            buf r;

            if (mode == MODE_HOST) {
                if (from && from->c) from->c->send(MSG_PONG, r);
            } else if (mode == MODE_CLIENT) {
                _server.send(MSG_PONG, r);
            }
        } break;

        case MSG_PONG: {
            double t = now_sec();

            if (mode == MODE_HOST) {
                if (from && from->ping_sent > 0.0) {
                    from->rtt_ms = (int)((t - from->ping_sent) * 1000.0);
                    from->ping_sent = 0.0;
                }
            } else if (_local_ping_sent > 0.0) {
                _local_rtt_ms = (int)((t - _local_ping_sent) * 1000.0);
                _local_ping_sent = 0.0;
            }
        } break;

        case MSG_PLAYERS: {
            uint32_t n = b->r_u32();

            if (b->err) break;
            if (n > (uint32_t)MP_MAX_PLAYERS) n = (uint32_t)MP_MAX_PLAYERS;

            _roster_count = (int)n;
            update_status();
        } break;

        case MSG_ANIM: {
            buf copy = *b;
            apply_anim(b);

            if (mode == MODE_HOST)
                broadcast(MSG_ANIM, copy, from ? from->id : -1);
        } break;

        case MSG_GROUP_XFORM: {
            buf copy = *b;
            apply_group_xform(b);

            if (mode == MODE_HOST)
                broadcast(MSG_GROUP_XFORM, copy, from ? from->id : -1);
        } break;

        case MSG_READY: {
            if (mode != MODE_HOST || !from) break;

            from->wants_play = true;
            host_check_ready();
        } break;

        case MSG_START_PLAY: {
            hud_add("Everyone is ready - the level is starting!");
            _local_ready = false;
            _pending_play = 1;
        } break;

        case MSG_STOP_READY: {
            if (mode != MODE_HOST || !from) break;

            from->wants_stop = true;
            announce("%s wants to go back to build mode", from->name.c_str());
            host_check_stop();
        } break;

        case MSG_STOP_PLAY: {
            /* Only the host decides when the round actually ends. A client
             * sending this (older build) is treated as "I want to stop". */
            if (mode == MODE_HOST) {
                if (from) {
                    from->wants_stop = true;
                    host_check_stop();
                    break;
                }

                buf f;
                broadcast(MSG_STOP_PLAY, f, -1);
            }

            hud_add("Back to build mode");
            _pending_play = 2;
        } break;

        case MSG_STATE: {
            if (mode == MODE_CLIENT && _session_playing) apply_state(b);
        } break;

        case MSG_TOOL: {
            if (mode != MODE_CLIENT) break;

            uint32_t id = b->r_u32();
            int tool   = (int)(int8_t)b->r_u8();
            int weapon = (int)(int8_t)b->r_u8();

            if (b->err || !W) break;

            /* our own robot: we picked the tool ourselves */
            if (adventure::player && id == adventure::player->id) break;

            apply_tools(id, tool, weapon);
        } break;

        case MSG_INPUT: {
            if (mode != MODE_HOST || !from) break;

            input_state in;
            in.move_dir = (int8_t)b->r_u8();
            in.look_dir = (int8_t)b->r_u8();
            in.jump   = b->r_u8() != 0;
            in.attack = b->r_u8() != 0;
            in.action = b->r_u8() != 0;
            in.aim    = b->r_f();
            in.layer_req = (int8_t)b->r_u8();
            int want_tool   = (int)(int8_t)b->r_u8();
            int want_weapon = (int)(int8_t)b->r_u8();
            in.mining = b->r_u8() != 0;
            in.mine_x = b->r_f();
            in.mine_y = b->r_f();
            in.up     = b->r_u8() != 0;
            in.down   = b->r_u8() != 0;

            if (b->err) break;

            if (from->ready && from->avatar_id && _session_playing)
                apply_tools(from->avatar_id, want_tool, want_weapon);

            /* The layer request is an edge that the client sends exactly once.
             * Edge detection against prev_input missed a second request when
             * nothing else changed in between (input == prev_input), so it
             * is queued right here and never stored as a state. */
            if (in.layer_req && from->ready && from->avatar_id)
                request_layermove(from->avatar_id, (int)in.layer_req);

            in.layer_req = 0;

            from->input = in;
        } break;

        case MSG_TERRAIN: {
            if (mode != MODE_CLIENT) break;

            terrain_px t;
            t.cx = b->r_i32();
            t.cy = b->r_i32();
            t.layer = b->r_u8();
            t.x = b->r_u8();
            t.y = b->r_u8();
            if (b->err) break;

            if (!apply_terrain_px(t))
                _terrain_pending.push_back(t);
        } break;

        default:
            tms_debugf("co-op: unknown message %d", (int)type);
            break;
    }
}

/* ------------------------------------------------ deferred message queue
 *
 * Messages used to be applied straight from the receive loop. Applying a
 * remote change deletes objects, rebuilds groups and re-enters the engine
 * while it is still walking its own lists ("REMOVING SELF" -> segfault), and
 * it could even end the session while we were iterating over the peer list.
 * Everything is parked here instead and applied from one single safe place
 * in step(). */
static void queue_message(uint8_t type, const buf &b, int peer_id) {
    pending_msg m;
    m.type = type;
    m.b = b;
    m.peer_id = peer_id;

    _inq.push_back(m);
}

static void drain_messages() {
    int budget = MP_MAX_OPS_PER_FRAME;

    /* One slow frame (level load, chunk generation) can park thousands of
     * messages. Applying only a fixed amount per frame then makes the queue
     * - and the memory it holds - grow forever while the world falls further
     * and further behind, so a backlog is flushed in one go. */
    if (_inq.size() > (size_t)(MP_MAX_OPS_PER_FRAME * 4))
        budget = (int)_inq.size();

    while (!_inq.empty() && budget-- > 0) {
        pending_msg m = _inq.front();
        _inq.pop_front();

        peer *from = 0;

        if (m.peer_id >= 0) {
            for (size_t x = 0; x < _peers.size(); ++x) {
                if (_peers[x] && _peers[x]->id == m.peer_id) {
                    from = _peers[x];
                    break;
                }
            }

            /* the peer left while its messages were still queued */
            if (!from) continue;
        }

        handle_message(m.type, &m.b, from);

        if (mode == MODE_OFF) {
            _inq.clear();
            return;
        }
    }
}

/* ------------------------------------------------------------- lifecycle */

/* Hand the local player a character: input, camera and control panel, plus
 * the bookkeeping the streaming code needs. */
static void bind_local_avatar(entity *e) {
    if (!e || !e->is_creature() || !W || !G) return;

    uint32_t prev = _local_avatar;

    _local_avatar    = e->id;
    _assigned_avatar = e->id;
    _avatars[e->id]  = local_peer_id;

    /* Only when something is actually wrong: adventure::set_player() resets
     * the camera and rebuilds the control panel, doing that every frame
     * would make the game unplayable. */
    if (W->is_adventure()
            && (adventure::player != static_cast<creature*>(e)
                || G->state.adventure_id != e->id)) {
        G->state.adventure_id = e->id;
        adventure::set_player(static_cast<creature*>(e), true);
    }

    /* sandbox room, or an adventure whose camera got lost during a reload */
    if (!G->follow_object)
        G->set_follow_object(e, true);

    if (prev == _local_avatar) return;

    tms_infof("co-op: local player controls entity %u", e->id);

    /* Tell the other side which robot is ours. The host's own character is
     * created by the level itself (not by spawn_avatar), so nobody else can
     * know its id - without this message the client treated the host as a
     * piece of scenery: no animations, no layer switches, no shots. */
    if (mode == MODE_HOST) {
        buf a;
        write_avatar_msg(&a, local_peer_id, _local_avatar, local_name.c_str());
        broadcast(MSG_AVATAR, a);
    }
}

uint32_t attach_local_player() {
    if (!W || !G) return 0;

    entity *e = 0;

    /* prefer the robot the local player already owns */
    if (_local_avatar)
        e = W->get_entity_by_id(_local_avatar);

    /* the one the session assigned to us before the last level (re)load */
    if (!e && _assigned_avatar)
        e = W->get_entity_by_id(_assigned_avatar);

    /* Otherwise the robot the level was created with - but only if we are
     * not a client: a client's own level robot carries the very id the host
     * uses for ITS character (both levels store the same
     * level.adventure_id), so adopting it would give two different robots
     * one id. A client always waits for MSG_AVATAR. */
    if (!e && mode != MODE_CLIENT && W->is_adventure())
        e = W->get_entity_by_id(W->level.get_adventure_id());

    if (!e && mode != MODE_CLIENT && adventure::player)
        e = W->get_entity_by_id(adventure::player->id);

    /* last resort: spawn a fresh one next to the camera */
    if (!e && mode != MODE_CLIENT) {
        b2Vec2 pos(G->cam->_position.x, G->cam->_position.y + 1.f);
        e = spawn_avatar(local_name.c_str(), pos, G->state.edit_layer);

        if (e && mode == MODE_HOST) {
            buf s;
            if (write_entity(&s, e))
                broadcast(MSG_SPAWN, s);
        }
    }

    if (!e || !e->is_creature()) return 0;

    bind_local_avatar(e);

    return _local_avatar;
}

/* Find our character again, whatever just happened to the world.
 *
 * Runs once per frame, and it is the fix for what the logs showed:
 *   host:   local_avatar=0 and its own robot missing from the avatar list,
 *           so nothing about the host was ever streamed;
 *   client: local_avatar=0 and adventure::player=0 after the world wipe, so
 *           it controlled nothing and could not change layers.
 * Both are cases of "the id was dropped and nobody restored it". */
static uint32_t resolve_local_avatar() {
    if (mode == MODE_OFF || !W || !G) return 0;

    entity *e = 0;

    if (_local_avatar) e = W->get_entity_by_id(_local_avatar);

    if ((!e || !e->is_creature()) && _assigned_avatar)
        e = W->get_entity_by_id(_assigned_avatar);

    /* see attach_local_player(): these fallbacks are for the host only */
    if (mode == MODE_HOST) {
        if ((!e || !e->is_creature()) && W->is_adventure())
            e = W->get_entity_by_id(W->level.get_adventure_id());

        if ((!e || !e->is_creature()) && adventure::player)
            e = W->get_entity_by_id(adventure::player->id);

        if ((!e || !e->is_creature()) && G->state.adventure_id)
            e = W->get_entity_by_id(G->state.adventure_id);
    }

    if (!e || !e->is_creature()) {
        _local_avatar = 0;

        /* the host may create one; a client waits for MSG_AVATAR instead of
         * inventing a second robot for itself */
        if (mode == MODE_HOST) {
            static double last_try = 0.0;
            double t = now_sec();

            if (t - last_try > 1.0) {
                last_try = t;
                return attach_local_player();
            }
        }

        return 0;
    }

    bind_local_avatar(e);

    return _local_avatar;
}

bool host_start(const host_config &cfg) {
    shutdown(0);

    cfg_host = cfg;

    if (cfg_host.port <= 0 || cfg_host.port > 65535) cfg_host.port = MP_DEFAULT_PORT;
    if (cfg_host.max_players < 2) cfg_host.max_players = 2;
    if (cfg_host.max_players > MP_MAX_PLAYERS) cfg_host.max_players = MP_MAX_PLAYERS;

    if (!_listener.open(cfg_host.port)) {
        ui::messagef("Co-op: %s", net_last_error());
        return false;
    }

    mode = MODE_HOST;
    had_session = true;
    local_peer_id = 0;
    local_name = cfg_host.player_name[0] ? cfg_host.player_name : "Host";
    _next_peer_id = 1;
    _xf.clear();
    _sig.clear();

    _session_playing = false;
    _local_ready = false;
    _pending_play = 0;
    _local_avatar = 0;
    _assigned_avatar = 0;
    _avatars.clear();
    _last_recv = 0.0;
    _last_ping = 0.0;

    /* after mode and local_peer_id are set, so the avatar announcement
     * carries the right owner */
    attach_local_player();

    hud_clear();
    hud_add("Server \"%s\" started on port %d", cfg_host.server_name, cfg_host.port);
    hud_add("Tell your friends to join by your IP address");

    update_status();

    return true;
}

bool client_start(const join_config &cfg) {
    shutdown(0);

    cfg_join = cfg;

    if (cfg_join.port <= 0 || cfg_join.port > 65535) cfg_join.port = MP_DEFAULT_PORT;

    intptr_t fd = net_connect(cfg_join.ip, cfg_join.port, 5000);

    if (fd == -1) {
        ui::messagef("Co-op: %s", net_last_error());
        return false;
    }

    _server.attach(fd);

    mode = MODE_CLIENT;
    had_session = true;
    local_name = cfg_join.player_name[0] ? cfg_join.player_name : "Player";
    _need_wipe = true;
    _xf.clear();
    _sig.clear();
    _session_playing = false;
    _local_ready = false;
    _pending_play = 0;
    _local_avatar = 0;
    _assigned_avatar = 0;
    _avatars.clear();
    _last_recv = now_sec();
    _last_ping = 0.0;

    /* the host streams us the whole room, including our own robot, so get
     * rid of the one our local level was created with */
    if (W && G && W->is_adventure()) {
        entity *old = W->get_entity_by_id(W->level.get_adventure_id());

        if (old) {
            adventure::player = 0;
            G->state.adventure_id = 0;

            applying = true;
            G->delete_entity(old);
            applying = false;
        }
    }

    buf h;
    h.w_u32(MP_PROTOCOL_VERSION);
    h.w_str(local_name.c_str());
    _server.send(MSG_HELLO, h);

    hud_clear();
    hud_add("Connecting to %s:%d...", cfg_join.ip, cfg_join.port);

    update_status();

    return true;
}

void shutdown(const char *reason) {
    if (mode == MODE_HOST) {
        for (size_t x = 0; x < _peers.size(); ++x) {
            if (_peers[x]->c) {
                _peers[x]->c->close();
                delete _peers[x]->c;
            }
            delete _peers[x];
        }
        _peers.clear();
        _listener.close();
    } else if (mode == MODE_CLIENT) {
        _server.close();
    }

    if (mode != MODE_OFF && reason)
        hud_add("Co-op session ended: %s", reason);

    mode = MODE_OFF;
    local_peer_id = 0;
    _session_playing = false;
    _local_ready = false;
    _pending_play = 0;
    _local_avatar = 0;
    _assigned_avatar = 0;
    _avatars.clear();
    _xf.clear();
    _gxf.clear();
    _inq.clear();
    _lreq.clear();
    _loadout_wait.clear();
    _layer_req = 0;
    _chunk_loading = false;
    _sig.clear();
    _awake.clear();
    _remote_touch.clear();
    _refollow = 0;
    _need_wipe = false;
    _roster_count = 0;
    _local_ping_sent = 0.0;
    _local_rtt_ms = 0;
    _loading = false;
    _id_base = 0;
    _resync_requests = 0;
    _prop_scan_cursor = 0;
    _last_prop_scan = 0.0;
    _local_input = input_state();
    _local_input_prev = input_state();
    _local_input_sent = input_state();
    _status[0] = 0;

    sync::reset();
}

/* ------------------------------------------------------------------ step */

static void sync_xforms() {
    if (!W || !G || _loading) return;

    /* In build mode every change is streamed. During a round the host owns
     * physics, so a client only streams what its own mouse is dragging. */
    bool only_dragged = !W->is_paused();
    if (only_dragged && mode != MODE_CLIENT) return;

    double t = now_sec();
    if (t - _last_xform_sync < 0.05) return;
    _last_xform_sync = t;

    int sent = 0;

    for (std::map<uint32_t, entity*>::iterator it = W->all_entities.begin();
         it != W->all_entities.end(); ++it) {
        entity *e = it->second;
        if (!e) continue;

        /* generated terrain is identical on every side and never moves */
        if (!is_streamable(e)) continue;

        if (only_dragged && !G->interacting_with(e)) continue;

        b2Vec2 p = e->get_position();
        float a = e->get_angle();
        int l = e->get_layer();

        std::map<uint32_t, xform_cache>::iterator c = _xf.find(e->id);

        bool changed = true;

        if (c != _xf.end()) {
            changed = (fabsf(c->second.x - p.x) > 0.0015f)
                   || (fabsf(c->second.y - p.y) > 0.0015f)
                   || (fabsf(c->second.angle - a) > 0.0015f)
                   || (c->second.layer != l);
        }

        if (!changed) continue;

        xform_cache nc;
        nc.x = p.x;
        nc.y = p.y;
        nc.angle = a;
        nc.layer = l;
        _xf[e->id] = nc;

        if (c == _xf.end() && !only_dragged) {
            /* first time we see this object: it was probably created
             * without going through the editor, do not spam the network */
            continue;
        }

        buf b;
        b.w_u32(e->id);
        b.w_f(p.x);
        b.w_f(p.y);
        b.w_f(a);
        b.w_i32(l);

        broadcast(MSG_XFORM, b);

        if (++sent > 256) break;
    }
}

static void no_roam(uint32_t id) {
    if (!id || !W) return;
    entity *e = W->get_entity_by_id(id);
    if (!e || !e->is_robot()) return;
    if (e->properties && e->num_properties > ROBOT_PROPERTY_ROAMING)
        e->properties[ROBOT_PROPERTY_ROAMING].v.i8 = 0;
}

static void disable_avatar_roaming() {
    if (!W || !_session_playing) return;

    no_roam(_local_avatar);
    for (std::map<uint32_t, int>::iterator it = _avatars.begin(); it != _avatars.end(); ++it)
        no_roam(it->first);
    for (size_t x = 0; x < _peers.size(); ++x)
        if (_peers[x]) no_roam(_peers[x]->avatar_id);
}

void step() {
    hud_step();

    if (mode == MODE_OFF) return;

    disable_avatar_roaming();
    process_client_doomed();
    sync_tools();
    flush_spawns();
    process_terrain_pending();

    /* The layer request is no longer latched here: adventure.cc performs the
     * switch locally and calls on_local_layermove(), which is the only place
     * that still knows the direction by the time this runs. */

    if (mode == MODE_HOST) {
        /* accept new players */
        for (;;) {
            std::string ip;
            intptr_t fd = _listener.accept_one(&ip);
            if (fd == -1) break;

            if ((int)_peers.size() + 1 > cfg_host.max_players) {
                tms_infof("co-op: server full, rejecting %s", ip.c_str());
                /* mp_net owns every platform specific socket call */
                conn tmp;
                tmp.attach(fd);
                tmp.close();
                continue;
            }

            peer *p = new peer();
            p->id = _next_peer_id++;
            p->last_recv = now_sec();
            p->connected_at = now_sec();
            p->ip = ip;
            p->c = new conn();
            p->c->attach(fd);

            _peers.push_back(p);

            tms_infof("co-op: incoming connection from %s (peer %d)", ip.c_str(), p->id);
        }

        /* pump peers */
        for (size_t x = 0; x < _peers.size(); ) {
            peer *p = _peers[x];

            bool alive = p->c && p->c->pump();

            /* Loading a level (round start, rebuild from the host seed)
             * blocks the main loop for seconds. Counting that as silence is
             * what dropped everyone exactly when a round started. */
            if (alive && _loading) {
                p->last_recv = now_sec();
                p->connected_at = now_sec();
            }

            if (alive && p->last_recv > 0.0
                    && now_sec() - p->last_recv > MP_TIMEOUT) {
                tms_infof("co-op: peer %d timed out", p->id);
                p->c->close();
                alive = false;
            }

            /* a socket that connects and never introduces itself (port
             * scanner, half-open connection) must not hold a player slot */
            if (alive && !p->ready && p->connected_at > 0.0
                    && now_sec() - p->connected_at > MP_HANDSHAKE_TIMEOUT) {
                tms_infof("co-op: peer %d never completed the handshake", p->id);
                p->c->close();
                alive = false;
            }

            if (alive) {
                uint8_t type;
                buf b;
                while (p->c->next(&type, &b)) {
                    p->last_recv = now_sec();
                    queue_message(type, b, p->id);
                }
            }

            if (!p->c || !p->c->valid()) {
                if (p->ready) {
                    hud_add("%s left the game", p->name.c_str());

                    buf l;
                    l.w_i32(p->id);
                    l.w_str(p->name.c_str());
                    broadcast(MSG_PEER_LEAVE, l, p->id);
                }

                /* remove the character of the player that left */
                if (p->avatar_id && W && G) {
                    entity *av = W->get_entity_by_id(p->avatar_id);

                    if (av)
                        safe_delete_entity(av);

                    buf d;
                    d.w_u32(p->avatar_id);
                    broadcast(MSG_DELETE, d, p->id);

                    _xf.erase(p->avatar_id);
                    _sig.erase(p->avatar_id);
                    _awake.erase(p->avatar_id);
                    _remote_touch.erase(p->avatar_id);
                    _avatars.erase(p->avatar_id);
                }

                bool was_waiting = p->wants_play || _local_ready;

                if (p->c) {
                    p->c->close();
                    delete p->c;
                }
                delete p;
                _peers.erase(_peers.begin() + x);
                update_status();
                send_roster();

                /* the player we were waiting for is gone, re-evaluate */
                if (was_waiting && !_session_playing) host_check_ready();

                continue;
            }

            ++x;
        }
    } else if (mode == MODE_CLIENT) {
        /* see the host side: a level load is not silence */
        if (_loading) _last_recv = now_sec();

        if (_last_recv > 0.0 && now_sec() - _last_recv > MP_TIMEOUT) {
            shutdown("the server stopped responding");
            ui::message("Co-op: the server stopped responding");
            return;
        }

        if (!_server.pump()) {
            shutdown("lost connection to the server");
            ui::message("Co-op: lost connection to the server");
            return;
        }

        uint8_t type;
        buf b;
        while (_server.next(&type, &b)) {
            _last_recv = now_sec();
            queue_message(type, b, -1);
        }
    }

    /* One single place where remote changes touch the world. */
    drain_messages();

    /* weapons for characters that only just arrived */
    flush_pending_loadouts();

    if (mode == MODE_OFF) return;

    /* Our own character, whatever the last level (re)load, round start or
     * world wipe did to it. Everything below needs it. */
    resolve_local_avatar();

    /* Still nothing to control? The answer may have been dropped with the
     * message queue during a level load - ask the host again. */
    if (mode == MODE_CLIENT && !_local_avatar && _server.valid()) {
        static double last_avatar_req = 0.0;
        double t = now_sec();

        if (t - last_avatar_req > 1.5) {
            last_avatar_req = t;

            buf r;
            _server.send(MSG_REQUEST_AVATARS, r);
        }
    }

    if (_session_playing) {
        if (mode == MODE_HOST) {
            /* our own character */
            input_state in = read_local_input();

            /* In an adventure level the game itself turns the host's keys
             * into movement, jumps and shots. Driving the same character a
             * second time from here made the host fire twice per click and
             * fight the game over move/stop every single frame. */
            if (!W->is_adventure())
                apply_input(_local_avatar, in, &_local_input_prev);

            _local_input_prev = in;
            _local_input = in;

            /* the characters of everyone else */
            for (size_t x = 0; x < _peers.size(); ++x) {
                peer *p = _peers[x];
                if (!p->ready || !p->avatar_id) continue;

                apply_input(p->avatar_id, p->input, &p->prev_input);
                p->prev_input = p->input;
            }

            /* pending layer switches (retried until they succeed) */
            process_layer_requests();
        } else {
            send_local_input();
        }

        /* Both players are authoritative for their own character: each side
         * streams what it owns (positions 20 Hz unreliable, state changes,
         * layer switches and shots reliable) and corrects everything it does
         * not own towards the stream it receives. */
        sync::send_tick();
        sync::recv_tick();
    }

    /* Loading a level resets the global object id counter to 1, which would
     * make everything we build afterwards collide with the host's ids (two
     * different objects sharing an id desynchronizes everything that
     * references them). Restore the block the host reserved for us. */
    if (mode == MODE_CLIENT && _id_base && of::_id < _id_base)
        of::_id = _id_base;

    sync_xforms();
    sync_group_xforms();
    sync_entity_settings();

    /* keepalive so dead connections are noticed even when idle */
    {
        double t = now_sec();

        if (t - _last_ping > MP_PING_INTERVAL) {
            _last_ping = t;

            buf p;
            broadcast(MSG_PING, p);

            if (mode == MODE_HOST) {
                for (size_t x = 0; x < _peers.size(); ++x)
                    if (_peers[x] && _peers[x]->c && _peers[x]->c->valid())
                        _peers[x]->ping_sent = t;
            } else {
                _local_ping_sent = t;
            }
        }
    }

    process_pending_play();
}

/* ------------------------------------------------------------ local hooks */

/* Both players are authoritative for what they place, delete, connect or
 * re-configure themselves, in build mode and during a round alike. The host
 * only relays those messages to the remaining clients. */
static bool local_changes_are_authoritative() {
    if (mode == MODE_HOST) return true;

    /* A client sends what it builds (spawn, connect, delete, settings) as a
     * request; the host validates it, applies it to its world and relays it.
     * Physics, animation and damage stay host-only. */
    return mode == MODE_CLIENT;
}

void on_local_spawn(entity *e) {
    if (e) _client_doomed.erase(e->id);
    if (mode == MODE_OFF || applying || !e) return;
    if (!local_changes_are_authoritative()) return;
    if (!is_streamable(e)) return;

    xform_cache c;
    b2Vec2 p = e->get_position();
    c.x = p.x;
    c.y = p.y;
    c.angle = e->get_angle();
    c.layer = e->get_layer();
    _xf[e->id] = c;

    buf b;
    if (write_entity(&b, e))
        broadcast(MSG_SPAWN, b);
}

void on_local_delete(uint32_t entity_id) {
    if (mode == MODE_OFF || applying || entity_id == 0) return;
    if (!local_changes_are_authoritative()) return;

    _xf.erase(entity_id);

    buf b;
    b.w_u32(entity_id);
    broadcast(MSG_DELETE, b);
}

void on_local_connection(connection *c, int option) {
    if (mode == MODE_OFF || applying || !c || !c->e || !c->o) return;
    if (!local_changes_are_authoritative()) return;

    buf b;
    b.w_u32(c->e->id);
    b.w_u32(c->o->id);
    b.w_u8(c->type);
    b.w_u8(c->f[0]);
    b.w_u8(c->f[1]);
    b.w_f(c->p.x);
    b.w_f(c->p.y);
    b.w_f(c->p_s.x);
    b.w_f(c->p_s.y);
    b.w_i32(c->layer);
    b.w_f(c->max_force);
    b.w_f(c->damping);
    b.w_f(c->angle);
    b.w_i32(option);
    b.w_u8(c->render_type);
    write_conn_xforms(&b, c->e, c->o);

    broadcast(MSG_CONNECT, b);
}

bool is_syncing() {
    return mode != MODE_OFF && !applying;
}

static bool user_is_acting_on(uint32_t id) {
    if (!G || !W || !id) return false;

    entity *e = W->get_entity_by_id(id);
    if (!e) return false;

    if (G->selection.e == e) return true;
    if (G->selection.m && G->selection.m->count(e)) return true;

    return G->interacting_with(e) != 0;
}

void on_local_disconnect(uint32_t a, uint32_t bb) {
    if (mode == MODE_OFF || applying) return;
    if (!local_changes_are_authoritative()) return;

    /* a robot's own joints are rebuilt by the engine itself */
    if (is_player_avatar(a) || is_player_avatar(bb)) return;

    /* during a round the client's physics is only visual: a joint that
     * breaks there by itself must not break the real one on the host */
    if (mode == MODE_CLIENT && _session_playing
        && !user_is_acting_on(a) && !user_is_acting_on(bb))
        return;

    buf b;
    b.w_u32(a);
    b.w_u32(bb);
    broadcast(MSG_DISCONNECT, b);
}

void send_chat(const char *text) {
    if (mode == MODE_OFF || !text || !text[0]) return;

    buf b;
    b.w_str(local_name.c_str());
    b.w_str(text);

    broadcast(MSG_CHAT, b);

    hud_add("%s: %s", local_name.c_str(), text);
}


/* ------------------------------------------------------------------ glue */

/* The sync module needs a few facts about the session without knowing how
 * the session is put together. */
namespace glue {

void send_all(uint8_t type, const buf &b, bool reliable, int except_peer) {
    if (mode == MODE_HOST) {
        for (size_t x = 0; x < _peers.size(); ++x) {
            peer *p = _peers[x];
            if (!p || !p->c || !p->c->valid()) continue;
            if (p->id == except_peer) continue;

            p->c->send(type, b, reliable);
        }
    } else if (mode == MODE_CLIENT) {
        if (_server.valid()) _server.send(type, b, reliable);
    }
}

void avatar_ids(std::vector<uint32_t> *out) {
    if (!out) return;

    if (_local_avatar) out->push_back(_local_avatar);

    /* the registry is filled on both sides (MSG_AVATAR), so a client also
     * knows which robots belong to players */
    for (std::map<uint32_t, int>::iterator it = _avatars.begin();
         it != _avatars.end(); ++it) {
        if (!it->first || it->first == _local_avatar) continue;
        out->push_back(it->first);
    }

    for (size_t x = 0; x < _peers.size(); ++x) {
        peer *p = _peers[x];
        if (!p || !p->ready || !p->avatar_id) continue;
        if (p->avatar_id == _local_avatar) continue;
        if (_avatars.find(p->avatar_id) != _avatars.end()) continue;

        out->push_back(p->avatar_id);
    }
}

uint32_t local_avatar() { return _local_avatar; }
bool     is_avatar(uint32_t id) { return is_player_avatar(id); }
bool     playing() { return _session_playing; }
bool     is_host() { return mode == MODE_HOST; }
bool     is_client() { return mode == MODE_CLIENT; }
double   now() { return now_sec(); }
void     set_applying(bool v) { applying = v; }

}

}
