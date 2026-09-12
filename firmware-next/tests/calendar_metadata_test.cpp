#include "../main/calendar_metadata.h"
#include <cassert>
#include <iostream>
using namespace localcalendar;
int main(){
 JsonDocument schedule;
 assert(!deserializeJson(schedule,R"({"months":{"2026-09":{"days":[],"times":["07:00"]},"2026-10":{"days":[1,3],"times":["08:30"]}}})"));
 assert(validMonths(schedule["months"]));
 std::string stored;serializeJson(schedule,stored);JsonDocument reboot;
 assert(!deserializeJson(reboot,stored));assert(validMonths(reboot["months"]));
 assert(reboot["months"]["2026-09"]["days"].size()==0);
 assert(reboot["months"]["2026-09"]["times"][0]=="07:00");
 JsonDocument next;next["months"]=reboot["months"];
 auto month=next["months"]["2026-09"].to<JsonObject>();
 month["days"].to<JsonArray>();month["times"].to<JsonArray>().add("09:15");
 assert(validMonths(next["months"]));
 assert(next["months"]["2026-10"]["times"][0]=="08:30");
 assert(next["months"]["2026-10"]["days"][1]==3);
 for(const char *bad:{"null","[]",R"({"bad":{"days":[],"times":[]}})",R"({"2026-02":{"days":[29],"times":["07:00"]}})",R"({"2026-09":{"days":[1],"times":[]}})",R"({"2026-09":{"days":[],"times":["24:00"]}})",R"({"2026-09":{"days":[1,1],"times":["07:00"]}})",R"({"2026-09":{"days":[],"times":["07:00","07:00"]}})",R"({"2026-09":{"days":[],"times":[],"extra":1}})"}){
  JsonDocument invalid;assert(!deserializeJson(invalid,bad));assert(!validMonths(invalid.as<JsonVariantConst>()));
 }
 JsonDocument many;
 for(int i=0;i<121;i++){char key[8];snprintf(key,sizeof(key),"%04d-%02d",2024+i/12,1+i%12);auto entry=many[key].to<JsonObject>();entry["days"].to<JsonArray>();entry["times"].to<JsonArray>();if(i==119)assert(validMonths(many.as<JsonVariantConst>()));}
 assert(!validMonths(many.as<JsonVariantConst>()));
 std::cout<<"Monthly metadata roundtrip, independent months, malformed data and capacity passed\n";
}
