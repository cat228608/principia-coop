#pragma once

/* Principia co-op: low level networking.
 *
 * Transport: ENet (UDP) - reliable channel 0, unreliable channel 1.
 *
 * The public API is deliberately identical to the previous TCP version so
 * multiplayer.cc keeps working:
 *
 *   - net_connect() still returns an opaque intptr_t handle (an ENetPeer*)
 *   - conn::attach()/pump()/next()/send()/close() are unchanged
 *   - listener::open()/accept_one()/close() are unchanged
 *
 * New:
 *   - conn::send(type, buf, false) sends on the unreliable channel
 *   - net_service() drains the ENet event loop; it is called for you from
 *     conn::pump() and listener::accept_one(), so no extra call is needed
 *     in game code.
 *
 * ENet keeps message boundaries, so the old [u8 type][u32 size][payload]
 * framing is gone: a packet is [u8 type][payload].
 */

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

/* a single message may never be bigger than this */
#define MP_MAX_MSG_SIZE   (32u*1024u*1024u)

/* ENet channels */
#define MP_CHAN_RELIABLE    0
#define MP_CHAN_UNRELIABLE  1
#define MP_NUM_CHANNELS     2

namespace mp {

/* Little endian serialization buffer (unchanged) */
class buf {
  public:
    std::vector<uint8_t> d;
    size_t rp;
    bool err;

    buf() : rp(0), err(false) {}

    inline void clear() { this->d.clear(); this->rp = 0; this->err = false; }
    inline void rewind() { this->rp = 0; this->err = false; }
    inline size_t size() const { return this->d.size(); }
    inline size_t left() const { return this->d.size() - this->rp; }
    inline bool eof(size_t n) const { return this->rp + n > this->d.size(); }

    inline void w_u8(uint8_t v) { this->d.push_back(v); }
    inline void w_i8(int8_t v) { this->d.push_back((uint8_t)v); }
    inline void w_bool(bool v) { this->w_u8(v ? 1 : 0); }
    inline void w_u16(uint16_t v) {
        this->d.push_back((uint8_t)(v & 0xff));
        this->d.push_back((uint8_t)((v >> 8) & 0xff));
    }
    inline void w_u32(uint32_t v) {
        for (int x = 0; x < 4; ++x) this->d.push_back((uint8_t)((v >> (x*8)) & 0xff));
    }
    inline void w_i32(int32_t v) { this->w_u32((uint32_t)v); }
    inline void w_u64(uint64_t v) {
        this->w_u32((uint32_t)(v & 0xffffffffull));
        this->w_u32((uint32_t)(v >> 32));
    }
    inline void w_f(float v) { uint32_t t; memcpy(&t, &v, 4); this->w_u32(t); }
    inline void w_str(const char *s) {
        uint32_t l = s ? (uint32_t)strlen(s) : 0;
        if (l > 4096) l = 4096;
        this->w_u32(l);
        for (uint32_t x = 0; x < l; ++x) this->d.push_back((uint8_t)s[x]);
    }
    inline void w_blob(const void *p, uint32_t len) {
        this->w_u32(len);
        if (len) {
            const uint8_t *b = (const uint8_t*)p;
            this->d.insert(this->d.end(), b, b + len);
        }
    }
    inline void w_buf(const buf &o) { this->w_blob(o.d.empty() ? 0 : &o.d[0], (uint32_t)o.d.size()); }

    inline uint8_t r_u8() {
        if (this->eof(1)) { this->err = true; return 0; }
        return this->d[this->rp++];
    }
    inline int8_t r_i8() { return (int8_t)this->r_u8(); }
    inline bool r_bool() { return this->r_u8() != 0; }
    inline uint16_t r_u16() {
        if (this->eof(2)) { this->err = true; return 0; }
        uint16_t v = (uint16_t)this->d[this->rp] | ((uint16_t)this->d[this->rp+1] << 8);
        this->rp += 2;
        return v;
    }
    inline uint32_t r_u32() {
        if (this->eof(4)) { this->err = true; return 0; }
        uint32_t v = 0;
        for (int x = 0; x < 4; ++x) v |= ((uint32_t)this->d[this->rp+x]) << (x*8);
        this->rp += 4;
        return v;
    }
    inline int32_t r_i32() { return (int32_t)this->r_u32(); }
    inline uint64_t r_u64() {
        uint64_t lo = this->r_u32();
        uint64_t hi = this->r_u32();
        return lo | (hi << 32);
    }
    inline float r_f() {
        uint32_t t = this->r_u32();
        float v;
        memcpy(&v, &t, 4);
        return v;
    }
    inline std::string r_str() {
        uint32_t l = this->r_u32();
        if (this->err || l > 4096 || this->eof(l)) { this->err = true; return std::string(); }
        std::string s(l ? (const char*)&this->d[this->rp] : "", l);
        this->rp += l;
        return s;
    }
    inline bool r_blob(std::vector<uint8_t> *out) {
        uint32_t l = this->r_u32();
        if (this->err || this->eof(l)) { this->err = true; return false; }
        out->assign(this->d.begin() + this->rp, this->d.begin() + this->rp + l);
        this->rp += l;
        return true;
    }
    inline bool r_buf(buf *out) {
        out->clear();
        if (!this->r_blob(&out->d)) return false;
        out->rp = 0;
        out->err = false;
        return true;
    }
};

bool net_init();
void net_quit();

/* Drain the ENet event queue. Safe (and cheap) to call several times per
 * frame; conn::pump() and listener::accept_one() call it themselves. */
void net_service();

/* Connect to a host. Returns an opaque peer handle or -1. Blocks up to
 * timeout_ms waiting for the ENet handshake. */
intptr_t net_connect(const char *host, int port, int timeout_ms);

const char *net_last_error();

/* close a raw peer handle (used when a connection is rejected) */
void net_close_raw(intptr_t handle);

/* round trip time of a peer handle, in milliseconds (ENet measures it) */
int net_rtt(intptr_t handle);

class conn {
  public:
    intptr_t fd;    /* ENetPeer*, kept as intptr_t for API compatibility */
    bool dead;

    conn() : fd(-1), dead(true), bytes_in(0), bytes_out(0) {}
    ~conn() { this->close(); }

    inline bool valid() const { return this->fd != -1 && !this->dead; }

    void attach(intptr_t handle);
    void close();

    /* Queue a message. reliable=false puts it on the unreliable channel,
     * where a late packet is dropped instead of delaying the stream. */
    void send(uint8_t type, const buf &b, bool reliable = true);
    void send_empty(uint8_t type, bool reliable = true);

    /* Service the transport. Returns false if the peer died. */
    bool pump();

    /* Pop the next complete message. */
    bool next(uint8_t *type, buf *payload);

    int rtt_ms() const { return net_rtt(this->fd); }

    inline size_t pending_out() const;
    inline uint64_t total_in() const { return this->bytes_in; }
    inline uint64_t total_out() const { return this->bytes_out; }

  private:
    uint64_t bytes_in;
    uint64_t bytes_out;
};

class listener {
  public:
    intptr_t fd;    /* 1 while the ENet server host is open */

    listener() : fd(-1) {}
    ~listener() { this->close(); }

    inline bool valid() const { return this->fd != -1; }

    bool open(int port);
    void close();

    /* Returns a new peer handle, or -1 if nobody is waiting */
    intptr_t accept_one(std::string *out_ip);
};

}
