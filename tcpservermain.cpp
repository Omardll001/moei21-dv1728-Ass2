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
        } else if(pid == 0) {
            close(listenfd);

    // Immediately send greeting for all clients
    const char* hello = "TEXT TCP 1.1\n";
    send(connfd, hello, strlen(hello), 0);

	// Now read the first line from client to check if it's binary
	std::string line;
	ssize_t r = recv_line(connfd, line); // blocking
	std::string low = line;
	for(auto &c: low) c = (char)tolower(c);

	if(low.find("/binary") != std::string::npos) {
		// Binary client
		const char* bin_hello = "BINARY TCP 1.0\n";
		send(connfd, bin_hello, strlen(bin_hello), 0);

		// then handle binary client protocol ...
		handle_binary_client(connfd);
	} else {
		// Normal text client: continue handling
		handle_text_client(connfd);
	}

            close(connfd);
            _exit(0);
        }

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
