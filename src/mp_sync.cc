/* Principia co-op: world synchronization, host is the authority. */

#include "mp_sync.hh"
#include "multiplayer.hh"

#include "adventure.hh"
#include "creature.hh"
#include "entity.hh"
#include "game.hh"
#include "robot_base.hh"
#include "robot_parts.hh"
#include "world.hh"

#include <tms/core/print.h>
#include <tms/math/misc.h>
#include <cmath>
#include <map>
#include <algorithm>

namespace mp {
namespace sync {

/* ------------------------------------------------------------- host state */

struct anim_state {
    int      state;
    uint64_t flags;
    int      look_dir;
    int      dir;
    int      layer;

    anim_state() : state(-1), flags(0), look_dir(0), dir(0), layer(-1) {}

    bool differs(const anim_state &o) const {
        return this->state != o.state
            || this->flags != o.flags
            || this->look_dir != o.look_dir
            || this->dir != o.dir;
    }
};

static std::map<uint32_t, anim_state> _sent_state;   /* host: last state sent */
static std::map<uint32_t, double>     _last_moved;   /* host: body activity */
static std::map<uint32_t, int>        _last_cd;      /* host: weapon cooldown */
static std::map<uint32_t, float> _sent_hp;     /* host: last hp sent */
static std::map<uint32_t, bool>  _sent_dead;   /* host: last life state sent */
static double _last_hp_sync = 0.0;
static double _last_diag = 0.0;
static double _last_player_sync = 0.0;
static double _last_body_sync   = 0.0;

/* ----------------------------------------------------------- client state */

struct target {
    float  x, y, angle;
    float  vx, vy, w;
    int    layer;
    double t;        /* arrival time */
    bool   has_prev;
    float  px, py, pangle;

