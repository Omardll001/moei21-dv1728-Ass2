// Bulk UDP Client for testing UDP server with multiple clients
// Usage: ./bulkUDPclient host:port num_tests drop_prob logfile

#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <thread>
#include <vector>
#include <fstream>
#include <random>
#include <chrono>
#include <atomic>
#include <mutex>
#include "protocol.h"

using namespace std;

struct TestResult {
    int client_id;
    bool success;
    string error_msg;
    int task_type;  // 1=text, 2=binary
};

atomic<int> completed_tests(0);
atomic<int> successful_tests(0);
atomic<int> failed_tests(0);
mutex results_mutex;
vector<TestResult> all_results;

void parse_address(const string& addr, string& host, int& port) {
    size_t colon = addr.find(':');
    if (colon == string::npos) throw runtime_error("Invalid address format");
    
    host = addr.substr(0, colon);
    port = stoi(addr.substr(colon + 1));
    
    // Handle special test hostnames
    if (host == "ip4-localhost") {
        host = "127.0.0.1";
    } else if (host == "ip6-localhost") {
        host = "::1";
    }
}

int create_udp_socket(const string& host, int port, struct sockaddr_storage& addr, socklen_t& addr_len) {
    struct addrinfo hints{}, *res;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    
    string port_str = to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0) {
        throw runtime_error("getaddrinfo failed");
    }
    
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        throw runtime_error("socket failed");
    }
    
    memcpy(&addr, res->ai_addr, res->ai_addrlen);
    addr_len = res->ai_addrlen;
    
    freeaddrinfo(res);
    return sock;
}

