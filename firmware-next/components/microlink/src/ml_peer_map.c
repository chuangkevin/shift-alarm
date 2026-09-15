/* Authoritative control map reconciliation by tailcfg.NodeID, never node-key string.
 * All WG updates cross the owner queue. Generation acknowledgements prevent a
 * new map/ACL from permitting data before revoked keys have been removed.
 */
#include "microlink_internal.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
static bool node_id(cJSON *value,uint64_t *out,char key[24]) {
    if(!cJSON_IsNumber(value)||value->valuedouble<1||value->valuedouble>9007199254740991.0)return false;
    *out=(uint64_t)value->valuedouble;
    if((double)*out!=value->valuedouble)return false;
    snprintf(key,24,"%llu",(unsigned long long)*out);return true;
}
static bool key_bytes(cJSON *value,const char *prefix,uint8_t out[32]) {
    if(!cJSON_IsString(value))return false;
    const char *s=value->valuestring;size_t n=strlen(prefix);
    if(strncmp(s,prefix,n)||strlen(s+n)!=64)return false;
    for(unsigned i=0;i<32;i++) {
        unsigned v=0;
        for(unsigned j=0;j<2;j++) {
            char c=s[n+i*2+j];unsigned x;
            if(c>='0'&&c<='9')x=c-'0';else if(c>='a'&&c<='f')x=c-'a'+10;else if(c>='A'&&c<='F')x=c-'A'+10;else return false;
            v=v*16+x;
        }
        out[i]=v;
    }
    return true;
}
static bool compile_node(cJSON *node,ml_peer_update_t *out,int64_t *expiry) {
    memset(out,0,sizeof(*out));out->action=ML_PEER_ADD;
    char id[24];if(!node_id(cJSON_GetObjectItemCaseSensitive(node,"ID"),&out->node_id,id))return false;
    if(!key_bytes(cJSON_GetObjectItemCaseSensitive(node,"Key"),"nodekey:",out->public_key)||
       !key_bytes(cJSON_GetObjectItemCaseSensitive(node,"DiscoKey"),"discokey:",out->disco_key))return false;
    cJSON *ex=cJSON_GetObjectItemCaseSensitive(node,"KeyExpiry");
    *expiry=ex?ml_parse_expiry(cJSON_IsString(ex)?ex->valuestring:NULL):0;
    /* Remote Peers membership is granted by control; MachineAuthorized is not
     * a remote WG eligibility requirement and is normally omitted. Our own
     * Node/RegisterResponse authorization is enforced separately by ml_security.
     * Keep explicit peer expiry and unsigned-only restrictions fail-closed. */
    if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(node,"Expired"))||
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(node,"UnsignedPeerAPIOnly"))) *expiry=1;
    cJSON *addresses=cJSON_GetObjectItemCaseSensitive(node,"Addresses");
    if(!cJSON_IsArray(addresses))return false;
    cJSON *addr;cJSON_ArrayForEach(addr,addresses) {
        unsigned a,b,c,d;int used=0;
        if(cJSON_IsString(addr)&&sscanf(addr->valuestring,"%u.%u.%u.%u/32%n",&a,&b,&c,&d,&used)==4&&used>0&&!addr->valuestring[used]&&a<=255&&b<=255&&c<=255&&d<=255) {
            out->vpn_ip=(a<<24)|(b<<16)|(c<<8)|d;break;
        }
    }
    if(!out->vpn_ip)return false;
    cJSON *name=cJSON_GetObjectItemCaseSensitive(node,"Name");
    if(cJSON_IsString(name))snprintf(out->hostname,sizeof(out->hostname),"%s",name->valuestring);
    cJSON *derp=cJSON_GetObjectItemCaseSensitive(node,"HomeDERP");
    if(cJSON_IsNumber(derp)&&derp->valueint>0&&derp->valueint<=65535)out->derp_region=derp->valueint;
    else {
        derp=cJSON_GetObjectItemCaseSensitive(node,"DERP");unsigned region;
        if(cJSON_IsString(derp)&&sscanf(derp->valuestring,"127.3.3.40:%u",&region)==1&&region<=65535)out->derp_region=region;
    }
    cJSON *endpoints=cJSON_GetObjectItemCaseSensitive(node,"Endpoints");
    cJSON *ep;cJSON_ArrayForEach(ep,endpoints) {
        unsigned a,b,c,d,port;int used=0;
        if(out->endpoint_count>=ML_MAX_ENDPOINTS)break;
        if(cJSON_IsString(ep)&&sscanf(ep->valuestring,"%u.%u.%u.%u:%u%n",&a,&b,&c,&d,&port,&used)==5&&!ep->valuestring[used]&&a<=255&&b<=255&&c<=255&&d<=255&&port>0&&port<=65535) {
            unsigned i=out->endpoint_count++;
            out->endpoints[i].ip=(a<<24)|(b<<16)|(c<<8)|d;out->endpoints[i].port=port;
        }
    }
    return true;
}
static bool enqueue(microlink_t *ml,const ml_peer_update_t *value) {
    ml_peer_update_t *copy=ml_psram_malloc(sizeof(*copy));if(!copy)return false;
    *copy=*value;
    if(!copy->generation){xSemaphoreTake(ml->security.lock,portMAX_DELAY);copy->generation=ml->security.peer_generation;xSemaphoreGive(ml->security.lock);}
    if(xQueueSend(ml->peer_update_queue,&copy,pdMS_TO_TICKS(1000))==pdTRUE)return true;
    free(copy);return false;
}
static bool replace_node(cJSON *nodes,cJSON *node) {
    uint64_t id;char name[24];if(!node_id(cJSON_GetObjectItemCaseSensitive(node,"ID"),&id,name))return false;
    cJSON *copy=cJSON_Duplicate(node,true);if(!copy)return false;
    cJSON_DeleteItemFromObjectCaseSensitive(nodes,name);
    if(!cJSON_AddItemToObject(nodes,name,copy)){cJSON_Delete(copy);return false;}return true;
}
bool ml_peer_map_update(microlink_t *ml,cJSON *map) {
    cJSON *full=cJSON_GetObjectItemCaseSensitive(map,"Peers");
    cJSON *changed=cJSON_GetObjectItemCaseSensitive(map,"PeersChanged");
    cJSON *removed=cJSON_GetObjectItemCaseSensitive(map,"PeersRemoved");
    cJSON *patches=cJSON_GetObjectItemCaseSensitive(map,"PeersChangedPatch");
    if((!full||cJSON_IsNull(full))&&(!changed||cJSON_IsNull(changed))&&(!removed||cJSON_IsNull(removed))&&(!patches||cJSON_IsNull(patches)))return true;
    bool reset=!ml->peer_map||ml->peer_map_dirty;
    cJSON *next=full&&!cJSON_IsNull(full)?cJSON_CreateObject():ml->peer_map?cJSON_Duplicate(ml->peer_map,true):cJSON_CreateObject();
    if(!next)goto fail;
    cJSON *lists[]={full,changed};
    for(unsigned i=0;i<2;i++)if(lists[i]&&!cJSON_IsNull(lists[i])) {
        if(!cJSON_IsArray(lists[i]))goto fail_next;
        cJSON *node;cJSON_ArrayForEach(node,lists[i])if(!replace_node(next,node))goto fail_next;
    }
    if(removed&&!cJSON_IsNull(removed)) {
        if(!cJSON_IsArray(removed))goto fail_next;
        cJSON *id;cJSON_ArrayForEach(id,removed) {uint64_t n;char key[24];if(!node_id(id,&n,key))goto fail_next;cJSON_DeleteItemFromObjectCaseSensitive(next,key);}
    }
    if(patches&&!cJSON_IsNull(patches)) {
        if(!cJSON_IsArray(patches))goto fail_next;
        cJSON *patch;cJSON_ArrayForEach(patch,patches) {
            uint64_t id;char key[24];if(!node_id(cJSON_GetObjectItemCaseSensitive(patch,"NodeID"),&id,key))goto fail_next;
            cJSON *node=cJSON_GetObjectItemCaseSensitive(next,key);if(!node)continue; /* Official semantics: unknown IDs ignored. */
            cJSON *field;cJSON_ArrayForEach(field,patch) {
                if(!field->string)goto fail_next;
                const char *name=field->string;
                if(!strcmp(name,"NodeID")||cJSON_IsNull(field))continue;
                if(!strcmp(name,"DERPRegion")) {if(!cJSON_IsNumber(field))goto fail_next;if(field->valueint==0)continue;name="HomeDERP";}
                if(!strcmp(name,"Endpoints")) {if(!cJSON_IsArray(field))goto fail_next;if(!cJSON_GetArraySize(field))continue;}
                cJSON *copy=cJSON_Duplicate(field,true);if(!copy)goto fail_next;
                cJSON_DeleteItemFromObjectCaseSensitive(node,name);
                if(!cJSON_AddItemToObject(node,name,copy)){cJSON_Delete(copy);goto fail_next;}
            }
        }
    }
    unsigned observed=cJSON_GetArraySize(next);
    xSemaphoreTake(ml->security.lock,portMAX_DELAY);
    ml->security.observed_peer_count=observed;
    ml->security.capacity_exceeded=observed>ML_POLICY_MAX_PEERS;
    xSemaphoreGive(ml->security.lock);
    if(observed>ML_POLICY_MAX_PEERS)goto fail_next;
    /* Publish membership/expiry and suspend packets until owner processes the same generation. */
    xSemaphoreTake(ml->security.lock,portMAX_DELAY);
    uint32_t generation=++ml->security.peer_generation;
    ml->security.peers_ready=false;ml->security.peer_count=0;ml->security.peer_install_failed=false;
    memset(ml->security.flows,0,sizeof(ml->security.flows));
    cJSON *node;cJSON_ArrayForEach(node,next) {
        ml_peer_update_t value;int64_t expiry;
        if(!compile_node(node,&value,&expiry)){xSemaphoreGive(ml->security.lock);goto fail_next;}
        if(expiry&&time(NULL)>=expiry)continue;
        ml_allowed_peer_t *allowed=&ml->security.peers[ml->security.peer_count++];
        *allowed=(ml_allowed_peer_t){.ip=value.vpn_ip,.expiry=expiry,.derp_region=value.derp_region};
        memcpy(allowed->public_key,value.public_key,32);
    }
    xSemaphoreGive(ml->security.lock);
    if(reset) {ml_peer_update_t value={.action=ML_PEER_RESET};if(!enqueue(ml,&value))goto fail_next;}
    else {
        cJSON_ArrayForEach(node,ml->peer_map) {
            cJSON *newnode=cJSON_GetObjectItemCaseSensitive(next,node->string);
            ml_peer_update_t value;int64_t oldexpiry,newexpiry;
            if(!compile_node(node,&value,&oldexpiry))goto fail_next;
            ml_peer_update_t newer;
            bool newactive=newnode&&compile_node(newnode,&newer,&newexpiry)&&(!newexpiry||time(NULL)<newexpiry);
            if(!newactive){value.action=ML_PEER_REMOVE;if(!enqueue(ml,&value))goto fail_next;}
        }
    }
    /* Installing every advertised peer can exhaust the ESP32's available
     * WireGuard peer storage before the backend appears in control-map order.
     * Always install the configured service peer first on a full rebuild so
     * backend and OTA traffic cannot be displaced by unrelated tailnet nodes. */
    cJSON *priority_node=NULL;
    if(reset&&ml->config.priority_peer_ip) {
        cJSON_ArrayForEach(node,next) {
            ml_peer_update_t value;int64_t expiry;
            if(!compile_node(node,&value,&expiry))goto fail_next;
            if(value.vpn_ip==ml->config.priority_peer_ip&&(!expiry||time(NULL)<expiry)){
                priority_node=node;
                if(!enqueue(ml,&value))goto fail_next;
                break;
            }
        }
    }
    cJSON_ArrayForEach(node,next) {
        if(node==priority_node)continue;
        ml_peer_update_t value,old;int64_t expiry,oldexpiry;
        if(!compile_node(node,&value,&expiry))goto fail_next;
        if(expiry&&time(NULL)>=expiry)continue;
        cJSON *oldnode=ml->peer_map?cJSON_GetObjectItemCaseSensitive(ml->peer_map,node->string):NULL;
        if(!reset&&oldnode&&compile_node(oldnode,&old,&oldexpiry)&&(!oldexpiry||time(NULL)<oldexpiry)&&!memcmp(&value,&old,sizeof(value)))continue;
        if(!enqueue(ml,&value))goto fail_next;
    }
    {ml_peer_update_t done={.action=ML_PEER_SYNC_DONE,.generation=generation};if(!enqueue(ml,&done))goto fail_next;}
    cJSON_Delete(ml->peer_map);ml->peer_map=next;ml->peer_map_dirty=false;return true;
fail_next:
    cJSON_Delete(next);
fail:
    ml->peer_map_dirty=true;
    xSemaphoreTake(ml->security.lock,portMAX_DELAY);ml->security.peer_generation++;ml->security.peers_ready=false;xSemaphoreGive(ml->security.lock);
    return false;
}
