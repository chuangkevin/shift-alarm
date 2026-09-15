#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace backendendpoint {
struct Endpoint { char host[16]{}; uint16_t port=0; };
inline bool parse(const char *url, Endpoint &out) {
  out = {};
  if (!url || strlen(url)>48) return false;
  unsigned a,b,c,d,p; int consumed=0;
  if (sscanf(url,"http://%u.%u.%u.%u:%u%n",&a,&b,&c,&d,&p,&consumed)!=5 ||
      url[consumed]!='\0' || a!=100 || b<64 || b>127 || c>255 || d>255 || p==0 || p>65535) return false;
  char canonical[49];
  snprintf(canonical,sizeof(canonical),"http://%u.%u.%u.%u:%u",a,b,c,d,p);
  if (strcmp(url,canonical)!=0) return false;
  snprintf(out.host,sizeof(out.host),"%u.%u.%u.%u",a,b,c,d);
  out.port=static_cast<uint16_t>(p);
  return true;
}
}
