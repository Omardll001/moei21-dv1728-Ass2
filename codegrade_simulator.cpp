// Test client to simulate codegrade behavior
// Sends text protocol negotiation but expects binary responses

#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "protocol.h"

using namespace std;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " host:port" << endl;
        return 1;
    }
    
    // Parse host:port
    string addr = argv[1];
    size_t colon = addr.find(':');
    string host = addr.substr(0, colon);
    int port = stoi(addr.substr(colon + 1));
    
    if (host == "ip4-localhost") host = "127.0.0.1";
    
    try {
        // Create UDP socket
        struct addrinfo hints{}, *res;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        
        if (getaddrinfo(host.c_str(), to_string(port).c_str(), &hints, &res) != 0) {
            throw runtime_error("getaddrinfo failed");
        }
        
        int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock < 0) {
            freeaddrinfo(res);
            throw runtime_error("socket failed");
        }
        
        struct sockaddr_storage server_addr;
        socklen_t server_len = res->ai_addrlen;
        memcpy(&server_addr, res->ai_addr, res->ai_addrlen);
        freeaddrinfo(res);
        
        cout << "Simulating codegrade ptu behavior:" << endl;
        
        // Step 1: Send text protocol negotiation (12 bytes)
        string text_msg = "TEXT UDP 1.1";
        cout << "Client registered, sent " << text_msg.length() << " bytes" << endl;
        sendto(sock, text_msg.c_str(), text_msg.length(), 0, 
               (struct sockaddr*)&server_addr, server_len);
        
        // Step 2: Expect binary protocol response (26 bytes = sizeof(calcProtocol))
        calcProtocol task;
        ssize_t n = recvfrom(sock, &task, sizeof(task), 0, nullptr, nullptr);
        
        if (n == sizeof(task)) {
            cout << "Got " << n << " bytes, expected " << sizeof(task) << " bytes (sizeof(cProtocol))" << endl;
            
            // Convert from network order
            uint32_t id = ntohl(task.id);
            uint32_t arith = ntohl(task.arith);
            int32_t val1 = ntohl(task.inValue1);
            int32_t val2 = ntohl(task.inValue2);
            
            cout << "Task: " << arith << " " << val1 << " " << val2 << endl;
            
            // Calculate result
            int32_t result;
            if (arith == 1) result = val1 + val2;
            else if (arith == 2) result = val1 - val2;
            else if (arith == 3) result = val1 * val2;
            else if (arith == 4) result = val1 / val2;
            else {
                cout << " ** SHIT unkown arithm. " << arith << " ** " << endl;
                cout << "SUMMARY| ERROR There was atleast ONE error detected |" << endl;
                close(sock);
                return 1;
            }
            
            // Step 3: Send binary response
            calcProtocol response{};
            response.type = htons(22);
            response.major_version = htons(1);
            response.minor_version = htons(1);
            response.id = htonl(id);
            response.arith = htonl(arith);
            response.inValue1 = htonl(val1);
            response.inValue2 = htonl(val2);
            response.inResult = htonl(result);
            
            cout << "(" << host << ":" << port << ") sent " << sizeof(response) << " bytes" << endl;
            sendto(sock, &response, sizeof(response), 0,
                   (struct sockaddr*)&server_addr, server_len);
            
            // Step 4: Expect confirmation
            calcMessage msg;
            n = recvfrom(sock, &msg, sizeof(msg), 0, nullptr, nullptr);
            if (n == sizeof(msg)) {
                uint32_t message = ntohl(msg.message);
                if (message == 1) {
                    cout << "SUMMARY| OK | commit b8372f33efd3b07c23bffd6740997d43387551d2" << endl;
                } else {
                    cout << "Unknown msg = " << message << " ** SHIT unkown arithm. " << arith << " ** " << endl;
                    cout << "SUMMARY| ERROR There was atleast ONE error detected |" << endl;
                }
            } else {
                cout << "No confirmation received" << endl;
                cout << "SUMMARY| ERROR There was atleast ONE error detected |" << endl;
            }
            
        } else {
            cout << "| ODD SIZE MESSAGE. Got " << n << " bytes, expected " << sizeof(task) << " bytes (sizeof(cProtocol)) . " << endl;
            cout << "ERROR WRONG SIZE OR INCORRECT PROTOCOL" << endl;
            cout << "SUMMARY| ERROR There was atleast ONE error detected |" << endl;
        }
        
        close(sock);
        
    } catch (const exception& e) {
        cerr << "ERROR: " << e.what() << endl;
        return 1;
    }
    
    return 0;
}