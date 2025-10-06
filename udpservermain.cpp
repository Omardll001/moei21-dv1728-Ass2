// Simplified, robust UDP server implementation for assignment.
// Handles both TEXT UDP 1.1 and binary (calcProtocol / calcMessage) version 1.1.
// Single socket (IPv4 or IPv6 depending on host argument) + select() loop, 10s task timeout.

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <cstring>
#include <csignal>
#include <algorithm>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include "protocol.h"
#include "calcLib.h"
#include "myGitdata.h"
#ifndef COMMIT_HASH
#define COMMIT_HASH "unknown"
#endif

using namespace std;
using Clock = chrono::steady_clock;

struct TaskInfo { uint32_t id; uint32_t arith; int32_t v1; int32_t v2; Clock::time_point ts; bool isText=false; };
struct ClientKey { sockaddr_storage addr{}; socklen_t len{}; bool operator==(ClientKey const& o) const noexcept { if(len!=o.len) return false; if(addr.ss_family!=o.addr.ss_family) return false; if(addr.ss_family==AF_INET){auto *a=(sockaddr_in*)&addr;auto *b=(sockaddr_in*)&o.addr; return a->sin_port==b->sin_port && a->sin_addr.s_addr==b->sin_addr.s_addr;} else {auto *a=(sockaddr_in6*)&addr;auto *b=(sockaddr_in6*)&o.addr; return a->sin6_port==b->sin6_port && memcmp(&a->sin6_addr,&b->sin6_addr,sizeof(in6_addr))==0;} } };
struct ClientHash { size_t operator()(ClientKey const& k) const noexcept { size_t h=0xcbf29ce484222325ULL; auto mix=[&](const void* d,size_t l){auto p=(const unsigned char*)d; for(size_t i=0;i<l;++i){h^=p[i]; h*=0x100000001b3ULL;}}; if(k.addr.ss_family==AF_INET){auto *a=(sockaddr_in*)&k.addr; mix(&a->sin_port,sizeof(a->sin_port)); mix(&a->sin_addr,sizeof(a->sin_addr));} else {auto *a=(sockaddr_in6*)&k.addr; mix(&a->sin6_port,sizeof(a->sin6_port)); mix(&a->sin6_addr,sizeof(a->sin6_addr));} return h; } };

static volatile bool g_run=true; void handle_sig(int){ g_run=false; }

static TaskInfo makeTask(uint32_t id){ TaskInfo t{}; t.id=id; int op=(randomInt()%4)+1; if(op==4){ do{ t.v2=randomInt()%100;}while(t.v2==0); t.v1=randomInt()%100; } else { t.v1=randomInt()%100; t.v2=randomInt()%100; } t.arith=op; t.ts=Clock::now(); return t; }
static int32_t eval(const TaskInfo&t){ switch(t.arith){case 1: return t.v1+t.v2; case 2: return t.v1-t.v2; case 3: return t.v1*t.v2; case 4: return t.v1/t.v2;} return 0; }
static const char* opname(uint32_t a){ switch(a){case 1:return "add"; case 2:return "sub"; case 3:return "mul"; case 4:return "div";} return "na"; }

static bool splitAddress(const string &arg,string &host,string &port){ auto p=arg.rfind(':'); if(p==string::npos) return false; host=arg.substr(0,p); port=arg.substr(p+1); return !host.empty() && !port.empty(); }

