// Minimal, fast UDP server (binary + optional text) cleaned rewrite.
// Goals: low overhead, finish 100 task interactions quickly (<60s), robust to minor packet size variations.

#include <iostream>
#include <unordered_map>
#include <chrono>
#include <cstring>
#include <csignal>
#include <vector>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include "protocol.h"
#include "calcLib.h"

using namespace std; using Clock=chrono::steady_clock;
static volatile bool g_run=true; void sigint(int){ g_run=false; }

struct ClientKey{ sockaddr_storage addr{}; socklen_t len{}; bool operator==(ClientKey const&o) const noexcept{ if(addr.ss_family!=o.addr.ss_family) return false; if(addr.ss_family==AF_INET){auto*A=(sockaddr_in*)&addr;auto*B=(sockaddr_in*)&o.addr; return A->sin_addr.s_addr==B->sin_addr.s_addr && A->sin_port==B->sin_port;} auto*A=(sockaddr_in6*)&addr;auto*B=(sockaddr_in6*)&o.addr; return memcmp(&A->sin6_addr,&B->sin6_addr,sizeof(in6_addr))==0 && A->sin6_port==B->sin6_port; }};
struct ClientHash{ size_t operator()(ClientKey const&k) const noexcept{ size_t h=1469598103934665603ULL; auto mix=[&](const void*d,size_t l){ auto p=(const unsigned char*)d; for(size_t i=0;i<l;++i){ h^=p[i]; h*=1099511628211ULL; } }; if(k.addr.ss_family==AF_INET){ auto*A=(sockaddr_in*)&k.addr; mix(&A->sin_addr,sizeof(A->sin_addr)); mix(&A->sin_port,sizeof(A->sin_port)); } else { auto*A=(sockaddr_in6*)&k.addr; mix(&A->sin6_addr,sizeof(A->sin6_addr)); mix(&A->sin6_port,sizeof(A->sin6_port)); } return h; }};

struct Task { uint32_t id; uint32_t op; int32_t v1; int32_t v2; bool text=false; bool done=false; bool ok=false; Clock::time_point created; Clock::time_point lastSend; int resend=0; };
static Task makeTask(uint32_t id){ Task t{}; t.id=id; t.op=(randomInt()%4)+1; if(t.op==4){ do{ t.v2=randomInt()%100; }while(t.v2==0); t.v1=randomInt()%100; } else { t.v1=randomInt()%100; t.v2=randomInt()%100; } t.created=Clock::now(); t.lastSend=t.created; return t; }
static int32_t eval(const Task&t){ switch(t.op){case 1:return t.v1+t.v2; case 2:return t.v1-t.v2; case 3:return t.v1*t.v2; case 4:return t.v1/t.v2;} return 0; }
static bool splitAddr(const string&s,string&h,string&p){ auto pos=s.rfind(':'); if(pos==string::npos) return false; h=s.substr(0,pos); p=s.substr(pos+1); return !h.empty() && !p.empty(); }

