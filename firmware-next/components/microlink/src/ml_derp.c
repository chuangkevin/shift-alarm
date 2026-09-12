#include "esp_crt_bundle.h"
/**
 * @file ml_derp.c
 * @brief Unified DERP I/O Task + Connection Management
 *
 * Single task owns BOTH reading and writing on home + two cached region TLS connections.
 * This eliminates the need for a TLS mutex since only one task touches
 * the SSL context. Matches v1's single-threaded DERP model.
 *
 * Architecture:
 * - Poll for incoming DERP frames (TLS read) every iteration
 * - Drain TX queue between reads (TLS write)
 * - No mutex needed — single task owns the SSL context exclusively
 *
 * Backpressure strategy (from tailscaled):
 * When queue is full, dequeue oldest packet and retry up to 3 times.
 * If still full, drop the new packet.
 *
 * Reference: tailscale/wgengine/magicsock/derp.go (runDerpWriter)
 *            tailscale/derp/derphttp/derphttp_client.go
 */

#include "microlink_internal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/error.h"
#include "nacl_box.h"
#include "ml_derp_stream.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>

static const char *TAG = "ml_derp";

/* Timeout for DERP connection handshake operations */
#define DERP_CONNECT_TIMEOUT_MS  10000

/* ============================================================================
 * Custom BIO callbacks for non-blocking TLS I/O
 *
 * These wrap lwIP recv/send with guaranteed timeout behavior.
 * We don't trust mbedtls_net_recv_timeout on lwIP because lwIP's select()
 * can sometimes block indefinitely on ESP32.
 * ========================================================================== */

/**
 * Custom recv with timeout for mbedtls BIO.
 * Uses SO_RCVTIMEO on the socket as the timeout mechanism (simpler than select).
 * Returns bytes read, or MBEDTLS_ERR_SSL_TIMEOUT, MBEDTLS_ERR_SSL_WANT_READ.
 */
static int ml_derp_bio_recv_timeout(void *ctx, unsigned char *buf, size_t len,
                                      uint32_t timeout) {
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    /* Set SO_RCVTIMEO to the requested timeout.
     * If timeout is 0 (mbedTLS default = "no timeout"), use 10s as a sane
     * default to avoid indefinite blocking on AT sockets. */
    uint32_t effective_timeout = (timeout > 0) ? timeout : DERP_CONNECT_TIMEOUT_MS;
    struct timeval tv;
    tv.tv_sec = effective_timeout / 1000;
    tv.tv_usec = (effective_timeout % 1000) * 1000;
    ml_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int ret = (int)ml_read_sock(fd, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_TIMEOUT;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        if (errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return ret;
}

/**
 * Custom send for mbedtls BIO.
 */
static int ml_derp_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    int ret = (int)ml_write_sock(fd, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        if (errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return ret;
}

/* DERP frame types */
#define DERP_FRAME_SERVER_KEY   0x01
#define DERP_FRAME_CLIENT_INFO  0x02
#define DERP_FRAME_SERVER_INFO  0x03
#define DERP_FRAME_SEND_PACKET  0x04
#define DERP_FRAME_RECV_PACKET  0x05
#define DERP_FRAME_KEEP_ALIVE   0x06
#define DERP_FRAME_NOTE_PREFERRED 0x07
#define DERP_FRAME_PEER_GONE    0x08
#define DERP_FRAME_PING         0x12
#define DERP_FRAME_PONG         0x13

/* DISCO magic bytes: "TS" + sparkles emoji UTF-8 */
static const uint8_t DISCO_MAGIC[6] = { 'T', 'S', 0xf0, 0x9f, 0x92, 0xac };

/* ============================================================================
 * TLS Read/Write Helpers
 * ========================================================================== */

/**
 * Read exactly `len` bytes via TLS with timeout and WANT_READ retry.
 * Returns number of bytes read on success, -1 on error, -2 on timeout.
 */
static int derp_tls_read_all(ml_derp_conn_t *conn, uint8_t *data, size_t len, int timeout_ms) {
    size_t received = 0;
    uint64_t start_ms = ml_get_time_ms();

    while (received < len) {
        if(conn->events&&(xEventGroupGetBits(conn->events)&ML_EVT_SHUTDOWN_REQUEST))return -1;
        if (timeout_ms > 0 && (ml_get_time_ms() - start_ms) > (uint64_t)timeout_ms) {
            ESP_LOGW(TAG, "derp_tls_read_all timeout (%d/%d bytes in %dms)",
                     (int)received, (int)len, timeout_ms);
            return -2;
        }

        int ret = mbedtls_ssl_read(&conn->ssl, data + received, len - received);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                ESP_LOGW(TAG, "DERP server closed connection");
                return -1;
            }
            ESP_LOGE(TAG, "TLS read failed: -0x%04x", -ret);
            return -1;
        }
        if (ret == 0) {
            ESP_LOGW(TAG, "TLS connection closed by peer (%d/%d bytes)",
                     (int)received, (int)len);
            return -1;
        }
        received += ret;
    }
    return (int)received;
}

/**
 * Read a DERP frame header (5 bytes: type + 4-byte BE length) with timeout.
 */
static esp_err_t derp_recv_frame_header(ml_derp_conn_t *conn, uint8_t *type,
                                          uint32_t *len, int timeout_ms) {
    uint8_t header[5];
    int ret = derp_tls_read_all(conn, header, 5, timeout_ms);
    if (ret < 0) {
        return (ret == -2) ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    *type = header[0];
    *len = ((uint32_t)header[1] << 24) |
           ((uint32_t)header[2] << 16) |
           ((uint32_t)header[3] << 8) |
           (uint32_t)header[4];

    return ESP_OK;
}

/**
 * Write exactly `len` bytes via TLS with WANT_WRITE retry.
 * Returns bytes written on success, -1 on error.
 * No mutex needed — called only from the DERP I/O task.
 */
static int derp_tls_write_all(ml_derp_conn_t *conn, const uint8_t *data, size_t len) {
    size_t written = 0;
    int retries = 0;
    const int max_retries = 50;  /* 50 * 10ms = 500ms max */

    while (written < len) {
        if(conn->events&&(xEventGroupGetBits(conn->events)&ML_EVT_SHUTDOWN_REQUEST))return -1;
        int ret = mbedtls_ssl_write(&conn->ssl, data + written, len - written);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(10));
                if (++retries > max_retries) {
                    ESP_LOGW(TAG, "TLS write timeout after %d retries", retries);
                    return -1;
                }
                continue;
            }
            ESP_LOGE(TAG, "TLS write failed: -0x%04x", -ret);
            return -1;
        }
        if(ret==0)return -1;
        written += ret;
        retries = 0;
    }
    return (int)written;
}

