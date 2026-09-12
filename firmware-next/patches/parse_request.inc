// Alarm appliance: bounded HTTP/1.1 subset, deliberately no uploads/chunking.
#include "alarm_http_request.h"
extern "C" bool alarm_http_headers_allowed(const char *,const char *,const char *,const char *) __attribute__((weak));
bool WebServer::_parseRequest(NetworkClient &client) {
  struct Input {NetworkClient &c;int read(){return c.read();}uint32_t now(){return millis();}void idle(){delay(1);}} input{client};
  alarm_http::Parser<Input> parser(input);alarm_http::Request request;
  if(!parser.headers(request)){client.stop();return false;}
  if(_collectAllHeaders)collectAllHeaders();
  else for(RequestArgument *h=_currentHeaders;h;h=h->next)h->value=String();
  _hostHeader=request.host.c_str();_clientContentLength=request.length;
  _currentVersion=1;_chunked=false;
  _currentMethod=HTTP_ANY;
  for(size_t i=0;i<sizeof(_http_method_str)/sizeof(*_http_method_str);i++)if(request.method==_http_method_str[i]){_currentMethod=(HTTPMethod)i;break;}
  if(_currentMethod==HTTP_ANY){client.stop();return false;}
  size_t q=request.target.find('?');std::string path=request.target.substr(0,q);
  std::string args=q==std::string::npos?"":request.target.substr(q+1);
  _currentUri=path.c_str();std::string authorization;
  for(auto &h:request.headers){_collectHeader(h.name.c_str(),h.value.c_str());if(h.name=="authorization")authorization=h.value;}
  if(!alarm_http_headers_allowed||!alarm_http_headers_allowed(request.host.c_str(),path.c_str(),request.method.c_str(),authorization.c_str())){client.stop();return false;}
  if(!alarm_http::bounded_args(args)){client.stop();return false;}
  std::string body;if(!parser.body(request,body)){client.stop();return false;}
  if(request.encoded){if(!args.empty()&&!body.empty())args+='&';args+=body;}
  if(!alarm_http::bounded_args(args)){client.stop();return false;}
  _parseArguments(String(args.c_str()));
  if(!request.encoded&&!body.empty()){RequestArgument &arg=_currentArgs[_currentArgCount++];arg.key="plain";arg.value=String(body.c_str());}
  _currentHandler=nullptr;
  for(RequestHandler *handler=_firstHandler;handler;handler=handler->next())if(handler->canHandle(*this,_currentMethod,_currentUri)){_currentHandler=handler;break;}
  return true;
}
