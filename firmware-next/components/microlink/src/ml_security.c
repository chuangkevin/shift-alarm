/* Conservative IPv4 data-plane filter for this vendored MicroLink build.
 * Unsupported syntax never produces an allow. No IPv6 or fragmented packets.
 * Control-plane PacketFilters named deltas follow tailcfg semantics.
 */
#include "microlink_internal.h"
#include "wireguardif.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint16_t be16(const uint8_t *p) { return ((uint16_t)p[0]<<8)|p[1]; }
bool ml_security_init(microlink_t *ml) {
    ml->security.lock=xSemaphoreCreateMutex();
    if (!ml->security.lock) return false;
    wireguardif_set_packet_filter(ml_security_packet,ml);
    return true;
}
void ml_security_destroy(microlink_t *ml) {
    wireguardif_set_packet_filter(NULL,NULL);
    cJSON_Delete(ml->security.filters);
    if(ml->security.lock)vSemaphoreDelete(ml->security.lock);
}
void ml_security_close(microlink_t *ml) {
    xSemaphoreTake(ml->security.lock,portMAX_DELAY);
    ml->security.ready=false;ml->security.peers_ready=false;ml->security.peer_generation++;
    cJSON_Delete(ml->security.filters);ml->security.filters=NULL;
    memset(ml->security.flows,0,sizeof(ml->security.flows));
    xSemaphoreGive(ml->security.lock);
}
void ml_security_followup(microlink_t *ml,cJSON *request) {
    xSemaphoreTake(ml->security.lock,portMAX_DELAY);
    if(ml->security.auth_url[0])cJSON_AddStringToObject(request,"Followup",ml->security.auth_url);
    xSemaphoreGive(ml->security.lock);
}
int ml_security_register(microlink_t *ml,cJSON *response) {
    cJSON *err=cJSON_GetObjectItemCaseSensitive(response,"Error");
    cJSON *url=cJSON_GetObjectItemCaseSensitive(response,"AuthURL");
    cJSON *expired=cJSON_GetObjectItemCaseSensitive(response,"NodeKeyExpired");
    cJSON *ok=cJSON_GetObjectItemCaseSensitive(response,"MachineAuthorized");
    xSemaphoreTake(ml->security.lock,portMAX_DELAY);
    ml->security.authorized=false;
    int result=-1;
    if(cJSON_IsString(err)&&err->valuestring[0])goto done;
    if(cJSON_IsTrue(expired)){ml->key_expired=true;ml->security.expired=true;goto done;}
    if(cJSON_IsString(url)&&url->valuestring[0]) {
        /* Fixed official login origin: never turn control responses into arbitrary links. */
        if(strncmp(url->valuestring,"https://login.tailscale.com/",27)!=0 || strlen(url->valuestring)>=sizeof(ml->security.auth_url))goto done;
        strcpy(ml->security.auth_url,url->valuestring);
        ml->security.auth_pending=true;
        result=1;goto done;
    }
    if(cJSON_IsTrue(ok)) {
        ml->security.authorized=true;
        ml->security.auth_pending=false;
        ml->security.auth_url[0]=0;
        ml->key_expired=false;ml->security.expired=false;
        result=0;
    }
done:
    xSemaphoreGive(ml->security.lock);
    return result;
}
int64_t ml_parse_expiry(const char *s) {
    if(!s)return 1;
    if(!strcmp(s,"0001-01-01T00:00:00Z"))return 0;
    int y,m,d,h,n,sec,used=0;
    if(sscanf(s,"%d-%d-%dT%d:%d:%d%n",&y,&m,&d,&h,&n,&sec,&used)!=6||y<1970||y>9999||m<1||m>12||d<1||d>31||h<0||h>23||n<0||n>59||sec<0||sec>59)return 1;
    const char *tail=s+used;
    if(*tail=='.'){tail++;while(*tail>='0'&&*tail<='9')tail++;}
    if(strcmp(tail,"Z"))return 1;
    int64_t days=0;
    for(int a=1970;a<y;a++)days+=(a%4==0&&(a%100!=0||a%400==0))?366:365;
    const int md[]={31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap=y%4==0&&(y%100!=0||y%400==0);
    if(d>md[m-1]+(m==2&&leap))return 1;
    for(int a=1;a<m;a++)days+=md[a-1]+(a==2&&leap);
    return (days+d-1)*86400+h*3600+n*60+sec;
}
void ml_security_map(microlink_t *ml,cJSON *map) {
    cJSON *legacy=cJSON_GetObjectItemCaseSensitive(map,"PacketFilter");
    cJSON *delta=cJSON_GetObjectItemCaseSensitive(map,"PacketFilters");
    cJSON *node=cJSON_GetObjectItemCaseSensitive(map,"Node");
    xSemaphoreTake(ml->security.lock,portMAX_DELAY);
    /* Node is a complete value when present, not a field-level patch. Go's
     * omitempty/omitzero fields decode to false/zero when omitted. */
    if(cJSON_IsObject(node)) {
        ml->security.expired=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(node,"Expired"));
        cJSON *expiry=cJSON_GetObjectItemCaseSensitive(node,"KeyExpiry");
        ml->security.expiry=(!expiry||cJSON_IsNull(expiry))?0:ml_parse_expiry(cJSON_IsString(expiry)?expiry->valuestring:NULL);
        ml->security.authorized=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(node,"MachineAuthorized"))&&!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(node,"UnsignedPeerAPIOnly"));
        ml->key_expired=ml->security.expired;
        ml->key_expiry_epoch=ml->security.expiry;
    } else if(node&&!cJSON_IsNull(node)) {
        ml->security.authorized=false; /* Malformed self-node fails closed. */
    }
    /* Absent/null fields mean unchanged, including deny-all at initial startup. */
    if((!legacy||cJSON_IsNull(legacy))&&(!delta||cJSON_IsNull(delta)))goto done;
    cJSON *next=ml->security.filters?cJSON_Duplicate(ml->security.filters,true):cJSON_CreateObject();
    if(!next)goto fail;
    if(legacy&&!cJSON_IsNull(legacy)) {
        if(!cJSON_IsArray(legacy)){cJSON_Delete(next);goto fail;}
        cJSON_DeleteItemFromObjectCaseSensitive(next,"base");
        cJSON *copy=cJSON_Duplicate(legacy,true);
        if(!copy||!cJSON_AddItemToObject(next,"base",copy)){cJSON_Delete(copy);cJSON_Delete(next);goto fail;}
    }
    if(delta&&!cJSON_IsNull(delta)) {
        if(!cJSON_IsObject(delta)){cJSON_Delete(next);goto fail;}
        cJSON *clear=cJSON_GetObjectItemCaseSensitive(delta,"*");
        if(cJSON_IsNull(clear)){cJSON_Delete(next);next=cJSON_CreateObject();if(!next)goto fail;}
        cJSON *part;
        cJSON_ArrayForEach(part,delta) {
            if(!part->string){cJSON_Delete(next);goto fail;}
            if(!strcmp(part->string,"*")&&cJSON_IsNull(part)) {
                continue;
            }
            cJSON_DeleteItemFromObjectCaseSensitive(next,part->string);
            if(cJSON_IsNull(part))continue;
            if(!cJSON_IsArray(part)){cJSON_Delete(next);goto fail;}
            cJSON *copy=cJSON_Duplicate(part,true);
            if(!copy||!cJSON_AddItemToObject(next,part->string,copy)){cJSON_Delete(copy);cJSON_Delete(next);goto fail;}
        }
    }
    cJSON_Delete(ml->security.filters);ml->security.filters=next;
    ml->security.ready=true;
    memset(ml->security.flows,0,sizeof(ml->security.flows));goto done;