int main(int argc,char*argv[]){
    if(argc!=2){ cerr<<"Usage: "<<argv[0]<<" <host:port>\n"; return 1; }
    signal(SIGINT,handle_sig); initCalcLib();
    string host,port; if(!splitAddress(argv[1],host,port)){ cerr<<"Bad address format\n"; return 1; }
    bool specialLocalHost = (host=="localhost");
    if(host=="ip4-localhost") host="127.0.0.1"; else if(host=="ip6-localhost") host="::1";
    vector<int> socks;
    auto doBind=[&](const string &bindHost){
        struct addrinfo hints{}; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_DGRAM; hints.ai_flags=AI_PASSIVE; struct addrinfo *res=nullptr; 
        if(getaddrinfo(bindHost.c_str(),port.c_str(),&hints,&res)!=0) return; 
        for(auto *p=res;p;p=p->ai_next){
            int s = socket(p->ai_family,p->ai_socktype,p->ai_protocol); if(s<0) continue; int yes=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
            // Try dual-stack when IPv6 (set V6ONLY=0) so it can also accept v4-mapped
            if(p->ai_family==AF_INET6){ int off=0; setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,&off,sizeof(off)); }
            if(bind(s,p->ai_addr,p->ai_addrlen)==0){ socks.push_back(s);} else { close(s);} }
        freeaddrinfo(res);
    };
    if(specialLocalHost){
        doBind("127.0.0.1");
        doBind("::1");
    } else {
        doBind(host);
    }
    // Fallback: if 'localhost' given but nothing worked, try generic resolution once
    if(socks.empty() && specialLocalHost){ doBind("localhost"); }
    if(socks.empty()){ cerr<<"bind failed\n"; return 1; }
    cout<<"udpserver running on "<<host<<":"<<port<<" sockets="<<socks.size()<<"\n"; cout.flush();
    unordered_map<ClientKey,TaskInfo,ClientHash> tasks; uint32_t nextId=1;
    // Diagnostics counters
    size_t pkt_recv=0, pkt_binary=0, pkt_text=0, tasks_issued=0, answers_ok=0, answers_fail=0, resend_task=0;
    auto lastDiag = Clock::now();
    while(g_run){
        fd_set rfds; FD_ZERO(&rfds); int maxfd=0; for(int s: socks){ FD_SET(s,&rfds); if(s>maxfd) maxfd=s; }
        timeval tv{0,100000}; int sel=select(maxfd+1,&rfds,nullptr,nullptr,&tv); if(sel<0){ if(errno==EINTR) continue; perror("select"); break; }
        auto now=Clock::now(); for(auto it=tasks.begin(); it!=tasks.end();){ auto age=chrono::duration_cast<chrono::seconds>(now - it->second.ts).count(); if(age>10) it=tasks.erase(it); else ++it; }
        if(sel==0) continue;
        for(int s: socks){
            if(!FD_ISSET(s,&rfds)) continue;
            for(int drain=0; drain<1024; ++drain){
                sockaddr_storage caddr{}; socklen_t clen=sizeof(caddr); unsigned char buf[256];
                ssize_t n=recvfrom(s,buf,sizeof(buf),MSG_DONTWAIT,(sockaddr*)&caddr,&clen);
                if(n<0){
                    if(errno==EAGAIN||errno==EWOULDBLOCK) break; else { perror("recvfrom"); break; }
                }
                if(n<0) break;
                ++pkt_recv;
                ClientKey key; key.addr=caddr; key.len=clen;
                if((size_t)n == sizeof(calcProtocol)) { // exact match -> binary
                    ++pkt_binary; calcProtocol cp{}; memcpy(&cp,buf,sizeof(cp));
                    uint16_t /*type*/ maj=ntohs(cp.major_version); uint16_t min=ntohs(cp.minor_version);
                // Accept both legacy (1/2) and assignment provided (21/22) client type codes
                if(!(maj==1 && min==1)) continue;
                auto it=tasks.find(key);
                uint32_t idNet=ntohl(cp.id);
                int32_t inRes=ntohl(cp.inResult);
                // NEW logic:
                //  * A (new) request MUST have id==0 (ignore inResult which may be 0 for valid answers later)
                //  * An answer has id==task.id and may have inRes == 0 (e.g., sub where v1==v2, or div result 0)
                if(it==tasks.end()){
                    if(idNet==0){
                        TaskInfo t=makeTask(nextId++); t.isText=false; tasks[key]=t;
                        calcProtocol out{}; out.type=htons(1); // server->client
                        out.major_version=htons(1); out.minor_version=htons(1);
                        out.id=htonl(t.id); out.arith=htonl(t.arith);
                        out.inValue1=htonl(t.v1); out.inValue2=htonl(t.v2); out.inResult=0;
                        if(sendto(s,&out,sizeof(out),0,(sockaddr*)&caddr,clen)!=(ssize_t)sizeof(out)) perror("sendto-task"); else ++tasks_issued;
                    } else {
                        // Unknown task id from client without existing state -> reject
                        calcMessage msg{}; msg.type=htons(2); msg.message=htonl(2); msg.protocol=htons(17); msg.major_version=htons(1); msg.minor_version=htons(1);
                        if(sendto(s,&msg,sizeof(msg),0,(sockaddr*)&caddr,clen)!=(ssize_t)sizeof(msg)) perror("sendto-reject"); ++answers_fail;
                    }
                } else {
                    TaskInfo &t=it->second;
                    if(idNet==0){
                        // Client re-request (lost task?) -> resend original task
                        calcProtocol out{}; out.type=htons(1); out.major_version=htons(1); out.minor_version=htons(1);
                        out.id=htonl(t.id); out.arith=htonl(t.arith); out.inValue1=htonl(t.v1); out.inValue2=htonl(t.v2); out.inResult=0;
                        if(sendto(s,&out,sizeof(out),0,(sockaddr*)&caddr,clen)!=(ssize_t)sizeof(out)) perror("sendto-resend"); else ++resend_task;
                    } else if(idNet==t.id){
                        // Treat ANY inRes (including 0) as final answer
                        auto age=chrono::duration_cast<chrono::seconds>(now - t.ts).count();
                        int32_t real=eval(t);
                        calcMessage msg{}; msg.type=htons(2); bool ok=(age<=10)&&(inRes==real);
                        msg.message=htonl(ok?1:2); msg.protocol=htons(17); msg.major_version=htons(1); msg.minor_version=htons(1);
                        if(sendto(s,&msg,sizeof(msg),0,(sockaddr*)&caddr,clen)!=(ssize_t)sizeof(msg)) perror("sendto-answer");
                        if(ok) ++answers_ok; else ++answers_fail;
                        tasks.erase(it);
                    } else {
                        // Different id -> ignore or optionally send rejection (ignore silently to be tolerant)
                    }
                }
                } else { // treat as TEXT
                    ++pkt_text;
                    string txt((char*)buf,(size_t)n);
                    txt.erase(remove(txt.begin(),txt.end(),'\r'),txt.end());
                    txt.erase(remove(txt.begin(),txt.end(),'\n'),txt.end());
                    auto it=tasks.find(key);
                    if(txt=="TEXT UDP 1.1"){
                        TaskInfo t;
                        if(it==tasks.end()){ t=makeTask(nextId++); t.isText=true; tasks[key]=t; } else t=it->second;
                        string line=to_string(t.id)+" "+opname(t.arith)+" "+to_string(t.v1)+" "+to_string(t.v2)+"\n";
                        if(sendto(s,line.c_str(),line.size(),0,(sockaddr*)&caddr,clen)<0) perror("sendto-text-task"); else ++tasks_issued;
                    } else if(it!=tasks.end() && it->second.isText){
                        size_t sp=txt.find(' ');
                        if(sp!=string::npos){
                            bool parsed=true; uint32_t rid=0; long long ans=0;
                            try{ rid=stoul(txt.substr(0,sp)); ans=stoll(txt.substr(sp+1)); }catch(...){ parsed=false; }
                            if(parsed && rid==it->second.id){
                                auto age=chrono::duration_cast<chrono::seconds>(now - it->second.ts).count();
                                long long real=eval(it->second); bool ok=(age<=10)&&(ans==real);
                                string resp=(ok?"OK ":"NOT OK ")+string(COMMIT_HASH)+"\n";
                                if(sendto(s,resp.c_str(),resp.size(),0,(sockaddr*)&caddr,clen)<0) perror("sendto-text-answer");
                                if(ok) ++answers_ok; else ++answers_fail; tasks.erase(it);
                            } else if(parsed){
                                string resp=string("NOT OK ")+COMMIT_HASH+"\n";
                                if(sendto(s,resp.c_str(),resp.size(),0,(sockaddr*)&caddr,clen)<0) perror("sendto-text-reject");
                                ++answers_fail; tasks.erase(it);
                            }
                        }
                    }
                }
            }
        }
        auto now2 = Clock::now(); if(chrono::duration_cast<chrono::seconds>(now2 - lastDiag).count()>=1){
            cerr << "DIAG pkts="<<pkt_recv<<" bin="<<pkt_binary<<" txt="<<pkt_text<<" tasks="<<tasks_issued<<" resend="<<resend_task<<" ok="<<answers_ok<<" fail="<<answers_fail<<" outstanding="<<tasks.size()<<"\n"; lastDiag=now2;
        }
    }
    for(int s: socks) close(s); return 0;
}



