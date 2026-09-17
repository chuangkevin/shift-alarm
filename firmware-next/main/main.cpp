#include "ota_download_policy.h"
#include "battery_policy.h"
#include "connectivity_policy.h"
#include "backend_poll_policy.h"
#include "wifi_profiles.h"
#include "wifi_failover_policy.h"
#include "wifi_page.h"
#include "button_policy.h"
#include "device_identity.h"
#include "ui_policy.h"
#include "ota_manifest_policy.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include "wifi_indicator.h"
#include "backend_endpoint.h"
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_SSD1306.h>
#include <qrcode.h>
#include <driver/i2s.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_heap_caps.h>
#include <esp_app_desc.h>
#include <esp_mac.h>
#include <time.h>
#include <sys/time.h>
#if __has_include("provisioning.h")
#include "provisioning.h"
#endif
#include <vector>
#include <algorithm>
#include <atomic>
#include <new>
#include "freertos/queue.h"
#include "scheduler.h"
#include "alarm_tailnet.h"
#include "alarm_ota.h"
#include "alarm_proxy.h"
#include "driver/rtc_io.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_task_wdt.h"
#include "esp_sntp.h"
#include "zh_glyphs.h"
#include "clock_page.h"
#include "screen_policy.h"
#include "schedule_candidate.h"

const char *const VERSION=esp_app_get_description()->version;
constexpr size_t MAX_ALARMS=schedulecandidate::MAX_ALARMS, MAX_JSON=schedulecandidate::MAX_BYTES;
constexpr uint32_t POLL_MS=5000, RING_MS=180000;
constexpr uint32_t CLOCK_SYNC_INTERVAL_MS=3UL*60*60*1000;
std::atomic<uint32_t> lastNetworkClock{0},lastNetworkClockMs{0};
void networkClockSynced(struct timeval *tv){lastNetworkClock=uint32_t(tv->tv_sec);lastNetworkClockMs=millis();}
constexpr int BUTTON_STOP=0, BUTTON_SNOOZE=39, BUTTON_TEST=40;
using Alarm=schedulecandidate::Alarm;
std::vector<Alarm> alarms;
JsonDocument calendarMonths,weeklyProfiles;
Preferences prefs;
WebServer server(IPAddress(127,0,0,1),8081);
DNSServer dns;
String apName,setupNonce;
wifiprofiles::SensitiveText<wifiprofiles::MAX_SSID_BYTES> pendingSsid;
wifiprofiles::SensitiveText<wifiprofiles::MAX_PASSWORD_BYTES> pendingPassword;
bool connecting=false, setupFailed=false;
uint8_t portalView=0;
uint32_t connectStarted=0,apCloseAt=0;
String lastCommand;
bool tailnetStarted=false,proxyStarted=false;
bool savedScheduleRestored=true, deviceRoutesReady=false, localSchedule=false, firstConsecutiveOnly=false;
std::atomic<bool> speakerReady{false};
uint32_t tailnetAttempt=0;
std::atomic<uint32_t> backendLastSuccessMs{0};
std::atomic<bool> backendLastResult{false};
std::atomic<bool> backendHasSuccess{false};
constexpr size_t BACKEND_URL_CAP=192,BACKEND_TOKEN_CAP=256,REVISION_CAP=192,HEARTBEAT_CAP=1024;
struct BackendPollExchange {
  backendpoll::Ticket ticket{};
  bool fetch_schedule=false,schedule_body_valid=false,heartbeat_ok=false;
  int schedule_status=0;
  char backend[BACKEND_URL_CAP]={},token[BACKEND_TOKEN_CAP]={},revision[REVISION_CAP]={},heartbeat[HEARTBEAT_CAP]={};
  String schedule_body;
};
backendpoll::State backendPollState;
QueueHandle_t backendRequestQueue=nullptr,backendResultQueue=nullptr;
std::atomic<uint32_t> backendDroppedGeneration{0};
bool backendPollReady=false,backendPollObservedOtaBusy=false;
enum class ScheduleOrigin:uint8_t{Backend,Local,Direct,Rollback,Cleanup};
struct ScheduleWork {uint32_t id=0,schedule_generation=0,backend_generation=0;ScheduleOrigin origin=ScheduleOrigin::Backend;String input,previous,error;schedulecandidate::Candidate candidate;bool persisted=false;};
QueueHandle_t scheduleRequestQueue=nullptr,scheduleResultQueue=nullptr;
bool scheduleWorkReady=false,scheduleWorkBusy=false;uint32_t scheduleWorkId=0,scheduleGeneration=0,localSaveId=0;String localSaveState="idle",localSaveMessage;
std::atomic<uint32_t> scheduleDroppedId{0};
std::atomic<bool> scheduleStorageFault{false};
bool scheduleStorageFaultObserved=false;
// Prevent Arduino from confirming a pending image before application diagnostics.
extern "C" bool verifyRollbackLater(){return true;}

String revision, backend, token, manageUrl, apPassword;
wifiprofiles::List wifiProfiles;
wififailover::State wifiState;
bool wifiPendingTrial=false,wifiTrialStarted=false;
uint32_t wifiTrialAt=0;
bool wifiMigrationPending=false;
uint32_t wifiMigrationAttempt=0;
wifiprofiles::Selector wifiSelector;
bool wifiSelectorValid=false,wifiStorageFault=false;
int64_t handled=0,snooze=0;
volatile bool ringing=false;
uint32_t ringStarted=0,lastDraw=0,lastSerial=0,bootMs=0;
bool portal=false;
uint32_t pairingHoldStarted=0;
bool pairingHoldActive=false;
buttons::State buttonState;
deviceui::State uiState;
String ringLabel="鬧鐘", syncState="等待無線網路";
#if CUBE_TFT
Adafruit_ST7789 screen(&SPI,14,8,18);
GFXcanvas16 *frame=nullptr;
#define surface (*frame)
#else
Adafruit_SSD1306 screen(128,64,&Wire,-1);
#define surface screen
#endif

uint8_t displayRotation=0;
uint16_t screenTimeoutMinutes=0;
uint8_t screenBrightness=40;
bool screenAwake=true;
uint32_t screenLastActivity=0;
bool forceDraw=true;
uint32_t previousFrameHash=0;
bool displaySettingsValid=false;
adc_oneshot_unit_handle_t batteryAdc=nullptr;
battery::State batteryState;
bool chargingInputValid=false;
std::atomic<bool> charging{false};

constexpr char DISPLAY_SETTINGS_KEY[]="display-v1";
bool saveDisplaySettings(uint8_t rotation,uint16_t timeoutMinutes,uint8_t brightness){
  if(rotation>3||!screenpolicy::validTimeout(timeoutMinutes)||!screenpolicy::validBrightness(brightness))return false;
  const uint8_t blob[]={ 'D',2,rotation,uint8_t(timeoutMinutes),uint8_t(timeoutMinutes>>8),brightness };
  if(prefs.putBytes(DISPLAY_SETTINGS_KEY,blob,sizeof(blob))!=sizeof(blob)||prefs.getBytesLength(DISPLAY_SETTINGS_KEY)!=sizeof(blob))return false;
  uint8_t verified[sizeof(blob)]={};
  return prefs.getBytes(DISPLAY_SETTINGS_KEY,verified,sizeof(verified))==sizeof(verified)&&memcmp(blob,verified,sizeof(blob))==0;
}
void loadDisplaySettings(){
  displaySettingsValid=false;
  displayRotation=prefs.getUChar("rotation",prefs.getBool("flipped",false)?2:0)%4;
  screenTimeoutMinutes=0;
  screenBrightness=40;
  if(prefs.isKey(DISPLAY_SETTINGS_KEY)){
    const size_t length=prefs.getBytesLength(DISPLAY_SETTINGS_KEY);uint8_t blob[6]={};
    if((length==5||length==sizeof(blob))&&prefs.getBytes(DISPLAY_SETTINGS_KEY,blob,length)==length&&
       blob[0]=='D'&&(blob[1]==1||blob[1]==2)&&blob[2]<=3){
      uint16_t timeout=uint16_t(blob[3])|(uint16_t(blob[4])<<8);
      uint8_t brightness=length==sizeof(blob)?blob[5]:40;
      if(screenpolicy::validTimeout(timeout)&&screenpolicy::validBrightness(brightness)){
        displayRotation=blob[2];
        screenTimeoutMinutes=timeout;
        screenBrightness=brightness;
        const bool migrationRequired=length==5;
        const bool migrationSaved=!migrationRequired||
          saveDisplaySettings(displayRotation,screenTimeoutMinutes,screenBrightness);
        displaySettingsValid=screenpolicy::settingsLoadValid(
          true,migrationRequired,migrationSaved);
      }
    }
  }else displaySettingsValid=saveDisplaySettings(displayRotation,screenTimeoutMinutes,screenBrightness);
}
void initBattery(){
#if CUBE_TFT
  adc_oneshot_unit_init_cfg_t unit={};unit.unit_id=ADC_UNIT_2;
  if(adc_oneshot_new_unit(&unit,&batteryAdc)!=ESP_OK){batteryAdc=nullptr;Serial.println("BATTERY_ADC_INIT_FAILED");}
  if(batteryAdc){adc_oneshot_chan_cfg_t channel={};channel.atten=ADC_ATTEN_DB_12;channel.bitwidth=ADC_BITWIDTH_12;
    if(adc_oneshot_config_channel(batteryAdc,ADC_CHANNEL_6,&channel)!=ESP_OK){adc_oneshot_del_unit(batteryAdc);batteryAdc=nullptr;Serial.println("BATTERY_ADC_CONFIG_FAILED");}}
  gpio_config_t charge={};charge.pin_bit_mask=1ULL<<GPIO_NUM_38;charge.mode=GPIO_MODE_INPUT;
  chargingInputValid=gpio_config(&charge)==ESP_OK;
#endif
}
void sampleBattery(uint32_t now){
#if CUBE_TFT
  if(!battery::should_sample(batteryState,now))return;
  int raw=0;const esp_err_t result=batteryAdc?adc_oneshot_read(batteryAdc,ADC_CHANNEL_6,&raw):ESP_ERR_INVALID_STATE;
  battery::record(batteryState,now,result==ESP_OK&&raw>=0&&raw<=4095,uint16_t(raw));
  if(result!=ESP_OK)Serial.printf("BATTERY_ADC_READ_FAILED code=%d\n",int(result));
  if(chargingInputValid){
    const int level=gpio_get_level(GPIO_NUM_38);
    if(level==0||level==1)charging=battery::charging_active(level);
    else chargingInputValid=false;
  }
  forceDraw=true;
#endif
}
bool backlightPwm=false;
void setBacklight(bool awake){
#if CUBE_TFT
  if(backlightPwm)ledcWrite(13,awake?screenpolicy::brightnessDuty(screenBrightness):0);
  else digitalWrite(13,awake?HIGH:LOW);
#endif
}
void setScreenAwake(bool awake){
  if(screenAwake==awake)return;
#if CUBE_TFT
  if(awake){screen.enableDisplay(true);setBacklight(true);}else{setBacklight(false);screen.enableDisplay(false);}
#else
  screen.ssd1306_command(awake?SSD1306_DISPLAYON:SSD1306_DISPLAYOFF);
#endif
  screenAwake=awake;
  if(awake){forceDraw=true;previousFrameHash=0;}
}
void wakeScreen(){screenLastActivity=millis();setScreenAwake(true);}

