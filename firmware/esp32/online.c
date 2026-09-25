#include "online.h"
#if PICTOCHAT_ONLINE
#include <stdatomic.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#if !PICTOCHAT_USB && !PICTOCHAT_BLE
#include "wifi_credentials.h"
#endif
#include "pictochat/relay_wire.h"

#define LINK_PORT 26711
#define GHOST_SLOT 3u
#define GHOST_AID 15u
static const char *TAG="online";
typedef struct { uint32_t boot, generation; uint8_t profile[84]; } state_t;
typedef struct {
    uint32_t seq, epoch;
    size_t len;
    uint8_t payload[RELAY_MAX_PAYLOAD];
} drawing_t;
typedef struct { uint32_t boot, seq, epoch; } ack_t;
static QueueHandle_t outgoing, incoming, acknowledgments;
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static state_t local_state, remote_state;
static bool link_up;
static uint32_t link_epoch;
#if !PICTOCHAT_USB && !PICTOCHAT_BLE
static atomic_bool have_ip;
static atomic_uint sta_ip, sta_broadcast;
#endif
static atomic_uint channel=7, ghost_count;
static uint32_t next_seq=1, ghost_generation, last_boot, last_seq;
static state_t installed;
static unsigned node;
unsigned online_node(void) { return node; }

