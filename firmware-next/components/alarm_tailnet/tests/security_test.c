#include "microlink_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint64_t clock_ms=1000;
uint64_t ml_get_time_ms(void){return clock_ms;}
static void map(microlink_t *m,const char *s){cJSON *j=cJSON_Parse(s);assert(j);ml_security_map(m,j);cJSON_Delete(j);}
static int auth(microlink_t *m,const char *s){cJSON *j=cJSON_Parse(s);assert(j);int r=ml_security_register(m,j);cJSON_Delete(j);return r;}
static void put32(uint8_t *p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static struct pbuf packet(uint8_t h[40],uint32_t src,uint32_t dst,unsigned sport,unsigned dport){
 memset(h,0,40);h[0]=0x45;h[3]=40;h[9]=6;put32(h+12,src);put32(h+16,dst);h[20]=sport>>8;h[21]=sport;h[22]=dport>>8;h[23]=dport;h[32]=0x50;h[33]=0x10;
 return (struct pbuf){40,h};
}
int main(void){
 microlink_t m={.vpn_ip=0x64400001};assert(ml_security_init(&m));
 ml_coord_diag_stage(&m,8);assert(m.security.coord_stage==8&&m.security.coord_stage_since_ms==1000);
 clock_ms=2000;ml_coord_diag_stage(&m,8);assert(m.security.coord_stage_since_ms==1000);
 ml_coord_diag_reconnect(&m,ML_COORD_REASON_POLL);assert(m.security.coord_reconnects==1&&m.security.coord_last_reason==ML_COORD_REASON_POLL&&m.security.coord_last_failure_ms==2000);
 ml_security_close(&m);assert(m.security.coord_last_reason==ML_COORD_REASON_POLL);
 ml_coord_diag_success(&m);assert(m.security.coord_successes==1&&m.security.coord_last_reason==ML_COORD_REASON_POLL);
 ml_coord_diag_stage(&m,9);assert(m.security.coord_stage_since_ms==2000);clock_ms=1000;
 uint8_t h[40];struct pbuf p=packet(h,0x64400002,m.vpn_ip,5000,80);
 assert(!ml_security_packet(&p,false,&m));
 assert(auth(&m,"{\"AuthURL\":\"https://evil.test/login\"}")==-1);
 assert(auth(&m,"{\"AuthURL\":\"https://login.tailscale.com/a/example\"}")==1);
 cJSON *req=cJSON_CreateObject();ml_security_followup(&m,req);assert(cJSON_IsString(cJSON_GetObjectItem(req,"Followup")));cJSON_Delete(req);
 assert(auth(&m,"{\"Error\":\"denied\",\"MachineAuthorized\":true}")==-1);
 assert(auth(&m,"{\"MachineAuthorized\":true}")==0);
 m.security.peers_ready=true;m.security.peer_count=1;m.security.peers[0]=(ml_allowed_peer_t){.ip=0x64400002,.expiry=0};
 /* Inclusive source and destination ranges are tailcfg FilterRule syntax. */
 const char *ranges[]={"100.64.0.2-100.64.0.2","100.64.0.1-100.64.0.2","100.64.0.2-100.64.0.3","100.64.0.3-100.64.0.4","100.64.0.3-100.64.0.1","100.64.0.2-100.64.0.999","100.64.0.2-100.64.0.3x"};
 for(unsigned i=0;i<sizeof(ranges)/sizeof(ranges[0]);i++) {
   char json[320];snprintf(json,sizeof(json),"{\"PacketFilter\":[{\"SrcIPs\":[\"%s\"],\"DstPorts\":[{\"IP\":\"100.64.0.1-100.64.0.1\",\"Ports\":{\"First\":80,\"Last\":80}}]}]}",ranges[i]);
   map(&m,json);assert(ml_security_packet(&p,false,&m)==(i<3));
   if(i>=3){assert(m.security.wg_last_in_drop==7&&m.security.acl_range_count==2&&m.security.acl_rule_count==1);assert(m.security.wg_last_in_src==0x64400002&&m.security.wg_last_in_dst==m.vpn_ip&&m.security.wg_last_in_port==80);}
 }
 map(&m,"{\"PacketFilter\":[]}");assert(!ml_security_packet(&p,false,&m));
 map(&m,"{\"PacketFilter\":[{\"SrcIPs\":[\"100.64.0.2/32\"],\"IPProto\":[6],\"DstPorts\":[{\"IP\":\"100.64.0.1\",\"Ports\":{\"First\":80,\"Last\":80}}]}]}");
 assert(ml_security_packet(&p,false,&m));h[23]=81;assert(!ml_security_packet(&p,false,&m));h[23]=80;
 h[6]=0x20;assert(!ml_security_packet(&p,false,&m));h[6]=0;
 h[15]=3;assert(!ml_security_packet(&p,false,&m));h[15]=2;
 map(&m,"{}");assert(ml_security_packet(&p,false,&m));
 map(&m,"{\"PacketFilters\":{\"base\":null}}");assert(!ml_security_packet(&p,false,&m));
 /* Explicit outbound request admits only its reverse flow; no broad ACK bypass. */
 p=packet(h,m.vpn_ip,0x64400002,43000,8237);assert(ml_security_packet(&p,true,&m));
 p=packet(h,0x64400002,m.vpn_ip,8237,43000);assert(ml_security_packet(&p,false,&m));
 h[23]++;assert(!ml_security_packet(&p,false,&m));h[23]--;
 clock_ms+=120001;assert(!ml_security_packet(&p,false,&m));
 map(&m,"{\"Node\":{\"KeyExpiry\":\"0001-01-01T00:00:00Z\"}}");assert(m.security.expiry==0);
 map(&m,"{\"Node\":{\"KeyExpiry\":\"2024-02-29T00:00:00Z\"}}");assert(m.security.expiry==1709164800);
 map(&m,"{\"Node\":{\"KeyExpiry\":\"2024-13-99T00:00:00Z\"}}");assert(m.security.expiry==1);
 p=packet(h,m.vpn_ip,0x64400002,43000,8237);assert(!ml_security_packet(&p,true,&m));
 map(&m,"{\"Node\":{\"KeyExpiry\":\"0001-01-01T00:00:00Z\",\"Expired\":true}}");assert(!ml_security_packet(&p,true,&m));
 /* Node omitted/null preserves state; an object supplies Go zero values. */
 map(&m,"{\"Node\":{\"MachineAuthorized\":true,\"Expired\":true,\"KeyExpiry\":\"2024-02-29T00:00:00Z\"}}");
 map(&m,"{\"Node\":null}");assert(m.security.authorized&&m.security.expired&&m.security.expiry==1709164800);
 map(&m,"{\"Node\":{\"MachineAuthorized\":true}}");assert(m.security.authorized&&!m.security.expired&&m.security.expiry==0);
 map(&m,"{\"Node\":{}}");assert(!m.security.authorized&&!m.security.expired&&m.security.expiry==0);
 ml_security_destroy(&m);puts("PASS: auth, followup, ACL, deltas, ports, fragments, reply flow, expiry");
}
