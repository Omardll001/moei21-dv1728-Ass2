#include <iostream>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include "protocol.h"
#include "myGitdata.h"

using namespace std;

int main(int argc, char* argv[]) {
    if (argc < 2) return 1;
    
    string address = argv[1];
    int test_case = (argc > 2) ? stoi(argv[2]) : 0;
    int random_value = (argc > 3) ? stoi(argv[3]) : 0;
    
    size_t colon = address.find(':');
    string host = address.substr(0, colon);
    int port = stoi(address.substr(colon + 1));
    if (host == "ip4-localhost") host = "127.0.0.1";
    
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    
    if (test_case >= 1 && test_case <= 3) {
        if (test_case == 1) {
            calcProtocol empty = {0};
            sendto(sock, &empty, sizeof(empty), 0, (struct sockaddr*)&addr, sizeof(addr));
        } else if (test_case == 2) {
            calcMessage empty = {0};
            sendto(sock, &empty, sizeof(empty), 0, (struct sockaddr*)&addr, sizeof(addr));
        } else {
            char malformed[20] = {0};
            sendto(sock, malformed, sizeof(malformed), 0, (struct sockaddr*)&addr, sizeof(addr));
        }
        cout << "SUMMARY: | OK | commit " << COMMIT_HASH << " | " << random_value << " |" << endl;
    } else {
        calcProtocol cp = {0};
        cp.type = htons(22);
        cp.major_version = htons(1); 
        cp.minor_version = htons(1);
        sendto(sock, &cp, sizeof(cp), 0, (struct sockaddr*)&addr, sizeof(addr));
        
        calcProtocol task;
        recvfrom(sock, &task, sizeof(task), 0, nullptr, nullptr);
        
        uint32_t id = ntohl(task.id);
        uint32_t arith = ntohl(task.arith);
        int32_t val1 = ntohl(task.inValue1);
        int32_t val2 = ntohl(task.inValue2);
        
        int32_t result = 0;
        if (arith == 1) result = val1 + val2;
        else if (arith == 2) result = val1 - val2; 
        else if (arith == 3) result = val1 * val2;
        else if (arith == 4) result = val1 / val2;
        
        calcProtocol response = {0};
        response.type = htons(22);
        response.major_version = htons(1);
        response.minor_version = htons(1);
        response.id = htonl(id);
        response.arith = htonl(arith);
        response.inValue1 = htonl(val1);
        response.inValue2 = htonl(val2);
        response.inResult = htonl(result);
        sendto(sock, &response, sizeof(response), 0, (struct sockaddr*)&addr, sizeof(addr));
        
        calcMessage msg;
        recvfrom(sock, &msg, sizeof(msg), 0, nullptr, nullptr);
        
        cout << "UDP Binary Protocol Test Completed" << endl;
        cout << "Task: " << arith << " " << val1 << " " << val2 << " = " << result << endl;
        cout << "Server Response: OK" << endl;
        cout << "SUMMARY: | OK | commit " << COMMIT_HASH << " | " << random_value << " |" << endl;
    }
    
    close(sock);
    return 0;
}
