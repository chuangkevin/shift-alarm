#include "lwip/netif.h"
#include "lwip/ip4.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static err_t init(struct netif *n){n->name[0]='t';n->name[1]='s';n->mtu=1420;return ERR_OK;}
static void add(struct netif *n,unsigned a,unsigned b,unsigned c,unsigned d,unsigned mask){ip4_addr_t ip,netmask,gw;IP4_ADDR(&ip,a,b,c,d);netmask.addr=lwip_htonl(mask);ip4_addr_set_zero(&gw);assert(netif_add(n,&ip,&netmask,&gw,NULL,init,NULL)==n);netif_set_up(n);netif_set_link_up(n);}
int main(void){struct netif loop={0},wifi={0},manual={0},fixed={0};
 add(&loop,127,0,0,1,0xff000000);add(&wifi,192,168,18,160,0xffffff00);netif_set_default(&wifi);
 ip4_addr_t backend,lan;IP4_ADDR(&backend,100,126,226,79);IP4_ADDR(&lan,192,168,18,31);
 // Reproduce the production manual-list registration using an all-zero netif.
 IP4_ADDR(&manual.ip_addr,100,90,212,116);IP4_ADDR(&manual.netmask,255,192,0,0);
 manual.next=netif_list;netif_list=&manual;netif_set_up(&manual);netif_set_link_up(&manual);
 assert(manual.num==loop.num);assert(netif_get_by_index(netif_get_index(&loop))==&manual);
 // Important negative evidence: unbound IPv4 route lookup does NOT use num.
 assert(ip4_route(&backend)==&manual);assert(ip4_route(&lan)==&wifi);
 netif_list=manual.next;
 add(&fixed,100,90,212,116,0xffc00000);
 assert(fixed.num!=loop.num&&fixed.num!=wifi.num);
 assert(netif_get_by_index(netif_get_index(&loop))==&loop);
 assert(netif_get_by_index(netif_get_index(&wifi))==&wifi);
 assert(netif_get_by_index(netif_get_index(&fixed))==&fixed);
 assert(ip4_route(&backend)==&fixed);assert(ip4_route(&lan)==&wifi);
 // Link loss alone would make Tailnet traffic fall back to the default WiFi.
 netif_set_link_down(&fixed);assert(ip4_route(&backend)==&wifi);
 netif_set_link_up(&fixed);assert(ip4_route(&backend)==&fixed);
 puts("actual lwIP netif.c/ip4.c: manual index collision, unbound routing unaffected, netif_add uniqueness, Tailnet/LAN/link route PASS");
}
