// udpservermain.cpp
// Minimal UDP server, according to feedback
// - Binds only to the address provided (IPv4 by default for 'localhost')
// - Single socket
// - Distinguishes binary (calcProtocol) and text messages
// - On new client: send task, wait for one response, reply OK/NOT OK, then forget client
// - Minimal output: prints only startup line and errors. Optional debug via -DDEBUG or keypoint logging via -DKEYPOINT

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

static int setup_socket_bind(const char *host, const char *port) {
    struct addrinfo hints{}, *res = NULL, *rp;
    // default to IPv4 unless host explicitly looks like IPv6
    bool prefer_ipv6 = false;
    if (strchr(host, ':') != NULL || strcmp(host, "::1") == 0 || strcmp(host, "ip6-localhost") == 0) prefer_ipv6 = true;
    hints.ai_family = prefer_ipv6 ? AF_INET6 : AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST; // require numeric host; if fails, fall back

    int s = getaddrinfo(host, port, &hints, &res);
    if (s != 0) {
        // try without AI_NUMERICHOST to allow names like 'localhost'
        hints.ai_flags = 0;
        s = getaddrinfo(host, port, &hints, &res);
        if (s != 0) return -1;
    }

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        // Only bind to same family as requested (prevent binding both v4+v6)
        if (prefer_ipv6 && rp->ai_family != AF_INET6) continue;
        if (!prefer_ipv6 && rp->ai_family != AF_INET) continue;

        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) continue;
        // increase buffers to handle concurrent bursts
        int buf = 4 * 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));

        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break; // success
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int send_calcProtocol_udp(int sockfd, const struct sockaddr *to, socklen_t tolen, const calcProtocol &cp_host) {
    calcProtocol cp_net{};
    cp_net.type = htons(cp_host.type);
    cp_net.major_version = htons(cp_host.major_version);
    cp_net.minor_version = htons(cp_host.minor_version);
    cp_net.id = htonl(cp_host.id);
    cp_net.arith = htonl(cp_host.arith);
    cp_net.inValue1 = htonl(cp_host.inValue1);
    cp_net.inValue2 = htonl(cp_host.inValue2);
    cp_net.inResult = htonl(cp_host.inResult);
    ssize_t s = sendto(sockfd, &cp_net, sizeof(cp_net), 0, to, tolen);
    return (s == (ssize_t)sizeof(cp_net)) ? 0 : -1;
}

static int send_calcMessage_udp(int sockfd, const struct sockaddr *to, socklen_t tolen, uint32_t message) {
    calcMessage m{};
    m.type = htons(2);
    m.message = htonl(message);
    m.protocol = htons(17);
    m.major_version = htons(1);
    m.minor_version = htons(1);
    ssize_t s = sendto(sockfd, &m, sizeof(m), 0, to, tolen);
    return (s == (ssize_t)sizeof(m)) ? 0 : -1;
}

static bool is_valid_binary_protocol(const calcProtocol &cp) {
    if (cp.major_version != 1 || cp.minor_version != 1) return false;
    if (cp.type == 0 && cp.id == 0 && cp.arith == 0 && cp.inValue1 == 0 && cp.inValue2 == 0 && cp.inResult == 0) return false;
    return true;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s host:port\n", argv[0]);
        return 1;
    }

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
                if (p.second.waiting && (now - p.second.timestamp) > 10) {
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
        memcpy(&key.ss, &cliaddr, sizeof(cliaddr)); key.len = clilen;
        auto it = clients.find(key);
        bool client_exists = (it != clients.end());

        // Try binary
        if (n == (ssize_t)sizeof(calcProtocol)) {
            calcProtocol cp_net; memcpy(&cp_net, buf, sizeof(cp_net));
            calcProtocol cp_host;
            cp_host.type = ntohs(cp_net.type);
            cp_host.major_version = ntohs(cp_net.major_version);
            cp_host.minor_version = ntohs(cp_net.minor_version);
            cp_host.id = ntohl(cp_net.id);
            cp_host.arith = ntohl(cp_net.arith);
            cp_host.inValue1 = ntohl(cp_net.inValue1);
            cp_host.inValue2 = ntohl(cp_net.inValue2);
            cp_host.inResult = ntohl(cp_net.inResult);

            if (!client_exists) {
                if (!is_valid_binary_protocol(cp_host)) {
                    // ignore invalid binary hello
                    continue;
                }
                // New binary client: send task
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

        // If size matches calcMessage (binary) - handle 'hello' messages maybe ignored
        if (n == (ssize_t)sizeof(calcMessage)) {
            calcMessage m; memcpy(&m, buf, sizeof(m));
            if (m.type == 0 && m.message == 0 && m.protocol == 0 && m.major_version == 0 && m.minor_version == 0) {
                // ignore empty calcMessage
                continue;
            }
            // fallthrough to text parsing in case it's actually text
        }

        // Text protocol handling
        string s(buf, n);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();

        if (!client_exists) {
            // New text client: send task
            ClientState cs{}; cs.is_binary = false; cs.waiting = true; cs.timestamp = now;
            uint32_t code = (rand() % 4) + 1;
            int32_t a = randomInt(); int32_t b = randomInt(); if (code == 4 && b == 0) b = 1;
            int32_t expected = (code==1? a+b : code==2? a-b : code==3? a*b : a/b);
            uint32_t id = (uint32_t)(rand() ^ time(NULL));
            cs.task_id = id; cs.expected = expected; cs.v1 = a; cs.v2 = b; cs.arith = code;
            clients[key] = cs;

            const char *opstr = (code==1? "add" : code==2? "sub" : code==3? "mul" : "div");
            char outmsg[128]; int len = snprintf(outmsg, sizeof(outmsg), "%u %s %d %d\n", id, opstr, a, b);
            sendto(sockfd, outmsg, len, 0, (struct sockaddr*)&cliaddr, clilen);
        } else {
            // Existing text client: parse "id result"
            ClientState &cs = it->second;
            uint32_t id = 0; int32_t res = 0;
            if (sscanf(s.c_str(), "%u %d", &id, &res) == 2) {
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
    }

    close(sockfd);
    return 0;
}
