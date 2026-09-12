"""Compile the actual IDF getter + complete DHCP callback, with event/IP doubles.
Unsafe SDK config must trip UBSan on custom driver state; separated client_data
must ignore that state while continuing to deliver normal ESP-netif IP events.
"""
import os
from pathlib import Path
import subprocess
import tempfile
idf=Path(os.environ['IDF_PATH'])
s=(idf/'components/esp_netif/lwip/esp_netif_lwip.c').read_text()
a=s.index('static inline esp_netif_t* lwip_get_esp_netif(')
getter=s[a:s.index('\nstatic inline void lwip_set_esp_netif',a)]
a=s.index('static void esp_netif_internal_dhcpc_cb(struct netif *netif)\n{')
callback=s[a:s.index('\nstatic void esp_netif_ip_lost_timer',a)]
opts=(idf/'components/lwip/port/include/lwipopts.h').read_text()
a=opts.index('#if defined(CONFIG_ESP_NETIF_BRIDGE_EN) || defined(CONFIG_LWIP_PPP_SUPPORT)')
selection=opts[a:opts.index('#define LWIP_NUM_NETIF_CLIENT_DATA',a)]
stubs=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef struct {uint32_t addr;} ip4_addr_t;
static const ip4_addr_t zero;
#define IP4_ADDR_ANY4 (&zero)
#define ip_2_ip4(x) (x)
#define ip4_addr_cmp(a,b) ((a)->addr==(b)->addr)
#define ip4_addr_set(a,b) (*(a)=*(b))
typedef struct {ip4_addr_t ip,netmask,gw;} esp_netif_ip_info_t;
typedef struct {esp_netif_ip_info_t *ip_info,*ip_info_old;} esp_netif_t;
struct netif {void *state,*client_data[1];ip4_addr_t ip_addr,netmask,gw;};
#define netif_get_client_data(n,id) ((n)->client_data[(id)])
static const unsigned lwip_netif_client_id=0;
#define ESP_LOGD(...)
#define ESP_LOGE(...)
#define _IS_NETIF_ANY_POINT2POINT_TYPE(n) 0
#define ESP_NETIF_IP_EVENT_GOT_IP 1
#define ESP_NETIF_GOT_IP 1
#define IP_EVENT 1
#define ESP_OK 0
typedef int ip_event_t;
typedef struct {esp_netif_t *esp_netif;bool ip_changed;esp_netif_ip_info_t ip_info;} ip_event_got_ip_t;
static unsigned posts,defaults,lost;
static int esp_netif_get_event_id(esp_netif_t*n,int e){(void)n;return e;}
static void esp_netif_update_default_netif(esp_netif_t*n,int e){(void)n;(void)e;defaults++;}
static int esp_event_post(int e,int id,void*p,size_t n,int t){(void)e;(void)id;(void)p;(void)n;(void)t;posts++;return 0;}
static void esp_netif_start_ip_lost_timer(esp_netif_t*n){(void)n;lost++;}
'''
main=r'''
int main(int argc,char **argv){(void)argv;
 esp_netif_ip_info_t current={0},old={0};esp_netif_t real={&current,&old},opaque={0};
 struct netif n={.state=&opaque,.ip_addr={0x645ad474},.netmask={0xffc00000}};
 if(argc==1){esp_netif_internal_dhcpc_cb(&n);assert(posts==0&&defaults==0&&lost==0);}
 else{n.client_data[0]=&real;n.state=&real;esp_netif_internal_dhcpc_cb(&n);assert(posts==1&&defaults==1&&current.ip.addr==n.ip_addr.addr);}
 return 0;}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);src=p/'callback.c';src.write_text(selection+stubs+getter+callback+main)
 for enabled in (False,True):
  exe=p/('safe' if enabled else 'unsafe')
  flags=['-DCONFIG_LWIP_PPP_SUPPORT=1'] if enabled else []
  subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-const-variable','-fsanitize=undefined',*flags,str(src),'-o',str(exe)],check=True)
  result=subprocess.run([str(exe)],env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'},capture_output=True)
  assert (result.returncode==0)==enabled, result.stderr.decode()
  subprocess.run([str(exe),'wifi'],check=True)
 guard=Path(__file__).resolve().parents[1]/'include/ml_netif_compat.h'
 src.write_text('int main(void){return 0;}\n')
 for enabled in (0,1):
  result=subprocess.run(['cc','-fsyntax-only',f'-DLWIP_ESP_NETIF_DATA={enabled}','-include',str(guard),str(src)],capture_output=True)
  assert (result.returncode==0)==bool(enabled)
print('actual SDK DHCP callback: unsafe state-cast rejected, client-data separation safe, normal IP events retained; compile guard PASS')
