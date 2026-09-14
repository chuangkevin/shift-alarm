#include "alarm_ota_policy.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static alarm_ota_manifest_t good(void) {
    alarm_ota_manifest_t m = {.board="xingzhi-cube-1.54tft-wifi", .version="1.2.3", .size=1024};
    memset(m.sha256, 'a', 64); memset(m.hmac_sha256, 'b', 64); return m;
}

int main(void) {
    uint32_t v[3]; uint8_t a[32], b[32];
    assert(alarm_ota_parse_version("1.2.3", 6, v)); assert(v[0]==1 && v[1]==2 && v[2]==3);
    const char *bad[]={"1.2","1.2.3.4","01.2.3","1.02.3","1.2.03","v1.2.3","1.2.3-rc1","1.2.-3","1.2.4294967296","1.2.3\n",""};
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) assert(!alarm_ota_parse_version(bad[i],strlen(bad[i])+1,v));
    char unterminated[32];memset(unterminated,'1',sizeof(unterminated));assert(!alarm_ota_parse_version(unterminated,32,v));
    assert(alarm_ota_parse_version("4294967295.0.0",15,v));assert(v[0]==UINT32_MAX);
    alarm_ota_manifest_t m=good();
    assert(alarm_ota_check_manifest(&m,m.board,"1.2.2",4096,288)==ALARM_OTA_POLICY_OK);
    assert(alarm_ota_check_manifest(&m,m.board,"1.2.3",4096,288)==ALARM_OTA_POLICY_VERSION_REJECTED);
    assert(alarm_ota_check_manifest(&m,m.board,"1.3.0",4096,288)==ALARM_OTA_POLICY_VERSION_REJECTED);
    assert(alarm_ota_check_manifest(&m,"another-board","1.2.2",4096,288)==ALARM_OTA_POLICY_BOARD_MISMATCH);
    assert(alarm_ota_check_manifest(&m,m.board,"1.2.2",512,288)==ALARM_OTA_POLICY_SIZE_REJECTED);
    m.size=0;assert(alarm_ota_check_manifest(&m,m.board,"1.2.2",4096,288)==ALARM_OTA_POLICY_SIZE_REJECTED);
    m=good();m.size=287;assert(alarm_ota_check_manifest(&m,m.board,"1.2.2",4096,288)==ALARM_OTA_POLICY_SIZE_REJECTED);
    m=good();m.sha256[0]='A';assert(alarm_ota_check_manifest(&m,m.board,"1.2.2",4096,288)==ALARM_OTA_POLICY_BAD_MANIFEST);
    m=good();m.board[2]='\n';assert(alarm_ota_check_manifest(&m,"board","1.2.2",4096,288)==ALARM_OTA_POLICY_BAD_MANIFEST);
    m=good();m.hmac_sha256[64]='0';assert(alarm_ota_check_manifest(&m,m.board,"1.2.2",4096,288)==ALARM_OTA_POLICY_BAD_MANIFEST);
    m=good();assert(alarm_ota_decode_digest(m.sha256,a));memcpy(b,a,32);assert(alarm_ota_digest_equal(a,b));
    for(unsigned i=0;i<32;i++){b[i]^=1;assert(!alarm_ota_digest_equal(a,b));b[i]^=1;}
    char canonical[ALARM_OTA_CANONICAL_CAP];size_t n=alarm_ota_manifest_canonical(&m,canonical,sizeof(canonical));
    assert(n==strlen(canonical));assert(strstr(canonical,"\n1.2.3\n1024\n")!=NULL);assert(canonical[n-1]=='\n');
    char small[8];assert(alarm_ota_manifest_canonical(&m,small,sizeof(small))==0);
    alarm_ota_guard_t g={.clock_valid=true,.schedule_ready=true,.charging_valid=true,.charging=true,.now_epoch=1800000000};
    assert(alarm_ota_check_guard(&g,300)==ALARM_OTA_POLICY_OK);
    g.next_alarm_epoch=g.now_epoch+301;assert(alarm_ota_check_guard(&g,300)==ALARM_OTA_POLICY_OK);
    g.next_alarm_epoch--;assert(alarm_ota_check_guard(&g,300)==ALARM_OTA_POLICY_ALARM_NEAR);
    g.next_alarm_epoch=g.now_epoch-1;assert(alarm_ota_check_guard(&g,300)==ALARM_OTA_POLICY_ALARM_NEAR);
    g.next_alarm_epoch=INT64_MAX;assert(alarm_ota_check_guard(&g,300)==ALARM_OTA_POLICY_OK);
    g.now_epoch=INT64_MAX;assert(alarm_ota_check_guard(&g,300)==ALARM_OTA_POLICY_ALARM_NEAR);
    g.next_alarm_epoch=-1;assert(alarm_ota_check_guard(&g,300)==ALARM_OTA_POLICY_CLOCK_UNTRUSTED);
    g.next_alarm_epoch=0;g.ringing=true;assert(alarm_ota_check_guard(&g,300)==ALARM_OTA_POLICY_ALARM_ACTIVE);
    g.ringing=false;g.snoozed=true;assert(alarm_ota_check_guard(&g,300)==ALARM_OTA_POLICY_ALARM_ACTIVE);
    g.snoozed=false;g.clock_valid=false;assert(alarm_ota_check_guard(&g,300)==ALARM_OTA_POLICY_CLOCK_UNTRUSTED);
    g.clock_valid=true;g.schedule_ready=false;assert(alarm_ota_check_guard(&g,300)==ALARM_OTA_POLICY_CLOCK_UNTRUSTED);
    g.schedule_ready=true;g.charging=false;assert(alarm_ota_check_guard(&g,300)==ALARM_OTA_POLICY_CHARGING_REQUIRED);
    g.charging=true;g.charging_valid=false;assert(alarm_ota_check_guard(&g,300)==ALARM_OTA_POLICY_CHARGING_REQUIRED);
    alarm_ota_staged_record_t staged={.schema=ALARM_OTA_STAGED_SCHEMA,.manifest=good(),.target_subtype=17,.target_address=0x410000};
    memset(staged.record_hmac_sha256,'c',64);
    char staged_canonical[ALARM_OTA_STAGED_CANONICAL_CAP];
    size_t staged_n=alarm_ota_staged_canonical(&staged,staged_canonical,sizeof(staged_canonical));
    assert(staged_n==strlen(staged_canonical));
    assert(strstr(staged_canonical,"1\nxingzhi-cube-1.54tft-wifi\n1.2.3\n1024\n")!=NULL);
    assert(strstr(staged_canonical,"\n17\n4259840\n")!=NULL);
    assert(alarm_ota_staged_record_shape_valid(&staged));
    staged.schema++;assert(!alarm_ota_staged_record_shape_valid(&staged));staged.schema=ALARM_OTA_STAGED_SCHEMA;
    staged.target_address++;assert(!alarm_ota_staged_record_shape_valid(&staged));
    assert(alarm_ota_chunk_fits(0,1024,1024));assert(alarm_ota_chunk_fits(1000,24,1024));
    assert(!alarm_ota_chunk_fits(1024,1,1024));assert(!alarm_ota_chunk_fits(0,0,1024));
    assert(!alarm_ota_chunk_fits(1025,1,1024));assert(!alarm_ota_chunk_fits(UINT32_MAX,SIZE_MAX,UINT32_MAX));
    puts("alarm_ota policy checks passed");return 0;
}
