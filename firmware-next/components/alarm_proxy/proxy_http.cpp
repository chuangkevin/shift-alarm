#include "proxy_http.h"
#include <algorithm>
#include <set>
#include <utility>
#include <vector>
namespace alarm_proxy {
static std::string lower(std::string s) {
    for (char &c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
static std::string trim(const std::string &s) {
    const auto first = s.find_first_not_of(" \t");
    return first == std::string::npos ? "" : s.substr(first, s.find_last_not_of(" \t") - first + 1);
}
static bool token(const std::string &s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || std::string("!#$%&'*+-.^_`|~").find(c) != std::string::npos)) return false;
    return true;
}
bool response_status(const std::string &input, unsigned &status) {
    if (input.size()>MAX_HEADER || input.size()<4 || input.substr(input.size()-4)!="\r\n\r\n") return false;
    size_t end=input.find("\r\n"); if(end==std::string::npos)return false;
    const std::string line=input.substr(0,end);
    if(line.size()<12 || (line.compare(0,9,"HTTP/1.1 ")!=0 && line.compare(0,9,"HTTP/1.0 ")!=0) ||
       (line.size()>12 && line[12]!=' '))return false;
    status=0;for(size_t i=9;i<12;i++){if(line[i]<'0'||line[i]>'9')return false;status=status*10+unsigned(line[i]-'0');}
    if(status<100 || status>599 || status==101)return false;
    for(unsigned char c:line)if(c<32||c==127)return false;
    size_t pos=end+2;
    while(pos<input.size()-2) {
        end=input.find("\r\n",pos);if(end==std::string::npos||end==pos)return false;
        std::string row=input.substr(pos,end-pos);size_t colon=row.find(':');
        if(colon==std::string::npos || !token(row.substr(0,colon)))return false;
        std::string name=lower(row.substr(0,colon)),value=trim(row.substr(colon+1));
        for(unsigned char c:value)if((c<32&&c!='\t')||c==127)return false;
        if(status<200 && (name=="content-length"||name=="transfer-encoding"))return false;
        pos=end+2;
    }
    return pos==input.size()-2;
}
bool rewrite_request(const std::string &input, const std::string &lan,
                     const std::string &backend, Request &out) {
    out = Request{};
    if (input.size() > MAX_HEADER || input.size() < 4 || input.substr(input.size()-4) != "\r\n\r\n") return false;
    size_t end = input.find("\r\n");
    if (end == std::string::npos) return false;
    const std::string line = input.substr(0, end);
    size_t a = line.find(' '), b = line.find(' ', a == std::string::npos ? 0 : a+1);
    if (a == std::string::npos || b == std::string::npos || line.find(' ', b+1) != std::string::npos) return false;
    const std::string method = line.substr(0,a), target = line.substr(a+1,b-a-1), protocol = line.substr(b+1);
    if (method != "GET" && method != "HEAD" && method != "POST" && method != "PUT" && method != "DELETE" && method != "PATCH" && method != "OPTIONS") return false;
    if (protocol != "HTTP/1.1" && protocol != "HTTP/1.0") return false;
    if (target.empty() || target[0] != '/' || target.compare(0,2,"//") == 0) return false;
    for (unsigned char c : target) if (c <= 32 || c == 127 || c == '#' || c == '\\') return false;
    std::set<std::string> unique;
    std::vector<std::pair<std::string,std::string>> headers;
    bool host = false, cl = false;
    size_t pos = end+2;
    while (pos < input.size()-2) {
        end = input.find("\r\n", pos);
        if (end == std::string::npos || end == pos) return false;
        std::string row = input.substr(pos,end-pos);
        size_t colon = row.find(':');
        if (colon == std::string::npos || !token(row.substr(0,colon))) return false;
        std::string name = lower(row.substr(0,colon)), value = trim(row.substr(colon+1));
        for (unsigned char c : value) if ((c < 32 && c != '\t') || c == 127) return false;
        if (name == "transfer-encoding" || name == "upgrade" || name == "expect") return false;
        if (name == "host" || name == "content-length" || name == "origin" || name == "x-alarm-ui")
            if (!unique.insert(name).second) return false;
        if (name == "host") { if (value != lan) return false; host = true; }
        else if (name == "content-length") {
            if (value.empty()) return false;
            uint64_t n=0;
            for (char c : value) { if (c < '0' || c > '9' || n > (MAX_BODY-uint64_t(c-'0'))/10) return false; n=n*10+uint64_t(c-'0'); }
            out.content_length=n; cl=true;
        } else if (name == "connection") {
            const auto v=lower(value);
            if (v != "close" && v != "keep-alive") return false;
        } else if (name == "proxy-connection" || name == "keep-alive" || name == "forwarded" || name.compare(0,12,"x-forwarded-") == 0 || name == "proxy-authorization") {
            // Never allow caller-supplied proxy metadata to affect backend trust.
        } else {
            if (name == "origin" && value == "http://"+lan) value="http://"+backend;
            headers.emplace_back(name,value);
        }
        pos=end+2;
    }
    if (!host || pos != input.size()-2) return false;
    if ((method == "POST" || method == "PUT" || method == "PATCH") && !cl) return false;
    out.header=method+" "+target+" HTTP/1.1\r\nHost: "+backend+"\r\nConnection: close\r\n";
    if (cl) out.header+="Content-Length: "+std::to_string(out.content_length)+"\r\n";
    for (const auto &h : headers) out.header+=h.first+": "+h.second+"\r\n";
    out.header+="\r\n";
    return true;
}
}
