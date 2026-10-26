#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include "output.h"
#include "common.h"
#include "tsmux.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

using Chunk = std::shared_ptr<const std::vector<uint8_t>>;

static const size_t kMaxQueuedItems  = 600;              // ~5 s of 60 fps video + audio
static const size_t kMaxClientBytes  = 64u << 20;        // drop a viewer that falls this far behind

// ---- queue from the game's threads to the worker -------------------------------------------

static std::mutex              g_qMtx;
static std::condition_variable g_qCv;
static std::deque<Item>        g_queue;
static std::atomic<bool>       g_resync{ false };
static std::atomic<bool>       g_idrRequest{ false };
static std::atomic<int>        g_clientCount{ 0 };
static bool                    g_staticSinks = false;    // file or UDP configured

bool output_active() { return g_staticSinks || g_clientCount.load() > 0; }

bool output_take_idr_request() { return g_idrRequest.exchange(false); }

void output_push(Item&& it) {
    std::lock_guard<std::mutex> lk(g_qMtx);
    if (g_queue.size() >= kMaxQueuedItems) {
        // Worker fell behind: drop media but keep session descriptions, restart viewers on a keyframe.
        g_queue.erase(std::remove_if(g_queue.begin(), g_queue.end(),
                      [](const Item& q) { return q.kind != ItemKind::Session; }), g_queue.end());
        g_resync = true;
        g_idrRequest = true;
        logf("output queue overflow, dropped pending samples");
    }
    g_queue.push_back(std::move(it));
    g_qCv.notify_one();
}

// ---- live session -----------------------------------------------------------------------------

static std::mutex            g_liveMtx;
static SessionInfo           g_liveInfo;
static std::atomic<uint32_t> g_liveSession{ 0 };

static void live_push_locked() {
    Item it;
    it.kind = ItemKind::Session;
    it.session = ++g_liveSession;
    it.info = g_liveInfo;
    output_push(std::move(it));
}

void live_update_video(const std::vector<uint8_t>& seqHeader) {
    std::lock_guard<std::mutex> lk(g_liveMtx);
    g_liveInfo.seqHeader = seqHeader;
    live_push_locked();
}

void live_update_audio(const AudioInfo& audio) {
    std::lock_guard<std::mutex> lk(g_liveMtx);
    g_liveInfo.audio = audio;
    live_push_locked();
}

uint32_t live_session() { return g_liveSession.load(); }

// ---- sinks -----------------------------------------------------------------------------------

struct Client {
    SOCKET                  sock;
    std::mutex              mtx;
    std::condition_variable cv;
    std::deque<Chunk>       pending;
    size_t                  bytes = 0;
    bool                    waitingKey = true;
    std::atomic<bool>       dead{ false };
    std::string             name;
};

static std::mutex                           g_clientsMtx;
static std::vector<std::shared_ptr<Client>> g_clients;

static HANDLE   g_file = INVALID_HANDLE_VALUE;
static bool     g_fileWaitingKey = true;
static SOCKET   g_udp = INVALID_SOCKET;
static sockaddr_storage g_udpAddr;
static int      g_udpAddrLen = 0;
static bool     g_udpWaitingKey = true;

static bool split_host_port(const std::string& s, std::string& host, std::string& port) {
    size_t k = s.rfind(':');
    if (k == std::string::npos) return false;
    host = s.substr(0, k);
    port = s.substr(k + 1);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    if (host.empty()) host = "0.0.0.0";
    return !port.empty();
}

static void client_loop(std::shared_ptr<Client> c) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        Chunk ch;
        {
            std::unique_lock<std::mutex> lk(c->mtx);
            c->cv.wait(lk, [&] { return c->dead || !c->pending.empty(); });
            if (c->dead) break;
            ch = c->pending.front();
            c->pending.pop_front();
            c->bytes -= ch->size();
        }
        const char* p = (const char*)ch->data();
        size_t left = ch->size();
        while (left) {
            int n = send(c->sock, p, (int)std::min<size_t>(left, 1 << 20), 0);
            if (n <= 0) { c->dead = true; break; }
            p += n; left -= n;
        }
        if (c->dead) break;
    }
    closesocket(c->sock);
    logf("viewer %s disconnected", c->name.c_str());
}

