#pragma once
#include "calendar_policy.h"
#include <ArduinoJson.h>
#include <set>
#include <string>

namespace weeklyprofiles {
constexpr size_t MAX_TIMES=8;

inline bool validTimes(JsonVariantConst value,std::set<std::string> &out){
 if(!value.is<JsonArrayConst>()||value.size()>MAX_TIMES)return false;
 for(JsonVariantConst item:value.as<JsonArrayConst>()){
  int hour,minute;
  if(!item.is<const char*>()||!localcalendar::time(item.as<const char*>(),hour,minute)||!out.insert(item.as<const char*>()).second)return false;
 }
 return true;
}

inline bool validDisabled(JsonVariantConst value,const std::set<std::string> &times){
 if(!value.is<JsonArrayConst>()||value.size()>MAX_TIMES)return false;
 std::set<std::string> disabled;
 for(JsonVariantConst item:value.as<JsonArrayConst>())
  if(!item.is<const char*>()||!times.count(item.as<const char*>())||!disabled.insert(item.as<const char*>()).second)return false;
 return true;
}

inline bool validProfile(JsonVariantConst value){
 if(!value.is<JsonObjectConst>()||value.size()!=4)return false;
 auto profile=value.as<JsonObjectConst>();std::set<std::string> regular,thursday;
 return validTimes(profile["regular_times"],regular)&&validTimes(profile["thursday_times"],thursday)&&
        validDisabled(profile["disabled_regular_times"],regular)&&validDisabled(profile["disabled_thursday_times"],thursday);
}

inline bool valid(JsonVariantConst value){
 if(!value.is<JsonObjectConst>()||value.size()!=2)return false;
 auto profiles=value.as<JsonObjectConst>();
 return profiles.containsKey("kevin")&&profiles.containsKey("qingqing")&&validProfile(profiles["kevin"])&&validProfile(profiles["qingqing"]);
}

inline size_t enabledCount(JsonVariantConst value){
 if(!valid(value))return 0;
 size_t count=0;
 for(const char *person:{"kevin","qingqing"})for(const char *group:{"regular","thursday"}){
  std::set<std::string> disabled;std::string disabledKey=std::string("disabled_")+group+"_times",timesKey=std::string(group)+"_times";
  for(JsonVariantConst item:value[person][disabledKey].as<JsonArrayConst>())disabled.insert(item.as<const char*>());
  for(JsonVariantConst item:value[person][timesKey].as<JsonArrayConst>())if(!disabled.count(item.as<const char*>()))count++;
 }
 return count;
}
}
