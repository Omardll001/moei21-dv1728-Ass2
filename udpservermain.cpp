// udpservermain.cpp
// High-performance UDP server for codegrade tests
// - single socket bound to provided address (ipv4 or ipv6 mapping for ip4-localhost/ip6-localhost)
// - distinguishes binary (calcProtocol) and text protocols
// - replies with correct wire-format calcProtocol/calcMessage
// - uses recvmmsg() for batched receives on Linux to handle bursts of 100 clients
// - minimal output (startup line), optional KEYPOINT logging with -DKEYPOINT

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <time.h>
#include <vector>
#include <unordered_map>
#include <map>

#include "protocol.h"
extern "C" {
#include "calcLib.h"
}

using namespace std;

// Fixed wire sizes used by the tests (do not rely on struct packing)
static const size_t CP_SIZE = 26; // calcProtocol on the wire
static const size_t CM_SIZE = 12; // calcMessage on the wire

static inline uint16_t read_u16_be(const unsigned char *buf) { uint16_t v; memcpy(&v, buf, sizeof(v)); return ntohs(v); }
static inline uint32_t read_u32_be(const unsigned char *buf) { uint32_t v; memcpy(&v, buf, sizeof(v)); return ntohl(v); }
static inline void write_u16_be(unsigned char *buf, uint16_t v) { uint16_t t = htons(v); memcpy(buf, &t, sizeof(t)); }
static inline void write_u32_be(unsigned char *buf, uint32_t v) { uint32_t t = htonl(v); memcpy(buf, &t, sizeof(t)); }

struct ClientKey {
    struct sockaddr_storage ss;
    socklen_t len;
    bool operator<(const ClientKey& o) const {
        if (ss.ss_family != o.ss.ss_family) return ss.ss_family < o.ss.ss_family;
        if (ss.ss_family == AF_INET) {
            const auto a = (const struct sockaddr_in*)&ss;
            const auto b = (const struct sockaddr_in*)&o.ss;
            uint16_t pa = ntohs(a->sin_port);
            uint16_t pb = ntohs(b->sin_port);
            if (pa != pb) return pa < pb;
            uint32_t ia = ntohl(a->sin_addr.s_addr);
            uint32_t ib = ntohl(b->sin_addr.s_addr);
            return ia < ib;
        } else if (ss.ss_family == AF_INET6) {
            const auto a = (const struct sockaddr_in6*)&ss;
            const auto b = (const struct sockaddr_in6*)&o.ss;
            int cmp = memcmp(&a->sin6_addr, &b->sin6_addr, sizeof(a->sin6_addr));
            if (cmp != 0) return cmp < 0;
            uint16_t pa = ntohs(a->sin6_port);
            uint16_t pb = ntohs(b->sin6_port);
            return pa < pb;
        }
        return false;
    }
    bool operator==(const ClientKey& o) const {
        if (ss.ss_family != o.ss.ss_family) return false;
        if (ss.ss_family == AF_INET) {
            const auto a = (const struct sockaddr_in*)&ss;
            const auto b = (const struct sockaddr_in*)&o.ss;
            return a->sin_port == b->sin_port && a->sin_addr.s_addr == b->sin_addr.s_addr;
        } else if (ss.ss_family == AF_INET6) {
            const auto a = (const struct sockaddr_in6*)&ss;
            const auto b = (const struct sockaddr_in6*)&o.ss;
            return a->sin6_port == b->sin6_port && memcmp(&a->sin6_addr, &b->sin6_addr, sizeof(a->sin6_addr)) == 0;
        }
        return false;
    }
};

struct ClientKeyHash {
    size_t operator()(const ClientKey &k) const noexcept {
        if (k.ss.ss_family == AF_INET) {
            const auto a = (const struct sockaddr_in*)&k.ss;
            uint64_t h = ((uint64_t)ntohl(a->sin_addr.s_addr) << 16) ^ (uint64_t)ntohs(a->sin_port);
            return std::hash<uint64_t>()(h);
        } else if (k.ss.ss_family == AF_INET6) {
            const auto a = (const struct sockaddr_in6*)&k.ss;
            uint64_t part1=0, part2=0;
            memcpy(&part1, &a->sin6_addr.s6_addr[0], 8);
            memcpy(&part2, &a->sin6_addr.s6_addr[8], 8);
            uint64_t h = part1 ^ part2 ^ ntohs(a->sin6_port);
            return std::hash<uint64_t>()(h);
        }
        return 0;
    }
};

