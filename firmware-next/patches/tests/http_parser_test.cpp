#include "../alarm_http_request.h"
#include <cassert>
#include <iostream>
struct Fake {std::string input;size_t pos=0;uint32_t ms=0;uint32_t each=0;int read(){if(pos==input.size())return -1;ms+=each;return (unsigned char)input[pos++];}uint32_t now(){return ms;}void idle(){++ms;}};
bool parse(std::string raw){Fake f{raw};alarm_http::Parser<Fake> p(f);alarm_http::Request r;std::string body;return p.headers(r)&&p.body(r,body);}
int main(){
 assert(parse("GET / HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n"));
 assert(parse("POST /setup HTTP/1.1\r\nHost: 192.168.4.1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 6\r\n\r\na=1234"));
 for(auto h:{"Content-Length: 98305","Content-Length: -1","Content-Length: 18446744073709551616","Transfer-Encoding: chunked","Expect: 100-continue","Content-Length: 0\r\nContent-Length: 0","Host: evil","Content-Type: multipart/form-data"})assert(!parse(std::string("POST /setup HTTP/1.1\r\nHost: 192.168.4.1\r\n")+h+"\r\n\r\n"));
 assert(!parse("GET / HTTP/1.1\r\nHost: x\r\nX: "+std::string(9000,'x')+"\r\n\r\n"));
 {Fake f{"GET / HTTP/1.1\r\nHost: x\r\n\r\n"};f.each=100;alarm_http::Parser<Fake> p(f);alarm_http::Request r;assert(!p.headers(r));assert(f.ms<=2100);}
 {Fake f{"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 98304\r\n\r\nx"};alarm_http::Parser<Fake> p(f);alarm_http::Request r;std::string b;assert(p.headers(r));assert(!p.body(r,b));assert(f.ms==2000);}
 {Fake f{"POST /api/schedule HTTP/1.1\r\nHost: x\r\nContent-Length: 98304\r\n\r\n"+std::string(98304,'x')};alarm_http::Parser<Fake> p(f);alarm_http::Request r;assert(p.headers(r));assert(f.pos<f.input.size());std::string b;assert(p.body(r,b));assert(b.size()==98304);}
 assert(!alarm_http::bounded_args(std::string(32,'&')));
 assert(alarm_http::bounded_args("nonce=abc&ssid=test&password=foo"));
 std::cout<<"bounded HTTP parser: valid forms/JSON, headers, framing, limits, early gate, total deadline PASS\n";
}
