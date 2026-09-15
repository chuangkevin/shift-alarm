#include "../main/backend_endpoint.h"
#include <cassert>
#include <cstring>
#include <string>
int main(){
  backendendpoint::Endpoint e;
  assert(backendendpoint::parse("http://100.126.226.79:8237",e));
  assert(e.port==8237 && strcmp(e.host,"100.126.226.79")==0);
  assert(backendendpoint::parse("http://100.126.226.79:8238",e));assert(e.port==8238);
  for(const char *s:{"http://127.0.0.1:8238","http://192.168.1.2:8238","http://100.126.226.79:0","http://100.126.226.79:65536","http://100.126.226.79:8238/path","http://100.126.226.79:08238"}) assert(!backendendpoint::parse(s,e));
  const std::string credentials = std::string("http://100.126.226.79:8238") + "@" + "evil";
  assert(!backendendpoint::parse(credentials.c_str(),e));
}