    target() : x(0), y(0), angle(0), vx(0), vy(0), w(0), layer(-1), t(0.0),
               has_prev(false), px(0), py(0), pangle(0) {}
};

static std::map<uint32_t, target> _targets;

void reset() {
    _sent_state.clear();
    _last_moved.clear();
    _last_cd.clear();
    _sent_hp.clear();
    _sent_dead.clear();
    _last_hp_sync = 0.0;
    _targets.clear();
    _last_player_sync = 0.0;
    _last_body_sync = 0.0;
}

/* ------------------------------------------------------------- utilities */

/* The characters this machine is responsible for. Both players simulate, so
 * each side owns exactly one robot - its own - and receives everybody
 * else's. Streaming a character we do not own would fight its owner. */
static void owned_ids(std::vector<uint32_t> *out) {
    /* HOST AS AUTHORITY: the host simulates and streams EVERY player
     * character (its own and the remote ones it drives from MSG_INPUT).
     * A client owns nothing - it only sends input. */
    if (!glue::is_host()) return;

    glue::avatar_ids(out);

    uint32_t me = glue::local_avatar();
    if (me && std::find(out->begin(), out->end(), me) == out->end())
        out->push_back(me);
}

static inline creature *creature_by_id(uint32_t id) {
    if (!W || !id) return 0;

    entity *e = W->get_entity_by_id(id);
    if (!e || !e->is_creature()) return 0;

    return static_cast<creature*>(e);
}

static void write_body(buf *b, entity *e) {
    b2Body *bd = e->get_body(0);

    b2Vec2 p = bd->GetPosition();
    b2Vec2 v = bd->GetLinearVelocity();

    b->w_u32(e->id);
    b->w_f(p.x);
    b->w_f(p.y);
    b->w_f(bd->GetAngle());
    b->w_f(v.x);
    b->w_f(v.y);
    b->w_f(bd->GetAngularVelocity());
    b->w_i8((int8_t)e->get_layer());
}

static uint32_t read_target(buf *b) {
    uint32_t id = b->r_u32();

    target t;
    t.x     = b->r_f();
    t.y     = b->r_f();
    t.angle = b->r_f();
    t.vx    = b->r_f();
    t.vy    = b->r_f();
    t.w     = b->r_f();
    t.layer = (int)b->r_i8();
    t.t     = glue::now();

    if (b->err || !id) return 0;

    /* our own character is driven by our own input and corrected softly, so
     * keep the previous sample for interpolation */
    std::map<uint32_t, target>::iterator it = _targets.find(id);

    if (it != _targets.end()) {
        t.has_prev = true;
        t.px = it->second.x;
        t.py = it->second.y;
        t.pangle = it->second.angle;
    }

    _targets[id] = t;

    return id;
}

/* A player packet carries two more things than a plain body: the angle of
 * the arm and the direction the character is facing. Without them a remote
 * player stood there with its gun pointing at the floor and never turned
 * around between two state messages - which is exactly what made the
 * animations look broken. */
static void read_player(buf *b) {
    uint32_t id = read_target(b);

    float  aim  = b->r_f();
    int8_t look = b->r_i8();

    if (b->err || !id) return;

    /* our own arm is aimed by our own mouse, with no latency at all */
    if (id == glue::local_avatar()) return;

    creature *c = creature_by_id(id);
    if (!c) return;

    glue::set_applying(true);

    if (look) c->look((int)look, true);

    robot_parts::weapon *wp = c->get_weapon();
    if (wp) wp->set_arm_angle_raw(aim);

    glue::set_applying(false);
}

/* ------------------------------------------------------------- host: send */

/* Positions of every player, one packet, unreliable: a lost packet is
 * replaced by the next one 50 ms later, resending it would only add lag. */
static void sync_players(double t) {
    if (t - _last_player_sync < MP_SYNC_INTERVAL) return;
    _last_player_sync = t;

    std::vector<uint32_t> ids;
    owned_ids(&ids);

    if (ids.empty()) return;

    buf b;
    b.w_u16((uint16_t)ids.size());

    uint16_t n = 0;

    for (size_t x = 0; x < ids.size(); ++x) {
        entity *e = W->get_entity_by_id(ids[x]);
        if (!e || !e->get_body(0)) continue;

        write_body(&b, e);

        creature *c = e->is_creature() ? static_cast<creature*>(e) : 0;
        robot_parts::weapon *wp = c ? c->get_weapon() : 0;

        /* where the gun points and which way the character faces */
        b.w_f(wp ? wp->get_arm_angle() : 0.f);
        b.w_i8(c ? (int8_t)c->look_dir : (int8_t)0);

        ++n;
    }

    if (!n) return;

    /* patch the real count back in */
    b.d[0] = (uint8_t)(n & 0xff);
    b.d[1] = (uint8_t)((n >> 8) & 0xff);

    glue::send_all(MSG_PLAYER_POS, b, false);
}

/* Everything else that moves: dynamic bodies only, and only while they
 * actually move. A crate that came to rest is sent once more and then
 * dropped from the stream. */
static void sync_bodies(double t) {
    if (t - _last_body_sync < MP_SYNC_INTERVAL) return;
    _last_body_sync = t;

    buf b;
    b.w_u16(0);
    uint16_t n = 0;

    for (std::map<uint32_t, entity*>::iterator it = W->all_entities.begin();
         it != W->all_entities.end(); ++it) {

        entity *e = it->second;
        if (!e) continue;

        b2Body *bd = e->get_body(0);
        if (!bd) continue;

        /* static and kinematic geometry is identical on every machine */
        if (bd->GetType() != b2_dynamicBody) continue;

        /* players have their own, higher priority stream */
        if (glue::is_avatar(e->id)) continue;

        b2Vec2 v = bd->GetLinearVelocity();
        float sq = v.x*v.x + v.y*v.y;
        float w = bd->GetAngularVelocity();

        bool moving = bd->IsAwake()
                   && (sq > MP_SYNC_VEL_EPS || fabsf(w) > 0.2f);

        if (moving) {
            _last_moved[e->id] = t;
        } else {
            std::map<uint32_t, double>::iterator m = _last_moved.find(e->id);

            /* keep streaming a little after it stopped so both sides agree
             * on the resting place, then stop sending it entirely */
            if (m == _last_moved.end()) continue;

            if (t - m->second > MP_SYNC_STILL_HOLD) {
                _last_moved.erase(m);
                continue;
            }
        }

        write_body(&b, e);
        ++n;

        if (n >= MP_SYNC_BATCH) {
            b.d[0] = (uint8_t)(n & 0xff);
            b.d[1] = (uint8_t)((n >> 8) & 0xff);
            glue::send_all(MSG_BODY_POS, b, false);

            b.clear();
            b.w_u16(0);
            n = 0;
        }
    }

    if (!n) return;

    b.d[0] = (uint8_t)(n & 0xff);
    b.d[1] = (uint8_t)((n >> 8) & 0xff);
    glue::send_all(MSG_BODY_POS, b, false);
}

/* Animation: only the transitions go over the wire. Between two of these
 * messages the client plays the animation itself, at its own frame rate,
 * which is what makes a remote player walk smoothly instead of stepping a
 * packet at a time. */
static void sync_creature_states() {
    std::vector<uint32_t> ids;
    owned_ids(&ids);

    for (size_t x = 0; x < ids.size(); ++x) {
        creature *c = creature_by_id(ids[x]);
        if (!c) continue;

        anim_state cur;
        cur.state    = c->get_state();
        cur.flags    = c->creature_flags & MP_ANIM_FLAG_MASK;
        cur.look_dir = c->look_dir;
        cur.dir      = c->dir;
        cur.layer    = c->get_layer();

        anim_state &old = _sent_state[ids[x]];

        if (cur.differs(old)) {
            buf b;
            b.w_u32(ids[x]);
            b.w_i32(cur.state);
            b.w_u64(cur.flags);
            b.w_i8((int8_t)cur.look_dir);
            b.w_i8((int8_t)cur.dir);

            glue::send_all(MSG_CREATURE_STATE, b, true);
        }

        if (cur.layer != old.layer) {
            b2Vec2 p = c->get_position();

            buf b;
            b.w_u32(ids[x]);
            b.w_i8((int8_t)cur.layer);
            b.w_f(p.x);
            b.w_f(p.y);

            glue::send_all(MSG_LAYER, b, true);
        }

        old = cur;
    }
}

/* A shot is not an input event. The host fires its own character through
 * the game itself (adventure::step), so nothing in apply_input() ever saw
 * it - that is why the client never got the host's shots. A held trigger
 * also fires once per cooldown, not once per frame. The rising edge of the
 * weapon cooldown is the one signal that means "a shot just left the
 * barrel", whoever pulled the trigger and whatever weapon it is. */
static void watch_shots() {
    std::vector<uint32_t> ids;
    owned_ids(&ids);

    for (size_t x = 0; x < ids.size(); ++x) {
        creature *c = creature_by_id(ids[x]);
        robot_parts::weapon *wp = c ? c->get_weapon() : 0;

        if (!wp) { _last_cd.erase(ids[x]); continue; }

        int cd = wp->cooldown_timer;

        std::map<uint32_t, int>::iterator it = _last_cd.find(ids[x]);
        int prev = (it == _last_cd.end()) ? 0 : it->second;

        _last_cd[ids[x]] = cd;

        if (prev <= 0 && cd > 0) on_shot(c);
    }
}

/* Damage results. creature::damage() only accumulates, the hp really changes
 * in creature::apply_damages() during the step, so the result is sampled
 * here: only the host, only on change, at most 20 times per second. */
static void sync_hp(double t) {
    if (t - _last_hp_sync < MP_SYNC_INTERVAL) return;
    _last_hp_sync = t;

    for (std::map<uint32_t, entity*>::iterator it = W->all_entities.begin();
         it != W->all_entities.end(); ++it) {
        entity *e = it->second;
        if (!e || !e->is_creature()) continue;

        creature *c = static_cast<creature*>(e);
        float hp   = c->get_hp();
        bool  dead = c->is_dead();

        std::map<uint32_t, float>::iterator h = _sent_hp.find(e->id);
        std::map<uint32_t, bool>::iterator  d = _sent_dead.find(e->id);

        /* first sight: remember, the snapshot already carried the hp */
        if (h == _sent_hp.end() || d == _sent_dead.end()) {
            _sent_hp[e->id] = hp;
            _sent_dead[e->id] = dead;
            continue;
        }

        if (dead != d->second) {
            if (dead) mp::send_event(MP_EV_DEATH, e->id, 0.f);
            else      mp::send_event(MP_EV_RESPAWN, e->id, hp);
        } else if (fabsf(hp - h->second) > 0.01f) {
            mp::send_event(MP_EV_DAMAGE, e->id, hp);
        } else {
            continue;
        }

        h->second = hp;
        d->second = dead;
    }
}

/* One line per second per character, on both sides: enough to tell a wrong
 * avatar, a missing weapon and a stuck aim apart without a debugger. */
static void diag(double t) {
    if (t - _last_diag < 1.0) return;
    _last_diag = t;

    uint32_t adv = adventure::player ? adventure::player->id : 0;
    uint32_t fol = (G && G->follow_object) ? G->follow_object->id : 0;

    tms_infof("mp diag [%s] local_avatar=%u adventure::player=%u adventure_id=%u follow=%u",
              glue::is_host() ? "host" : "client",
              glue::local_avatar(), adv,
              G ? G->state.adventure_id : 0, fol);

    std::vector<uint32_t> ids;
    glue::avatar_ids(&ids);

    for (size_t x = 0; x < ids.size(); ++x) {
        creature *c = creature_by_id(ids[x]);

        if (!c) {
            tms_infof("mp diag   avatar %u: MISSING in this world", ids[x]);
            continue;
        }

        robot_parts::weapon *wp = c->get_weapon();
        b2Vec2 p = c->get_position();

        tms_infof("mp diag   avatar %u%s: look=%d dir=%d state=%d layer=%d wep=%d arm=%.3f cd=%d pos=%.2f,%.2f",
                  ids[x], ids[x] == glue::local_avatar() ? " (mine)" : "",
                  c->look_dir, c->dir, c->get_state(), c->get_layer(),
                  wp ? wp->get_arm_type() : -1,
                  wp ? wp->get_arm_angle() : 0.f,
                  wp ? wp->cooldown_timer : -1,
                  p.x, p.y);
    }
}

void send_tick() {
    if (!W || !G || !glue::playing()) return;
    if (W->is_paused()) return;

    /* only the host streams world state; a client sends MSG_INPUT only */
    if (!glue::is_host()) return;

    double t = glue::now();

    /* every player character: shots, position, animation state, layer */
    watch_shots();
    sync_players(t);
    sync_creature_states();

    /* Everything that is not a player is still owned by the host alone: two
     * machines pushing the same crate over the wire fight each other. */
    sync_bodies(t);
    sync_hp(t);

    diag(t);
}

/* ----------------------------------------------------------- host: events */

/* Only the shot itself travels: where it started, where it points and what
 * fired it. The projectile is a local, purely visual object on every other
 * machine - the host decides who got hit. */
void on_shot(creature *shooter) {
    if (!shooter) return;

    robot_parts::weapon *wp = shooter->get_weapon();
    if (!wp) return;

    /* The weapon fires from the arm, not from the middle of the robot, and
     * it uses look_dir plus the body angle - exactly like
     * robot_parts::arm_cannon::attack() does. */
    float a = shooter->get_angle() + (float)M_PI*1.5f
            + shooter->look_dir * wp->get_arm_angle() * (float)M_PI;

    b2Vec2 p = shooter->local_to_world(ROBOT_ARM_POS, 0);

    float dx, dy;
    tmath_sincos(a, &dy, &dx);

    buf b;
    b.w_u32(shooter->id);
    b.w_f(p.x);
    b.w_f(p.y);
    b.w_f(-dx);          /* the game negates the vector before emitting */
    b.w_f(-dy);
    b.w_u8((uint8_t)wp->get_arm_type());
    b.w_i8((int8_t)shooter->get_layer());
    b.w_f(wp->get_arm_angle());

    glue::send_all(MSG_SHOT, b, true);
}

/* --------------------------------------------------------- client: apply */

static void apply_creature_state(buf *b) {
    uint32_t id    = b->r_u32();
    int32_t  state = b->r_i32();
    uint64_t flags = b->r_u64();
    int8_t   look  = b->r_i8();
    int8_t   dir   = b->r_i8();

    if (b->err) return;

    /* Everyone drives their own character themselves now (the client
     * simulates as well), so the echo of our own state is always one round
     * trip old: applying it made the body flip between the direction we are
     * pressing and the one the host knew about half a frame ago. */
    creature *c = creature_by_id(id);
    if (!c) return;

    glue::set_applying(true);

    if (id == glue::local_avatar()) {
        /* Our own character is predicted locally from our own input, so the
         * input-driven flags (MOVING_*, look_dir, dir) stay local - echoing
         * them back one RTT late is what made walking stutter. What the host
         * decides on its own is authoritative: death, frozen, lost balance. */
        const uint64_t auth = CREATURE_FROZEN | CREATURE_LOST_BALANCE;
        c->creature_flags = (c->creature_flags & ~auth) | (flags & auth);

        if (state == CREATURE_DEAD && c->get_state() != CREATURE_DEAD)
            c->set_state(state);
        else if (state != CREATURE_DEAD && c->get_state() == CREATURE_DEAD)
            c->set_state(state);
    } else {
        /* remote creature: adopt the transition, the animation then plays
         * locally until the next state message */
        c->creature_flags = (c->creature_flags & ~(uint64_t)MP_ANIM_FLAG_MASK)
                          | (flags & MP_ANIM_FLAG_MASK);

        c->look_dir = look;
        c->dir      = dir;

        if (c->get_state() != state)
            c->set_state(state);
    }

    glue::set_applying(false);
}

static void apply_layer(buf *b) {
    uint32_t id    = b->r_u32();
    int8_t   layer = b->r_i8();
    float    x     = b->r_f();
    float    y     = b->r_f();

    if (b->err) return;

    /* HOST AS AUTHORITY: a client never switches layers on its own (see
     * mp::suppress_local_layermove), it only requests it. This message is
     * the one and only place where the switch happens on the client - for
     * remote creatures and for our own character alike. */
    creature *c = creature_by_id(id);
    if (!c) return;

    glue::set_applying(true);

    if (c->get_layer() != (int)layer) {
        int dir = (int)layer - c->get_layer();

        /* a real layermove() also plays the lean-in animation; the hard set
         * stays as a fallback for a refused or multi-layer jump */
        if ((dir != 1 && dir != -1) || !c->layermove(dir)) {
            c->layer_old = c->layer_new;
            c->layer_blend = 0.f;
            c->set_layer((int)layer);
        }
    }

    /* our own character keeps its predicted position (a big error is still
     * snapped by recv_tick), remote ones jump to the host's position */
    if (id != glue::local_avatar()) {
        c->set_position(x, y);
        c->update();
    }

    std::map<uint32_t, target>::iterator it = _targets.find(id);
    if (it != _targets.end()) {
        it->second.x = x;
        it->second.y = y;
        it->second.layer = (int)layer;
    }

    glue::set_applying(false);
}

static void apply_shot(buf *b) {
    uint32_t id  = b->r_u32();
    float    x   = b->r_f();
    float    y   = b->r_f();
    float    dx  = b->r_f();
    float    dy  = b->r_f();
    uint8_t  wid = b->r_u8();
    int8_t   lay = b->r_i8();
    float    arm = b->r_f();

    if (b->err) return;

    /* A shot can arrive while this side is still loading the level or is
     * paused (start of a round, back to build mode). The shotgun creates
     * its 8 pellets through game::emit() and game::timed_absorb(), and both
     * abort the game when the world is paused - that was the crash. */
    if (!W || !G || W->is_paused() || !mp::session_playing()) return;

    /* our own shot was already fired locally by the game - replaying the
     * echo of it would fire twice per click */
    if (id == glue::local_avatar()) return;

    creature *c = creature_by_id(id);
    if (!c) return;

    /* a body that is not built (yet) or a dead character can not shoot -
     * the shotgun pushes the body back and reads its velocity */
    if (!c->body || !c->get_body(0) || c->is_dead()) return;

    robot_parts::weapon *wp = c->get_weapon();

    /* the character is holding something else here - do not fake a shot */
    if (!wp || wp->get_arm_type() != (int)wid) return;

    (void)x; (void)y; (void)lay;

    glue::set_applying(true);

    /* Put the arm exactly where the host had it, then let the weapon do its
     * own thing: muzzle flash, sound and a local projectile. Its flight path
     * may differ slightly from the host's, which is fine - only the host
     * decides who was hit (MP_EV_DAMAGE). */
    wp->set_arm_angle_raw(arm);
    wp->cooldown_timer = 0;

    if (!wp->is_melee()) {
        float len = sqrtf(dx*dx + dy*dy);
        if (len > 0.0001f) c->look(dx > 0.f ? 1 : -1, true);
    }

    c->attack();

    glue::set_applying(false);
}

bool handle(uint8_t type, buf *b) {
    switch (type) {
        case MSG_PLAYER_POS: {
            uint16_t n = b->r_u16();

            for (uint16_t x = 0; x < n && !b->err; ++x)
                read_player(b);

            return true;
        }

        case MSG_BODY_POS: {
            uint16_t n = b->r_u16();

            for (uint16_t x = 0; x < n && !b->err; ++x)
                read_target(b);

            return true;
        }

        case MSG_CREATURE_STATE: apply_creature_state(b); return true;
        case MSG_LAYER:          apply_layer(b);          return true;
        case MSG_SHOT:           apply_shot(b);           return true;
    }

    return false;
}

/* ------------------------------------------------ client: soft correction */

/* The client simulates locally (that is what keeps movement smooth between
 * two 20 Hz packets) and is nudged back towards the host: a small error is
 * blended away, a big one means we are looking at something completely
 * different and is snapped. */
void recv_tick() {
    if (!W || !glue::playing()) return;

    double t = glue::now();

    for (std::map<uint32_t, target>::iterator it = _targets.begin();
         it != _targets.end(); ) {

        entity *e = W->get_entity_by_id(it->first);

        if (!e || !e->get_body(0)) {
            /* the object is gone or not loaded here - drop it after a while */
            if (t - it->second.t > 10.0) { _targets.erase(it++); continue; }
            ++it;
            continue;
        }

        /* the local player is dragging it: the host follows our MSG_XFORM,
         * fighting the mouse here would make the object jitter */
        if (G && G->interacting_with(e)) { ++it; continue; }

        target &tg = it->second;
        b2Body *bd = e->get_body(0);

        /* dead reckoning: where the host's object should be by now */
        float dt = (float)(t - tg.t);
        if (dt < 0.f) dt = 0.f;
        if (dt > 0.5f) dt = 0.5f;

        float tx = tg.x + tg.vx * dt;
        float ty = tg.y + tg.vy * dt;
        float ta = tg.angle + tg.w * dt;

        b2Vec2 cur = bd->GetPosition();

        float ex = tx - cur.x;
        float ey = ty - cur.y;
        float err = sqrtf(ex*ex + ey*ey);

        glue::set_applying(true);

        if (tg.layer >= 0 && e->get_layer() != tg.layer && !e->is_creature())
            e->set_layer(tg.layer);

        /* A creature is a compound of bodies held together by joints. Forcing
         * the angle of the torso alone fights those joints and makes the
         * body spin, so for creatures only the position is corrected and the
         * pose is left to the local simulation. */
        bool keep_angle = e->is_creature();

        /* Our own character is simulated here with zero latency, while the
         * picture the host sends is always one round trip old. Blending
         * towards it every single tick is what rubber-banded the movement
         * and made the animation stutter, so only a real disagreement
         * (> MP_SYNC_SNAP_DIST) is corrected for ourselves. */
        bool own = (it->first == glue::local_avatar());

        if (err > MP_SYNC_SNAP_DIST) {
            /* too far off to blend - we are simply wrong */
            bd->SetTransform(b2Vec2(tx, ty), keep_angle ? bd->GetAngle() : ta);
            bd->SetLinearVelocity(b2Vec2(tg.vx, tg.vy));
            if (!keep_angle) bd->SetAngularVelocity(tg.w);
            e->update();
        } else if (err > (own ? MP_SYNC_OWN_TOLERANCE : MP_SYNC_DEADZONE)) {
            float k = own ? MP_SYNC_OWN_LERP : MP_SYNC_LERP;
            b2Vec2 np(cur.x + ex * k, cur.y + ey * k);

            float da = ta - bd->GetAngle();
            while (da > (float)M_PI) da -= 2.f*(float)M_PI;
            while (da < -(float)M_PI) da += 2.f*(float)M_PI;

            bd->SetTransform(np, keep_angle ? bd->GetAngle()
                                           : bd->GetAngle() + da * MP_SYNC_LERP);

            /* adopt the host's velocity so our own physics keeps predicting
             * in the right direction instead of fighting the correction */
            if (!own) {
                bd->SetLinearVelocity(b2Vec2(tg.vx, tg.vy));
                if (!keep_angle) bd->SetAngularVelocity(tg.w);
            }

            if (!bd->IsAwake()) bd->SetAwake(true);
        }

        glue::set_applying(false);

        ++it;
    }
}

}
}