/* Write a complete DERP frame via TLS */
static int derp_write_frame(ml_derp_conn_t *conn, uint8_t type,
                             const uint8_t *payload, uint32_t len) {
    /* 5-byte header: 1 type + 4 length (big-endian) */
    uint8_t header[5];
    header[0] = type;
    header[1] = (len >> 24) & 0xFF;
    header[2] = (len >> 16) & 0xFF;
    header[3] = (len >> 8) & 0xFF;
    header[4] = len & 0xFF;

    if (derp_tls_write_all(conn, header, 5) < 0) return -1;

    if (len > 0 && payload) {
        if (derp_tls_write_all(conn, payload, len) < 0) return -1;
    }
    return 0;
}

/* Send a packet to a peer via DERP */
static int derp_send_packet(ml_derp_conn_t *conn, const uint8_t *dest_key,
                              const uint8_t *data, size_t len) {
    /* SendPacket frame: 32-byte dest key + payload */
    size_t frame_len = 32 + len;
    uint8_t *frame = malloc(frame_len);
    if (!frame) return -1;

    memcpy(frame, dest_key, 32);
    memcpy(frame + 32, data, len);

    int ret = derp_write_frame(conn, DERP_FRAME_SEND_PACKET, frame, frame_len);
    if (ret < 0) {
        ESP_LOGW(TAG, "derp_send_packet FAILED: dest=%02x%02x%02x%02x len=%d",
                 dest_key[0], dest_key[1], dest_key[2], dest_key[3], (int)len);
    }
    free(frame);
    return ret;
}

/* ============================================================================
 * DERP Frame Reading and Dispatch (runs on DERP I/O task)
 * ========================================================================== */

/* Packet classification */
typedef enum {
    PKT_DISCO,
    PKT_STUN,
    PKT_WIREGUARD,
    PKT_UNKNOWN,
} pkt_type_t;

static pkt_type_t classify_packet(const uint8_t *data, size_t len) {
    if (len >= 20 && (data[0] == 0x00 || data[0] == 0x01) && data[1] == 0x01) {
        return PKT_STUN;
    }
    if (len >= 62 && memcmp(data, DISCO_MAGIC, 6) == 0) {
        return PKT_DISCO;
    }
    if (len >= 4) {
        return PKT_WIREGUARD;
    }
    return PKT_UNKNOWN;
}

static void route_derp_packet(microlink_t *ml, ml_derp_conn_t *conn, uint8_t *data, size_t len,
                               const uint8_t *src_pubkey) {
    /* Replies may reuse the exact live session on which the peer reached us.
     * This is a transport hint only: WG authentication and ACL still decide
     * whether its payload is accepted. Never use a hint after that TLS closes. */
    xSemaphoreTake(ml->security.lock,portMAX_DELAY);
    for(unsigned i=0;i<ml->security.peer_count;i++) {
        ml_allowed_peer_t *p=&ml->security.peers[i];
        if(!memcmp(p->public_key,src_pubkey,32)) {
            p->derp_recv_region=conn->region;p->derp_recv_ms=ml_get_time_ms();break;
        }
    }
    xSemaphoreGive(ml->security.lock);
    pkt_type_t type = classify_packet(data, len);

    ml_rx_packet_t pkt = {
        .data = data,
        .len = len,
        .via_derp = true,
    };
    memcpy(pkt.src_pubkey, src_pubkey, 32);

    QueueHandle_t target = (type == PKT_DISCO) ? ml->disco_rx_queue : ml->wg_rx_queue;
    if (xQueueSend(target, &pkt, 0) != pdTRUE) {
        free(data);
    }
}

