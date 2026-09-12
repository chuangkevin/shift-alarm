#include "alarm_tailnet.h"
#include "microlink_internal.h"
#include "nvs.h"
#include "esp_random.h"
#include <string.h>
#include <time.h>
/* Only this worker may access the client or persistent identity. HTTP/loop
 * callers enqueue commands and read a bounded cached status copy. */
static microlink_t *client;
static char hostname[64];
static bool want_running;
static esp_err_t last_error;
static alarm_tailnet_status_t snapshot;
static portMUX_TYPE snapshot_lock=portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t commands;
static int initialized;
typedef struct {enum {START,STOP,REAUTH} op;char name[64];} command_t;
static void publish(void) {
    alarm_tailnet_status_t next={.last_error=last_error,.peer_capacity=ML_POLICY_MAX_PEERS};
    if(!client)next.state=want_running?ALARM_TAILNET_CONNECTING:ALARM_TAILNET_OFF;
    else {
        xSemaphoreTake(client->security.lock,portMAX_DELAY);
        memcpy(next.auth_url,client->security.auth_url,sizeof(next.auth_url));
        next.acl_ready=client->security.ready&&client->security.peers_ready&&client->security.authorized;
        next.peer_count=client->security.observed_peer_count;
        next.peer_capacity=ML_POLICY_MAX_PEERS;
        next.capacity_exceeded=client->security.capacity_exceeded;
        next.wg_out_packets=client->security.wg_out_packets;
        next.wg_out_dropped=client->security.wg_out_dropped;
        next.wg_in_packets=client->security.wg_in_packets;
        next.wg_in_dropped=client->security.wg_in_dropped;
        next.wg_rx_packets=client->security.wg_rx_packets;
        next.wg_netif_ip=client->security.wg_netif_ip;
        next.wg_last_out_src=client->security.wg_last_out_src;
        next.wg_last_out_dst=client->security.wg_last_out_dst;
        next.wg_sessions=client->security.wg_sessions;
        next.wg_netif_mask=client->security.wg_netif_mask;
        next.wg_lookup_misses=client->security.wg_lookup_misses;
        next.wg_derp_enqueue=client->security.wg_derp_enqueue;
        next.wg_derp_enqueue_fail=client->security.wg_derp_enqueue_fail;
        next.wg_udp_tx=client->security.wg_udp_tx;
        next.wg_peer_count=client->security.wg_peer_count;
        next.wg_netif_index=client->security.wg_netif_index;
        next.wg_netif_up=client->security.wg_netif_up;
        next.wg_netif_link_up=client->security.wg_netif_link_up;
        next.derp_home_connected=client->security.derp_home_connected;
        next.derp_home_region=client->security.derp_home_region;
        next.derp_remote_connected=client->security.derp_remote_connected;
        next.derp_frames_tx=client->security.derp_frames_tx;
        next.derp_frames_rx=client->security.derp_frames_rx;
        next.derp_connect_failures=client->security.derp_connect_failures;
        next.derp_capacity_drops=client->security.derp_capacity_drops;
        next.derp_queue_drops=client->security.derp_queue_drops;
        next.derp_route_drops=client->security.derp_route_drops;
        if(next.capacity_exceeded)next.last_error=ESP_ERR_INVALID_SIZE;
        else if(client->security.peer_install_failed)next.last_error=ESP_ERR_NO_MEM;
        next.expires_at=client->security.expiry;
        microlink_ip_to_str(client->vpn_ip,next.ip);
        if(client->security.expired||(next.expires_at>0&&time(NULL)>=next.expires_at))next.state=ALARM_TAILNET_EXPIRED;
        else if(client->security.auth_pending)next.state=ALARM_TAILNET_AUTH_REQUIRED;
        else if(microlink_is_connected(client))next.state=next.acl_ready?ALARM_TAILNET_CONNECTED:ALARM_TAILNET_BLOCKED;
        else next.state=ALARM_TAILNET_CONNECTING;
        xSemaphoreGive(client->security.lock);
    }
    if(next.last_error!=ESP_OK)next.state=ALARM_TAILNET_BLOCKED;
    taskENTER_CRITICAL(&snapshot_lock);snapshot=next;taskEXIT_CRITICAL(&snapshot_lock);
}
static esp_err_t stop_client(void) {
    if(!client)return ESP_OK;
    ml_security_close(client);
    esp_err_t err=microlink_stop(client);
    if(err!=ESP_OK)return err; /* Preserve live context on timeout. */
    microlink_destroy(client);client=NULL;return ESP_OK;
}
static esp_err_t start_client(void) {
    if(client)return ESP_ERR_INVALID_STATE;
    microlink_config_t config={.device_name=hostname,.auth_key=NULL,.enable_derp=true,.max_peers=ML_MAX_PEERS};
    client=microlink_init(&config);if(!client)return ESP_FAIL;
    esp_err_t err=microlink_start(client);
    if(err!=ESP_OK){esp_err_t stopped=stop_client();if(stopped!=ESP_OK)return stopped;}
    return err;
}
static esp_err_t rotate_node(void) {
    if(!client)return ESP_ERR_INVALID_STATE;
    uint8_t old_pub[32],new_private[32];memcpy(old_pub,client->wg_public_key,32);
    esp_err_t err=stop_client();if(err!=ESP_OK)return err;
    esp_fill_random(new_private,sizeof(new_private));new_private[0]&=248;new_private[31]&=127;new_private[31]|=64;
    nvs_handle_t nvs;err=nvs_open("microlink",NVS_READWRITE,&nvs);
    if(err==ESP_OK){
        err=nvs_set_blob(nvs,"old_node_pub",old_pub,32);
        if(err==ESP_OK)err=nvs_set_blob(nvs,"wg_private",new_private,32);
        if(err==ESP_OK)err=nvs_commit(nvs);
        nvs_close(nvs);
    }
    volatile uint8_t *wipe=new_private;for(unsigned i=0;i<32;i++)wipe[i]=0;
    return err;
}
static void worker(void *unused) {
    (void)unused;command_t command;
    for(;;) {
        if(xQueueReceive(commands,&command,pdMS_TO_TICKS(250))==pdTRUE) {
            last_error=ESP_OK;
            switch(command.op) {
            case START:
                if(client)last_error=ESP_ERR_INVALID_STATE;
                else {memcpy(hostname,command.name,sizeof(hostname));want_running=true;}
                break;
            case STOP:
                want_running=false;last_error=stop_client();break;
            case REAUTH:
                /* Long shutdown happens here, never in an HTTP request. */
                taskENTER_CRITICAL(&snapshot_lock);
                snapshot.state=ALARM_TAILNET_CONNECTING;snapshot.auth_url[0]=0;
                taskEXIT_CRITICAL(&snapshot_lock);
                if(client)last_error=rotate_node();
                else last_error=hostname[0]?ESP_OK:ESP_ERR_INVALID_STATE;
                /* A prior allocation/network startup failure must be retryable.
                 * With no live client, reuse the existing NVS identity. */
                want_running=last_error==ESP_OK;break;
            }
        }
        /* SNTP may become ready after start was requested during AP/WiFi setup. */
        if(want_running&&!client&&last_error==ESP_OK&&time(NULL)>=1700000000)last_error=start_client();
        publish();
    }
}
static esp_err_t ensure_worker(void) {
    int state=__atomic_load_n(&initialized,__ATOMIC_ACQUIRE);
    if(state==2)return ESP_OK;
    int expected=0;
    if(!__atomic_compare_exchange_n(&initialized,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return ESP_ERR_INVALID_STATE;
    commands=xQueueCreate(4,sizeof(command_t));
    if(!commands){__atomic_store_n(&initialized,0,__ATOMIC_RELEASE);return ESP_ERR_NO_MEM;}
    if(xTaskCreate(worker,"alarm_tailnet",8192,NULL,4,NULL)!=pdPASS){vQueueDelete(commands);commands=NULL;__atomic_store_n(&initialized,0,__ATOMIC_RELEASE);return ESP_ERR_NO_MEM;}
    __atomic_store_n(&initialized,2,__ATOMIC_RELEASE);return ESP_OK;
}
esp_err_t alarm_tailnet_start(const char *name) {
    if(!name||!name[0]||strlen(name)>=sizeof(hostname))return ESP_ERR_INVALID_ARG;
    esp_err_t err=ensure_worker();if(err!=ESP_OK)return err;
    command_t cmd={.op=START};strcpy(cmd.name,name);
    return xQueueSend(commands,&cmd,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
esp_err_t alarm_tailnet_get_status(alarm_tailnet_status_t *out) {
    if(!out)return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&snapshot_lock);*out=snapshot;taskEXIT_CRITICAL(&snapshot_lock);return ESP_OK;
}
esp_err_t alarm_tailnet_reauth(void) {
    if(__atomic_load_n(&initialized,__ATOMIC_ACQUIRE)!=2)return ESP_ERR_INVALID_STATE;
    command_t cmd={.op=REAUTH};return xQueueSend(commands,&cmd,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
esp_err_t alarm_tailnet_stop(void) {
    if(__atomic_load_n(&initialized,__ATOMIC_ACQUIRE)!=2)return ESP_OK;
    command_t cmd={.op=STOP};return xQueueSend(commands,&cmd,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
