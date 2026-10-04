/* Principia co-op transport, ENet backend.
 *
 * One ENetHost per process: either a server host (listener::open) or a
 * client host (net_connect). Every ENetPeer gets a peer_state that buffers
 * the messages that arrived for it, so conn objects can be created, attached
 * and destroyed without losing anything.
 */

#include "mp_net.hh"

#include <enet/enet.h>

#include <map>

#include <tms/core/print.h>

namespace mp {

static char _err[256] = {0};
static bool _inited = false;

static ENetHost *_host = 0;      /* server or client host */
static bool      _is_server = false;

struct pending_msg_raw {
    uint8_t              type;
    std::vector<uint8_t> payload;
};

struct peer_state {
    ENetPeer                  *peer;
    std::string                ip;
    std::deque<pending_msg_raw> inq;
    bool                       dead;
    bool                       announced;  /* handed out by accept_one() */

    peer_state() : peer(0), dead(false), announced(false) {}
};

static std::map<ENetPeer*, peer_state*> _states;
static std::deque<ENetPeer*>            _accept_q;

static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(_err, sizeof(_err), fmt, ap);
    va_end(ap);
}

const char *net_last_error() { return _err[0] ? _err : "unknown network error"; }

static peer_state *state_of(ENetPeer *p, bool create) {
    if (!p) return 0;

    std::map<ENetPeer*, peer_state*>::iterator it = _states.find(p);
    if (it != _states.end()) return it->second;
    if (!create) return 0;

    peer_state *s = new peer_state();
    s->peer = p;

    char ip[64] = {0};
    enet_address_get_host_ip(&p->address, ip, sizeof(ip));
    s->ip = ip;

    _states[p] = s;
    p->data = s;

    return s;
}

static void drop_state(ENetPeer *p) {
    std::map<ENetPeer*, peer_state*>::iterator it = _states.find(p);
    if (it == _states.end()) return;

    delete it->second;
    _states.erase(it);

    if (p) p->data = 0;
}

bool net_init() {
    if (_inited) return true;

    if (enet_initialize() != 0) {
        set_error("could not initialize ENet");
        return false;
    }

    _inited = true;
    return true;
}

void net_quit() {
    if (!_inited) return;

    for (std::map<ENetPeer*, peer_state*>::iterator it = _states.begin();
         it != _states.end(); ++it)
        delete it->second;

    _states.clear();
    _accept_q.clear();

    if (_host) {
        enet_host_destroy(_host);
        _host = 0;
    }

    enet_deinitialize();
    _inited = false;
}

void net_service() {
    if (!_host) return;

    ENetEvent ev;

    /* timeout 0: never block the frame */
    while (enet_host_service(_host, &ev, 0) > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                peer_state *s = state_of(ev.peer, true);

                if (_is_server && s && !s->announced)
                    _accept_q.push_back(ev.peer);

                break;
            }

            case ENET_EVENT_TYPE_RECEIVE: {
                peer_state *s = state_of(ev.peer, true);

                if (s && ev.packet && ev.packet->dataLength >= 1
                        && ev.packet->dataLength <= MP_MAX_MSG_SIZE) {

                    pending_msg_raw m;
                    m.type = ev.packet->data[0];
                    m.payload.assign(ev.packet->data + 1,
                                     ev.packet->data + ev.packet->dataLength);
                    s->inq.push_back(m);
                }

                if (ev.packet) enet_packet_destroy(ev.packet);
                break;
            }

            case ENET_EVENT_TYPE_DISCONNECT: {
                peer_state *s = state_of(ev.peer, false);
                if (s) s->dead = true;

                if (ev.peer) ev.peer->data = s;
                break;
            }

            default:
                break;
        }
    }
}

intptr_t net_connect(const char *host, int port, int timeout_ms) {
    if (!net_init()) return -1;

    if (_host) {
        enet_host_destroy(_host);
        _host = 0;
    }

    _states.clear();
    _accept_q.clear();
    _is_server = false;

    _host = enet_host_create(0 /* client */, 1, MP_NUM_CHANNELS, 0, 0);

    if (!_host) {
        set_error("could not create the ENet client host");
        return -1;
    }

    ENetAddress addr;

    if (enet_address_set_host(&addr, host) != 0) {
        set_error("could not resolve %s", host);
        enet_host_destroy(_host);
        _host = 0;
        return -1;
    }

    addr.port = (enet_uint16)port;

    ENetPeer *p = enet_host_connect(_host, &addr, MP_NUM_CHANNELS, 0);

    if (!p) {
        set_error("no free peer slot");
        enet_host_destroy(_host);
        _host = 0;
        return -1;
    }

    if (timeout_ms <= 0) timeout_ms = 5000;

    ENetEvent ev;

    if (enet_host_service(_host, &ev, (enet_uint32)timeout_ms) > 0
            && ev.type == ENET_EVENT_TYPE_CONNECT) {

        state_of(p, true);
        return (intptr_t)p;
    }

    set_error("could not connect to %s:%d", host, port);
    enet_peer_reset(p);
    enet_host_destroy(_host);
    _host = 0;

    return -1;
}