/* Dispatch a received DERP frame */
static void dispatch_derp_frame(microlink_t *ml, ml_derp_conn_t *conn, uint8_t frame_type,
                                 uint8_t *src_key, uint8_t *payload, size_t payload_len) {
    switch (frame_type) {
    case DERP_FRAME_RECV_PACKET:
        if (payload) {
            ESP_LOGI(TAG, "DERP RecvPacket: %d bytes from %02x%02x%02x%02x, hdr=%02x",
                     (int)payload_len,
                     src_key[0], src_key[1], src_key[2], src_key[3],
                     payload_len > 0 ? payload[0] : 0xFF);
            route_derp_packet(ml, conn, payload, payload_len, src_key);
            return;  /* payload ownership transferred */
        }
        break;

    case DERP_FRAME_KEEP_ALIVE:
        ESP_LOGD(TAG, "DERP KeepAlive received");
        break;

    case DERP_FRAME_PING:
        /* Echo ping data back as PONG directly (single-threaded, safe to write) */
        if (payload && payload_len > 0) {
            ESP_LOGD(TAG, "DERP PING received, sending PONG");
            derp_write_frame(conn, DERP_FRAME_PONG, payload, payload_len);
        }
        break;

    case DERP_FRAME_PONG:
        ESP_LOGD(TAG, "DERP PONG received");
        break;

    case DERP_FRAME_PEER_GONE:
        if (payload && payload_len >= 32) {
            ESP_LOGI(TAG, "DERP PeerGone: %02x%02x%02x%02x (len=%d)",
                     payload[0], payload[1], payload[2], payload[3],
                     (int)payload_len);
        }
        break;

    default:
        ESP_LOGD(TAG, "DERP frame type 0x%02x ignored (%d bytes)",
                 frame_type, (int)payload_len);
        break;
    }

    if (payload) free(payload);
}

/**
 * Try to read one DERP frame.
 * Uses mbedtls recv_timeout (100ms) so ssl_read never blocks indefinitely.
 * Returns: 1 = frame read and dispatched, 0 = timeout (no data), <0 = error
 */
static int derp_header_read(void *ctx,uint8_t *buf,size_t length) {
    ml_derp_conn_t *conn=ctx;int n=mbedtls_ssl_read(&conn->ssl,buf,length);
    if(n==MBEDTLS_ERR_SSL_WANT_READ||n==MBEDTLS_ERR_SSL_WANT_WRITE||n==MBEDTLS_ERR_SSL_TIMEOUT)return -2;
    return n<0?-1:n;
}
static uint64_t derp_header_now(void *ctx){(void)ctx;return ml_get_time_ms();}
static void derp_header_idle(void *ctx){(void)ctx;vTaskDelay(pdMS_TO_TICKS(5));}
static int poll_derp_read(microlink_t *ml, ml_derp_conn_t *conn) {
    if (!conn->connected || conn->sockfd < 0) return -1;

    /* Read 5-byte frame header.
     * SO_RCVTIMEO=100ms ensures read() returns within 100ms if no data. */
    uint8_t header[5];
    int n=ml_derp_read_header(conn,derp_header_read,derp_header_now,derp_header_idle,header,2000);
    if(n<=0)return n; /* 0 only means idle before any header; EOF is fatal. */

    uint8_t frame_type = header[0];
    uint32_t len = ((uint32_t)header[1] << 24) | ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 8) | header[4];

    uint8_t src_key[32] = {0};
    uint8_t *payload = NULL;
    size_t payload_len = 0;

    if (len == 0) {
        dispatch_derp_frame(ml, conn, frame_type, src_key, NULL, 0);
        return 1;
    }

    if (len > 65536) {
        ESP_LOGW(TAG, "DERP frame too large: %lu", (unsigned long)len);
        return -1;
    }

    /* Read frame payload - we already got the header so payload should follow.
     * Use longer timeout (2s) since we KNOW data is coming. */
    uint8_t *buf = ml_psram_malloc(len);
    if (!buf) return -1;

    size_t total_read = 0;
    uint64_t payload_start = ml_get_time_ms();
    while (total_read < len) {
        if(xEventGroupGetBits(ml->events)&ML_EVT_SHUTDOWN_REQUEST){free(buf);return -1;}
        /* Safety timeout: 5 seconds for payload */
        if (ml_get_time_ms() - payload_start > 5000) {
            ESP_LOGW(TAG, "DERP payload timeout at %d/%lu bytes",
                     (int)total_read, (unsigned long)len);
            free(buf);
            return -1;
        }
        n = mbedtls_ssl_read(&conn->ssl, buf + total_read, len - total_read);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
            n == MBEDTLS_ERR_SSL_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (n <= 0) {
            ESP_LOGW(TAG, "DERP payload read error: %d (0x%04x) at %d/%lu bytes",
                     n, n < 0 ? -n : 0, (int)total_read, (unsigned long)len);
            free(buf);
            return -1; /* EOF after a header is a broken frame, never idle. */
        }
        total_read += n;
    }

    /* For RecvPacket (0x05): first 32 bytes are sender's public key */
    if (frame_type == DERP_FRAME_RECV_PACKET && len > 32) {
        memcpy(src_key, buf, 32);
        payload = malloc(len - 32);
        if (payload) {
            memcpy(payload, buf + 32, len - 32);
            payload_len = len - 32;
        }
        free(buf);
    } else {
        payload = buf;
        payload_len = len;
    }

    dispatch_derp_frame(ml, conn, frame_type, src_key, payload, payload_len);
    return 1;
}

/* ============================================================================
 * DERP TX Queue Processing
 * ========================================================================== */