void run_text_client(int client_id, const string& host, int port, int drop_prob) {
    TestResult result;
    result.client_id = client_id;
    result.success = false;
    result.task_type = 1;
    
    try {
        struct sockaddr_storage server_addr;
        socklen_t server_len;
        int sock = create_udp_socket(host, port, server_addr, server_len);
        
        // Send initial text request
        string request = "TEXT UDP 1.1\n";
        
        // Simulate message dropping
        random_device rd;
        mt19937 gen(rd());
        uniform_int_distribution<> dis(1, 100);
        if (dis(gen) <= drop_prob) {
            // Drop this message - simulate network loss
            close(sock);
            result.error_msg = "Simulated message drop";
            lock_guard<mutex> lock(results_mutex);
            all_results.push_back(result);
            failed_tests++;
            completed_tests++;
            return;
        }
        
        sendto(sock, request.c_str(), request.length(), 0, 
               (struct sockaddr*)&server_addr, server_len);
        
        // Receive task
        char buf[1024];
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
        if (n <= 0) {
            result.error_msg = "No response from server";
        } else {
            buf[n] = '\0';
            string response(buf);
            
            // Parse task: "id op val1 val2"
            uint32_t task_id;
            string op;
            int val1, val2;
            istringstream iss(response);
            if (iss >> task_id >> op >> val1 >> val2) {
                // Calculate result
                int result_val;
                if (op == "add") result_val = val1 + val2;
                else if (op == "sub") result_val = val1 - val2;
                else if (op == "mul") result_val = val1 * val2;
                else if (op == "div") result_val = val1 / val2;
                else throw runtime_error("Unknown operation");
                
                // Send response
                string response_str = to_string(task_id) + " " + to_string(result_val) + "\n";
                sendto(sock, response_str.c_str(), response_str.length(), 0,
                       (struct sockaddr*)&server_addr, server_len);
                
                // Receive confirmation  
                n = recvfrom(sock, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
                if (n > 0) {
                    buf[n] = '\0';
                    if (strstr(buf, "OK")) {
                        result.success = true;
                    } else {
                        result.error_msg = "Server rejected answer";
                    }
                } else {
                    result.error_msg = "No confirmation from server";
                }
            } else {
                result.error_msg = "Failed to parse task";
            }
        }
        
        close(sock);
        
    } catch (const exception& e) {
        result.error_msg = e.what();
    }
    
    lock_guard<mutex> lock(results_mutex);
    all_results.push_back(result);
    if (result.success) {
        successful_tests++;
    } else {
        failed_tests++;
    }
    completed_tests++;
}

void run_binary_client(int client_id, const string& host, int port, int drop_prob) {
    TestResult result;
    result.client_id = client_id;
    result.success = false;
    result.task_type = 2;
    
    try {
        struct sockaddr_storage server_addr;
        socklen_t server_len;
        int sock = create_udp_socket(host, port, server_addr, server_len);
        
        // Simulate message dropping
        random_device rd;
        mt19937 gen(rd());
        uniform_int_distribution<> dis(1, 100);
        if (dis(gen) <= drop_prob) {
            // Drop this message - simulate network loss
            close(sock);
            result.error_msg = "Simulated message drop";
            lock_guard<mutex> lock(results_mutex);
            all_results.push_back(result);
            failed_tests++;
            completed_tests++;
            return;
        }
        
        // Send binary protocol request
        calcProtocol cp{};
        cp.type = htons(22);  // client to server
        cp.major_version = htons(1);
        cp.minor_version = htons(1);
        cp.id = htonl(0);  // Request new task
        cp.arith = htonl(0);
        cp.inValue1 = htonl(0);
        cp.inValue2 = htonl(0);
        cp.inResult = htonl(0);
        
        sendto(sock, &cp, sizeof(cp), 0, (struct sockaddr*)&server_addr, server_len);
        
        // Receive task
        calcProtocol task;
        ssize_t n = recvfrom(sock, &task, sizeof(task), 0, nullptr, nullptr);
        if (n == sizeof(task)) {
            // Convert from network order
            uint32_t id = ntohl(task.id);
            uint32_t arith = ntohl(task.arith);
            int32_t val1 = ntohl(task.inValue1);
            int32_t val2 = ntohl(task.inValue2);
            
            // Calculate result
            int32_t result_val;
            if (arith == 1) result_val = val1 + val2;
            else if (arith == 2) result_val = val1 - val2;
            else if (arith == 3) result_val = val1 * val2;
            else if (arith == 4) result_val = val1 / val2;
            else throw runtime_error("Unknown operation");
            
            // Send response
            calcProtocol response{};
            response.type = htons(22);
            response.major_version = htons(1);
            response.minor_version = htons(1);
            response.id = htonl(id);
            response.arith = htonl(arith);
            response.inValue1 = htonl(val1);
            response.inValue2 = htonl(val2);
            response.inResult = htonl(result_val);
            
            sendto(sock, &response, sizeof(response), 0, 
                   (struct sockaddr*)&server_addr, server_len);
            
            // Receive confirmation
            calcMessage msg;
            n = recvfrom(sock, &msg, sizeof(msg), 0, nullptr, nullptr);
            if (n == sizeof(msg)) {
                uint32_t message = ntohl(msg.message);
                if (message == 1) {
                    result.success = true;
                } else {
                    result.error_msg = "Server rejected answer";
                }
            } else {
                result.error_msg = "No confirmation from server";
            }
        } else {
            result.error_msg = "Failed to receive task";
        }
        
        close(sock);
        
    } catch (const exception& e) {
        result.error_msg = e.what();
    }
    
    lock_guard<mutex> lock(results_mutex);
    all_results.push_back(result);
    if (result.success) {
        successful_tests++;
    } else {
        failed_tests++;
    }
    completed_tests++;
}

int main(int argc, char* argv[]) {
    if (argc != 5) {
        cerr << "Usage: " << argv[0] << " host:port num_tests drop_prob logfile" << endl;
        return 1;
    }
    
    try {
        string host;
        int port;
        parse_address(argv[1], host, port);
        
        int num_tests = stoi(argv[2]);
        int drop_prob = stoi(argv[3]);
        string logfile = argv[4];
        
        cout << "Starting " << num_tests << " concurrent UDP tests..." << endl;
        cout << "Target: " << host << ":" << port << endl;
        cout << "Drop probability: " << drop_prob << "%" << endl;
        
        vector<thread> threads;
        
        // Launch concurrent clients (mix of text and binary)
        for (int i = 0; i < num_tests; i++) {
            if (i % 2 == 0) {
                threads.emplace_back(run_binary_client, i, host, port, drop_prob);
            } else {
                threads.emplace_back(run_text_client, i, host, port, drop_prob);
            }
            
            // Small delay to avoid overwhelming the server
            this_thread::sleep_for(chrono::milliseconds(10));
        }
        
        // Wait for all threads to complete
        for (auto& t : threads) {
            t.join();
        }
        
        // Write results to log file
        ofstream log(logfile);
        if (log.is_open()) {
            log << "Completed: " << completed_tests << endl;
            log << "Successful: " << successful_tests << endl;
            log << "Failed: " << failed_tests << endl;
            log.close();
        }
        
        // Print summary to stdout
        cout << "Test Results:" << endl;
        cout << "Total: " << completed_tests << endl;
        cout << "Successful: " << successful_tests << endl;
        cout << "Failed: " << failed_tests << endl;
        cout << "Success rate: " << (100.0 * successful_tests / completed_tests) << "%" << endl;
        
        if (successful_tests > (num_tests * 0.65)) {  // At least 65% success rate (accounting for 30% drop + network conditions)
            cout << "SUMMARY: PASSED!" << endl;
        } else {
            cout << "SUMMARY: FAILED!" << endl;
        }
        
    } catch (const exception& e) {
        cerr << "ERROR: " << e.what() << endl;
        return 1;
    }
    
    return 0;
}