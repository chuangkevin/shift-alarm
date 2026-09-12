// Runs the actual relay implementation against POSIX sockets/fake upstream.
// Host shims only replace ESP time, netif discovery, and unused task startup.
#include "../alarm_proxy.cpp"
#include <cassert>
#include <csignal>
#include <iostream>
#include <thread>

static void write_exact(int fd,const std::string &data) {
    size_t done=0;while(done<data.size()) {ssize_t n=send(fd,data.data()+done,data.size()-done,0);assert(n>0);done+=size_t(n);}
}
static std::string read_all(int fd) {
    std::string result;char data[2048];for(;;){ssize_t n=recv(fd,data,sizeof(data),0);if(n<=0)break;result.append(data,size_t(n));}return result;
}
static void scenario(bool upload, bool informational, bool fragmented, bool chunked) {
    int listener_fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);assert(listener_fd>=0);
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);addr.sin_port=0;
    assert(bind(listener_fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0);assert(listen(listener_fd,1)==0);
    socklen_t len=sizeof(addr);assert(getsockname(listener_fd,reinterpret_cast<sockaddr*>(&addr),&len)==0);
    upstream=addr;authority="127.0.0.1:"+std::to_string(ntohs(addr.sin_port));
    enabled=true;unsigned epoch=generation.fetch_add(1)+1;bound_ip=htonl(INADDR_LOOPBACK);
    int pair[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);timeouts(pair[0]);timeouts(pair[1]);
    Session session{pair[1],epoch,bound_ip.load(),esp_timer_get_time()+3LL*1000000};
    bool relayed=false;
    std::string body(256*1024,'Z');
    const std::string final=upload?(chunked?"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n3\r\nhey\r\n0\r\n\r\n":"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK"):
        "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    std::thread backend([&]{
        int peer=accept(listener_fd,nullptr,nullptr);assert(peer>=0);timeouts(peer);
        std::string input;char data[2048];size_t boundary;
        while((boundary=input.find("\r\n\r\n"))==std::string::npos){int n=recv(peer,data,sizeof(data),0);assert(n>0);input.append(data,size_t(n));}
        boundary+=4;
        if(upload){while(input.size()-boundary<body.size()){int n=recv(peer,data,sizeof(data),0);assert(n>0);input.append(data,size_t(n));}assert(input.substr(boundary)==body);}
        // In rejection tests, deliberately NEVER read the advertised 8MiB body.
        if(informational)write_exact(peer,"HTTP/1.1 103 Early Hints\r\nLink: </app.js>; rel=preload\r\n\r\n");
        if(fragmented){write_exact(peer,final.substr(0,10));std::this_thread::sleep_for(std::chrono::milliseconds(10));write_exact(peer,final.substr(10));}
        else write_exact(peer,final);
        close_fd(peer);close_fd(listener_fd);
    });
    std::thread proxy([&]{relayed=relay(session);close_fd(pair[1]);});
    std::string header="POST /api/import HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/octet-stream\r\nContent-Length: "+std::to_string(upload?body.size():8*1024*1024)+"\r\n\r\n";
    const auto start=esp_timer_get_time();write_exact(pair[0],header);
    std::thread producer;
    if(upload)producer=std::thread([&]{for(size_t i=0;i<body.size();i+=1024)write_exact(pair[0],body.substr(i,1024));});
    const std::string response=read_all(pair[0]);
    const auto elapsed=esp_timer_get_time()-start;
    if(producer.joinable()){producer.join();}
    proxy.join();backend.join();close_fd(pair[0]);enabled=false;
    assert(relayed);assert(elapsed<1500000); // Old upload-first relay hits its 3s deadline and fails.
    if(informational)assert(response.find("HTTP/1.1 103 Early Hints\r\n")==0);
    assert(response.size()>=final.size());assert(response.substr(response.size()-final.size())==final);
}
static void local_route(){
 // Production configure() initializes the address family before accepting work.
 // Linux rejects AF_UNSPEC even when the local-route override supplies IP/port.
 upstream={};upstream.sin_family=AF_INET;
 int listener_fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);assert(listener_fd>=0);int yes=1;setsockopt(listener_fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
 sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);addr.sin_port=htons(8081);
 assert(bind(listener_fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0);assert(listen(listener_fd,1)==0);
 int pair[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);timeouts(pair[0]);timeouts(pair[1]);enabled=true;unsigned epoch=generation.fetch_add(1)+1;
 Session session{pair[1],epoch,htonl(INADDR_LOOPBACK),esp_timer_get_time()+3000000};
 std::thread local([&]{int fd=accept(listener_fd,nullptr,nullptr);assert(fd>=0);timeouts(fd);char buf[2048];std::string req;while(req.find("\r\n\r\n")==std::string::npos){int n=recv(fd,buf,sizeof(buf),0);assert(n>0);req.append(buf,size_t(n));}assert(req.find("GET /display HTTP/1.1") == 0);assert(req.find("Host: 127.0.0.1\r\n")!=std::string::npos);write_exact(fd,"HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nLOCAL");close_fd(fd);});
 std::thread gateway([&]{assert(relay(session));close_fd(pair[1]);});
 write_exact(pair[0],"GET /display HTTP/1.1\r\nHost: 127.0.0.1:80\r\n\r\n");auto response=read_all(pair[0]);assert(response.find("LOCAL")!=std::string::npos);
 gateway.join();local.join();close_fd(pair[0]);close_fd(listener_fd);enabled=false;
}
int main() {
    std::signal(SIGPIPE,SIG_IGN);
    local_route();
    scenario(false,false,false,false);
    scenario(false,true,true,false);
    scenario(true,false,true,false);
    scenario(true,false,false,true);
    std::cout<<"Actual relay: early 403, fragmented headers, 103, full upload, plain/chunked response passed\n";
}
