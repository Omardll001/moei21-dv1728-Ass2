// One socket only, minimal output (errors + key points). Protocol: client handshake (calcMessage type 21=text or 22=binary).
// Server sends a single task (calcProtocol type=1 or text line). Client replies (calcProtocol type=2 or text answer). Server ACK calcMessage type=2.
// Optional DEBUG mode with detailed packet trace when compiled with -DDEBUG.

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

#ifdef DEBUG
static void debugPacket(const sockaddr_storage &ca, socklen_t clen, ssize_t n, bool isNew){
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    if(getnameinfo((const sockaddr*)&ca,clen,hbuf,sizeof(hbuf),sbuf,sizeof(sbuf),NI_NUMERICHOST|NI_NUMERICSERV)==0){
        std::cout<<"PKT len="<<n<<" from="<<hbuf<<":"<<sbuf<<(isNew?" NEW":"")<<"\n";
    }
}
#define DBG(x) do { x; } while(0)
#else
#define DBG(x) do{}while(0)
#endif


struct ClientKey{ sockaddr_storage addr{}; socklen_t len{}; bool operator==(ClientKey const&o) const noexcept{ if(addr.ss_family!=o.addr.ss_family) return false; if(addr.ss_family==AF_INET){auto*A=(sockaddr_in*)&addr;auto*B=(sockaddr_in*)&o.addr; return A->sin_addr.s_addr==B->sin_addr.s_addr && A->sin_port==B->sin_port;} auto*A=(sockaddr_in6*)&addr;auto*B=(sockaddr_in6*)&o.addr; return memcmp(&A->sin6_addr,&B->sin6_addr,sizeof(in6_addr))==0 && A->sin6_port==B->sin6_port; }};
struct ClientHash{ size_t operator()(ClientKey const&k) const noexcept{ size_t h=1469598103934665603ULL; auto mix=[&](const void*d,size_t l){ auto p=(const unsigned char*)d; for(size_t i=0;i<l;++i){ h^=p[i]; h*=1099511628211ULL; } }; if(k.addr.ss_family==AF_INET){ auto*A=(sockaddr_in*)&k.addr; mix(&A->sin_addr,sizeof(A->sin_addr)); mix(&A->sin_port,sizeof(A->sin_port)); } else { auto*A=(sockaddr_in6*)&k.addr; mix(&A->sin6_addr,sizeof(A->sin6_addr)); mix(&A->sin6_port,sizeof(A->sin6_port)); } return h; }};

struct Task { uint32_t id; uint32_t op; int32_t v1; int32_t v2; bool text=false; bool done=false; bool ok=false; Clock::time_point created; };
static Task makeTask(uint32_t id){ Task t{}; t.id=id; t.op=(randomInt()%4)+1; if(t.op==4){ do{ t.v2=randomInt()%100; }while(t.v2==0); t.v1=randomInt()%100; } else { t.v1=randomInt()%100; t.v2=randomInt()%100; } t.created=Clock::now(); return t; }
static int32_t eval(const Task&t){ switch(t.op){case 1:return t.v1+t.v2; case 2:return t.v1-t.v2; case 3:return t.v1*t.v2; case 4:return t.v1/t.v2;} return 0; }
static bool splitAddr(const string&s,string&h,string&p){ auto pos=s.rfind(':'); if(pos==string::npos) return false; h=s.substr(0,pos); p=s.substr(pos+1); return !h.empty() && !p.empty(); }