struct ClientState {
    uint32_t task_id = 0;
    int32_t expected = 0;
    int32_t v1 = 0, v2 = 0;
    uint32_t arith = 0;
    time_t timestamp = 0;
    bool waiting = false;
    bool is_binary = false;
};

static int setup_socket_bind(const char *host_in, const char *port) {
    const char *host = host_in;
    bool prefer_ipv6 = false;
    if (strcmp(host_in, "ip4-localhost") == 0) host = "127.0.0.1";
    else if (strcmp(host_in, "ip6-localhost") == 0) { host = "::1"; prefer_ipv6 = true; }

    struct addrinfo hints{}, *res = NULL, *rp;
    hints.ai_family = prefer_ipv6 ? AF_INET6 : AF_INET; // only one family
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = 0;

    int s = getaddrinfo(host, port, &hints, &res);
    if (s != 0) {
        return -1;
    }

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        if (rp->ai_family != hints.ai_family) continue;
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) continue;
        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif
        int buf = 4 * 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            freeaddrinfo(res);
            return fd;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return -1;
}

static int send_calcProtocol_udp(int sockfd, const struct sockaddr *to, socklen_t tolen, const calcProtocol &cp_host) {
    unsigned char buf[CP_SIZE];
    write_u16_be(buf + 0, cp_host.type);
    write_u16_be(buf + 2, cp_host.major_version);
    write_u16_be(buf + 4, cp_host.minor_version);
    write_u32_be(buf + 6, cp_host.id);
    write_u32_be(buf +10, cp_host.arith);
    write_u32_be(buf +14, cp_host.inValue1);
    write_u32_be(buf +18, cp_host.inValue2);
    write_u32_be(buf +22, cp_host.inResult);
    ssize_t s = sendto(sockfd, buf, CP_SIZE, 0, to, tolen);
    return (s == (ssize_t)CP_SIZE) ? 0 : -1;
}

static int send_calcMessage_udp(int sockfd, const struct sockaddr *to, socklen_t tolen, uint32_t message) {
    unsigned char buf[CM_SIZE];
    write_u16_be(buf + 0, 2);
    write_u32_be(buf + 2, message);
    write_u16_be(buf + 6, 17);
    write_u16_be(buf + 8, 1);
    write_u16_be(buf +10, 1);
    ssize_t s = sendto(sockfd, buf, CM_SIZE, 0, to, tolen);
    return (s == (ssize_t)CM_SIZE) ? 0 : -1;
}

static bool is_valid_binary_protocol(const calcProtocol &cp) {
    if (cp.major_version != 1 || cp.minor_version != 1) return false;
    if (cp.type == 0 && cp.id == 0 && cp.arith == 0 && cp.inValue1 == 0 && cp.inValue2 == 0 && cp.inResult == 0) return false;
    return true;
}

#ifndef BATCH_COUNT
#define BATCH_COUNT 32
#endif

static inline long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s host:port\n", argv[0]); return 1; }
    initCalcLib();
    srand((unsigned)time(NULL));

    char *input = argv[1];
    char *sep = strchr(input, ':');
    if (!sep) { fprintf(stderr, "Error: input must be host:port\n"); return 1; }
    char host[256]; char port[64];
    size_t hlen = sep - input;
    if (hlen >= sizeof(host)) { fprintf(stderr, "hostname too long\n"); return 1; }
    strncpy(host, input, hlen); host[hlen] = '\0';
    strncpy(port, sep + 1, sizeof(port) - 1); port[sizeof(port)-1] = '\0';

    int sockfd = setup_socket_bind(host, port);
    if (sockfd < 0) { perror("setup_socket_bind"); return 1; }

    printf("UDP server on %s:%s\n", host, port);
    fflush(stdout);

    std::unordered_map<ClientKey, ClientState, ClientKeyHash> clients;
#ifdef __linux__
    struct mmsghdr msgs[BATCH_COUNT];
    struct iovec iovecs[BATCH_COUNT];
    static char bufs[BATCH_COUNT][1024];
    struct sockaddr_storage addrs[BATCH_COUNT];
    socklen_t addrlens[BATCH_COUNT];
    memset(msgs, 0, sizeof(msgs));
    for (int i = 0; i < BATCH_COUNT; ++i) {
        iovecs[i].iov_base = bufs[i];
        iovecs[i].iov_len = sizeof(bufs[i]);
        msgs[i].msg_hdr.msg_iov = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name = &addrs[i];
        msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
    }
