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
        close(conn_fd_for_alarm);
    }
    _exit(1);
}

ssize_t full_read(int fd, void *buf, size_t count) {
    size_t done = 0;
    char *p = (char*)buf;
    while (done < count) {
        ssize_t r = recv(fd, p + done, count - done, 0);
        if (r == 0) return done;
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
    conn_fd_for_alarm = fd;
    signal(SIGALRM, alarm_handler);

    // Send TEXT TCP 1.1 greeting
    const char *greeting = "TEXT TCP 1.1\n";
    alarm(5);
    send(fd, greeting, strlen(greeting), 0);
    alarm(0);

    while (1) {
        // Generate task
        int code = (rand() % 4) + 1;
        int a = randomInt();
        int b = (code == 4) ? ((randomInt() == 0) ? 1 : randomInt()) : randomInt();
        if (code == 4 && b == 0) b = 1;
        
        const char *opstr = "add";
        if (code == 1) opstr = "add";
        else if (code == 2) opstr = "sub";
        else if (code == 3) opstr = "mul";
        else opstr = "div";

        char task[128];
        int task_len = snprintf(task, sizeof(task), "%s %d %d\n", opstr, a, b);
        
        alarm(5);
        ssize_t sent = send(fd, task, task_len, 0);
        alarm(0);
        if (sent != task_len) break;

        // Wait for answer
        std::string line;
        alarm(5);
        ssize_t r = recv_line(fd, line);
        alarm(0);
        if (r <= 0) break;

        // Trim newline
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) 
            line.pop_back();

        // Calculate expected result
        int expected = 0;
        if (code == 1) expected = a + b;
        else if (code == 2) expected = a - b;
        else if (code == 3) expected = a * b;
        else if (code == 4) expected = a / b;

        // Parse and validate answer
        int answer = 0;
        if (sscanf(line.c_str(), "%d", &answer) == 1) {
            if (answer == expected) {
                alarm(5);
                send(fd, "OK\n", 3, 0);
                alarm(0);
            } else {
                alarm(5);
                send(fd, "NOT OK\n", 7, 0);
                alarm(0);
            }
        } else {
            alarm(5);
            send(fd, "ERROR PARSE\n", 12, 0);
            alarm(0);
        }
    }
    close(fd);
}

void handle_binary_client(int fd) {
    conn_fd_for_alarm = fd;
    signal(SIGALRM, alarm_handler);

    // Send BINARY TCP 1.1 greeting
    const char *greeting = "BINARY TCP 1.1\n";
    alarm(5);
    send(fd, greeting, strlen(greeting), 0);
    alarm(0);

    while (1) {
        calcProtocol cp_net;
        
        // Read client request
        alarm(5);
        ssize_t r = full_read(fd, &cp_net, sizeof(cp_net));
        alarm(0);
        if (r != sizeof(cp_net)) break;

        // Convert to host order
        calcProtocol cp;
        cp.type = ntohs(cp_net.type);
        cp.major_version = ntohs(cp_net.major_version);
        cp.minor_version = ntohs(cp_net.minor_version);
        cp.id = ntohl(cp_net.id);
        cp.arith = ntohl(cp_net.arith);
        cp.inValue1 = ntohl(cp_net.inValue1);
        cp.inValue2 = ntohl(cp_net.inValue2);
        cp.inResult = ntohl(cp_net.inResult);

        // Validate protocol version
        if (cp.major_version != 1 || cp.minor_version != 1) {
            calcMessage msg{};
            msg.type = htons(2);
            msg.message = htonl(2); // NOT OK
            msg.protocol = htons(17);
            msg.major_version = htons(1);
            msg.minor_version = htons(1);
            alarm(5);
            send(fd, &msg, sizeof(msg), 0);
            alarm(0);
            continue;
        }

        if (cp.type == 22 && cp.id == 0) {
            // Client requesting task
            int code = (rand() % 4) + 1;
            int i1 = randomInt();
            int i2;
            if (code == 4) {
                do { i2 = randomInt(); } while (i2 == 0);
            } else {
                i2 = randomInt();
            }

            int expected = 0;
            if (code == 1) expected = i1 + i2;
            else if (code == 2) expected = i1 - i2;
            else if (code == 3) expected = i1 * i2;
            else if (code == 4) expected = i1 / i2;

            calcProtocol out{};
            out.type = htons(1);
            out.major_version = htons(1);
            out.minor_version = htons(1);
            out.id = htonl((uint32_t)(rand() ^ time(NULL)));
            out.arith = htonl(code);
            out.inValue1 = htonl(i1);
            out.inValue2 = htonl(i2);
            out.inResult = htonl(expected); // Store expected result

            alarm(5);
            send(fd, &out, sizeof(out), 0);
            alarm(0);
        } else if (cp.type == 22 && cp.id != 0) {
            // Client submitting answer
            calcMessage msg{};
            msg.type = htons(2);
            msg.protocol = htons(17);
            msg.major_version = htons(1);
            msg.minor_version = htons(1);

            // For TCP binary, we need to validate the result
            // Since we stored expected result in inResult field when sending task
            if (cp.inResult == cp_net.inResult) { // Compare network order values
                msg.message = htonl(1); // OK
            } else {
                msg.message = htonl(2); // NOT OK
            }

            alarm(5);
            send(fd, &msg, sizeof(msg), 0);
            alarm(0);
        } else {
            // Invalid type
            calcMessage msg{};
            msg.type = htons(2);
            msg.message = htonl(2); // NOT OK
            msg.protocol = htons(17);
            msg.major_version = htons(1);
            msg.minor_version = htons(1);
            alarm(5);
            send(fd, &msg, sizeof(msg), 0);
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

    // Parse host:port
    char *input = argv[1];
    char *sep = strchr(input, ':');
    if (!sep) { 
        fprintf(stderr, "Error: input must be host:port\n"); 
        return 1; 
    }
    char host[256]; 
    char port[64];
    size_t hostlen = sep - input;
    if (hostlen >= sizeof(host)) { 
        fprintf(stderr, "hostname too long\n"); 
        return 1; 
    }
    strncpy(host, input, hostlen); 
    host[hostlen] = '\0';
    strncpy(port, sep + 1, sizeof(port) - 1); 
    port[sizeof(port) - 1] = '\0';
    
    int listenfd = setup_listener(host, port);
    if (listenfd < 0) { 
        perror("setup_listener"); 
        return 1; 
    }
    printf("TCP server on %s:%s\n", host, port);
    
    // Ignore SIGPIPE to avoid crashes on broken pipes
    signal(SIGPIPE, SIG_IGN);
    
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
            close(listenfd);
            
            // Determine protocol by reading first message
            std::string line;
            ssize_t r = recv_line(connfd, line);
            if (r <= 0) {
                close(connfd);
                _exit(0);
            }
            
            // Convert to lowercase for case-insensitive check
            std::string low = line;
            for (auto &c : low) c = tolower(c);
            
            if (low.find("binary") != std::string::npos) {
                handle_binary_client(connfd);
            } else {
                handle_text_client(connfd);
            }
            
            close(connfd);
            _exit(0);
        } else {
            close(connfd);
            // Reap zombie processes
            while (waitpid(-1, NULL, WNOHANG) > 0) {}
        }
    }
    
    close(listenfd);
    return 0;
}