int main(int argc, char* argv[]) {
    std::cout.setf(std::ios::unitbuf);
    if (argc < 2) { std::cerr << "Usage: " << argv[0] << " host:port\n"; return 1; }
    std::string host, port;
    if (!splitAddr(argv[1], host, port)) { std::cerr << "Bad host:port\n"; return 1; }
    if (host == "localhost" || host == "ip4-localhost") host = "127.0.0.1";
    signal(SIGINT,sigint); initCalcLib();
    int sock=-1; {
        struct addrinfo hints{}; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_DGRAM; hints.ai_flags=AI_PASSIVE; struct addrinfo*res=nullptr; if(getaddrinfo(host.c_str(),port.c_str(),&hints,&res)!=0){ perror("getaddrinfo"); return 1; }
        for(auto*p=res;p;p=p->ai_next){ int s=socket(p->ai_family,p->ai_socktype,p->ai_protocol); if(s<0) continue; int yes=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes)); if(bind(s,p->ai_addr,p->ai_addrlen)==0){ sock=s; break; } close(s);} freeaddrinfo(res);
    }
    if(sock<0){ perror("bind"); return 1; }
    auto addrStr=[&](const sockaddr_storage &sa)->std::string{
        char h[NI_MAXHOST], s[NI_MAXSERV];
        if(getnameinfo((const sockaddr*)&sa,(sa.ss_family==AF_INET?sizeof(sockaddr_in):sizeof(sockaddr_in6)),h,sizeof(h),s,sizeof(s),NI_NUMERICHOST|NI_NUMERICSERV)==0){
            return std::string(h)+":"+s; }
        return std::string("?"); };
    DBG(cout<<"LISTEN "<<host<<":"<<port<<"\n");
    struct TaskRec{ Task task; sockaddr_storage addr; socklen_t len; };
    // Single map keyed by client address; task id unique per client lifecycle.
    unordered_map<ClientKey,TaskRec,ClientHash> clients; uint32_t nextId=1; size_t ok=0,fail=0;
    while(g_run){
        fd_set rf; FD_ZERO(&rf); FD_SET(sock,&rf); timeval tv{1,0}; int sel=select(sock+1,&rf,nullptr,nullptr,&tv); if(sel<0){ if(errno==EINTR) continue; perror("select"); break; }
        if(sel==0) continue;
        sockaddr_storage ca{}; socklen_t clen=sizeof(ca); unsigned char buf[128]; ssize_t n=recvfrom(sock,buf,sizeof(buf),0,(sockaddr*)&ca,&clen); if(n<0){ if(errno==EINTR) continue; perror("recvfrom"); continue; }
    ClientKey key{ca,clen}; auto itClient=clients.find(key); DBG(debugPacket(ca,clen,n,itClient==clients.end()));
        if(n==(ssize_t)sizeof(calcMessage)){
            calcMessage cm{}; memcpy(&cm,buf,sizeof(cm)); uint16_t t=ntohs(cm.type); if(ntohs(cm.major_version)==1 && ntohs(cm.minor_version)==1 && (t==21||t==22)){
                bool wantText = (t==21);
                if(itClient==clients.end()){
                    Task tk=makeTask(nextId++); tk.text=wantText; TaskRec rec{tk,ca,clen}; clients[key]=rec; 
                    if(wantText){ 
                        string line=to_string(tk.id)+" "+(tk.op==1?"add":tk.op==2?"sub":tk.op==3?"mul":"div")+" "+to_string(tk.v1)+" "+to_string(tk.v2)+"\n"; 
                        sendto(sock,line.c_str(),line.size(),0,(sockaddr*)&ca,clen);
                    } else { 
                        calcProtocol out{}; out.type=htons(1); out.major_version=htons(1); out.minor_version=htons(1); out.id=htonl(tk.id); out.arith=htonl(tk.op); out.inValue1=htonl(tk.v1); out.inValue2=htonl(tk.v2); 
                        sendto(sock,&out,sizeof(out),0,(sockaddr*)&ca,clen);
                    } 
                    DBG(std::cout<<"TASK id="<<tk.id<<" op="<<tk.op<<" v1="<<tk.v1<<" v2="<<tk.v2<<(wantText?" TEXT":" BIN")<<" from="<<addrStr(ca)<<"\n");
                } else { 
                    TaskRec &rec=itClient->second; Task &tk=rec.task; 
                    if(!tk.done){ 
                        if(tk.text){ 
                            string line=to_string(tk.id)+" "+(tk.op==1?"add":tk.op==2?"sub":tk.op==3?"mul":"div")+" "+to_string(tk.v1)+" "+to_string(tk.v2)+"\n"; 
                            sendto(sock,line.c_str(),line.size(),0,(sockaddr*)&ca,clen);
                        } else { 
                            calcProtocol out{}; out.type=htons(1); out.major_version=htons(1); out.minor_version=htons(1); out.id=htonl(tk.id); out.arith=htonl(tk.op); out.inValue1=htonl(tk.v1); out.inValue2=htonl(tk.v2); 
                            sendto(sock,&out,sizeof(out),0,(sockaddr*)&ca,clen);
                        } 
                    } 
                }
                continue;
            }
        }
        if(n>=(ssize_t)24 && n<=(ssize_t)sizeof(calcProtocol)){
            calcProtocol cp{}; memcpy(&cp,buf,std::min<size_t>(n,sizeof(cp)));
            if(ntohs(cp.major_version)==1 && ntohs(cp.minor_version)==1){
                uint16_t t=ntohs(cp.type); uint32_t id=ntohl(cp.id);
                if(t==2 && id>0){
                    // Find client by scanning (expected small number of active clients)
                    Task *found=nullptr; ClientKey foundKey; 
                    for(auto &kv:clients){ if(kv.second.task.id==id){ found=&kv.second.task; foundKey=kv.first; break; } }
                    if(found){
                        Task &tk=*found;
                        auto age = chrono::duration_cast<chrono::seconds>(Clock::now()-tk.created).count();
                        bool expired = age>10; // spec: remove task if >10s
                        if(!tk.done){
                            bool okAns = (!expired) && (ntohl(cp.inResult)==eval(tk));
                            tk.done=true; tk.ok=okAns;
                            calcMessage m{}; m.type=htons(2); m.message=htonl(okAns?1:2); m.protocol=htons(17); m.major_version=htons(1); m.minor_version=htons(1);
                            sendto(sock,&m,sizeof(m),0,(sockaddr*)&ca,clen);
                            DBG(std::cout<<"ANS id="<<id<<" ok="<<okAns<<(expired?" EXPIRED":"")<<" from="<<addrStr(ca)<<"\n");
                            if(okAns) ++ok; else ++fail;
                        }
                        clients.erase(foundKey);
                    } else {
                        // Unknown or expired/removed id: explicit NOT OK rejection
                        calcMessage m{}; m.type=htons(2); m.message=htonl(2); m.protocol=htons(17); m.major_version=htons(1); m.minor_version=htons(1);
                        sendto(sock,&m,sizeof(m),0,(sockaddr*)&ca,clen);
                        DBG(std::cout<<"ANS id="<<id<<" late-or-unknown NOTOK from="<<addrStr(ca)<<"\n");
                    }
                    continue;
                }
            }
        }
        // Text protocol handling
        {
            string msg((char*)buf,(size_t)n);
            while(!msg.empty()&&(msg.back()=='\n'||msg.back()=='\r')) msg.pop_back();
            if(msg=="TEXT UDP 1.1"){
                if(itClient==clients.end()){
                    Task tk=makeTask(nextId++); tk.text=true; TaskRec rec{tk,ca,clen}; clients[key]=rec;
                    string line=to_string(tk.id)+" "+(tk.op==1?"add":tk.op==2?"sub":tk.op==3?"mul":"div")+" "+to_string(tk.v1)+" "+to_string(tk.v2)+"\n";
                    sendto(sock,line.c_str(),line.size(),0,(sockaddr*)&ca,clen);
                    DBG(std::cout<<"TASK id="<<tk.id<<" text from="<<addrStr(ca)<<"\n");
                } else { TaskRec &rec=itClient->second; Task &tk=rec.task; if(!tk.done){ string line=to_string(tk.id)+" "+(tk.op==1?"add":tk.op==2?"sub":tk.op==3?"mul":"div")+" "+to_string(tk.v1)+" "+to_string(tk.v2)+"\n"; sendto(sock,line.c_str(),line.size(),0,(sockaddr*)&ca,clen);} }
                continue;
            }
            if(itClient!=clients.end()){
                TaskRec &rec=itClient->second; Task &tk=rec.task; 
                bool handled=false; 
                size_t sp=msg.find(' ');
                if(sp!=string::npos){
                    bool pars=true; uint32_t rid=0; long long ans=0; try{ rid=stoul(msg.substr(0,sp)); ans=stoll(msg.substr(sp+1)); }catch(...){ pars=false; }
                    if(pars && rid==tk.id && !tk.done){
                        auto age=chrono::duration_cast<chrono::seconds>(Clock::now()-tk.created).count(); bool expired=age>10;
                        bool okAns = (!expired) && (ans==eval(tk));
                        tk.done=true; tk.ok=okAns; handled=true;
                        string resp=(okAns?"OK ":"NOT OK ");
                        sendto(sock,resp.c_str(),resp.size(),0,(sockaddr*)&ca,clen);
                        DBG(std::cout<<"ANS id="<<tk.id<<" ok="<<okAns<<(expired?" EXPIRED":"")<<" from="<<addrStr(ca)<<"\n");
                        if(okAns) ++ok; else ++fail;
                        clients.erase(key);
                    }
                }
                if(!handled && !tk.done){
                    // Try interpreting whole msg as just the answer (no id provided)
                    bool pars=true; long long ans=0; try{ ans=stoll(msg); }catch(...){ pars=false; }
                    if(pars){
                        auto age=chrono::duration_cast<chrono::seconds>(Clock::now()-tk.created).count(); bool expired=age>10;
                        bool okAns = (!expired) && (ans==eval(tk));
                        tk.done=true; tk.ok=okAns; handled=true;
                        string resp=(okAns?"OK ":"NOT OK ");
                        sendto(sock,resp.c_str(),resp.size(),0,(sockaddr*)&ca,clen);
                        DBG(std::cout<<"ANS id="<<tk.id<<" ok="<<okAns<<(expired?" EXPIRED":"")<<" from="<<addrStr(ca)<<"\n");
                        if(okAns) ++ok; else ++fail; clients.erase(key);
                    }
                }
            }
    }

        // Periodic cleanup of expired tasks (no answer within 10s)
        for(auto it=clients.begin(); it!=clients.end();){
            Task &tk=it->second.task;
            if(!tk.done){ auto age=chrono::duration_cast<chrono::seconds>(Clock::now()-tk.created).count(); if(age>10){ it=clients.erase(it); continue; } }
            ++it;
        }
    }
    close(sock);
    return 0;
}