static void tcp_server(std::string addr) {
    std::string host, port;
    if (!split_host_port(addr, host, port)) { logf("bad tcp_listen '%s'", addr.c_str()); return; }
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) || !res) {
        logf("tcp_listen: cannot resolve '%s'", addr.c_str()); return;
    }
    SOCKET ls = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
    BOOL excl = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof excl);
    if (ls == INVALID_SOCKET || bind(ls, res->ai_addr, (int)res->ai_addrlen) || listen(ls, 4)) {
        logf("tcp_listen: cannot listen on %s (wsa %d)", addr.c_str(), WSAGetLastError());
        freeaddrinfo(res); return;
    }
    freeaddrinfo(res);
    logf("listening for viewers on tcp://%s", addr.c_str());

    for (;;) {
        sockaddr_storage peer; int plen = sizeof peer;
        SOCKET s = accept(ls, (sockaddr*)&peer, &plen);
        if (s == INVALID_SOCKET) { Sleep(100); continue; }
        BOOL nd = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof nd);
        auto c = std::make_shared<Client>();
        c->sock = s;
        char hostbuf[NI_MAXHOST] = "?", portbuf[NI_MAXSERV] = "?";
        getnameinfo((sockaddr*)&peer, plen, hostbuf, sizeof hostbuf, portbuf, sizeof portbuf,
                    NI_NUMERICHOST | NI_NUMERICSERV);
        c->name = std::string(hostbuf) + ":" + portbuf;
        {
            std::lock_guard<std::mutex> lk(g_clientsMtx);
            g_clients.push_back(c);
            g_clientCount = (int)g_clients.size();
        }
        g_idrRequest = true;   // start the new viewer on a fresh keyframe
        logf("viewer %s connected", c->name.c_str());
        std::thread(client_loop, c).detach();
    }
}

static void open_udp(const std::string& addr) {
    std::string host, port;
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_DGRAM;
    if (!split_host_port(addr, host, port) || getaddrinfo(host.c_str(), port.c_str(), &hints, &res) || !res) {
        logf("udp_target: cannot resolve '%s'", addr.c_str()); return;
    }
    g_udp = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    memcpy(&g_udpAddr, res->ai_addr, res->ai_addrlen);
    g_udpAddrLen = (int)res->ai_addrlen;
    freeaddrinfo(res);
    int buf = 4 << 20;
    setsockopt(g_udp, SOL_SOCKET, SO_SNDBUF, (const char*)&buf, sizeof buf);
    logf("pushing MPEG-TS to udp://%s", addr.c_str());
}

static void open_file(std::wstring path) {
    size_t k = path.find(L"{time}");
    if (k != std::wstring::npos) {
        SYSTEMTIME t; GetLocalTime(&t);
        wchar_t ts[32];
        swprintf_s(ts, L"%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        path.replace(k, 6, ts);
    }
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) logf("cannot open output file (err %lu)", GetLastError());
    else logf("writing MPEG-TS to %ls", path.c_str());
}

