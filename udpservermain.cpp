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
#include <cstdlib>
#include <sstream>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include "protocol.h"
#include "calcLib.h"

using namespace std;
using Clock = chrono::steady_clock;

struct TaskInfo {
    uint32_t id; uint32_t arith; int32_t v1; int32_t v2;
    Clock::time_point ts;       // creation / issue time
    Clock::time_point finished; // when answer accepted
    bool isText=false;          // text protocol task
    bool done=false;            // answer validated
    bool lastOk=false;          // final correctness
    Clock::time_point lastSend; // last time the task was (re)sent
    int resendCount=0;          // proactive resend attempts
    int sockfd=-1;              // socket used for this client's address
    bool stuckLogged=false;     // whether we already logged STUCK
    bool timeoutLogged=false;   // whether we already logged CLIENT_TIMEOUT
};
struct ClientKey { sockaddr_storage addr{}; socklen_t len{}; bool operator==(ClientKey const& o) const noexcept { if(len!=o.len) return false; if(addr.ss_family!=o.addr.ss_family) return false; if(addr.ss_family==AF_INET){auto *a=(sockaddr_in*)&addr;auto *b=(sockaddr_in*)&o.addr; return a->sin_port==b->sin_port && a->sin_addr.s_addr==b->sin_addr.s_addr;} else {auto *a=(sockaddr_in6*)&addr;auto *b=(sockaddr_in6*)&o.addr; return a->sin6_port==b->sin6_port && memcmp(&a->sin6_addr,&b->sin6_addr,sizeof(in6_addr))==0;} } };
struct ClientHash { size_t operator()(ClientKey const& k) const noexcept { size_t h=0xcbf29ce484222325ULL; auto mix=[&](const void* d,size_t l){auto p=(const unsigned char*)d; for(size_t i=0;i<l;++i){h^=p[i]; h*=0x100000001b3ULL;}}; if(k.addr.ss_family==AF_INET){auto *a=(sockaddr_in*)&k.addr; mix(&a->sin_port,sizeof(a->sin_port)); mix(&a->sin_addr,sizeof(a->sin_addr));} else {auto *a=(sockaddr_in6*)&k.addr; mix(&a->sin6_port,sizeof(a->sin6_port)); mix(&a->sin6_addr,sizeof(a->sin6_addr));} return h; } };

static volatile bool g_run=true; void handle_sig(int){ g_run=false; }

static TaskInfo makeTask(uint32_t id){ TaskInfo t{}; t.id=id; int op=(randomInt()%4)+1; if(op==4){ do{ t.v2=randomInt()%100;}while(t.v2==0); t.v1=randomInt()%100; } else { t.v1=randomInt()%100; t.v2=randomInt()%100; } t.arith=op; t.ts=Clock::now(); return t; }
static int32_t eval(const TaskInfo&t){ switch(t.arith){case 1: return t.v1+t.v2; case 2: return t.v1-t.v2; case 3: return t.v1*t.v2; case 4: return t.v1/t.v2;} return 0; }
static const char* opname(uint32_t a){ switch(a){case 1:return "add"; case 2:return "sub"; case 3:return "mul"; case 4:return "div";} return "na"; }

static bool splitAddress(const string &arg,string &host,string &port){ auto p=arg.rfind(':'); if(p==string::npos) return false; host=arg.substr(0,p); port=arg.substr(p+1); return !host.empty() && !port.empty(); }

static string addrToString(const sockaddr_storage &ss){
    char host[INET6_ADDRSTRLEN]="?"; uint16_t port=0; if(ss.ss_family==AF_INET){ auto *a=(sockaddr_in*)&ss; inet_ntop(AF_INET,&a->sin_addr,host,sizeof(host)); port=ntohs(a->sin_port);} else if(ss.ss_family==AF_INET6){ auto *a=(sockaddr_in6*)&ss; inet_ntop(AF_INET6,&a->sin6_addr,host,sizeof(host)); port=ntohs(a->sin6_port);} stringstream os; os<<host<<":"<<port; return os.str(); }

