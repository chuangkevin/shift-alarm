#pragma once
#include "calendar_metadata.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <set>
#include <vector>

namespace schedulecandidate {
constexpr size_t MAX_ALARMS=512,MAX_BYTES=98304;
struct Alarm {String id,label;int64_t epoch;};
struct Candidate {std::vector<Alarm> alarms;JsonDocument months;String revision,canonical,management_url,command_id;bool local=false,first_only=false;int64_t server_time=0,command_issued=0;bool stop_command=false;};

inline bool parse(const String &body,Candidate &out,String &error){
 if(body.length()>MAX_BYTES){error="班表資料過大";return false;}JsonDocument doc;if(deserializeJson(doc,body)){error="資料格式無效";return false;}
 if(!doc["revision"].is<String>()||doc["revision"].as<String>().isEmpty()||doc["timezone"]!="Asia/Taipei"||!doc["alarms"].is<JsonArray>()||doc["alarms"].size()>MAX_ALARMS){error="班表欄位不完整";return false;}
 auto first=doc["first_consecutive_only"];if(!first.isNull()&&!first.is<bool>()){error="連續上班通知設定無效";return false;}out.first_only=first.is<bool>()?first.as<bool>():false;
 if(doc.as<JsonObjectConst>().containsKey("months")){if(!localcalendar::validMonths(doc["months"])){error="月份設定格式無效或超過 120 個月";return false;}out.months.set(doc["months"]);}else out.months.to<JsonObject>();
 if(out.months.overflowed()){error="月份設定記憶體不足";return false;}
 for(JsonObject a:doc["alarms"].as<JsonArray>()){
  if(!a["id"].is<String>()||a["id"].as<String>().isEmpty()||a["id"].as<String>().length()>128||!a["epoch"].is<int64_t>()||a["epoch"].as<int64_t>()<1700000000||!a["label"].is<String>()||a["label"].as<String>().length()>256){error="鬧鐘資料無效";return false;}
  for(auto &b:out.alarms)if(b.id==a["id"].as<String>()){error="鬧鐘識別碼重複";return false;}
  out.alarms.push_back({a["id"].as<String>(),a["label"].as<String>(),a["epoch"].as<int64_t>()});
 }
 std::sort(out.alarms.begin(),out.alarms.end(),[](const Alarm&a,const Alarm&b){return a.epoch<b.epoch;});out.revision=doc["revision"].as<String>();out.local=doc["source"]=="local";
 JsonDocument saved;saved["revision"]=out.revision;saved["timezone"]="Asia/Taipei";auto list=saved["alarms"].to<JsonArray>();for(const auto&a:out.alarms){auto item=list.add<JsonObject>();item["id"]=a.id;item["label"]=a.label;item["epoch"]=a.epoch;}saved["source"]=out.local?"local":"remote";saved["months"]=out.months;saved["first_consecutive_only"]=out.first_only;serializeJson(saved,out.canonical);
 if(saved.overflowed()||out.canonical.length()>MAX_BYTES){error="班表資料過大";return false;}
 if(doc["server_time"].is<int64_t>())out.server_time=doc["server_time"];
 if(doc["management_url"].is<String>()&&doc["management_url"].as<String>().length()<=180)out.management_url=doc["management_url"].as<String>();
 out.stop_command=doc["command"]["type"]=="stop";if(out.stop_command){out.command_id=doc["command"]["id"].as<String>();out.command_issued=doc["command"]["issued_at"]|int64_t(0);}return true;
}
}