constexpr char WIFI_PROFILE_SLOT_KEYS[2][10]={"wifi-v2-a","wifi-v2-b"};
constexpr char WIFI_PROFILE_SELECTOR_KEY[]="wifi-v2-sel";
constexpr char WIFI_CREDENTIALS_KEY[]="wifi-v1";
void wipeString(String &value){for(unsigned i=0;i<value.length();i++)value.setCharAt(i,'\0');value="";}
class WifiNvsStorage : public wifiprofiles::Storage {
  ReadResult query(const char *key,size_t &length){
    nvs_handle_t handle;const esp_err_t opened=nvs_open_from_partition("alarm_nvs","shift-alarm",NVS_READONLY,&handle);if(opened!=ESP_OK)return ReadResult::Error;
    size_t stored=0;const esp_err_t result=nvs_get_blob(handle,key,nullptr,&stored);nvs_close(handle);
    if(result==ESP_ERR_NVS_NOT_FOUND){length=0;return ReadResult::NotFound;}
    if(result!=ESP_OK){length=0;return ReadResult::Error;}
    length=stored;return ReadResult::Ok;
  }
  ReadResult read(const char *key,uint8_t *out,size_t &length){
    nvs_handle_t handle;const esp_err_t opened=nvs_open_from_partition("alarm_nvs","shift-alarm",NVS_READONLY,&handle);if(opened!=ESP_OK)return ReadResult::Error;
    size_t returned=length;const esp_err_t result=nvs_get_blob(handle,key,out,&returned);nvs_close(handle);length=returned;
    if(result==ESP_ERR_NVS_NOT_FOUND)return ReadResult::NotFound;
    return result==ESP_OK?ReadResult::Ok:ReadResult::Error;
  }
  bool write(const char *key,const uint8_t *data,size_t length){
    nvs_handle_t handle;esp_err_t result=nvs_open_from_partition("alarm_nvs","shift-alarm",NVS_READWRITE,&handle);if(result!=ESP_OK)return false;
    result=nvs_set_blob(handle,key,data,length);if(result==ESP_OK)result=nvs_commit(handle);nvs_close(handle);return result==ESP_OK;
  }
  bool erase(const char *key){
    nvs_handle_t handle;esp_err_t result=nvs_open_from_partition("alarm_nvs","shift-alarm",NVS_READWRITE,&handle);if(result!=ESP_OK)return false;
    result=nvs_erase_key(handle,key);if(result==ESP_OK)result=nvs_commit(handle);nvs_close(handle);return result==ESP_OK;
  }
 public:
  ReadResult selectorSize(size_t &length) override{return query(WIFI_PROFILE_SELECTOR_KEY,length);}
  ReadResult selectorData(uint8_t *out,size_t &length) override{return read(WIFI_PROFILE_SELECTOR_KEY,out,length);}
  ReadResult slotSize(uint8_t slot,size_t &length) override{return slot<2?query(WIFI_PROFILE_SLOT_KEYS[slot],length):ReadResult::Error;}
  ReadResult slotData(uint8_t slot,uint8_t *out,size_t &length) override{return slot<2?read(WIFI_PROFILE_SLOT_KEYS[slot],out,length):ReadResult::Error;}
  bool writeSelector(const uint8_t *data,size_t length) override{return write(WIFI_PROFILE_SELECTOR_KEY,data,length);}
  bool writeSlot(uint8_t slot,const uint8_t *data,size_t length) override{return slot<2&&write(WIFI_PROFILE_SLOT_KEYS[slot],data,length);}
  bool eraseSlot(uint8_t slot) override{
    if(slot>=2)return false;
    const char *key=WIFI_PROFILE_SLOT_KEYS[slot];size_t length=0;const auto found=query(key,length);if(found==ReadResult::NotFound)return true;if(found!=ReadResult::Ok||!length||length>wifiprofiles::MAX_BLOB_SIZE)return false;
    uint8_t zeros[wifiprofiles::MAX_BLOB_SIZE]={},verified[wifiprofiles::MAX_BLOB_SIZE];
    size_t returned=sizeof(verified);const bool overwritten=write(key,zeros,length)&&wifiprofiles::readSlot(*this,slot,verified,returned)==ReadResult::Ok&&returned==length&&memcmp(zeros,verified,length)==0;
    wifiprofiles::secureWipe(zeros,sizeof(zeros));wifiprofiles::secureWipe(verified,sizeof(verified));
    return overwritten&&erase(key);
  }
};
WifiNvsStorage wifiStorage;
bool clearInactiveWifiSlot(uint8_t active){
  const uint8_t inactive=active^1u;if(!wifiStorage.eraseSlot(inactive))return false;
  uint8_t readback[wifiprofiles::MAX_BLOB_SIZE]={};size_t length=sizeof(readback);const bool clear=wifiprofiles::readSlot(wifiStorage,inactive,readback,length)==wifiprofiles::Storage::ReadResult::NotFound;wifiprofiles::secureWipe(readback,sizeof(readback));return clear;
}
wifiprofiles::CommitResult persistWifiProfiles(const wifiprofiles::List &profiles,wifiprofiles::Selector &committed){
  if(wifiStorageFault)return wifiprofiles::CommitResult::StorageFault;
  const auto result=wifiprofiles::commit(wifiStorage,profiles,wifiSelectorValid?&wifiSelector:nullptr,committed);
  if(result==wifiprofiles::CommitResult::StorageFault||result==wifiprofiles::CommitResult::CommittedStorageFault)wifiStorageFault=true;
  return result;
}
bool legacyWifiProfile(wifiprofiles::Profile &profile){
  if(prefs.isKey(WIFI_CREDENTIALS_KEY)){
    size_t length=prefs.getBytesLength(WIFI_CREDENTIALS_KEY);wifiprofiles::SensitiveBytes<wifiprofiles::LEGACY_MAX_BLOB_SIZE> blob;
    if(!length||length>blob.size()||prefs.getBytes(WIFI_CREDENTIALS_KEY,blob.data(),length)!=length)return false;
    return wifiprofiles::decodeLegacy(blob.data(),length,profile);
  }
  String name=prefs.getString("ssid"),secret=prefs.getString("password");
  if(name.isEmpty())return false;
  const bool assigned=profile.ssid.assign(name.c_str(),name.length())&&profile.password.assign(secret.c_str(),secret.length());
  wipeString(name);wipeString(secret);return assigned&&wifiprofiles::valid(profile);
}
bool loadWifiProfiles(){
  wifiProfiles={};
  wifiprofiles::Record loaded;wifiprofiles::Selector selected;const auto load=wifiprofiles::load(wifiStorage,loaded,selected);
  if(load==wifiprofiles::LoadResult::Ok){wifiProfiles=loaded.list;wifiSelector=selected;wifiSelectorValid=true;wifiStorageFault=!clearInactiveWifiSlot(selected.slot);if(!wifiStorageFault){prefs.remove(WIFI_CREDENTIALS_KEY);prefs.remove("ssid");prefs.remove("password");}return true;}
  if(load==wifiprofiles::LoadResult::StorageFault){wifiStorageFault=true;wifiprofiles::Profile fallback;if(legacyWifiProfile(fallback)){wifiprofiles::addOrUpdate(wifiProfiles,fallback);return true;}return false;}
  wifiprofiles::Profile legacy;
  if(!legacyWifiProfile(legacy))return !prefs.isKey(WIFI_CREDENTIALS_KEY)&&!prefs.isKey("ssid");
  wifiprofiles::List migrated;wifiprofiles::addOrUpdate(migrated,legacy);
  wifiprofiles::Selector committed;const auto result=persistWifiProfiles(migrated,committed);
  if(result!=wifiprofiles::CommitResult::Committed){wifiProfiles=migrated;wifiMigrationPending=result==wifiprofiles::CommitResult::Failed;return true;}
  wifiProfiles=migrated;wifiSelector=committed;wifiSelectorValid=true;
  // Old keys are deleted only after the checksummed replacement passes a full readback decode.
  prefs.remove(WIFI_CREDENTIALS_KEY);prefs.remove("ssid");prefs.remove("password");
  return true;
}
String currentWifiSsid(){return WiFi.isConnected()?WiFi.SSID():String("未連線");}
String savedSchedule() {
  size_t len=prefs.getBytesLength("schedule");if(!len||len>MAX_JSON)return "";
  std::vector<char> bytes(len+1,0);prefs.getBytes("schedule",bytes.data(),len);return String(bytes.data());
}
bool clockValid() { return time(nullptr)>=alarmclock::VALID_CLOCK; }
String datetime(int64_t epoch) { struct tm t; time_t e=epoch; localtime_r(&e,&t); char b[32]; strftime(b,sizeof(b),"%m/%d %H:%M",&t); return b; }
String ascii(String s) { for(unsigned i=0;i<s.length();i++) if((uint8_t)s[i]<32||(uint8_t)s[i]>126)s[i]='?'; return s; }
void fill(uint16_t c) {
#if CUBE_TFT
surface.fillScreen(c);
#else
screen.clearDisplay();
#endif
}
void lineColor(int x,int y,String s,int size,uint16_t color) {
  surface.setTextSize(size);surface.setTextColor(color);
  for(unsigned i=0;i<s.length();) {
    uint32_t cp=(uint8_t)s[i++];
    if(cp>=0xe0&&cp<0xf0&&i+1<s.length()){cp=((cp&15)<<12)|(((uint8_t)s[i]&63)<<6)|((uint8_t)s[i+1]&63);i+=2;}
    else if(cp>=0xc0&&cp<0xe0&&i<s.length()){cp=((cp&31)<<6)|((uint8_t)s[i++]&63);}
    if(cp<128){if(x+6*size>240)break;surface.setCursor(x,y+2*size);surface.write(cp);x+=6*size;continue;}
    if(x+12*size>240)break;
    for(const auto &g:ZH_GLYPHS)if(g.code==cp){for(int row=0;row<12;row++)for(int col=0;col<12;col++)if(g.rows[row]&(1<<(11-col)))surface.fillRect(x+col*size,y+row*size,size,size,color);break;}
    x+=12*size;
  }
}
void line(int x,int y,String s,int size=1) { lineColor(x,y,s,size,0xffff); }
void qr(const String &text,int x,int y,int scale){
#if CUBE_TFT
  if(text.isEmpty()||text.length()>180)return;
  uint8_t data[qrcode_getBufferSize(8)];QRCode code;
  if(qrcode_initText(&code,data,8,ECC_LOW,text.c_str())!=0)return;
  const int quiet=4*scale;surface.fillRect(x-quiet,y-quiet,(code.size+8)*scale,(code.size+8)*scale,0xffff);
  for(int row=0;row<code.size;row++)for(int col=0;col<code.size;col++)
    if(qrcode_getModule(&code,col,row))surface.fillRect(x+col*scale,y+row*scale,scale,scale,0);
#endif
}
bool weeklyConfigured(){return weeklyprofiles::valid(weeklyProfiles.as<JsonVariantConst>());}
String weeklyPersonName(const char *key){return strcmp(key,"kevin")==0?"Kevin":"晴晴";}
void weeklyOccurrence(int64_t now,bool dueOnly,int64_t handledEpoch,int64_t &best,String &label){
 if(!weeklyConfigured())return;
 time_t base=now;tm today={};localtime_r(&base,&today);
 const int firstDelta=dueOnly?0:0,lastDelta=dueOnly?0:7;
 for(int delta=firstDelta;delta<=lastDelta;delta++){
  tm date=today;date.tm_mday+=delta;date.tm_hour=12;date.tm_min=0;date.tm_sec=0;time_t noon=mktime(&date);localtime_r(&noon,&date);
  const char *group=date.tm_wday==4?"thursday":(date.tm_wday>=1&&date.tm_wday<=5?"regular":nullptr);if(!group)continue;
  for(const char *person:{"kevin","qingqing"}){
   String timesKey=String(group)+"_times",disabledKey=String("disabled_")+group+"_times";std::set<String> disabled;
   for(JsonVariantConst item:weeklyProfiles[person][disabledKey].as<JsonArrayConst>())disabled.insert(item.as<String>());
   for(JsonVariantConst item:weeklyProfiles[person][timesKey].as<JsonArrayConst>()){
    String text=item.as<String>();if(disabled.count(text))continue;int hour,minute;if(!localcalendar::time(text.c_str(),hour,minute))continue;
    tm candidate=date;candidate.tm_hour=hour;candidate.tm_min=minute;candidate.tm_sec=0;int64_t epoch=mktime(&candidate);
    const bool match=dueOnly?alarmclock::due(epoch,now,handledEpoch):alarmclock::upcoming(epoch,now,handledEpoch);if(!match)continue;
    if(epoch<best){best=epoch;label=weeklyPersonName(person)+" 上班鬧鐘";}
    else if(epoch==best&&!label.startsWith(weeklyPersonName(person))){const String suffix=" 上班鬧鐘";if(label.endsWith(suffix))label.remove(label.length()-suffix.length());label+="、"+weeklyPersonName(person)+suffix;}
   }
  }
 }
}
int64_t nextAlarmEpoch(int64_t now){
  int64_t next=INT64_MAX;
  for(auto &alarm:alarms)if(alarmclock::upcoming(alarm.epoch,now,handled)&&alarm.epoch<next)next=alarm.epoch;
  String ignored;weeklyOccurrence(now,false,handled,next,ignored);
  return next;
}
String weekday(const tm &value){static const char *days[]={"日","一","二","三","四","五","六"};return days[value.tm_wday];}
String dateWeek(int64_t epoch){time_t value=epoch;tm local={};localtime_r(&value,&local);char date[16];snprintf(date,sizeof(date),"%02d/%02d",local.tm_mon+1,local.tm_mday);return String(date)+"（"+weekday(local)+"）";}
String alarmTime(int64_t epoch){time_t value=epoch;tm local={};localtime_r(&value,&local);char text[8];snprintf(text,sizeof(text),"%02d:%02d",local.tm_hour,local.tm_min);return text;}
bool backendReachableNow(uint32_t nowMs);
String tailnetLabel(alarm_tailnet_state_t state);
wifiindicator::Value wifiIndicatorValue(){
  const bool trying=connecting||wifiState.phase==wififailover::Phase::Scanning||wifiState.phase==wififailover::Phase::Connecting;
  return wifiindicator::value(WiFi.isConnected(),trying,WiFi.isConnected()?WiFi.RSSI():0);
}
void drawWifiIndicator(){
  const auto v=wifiIndicatorValue();
  const int x=wifiindicator::X,y=wifiindicator::Y;
  surface.fillRect(x,y,wifiindicator::WIDTH,wifiindicator::HEIGHT,0x0021);
  for(int i=0;i<4;i++){
    const int height=4+i*3;
    const uint16_t color=v.state==wifiindicator::State::Connected&&i<v.bars?0xaf7b:0x4208;
    surface.fillRect(x+i*6,y+14-height,4,height,color);
  }
  if(v.state==wifiindicator::State::Offline){surface.drawLine(x+2,y+1,x+21,y+15,0xf800);surface.drawLine(x+21,y+1,x+2,y+15,0xf800);}
  else if(v.state==wifiindicator::State::Connecting){surface.drawCircle(x+26,y+7,2,0xffe0);}
}
void draw() {
  if(!screenAwake)return;
  fill(ringing && (millis()/500)%2 ? 0x7800 : 0x0021); surface.setTextColor(0xffff);
#if CUBE_TFT
  if(ringing){
    line(36,52,"鬧鐘響了",2);line(24,104,ringLabel,1);line(18,166,"按任一按鈕停止",2);
  } else if(portal&&!(WiFi.isConnected()&&apCloseAt)){
    line(8,deviceui::TITLE_Y,"手機設定",2);
    if(portalView<2){
      String text=portalView==0?String("WIFI:T:WPA;S:")+apName+";P:"+apPassword+";;":"http://192.168.4.1";
      qr(text,deviceui::QR_X,deviceui::QR_Y,deviceui::QR_SCALE);
      line(portalView==0?48:42,deviceui::BOTTOM_TEXT_Y,portalView==0?"右鍵切換 · 掃碼加入":"右鍵切換 · 掃碼設定");
    }else{
      line(8,54,"無法掃碼時手動加入");line(8,88,String("熱點：")+apName);line(8,116,String("密碼：")+apPassword);line(8,150,"設定頁：192.168.4.1");line(8,184,connecting?"正在連線":setupFailed?"連線失敗，請重試":"右鍵返回條碼");
    }
  } else if(uiState.page==deviceui::Page::Main){
    const int64_t now=time(nullptr);time_t current=now;tm local={};localtime_r(&current,&local);char date[40];if(clockValid())snprintf(date,sizeof(date),"%02d/%02d（%s）",local.tm_mon+1,local.tm_mday,weekday(local).c_str());else snprintf(date,sizeof(date),"--/--");lineColor(6,6,date,1,0xce79);
    int percent=0;uint32_t age=0;const bool valid=battery::value(batteryState,millis(),percent,age);const auto batteryUi=deviceui::batteryDisplay(valid,percent);const uint16_t color=batteryUi.warning?0xf800:0xffff;
    surface.drawRect(154,5,26,13,color);surface.fillRect(180,9,3,5,color);if(valid){const int width=(22*percent)/100;if(width)surface.fillRect(156,7,width,9,batteryUi.warning?color:0xaf7b);}lineColor(187,4,valid?String(percent)+"%":"--%",1,color);if(batteryUi.show_marker)lineColor(230,5,"!",1,color);if(chargingInputValid&&charging){lineColor(230,18,"+",1,0xaf7b);}
     drawWifiIndicator();
     char clockText[8];if(clockValid())snprintf(clockText,sizeof(clockText),"%02d:%02d",local.tm_hour,local.tm_min);else snprintf(clockText,sizeof(clockText),"--:--");line(45,37,clockText,5);
    const int64_t next=clockValid()?nextAlarmEpoch(now):INT64_MAX;
    surface.drawFastHLine(8,96,224,0x31e7);
    if(next==INT64_MAX){lineColor(8,108,"下次上班",1,0x9d34);line(142,106,"--",2);lineColor(8,137,"響鈴時間",1,0x9d34);line(142,132,"--:--",2);surface.fillRect(8,174,224,34,0x11c5);lineColor(28,180,"尚無下一次鬧鐘",2,0xaf7b);}
    else{lineColor(8,108,"下次上班",1,0x9d34);line(142,108,dateWeek(next));lineColor(8,137,"響鈴時間",1,0x9d34);line(142,132,alarmTime(next),2);char countdown[96];deviceui::countdown(now,next,countdown,sizeof(countdown));surface.fillRect(8,174,224,34,0x11c5);lineColor(16,184,countdown,1,0xaf7b);}
    if(!savedScheduleRestored){surface.fillRect(8,174,224,42,0xf800);line(16,187,"班表讀取失敗，鬧鐘暫停");}
    else if(!clockValid()){surface.fillRect(8,174,224,42,0xf800);line(22,187,"等待校時，鬧鐘暫停");}
    else if(pairingHoldActive){surface.fillRect(8,174,224,34,0x11c5);lineColor(18,184,String("配網倒數 ")+String(buttons::pairingSecondsRemaining(buttonState,millis()))+" 秒",1,0xaf7b);}
    lineColor(36,220,"左右鍵查看資訊 · 中鍵進入",1,0x8410);
  } else if(uiState.page==deviceui::Page::Menu){
    static const char *items[]={"手機設定","連線狀態","鬧鐘資訊","裝置資訊","檢查更新","返回主畫面"};line(8,6,"更多資訊",2);
    for(uint8_t i=0;i<deviceui::MENU_ITEM_COUNT;i++){const int y=38+i*30;if(i==uiState.selection){surface.fillRect(6,y-3,228,27,0xaf7b);lineColor(18,y,String("> ")+items[i],1,0x1103);}else{surface.drawRect(6,y-3,228,27,0x31e7);line(18,y,String("  ")+items[i]);}}
  } else {
    const int64_t now=time(nullptr),next=clockValid()?nextAlarmEpoch(now):INT64_MAX;alarm_tailnet_status_t tail={};alarm_tailnet_get_status(&tail);int percent=0;uint32_t age=0;const bool valid=battery::value(batteryState,millis(),percent,age);
    if(uiState.page==deviceui::Page::PhoneSetup){line(8,deviceui::TITLE_Y,"手機設定",2);if(WiFi.isConnected()){String url=String("http://")+WiFi.localIP().toString()+"/calendar";qr(url,deviceui::QR_X,deviceui::QR_Y,deviceui::QR_SCALE);line(deviceui::BOTTOM_TEXT_X,deviceui::BOTTOM_TEXT_Y,"掃碼設定班表");}else{line(8,48,"尚未連上無線網路");line(8,76,"按住左右鍵 10 秒");line(8,98,"再依畫面加入裝置熱點");line(8,126,"手機開啟 192.168.4.1");}}
    else if(uiState.page==deviceui::Page::Connectivity){const bool remoteOk=tail.state==ALARM_TAILNET_CONNECTED;const bool backendOk=backendReachableNow(millis());line(8,6,"連線狀態",2);line(8,42,String("目前網路：")+currentWifiSsid());line(8,68,String("已保存 ")+String(unsigned(wifiProfiles.count))+" 組");line(8,94,String("遠端連線：")+(remoteOk?"已連線":"未連線"));line(8,120,String("後端服務：")+(backendOk?"可連線":"無法連線"));lineColor(remoteOk&&backendOk?34:8,154,remoteOk&&backendOk?"遠端與後端連線正常":"連線異常不影響本機鬧鐘",1,remoteOk&&backendOk?0xaf7b:0xf800);}
    else if(uiState.page==deviceui::Page::Schedule){line(8,6,"鬧鐘資訊",2);if(scheduleStorageFault.load()){line(8,48,"鬧鐘儲存狀態不明");line(8,76,"重開前請勿修改");line(8,108,"目前僅沿用本次開機設定");}else{line(8,44,String("下次上班：")+(next==INT64_MAX?"尚無":dateWeek(next)));line(8,70,String("響鈴時間：")+(next==INT64_MAX?"--:--":alarmTime(next)));line(8,96,String("啟用時段：")+String(unsigned(alarms.size()+weeklyprofiles::enabledCount(weeklyProfiles.as<JsonVariantConst>()))));line(8,122,String("儲存狀態：")+(revision.isEmpty()?"尚未儲存":localSchedule?"本機已儲存":"已同步"));line(8,148,String("版次：")+(revision.isEmpty()?"--":revision.substring(0,18)));}}
    else if(uiState.page==deviceui::Page::Device){line(8,6,"裝置資訊",2);line(8,48,String("韌體版本：")+VERSION);line(8,78,String("IP：")+(WiFi.isConnected()?WiFi.localIP().toString():"未連線"));line(8,108,String("電池：")+(valid?String(percent)+"%":"未知"));line(8,134,String("充電：")+(chargingInputValid?(charging.load()?"是":"否"):"未知"));}
    else if(uiState.page==deviceui::Page::Update){alarm_ota_status_t ota={};alarm_ota_get_status(&ota);line(8,deviceui::TITLE_Y,"檢查更新",2);if(WiFi.isConnected()){String url=String("http://")+WiFi.localIP().toString()+"/update";qr(url,deviceui::QR_X,deviceui::QR_Y,deviceui::QR_SCALE);const uint8_t detail=(millis()/2000)%4;if(detail==0)line(66,deviceui::BOTTOM_TEXT_Y,"掃碼開啟更新頁");else if(detail==1)line(66,deviceui::BOTTOM_TEXT_Y,String("目前：")+VERSION);else if(detail==2)line(ota.marker_fault?48:ota.staged_valid?66:72,deviceui::BOTTOM_TEXT_Y,ota.marker_fault?"已下載狀態異常":ota.staged_valid?String("已下載：")+ota.staged.manifest.version:"已下載：沒有");else line(60,deviceui::BOTTOM_TEXT_Y,String("充電：")+(chargingInputValid&&charging.load()?"可以安裝":"尚未就緒"));}else line(8,120,"連上無線網路後顯示條碼");}
    const bool qrPage=WiFi.isConnected()&&(uiState.page==deviceui::Page::PhoneSetup||uiState.page==deviceui::Page::Update);
    if(!qrPage)line(8,220,"左右切換 · 中鍵返回");
  }
  if(pairingHoldActive&&uiState.page!=deviceui::Page::Main){surface.fillRect(8,210,224,28,0x11c5);lineColor(34,217,String("配網倒數 ")+String(buttons::pairingSecondsRemaining(buttonState,millis()))+" 秒",1,0xaf7b);}
  uint16_t *pixels=frame->getBuffer(); uint32_t hash=2166136261u;
  for(size_t i=0;i<240*240;i++){hash^=pixels[i];hash*=16777619u;}
  if(forceDraw||hash!=previousFrameHash){screen.drawRGBBitmap(0,0,pixels,240,240);previousFrameHash=hash;forceDraw=false;}
#else
  line(0,0,ringing?"鬧鐘響了，任意鍵停止":"班表鬧鐘");
  line(0,12,clockValid()?datetime(time(nullptr)):"等待時間校正");
  int64_t next=INT64_MAX; for(auto &a:alarms)if(alarmclock::upcoming(a.epoch,time(nullptr),handled)&&a.epoch<next)next=a.epoch;
  line(0,26,next==INT64_MAX?"目前沒有鬧鐘":datetime(next));
  line(0,40,portal?"裝置配網熱點":syncState);
  line(0,52,portal?apPassword:WiFi.localIP().toString()); screen.display();
#endif
}
void soundTask(void*) {
  int16_t samples[256*2]; uint32_t phase=0;
  for(;;) {
    bool play=ringing && (millis()%1000)<700;
    for(int i=0;i<256;i++) { int16_t v=play?((phase++%24)<12?5000:-5000):0; samples[i*2]=samples[i*2+1]=v; }
    size_t written=0;esp_err_t result=i2s_write(I2S_NUM_0,samples,sizeof(samples),&written,portMAX_DELAY);
    if(result==ESP_OK&&written==sizeof(samples))speakerReady=true;
  }
}
void closePortal(){if(!portal)return;portal=false;apCloseAt=0;dns.stop();WiFi.softAPdisconnect(true);WiFi.mode(WIFI_STA);}
void startRing(String label) { ringLabel=label;ringStarted=millis();if(!ringing){closePortal();uiState.page=deviceui::Page::Main;buttons::beginRinging(buttonState,ringStarted,!digitalRead(BUTTON_SNOOZE),!digitalRead(BUTTON_STOP),!digitalRead(BUTTON_TEST));}ringing=true;wakeScreen();forceDraw=true;Serial.println("ALARM_RING_STARTED"); }
void stopRing(bool doSnooze) { ringing=false;buttons::endRinging(buttonState);snooze=doSnooze&&clockValid()?time(nullptr)+alarmclock::SNOOZE_SECONDS:0;prefs.putLong64("snooze",snooze);forceDraw=true;Serial.println(doSnooze?"ALARM_SNOOZED":"ALARM_STOPPED"); }
#include "calendar_metadata.h"
void applyScheduleCandidate(schedulecandidate::Candidate &candidate,bool sideEffects=true){
  alarms.swap(candidate.alarms);swap(calendarMonths,candidate.months);swap(weeklyProfiles,candidate.weekly_profiles);revision=std::move(candidate.revision);localSchedule=candidate.local;firstConsecutiveOnly=candidate.first_only;forceDraw=true;
  ++scheduleGeneration;
  backendpoll::invalidate(backendPollState);
  if(!sideEffects)return;
  if(!clockValid()&&candidate.server_time>=alarmclock::VALID_CLOCK){timeval tv={};tv.tv_sec=candidate.server_time;settimeofday(&tv,nullptr);}
  if(candidate.management_url.startsWith("http")&&candidate.management_url!=manageUrl){manageUrl=candidate.management_url;prefs.putString("manage",manageUrl);}
  if(candidate.stop_command&&candidate.command_id.length()&&candidate.command_id!=lastCommand&&clockValid()&&time(nullptr)-candidate.command_issued>=0&&time(nullptr)-candidate.command_issued<120){lastCommand=candidate.command_id;prefs.putString("command",lastCommand);stopRing(false);}
}
bool applySchedule(const String &body,bool persist,String &error){
  if(persist){error="班表儲存必須由背景工作執行";return false;}schedulecandidate::Candidate candidate;if(!schedulecandidate::parse(body,candidate,error))return false;applyScheduleCandidate(candidate,false);return true;
}
bool scheduleNvsRead(String &out){
  nvs_handle_t handle;if(nvs_open_from_partition("alarm_nvs","shift-alarm",NVS_READONLY,&handle)!=ESP_OK)return false;size_t size=0;esp_err_t err=nvs_get_blob(handle,"schedule",nullptr,&size);if(err==ESP_ERR_NVS_NOT_FOUND){nvs_close(handle);out="";return true;}if(err!=ESP_OK||size>MAX_JSON){nvs_close(handle);return false;}std::vector<char> bytes(size+1,0);err=nvs_get_blob(handle,"schedule",bytes.data(),&size);nvs_close(handle);if(err!=ESP_OK)return false;out=String(bytes.data());return out.length()==size;
}
bool scheduleNvsWriteExact(const String &value){
  nvs_handle_t handle;if(nvs_open_from_partition("alarm_nvs","shift-alarm",NVS_READWRITE,&handle)!=ESP_OK)return false;esp_err_t err=value.isEmpty()?nvs_erase_key(handle,"schedule"):nvs_set_blob(handle,"schedule",value.c_str(),value.length());if(err==ESP_ERR_NVS_NOT_FOUND&&value.isEmpty())err=ESP_OK;if(err==ESP_OK)err=nvs_commit(handle);size_t size=0;if(err==ESP_OK)err=nvs_get_blob(handle,"schedule",nullptr,&size);bool ok=value.isEmpty()?err==ESP_ERR_NVS_NOT_FOUND:err==ESP_OK&&size==value.length();if(ok&&!value.isEmpty()){std::vector<char> check(size+1,0);ok=nvs_get_blob(handle,"schedule",check.data(),&size)==ESP_OK&&memcmp(check.data(),value.c_str(),size)==0;}nvs_close(handle);return ok;
}
bool scheduleFaultMarker(bool write){nvs_handle_t handle;if(nvs_open_from_partition("alarm_nvs","shift-alarm",write?NVS_READWRITE:NVS_READONLY,&handle)!=ESP_OK)return write?false:true;uint8_t value=0;esp_err_t err;if(write){err=nvs_set_u8(handle,"schedule-fault-v1",1);if(err==ESP_OK)err=nvs_commit(handle);if(err==ESP_OK)err=nvs_get_u8(handle,"schedule-fault-v1",&value);}else err=nvs_get_u8(handle,"schedule-fault-v1",&value);nvs_close(handle);return write?err==ESP_OK&&value==1:err==ESP_OK&&value==1;}
void latchScheduleStorageFault(){scheduleStorageFault.store(true);scheduleFaultMarker(true);}
bool buildLocalSchedule(const String &base,const String &patch,schedulecandidate::Candidate &candidate,String &error){
  schedulecandidate::Candidate current;if(!base.isEmpty()&&!schedulecandidate::parse(base,current,error))return false;JsonDocument input;int year,month;if(deserializeJson(input,patch)){error="班表資料格式無效";return false;}
  if(input.as<JsonObjectConst>().containsKey("weekly_profiles")){
    if(!weeklyprofiles::valid(input["weekly_profiles"])){error="Kevin 與晴晴的每週鬧鐘設定無效";return false;}
    JsonDocument next;next["revision"]=String("local-")+String(esp_random(),HEX)+String(esp_random(),HEX);next["timezone"]="Asia/Taipei";next["source"]="local";next["first_consecutive_only"]=false;next["months"].to<JsonObject>();next["weekly_profiles"]=input["weekly_profiles"];
    auto list=next["alarms"].to<JsonArray>();for(const auto &alarm:current.alarms)if(alarm.id.startsWith("test-")){auto saved=list.add<JsonObject>();saved["id"]=alarm.id;saved["label"]=alarm.label;saved["epoch"]=alarm.epoch;}
    if(next.overflowed()){error="每週鬧鐘設定記憶體不足，原設定未變更";return false;}String full;serializeJson(next,full);return schedulecandidate::parse(full,candidate,error);
  }
  auto first=input["first_consecutive_only"];
  if(!input["month"].is<String>()||!localcalendar::month(input["month"].as<String>().c_str(),year,month)||!input["days"].is<JsonArray>()||!input["times"].is<JsonArray>()||(!input["disabled_times"].isNull()&&!input["disabled_times"].is<JsonArray>())||input["days"].size()>31||(input["days"].size()&&!input["times"].size())||input["times"].size()>8||input["disabled_times"].size()>8||(!first.isNull()&&!first.is<bool>())){error="請選擇有效月份、日期、鬧鐘時間及通知方式";return false;}
  std::vector<int> days;std::vector<String> times;std::set<String> disabled;for(JsonVariant v:input["days"].as<JsonArray>()){if(!v.is<int>()||v.as<int>()<1||v.as<int>()>localcalendar::days(year,month)||std::find(days.begin(),days.end(),v.as<int>())!=days.end()){error="上班日期無效或重複";return false;}days.push_back(v.as<int>());}for(JsonVariant v:input["times"].as<JsonArray>()){int h,m;String at=v.as<String>();if(!v.is<String>()||!localcalendar::time(at.c_str(),h,m)||std::find(times.begin(),times.end(),at)!=times.end()){error="鬧鐘時間無效或重複，請使用 07:00 格式";return false;}times.push_back(at);}for(JsonVariant v:input["disabled_times"].as<JsonArray>()){String at=v.as<String>();if(!v.is<String>()||std::find(times.begin(),times.end(),at)==times.end()||!disabled.insert(at).second){error="停用的鬧鐘時間無效或重複";return false;}}
  const bool firstOnly=first.is<bool>()?first.as<bool>():current.first_only;JsonDocument next;next["revision"]=String("local-")+String(esp_random(),HEX)+String(esp_random(),HEX);next["timezone"]="Asia/Taipei";next["source"]="local";next["first_consecutive_only"]=firstOnly;auto list=next["alarms"].to<JsonArray>();next["months"]=current.months;String monthKey=input["month"].as<String>();auto metadata=next["months"][monthKey].to<JsonObject>();metadata["days"]=input["days"];metadata["times"]=input["times"];auto disabledMeta=metadata["disabled_times"].to<JsonArray>();for(const auto &at:disabled)disabledMeta.add(at);if(!localcalendar::validMonths(next["months"])){error="月份設定格式無效或超過 120 個月";return false;}
  std::set<int64_t> known;for(JsonPairConst entry:next["months"].as<JsonObjectConst>()){int y,m;localcalendar::month(entry.key().c_str(),y,m);for(JsonVariantConst day:entry.value()["days"].as<JsonArrayConst>())known.insert(localcalendar::epoch(y,m,day.as<int>(),0,0));}
  for(const auto &alarm:current.alarms){time_t value=alarm.epoch;tm date;localtime_r(&value,&date);char text[32];snprintf(text,sizeof(text),"%04d-%02d",date.tm_year+1900,date.tm_mon+1);String key=text;bool currentMonth=date.tm_year+1900==year&&date.tm_mon+1==month,backed=next["months"][key].is<JsonObject>();if(alarm.id.startsWith("local-")&&!backed)known.insert(localcalendar::epoch(date.tm_year+1900,date.tm_mon+1,date.tm_mday,0,0));if(!alarm.id.startsWith("test-")&&(currentMonth||(alarm.id.startsWith("local-")&&backed)))continue;auto saved=list.add<JsonObject>();saved["id"]=alarm.id;saved["label"]=alarm.label;saved["epoch"]=alarm.epoch;}
  for(JsonPairConst entry:next["months"].as<JsonObjectConst>()){int y,m;localcalendar::month(entry.key().c_str(),y,m);std::set<String> off;for(JsonVariantConst at:entry.value()["disabled_times"].as<JsonArrayConst>())off.insert(at.as<String>());for(JsonVariantConst day:entry.value()["days"].as<JsonArrayConst>()){int selected=day.as<int>();int64_t work=localcalendar::epoch(y,m,selected,0,0);if(!localcalendar::shouldNotify(work,known,firstOnly))continue;for(JsonVariantConst av:entry.value()["times"].as<JsonArrayConst>()){String at=av.as<String>();if(off.count(at))continue;if(list.size()>=MAX_ALARMS){error="已超過裝置可儲存的 512 個鬧鐘，請先清除不需要的月份";return false;}int h,min;localcalendar::time(at.c_str(),h,min);auto alarm=list.add<JsonObject>();alarm["id"]=String("local-")+entry.key().c_str()+"-"+selected+"-"+at;alarm["label"]="上班鬧鐘";alarm["epoch"]=localcalendar::epoch(y,m,selected,h,min);}}}
  if(next.overflowed()){error="班表設定記憶體不足，原設定未變更";return false;}String full;serializeJson(next,full);return schedulecandidate::parse(full,candidate,error);
}
void wipeScheduleWork(ScheduleWork *work){if(!work)return;wipeString(work->input);wipeString(work->previous);for(auto &alarm:work->candidate.alarms){wipeString(alarm.id);wipeString(alarm.label);}work->candidate.alarms.clear();work->candidate.months.clear();work->candidate.weekly_profiles.clear();wipeString(work->candidate.revision);wipeString(work->candidate.canonical);wipeString(work->candidate.management_url);wipeString(work->candidate.command_id);delete work;}
void scheduleWorker(void*){for(;;){ScheduleWork *work=nullptr;if(xQueueReceive(scheduleRequestQueue,&work,portMAX_DELAY)!=pdTRUE||!work)continue;if(work->origin==ScheduleOrigin::Cleanup){wipeScheduleWork(work);continue;}if(work->origin==ScheduleOrigin::Rollback){work->persisted=scheduleNvsWriteExact(work->previous);if(!work->persisted)latchScheduleStorageFault();}else if(!scheduleNvsRead(work->previous))work->error="無法讀取既有班表";else if(work->origin==ScheduleOrigin::Local?!buildLocalSchedule(work->previous,work->input,work->candidate,work->error):!schedulecandidate::parse(work->input,work->candidate,work->error)){}else{work->persisted=scheduleNvsWriteExact(work->candidate.canonical);if(!work->persisted&&!scheduleNvsWriteExact(work->previous)){work->error="班表儲存狀態不明；重開前請勿修改";latchScheduleStorageFault();}}if(!work->persisted&&work->error.isEmpty())work->error="班表儲存失敗；未變更執行中班表";wipeString(work->input);if(xQueueSend(scheduleResultQueue,&work,0)!=pdTRUE){if(work->persisted&&work->origin!=ScheduleOrigin::Rollback&&!scheduleNvsWriteExact(work->previous))latchScheduleStorageFault();scheduleDroppedId.store(work->id);wipeScheduleWork(work);}}}
bool initScheduleWorker(){scheduleRequestQueue=xQueueCreate(1,sizeof(ScheduleWork*));scheduleResultQueue=xQueueCreate(1,sizeof(ScheduleWork*));if(!scheduleRequestQueue||!scheduleResultQueue){if(scheduleRequestQueue)vQueueDelete(scheduleRequestQueue);if(scheduleResultQueue)vQueueDelete(scheduleResultQueue);scheduleRequestQueue=nullptr;scheduleResultQueue=nullptr;return false;}if(xTaskCreate(scheduleWorker,"schedule_store",12288,nullptr,1,nullptr)!=pdPASS){vQueueDelete(scheduleRequestQueue);vQueueDelete(scheduleResultQueue);scheduleRequestQueue=nullptr;scheduleResultQueue=nullptr;return false;}return true;}
bool enqueueScheduleWork(ScheduleOrigin origin,String &&input,uint32_t &id){if(scheduleStorageFault.load()||!scheduleWorkReady||scheduleWorkBusy)return false;auto *work=new(std::nothrow)ScheduleWork;if(!work)return false;work->id=++scheduleWorkId;work->schedule_generation=scheduleGeneration;work->backend_generation=backendPollState.validity_generation;work->origin=origin;work->input=std::move(input);if(xQueueSend(scheduleRequestQueue,&work,0)!=pdTRUE){wipeScheduleWork(work);return false;}scheduleWorkBusy=true;id=work->id;return true;}
void drainScheduleResult(){
 const uint32_t dropped=scheduleDroppedId.exchange(0);
 if(dropped){scheduleWorkBusy=false;syncState=scheduleStorageFault.load()?"班表儲存狀態不明；重開前請勿修改":"班表結果傳遞失敗；未變更執行中班表";if(dropped==localSaveId){localSaveState="error";localSaveMessage=syncState;}}
 if(scheduleStorageFault.load()&&!scheduleStorageFaultObserved){scheduleStorageFaultObserved=true;++scheduleGeneration;backendpoll::invalidate(backendPollState);scheduleWorkBusy=false;localSaveState="error";localSaveMessage="班表儲存狀態不明；重開前請勿修改";}
 if(!scheduleResultQueue)return;
 ScheduleWork *work=nullptr;
 while(xQueueReceive(scheduleResultQueue,&work,0)==pdTRUE){
  scheduleWorkBusy=false;
  if(work->origin==ScheduleOrigin::Rollback){if(!work->persisted){latchScheduleStorageFault();syncState="班表儲存狀態不明；重開前請勿修改";}work->origin=ScheduleOrigin::Cleanup;if(xQueueSend(scheduleRequestQueue,&work,0)!=pdTRUE)wipeScheduleWork(work);continue;}
  const bool current=!scheduleStorageFault.load()&&work->schedule_generation==scheduleGeneration&&(work->origin!=ScheduleOrigin::Backend||work->backend_generation==backendPollState.validity_generation);
  if(work->persisted&&current){const bool local=work->origin==ScheduleOrigin::Local;applyScheduleCandidate(work->candidate);syncState=local?"班表已儲存":"班表已同步";if(local){localSaveState="saved";localSaveMessage="已儲存";}work->origin=ScheduleOrigin::Cleanup;if(xQueueSend(scheduleRequestQueue,&work,0)!=pdTRUE)wipeScheduleWork(work);}
  else if(work->persisted){if(work->origin==ScheduleOrigin::Local){localSaveState="error";localSaveMessage="班表已被較新的設定取代";}work->origin=ScheduleOrigin::Rollback;work->persisted=false;work->error="";scheduleWorkBusy=xQueueSend(scheduleRequestQueue,&work,0)==pdTRUE;if(!scheduleWorkBusy){latchScheduleStorageFault();syncState="班表儲存狀態不明；重開前請勿修改";wipeScheduleWork(work);}}
  else{syncState=scheduleStorageFault.load()?"班表儲存狀態不明；重開前請勿修改":work->error;if(work->origin==ScheduleOrigin::Local){localSaveState="error";localSaveMessage=syncState;}work->origin=ScheduleOrigin::Cleanup;if(xQueueSend(scheduleRequestQueue,&work,0)!=pdTRUE)wipeScheduleWork(work);}
 }
}
bool authorized() { if(token.length()&&server.header("Authorization")==String("Bearer ")+token)return true; server.send(401,"application/json","{\"error\":\"需要裝置授權\"}");return false; }
void startPortal() { wakeScreen();if(portal)return;portal=true; WiFi.mode(WIFI_AP_STA); WiFi.softAP(apName.c_str(),apPassword.c_str());dns.start(53,"*",WiFi.softAPIP());Serial.println("SETUP_AP_STARTED");Serial.println(String("SETUP_AP_SSID ")+apName);Serial.println("SETUP_URL http://192.168.4.1"); }
void wifiBackoff(uint32_t now){wififailover::retryLater(wifiState,now);}
void startWifiScan(uint32_t now){
  if(wifiProfiles.count==0){startPortal();wifiState.phase=wififailover::Phase::Idle;return;}
  WiFi.scanDelete();const int result=WiFi.scanNetworks(true,true);
  if(result==WIFI_SCAN_FAILED){wifiBackoff(now);return;}
  wififailover::beginScan(wifiState,now);connectStarted=now;
}
void connectSavedWifi(size_t index,uint32_t now){
  if(index>=wifiProfiles.count){wifiBackoff(now);return;}
  const auto &profile=wifiProfiles.profiles[index];
  Serial.printf("WIFI_CONNECT_ATTEMPT index=%u ssid=%s\n",unsigned(index),profile.ssid.c_str());
  WiFi.begin(profile.ssid.c_str(),profile.password.c_str());wifiState.phase=wififailover::Phase::Connecting;wifiState.phase_started_ms=now;connectStarted=now;
}
void clearPendingWifi(){pendingPassword.clear();pendingSsid.clear();wifiPendingTrial=false;wifiTrialStarted=false;wifiTrialAt=0;connecting=false;}
void restartWifiSelection(uint32_t now){
  wifiState={};wifiState.phase_started_ms=now;wifiState.retry_delay_ms=0;
  if(wifiProfiles.count)startWifiScan(now);else startPortal();
}
void cancelWifiSelection(uint32_t now){
  WiFi.scanDelete();WiFi.disconnect(false,false);clearPendingWifi();wififailover::cancelForMutation(wifiState,now);
}
void continueSavedWifi(uint32_t now){
  const int candidate=wififailover::nextCandidate(wifiState,now);if(candidate>=0)connectSavedWifi(size_t(candidate),now);else wifiBackoff(now);
}
void serviceWifi(uint32_t now){
  if(wifiStorageFault&&wififailover::elapsed(now,wifiMigrationAttempt,1000)){
    wifiMigrationAttempt=now;wifiprofiles::Record recovered;wifiprofiles::Selector selected;
    const auto result=wifiprofiles::load(wifiStorage,recovered,selected);
    if(result==wifiprofiles::LoadResult::Ok&&clearInactiveWifiSlot(selected.slot)){wifiProfiles=recovered.list;wifiSelector=selected;wifiSelectorValid=true;wifiStorageFault=false;wifiMigrationPending=false;prefs.remove(WIFI_CREDENTIALS_KEY);prefs.remove("ssid");prefs.remove("password");cancelWifiSelection(now);restartWifiSelection(now);}
    else if(result==wifiprofiles::LoadResult::Empty&&(prefs.isKey(WIFI_CREDENTIALS_KEY)||prefs.isKey("ssid"))){wifiStorageFault=false;wifiSelectorValid=false;wifiMigrationPending=true;}
  }
  if(wifiMigrationPending&&!wifiStorageFault&&wififailover::elapsed(now,wifiMigrationAttempt,60000)){
    wifiMigrationAttempt=now;wifiprofiles::Selector committed;const auto result=persistWifiProfiles(wifiProfiles,committed);
    if(result==wifiprofiles::CommitResult::Committed){wifiSelector=committed;wifiSelectorValid=true;prefs.remove(WIFI_CREDENTIALS_KEY);prefs.remove("ssid");prefs.remove("password");wifiMigrationPending=false;}
  }
  if(wifiPendingTrial&&!wifiTrialStarted&&wififailover::deadlineReached(now,wifiTrialAt)){wifiTrialStarted=true;connectStarted=now;Serial.printf("WIFI_TRIAL_BEGIN ssid=%s\n",pendingSsid.c_str());WiFi.begin(pendingSsid.c_str(),pendingPassword.c_str());}
  if(WiFi.isConnected()){
    if(wifiPendingTrial&&pendingSsid.equals(WiFi.SSID().c_str(),WiFi.SSID().length())){
      wifiprofiles::List next=wifiProfiles;
      const auto result=wifiprofiles::addOrUpdate(next,{pendingSsid.c_str(),pendingSsid.size(),pendingPassword.c_str(),pendingPassword.size()});wifiprofiles::Selector committed;
      const auto stored=result==wifiprofiles::Result::Ok?persistWifiProfiles(next,committed):wifiprofiles::CommitResult::Failed;
      if(stored==wifiprofiles::CommitResult::Committed||stored==wifiprofiles::CommitResult::CommittedStorageFault){wifiProfiles=next;wifiSelector=committed;wifiSelectorValid=true;setupFailed=false;clearPendingWifi();apCloseAt=now+20000;backendPollState.next_attempt_ms=now;}
      else{setupFailed=true;clearPendingWifi();WiFi.disconnect(false,false);wififailover::candidateFailed(wifiState,now);}
    }else if(wifiPendingTrial){WiFi.disconnect(false,false);}
    else wififailover::healthy(wifiState);
    if(wifiPendingTrial&&wififailover::elapsed(now,connectStarted,wififailover::CONNECT_TIMEOUT_MS)){setupFailed=true;clearPendingWifi();WiFi.disconnect(false,false);wififailover::candidateFailed(wifiState,now);}
    return;
  }
  if(wifiPendingTrial){
    if(wififailover::elapsed(now,connectStarted,wififailover::CONNECT_TIMEOUT_MS)){setupFailed=true;clearPendingWifi();WiFi.disconnect(false,false);wififailover::candidateFailed(wifiState,now);}
    return;
  }
  if(wifiState.phase==wififailover::Phase::Scanning){
    const int scan=WiFi.scanComplete();
    if(scan>=0){
      std::array<wififailover::Observation,wifiprofiles::MAX_PROFILES> observations{};
      for(size_t p=0;p<wifiProfiles.count;p++)for(int i=0;i<scan;i++)if(WiFi.SSID(i).equals(wifiProfiles.profiles[p].ssid.c_str())){
        if(!observations[p].visible||WiFi.RSSI(i)>observations[p].rssi)observations[p]={true,WiFi.RSSI(i)};
      }
      for(size_t p=0;p<wifiProfiles.count;p++)Serial.printf("WIFI_SCAN_SAVED index=%u visible=%d rssi=%d\n",unsigned(p),observations[p].visible,observations[p].rssi);
      wififailover::scanned(wifiState,wififailover::candidates(wifiProfiles,observations),now);continueSavedWifi(now);
    }else if(scan==WIFI_SCAN_FAILED||wififailover::elapsed(now,connectStarted,15000)){WiFi.scanDelete();wifiBackoff(now);}
    return;
  }
  if(wifiState.phase==wififailover::Phase::Connecting){
    if(wififailover::elapsed(now,connectStarted,wififailover::CONNECT_TIMEOUT_MS)){Serial.printf("WIFI_CONNECT_TIMEOUT status=%d\n",int(WiFi.status()));WiFi.disconnect(false,false);wififailover::waitForDisconnect(wifiState,now);}
    return;
  }
  if(wififailover::disconnectSettled(wifiState,now)){
    continueSavedWifi(now);
    return;
  }
  if(wififailover::shouldStartCycle(false,false,now,wifiState.phase_started_ms,wifiState.retry_delay_ms))startWifiScan(now);
}
String devicePage(String content) {
  // Individual pages may add local CSS after their visible controls. Remove
  // only the style element; keeping content before it is essential because the
  // shared shell injects the remaining markup into the card.
  int styleStart=content.indexOf("<style>");
  if(styleStart>=0){int styleEnd=content.indexOf("</style>",styleStart);if(styleEnd>=0)content.remove(styleStart,styleEnd+8-styleStart);}
  int meta;while((meta=content.indexOf("<meta"))>=0){int end=content.indexOf('>',meta);if(end<0)break;content.remove(meta,end-meta+1);}
  return String(R"PAGE(<!doctype html><html lang="zh-Hant"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#152b42"><title>班表鬧鐘・裝置設定</title><style>
:root{color-scheme:light;font-family:system-ui,-apple-system,"Noto Sans TC",sans-serif;color:#152b42;background:#f6f7f3}*{box-sizing:border-box}[hidden]{display:none!important}input[type=checkbox]{width:20px;height:20px;margin-right:8px}body{margin:0;padding:32px 20px 64px;font-size:16px;line-height:1.65}header,main,footer{width:100%;max-width:720px;margin:auto}header{display:flex;align-items:center;gap:14px;margin-bottom:28px}.brand-icon{display:grid;place-items:center;background:#152b42;color:#a8ead6;border-radius:16px;width:52px;height:52px;font-size:32px}.brand small{display:block;color:#668078;font-size:12px;letter-spacing:.08em}.brand strong{font-size:24px;letter-spacing:.02em}.badge{margin-left:auto;background:#e2f1e9;border:1px solid #cce3d6;color:#34614f;border-radius:30px;padding:6px 12px;font-size:12px}.card{background:#fff;border:1px solid #e0e5df;border-radius:24px;box-shadow:0 8px 32px #152b4207;padding:30px}h1{font-size:28px;line-height:1.3;margin:0 0 20px;letter-spacing:-.03em}h2{font-size:21px;border-top:1px solid #e6eae5;padding-top:28px;margin-top:32px}p{color:#63716c;margin:16px 0}form{margin:16px 0 20px}label{display:block;font-weight:600;margin-top:16px}input:not([type=hidden]):not([type=checkbox]),select{display:block;width:100%;min-height:50px;border:1px solid #ced8d1;border-radius:12px;background:#fafcf9;color:#152b42;font:inherit;padding:12px 14px;margin:8px 0 18px}input:focus,select:focus{outline:3px solid #a6dfce;outline-offset:2px}button,.primary-link{display:inline-block;border:0;border-radius:12px;background:#152b42;color:white;font:inherit;font-weight:600;padding:13px 20px;min-height:48px;cursor:pointer;text-align:center}button:hover{background:#28455e}button:active{transform:translateY(1px)}a{color:#276453;text-underline-offset:4px}a.primary-link{color:white;text-decoration:none;display:block}#status:not(:empty){background:#eaf5ee;border-radius:14px;padding:16px;overflow-wrap:anywhere}footer{text-align:center;color:#819087;font-size:12px;padding-top:24px}.help{font-size:13px}@media(max-width:480px){body{padding:22px 14px 40px}.card{padding:22px 20px;border-radius:20px}h1{font-size:25px}.badge{display:none}button{width:100%}}
.calendar{display:grid;grid-template-columns:repeat(7,minmax(0,1fr));gap:5px;text-align:center}.calendar button{position:relative;min-height:44px;padding:5px;border-radius:9px;background:#edf1ee;color:#152b42;width:100%;font-size:16px}.calendar button.workday{background:#152b42;color:#a8ead6}.calendar button.silent-workday{background:#fff;color:#152b42;border:2px solid #152b42}.calendar button.reviewday{background:#ffe4b1;color:#714800}.calendar button.today{box-shadow:inset 0 0 0 2px #d43c32}.calendar button.today::after{content:"";position:absolute;top:5px;right:5px;width:6px;height:6px;border-radius:50%;background:#e23b32;box-shadow:0 0 0 1px #fff}.calendar button:disabled{opacity:.6}.time-row{display:grid;grid-template-columns:auto minmax(0,1fr) minmax(0,1fr) auto auto;gap:8px;align-items:center;margin:12px 0}.time-row label,.time-row select{margin:0!important}.time-row select{padding:10px 6px!important}.time-row button{padding:10px;min-height:48px;width:auto}.alarm-enable{display:flex!important;align-items:center;gap:5px;white-space:nowrap}.alarm-enable input{margin:0}.secondary{background:#e9efec;color:#274c42}.secondary:hover{background:#dce8e1}.upload-box{padding:16px;background:#f1f6f2;border:1px dashed #b9d0c4;border-radius:12px}.check-option{display:flex;align-items:center;gap:8px;padding:14px 0}.check-option input{flex:0 0 auto;margin:0}.time-row strong{font-size:14px}#ai-status{overflow-wrap:anywhere}button:disabled{cursor:wait;opacity:.6}#weekdays{margin-bottom:10px;color:#63716c}@media(max-width:600px){.time-row{grid-template-columns:1fr 1fr auto}.time-row strong{grid-column:1/-1}.alarm-enable{grid-column:1/3}}
.device-health{flex:1 0 100%;font-size:13px;color:#526a62;background:#edf3ef;border-left:4px solid #5b8e79;padding:8px 12px}.device-health.interrupted{border-color:#b4473e;color:#82342e}body{overflow-x:hidden}header{flex-wrap:wrap}input:not([type=hidden]):not([type=checkbox]),select{font-size:16px}@media(max-width:600px){body{padding:20px 12px 48px}.card{padding:20px 14px}}
</style><header><div class="brand-icon">◷</div><div class="brand"><small>每個上班日，準時提醒</small><strong>班表鬧鐘</strong></div><span class="badge">韌體 <span id="firmware-version">讀取中</span></span><div id="device-health" class="device-health" role="status">正在讀取裝置狀態…</div></header><main><section class="card">)PAGE")+content+R"PAGE(</section></main><footer>設定保存在你的裝置 · 重新開機仍會保留</footer><script>
(()=>{const el=document.querySelector('#device-health'),version=document.querySelector('#firmware-version');let lastSuccess=0;const age=s=>s<60?s+' 秒':Math.floor(s/60)+' 分鐘';async function refreshDeviceHealth(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw Error();const d=await r.json();lastSuccess=Date.now();version.textContent=d.version;const b=d.battery;const charge=b&&b.charging===true?'，充電中':b&&b.charging===false?'，未充電':'，充電狀態未知';const battery=b&&b.valid?b.percent+'%'+charge+'，取樣 '+age(b.sample_age_seconds)+'前':'電量未知'+charge;const backend=d.backendReachable?'後端可連線':d.backendLastSuccessAgeSeconds===null?'後端尚未連線':'後端連線中斷，上次成功 '+age(d.backendLastSuccessAgeSeconds)+'前';el.textContent=battery+'；'+d.tailnetLifecycleLabel+'；'+backend;el.classList.toggle('interrupted',!d.backendReachable)}catch(e){el.textContent='裝置連線中斷'+(lastSuccess?'，上次更新 '+age(Math.floor((Date.now()-lastSuccess)/1000))+'前':'；尚未成功讀取');el.classList.add('interrupted')}}refreshDeviceHealth();setInterval(refreshDeviceHealth,30000)})();
</script></html>)PAGE";
}
bool localNonce();
std::atomic<uint32_t> otaReceived{0},otaTotal{0};
std::atomic<bool> otaBusy{false};
std::atomic<uint32_t> otaReconnectCount{0},otaLastProgressMs{0},otaStatusSequence{0};
char otaBootSession[17]={};
std::atomic<int> otaLastHttpStatus{0};
std::atomic<bool> otaActivation{false};
bool otaReady=false;int otaSession;
portMUX_TYPE otaMux=portMUX_INITIALIZER_UNLOCKED;
alarm_ota_guard_t otaGuard={};char otaMessage[128]="尚未檢查更新",otaPhase[24]="idle",otaReason[32]="idle";
char otaBackendLatestVersion[ALARM_OTA_NAME_CAP]={};
void otaMessageSet(const char *message){portENTER_CRITICAL(&otaMux);strlcpy(otaMessage,message,sizeof(otaMessage));portEXIT_CRITICAL(&otaMux);}
String otaMessageGet(){char msg[128];portENTER_CRITICAL(&otaMux);memcpy(msg,otaMessage,sizeof(msg));portEXIT_CRITICAL(&otaMux);return String(msg);}
void otaStateSet(const char *phase,const char *reason,const char *message){portENTER_CRITICAL(&otaMux);strlcpy(otaPhase,phase,sizeof(otaPhase));strlcpy(otaReason,reason,sizeof(otaReason));strlcpy(otaMessage,message,sizeof(otaMessage));portEXIT_CRITICAL(&otaMux);otaStatusSequence.fetch_add(1);}
String otaPhaseGet(){char value[sizeof(otaPhase)];portENTER_CRITICAL(&otaMux);memcpy(value,otaPhase,sizeof(value));portEXIT_CRITICAL(&otaMux);return String(value);}
String otaReasonGet(){char value[sizeof(otaReason)];portENTER_CRITICAL(&otaMux);memcpy(value,otaReason,sizeof(value));portEXIT_CRITICAL(&otaMux);return String(value);}
void otaResetTerminalNoStaged(){otaReceived=0;otaTotal=0;otaLastProgressMs=0;}
bool otaAuthorize(const void *request,void*){return request==&otaSession&&otaBusy.load();}
bool readChargingNow(bool &active){if(!chargingInputValid)return false;const int level=gpio_get_level(GPIO_NUM_38);if(level!=0&&level!=1)return false;active=battery::charging_active(level);charging=active;return true;}
esp_err_t otaReadGuard(alarm_ota_guard_t *out,void*){portENTER_CRITICAL(&otaMux);*out=otaGuard;portEXIT_CRITICAL(&otaMux);out->charging_valid=readChargingNow(out->charging);out->ringing=ringing;out->now_epoch=time(nullptr);return ESP_OK;}
void refreshOtaGuard(){alarm_ota_guard_t g={};g.clock_valid=clockValid();g.ringing=ringing;g.snoozed=snooze>0;g.schedule_ready=!revision.isEmpty();g.now_epoch=time(nullptr);int64_t next=snooze>0?snooze:nextAlarmEpoch(g.now_epoch);g.next_alarm_epoch=next==INT64_MAX?0:next;portENTER_CRITICAL(&otaMux);otaGuard=g;portEXIT_CRITICAL(&otaMux);}
bool backendReachableNow(uint32_t nowMs){
  alarm_tailnet_status_t tail={};alarm_tailnet_get_status(&tail);
  return connectivity::backend_reachable(
    WiFi.isConnected(),tail.state==ALARM_TAILNET_CONNECTED,
    backendLastResult.load(),backendHasSuccess.load(),
    nowMs,backendLastSuccessMs.load());
}
const char *otaLocalReason(){
  if(!otaReady){
    return "ota-not-ready";
  }
  if(otaBusy.load()){
    return "busy";
  }
  alarm_ota_status_t marker={};
  if(alarm_ota_get_status(&marker)!=ESP_OK||marker.marker_fault)return "marker-fault";
  alarm_ota_guard_t guard={};
  otaReadGuard(&guard,nullptr);
  if(!guard.charging_valid||!guard.charging){return "charging-required";}
  if(!guard.clock_valid){
    return "clock-not-ready";
  }
  if(guard.ringing){
    return "alarm-active";
  }
  if(guard.snoozed){
    return "snoozed";
  }
  if(!guard.schedule_ready){
    return "schedule-not-ready";
  }
  if(guard.next_alarm_epoch>guard.now_epoch&&
     guard.next_alarm_epoch-guard.now_epoch<=300){
    return "alarm-near";
  }
  return "ready";
}
const char *otaDownloadReason(){
  const char *local=otaLocalReason();if(strcmp(local,"ready"))return local;
  alarm_ota_status_t status={};if(alarm_ota_get_status(&status)==ESP_OK&&status.staged_valid)return "staged-update-exists";
  if(!WiFi.isConnected())return "wifi-not-ready";
  alarm_tailnet_status_t tail={};alarm_tailnet_get_status(&tail);
  if(tail.state!=ALARM_TAILNET_CONNECTED||!tail.acl_ready)return "tailnet-not-ready";
  if(!backendReachableNow(millis()))return "backend-not-reachable";
  return "ready";
}
const char *otaInstallReason(){if(!otaReady)return "ota-not-ready";if(otaBusy.load())return "busy";alarm_ota_status_t status={};if(alarm_ota_get_status(&status)!=ESP_OK||status.marker_fault)return "marker-fault";if(!status.staged_valid)return "no-staged-update";return otaLocalReason();}
constexpr char OTA_MARKER_KEY[]="ota-stage-v1";
esp_err_t otaMarkerLoad(alarm_ota_staged_record_t *out,void*){nvs_handle_t handle;esp_err_t err=nvs_open_from_partition("alarm_nvs","shift-alarm",NVS_READONLY,&handle);if(err!=ESP_OK)return err;size_t size=sizeof(*out);err=nvs_get_blob(handle,OTA_MARKER_KEY,out,&size);nvs_close(handle);if(err==ESP_ERR_NVS_NOT_FOUND)return ESP_ERR_NOT_FOUND;if(err!=ESP_OK)return err;return size==sizeof(*out)?ESP_OK:ESP_ERR_INVALID_SIZE;}
esp_err_t otaMarkerStore(const alarm_ota_staged_record_t *record,void*){nvs_handle_t handle;esp_err_t err=nvs_open_from_partition("alarm_nvs","shift-alarm",NVS_READWRITE,&handle);if(err!=ESP_OK)return err;err=nvs_set_blob(handle,OTA_MARKER_KEY,record,sizeof(*record));if(err==ESP_OK)err=nvs_commit(handle);nvs_close(handle);return err;}
esp_err_t otaMarkerClear(void*){nvs_handle_t handle;esp_err_t err=nvs_open_from_partition("alarm_nvs","shift-alarm",NVS_READWRITE,&handle);if(err!=ESP_OK)return err;err=nvs_erase_key(handle,OTA_MARKER_KEY);if(err==ESP_ERR_NVS_NOT_FOUND)err=ESP_OK;else if(err==ESP_OK)err=nvs_commit(handle);nvs_close(handle);return err;}
esp_err_t otaDiagnostics(void*){
  if(!frame||!frame->getBuffer()||!prefs.isKey("ap-pass")||!displaySettingsValid||!savedScheduleRestored||
     !deviceRoutesReady||ESP.getPsramSize()<8*1024*1024||WiFi.getMode()==WIFI_OFF)return ESP_FAIL;
  const uint32_t started=millis();
  while(!speakerReady.load()&&millis()-started<1000)vTaskDelay(pdMS_TO_TICKS(10));
  if(!speakerReady.load())return ESP_FAIL;
  alarm_proxy_status_t gateway={};for(unsigned i=0;i<20;i++){alarm_proxy_get_status(&gateway);if(gateway.listening)break;vTaskDelay(pdMS_TO_TICKS(50));}if(!gateway.listening)return ESP_FAIL;
  NetworkClient probe;bool accepting=probe.connect(IPAddress(127,0,0,1),8081,500)==1;probe.stop();
  return accepting?ESP_OK:ESP_FAIL;
}
// Never allocate an unknown/chunked response; consume exactly Content-Length.
bool boundedHttpBody(HTTPClient &h,size_t maximum,String &out,uint32_t timeoutMs){
  const int length=h.getSize();out="";
  if(length<=0||size_t(length)>maximum||!out.reserve(size_t(length)))return false;
  auto *stream=h.getStreamPtr();if(!stream)return false;
  uint32_t started=millis();size_t received=0;char chunk[1024];
  while(received<size_t(length)){
    if(millis()-started>=timeoutMs)return false;
    int available=stream->available();
    if(available<=0){if(!h.connected())return false;vTaskDelay(pdMS_TO_TICKS(1));continue;}
    size_t wanted=std::min(sizeof(chunk),std::min(size_t(available),size_t(length)-received));
    int n=stream->read(reinterpret_cast<uint8_t*>(chunk),wanted);
    if(n<=0||!out.concat(chunk,size_t(n)))return false;
    received+=size_t(n);
  }
  return out.length()==size_t(length);
}
void secureWipeBackendExchange(BackendPollExchange *exchange){
  if(!exchange)return;
  volatile char *wipe=exchange->token;for(size_t i=0;i<sizeof(exchange->token);i++)wipe[i]=0;
  wipe=exchange->backend;for(size_t i=0;i<sizeof(exchange->backend);i++)wipe[i]=0;
  wipe=exchange->revision;for(size_t i=0;i<sizeof(exchange->revision);i++)wipe[i]=0;
  wipe=exchange->heartbeat;for(size_t i=0;i<sizeof(exchange->heartbeat);i++)wipe[i]=0;
  volatile char *body=const_cast<char*>(exchange->schedule_body.c_str());for(unsigned i=0;i<exchange->schedule_body.length();i++)body[i]=0;
  exchange->schedule_body="";delete exchange;
}
void backendPollWorker(void*){
  for(;;){
    BackendPollExchange *request=nullptr;
    if(xQueueReceive(backendRequestQueue,&request,portMAX_DELAY)!=pdTRUE||!request)continue;
    // The native Tailnet path can need tens of seconds for a cold WireGuard
    // handshake.  Keep the loopback HTTP request alive long enough for that
    // handshake and establish reachability before downloading the schedule.
    HTTPClient heartbeat;heartbeat.setConnectTimeout(3000);heartbeat.setTimeout(60000);
    const String url="http://127.0.0.1/api/device/heartbeat";
    Serial.printf("BACKEND_POLL_START generation=%lu\n",(unsigned long)request->ticket.request_generation);
    int heartbeatStatus=0;
    if(heartbeat.begin(url)){
      String authorization=String("Bearer ")+request->token;heartbeat.addHeader("Authorization",authorization);wipeString(authorization);
      heartbeat.addHeader("Content-Type","application/json");
      heartbeatStatus=heartbeat.POST(reinterpret_cast<uint8_t*>(request->heartbeat),strlen(request->heartbeat));
      request->heartbeat_ok=heartbeatStatus==200;
    }
    heartbeat.end();
    Serial.printf("BACKEND_POLL_HEARTBEAT generation=%lu status=%d\n",(unsigned long)request->ticket.request_generation,heartbeatStatus);
    if(request->heartbeat_ok&&request->fetch_schedule){
      HTTPClient schedule;schedule.setConnectTimeout(3000);schedule.setTimeout(60000);
      const String scheduleUrl="http://127.0.0.1/api/device/schedule";
      if(schedule.begin(scheduleUrl)){
        String authorization=String("Bearer ")+request->token;schedule.addHeader("Authorization",authorization);wipeString(authorization);
        request->schedule_status=schedule.GET();
        if(request->schedule_status==200)request->schedule_body_valid=boundedHttpBody(schedule,MAX_JSON,request->schedule_body,60000);
      }
      schedule.end();
    }
    if(xQueueSend(backendResultQueue,&request,0)!=pdTRUE){backendDroppedGeneration.store(request->ticket.request_generation);secureWipeBackendExchange(request);}
  }
}
bool initBackendPoll(){
  backendRequestQueue=xQueueCreate(1,sizeof(BackendPollExchange*));backendResultQueue=xQueueCreate(1,sizeof(BackendPollExchange*));
  if(!backendRequestQueue||!backendResultQueue){if(backendRequestQueue)vQueueDelete(backendRequestQueue);if(backendResultQueue)vQueueDelete(backendResultQueue);backendRequestQueue=nullptr;backendResultQueue=nullptr;return false;}
  // Low priority and no core pinning keep Tailnet/proxy tasks schedulable; 8 KiB covers bounded HTTP/JSON locals.
  if(xTaskCreate(backendPollWorker,"backend_poll",8192,nullptr,1,nullptr)!=pdPASS){vQueueDelete(backendRequestQueue);vQueueDelete(backendResultQueue);backendRequestQueue=nullptr;backendResultQueue=nullptr;return false;}
  return true;
}
void observeBackendPollAvailability(){
  alarm_tailnet_status_t tail={};alarm_tailnet_get_status(&tail);
  const bool ready=WiFi.isConnected()&&tail.state==ALARM_TAILNET_CONNECTED;
  const uint32_t previous=backendPollState.validity_generation;backendpoll::observeConnectivity(backendPollState,ready,ready?uint32_t(WiFi.localIP()):0);
  const bool busy=otaBusy.load();if(busy&&!backendPollObservedOtaBusy)backendpoll::invalidate(backendPollState);backendPollObservedOtaBusy=busy;
  if(previous!=backendPollState.validity_generation)backendLastResult.store(false);
}
void startBackendPoll(uint32_t nowMs){
  if(!backendPollReady)return;
  alarm_tailnet_status_t tail={};alarm_tailnet_get_status(&tail);
  const bool transportReady=connectivity::should_poll_backend(WiFi.isConnected(),tail.state==ALARM_TAILNET_CONNECTED,!backend.isEmpty(),!token.isEmpty());
  if(!backendpoll::canStart(backendPollState,nowMs,transportReady,otaBusy.load()))return;
  const auto ticket=backendpoll::begin(backendPollState,nowMs,POLL_MS);
  auto *request=new(std::nothrow) BackendPollExchange;
  if(!request){backendpoll::allocationFailed(backendPollState,nowMs);syncState="後端服務無法使用";return;}
  request->ticket=ticket;request->fetch_schedule=!localSchedule;
  JsonDocument d;d["revision"]=revision;d["status"]=ringing?"ringing":(clockValid()?"ready":"waiting_for_time");d["ip"]=WiFi.localIP().toString();
  int64_t next=nextAlarmEpoch(time(nullptr));if(next!=INT64_MAX)d["next_alarm"]=next;
  int percent=0;uint32_t age=0;JsonObject battery=d["battery"].to<JsonObject>();battery["schema"]=1;battery["valid"]=battery::value(batteryState,millis(),percent,age);if(battery["valid"].as<bool>()){battery["percent"]=percent;battery["sample_age_seconds"]=age;}else{battery["percent"]=nullptr;battery["sample_age_seconds"]=nullptr;}if(chargingInputValid)battery["charging"]=charging.load();else battery["charging"]=nullptr;
  const size_t heartbeatSize=measureJson(d);
  if(backend.length()>=sizeof(request->backend)||token.length()>=sizeof(request->token)||revision.length()>=sizeof(request->revision)||heartbeatSize>=sizeof(request->heartbeat)){
    secureWipeBackendExchange(request);backendpoll::allocationFailed(backendPollState,nowMs);syncState="後端資料超過限制";return;
  }
  strlcpy(request->backend,backend.c_str(),sizeof(request->backend));strlcpy(request->token,token.c_str(),sizeof(request->token));strlcpy(request->revision,revision.c_str(),sizeof(request->revision));if(serializeJson(d,request->heartbeat,sizeof(request->heartbeat))!=heartbeatSize){secureWipeBackendExchange(request);backendpoll::allocationFailed(backendPollState,nowMs);syncState="後端資料序列化失敗";return;}
  if(xQueueSend(backendRequestQueue,&request,0)!=pdTRUE){secureWipeBackendExchange(request);backendpoll::allocationFailed(backendPollState,nowMs);syncState="後端服務忙碌，稍後重試";}else backendpoll::enqueued(backendPollState);
}
void drainBackendPollResult(){
  const uint32_t dropped=backendDroppedGeneration.exchange(0);if(dropped)backendpoll::finishGeneration(backendPollState,dropped);
  if(!backendResultQueue)return;
  BackendPollExchange *result=nullptr;
  while(xQueueReceive(backendResultQueue,&result,0)==pdTRUE){
    const bool accepted=backendpoll::accepts(backendPollState,result->ticket,otaBusy.load());
    backendpoll::finish(backendPollState,result->ticket);
    if(accepted){
      if(result->fetch_schedule&&!localSchedule&&revision==result->revision){if(result->schedule_status!=200)syncState=String("同步失敗，回應碼 ")+result->schedule_status;else if(!result->schedule_body_valid)syncState="班表大小無效或傳輸不完整";else if(scheduleStorageFault.load())syncState="班表儲存狀態不明；重開前請勿修改";else{uint32_t id=0;if(enqueueScheduleWork(ScheduleOrigin::Backend,std::move(result->schedule_body),id))syncState="班表驗證與儲存中";else syncState="班表背景工作忙碌";}}
      backendLastResult.store(result->heartbeat_ok);if(result->heartbeat_ok){backendLastSuccessMs.store(millis());backendHasSuccess.store(true);}
    }
    secureWipeBackendExchange(result);result=nullptr;
  }
}
void otaWorker(void*){
 {
  bool stagedComplete=false;
  alarm_ota_handle_t handle=0;HTTPClient h;esp_err_t err=ESP_FAIL;
  do{
    otaStateSet("checking","checking","正在檢查更新版本");
    h.setConnectTimeout(3000);h.setTimeout(45000);h.begin("http://127.0.0.1/api/device/update");h.addHeader("Authorization",String("Bearer ")+token);
    otaLastHttpStatus.store(h.GET());if(otaLastHttpStatus.load()!=200){otaStateSet("error","manifest-http","無法取得更新資訊");break;}
    String manifestBody;if(!boundedHttpBody(h,4096,manifestBody,5000)){otaStateSet("error","manifest-body","更新資訊大小無效或傳輸不完整");break;}
    JsonDocument d;if(deserializeJson(d,manifestBody)){otaStateSet("error","manifest-json","更新資訊格式無效");break;}h.end();
    if(!ota_manifest::available(d["available"])){
      otaStateSet("error","no-release","目前沒有已發布的更新");
      break;
    }
    JsonObject m=d["manifest"];alarm_ota_manifest_t manifest={};
    const char *board=m["board"]|"",*version=m["version"]|"",*sha=m["sha256"]|"",*mac=m["hmac_sha256"]|"";
    if(strlen(board)>=sizeof(manifest.board)||strlen(version)>=sizeof(manifest.version)||strlen(sha)!=64||strlen(mac)!=64||!m["size"].is<uint32_t>()){otaStateSet("error","manifest-fields","更新資訊格式無效");break;}
    strlcpy(manifest.board,board,sizeof(manifest.board));strlcpy(manifest.version,version,sizeof(manifest.version));strlcpy(manifest.sha256,sha,sizeof(manifest.sha256));strlcpy(manifest.hmac_sha256,mac,sizeof(manifest.hmac_sha256));manifest.size=m["size"];
    portENTER_CRITICAL(&otaMux);strlcpy(otaBackendLatestVersion,manifest.version,sizeof(otaBackendLatestVersion));portEXIT_CRITICAL(&otaMux);
    otaTotal.store(manifest.size);
    err=alarm_ota_begin(&manifest,&otaSession,&handle);if(err!=ESP_OK){otaStateSet("error",err==ALARM_OTA_ERR_MARKER?"marker-fault":err==ALARM_OTA_ERR_CHARGING_REQUIRED?"charging-required":"guard-rejected",err==ALARM_OTA_ERR_MARKER?"更新記錄狀態不明，已封鎖下載":err==ALARM_OTA_ERR_CHARGING_REQUIRED?"裝置未顯示充電，已拒絕下載":"更新遭拒：請確認版本與排程");break;}
    const String firmwareUrl=String("http://127.0.0.1/api/device/firmware/")+manifest.sha256+".bin";
    h.begin(firmwareUrl);h.addHeader("Authorization",String("Bearer ")+token);
    otaLastHttpStatus.store(h.GET());if(otaLastHttpStatus.load()!=200||h.getSize()!=int(manifest.size)){otaStateSet("error","download-http","更新檔大小或回應不正確");break;}
    auto *stream=h.getStreamPtr();uint8_t buffer[4096];size_t received=0;uint8_t reconnects=0;uint32_t started=millis(),lastProgress=started;const char *failure=nullptr;
    otaLastProgressMs.store(started);otaStateSet("downloading","downloading","正在下載並驗證，請保持供電");
    while(received<manifest.size){
      auto limit=alarm_download::deadline(millis(),started,lastProgress);
      if(limit==alarm_download::Deadline::total){failure="下載超過十分鐘，保留原有版本";break;}
      int available=stream->available();
      if(available<=0&&(limit==alarm_download::Deadline::idle||!h.connected())){
        if(!alarm_download::mayReconnect(reconnects)){failure="下載多次中斷，保留原有版本";break;}
        reconnects++;bool resumed=false;const String expectedRange=String("bytes ")+received+"-"+(manifest.size-1)+"/"+manifest.size;
        for(uint8_t handshake=0;handshake<alarm_download::max_handshake_attempts&&!resumed;handshake++){h.end();otaReconnectCount.fetch_add(1);otaStateSet("reconnecting","stream-interrupted","更新連線中斷，正在安全續傳");h.setConnectTimeout(3000);h.setTimeout(5000);h.begin(firmwareUrl);
          h.addHeader("Authorization",String("Bearer ")+token);h.addHeader("Range",String("bytes=")+received+"-");const char *rangeHeaders[]={"Content-Range"};h.collectHeaders(rangeHeaders,1);
          const int code=h.GET();otaLastHttpStatus.store(code);resumed=code==HTTP_CODE_PARTIAL_CONTENT&&h.getSize()==int(manifest.size-received)&&h.header("Content-Range")==expectedRange;
          if(!resumed&&handshake+1<alarm_download::max_handshake_attempts)vTaskDelay(pdMS_TO_TICKS(alarm_download::handshakeBackoffMs(handshake)));}
        if(!resumed){failure="無法安全續傳更新檔，保留原有版本";break;}
        stream=h.getStreamPtr();lastProgress=millis();otaStateSet("downloading","downloading","正在下載並驗證，請保持供電");continue;
      }
      if(available<=0){vTaskDelay(pdMS_TO_TICKS(1));continue;}
      size_t need=std::min(sizeof(buffer),std::min(size_t(available),size_t(manifest.size-received)));
      int n=stream->read(buffer,need);
      if(n<=0){failure="更新串流讀取失敗，保留原有版本";break;}err=alarm_ota_write(handle,&otaSession,buffer,size_t(n));if(err!=ESP_OK){failure=err==ALARM_OTA_ERR_CHARGING_REQUIRED?"充電狀態中斷，已停止下載":"更新寫入或安全檢查失敗，保留原有版本";break;}
      received+=size_t(n);otaReceived.store(received);lastProgress=millis();otaLastProgressMs.store(lastProgress);vTaskDelay(1);
    }
    h.end();if(failure){otaStateSet("error",err==ALARM_OTA_ERR_CHARGING_REQUIRED?"charging-required":"resume-failed",failure);break;}
    otaStateSet("verifying","verifying","下載完成，正在驗證");err=alarm_ota_finish(handle,&otaSession);if(err!=ESP_OK){otaStateSet("error",err==ALARM_OTA_ERR_CHARGING_REQUIRED?"charging-required":"verification-failed",err==ALARM_OTA_ERR_CHARGING_REQUIRED?"裝置未顯示充電，未保存更新":"更新驗證失敗，保留原有版本");break;}
    otaStateSet("staged","ready","更新已完整下載並驗證，可稍後離線安裝");
    stagedComplete=true;
  }while(false);
  h.end();
   if(!stagedComplete&&handle)alarm_ota_abort(handle,&otaSession);
   alarm_ota_status_t terminal={};if(alarm_ota_get_status(&terminal)==ESP_OK&&terminal.state!=ALARM_OTA_RECEIVING&&terminal.state!=ALARM_OTA_STAGED)otaResetTerminalNoStaged();
   otaBusy=false;
 }
 vTaskDelete(nullptr);
}
void otaStagedRoutes() {
  server.on("/update", HTTP_GET, [] {
    String page=R"HTML(<h1>裝置更新</h1><p>下載與安裝是兩個分開的步驟。完整下載並驗證後，更新會保存在備用分區；下載完成不會重新啟動。</p><div class="upload-box"><strong>供電限制</strong><p>下載與安裝時，裝置都必須由 GPIO38 顯示「充電中」。這是充電狀態，不是可靠的 USB／VBUS 偵測。電池完整充電時即使接著 USB，也可能因未顯示充電而拒絕下載或安裝。</p><p>下載中斷後，同一次開機可安全續傳；若重新開機，部分檔案不會保存，重新開機後會從 0 重新下載。完整且已驗證的更新可在沒有 Wi-Fi、Tailscale 或後端時稍後安裝。</p></div><p>目前版本：<strong id="current-version">讀取中</strong></p><p>最新版本：<strong id="latest-version">尚未檢查</strong></p><p>已下載版本：<strong id="staged-version">沒有</strong></p><p id="status" role="status">正在取得狀態…</p><progress id="progress" max="100" value="0" style="width:100%"></progress><div class="button-row"><button id="download-update" disabled>下載更新</button><button id="install-update" disabled>安裝並重新啟動</button><button id="discard-update" class="secondary" disabled>刪除已下載更新</button></div><p class="help">按鈕只會各送出一次請求；回應不明時先讀取狀態，不會自動重送。</p><a href="/display">返回裝置設定</a><style>.button-row{display:flex;gap:10px;flex-wrap:wrap}.button-row button{flex:1 1 190px}progress{height:18px}@media(max-width:600px){.button-row{display:grid}.button-row button{width:100%}}</style><script>
const nonce='NONCE',statusEl=document.querySelector('#status'),downloadButton=document.querySelector('#download-update'),installButton=document.querySelector('#install-update'),discardButton=document.querySelector('#discard-update');let currentSession=null,newest=0,nextRequest=0,acceptedRequest=0,mutationPending=false;/* OTA_STATUS_POLICY_START */function recordUpdateFailure(requestId){if(requestId>acceptedRequest)acceptedRequest=requestId}function acceptUpdateStatus(d,requestId){if(requestId<acceptedRequest)return false;if(currentSession===null||d.session!==currentSession){currentSession=d.session;newest=d.sequence;acceptedRequest=requestId;return true}if(d.sequence<newest)return false;newest=d.sequence;acceptedRequest=requestId;return true}/* OTA_STATUS_POLICY_END */const reasons={ready:'可以操作',busy:'更新操作進行中','marker-fault':'更新記錄狀態不明；下載與安裝已封鎖，可嘗試刪除後重新讀取','charging-required':'裝置目前未顯示充電','ota-not-ready':'更新功能尚未就緒','wifi-not-ready':'無線網路尚未連線','tailnet-not-ready':'Tailscale 尚未就緒','backend-not-reachable':'更新後端目前無法連線','clock-not-ready':'裝置尚未校時','alarm-active':'鬧鐘正在響鈴',snoozed:'貪睡提醒尚未結束','schedule-not-ready':'排程尚未就緒','alarm-near':'五分鐘內有鬧鐘','no-staged-update':'沒有已下載更新','staged-update-exists':'請先安裝或刪除已下載更新'};async function refresh(){const requestId=++nextRequest;try{const r=await fetch('/api/update',{headers:{'X-Setup-Nonce':nonce},cache:'no-store'});const d=await r.json();if(!acceptUpdateStatus(d,requestId))return;document.querySelector('#current-version').textContent=d.currentVersion;document.querySelector('#latest-version').textContent=d.latestVersion||'尚未檢查';document.querySelector('#staged-version').textContent=d.staged?d.staged.version:'沒有';downloadButton.disabled=mutationPending||d.busy||!d.canDownload;installButton.disabled=mutationPending||d.busy||!d.canInstall;discardButton.disabled=mutationPending||d.busy||(!d.staged&&!d.markerFault);const percent=d.total?Math.floor(d.received*100/d.total):0;document.querySelector('#progress').value=percent;statusEl.textContent=d.message+'；下載：'+(reasons[d.downloadReason]||d.downloadReason)+'；安裝：'+(reasons[d.installReason]||d.installReason)+(d.total?'（'+d.received+' / '+d.total+' 位元組，'+percent+'%）':'');}catch(e){recordUpdateFailure(requestId);statusEl.textContent='裝置連線中斷；保留上次顯示，稍後只重新讀取狀態。'}}async function mutate(path){if(mutationPending)return;mutationPending=true;downloadButton.disabled=installButton.disabled=discardButton.disabled=true;try{const r=await fetch(path,{method:'POST',headers:{'X-Setup-Nonce':nonce}});statusEl.textContent=await r.text();}catch(e){statusEl.textContent='請求結果不明；不會自動重送，請等待唯讀狀態更新。'}finally{mutationPending=false;setTimeout(refresh,500)}}downloadButton.onclick=()=>mutate('/api/update/download');installButton.onclick=()=>mutate('/api/update/install');discardButton.onclick=()=>mutate('/api/update/discard');refresh();setInterval(refresh,2000);
</script>)HTML";
    page.replace("NONCE", setupNonce);
    server.send(200, "text/html; charset=utf-8", devicePage(page));
  });
  server.on("/api/update",HTTP_GET,[]{
    refreshOtaGuard();alarm_ota_status_t status={};alarm_ota_get_status(&status);const bool reloading=status.marker_fault&&!otaBusy.load();if(reloading){alarm_ota_load_staged();alarm_ota_get_status(&status);if(status.staged_valid){otaReceived=status.staged.manifest.size;otaTotal=status.staged.manifest.size;otaStateSet("staged","ready","已重新載入完整驗證的更新");}else if(status.state==ALARM_OTA_IDLE){otaResetTerminalNoStaged();otaStateSet("idle","idle","已確認沒有已下載更新");}}if(status.state!=ALARM_OTA_RECEIVING&&status.state!=ALARM_OTA_STAGED&&!otaBusy.load())otaResetTerminalNoStaged();const char *download=otaDownloadReason(),*install=otaInstallReason();JsonDocument d;
    d["currentVersion"]=VERSION;d["phase"]=status.marker_fault?"marker-fault":otaPhaseGet();d["message"]=status.marker_fault?"更新記錄狀態不明，已封鎖下載與安裝":otaMessageGet();d["lastReason"]=status.marker_fault?"marker-fault":otaReasonGet();d["session"]=otaBootSession;d["sequence"]=otaStatusSequence.load();d["busy"]=otaBusy.load();d["ready"]=otaReady;d["markerFault"]=status.marker_fault;d["canDownload"]=strcmp(download,"ready")==0;d["downloadReason"]=download;d["canInstall"]=strcmp(install,"ready")==0;d["installReason"]=install;d["received"]=otaReceived.load();d["total"]=otaTotal.load();d["reconnectCount"]=otaReconnectCount.load();d["lastHttpStatus"]=otaLastHttpStatus.load();portENTER_CRITICAL(&otaMux);if(otaBackendLatestVersion[0])d["latestVersion"]=otaBackendLatestVersion;else d["latestVersion"]=nullptr;portEXIT_CRITICAL(&otaMux);
    if(status.staged_valid){JsonObject s=d["staged"].to<JsonObject>();s["schema"]=status.staged.schema;s["board"]=status.staged.manifest.board;s["version"]=status.staged.manifest.version;s["size"]=status.staged.manifest.size;s["sha256"]=status.staged.manifest.sha256;}else d["staged"]=nullptr;
    uint32_t progress=otaLastProgressMs.load();if(progress)d["lastProgressAgeSeconds"]=(millis()-progress)/1000;else d["lastProgressAgeSeconds"]=nullptr;String out;serializeJson(d,out);server.send(200,"application/json",out);
  });
  server.on("/api/update/download",HTTP_POST,[]{if(!localNonce())return;refreshOtaGuard();const char *reason=otaDownloadReason();if(strcmp(reason,"ready")){server.send(409,"text/plain; charset=utf-8",String("目前不能下載：")+reason);return;}bool expected=false;if(!otaBusy.compare_exchange_strong(expected,true)){server.send(409,"text/plain; charset=utf-8","更新操作進行中");return;}otaReceived=0;otaTotal=0;otaReconnectCount=0;otaLastHttpStatus=0;otaLastProgressMs=millis();otaStateSet("starting","starting","開始檢查更新");if(xTaskCreate(otaWorker,"alarm_update",12288,nullptr,1,nullptr)!=pdPASS){otaBusy=false;otaStateSet("error","worker-start-failed","記憶體不足，請稍後重試");server.send(503,"text/plain; charset=utf-8","記憶體不足，請稍後重試");return;}server.send(202,"text/plain; charset=utf-8","已開始下載；完成後不會重新啟動");});
  server.on("/api/update/install",HTTP_POST,[]{if(!localNonce())return;refreshOtaGuard();const char *reason=otaInstallReason();if(strcmp(reason,"ready")){server.send(409,"text/plain; charset=utf-8",String("目前不能安裝：")+reason);return;}bool expected=false;if(!otaBusy.compare_exchange_strong(expected,true)){server.send(409,"text/plain; charset=utf-8","更新操作進行中");return;}otaStateSet("verifying-local","verifying-local","正在從本機 Flash 完整驗證更新");otaActivation=true;server.send(202,"text/plain; charset=utf-8","已接受安裝，驗證通過後重新啟動");});
  server.on("/api/update/discard",HTTP_POST,[]{if(!localNonce())return;bool expected=false;if(!otaBusy.compare_exchange_strong(expected,true)){server.send(409,"text/plain; charset=utf-8","更新操作進行中");return;}esp_err_t err=alarm_ota_discard_staged(&otaSession);otaBusy=false;if(err==ESP_OK){otaResetTerminalNoStaged();otaStateSet("idle","discarded","已確認沒有已下載更新");server.send(200,"text/plain; charset=utf-8","已確認沒有已下載更新");}else{alarm_ota_status_t status={};alarm_ota_get_status(&status);otaStateSet(status.marker_fault?"marker-fault":"staged",status.marker_fault?"marker-fault":"discard-failed",status.marker_fault?"更新記錄狀態不明，操作維持封鎖":"刪除未完成，已下載更新仍保留");server.send(500,"text/plain; charset=utf-8",status.marker_fault?"更新記錄狀態不明，操作維持封鎖":"刪除未完成，已下載更新仍保留");}});
}
String tailnetLabel(alarm_tailnet_state_t state) {
  switch(state){case ALARM_TAILNET_CONNECTED:return "Tailscale已連線";case ALARM_TAILNET_AUTH_REQUIRED:return "等待登入授權";case ALARM_TAILNET_EXPIRED:return "授權已到期";case ALARM_TAILNET_BLOCKED:return "存取規則尚未通過";case ALARM_TAILNET_CONNECTING:return "正在連線";default:return "尚未連線";}
}
String tailnetLifecycleLabel(alarm_tailnet_lifecycle_t state){switch(state){case ALARM_TAILNET_QUEUED:return "Tailscale 已排入啟動";case ALARM_TAILNET_STARTING:return "Tailscale 正在啟動";case ALARM_TAILNET_RUNNING:return "Tailscale 控制面運行中";case ALARM_TAILNET_RETRY_WAIT:return "Tailscale 等待自動重試";case ALARM_TAILNET_LIFECYCLE_BLOCKED:return "Tailscale 啟動受阻";default:return "Tailscale 尚未啟動";}}
bool localNonce(){if(server.arg("nonce")==setupNonce||server.header("X-Setup-Nonce")==setupNonce)return true;server.send(403,"text/plain; charset=utf-8","請從裝置設定頁操作");return false;}
String tailnetDetail(const alarm_tailnet_status_t &t){
 if(!WiFi.isConnected())return "無線網路未連線，請先設定無線網路";
 if(!clockValid())return "裝置尚未校時，請在班表頁校時或確認網際網路連線";
 if(t.capacity_exceeded)return "Tailscale 裝置數超過韌體容量";
 if(t.lifecycle==ALARM_TAILNET_RETRY_WAIT)return String("Tailscale 暫時無法啟動，將在 ")+String(t.retry_in_seconds)+" 秒內自動重試（第 "+String(t.retry_count)+" 次）";
 if(t.last_error!=ESP_OK&&t.lifecycle==ALARM_TAILNET_LIFECYCLE_BLOCKED)return String("Tailscale 已停止，錯誤碼 ")+String(t.last_error)+"；請檢查裝置容量或授權";
 if(t.state==ALARM_TAILNET_EXPIRED)return "Tailscale 授權已到期，請按加入／重新授權";
 if(t.state==ALARM_TAILNET_AUTH_REQUIRED)return "請開啟下方 Tailscale 官方登入頁完成授權";
 if(t.state==ALARM_TAILNET_BLOCKED)return "Tailscale 存取規則尚未通過，請檢查管理後台的裝置核准與存取規則";
 if(t.state==ALARM_TAILNET_CONNECTING)return millis()-tailnetAttempt>60000?"Tailscale 連線逾時，尚未建立連線；請確認網際網路及授權狀態後重試":"正在連線至 Tailscale，尚未完成";
 if(t.state==ALARM_TAILNET_CONNECTED)return "Tailscale 已連線；若 AI 服務無回應，仍可使用班表頁";
 return "Tailscale 尚未連線；班表頁與鬧鐘仍可使用";
}
void tailnetRoutes(){
  server.on("/tailnet",HTTP_GET,[]{
    String page=R"HTML(<h1>Tailscale</h1><p>本頁所有時間固定使用臺北時間（UTC+8）。</p><p>裝置會自己加入你的Tailscale，換環境後只要設定新的無線網路即可重新連線。</p><div id="status">正在取得連線狀態…</div><p id="address"></p><p id="expiry"></p><a id="approve" class="primary-link" hidden target="_blank" rel="noopener noreferrer">前往Tailscale 官方頁面登入授權</a><form id="join"><button>加入／重新授權</button></form><p class="help">帳號密碼只輸入在官方登入頁，裝置不會收取密碼。授權頁需要網際網路；手機可切換到行動網路完成登入。</p><a href="/display">返回裝置設定</a><script>
const nonce='NONCE',statusEl=document.querySelector('#status'),link=document.querySelector('#approve');
async function refresh(){try{const r=await fetch('/api/tailnet',{headers:{'X-Setup-Nonce':nonce}});if(!r.ok)throw Error();const d=await r.json();statusEl.textContent=d.label+"。"+d.detail;document.querySelector('#address').textContent=d.ip?'Tailscale 位址：'+d.ip:'';document.querySelector('#expiry').textContent=d.expires_at>0?'授權有效期限：'+new Date(d.expires_at*1000).toLocaleString('zh-TW',{timeZone:'Asia/Taipei'}):'';link.hidden=true;if(d.auth_url){const u=new URL(d.auth_url);if(u.protocol==='https:'&&u.hostname==='login.tailscale.com'){link.href=u.href;link.hidden=false;}}}catch(e){statusEl.textContent='無法取得狀態，請確認仍連上裝置網路。'}}
document.querySelector('#join').onsubmit=async(e)=>{e.preventDefault();try{const r=await fetch('/api/tailnet/join',{method:'POST',headers:{'X-Setup-Nonce':nonce}});statusEl.textContent=r.ok?'正在準備 Tailscale 授權…':await r.text();}catch(e){statusEl.textContent='Tailscale 授權請求失敗，請確認仍連上裝置網路，再重試。'}};refresh();setInterval(refresh,3000);
</script>)HTML";page.replace("NONCE",setupNonce);server.send(200,"text/html; charset=utf-8",devicePage(page));
  });
  server.on("/api/tailnet",HTTP_GET,[]{if(!localNonce())return;alarm_tailnet_status_t t={};alarm_tailnet_get_status(&t);JsonDocument d;d["label"]=tailnetLabel(t.state);d["connected"]=t.state==ALARM_TAILNET_CONNECTED;d["detail"]=tailnetDetail(t);d["ip"]=t.ip;d["expires_at"]=t.expires_at;d["auth_url"]=t.auth_url;d["acl_ready"]=t.acl_ready;d["peer_count"]=t.peer_count;d["peer_capacity"]=t.peer_capacity;d["derp_home_connected"] = t.derp_home_connected;d["derp_home_region"] = t.derp_home_region;d["derp_remote_connected"] = t.derp_remote_connected;d["derp_frames_tx"] = t.derp_frames_tx;d["derp_frames_rx"] = t.derp_frames_rx;d["derp_connect_failures"] = t.derp_connect_failures;d["derp_capacity_drops"] = t.derp_capacity_drops;d["derp_queue_drops"] = t.derp_queue_drops;d["derp_route_drops"] = t.derp_route_drops;d["wg_netif_ip"] = t.wg_netif_ip;d["wg_netif_mask"] = t.wg_netif_mask;d["wg_netif_index"] = t.wg_netif_index;d["wg_netif_up"] = t.wg_netif_up;d["wg_netif_link_up"] = t.wg_netif_link_up;d["wg_peer_count"] = t.wg_peer_count;d["wg_sessions"] = t.wg_sessions;d["wg_out_packets"] = t.wg_out_packets;d["wg_out_dropped"] = t.wg_out_dropped;d["wg_last_out_src"] = t.wg_last_out_src;d["wg_last_out_dst"] = t.wg_last_out_dst;d["wg_lookup_misses"] = t.wg_lookup_misses;d["wg_derp_enqueue"] = t.wg_derp_enqueue;d["wg_derp_enqueue_fail"] = t.wg_derp_enqueue_fail;d["wg_udp_tx"] = t.wg_udp_tx;d["wg_rx_packets"] = t.wg_rx_packets;d["wg_in_packets"] = t.wg_in_packets;d["wg_in_dropped"] = t.wg_in_dropped;d["coord_stage"]=t.coord_stage;d["coord_last_reason"]=t.coord_last_reason;d["coord_reconnects"]=t.coord_reconnects;d["coord_successes"]=t.coord_successes;d["coord_stage_since_ms"]=t.coord_stage_since_ms;d["coord_last_failure_ms"]=t.coord_last_failure_ms;d["policy_ready"]=t.policy_ready;d["peers_ready"]=t.peers_ready;d["node_authorized"]=t.node_authorized;d["peer_generation"]=t.peer_generation;d["wg_last_in_src"]=t.wg_last_in_src;d["wg_last_in_dst"]=t.wg_last_in_dst;d["wg_last_in_port"]=t.wg_last_in_port;d["wg_last_in_drop"]=t.wg_last_in_drop;d["acl_rule_count"]=t.acl_rule_count;d["acl_range_count"]=t.acl_range_count;d["acl_unsupported_count"]=t.acl_unsupported_count;if(t.capacity_exceeded)d["label"]="Tailscale裝置數超過容量，請更新韌體";String out;serializeJson(d,out);server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",out);});
  server.on("/api/tailnet/join",HTTP_POST,[]{if(!localNonce())return;if(!WiFi.isConnected()||!clockValid()){server.send(409,"text/plain; charset=utf-8","請先連上無線網路並完成校時");return;}esp_err_t e=tailnetStarted?alarm_tailnet_reauth():alarm_tailnet_start(apName.c_str());if(e==ESP_OK){tailnetStarted=true;tailnetAttempt=millis();}server.send(e==ESP_OK?202:503,"text/plain; charset=utf-8",e==ESP_OK?"等待 Tailscale 官方授權":"無法啟動Tailscale，請稍後重試");});
}
bool allowedDeviceHost(const String &host){
  auto matches=[&](IPAddress ip){String value=ip.toString();return value!="0.0.0.0"&&(host==value||host==value+":80");};
  if(WiFi.isConnected()&&matches(WiFi.localIP()))return true;
  if(portal&&matches(WiFi.softAPIP()))return true;
  alarm_tailnet_status_t t={};
  if(alarm_tailnet_get_status(&t)==ESP_OK&&t.state==ALARM_TAILNET_CONNECTED){IPAddress ip;if(ip.fromString(t.ip)&&matches(ip))return true;}
  return false;
}
extern "C" bool alarm_http_headers_allowed(const char *host,const char *path,const char *method,const char *auth){
  if(!allowedDeviceHost(String(host)))return false;
  if(strcmp(method,"POST")==0&&(strcmp(path,"/api/schedule")==0||strcmp(path,"/api/test")==0||strcmp(path,"/api/stop")==0))
    return token.length()&&String(auth)==String("Bearer ")+token;
  return true;
}
#include "local_calendar_routes.h"
void routes() {
  server.on("/clock",HTTP_GET,[]{String page=CLOCK_PAGE;page.replace("NONCE",setupNonce);server.send(200,"text/html; charset=utf-8",devicePage(page));});
  server.on("/schedule",HTTP_GET,[]{server.sendHeader("Location","/calendar");server.send(303,"text/plain","");});
  server.addMiddleware([](WebServer &request,Middleware::Callback next){
    if(!allowedDeviceHost(request.hostHeader())){request.send(400,"text/plain; charset=utf-8","不接受此主機名稱，請使用裝置畫面上的位址");return true;}
    request.sendHeader("Cache-Control","no-store");request.sendHeader("X-Frame-Options","DENY");request.sendHeader("Referrer-Policy","no-referrer");
    return next();
  });
  const char *headers[]={"Authorization","X-Setup-Nonce"}; server.collectHeaders(headers,2);tailnetRoutes();otaStagedRoutes();localCalendarRoutes();
  server.on("/api/clock",HTTP_GET,[]{JsonDocument d;d["ready"]=clockValid();d["last_sync"]=lastNetworkClock.load();d["interval_seconds"]=CLOCK_SYNC_INTERVAL_MS/1000;d["overdue"]=lastNetworkClock.load()?millis()-lastNetworkClockMs.load()>CLOCK_SYNC_INTERVAL_MS+300000:millis()-bootMs>120000;String out;serializeJson(d,out);server.send(200,"application/json",out);});
  server.on("/api/status",HTTP_GET,[]{ JsonDocument d; d["version"]=VERSION;d["displaySettingsValid"]=displaySettingsValid;alarm_tailnet_status_t tail={};alarm_tailnet_get_status(&tail);d["tailnetLifecycle"]=tail.lifecycle==ALARM_TAILNET_QUEUED?"queued":tail.lifecycle==ALARM_TAILNET_STARTING?"starting":tail.lifecycle==ALARM_TAILNET_RUNNING?"running":tail.lifecycle==ALARM_TAILNET_RETRY_WAIT?"retry_wait":tail.lifecycle==ALARM_TAILNET_LIFECYCLE_BLOCKED?"blocked":"off";d["tailnetLifecycleLabel"]=tailnetLifecycleLabel(tail.lifecycle);d["tailnetRetryCount"]=tail.retry_count;backendendpoint::Endpoint backendInfo;const bool nativeBackend=backendendpoint::parse(backend.c_str(),backendInfo);d["backendTransport"]=nativeBackend?"tailscale":"unsupported";d["backendHost"]=nativeBackend?backendInfo.host:"";const uint32_t nowMs=millis();d["backendReachable"]=backendReachableNow(nowMs);if(backendHasSuccess.load())d["backendLastSuccessAgeSeconds"]=(nowMs-backendLastSuccessMs.load())/1000;else d["backendLastSuccessAgeSeconds"]=nullptr;int percent=0;uint32_t age=0;JsonObject battery=d["battery"].to<JsonObject>();battery["schema"]=1;battery["valid"]=battery::value(batteryState,millis(),percent,age);if(battery["valid"].as<bool>()){battery["percent"]=percent;battery["sample_age_seconds"]=age;}else{battery["percent"]=nullptr;battery["sample_age_seconds"]=nullptr;}if(chargingInputValid)battery["charging"]=charging.load();else battery["charging"]=nullptr;d["localSchedule"]=localSchedule;d["weeklyProfiles"]=weeklyConfigured();d["firstConsecutiveOnly"]=firstConsecutiveOnly;d["rotation"]=displayRotation*90;d["screenTimeoutMinutes"]=screenTimeoutMinutes;d["screenBrightness"]=screenBrightness;d["screenAwake"]=screenAwake; d["revision"]=revision;d["clockReady"]=clockValid();d["epoch"]=time(nullptr);d["ringing"]=ringing;d["wifi"]=WiFi.isConnected();d["alarmCount"]=alarms.size()+weeklyprofiles::enabledCount(weeklyProfiles.as<JsonVariantConst>());d["sync"]=syncState;String out;serializeJson(d,out);server.send(200,"application/json",out); });
  server.on("/display",HTTP_GET,[]{
    String page=String("<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>裝置設定</title><style>body{font:20px system-ui;padding:24px}button,select{font:20px system-ui;padding:12px;margin:12px 0}</style><h1>裝置設定</h1><a class='primary-link' href='/calendar'>設定 Kevin 與晴晴鬧鐘</a><h2>螢幕</h2><form method='post' action='/display'><input type='hidden' name='nonce' value='")+setupNonce+"'><label for='rotation'>顯示方向</label><select id='rotation' name='rotation'>";
    for(int r=0;r<4;r++)page+=String("<option value='")+r+"'"+(displayRotation==r?" selected":"")+">"+r*90+"°"+(r==0?"（正常）":r==2?"（上下顛倒）":"")+"</option>";
    page+="</select><label for='screen-timeout'>閒置多久後關閉螢幕</label><select id='screen-timeout' name='screen_timeout'>";
    const uint16_t timeouts[]={0,1,5,15,30,60};
    for(uint16_t minutes:timeouts)page+=String("<option value='")+minutes+"'"+(screenTimeoutMinutes==minutes?" selected":"")+">"+(minutes?String(minutes)+" 分鐘":"永久開啟")+"</option>";
    page+="</select><label for='brightness'>螢幕亮度</label><select id='brightness' name='brightness'>";
    const uint8_t brightnessLevels[]={10,25,40,60,80,100};
    for(uint8_t percent:brightnessLevels)page+=String("<option value='")+percent+"'"+(screenBrightness==percent?" selected":"")+">"+percent+"%</option>";
    page+="</select><p>降低亮度可減少背光耗電與發熱。</p><button>儲存螢幕設定</button></form><p>非響鈴時長按機殼頂部中間按鈕 1.2 秒，放開後關屏；關屏後按任一按鈕只會喚醒。鬧鐘到點會依設定亮度自動亮屏並正常響鈴，響鈴時按任一按鈕即可停止。</p><h2>時鐘校對</h2><p>固定使用臺北時間（UTC+8）；每三小時自動透過網路校時，重新開機及恢復網路後也會由網路時間服務重試。</p><a href='/clock'>手動調整時鐘</a><p id='clock-status'>正在讀取校時狀態…</p><script>async function clockStatus(){const el=document.querySelector('#clock-status');try{const r=await fetch('/api/clock');if(!r.ok)throw Error();const d=await r.json();el.textContent=d.last_sync?'最近網路校時：'+new Date(d.last_sync*1000).toLocaleString('zh-TW',{timeZone:'Asia/Taipei'})+(d.overdue?'。校時已逾期，請檢查網際網路連線。':'。每三小時自動校對。'):d.overdue?'網路校時尚未成功，請檢查網際網路連線，或到班表頁使用手機校時。':'等待首次網路校時…';}catch(e){el.textContent='無法取得校時狀態，請確認裝置連線。'}}clockStatus();setInterval(clockStatus,10000)</script><h2>韌體更新</h2><p>下載已發布版本，保留舊版供失敗時回復。</p><a href='/update'>檢查裝置更新</a><h2>Tailscale連線</h2><p>登入授權，讓裝置在不同環境仍能同步班表。</p><a href='/tailnet'>設定Tailscale</a>";
    page+=WIFI_MANAGER_PAGE;page.replace("WIFI_NONCE",setupNonce);
    page+="<h2>配網熱點</h2><p>需要從熱點操作時才啟動。已保存網路不會被清除。</p><form method='post' action='/wifi/reset'><input type='hidden' name='nonce' value='"+setupNonce+"'><button>啟動配網熱點</button></form><p>無法連上此頁時，可同時按住「＋」與「－」十秒，啟動配網。</p><a href='/'>返回</a>";
    server.send(200,"text/html; charset=utf-8",devicePage(page));
  });
  server.on("/display",HTTP_POST,[]{
    if(server.arg("nonce")!=setupNonce){server.send(403,"text/plain; charset=utf-8","請重新開啟設定頁");return;}
    String value=server.arg("rotation"),timeoutValue=server.arg("screen_timeout"),brightnessValue=server.arg("brightness");if(value!="0"&&value!="1"&&value!="2"&&value!="3"){server.send(400,"text/plain; charset=utf-8","方向設定無效");return;}
    uint8_t requested=value.toInt(),requestedBrightness=brightnessValue.toInt();uint16_t requestedTimeout=timeoutValue.toInt();
    if(String(requestedTimeout)!=timeoutValue||!screenpolicy::validTimeout(requestedTimeout)){server.send(400,"text/plain; charset=utf-8","關屏時間設定無效");return;}
    if(String(requestedBrightness)!=brightnessValue||!screenpolicy::validBrightness(requestedBrightness)){server.send(400,"text/plain; charset=utf-8","亮度設定無效");return;}
    if(!saveDisplaySettings(requested,requestedTimeout,requestedBrightness)){server.send(500,"text/plain; charset=utf-8","儲存失敗，螢幕設定尚未變更");return;}
    displayRotation=requested;screenTimeoutMinutes=requestedTimeout;screenBrightness=requestedBrightness;screen.setRotation(displayRotation);setBacklight(true);wakeScreen();forceDraw=true;
    server.sendHeader("Location","/display");server.send(303,"text/plain","");
  });
  server.on("/wifi/reset",HTTP_POST,[]{
    if(server.arg("nonce")!=setupNonce){server.send(403,"text/plain; charset=utf-8","請重新開啟設定頁");return;}
    apCloseAt=0;portalView=0;startPortal();server.send(200,"text/html; charset=utf-8",devicePage("<h1>配網已啟動</h1><p>請掃描裝置螢幕條碼，加入熱點後重新設定無線網路。</p><a href='/'>開啟配網頁</a>"));
  });
  server.on("/api/schedule",HTTP_POST,[]{if(!authorized())return;if(scheduleStorageFault.load()){server.send(409,"application/json","{\"error\":\"schedule-storage-fault\",\"detail\":\"班表儲存狀態不明；重開前請勿修改\"}");return;}if(localSchedule){server.send(409,"text/plain; charset=utf-8","目前使用班表，請從班表頁修改");return;}String input=server.arg("plain");uint32_t id=0;if(input.length()>MAX_JSON){server.send(413,"text/plain; charset=utf-8","班表資料過大");return;}if(!enqueueScheduleWork(ScheduleOrigin::Direct,std::move(input),id)){server.send(409,"text/plain; charset=utf-8","班表背景工作忙碌");return;}server.send(202,"application/json",String("{\"pending\":true,\"id\":")+id+"}");});
  server.on("/api/schedule-storage-status",HTTP_GET,[]{if(!authorized())return;server.send(200,"application/json",scheduleStorageFault.load()?"{\"fault\":true,\"reason\":\"schedule-storage-fault\",\"message\":\"班表儲存狀態不明；重開前請勿修改\"}":"{\"fault\":false,\"reason\":\"ready\"}");});
  server.on("/api/test",HTTP_POST,[]{if(!authorized())return;startRing("喇叭測試");server.send(200,"application/json","{\"ok\":true}");});
  server.on("/api/stop",HTTP_POST,[]{if(!authorized())return;stopRing(false);server.send(200,"application/json","{\"ok\":true}");});
  server.on("/api/wifi",HTTP_GET,[]{
    JsonDocument d;d["connected"]=WiFi.isConnected();d["connecting"]=connecting;d["failed"]=setupFailed;d["storageFault"]=wifiStorageFault;d["ip"]=WiFi.localIP().toString();d["management_url"]=manageUrl;
    auto list=d["networks"].to<JsonArray>();int n=WiFi.scanComplete();d["scanning"]=n==WIFI_SCAN_RUNNING;for(int i=0;i<n;i++){auto a=list.add<JsonObject>();a["ssid"]=WiFi.SSID(i);a["rssi"]=WiFi.RSSI(i);}if(n==WIFI_SCAN_FAILED&&!connecting&&wifiState.phase!=wififailover::Phase::Scanning)WiFi.scanNetworks(true,true);
    auto profiles=d["profiles"].to<JsonArray>();for(size_t p=0;p<wifiProfiles.count;p++){auto profile=profiles.add<JsonObject>();profile["ssid"]=wifiProfiles.profiles[p].ssid.c_str();profile["connected"]=WiFi.isConnected()&&WiFi.SSID().equals(wifiProfiles.profiles[p].ssid.c_str());bool visible=false;int strongest=-127;for(int i=0;i<n;i++)if(WiFi.SSID(i).equals(wifiProfiles.profiles[p].ssid.c_str())){visible=true;strongest=std::max(strongest,int(WiFi.RSSI(i)));}profile["visible"]=visible;if(visible)profile["rssi"]=strongest;else profile["rssi"]=nullptr;}
    String out;serializeJson(d,out);server.send(200,"application/json",out);
  });
  server.on("/api/wifi/scan",HTTP_POST,[]{
    if(!localNonce())return;
    if(connecting||wifiState.phase==wififailover::Phase::Scanning){server.send(409,"text/plain; charset=utf-8","裝置正在連線或掃描，請稍候再試");return;}
    if(WiFi.scanNetworks(true,true)==WIFI_SCAN_FAILED){server.send(503,"text/plain; charset=utf-8","掃描無法啟動，請稍後再試");return;}
    server.send(202,"text/plain; charset=utf-8","正在掃描無線網路");
  });
  server.on("/api/wifi/remove",HTTP_POST,[]{
    if(!localNonce())return;
    if(wifiStorageFault){server.send(409,"text/plain; charset=utf-8","Wi-Fi 儲存狀態待確認，暫停修改；裝置會重新載入確認");return;}
    String requested=server.arg("ssid");wifiprofiles::List next=wifiProfiles;
    const auto result=wifiprofiles::remove(next,requested.c_str(),requested.length());
    if(result==wifiprofiles::Result::Invalid){server.send(422,"text/plain; charset=utf-8","網路名稱格式無效");return;}
    if(result==wifiprofiles::Result::NotFound){server.send(404,"text/plain; charset=utf-8","找不到這組已保存網路");return;}
    const uint32_t now=millis();cancelWifiSelection(now);wifiprofiles::Selector committed;const auto stored=persistWifiProfiles(next,committed);
    if(stored==wifiprofiles::CommitResult::Failed){wipeString(requested);restartWifiSelection(now);server.send(409,"text/plain; charset=utf-8","儲存失敗，網路尚未移除");return;}
    if(stored==wifiprofiles::CommitResult::StorageFault){wipeString(requested);restartWifiSelection(now);server.send(409,"text/plain; charset=utf-8","Wi-Fi 儲存結果待確認，暫停修改；裝置會重新載入確認");return;}
    wifiProfiles=next;wifiSelector=committed;wifiSelectorValid=true;wipeString(requested);restartWifiSelection(now);
    server.send(200,"text/plain; charset=utf-8",stored==wifiprofiles::CommitResult::CommittedStorageFault?"已移除；舊儲存槽清理待確認，暫停其他修改":"已移除保存的 Wi-Fi");
  });
  server.on("/",HTTP_GET,[]{
    if(!portal){
      alarm_tailnet_status_t t={};alarm_tailnet_get_status(&t);
      String page=String("<h1>裝置管理</h1><p>本地設定網址：<a href='/'>http://")+WiFi.localIP().toString()+"/</a></p><p>"+tailnetLabel(t.state)+"</p>";
      page+="<a class='primary-link' href='/calendar'>開啟 Kevin 與晴晴鬧鐘</a>";
      page+="<p>在同一頁設定兩人的一般工作日與週四鬧鐘。</p><a href='/tailnet'>Tailscale 連線與授權</a>";
      page+="<h2>裝置設定</h2><p>調整螢幕方向、更換無線網路，或檢查更新。</p><a href='/display'>開啟裝置設定</a><p class='help'>同時按住「＋」與「－」十秒，也能重新配網。</p>";
      server.send(200,"text/html; charset=utf-8",devicePage(page));return;
    }
    String setupPage=String("<style></style><h1>班表鬧鐘配網</h1><p>配網熱點沒有網際網路。裝置嘗試新網路時，本頁仍可繼續使用。</p>")+WIFI_MANAGER_PAGE;setupPage.replace("WIFI_NONCE",setupNonce);server.send(200,"text/html; charset=utf-8",devicePage(setupPage));
#if 0

    String page=R"HTML(<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>班表鬧鐘配網</title><style>body{font:18px system-ui;max-width:32em;margin:24px}input,select{display:block;box-sizing:border-box;font:18px system-ui;width:100%;margin:8px 0 20px;padding:10px}button{font:18px system-ui;padding:12px}#status{line-height:1.6}</style><h1>班表鬧鐘</h1><p><a href=/display>裝置設定（方向／無線網路）</a></p><p>選擇目前環境的無線網路。配網熱點沒有網際網路，設定時請保持連線。</p><form id="wifi"><input type="hidden" name="nonce" value="SETUP_NONCE"><label>無線網路<select id="networks"><option value="">請選擇網路，或在下方輸入</option></select></label><label>網路名稱<input id="ssid" name="ssid" required maxlength="32" autocomplete="off"></label><label>密碼<input name="password" type="password" maxlength="63" autocomplete="off"></label><button>連線並儲存</button></form><p id="status"></p><script>const statusEl=document.querySelector('#status'),form=document.querySelector('#wifi'),list=document.querySelector('#networks');list.onchange=()=>document.querySelector('#ssid').value=list.value;let populated=false;async function update(){try{const d=await(await fetch('/api/wifi')).json();if(!populated&&d.networks.length){for(const n of d.networks){const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)';list.append(o)}populated=true}if(d.connected&&!d.connecting){statusEl.replaceChildren(document.createTextNode('連線成功，請將手機切回剛設定的無線網路，再開啟：'));const a=document.createElement('a');a.href='http://'+d.ip;a.textContent=a.href;statusEl.append(a)}else if(d.connecting)statusEl.textContent='連線中，請保持此頁開啟…';else if(d.failed)statusEl.textContent='連線失敗，請檢查密碼後重試。';}catch(e){}}form.onsubmit=async(e)=>{e.preventDefault();statusEl.textContent='連線中…';try{const r=await fetch('/setup',{method:'POST',body:new URLSearchParams(new FormData(form))});if(!r.ok)statusEl.textContent=await r.text()}catch(e){statusEl.textContent='請重新連上裝置熱點後再試。'}};update();setInterval(update,2000)</script>)HTML";page.replace("SETUP_NONCE",setupNonce);server.send(200,"text/html; charset=utf-8",devicePage(page));
#endif
  });
  server.on("/setup",HTTP_POST,[]{
    if(!localNonce())return;
    if(wifiStorageFault){server.send(409,"text/plain; charset=utf-8","Wi-Fi 儲存狀態待確認，暫停修改；裝置會重新載入確認");return;}
    String s=server.arg("ssid"),p=server.arg("password");
    wifiprofiles::Profile candidate{s.c_str(),s.length(),p.c_str(),p.length()};
    if(!wifiprofiles::valid(candidate)){wipeString(p);wipeString(s);server.send(422,"text/plain; charset=utf-8","網路名稱須為 1 至 32 bytes；密碼須留空或為 8 至 63 bytes");return;}
    if(wifiprofiles::indexOf(wifiProfiles,candidate.ssid.c_str(),candidate.ssid.size())<0&&wifiProfiles.count>=wifiprofiles::MAX_PROFILES){wipeString(p);wipeString(s);server.send(409,"text/plain; charset=utf-8","已保存 4 組網路，請先明確移除一組");return;}
    const uint32_t now=millis();cancelWifiSelection(now);startPortal();pendingSsid=candidate.ssid;pendingPassword=candidate.password;wipeString(p);wipeString(s);connecting=true;setupFailed=false;wifiPendingTrial=true;wifiTrialStarted=false;wifiTrialAt=now+wififailover::DISCONNECT_SETTLE_MS;connectStarted=now;apCloseAt=0;
    server.send(202,"application/json","{\"connecting\":true}");
  });
  server.onNotFound([]{if(portal){server.sendHeader("Location","http://192.168.4.1/");server.send(302,"text/plain","");}else server.send(404,"text/plain; charset=utf-8","找不到此頁面");}); server.begin();deviceRoutesReady=true;
}
void setup() {
  // Keep warnings/errors; packet-by-packet network logging stalls USB output.
  esp_log_level_set("ml_wg_mgr", ESP_LOG_WARN);
  esp_log_level_set("ml_net_io", ESP_LOG_WARN);
  esp_log_level_set("ml_tcp", ESP_LOG_WARN);
  esp_log_level_set("ml_derp", ESP_LOG_WARN);
  rtc_gpio_hold_dis(GPIO_NUM_21);rtc_gpio_init(GPIO_NUM_21);rtc_gpio_set_direction(GPIO_NUM_21,RTC_GPIO_MODE_OUTPUT_ONLY);rtc_gpio_set_level(GPIO_NUM_21,1); // Match the verified upstream board power control.
  Serial.begin(115200);ESP_ERROR_CHECK(nvs_flash_init());if(psramFound())heap_caps_malloc_extmem_enable(4096); esp_err_t nvs=nvs_flash_init_partition("alarm_nvs");ESP_ERROR_CHECK(nvs);if(!prefs.begin("shift-alarm",false,"alarm_nvs")){Serial.println("SETTINGS_STORAGE_FAILED");abort();}
  char nonce[33];snprintf(nonce,sizeof(nonce),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());setupNonce=nonce;snprintf(otaBootSession,sizeof(otaBootSession),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());lastCommand=prefs.getString("command");
  handled=prefs.getLong64("handled",0);snooze=prefs.getLong64("snooze",0);
#ifdef PROVISION_BACKEND
  /* A replacement board may contain an old device credential in NVS.  The
   * private production provisioning header is authoritative for this unit;
   * rotate only mismatched values so ordinary boots do not rewrite flash. */
  if(prefs.getString("token")!=PROVISION_TOKEN&&
     (prefs.putString("token",PROVISION_TOKEN)!=strlen(PROVISION_TOKEN)||prefs.getString("token")!=PROVISION_TOKEN))abort();
  if(prefs.getString("backend")!=PROVISION_BACKEND&&
     (prefs.putString("backend",PROVISION_BACKEND)!=strlen(PROVISION_BACKEND)||prefs.getString("backend")!=PROVISION_BACKEND))abort();
  if(prefs.getString("manage")!=PROVISION_MANAGE&&
     (prefs.putString("manage",PROVISION_MANAGE)!=strlen(PROVISION_MANAGE)||prefs.getString("manage")!=PROVISION_MANAGE))abort();
#endif
  if(!loadWifiProfiles()){setupFailed=true;}
  backend=prefs.getString("backend");
  if(backend=="http://192.168.18.31:8237"||backend=="http://192.168.18.31:8238"||backend=="http://100.126.226.79:8237"||backend=="http://100.126.226.79:8238"){
    backend="http://100.127.82.47:8237";
    if(prefs.putString("backend",backend)!=backend.length()||prefs.getString("backend")!=backend)abort();
  }
  token=prefs.getString("token");manageUrl=prefs.getString("manage");
  apPassword=prefs.getString("ap-pass");if(apPassword.isEmpty()){char b[13];snprintf(b,sizeof(b),"%08lx%04lx",(unsigned long)esp_random(),(unsigned long)(esp_random()&0xffff));apPassword=b;prefs.putString("ap-pass",apPassword);}
  uint8_t staMac[6]={};ESP_ERROR_CHECK(esp_read_mac(staMac,ESP_MAC_WIFI_STA));char apSuffix[5];deviceidentity::suffix(staMac,apSuffix);apName=String("ShiftAlarm-")+apSuffix;
  scheduleStorageFault.store(scheduleFaultMarker(false));String err;String stored=savedSchedule();savedScheduleRestored=!scheduleStorageFault.load()&&(!prefs.isKey("schedule")||(!stored.isEmpty()&&applySchedule(stored,false,err)));if(scheduleStorageFault.load())syncState="班表儲存狀態不明；重開前請勿修改";
  pinMode(BUTTON_STOP,INPUT_PULLUP);pinMode(BUTTON_SNOOZE,INPUT_PULLUP);pinMode(BUTTON_TEST,INPUT_PULLUP);
#if CUBE_TFT
  frame=new GFXcanvas16(240,240);assert(frame && frame->getBuffer());
  SPI.begin(9,-1,10,14);screen.init(240,240);loadDisplaySettings();screen.setRotation(displayRotation);screen.invertDisplay(true);backlightPwm=ledcAttach(13,5000,8);if(!backlightPwm)pinMode(13,OUTPUT);setBacklight(true);screenAwake=true;screenLastActivity=millis();
#else
  Wire.begin(41,42);screen.begin(SSD1306_SWITCHCAPVCC,0x3c);screen.setRotation(2);
#endif
  initBattery();
  i2s_config_t cfg={};cfg.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_TX);cfg.sample_rate=24000;cfg.bits_per_sample=I2S_BITS_PER_SAMPLE_16BIT;cfg.channel_format=I2S_CHANNEL_FMT_RIGHT_LEFT;cfg.communication_format=I2S_COMM_FORMAT_STAND_I2S;cfg.intr_alloc_flags=ESP_INTR_FLAG_LEVEL1;cfg.dma_buf_count=6;cfg.dma_buf_len=256;cfg.tx_desc_auto_clear=true;
  i2s_pin_config_t pins={};pins.mck_io_num=I2S_PIN_NO_CHANGE;pins.bck_io_num=15;pins.ws_io_num=16;pins.data_out_num=7;pins.data_in_num=I2S_PIN_NO_CHANGE;
  ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0,&cfg,0,nullptr));ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0,&pins));if(xTaskCreatePinnedToCore(soundTask,"speaker",3072,nullptr,2,nullptr,0)!=pdPASS)abort();
  alarm_ota_config_t otaConfig={};otaConfig.board_id="xingzhi-cube-1.54tft-wifi";otaConfig.device_token=(const uint8_t*)token.c_str();otaConfig.device_token_length=token.length();otaConfig.quiet_window_seconds=300;otaConfig.transfer_timeout_seconds=600;otaConfig.authorize=otaAuthorize;otaConfig.read_guard=otaReadGuard;otaConfig.marker_load=otaMarkerLoad;otaConfig.marker_store=otaMarkerStore;otaConfig.marker_clear=otaMarkerClear;
  backendendpoint::Endpoint endpoint;
  if(backendendpoint::parse(backend.c_str(),endpoint)){
    ESP_ERROR_CHECK(alarm_proxy_init(endpoint.host,endpoint.port));
  }
  scheduleWorkReady=initScheduleWorker();if(!scheduleWorkReady&&!scheduleStorageFault.load())syncState="班表背景儲存無法使用";backendPollReady=initBackendPoll();if(!backendPollReady&&!scheduleStorageFault.load())syncState="後端服務無法使用";
  WiFi.onEvent([](WiFiEvent_t,WiFiEventInfo_t info){Serial.printf("WIFI_DISCONNECTED reason=%u\n",unsigned(info.wifi_sta_disconnected.reason));},ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  setenv("TZ","CST-8",1);tzset();WiFi.persistent(false);WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(false);wifiState.phase_started_ms=millis();wifiState.retry_delay_ms=0;if(wifiProfiles.count)startWifiScan(millis());else startPortal();
  esp_sntp_set_time_sync_notification_cb(networkClockSynced);esp_sntp_set_sync_interval(CLOCK_SYNC_INTERVAL_MS);configTime(8*3600,0,"pool.ntp.org","time.google.com");routes();proxyStarted=alarm_proxy_start()==ESP_OK;bootMs=millis();draw();
  ESP_ERROR_CHECK(alarm_ota_boot_self_test(otaDiagnostics,nullptr,15000));
  otaReady=alarm_ota_init(&otaConfig)==ESP_OK;if(otaReady){esp_err_t staged=alarm_ota_load_staged();alarm_ota_status_t status={};alarm_ota_get_status(&status);if(staged!=ESP_OK||status.marker_fault){otaStateSet("marker-fault","marker-fault","更新記錄狀態不明，已封鎖下載與安裝");}else if(status.staged_valid){otaReceived=status.staged.manifest.size;otaTotal=status.staged.manifest.size;otaStateSet("staged","ready","已載入完整驗證的更新，可離線安裝");}else otaResetTerminalNoStaged();}
  Serial.printf("SHIFT_ALARM_READY v%s\n",VERSION);Serial.printf("PSRAM_BYTES %lu\n",(unsigned long)ESP.getPsramSize());Serial.println(wifiProfiles.count?"BOOT_WIFI_MODE SAVED":"BOOT_WIFI_MODE FIRST_SETUP");
  // MicroLink has priority-7 workers on both cores.  Keep the UI loop one
  // level above them so physical input remains responsive during Tailnet I/O.
  vTaskPrioritySet(nullptr,8);
}
void loop() {
  sampleBattery(millis());
  if(!tailnetStarted&&WiFi.isConnected()&&clockValid()&&millis()-tailnetAttempt>=30000){tailnetAttempt=millis();if(alarm_tailnet_start(apName.c_str())==ESP_OK)tailnetStarted=true;}
  if(!proxyStarted){if(alarm_proxy_start()==ESP_OK)proxyStarted=true;}
  refreshOtaGuard();if(otaReady&&!otaBusy.load())alarm_ota_maintenance();
  server.handleClient();if(portal)dns.processNextRequest();uint32_t ms=millis();time_t now=time(nullptr);
  serviceWifi(ms);
  if(portal&&apCloseAt&&int32_t(ms-apCloseAt)>=0)closePortal();
  observeBackendPollAvailability();drainBackendPollResult();drainScheduleResult();

  const buttons::Event event=buttons::update(buttonState,ms,!digitalRead(BUTTON_SNOOZE),!digitalRead(BUTTON_STOP),!digitalRead(BUTTON_TEST),screenAwake,ringing);
  observeBackendPollAvailability();drainBackendPollResult();drainScheduleResult();
  pairingHoldActive=buttonState.chord&&!buttonState.pairing_sent&&!portal;
  if(pairingHoldActive)pairingHoldStarted=buttonState.chord_started_ms;
  if(event!=buttons::None){screenLastActivity=ms;uiState.last_interaction_ms=ms;forceDraw=true;}
  if(event==buttons::StopAlarm)stopRing(false);
  if(clockValid()) {
    const int64_t corrected=alarmclock::reconcileHandled(now,handled);
    if(corrected!=handled){handled=corrected;prefs.putLong64("handled",handled);}
    int64_t latest=handled;String weeklyLabel;int64_t weeklyDue=INT64_MAX;weeklyOccurrence(now,true,handled,weeklyDue,weeklyLabel);if(weeklyDue!=INT64_MAX){startRing(weeklyLabel);latest=std::max(latest,weeklyDue);}
    for(auto &alarm:alarms)if(alarmclock::due(alarm.epoch,now,handled)){startRing(alarm.label);latest=std::max(latest,alarm.epoch);}
    if(latest!=handled){handled=latest;prefs.putLong64("handled",handled);}
    if(snooze&&now>=snooze){bool fresh=now-snooze<=alarmclock::CATCHUP_SECONDS;snooze=0;prefs.putLong64("snooze",0);if(fresh)startRing("貪睡提醒");}
  }
  if(!ringing){
    if(event==buttons::Wake)wakeScreen();
    else if(event==buttons::CenterLong)setScreenAwake(false);
    else if(buttons::pairingAllowed(event,ringing)){apCloseAt=0;portalView=0;startPortal();}
    else if(event==buttons::LeftShort&&!portal)deviceui::left(uiState,ms);
    else if(event==buttons::RightShort){if(portal)portalView=(portalView+1)%3;else deviceui::right(uiState,ms);}
    else if(event==buttons::CenterShort&&!portal)deviceui::center(uiState,ms);
    if(deviceui::returnIfInactive(uiState,ms))forceDraw=true;
  }
  if(ringing&&millis()-ringStarted>=RING_MS)stopRing(false);
  const bool buttonHeld=buttonState.left.stable||buttonState.center.stable||buttonState.right.stable;
  if(screenpolicy::shouldTurnOff(screenTimeoutMinutes,ms,screenLastActivity,ringing||portal||pairingHoldActive||buttonHeld))setScreenAwake(false);
  // Saved networks do not reopen setup automatically; require the deliberate chord.
  if(!pairingHoldActive)startBackendPoll(ms);
  // Main alone mutates schedules/snooze and selects the next boot image.
  // UI/backend changes above precede the fresh activation guard.
  bool activation=otaActivation.exchange(false);
  if(activation){refreshOtaGuard();esp_err_t result=alarm_ota_activate(&otaSession);if(result!=ESP_OK){otaBusy=false;alarm_ota_status_t status={};alarm_ota_get_status(&status);if(status.state==ALARM_OTA_IDLE)otaResetTerminalNoStaged();const bool guardBlocked=result==ALARM_OTA_ERR_CHARGING_REQUIRED||result==ALARM_OTA_ERR_UNSAFE;otaStateSet("error",status.marker_fault?"marker-fault":result==ALARM_OTA_ERR_CHARGING_REQUIRED?"charging-required":guardBlocked?"guard-rejected":"activation-failed",status.marker_fault?"更新記錄狀態不明，操作維持封鎖":result==ALARM_OTA_ERR_CHARGING_REQUIRED?"裝置未顯示充電，保留已下載更新":guardBlocked?"本機安全條件已改變，保留已下載更新":"本機驗證或啟動選擇失敗；請重新下載");}}
  static int64_t renderedMinute=-1;const bool mainMinute=uiState.page==deviceui::Page::Main&&now/60!=renderedMinute;
  static uint32_t wifiDrawAt=0;static wifiindicator::Value displayedWifi={wifiindicator::State::Offline,0};
  if(uint32_t(ms-wifiDrawAt)>=1000){
    wifiDrawAt=ms;const auto currentWifi=wifiIndicatorValue();
    if(wifiindicator::changed(displayedWifi,currentWifi)&&screenAwake&&!ringing&&!portal&&uiState.page==deviceui::Page::Main){
#if CUBE_TFT
      drawWifiIndicator();
      for(int row=0;row<wifiindicator::HEIGHT;row++)screen.drawRGBBitmap(wifiindicator::X,wifiindicator::Y+row,frame->getBuffer()+(wifiindicator::Y+row)*240+wifiindicator::X,wifiindicator::WIDTH,1);
#endif
    }
    displayedWifi=currentWifi;
  }
  const bool portalRefresh=portal&&uint32_t(ms-lastDraw)>=500;
  const bool pairingRefresh=pairingHoldActive&&uint32_t(ms-lastDraw)>=200;
  const bool detailRefresh=uiState.page!=deviceui::Page::Main&&uiState.page!=deviceui::Page::Menu&&uint32_t(ms-lastDraw)>=1000;
  if(forceDraw||(ringing&&uint32_t(ms-lastDraw)>=200)||portalRefresh||pairingRefresh||mainMinute||detailRefresh){lastDraw=ms;draw();renderedMinute=now/60;}
  if(ms-lastSerial>=10000){lastSerial=ms;Serial.printf("STATUS setup=%d wifi=%d clock=%d alarms=%u ringing=%d\n",portal,WiFi.isConnected(),clockValid(),unsigned(alarms.size()),ringing);}
  delay(2);
}