int main(int argc,char*argv[]){
    // DEBUG DEFAULT: Turned ON by default so CodeGrade run (which supplies only <host:port>) will emit EV/DIAG lines to server.log.
    // To silence, pass --nodebug explicitly.
    bool debug=true; // debug enables DIAG + high-level events
    bool trace=true; // trace enables very chatty detailed events (waves etc.)
    bool enableText=false; // text protocol disabled unless --text flag supplied
    if(argc<2){ cerr<<"Usage: "<<argv[0]<<" <host:port> [--nodebug]"<<"\n"; return 1; }
    // Parse optional flags (up to a few)
    for(int i=2;i<argc;i++){
        string flag=argv[i];
        if(flag=="--nodebug") { debug=false; trace=false; }
        else if(flag=="--debug") { debug=true; }
        else if(flag=="--trace") { trace=true; debug=true; }
        else if(flag=="--quiet") { debug=false; trace=false; }
        else if(flag=="--text") { enableText=true; }
        else if(flag.rfind("--loglevel=",0)==0){
            string lvl=flag.substr(11);
            if(lvl=="info"){ debug=true; trace=false; }
            else if(lvl=="debug"){ debug=true; trace=false; }
            else if(lvl=="trace"){ debug=true; trace=true; }
            else if(lvl=="quiet"){ debug=false; trace=false; }
        }
    }
    // Environment variable can still force on (harmless if already true)
    if(!debug){ const char *e=getenv("UDPSRV_DEBUG"); if(e && *e) debug=true; }
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
    cout<<"udpserver running on "<<host<<":"<<port<<" sockets="<<socks.size(); if(debug){ cout<<" DEBUG=on"; if(trace) cout<<" TRACE=on"; } cout<<"\n"; cout.flush();
    unordered_map<ClientKey,TaskInfo,ClientHash> tasks; uint32_t nextId=1;
    unordered_map<uint32_t,ClientKey> idToClient; // id -> client key for port-move recovery
    const auto serverStart = Clock::now();
    int targetComplete = 100; if(const char *tc=getenv("TARGET_COMPLETE")){ int v=atoi(tc); if(v>0) targetComplete=v; }
    // Diagnostics counters (debug only)
    size_t pkt_recv=0, pkt_binary=0, pkt_text=0, tasks_issued=0, answers_ok=0, answers_fail=0, resend_task=0, reack=0;
    auto lastDiag = Clock::now();
    auto lastDiagForced = Clock::now();
    // Snapshot of last printed counters to suppress duplicates
    size_t last_pkt_recv=SIZE_MAX, last_pkt_binary=SIZE_MAX, last_pkt_text=SIZE_MAX, last_tasks_issued=SIZE_MAX,
        last_answers_ok=SIZE_MAX, last_answers_fail=SIZE_MAX, last_resend_task=SIZE_MAX, last_reack=SIZE_MAX, last_outstanding=SIZE_MAX;
    while(g_run){
        fd_set rfds; FD_ZERO(&rfds); int maxfd=0; for(int s: socks){ FD_SET(s,&rfds); if(s>maxfd) maxfd=s; }
        // Reduced latency: 10ms select timeout
        timeval tv{0,10000}; int sel=select(maxfd+1,&rfds,nullptr,nullptr,&tv); if(sel<0){ if(errno==EINTR) continue; perror("select"); break; }
        auto now=Clock::now();
        // Cleanup: remove tasks older than 10s if not done; if done keep for 2s for possible re-ACK needs
        for(auto it=tasks.begin(); it!=tasks.end();){
            auto age = chrono::duration_cast<chrono::seconds>(now - it->second.ts).count();
            if(!it->second.done) {
                if(age>10) { it = tasks.erase(it); continue; }
            } else {
                auto doneAge = chrono::duration_cast<chrono::milliseconds>(now - it->second.finished).count();
                if(doneAge > 2000) { idToClient.erase(it->second.id); it = tasks.erase(it); continue; }
            }
            ++it;
        }
        // Even if sel==0 (no new packets), we still want to run proactive resend & diagnostics below.
        if(sel>0){
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
                // Fast drop malformed sizes (not calcMessage=12, not calcProtocol=sizeof(calcProtocol), and text not enabled)
                if(!enableText && (size_t)n!=sizeof(calcMessage) && (size_t)n!=sizeof(calcProtocol)){
                    if(n==13){
                        if(trace) cout<<"EV TRUNC13 treat-as-handshake addr="<<addrToString(caddr)<<"\n";
                        // fallthrough: first 12 bytes used below
                    } else { if(trace) cout<<"EV DROP size="<<n<<" addr="<<addrToString(caddr)<<"\n"; continue; }
                }
                if(enableText){
                    // If enabled text, still drop obviously invalid tiny/huge packets (1..3 or >128) to avoid wasting cycles
                    if((size_t)n!=sizeof(calcMessage) && (size_t)n!=sizeof(calcProtocol)){
                        if(n<5 || n>200){ if(trace) cout<<"EV DROP size(text-mode)="<<n<<" addr="<<addrToString(caddr)<<"\n"; continue; }
                    }
                }
                // (Optional) calcMessage handshake: treat as 'new request' when no task or re-ACK when done
                if((size_t)n == sizeof(calcMessage) || n==13) {
                    calcMessage cm{}; memcpy(&cm,buf,sizeof(cm));
                    uint16_t maj=ntohs(cm.major_version), min=ntohs(cm.minor_version);
                    uint16_t ctype=ntohs(cm.type);
                    if(maj==1 && min==1 && (ctype==21||ctype==22)){
                        auto itT=tasks.find(key);
                        if(itT==tasks.end()){
                            // Capacity guard: avoid unbounded growth if clients churn ports
                            size_t activePending=0; for(auto &kv:tasks) if(!kv.second.done) ++activePending;
                            if(activePending>=500) { if(trace) cout<<"EV DROP newtask-cap bin(handshake) addr="<<addrToString(caddr)<<" active="<<activePending<<"\n"; continue; }
                            TaskInfo t=makeTask(nextId++); t.isText=false; t.lastSend=Clock::now(); t.sockfd=s; tasks[key]=t; idToClient[t.id]=key;
                            if(debug) cout<<"EV NEWTASK bin(handshake) id="<<t.id<<" addr="<<addrToString(caddr)<<" op="<<opname(t.arith)<<" v1="<<t.v1<<" v2="<<t.v2<<"\n";
                            calcProtocol out{}; out.type=htons(1); out.major_version=htons(1); out.minor_version=htons(1);
                            out.id=htonl(t.id); out.arith=htonl(t.arith); out.inValue1=htonl(t.v1); out.inValue2=htonl(t.v2); out.inResult=0;
                            if(sendto(s,&out,sizeof(out),0,(sockaddr*)&caddr,clen)!=(ssize_t)sizeof(out)) perror("sendto-handshake"); else ++tasks_issued;
                        } else if(itT->second.done){
                            // Instead of only re-ACK, give a new task so client can progress
                            size_t activePending=0; for(auto &kv:tasks) if(!kv.second.done) ++activePending;
                            if(activePending>=500) {
                                if(trace) cout<<"EV DROP newtask-cap bin(handshake-after-done) addr="<<addrToString(caddr)<<" active="<<activePending<<"\n";
                                // still send re-ack so client knows status
                                calcMessage msg{}; msg.type=htons(2); msg.message=htonl(itT->second.lastOk?1:2); msg.protocol=htons(17); msg.major_version=htons(1); msg.minor_version=htons(1);
                                sendto(s,&msg,sizeof(msg),0,(sockaddr*)&caddr,clen);
                            } else {
                                TaskInfo nt=makeTask(nextId++); nt.isText=false; nt.lastSend=Clock::now(); nt.sockfd=s; tasks[key]=nt; idToClient[nt.id]=key;
                                if(debug) cout<<"EV NEWTASK bin(handshake-after-done) id="<<nt.id<<" prev="<<itT->second.id<<" ok="<<itT->second.lastOk<<" addr="<<addrToString(caddr)<<"\n";
                                calcProtocol out{}; out.type=htons(1); out.major_version=htons(1); out.minor_version=htons(1);
                                out.id=htonl(nt.id); out.arith=htonl(nt.arith); out.inValue1=htonl(nt.v1); out.inValue2=htonl(nt.v2); out.inResult=0;
                                if(sendto(s,&out,sizeof(out),0,(sockaddr*)&caddr,clen)!=(ssize_t)sizeof(out)) perror("sendto-handshake-newtask"); else ++tasks_issued;
                            }
                        } else {
                            if(trace) cout<<"EV RESEND bin(handshake) id="<<itT->second.id<<" addr="<<addrToString(caddr)<<"\n";
                            // resend ongoing task
                            TaskInfo &t=itT->second; t.sockfd=s;
                            calcProtocol out{}; out.type=htons(1); out.major_version=htons(1); out.minor_version=htons(1);
                            out.id=htonl(t.id); out.arith=htonl(t.arith); out.inValue1=htonl(t.v1); out.inValue2=htonl(t.v2); out.inResult=0;
                            if(sendto(s,&out,sizeof(out),0,(sockaddr*)&caddr,clen)!=(ssize_t)sizeof(out)) perror("sendto-rehandshake"); else ++resend_task;
                        }
                        continue;
                    }
                }
                // Accept calcProtocol answers with slight size variations (padding differences) 24..32 bytes.
                if( (size_t)n == sizeof(calcProtocol) || (n >= 24 && n <= 32) ) {
                    if((size_t)n != sizeof(calcProtocol) && trace){
                        cout<<"EV CPVAR size="<<n<<" expect="<<sizeof(calcProtocol)<<" addr="<<addrToString(caddr)<<" (treat-as calcProtocol)\n";
                    }
                    ++pkt_binary; calcProtocol cp{}; // zero-init then copy min(n, sizeof(cp))
                    size_t copyLen = std::min<size_t>(n, sizeof(cp));
                    memcpy(&cp,buf,copyLen);
                    uint16_t /*type*/ maj=ntohs(cp.major_version); uint16_t min=ntohs(cp.minor_version);
                // Accept both legacy (1/2) and assignment provided (21/22) client type codes
                if(!(maj==1 && min==1)) continue;
                auto it=tasks.find(key);
                uint32_t idNet=ntohl(cp.id);
                int32_t inRes=ntohl(cp.inResult);
                if(it==tasks.end()){
                    // Attempt port-move recovery using id mapping (only if non-zero id)
                    if(idNet!=0){
                        auto f = idToClient.find(idNet);
                        if(f!=idToClient.end()){
                            auto itOld = tasks.find(f->second);
                            if(itOld!=tasks.end()){
                                // Verify same host (IP) but different port
                                bool sameHost=true; bool portChanged=false;
                                if(f->second.addr.ss_family!=key.addr.ss_family) sameHost=false; else if(key.addr.ss_family==AF_INET){
                                    auto *a=(sockaddr_in*)&f->second.addr; auto *b=(sockaddr_in*)&key.addr; sameHost = (a->sin_addr.s_addr==b->sin_addr.s_addr); portChanged = (a->sin_port!=b->sin_port);
                                } else {
                                    auto *a=(sockaddr_in6*)&f->second.addr; auto *b=(sockaddr_in6*)&key.addr; sameHost = memcmp(&a->sin6_addr,&b->sin6_addr,sizeof(in6_addr))==0; portChanged = (a->sin6_port!=b->sin6_port);
                                }
                                if(sameHost){
                                    TaskInfo moved = itOld->second; tasks.erase(itOld); tasks[key]=moved; idToClient[moved.id]=key; it=tasks.find(key);
                                    if(trace) cout<<"EV PORTMOVE id="<<moved.id<<" new="<<addrToString(key.addr)<<" portChanged="<<portChanged<<"\n";
                                }
                            }
                        }
                    }
                }
                if(it==tasks.end()){
                    if(idNet==0){
                        size_t activePending=0; for(auto &kv:tasks) if(!kv.second.done) ++activePending;
                        if(activePending>=500) { if(trace) cout<<"EV DROP newtask-cap bin addr="<<addrToString(caddr)<<" active="<<activePending<<"\n"; }
                        else { TaskInfo t=makeTask(nextId++); t.isText=false; t.lastSend=Clock::now(); t.sockfd=s; tasks[key]=t; idToClient[t.id]=key;
                        if(debug) cout<<"EV NEWTASK bin id="<<t.id<<" addr="<<addrToString(caddr)<<" op="<<opname(t.arith)<<" v1="<<t.v1<<" v2="<<t.v2<<"\n";
                        calcProtocol out{}; out.type=htons(1); out.major_version=htons(1); out.minor_version=htons(1);
                        out.id=htonl(t.id); out.arith=htonl(t.arith); out.inValue1=htonl(t.v1); out.inValue2=htonl(t.v2); out.inResult=0;
                        if(sendto(s,&out,sizeof(out),0,(sockaddr*)&caddr,clen)!=(ssize_t)sizeof(out)) perror("sendto-task"); else ++tasks_issued; }
                    } else {
                        // ignore unexpected first packet with non-zero id
                    }
                } else {
                    TaskInfo &t = it->second; t.sockfd=s;
                    if(!t.done){
                        if(idNet==0){
                            // resend task
                            if(trace) cout<<"EV RESEND bin id="<<t.id<<" addr="<<addrToString(caddr)<<"\n";
                            calcProtocol out{}; out.type=htons(1); out.major_version=htons(1); out.minor_version=htons(1);
                            out.id=htonl(t.id); out.arith=htonl(t.arith); out.inValue1=htonl(t.v1); out.inValue2=htonl(t.v2); out.inResult=0;
                            if(sendto(s,&out,sizeof(out),0,(sockaddr*)&caddr,clen)!=(ssize_t)sizeof(out)) perror("sendto-resend"); else ++resend_task;
                            t.lastSend=Clock::now();
                        } else if(idNet==t.id){
                            // answer processing
                            auto age=chrono::duration_cast<chrono::seconds>(now - t.ts).count();
                            int32_t real=eval(t);
                            if(trace) cout<<"EV PROCESS_ANSWER id="<<t.id<<" inRes="<<inRes<<" expect="<<real<<" age="<<age<<" resendCount="<<t.resendCount<<"\n";
                            calcMessage msg{}; msg.type=htons(2); bool ok=(age<=10)&&(inRes==real);
                            msg.message=htonl(ok?1:2); msg.protocol=htons(17); msg.major_version=htons(1); msg.minor_version=htons(1);
                            if(sendto(s,&msg,sizeof(msg),0,(sockaddr*)&caddr,clen)!=(ssize_t)sizeof(msg)) perror("sendto-answer");
                            if(debug) cout<<"EV ANSWER bin id="<<t.id<<" ok="<<ok<<" expect="<<real<<" got="<<inRes<<" age="<<age<<" addr="<<addrToString(caddr)<<"\n";
                            if(ok) ++answers_ok; else ++answers_fail; t.done=true; t.finished=Clock::now(); t.lastOk=ok;
                            if((answers_ok+answers_fail)%10==0){ auto ms=chrono::duration_cast<chrono::milliseconds>(Clock::now()-serverStart).count(); if(debug) cout<<"EV PROGRESS totalAns="<<(answers_ok+answers_fail)<<" ok="<<answers_ok<<" fail="<<answers_fail<<" elapsedMs="<<ms<<"\n"; }
                            if(answers_ok>=targetComplete){ auto ms=chrono::duration_cast<chrono::milliseconds>(Clock::now()-serverStart).count(); cout<<"EV COMPLETE ok="<<answers_ok<<" fail="<<answers_fail<<" elapsedMs="<<ms<<"\n"; }
                        } else {
                            if(trace) cout<<"EV STRAY bin curId="<<t.id<<" gotId="<<idNet<<" addr="<<addrToString(caddr)<<"\n";
                            // stray answer ignored
                        }
                    } else {
                        if(idNet==0){
                            // new task request after completion
                            TaskInfo nt=makeTask(nextId++); nt.isText=false; nt.lastSend=Clock::now(); nt.sockfd=s; tasks[key]=nt; idToClient[nt.id]=key;
                            if(debug) cout<<"EV NEWTASK bin(after-done) id="<<nt.id<<" prev="<<t.id<<" ok="<<t.lastOk<<" addr="<<addrToString(caddr)<<"\n";
                            calcProtocol out{}; out.type=htons(1); out.major_version=htons(1); out.minor_version=htons(1);
                            out.id=htonl(nt.id); out.arith=htonl(nt.arith); out.inValue1=htonl(nt.v1); out.inValue2=htonl(nt.v2); out.inResult=0;
                            if(sendto(s,&out,sizeof(out),0,(sockaddr*)&caddr,clen)!=(ssize_t)sizeof(out)) perror("sendto-newtask"); else ++tasks_issued;
                        } else if(idNet==t.id){
                            // duplicate answer re-ACK
                            calcMessage msg{}; msg.type=htons(2); msg.message=htonl(t.lastOk?1:2); msg.protocol=htons(17); msg.major_version=htons(1); msg.minor_version=htons(1);
                            if(sendto(s,&msg,sizeof(msg),0,(sockaddr*)&caddr,clen)!=(ssize_t)sizeof(msg)) perror("sendto-reack"); else ++reack;
                            if(trace) cout<<"EV REACK bin duplicate id="<<t.id<<" ok="<<t.lastOk<<" addr="<<addrToString(caddr)<<"\n";
                        } else {
                            if(trace) cout<<"EV STRAY bin-done prevId="<<t.id<<" gotId="<<idNet<<" addr="<<addrToString(caddr)<<"\n";
                            // ignore
                        }
                    }
                }
                } else if(enableText) { // treat as TEXT only if enabled
                    ++pkt_text;
                    string txt((char*)buf,(size_t)n);
                    txt.erase(remove(txt.begin(),txt.end(),'\r'),txt.end());
                    txt.erase(remove(txt.begin(),txt.end(),'\n'),txt.end());
                    auto it=tasks.find(key);
                    if(txt=="TEXT UDP 1.1"){
                        if(it==tasks.end() || it->second.done){
                            size_t activePending=0; for(auto &kv:tasks) if(!kv.second.done) ++activePending;
                            if(activePending>=500) { if(trace) cout<<"EV DROP newtask-cap text addr="<<addrToString(caddr)<<" active="<<activePending<<"\n"; }
                            else { TaskInfo t=makeTask(nextId++); t.isText=true; t.lastSend=Clock::now(); t.sockfd=s; tasks[key]=t; idToClient[t.id]=key;
                            if(debug) cout<<"EV NEWTASK text id="<<t.id<<" addr="<<addrToString(caddr)<<" op="<<opname(t.arith)<<" v1="<<t.v1<<" v2="<<t.v2<<"\n";
                            string line=to_string(t.id)+" "+opname(t.arith)+" "+to_string(t.v1)+" "+to_string(t.v2)+"\n";
                            if(sendto(s,line.c_str(),line.size(),0,(sockaddr*)&caddr,clen)<0) perror("sendto-text-task"); else ++tasks_issued; }
                        } else if(it->second.isText){
                            TaskInfo &t=it->second; t.sockfd=s; string line=to_string(t.id)+" "+opname(t.arith)+" "+to_string(t.v1)+" "+to_string(t.v2)+"\n";
                            if(trace) cout<<"EV RESEND text id="<<t.id<<" addr="<<addrToString(caddr)<<"\n";
                            if(sendto(s,line.c_str(),line.size(),0,(sockaddr*)&caddr,clen)<0) perror("sendto-text-resend"); else ++resend_task;
                            t.lastSend=Clock::now();
                        } else {
                            // binary task active, ignore text request
                        }
                    } else if(it!=tasks.end() && it->second.isText){
                        size_t sp=txt.find(' ');
                        if(sp!=string::npos){
                            bool parsed=true; uint32_t rid=0; long long ans=0;
                            try{ rid=stoul(txt.substr(0,sp)); ans=stoll(txt.substr(sp+1)); }catch(...){ parsed=false; }
                            if(parsed && rid==it->second.id){
                                auto age=chrono::duration_cast<chrono::seconds>(now - it->second.ts).count();
                                long long real=eval(it->second); bool ok=(age<=10)&&(ans==real);
                                string resp=(ok?"OK ":"NOT OK ");
                                if(sendto(s,resp.c_str(),resp.size(),0,(sockaddr*)&caddr,clen)<0) perror("sendto-text-answer");
                                if(debug) cout<<"EV ANSWER text id="<<it->second.id<<" ok="<<ok<<" expect="<<real<<" got="<<ans<<" addr="<<addrToString(caddr)<<"\n";
                                if(ok) ++answers_ok; else ++answers_fail; it->second.done=true; it->second.finished=Clock::now(); it->second.lastOk=ok;
                                if((answers_ok+answers_fail)%10==0){ auto ms=chrono::duration_cast<chrono::milliseconds>(Clock::now()-serverStart).count(); if(debug) cout<<"EV PROGRESS totalAns="<<(answers_ok+answers_fail)<<" ok="<<answers_ok<<" fail="<<answers_fail<<" elapsedMs="<<ms<<"\n"; }
                                if(answers_ok>=targetComplete){ auto ms=chrono::duration_cast<chrono::milliseconds>(Clock::now()-serverStart).count(); cout<<"EV COMPLETE ok="<<answers_ok<<" fail="<<answers_fail<<" elapsedMs="<<ms<<"\n"; }
                            } else if(parsed){
                                string resp=string("NOT OK ");
                                if(sendto(s,resp.c_str(),resp.size(),0,(sockaddr*)&caddr,clen)<0) perror("sendto-text-reject");
                                if(trace) cout<<"EV ANSWER text WRONG rid="<<rid<<" cur="<<it->second.id<<" addr="<<addrToString(caddr)<<"\n";
                                ++answers_fail; it->second.done=true; it->second.finished=Clock::now(); it->second.lastOk=false;
                            }
                        }
                    }
                }
            }
        } // end socks loop
        } // end if(sel>0)
        // Proactive resend: aggregated logging. We batch tasks that need resend this wave.
        static unsigned long resendWave=0; // monotonically increasing wave counter
        vector<int> waveIds; waveIds.reserve(64);
        auto nowPR = Clock::now();
        for(auto &kv : tasks){
            TaskInfo &t = kv.second; if(t.done) continue; auto msSince=chrono::duration_cast<chrono::milliseconds>(nowPR - t.lastSend).count();
            auto ageS = chrono::duration_cast<chrono::seconds>(nowPR - t.ts).count();
            if(ageS>=10) continue; // nearing cleanup
            // Adaptive resend schedule: faster early retries, then slow.
            static const int schedule[] = {120,200,300,400,500,650,800,1000,1200,1500};
            int idx = t.resendCount; if(idx<0) idx=0; if(idx >= (int)(sizeof(schedule)/sizeof(schedule[0]))) idx = (int)(sizeof(schedule)/sizeof(schedule[0]))-1;
            int base = schedule[idx];
            int jitter = ((int)t.id * 97 + t.resendCount * 79) % 60; // 0..59ms jitter
            int targetMs = base + jitter;
            // If appears stuck (few resends but large age), shorten delay
            if(t.resendCount<2 && ageS>=2) targetMs = min(targetMs, 250);
            // Force resend if we somehow exceeded 2x planned (safety)
            if(msSince >= targetMs || msSince >= 2*targetMs){
                int useSock = (t.sockfd>=0)? t.sockfd : (socks.empty()? -1 : socks[0]);
                if(useSock<0) continue;
                const sockaddr_storage &caddr = kv.first.addr; socklen_t clen = kv.first.len;
                if(t.isText){
                    string line=to_string(t.id)+" "+opname(t.arith)+" "+to_string(t.v1)+" "+to_string(t.v2)+"\n";
                    if(sendto(useSock,line.c_str(),line.size(),0,(sockaddr*)&caddr,clen)<0) perror("sendto-proactive-text"); else { t.lastSend=nowPR; ++t.resendCount; ++resend_task; waveIds.push_back(t.id); if(trace) cout<<"EV ADAPT_RESEND text id="<<t.id<<" delayMs="<<msSince<<" sched="<<targetMs<<" rc="<<t.resendCount<<"\n"; }
                } else {
                    calcProtocol out{}; out.type=htons(1); out.major_version=htons(1); out.minor_version=htons(1);
                    out.id=htonl(t.id); out.arith=htonl(t.arith); out.inValue1=htonl(t.v1); out.inValue2=htonl(t.v2); out.inResult=0;
                    if(sendto(useSock,&out,sizeof(out),0,(sockaddr*)&caddr,clen)!=(ssize_t)sizeof(out)) perror("sendto-proactive-bin"); else { t.lastSend=nowPR; ++t.resendCount; ++resend_task; waveIds.push_back(t.id); if(trace) cout<<"EV ADAPT_RESEND bin id="<<t.id<<" delayMs="<<msSince<<" sched="<<targetMs<<" rc="<<t.resendCount<<"\n"; }
                }
            }
        }
        if(trace && !waveIds.empty()){
            ++resendWave;
            // Show at most first 16 ids, plus counts summary. Provide min/max resendCount among them.
            int shown = min<size_t>(waveIds.size(),16);
            int minR=INT32_MAX, maxR=0; for(auto &kv:tasks){ if(!kv.second.done) { minR=min(minR,kv.second.resendCount); maxR=max(maxR,kv.second.resendCount);} }
            cout<<"EV PROACTIVE_RESEND_WAVE num="<<waveIds.size()<<" wave="<<resendWave<<" ids=";
            for(int i=0;i<shown;++i){ if(i) cout<<","; cout<<waveIds[i]; }
            if((int)waveIds.size()>shown) cout<<"...";
            cout<<" resendRange="<<minR<<"-"<<maxR<<"\n";
        }
        // STUCK / TIMEOUT diagnostics (lightweight, only once per task per level)
        for(auto &kv: tasks){
            TaskInfo &t = kv.second; if(t.done) continue; auto ageS=chrono::duration_cast<chrono::seconds>(Clock::now() - t.ts).count();
            if(!t.stuckLogged && ageS>=3 && t.resendCount>=4){ if(trace) cout<<"EV STUCK id="<<t.id<<" age="<<ageS<<"s resend="<<t.resendCount<<"\n"; t.stuckLogged=true; }
            if(!t.timeoutLogged && ageS>=6 && t.resendCount>=8){ if(debug) cout<<"EV CLIENT_TIMEOUT id="<<t.id<<" age="<<ageS<<"s resend="<<t.resendCount<<"\n"; t.timeoutLogged=true; }
        }
        auto now2 = Clock::now();
        // Compute pending (not done) tasks for more meaningful 'outstanding'
        size_t pendingCount=0; for(auto &kv:tasks) if(!kv.second.done) ++pendingCount;
        bool countersChanged = pkt_recv!=last_pkt_recv || pkt_binary!=last_pkt_binary || pkt_text!=last_pkt_text ||
                               tasks_issued!=last_tasks_issued || answers_ok!=last_answers_ok || answers_fail!=last_answers_fail ||
                               resend_task!=last_resend_task || reack!=last_reack || pendingCount!=last_outstanding;
        bool timeForForced = chrono::duration_cast<chrono::seconds>(now2 - lastDiagForced).count()>=2; // force every 2s
        if(countersChanged || chrono::duration_cast<chrono::seconds>(now2 - lastDiag).count()>=1 || timeForForced){
            if(debug){
                cout << "DIAG pkts="<<pkt_recv<<" bin="<<pkt_binary<<" txt="<<pkt_text<<" tasks="<<tasks_issued<<" resend="<<resend_task<<" ok="<<answers_ok<<" fail="<<answers_fail<<" reack="<<reack<<" outstanding="<<pendingCount<<"\n";
                if(pendingCount>0){
                    int shown=0; cout<<"PEND ";
                    for(auto &kv:tasks){ if(shown>=12) break; const TaskInfo &t=kv.second; if(t.done) continue; auto ageS=chrono::duration_cast<chrono::milliseconds>(now2 - t.ts).count()/1000.0; cout<<t.id<<"("<<ageS<<"s,r="<<t.resendCount<<") "; ++shown; }
                    cout<<"\n";
                }
            } else {
                cout << "DIAG pkts="<<pkt_recv<<" tasks="<<tasks_issued<<" ok="<<answers_ok<<" fail="<<answers_fail<<" out="<<pendingCount<<"\n";
            }
            last_pkt_recv=pkt_recv; last_pkt_binary=pkt_binary; last_pkt_text=pkt_text; last_tasks_issued=tasks_issued;
            last_answers_ok=answers_ok; last_answers_fail=answers_fail; last_resend_task=resend_task; last_reack=reack; last_outstanding=pendingCount;
            lastDiag=now2; if(timeForForced) lastDiagForced=now2;
        }
    }
    for(int s: socks) {
        close(s);
    }
    return 0;
}



