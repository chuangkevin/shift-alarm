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
#include <time.h>
#include <sys/time.h>
#if __has_include("provisioning.h")
#include "provisioning.h"
#endif
#include <vector>
#include <algorithm>
#include "scheduler.h"

constexpr char VERSION[]="0.1.0";
constexpr size_t MAX_ALARMS=512, MAX_JSON=98304;
constexpr uint32_t POLL_MS=5000, RING_MS=180000;
constexpr int BUTTON_STOP=0, BUTTON_SNOOZE=39, BUTTON_TEST=40;
struct Alarm { String id,label; int64_t epoch; };
std::vector<Alarm> alarms;
Preferences prefs;
WebServer server(80);
DNSServer dns;
String apName,pendingSsid,pendingPassword,setupNonce;
bool connecting=false, setupFailed=false, showJoinQr=true;
uint32_t connectStarted=0,apCloseAt=0;
String lastCommand;
String revision, backend, token, manageUrl, ssid, password, apPassword;
int64_t handled=0,snooze=0;
volatile bool ringing=false;
volatile uint32_t buttonEvents=0,physicalStopDown=0;
portMUX_TYPE buttonMux=portMUX_INITIALIZER_UNLOCKED;
void IRAM_ATTR stopPressed(){portENTER_CRITICAL_ISR(&buttonMux);buttonEvents|=1;physicalStopDown=xTaskGetTickCountFromISR()*portTICK_PERIOD_MS;ringing=false;portEXIT_CRITICAL_ISR(&buttonMux);}
void IRAM_ATTR snoozePressed(){portENTER_CRITICAL_ISR(&buttonMux);buttonEvents|=2;portEXIT_CRITICAL_ISR(&buttonMux);}
void IRAM_ATTR plusPressed(){portENTER_CRITICAL_ISR(&buttonMux);buttonEvents|=4;portEXIT_CRITICAL_ISR(&buttonMux);}
uint32_t ringStarted=0,lastPoll=0,lastDraw=0,lastSerial=0,bootMs=0;
bool portal=false;
String ringLabel="Alarm", syncState="Waiting for WiFi";
#if CUBE_TFT
Adafruit_ST7789 screen(&SPI,14,8,18);
#else
Adafruit_SSD1306 screen(128,64,&Wire,-1);
#endif

