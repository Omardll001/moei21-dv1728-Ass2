// This should be a C++ client that implements the protocol from Assignment 1
// Usage: ./refClient tcp://host:port/protocol

#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <algorithm>
#include <cctype>
#include "protocol.h"

using namespace std;

void parse_url(const string& url, string& protocol, string& host, int& port, string& path) {
    // Parse URL like tcp://host:port/path
    size_t proto_end = url.find("://");
    if (proto_end == string::npos) throw runtime_error("Invalid URL format");
    
    protocol = url.substr(0, proto_end);
    transform(protocol.begin(), protocol.end(), protocol.begin(), ::tolower);
    
    size_t host_start = proto_end + 3;
    size_t path_start = url.find('/', host_start);
    if (path_start == string::npos) throw runtime_error("No path in URL");
    
    string host_port = url.substr(host_start, path_start - host_start);
    path = url.substr(path_start + 1);
    transform(path.begin(), path.end(), path.begin(), ::tolower);
    
    size_t port_start = host_port.find(':');
    if (port_start == string::npos) throw runtime_error("No port in URL");
    
    host = host_port.substr(0, port_start);
    port = stoi(host_port.substr(port_start + 1));
}

int connect_tcp(const string& host, int port) {
    struct addrinfo hints{}, *res;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    // Handle special test hostnames
    string actual_host = host;
    if (host == "ip4-localhost") {
        actual_host = "127.0.0.1";
        hints.ai_family = AF_INET;
    } else if (host == "ip6-localhost") {
        actual_host = "::1";
        hints.ai_family = AF_INET6;
    }
    
    string port_str = to_string(port);
    if (getaddrinfo(actual_host.c_str(), port_str.c_str(), &hints, &res) != 0) {
        throw runtime_error("getaddrinfo failed");
    }
    
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        throw runtime_error("socket failed");
    }
    
    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        close(sock);
        freeaddrinfo(res);
        throw runtime_error("connect failed");
    }
    
    freeaddrinfo(res);
    return sock;
}

string recv_line(int sock) {
    string line;
    char c;
    while (recv(sock, &c, 1, 0) > 0) {
        line += c;
        if (c == '\n') break;
    }
    return line;
}

ssize_t recv_full(int sock, void* buf, size_t len) {
    size_t received = 0;
    char* ptr = (char*)buf;
    while (received < len) {
        ssize_t r = recv(sock, ptr + received, len - received, 0);
        if (r <= 0) return r;
        received += r;
    }
    return received;
}

void handle_tcp_binary(int sock, const string& host, int port) {
    cout << "Protocol: tcp, Host " << host << ", port = " << port << " and path = binary." << endl;
    
    // Read protocols from server
    string protocols;
    string line;
    while (!(line = recv_line(sock)).empty()) {
        protocols += line;
        if (line == "\n") break;
    }
    
    // Send protocol selection
    string selection = "BINARY TCP 1.1 OK\n";
    send(sock, selection.c_str(), selection.length(), 0);
    
    // Receive calcProtocol message
    calcProtocol cp;
    if (recv_full(sock, &cp, sizeof(cp)) != sizeof(cp)) {
        throw runtime_error("Failed to receive calcProtocol");
    }
    
    // Convert from network byte order
    uint16_t type = ntohs(cp.type);
    uint32_t id = ntohl(cp.id);
    uint32_t arith = ntohl(cp.arith);
    int32_t val1 = ntohl(cp.inValue1);
    int32_t val2 = ntohl(cp.inValue2);
    
    // Print assignment
    string op_name = (arith == 1) ? "add" : (arith == 2) ? "sub" : (arith == 3) ? "mul" : "div";
    cout << "ASSIGNMENT: " << op_name << " " << val1 << " " << val2 << endl;
    
    // Calculate result
    int32_t result;
    if (arith == 1) result = val1 + val2;
    else if (arith == 2) result = val1 - val2;
    else if (arith == 3) result = val1 * val2;
    else if (arith == 4) result = val1 / val2;
    else throw runtime_error("Unknown operation");
    
    // Send response
    calcProtocol response{};
    response.type = htons(2);  // client to server
    response.major_version = htons(1);
    response.minor_version = htons(1);
    response.id = htonl(id);
    response.arith = htonl(arith);
    response.inValue1 = htonl(val1);
    response.inValue2 = htonl(val2);
    response.inResult = htonl(result);
    
    send(sock, &response, sizeof(response), 0);
    
    // Receive server response (calcMessage + text)
    calcMessage msg;
    recv_full(sock, &msg, sizeof(msg));
    
    // Read text response
    string text_response = recv_line(sock);
    while (!text_response.empty() && (text_response.back() == '\n' || text_response.back() == '\r')) {
        text_response.pop_back();
    }
    
    cout << text_response << endl;
}

void handle_tcp_text(int sock, const string& host, int port) {
    cout << "Protocol: tcp, Host " << host << ", port = " << port << " and path = text." << endl;
    
    // Read protocols from server
    string protocols;
    string line;
    while (!(line = recv_line(sock)).empty()) {
        protocols += line;
        if (line == "\n") break;
    }
    
    // Send protocol selection
    string selection = "TEXT TCP 1.1 OK\n";
    send(sock, selection.c_str(), selection.length(), 0);
    
    // Receive assignment
    string assignment = recv_line(sock);
    while (!assignment.empty() && (assignment.back() == '\n' || assignment.back() == '\r')) {
        assignment.pop_back();
    }
    cout << assignment << endl;
    
    // Parse assignment
    char op_name_buf[32];
    int val1, val2;
    if (sscanf(assignment.c_str(), "ASSIGNMENT: %s %d %d", op_name_buf, &val1, &val2) != 3) {
        throw runtime_error("Failed to parse assignment");
    }
    string op_name = op_name_buf;
    
    // Calculate result
    int result;
    if (op_name == "add") result = val1 + val2;
    else if (op_name == "sub") result = val1 - val2;
    else if (op_name == "mul") result = val1 * val2;
    else if (op_name == "div") result = val1 / val2;
    else throw runtime_error("Unknown operation: " + op_name);
    
    // Send result
    string result_str = to_string(result) + "\n";
    send(sock, result_str.c_str(), result_str.length(), 0);
    
    // Receive server response
    string response = recv_line(sock);
    while (!response.empty() && (response.back() == '\n' || response.back() == '\r')) {
        response.pop_back();
    }
    cout << response << endl;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " protocol://host:port/path" << endl;
        return 1;
    }
    
    try {
        string protocol, host, path;
        int port;
        parse_url(argv[1], protocol, host, port, path);
        
        if (protocol != "tcp") {
            throw runtime_error("Only TCP protocol supported");
        }
        
        int sock = connect_tcp(host, port);
        
        if (path == "binary") {
            handle_tcp_binary(sock, host, port);
        } else if (path == "text") {
            handle_tcp_text(sock, host, port);
        } else {
            throw runtime_error("Unknown path: " + path);
        }
        
        close(sock);
        
    } catch (const exception& e) {
        cerr << "ERROR: " << e.what() << endl;
        return 1;
    }
    
    return 0;
}