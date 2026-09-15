#include "proxy_http.h"
#include <cassert>
#include <iostream>
using alarm_proxy::Request;
static bool parse(const std::string &head,Request &r) {
    return alarm_proxy::rewrite_request(head,"192.168.18.55:8080","100.126.226.79:8237",r);
}
static std::string post(std::string extra) {
    return "POST /api/import HTTP/1.1\r\nHost: 192.168.18.55:8080\r\n"+extra+"\r\n";
}
int main() {
    assert(!alarm_proxy::remote_path("/schedule"));assert(alarm_proxy::remote_path("/api/import"));assert(alarm_proxy::remote_path("/static/app.js"));
    assert(!alarm_proxy::remote_path("/"));assert(!alarm_proxy::remote_path("/display"));assert(!alarm_proxy::remote_path("/calendar"));assert(!alarm_proxy::remote_path("/tailnet"));assert(!alarm_proxy::remote_path("/api/local-calendar?month=2026-07"));assert(!alarm_proxy::remote_path("/api/status"));assert(alarm_proxy::remote_path("/api/device/firmware/test.bin"));assert(alarm_proxy::remote_path("/api/device/heartbeat"));assert(alarm_proxy::remote_path("/api/device/update"));

    unsigned status=0;
    assert(alarm_proxy::response_status("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n",status)&&status==403);
    assert(alarm_proxy::response_status("HTTP/1.0 200 OK\r\n\r\n",status)&&status==200);
    assert(alarm_proxy::response_status("HTTP/1.1 103 Early Hints\r\nLink: </app.js>\r\n\r\n",status)&&status==103);
    assert(!alarm_proxy::response_status("HTTP/1.1 101 Switching Protocols\r\n\r\n",status));
    assert(!alarm_proxy::response_status("HTTP/1.1 100 Continue\r\nContent-Length: 1\r\n\r\n",status));
    assert(!alarm_proxy::response_status("HTTP/1.1 200 OK\n\n",status));
    assert(!alarm_proxy::response_status("HTTP/1.1 200x\r\n\r\n",status));
    assert(!alarm_proxy::response_status("HTTP/1.1 200 OK\r\nBad: x\ny\r\n\r\n",status));
    Request r;
    assert(parse(post("Content-Length: 10485760\r\nOrigin: http://192.168.18.55:8080\r\nX-Alarm-UI: 1\r\nContent-Type: multipart/form-data; boundary=abc\r\n"),r));
    assert(r.content_length==10485760);
    assert(r.header.find("Host: 100.126.226.79:8237\r\n")!=std::string::npos);
    assert(r.header.find("origin: http://100.126.226.79:8237\r\n")!=std::string::npos);
    assert(r.header.find("x-alarm-ui: 1\r\n")!=std::string::npos);
    assert(r.header.find("multipart/form-data; boundary=abc")!=std::string::npos);
    assert(parse(post("Content-Length: 0\r\nOrigin: https://evil.example\r\n"),r));
    assert(r.header.find("origin: https://evil.example")!=std::string::npos);
    assert(r.header.find("x-alarm-ui")==std::string::npos);
    assert(parse(post("Content-Length: 0\r\nOrigin: null\r\n"),r));
    assert(r.header.find("origin: null")!=std::string::npos);
    for(auto value : {"-1","+1","1,1","1 1","12582913","184467440737095516160",""})
        assert(!parse(post(std::string("Content-Length: ")+value+"\r\n"),r));
    assert(parse(post("Content-Length: 12582912\r\n"),r));
    assert(!parse(post("Content-Length: 1\r\nContent-Length: 1\r\n"),r));
    assert(!parse(post("Content-Length: 0\r\nTransfer-Encoding: chunked\r\n"),r));
    assert(!parse(post("Transfer-Encoding: chunked\r\n"),r));
    assert(!parse(post("Content-Length: 0\r\nUpgrade: websocket\r\n"),r));
    assert(!parse(post("Content-Length: 0\r\nExpect: 100-continue\r\n"),r));
    assert(!parse(post("Content-Length: 0\r\nConnection: x-alarm-ui\r\n"),r));
    assert(!parse(post("Content-Length : 0\r\n"),r));
    assert(!parse(post("Content-Length: 0\r\n folded: header\r\n"),r));
    assert(!parse(post("Content-Length: 0\r\nOrigin: null\r\nOrigin: http://192.168.18.55:8080\r\n"),r));
    assert(!parse(post("Content-Length: 0\r\nHost: 192.168.18.55:8080\r\n"),r));
    assert(!parse(post("Content-Length: 0\r\nX-Alarm-UI: 1\r\nX-Alarm-UI: 0\r\n"),r));
    assert(!parse(post(""),r));
    assert(!parse("CONNECT example.com:443 HTTP/1.1\r\nHost: 192.168.18.55:8080\r\n\r\n",r));
    assert(!parse("GET http://evil/ HTTP/1.1\r\nHost: 192.168.18.55:8080\r\n\r\n",r));
    assert(!parse("GET //evil/ HTTP/1.1\r\nHost: 192.168.18.55:8080\r\n\r\n",r));
    assert(!parse("GET / HTTP/1.1\r\nHost: evil.example\r\n\r\n",r));
    assert(!parse("GET / HTTP/1.1\nHost: 192.168.18.55:8080\n\n",r));
    assert(!parse(post("Content-Length: 0\r\nBad: a\nb\r\n"),r));
    assert(!parse(post("Content-Length: 0\r\nBad: "+std::string(16384,'a')+"\r\n"),r));
    assert(parse("GET /static/app.js?x=1 HTTP/1.1\r\nHost: 192.168.18.55:8080\r\nConnection: keep-alive\r\nX-Forwarded-Proto: https\r\nForwarded: host=evil\r\n\r\n",r));
    assert(r.header.find("Connection: close")!=std::string::npos);
    assert(r.header.find("Forwarded")==std::string::npos && r.header.find("forwarded")==std::string::npos);
    assert(!parse(post("Content-Length: 0\r\n")+"GET / HTTP/1.1\r\n\r\n",r));
    std::cout<<"HTTP framing, origin and fixed-destination tests passed\n";
}
