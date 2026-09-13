#pragma once
#include "calendar_policy.h"
#include "calendar_page.h"

String localCalendarMonthKey(int year,int month){
 char key[8];snprintf(key,sizeof(key),"%04d-%02d",year,month);return String(key);
}

std::set<int64_t> knownLocalWorkDates(JsonVariantConst months){
 std::set<int64_t> dates;
 for(JsonPairConst entry:months.as<JsonObjectConst>()){
  int year,month;if(!localcalendar::month(entry.key().c_str(),year,month))continue;
  for(JsonVariantConst day:entry.value()["days"].as<JsonArrayConst>())dates.insert(localcalendar::epoch(year,month,day.as<int>(),0,0));
 }
 // Old schedules may have alarms but no monthly metadata. Keep those dates
 // available for cross-month detection until the month is edited once.
 for(const auto &alarm:alarms){
  if(!alarm.id.startsWith("local-"))continue;
  time_t value=alarm.epoch;struct tm date;localtime_r(&value,&date);
  String key=localCalendarMonthKey(date.tm_year+1900,date.tm_mon+1);
  if(!months[key].is<JsonObjectConst>())dates.insert(localcalendar::epoch(date.tm_year+1900,date.tm_mon+1,date.tm_mday,0,0));
 }
 return dates;
}