String savedSchedule() {
  size_t len=prefs.getBytesLength("schedule");if(!len||len>MAX_JSON)return "";
  std::vector<char> bytes(len+1,0);prefs.getBytes("schedule",bytes.data(),len);return String(bytes.data());
}
bool clockValid() { return time(nullptr)>=alarmclock::VALID_CLOCK; }
String datetime(int64_t epoch) { struct tm t; time_t e=epoch; localtime_r(&e,&t); char b[32]; strftime(b,sizeof(b),"%m/%d %a %H:%M",&t); return b; }
String ascii(String s) { for(unsigned i=0;i<s.length();i++) if((uint8_t)s[i]<32||(uint8_t)s[i]>126)s[i]='?'; return s; }
void fill(uint16_t c) {
#if CUBE_TFT
screen.fillScreen(c);
#else
screen.clearDisplay();
#endif
}
void line(int x,int y,String s,int size=1) { screen.setTextSize(size); screen.setCursor(x,y); screen.print(ascii(s)); }
void draw() {
  fill(0); screen.setTextColor(0xffff);
#if CUBE_TFT
  line(8,6,String("SHIFT ALARM v")+VERSION); line(8,24,clockValid()?datetime(time(nullptr)):"WAITING FOR TIME",2);
  if(ringing) { line(8,58,"ALARM",3); line(8,89,ringLabel.substring(0,28)); line(8,110,"BOOT: Stop   -: Snooze 5m"); }
  else {
    int64_t next=snooze>time(nullptr)?snooze:INT64_MAX;
    String label="Snooze";
    for(auto &a:alarms) if(alarmclock::upcoming(a.epoch,time(nullptr),handled)&&a.epoch<next){next=a.epoch;label=a.label;}
    line(8,52,next==INT64_MAX?"No upcoming alarm":String("Next ")+datetime(next));
    line(8,69,next==INT64_MAX?"Sync a shift schedule":label.substring(0,34));
  }
  String qrText=portal?(showJoinQr?String("WIFI:T:WPA;S:")+apName+";P:"+apPassword+";;":"http://192.168.4.1"):String("http://")+WiFi.localIP().toString();
  if(qrText.length()&&qrText.length()<=180) {
    uint8_t data[qrcode_getBufferSize(8)]; QRCode qr;
    if(qrcode_initText(&qr,data,8,ECC_LOW,qrText.c_str())==0) {
      const int scale=2, x=8,y=130;
      screen.fillRect(x-8,y-8,(qr.size+8)*scale,(qr.size+8)*scale,0xffff);
      for(int row=0;row<qr.size;row++)for(int col=0;col<qr.size;col++)if(qrcode_getModule(&qr,col,row))screen.fillRect(x+col*scale,y+row*scale,scale,scale,0);
    }
  }
  if(portal) { line(115,130,showJoinQr?"1. Scan join WiFi":"2. Scan setup"); line(115,145,apName.substring(0,19)); line(115,160,"Password:"); line(115,174,apPassword);line(115,190,"+: Switch QR");line(115,204,connecting?"Connecting...":setupFailed?"Retry WiFi":"192.168.4.1"); }
  else { line(115,133,WiFi.isConnected()?"WiFi connected":"WiFi offline"); line(115,151,"Scan to manage"); line(115,169,String(alarms.size())+" alarms"); line(115,187,syncState.substring(0,19)); }
  line(115,219,"+: test  BOOT: stop");
#else
  line(0,0,ringing?"ALARM - BOOT:stop":"SHIFT ALARM v0.1.0");
  line(0,12,clockValid()?datetime(time(nullptr)):"Waiting for time");
  int64_t next=INT64_MAX; for(auto &a:alarms)if(alarmclock::upcoming(a.epoch,time(nullptr),handled)&&a.epoch<next)next=a.epoch;
  line(0,26,next==INT64_MAX?"No upcoming alarm":datetime(next));
  line(0,40,portal?"AP: CUBE-Setup":syncState);
  line(0,52,portal?apPassword:WiFi.localIP().toString()); screen.display();
#endif
}
void soundTask(void*) {
  int16_t samples[256*2]; uint32_t phase=0;
  for(;;) {
    bool play=ringing && (millis()%1000)<700;
    for(int i=0;i<256;i++) { int16_t v=play?((phase++%24)<12?5000:-5000):0; samples[i*2]=samples[i*2+1]=v; }
    size_t written; i2s_write(I2S_NUM_0,samples,sizeof(samples),&written,portMAX_DELAY);
  }
}
void startRing(String label) { ringLabel=label; ringStarted=millis(); ringing=true; Serial.println("ALARM_RING_STARTED"); }
void stopRing(bool doSnooze) { ringing=false; snooze=doSnooze&&clockValid()?time(nullptr)+alarmclock::SNOOZE_SECONDS:0; prefs.putLong64("snooze",snooze); Serial.println(doSnooze?"ALARM_SNOOZED":"ALARM_STOPPED"); }
bool applySchedule(const String &body,bool persist,String &error) {
  if(body.length()>MAX_JSON){error="Schedule too large";return false;}
  JsonDocument doc; if(deserializeJson(doc,body)){error="Invalid JSON";return false;}
  if(!doc["revision"].is<String>()||doc["revision"].as<String>().isEmpty()||doc["timezone"]!="Asia/Taipei"||!doc["alarms"].is<JsonArray>()||doc["alarms"].size()>MAX_ALARMS){error="Invalid schedule schema";return false;}
  std::vector<Alarm> next;
  for(JsonObject a:doc["alarms"].as<JsonArray>()) {
    if(!a["id"].is<String>()||a["id"].as<String>().isEmpty()||a["id"].as<String>().length()>128||!a["epoch"].is<int64_t>()||a["epoch"].as<int64_t>()<alarmclock::VALID_CLOCK||!a["label"].is<String>()||a["label"].as<String>().length()>256){error="Invalid alarm";return false;}
    for(auto &b:next)if(b.id==a["id"].as<String>()){error="Duplicate alarm ID";return false;}
    next.push_back({a["id"].as<String>(),a["label"].as<String>(),a["epoch"].as<int64_t>()});
  }
  std::sort(next.begin(),next.end(),[](const Alarm&a,const Alarm&b){return a.epoch<b.epoch;});
  JsonDocument saved;saved["revision"]=doc["revision"];saved["timezone"]=doc["timezone"];saved["alarms"]=doc["alarms"];String canonical;serializeJson(saved,canonical);
  if(persist&&savedSchedule()!=canonical&&prefs.putBytes("schedule",canonical.c_str(),canonical.length())!=canonical.length()){error="Storage failed";return false;}
  alarms=std::move(next);revision=doc["revision"].as<String>();
  // Authenticated LAN server also provides time if outbound NTP is unavailable.
  if(persist && doc["server_time"].is<int64_t>() && doc["server_time"].as<int64_t>()>=alarmclock::VALID_CLOCK) {
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
bool authorized() { if(token.length()&&server.header("Authorization")==String("Bearer ")+token)return true; server.send(401,"application/json","{\"error\":\"Authorization required\"}");return false; }
void startPortal() { if(portal)return; portal=true; WiFi.mode(WIFI_AP_STA); WiFi.softAP(apName.c_str(),apPassword.c_str());dns.start(53,"*",WiFi.softAPIP());WiFi.scanNetworks(true); Serial.println("SETUP_AP_STARTED");Serial.println(String("SETUP_AP_SSID ")+apName);Serial.println("SETUP_URL http://192.168.4.1"); }
void routes() {
  const char *headers[]={"Authorization"}; server.collectHeaders(headers,1);
  server.on("/api/status",HTTP_GET,[]{ JsonDocument d; d["version"]=VERSION; d["revision"]=revision;d["clockReady"]=clockValid();d["epoch"]=time(nullptr);d["ringing"]=ringing;d["wifi"]=WiFi.isConnected();d["alarmCount"]=alarms.size();d["sync"]=syncState;String out;serializeJson(d,out);server.send(200,"application/json",out); });
  server.on("/api/schedule",HTTP_POST,[]{if(!authorized())return;String err;if(!applySchedule(server.arg("plain"),true,err)){server.send(400,"application/json",String("{\"error\":\"")+err+"\"}");return;}server.send(200,"application/json","{\"ok\":true}");});
  server.on("/api/test",HTTP_POST,[]{if(!authorized())return;startRing("Speaker test");server.send(200,"application/json","{\"ok\":true}");});
  server.on("/api/stop",HTTP_POST,[]{if(!authorized())return;stopRing(false);server.send(200,"application/json","{\"ok\":true}");});
  server.on("/api/wifi",HTTP_GET,[]{
    JsonDocument d;d["connected"]=WiFi.isConnected();d["connecting"]=connecting;d["failed"]=setupFailed;d["ip"]=WiFi.localIP().toString();d["management_url"]=manageUrl;
    auto list=d["networks"].to<JsonArray>();int n=WiFi.scanComplete();for(int i=0;i<n;i++){auto a=list.add<JsonObject>();a["ssid"]=WiFi.SSID(i);a["rssi"]=WiFi.RSSI(i);}
    String out;serializeJson(d,out);server.send(200,"application/json",out);
  });
  server.on("/",HTTP_GET,[]{
    if(!portal){server.send(200,"text/html",String("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'><style>body{font:20px system-ui;padding:24px}a{display:block;padding:20px}</style><h1>Shift Alarm v0.1.0</h1><p>")+ (clockValid()?"Clock ready":"Waiting for time")+"</p><a href='"+manageUrl+"'>Manage shift alarms</a><p>Hold BOOT 3 seconds to change WiFi.</p>");return;}
    String page=R"HTML(<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1"><title>Shift Alarm WiFi</title><style>body{font:18px system-ui;max-width:32em;margin:24px}input,select{display:block;box-sizing:border-box;font:18px system-ui;width:100%;margin:8px 0 20px;padding:10px}button{font:18px system-ui;padding:12px}#status{line-height:1.6}</style><h1>Shift Alarm v0.1.0</h1><p>Select your home WiFi. This setup WiFi has no Internet; stay connected while configuring.</p><form id="wifi"><input type="hidden" name="nonce" value="SETUP_NONCE"><label>Network<select id="networks"><option value="">Choose network or type below</option></select></label><label>WiFi name<input id="ssid" name="ssid" required maxlength="32" autocomplete="off"></label><label>Password<input name="password" type="password" maxlength="63" autocomplete="off"></label><button>Connect WiFi</button></form><p id="status"></p><script>const statusEl=document.querySelector('#status'),form=document.querySelector('#wifi'),list=document.querySelector('#networks');list.onchange=()=>document.querySelector('#ssid').value=list.value;let populated=false;async function update(){try{const d=await(await fetch('/api/wifi')).json();if(!populated&&d.networks.length){for(const n of d.networks){const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)';list.append(o)}populated=true}if(d.connected&&!d.connecting){statusEl.replaceChildren(document.createTextNode('Connected. Rejoin your home WiFi, then open: '));const a=document.createElement('a');a.href='http://'+d.ip;a.textContent=a.href;statusEl.append(a)}else if(d.connecting)statusEl.textContent='Connecting. Keep this page open...';else if(d.failed)statusEl.textContent='Connection failed. Check password and try again.';}catch(e){}}form.onsubmit=async(e)=>{e.preventDefault();statusEl.textContent='Connecting...';try{const r=await fetch('/setup',{method:'POST',body:new URLSearchParams(new FormData(form))});if(!r.ok)statusEl.textContent=await r.text()}catch(e){statusEl.textContent='Reconnect to the setup WiFi and retry.'}};update();setInterval(update,2000)</script>)HTML";page.replace("SETUP_NONCE",setupNonce);server.send(200,"text/html",page);
  });
  server.on("/setup",HTTP_POST,[]{
    if(!portal||server.arg("nonce")!=setupNonce){server.send(403,"text/plain","Reopen the physical setup page and retry");return;}
    String s=server.arg("ssid"),p=server.arg("password");
    if(!s.length()||s.length()>32||p.length()>63){server.send(400,"text/plain","Invalid WiFi name/password");return;}
    pendingSsid=s;pendingPassword=p;connecting=true;setupFailed=false;connectStarted=millis();apCloseAt=0;
    WiFi.begin(s.c_str(),p.c_str());server.send(202,"application/json","{\"connecting\":true}");
  });
  server.onNotFound([]{if(portal){server.sendHeader("Location","http://192.168.4.1/");server.send(302,"text/plain","");}else server.send(404,"text/plain","Not found");}); server.begin();
}
void pollBackend() {
  if(!WiFi.isConnected()||backend.isEmpty()||token.isEmpty())return;
  HTTPClient h; h.setConnectTimeout(1500);h.setTimeout(2000);h.begin(backend+"/api/device/schedule");h.addHeader("Authorization",String("Bearer ")+token);
  int code=h.GET(); if(code==200){String err;if(h.getSize()>int(MAX_JSON))syncState="Schedule too large";else if(applySchedule(h.getString(),true,err))syncState="Synced";else syncState=err;}else syncState=String("Sync HTTP ")+code;h.end();
  JsonDocument d;d["revision"]=revision;d["status"]=ringing?"ringing":(clockValid()?"ready":"waiting_for_time");d["ip"]=WiFi.localIP().toString();
  int64_t next=INT64_MAX;for(auto &a:alarms)if(alarmclock::upcoming(a.epoch,time(nullptr),handled)&&a.epoch<next)next=a.epoch;
  if(next!=INT64_MAX)d["next_alarm"]=next;
  String body;serializeJson(d,body);h.begin(backend+"/api/device/heartbeat");h.addHeader("Authorization",String("Bearer ")+token);h.addHeader("Content-Type","application/json");h.POST(body);h.end();
}
void setup() {
  pinMode(21,OUTPUT);digitalWrite(21,HIGH); // Official board power latch.
  Serial.begin(115200);if(psramFound())heap_caps_malloc_extmem_enable(4096); esp_err_t nvs=nvs_flash_init_partition("alarm_nvs");if(nvs==ESP_ERR_NVS_NO_FREE_PAGES||nvs==ESP_ERR_NVS_NEW_VERSION_FOUND){ESP_ERROR_CHECK(nvs_flash_erase_partition("alarm_nvs"));nvs=nvs_flash_init_partition("alarm_nvs");}ESP_ERROR_CHECK(nvs);prefs.begin("shift-alarm",false,"alarm_nvs");
  char nonce[33];snprintf(nonce,sizeof(nonce),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());setupNonce=nonce;lastCommand=prefs.getString("command");
  handled=prefs.getLong64("handled",0);snooze=prefs.getLong64("snooze",0);
  #ifdef PROVISION_BACKEND
  if(!prefs.getString("backend").length()){prefs.putString("backend",PROVISION_BACKEND);prefs.putString("token",PROVISION_TOKEN);prefs.putString("manage",PROVISION_MANAGE);}
#endif
  ssid=prefs.getString("ssid");password=prefs.getString("password");backend=prefs.getString("backend");token=prefs.getString("token");manageUrl=prefs.getString("manage");
  apPassword=prefs.getString("ap-pass");if(apPassword.isEmpty()){char b[13];snprintf(b,sizeof(b),"%08lx%04lx",(unsigned long)esp_random(),(unsigned long)(esp_random()&0xffff));apPassword=b;prefs.putString("ap-pass",apPassword);}
  char apSuffix[5];snprintf(apSuffix,sizeof(apSuffix),"%04X",(unsigned)(ESP.getEfuseMac()&0xffff));apName=String("ShiftAlarm-")+apSuffix;
  String err;applySchedule(savedSchedule(),false,err);
  pinMode(BUTTON_STOP,INPUT_PULLUP);pinMode(BUTTON_SNOOZE,INPUT_PULLUP);pinMode(BUTTON_TEST,INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_STOP),stopPressed,FALLING);attachInterrupt(digitalPinToInterrupt(BUTTON_SNOOZE),snoozePressed,FALLING);attachInterrupt(digitalPinToInterrupt(BUTTON_TEST),plusPressed,FALLING);
#if CUBE_TFT
  SPI.begin(9,-1,10,14);screen.init(240,240);screen.setRotation(0);screen.invertDisplay(true);pinMode(13,OUTPUT);digitalWrite(13,HIGH);
#else
  Wire.begin(41,42);screen.begin(SSD1306_SWITCHCAPVCC,0x3c);screen.setRotation(2);
#endif
  i2s_config_t cfg={};cfg.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_TX);cfg.sample_rate=24000;cfg.bits_per_sample=I2S_BITS_PER_SAMPLE_16BIT;cfg.channel_format=I2S_CHANNEL_FMT_RIGHT_LEFT;cfg.communication_format=I2S_COMM_FORMAT_STAND_I2S;cfg.intr_alloc_flags=ESP_INTR_FLAG_LEVEL1;cfg.dma_buf_count=6;cfg.dma_buf_len=256;cfg.tx_desc_auto_clear=true;
  i2s_pin_config_t pins={};pins.mck_io_num=I2S_PIN_NO_CHANGE;pins.bck_io_num=15;pins.ws_io_num=16;pins.data_out_num=7;pins.data_in_num=I2S_PIN_NO_CHANGE;
  ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0,&cfg,0,nullptr));ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0,&pins));xTaskCreatePinnedToCore(soundTask,"speaker",3072,nullptr,2,nullptr,0);
  setenv("TZ","CST-8",1);tzset();WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);if(ssid.length())WiFi.begin(ssid.c_str(),password.c_str());else startPortal();
  configTime(8*3600,0,"pool.ntp.org","time.google.com");routes();bootMs=millis();lastPoll=millis()-POLL_MS;draw();Serial.println("SHIFT_ALARM_READY v0.1.0");Serial.printf("PSRAM_BYTES %u\n",ESP.getPsramSize());Serial.println(ssid.length()?"BOOT_WIFI_MODE SAVED":"BOOT_WIFI_MODE FIRST_SETUP");
}
void loop() {
  server.handleClient();if(portal)dns.processNextRequest();uint32_t ms=millis();time_t now=time(nullptr);
  if(connecting&&WiFi.isConnected()&&WiFi.SSID()==pendingSsid) { connecting=false;setupFailed=false;ssid=pendingSsid;password=pendingPassword;prefs.putString("ssid",ssid);prefs.putString("password",password);apCloseAt=ms+20000;lastPoll=ms-POLL_MS; }
  if(connecting&&ms-connectStarted>=20000){connecting=false;setupFailed=true;WiFi.disconnect(false,false);}
  if(portal&&apCloseAt&&int32_t(ms-apCloseAt)>=0){portal=false;apCloseAt=0;dns.stop();WiFi.softAPdisconnect(true);WiFi.mode(WIFI_STA);}

  static uint32_t lastButtons=0;
  portENTER_CRITICAL(&buttonMux);uint32_t events=buttonEvents;buttonEvents=0;portEXIT_CRITICAL(&buttonMux);
  if(ms-lastButtons<150)events=0;else if(events)lastButtons=ms;
  bool a=!digitalRead(BUTTON_STOP);
  if(a&&ms-physicalStopDown>3000)startPortal();
  if(events&1)stopRing(false);if((events&2)&&ringing)stopRing(true);if(events&4){if(portal)showJoinQr=!showJoinQr;else if(!ringing)startRing("Speaker test");}
  if(clockValid()) {
    int64_t latest=handled;
    for(auto &alarm:alarms)if(alarmclock::due(alarm.epoch,now,handled)){startRing(alarm.label);latest=std::max(latest,alarm.epoch);}
    if(latest!=handled){handled=latest;prefs.putLong64("handled",handled);}
    if(snooze&&now>=snooze){bool fresh=now-snooze<=alarmclock::CATCHUP_SECONDS;snooze=0;prefs.putLong64("snooze",0);if(fresh)startRing("Snooze");}
  }
  if(ringing&&ms-ringStarted>=RING_MS)stopRing(false);
  if(!portal&&!WiFi.isConnected()&&ms-bootMs>30000)startPortal();
  if(ms-lastPoll>=POLL_MS){lastPoll=ms;pollBackend();}
  if(ms-lastDraw>=1000){lastDraw=ms;draw();}
  if(ms-lastSerial>=10000){lastSerial=ms;Serial.printf("STATUS setup=%d wifi=%d clock=%d alarms=%u ringing=%d\n",portal,WiFi.isConnected(),clockValid(),unsigned(alarms.size()),ringing);}
  delay(10);
}
