#include "tsmux.h"
#include <cstring>

static const uint16_t kPmtPid = 0x1000, kVideoPid = 0x100, kAudioPid = 0x101;
static const int64_t  kMask33 = (1LL << 33) - 1;
static const int64_t  kPcrLead = 45000;   // PCR runs 0.5 s behind video DTS

static uint32_t crc32_mpeg(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFF;
    while (n--) {
        c ^= (uint32_t)*p++ << 24;
        for (int i = 0; i < 8; ++i) c = (c & 0x80000000) ? (c << 1) ^ 0x04C11DB7 : c << 1;
    }
    return c;
}

static void put_ts(uint8_t* p, uint8_t prefix, int64_t t) {
    t &= kMask33;
    p[0] = (uint8_t)((prefix << 4) | (((t >> 30) & 7) << 1) | 1);
    p[1] = (uint8_t)(t >> 22);
    p[2] = (uint8_t)((((t >> 15) & 0x7F) << 1) | 1);
    p[3] = (uint8_t)(t >> 7);
    p[4] = (uint8_t)(((t & 0x7F) << 1) | 1);
}

static std::vector<uint8_t> make_pes(uint8_t streamId, const std::vector<uint8_t>& data,
                                     int64_t pts, int64_t dts, bool bounded) {
    bool both = dts != pts;
    uint8_t hlen = both ? 10 : 5;
    std::vector<uint8_t> v(9 + hlen);
    v[0] = 0; v[1] = 0; v[2] = 1; v[3] = streamId;
    size_t plen = 3 + hlen + data.size();
    if (bounded && plen <= 0xFFFF) { v[4] = (uint8_t)(plen >> 8); v[5] = (uint8_t)plen; }
    v[6] = 0x80;
    v[7] = both ? 0xC0 : 0x80;
    v[8] = hlen;
    put_ts(&v[9], both ? 3 : 2, pts);
    if (both) put_ts(&v[14], 1, dts);
    v.insert(v.end(), data.begin(), data.end());
    return v;
}

void TsMuxer::set_audio(bool present) {
    if (present == audio_) return;
    audio_ = present;
    version_ = (version_ + 1) & 0x1F;
}

void TsMuxer::section(std::vector<uint8_t>& out, uint16_t pid, std::vector<uint8_t> sec) {
    size_t len = sec.size() - 3 + 4;
    sec[1] = (uint8_t)(0xB0 | ((len >> 8) & 0x0F));
    sec[2] = (uint8_t)len;
    uint32_t crc = crc32_mpeg(sec.data(), sec.size());
    for (int s = 24; s >= 0; s -= 8) sec.push_back((uint8_t)(crc >> s));

    uint8_t pkt[188];
    memset(pkt, 0xFF, sizeof pkt);
    pkt[0] = 0x47;
    pkt[1] = (uint8_t)(0x40 | ((pid >> 8) & 0x1F));
    pkt[2] = (uint8_t)pid;
    pkt[3] = (uint8_t)(0x10 | (cc_[pid]++ & 0x0F));
    pkt[4] = 0;  // pointer_field
    memcpy(pkt + 5, sec.data(), sec.size());
    out.insert(out.end(), pkt, pkt + 188);
}

void TsMuxer::psi(std::vector<uint8_t>& out) {
    section(out, 0, { 0x00, 0, 0, 0x00, 0x01, 0xC1, 0, 0,
                      0x00, 0x01, (uint8_t)(0xE0 | (kPmtPid >> 8)), (uint8_t)kPmtPid });
    std::vector<uint8_t> pmt = { 0x02, 0, 0, 0x00, 0x01, (uint8_t)(0xC1 | (version_ << 1)), 0, 0,
                                 (uint8_t)(0xE0 | (kVideoPid >> 8)), (uint8_t)kVideoPid, 0xF0, 0x00,
                                 0x1B, (uint8_t)(0xE0 | (kVideoPid >> 8)), (uint8_t)kVideoPid, 0xF0, 0x00 };
    if (audio_) pmt.insert(pmt.end(), { 0x0F, (uint8_t)(0xE0 | (kAudioPid >> 8)), (uint8_t)kAudioPid, 0xF0, 0x00 });
    section(out, kPmtPid, pmt);
}

void TsMuxer::packetize(std::vector<uint8_t>& out, uint16_t pid, const std::vector<uint8_t>& pes,
                        int64_t pcr, bool randomAccess) {
    size_t pos = 0;
    bool first = true;
    while (pos < pes.size()) {
        std::vector<uint8_t> af;  // adaptation field bytes after the length byte
        if (first && (randomAccess || pcr >= 0)) {
            af.push_back((uint8_t)((randomAccess ? 0x40 : 0) | (pcr >= 0 ? 0x10 : 0)));
            if (pcr >= 0) {
                int64_t b = pcr & kMask33;
                af.insert(af.end(), { (uint8_t)(b >> 25), (uint8_t)(b >> 17), (uint8_t)(b >> 9),
                                      (uint8_t)(b >> 1), (uint8_t)(((b & 1) << 7) | 0x7E), 0x00 });
            }
        }
        size_t afTotal = af.empty() ? 0 : 1 + af.size();
        size_t payload = pes.size() - pos;
        if (payload > 184 - afTotal) payload = 184 - afTotal;
        if (afTotal + payload < 184) {
            afTotal = 184 - payload;
            if (af.empty() && afTotal >= 2) af.push_back(0x00);
            while (1 + af.size() < afTotal) af.push_back(0xFF);
        }

        uint8_t pkt[188];
        pkt[0] = 0x47;
        pkt[1] = (uint8_t)((first ? 0x40 : 0) | ((pid >> 8) & 0x1F));
        pkt[2] = (uint8_t)pid;
        pkt[3] = (uint8_t)(((afTotal ? 3 : 1) << 4) | (cc_[pid]++ & 0x0F));
        size_t i = 4;
        if (afTotal) {
            pkt[i++] = (uint8_t)(afTotal - 1);
            if (!af.empty()) memcpy(pkt + i, af.data(), af.size());
            i += af.size();
        }
        memcpy(pkt + i, pes.data() + pos, payload);
        out.insert(out.end(), pkt, pkt + 188);
        pos += payload;
        first = false;
    }
}

void TsMuxer::video(std::vector<uint8_t>& out, const std::vector<uint8_t>& au, int64_t pts, int64_t dts, bool key) {
    int64_t pcr = dts - kPcrLead;
    if (pcr < 0) pcr = 0;
    packetize(out, kVideoPid, make_pes(0xE0, au, pts, dts, false), pcr, key);
}

void TsMuxer::audio(std::vector<uint8_t>& out, const std::vector<uint8_t>& adtsFrame, int64_t pts) {
    packetize(out, kAudioPid, make_pes(0xC0, adtsFrame, pts, pts, true), -1, false);
}
