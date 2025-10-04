// udpServer.cpp
// Usage: udpServer host:port
// Uses select() and tracks clients (addr) and outstanding tasks with timestamps.
// Removes tasks not responded within 10s.

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
        if (len != o.len) return len < o.len;
        if (ss.ss_family != o.ss.ss_family) return ss.ss_family < o.ss.ss_family;
        return memcmp(&ss, &o.ss, sizeof(ss)) < 0;
    }
};

struct ClientState {
    uint32_t task_id;
    int32_t expected;
    int32_t v1, v2;
    uint32_t arith;
    time_t timestamp;
    bool waiting;
    bool is_binary;
};

int setup_socket(const char *host, const char *port) {
    struct addrinfo hints{}, *res, *rp;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) continue;
        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); 
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int send_calcProtocol_udp(int sockfd, const struct sockaddr *to, socklen_t tolen, const calcProtocol &cp_host) {
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

int send_calcMessage_udp(int sockfd, const struct sockaddr *to, socklen_t tolen, uint32_t message) {
    calcMessage m{};
    m.type = htons(2);
    m.message = htonl(message);
    m.protocol = htons(17);
    m.major_version = htons(1);
    m.minor_version = htons(1);
    ssize_t s = sendto(sockfd, &m, sizeof(m), 0, to, tolen);
    return (s == (ssize_t)sizeof(m)) ? 0 : -1;
}

bool is_valid_binary_protocol(const calcProtocol &cp) {
    return (cp.major_version == 1 && cp.minor_version == 1) &&
           (cp.type == 21 || cp.type == 22);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { 
        fprintf(stderr, "Usage: %s host:port\n", argv[0]); 
        return 1; 
    }
    initCalcLib();
    srand(time(NULL));
    
    char *input = argv[1];
    char *sep = strchr(input, ':');
    if (!sep) { 
        fprintf(stderr, "Error: input must be host:port\n"); 
        return 1; 
    }
    char host[256], port[64];
    size_t hlen = sep - input;
    if (hlen >= sizeof(host)) { 
        fprintf(stderr, "hostname too long\n"); 
        return 1; 
    }
    strncpy(host, input, hlen); 
    host[hlen] = '\0';
    strncpy(port, sep + 1, sizeof(port) - 1); 
    port[sizeof(port) - 1] = '\0';
    
    int sockfd = setup_socket(host, port);
    if (sockfd < 0) { 
        perror("setup_socket"); 
        return 1; 
    }
    fprintf(stderr, "UDP server on %s:%s\n", host, port);

    std::map<ClientKey, ClientState> clients;

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        struct timeval tv; 
        tv.tv_sec = 1; 
        tv.tv_usec = 0;
        
        int rv = select(sockfd + 1, &rfds, NULL, NULL, &tv);
        time_t now = time(NULL);
        
        // Cleanup stale clients (>10s waiting)
        std::vector<ClientKey> to_delete;
        for (auto &p : clients) {
            if (p.second.waiting && (now - p.second.timestamp) > 10) {
                to_delete.push_back(p.first);
            }
        }
        for (auto &k : to_delete) {
            clients.erase(k);
        }

        if (rv <= 0) continue;
        
        if (FD_ISSET(sockfd, &rfds)) {
            char buf[1024];
            struct sockaddr_storage cliaddr;
            socklen_t clilen = sizeof(cliaddr);
            ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr*)&cliaddr, &clilen);
            if (n <= 0) continue;

            ClientKey key;
            memset(&key, 0, sizeof(key));
            memcpy(&key.ss, &cliaddr, sizeof(cliaddr));
            key.len = clilen;

            auto it = clients.find(key);
            bool client_exists = (it != clients.end());

            // Try to parse as binary first
            if (n >= (ssize_t)sizeof(calcProtocol)) {
                calcProtocol cp_net;
                memcpy(&cp_net, buf, sizeof(calcProtocol));
                calcProtocol cp_host;
                cp_host.type = ntohs(cp_net.type);
                cp_host.major_version = ntohs(cp_net.major_version);
                cp_host.minor_version = ntohs(cp_net.minor_version);
                cp_host.id = ntohl(cp_net.id);
                cp_host.arith = ntohl(cp_net.arith);
                cp_host.inValue1 = ntohl(cp_net.inValue1);
                cp_host.inValue2 = ntohl(cp_net.inValue2);
                cp_host.inResult = ntohl(cp_net.inResult);

                if (is_valid_binary_protocol(cp_host)) {
                    if (!client_exists) {
                        // New binary client - send task
                        ClientState cs{};
                        cs.is_binary = true;
                        cs.waiting = true;
                        cs.timestamp = now;
                        
                        uint32_t code = (rand() % 4) + 1;
                        int32_t a = randomInt();
                        int32_t b = (code == 4) ? ((randomInt() == 0) ? 1 : randomInt()) : randomInt();
                        if (code == 4 && b == 0) b = 1;
                        
                        int32_t expected = 0;
                        if (code == 1) expected = a + b;
                        else if (code == 2) expected = a - b;
                        else if (code == 3) expected = a * b;
                        else if (code == 4) expected = a / b;
                        
                        uint32_t id = (uint32_t)(rand() ^ time(NULL));
                        cs.task_id = id; 
                        cs.expected = expected; 
                        cs.v1 = a; 
                        cs.v2 = b; 
                        cs.arith = code;
                        
                        clients[key] = cs;
                        
                        calcProtocol out{};
                        out.type = 1; 
                        out.major_version = 1; 
                        out.minor_version = 1;
                        out.id = id; 
                        out.arith = code; 
                        out.inValue1 = a; 
                        out.inValue2 = b; 
                        out.inResult = 0;
                        
                        send_calcProtocol_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, out);
                        continue;
                    } else {
                        // Existing binary client - validate answer
                        ClientState &cs = it->second;
                        
                        if (!cs.waiting) {
                            // Client sent without active task - send new task
                            cs.waiting = true;
                            cs.timestamp = now;
                            
                            uint32_t code = (rand() % 4) + 1;
                            int32_t a = randomInt();
                            int32_t b = (code == 4) ? ((randomInt() == 0) ? 1 : randomInt()) : randomInt();
                            if (code == 4 && b == 0) b = 1;
                            
                            int32_t expected = 0;
                            if (code == 1) expected = a + b;
                            else if (code == 2) expected = a - b;
                            else if (code == 3) expected = a * b;
                            else if (code == 4) expected = a / b;
                            
                            uint32_t id = (uint32_t)(rand() ^ time(NULL));
                            cs.task_id = id; 
                            cs.expected = expected; 
                            cs.v1 = a; 
                            cs.v2 = b; 
                            cs.arith = code;
                            
                            calcProtocol out{};
                            out.type = 1; 
                            out.major_version = 1; 
                            out.minor_version = 1;
                            out.id = id; 
                            out.arith = code; 
                            out.inValue1 = a; 
                            out.inValue2 = b; 
                            out.inResult = 0;
                            
                            send_calcProtocol_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, out);
                            continue;
                        } else {
                            // Client has active task - validate answer
                            if (cp_host.id != cs.task_id) {
                                send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                                clients.erase(it);
                            } else {
                                if ((now - cs.timestamp) > 10) {
                                    // Too late
                                    send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                                    clients.erase(it);
                                } else {
                                    if (cp_host.inResult == (uint32_t)cs.expected) {
                                        send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 1);
                                    } else {
                                        send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                                    }
                                    clients.erase(it);
                                }
                            }
                            continue;
                        }
                    }
                }
            }

            // Treat as text protocol
            std::string s(buf, n);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) 
                s.pop_back();

            if (!client_exists) {
                // New text client - send task
                int code = (rand() % 4) + 1;
                int a = randomInt();
                int b = (code == 4) ? ((randomInt() == 0) ? 1 : randomInt()) : randomInt();
                if (code == 4 && b == 0) b = 1;
                
                int expected = 0;
                const char *opstr = "add";
                if (code == 1) { expected = a + b; opstr = "add"; }
                else if (code == 2) { expected = a - b; opstr = "sub"; }
                else if (code == 3) { expected = a * b; opstr = "mul"; }
                else { expected = a / b; opstr = "div"; }
                
                uint32_t id = (uint32_t)(rand() ^ time(NULL));
                ClientState cs{};
                cs.task_id = id; 
                cs.expected = expected; 
                cs.v1 = a; 
                cs.v2 = b; 
                cs.arith = code; 
                cs.timestamp = now; 
                cs.waiting = true; 
                cs.is_binary = false;
                
                clients[key] = cs;
                
                char outmsg[128];
                int len = snprintf(outmsg, sizeof(outmsg), "%u %s %d %d\n", id, opstr, a, b);
                sendto(sockfd, outmsg, len, 0, (struct sockaddr*)&cliaddr, clilen);
            } else {
                // Existing text client - validate answer
                ClientState &cs = it->second;
                uint32_t id = 0; 
                int32_t res = 0;
                
                if (sscanf(s.c_str(), "%u %d", &id, &res) == 2) {
                    if (id != cs.task_id) {
                        const char *nok = "NOT OK\n";
                        sendto(sockfd, nok, strlen(nok), 0, (struct sockaddr*)&cliaddr, clilen);
                    } else {
                        if ((now - cs.timestamp) > 10) {
                            const char *late = "NOT OK\n";
                            sendto(sockfd, late, strlen(late), 0, (struct sockaddr*)&cliaddr, clilen);
                            clients.erase(it);
                        } else {
                            if (res == cs.expected) {
                                const char *ok = "OK\n";
                                sendto(sockfd, ok, strlen(ok), 0, (struct sockaddr*)&cliaddr, clilen);
                            } else {
                                const char *nok = "NOT OK\n";
                                sendto(sockfd, nok, strlen(nok), 0, (struct sockaddr*)&cliaddr, clilen);
                            }
                            clients.erase(it);
                        }
                    }
                } else {
                    const char *err = "ERROR PARSE\n";
                    sendto(sockfd, err, strlen(err), 0, (struct sockaddr*)&cliaddr, clilen);
                }
            }
        }
    }

    close(sockfd);
    return 0;
}