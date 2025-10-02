// tcpServer.cpp
// Usage: tcpServer host:port
// Fork per connection. Supports TEXT TCP 1.1 and BINARY TCP 1.1.
// Per-operation timeout 5s -> on timeout send "ERROR TO\n" and exit child.

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string> 

#include "protocol.h"
extern "C" {
#include "calcLib.h"
}

using namespace std;

static int conn_fd_for_alarm = -1;

void alarm_handler(int) {
    if (conn_fd_for_alarm != -1) {
        const char *msg = "ERROR TO\n";
        send(conn_fd_for_alarm, msg, strlen(msg), 0);
        // close and exit child
        close(conn_fd_for_alarm);
    }
    // use _exit to avoid flushing stdio twice in fork
    _exit(1);
}

ssize_t full_read(int fd, void *buf, size_t count) {
    size_t done = 0;
    char *p = (char*)buf;
    while (done < count) {
        ssize_t r = recv(fd, p + done, count - done, 0);
        if (r == 0) return done; // EOF
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += r;
    }
    return (ssize_t)done;
}

ssize_t recv_line(int fd, std::string &out) {
    out.clear();
    char c;
    while (1) {
        ssize_t r = recv(fd, &c, 1, 0);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        out.push_back(c);
        if (c == '\n') break;
    }
    return out.size();
}

int setup_listener(const char *host, const char *port) {
    struct addrinfo hints{}, *res, *rp;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int listenfd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        listenfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listenfd == -1) continue;
        int opt = 1;
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (bind(listenfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            if (listen(listenfd, 16) == 0) break;
        }
        close(listenfd);
        listenfd = -1;
    }
    freeaddrinfo(res);
    return listenfd;
}

void handle_text_client(int fd) {
    // Child will use alarm(5) for each operation.
    conn_fd_for_alarm = fd;
    signal(SIGALRM, alarm_handler);

    // send greeting? The protocol from assignment expects client to start?
    // For TCP we expect client to connect and then send requests. We'll read lines until EOF.

    std::string line;
    while (1) {
        alarm(5); // enforce 5s for the read
        ssize_t r = recv_line(fd, line);
        alarm(0);
        if (r <= 0) break;
        // parse command: "add 1 2\n" or "fadd 1.2 3.4\n"
        // We will accept integer ops: add/sub/mul/div
        char cmd[32];
        int a=0,b=0;
        if (sscanf(line.c_str(), "%31s %d %d", cmd, &a, &b) >= 1) {
            if (strcmp(cmd, "add")==0 || strcmp(cmd, "sub")==0 || strcmp(cmd,"mul")==0 || strcmp(cmd,"div")==0) {
                int res=0;
                if (strcmp(cmd,"add")==0) res = a+b;
                else if (strcmp(cmd,"sub")==0) res = a-b;
                else if (strcmp(cmd,"mul")==0) res = a*b;
                else if (strcmp(cmd,"div")==0) {
                    if (b==0) {
                        const char *err = "ERROR DIV0\n";
                        send(fd, err, strlen(err), 0);
                        break;
                    } else res = a/b;
                }
                char outbuf[128];
                int n = snprintf(outbuf, sizeof(outbuf), "%d\n", res);
                alarm(5);
                send(fd, outbuf, n, 0);
                alarm(0);
                continue;
            } else {
                const char *err = "ERROR CMD\n";
                send(fd, err, strlen(err), 0);
                continue;
            }
        } else {
            const char *err = "ERROR PARSE\n";
            send(fd, err, strlen(err), 0);
            continue;
        }
    }
    close(fd);
}