void localCalendarRoutes(){
 server.on("/calendar",HTTP_GET,[]{String page=CALENDAR_PAGE;page.replace("NONCE",setupNonce);server.send(200,"text/html; charset=utf-8",devicePage(page));});
 server.on("/api/local-calendar",HTTP_GET,[]{
  if(!localNonce())return;
  int year,month;String value=server.arg("month");
  if(!localcalendar::month(value.c_str(),year,month)){server.send(400,"text/plain; charset=utf-8","月份格式無效");return;}
  JsonDocument d;d["local"]=localSchedule;d["clock_ready"]=clockValid();d["first_consecutive_only"]=firstConsecutiveOnly;
  const auto knownDates=knownLocalWorkDates(calendarMonths.as<JsonVariantConst>());
  d["previous_day_work"]=knownDates.count(localcalendar::epoch(year,month,1,0,0)-86400)>0;
  auto days=d["days"].to<JsonArray>();auto times=d["times"].to<JsonArray>();
  std::vector<int> selected;std::vector<String> hours;std::set<std::pair<int,int>> slots;
  if(calendarMonths[value].is<JsonObject>()){
    d["days"]=calendarMonths[value]["days"];d["times"]=calendarMonths[value]["times"];d["disabled_times"]=calendarMonths[value]["disabled_times"];d["mixed_times"]=false;
    String out;serializeJson(d,out);server.send(200,"application/json",out);return;
  }
  for(const auto &alarm:alarms){
    if(alarm.id.startsWith("test-"))continue;
    time_t value=alarm.epoch;struct tm date;localtime_r(&value,&date);
    if(date.tm_year+1900!=year||date.tm_mon+1!=month)continue;
    slots.emplace(date.tm_mday,date.tm_hour*60+date.tm_min);
    if(std::find(selected.begin(),selected.end(),date.tm_mday)==selected.end())selected.push_back(date.tm_mday);
    char at[6];snprintf(at,sizeof(at),"%02d:%02d",date.tm_hour,date.tm_min);
    if(std::find(hours.begin(),hours.end(),String(at))==hours.end())hours.push_back(at);
  }
  d["mixed_times"]=localcalendar::mixedTimes(slots);std::sort(selected.begin(),selected.end());std::sort(hours.begin(),hours.end());
  for(int day:selected)days.add(day);
  for(const auto &at:hours)times.add(at);
  String out;serializeJson(d,out);server.send(200,"application/json",out);
 });
 server.on("/api/local-calendar",HTTP_POST,[]{
  if(!localNonce())return;
  if(server.arg("plain").length()>4096){server.send(413,"text/plain; charset=utf-8","班表資料過大");return;}
  JsonDocument input;int year,month;
  if(deserializeJson(input,server.arg("plain"))){server.send(400,"text/plain; charset=utf-8","班表資料格式無效");return;}
  const JsonVariantConst firstOnlySetting=input["first_consecutive_only"];
  if(!input["month"].is<String>()||!localcalendar::month(input["month"].as<String>().c_str(),year,month)||!input["days"].is<JsonArray>()||!input["times"].is<JsonArray>()||(!input["disabled_times"].isNull()&&!input["disabled_times"].is<JsonArray>())||input["days"].size()>31||(input["days"].size()>0&&input["times"].size()<1)||input["times"].size()>8||input["disabled_times"].size()>8||
     (!firstOnlySetting.isNull()&&!firstOnlySetting.is<bool>())){
    server.send(400,"text/plain; charset=utf-8","請選擇有效月份、日期、鬧鐘時間及通知方式");return;
  }
  const bool firstOnly=firstOnlySetting.is<bool>()?firstOnlySetting.as<bool>():firstConsecutiveOnly;
  std::vector<int> selectedDays;std::vector<String> selectedTimes;std::set<String> disabledTimes;
  for(JsonVariant value:input["days"].as<JsonArray>()){
    if(!value.is<int>()||value.as<int>()<1||value.as<int>()>localcalendar::days(year,month)||std::find(selectedDays.begin(),selectedDays.end(),value.as<int>())!=selectedDays.end()){
      server.send(400,"text/plain; charset=utf-8","上班日期無效或重複");return;
    }
    selectedDays.push_back(value.as<int>());
  }
  for(JsonVariant value:input["times"].as<JsonArray>()){
    int hour,minute;String at=value.as<String>();
    if(!value.is<String>()||!localcalendar::time(at.c_str(),hour,minute)||std::find(selectedTimes.begin(),selectedTimes.end(),at)!=selectedTimes.end()){
      server.send(400,"text/plain; charset=utf-8","鬧鐘時間無效或重複，請使用 07:00 格式");return;
    }
    selectedTimes.push_back(at);
  }
  for(JsonVariant value:input["disabled_times"].as<JsonArray>()){
    String at=value.as<String>();
    if(!value.is<String>()||std::find(selectedTimes.begin(),selectedTimes.end(),at)==selectedTimes.end()||!disabledTimes.insert(at).second){
      server.send(400,"text/plain; charset=utf-8","停用的鬧鐘時間無效或重複");return;
    }
  }
  JsonDocument next;next["revision"]=String("local-")+String(esp_random(),HEX)+String(esp_random(),HEX);next["timezone"]="Asia/Taipei";next["source"]="local";next["first_consecutive_only"]=firstOnly;
  auto list=next["alarms"].to<JsonArray>();next["months"]=calendarMonths;
  auto metadata=next["months"][input["month"].as<String>()].to<JsonObject>();metadata["days"]=input["days"];metadata["times"]=input["times"];
  auto disabledMetadata=metadata["disabled_times"].to<JsonArray>();disabledMetadata.clear();for(const auto &at:disabledTimes)disabledMetadata.add(at);
  if(!localcalendar::validMonths(next["months"])){server.send(400,"text/plain; charset=utf-8","月份設定格式無效或超過 120 個月");return;}

  const auto knownDates=knownLocalWorkDates(next["months"].as<JsonVariantConst>());
  // Preserve test alarms and remote/legacy alarms outside metadata-backed months.
  for(const auto &alarm:alarms){
    time_t value=alarm.epoch;struct tm date;localtime_r(&value,&date);
    const bool currentMonth=date.tm_year+1900==year&&date.tm_mon+1==month;
    const bool metadataBacked=next["months"][localCalendarMonthKey(date.tm_year+1900,date.tm_mon+1)].is<JsonObject>();
    if(!alarm.id.startsWith("test-")&&(currentMonth||(alarm.id.startsWith("local-")&&metadataBacked)))continue;
    auto saved=list.add<JsonObject>();saved["id"]=alarm.id;saved["label"]=alarm.label;saved["epoch"]=alarm.epoch;
  }
  for(JsonPairConst entry:next["months"].as<JsonObjectConst>()){
    int entryYear,entryMonth;localcalendar::month(entry.key().c_str(),entryYear,entryMonth);
    std::set<String> disabled;
    for(JsonVariantConst at:entry.value()["disabled_times"].as<JsonArrayConst>())disabled.insert(at.as<String>());
    for(JsonVariantConst day:entry.value()["days"].as<JsonArrayConst>()){
      const int selectedDay=day.as<int>();const int64_t workDate=localcalendar::epoch(entryYear,entryMonth,selectedDay,0,0);
      if(!localcalendar::shouldNotify(workDate,knownDates,firstOnly))continue;
      for(JsonVariantConst atValue:entry.value()["times"].as<JsonArrayConst>()){
        int hour,minute;String at=atValue.as<String>();localcalendar::time(at.c_str(),hour,minute);
        if(disabled.count(at))continue;
        if(list.size()>=MAX_ALARMS){server.send(400,"text/plain; charset=utf-8","已超過裝置可儲存的 512 個鬧鐘，請先清除不需要的月份");return;}
        auto alarm=list.add<JsonObject>();alarm["id"]=String("local-")+entry.key().c_str()+"-"+selectedDay+"-"+at;alarm["label"]="上班鬧鐘";alarm["epoch"]=localcalendar::epoch(entryYear,entryMonth,selectedDay,hour,minute);
      }
    }
  }
  if(next.overflowed()){server.send(500,"text/plain; charset=utf-8","班表設定記憶體不足，原設定未變更");return;}
  String body,error;serializeJson(next,body);
  if(!applySchedule(body,true,error)){server.send(500,"text/plain; charset=utf-8",error);return;}
  syncState="班表已儲存";forceDraw=true;server.send(200,"application/json","{\"ok\":true}");
 });
 server.on("/api/local-clock",HTTP_POST,[]{
  if(!localNonce())return;
  JsonDocument d;
  if(server.arg("plain").length()>128||deserializeJson(d,server.arg("plain"))||!d["epoch"].is<int64_t>()||d["epoch"].as<int64_t>()<1704067200||d["epoch"].as<int64_t>()>=4102444800LL){server.send(400,"text/plain; charset=utf-8","校時資料無效");return;}
  timeval tv={};tv.tv_sec=d["epoch"].as<int64_t>();settimeofday(&tv,nullptr);server.send(200,"application/json","{\"ok\":true}");
 });
}
