#pragma once
#include "calendar_policy.h"
#include <ArduinoJson.h>
namespace localcalendar {
constexpr size_t MAX_MONTHS=120;
inline bool validMonths(JsonVariantConst value){
 if(!value.is<JsonObjectConst>()||value.size()>MAX_MONTHS)return false;
 for(JsonPairConst entry:value.as<JsonObjectConst>()){
  int year,mon;
  if(!month(entry.key().c_str(),year,mon)||!entry.value().is<JsonObjectConst>())return false;
  auto record=entry.value().as<JsonObjectConst>();
  if(record.size()!=2||!record["days"].is<JsonArrayConst>()||!record["times"].is<JsonArrayConst>())return false;
  if(record["days"].size()>31||record["times"].size()>8||(record["days"].size()&&!record["times"].size()))return false;
  std::set<int> selected;std::set<std::string> times;
  for(JsonVariantConst day:record["days"].as<JsonArrayConst>()){
   if(!day.is<int>()||day.as<int>()<1||day.as<int>()>days(year,mon)||!selected.insert(day.as<int>()).second)return false;
  }
  for(JsonVariantConst at:record["times"].as<JsonArrayConst>()){
   int h,m;if(!at.is<const char*>()||!time(at.as<const char*>(),h,m)||!times.insert(at.as<const char*>()).second)return false;
  }
 }
 return true;
}
}