fail:
    cJSON_Delete(ml->security.filters);ml->security.filters=NULL;ml->security.ready=false;
    memset(ml->security.flows,0,sizeof(ml->security.flows));
done:
    xSemaphoreGive(ml->security.lock);
}
static bool match_ip(const char *s,uint32_t ip) {
    if(!s)return false;
    if(!strcmp(s,"*"))return true;
    unsigned a,b,c,d,bits=32;int used=0;
    if(sscanf(s,"%u.%u.%u.%u%n",&a,&b,&c,&d,&used)!=4||a>255||b>255||c>255||d>255)return false;
    if(s[used]) { int n=0;if(sscanf(s+used,"/%u%n",&bits,&n)!=1||s[used+n]||bits>32)return false; }
    uint32_t mask=bits?UINT32_MAX<<(32-bits):0;
    return (ip&mask)==(((a<<24)|(b<<16)|(c<<8)|d)&mask);
}
static bool rule_allows(cJSON *rule,uint32_t src,uint32_t dst,uint16_t port,uint8_t proto) {
    /* Deprecated CIDR bit arrays and capability-based sources need a fuller compiler. */
    if(cJSON_GetObjectItemCaseSensitive(rule,"SrcBits"))return false;
    cJSON *field;
    cJSON_ArrayForEach(field,rule) {
        if(!field->string)return false;
        if(strcmp(field->string,"SrcIPs")&&strcmp(field->string,"DstPorts")&&strcmp(field->string,"IPProto")&&strcmp(field->string,"CapGrant"))return false;
    }
    cJSON *caps=cJSON_GetObjectItemCaseSensitive(rule,"CapGrant");
    if(caps&&!cJSON_IsNull(caps)&&(!cJSON_IsArray(caps)||cJSON_GetArraySize(caps)>0))return false;
    cJSON *protos=cJSON_GetObjectItemCaseSensitive(rule,"IPProto");
    if(protos&&!cJSON_IsNull(protos)&&!cJSON_IsArray(protos))return false;
    if(protos&&!cJSON_IsNull(protos)&&cJSON_GetArraySize(protos)>0) {
        bool found=false;cJSON *p;cJSON_ArrayForEach(p,protos){if(!cJSON_IsNumber(p)||p->valuedouble!=p->valueint)return false;if(p->valueint==proto)found=true;}
        if(!found)return false;
    } else if(proto!=6&&proto!=17)return false;
    bool found=false;cJSON *s;cJSON *sources=cJSON_GetObjectItemCaseSensitive(rule,"SrcIPs");
    if(!cJSON_IsArray(sources))return false;
    cJSON_ArrayForEach(s,sources)if(cJSON_IsString(s)&&match_ip(s->valuestring,src))found=true;
    if(!found)return false;
    cJSON *dests=cJSON_GetObjectItemCaseSensitive(rule,"DstPorts");
    if(!cJSON_IsArray(dests))return false;
    cJSON *d;cJSON_ArrayForEach(d,dests) {
        if(cJSON_GetObjectItemCaseSensitive(d,"Bits"))continue;
        cJSON *ip=cJSON_GetObjectItemCaseSensitive(d,"IP");
        cJSON *ports=cJSON_GetObjectItemCaseSensitive(d,"Ports");
        cJSON *first=cJSON_GetObjectItemCaseSensitive(ports,"First");
        cJSON *last=cJSON_GetObjectItemCaseSensitive(ports,"Last");
        if(cJSON_IsString(ip)&&match_ip(ip->valuestring,dst)&&cJSON_IsNumber(first)&&cJSON_IsNumber(last)&&first->valuedouble==first->valueint&&last->valuedouble==last->valueint&&first->valuedouble>=0&&last->valuedouble<=65535&&first->valuedouble<=port&&port<=last->valuedouble)return true;
    }
    return false;
}
bool ml_security_packet(struct pbuf *p,bool outbound,void *ctx) {
    microlink_t *ml=ctx;uint8_t h[64];
    if(!ml||!p)return false;
    xSemaphoreTake(ml->security.lock,portMAX_DELAY);
    bool allowed=false;
    if(outbound)ml->security.wg_out_packets++;else ml->security.wg_in_packets++;
    if(p->tot_len<28)goto done;
    size_t n=pbuf_copy_partial(p,h,sizeof(h),0);
    unsigned ihl=(h[0]&15)*4;
    if((h[0]>>4)!=4||ihl<20||ihl>60||n<ihl+4||be16(h+2)>p->tot_len||be16(h+2)<ihl+8||(be16(h+6)&0x3fff))goto done;
    uint8_t proto=h[9];if(proto!=6&&proto!=17)goto done;
    uint32_t src=be32(h+12),dst=be32(h+16);
    uint16_t sport=be16(h+ihl),dport=be16(h+ihl+2);
    if(outbound){ml->security.wg_last_out_src=src;ml->security.wg_last_out_dst=dst;}
    uint64_t now=ml_get_time_ms();
    if(!ml->security.peers_ready||!ml->security.ready||!ml->security.authorized||ml->security.expired||
       (ml->security.expiry>0&&time(NULL)>=ml->security.expiry))goto done;
    uint32_t remote=outbound?dst:src;bool known=false;
    for(unsigned i=0;i<ml->security.peer_count;i++) {
        ml_allowed_peer_t *peer=&ml->security.peers[i];
        if(peer->ip==remote && (!peer->expiry||time(NULL)<peer->expiry)){known=true;break;}
    }
    if(!known)goto done;
    if(outbound) {
        if(src!=ml->vpn_ip)goto done;
        unsigned slot=0;
        for(unsigned i=0;i<16;i++) {
            ml_flow_t *f=&ml->security.flows[i];
            if(f->until_ms<ml->security.flows[slot].until_ms)slot=i;
            if(f->src==src&&f->dst==dst&&f->sport==sport&&f->dport==dport&&f->proto==proto){slot=i;break;}
        }
        ml->security.flows[slot]=(ml_flow_t){src,dst,sport,dport,proto,now+120000};allowed=true;goto done;
    }
    if(dst!=ml->vpn_ip)goto done;
    for(unsigned i=0;i<16;i++) {
        ml_flow_t *f=&ml->security.flows[i];
        if(f->until_ms>=now&&f->src==dst&&f->dst==src&&f->sport==dport&&f->dport==sport&&f->proto==proto){allowed=true;goto done;}
    }
    cJSON *part,*rule;
    cJSON_ArrayForEach(part,ml->security.filters)cJSON_ArrayForEach(rule,part)
        if(rule_allows(rule,src,dst,dport,proto)){allowed=true;goto done;}
done:
    if(!allowed){if(outbound)ml->security.wg_out_dropped++;else ml->security.wg_in_dropped++;}
    xSemaphoreGive(ml->security.lock);return allowed;
}
