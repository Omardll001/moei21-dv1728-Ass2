// udpservermain.cpp
// Minimal UDP server for codegrade tests - updated to reply binary error messages for malformed binary input

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
        if (rp->ai_family != hints.ai_family) continue; // only bind to requested family
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) continue;
        int reuse = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
            perror("setsockopt(SO_REUSEADDR) failed");
        }
        int buf = 4 * 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            freeaddrinfo(res);
            return fd; // success
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return -1;
}

static int send_calcProtocol_udp(int sockfd, const struct sockaddr *to, socklen_t tolen, const calcProtocol &cp_host) {
    unsigned char buf[CP_SIZE];
    // layout: type(2), major(2), minor(2), id(4), arith(4), inValue1(4), inValue2(4), inResult(4)
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
    // layout: type(2), message(4), protocol(2), major(2), minor(2)
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

    // Minimal startup print (required by tester)
    printf("UDP server on %s:%s\n", host, port);
    fflush(stdout);

    std::map<ClientKey, ClientState> clients;

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 5000; // 5ms
        int rv = select(sockfd + 1, &rfds, NULL, NULL, &tv);
        time_t now = time(NULL);

        // cleanup stale clients periodically
        static int cleanup_counter = 0;
        if (++cleanup_counter >= 1000) {
            cleanup_counter = 0;
            std::vector<ClientKey> to_del;
            for (auto &p : clients) {
                if (p.second.waiting && (now - p.second.timestamp) > 60) {
                    to_del.push_back(p.first);
                }
            }
            for (auto &k : to_del) clients.erase(k);
        }

        if (rv <= 0) continue;
        if (!FD_ISSET(sockfd, &rfds)) continue;

        char buf[1024];
        struct sockaddr_storage cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr*)&cliaddr, &clilen);
        if (n <= 0) continue;

        ClientKey key; memset(&key, 0, sizeof(key));
        // Copy only the actual address bytes we received and zero the rest
        if (clilen <= (socklen_t)sizeof(key.ss)) {
            memcpy(&key.ss, &cliaddr, clilen);
            if (clilen < (socklen_t)sizeof(key.ss)) memset(((char*)&key.ss) + clilen, 0, sizeof(key.ss) - clilen);
        } else {
            memcpy(&key.ss, &cliaddr, sizeof(key.ss));
        }
        key.len = clilen;

        auto it = clients.find(key);
        bool client_exists = (it != clients.end());

        // If message size is neither calcProtocol nor calcMessage, test whether printable text
        if (n != (ssize_t)CP_SIZE && n != (ssize_t)CM_SIZE) {
            bool printable = true;
            for (ssize_t i = 0; i < n; ++i) {
                unsigned char c = (unsigned char)buf[i];
                if (c < 9 || (c > 13 && c < 32) || c == 127) { printable = false; break; }
            }
            if (!printable) {
                // Malformed binary/intermediate size -> reply binary NOT-OK (calcMessage with message=2)
                send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                continue;
            }
            // else treat as text protocol ..
        }

        // Try binary (calcProtocol)
        if (n == (ssize_t)CP_SIZE) {
            // Parse calcProtocol from wire buffer
            calcProtocol cp_host{};
            const unsigned char *b = (const unsigned char*)buf;
            cp_host.type = read_u16_be(b + 0);
            cp_host.major_version = read_u16_be(b + 2);
            cp_host.minor_version = read_u16_be(b + 4);
            cp_host.id = read_u32_be(b + 6);
            cp_host.arith = read_u32_be(b +10);
            cp_host.inValue1 = read_u32_be(b +14);
            cp_host.inValue2 = read_u32_be(b +18);
            cp_host.inResult = read_u32_be(b +22);

            // Empty/invalid binary hello -> send binary error
            if (!client_exists && !is_valid_binary_protocol(cp_host)) {
                send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                continue;
            }

            if (!client_exists) {
                // This is a response from a client that has already been timed out and removed.
                // The client is sending a valid calcProtocol, but we don't have a state for it.
                // Instead of treating it as a new client, we should ignore it to prevent errors.
                continue;
            } else {
                // Existing binary client: validate answer
                ClientState &cs = it->second;
                if (cp_host.id != cs.task_id) {
                    send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                } else if ((now - cs.timestamp) > 10) {
                    send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                } else {
                    int32_t received_result = (int32_t)cp_host.inResult;
                    if (received_result == cs.expected) send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 1);
                    else send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                }
                clients.erase(it);
                continue;
            }
        }

        // If size matches calcMessage (binary), parse/wrap behavior:
        if (n == (ssize_t)CM_SIZE) {
            const unsigned char *mb = (const unsigned char*)buf;
            uint16_t m_type = read_u16_be(mb + 0);
            uint32_t m_message = read_u32_be(mb + 2);
            uint16_t m_protocol = read_u16_be(mb + 6);
            uint16_t m_maj = read_u16_be(mb + 8);
            uint16_t m_min = read_u16_be(mb +10);

            // If it's a truly empty calcMessage, respond with binary NOT-OK
            if (!client_exists && m_type == 0 && m_message == 0 && m_protocol == 0 && m_maj == 0 && m_min == 0) {
                send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                continue;
            }

            // If this is a registration / hello (non-empty calcMessage) and new client, treat as binary hello
            if (!client_exists) {
                // Stricter check for binary hello based on protocol description
                if (m_type == 22 && m_protocol == 17) {
                    ClientState cs{}; cs.is_binary = true; cs.waiting = true; cs.timestamp = now;
                    uint32_t code = (rand() % 4) + 1;
                    int32_t a = randomInt(); int32_t b = randomInt(); if (code == 4 && b == 0) b = 1;
                    int32_t expected = (code==1? a+b : code==2? a-b : code==3? a*b : a/b);
                    uint32_t id = (uint32_t)(rand() ^ time(NULL));
                    cs.task_id = id; cs.expected = expected; cs.v1 = a; cs.v2 = b; cs.arith = code;
                    clients[key] = cs;

                    calcProtocol out{}; out.type = 1; out.major_version = 1; out.minor_version = 1;
                    out.id = id; out.arith = code; out.inValue1 = a; out.inValue2 = b; out.inResult = 0;
                    send_calcProtocol_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, out);
                } else {
                    // Not a valid binary hello, treat as malformed
                    send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                }
                continue;
            }

            // If client exists and sent a calcMessage mid-dialog, it's unexpected for binary flow -> reply binary NOT-OK
            if (client_exists) {
                send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                // do not erase client here; wait for proper response
                continue;
            }
        }

        // Text protocol handling
        string s(buf, n);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();

        if (!client_exists) {
            // New text client. The first message from a text client must be "TEXT UDP 1.1".
            if (s == "TEXT UDP 1.1") {
                // New text client: send task (text)
                ClientState cs{}; cs.is_binary = false; cs.waiting = true; cs.timestamp = now;
                uint32_t code = (rand() % 4) + 1;
                int32_t a = randomInt(); int32_t b = randomInt(); if (code == 4 && b == 0) b = 1;
                int32_t expected = (code==1? a+b : code==2? a-b : code==3? a*b : a/b);
                cs.expected = expected; cs.v1 = a; cs.v2 = b; cs.arith = code;
                clients[key] = cs;

                const char *opstr = (code==1? "add" : code==2? "sub" : code==3? "mul" : "div");
                char outmsg[128]; int len = snprintf(outmsg, sizeof(outmsg), "%s %d %d\n", opstr, a, b);
                sendto(sockfd, outmsg, len, 0, (struct sockaddr*)&cliaddr, clilen);
            } else {
                 // This is a malformed request (wrong version, rubbish, or late answer). Send error.
                const char *err = "ERROR\n"; sendto(sockfd, err, strlen(err), 0, (struct sockaddr*)&cliaddr, clilen);
            }
        } else {
            // Existing text client: parse "result"
            ClientState &cs = it->second;
            int32_t res = 0;
            if (sscanf(s.c_str(), "%d", &res) == 1) {
                if ((now - cs.timestamp) > 60) {
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
                const char *err = "ERROR\n"; sendto(sockfd, err, strlen(err), 0, (struct sockaddr*)&cliaddr, clilen);
            }
        }
    }

    close(sockfd);
    return 0;
}
// --- IGNORE ---