static void distribute(std::vector<uint8_t>&& buf, bool key) {
    if (buf.empty()) return;
    if (g_resync.exchange(false)) {
        g_fileWaitingKey = g_udpWaitingKey = true;
        std::lock_guard<std::mutex> lk(g_clientsMtx);
        for (auto& c : g_clients) c->waitingKey = true;
    }

    if (g_file != INVALID_HANDLE_VALUE && !(g_fileWaitingKey && !key)) {
        g_fileWaitingKey = false;
        DWORD w;
        if (!WriteFile(g_file, buf.data(), (DWORD)buf.size(), &w, nullptr)) {
            logf("file write failed (err %lu), closing", GetLastError());
            CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE;
        }
    }

    if (g_udp != INVALID_SOCKET && !(g_udpWaitingKey && !key)) {
        g_udpWaitingKey = false;
        for (size_t off = 0; off < buf.size(); off += 7 * 188) {
            int n = (int)std::min<size_t>(7 * 188, buf.size() - off);
            sendto(g_udp, (const char*)buf.data() + off, n, 0, (sockaddr*)&g_udpAddr, g_udpAddrLen);
        }
    }

    std::lock_guard<std::mutex> lk(g_clientsMtx);
    if (g_clients.empty()) return;
    Chunk ch = std::make_shared<const std::vector<uint8_t>>(std::move(buf));
    for (auto it = g_clients.begin(); it != g_clients.end();) {
        auto& c = *it;
        if (c->dead) { it = g_clients.erase(it); continue; }
        if (c->waitingKey && !key) { ++it; continue; }
        {
            std::lock_guard<std::mutex> clk(c->mtx);
            c->waitingKey = false;
            c->pending.push_back(ch);
            c->bytes += ch->size();
            if (c->bytes > kMaxClientBytes) {
                logf("viewer %s too slow, dropping", c->name.c_str());
                c->dead = true;
                shutdown(c->sock, SD_BOTH);
            }
        }
        c->cv.notify_one();
        ++it;
    }
    g_clientCount = (int)g_clients.size();
}

// ---- H.264 / AAC framing ----------------------------------------------------------------------

struct Nal { size_t off, len; uint8_t type; };

static void split_annexb(const uint8_t* p, size_t n, std::vector<Nal>& out) {
    out.clear();
    size_t i = 0, start = SIZE_MAX;
    while (i + 3 <= n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t end = i;
                while (end > start && p[end - 1] == 0) --end;
                out.push_back({ start, end - start, (uint8_t)(p[start] & 0x1F) });
            }
            start = i + 3;
            i += 3;
        } else {
            ++i;
        }
    }
    if (start != SIZE_MAX && start < n) out.push_back({ start, n - start, (uint8_t)(p[start] & 0x1F) });
}

static bool is_annexb(const std::vector<uint8_t>& d) {
    return d.size() >= 4 && d[0] == 0 && d[1] == 0 && (d[2] == 1 || (d[2] == 0 && d[3] == 1));
}

// MP4-style 4-byte length prefixes -> start codes. Returns false if the lengths don't add up.
static bool avcc_to_annexb(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    out.clear();
    size_t i = 0;
    while (i + 4 <= in.size()) {
        size_t len = ((size_t)in[i] << 24) | ((size_t)in[i + 1] << 16) | ((size_t)in[i + 2] << 8) | in[i + 3];
        i += 4;
        if (len == 0 || len > in.size() - i) return false;
        out.insert(out.end(), { 0, 0, 0, 1 });
        out.insert(out.end(), in.begin() + i, in.begin() + i + len);
        i += len;
    }
    return i == in.size();
}

struct ParamSets { std::vector<uint8_t> sps, pps; };

static void put_nal(std::vector<uint8_t>& out, const uint8_t* p, size_t n) {
    out.insert(out.end(), { 0, 0, 0, 1 });
    out.insert(out.end(), p, p + n);
}

// Rebuilds one access unit: leading AUD, SPS/PPS before every IDR, 4-byte start codes.
static std::vector<uint8_t> build_au(const std::vector<uint8_t>& raw, ParamSets& ps, bool& key) {
    std::vector<uint8_t> conv;
    const std::vector<uint8_t>* src = &raw;
    if (!is_annexb(raw) && avcc_to_annexb(raw, conv)) src = &conv;
    const uint8_t* p = src->data();

    std::vector<Nal> nals;
    split_annexb(p, src->size(), nals);
    bool hasSps = false;
    for (auto& n : nals) {
        if (n.type == 5) key = true;
        if (n.type == 7) { hasSps = true; ps.sps.assign(p + n.off, p + n.off + n.len); }
        if (n.type == 8) ps.pps.assign(p + n.off, p + n.off + n.len);
    }

    std::vector<uint8_t> out;
    out.reserve(src->size() + 64);
    size_t i = 0;
    if (!nals.empty() && nals[0].type == 9) { put_nal(out, p + nals[0].off, nals[0].len); i = 1; }
    else { static const uint8_t aud[] = { 0x09, 0xF0 }; put_nal(out, aud, 2); }
    if (key && !hasSps && !ps.sps.empty() && !ps.pps.empty()) {
        put_nal(out, ps.sps.data(), ps.sps.size());
        put_nal(out, ps.pps.data(), ps.pps.size());
    }
    for (; i < nals.size(); ++i) put_nal(out, p + nals[i].off, nals[i].len);
    return out;
}

