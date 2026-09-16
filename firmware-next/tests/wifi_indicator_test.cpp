#include "../main/wifi_indicator.h"
#include <cassert>
int main(){
 using namespace wifiindicator;
 assert(value(false,false,-40).state==State::Offline);
 assert(value(false,true,-40).state==State::Connecting);
 assert(value(true,true,-55).state==State::Connected);
 assert(value(true,false,-55).bars==4);
 assert(value(true,false,-56).bars==3);
 assert(value(true,false,-67).bars==3);
 assert(value(true,false,-68).bars==2);
 assert(value(true,false,-75).bars==2);
 assert(value(true,false,-76).bars==1);
 assert(value(true,false,0).bars==0);
 assert(value(true,false,-101).bars==0);
 assert(changed(value(true,false,-60),value(false,false,-60)));
 assert(!changed(value(true,false,-60),value(true,false,-62)));
}
