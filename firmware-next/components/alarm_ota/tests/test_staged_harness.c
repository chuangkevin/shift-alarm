#include "alarm_ota_policy.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    bool authenticate, target, image, guard, clear, select;
    unsigned calls;
    char order[16];
} fake_t;

static bool step(fake_t *f, bool result, char name) {
    f->order[f->calls++] = name;
    f->order[f->calls] = 0;
    return result;
}
static bool auth(void *p, const alarm_ota_staged_record_t *r) {(void)r;fake_t *f=p;return step(f,f->authenticate,'A');}
static bool target(void *p, const alarm_ota_staged_record_t *r) {(void)r;fake_t *f=p;return step(f,f->target,'T');}
static bool image(void *p, const alarm_ota_staged_record_t *r) {(void)r;fake_t *f=p;return step(f,f->image,'V');}
static bool guard(void *p) {fake_t *f=p;return step(f,f->guard,'G');}
static bool clear(void *p) {fake_t *f=p;return step(f,f->clear,'C');}
static bool select_boot(void *p) {fake_t *f=p;return step(f,f->select,'B');}

static alarm_ota_staged_record_t record(void) {
    alarm_ota_staged_record_t r={.schema=ALARM_OTA_STAGED_SCHEMA,.target_subtype=17,.target_address=0x410000};
    strcpy(r.manifest.board,"xingzhi-cube-1.54tft-wifi");strcpy(r.manifest.version,"1.2.3");r.manifest.size=1024;
    memset(r.manifest.sha256,'a',64);memset(r.manifest.hmac_sha256,'b',64);memset(r.record_hmac_sha256,'c',64);
    return r;
}
static alarm_ota_activation_ops_t ops={auth,target,image,guard,clear,select_boot};

int main(void) {
    /* Fake NVS survives a simulated reboot; fake target/image callbacks stand in
     * for partition-table derivation and full-flash SHA/descriptor verification. */
    alarm_ota_staged_record_t fake_nvs=record();
    alarm_ota_staged_record_t r=fake_nvs;
    assert(memcmp(&r,&fake_nvs,sizeof(r))==0&&alarm_ota_staged_record_shape_valid(&r));
    fake_t f={true,true,true,true,true,true,0,""};
    assert(alarm_ota_run_activation(&r,&ops,&f)==ALARM_OTA_ACTIVATION_OK);
    assert(strcmp(f.order,"AGTVGCB")==0); /* charging before, during verification callback, and before selection */
    f=(fake_t){true,true,true,true,false,true,0,""};
    assert(alarm_ota_run_activation(&r,&ops,&f)==ALARM_OTA_ACTIVATION_CLEAR_FAILED);
    assert(strcmp(f.order,"AGTVGC")==0);
    f=(fake_t){true,true,true,true,true,false,0,""};
    assert(alarm_ota_run_activation(&r,&ops,&f)==ALARM_OTA_ACTIVATION_BOOT_FAILED);
    assert(strcmp(f.order,"AGTVGCB")==0); /* marker is already gone: require a new download */
    f=(fake_t){true,false,true,true,true,true,0,""};
    assert(alarm_ota_run_activation(&r,&ops,&f)==ALARM_OTA_ACTIVATION_TARGET_INVALID);
    assert(strcmp(f.order,"AGT")==0);
    f=(fake_t){true,true,false,true,true,true,0,""};
    assert(alarm_ota_run_activation(&r,&ops,&f)==ALARM_OTA_ACTIVATION_IMAGE_INVALID);
    assert(strcmp(f.order,"AGTV")==0);
    r.record_hmac_sha256[0]='Z';f=(fake_t){true,true,true,true,true,true,0,""};
    assert(alarm_ota_run_activation(&r,&ops,&f)==ALARM_OTA_ACTIVATION_BAD_RECORD&&f.calls==0);
    r=fake_nvs;
    f=(fake_t){true,true,true,false,true,true,0,""};
    assert(alarm_ota_run_activation(&r,&ops,&f)==ALARM_OTA_ACTIVATION_CHARGING_REQUIRED);
    assert(strcmp(f.order,"AG")==0);
    puts("alarm_ota staged reboot/activation ordering checks passed");
}
