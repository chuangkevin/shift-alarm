/* Compile the complete production source, not a copy of its routing logic. */
#ifndef WG_TEST_SOURCE
#define WG_TEST_SOURCE "../src/wireguardif.c"
#endif
#include WG_TEST_SOURCE
#include <assert.h>
static unsigned derp_count,udp_count,input_count;
static uint8_t derp_type,udp_type;
static uint8_t derp_bytes[256],udp_bytes[256];
static size_t derp_len,udp_len;
static bool auth_ok=true;
static uint32_t fake_now=100000;
static err_t derp(const uint8_t *key,const uint8_t *data,size_t len,void *ctx){(void)key;(void)ctx;assert(len>=4);derp_count++;derp_type=data[0];assert(len<=sizeof(derp_bytes));memcpy(derp_bytes,data,len);derp_len=len;return ERR_OK;}
static err_t direct(uint32_t ip,uint16_t port,const uint8_t *data,size_t len,void *ctx){(void)ctx;assert(ip&&port&&len>=4);udp_count++;udp_type=data[0];assert(len<=sizeof(udp_bytes));memcpy(udp_bytes,data,len);udp_len=len;return ERR_OK;}
static bool allow(struct pbuf *p,bool out,void *ctx){(void)p;(void)out;(void)ctx;return true;}
static err_t input(struct pbuf *p,struct netif *n){(void)n;input_count++;pbuf_free(p);return ERR_OK;}
void sys_lock_tcpip_core(void){} void sys_unlock_tcpip_core(void){}
uint32_t wireguard_sys_now(void){return fake_now;}
bool wireguard_expired(uint32_t ms,uint32_t seconds){return fake_now-ms>=seconds*1000;}
bool wireguard_is_under_load(void){return false;}
void netif_set_link_up(struct netif *n){n->flags|=NETIF_FLAG_LINK_UP;}
char *ip4addr_ntoa(const ip4_addr_t *a){(void)a;return "host-test";}
char *ip6addr_ntoa(const ip6_addr_t *a){(void)a;return "host-test6";}
u16_t lwip_htons(u16_t v){return (u16_t)((v<<8)|(v>>8));}
u32_t lwip_htonl(u32_t v){return __builtin_bswap32(v);}
void *mem_malloc(mem_size_t n){return malloc(n);}void mem_free(void *p){free(p);}
struct pbuf *pbuf_alloc(pbuf_layer layer,u16_t length,pbuf_type type){(void)layer;(void)type;struct pbuf *p=calloc(1,sizeof(*p)+length);assert(p);p->payload=p+1;p->len=p->tot_len=length;return p;}
u8_t pbuf_free(struct pbuf *p){free(p);return 1;}
u16_t pbuf_copy_partial(const struct pbuf *p,void *to,u16_t n,u16_t off){assert(off+n<=p->len);memcpy(to,(uint8_t*)p->payload+off,n);return n;}
err_t pbuf_take(struct pbuf *p,const void *from,u16_t n){assert(n<=p->len);memcpy(p->payload,from,n);return ERR_OK;}
void pbuf_realloc(struct pbuf *p,u16_t size){assert(size<=p->len);p->len=p->tot_len=size;}
err_t udp_sendto(struct udp_pcb *pcb,struct pbuf *p,const ip_addr_t *ip,u16_t port){(void)pcb;(void)p;(void)ip;(void)port;assert(!"unexpected internal UDP path");return ERR_IF;}
uint8_t wireguard_get_message_type(const uint8_t *data,size_t len){return len>=4?data[0]:0;}
uint8_t wireguard_peer_index(struct wireguard_device *d,struct wireguard_peer *p){return (uint8_t)(p-d->peers);}
struct wireguard_peer *peer_lookup_by_receiver(struct wireguard_device *d,uint32_t index){return index==1?&d->peers[0]:NULL;}
struct wireguard_peer *peer_lookup_by_handshake(struct wireguard_device *d,uint32_t index){return index==1?&d->peers[0]:NULL;}
struct wireguard_peer *wireguard_process_initiation_message(struct wireguard_device *d,struct message_handshake_initiation *m){(void)m;return auth_ok?&d->peers[0]:NULL;}
bool wireguard_check_mac1(struct wireguard_device *d,const uint8_t *data,size_t n,const uint8_t *mac){(void)d;(void)data;(void)n;(void)mac;return auth_ok;}
bool wireguard_check_mac2(struct wireguard_device *d,const uint8_t *data,size_t n,uint8_t *addr,size_t alen,const uint8_t *mac){(void)d;(void)data;(void)n;(void)addr;(void)alen;(void)mac;return auth_ok;}
void wireguard_create_cookie_reply(struct wireguard_device *d,struct message_cookie_reply *dst,const uint8_t *mac,uint32_t index,uint8_t *addr,size_t n){(void)d;(void)dst;(void)mac;(void)index;(void)addr;(void)n;assert(!"unexpected cookie path");}
bool wireguard_create_handshake_initiation(struct wireguard_device *d,struct wireguard_peer *p,struct message_handshake_initiation *m){(void)d;(void)p;memset(m,0,sizeof(*m));m->type=1;return true;}
bool wireguard_create_handshake_response(struct wireguard_device *d,struct wireguard_peer *p,struct message_handshake_response *m){(void)d;(void)p;memset(m,0,sizeof(*m));m->type=2;return true;}
void wireguard_start_session(struct wireguard_peer *p,bool initiator){p->curr_keypair.valid=true;p->curr_keypair.receiving_valid=true;p->curr_keypair.initiator=initiator;p->curr_keypair.local_index=1;p->curr_keypair.remote_index=2;p->curr_keypair.keypair_millis=fake_now;}
bool wireguard_process_handshake_response(struct wireguard_device *d,struct wireguard_peer *p,struct message_handshake_response *m){(void)d;(void)p;(void)m;return auth_ok;}
bool wireguard_process_cookie_message(struct wireguard_device *d,struct wireguard_peer *p,struct message_cookie_reply *m){(void)d;(void)p;(void)m;return auth_ok;}
void keypair_destroy(struct wireguard_keypair *p){memset(p,0,sizeof(*p));}
void keypair_update(struct wireguard_peer *p,struct wireguard_keypair *k){(void)p;(void)k;}
bool wireguard_check_replay(struct wireguard_keypair *k,uint64_t seq){(void)k;(void)seq;return true;}
void wireguard_encrypt_packet(uint8_t *dst,const uint8_t *src,size_t n,struct wireguard_keypair *k){(void)k;memmove(dst,src,n);}
bool wireguard_decrypt_packet(uint8_t *dst,const uint8_t *src,size_t n,uint64_t seq,struct wireguard_keypair *k){(void)seq;(void)k;if(!auth_ok)return false;assert(n>=16);memcpy(dst,src,n-16);return true;}
sys_mutex_t lock_tcpip_core;
void sys_mutex_lock(sys_mutex_t *m){(void)m;}
void sys_mutex_unlock(sys_mutex_t *m){(void)m;}
char *ipaddr_ntoa(const ip_addr_t *a){(void)a;return "host-test";}
struct wireguard_keypair *get_peer_keypair_for_idx(struct wireguard_peer *p,uint32_t i){return i==p->curr_keypair.local_index?&p->curr_keypair:NULL;}
static struct netif net;
static struct wireguard_device device;
static struct wireguard_peer *reset(void){memset(&net,0,sizeof(net));memset(&device,0,sizeof(device));net.state=&device;net.input=input;device.netif=&net;device.derp_output_fn=derp;device.udp_output_fn=direct;derp_count=udp_count=input_count=0;auth_ok=true;wireguardif_set_packet_filter(allow,NULL);struct wireguard_peer *p=&device.peers[0];p->valid=true;return p;}
static ip_addr_t old_udp(void){ip_addr_t a;IP_ADDR4(&a,203,0,113,9);return a;}
static void inject_init(ip_addr_t source){struct pbuf *b=pbuf_alloc(PBUF_RAW,sizeof(struct message_handshake_initiation),PBUF_RAM);memset(b->payload,0,b->len);((uint8_t*)b->payload)[0]=1;wireguardif_network_rx(&device,NULL,b,&source,ip_addr_isany(&source)?0:51820);}
static void inject_data(ip_addr_t source){struct pbuf *b=pbuf_alloc(PBUF_RAW,16+20+16,PBUF_RAM);memset(b->payload,0,b->len);struct message_transport_data *m=b->payload;m->type=4;m->receiver=1;uint8_t *ip=m->enc_packet;ip[0]=0x45;ip[3]=20;ip[12]=100;ip[13]=126;ip[14]=226;ip[15]=79;wireguardif_network_rx(&device,NULL,b,&source,ip_addr_isany(&source)?0:51820);}
static void reply(void){struct pbuf *b=pbuf_alloc(PBUF_RAW,20,PBUF_RAM);memset(b->payload,0,20);assert(wireguardif_output_to_peer(&net,b,NULL,&device.peers[0])==ERR_OK);pbuf_free(b);}
int main(void){
 ip_addr_t relay;ip_addr_set_any(false,&relay);struct wireguard_peer *p=reset();p->ip=old_udp();p->port=51820;
 inject_init(relay);assert(derp_count==1&&derp_type==2&&udp_count==0);
 ip_addr_t allowed;IP_ADDR4(&allowed,100,126,226,79);ip_addr_t mask;IP_ADDR4(&mask,255,255,255,255);assert(peer_add_ip(p,allowed,mask));
 inject_data(relay);assert(input_count==1);reply();assert(derp_count==2&&derp_type==4&&udp_count==0);
 // A verified direct packet upgrades transport; normal DATA is not duplicated.
 inject_data(old_udp());assert(input_count==2);reply();assert(udp_count==1&&udp_type==4&&derp_count==2);
 // Authentication failure must not switch a verified direct path to DERP.
 p=reset();p->ip=old_udp();p->port=51820;auth_ok=false;inject_init(relay);assert(!ip_addr_isany(&p->ip)&&!derp_count&&!udp_count);
 auth_ok=true;wireguard_start_session(p,true);auth_ok=false;inject_data(relay);assert(!ip_addr_isany(&p->ip)&&input_count==0);
 // A fresh native BSD-style outbound IP packet schedules a relay handshake.
 p=reset();assert(peer_add_ip(p,allowed,mask));struct pbuf *syn=pbuf_alloc(PBUF_RAW,20,PBUF_RAM);memset(syn->payload,0,20);assert(wireguardif_output(&net,syn,ip_2_ip4(&allowed))==ERR_OK);pbuf_free(syn);assert(p->send_handshake);wireguardif_periodic(&net);assert(derp_count==1&&derp_type==1&&udp_count==0);
 // Candidate direct endpoint: duplicate the SAME initiation via relay fallback.
 p=reset();p->ip=old_udp();p->port=51820;assert(wireguard_start_handshake(&net,p)==ERR_OK);assert(derp_count==1&&udp_count==1&&derp_type==1&&udp_type==1);assert(derp_len==udp_len&&!memcmp(derp_bytes,udp_bytes,derp_len));
 puts("actual wireguardif transport: DERP INIT/DATA/reply, auth rejection, native SYN, direct+relay handshake PASS");
}
