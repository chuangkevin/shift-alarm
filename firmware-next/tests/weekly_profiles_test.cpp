#include "../main/weekly_profiles.h"
#include <cassert>

int main(){
 JsonDocument doc;
 for(const char *person:{"kevin","qingqing"}){
  auto profile=doc[person].to<JsonObject>();
  profile["regular_times"].to<JsonArray>().add("07:00");
  profile["thursday_times"].to<JsonArray>().add("08:15");
  profile["disabled_regular_times"].to<JsonArray>();
  profile["disabled_thursday_times"].to<JsonArray>();
 }
 assert(weeklyprofiles::valid(doc.as<JsonVariantConst>()));
 assert(weeklyprofiles::enabledCount(doc.as<JsonVariantConst>())==4);
 doc["kevin"]["disabled_thursday_times"].add("08:15");
 assert(weeklyprofiles::valid(doc.as<JsonVariantConst>()));
 assert(weeklyprofiles::enabledCount(doc.as<JsonVariantConst>())==3);
 doc["qingqing"]["disabled_regular_times"].add("09:00");
 assert(!weeklyprofiles::valid(doc.as<JsonVariantConst>()));
 return 0;
}
