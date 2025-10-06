// tcpServer.cpp
// Usage: tcpServer host:port
// Fork per connection. Supports TEXT TCP 1.1 and BINARY TCP 1.1.
// Per-operation timeout 5s -> on timeout send "ERROR TO\n" and exit child.

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h> 
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
#include <algorithm>
#include <math.h> 
#include <poll.h>
#include <vector>
#include <string>
#include <ctype.h>


#include "protocol.h"
extern "C" {
#include "calcLib.h"
}

using namespace std;

static int conn_fd_for_alarm = -1;

// Function declarations
void handle_tcp_client(int fd);
void handle_text_protocol(int fd);
void handle_binary_protocol(int fd);

void alarm_handler(int) {
    if (conn_fd_for_alarm != -1) {
        const char *msg = "ERROR TO\n";
        write(conn_fd_for_alarm, msg, strlen(msg));
        close(conn_fd_for_alarm);
    }
    _exit(1);
}

ssize_t full_read(int fd, void *buf, size_t count) {
    size_t done = 0;
    char *p = (char*)buf;
    while (done < count) {
        ssize_t r = read(fd, p + done, count - done);
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
        ssize_t r = read(fd, &c, 1);
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
    
    // Handle special test hostnames
    const char *actual_host = host;
    if (strcmp(host, "ip4-localhost") == 0) {
        actual_host = "127.0.0.1";
        hints.ai_family = AF_INET;  // Force IPv4
    } else if (strcmp(host, "ip6-localhost") == 0) {
        actual_host = "::1";
        hints.ai_family = AF_INET6; // Force IPv6
    }
    
    if (getaddrinfo(actual_host, port, &hints, &res) != 0) return -1;
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



void handle_tcp_client(int fd) {
    conn_fd_for_alarm = fd;
    signal(SIGALRM, alarm_handler);

    // Send list of supported protocols
    const char *protocols = "TEXT TCP 1.1\nBINARY TCP 1.1\n\n";
    alarm(5);
    ssize_t sent = write(fd, protocols, strlen(protocols));
    alarm(0);

    if (sent != (ssize_t)strlen(protocols)) {
        close(fd);
        return;
    }

    // Wait for client protocol selection
    std::string client_response;
    alarm(5);
    ssize_t r = recv_line(fd, client_response);
    alarm(0);
    if (r <= 0) {
        const char *err = "ERROR TO\n";
        write(fd, err, strlen(err));
        close(fd);
        return;
    }

    // Trim whitespace
    while (!client_response.empty() && (client_response.back() == '\n' || client_response.back() == '\r'))
        client_response.pop_back();

    // Check if client selected binary or text protocol
    std::string lower = client_response;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("binary tcp 1.1 ok") != std::string::npos) {
        handle_binary_protocol(fd);
    } else if (lower.find("text tcp 1.1 ok") != std::string::npos) {
        handle_text_protocol(fd);
    } else {
        // Unsupported protocol
        const char *err = "ERROR: MISSMATCH PROTOCOL\n";
        write(fd, err, strlen(err));
        close(fd);
    }
}

void handle_text_protocol(int fd) {
    // Generate and send assignment
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
    int task_len = snprintf(task, sizeof(task), "ASSIGNMENT: %s %d %d\n", opstr, a, b);

    alarm(5);
    ssize_t sent = write(fd, task, task_len);
    alarm(0);
    if (sent != task_len) {
        close(fd);
        return;
    }

    // Wait for answer
    std::string line;
    alarm(5);
    ssize_t r = recv_line(fd, line);
    alarm(0);
    if (r <= 0) {
        const char *err = "ERROR TO\n";
        write(fd, err, strlen(err));
        close(fd);
        return;
    }

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
    line.erase(std::remove_if(line.begin(), line.end(), ::isspace), line.end());

    bool ok = false;
    int answer_int = 0;
    double answer_double = 0.0;

    // Try integer first
    if (sscanf(line.c_str(), "%d", &answer_int) == 1) {
        if (answer_int == expected) ok = true;
    } else if (sscanf(line.c_str(), "%lf", &answer_double) == 1) {
        if (fabs(answer_double - expected) < 0.0001) ok = true;
    }

    if (ok) {
        char result[64];
        snprintf(result, sizeof(result), "OK (myresult=%d)\n", answer_int);
        alarm(5);
        write(fd, result, strlen(result));
        alarm(0);
    } else {
        alarm(5);
        write(fd, "ERROR\n", 6);
        alarm(0);
    }
    close(fd);
}

void handle_binary_protocol(int fd) {
    // Generate task
    int code = (rand() % 4) + 1;
    int i1 = randomInt();
    int i2;
    if (code == 4) {
        do { i2 = randomInt(); } while (i2 == 0);
    } else {
        i2 = randomInt();
    }

    int32_t expected = 0;
    if (code == 1) expected = i1 + i2;
    else if (code == 2) expected = i1 - i2;
    else if (code == 3) expected = i1 * i2;
    else if (code == 4) expected = i1 / i2;

    uint32_t task_id = (uint32_t)(rand() ^ time(NULL));

    // For TCP Binary, send calcProtocol message directly (no text assignment line)
    calcProtocol cp{};
    cp.type = htons(1);  // server to client
    cp.major_version = htons(1);
    cp.minor_version = htons(1);
    cp.id = htonl(task_id);
    cp.arith = htonl(code);
    cp.inValue1 = htonl(i1);
    cp.inValue2 = htonl(i2);
    cp.inResult = htonl(0);

    alarm(5);
    ssize_t sent = write(fd, &cp, sizeof(cp));
    alarm(0);
    if (sent != sizeof(cp)) {
        close(fd);
        return;
    }

    // Wait for client response
    calcProtocol response;
    alarm(5);
    ssize_t r = full_read(fd, &response, sizeof(response));
    alarm(0);
    if (r != sizeof(response)) {
        const char *err = "ERROR TO\n";
        write(fd, err, strlen(err));
        close(fd);
        return;
    }

    // Convert to host order
    uint16_t resp_type = ntohs(response.type);
    uint32_t resp_id = ntohl(response.id);
    int32_t resp_result = ntohl(response.inResult);

    // Send response
    calcMessage msg{};
    msg.type = htons(2);  // server to client
    msg.protocol = htons(6);  // TCP
    msg.major_version = htons(1);
    msg.minor_version = htons(1);

    if (resp_type == 2 && resp_id == task_id && resp_result == expected) {
        msg.message = htonl(1);  // OK
        alarm(5);
        write(fd, &msg, sizeof(msg));
        alarm(0);

        // Send human-readable OK line for compatibility
        char okline[64];
        snprintf(okline, sizeof(okline), "OK (myresult=%d)\n", resp_result);
        alarm(5);
        write(fd, okline, strlen(okline));
        alarm(0);
    } else {
        msg.message = htonl(2);  // NOT OK
        alarm(5);
        write(fd, &msg, sizeof(msg));
        alarm(0);

        char nokline[64];
        snprintf(nokline, sizeof(nokline), "ERROR\n");
        alarm(5);
        write(fd, nokline, strlen(nokline));
        alarm(0);
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

    // Parse port after colon, digits only
    char *port_start = sep + 1;
    char *port_end = port_start;
    while (*port_end && isdigit(*port_end)) port_end++;
    size_t portlen = port_end - port_start;
    if (portlen == 0 || portlen >= sizeof(port)) {
        fprintf(stderr, "Invalid port\n");
        return 1;
    }
    strncpy(port, port_start, portlen);
    port[portlen] = '\0';

    int listenfd = setup_listener(host, port);
    if (listenfd < 0) { 
        perror("setup_listener"); 
        return 1; 
    }
    fprintf(stderr, "TCP server on %s:%s\n", host, port);

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
            
            // TCP protocol: Server sends list of supported protocols first
            handle_tcp_client(connfd);
            _exit(0);
        } else {
            close(connfd);
            while (waitpid(-1, NULL, WNOHANG) > 0) {}
        }
    }
    close(listenfd);
    return 0;
}