#include "alarm_proxy.h"
#include "proxy_http.h"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <new>
#include <string>
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <fcntl.h>
#include <unistd.h>

namespace {
constexpr uint16_t LISTEN_PORT=80;
constexpr unsigned MAX_CLIENTS=2;
constexpr int64_t REQUEST_US=240LL*1000000;
std::atomic<bool> enabled{false}, running{false}, listening{false};
std::atomic<unsigned> generation{0}, clients{0}, completed{0}, rejected{0};
std::atomic<uint32_t> bound_ip{0};
bool configured=false;
sockaddr_in upstream{};
std::string authority, upstream_host;
struct Session { int fd; unsigned generation; uint32_t local_ip; int64_t deadline; };
bool live(const Session &s) { return enabled.load() && s.generation==generation.load() &&
    esp_timer_get_time()<s.deadline; }
void close_fd(int fd) { if(fd>=0) { shutdown(fd,SHUT_RDWR); close(fd); } }
void timeouts(int fd) {
    timeval tv{1,0};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
}
bool retryable() { return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR; }
bool send_all(int fd,const char *p,size_t n,const Session &s) {
    while(n && live(s)) {
        int count=send(fd,p,n,0);
        if(count>0) {p+=count;n-=size_t(count);}
        else if(count<0 && retryable()) continue;
        else return false;
    }
    return n==0;
}
int receive(int fd,char *buffer,size_t size,const Session &s) {
    while(live(s)) { int n=recv(fd,buffer,size,0); if(n<0 && retryable()) continue; return n; }
    return -1;
}
void error_response(int fd,const char *status,const Session &s) {
    std::string response=std::string("HTTP/1.1 ")+status+"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send_all(fd,response.data(),response.size(),s);
}
std::string ip_string(uint32_t addr) {
    char text[INET_ADDRSTRLEN]{};
    in_addr a{};a.s_addr=addr;inet_ntop(AF_INET,&a,text,sizeof(text));return text;
}
int connect_upstream(const Session &s,bool local) {
    sockaddr_in destination=upstream;
    inet_pton(AF_INET,alarm_proxy_backend_host(),&destination.sin_addr);
    if(local){destination.sin_addr.s_addr=htonl(INADDR_LOOPBACK);destination.sin_port=htons(8081);}
    int fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(fd<0)return -1;
    int flags=fcntl(fd,F_GETFL,0);fcntl(fd,F_SETFL,flags|O_NONBLOCK);
    int result=connect(fd,reinterpret_cast<const sockaddr*>(&destination),sizeof(destination));
    if(result<0 && errno!=EINPROGRESS) {close_fd(fd);return -1;}
    const int64_t limit=esp_timer_get_time()+10LL*1000000;
    while(result<0 && live(s) && esp_timer_get_time()<limit) {
        fd_set writes;FD_ZERO(&writes);FD_SET(fd,&writes);timeval tv{1,0};
        int ready=select(fd+1,nullptr,&writes,nullptr,&tv);
        if(ready<0 && errno==EINTR)continue;
        if(ready<0)break;
        if(ready>0) {int err=0;socklen_t length=sizeof(err);if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&length)==0 && err==0)result=0;break;}
    }
    if(result<0 || !live(s)){close_fd(fd);return -1;}
    fcntl(fd,F_SETFL,flags);timeouts(fd);return fd;
}
// Upload and response headers are pumped together. A server may reject an
// upload before consuming its body (403/413); never block writing that body
// while its final response is waiting to be read.
bool upload_until_response(int remote, Session &s, char *pending, size_t pending_size,
                           uint64_t remaining, std::string &response, uint64_t &returned) {
    int flags=fcntl(remote,F_GETFL,0);
    if(flags<0 || fcntl(remote,F_SETFL,flags|O_NONBLOCK)<0)return false;
    struct RestoreFlags { int fd,flags; ~RestoreFlags(){fcntl(fd,F_SETFL,flags);} } restore{remote,flags};
    size_t pending_offset=0;
    char incoming[512];
    while(live(s)) {
        fd_set reads,writes;FD_ZERO(&reads);FD_ZERO(&writes);FD_SET(remote,&reads);
        if(pending_offset<pending_size)FD_SET(remote,&writes);
        else if(remaining)FD_SET(s.fd,&reads);
        timeval tv{1,0};int ready=select((remote>s.fd?remote:s.fd)+1,&reads,&writes,nullptr,&tv);
        if(ready<0){if(errno==EINTR)continue;return false;}
        if(!ready)continue;
        if(FD_ISSET(remote,&reads)) {
            int n=recv(remote,incoming,sizeof(incoming),0);
            if(n==0)return false;
            if(n<0){if(!retryable())return false;}
            else {
                response.append(incoming,size_t(n));
                for(;;) {
                    size_t boundary=response.find("\r\n\r\n");
                    if(boundary==std::string::npos){if(response.size()>alarm_proxy::MAX_HEADER)return false;break;}
                    boundary+=4;unsigned status=0;
                    if(!alarm_proxy::response_status(response.substr(0,boundary),status))return false;
                    if(status>=200)return true; // Stop consuming/sending upload bytes immediately.
                    if(returned+boundary>alarm_proxy::MAX_BODY+alarm_proxy::MAX_HEADER ||
                       !send_all(s.fd,response.data(),boundary,s))return false;
                    returned+=boundary;response.erase(0,boundary); // 100/103 may precede a final response.
                }
            }
        }
        if(FD_ISSET(remote,&writes) && pending_offset<pending_size) {
            int n=send(remote,pending+pending_offset,pending_size-pending_offset,0);
            if(n>0)pending_offset+=size_t(n);
            else if(n==0 || !retryable())return false;
        }
        if(FD_ISSET(s.fd,&reads) && pending_offset==pending_size && remaining) {
            size_t wanted=remaining<4096?size_t(remaining):4096;
            int n=recv(s.fd,pending,wanted,0);
            if(n>0){pending_offset=0;pending_size=size_t(n);remaining-=uint64_t(n);}
            else if(n==0 || !retryable())return false;
        }
    }
    return false;
}
bool relay(Session &s) {
    char buffer[4096];std::string head;
    size_t boundary=std::string::npos;
    while(boundary==std::string::npos) {
        int n=receive(s.fd,buffer,sizeof(buffer),s);if(n<=0)return false;
        head.append(buffer,size_t(n));boundary=head.find("\r\n\r\n");
        if((boundary==std::string::npos && head.size()>alarm_proxy::MAX_HEADER) ||
           (boundary!=std::string::npos && boundary+4>alarm_proxy::MAX_HEADER)) {error_response(s.fd,"431 Request Header Fields Too Large",s);return false;}
    }
    boundary+=4;
    alarm_proxy::Request request;
    // Validate all framing before choosing a fixed route. Never proxy arbitrary destinations.
    const std::string lan=ip_string(s.local_ip);
    if(!alarm_proxy::rewrite_request(head.substr(0,boundary),lan,lan,request)) {
        error_response(s.fd,"400 Bad Request",s);return false;
    }
    size_t first=request.header.find(' '),last=request.header.find(' ',first+1);
    std::string path=request.header.substr(first+1,last-first-1);
    request.local=!alarm_proxy::remote_path(path);
    if(!request.local){
        if(!alarm_proxy::rewrite_request(head.substr(0,boundary),lan,std::string(alarm_proxy_backend_host())+":"+std::to_string(ntohs(upstream.sin_port)),request)){error_response(s.fd,"400 Bad Request",s);return false;}
    }else if(request.content_length>98304){error_response(s.fd,"413 Payload Too Large",s);return false;}
    size_t buffered_body=head.size()-boundary;
    // Reject pipelining and bytes after the declared body before forwarding any data.
    if(buffered_body>request.content_length){error_response(s.fd,"400 Bad Request",s);return false;}
    int remote=connect_upstream(s,request.local);
    if(remote<0){
      const std::string body=request.local?"裝置設定服務暫時無法回應，請稍後重試":"裝置無法連上班表辨識伺服器，請確認伺服器及資料通道；Tailscale 授權狀態不受影響。";
      std::string response="HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: "+std::to_string(body.size())+"\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"+body;
      send_all(s.fd,response.data(),response.size(),s);return false;
    }
    bool ok=send_all(remote,request.header.data(),request.header.size(),s);
    if(buffered_body)std::memcpy(buffer,head.data()+boundary,buffered_body);
    uint64_t remaining=request.content_length-buffered_body;
    head.clear();
    uint64_t returned=0;
    if(ok)ok=upload_until_response(remote,s,buffer,buffered_body,remaining,head,returned);
    if(ok) {
        if(returned+head.size()>alarm_proxy::MAX_BODY+alarm_proxy::MAX_HEADER)ok=false;
        else {returned+=head.size();ok=send_all(s.fd,head.data(),head.size(),s);}
    }
    while(ok) {
        int n=receive(remote,buffer,sizeof(buffer),s);
        if(n==0)break;
        if(n<0 || returned+uint64_t(n)>alarm_proxy::MAX_BODY+alarm_proxy::MAX_HEADER){ok=false;break;}
        returned+=uint64_t(n);ok=send_all(s.fd,buffer,size_t(n),s);
    }
    close_fd(remote);return ok && returned>0;
}
void worker(void *arg) {
    Session *s=static_cast<Session*>(arg);
    if(relay(*s))completed.fetch_add(1);else rejected.fetch_add(1);
    close_fd(s->fd);delete s;clients.fetch_sub(1);vTaskDelete(nullptr);
}
bool sta(esp_netif_ip_info_t &ip) {
    auto *net=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return net && esp_netif_is_netif_up(net) && esp_netif_get_ip_info(net,&ip)==ESP_OK && ip.ip.addr && ip.netmask.addr;
}
bool accepted_peer(uint32_t destination,uint32_t source){
 for(const char *key:{"WIFI_STA_DEF","WIFI_AP_DEF"}){
  auto *net=esp_netif_get_handle_from_ifkey(key);esp_netif_ip_info_t ip{};
  if(net&&esp_netif_is_netif_up(net)&&esp_netif_get_ip_info(net,&ip)==ESP_OK&&ip.ip.addr==destination&&ip.netmask.addr)
   return (source&ip.netmask.addr)==(destination&ip.netmask.addr);
 }
 // Native WireGuard already applies the tailnet ACL to decrypted packets.
 return (ntohl(destination)&0xffc00000u)==0x64400000u&&(ntohl(source)&0xffc00000u)==0x64400000u;
}
void listener(void *) {
    int fd=-1;const unsigned epoch=generation.load();
    while(enabled.load() && generation.load()==epoch) {
        esp_netif_ip_info_t ip{};if(sta(ip))bound_ip=ip.ip.addr;else bound_ip=0;
        if(fd<0){
            fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
            if(fd>=0){int yes=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
             sockaddr_in local{};local.sin_family=AF_INET;local.sin_port=htons(LISTEN_PORT);local.sin_addr.s_addr=htonl(INADDR_ANY);
             if(bind(fd,reinterpret_cast<sockaddr*>(&local),sizeof(local))<0||listen(fd,MAX_CLIENTS)<0){close_fd(fd);fd=-1;}else listening=true;
            }
        }
        if(fd<0){vTaskDelay(pdMS_TO_TICKS(250));continue;}
        fd_set reads;FD_ZERO(&reads);FD_SET(fd,&reads);timeval tv{1,0};
        if(select(fd+1,&reads,nullptr,nullptr,&tv)<=0)continue;
        sockaddr_in peer{},local{};socklen_t length=sizeof(peer);int client=accept(fd,reinterpret_cast<sockaddr*>(&peer),&length);
        if(client<0){continue;}length=sizeof(local);
        if(getsockname(client,reinterpret_cast<sockaddr*>(&local),&length)<0||!accepted_peer(local.sin_addr.s_addr,peer.sin_addr.s_addr)||clients.load()>=MAX_CLIENTS||!enabled.load()){
            rejected.fetch_add(1);close_fd(client);continue;
        }
        timeouts(client);auto *session=new(std::nothrow) Session{client,epoch,local.sin_addr.s_addr,esp_timer_get_time()+REQUEST_US};
        if(!session){close_fd(client);continue;}clients.fetch_add(1);
        if(xTaskCreate(worker,"alarm_proxy_io",8192,session,3,nullptr)!=pdPASS){clients.fetch_sub(1);delete session;close_fd(client);}
    }
    close_fd(fd);bound_ip=0;listening=false;running=false;vTaskDelete(nullptr);
}
}
// The same service has a fixed LAN address. Prefer it only on its actual subnet;
// never accept a destination supplied by a browser or bypass Tailnet ACLs.
extern "C" const char *alarm_proxy_backend_host(void) {
    esp_netif_ip_info_t ip{};
    const uint32_t lan=inet_addr("192.168.18.31");
    if(upstream.sin_addr.s_addr==inet_addr("100.126.226.79") && sta(ip) &&
       (ip.ip.addr&ip.netmask.addr)==(lan&ip.netmask.addr))return "192.168.18.31";
    return upstream_host.c_str();
}
extern "C" esp_err_t alarm_proxy_init(const char *ip,uint16_t port) {
    if(enabled.load() || running.load() || clients.load())return ESP_ERR_INVALID_STATE;
    in_addr addr{};
    if(!ip || !port || inet_pton(AF_INET,ip,&addr)!=1)return ESP_ERR_INVALID_ARG;
    const uint32_t n=ntohl(addr.s_addr);
    if(n==0 || (n>>24)==127 || (n>>24)>=224 || n==0xffffffffu)return ESP_ERR_INVALID_ARG;
    upstream={};upstream.sin_family=AF_INET;upstream.sin_port=htons(port);upstream.sin_addr=addr;
    upstream_host=ip;authority=std::string(ip)+":"+std::to_string(port);configured=true;return ESP_OK;
}
extern "C" esp_err_t alarm_proxy_start(void) {
    if(!configured)return ESP_ERR_INVALID_STATE;
    if(enabled.load())return ESP_OK;
    if(running.load() || clients.load())return ESP_ERR_INVALID_STATE;
    generation.fetch_add(1);enabled=true;running=true;
    if(xTaskCreate(listener,"alarm_proxy",4096,nullptr,3,nullptr)!=pdPASS){enabled=false;running=false;return ESP_ERR_NO_MEM;}
    return ESP_OK;
}
extern "C" esp_err_t alarm_proxy_stop(void) {enabled=false;generation.fetch_add(1);return ESP_OK;}
extern "C" esp_err_t alarm_proxy_get_status(alarm_proxy_status_t *out) {
    if(!out)return ESP_ERR_INVALID_ARG;
    *out={};out->enabled=enabled.load();out->listening=listening.load();out->port=LISTEN_PORT;
    out->active_connections=clients.load();out->completed_requests=completed.load();out->rejected_requests=rejected.load();
    const auto ip=ip_string(bound_ip.load());std::strncpy(out->lan_ip,ip.c_str(),sizeof(out->lan_ip)-1);return ESP_OK;
}
