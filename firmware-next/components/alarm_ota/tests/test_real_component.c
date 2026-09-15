#include "alarm_ota.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "host_runtime.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char token[]="host-test-device-token";
static bool charging=true,marker_clear_fail=false,marker_load_fail=false,marker_fail_after_clear=false;
static unsigned marker_loads,marker_stores,marker_clears,guard_calls,charging_drop_after;
static char marker_path[512];

static void hex(const unsigned char in[32],char out[65]){static const char h[]="0123456789abcdef";for(unsigned i=0;i<32;i++){out[i*2]=h[in[i]>>4];out[i*2+1]=h[in[i]&15];}out[64]=0;}
static bool auth(const void *request,void *context){(void)context;return request==(void*)1;}
static esp_err_t guard(alarm_ota_guard_t *out,void *context){(void)context;guard_calls++;bool active=charging&&(!charging_drop_after||guard_calls<charging_drop_after);*out=(alarm_ota_guard_t){.clock_valid=true,.schedule_ready=true,.charging_valid=true,.charging=active,.now_epoch=1800000000};return ESP_OK;}
static esp_err_t marker_load(alarm_ota_staged_record_t *out,void *context){(void)context;marker_loads++;if(marker_load_fail||(marker_fail_after_clear&&marker_loads>1))return ESP_FAIL;FILE *f=fopen(marker_path,"rb");if(!f)return ESP_ERR_NOT_FOUND;size_t n=fread(out,1,sizeof(*out),f);fclose(f);return n==sizeof(*out)?ESP_OK:ESP_ERR_INVALID_SIZE;}
static esp_err_t marker_store(const alarm_ota_staged_record_t *r,void *context){(void)context;marker_stores++;FILE *f=fopen(marker_path,"wb");if(!f)return ESP_FAIL;size_t n=fwrite(r,1,sizeof(*r),f);fclose(f);return n==sizeof(*r)?ESP_OK:ESP_FAIL;}
static esp_err_t marker_clear(void *context){(void)context;marker_clears++;if(marker_clear_fail)return ESP_FAIL;if(remove(marker_path)==0)return ESP_OK;FILE *f=fopen(marker_path,"rb");if(!f)return ESP_OK;fclose(f);return ESP_FAIL;}
static alarm_ota_config_t config(void){alarm_ota_config_t c={.board_id="xingzhi-cube-1.54tft-wifi",.device_token=token,.device_token_length=sizeof(token)-1,.quiet_window_seconds=300,.transfer_timeout_seconds=600,.authorize=auth,.read_guard=guard,.marker_load=marker_load,.marker_store=marker_store,.marker_clear=marker_clear};return c;}
static void digest(const void *data,size_t size,char out[65]){unsigned char raw[32];mbedtls_sha256_context c;mbedtls_sha256_init(&c);mbedtls_sha256_starts(&c,0);mbedtls_sha256_update(&c,data,size);mbedtls_sha256_finish(&c,raw);hex(raw,out);}
static void sign_manifest(alarm_ota_manifest_t *m){char canonical[ALARM_OTA_CANONICAL_CAP];unsigned char raw[32];size_t n=alarm_ota_manifest_canonical(m,canonical,sizeof(canonical));assert(n);assert(mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),token,sizeof(token)-1,(unsigned char*)canonical,n,raw)==0);hex(raw,m->hmac_sha256);}
static void make_image(unsigned char image[1024]){memset(image,0x5a,1024);esp_image_header_t header={.magic=ESP_IMAGE_HEADER_MAGIC,.chip_id=ESP_CHIP_ID_ESP32S3};esp_image_segment_header_t segment={0};esp_app_desc_t desc={.magic_word=ESP_APP_DESC_MAGIC_WORD,.version="0.3.12",.project_name="xingzhi-cube-1.54tft-wifi"};memcpy(image,&header,sizeof(header));memcpy(image+sizeof(header),&segment,sizeof(segment));memcpy(image+sizeof(header)+sizeof(segment),&desc,sizeof(desc));}
static alarm_ota_manifest_t manifest_bytes(const unsigned char *image,size_t size){alarm_ota_manifest_t m={.board="xingzhi-cube-1.54tft-wifi",.version="0.3.12",.size=(uint32_t)size};digest(image,size,m.sha256);sign_manifest(&m);return m;}
static alarm_ota_manifest_t manifest(const unsigned char image[1024]){return manifest_bytes(image,1024);}
static void status(alarm_ota_state_t state,bool staged,bool fault){alarm_ota_status_t s={};assert(alarm_ota_get_status(&s)==ESP_OK);assert(s.state==state&&s.staged_valid==staged&&s.marker_fault==fault);}
static void init_load(void){alarm_ota_config_t c=config();assert(alarm_ota_init(&c)==ESP_OK);}
static void assert_flash_present(void){char path[512];snprintf(path,sizeof(path),"%s/flash.bin",host_test_dir());FILE *f=fopen(path,"rb");assert(f);assert(fseek(f,0,SEEK_END)==0&&ftell(f)==1024);fclose(f);}
static void corrupt_flash(void){char path[512];snprintf(path,sizeof(path),"%s/flash.bin",host_test_dir());FILE *f=fopen(path,"r+b");assert(f);assert(fseek(f,900,SEEK_SET)==0);unsigned char value=0;assert(fwrite(&value,1,1,f)==1);fclose(f);}