void net_close_raw(intptr_t handle) {
    if (handle == -1) return;

    ENetPeer *p = (ENetPeer*)handle;

    enet_peer_disconnect_now(p, 0);
    drop_state(p);
}

int net_rtt(intptr_t handle) {
    if (handle == -1) return 0;

    ENetPeer *p = (ENetPeer*)handle;
    return (int)p->roundTripTime;
}

/* ------------------------------------------------------------------ conn */

void conn::attach(intptr_t handle) {
    this->close();

    if (handle == -1) return;

    ENetPeer *p = (ENetPeer*)handle;
    peer_state *s = state_of(p, true);

    this->fd = handle;
    this->dead = (s && s->dead);
}

void conn::close() {
    if (this->fd == -1) {
        this->dead = true;
        return;
    }

    ENetPeer *p = (ENetPeer*)this->fd;

    /* flush what is still queued, then go away */
    enet_peer_disconnect_later(p, 0);
    if (_host) enet_host_flush(_host);

    drop_state(p);

    this->fd = -1;
    this->dead = true;
}

void conn::send(uint8_t type, const buf &b, bool reliable) {
    if (!this->valid() || !_host) return;

    size_t len = 1 + b.d.size();

    if (len > MP_MAX_MSG_SIZE) {
        tms_errorf("co-op: refusing to send a %zu byte message", len);
        return;
    }

    enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket *pk = enet_packet_create(0, len, flags);

    if (!pk) return;

    pk->data[0] = type;
    if (!b.d.empty()) memcpy(pk->data + 1, &b.d[0], b.d.size());

    ENetPeer *p = (ENetPeer*)this->fd;

    if (enet_peer_send(p, reliable ? MP_CHAN_RELIABLE : MP_CHAN_UNRELIABLE, pk) < 0) {
        enet_packet_destroy(pk);
        return;
    }

    this->bytes_out += len;
}

void conn::send_empty(uint8_t type, bool reliable) {
    buf b;
    this->send(type, b, reliable);
}

bool conn::pump() {
    net_service();

    if (this->fd == -1) return false;

    peer_state *s = state_of((ENetPeer*)this->fd, false);

    if (!s || s->dead) {
        this->dead = true;
        return false;
    }

    return true;
}

bool conn::next(uint8_t *type, buf *payload) {
    if (this->fd == -1) return false;

    peer_state *s = state_of((ENetPeer*)this->fd, false);
    if (!s || s->inq.empty()) return false;

    pending_msg_raw &m = s->inq.front();

    *type = m.type;
    payload->clear();
    payload->d.swap(m.payload);
    payload->rp = 0;
    payload->err = false;

    this->bytes_in += payload->d.size() + 1;

    s->inq.pop_front();

    return true;
}

size_t conn::pending_out() const {
    if (this->fd == -1) return 0;
    ENetPeer *p = (ENetPeer*)this->fd;
    return (size_t)p->reliableDataInTransit;
}

/* -------------------------------------------------------------- listener */

bool listener::open(int port) {
    if (!net_init()) return false;

    this->close();

    if (_host) {
        enet_host_destroy(_host);
        _host = 0;
    }

    _states.clear();
    _accept_q.clear();

    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = (enet_uint16)port;

    /* a few slots more than MP_MAX_PLAYERS so a rejected client still gets a
     * proper answer instead of being silently dropped */
    _host = enet_host_create(&addr, 16, MP_NUM_CHANNELS, 0, 0);

    if (!_host) {
        set_error("could not bind UDP port %d", port);
        return false;
    }

    _is_server = true;
    this->fd = 1;

    tms_infof("co-op: ENet server listening on UDP %d", port);

    return true;
}

void listener::close() {
    if (this->fd == -1) return;

    this->fd = -1;

    /* the host itself is destroyed by net_quit() or the next open() so peers
     * that are still disconnecting get their packets out */
}

intptr_t listener::accept_one(std::string *out_ip) {
    if (this->fd == -1) return -1;

    net_service();

    while (!_accept_q.empty()) {
        ENetPeer *p = _accept_q.front();
        _accept_q.pop_front();

        peer_state *s = state_of(p, false);
        if (!s || s->dead) continue;

        s->announced = true;

        if (out_ip) *out_ip = s->ip;

        return (intptr_t)p;
    }

    return -1;
}

}
