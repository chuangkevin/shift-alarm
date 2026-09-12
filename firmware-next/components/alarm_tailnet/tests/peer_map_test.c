#include "microlink_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static ml_peer_update_t *pending[256];static unsigned count;
uint64_t ml_get_time_ms(void){return 1000;}
int xQueueSend(void *queue,const void *item,unsigned timeout){(void)queue;(void)timeout;assert(count<256);pending[count++]=*(ml_peer_update_t *const*)item;return pdTRUE;}
static void clear(void){while(count)free(pending[--count]);}
static bool update(microlink_t *m,const char *s){cJSON *j=cJSON_Parse(s);assert(j);bool ok=ml_peer_map_update(m,j);cJSON_Delete(j);return ok;}
static void initial(microlink_t *m){
 const char *s="{\"Peers\":[{\"ID\":7,\"Name\":\"backend\",\"Key\":\"nodekey:1111111111111111111111111111111111111111111111111111111111111111\",\"DiscoKey\":\"discokey:3333333333333333333333333333333333333333333333333333333333333333\",\"Addresses\":[\"100.64.0.2/32\"],\"HomeDERP\":1}]}";
 assert(update(m,s));
}
int main(void){
 microlink_t m={0};assert(ml_security_init(&m));initial(&m);
 assert(count==3&&pending[0]->action==ML_PEER_RESET&&pending[1]->action==ML_PEER_ADD&&pending[1]->node_id==7&&pending[2]->action==ML_PEER_SYNC_DONE);
 assert(!m.security.peers_ready&&m.security.peer_count==1);
 assert(m.security.peers[0].derp_region==1&&!memcmp(m.security.peers[0].public_key,pending[1]->public_key,32));clear();
 assert(update(&m,"{\"PeersChangedPatch\":[{\"NodeID\":7,\"DERPRegion\":4,\"Endpoints\":[\"192.0.2.8:12345\"],\"Key\":\"nodekey:2222222222222222222222222222222222222222222222222222222222222222\"}]}"));
 assert(count==2&&pending[0]->action==ML_PEER_ADD&&pending[0]->node_id==7&&pending[0]->derp_region==4&&pending[0]->endpoint_count==1&&pending[0]->endpoints[0].port==12345&&pending[0]->public_key[0]==0x22);
 assert(m.security.peers[0].derp_region==4&&!memcmp(m.security.peers[0].public_key,pending[0]->public_key,32));clear();
 assert(update(&m,"{\"PeersChangedPatch\":[{\"NodeID\":999,\"DERPRegion\":3},{\"NodeID\":7,\"Online\":false}]}"));
 assert(count==1&&pending[0]->action==ML_PEER_SYNC_DONE);clear();
 assert(update(&m,"{\"PeersChangedPatch\":[{\"NodeID\":7,\"KeyExpiry\":\"2020-01-01T00:00:00Z\"}]}"));
 assert(m.security.peer_count==0&&count==2&&pending[0]->action==ML_PEER_REMOVE&&pending[0]->node_id==7);clear();
 /* Remote peer authorization comes from control's Peers membership and ACLs.
  * Real map peers omit MachineAuthorized. Explicit false has the same remote
  * semantics, while expiry and unsigned-only still remove network access. */
 for(unsigned mode=0;mode<4;mode++) {
   initial(&m);clear();
   cJSON *root=cJSON_CreateObject(),*peers=cJSON_AddArrayToObject(root,"PeersChanged");
   cJSON *node=cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(m.peer_map,"7"),true);
   assert(node);cJSON_AddItemToArray(peers,node);
   if(mode==0)cJSON_AddBoolToObject(node,"MachineAuthorized",false);
   if(mode==1)cJSON_AddBoolToObject(node,"Expired",true);
   if(mode==2)cJSON_AddBoolToObject(node,"UnsignedPeerAPIOnly",true);
   if(mode==3)cJSON_AddStringToObject(node,"KeyExpiry","2020-01-01T00:00:00Z");
   assert(ml_peer_map_update(&m,root));cJSON_Delete(root);
   if(mode==0)assert(m.security.peer_count==1&&count==1&&pending[0]->action==ML_PEER_SYNC_DONE);
   else assert(m.security.peer_count==0&&count==2&&pending[0]->action==ML_PEER_REMOVE);
   clear();
 }
 initial(&m);clear();
 assert(update(&m,"{\"PeersRemoved\":[7]}"));assert(count==2&&pending[0]->action==ML_PEER_REMOVE&&pending[0]->node_id==7&&m.security.peer_count==0);clear();
 initial(&m);clear();
 assert(update(&m,"{\"Peers\":[]}"));assert(count==2&&pending[0]->action==ML_PEER_REMOVE);clear();
 uint32_t gen=m.security.peer_generation;assert(!update(&m,"{\"PeersRemoved\":[7.5]}"));assert(m.security.peer_generation>gen&&!m.security.peers_ready);clear();
 /* Capacity: the user's 57-peer tailnet fits, 64 is the exact bound,
  * 65 is rejected before any owner mutation, never silently truncated. */
 for(unsigned total=57;total<=65;total+=(total==57?7:1)) {
   cJSON *root=cJSON_CreateObject(),*peers=cJSON_AddArrayToObject(root,"Peers");
   for(unsigned i=1;i<=total;i++) {
     cJSON *node=cJSON_CreateObject();cJSON_AddNumberToObject(node,"ID",i);
     char key[80],ip[32];snprintf(key,sizeof(key),"nodekey:%064x",i);cJSON_AddStringToObject(node,"Key",key);
     snprintf(key,sizeof(key),"discokey:%064x",i);cJSON_AddStringToObject(node,"DiscoKey",key);
     snprintf(ip,sizeof(ip),"100.64.0.%u/32",i);cJSON *addresses=cJSON_AddArrayToObject(node,"Addresses");cJSON_AddItemToArray(addresses,cJSON_CreateString(ip));
     cJSON_AddItemToArray(peers,node);
   }
   bool ok=ml_peer_map_update(&m,root);cJSON_Delete(root);
   assert(ok==(total<=64));assert(m.security.observed_peer_count==total);
   if(total>64)assert(m.security.capacity_exceeded&&!m.security.peers_ready&&count==0);
   else assert(!m.security.capacity_exceeded&&m.security.peer_count==total);
   clear();
 }
 cJSON_Delete(m.peer_map);ml_security_destroy(&m);
 puts("PASS: NodeID full/delta maps, removal, patch array, key/endpoint/DERP/expiry, unknown IDs, empty snapshot, fail-closed generation");
}