int main(int argc,char*argv[]){
    if(argc<2){ cerr<<"Usage: "<<argv[0]<<" host:port [--text] [--quiet] [--exit-on-complete]\n"; return 1; }
    bool enableText=false, quiet=false, exitOnComplete=false;
    for(int i=2;i<argc;i++){ string f=argv[i];
        if(f=="--text") enableText=true; else if(f=="--quiet") quiet=true; else if(f=="--exit-on-complete") exitOnComplete=true;
    }
    string host,port; if(!splitAddr(argv[1],host,port)){ cerr<<"Bad host:port\n"; return 1; }
    if(host=="localhost"||host=="ip4-localhost") host="127.0.0.1"; // prefer IPv4
    signal(SIGINT,sigint); initCalcLib();
    // Bind single (IPv4 preferred); if user gave IPv6 explicit, attempt that.
    vector<int> sockets; auto bindOne=[&](const string&H){ struct addrinfo hints{}; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_DGRAM; hints.ai_flags=AI_PASSIVE; struct addrinfo*res=nullptr; if(getaddrinfo(H.c_str(),port.c_str(),&hints,&res)!=0) return; for(auto*p=res;p;p=p->ai_next){ int s=socket(p->ai_family,p->ai_socktype,p->ai_protocol); if(s<0) continue; int yes=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes)); if(p->ai_family==AF_INET6){ int off=0; setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,&off,sizeof(off)); } if(bind(s,p->ai_addr,p->ai_addrlen)==0){ sockets.push_back(s); break; } close(s);} freeaddrinfo(res); };
    bindOne(host);
    // Attempt additional wildcard binds to catch clients resolving differently.
    auto haveFam=[&](int fam){ for(int s: sockets){ sockaddr_storage ss{}; socklen_t sl=sizeof(ss); if(getsockname(s,(sockaddr*)&ss,&sl)==0 && ss.ss_family==fam) return true; } return false; };
    if(!haveFam(AF_INET)) bindOne("0.0.0.0"); // wildcard IPv4
    if(!haveFam(AF_INET6)) bindOne("::");      // wildcard IPv6
    if(host=="127.0.0.1" && !haveFam(AF_INET6)){ bindOne("::1"); }
    if(sockets.empty()){ cerr<<"bind failed\n"; return 1; }
    if(!quiet){ cout<<"udpserver fast on "<<host<<":"<<port<<" text="<<(enableText?"on":"off")<<"\n"; cout<<"SOCKETS bound="<<sockets.size(); for(int s:sockets){ sockaddr_storage ss{}; socklen_t sl=sizeof(ss); if(getsockname(s,(sockaddr*)&ss,&sl)==0){ if(ss.ss_family==AF_INET) cout<<" [IPv4]"; else if(ss.ss_family==AF_INET6) cout<<" [IPv6]"; } } cout<<"\n"; }

    struct TaskRec{ Task task; sockaddr_storage addr; socklen_t len; int sockIdx; };
    unordered_map<uint32_t,TaskRec> idTasks; // id -> task record
    unordered_map<ClientKey,uint32_t,ClientHash> clientToId; // address -> id
    uint32_t nextId=1; size_t ok=0, fail=0, issued=0; auto start=Clock::now(); const int TARGET=100; bool completedLogged=false;

    while(g_run){
        fd_set rf; FD_ZERO(&rf); int maxfd=0; for(int s: sockets){ FD_SET(s,&rf); if(s>maxfd) maxfd=s; }
        timeval tv{0,10000}; int sel=select(maxfd+1,&rf,nullptr,nullptr,&tv); if(sel<0){ if(errno==EINTR) continue; perror("select"); break; }
        auto now=Clock::now();
    // (Removed hard timeout and idle watchdog as per user request)
    // (No aggressive cleanup needed; small task set.)
        if(sel>0){
            for(size_t si=0; si<sockets.size(); ++si){ int s=sockets[si]; if(!FD_ISSET(s,&rf)) continue; for(int drain=0; drain<512; ++drain){ sockaddr_storage ca{}; socklen_t clen=sizeof(ca); unsigned char buf[128]; ssize_t n=recvfrom(s,buf,sizeof(buf),MSG_DONTWAIT,(sockaddr*)&ca,&clen); if(n<0){ if(errno==EAGAIN||errno==EWOULDBLOCK) break; perror("recvfrom"); break; } if(n==0) break; ClientKey key{ca,clen};
                // Packet debug (first 15 packets overall)
                static int rawDebugCount=0; if(rawDebugCount<15){ cout<<"RAW len="<<n; if(n>=2) cout<<" b0="<<(int)buf[0]<<" b1="<<(int)buf[1]; cout<<"\n"; ++rawDebugCount; }
                auto existingAddr = clientToId.find(key);
                // Handshake path: client sends calcMessage (12 bytes) type 21 (text) or 22 (binary)
                if(n==sizeof(calcMessage) || n==13){ calcMessage cm{}; memcpy(&cm,buf,sizeof(cm)); uint16_t ctype=ntohs(cm.type); uint16_t maj=ntohs(cm.major_version), min=ntohs(cm.minor_version); if(maj==1 && min==1 && (ctype==21 || ctype==22)){
                        bool wantText = enableText && ctype==21;
                        // For binary clients (22) DO NOT send 12-byte ACK (client expects first reply = calcProtocol)
                        // For text clients (21) send a text line directly (optionally could ACK first, but skip to save RTT)
                        if(existingAddr==clientToId.end() || idTasks[existingAddr->second].task.done){ Task t=makeTask(nextId++); t.text=wantText; TaskRec rec{t,ca,clen,(int)si}; idTasks[t.id]=rec; clientToId[key]=t.id; if(wantText){ string line=to_string(t.id)+" "+(t.op==1?"add":t.op==2?"sub":t.op==3?"mul":"div")+" "+to_string(t.v1)+" "+to_string(t.v2)+"\n"; sendto(s,line.c_str(),line.size(),0,(sockaddr*)&ca,clen); }
                            else { calcProtocol out{}; out.type=htons(1); out.major_version=htons(1); out.minor_version=htons(1); out.id=htonl(t.id); out.arith=htonl(t.op); out.inValue1=htonl(t.v1); out.inValue2=htonl(t.v2); sendto(s,&out,sizeof(out),0,(sockaddr*)&ca,clen); }
                            ++issued; }
                        else { TaskRec &rec=idTasks[ existingAddr->second ]; Task &t=rec.task; if(wantText){ string line=to_string(t.id)+" "+(t.op==1?"add":t.op==2?"sub":t.op==3?"mul":"div")+" "+to_string(t.v1)+" "+to_string(t.v2)+"\n"; sendto(s,line.c_str(),line.size(),0,(sockaddr*)&ca,clen); }
                            else { calcProtocol out{}; out.type=htons(1); out.major_version=htons(1); out.minor_version=htons(1); out.id=htonl(t.id); out.arith=htonl(t.op); out.inValue1=htonl(t.v1); out.inValue2=htonl(t.v2); sendto(s,&out,sizeof(out),0,(sockaddr*)&ca,clen); } }
                        continue; }
                }
                // If new client sends calcProtocol directly with id=0 treat as implicit handshake
                if(existingAddr==clientToId.end() && n>=(ssize_t)24 && n<=(ssize_t)sizeof(calcProtocol)){
                    calcProtocol cp{}; memcpy(&cp,buf,std::min<size_t>(n,sizeof(cp))); if(ntohs(cp.major_version)==1 && ntohs(cp.minor_version)==1){ uint16_t ttype=ntohs(cp.type); if(ttype==2 || ttype==0){ Task t=makeTask(nextId++); TaskRec rec{t,ca,clen,(int)si}; idTasks[t.id]=rec; clientToId[key]=t.id; calcProtocol out{}; out.type=htons(1); out.major_version=htons(1); out.minor_version=htons(1); out.id=htonl(t.id); out.arith=htonl(t.op); out.inValue1=htonl(t.v1); out.inValue2=htonl(t.v2); sendto(s,&out,sizeof(out),0,(sockaddr*)&ca,clen); ++issued; continue; } }
                }
                // Legacy re-send for existing client handshake-size packet
                if(existingAddr!=clientToId.end() && n>=12 && n<=20){ TaskRec &rec=idTasks[ existingAddr->second ]; Task &t=rec.task; // do NOT create new task here; just resend existing if not done
                    if(!t.done){ calcProtocol out{}; out.type=htons(1); out.major_version=htons(1); out.minor_version=htons(1); out.id=htonl(t.id); out.arith=htonl(t.op); out.inValue1=htonl(t.v1); out.inValue2=htonl(t.v2); sendto(s,&out,sizeof(out),0,(sockaddr*)&ca,clen); }
                    continue; }
                // Binary answer (accept 24..sizeof(calcProtocol)) client->server must have type=2
                if(n>=(ssize_t)24 && n<=(ssize_t)sizeof(calcProtocol)){ calcProtocol cp{}; memcpy(&cp,buf,std::min<size_t>(n,sizeof(cp))); if(ntohs(cp.major_version)!=1||ntohs(cp.minor_version)!=1) continue; uint16_t ctype=ntohs(cp.type); if(ctype!=2) { continue; } uint32_t id=ntohl(cp.id); int32_t res=ntohl(cp.inResult); if(id==0){ // resend request without address match
                        if(existingAddr!=clientToId.end()){ TaskRec &rec=idTasks[existingAddr->second]; Task &t=rec.task; if(!t.done){ calcProtocol out{}; out.type=htons(1); out.major_version=htons(1); out.minor_version=htons(1); out.id=htonl(t.id); out.arith=htonl(t.op); out.inValue1=htonl(t.v1); out.inValue2=htonl(t.v2); sendto(s,&out,sizeof(out),0,(sockaddr*)&ca,clen); } }
                        continue; }
                    auto itId=idTasks.find(id); if(itId==idTasks.end()) continue; TaskRec &rec=itId->second; Task &t=rec.task; // Update address if changed (port migration)
                    if( ( (sockaddr*)&rec.addr )->sa_family != ((sockaddr*)&ca)->sa_family || memcmp(&rec.addr,&ca,clen)!=0){ // simplistic update
                        // add new mapping
                        ClientKey newKey{ca,clen}; clientToId[newKey]=id; rec.addr=ca; rec.len=clen; }
                    if(!t.done){ bool okAns=(res==eval(t)); t.done=true; t.ok=okAns; if(okAns) ++ok; else ++fail; calcMessage m{}; m.type=htons(2); m.message=htonl(okAns?1:2); m.protocol=htons(17); m.major_version=htons(1); m.minor_version=htons(1); sendto(s,&m,sizeof(m),0,(sockaddr*)&ca,clen); if(!quiet){ cout<<"ANS id="<<t.id<<" ok="<<okAns<<" res="<<res<<"\n"; if((ok+fail)%20==0){ auto ms=chrono::duration_cast<chrono::milliseconds>(now-start).count(); cout<<"PROGRESS ans="<<(ok+fail)<<" ok="<<ok<<" ms="<<ms<<"\n"; } if(ok>=TARGET && !completedLogged){ auto ms=chrono::duration_cast<chrono::milliseconds>(now-start).count(); cout<<"COMPLETE ok="<<ok<<" fail="<<fail<<" ms="<<ms<<"\n"; completedLogged=true; if(exitOnComplete) { g_run=false; } } } }
                    else { calcMessage m{}; m.type=htons(2); m.message=htonl(t.ok?1:2); m.protocol=htons(17); m.major_version=htons(1); m.minor_version=htons(1); sendto(s,&m,sizeof(m),0,(sockaddr*)&ca,clen); }
                    continue; }
                if(enableText){ string msg((char*)buf,(size_t)n); while(!msg.empty()&&(msg.back()=='\n'||msg.back()=='\r')) msg.pop_back(); auto existingTxt = clientToId.find(key); if(msg=="TEXT UDP 1.1"){ if(existingTxt==clientToId.end() || idTasks[existingTxt->second].task.done){ Task t=makeTask(nextId++); t.text=true; TaskRec rec{t,ca,clen}; idTasks[t.id]=rec; clientToId[key]=t.id; string line=to_string(t.id)+" "+(t.op==1?"add":t.op==2?"sub":t.op==3?"mul":"div")+" "+to_string(t.v1)+" "+to_string(t.v2)+"\n"; sendto(s,line.c_str(),line.size(),0,(sockaddr*)&ca,clen); ++issued; } else { TaskRec &rec=idTasks[ existingTxt->second ]; Task &t=rec.task; string line=to_string(t.id)+" "+(t.op==1?"add":t.op==2?"sub":t.op==3?"mul":"div")+" "+to_string(t.v1)+" "+to_string(t.v2)+"\n"; sendto(s,line.c_str(),line.size(),0,(sockaddr*)&ca,clen); } }
                            else if(existingTxt!=clientToId.end() && idTasks[existingTxt->second].task.text){ TaskRec &rec=idTasks[ existingTxt->second ]; Task &t=rec.task; size_t sp=msg.find(' '); if(sp!=string::npos){ bool parse=true; uint32_t rid=0; long long ans=0; try{ rid=stoul(msg.substr(0,sp)); ans=stoll(msg.substr(sp+1)); }catch(...){ parse=false; } if(parse && rid==t.id && !t.done){ bool okAns=(ans==eval(t)); t.done=true; t.ok=okAns; if(okAns) ++ok; else ++fail; string resp=(okAns?"OK ":"NOT OK "); sendto(s,resp.c_str(),resp.size(),0,(sockaddr*)&ca,clen); if(!quiet && (ok+fail)%20==0){ auto ms=chrono::duration_cast<chrono::milliseconds>(now-start).count(); cout<<"PROGRESS ans="<<(ok+fail)<<" ok="<<ok<<" ms="<<ms<<"\n"; } if(ok>=TARGET && !quiet && !completedLogged){ auto ms=chrono::duration_cast<chrono::milliseconds>(now-start).count(); cout<<"COMPLETE ok="<<ok<<" fail="<<fail<<" ms="<<ms<<"\n"; completedLogged=true; if(exitOnComplete) { g_run=false; } } } } }
                }
            }}
        }
    // Simple proactive resend (lightweight) using idTasks
    auto now2=Clock::now(); for(auto &kv:idTasks){ Task &t=kv.second.task; if(t.done) continue; auto ms=chrono::duration_cast<chrono::milliseconds>(now2 - t.lastSend).count(); if(ms < 80) continue; int target=(t.resend<2?80:(t.resend<5?140:220)); if(t.resend>=5) target=300; if(t.resend>=8) target=450; if(ms>=target){ if(kv.second.sockIdx>=0 && kv.second.sockIdx < (int)sockets.size()){ int outSock=sockets[kv.second.sockIdx]; calcProtocol out{}; out.type=htons(1); out.major_version=htons(1); out.minor_version=htons(1); out.id=htonl(t.id); out.arith=htonl(t.op); out.inValue1=htonl(t.v1); out.inValue2=htonl(t.v2); sendto(outSock,&out,sizeof(out),0,(sockaddr*)&kv.second.addr,kv.second.len); t.lastSend=now2; ++t.resend; } }
    }
    if(!quiet){ static auto lastPrint=start; if(chrono::duration_cast<chrono::milliseconds>(Clock::now()-lastPrint).count()>=1000){ size_t pending=0; for(auto &kv:idTasks) if(!kv.second.task.done) ++pending; auto elapsedMs=chrono::duration_cast<chrono::milliseconds>(Clock::now()-start).count(); cout<<"DIAG tasks="<<issued<<" ok="<<ok<<" fail="<<fail<<" pend="<<pending<<" elapsedMs="<<elapsedMs<<"\n"; lastPrint=Clock::now(); } }
        // Keep running; do NOT exit automatically after target to stay serviceable.
    }
    for(int s: sockets) close(s);
    return 0;
}