static std::vector<uint8_t> adts_frame(const AudioInfo& a, const std::vector<uint8_t>& raw) {
    if (a.adts) return raw;
    size_t len = raw.size() + 7;
    int profile = (a.objectType >= 1 && a.objectType <= 4 ? a.objectType : 2) - 1;
    std::vector<uint8_t> f(7);
    f[0] = 0xFF;
    f[1] = 0xF1;
    f[2] = (uint8_t)((profile << 6) | ((a.freqIndex & 0xF) << 2) | ((a.channels >> 2) & 1));
    f[3] = (uint8_t)(((a.channels & 3) << 6) | ((len >> 11) & 3));
    f[4] = (uint8_t)(len >> 3);
    f[5] = (uint8_t)(((len & 7) << 5) | 0x1F);
    f[6] = 0xFC;
    f.insert(f.end(), raw.begin(), raw.end());
    return f;
}

// ---- worker -----------------------------------------------------------------------------------

static void worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    TsMuxer   mux;
    ParamSets ps;
    AudioInfo audio;
    uint32_t  session = 0;
    bool      haveOffset = false, haveLast = false;
    int64_t   offset = 0, lastOut = 0, lastWall = 0;

    for (;;) {
        Item it;
        {
            std::unique_lock<std::mutex> lk(g_qMtx);
            g_qCv.wait(lk, [] { return !g_queue.empty(); });
            it = std::move(g_queue.front());
            g_queue.pop_front();
        }

        if (it.kind == ItemKind::Session) {
            session = it.session;
            audio = it.info.audio;
            mux.set_audio(audio.present);
            ParamSets fresh;
            bool k = false;
            if (is_annexb(it.info.seqHeader)) build_au(it.info.seqHeader, fresh, k);
            ps = fresh;
            haveOffset = false;
            continue;
        }
        if (it.session != session) continue;
        if (it.kind == ItemKind::Audio && !audio.present) continue;

        // Each game recording restarts at 0; keep the outgoing timeline monotonic and in step
        // with wall-clock time so viewers see one continuous live stream.
        if (!haveOffset) {
            int64_t target = haveLast ? lastOut + std::max<int64_t>(it.wall - lastWall, 0) + 166667
                                      : 10000000;
            offset = target - it.pts;
            haveOffset = true;
        }
        int64_t pts = it.pts + offset, dts = it.dts + offset;
        if (pts < 0 || dts < 0) continue;
        lastOut = std::max(lastOut, pts);
        lastWall = it.wall;
        haveLast = true;

        std::vector<uint8_t> buf;
        bool key = false;
        if (it.kind == ItemKind::Video) {
            key = it.key;
            std::vector<uint8_t> au = build_au(it.data, ps, key);
            if (key) mux.psi(buf);
            mux.video(buf, au, pts * 9 / 1000, dts * 9 / 1000, key);
        } else {
            mux.audio(buf, adts_frame(audio, it.data), pts * 9 / 1000);
        }
        distribute(std::move(buf), key);
    }
}

void output_start() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    if (!g_cfg.file.empty()) open_file(g_cfg.file);
    if (!g_cfg.udpTarget.empty()) open_udp(g_cfg.udpTarget);
    g_staticSinks = g_file != INVALID_HANDLE_VALUE || g_udp != INVALID_SOCKET;
    if (!g_cfg.tcpListen.empty()) std::thread(tcp_server, g_cfg.tcpListen).detach();
    std::thread(worker).detach();
}