/* Queue a packet for DERP TX with backpressure */
esp_err_t ml_derp_queue_send(microlink_t *ml, const uint8_t *dest_key,
                              const uint8_t *data, size_t len) {
    if (!ml || !dest_key || !data || len == 0) return ESP_ERR_INVALID_ARG;

    uint8_t *pkt_data = malloc(len);
    if (!pkt_data) return ESP_ERR_NO_MEM;
    memcpy(pkt_data, data, len);

    ml_derp_tx_item_t item = {
        .data = pkt_data,
        .len = len,
        .frame_type = DERP_FRAME_SEND_PACKET,
        .demand = len>=4 && data[0]>=1 && data[0]<=4 && data[1]==0 && data[2]==0 && data[3]==0,
    };
    memcpy(item.dest_pubkey, dest_key, 32);

    /* WG handshake packets (type 1=init, 2=response) get priority — front of queue.
     * This ensures handshake responses aren't delayed behind DISCO pings. */
    bool is_wg_handshake = (len >= 4 && (data[0] == 0x01 || data[0] == 0x02));

    /* Try to send to queue */
    if ((is_wg_handshake ? xQueueSendToFront(ml->derp_tx_queue, &item, 0)
                         : xQueueSend(ml->derp_tx_queue, &item, 0)) == pdTRUE) {
        return ESP_OK;
    }

    /* Queue full - backpressure: drop oldest, retry up to 3 times */
    for (int i = 0; i < 3; i++) {
        ml_derp_tx_item_t dropped;
        if (xQueueReceive(ml->derp_tx_queue, &dropped, 0) == pdTRUE) {
            free(dropped.data);  /* Drop oldest */
            xSemaphoreTake(ml->security.lock,portMAX_DELAY);ml->security.derp_queue_drops++;xSemaphoreGive(ml->security.lock);
        }
        if (xQueueSend(ml->derp_tx_queue, &item, 0) == pdTRUE) {
            return ESP_OK;
        }
    }

    /* Still full after 3 attempts, drop new packet */
    free(pkt_data);
    xSemaphoreTake(ml->security.lock,portMAX_DELAY);ml->security.derp_queue_drops++;xSemaphoreGive(ml->security.lock);
    return ESP_ERR_TIMEOUT;
}

/* ============================================================================
 * Unified DERP I/O Task
 * ========================================================================== */

static void derp_tls_cleanup(microlink_t *ml,ml_derp_conn_t *conn);
static esp_err_t derp_connect_session(microlink_t *ml,ml_derp_conn_t *conn,uint16_t region);

static uint16_t derp_home_region(microlink_t *ml) {
    uint16_t region=__atomic_load_n(&ml->derp_home_region,__ATOMIC_ACQUIRE);
    return region?region:ML_DERP_REGION;
}
static bool derp_region_is_live(microlink_t *ml,uint16_t region) {
    if(ml->derp.connected&&ml->derp.region==region)return true;
    for(unsigned i=0;i<ML_DERP_REMOTE_SLOTS;i++)
        if(ml->derp_remote[i].connected&&ml->derp_remote[i].region==region)return true;
    return false;
}
static uint16_t derp_destination_region(microlink_t *ml,const uint8_t key[32]) {
    uint16_t region=0;uint64_t now=ml_get_time_ms();
    xSemaphoreTake(ml->security.lock,portMAX_DELAY);
    for(unsigned i=0;i<ml->security.peer_count;i++) {
        ml_allowed_peer_t *peer=&ml->security.peers[i];
        if(memcmp(peer->public_key,key,32))continue;
        region=peer->derp_region;
        if(peer->derp_recv_region && now>=peer->derp_recv_ms && now-peer->derp_recv_ms<30000 &&
           derp_region_is_live(ml,peer->derp_recv_region))region=peer->derp_recv_region;
        break;
    }
    xSemaphoreGive(ml->security.lock);
    return region;
}
static void derp_publish(microlink_t *ml) {
    unsigned active=0;
    for(unsigned i=0;i<ML_DERP_REMOTE_SLOTS;i++)if(ml->derp_remote[i].connected)active++;
    xSemaphoreTake(ml->security.lock,portMAX_DELAY);
    ml->security.derp_home_region=ml->derp.region;
    ml->security.derp_home_connected=ml->derp.connected;
    ml->security.derp_remote_connected=active;
    xSemaphoreGive(ml->security.lock);
}
#define DERP_COUNT(field) do {xSemaphoreTake(ml->security.lock,portMAX_DELAY);ml->security.field++;xSemaphoreGive(ml->security.lock);} while(0)

