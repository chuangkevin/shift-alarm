#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
namespace alarm_http {
constexpr size_t MAX_BODY=98304, MAX_HEADERS=8192, MAX_LINE=2048;
constexpr uint32_t DEADLINE_MS=2000;
struct Header {std::string name,value;};
struct Request {std::string method,target,host,content_type;std::vector<Header> headers;size_t length=0;bool encoded=false;};
inline std::string lower(std::string s){for(char &c:s)if(c>='A'&&c<='Z')c+=32;return s;}
inline bool token(char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||std::string("!#$%&'*+-.^_`|~").find(c)!=std::string::npos;}
// IO is nonblocking read(), now(), idle(); one deadline spans headers and body.
template<class IO> class Parser {
 IO &io;uint32_t started;size_t header_bytes=0;
 bool line(std::string &s){s.clear();bool cr=false;while(!expired()){
  int c=io.read();if(c<0){io.idle();continue;}if(++header_bytes>MAX_HEADERS)return false;
  if(cr){return c=='\n';}
  if(c=='\r'){cr=true;continue;}
  if(c<32||c>126||s.size()>=MAX_LINE){return false;}
  s.push_back(char(c));
 }return false;}
 public:
 explicit Parser(IO &v):io(v),started(v.now()){}
 bool expired(){return uint32_t(io.now()-started)>=DEADLINE_MS;}
 bool headers(Request &r){r=Request{};std::string s;if(!line(s))return false;
  size_t a=s.find(' '),b=s.find(' ',a==s.npos?0:a+1);
  if(a==s.npos||b==s.npos||s.substr(b+1)!="HTTP/1.1")return false;
  r.method=s.substr(0,a);r.target=s.substr(a+1,b-a-1);
  if(r.method!="GET"&&r.method!="HEAD"&&r.method!="POST"&&r.method!="PUT"&&r.method!="PATCH"&&r.method!="DELETE"&&r.method!="OPTIONS")return false;
  if(r.target.empty()||r.target[0]!='/'||r.target.find('#')!=s.npos)return false;
  bool cl=false,host=false;
  while(true){if(!line(s))return false;if(s.empty())break;if(r.headers.size()>=40)return false;
   size_t colon=s.find(':');if(colon==0||colon==s.npos)return false;
   std::string name=lower(s.substr(0,colon));for(char c:name)if(!token(c))return false;
   std::string value=s.substr(colon+1);size_t first=value.find_first_not_of(' '),last=value.find_last_not_of(' ');value=first==value.npos?"":value.substr(first,last-first+1);
   // No ambiguous duplicate framing, credentials, or CSRF headers.
   for(auto &h:r.headers)if(h.name==name&&(name=="host"||name=="content-length"||name=="content-type"||name=="authorization"||name=="x-setup-nonce"||name=="origin"))return false;
   if(name=="transfer-encoding"||name=="expect")return false;
   if(name=="host"){host=true;r.host=value;if(value.empty())return false;}
   if(name=="content-length"){cl=true;if(value.empty())return false;size_t n=0;for(char c:value){if(c<'0'||c>'9'||n>(MAX_BODY-size_t(c-'0'))/10)return false;n=n*10+size_t(c-'0');}r.length=n;}
   if(name=="content-type"){r.content_type=lower(value);if(r.content_type.find("multipart/")==0)return false;r.encoded=r.content_type.find("application/x-www-form-urlencoded")==0;}
   r.headers.push_back({name,value});
  }
  if(!host)return false;
  bool mutation=r.method=="POST"||r.method=="PUT"||r.method=="PATCH"||r.method=="DELETE";
  if(mutation&&!cl){return false;}
  if(!mutation&&r.length)return false;
  // Local settings forms are tiny; the larger allowance is only for JSON.
  if(r.encoded&&r.length>4096)return false;
  return true;
 }
 bool body(const Request &r,std::string &s){s.clear();s.reserve(r.length);
  while(s.size()<r.length&&!expired()){int c=io.read();if(c<0){io.idle();continue;}if(c==0)return false;s.push_back(char(c));}
  return s.size()==r.length&&!expired();
 }
};
inline bool bounded_args(const std::string &s){return std::count(s.begin(),s.end(),'&')<32;}
}
