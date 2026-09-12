#pragma once
#include "calendar_policy.h"
#include "calendar_page.h"
void localCalendarRoutes(){
 server.on("/calendar",HTTP_GET,[]{String page=CALENDAR_PAGE;page.replace("NONCE",setupNonce);server.send(200,"text/html; charset=utf-8",devicePage(page));});
 server.on("/api/local-calendar",HTTP_GET,[]{
  if(!localNonce()){return;}int year,month;String value=server.arg("month");if(!localcalendar::month(value.c_str(),year,month)){server.send(400,"text/plain; charset=utf-8","月份格式無效");return;}
  JsonDocument d;d["local"]=localSchedule;d["clock_ready"]=clockValid();auto days=d["days"].to<JsonArray>();auto times=d["times"].to<JsonArray>();std::vector<int> selected;std::vector<String> hours;std::set<std::pair<int,int>> slots;
  if(calendarMonths[value].is<JsonObject>()){
    d["days"]=calendarMonths[value]["days"];d["times"]=calendarMonths[value]["times"];d["mixed_times"]=false;
    String out;serializeJson(d,out);server.send(200,"application/json",out);return;
  }
  for(const auto &a:alarms){if(a.id.startsWith("test-"))continue;time_t e=a.epoch;struct tm t;localtime_r(&e,&t);if(t.tm_year+1900!=year||t.tm_mon+1!=month)continue;slots.emplace(t.tm_mday,t.tm_hour*60+t.tm_min);if(std::find(selected.begin(),selected.end(),t.tm_mday)==selected.end())selected.push_back(t.tm_mday);char h[6];snprintf(h,sizeof(h),"%02d:%02d",t.tm_hour,t.tm_min);if(std::find(hours.begin(),hours.end(),String(h))==hours.end())hours.push_back(h);}
  d["mixed_times"]=localcalendar::mixedTimes(slots);std::sort(selected.begin(),selected.end());std::sort(hours.begin(),hours.end());for(int n:selected)days.add(n);for(const auto &h:hours)times.add(h);String out;serializeJson(d,out);server.send(200,"application/json",out);
 });
 server.on("/api/local-calendar",HTTP_POST,[]{
  if(!localNonce()){return;}if(server.arg("plain").length()>4096){server.send(413,"text/plain; charset=utf-8","班表資料過大");return;}
  JsonDocument input;int year,month;if(deserializeJson(input,server.arg("plain"))||!input["month"].is<String>()||!localcalendar::month(input["month"].as<String>().c_str(),year,month)||!input["days"].is<JsonArray>()||!input["times"].is<JsonArray>()||input["days"].size()>31||(input["days"].size()>0&&input["times"].size()<1)||input["times"].size()>8){server.send(400,"text/plain; charset=utf-8","請選擇有效月份、日期及一至八個鬧鐘時間");return;}
  std::vector<int> days;std::vector<String> times;
  for(JsonVariant v:input["days"].as<JsonArray>()){if(!v.is<int>()||v.as<int>()<1||v.as<int>()>localcalendar::days(year,month)||std::find(days.begin(),days.end(),v.as<int>())!=days.end()){server.send(400,"text/plain; charset=utf-8","上班日期無效或重複");return;}days.push_back(v.as<int>());}
  for(JsonVariant v:input["times"].as<JsonArray>()){int h,m;String t=v.as<String>();if(!v.is<String>()||!localcalendar::time(t.c_str(),h,m)||std::find(times.begin(),times.end(),t)!=times.end()){server.send(400,"text/plain; charset=utf-8","鬧鐘時間無效或重複，請使用 07:00 格式");return;}times.push_back(t);}
  JsonDocument next;next["revision"]=String("local-")+String(esp_random(),HEX)+String(esp_random(),HEX);next["timezone"]="Asia/Taipei";next["source"]="local";auto list=next["alarms"].to<JsonArray>();
  next["months"]=calendarMonths;
  auto metadata=next["months"][input["month"].as<String>()].to<JsonObject>();metadata["days"]=input["days"];metadata["times"]=input["times"];
  if(!localcalendar::validMonths(next["months"])){server.send(400,"text/plain; charset=utf-8","月份設定格式無效或超過 120 個月");return;}
  for(const auto &a:alarms){time_t e=a.epoch;struct tm t;localtime_r(&e,&t);if(!a.id.startsWith("test-")&&t.tm_year+1900==year&&t.tm_mon+1==month)continue;auto b=list.add<JsonObject>();b["id"]=a.id;b["label"]=a.label;b["epoch"]=a.epoch;}
  if(list.size()+days.size()*times.size()>MAX_ALARMS){server.send(400,"text/plain; charset=utf-8","已超過裝置可儲存的 512 個鬧鐘，請先清除不需要的月份");return;}
  for(int day:days)for(const auto &t:times){int h,m;localcalendar::time(t.c_str(),h,m);auto a=list.add<JsonObject>();a["id"]=String("local-")+input["month"].as<String>()+"-"+day+"-"+t;a["label"]="上班鬧鐘";a["epoch"]=localcalendar::epoch(year,month,day,h,m);}
  if(next.overflowed()){server.send(500,"text/plain; charset=utf-8","班表設定記憶體不足，原設定未變更");return;}
  String body,error;serializeJson(next,body);if(!applySchedule(body,true,error)){server.send(500,"text/plain; charset=utf-8",error);return;}syncState="班表已儲存";forceDraw=true;server.send(200,"application/json","{\"ok\":true}");
 });
 server.on("/api/local-clock",HTTP_POST,[]{if(!localNonce()){return;}JsonDocument d;if(server.arg("plain").length()>128||deserializeJson(d,server.arg("plain"))||!d["epoch"].is<int64_t>()||d["epoch"].as<int64_t>()<1704067200||d["epoch"].as<int64_t>()>=4102444800LL){server.send(400,"text/plain; charset=utf-8","校時資料無效");return;}timeval tv={};tv.tv_sec=d["epoch"].as<int64_t>();settimeofday(&tv,nullptr);server.send(200,"application/json","{\"ok\":true}");});
}
