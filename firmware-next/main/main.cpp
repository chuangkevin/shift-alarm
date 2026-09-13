#include "ota_download_policy.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
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
#include <esp_heap_caps.h>
#include <esp_app_desc.h>
#include <time.h>
#include <sys/time.h>
#if __has_include("provisioning.h")
#include "provisioning.h"
#endif
#include <vector>
#include <algorithm>
#include <atomic>
#include "scheduler.h"
#include "alarm_tailnet.h"
#include "alarm_ota.h"
#include "alarm_proxy.h"
#include "driver/rtc_io.h"
#include "esp_task_wdt.h"
#include "esp_sntp.h"
#include "zh_glyphs.h"
#include "clock_page.h"
#include "screen_policy.h"

const char *const VERSION=esp_app_get_description()->version;
constexpr size_t MAX_ALARMS=512, MAX_JSON=98304;
constexpr uint32_t POLL_MS=5000, RING_MS=180000;
constexpr uint32_t CLOCK_SYNC_INTERVAL_MS=3UL*60*60*1000;
std::atomic<uint32_t> lastNetworkClock{0},lastNetworkClockMs{0};
void networkClockSynced(struct timeval *tv){lastNetworkClock=uint32_t(tv->tv_sec);lastNetworkClockMs=millis();}
constexpr int BUTTON_STOP=0, BUTTON_SNOOZE=39, BUTTON_TEST=40;
struct Alarm { String id,label; int64_t epoch; };
std::vector<Alarm> alarms;
Preferences prefs;
WebServer server(IPAddress(127,0,0,1),8081);
DNSServer dns;
String apName,pendingSsid,pendingPassword,setupNonce;
bool connecting=false, setupFailed=false, showJoinQr=true;
uint32_t connectStarted=0,apCloseAt=0;
String lastCommand;
bool tailnetStarted=false,proxyStarted=false;
bool savedScheduleRestored=true, deviceRoutesReady=false, localSchedule=false, firstConsecutiveOnly=false;
std::atomic<bool> speakerReady{false};
uint32_t tailnetAttempt=0;
// Prevent Arduino from confirming a pending image before application diagnostics.
extern "C" bool verifyRollbackLater(){return true;}

String revision, backend, token, manageUrl, ssid, password, apPassword;
int64_t handled=0,snooze=0;
volatile bool ringing=false;
volatile uint32_t buttonEvents=0,physicalStopDown=0,lastPlusPress=0;
portMUX_TYPE buttonMux=portMUX_INITIALIZER_UNLOCKED;
void IRAM_ATTR stopPressed(){portENTER_CRITICAL_ISR(&buttonMux);buttonEvents|=ringing?8:1;physicalStopDown=xTaskGetTickCountFromISR()*portTICK_PERIOD_MS;ringing=false;portEXIT_CRITICAL_ISR(&buttonMux);}
void IRAM_ATTR snoozePressed(){portENTER_CRITICAL_ISR(&buttonMux);buttonEvents|=ringing?8:2;ringing=false;portEXIT_CRITICAL_ISR(&buttonMux);}
void IRAM_ATTR plusPressed(){portENTER_CRITICAL_ISR(&buttonMux);uint32_t now=xTaskGetTickCountFromISR()*portTICK_PERIOD_MS;if(now-lastPlusPress>=200){lastPlusPress=now;buttonEvents|=ringing?8:4;ringing=false;}portEXIT_CRITICAL_ISR(&buttonMux);}
uint32_t ringStarted=0,lastPoll=0,lastDraw=0,lastSerial=0,bootMs=0;
bool portal=false;
uint32_t pairingHoldStarted=0;
bool pairingHoldActive=false,pairingTriggered=false;
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

constexpr char DISPLAY_SETTINGS_KEY[]="display-v1";
bool saveDisplaySettings(uint8_t rotation,uint16_t timeoutMinutes,uint8_t brightness){
  if(rotation>3||!screenpolicy::validTimeout(timeoutMinutes)||!screenpolicy::validBrightness(brightness))return false;
  const uint8_t blob[]={ 'D',2,rotation,uint8_t(timeoutMinutes),uint8_t(timeoutMinutes>>8),brightness };
  if(prefs.putBytes(DISPLAY_SETTINGS_KEY,blob,sizeof(blob))!=sizeof(blob)||prefs.getBytesLength(DISPLAY_SETTINGS_KEY)!=sizeof(blob))return false;
  uint8_t verified[sizeof(blob)]={};
  return prefs.getBytes(DISPLAY_SETTINGS_KEY,verified,sizeof(verified))==sizeof(verified)&&memcmp(blob,verified,sizeof(blob))==0;
}
void loadDisplaySettings(){
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
        displayRotation=blob[2];screenTimeoutMinutes=timeout;screenBrightness=brightness;
        if(length==5)saveDisplaySettings(displayRotation,screenTimeoutMinutes,screenBrightness);
      }
    }
  }else saveDisplaySettings(displayRotation,screenTimeoutMinutes,screenBrightness);
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

