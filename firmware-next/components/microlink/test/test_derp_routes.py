"""Exercise exact production destination selection, including live-session hints."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/ml_derp.c').read_text()
def fn(start):
 a=s.index(start);return s[a:s.index('\n}',a)+2]
stubs=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "ml_derp_cache.h"
#define ML_DERP_REGION 9
#define portMAX_DELAY 0
static uint64_t tick=100000;
static uint64_t ml_get_time_ms(void){return tick;}
static void xSemaphoreTake(int lock,int t){(void)lock;(void)t;}
static void xSemaphoreGive(int lock){(void)lock;}
typedef struct {uint8_t public_key[32];uint16_t derp_region,derp_recv_region;uint64_t derp_recv_ms;} ml_allowed_peer_t;
typedef struct {bool connected;uint16_t region;} ml_derp_conn_t;
typedef struct {uint16_t derp_home_region;ml_derp_cache_slot_t derp_cache[2];ml_derp_conn_t derp,derp_remote[2];struct {int lock;unsigned peer_count;ml_allowed_peer_t peers[2];} security;} microlink_t;
static int cleanup_count;
static void derp_tls_cleanup(microlink_t *ml,ml_derp_conn_t *c){(void)ml;c->connected=false;cleanup_count++;}
'''
# Compile the actual owner-loop home transition block, not a translated model.
a=s.index('        uint16_t home=derp_home_region(ml);');b=s.index('        if(bits&ML_EVT_DERP_CONNECT_REQ)',a)
home_step='static void home_step(microlink_t *ml,ml_derp_cache_slot_t *retry){ml_derp_cache_slot_t home_retry=*retry;'+s[a:b]+'*retry=home_retry;}\n'
main=r'''
int main(void){microlink_t ml={0};uint8_t key[32]={1},unknown[32]={2};ml.derp.connected=true;ml.derp.region=9;ml.security.peer_count=1;
 ml_allowed_peer_t *p=&ml.security.peers[0];memcpy(p->public_key,key,32);p->derp_region=20;
 assert(derp_destination_region(&ml,key)==20); // DFW home -> HKG destination
 assert(derp_destination_region(&ml,unknown)==0);
 p->derp_recv_region=9;p->derp_recv_ms=tick;assert(derp_destination_region(&ml,key)==9);
 ml.derp.connected=false;assert(derp_destination_region(&ml,key)==20);
 ml.derp.connected=true;tick+=30000;assert(derp_destination_region(&ml,key)==20);
 p->derp_recv_ms=tick+1;assert(derp_destination_region(&ml,key)==20);
 p->derp_recv_region=3;p->derp_recv_ms=tick;ml.derp_remote[0].region=3;ml.derp_remote[0].connected=true;assert(derp_destination_region(&ml,key)==3);
 ml.derp_remote[0].connected=false;assert(derp_destination_region(&ml,key)==20);
 ml.derp_home_region=20;ml.derp.region=9;ml.derp.connected=true;
 ml.derp_remote[0].region=20;ml.derp_remote[0].connected=true;ml.derp_cache[0].region=20;
 ml.derp_remote[1].region=3;ml.derp_remote[1].connected=true;ml.derp_cache[1].region=3;
 ml_derp_cache_slot_t retry={.region=9,.failures=5,.retry_after_ms=999999};
 home_step(&ml,&retry);assert(retry.region==20&&retry.failures==0&&retry.retry_after_ms==0);
 assert(!ml.derp.connected&&!ml.derp_remote[0].connected&&ml.derp_cache[0].region==0);
 assert(ml.derp_remote[1].connected&&ml.derp_cache[1].region==3&&cleanup_count==2);
 home_step(&ml,&retry);assert(cleanup_count==2);
 puts("actual DERP destination: DFW/HKG, home/remote hint, expiry, future/closed/unknown rejection PASS");}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';exe=Path(d)/'test';p.write_text(stubs+fn('static uint16_t derp_home_region(')+home_step+fn('static bool derp_region_is_live(')+fn('static uint16_t derp_destination_region(')+main)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-I'+str(root/'include'),str(p),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
