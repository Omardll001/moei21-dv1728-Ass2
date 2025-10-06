#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <fstream>
#include <chrono>
#include <random>
#include "protocol.h"

using namespace std; using Clock=chrono::steady_clock;

int main(int argc,char*argv[]){
  if(argc<4){ cerr<<"usage: host:port tests dropProb outputFile\n"; return 1; }
  string hp=argv[1]; auto pos=hp.rfind(':'); if(pos==string::npos){ cerr<<"bad host:port\n"; return 1; }
  string host=hp.substr(0,pos), port=hp.substr(pos+1);
  int tests=stoi(argv[2]); int dropProb=stoi(argv[3]); string outFile=(argc>4?argv[4]:"client_results.log");
  ofstream ofs(outFile);
  std::mt19937 rng((unsigned)time(nullptr)); std::uniform_int_distribution<int> drop(0,99);
  struct addrinfo hints{}; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_DGRAM; struct addrinfo*res=nullptr; if(getaddrinfo(host.c_str(),port.c_str(),&hints,&res)!=0){ perror("getaddrinfo"); return 1; }
  int s=-1; sockaddr_storage srv{}; socklen_t slen=0; for(auto*p=res;p;p=p->ai_next){ s=socket(p->ai_family,p->ai_socktype,p->ai_protocol); if(s<0) continue; memcpy(&srv,p->ai_addr,p->ai_addrlen); slen=p->ai_addrlen; break; }
  freeaddrinfo(res); if(s<0){ cerr<<"no socket\n"; return 1; }
  // Send handshake (binary) a few times to cope with drops
  for(int i=0;i<5;i++){ calcMessage m{}; m.type=htons(22); m.message=htonl(1); m.protocol=htons(17); m.major_version=htons(1); m.minor_version=htons(1); sendto(s,&m,sizeof(m),0,(sockaddr*)&srv,slen); usleep(20000); }
  vector<int> answered; answered.reserve(tests);
  auto deadline=Clock::now()+chrono::seconds(60);
  while((int)answered.size()<tests && Clock::now()<deadline){ fd_set rf; FD_ZERO(&rf); FD_SET(s,&rf); timeval tv{0,100000}; int r=select(s+1,&rf,nullptr,nullptr,&tv); if(r>0 && FD_ISSET(s,&rf)){
      unsigned char buf[128]; sockaddr_storage from{}; socklen_t flen=sizeof(from); ssize_t n=recvfrom(s,buf,sizeof(buf),0,(sockaddr*)&from,&flen); if(n==(ssize_t)sizeof(calcProtocol)){
        calcProtocol cp{}; memcpy(&cp,buf,sizeof(cp)); if(ntohs(cp.type)==1){ // task
          uint32_t id=ntohl(cp.id); uint32_t arith=ntohl(cp.arith); int32_t v1=ntohl(cp.inValue1), v2=ntohl(cp.inValue2);
          int32_t res=0; switch(arith){case 1: res=v1+v2; break; case 2: res=v1-v2; break; case 3: res=v1*v2; break; case 4: if(v2) res=v1/v2; else res=0; break;}
          if(drop(rng)<dropProb){ continue; } // simulate drop before sending answer
          calcProtocol ans{}; ans.type=htons(2); ans.major_version=htons(1); ans.minor_version=htons(1); ans.id=htonl(id); ans.inResult=htonl(res); sendto(s,&ans,sizeof(ans),0,(sockaddr*)&srv,slen);
        } else if(ntohs(cp.type)==2){ // echo or stray
        }
      } else if(n==(ssize_t)sizeof(calcMessage)){
        // result ack
        calcMessage rm{}; memcpy(&rm,buf,sizeof(rm)); uint32_t state=ntohl(rm.message); if(state==1){ answered.push_back(answered.size()+1); }
      }
    }
    // Periodically request resend if idle
  if(rng()%15==0){ calcProtocol req{}; req.type=htons(2); req.major_version=htons(1); req.minor_version=htons(1); req.id=htonl(0); sendto(s,&req,sizeof(req),0,(sockaddr*)&srv,slen); }
  }
  ofs<<"answered="<<answered.size()<<"\n"; ofs.close();
  close(s); cerr<<"client done answered="<<answered.size()<<"\n"; return 0; }