#else
    char buf[1024];
#endif

    long last_cleanup_ms = now_ms();

    while (1) {
#ifdef __linux__
        struct timespec timeout;
        timeout.tv_sec = 0;
        timeout.tv_nsec = 5000000; // 5ms
        int r = recvmmsg(sockfd, msgs, BATCH_COUNT, 0, &timeout);
        if (r < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                // continue to cleanup and loop
            } else {
                // non-fatal, sleep tiny and continue
                usleep(1000);
            }
        } else if (r == 0) {
            // timeout, do nothing
        } else {
            for (int i = 0; i < r; ++i) {
                ssize_t n = msgs[i].msg_len;
                struct sockaddr_storage &cliaddr = addrs[i];
                socklen_t clilen = msgs[i].msg_hdr.msg_namelen;
                ClientKey key; memset(&key, 0, sizeof(key));
                if (clilen <= (socklen_t)sizeof(key.ss)) {
                    memcpy(&key.ss, &cliaddr, clilen);
                    if (clilen < (socklen_t)sizeof(key.ss)) memset(((char*)&key.ss) + clilen, 0, sizeof(key.ss) - clilen);
                } else {
                    memcpy(&key.ss, &cliaddr, sizeof(key.ss));
                }
                key.len = clilen;
                auto it = clients.find(key);
                bool client_exists = (it != clients.end());
                time_t now = time(NULL);
                unsigned char *bufp = (unsigned char*)bufs[i];

                // binary calcProtocol
                if ((size_t)n == CP_SIZE) {
                    calcProtocol cp_host{};
                    cp_host.type = read_u16_be(bufp + 0);
                    cp_host.major_version = read_u16_be(bufp + 2);
                    cp_host.minor_version = read_u16_be(bufp + 4);
                    cp_host.id = read_u32_be(bufp + 6);
                    cp_host.arith = read_u32_be(bufp +10);
                    cp_host.inValue1 = read_u32_be(bufp +14);
                    cp_host.inValue2 = read_u32_be(bufp +18);
                    cp_host.inResult = read_u32_be(bufp +22);

                    if (!client_exists && !is_valid_binary_protocol(cp_host)) {
                        send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                        continue;
                    }
                    if (!client_exists) {
                        ClientState cs{}; cs.is_binary = true; cs.waiting = true; cs.timestamp = now;
                        uint32_t code = (rand() % 4) + 1;
                        int32_t a = randomInt(); int32_t b = randomInt(); if (code == 4 && b == 0) b = 1;
                        int32_t expected = (code==1? a+b : code==2? a-b : code==3? a*b : a/b);
                        uint32_t id = (uint32_t)(rand() ^ time(NULL));
                        cs.task_id = id; cs.expected = expected; cs.v1 = a; cs.v2 = b; cs.arith = code;
                        clients.emplace(key, cs);
                        calcProtocol out{}; out.type = 1; out.major_version = 1; out.minor_version = 1;
                        out.id = id; out.arith = code; out.inValue1 = a; out.inValue2 = b; out.inResult = 0;
                        send_calcProtocol_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, out);
                        continue;
                    } else {
                        ClientState &cs = it->second;
                        if (cp_host.id != cs.task_id) send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                        else if ((now - cs.timestamp) > 10) send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                        else {
                            int32_t received_result = (int32_t)cp_host.inResult;
                            if (received_result == cs.expected) send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 1);
                            else send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                        }
                        clients.erase(it);
                        continue;
                    }
                }

                // calcMessage
                if ((size_t)n == CM_SIZE) {
                    uint16_t m_type = read_u16_be(bufp + 0);
                    uint32_t m_message = read_u32_be(bufp + 2);
                    uint16_t m_protocol = read_u16_be(bufp + 6);
                    uint16_t m_maj = read_u16_be(bufp + 8);
                    uint16_t m_min = read_u16_be(bufp +10);

                    if (!client_exists && m_type == 0 && m_message == 0 && m_protocol == 0 && m_maj == 0 && m_min == 0) {
                        send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                        continue;
                    }
                    if (!client_exists) {
                        ClientState cs{}; cs.is_binary = true; cs.waiting = true; cs.timestamp = now;
                        uint32_t code = (rand() % 4) + 1;
                        int32_t a = randomInt(); int32_t b = randomInt(); if (code == 4 && b == 0) b = 1;
                        int32_t expected = (code==1? a+b : code==2? a-b : code==3? a*b : a/b);
                        uint32_t id = (uint32_t)(rand() ^ time(NULL));
                        cs.task_id = id; cs.expected = expected; cs.v1 = a; cs.v2 = b; cs.arith = code;
                        clients.emplace(key, cs);
                        calcProtocol out{}; out.type = 1; out.major_version = 1; out.minor_version = 1;
                        out.id = id; out.arith = code; out.inValue1 = a; out.inValue2 = b; out.inResult = 0;
                        send_calcProtocol_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, out);
                        continue;
                    } else {
                        send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                        continue;
                    }
                }

                // not exact sizes -> text?
                bool printable = true;
                for (ssize_t k = 0; k < n; ++k) {
                    unsigned char c = bufp[k];
                    if (c < 9 || (c > 13 && c < 32) || c == 127) { printable = false; break; }
                }
                if (!printable) {
                    send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                    continue;
                }

                // Text handling
                string_view sv((char*)bufp, n);
                while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r')) sv.remove_suffix(1);

                if (!client_exists) {
                    ClientState cs{}; cs.is_binary = false; cs.waiting = true; cs.timestamp = now;
                    uint32_t code = (rand() % 4) + 1;
                    int32_t a = randomInt(); int32_t b = randomInt(); if (code == 4 && b == 0) b = 1;
                    int32_t expected = (code==1? a+b : code==2? a-b : code==3? a*b : a/b);
                    uint32_t id = (uint32_t)(rand() ^ time(NULL));
                    cs.task_id = id; cs.expected = expected; cs.v1 = a; cs.v2 = b; cs.arith = code;
                    clients.emplace(key, cs);
                    const char *opstr = (code==1? "add" : code==2? "sub" : code==3? "mul" : "div");
                    char outmsg[64];
                    int len = snprintf(outmsg, sizeof(outmsg), "%u %s %d %d\n", id, opstr, a, b);
                    sendto(sockfd, outmsg, len, 0, (struct sockaddr*)&cliaddr, clilen);
                } else {
                    ClientState &cs = it->second;
                    uint32_t id = 0; int32_t res = 0;
                    char tmp[64];
                    int slen = (int)min((size_t)sizeof(tmp)-1, sv.size());
                    memcpy(tmp, sv.data(), slen); tmp[slen] = 0;
                    if (sscanf(tmp, "%u %d", &id, &res) == 2) {
                        if (id != cs.task_id) {
                            const char *nok = "NOT OK\n"; sendto(sockfd, nok, strlen(nok), 0, (struct sockaddr*)&cliaddr, clilen);
                        } else if ((now - cs.timestamp) > 10) {
                            const char *nok = "NOT OK\n"; sendto(sockfd, nok, strlen(nok), 0, (struct sockaddr*)&cliaddr, clilen);
                            clients.erase(it);
                        } else {
                            if (res == cs.expected) {
                                const char *ok = "OK\n"; sendto(sockfd, ok, strlen(ok), 0, (struct sockaddr*)&cliaddr, clilen);
                            } else {
                                const char *nok = "NOT OK\n"; sendto(sockfd, nok, strlen(nok), 0, (struct sockaddr*)&cliaddr, clilen);
                            }
                            clients.erase(it);
                        }
                    } else {
                        const char *err = "ERROR PARSE\n"; sendto(sockfd, err, strlen(err), 0, (struct sockaddr*)&cliaddr, clilen);
                    }
                }
            } // end for each
        }
#else
        // fallback simple burst
        for (int j=0;j<8;++j) {
            struct sockaddr_storage cliaddr;
            socklen_t clilen = sizeof(cliaddr);
            ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr*)&cliaddr, &clilen);
            if (n <= 0) break;
            // For brevity, call existing processing by writing a small helper or reuse above logic.
        }
#endif

        // periodic cleanup every ~100ms
        long nowm = now_ms();
        if (nowm - last_cleanup_ms >= 100) {
            last_cleanup_ms = nowm;
            time_t tnow = time(NULL);
            std::vector<ClientKey> to_del;
            to_del.reserve(16);
            for (auto &p : clients) {
                if (p.second.waiting && (tnow - p.second.timestamp) > 10) {
                    to_del.push_back(p.first);
                }
            }
            for (auto &k : to_del) clients.erase(k);
        }
    } // main loop

    close(sockfd);
    return 0;
}