void ml_derp_tx_task(void *arg) {
    microlink_t *ml=arg;
    bool want_home=false;
    ml_derp_cache_slot_t home_retry={0};
    while(!(xEventGroupGetBits(ml->events)&ML_EVT_SHUTDOWN_REQUEST)) {
        EventBits_t bits=xEventGroupGetBits(ml->events);
        uint64_t now=ml_get_time_ms();
        uint16_t home=derp_home_region(ml);
        if(home_retry.region!=home) {
            /* A new advertised home must change the pinned listener too.
             * Never use an old home TLS session for the new home region. */
            home_retry=(ml_derp_cache_slot_t){.region=home};
            if(ml->derp.region!=home)derp_tls_cleanup(ml,&ml->derp);
            for(unsigned i=0;i<ML_DERP_REMOTE_SLOTS;i++) {
                if(ml->derp_cache[i].region==home) {
                    derp_tls_cleanup(ml,&ml->derp_remote[i]);
                    ml->derp_cache[i]=(ml_derp_cache_slot_t){0};
                }
            }
        }
        if(bits&ML_EVT_DERP_CONNECT_REQ) {
            xEventGroupClearBits(ml->events,ML_EVT_DERP_CONNECT_REQ);want_home=true;
        }
        if(bits&ML_EVT_DERP_RECONNECT) {
            xEventGroupClearBits(ml->events,ML_EVT_DERP_RECONNECT);
            derp_tls_cleanup(ml,&ml->derp);want_home=true;
        }
        if(want_home&&!ml->derp.connected&&now>=home_retry.retry_after_ms) {
            if(ml_derp_connect(ml)==ESP_OK)home_retry=(ml_derp_cache_slot_t){.region=home};
            else {ml_derp_cache_failure(&home_retry,ml_get_time_ms());DERP_COUNT(derp_connect_failures);}
        }
        /* At most three TLS sockets: pinned home plus two on-demand regions.
         * Background DISCO never opens a connection for every idle peer. */
        for(unsigned n=0;n<8;n++) {
            if(xEventGroupGetBits(ml->events)&ML_EVT_SHUTDOWN_REQUEST)break;
            ml_derp_tx_item_t item;
            if(xQueueReceive(ml->derp_tx_queue,&item,0)!=pdTRUE)break;
            uint16_t region=derp_destination_region(ml,item.dest_pubkey);
            ml_derp_conn_t *conn=NULL;
            now=ml_get_time_ms();
            if(!region)DERP_COUNT(derp_route_drops);
            else if(region==home)conn=&ml->derp;
            else {
                int idx=ml_derp_cache_find(ml->derp_cache,region,now,item.demand);
                if(idx<0) {if(item.demand)DERP_COUNT(derp_capacity_drops);}
                else {
                    conn=&ml->derp_remote[idx];
                    ml_derp_cache_slot_t *slot=&ml->derp_cache[idx];
                    if(slot->region!=region) {
                        derp_tls_cleanup(ml,conn);
                        *slot=(ml_derp_cache_slot_t){.region=region,.last_used_ms=now};
                    }
                    if(item.demand)slot->last_used_ms=now;
                    if(!conn->connected&&item.demand&&now>=slot->retry_after_ms) {
                        if(derp_connect_session(ml,conn,region)==ESP_OK) {
                            slot->failures=0;slot->retry_after_ms=0;
                        } else {ml_derp_cache_failure(slot,ml_get_time_ms());DERP_COUNT(derp_connect_failures);}
                    }
                }
            }
            if(conn&&conn->connected&&conn->region==region) {
                if(derp_send_packet(conn,item.dest_pubkey,item.data,item.len)<0) {
                    derp_tls_cleanup(ml,conn);DERP_COUNT(derp_connect_failures);
                    if(conn==&ml->derp)ml_derp_cache_failure(&home_retry,ml_get_time_ms());
                    else ml_derp_cache_failure(&ml->derp_cache[conn-ml->derp_remote],ml_get_time_ms());
                } else DERP_COUNT(derp_frames_tx);
            }
            free(item.data);
        }
        /* RX and PONG writes use the same owner and same TLS session. */
        for(unsigned i=0;i<=ML_DERP_REMOTE_SLOTS;i++) {
            ml_derp_conn_t *conn=i?&ml->derp_remote[i-1]:&ml->derp;
            if(!conn->connected)continue;
            for(unsigned burst=0;burst<4;burst++) {
                int result=poll_derp_read(ml,conn);
                if(result==0)break;
                if(result<0) {
                    derp_tls_cleanup(ml,conn);DERP_COUNT(derp_connect_failures);
                    if(i)ml_derp_cache_failure(&ml->derp_cache[i-1],ml_get_time_ms());
                    else ml_derp_cache_failure(&home_retry,ml_get_time_ms());
                    break;
                }
                conn->last_recv_ms=ml_get_time_ms();DERP_COUNT(derp_frames_rx);
            }
        }
        derp_publish(ml);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    /* All contexts are freed before acknowledging task exit. Cleanup is
     * idempotent so stop/destroy may repeat it after the acknowledgement. */
    ml_derp_disconnect(ml);derp_publish(ml);
    xEventGroupSetBits(ml->events,ML_EVT_DERP_EXIT);
    vTaskDelete(NULL);
}

/* Single owner; TLS allocations must be released even after socket failure. */
static void derp_tls_cleanup(microlink_t *ml, ml_derp_conn_t *conn) {
    conn->connected=false;
    if(conn==&ml->derp)xEventGroupClearBits(ml->events,ML_EVT_DERP_CONNECTED);
    if(conn->sockfd>=0){ml_close_sock(conn->sockfd);conn->sockfd=-1;}
    if(conn->tls_initialized){
        mbedtls_ssl_free(&conn->ssl);
        mbedtls_ssl_config_free(&conn->ssl_conf);
        mbedtls_ctr_drbg_free(&conn->ctr_drbg);
        mbedtls_entropy_free(&conn->entropy);
        conn->tls_initialized=false;
    }
}
static esp_err_t derp_connect_once(microlink_t *ml, ml_derp_conn_t *conn, uint16_t region) {
    /* Copy the requested region's preferred node from the control map.
     * Never silently send a foreign-region packet to our home relay. */
    char derp_host[128]={0};
    int derp_port=ML_DERP_PORT;
    xSemaphoreTake(ml->security.lock,portMAX_DELAY);
    for(int i=0;i<ml->derp_region_count;i++) {
        if(ml->derp_regions[i].region_id!=region)continue;
        for(int j=0;j<ml->derp_regions[i].node_count;j++) {
            ml_derp_node_t *node=&ml->derp_regions[i].nodes[j];
            if(node->stun_only||!node->hostname[0])continue;
            snprintf(derp_host,sizeof(derp_host),"%s",node->hostname);
            if(node->derp_port)derp_port=node->derp_port;
            break;
        }
        break;
    }
    xSemaphoreGive(ml->security.lock);
    if(!derp_host[0])return ESP_ERR_NOT_FOUND;

    int64_t t_derp_start = esp_timer_get_time();

    ESP_LOGI(TAG, "Connecting to DERP %s:%d (region %d)",
             derp_host, derp_port, region);

    /* DNS resolve — accept IPv4 or IPv6 (carrier may be IPv6-only) */
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", derp_port);

    if (ml_getaddrinfo(derp_host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "DNS resolve failed for %s", derp_host);
        return ESP_FAIL;
    }

    int64_t t_derp_dns = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP DNS: %lld ms", (t_derp_dns - t_derp_start) / 1000);

    if(xEventGroupGetBits(ml->events)&ML_EVT_SHUTDOWN_REQUEST){ml_freeaddrinfo(res);return ESP_ERR_INVALID_STATE;}

    /* TCP connect — use address family from DNS result */
    int sock = ml_socket(res->ai_family, SOCK_STREAM, 0);
    if (sock < 0) {
        ml_freeaddrinfo(res);
        return ESP_FAIL;
    }

    conn->sockfd = sock; /* cleanup owns fd from this point, including TLS failures. */

    /* Set connect timeout */
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    ml_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ml_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (ml_connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        ESP_LOGE(TAG, "TCP connect failed: %d", errno);
        ml_freeaddrinfo(res);
        return ESP_FAIL;
    }
    ml_freeaddrinfo(res);

    int64_t t_derp_tcp = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP TCP connect: %lld ms", (t_derp_tcp - t_derp_dns) / 1000);

    /* TLS setup */

    if(mbedtls_ctr_drbg_seed(&conn->ctr_drbg, mbedtls_entropy_func,
                           &conn->entropy, NULL, 0)!=0)return ESP_FAIL;

    if(mbedtls_ssl_config_defaults(&conn->ssl_conf,
                                 MBEDTLS_SSL_IS_CLIENT,
                                 MBEDTLS_SSL_TRANSPORT_STREAM,
                                 MBEDTLS_SSL_PRESET_DEFAULT)!=0)return ESP_FAIL;
    mbedtls_ssl_conf_authmode(&conn->ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    if(esp_crt_bundle_attach(&conn->ssl_conf)!=ESP_OK){return ESP_FAIL;}
    mbedtls_ssl_conf_rng(&conn->ssl_conf, mbedtls_ctr_drbg_random, &conn->ctr_drbg);
    mbedtls_ssl_conf_read_timeout(&conn->ssl_conf, DERP_CONNECT_TIMEOUT_MS);

    if(mbedtls_ssl_setup(&conn->ssl, &conn->ssl_conf)!=0 ||
       mbedtls_ssl_set_hostname(&conn->ssl, derp_host)!=0){return ESP_FAIL;}
    /* Store socket fd BEFORE setting bio.
     * Use custom BIO callbacks that route through ml_read_sock/ml_write_sock,
     * which transparently support both lwIP and AT socket backends.
     * Timeout is handled via SO_RCVTIMEO. */
    conn->sockfd = sock;
    mbedtls_ssl_set_bio(&conn->ssl, &conn->sockfd,
                         ml_derp_bio_send, NULL, ml_derp_bio_recv_timeout);

    /* TLS handshake - socket has 10s SO_RCVTIMEO from connect phase. */
    int ret;
    uint64_t tls_start=ml_get_time_ms();
    while ((ret = mbedtls_ssl_handshake(&conn->ssl)) != 0) {
        if((xEventGroupGetBits(ml->events)&ML_EVT_SHUTDOWN_REQUEST)||ml_get_time_ms()-tls_start>DERP_CONNECT_TIMEOUT_MS)return ESP_ERR_TIMEOUT;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        char err_buf[128];
        mbedtls_strerror(ret, err_buf, sizeof(err_buf));
        ESP_LOGE(TAG, "TLS handshake failed: %s", err_buf);
        return ESP_FAIL;
    }

    int64_t t_derp_tls = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP TLS handshake: %lld ms", (t_derp_tls - t_derp_tcp) / 1000);
    ESP_LOGI(TAG, "TLS connected to DERP");

    /* HTTP Upgrade: GET /derp with Upgrade: DERP header */
    char upgrade_req[256];
    snprintf(upgrade_req, sizeof(upgrade_req),
             "GET /derp HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: Upgrade\r\n"
             "Upgrade: DERP\r\n"
             "\r\n",
             derp_host);

    ret = derp_tls_write_all(conn, (const uint8_t *)upgrade_req, strlen(upgrade_req));
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send HTTP upgrade");
        return ESP_FAIL;
    }

    /* Read HTTP response byte-by-byte until \r\n\r\n to avoid over-reading
     * into the DERP binary frame stream (matching v1 approach) */
    {
        uint8_t resp_buf[512];
        int resp_len = 0;
        bool found_end = false;
        uint64_t http_start = ml_get_time_ms();

        while (resp_len < (int)sizeof(resp_buf) - 1) {
            if(xEventGroupGetBits(ml->events)&ML_EVT_SHUTDOWN_REQUEST)return ESP_ERR_INVALID_STATE;
            if (ml_get_time_ms() - http_start > DERP_CONNECT_TIMEOUT_MS) {
                ESP_LOGE(TAG, "HTTP upgrade response timeout");
                return ESP_FAIL;
            }

            ret = mbedtls_ssl_read(&conn->ssl, resp_buf + resp_len, 1);
            if (ret < 0) {
                if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                    ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                ESP_LOGE(TAG, "HTTP upgrade read failed: -0x%04x", -ret);
                return ESP_FAIL;
            }
            if (ret == 0) {
                ESP_LOGE(TAG, "Connection closed during HTTP upgrade");
                return ESP_FAIL;
            }
            resp_len++;

            /* Check for \r\n\r\n */
            if (resp_len >= 4 &&
                resp_buf[resp_len - 4] == '\r' && resp_buf[resp_len - 3] == '\n' &&
                resp_buf[resp_len - 2] == '\r' && resp_buf[resp_len - 1] == '\n') {
                found_end = true;
                break;
            }
        }

        resp_buf[resp_len] = '\0';

        if (!found_end || strstr((char *)resp_buf, "101") == NULL) {
            ESP_LOGE(TAG, "DERP upgrade rejected: %.100s", resp_buf);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "HTTP 101 Switching Protocols received");
    }

    /* Match v1 exactly: O_NONBLOCK + short SO_RCVTIMEO + SO_SNDTIMEO.
     * O_NONBLOCK ensures read()/write() never block indefinitely.
     * SO_RCVTIMEO provides 100ms polling rhythm for reads.
     * SO_SNDTIMEO prevents writes from blocking too long. */
    {
        int flags = ml_fcntl(sock, F_GETFL, 0);
        if (flags >= 0) {
            ml_fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        }
        struct timeval io_tv = { .tv_sec = 0, .tv_usec = 100000 };  /* 100ms */
        ml_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &io_tv, sizeof(io_tv));
        ml_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &io_tv, sizeof(io_tv));
    }

    /* ========================================================
     * DERP Handshake: ServerKey -> ClientInfo -> ServerInfo
     * ======================================================== */

    /* Step 1: Read ServerKey frame header using reliable read helper */
    uint8_t frame_type;
    uint32_t frame_len;
    esp_err_t err = derp_recv_frame_header(conn, &frame_type, &frame_len, DERP_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read ServerKey frame header (err=%d)", err);
        return ESP_FAIL;
    }

    if (frame_type != DERP_FRAME_SERVER_KEY || frame_len < 40) {
        ESP_LOGE(TAG, "Expected ServerKey frame (0x01), got 0x%02x len=%lu",
                 frame_type, (unsigned long)frame_len);
        return ESP_FAIL;
    }

    /* Read and verify 8-byte magic */
    uint8_t magic[8];
    static const uint8_t DERP_MAGIC[8] = {0x44, 0x45, 0x52, 0x50, 0xf0, 0x9f, 0x94, 0x91};
    if (derp_tls_read_all(conn, magic, 8, DERP_CONNECT_TIMEOUT_MS) < 0) {
        ESP_LOGE(TAG, "Failed to read ServerKey magic");
        return ESP_FAIL;
    }

    if (memcmp(magic, DERP_MAGIC, 8) != 0) {
        ESP_LOGE(TAG, "Invalid DERP magic: %02x%02x%02x%02x%02x%02x%02x%02x",
                 magic[0], magic[1], magic[2], magic[3],
                 magic[4], magic[5], magic[6], magic[7]);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "DERP magic verified");

    /* Read 32-byte server public key */
    uint8_t derp_server_key[32];
    if (derp_tls_read_all(conn, derp_server_key, 32, DERP_CONNECT_TIMEOUT_MS) < 0) {
        ESP_LOGE(TAG, "Failed to read server key");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "DERP server key received (first 8): %02x%02x%02x%02x%02x%02x%02x%02x",
             derp_server_key[0], derp_server_key[1], derp_server_key[2], derp_server_key[3],
             derp_server_key[4], derp_server_key[5], derp_server_key[6], derp_server_key[7]);

    /* Skip remaining bytes if frame_len > 40 */
    if (frame_len > 40) {
        uint8_t skip_buf[64];
        size_t remaining = frame_len - 40;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(skip_buf) ? sizeof(skip_buf) : remaining;
            if (derp_tls_read_all(conn, skip_buf, chunk, DERP_CONNECT_TIMEOUT_MS) < 0) break;
            remaining -= chunk;
        }
    }

    /* Step 2: Send ClientInfo frame (type 0x02)
     * Payload: [our_nodekey(32)][nonce(24)][nacl_box(JSON)] */
    {
        const char *client_info_json = "{\"Version\":2,\"CanAckPings\":true,\"IsProber\":false}";
        size_t json_len = strlen(client_info_json);

        /* Generate random nonce */
        uint8_t nonce[NACL_BOX_NONCEBYTES];
        esp_fill_random(nonce, NACL_BOX_NONCEBYTES);

        /* Encrypt JSON with NaCl box: our WG private key -> DERP server public key */
        size_t ciphertext_len = json_len + NACL_BOX_MACBYTES;
        uint8_t *ciphertext = malloc(ciphertext_len);
        if (!ciphertext) {
            return ESP_FAIL;
        }

        if (nacl_box(ciphertext,
                     (const uint8_t *)client_info_json, json_len,
                     nonce,
                     derp_server_key,       /* recipient: DERP server */
                     ml->wg_private_key     /* sender: our WG node key */
                     ) != 0) {
            ESP_LOGE(TAG, "NaCl box encrypt failed");
            free(ciphertext);
            return ESP_FAIL;
        }

        /* Build ClientInfo frame payload: nodekey(32) + nonce(24) + ciphertext */
        size_t ci_payload_len = 32 + NACL_BOX_NONCEBYTES + ciphertext_len;
        uint8_t *ci_payload = malloc(ci_payload_len);
        if (!ci_payload) {
            free(ciphertext);
            return ESP_FAIL;
        }

        memcpy(ci_payload, ml->wg_public_key, 32);
        memcpy(ci_payload + 32, nonce, NACL_BOX_NONCEBYTES);
        memcpy(ci_payload + 32 + NACL_BOX_NONCEBYTES, ciphertext, ciphertext_len);
        free(ciphertext);

        ESP_LOGI(TAG, "DERP ClientInfo node_key=%02x%02x%02x%02x%02x%02x%02x%02x",
                 ml->wg_public_key[0], ml->wg_public_key[1],
                 ml->wg_public_key[2], ml->wg_public_key[3],
                 ml->wg_public_key[4], ml->wg_public_key[5],
                 ml->wg_public_key[6], ml->wg_public_key[7]);

        /* Send ClientInfo frame */
        if (derp_write_frame(conn, DERP_FRAME_CLIENT_INFO, ci_payload, ci_payload_len) < 0) {
            ESP_LOGE(TAG, "Failed to send ClientInfo");
            free(ci_payload);
            return ESP_FAIL;
        }
        free(ci_payload);

        ESP_LOGI(TAG, "ClientInfo sent");
    }

    /* Step 3: Read ServerInfo frame (type 0x03) */
    {
        uint8_t si_type;
        uint32_t si_len;
        err = derp_recv_frame_header(conn, &si_type, &si_len, DERP_CONNECT_TIMEOUT_MS);
        if(err!=ESP_OK||si_type!=DERP_FRAME_SERVER_INFO||si_len==0||si_len>65536)return ESP_FAIL;
        uint8_t *si_buf=ml_psram_malloc(si_len);
        if(!si_buf)return ESP_ERR_NO_MEM;
        int received=derp_tls_read_all(conn,si_buf,si_len,DERP_CONNECT_TIMEOUT_MS);
        free(si_buf);
        if(received<0)return ESP_FAIL;
    }

    /* Send NotePreferred (type 0x07): this is our preferred DERP */
    {
        uint8_t preferred = conn==&ml->derp ? 0x01 : 0x00;
        if(derp_write_frame(conn, DERP_FRAME_NOTE_PREFERRED, &preferred, 1)<0)return ESP_FAIL;
    }

    /* Switch socket to short timeout for data phase.
     * Long timeout was needed for TLS handshake, but polling must be fast. */
    {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };  /* 200ms */
        ml_setsockopt(conn->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        mbedtls_ssl_conf_read_timeout(&conn->ssl_conf, 200);
    }

    conn->connected = true;
    conn->last_recv_ms = ml_get_time_ms();
    if(conn==&ml->derp)xEventGroupSetBits(ml->events, ML_EVT_DERP_CONNECTED);

    int64_t t_derp_done = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP total: %lld ms (DNS=%lld, TCP=%lld, TLS=%lld, proto=%lld)",
             (t_derp_done - t_derp_start) / 1000,
             (t_derp_dns - t_derp_start) / 1000,
             (t_derp_tcp - t_derp_dns) / 1000,
             (t_derp_tls - t_derp_tcp) / 1000,
             (t_derp_done - t_derp_tls) / 1000);
    ESP_LOGI(TAG, "DERP handshake complete, connected");
    return ESP_OK;
}