constexpr char WIFI_CREDENTIALS_KEY[]="wifi-v1";
constexpr size_t WIFI_BLOB_HEADER=5, WIFI_BLOB_MAX=WIFI_BLOB_HEADER+32+63;
bool validWifiCredentials(const String &name,const String &secret){
  return name.length()>0&&name.length()<=32&&secret.length()<=63&&
    strlen(name.c_str())==name.length()&&strlen(secret.c_str())==secret.length();
}
bool saveWifiCredentials(const String &name,const String &secret){
  if(!validWifiCredentials(name,secret))return false;
  // One NVS blob/commit binds the SSID and password together across power loss.
  uint8_t blob[WIFI_BLOB_MAX]={ 'W','F',1,uint8_t(name.length()),uint8_t(secret.length()) };
  size_t length=WIFI_BLOB_HEADER+name.length()+secret.length();
  memcpy(blob+WIFI_BLOB_HEADER,name.c_str(),name.length());
  memcpy(blob+WIFI_BLOB_HEADER+name.length(),secret.c_str(),secret.length());
  if(prefs.putBytes(WIFI_CREDENTIALS_KEY,blob,length)!=length||prefs.getBytesLength(WIFI_CREDENTIALS_KEY)!=length)return false;
  uint8_t verified[WIFI_BLOB_MAX]={};
  return prefs.getBytes(WIFI_CREDENTIALS_KEY,verified,length)==length&&memcmp(blob,verified,length)==0;
}
bool loadWifiCredentials(){
  ssid="";password="";
  if(prefs.isKey(WIFI_CREDENTIALS_KEY)){
    // A present but invalid current blob never falls back to stale credentials.
    size_t length=prefs.getBytesLength(WIFI_CREDENTIALS_KEY);uint8_t blob[WIFI_BLOB_MAX]={};
    if(length<WIFI_BLOB_HEADER||length>sizeof(blob)||prefs.getBytes(WIFI_CREDENTIALS_KEY,blob,length)!=length)return false;
    if(blob[0]!='W'||blob[1]!='F'||blob[2]!=1||blob[3]==0||blob[3]>32||blob[4]>63||length!=WIFI_BLOB_HEADER+blob[3]+blob[4])return false;
    char name[33]={},secret[64]={};memcpy(name,blob+WIFI_BLOB_HEADER,blob[3]);memcpy(secret,blob+WIFI_BLOB_HEADER+blob[3],blob[4]);
    String decodedName(name),decodedSecret(secret);
    if(decodedName.length()!=blob[3]||decodedSecret.length()!=blob[4]||!validWifiCredentials(decodedName,decodedSecret))return false;
    ssid=decodedName;password=decodedSecret;return true;
  }
  // Legacy two-key storage is read only for a one-time migration, never updated.
  String oldName=prefs.getString("ssid"),oldSecret=prefs.getString("password");
  if(oldName.isEmpty())return !prefs.isKey("ssid");
  if(!saveWifiCredentials(oldName,oldSecret))return false;
  ssid=oldName;password=oldSecret;return true;
}
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
void line(int x,int y,String s,int size=1) {
  surface.setTextSize(size);surface.setTextColor(0xffff);
  for(unsigned i=0;i<s.length();) {
    uint32_t cp=(uint8_t)s[i++];
    if(cp>=0xe0&&cp<0xf0&&i+1<s.length()){cp=((cp&15)<<12)|(((uint8_t)s[i]&63)<<6)|((uint8_t)s[i+1]&63);i+=2;}
    else if(cp>=0xc0&&cp<0xe0&&i<s.length()){cp=((cp&31)<<6)|((uint8_t)s[i++]&63);}
    if(cp<128){if(x+6*size>240)break;surface.setCursor(x,y+2*size);surface.write(cp);x+=6*size;continue;}
    if(x+12*size>240)break;
    for(const auto &g:ZH_GLYPHS)if(g.code==cp){for(int row=0;row<12;row++)for(int col=0;col<12;col++)if(g.rows[row]&(1<<(11-col)))surface.fillRect(x+col*size,y+row*size,size,size,0xffff);break;}
    x+=12*size;
  }
}
String tailnetLabel(alarm_tailnet_state_t state);
void draw() {
  if(!screenAwake)return;
  fill(ringing && (millis()/500)%2 ? 0x7800 : 0); surface.setTextColor(0xffff);
#if CUBE_TFT
  line(8,4,"班表鬧鐘",2); line(8,31,pairingHoldActive?String("配網倒數 ")+String(10-(millis()-pairingHoldStarted)/1000):clockValid()?datetime(time(nullptr)):"等待校時",2);
  if(ringing) { line(8,58,"鬧鐘響了",2); line(8,89,"按任一按鈕停止"); }
  else {
    int64_t next=snooze>time(nullptr)?snooze:INT64_MAX;
    String label="貪睡提醒";
    for(auto &a:alarms) if(alarmclock::upcoming(a.epoch,time(nullptr),handled)&&a.epoch<next){next=a.epoch;label=a.label;}
    line(8,62,next==INT64_MAX?"目前沒有鬧鐘":String("下次：")+datetime(next));
    line(8,81,next==INT64_MAX?"請設定班表與時間":"上班鬧鐘");
  }
  const bool pairingScreen=portal&&!(WiFi.isConnected()&&apCloseAt);
  // Keep the readable LAN address above the QR quiet zone (starts at y=122).
  if(WiFi.isConnected())line(8,103,String("http://")+WiFi.localIP().toString()+"/display");
  else line(8,103,pairingScreen?"http://192.168.4.1/":"無線網路未連線");
  String qrText=pairingScreen?(showJoinQr?String("WIFI:T:WPA;S:")+apName+";P:"+apPassword+";;":"http://192.168.4.1"):(WiFi.isConnected()?String("http://")+WiFi.localIP().toString()+"/display":String());
  if(qrText.length()&&qrText.length()<=180) {
    uint8_t data[qrcode_getBufferSize(8)]; QRCode qr;
    if(qrcode_initText(&qr,data,8,ECC_LOW,qrText.c_str())==0) {
      const int scale=2, x=8,y=130;
      surface.fillRect(x-8,y-8,(qr.size+8)*scale,(qr.size+8)*scale,0xffff);
      for(int row=0;row<qr.size;row++)for(int col=0;col<qr.size;col++)if(qrcode_getModule(&qr,col,row))surface.fillRect(x+col*scale,y+row*scale,scale,scale,0);
    }
  }
  if(pairingScreen) { line(115,130,showJoinQr?"①掃碼加入熱點":"②掃碼設定網路"); line(115,145,apName.substring(0,19)); line(115,160,"熱點密碼："); line(115,174,apPassword);line(115,190,"＋：切換條碼");line(115,204,connecting?"連線中…":setupFailed?"請重試連線":"192.168.4.1"); }
  else { line(115,133,WiFi.isConnected()?"無線網路已連線":"無線網路未連線"); line(115,151,"掃碼設定裝置"); line(115,169,String(alarms.size())+" 個鬧鐘"); line(115,187,localSchedule?"班表已儲存":syncState=="班表已同步"?"班表已同步":"等待班表同步");alarm_tailnet_status_t ts={};alarm_tailnet_get_status(&ts);line(115,204,tailnetLabel(ts.state)); }
  line(115,219,pairingHoldActive?"放開即取消配網":ringing?"響鈴時任意鍵停止":"換網路按＋－十秒");
  // Compose off-screen: never clear the visible TFT between text and QR draws.
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
void startRing(String label) { ringLabel=label; ringStarted=millis(); ringing=true; wakeScreen(); Serial.println("ALARM_RING_STARTED"); }
void stopRing(bool doSnooze) { ringing=false; snooze=doSnooze&&clockValid()?time(nullptr)+alarmclock::SNOOZE_SECONDS:0; prefs.putLong64("snooze",snooze); Serial.println(doSnooze?"ALARM_SNOOZED":"ALARM_STOPPED"); }
#include "calendar_metadata.h"
JsonDocument calendarMonths;
bool applySchedule(const String &body,bool persist,String &error) {
  if(body.length()>MAX_JSON){error="班表資料過大";return false;}
  JsonDocument doc; if(deserializeJson(doc,body)){error="資料格式無效";return false;}
  if(!doc["revision"].is<String>()||doc["revision"].as<String>().isEmpty()||doc["timezone"]!="Asia/Taipei"||!doc["alarms"].is<JsonArray>()||doc["alarms"].size()>MAX_ALARMS){error="班表欄位不完整";return false;}
  const JsonVariantConst firstOnlySetting=doc["first_consecutive_only"];
  if(!firstOnlySetting.isNull()&&!firstOnlySetting.is<bool>()){error="連續上班通知設定無效";return false;}
  bool nextFirstConsecutiveOnly=firstOnlySetting.is<bool>()?firstOnlySetting.as<bool>():false;
  JsonDocument nextMonths;
  if(doc.as<JsonObjectConst>().containsKey("months")){
    if(!localcalendar::validMonths(doc["months"])) {error="月份設定格式無效或超過 120 個月";return false;}
    nextMonths.set(doc["months"]);
  }else nextMonths.to<JsonObject>();
  if(nextMonths.overflowed()){error="月份設定記憶體不足";return false;}
  std::vector<Alarm> next;
  for(JsonObject a:doc["alarms"].as<JsonArray>()) {
    if(!a["id"].is<String>()||a["id"].as<String>().isEmpty()||a["id"].as<String>().length()>128||!a["epoch"].is<int64_t>()||a["epoch"].as<int64_t>()<alarmclock::VALID_CLOCK||!a["label"].is<String>()||a["label"].as<String>().length()>256){error="鬧鐘資料無效";return false;}
    for(auto &b:next)if(b.id==a["id"].as<String>()){error="鬧鐘識別碼重複";return false;}
    next.push_back({a["id"].as<String>(),a["label"].as<String>(),a["epoch"].as<int64_t>()});
  }
  std::sort(next.begin(),next.end(),[](const Alarm&a,const Alarm&b){return a.epoch<b.epoch;});
  JsonDocument saved;saved["revision"]=doc["revision"];saved["timezone"]=doc["timezone"];saved["alarms"]=doc["alarms"];saved["source"]=doc["source"]=="local"?"local":"remote";saved["months"]=nextMonths;saved["first_consecutive_only"]=nextFirstConsecutiveOnly;String canonical;serializeJson(saved,canonical);
  if(saved.overflowed()||canonical.length()>MAX_JSON){error="班表資料過大";return false;}
  if(persist&&savedSchedule()!=canonical&&prefs.putBytes("schedule",canonical.c_str(),canonical.length())!=canonical.length()){error="儲存失敗";return false;}
  if(persist&&savedSchedule()!=canonical){error="儲存驗證失敗，請重試";return false;}
  calendarMonths=std::move(nextMonths);
  localSchedule=doc["source"]=="local";firstConsecutiveOnly=nextFirstConsecutiveOnly;alarms=std::move(next);revision=doc["revision"].as<String>();
  // Authenticated LAN server also provides time if outbound NTP is unavailable.
  if(persist && !clockValid() && doc["server_time"].is<int64_t>() && doc["server_time"].as<int64_t>()>=alarmclock::VALID_CLOCK) {
    timeval tv={};tv.tv_sec=doc["server_time"].as<int64_t>();settimeofday(&tv,nullptr);
  }
  if(persist && doc["management_url"].is<String>()) {
    String url=doc["management_url"].as<String>();if(url.length()<=180&&url.startsWith("http")&&url!=manageUrl){manageUrl=url;prefs.putString("manage",url);}
  }
  if(persist && doc["command"]["type"]=="stop") {
    String id=doc["command"]["id"].as<String>();int64_t issued=doc["command"]["issued_at"]|int64_t(0);
    if(id.length()&&id!=lastCommand&&clockValid()&&time(nullptr)-issued>=0&&time(nullptr)-issued<120){lastCommand=id;prefs.putString("command",id);stopRing(false);}
  }
  return true;
}
bool authorized() { if(token.length()&&server.header("Authorization")==String("Bearer ")+token)return true; server.send(401,"application/json","{\"error\":\"需要裝置授權\"}");return false; }
void startPortal() { wakeScreen();if(portal)return;portal=true; WiFi.mode(WIFI_AP_STA); WiFi.softAP(apName.c_str(),apPassword.c_str());dns.start(53,"*",WiFi.softAPIP());WiFi.scanNetworks(true); Serial.println("SETUP_AP_STARTED");Serial.println(String("SETUP_AP_SSID ")+apName);Serial.println("SETUP_URL http://192.168.4.1"); }
String devicePage(String content) {
  int styleEnd=content.indexOf("</style>");
  if(styleEnd>=0)content=content.substring(styleEnd+8);
  else {int meta;while((meta=content.indexOf("<meta"))>=0){int end=content.indexOf('>',meta);if(end<0)break;content.remove(meta,end-meta+1);}}
  return String(R"PAGE(<!doctype html><html lang="zh-Hant"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#152b42"><title>班表鬧鐘・裝置設定</title><style>
:root{color-scheme:light;font-family:system-ui,-apple-system,"Noto Sans TC",sans-serif;color:#152b42;background:#f6f7f3}*{box-sizing:border-box}[hidden]{display:none!important}input[type=checkbox]{width:20px;height:20px;margin-right:8px}body{margin:0;padding:32px 20px 64px;font-size:16px;line-height:1.65}header,main,footer{width:100%;max-width:720px;margin:auto}header{display:flex;align-items:center;gap:14px;margin-bottom:28px}.brand-icon{display:grid;place-items:center;background:#152b42;color:#a8ead6;border-radius:16px;width:52px;height:52px;font-size:32px}.brand small{display:block;color:#668078;font-size:12px;letter-spacing:.08em}.brand strong{font-size:24px;letter-spacing:.02em}.badge{margin-left:auto;background:#e2f1e9;border:1px solid #cce3d6;color:#34614f;border-radius:30px;padding:6px 12px;font-size:12px}.card{background:#fff;border:1px solid #e0e5df;border-radius:24px;box-shadow:0 8px 32px #152b4207;padding:30px}h1{font-size:28px;line-height:1.3;margin:0 0 20px;letter-spacing:-.03em}h2{font-size:21px;border-top:1px solid #e6eae5;padding-top:28px;margin-top:32px}p{color:#63716c;margin:16px 0}form{margin:16px 0 20px}label{display:block;font-weight:600;margin-top:16px}input:not([type=hidden]):not([type=checkbox]),select{display:block;width:100%;min-height:50px;border:1px solid #ced8d1;border-radius:12px;background:#fafcf9;color:#152b42;font:inherit;padding:12px 14px;margin:8px 0 18px}input:focus,select:focus{outline:3px solid #a6dfce;outline-offset:2px}button,.primary-link{display:inline-block;border:0;border-radius:12px;background:#152b42;color:white;font:inherit;font-weight:600;padding:13px 20px;min-height:48px;cursor:pointer;text-align:center}button:hover{background:#28455e}button:active{transform:translateY(1px)}a{color:#276453;text-underline-offset:4px}a.primary-link{color:white;text-decoration:none;display:block}#status:not(:empty){background:#eaf5ee;border-radius:14px;padding:16px;overflow-wrap:anywhere}footer{text-align:center;color:#819087;font-size:12px;padding-top:24px}.help{font-size:13px}@media(max-width:480px){body{padding:22px 14px 40px}.card{padding:22px 20px;border-radius:20px}h1{font-size:25px}.badge{display:none}button{width:100%}}
.calendar{display:grid;grid-template-columns:repeat(7,minmax(0,1fr));gap:5px;text-align:center}.calendar button{position:relative;min-height:42px;padding:5px;border-radius:9px;background:#edf1ee;color:#152b42;width:100%;font-size:16px}.calendar button.workday{background:#152b42;color:#a8ead6}.calendar button.silent-workday{background:#fff;color:#152b42;border:2px solid #152b42}.calendar button.reviewday{background:#ffe4b1;color:#714800}.calendar button.today{box-shadow:inset 0 0 0 2px #d43c32}.calendar button.today::after{content:"";position:absolute;top:5px;right:5px;width:6px;height:6px;border-radius:50%;background:#e23b32;box-shadow:0 0 0 1px #fff}.calendar button:disabled{opacity:.6}.time-row{display:grid;grid-template-columns:auto minmax(0,1fr) minmax(0,1fr) auto auto;gap:8px;align-items:center;margin:12px 0}.time-row label,.time-row select{margin:0!important}.time-row select{padding:10px 6px!important}.time-row button{padding:10px;min-height:48px;width:auto}.alarm-enable{display:flex!important;align-items:center;gap:5px;white-space:nowrap}.alarm-enable input{margin:0}.secondary{background:#e9efec;color:#274c42}.secondary:hover{background:#dce8e1}.upload-box{padding:16px;background:#f1f6f2;border:1px dashed #b9d0c4;border-radius:12px}.check-option{display:flex;align-items:center;gap:8px;padding:14px 0}.check-option input{flex:0 0 auto;margin:0}.time-row strong{font-size:14px}#ai-status{overflow-wrap:anywhere}button:disabled{cursor:wait;opacity:.6}#weekdays{margin-bottom:10px;color:#63716c}@media(max-width:600px){.time-row{grid-template-columns:1fr 1fr auto}.time-row strong{grid-column:1/-1}.alarm-enable{grid-column:1/3}}
</style><header><div class="brand-icon">◷</div><div class="brand"><small>每個上班日，準時提醒</small><strong>班表鬧鐘</strong></div><span class="badge">裝置設定</span></header><main><section class="card">)PAGE")+content+"</section></main><footer>設定保存在你的裝置 · 重新開機仍會保留</footer></html>";
}
bool localNonce();
std::atomic<uint32_t> otaReceived{0},otaTotal{0};
std::atomic<bool> otaBusy{false},otaPowerConfirmed{false};
std::atomic<alarm_ota_handle_t> otaActivation{0};
bool otaReady=false;int otaSession;
portMUX_TYPE otaMux=portMUX_INITIALIZER_UNLOCKED;
alarm_ota_guard_t otaGuard={};char otaMessage[128]="尚未檢查更新";
void otaMessageSet(const char *message){portENTER_CRITICAL(&otaMux);strlcpy(otaMessage,message,sizeof(otaMessage));portEXIT_CRITICAL(&otaMux);}
String otaMessageGet(){char msg[128];portENTER_CRITICAL(&otaMux);memcpy(msg,otaMessage,sizeof(msg));portEXIT_CRITICAL(&otaMux);return String(msg);}
bool otaAuthorize(const void *request,void*){return request==&otaSession&&otaBusy.load();}
esp_err_t otaReadGuard(alarm_ota_guard_t *out,void*){portENTER_CRITICAL(&otaMux);*out=otaGuard;portEXIT_CRITICAL(&otaMux);out->operator_confirmed_power=otaPowerConfirmed.load();out->ringing=ringing;out->now_epoch=time(nullptr);return ESP_OK;}
void refreshOtaGuard(){alarm_ota_guard_t g={};g.clock_valid=clockValid();g.ringing=ringing;g.snoozed=snooze>0;g.schedule_ready=!revision.isEmpty();g.now_epoch=time(nullptr);int64_t next=snooze>0?snooze:INT64_MAX;for(auto &a:alarms)if(alarmclock::upcoming(a.epoch,g.now_epoch,handled)&&a.epoch<next)next=a.epoch;g.next_alarm_epoch=next==INT64_MAX?0:next;portENTER_CRITICAL(&otaMux);otaGuard=g;portEXIT_CRITICAL(&otaMux);}
esp_err_t otaDiagnostics(void*){
  if(!frame||!frame->getBuffer()||!prefs.isKey("ap-pass")||!prefs.isKey(DISPLAY_SETTINGS_KEY)||!savedScheduleRestored||
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
void otaWorker(void*){
 {
  bool readyToActivate=false;
  alarm_ota_handle_t handle=0;HTTPClient h;esp_err_t err=ESP_FAIL;
  do{
    otaMessageSet("正在檢查更新版本");
    h.setConnectTimeout(3000);h.setTimeout(5000);h.begin(backend+"/api/device/update");h.addHeader("Authorization",String("Bearer ")+token);
    if(h.GET()!=200){otaMessageSet("無法取得更新資訊");break;}
    String manifestBody;if(!boundedHttpBody(h,4096,manifestBody,5000)){otaMessageSet("更新資訊大小無效或傳輸不完整");break;}
    JsonDocument d;if(deserializeJson(d,manifestBody)){otaMessageSet("更新資訊格式無效");break;}h.end();
    if(d["available"]!=true){otaMessageSet("目前沒有已發布的更新");break;}
    JsonObject m=d["manifest"];alarm_ota_manifest_t manifest={};
    const char *board=m["board"]|"",*version=m["version"]|"",*sha=m["sha256"]|"",*mac=m["hmac_sha256"]|"";
    if(strlen(board)>=sizeof(manifest.board)||strlen(version)>=sizeof(manifest.version)||strlen(sha)!=64||strlen(mac)!=64||!m["size"].is<uint32_t>()){otaMessageSet("更新資訊格式無效");break;}
    strlcpy(manifest.board,board,sizeof(manifest.board));strlcpy(manifest.version,version,sizeof(manifest.version));strlcpy(manifest.sha256,sha,sizeof(manifest.sha256));strlcpy(manifest.hmac_sha256,mac,sizeof(manifest.hmac_sha256));manifest.size=m["size"];
    otaTotal.store(manifest.size);
    err=alarm_ota_begin(&manifest,&otaSession,&handle);if(err!=ESP_OK){otaMessageSet("更新遭拒：請確認版本、排程與電源");break;}
    const String firmwareUrl=backend+"/api/device/firmware/"+manifest.sha256+".bin";
    h.begin(firmwareUrl);h.addHeader("Authorization",String("Bearer ")+token);
    if(h.GET()!=200||h.getSize()!=int(manifest.size)){otaMessageSet("更新檔大小或回應不正確");break;}
    auto *stream=h.getStreamPtr();uint8_t buffer[4096];size_t received=0;uint8_t reconnects=0;uint32_t started=millis(),lastProgress=started;const char *failure=nullptr;
    otaMessageSet("正在下載並驗證，請保持供電");
    while(received<manifest.size){
      auto limit=alarm_download::deadline(millis(),started,lastProgress);
      if(limit==alarm_download::Deadline::total){failure="下載超過十分鐘，保留原有版本";break;}
      int available=stream->available();
      if(available<=0&&(limit==alarm_download::Deadline::idle||!h.connected())){
        if(!alarm_download::mayReconnect(reconnects)){failure="下載多次中斷，保留原有版本";break;}
        reconnects++;h.end();h.setConnectTimeout(3000);h.setTimeout(5000);h.begin(firmwareUrl);
        h.addHeader("Authorization",String("Bearer ")+token);h.addHeader("Range",String("bytes=")+received+"-");
        const char *rangeHeaders[]={"Content-Range"};h.collectHeaders(rangeHeaders,1);
        const int code=h.GET();const String expectedRange=String("bytes ")+received+"-"+(manifest.size-1)+"/"+manifest.size;
        if(code!=HTTP_CODE_PARTIAL_CONTENT||h.getSize()!=int(manifest.size-received)||h.header("Content-Range")!=expectedRange){failure="無法安全續傳更新檔，保留原有版本";break;}
        stream=h.getStreamPtr();lastProgress=millis();continue;
      }
      if(available<=0){vTaskDelay(pdMS_TO_TICKS(1));continue;}
      size_t need=std::min(sizeof(buffer),std::min(size_t(available),size_t(manifest.size-received)));
      int n=stream->read(buffer,need);
      if(n<=0||alarm_ota_write(handle,&otaSession,buffer,size_t(n))!=ESP_OK){failure="更新寫入或安全檢查失敗，保留原有版本";break;}
      received+=size_t(n);otaReceived.store(received);lastProgress=millis();vTaskDelay(1);
    }
    h.end();if(failure){otaMessageSet(failure);break;}
    if(alarm_ota_finish(handle,&otaSession)!=ESP_OK){otaMessageSet("更新驗證失敗，保留原有版本");break;}
    otaMessageSet("驗證通過，準備重新啟動");
    readyToActivate=true;
  }while(false);
  h.end();
  if(readyToActivate)otaActivation.store(handle);
  else {if(handle)alarm_ota_abort(handle,&otaSession);otaPowerConfirmed=false;otaBusy=false;}
 }
 vTaskDelete(nullptr);
}
void otaRoutes(){
 server.on("/update",HTTP_GET,[]{String page=R"HTML(<h1>裝置更新</h1><p>更新寫入備用分區，通過驗證才重新啟動。響鈴中、貪睡中或五分鐘內有鬧鐘時不進行更新。</p><p id="status">正在取得狀態…</p><form id="update"><label><input type="checkbox" id="power" required>我已接上穩定電源，更新完成前不拔除</label><button>檢查並安裝已發布版本</button></form><p class="help">電源確認由你提供，裝置未量測外接電壓。更新失敗會保留原有版本。</p><a href="/display">返回裝置設定</a><script>const nonce='NONCE';const statusEl=document.querySelector('#status');async function refresh(){try{const r=await fetch('/api/update',{headers:{'X-Setup-Nonce':nonce}});const d=await r.json();statusEl.textContent=d.message+(d.total>0?'（已接收 '+d.received+' / '+d.total+' 位元組，'+Math.floor(d.received*100/d.total)+'%）':'');}catch(e){statusEl.textContent='裝置可能正在重新啟動，請稍候重新整理。'}}document.querySelector('#update').onsubmit=async(e)=>{e.preventDefault();if(!document.querySelector('#power').checked)return;const r=await fetch('/api/update/start',{method:'POST',headers:{'X-Setup-Nonce':nonce,'Content-Type':'application/x-www-form-urlencoded'},body:'power_confirmed=1'});statusEl.textContent=await r.text();};refresh();setInterval(refresh,2000);</script>)HTML";page.replace("NONCE",setupNonce);server.send(200,"text/html; charset=utf-8",devicePage(page));});
 server.on("/api/update",HTTP_GET,[]{if(!localNonce())return;JsonDocument d;d["message"]=otaMessageGet();d["busy"]=otaBusy.load();d["ready"]=otaReady;d["received"]=otaReceived.load();d["total"]=otaTotal.load();String out;serializeJson(d,out);server.send(200,"application/json",out);});
 server.on("/api/update/start",HTTP_POST,[]{if(!localNonce())return;if(!otaReady||!WiFi.isConnected()){server.send(503,"text/plain; charset=utf-8","更新功能尚未就緒");return;}if(server.arg("power_confirmed")!="1"){server.send(400,"text/plain; charset=utf-8","請先確認穩定供電");return;}bool expected=false;if(!otaBusy.compare_exchange_strong(expected,true)){server.send(409,"text/plain; charset=utf-8","更新正在進行中");return;}otaReceived.store(0);otaTotal.store(0);otaPowerConfirmed=true;if(xTaskCreate(otaWorker,"alarm_update",12288,nullptr,1,nullptr)!=pdPASS){otaBusy=false;otaPowerConfirmed=false;server.send(503,"text/plain; charset=utf-8","記憶體不足，請稍後重試");return;}server.send(202,"text/plain; charset=utf-8","開始檢查更新");});
}
String tailnetLabel(alarm_tailnet_state_t state) {
  switch(state){case ALARM_TAILNET_CONNECTED:return "Tailscale已連線";case ALARM_TAILNET_AUTH_REQUIRED:return "等待登入授權";case ALARM_TAILNET_EXPIRED:return "授權已到期";case ALARM_TAILNET_BLOCKED:return "存取規則尚未通過";case ALARM_TAILNET_CONNECTING:return "正在連線";default:return "尚未連線";}
}
bool localNonce(){if(server.arg("nonce")==setupNonce||server.header("X-Setup-Nonce")==setupNonce)return true;server.send(403,"text/plain; charset=utf-8","請從裝置設定頁操作");return false;}
String tailnetDetail(const alarm_tailnet_status_t &t){
 if(!WiFi.isConnected())return "無線網路未連線，請先設定無線網路";
 if(!clockValid())return "裝置尚未校時，請在班表頁校時或確認網際網路連線";
 if(t.capacity_exceeded)return "Tailscale 裝置數超過韌體容量";
 if(t.last_error!=ESP_OK)return String("Tailscale 連線失敗，錯誤碼 ")+String(t.last_error)+"；請檢查網際網路後重新授權";
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
  const char *headers[]={"Authorization","X-Setup-Nonce"}; server.collectHeaders(headers,2);tailnetRoutes();otaRoutes();localCalendarRoutes();
  server.on("/api/clock",HTTP_GET,[]{JsonDocument d;d["ready"]=clockValid();d["last_sync"]=lastNetworkClock.load();d["interval_seconds"]=CLOCK_SYNC_INTERVAL_MS/1000;d["overdue"]=lastNetworkClock.load()?millis()-lastNetworkClockMs.load()>CLOCK_SYNC_INTERVAL_MS+300000:millis()-bootMs>120000;String out;serializeJson(d,out);server.send(200,"application/json",out);});
  server.on("/api/status",HTTP_GET,[]{ JsonDocument d; d["version"]=VERSION;d["backendTransport"]=backend=="http://100.126.226.79:8237"?"tailscale":"unsupported";d["backendHost"]=backend=="http://100.126.226.79:8237"?"100.126.226.79":"";d["localSchedule"]=localSchedule;d["firstConsecutiveOnly"]=firstConsecutiveOnly;d["rotation"]=displayRotation*90;d["screenTimeoutMinutes"]=screenTimeoutMinutes;d["screenBrightness"]=screenBrightness;d["screenAwake"]=screenAwake; d["revision"]=revision;d["clockReady"]=clockValid();d["epoch"]=time(nullptr);d["ringing"]=ringing;d["wifi"]=WiFi.isConnected();d["alarmCount"]=alarms.size();d["sync"]=syncState;String out;serializeJson(d,out);server.send(200,"application/json",out); });
  server.on("/display",HTTP_GET,[]{
    String page=String("<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>裝置設定</title><style>body{font:20px system-ui;padding:24px}button,select{font:20px system-ui;padding:12px;margin:12px 0}</style><h1>裝置設定</h1><a class='primary-link' href='/calendar'>設定班表與鬧鐘</a><h2>螢幕</h2><form method='post' action='/display'><input type='hidden' name='nonce' value='")+setupNonce+"'><label for='rotation'>顯示方向</label><select id='rotation' name='rotation'>";
    for(int r=0;r<4;r++)page+=String("<option value='")+r+"'"+(displayRotation==r?" selected":"")+">"+r*90+"°"+(r==0?"（正常）":r==2?"（上下顛倒）":"")+"</option>";
    page+="</select><label for='screen-timeout'>閒置多久後關閉螢幕</label><select id='screen-timeout' name='screen_timeout'>";
    const uint16_t timeouts[]={0,1,5,15,30,60};
    for(uint16_t minutes:timeouts)page+=String("<option value='")+minutes+"'"+(screenTimeoutMinutes==minutes?" selected":"")+">"+(minutes?String(minutes)+" 分鐘":"永久開啟")+"</option>";
    page+="</select><label for='brightness'>螢幕亮度</label><select id='brightness' name='brightness'>";
    const uint8_t brightnessLevels[]={10,25,40,60,80,100};
    for(uint8_t percent:brightnessLevels)page+=String("<option value='")+percent+"'"+(screenBrightness==percent?" selected":"")+">"+percent+"%</option>";
    page+="</select><p>降低亮度可減少背光耗電與發熱。</p><button>儲存螢幕設定</button></form><p>非響鈴時短按機殼頂部中間按鈕可立即關屏；關屏後按任一按鈕即可喚醒。鬧鐘到點會依設定亮度自動亮屏並正常響鈴，響鈴時按任一按鈕即可停止。</p><h2>時鐘校對</h2><p>固定使用臺北時間（UTC+8）；每三小時自動透過網路校時，重新開機及恢復網路後也會由網路時間服務重試。</p><a href='/clock'>手動調整時鐘</a><p id='clock-status'>正在讀取校時狀態…</p><script>async function clockStatus(){const el=document.querySelector('#clock-status');try{const r=await fetch('/api/clock');if(!r.ok)throw Error();const d=await r.json();el.textContent=d.last_sync?'最近網路校時：'+new Date(d.last_sync*1000).toLocaleString('zh-TW',{timeZone:'Asia/Taipei'})+(d.overdue?'。校時已逾期，請檢查網際網路連線。':'。每三小時自動校對。'):d.overdue?'網路校時尚未成功，請檢查網際網路連線，或到班表頁使用手機校時。':'等待首次網路校時…';}catch(e){el.textContent='無法取得校時狀態，請確認裝置連線。'}}clockStatus();setInterval(clockStatus,10000)</script><h2>韌體更新</h2><p>下載已發布版本，保留舊版供失敗時回復。</p><a href='/update'>檢查裝置更新</a><h2>Tailscale連線</h2><p>登入授權，讓裝置在不同環境仍能同步班表。</p><a href='/tailnet'>設定Tailscale</a><h2>更換無線網路</h2><p>按下後裝置會開啟配網熱點，掃描螢幕條碼即可重新選擇網路。原設定會保留到新網路連線成功。</p><form method='post' action='/wifi/reset'><input type='hidden' name='nonce' value='"+setupNonce+"'><button>重新設定無線網路</button></form><p>無法連上此頁時，可同時按住「＋」與「－」十秒，啟動配網。</p><a href='/'>返回</a>";
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
    apCloseAt=0;showJoinQr=true;startPortal();server.send(200,"text/html; charset=utf-8",devicePage("<h1>配網已啟動</h1><p>請掃描裝置螢幕條碼，加入熱點後重新設定無線網路。</p><a href='/'>開啟配網頁</a>"));
  });
  server.on("/api/schedule",HTTP_POST,[]{if(!authorized())return;if(localSchedule){server.send(409,"text/plain; charset=utf-8","目前使用班表，請從班表頁修改");return;}String err;if(!applySchedule(server.arg("plain"),true,err)){server.send(400,"application/json",String("{\"error\":\"")+err+"\"}");return;}server.send(200,"application/json","{\"ok\":true}");});
  server.on("/api/test",HTTP_POST,[]{if(!authorized())return;startRing("喇叭測試");server.send(200,"application/json","{\"ok\":true}");});
  server.on("/api/stop",HTTP_POST,[]{if(!authorized())return;stopRing(false);server.send(200,"application/json","{\"ok\":true}");});
  server.on("/api/wifi",HTTP_GET,[]{
    JsonDocument d;d["connected"]=WiFi.isConnected();d["connecting"]=connecting;d["failed"]=setupFailed;d["ip"]=WiFi.localIP().toString();d["management_url"]=manageUrl;
    auto list=d["networks"].to<JsonArray>();int n=WiFi.scanComplete();for(int i=0;i<n;i++){auto a=list.add<JsonObject>();a["ssid"]=WiFi.SSID(i);a["rssi"]=WiFi.RSSI(i);}
    String out;serializeJson(d,out);server.send(200,"application/json",out);
  });
  server.on("/",HTTP_GET,[]{
    if(!portal){
      alarm_tailnet_status_t t={};alarm_tailnet_get_status(&t);
      String page=String("<h1>裝置管理</h1><p>本地設定網址：<a href='/'>http://")+WiFi.localIP().toString()+"/</a></p><p>"+tailnetLabel(t.state)+"</p>";
      page+="<a class='primary-link' href='/calendar'>開啟班表與鬧鐘</a>";
      page+="<p>在同一個班表頁上傳圖片或手動選擇日期。</p><a href='/tailnet'>Tailscale 連線與授權</a>";
      page+="<h2>裝置設定</h2><p>調整螢幕方向、更換無線網路，或檢查更新。</p><a href='/display'>開啟裝置設定</a><p class='help'>同時按住「＋」與「－」十秒，也能重新配網。</p>";
      server.send(200,"text/html; charset=utf-8",devicePage(page));return;
    }

    String page=R"HTML(<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>班表鬧鐘配網</title><style>body{font:18px system-ui;max-width:32em;margin:24px}input,select{display:block;box-sizing:border-box;font:18px system-ui;width:100%;margin:8px 0 20px;padding:10px}button{font:18px system-ui;padding:12px}#status{line-height:1.6}</style><h1>班表鬧鐘</h1><p><a href=/display>裝置設定（方向／無線網路）</a></p><p>選擇目前環境的無線網路。配網熱點沒有網際網路，設定時請保持連線。</p><form id="wifi"><input type="hidden" name="nonce" value="SETUP_NONCE"><label>無線網路<select id="networks"><option value="">請選擇網路，或在下方輸入</option></select></label><label>網路名稱<input id="ssid" name="ssid" required maxlength="32" autocomplete="off"></label><label>密碼<input name="password" type="password" maxlength="63" autocomplete="off"></label><button>連線並儲存</button></form><p id="status"></p><script>const statusEl=document.querySelector('#status'),form=document.querySelector('#wifi'),list=document.querySelector('#networks');list.onchange=()=>document.querySelector('#ssid').value=list.value;let populated=false;async function update(){try{const d=await(await fetch('/api/wifi')).json();if(!populated&&d.networks.length){for(const n of d.networks){const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)';list.append(o)}populated=true}if(d.connected&&!d.connecting){statusEl.replaceChildren(document.createTextNode('連線成功，請將手機切回剛設定的無線網路，再開啟：'));const a=document.createElement('a');a.href='http://'+d.ip;a.textContent=a.href;statusEl.append(a)}else if(d.connecting)statusEl.textContent='連線中，請保持此頁開啟…';else if(d.failed)statusEl.textContent='連線失敗，請檢查密碼後重試。';}catch(e){}}form.onsubmit=async(e)=>{e.preventDefault();statusEl.textContent='連線中…';try{const r=await fetch('/setup',{method:'POST',body:new URLSearchParams(new FormData(form))});if(!r.ok)statusEl.textContent=await r.text()}catch(e){statusEl.textContent='請重新連上裝置熱點後再試。'}};update();setInterval(update,2000)</script>)HTML";page.replace("SETUP_NONCE",setupNonce);server.send(200,"text/html; charset=utf-8",devicePage(page));
  });
  server.on("/setup",HTTP_POST,[]{
    if(!portal||server.arg("nonce")!=setupNonce){server.send(403,"text/plain; charset=utf-8","請重新開啟裝置配網頁再試");return;}
    String s=server.arg("ssid"),p=server.arg("password");
    if(!s.length()||s.length()>32||p.length()>63){server.send(400,"text/plain; charset=utf-8","網路名稱或密碼格式無效");return;}
    pendingSsid=s;pendingPassword=p;connecting=true;setupFailed=false;connectStarted=millis();apCloseAt=0;
    WiFi.begin(s.c_str(),p.c_str());server.send(202,"application/json","{\"connecting\":true}");
  });
  server.onNotFound([]{if(portal){server.sendHeader("Location","http://192.168.4.1/");server.send(302,"text/plain","");}else server.send(404,"text/plain; charset=utf-8","找不到此頁面");}); server.begin();deviceRoutesReady=true;
}
void pollBackend() {
  if(!WiFi.isConnected()||backend.isEmpty()||token.isEmpty())return;
  HTTPClient h;h.setConnectTimeout(1500);h.setTimeout(2000);
  if(localSchedule)syncState="班表已儲存";
  else {
    h.begin(backend+"/api/device/schedule");h.addHeader("Authorization",String("Bearer ")+token);
    int code=h.GET(); if(code==200){String err,body;if(!boundedHttpBody(h,MAX_JSON,body,8000))syncState="班表大小無效或傳輸不完整";else if(applySchedule(body,true,err))syncState="班表已同步";else syncState=err;}else syncState=String("同步失敗，回應碼 ")+code;h.end();
  }
  JsonDocument d;d["revision"]=revision;d["status"]=ringing?"ringing":(clockValid()?"ready":"waiting_for_time");d["ip"]=WiFi.localIP().toString();
  int64_t next=INT64_MAX;for(auto &a:alarms)if(alarmclock::upcoming(a.epoch,time(nullptr),handled)&&a.epoch<next)next=a.epoch;
  if(next!=INT64_MAX)d["next_alarm"]=next;
  String body;serializeJson(d,body);h.begin(backend+"/api/device/heartbeat");h.addHeader("Authorization",String("Bearer ")+token);h.addHeader("Content-Type","application/json");h.POST(body);h.end();
}
void setup() {
  rtc_gpio_hold_dis(GPIO_NUM_21);rtc_gpio_init(GPIO_NUM_21);rtc_gpio_set_direction(GPIO_NUM_21,RTC_GPIO_MODE_OUTPUT_ONLY);rtc_gpio_set_level(GPIO_NUM_21,1); // Match the verified upstream board power control.
  Serial.begin(115200);ESP_ERROR_CHECK(nvs_flash_init());if(psramFound())heap_caps_malloc_extmem_enable(4096); esp_err_t nvs=nvs_flash_init_partition("alarm_nvs");ESP_ERROR_CHECK(nvs);if(!prefs.begin("shift-alarm",false,"alarm_nvs")){Serial.println("SETTINGS_STORAGE_FAILED");abort();}
  char nonce[33];snprintf(nonce,sizeof(nonce),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());setupNonce=nonce;lastCommand=prefs.getString("command");
  handled=prefs.getLong64("handled",0);snooze=prefs.getLong64("snooze",0);
  #ifdef PROVISION_BACKEND
  if(prefs.getString("backend").isEmpty()||prefs.getString("token").isEmpty()){
    if(prefs.putString("token",PROVISION_TOKEN)!=strlen(PROVISION_TOKEN)||
       prefs.putString("manage",PROVISION_MANAGE)!=strlen(PROVISION_MANAGE)||
       prefs.putString("backend",PROVISION_BACKEND)!=strlen(PROVISION_BACKEND)||
       prefs.getString("token")!=PROVISION_TOKEN||prefs.getString("backend")!=PROVISION_BACKEND)abort();
  }
#endif
  if(!loadWifiCredentials()){setupFailed=true;}
  backend=prefs.getString("backend");if(backend=="http://192.168.18.31:8237"){backend="http://100.126.226.79:8237";prefs.putString("backend",backend);}token=prefs.getString("token");manageUrl=prefs.getString("manage");
  apPassword=prefs.getString("ap-pass");if(apPassword.isEmpty()){char b[13];snprintf(b,sizeof(b),"%08lx%04lx",(unsigned long)esp_random(),(unsigned long)(esp_random()&0xffff));apPassword=b;prefs.putString("ap-pass",apPassword);}
  char apSuffix[5];snprintf(apSuffix,sizeof(apSuffix),"%04X",(unsigned)(ESP.getEfuseMac()&0xffff));apName=String("ShiftAlarm-")+apSuffix;
  String err;String stored=savedSchedule();savedScheduleRestored=!prefs.isKey("schedule")||(!stored.isEmpty()&&applySchedule(stored,false,err));
  pinMode(BUTTON_STOP,INPUT_PULLUP);pinMode(BUTTON_SNOOZE,INPUT_PULLUP);pinMode(BUTTON_TEST,INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_STOP),stopPressed,FALLING);attachInterrupt(digitalPinToInterrupt(BUTTON_SNOOZE),snoozePressed,FALLING);attachInterrupt(digitalPinToInterrupt(BUTTON_TEST),plusPressed,FALLING);
#if CUBE_TFT
  frame=new GFXcanvas16(240,240);assert(frame && frame->getBuffer());
  SPI.begin(9,-1,10,14);screen.init(240,240);loadDisplaySettings();screen.setRotation(displayRotation);screen.invertDisplay(true);backlightPwm=ledcAttach(13,5000,8);if(!backlightPwm)pinMode(13,OUTPUT);setBacklight(true);screenAwake=true;screenLastActivity=millis();
#else
  Wire.begin(41,42);screen.begin(SSD1306_SWITCHCAPVCC,0x3c);screen.setRotation(2);
#endif
  i2s_config_t cfg={};cfg.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_TX);cfg.sample_rate=24000;cfg.bits_per_sample=I2S_BITS_PER_SAMPLE_16BIT;cfg.channel_format=I2S_CHANNEL_FMT_RIGHT_LEFT;cfg.communication_format=I2S_COMM_FORMAT_STAND_I2S;cfg.intr_alloc_flags=ESP_INTR_FLAG_LEVEL1;cfg.dma_buf_count=6;cfg.dma_buf_len=256;cfg.tx_desc_auto_clear=true;
  i2s_pin_config_t pins={};pins.mck_io_num=I2S_PIN_NO_CHANGE;pins.bck_io_num=15;pins.ws_io_num=16;pins.data_out_num=7;pins.data_in_num=I2S_PIN_NO_CHANGE;
  ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0,&cfg,0,nullptr));ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0,&pins));if(xTaskCreatePinnedToCore(soundTask,"speaker",3072,nullptr,2,nullptr,0)!=pdPASS)abort();
  alarm_ota_config_t otaConfig={};otaConfig.board_id="xingzhi-cube-1.54tft-wifi";otaConfig.device_token=(const uint8_t*)token.c_str();otaConfig.device_token_length=token.length();otaConfig.quiet_window_seconds=300;otaConfig.transfer_timeout_seconds=600;otaConfig.authorize=otaAuthorize;otaConfig.read_guard=otaReadGuard;
  ESP_ERROR_CHECK(alarm_proxy_init("100.126.226.79",8237));
  setenv("TZ","CST-8",1);tzset();WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);if(ssid.length())WiFi.begin(ssid.c_str(),password.c_str());else startPortal();
  esp_sntp_set_time_sync_notification_cb(networkClockSynced);esp_sntp_set_sync_interval(CLOCK_SYNC_INTERVAL_MS);configTime(8*3600,0,"pool.ntp.org","time.google.com");routes();proxyStarted=alarm_proxy_start()==ESP_OK;bootMs=millis();lastPoll=millis()-POLL_MS;draw();
  ESP_ERROR_CHECK(alarm_ota_boot_self_test(otaDiagnostics,nullptr,15000));
  otaReady=alarm_ota_init(&otaConfig)==ESP_OK;
  Serial.printf("SHIFT_ALARM_READY v%s\n",VERSION);Serial.printf("PSRAM_BYTES %lu\n",(unsigned long)ESP.getPsramSize());Serial.println(ssid.length()?"BOOT_WIFI_MODE SAVED":"BOOT_WIFI_MODE FIRST_SETUP");
}
void loop() {
  if(!tailnetStarted&&WiFi.isConnected()&&clockValid()&&millis()-tailnetAttempt>=30000){tailnetAttempt=millis();if(alarm_tailnet_start(apName.c_str())==ESP_OK)tailnetStarted=true;}
  if(!proxyStarted){if(alarm_proxy_start()==ESP_OK)proxyStarted=true;}
  refreshOtaGuard();if(otaReady)alarm_ota_maintenance();
  server.handleClient();if(portal)dns.processNextRequest();uint32_t ms=millis();time_t now=time(nullptr);
  if(connecting&&WiFi.isConnected()&&WiFi.SSID()==pendingSsid) {
    connecting=false;
    if(saveWifiCredentials(pendingSsid,pendingPassword)){
      setupFailed=false;ssid=pendingSsid;password=pendingPassword;apCloseAt=ms+20000;lastPoll=ms-POLL_MS;
    }else{
      setupFailed=true;apCloseAt=0;WiFi.disconnect(false,false);
      // Retain old RAM credentials and the provisioning AP for an explicit retry.
    }
    pendingPassword="";
  }
  if(connecting&&ms-connectStarted>=20000){connecting=false;setupFailed=true;WiFi.disconnect(false,false);}
  if(portal&&apCloseAt&&int32_t(ms-apCloseAt)>=0){portal=false;apCloseAt=0;dns.stop();WiFi.softAPdisconnect(true);WiFi.mode(WIFI_STA);}

  static uint32_t lastButtons=0;static bool ignoreChordUntilRelease=false;
  portENTER_CRITICAL(&buttonMux);uint32_t events=buttonEvents;buttonEvents=0;portEXIT_CRITICAL(&buttonMux);
  if(ms-lastButtons<150)events&=9;else if(events)lastButtons=ms;
  bool rawChord=!digitalRead(BUTTON_TEST)&&!digitalRead(BUTTON_SNOOZE);
  if(!rawChord)ignoreChordUntilRelease=false;
  if(events&&!screenAwake){wakeScreen();events=0;ignoreChordUntilRelease=true;}
  else if(events)screenLastActivity=ms;
  bool chord=rawChord&&!ignoreChordUntilRelease;
  if(chord&&!ringing&&!portal){
    if(!pairingHoldActive&&!pairingTriggered){pairingHoldActive=true;pairingHoldStarted=ms;}
    if(pairingHoldActive&&ms-pairingHoldStarted>=10000){pairingHoldActive=false;pairingTriggered=true;apCloseAt=0;showJoinQr=true;startPortal();}
  } else {pairingHoldActive=false;if(!chord)pairingTriggered=false;}
  if(events&8)stopRing(false);else if(events&1)setScreenAwake(false);else if((events&4)&&!chord){if(portal)showJoinQr=!showJoinQr;}
  if(clockValid()) {
    const int64_t corrected=alarmclock::reconcileHandled(now,handled);
    if(corrected!=handled){handled=corrected;prefs.putLong64("handled",handled);}
    int64_t latest=handled;
    for(auto &alarm:alarms)if(alarmclock::due(alarm.epoch,now,handled)){startRing(alarm.label);latest=std::max(latest,alarm.epoch);}
    if(latest!=handled){handled=latest;prefs.putLong64("handled",handled);}
    if(snooze&&now>=snooze){bool fresh=now-snooze<=alarmclock::CATCHUP_SECONDS;snooze=0;prefs.putLong64("snooze",0);if(fresh)startRing("貪睡提醒");}
  }
  if(ringing&&millis()-ringStarted>=RING_MS)stopRing(false);
  if(screenpolicy::shouldTurnOff(screenTimeoutMinutes,ms,screenLastActivity,ringing||portal||pairingHoldActive))setScreenAwake(false);
  // Saved networks do not reopen setup automatically; require the deliberate chord.
  if(!pairingHoldActive&&ms-lastPoll>=POLL_MS){lastPoll=ms;pollBackend();}
  // Main alone mutates schedules/snooze and selects the next boot image.
  // UI/backend changes above precede the fresh activation guard.
  alarm_ota_handle_t activation=otaActivation.exchange(0);
  if(activation){refreshOtaGuard();if(alarm_ota_activate(activation,&otaSession)!=ESP_OK){
    alarm_ota_abort(activation,&otaSession);otaPowerConfirmed=false;otaBusy=false;
    otaMessageSet("目前不適合重啟，保留原有版本");}}
  if(ms-lastDraw>=200){lastDraw=ms;draw();}
  if(ms-lastSerial>=10000){lastSerial=ms;Serial.printf("STATUS setup=%d wifi=%d clock=%d alarms=%u ringing=%d\n",portal,WiFi.isConnected(),clockValid(),unsigned(alarms.size()),ringing);}
  delay(10);
}