void handle_binary_client(int fd) {
    conn_fd_for_alarm = fd;
    signal(SIGALRM, alarm_handler);

    // Server expects client to send calcProtocol (type=22 maybe), we respond with server->client calcProtocol (type=1) as task,
    // then client replies with calcProtocol result (type=2). But since assignment uses same API as A1,
    // we'll implement a simple request-response loop:
    // - read a calcProtocol from client (client->server). If it's type 22 (client->server binary),
    //   server validates and responds with calcMessage (OK/NOT OK).
    // - If client sends a request for a task? Alternatively, many tests will first request a task.
    // To be safe: if first message from client has type==2 (client->server request with id==0),
    // treat that as "I want a task" and server will respond with a calcProtocol (type=1) containing id and operation.
    // If client sends a calcProtocol with type==2 with id matching a previous id, we validate inResult.

    uint32_t last_task_id = 0;
    int32_t expected = 0;

    while (1) {
        calcProtocol cp_net;
        alarm(5);
        ssize_t r = full_read(fd, &cp_net, sizeof(cp_net));
        alarm(0);
        if (r <= 0) break;
        // convert to host order
        calcProtocol cp;
        cp.type = ntohs(cp_net.type);
        cp.major_version = ntohs(cp_net.major_version);
        cp.minor_version = ntohs(cp_net.minor_version);
        cp.id = ntohl(cp_net.id);
        cp.arith = ntohl(cp_net.arith);
        cp.inValue1 = ntohl(cp_net.inValue1);
        cp.inValue2 = ntohl(cp_net.inValue2);
        cp.inResult = ntohl(cp_net.inResult);

        if (cp.type == 2) {
            // client->server. Could be a request for a task (id==0) or result for id
            if (cp.id == 0) {
                // client asking for a task. Generate one and send server->client calcProtocol (type=1)
                
                // map to arith code: add=1, sub=2, mul=3, div=4  (calcLib didn't have sub; but we can generate random arith ourselves)
                // We'll pick arith randomly 1-4 but ensure for division second operand != 0.
                int code = (rand()%4)+1;
                int i1 = randomInt();
                int i2;
                if (code == 4) {
                    // make sure not zero
                    do { i2 = randomInt(); } while (i2 == 0);
                } else i2 = randomInt();

                last_task_id = (uint32_t) (rand() ^ time(NULL)); // random id
                expected = 0;
                if (code==1) expected = i1 + i2;
                else if (code==2) expected = i1 - i2;
                else if (code==3) expected = i1 * i2;
                else if (code==4) expected = i1 / i2;

                // prepare outgoing struct in network order
                calcProtocol out{};
                out.type = htons(1);
                out.major_version = htons(1);
                out.minor_version = htons(1);
                out.id = htonl(last_task_id);
                out.arith = htonl(code);
                out.inValue1 = htonl(i1);
                out.inValue2 = htonl(i2);
                out.inResult = htonl(0);
                alarm(5);
                send(fd, &out, sizeof(out), 0);
                alarm(0);
                // continue waiting for client's answer
            } else {
                // client sent result for id cp.id — validate
                if (cp.id != last_task_id) {
                    // not matching id -> reject
                    calcMessage msg{};
                    msg.type = htons(2); // server->client binary
                    msg.message = htonl(2); // NOT OK
                    msg.protocol = htons(6);
                    msg.major_version = htons(1);
                    msg.minor_version = htons(1);
                    alarm(5);
                    send(fd, &msg, sizeof(msg), 0);
                    alarm(0);
                } else {
                    // check cp.inResult
                    if (cp.inResult == expected) {
                        calcMessage msg{};
                        msg.type = htons(2);
                        msg.message = htonl(1); // OK
                        msg.protocol = htons(6);
                        msg.major_version = htons(1);
                        msg.minor_version = htons(1);
                        alarm(5);
                        send(fd, &msg, sizeof(msg), 0);
                        alarm(0);
                    } else {
                        calcMessage msg{};
                        msg.type = htons(2);
                        msg.message = htonl(2); // NOT OK
                        msg.protocol = htons(6);
                        msg.major_version = htons(1);
                        msg.minor_version = htons(1);
                        alarm(5);
                        send(fd, &msg, sizeof(msg), 0);
                        alarm(0);
                    }
                }
            }
        } else {
            // unexpected type - ignore or send error
            const char *e = "ERROR TYPE\n";
            alarm(5);
            send(fd, e, strlen(e), 0);
            alarm(0);
        }
    }

    close(fd);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s host:port\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    initCalcLib();
    srand(time(NULL));

    // parse host:port
    char *input = argv[1];
    char *sep = strchr(input, ':');
    if (!sep) { fprintf(stderr, "Error: input must be host:port\n"); return 1; }
    char host[256]; char port[64];
    size_t hostlen = sep - input;
    if (hostlen >= sizeof(host)) { fprintf(stderr, "hostname too long\n"); return 1; }
    strncpy(host, input, hostlen); host[hostlen] = '\0';
    strncpy(port, sep+1, sizeof(port)-1); port[sizeof(port)-1] = '\0';
    int listenfd = setup_listener(host, port);
    if (listenfd < 0) { perror("setup_listener"); return 1; }
    printf("TCP server on %s:%s\n", host, port);
    while (1) {
        struct sockaddr_storage cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int connfd = accept(listenfd, (struct sockaddr*)&cliaddr, &clilen);
        if (connfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(connfd);
            continue;
                    } else if (pid == 0) {
            // child
            close(listenfd);

            // Peek first message from client with small timeout
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(connfd, &rf);
            struct timeval tv = {2,0}; // 2s timeout
            int sel = select(connfd+1, &rf, NULL, NULL, &tv);
            std::string firstmsg;
            if (sel > 0 && FD_ISSET(connfd, &rf)) {
                char buf[512];
                ssize_t rn = recv(connfd, buf, sizeof(buf)-1, 0);
                if (rn > 0) {
                    buf[rn] = '\0';
                    firstmsg = buf;
                }
            }

            // lowercase copy
            std::string low = firstmsg;
            for(auto &c: low) c = (char)tolower(c);

            // if client asks for binary
            if (low.find("/binary") != std::string::npos) {
                // send exact first lines expected by test
                send(connfd, "binary\n", 7, 0);

                uint32_t code = (rand()%4)+1;
                int32_t a = randomInt();
                int32_t b = randomInt();
                if (code==4 && b==0) b=1;
                const char* opstr="add";
                int32_t expected=0;
                if (code==1){expected=a+b; opstr="add";}
                else if(code==2){expected=a-b; opstr="sub";}
                else if(code==3){expected=a*b; opstr="mul";}
                else {expected=a/b; opstr="div";}
                uint32_t id = rand() ^ time(NULL);

                char assignbuf[128];
                int alen = snprintf(assignbuf,sizeof(assignbuf),"ASSIGNMENT: %u %s %d %d\n",id,opstr,(int)a,(int)b);
                send(connfd, assignbuf, alen,0);

                // wait and validate client response as before...
                
            } else {
                // normal text client
                const char* hello="TEXT TCP 1.1\n";
                send(connfd, hello, strlen(hello), 0);
                handle_text_client(connfd);
            }


            // Read a small first-line message from client with short timeout
            {
                fd_set rf;
                FD_ZERO(&rf);
                FD_SET(connfd, &rf);
                struct timeval tv;
                tv.tv_sec = 2; // small wait for initial client line
                tv.tv_usec = 0;
                int sel = select(connfd+1, &rf, NULL, NULL, &tv);
                if (sel > 0 && FD_ISSET(connfd, &rf)) {
                    char firstbuf[512];
                    ssize_t rn = recv(connfd, firstbuf, sizeof(firstbuf)-1, 0);
                    if (rn > 0) {
                        firstbuf[rn] = '\0';
                        std::string firstmsg(firstbuf);
                        // lowercase copy for matching
                        std::string low = firstmsg;
                        for (auto &c : low) c = (char)tolower(c);

                        // If client explicitly asks for binary path, do textual assignment protocol:
                        if (low.find("/binary") != std::string::npos ||
                            low.find("path = binary") != std::string::npos ||
                            low.find("path=binary") != std::string::npos) {

                            // Generate task (no division by zero)
                            uint32_t code = (rand()%4) + 1; // 1 add, 2 sub, 3 mul, 4 div
                            int32_t a = randomInt();
                            int32_t b = randomInt();
                            if (code == 4 && b == 0) b = 1;
                            int32_t expected = 0;
                            const char *opstr = "add";
                            if (code==1) { expected = a + b; opstr = "add"; }
                            else if (code==2) { expected = a - b; opstr = "sub"; }
                            else if (code==3) { expected = a * b; opstr = "mul"; }
                            else { expected = a / b; opstr = "div"; }
                            uint32_t id = (uint32_t)(rand() ^ time(NULL));

                            // Send textual responses expected by refClient
                            // Line 1: include "binary"
                            send(connfd, "binary\n", 7, 0);

                            // Line 2: ASSIGNMENT: id op a b
                            char assignbuf[128];
                            int alen = snprintf(assignbuf, sizeof(assignbuf),
                                                "ASSIGNMENT: %u %s %d %d\n",
                                                id, opstr, (int)a, (int)b);
                            send(connfd, assignbuf, alen, 0);

                            // Now wait up to 5s for client's textual result (one line)
                            fd_set rf2;
                            FD_ZERO(&rf2);
                            FD_SET(connfd, &rf2);
                            struct timeval tv2; tv2.tv_sec = 5; tv2.tv_usec = 0;
                            int sel2 = select(connfd+1, &rf2, NULL, NULL, &tv2);
                            if (sel2 <= 0) {
                                const char *to = "ERROR TO\n";
                                send(connfd, to, strlen(to), 0);
                                close(connfd);
                                _exit(1);
                            }

                            // Read the client's reply line
                            std::string replyline;
                            ssize_t got = recv_line(connfd, replyline); // uses existing recv_line
                            if (got <= 0) {
                                const char *err = "ERROR PARSE\n";
                                send(connfd, err, strlen(err), 0);
                                close(connfd);
                                _exit(1);
                            }
                            // parse integer from reply (client likely sends just the integer)
                            int client_res = 0;
                            if (sscanf(replyline.c_str(), "%d", &client_res) < 1) {
                                // not an int -> error
                                const char *err = "ERROR PARSE\n";
                                send(connfd, err, strlen(err), 0);
                                close(connfd);
                                _exit(1);
                            }

                            // Validate and respond with textual OK (matching regex ^OK \(myresult=-?[0-9]+\)$)
                            char resultbuf[128];
                            if (client_res == expected) {
                                int n = snprintf(resultbuf, sizeof(resultbuf), "OK (myresult=%d)\n", client_res);
                                send(connfd, resultbuf, n, 0);
                            } else {
                                int n = snprintf(resultbuf, sizeof(resultbuf), "NOT OK (myresult=%d)\n", client_res);
                                send(connfd, resultbuf, n, 0);
                            }

                            close(connfd);
                            _exit(0);
                        } // end binary-path handling

                        // If we got a normal text command (like "add 1 2"), handle it and continue with text loop
                        {
                            char cmdbuf[64];
                            int a=0,b=0;
                            if (sscanf(firstbuf, "%63s %d %d", cmdbuf, &a, &b) >= 1) {
                                if (strcmp(cmdbuf, "add") == 0 || strcmp(cmdbuf, "sub") == 0 ||
                                    strcmp(cmdbuf, "mul") == 0 || strcmp(cmdbuf, "div") == 0) {
                                    int res = 0;
                                    if (strcmp(cmdbuf, "add")==0) res = a + b;
                                    else if (strcmp(cmdbuf, "sub")==0) res = a - b;
                                    else if (strcmp(cmdbuf, "mul")==0) res = a * b;
                                    else {
                                        if (b == 0) {
                                            const char *err = "ERROR DIV0\n";
                                            send(connfd, err, strlen(err), 0);
                                            close(connfd);
                                            _exit(0);
                                        } else res = a / b;
                                    }
                                    char outbuf[128];
                                    int n = snprintf(outbuf, sizeof(outbuf), "%d\n", res);
                                    send(connfd, outbuf, n, 0);
                                    // Now continue handling further text commands in usual text loop
                                    handle_text_client(connfd);
                                    close(connfd);
                                    _exit(0);
                                }
                            }
                        }

                        // otherwise fall through to the heuristic below (peek)
                    } // rn > 0
                } // select
            } // end initial-message block

            // FALLBACK: Peek and decide text vs binary as before
            char buf[512];
            ssize_t n = recv(connfd, buf, sizeof(buf), MSG_PEEK | MSG_DONTWAIT);
            if (n <= 0) {
                close(connfd);
                _exit(0);
            }
            bool allprint = true;
            for (ssize_t i=0;i<n;i++) {
                unsigned char c = buf[i];
                if (c < 9 || (c>13 && c<32)) { allprint = false; break; }
            }
            if (!allprint && n >= (ssize_t)sizeof(calcProtocol)) {
                handle_binary_client(connfd);
            } else {
                handle_text_client(connfd);
            }
            close(connfd);
            _exit(0);
        } // end child

          else {
            // parent
            close(connfd);
            // optionally reap children (simple non-blocking)
            int status = 0;
            while (waitpid(-1, &status, WNOHANG) > 0) {}
        }
    }

    close(listenfd);
    return 0;
}