static esp_err_t derp_connect_session(microlink_t *ml, ml_derp_conn_t *conn, uint16_t region) {
    derp_tls_cleanup(ml,conn);
    conn->region=region;conn->events=ml->events;
    mbedtls_ssl_init(&conn->ssl);
    mbedtls_ssl_config_init(&conn->ssl_conf);
    mbedtls_entropy_init(&conn->entropy);
    mbedtls_ctr_drbg_init(&conn->ctr_drbg);
    conn->tls_initialized=true;
    esp_err_t result=derp_connect_once(ml,conn,region);
    if(result!=ESP_OK)derp_tls_cleanup(ml,conn);
    return result;
}
esp_err_t ml_derp_connect(microlink_t *ml) {
    return derp_connect_session(ml,&ml->derp,derp_home_region(ml));
}
void ml_derp_disconnect(microlink_t *ml) {
    derp_tls_cleanup(ml,&ml->derp);
    for(unsigned i=0;i<ML_DERP_REMOTE_SLOTS;i++)derp_tls_cleanup(ml,&ml->derp_remote[i]);
    memset(ml->derp_cache,0,sizeof(ml->derp_cache));
    ml_derp_tx_item_t item;
    while(xQueueReceive(ml->derp_tx_queue,&item,0)==pdTRUE)free(item.data);
}