unsigned online_channel(void) { return atomic_load(&channel); }
unsigned online_ghost_count(void) { return atomic_load(&ghost_count); }
#if !PICTOCHAT_USB && !PICTOCHAT_BLE
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e=data;
        atomic_store(&sta_ip,e->ip_info.ip.addr);
        atomic_store(&sta_broadcast,e->ip_info.ip.addr | ~e->ip_info.netmask.addr);
        atomic_store(&have_ip,true);
        ESP_LOGI(TAG,"Wi-Fi ready: " IPSTR " channel=%u node=%u",IP2STR(&e->ip_info.ip),online_channel(),online_node());
    } else if (base==WIFI_EVENT && id==WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *e=data;
        atomic_store(&channel,e->channel);
        ESP_LOGI(TAG,"Wi-Fi associated; PictoChat uses channel=%u",e->channel);
    } else if (base==WIFI_EVENT && id==WIFI_EVENT_HOME_CHANNEL_CHANGE) {
        const wifi_event_home_channel_change_t *e=data;
        atomic_store(&channel,e->new_chan);
        ESP_LOGI(TAG,"Radio home channel=%u",e->new_chan);
    } else if (base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e=data;
        atomic_store(&have_ip,false);
        ESP_LOGW(TAG,"Wi-Fi disconnected reason=%u; retrying",e->reason);
    }
}
void online_wifi_configure(void) {
    static const uint8_t mac_a[6]=ONLINE_NODE_A_MAC, mac_b[6]=ONLINE_NODE_B_MAC;
    uint8_t actual[6];
    ESP_ERROR_CHECK(esp_read_mac(actual,ESP_MAC_WIFI_STA));
    node=!memcmp(actual,mac_a,6) ? 1 : !memcmp(actual,mac_b,6) ? 2 : 0;
    if (!node) ESP_LOGE(TAG,"Board not assigned in wifi.local.json");
    ESP_ERROR_CHECK(node ? ESP_OK : ESP_ERR_INVALID_STATE);
    esp_netif_t *sta=esp_netif_create_default_wifi_sta();
    configASSERT(sta);
    ESP_ERROR_CHECK(esp_netif_set_default_netif(sta));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t cfg={0};
    cfg.sta.channel=ONLINE_WIFI_CHANNEL; // Initial scan preference; router remains authoritative.
    memcpy(cfg.sta.ssid,ONLINE_WIFI_SSID,sizeof(ONLINE_WIFI_SSID)-1);
    memcpy(cfg.sta.password,ONLINE_WIFI_PASSWORD,sizeof(ONLINE_WIFI_PASSWORD)-1);
    cfg.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable=true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL));
}
#else
void online_wifi_configure(void) {
#if defined(PICTOCHAT_NODE)
#if PICTOCHAT_NODE != 1 && PICTOCHAT_NODE != 2
#error "PICTOCHAT_NODE must be 1 (A) or 2 (B)"
#endif
    node=PICTOCHAT_NODE;
#else
    static const uint8_t mac_a[6]={0xcc,0x8d,0xa2,0xf2,0xc7,0x94};
    static const uint8_t mac_b[6]={0xcc,0x8d,0xa2,0xf2,0xcc,0x54};
    uint8_t actual[6];
    ESP_ERROR_CHECK(esp_read_mac(actual,ESP_MAC_WIFI_STA));
    // Isolation trial: COM12 hosts B, COM5 hosts A.
    node=!memcmp(actual,mac_a,6) ? 2 : !memcmp(actual,mac_b,6) ? 1 : 0;
    ESP_ERROR_CHECK(node ? ESP_OK : ESP_ERR_INVALID_STATE);
#endif
    atomic_store(&channel,node==1 ? 1 : 7);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
}
#endif
void online_message(const pictochat_event_t *event) {
    if (event->type!=PICTOCHAT_MESSAGE_RECEIVED || event->peer_slot!=0 ||
        event->length>HOST_MESSAGE_MAX) return;
    // No ghost injection produces a RECEIVE event, so messages cannot loop back.
    drawing_t *d=malloc(sizeof(*d));
    if (!d) { ESP_LOGW(TAG,"Outgoing drawing dropped: no memory"); return; }
    portENTER_CRITICAL(&lock);
    state_t state=local_state;
    bool connected=link_up;
    portEXIT_CRITICAL(&lock);
    if (!connected || !state.generation || state.generation!=event->generation) {
        free(d); ESP_LOGW(TAG,"Outgoing drawing dropped: peer link not ready"); return;
    }
    if (!next_seq) { free(d); ESP_LOGE(TAG,"Sequence exhausted; reboot required"); return; }
    d->seq=next_seq++; d->len=28+event->length;
    relay_put32(d->payload,state.boot); relay_put32(d->payload+4,event->generation);
    memcpy(d->payload+8,event->announcement,20);
    memcpy(d->payload+28,event->body,event->length);
    uint32_t queued_seq=d->seq;
    if (xQueueSend(outgoing,&d,0)!=pdTRUE) {
        free(d); ESP_LOGW(TAG,"Outgoing drawing dropped: queue full");
    } else ESP_LOGI(TAG,"Queued local drawing seq=%lu bytes=%u",(unsigned long)queued_seq,(unsigned)event->length);
}
bool online_tick(pictochat_room_t *room) {
    state_t state={0};
    const pictochat_peer_t *p=&room->peers[0];
    if (p->connected && p->admitted && p->session.identity.phase==HOST_ID_READY &&
        p->versions[0] && p->versions[1]) {
        state.generation=p->generation;
        memcpy(state.profile,p->identities[1].bytes+12,84);
    }
    portENTER_CRITICAL(&lock);
    state.boot=local_state.boot; local_state=state;
    state=remote_state;
    bool connected=link_up;
    uint32_t epoch=link_epoch;
    portEXIT_CRITICAL(&lock);
    bool changed=false;
    if (!connected) state.generation=0;
    if (memcmp(&state,&installed,sizeof(state))) {
        if (room->peers[GHOST_SLOT].connected) {
            pictochat_room_leave(room,GHOST_SLOT); changed=true;
        }
        if (state.generation) {
            if (++ghost_generation==0) ++ghost_generation;
            bool ok=pictochat_room_ghost_join(room,GHOST_SLOT,GHOST_AID,ghost_generation,
                                              state.profile,esp_random(),esp_random());
            if (!ok) ESP_LOGW(TAG,"Remote identity rejected (MAC/slot collision or invalid profile)");
            else { changed=true; ESP_LOGI(TAG,"Remote DS installed as ghost"); }
        }
        installed=state;
        atomic_store(&ghost_count,room->peers[GHOST_SLOT].connected ? 1 : 0);
    }
    drawing_t *d;
    if (xQueuePeek(incoming,&d,0)==pdTRUE) {
        uint32_t boot=relay_get32(d->payload), generation=relay_get32(d->payload+4);
        bool current=connected && d->epoch==epoch && boot==state.boot && generation==state.generation &&
                     state.generation && room->peers[GHOST_SLOT].connected;
        bool duplicate=boot==last_boot && d->seq<=last_seq;
        int result=-1;
        if (current && !duplicate)
            result=pictochat_room_ghost_send(room,GHOST_SLOT,ghost_generation,d->payload+8,
                                             d->payload+28,d->len-28,d->seq);
        if (result!=-2) {
            if (result==0) {
                last_boot=boot; last_seq=d->seq;
                ESP_LOGI(TAG,"Accepted remote drawing seq=%lu bytes=%u",(unsigned long)d->seq,(unsigned)(d->len-28));
            } else if (!duplicate) ESP_LOGW(TAG,"Discarded stale/invalid remote drawing seq=%lu",(unsigned long)d->seq);
            // ACK means consumed by the local bridge, not pixels displayed.
            ack_t ack={boot,d->seq,d->epoch};
            if (xQueueSend(acknowledgments,&ack,0)==pdTRUE) {
                xQueueReceive(incoming,&d,0); free(d);
            }
        }
    }
    return changed;
}
#if !PICTOCHAT_USB && !PICTOCHAT_BLE
static bool transfer(int fd, void *buf, size_t len, bool sending) {
    uint8_t *p=buf;
    while (len) {
        int n=sending ? send(fd,p,len,0) : recv(fd,p,len,0);
        if (n<=0) return false;
        p+=n; len-=n;
    }
    return true;
}
static bool send_packet(int fd, unsigned kind, uint32_t seq, const uint8_t *body, size_t len) {
    uint8_t header[RELAY_HEADER];
    return relay_header(header,kind,seq,body,len) && transfer(fd,header,sizeof(header),true) &&
           transfer(fd,(void *)body,len,true);
}
static int readable(int fd, unsigned ms) {
    fd_set set; FD_ZERO(&set); FD_SET(fd,&set);
    struct timeval timeout={ms/1000,(ms%1000)*1000};
    return select(fd+1,&set,NULL,NULL,&timeout);
}
static void socket_timeout(int fd) {
    struct timeval timeout={3,0};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
}
static int connect_peer(struct sockaddr_in *address) {
    int fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (fd<0) return -1;
    fcntl(fd,F_SETFL,O_NONBLOCK);
    int result=connect(fd,(struct sockaddr *)address,sizeof(*address));
    if (result<0 && errno!=EINPROGRESS) { close(fd); return -1; }
    fd_set set; FD_ZERO(&set); FD_SET(fd,&set);
    struct timeval timeout={1,0}; int error=0; socklen_t size=sizeof(error);
    if (select(fd+1,NULL,&set,NULL,&timeout)<=0 ||
        getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)<0 || error) { close(fd); return -1; }
    fcntl(fd,F_SETFL,0); socket_timeout(fd);
    return fd;
}
static void network_task(void *arg) {
    (void)arg;
    int udp=-1, listener=-1, fd=-1;
    int64_t last_connect=0, last_discovery=0, last_state=0, last_message=0, last_rx=0;
    int64_t last_status=0;
    uint32_t epoch=0, sent_seq=0;
    state_t sent_state={0};
    for (;;) {
        int64_t now=esp_timer_get_time();
        if (now-last_status>10000000) {
            ESP_LOGI(TAG,"Link status: ip=%u udp=%d listener=%d peer=%d heap=%u",
                     atomic_load(&have_ip),udp,listener,fd,(unsigned)esp_get_free_heap_size());
            last_status=now;
        }
        if (!atomic_load(&have_ip)) {
            if (fd>=0) { close(fd); fd=-1; }
            if (udp>=0) { close(udp); udp=-1; }
            last_rx=0;
            portENTER_CRITICAL(&lock); link_up=false; portEXIT_CRITICAL(&lock);
            if (now-last_connect>10000000) { esp_wifi_connect(); last_connect=now; }
            vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        if (udp<0) {
            udp=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
            if (udp<0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
            int yes=1; setsockopt(udp,SOL_SOCKET,SO_BROADCAST,&yes,sizeof(yes));
            struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(online_node()==1 ? LINK_PORT : 0),
                                       .sin_addr.s_addr=atomic_load(&sta_ip)};
            if (bind(udp,(struct sockaddr *)&address,sizeof(address))<0) { close(udp); udp=-1; continue; }
        }
        if (online_node()==1 && listener<0) {
            listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
            if (listener<0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
            int yes=1; setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
            struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(LINK_PORT),.sin_addr.s_addr=htonl(INADDR_ANY)};
            if (bind(listener,(struct sockaddr *)&address,sizeof(address))<0 || listen(listener,1)<0) {
                close(listener); listener=-1; vTaskDelay(pdMS_TO_TICKS(100)); continue;
            }
        }
        if (online_node()==2 && fd<0 && now-last_discovery>1000000) {
            struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(LINK_PORT),.sin_addr.s_addr=atomic_load(&sta_broadcast)};
            sendto(udp,"PCTR?1",6,0,(struct sockaddr *)&address,sizeof(address)); last_discovery=now;
            uint32_t configured=inet_addr(ONLINE_NODE_A_IP);
            if (configured && configured!=INADDR_NONE) {
                address.sin_addr.s_addr=configured;
                fd=connect_peer(&address);
            }
        }
        if (readable(udp,0)>0) {
            char message[16]; struct sockaddr_in address; socklen_t len=sizeof(address);
            int n=recvfrom(udp,message,sizeof(message),0,(struct sockaddr *)&address,&len);
            if (online_node()==1 && n==6 && !memcmp(message,"PCTR?1",6))
                sendto(udp,"PCTR!1",6,0,(struct sockaddr *)&address,len);
            else if (online_node()==2 && fd<0 && n==6 && !memcmp(message,"PCTR!1",6)) {
                address.sin_port=htons(LINK_PORT); fd=connect_peer(&address);
            }
        }
        if (online_node()==1 && fd<0 && listener>=0 && readable(listener,0)>0) {
            fd=accept(listener,NULL,NULL); if (fd>=0) socket_timeout(fd);
        }
        if (fd<0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (!last_rx) {
            if (++epoch==0) ++epoch;
            portENTER_CRITICAL(&lock); remote_state=(state_t){0}; link_epoch=epoch; link_up=false; portEXIT_CRITICAL(&lock);
            last_rx=now; last_state=0; sent_seq=0; sent_state=(state_t){0};
            ESP_LOGI(TAG,"Peer TCP connected node=%u",online_node());
        }
        bool ok=now-last_rx<6000000;
        portENTER_CRITICAL(&lock); state_t state=local_state; portEXIT_CRITICAL(&lock);
        if (ok && (memcmp(&state,&sent_state,sizeof(state)) || now-last_state>1000000)) {
            uint8_t payload[92]; relay_put32(payload,state.boot); relay_put32(payload+4,state.generation);
            memcpy(payload+8,state.profile,84);
            ok=send_packet(fd,RELAY_STATE,0,payload,sizeof(payload)); sent_state=state; last_state=now;
        }
        ack_t ack;
        while (ok && xQueueReceive(acknowledgments,&ack,0)==pdTRUE) {
            if (ack.epoch!=epoch) continue;
            uint8_t payload[8]; relay_put32(payload,ack.boot); relay_put32(payload+4,ack.seq);
            ok=send_packet(fd,RELAY_ACK,ack.seq,payload,sizeof(payload));
        }
        drawing_t *pending;
        if (ok && xQueuePeek(outgoing,&pending,0)==pdTRUE) {
            if (!state.generation || relay_get32(pending->payload+4)!=state.generation) {
                xQueueReceive(outgoing,&pending,0); free(pending); sent_seq=0;
            } else if (pending->seq!=sent_seq || now-last_message>4000000) {
                ok=send_packet(fd,RELAY_DRAWING,pending->seq,pending->payload,pending->len);
                sent_seq=pending->seq; last_message=now;
            }
        }
        if (ok && readable(fd,20)>0) {
            uint8_t header[RELAY_HEADER]; unsigned kind; uint32_t seq; size_t len;
            ok=transfer(fd,header,sizeof(header),false) && relay_parse_header(header,&kind,&seq,&len);
            drawing_t *packet=ok ? malloc(sizeof(*packet)) : NULL;
            if (ok && !packet) ok=false;
            if (ok) {
                packet->len=len; packet->seq=seq; packet->epoch=epoch;
                ok=transfer(fd,packet->payload,len,false) && relay_hash(packet->payload,len)==relay_get32(header+12);
            }
            if (ok) {
                last_rx=esp_timer_get_time(); uint8_t *payload=packet->payload;
                if (kind==RELAY_STATE) {
                    state_t received={.boot=relay_get32(payload),.generation=relay_get32(payload+4)};
                    memcpy(received.profile,payload+8,84);
                    ok=received.boot && (!received.generation || (received.profile[0]==3 && received.profile[1]<=1));
                    if (ok) { portENTER_CRITICAL(&lock); remote_state=received; link_up=true; portEXIT_CRITICAL(&lock); }
                } else if (kind==RELAY_ACK) {
                    if (xQueuePeek(outgoing,&pending,0)==pdTRUE && relay_get32(payload)==state.boot &&
                        relay_get32(payload+4)==pending->seq && seq==pending->seq) {
                        ESP_LOGI(TAG,"Peer consumed drawing seq=%lu",(unsigned long)seq);
                        xQueueReceive(outgoing,&pending,0); free(pending); sent_seq=0;
                    }
                } else if (kind==RELAY_DRAWING && seq && xQueueSend(incoming,&packet,0)==pdTRUE) packet=NULL;
            }
            free(packet);
        }
        if (!ok) {
            close(fd); fd=-1; last_rx=0;
            portENTER_CRITICAL(&lock); link_up=false; portEXIT_CRITICAL(&lock);
            ESP_LOGW(TAG,"Peer TCP disconnected; rediscovering");
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
#elif PICTOCHAT_BLE
#include "ble_transport.inc"
#else
#include "usb_transport.inc"
#endif
void online_start(void) {
    outgoing=xQueueCreate(2,sizeof(drawing_t *)); incoming=xQueueCreate(1,sizeof(drawing_t *));
    acknowledgments=xQueueCreate(4,sizeof(ack_t));
    configASSERT(outgoing && incoming && acknowledgments);
    local_state.boot=esp_random(); if (!local_state.boot) local_state.boot=1;
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#if PICTOCHAT_BLE
    ble_start();
#elif PICTOCHAT_USB
    usb_start();
#else
    if (ONLINE_DIAGNOSTIC_CHANNEL) {
        ESP_ERROR_CHECK(esp_wifi_set_channel(ONLINE_DIAGNOSTIC_CHANNEL,WIFI_SECOND_CHAN_NONE));
        atomic_store(&channel,ONLINE_DIAGNOSTIC_CHANNEL);
        ESP_LOGW(TAG,"Local radio diagnostic: channel=%u; router connection disabled",online_channel());
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_connect());
    BaseType_t created=xTaskCreate(network_task,"online",6144,NULL,2,NULL);
    configASSERT(created==pdPASS);
#endif
}
#endif
