#include <iostream>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sys/time.h>
#include "protocol.h"

using namespace std;

int main() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    
    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5352);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    
    // First, send "TEXT UDP 1.1" like bulkUDPclient does
    string initial_msg = "TEXT UDP 1.1";
    cout << "Sending: " << initial_msg << endl;
    sendto(sock, initial_msg.c_str(), initial_msg.length(), 0, (struct sockaddr*)&addr, sizeof(addr));
    
    // Wait for response
    char response[256];
    ssize_t recv_len = recvfrom(sock, response, sizeof(response)-1, 0, nullptr, nullptr);
    if (recv_len > 0) {
        response[recv_len] = '\0';
        cout << "Server response: " << response << endl;
        
        // Parse the task
        uint32_t task_id;
        char op[16];
        int32_t val1, val2;
        if (sscanf(response, "%u %s %d %d", &task_id, op, &val1, &val2) == 4) {
            cout << "Parsed task: ID=" << task_id << " op=" << op << " val1=" << val1 << " val2=" << val2 << endl;
            
            // Now switch to binary protocol like bulkUDPclient does
            calcProtocol binary_msg = {0};
            binary_msg.type = htons(22);
            binary_msg.major_version = htons(1);
            binary_msg.minor_version = htons(1);
            binary_msg.id = htonl(task_id);
            
            // Convert operation
            uint32_t arith = 1; // default add
            if (strcmp(op, "add") == 0) arith = 1;
            else if (strcmp(op, "sub") == 0) arith = 2;
            else if (strcmp(op, "mul") == 0) arith = 3;
            else if (strcmp(op, "div") == 0) arith = 4;
            
            binary_msg.arith = htonl(arith);
            binary_msg.inValue1 = htonl(val1);
            binary_msg.inValue2 = htonl(val2);
            
            // Calculate result
            int32_t result = 0;
            if (arith == 1) result = val1 + val2;
            else if (arith == 2) result = val1 - val2;
            else if (arith == 3) result = val1 * val2;
            else if (arith == 4) result = val1 / val2;
            
            binary_msg.inResult = htonl(result);
            
            cout << "Sending binary response with result: " << result << endl;
            sendto(sock, &binary_msg, sizeof(binary_msg), 0, (struct sockaddr*)&addr, sizeof(addr));
            
            // Wait for final response (calcMessage)
            calcMessage final_msg;
            ssize_t final_len = recvfrom(sock, &final_msg, sizeof(final_msg), 0, nullptr, nullptr);
            if (final_len > 0) {
                cout << "Received final calcMessage - size: " << final_len << endl;
                cout << "type: " << ntohs(final_msg.type) << endl;
                cout << "message: " << ntohl(final_msg.message) << endl;
                cout << "protocol: " << ntohs(final_msg.protocol) << endl;
                cout << "version: " << ntohs(final_msg.major_version) << "." << ntohs(final_msg.minor_version) << endl;
            } else {
                cout << "Timeout waiting for final response!" << endl;
            }
        }
    } else {
        cout << "No response received from server" << endl;
    }
    
    close(sock);
    return 0;
}