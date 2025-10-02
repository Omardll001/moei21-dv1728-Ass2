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
    bool operator<(ClientKey const& o) const {
        if (ss.ss_family != o.ss.ss_family) return ss.ss_family < o.ss.ss_family;
        // compare by bytes
        int cmp = memcmp(&ss, &o.ss, sizeof(ss));
        return cmp < 0;
    }
};

struct ClientState {
    uint32_t task_id;
    int32_t expected;
    int32_t v1, v2;
    uint32_t arith;
    time_t timestamp; // when task sent
    bool waiting; // waiting for answer
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
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

bool sockaddr_equal(const struct sockaddr_storage &a, const struct sockaddr_storage &b) {
    if (a.ss_family != b.ss_family) return false;
    if (a.ss_family == AF_INET) {
        return memcmp(&((struct sockaddr_in&)a).sin_addr, &((struct sockaddr_in&)b).sin_addr, sizeof(in_addr))==0 &&
               ((struct sockaddr_in&)a).sin_port == ((struct sockaddr_in&)b).sin_port;
    } else if (a.ss_family == AF_INET6) {
        return memcmp(&((struct sockaddr_in6&)a).sin6_addr, &((struct sockaddr_in6&)b).sin6_addr, sizeof(in6_addr))==0 &&
               ((struct sockaddr_in6&)a).sin6_port == ((struct sockaddr_in6&)b).sin6_port;
    }
    return false;
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
    m.type = htons(2); // server->client binary msg
    m.message = htonl(message);
    m.protocol = htons(17);
    m.major_version = htons(1);
    m.minor_version = htons(1);
    ssize_t s = sendto(sockfd, &m, sizeof(m), 0, to, tolen);
    return (s == (ssize_t)sizeof(m)) ? 0 : -1;
}

int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr,"Usage: %s host:port\n", argv[0]); return 1; }
    initCalcLib();
    srand(time(NULL));
    char *input = argv[1];
    char *sep = strchr(input, ':');
    if (!sep) { fprintf(stderr,"Error: input must be host:port\n"); return 1; }
    char host[256], port[64];
    size_t hlen = sep - input;
    if (hlen >= sizeof(host)) { fprintf(stderr,"hostname too long\n"); return 1; }
    strncpy(host, input, hlen); host[hlen] = '\0';
    strncpy(port, sep+1, sizeof(port)-1); port[sizeof(port)-1]='\0';
    int sockfd = setup_socket(host, port);
    if (sockfd < 0) { perror("setup_socket"); return 1; }
    printf("UDP server on %s:%s\n", host, port);

    std::map<ClientKey, ClientState> clients;

    while (1) {
        // select on sockfd, timeout 1s to sweep old tasks
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        struct timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;
        int rv = select(sockfd+1, &rfds, NULL, NULL, &tv);
        time_t now = time(NULL);
        // cleanup stale clients (>10s waiting)
        std::vector<ClientKey> to_delete;
        for (auto &p : clients) {
            if (p.second.waiting && (now - p.second.timestamp) > 10) {
                to_delete.push_back(p.first);
            }
        }
        for (auto &k : to_delete) clients.erase(k);

        if (rv <= 0) continue;
        if (FD_ISSET(sockfd, &rfds)) {
            char buf[1024];
            struct sockaddr_storage cliaddr;
            socklen_t clilen = sizeof(cliaddr);
            ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr*)&cliaddr, &clilen);
            if (n <= 0) continue;

            // find client key
            ClientKey key; memset(&key,0,sizeof(key));
            memcpy(&key.ss, &cliaddr, sizeof(cliaddr));
            key.len = clilen;

            bool found = false;
            ClientState state{};
            auto it = clients.find(key);
            if (it != clients.end()) { found = true; state = it->second; }

            // decide type: if incoming size matches calcProtocol and header fields valid, treat as binary
            bool likely_binary = (n >= (ssize_t)sizeof(calcProtocol));
            if (likely_binary) {
                // try to parse
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

                // basic validation
                if ( (cp_host.major_version == 1 && cp_host.minor_version == 1) &&
                     (cp_host.type == 21 || cp_host.type == 22) ) {
                    // treat as binary client
                    if (!found) {
                        // first message: If client sends type 21 (text) or 22 (binary) as client->server? In assignment, client->server binary type=22.
                        // We will treat id==0 as request for task.
                        // We'll respond with a calcProtocol task (type=1 server->client)
                        ClientState cs{};
                        cs.is_binary = true;
                        cs.waiting = true;
                        cs.timestamp = now;
                        // generate random op
                        uint32_t code = (rand()%4)+1;
                        int32_t a = randomInt();
                        int32_t b = (code==4) ? ( (randomInt()==0) ? 1 : randomInt() ) : randomInt();
                        if (code==4 && b==0) b=1;
                        int32_t expected=0;
                        if (code==1) expected = a+b;
                        else if (code==2) expected = a-b;
                        else if (code==3) expected = a*b;
                        else if (code==4) expected = a/b;
                        uint32_t id = (uint32_t)(rand() ^ time(NULL));
                        cs.task_id = id; cs.expected = expected; cs.v1 = a; cs.v2 = b; cs.arith = code;
                        // store
                        clients[key] = cs;
                        // send task (calcProtocol)
                        calcProtocol out{};
                        out.type = 1; out.major_version = 1; out.minor_version = 1;
                        out.id = id; out.arith = code; out.inValue1 = a; out.inValue2 = b; out.inResult = 0;
                        send_calcProtocol_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, out);
                        continue;
                    } else {
                        // existing client; interpret this as answer
                        if (!it->second.waiting) {
                            // client sent again without task - issue a new task
                            ClientState &cs = it->second;
                            cs.waiting = true; cs.timestamp = now;
                            uint32_t code = (rand()%4)+1;
                            int32_t a = randomInt();
                            int32_t b = (code==4) ? ( (randomInt()==0) ? 1 : randomInt() ) : randomInt();
                            if (code==4 && b==0) b=1;
                            int32_t expected=0;
                            if (code==1) expected = a+b;
                            else if (code==2) expected = a-b;
                            else if (code==3) expected = a*b;
                            else if (code==4) expected = a/b;
                            uint32_t id = (uint32_t)(rand() ^ time(NULL));
                            cs.task_id = id; cs.expected = expected; cs.v1=a; cs.v2=b; cs.arith=code;
                            calcProtocol out{};
                            out.type = 1; out.major_version = 1; out.minor_version = 1;
                            out.id = id; out.arith = code; out.inValue1 = a; out.inValue2 = b; out.inResult = 0;
                            send_calcProtocol_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, out);
                            continue;
                        } else {
                            // client had a waiting task: validate incoming cp_host.inResult if id matches
                            calcProtocol cp_net2; memcpy(&cp_net2, buf, sizeof(calcProtocol));
                            calcProtocol cp2{};
                            cp2.type = ntohs(cp_net2.type);
                            cp2.major_version = ntohs(cp_net2.major_version);
                            cp2.minor_version = ntohs(cp_net2.minor_version);
                            cp2.id = ntohl(cp_net2.id);
                            cp2.arith = ntohl(cp_net2.arith);
                            cp2.inValue1 = ntohl(cp_net2.inValue1);
                            cp2.inValue2 = ntohl(cp_net2.inValue2);
                            cp2.inResult = ntohl(cp_net2.inResult);
                            ClientState &cs = it->second;
                            if (cp2.id != cs.task_id) {
                                send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                            } else {
                                // check timeout
                                if ((now - cs.timestamp) > 10) {
                                    // too late -> reject and remove
                                    send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                                    clients.erase(it);
                                } else {
                                    if (cp2.inResult == cs.expected) {
                                        send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 1);
                                    } else {
                                        send_calcMessage_udp(sockfd, (struct sockaddr*)&cliaddr, clilen, 2);
                                    }
                                    // remove task
                                    clients.erase(it);
                                }
                            }
                            continue;
                        }
                    }
                } // end binary branch
                // If we reach here, treat it as text as fallback
            }

            // If not binary, treat as text (simple ASCII)
            // Interpret first message as request for task, second as answer.
            std::string s(buf, buf + (n>0 ? n : 0));
            // trim newline
            while (!s.empty() && (s.back()=='\n' || s.back()=='\r')) s.pop_back();

            if (!found) {
                // generate a text task like "add 4 5\n"
                int code = (rand()%4)+1;
                int a = randomInt();
                int b = (code==4) ? ( (randomInt()==0) ? 1 : randomInt() ) : randomInt();
                if (code==4 && b==0) b=1;
                int expected = 0;
                const char *opstr = "add";
                if (code==1) { expected = a+b; opstr = "add"; }
                else if (code==2) { expected = a-b; opstr = "sub"; }
                else if (code==3) { expected = a*b; opstr = "mul"; }
                else { expected = a/b; opstr = "div"; }
                uint32_t id = (uint32_t)(rand() ^ time(NULL));
                ClientState cs{};
                cs.task_id = id; cs.expected = expected; cs.v1=a; cs.v2=b; cs.arith=code; cs.timestamp = now; cs.waiting = true; cs.is_binary=false;
                clients[key] = cs;
                char outmsg[128];
                int len = snprintf(outmsg, sizeof(outmsg), "%u %s %d %d\n", id, opstr, a, b);
                sendto(sockfd, outmsg, len, 0, (struct sockaddr*)&cliaddr, clilen);
                continue;
            } else {
                // found existing client: interpret s as answer. Expect format "id result"
                uint32_t id=0; int32_t res=0;
                if (sscanf(s.c_str(), "%u %d", &id, &res) >= 2) {
                    ClientState cs = it->second;
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
                continue;
            }
        }
    }

    close(sockfd);
    return 0;
}