int main(int argc,char **argv){assert(argc>=2);snprintf(marker_path,sizeof(marker_path),"%s/marker.bin",host_test_dir());const char *mode=argv[1];unsigned char image[1024];make_image(image);alarm_ota_manifest_t m=manifest(image);init_load();
 if(!strcmp(mode,"stage")){assert(alarm_ota_load_staged()==ESP_OK);alarm_ota_handle_t h=0;assert(alarm_ota_begin(&m,(void*)1,&h)==ESP_OK);assert(alarm_ota_write(h,(void*)1,image,400)==ESP_OK);assert(alarm_ota_write(h,(void*)1,image+400,624)==ESP_OK);assert(alarm_ota_finish(h,(void*)1)==ESP_OK);status(ALARM_OTA_STAGED,true,false);}
 else if(!strcmp(mode,"install")){assert(alarm_ota_load_staged()==ESP_OK);status(ALARM_OTA_STAGED,true,false);assert(alarm_ota_activate((void*)1)==ESP_FAIL);assert(host_boot_selected&&host_restarted);status(ALARM_OTA_IDLE,false,false);}
 else if(!strcmp(mode,"store-fault")){assert(alarm_ota_load_staged()==ESP_OK);alarm_ota_handle_t h=0;assert(alarm_ota_begin(&m,(void*)1,&h)==ESP_OK);assert(alarm_ota_write(h,(void*)1,image,1024)==ESP_OK);marker_load_fail=true;marker_clear_fail=true;assert(alarm_ota_finish(h,(void*)1)==ALARM_OTA_ERR_MARKER);status(ALARM_OTA_MARKER_FAULT,false,true);assert_flash_present();assert(alarm_ota_begin(&m,(void*)1,&h)==ALARM_OTA_ERR_MARKER);assert(alarm_ota_discard_staged((void*)1)!=ESP_OK);status(ALARM_OTA_MARKER_FAULT,false,true);}
 else if(!strcmp(mode,"recover")){assert(alarm_ota_load_staged()==ESP_OK);status(ALARM_OTA_STAGED,true,false);assert_flash_present();}
 else if(!strcmp(mode,"observe-recover")){marker_load_fail=true;assert(alarm_ota_load_staged()==ALARM_OTA_ERR_MARKER);status(ALARM_OTA_MARKER_FAULT,false,true);marker_load_fail=false;assert(alarm_ota_load_staged()==ESP_OK);status(ALARM_OTA_STAGED,true,false);assert(marker_stores==0&&marker_clears==0);}
 else if(!strcmp(mode,"observe-absent")){marker_load_fail=true;assert(alarm_ota_load_staged()==ALARM_OTA_ERR_MARKER);marker_load_fail=false;assert(alarm_ota_load_staged()==ESP_OK);status(ALARM_OTA_IDLE,false,false);assert(marker_stores==0&&marker_clears==0);}
 else if(!strcmp(mode,"malformed-clear-fail")){FILE *f=fopen(marker_path,"wb");assert(f);alarm_ota_staged_record_t bad={0};assert(fwrite(&bad,1,sizeof(bad),f)==sizeof(bad));fclose(f);marker_clear_fail=true;assert(alarm_ota_load_staged()==ALARM_OTA_ERR_MARKER);assert(alarm_ota_load_staged()==ALARM_OTA_ERR_MARKER);status(ALARM_OTA_MARKER_FAULT,false,true);assert(marker_stores==0&&marker_clears==0);}
 else if(!strcmp(mode,"discard-confirm-fail")){assert(alarm_ota_load_staged()==ESP_OK);marker_fail_after_clear=true;assert(alarm_ota_discard_staged((void*)1)!=ESP_OK);status(ALARM_OTA_MARKER_FAULT,false,true);}
 else if(!strcmp(mode,"discard-success")){assert(alarm_ota_load_staged()==ESP_OK);assert(alarm_ota_discard_staged((void*)1)==ESP_OK);status(ALARM_OTA_IDLE,false,false);}
 else if(!strcmp(mode,"flash-fail")){assert(alarm_ota_load_staged()==ESP_OK);host_flash_read_fail=true;assert(alarm_ota_activate((void*)1)==ALARM_OTA_ERR_MARKER);status(ALARM_OTA_MARKER_FAULT,false,true);}
 else if(!strcmp(mode,"sha-mismatch")){assert(alarm_ota_load_staged()==ESP_OK);corrupt_flash();assert(alarm_ota_activate((void*)1)==ALARM_OTA_ERR_MARKER);status(ALARM_OTA_MARKER_FAULT,false,true);}
 else if(!strcmp(mode,"descriptor-fail")){assert(alarm_ota_load_staged()==ESP_OK);host_descriptor_fail=true;assert(alarm_ota_activate((void*)1)==ALARM_OTA_ERR_MARKER);status(ALARM_OTA_MARKER_FAULT,false,true);}
 else if(!strcmp(mode,"target-fail")){host_target_mismatch=true;assert(alarm_ota_load_staged()==ALARM_OTA_ERR_MARKER);status(ALARM_OTA_MARKER_FAULT,false,true);assert(marker_stores==0&&marker_clears==0);}
 else if(!strcmp(mode,"target-activation-fail")){assert(alarm_ota_load_staged()==ESP_OK);host_target_mismatch=true;assert(alarm_ota_activate((void*)1)==ALARM_OTA_ERR_MARKER);status(ALARM_OTA_MARKER_FAULT,false,true);}
 else if(!strcmp(mode,"charging-drop")){assert(alarm_ota_load_staged()==ESP_OK);charging_drop_after=2;assert(alarm_ota_activate((void*)1)==ALARM_OTA_ERR_CHARGING_REQUIRED);status(ALARM_OTA_STAGED,true,false);}
 else if(!strcmp(mode,"boot-fail")){assert(alarm_ota_load_staged()==ESP_OK);setenv("ALARM_OTA_BOOT_FAIL","1",1);assert(alarm_ota_activate((void*)1)==ESP_FAIL);assert(!host_boot_selected);status(ALARM_OTA_IDLE,false,false);alarm_ota_handle_t h=0;assert(alarm_ota_begin(&m,(void*)1,&h)==ESP_OK);}
 else if(!strcmp(mode,"real-image")){assert(argc==3);FILE *f=fopen(argv[2],"rb");assert(f&&fseek(f,0,SEEK_END)==0);long length=ftell(f);assert(length>288&&length<=0x400000&&fseek(f,0,SEEK_SET)==0);unsigned char *bytes=malloc((size_t)length);assert(bytes&&fread(bytes,1,(size_t)length,f)==(size_t)length);fclose(f);alarm_ota_manifest_t real=manifest_bytes(bytes,(size_t)length);assert(alarm_ota_load_staged()==ESP_OK);alarm_ota_handle_t h=0;assert(alarm_ota_begin(&real,(void*)1,&h)==ESP_OK);for(size_t offset=0;offset<(size_t)length;){size_t chunk=(size_t)length-offset;if(chunk>ALARM_OTA_MAX_CHUNK)chunk=ALARM_OTA_MAX_CHUNK;assert(alarm_ota_write(h,(void*)1,bytes+offset,chunk)==ESP_OK);offset+=chunk;}assert(alarm_ota_finish(h,(void*)1)==ESP_OK);assert(alarm_ota_activate((void*)1)==ESP_FAIL);assert(host_boot_selected&&host_restarted);free(bytes);}
 else if(!strcmp(mode,"crypto-vector")){char out[65];digest("abc",3,out);assert(!strcmp(out,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));unsigned char raw[32];assert(mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),(unsigned char*)"key",3,(unsigned char*)"The quick brown fox jumps over the lazy dog",43,raw)==0);hex(raw,out);assert(!strcmp(out,"f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"));}
 else assert(!"unknown mode");
 puts(mode);return 0;
}
