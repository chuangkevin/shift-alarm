#include "host_runtime.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ALARM_OTA_USE_OPENSSL
#include <openssl/hmac.h>
#endif

bool host_boot_selected=false,host_restarted=false,host_flash_read_fail=false;
bool host_descriptor_fail=false,host_target_mismatch=false;
static esp_partition_t running={0x10000,0x400000,ESP_PARTITION_TYPE_APP,16};
static esp_partition_t target={0x410000,0x400000,ESP_PARTITION_TYPE_APP,17};
static esp_app_desc_t current={.magic_word=ESP_APP_DESC_MAGIC_WORD,.version="0.3.9",.project_name="xingzhi-cube-1.54tft-wifi"};
static size_t write_offset;

const char *host_test_dir(void){const char *p=getenv("ALARM_OTA_TEST_DIR");return p?p:"/tmp";}
static void path(char out[512],const char *name){snprintf(out,512,"%s/%s",host_test_dir(),name);}
const esp_app_desc_t *esp_app_get_description(void){return &current;}
const esp_partition_t *esp_ota_get_running_partition(void){return &running;}
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *unused){(void)unused;if(host_target_mismatch)target.address=0x810000;else target.address=0x410000;return &target;}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p,esp_ota_img_states_t *s){(void)p;(void)s;return ESP_ERR_NOT_FOUND;}
esp_err_t esp_ota_begin(const esp_partition_t *p,size_t size,esp_ota_handle_t *h){(void)p;(void)size;char f[512];path(f,"flash.bin");FILE *fp=fopen(f,"wb");if(!fp)return ESP_FAIL;fclose(fp);write_offset=0;*h=1;return ESP_OK;}
esp_err_t esp_ota_write(esp_ota_handle_t h,const void *data,size_t size){(void)h;char f[512];path(f,"flash.bin");FILE *fp=fopen(f,"ab");if(!fp)return ESP_FAIL;size_t n=fwrite(data,1,size,fp);fclose(fp);write_offset+=n;return n==size?ESP_OK:ESP_FAIL;}
esp_err_t esp_ota_end(esp_ota_handle_t h){(void)h;return write_offset?ESP_OK:ESP_FAIL;}
esp_err_t esp_ota_abort(esp_ota_handle_t h){(void)h;return ESP_OK;}
esp_err_t esp_partition_read(const esp_partition_t *p,size_t offset,void *out,size_t size){(void)p;if(host_flash_read_fail)return ESP_FAIL;char f[512];path(f,"flash.bin");FILE *fp=fopen(f,"rb");if(!fp)return ESP_FAIL;if(fseek(fp,(long)offset,SEEK_SET)){fclose(fp);return ESP_FAIL;}size_t n=fread(out,1,size,fp);fclose(fp);return n==size?ESP_OK:ESP_FAIL;}
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p,esp_app_desc_t *out){(void)p;if(host_descriptor_fail)return ESP_FAIL;char f[512];path(f,"flash.bin");FILE *fp=fopen(f,"rb");if(!fp)return ESP_FAIL;if(fseek(fp,(long)(sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)),SEEK_SET)){fclose(fp);return ESP_FAIL;}size_t n=fread(out,1,sizeof(*out),fp);fclose(fp);return n==sizeof(*out)?ESP_OK:ESP_FAIL;}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p){(void)p;if(getenv("ALARM_OTA_BOOT_FAIL"))return ESP_FAIL;host_boot_selected=true;return ESP_OK;}
void esp_restart(void){host_restarted=true;}
int64_t esp_timer_get_time(void){static int64_t now;return ++now;}
esp_err_t esp_timer_create(const esp_timer_create_args_t *a,esp_timer_handle_t *o){(void)a;*o=(void*)1;return ESP_OK;}
esp_err_t esp_timer_start_once(esp_timer_handle_t t,uint64_t u){(void)t;(void)u;return ESP_OK;}
esp_err_t esp_timer_stop(esp_timer_handle_t t){(void)t;return ESP_OK;}
esp_err_t esp_timer_delete(esp_timer_handle_t t){(void)t;return ESP_OK;}
esp_err_t esp_task_wdt_add_user(const char *n,esp_task_wdt_user_handle_t *o){(void)n;*o=(void*)1;return ESP_OK;}
esp_err_t esp_task_wdt_delete_user(esp_task_wdt_user_handle_t u){(void)u;return ESP_OK;}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void){return ESP_OK;}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void){return ESP_OK;}

#ifdef ALARM_OTA_USE_OPENSSL
void mbedtls_sha256_init(mbedtls_sha256_context *c){c->ctx=NULL;}
void mbedtls_sha256_free(mbedtls_sha256_context *c){EVP_MD_CTX_free(c->ctx);c->ctx=NULL;}
int mbedtls_sha256_starts(mbedtls_sha256_context *c,int is224){if(is224)return -1;EVP_MD_CTX_free(c->ctx);c->ctx=EVP_MD_CTX_new();return c->ctx&&EVP_DigestInit_ex(c->ctx,EVP_sha256(),NULL)==1?0:-1;}
int mbedtls_sha256_update(mbedtls_sha256_context *c,const unsigned char *d,size_t n){return EVP_DigestUpdate(c->ctx,d,n)==1?0:-1;}
int mbedtls_sha256_finish(mbedtls_sha256_context *c,unsigned char out[32]){unsigned n=0;return EVP_DigestFinal_ex(c->ctx,out,&n)==1&&n==32?0:-1;}
#else
void mbedtls_sha256_init(mbedtls_sha256_context *c){memset(c,0,sizeof(*c));}
void mbedtls_sha256_free(mbedtls_sha256_context *c){memset(c,0,sizeof(*c));}
int mbedtls_sha256_starts(mbedtls_sha256_context *c,int is224){(void)is224;for(unsigned i=0;i<8;i++)c->state[i]=2166136261u^(i*0x9e3779b9u);c->length=0;return 0;}
int mbedtls_sha256_update(mbedtls_sha256_context *c,const unsigned char *d,size_t n){for(size_t j=0;j<n;j++){for(unsigned i=0;i<8;i++){c->state[i]^=(uint32_t)d[j]+i+(uint32_t)c->length;c->state[i]*=16777619u+(i*2u);}c->length++;}return 0;}
int mbedtls_sha256_finish(mbedtls_sha256_context *c,unsigned char out[32]){for(unsigned i=0;i<8;i++){uint32_t v=c->state[i]^(uint32_t)c->length;out[i*4]=v>>24;out[i*4+1]=v>>16;out[i*4+2]=v>>8;out[i*4+3]=v;}return 0;}
#endif
static const mbedtls_md_info_t md={MBEDTLS_MD_SHA256};
const mbedtls_md_info_t *mbedtls_md_info_from_type(int type){return type==MBEDTLS_MD_SHA256?&md:NULL;}
int mbedtls_md_hmac(const mbedtls_md_info_t *info,const unsigned char *key,size_t key_len,const unsigned char *input,size_t input_len,unsigned char out[32]){
#ifdef ALARM_OTA_USE_OPENSSL
    (void)info;unsigned n=0;return HMAC(EVP_sha256(),key,(int)key_len,input,input_len,out,&n)&&n==32?0:-1;
#else
    (void)info;mbedtls_sha256_context c;mbedtls_sha256_init(&c);mbedtls_sha256_starts(&c,0);mbedtls_sha256_update(&c,key,key_len);mbedtls_sha256_update(&c,input,input_len);mbedtls_sha256_update(&c,key,key_len);return mbedtls_sha256_finish(&c,out);
#endif
}
