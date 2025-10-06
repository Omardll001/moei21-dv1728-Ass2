#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/select.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <random>
#include <chrono>
#include <mutex>
#include <fstream>
#include "protocol.h"

using namespace std; using Clock=chrono::steady_clock;

struct WorkerCfg { string host; string port; int dropProb; int id; int ops; atomic<int>*answered; atomic<int>*ok; };

static int32_t evalOp(uint32_t ar, int32_t a,int32_t b){ switch(ar){case 1: return a+b; case 2: return a-b; case 3: return a*b; case 4: return (b? a/b:0); default: return 0;} }

void worker(WorkerCfg cfg){
    std::mt19937 rng((unsigned)time(nullptr) ^ (cfg.id*7919)); std::uniform_int_distribution<int> drop(0,99);
    struct addrinfo hints{}; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_DGRAM; struct addrinfo*res=nullptr; if(getaddrinfo(cfg.host.c_str(),cfg.port.c_str(),&hints,&res)!=0) return; int s=-1; sockaddr_storage srv{}; socklen_t slen=0; for(auto*p=res;p;p=p->ai_next){ s=socket(p->ai_family,p->ai_socktype,p->ai_protocol); if(s<0) continue; memcpy(&srv,p->ai_addr,p->ai_addrlen); slen=p->ai_addrlen; break; } freeaddrinfo(res); if(s<0) return; 
    // handshake
    calcMessage hm{}; hm.type=htons(22); hm.message=htonl(1); hm.protocol=htons(17); hm.major_version=htons(1); hm.minor_version=htons(1); sendto(s,&hm,sizeof(hm),0,(sockaddr*)&srv,slen);
    struct TaskState { uint32_t id=0; uint32_t ar=0; int32_t v1=0,v2=0; bool have=false; int resendReq=0; Clock::time_point lastReq=Clock::now(); } ts; 
    auto deadline=Clock::now()+chrono::seconds(60);
    while(Clock::now()<deadline){
        fd_set rf; FD_ZERO(&rf); FD_SET(s,&rf); timeval tv{0,100000}; int r=select(s+1,&rf,nullptr,nullptr,&tv);
        if(r>0 && FD_ISSET(s,&rf)){
            unsigned char buf[128]; sockaddr_storage from{}; socklen_t fl=sizeof(from); ssize_t n=recvfrom(s,buf,sizeof(buf),0,(sockaddr*)&from,&fl);
            if(n==(ssize_t)sizeof(calcProtocol)){
                calcProtocol cp{}; memcpy(&cp,buf,sizeof(cp)); uint16_t t=ntohs(cp.type); if(t==1){ ts.id=ntohl(cp.id); ts.ar=ntohl(cp.arith); ts.v1=ntohl(cp.inValue1); ts.v2=ntohl(cp.inValue2); ts.have=true; int32_t res=evalOp(ts.ar,ts.v1,ts.v2); if(drop(rng)>=cfg.dropProb){ calcProtocol ans{}; ans.type=htons(2); ans.major_version=htons(1); ans.minor_version=htons(1); ans.id=htonl(ts.id); ans.inResult=htonl(res); sendto(s,&ans,sizeof(ans),0,(sockaddr*)&srv,slen); }
                } else if(t==2){ // ignore stray
                }
            } else if(n==(ssize_t)sizeof(calcMessage)){
                calcMessage cm{}; memcpy(&cm,buf,sizeof(cm)); if(ntohs(cm.type)==2){ uint32_t msg=ntohl(cm.message); if(msg==1){ cfg.answered->fetch_add(1, memory_order_relaxed); cfg.ok->fetch_add(1, memory_order_relaxed); ts.have=false; ts.id=0; }
                }
            }
        }
        if(!ts.have){ // request resend periodically
            auto now=Clock::now(); if(chrono::duration_cast<chrono::milliseconds>(now - ts.lastReq).count()>200){ calcProtocol req{}; req.type=htons(2); req.major_version=htons(1); req.minor_version=htons(1); req.id=htonl(0); sendto(s,&req,sizeof(req),0,(sockaddr*)&srv,slen); ts.lastReq=now; }
        }
        if(cfg.answered->load(memory_order_relaxed) >= cfg.ops) break; // global completion
    }
    close(s);
}

int main(int argc,char*argv[]){ if(argc<4){ cerr<<"usage: host:port tests dropProb [logFile]\n"; return 1; }
    string hp=argv[1]; auto pos=hp.rfind(':'); if(pos==string::npos){ cerr<<"bad host:port\n"; return 1; }
    string host=hp.substr(0,pos), port=hp.substr(pos+1); int tests=stoi(argv[2]); int dropProb=stoi(argv[3]); string outFile = (argc>4?argv[4]:"server_test.log");
    atomic<int> answered{0}; atomic<int> ok{0}; int workers = tests; vector<thread> th; th.reserve(workers);
    for(int i=0;i<workers;i++){ WorkerCfg cfg{host,port,dropProb,i,tests,&answered,&ok}; th.emplace_back(worker,cfg); }
    auto start=Clock::now(); while(answered.load()<tests && chrono::duration_cast<chrono::seconds>(Clock::now()-start).count()<60){ this_thread::sleep_for(chrono::milliseconds(200)); }
    for(auto &t: th) if(t.joinable()) t.join();
    ofstream ofs(outFile); ofs<<"ANS="<<answered.load()<<" OK="<<ok.load()<<"\n"; ofs.close();
    if(answered.load()==tests) cout<<"SUMMARY: PASSED! answered="<<answered.load()<<"\n"; else cout<<"SUMMARY: INCOMPLETE answered="<<answered.load()<<"\n"; return 0; }
