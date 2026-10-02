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

// Relay limits and task-owned state. Shared remote state is guarded by lock.
#define LINK_PORT 26711
#ifndef ONLINE_LOCAL_SLOTS
#define ONLINE_LOCAL_SLOTS 1u
#endif
#define LOCAL_SLOTS ONLINE_LOCAL_SLOTS
#define GHOST_SLOTS (PICTOCHAT_ROOM_CLIENTS-LOCAL_SLOTS)
#define GHOST_ROOM_SLOT(g) (LOCAL_SLOTS+(g))
#define GHOST_AID(g) (15u-(g)) /* radio AIDs count up from 1; ghosts count down from 15 */
_Static_assert(LOCAL_SLOTS>=1 && GHOST_SLOTS>=1 && GHOST_SLOTS<=15-LOCAL_SLOTS && GHOST_SLOTS<=16,"slot layout");
#define PEER_TIMEOUT_US 6000000
#define DRAWING_RETRY_US 4000000
#define DRAWING_GIVEUP_US 30000000
static const char *TAG="online";
typedef struct { uint32_t boot, generation; uint8_t profile[84]; } state_t;
typedef struct {
    uint32_t seq, boot;
    unsigned from;            /* outgoing: local slot; incoming: remote peer id */
    uint16_t acked;           /* outgoing: bitmask of remote indexes that consumed it */
    int64_t first_sent;
    size_t len;
    uint8_t payload[RELAY_MAX_PAYLOAD];
} drawing_t;
typedef struct { uint32_t boot, seq; unsigned to; } ack_t;
typedef struct {
    unsigned id;              /* 0 = free */
    state_t state, installed;
    uint32_t ghost_generation, last_seq;
    int64_t last_rx;
} remote_t;
static QueueHandle_t outgoing, incoming, acknowledgments;
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static state_t local_state[LOCAL_SLOTS];
static remote_t remotes[GHOST_SLOTS];  /* guarded by lock */
static uint32_t boot_id;
#if !PICTOCHAT_USB && !PICTOCHAT_BLE
static atomic_bool have_ip;
static atomic_uint sta_ip, sta_broadcast;
#endif
static atomic_uint channel=7, ghost_count;
static uint32_t next_seq=1;
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
#include "nvs.h"
/* DS room letter A..D (node 1..4). Saved by the bridge over USB ("@ROOM X"); a
 * saved value wins over the build's PICTOCHAT_NODE. Channels are the
 * hardware-confirmed ones per room. */
static const uint8_t room_channel[4]={1,7,13,7};
bool online_room_save(unsigned room) {
    if (room<1 || room>4) return false;
    nvs_handle_t h; if (nvs_open("picto",NVS_READWRITE,&h)!=ESP_OK) return false;
    bool ok=nvs_set_u8(h,"room",(uint8_t)room)==ESP_OK && nvs_commit(h)==ESP_OK;
    nvs_close(h); return ok;
}
void online_wifi_configure(void) {
    nvs_handle_t h; uint8_t saved=0;
    if (nvs_open("picto",NVS_READONLY,&h)==ESP_OK) { nvs_get_u8(h,"room",&saved); nvs_close(h); }
    if (saved>=1 && saved<=4) node=saved;
    else {
#if defined(PICTOCHAT_NODE)
        node=PICTOCHAT_NODE;
#else
        node=1;
#endif
    }
    atomic_store(&channel,room_channel[node-1]);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
}
#endif
/* At most 15 remotes: a bounded linear scan keeps peer lookup simple. */
static int remote_index(unsigned id) {
    if (id) for (unsigned g=0; g<GHOST_SLOTS; ++g) if (remotes[g].id==id) return (int)g;
    return -1;
}
static uint16_t remote_mask(void) {
    uint16_t mask=0;
    for (unsigned g=0; g<GHOST_SLOTS; ++g) if (remotes[g].id) mask|=(uint16_t)(1u<<g);
    return mask;
}
// Room callbacks and member synchronization run in the host task.
void online_message(const pictochat_event_t *event) {
    if (event->type!=PICTOCHAT_MESSAGE_RECEIVED || event->peer_slot>=LOCAL_SLOTS ||
        event->length>HOST_MESSAGE_MAX) return;
    // No ghost injection produces a RECEIVE event, so messages cannot loop back.
    drawing_t *d=malloc(sizeof(*d));
    if (!d) { ESP_LOGW(TAG,"Outgoing drawing dropped: no memory"); return; }
    portENTER_CRITICAL(&lock);
    state_t state=local_state[event->peer_slot];
    bool connected=remote_mask()!=0;
    portEXIT_CRITICAL(&lock);
    if (!connected || !state.generation || state.generation!=event->generation) {
        free(d); ESP_LOGW(TAG,"Outgoing drawing dropped: no remote peer"); return;
    }
    if (!next_seq) { free(d); ESP_LOGE(TAG,"Sequence exhausted; reboot required"); return; }
    d->seq=next_seq++; d->len=28+event->length; d->from=event->peer_slot; d->acked=0; d->first_sent=0;
    relay_put32(d->payload,state.boot); relay_put32(d->payload+4,event->generation);
    memcpy(d->payload+8,event->announcement,20);
    memcpy(d->payload+28,event->body,event->length);
    uint32_t queued_seq=d->seq;
    if (xQueueSend(outgoing,&d,0)!=pdTRUE) {
        free(d); ESP_LOGW(TAG,"Outgoing drawing dropped: queue full");
    } else ESP_LOGI(TAG,"Queued local drawing slot=%u seq=%lu bytes=%u",event->peer_slot,(unsigned long)queued_seq,(unsigned)event->length);
}
bool online_tick(pictochat_room_t *room) {
    int64_t now=esp_timer_get_time();
    state_t local[LOCAL_SLOTS]={0};
    for (unsigned i=0; i<LOCAL_SLOTS; ++i) {
        const pictochat_peer_t *p=&room->peers[i];
        local[i].boot=boot_id;
        if (p->connected && p->admitted && p->session.identity.phase==HOST_ID_READY &&
            p->versions[0] && p->versions[1]) {
            local[i].generation=p->generation;
            memcpy(local[i].profile,p->identities[1].bytes+12,84);
        }
    }
    portENTER_CRITICAL(&lock);
    memcpy(local_state,local,sizeof(local));
    for (unsigned g=0; g<GHOST_SLOTS; ++g)
        if (remotes[g].id && now-remotes[g].last_rx>PEER_TIMEOUT_US) remotes[g].id=0;
    remote_t snapshot[GHOST_SLOTS]; memcpy(snapshot,remotes,sizeof(snapshot));
    portEXIT_CRITICAL(&lock);
    bool changed=false; unsigned ghosts=0;
    for (unsigned g=0; g<GHOST_SLOTS; ++g) {
        remote_t *r=&snapshot[g];
        state_t want=r->id ? r->state : (state_t){0};
        unsigned slot=GHOST_ROOM_SLOT(g);
        if (memcmp(&want,&r->installed,sizeof(want))) {
            if (room->peers[slot].connected) { pictochat_room_leave(room,slot); changed=true; }
            if (want.generation) {
                if (++r->ghost_generation==0) ++r->ghost_generation;
                bool ok=pictochat_room_ghost_join(room,slot,GHOST_AID(g),r->ghost_generation,
                                                  want.profile,esp_random(),esp_random());
                if (!ok) ESP_LOGW(TAG,"Remote %u rejected (MAC/slot collision or invalid profile)",r->id);
                else { changed=true; ESP_LOGI(TAG,"Remote %u installed as ghost slot=%u aid=%u",r->id,slot,GHOST_AID(g)); }
            }
            r->installed=want;
            portENTER_CRITICAL(&lock);
            if (remotes[g].id==r->id) { remotes[g].installed=want; remotes[g].ghost_generation=r->ghost_generation; }
            portEXIT_CRITICAL(&lock);
        }
        ghosts+=room->peers[slot].connected;
    }
    atomic_store(&ghost_count,ghosts);
    drawing_t *d;
    if (xQueuePeek(incoming,&d,0)==pdTRUE) {
        uint32_t boot=relay_get32(d->payload), generation=relay_get32(d->payload+4);
        portENTER_CRITICAL(&lock);
        int g=remote_index(d->from);
        remote_t r=g>=0 ? remotes[g] : (remote_t){0};
        portEXIT_CRITICAL(&lock);
        bool current=g>=0 && boot==r.state.boot && generation==r.state.generation && r.state.generation &&
                     room->peers[GHOST_ROOM_SLOT(g)].connected;
        bool duplicate=current && d->seq<=r.last_seq;
        int result=-1;
        if (current && !duplicate)
            result=pictochat_room_ghost_send(room,GHOST_ROOM_SLOT(g),r.ghost_generation,d->payload+8,
                                             d->payload+28,d->len-28,d->seq);
        if (result!=-2) {
            if (result==0) {
                portENTER_CRITICAL(&lock); if (remotes[g].id==d->from) remotes[g].last_seq=d->seq; portEXIT_CRITICAL(&lock);
                ESP_LOGI(TAG,"Accepted remote %u drawing seq=%lu bytes=%u",d->from,(unsigned long)d->seq,(unsigned)(d->len-28));
            } else if (!duplicate) ESP_LOGW(TAG,"Discarded stale/invalid remote %u drawing seq=%lu",d->from,(unsigned long)d->seq);
            // ACK means consumed by the local bridge, not pixels displayed.
            ack_t ack={boot,d->seq,d->from};
            if (xQueueSend(acknowledgments,&ack,0)==pdTRUE) {
                xQueueReceive(incoming,&d,0); free(d);
            }
        }
    }
    return changed;
}
/* Shared by every transport. relay_receive consumes one complete v2 frame;
 * transports without peer ids (LAN/BLE, one peer) pass from_default=1. */
// Shared relay protocol, independent of the selected transport.
static bool relay_receive(const uint8_t *wire,size_t bytes,unsigned from_default,int64_t now) {
    unsigned kind,from,to; uint32_t seq; size_t len;
    if (bytes<RELAY_HEADER || !relay_parse_header(wire,&kind,&seq,&from,&to,&len) || bytes!=RELAY_HEADER+len ||
        relay_hash(wire+RELAY_HEADER,len)!=relay_get32(wire+12)) return false;
    if (!from) from=from_default;
    if (!from) return false;
    const uint8_t *payload=wire+RELAY_HEADER;
    if (kind==RELAY_STATE) {
        state_t received={.boot=relay_get32(payload),.generation=relay_get32(payload+4)};
        memcpy(received.profile,payload+8,84);
        if (!received.boot || (received.generation && (received.profile[0]!=3 || received.profile[1]>1))) return false;
        bool fresh=false, full=false;
        portENTER_CRITICAL(&lock);
        int g=remote_index(from);
        if (g<0) for (unsigned i=0; i<GHOST_SLOTS; ++i) if (!remotes[i].id) { g=(int)i; break; }
        if (g<0) full=true;
        else {
            remote_t *r=&remotes[g];
            fresh=r->id!=from || r->state.boot!=received.boot;
            if (fresh) { r->id=from; r->last_seq=0; }
            r->state=received; r->last_rx=now;
        }
        portEXIT_CRITICAL(&lock);
        if (full) { ESP_LOGW(TAG,"Remote %u refused: no free ghost slot",from); return false; }
        if (fresh) ESP_LOGI(TAG,"Remote %u connected node=%u",from,node);
    } else if (kind==RELAY_LEAVE) {
        portENTER_CRITICAL(&lock); int g=remote_index(from); if (g>=0) remotes[g].id=0; portEXIT_CRITICAL(&lock);
        if (g>=0) ESP_LOGI(TAG,"Remote %u left",from);
    } else if (kind==RELAY_ACK) {
        portENTER_CRITICAL(&lock); int g=remote_index(from); portEXIT_CRITICAL(&lock);
        drawing_t *pending;
        if (g>=0 && xQueuePeek(outgoing,&pending,0)==pdTRUE && relay_get32(payload)==boot_id &&
            seq==pending->seq && relay_get32(payload+4)==seq) {
            pending->acked|=(uint16_t)(1u<<g);
            ESP_LOGI(TAG,"Remote %u consumed drawing seq=%lu",from,(unsigned long)seq);
        }
    } else if (kind==RELAY_DRAWING && seq) {
        drawing_t *packet=malloc(sizeof(*packet));
        if (!packet) return false;
        packet->seq=seq; packet->len=len; packet->from=from;
        memcpy(packet->payload,payload,len);
        if (xQueueSend(incoming,&packet,0)!=pdTRUE) free(packet); // Retry without ACK.
    }
    return true;
}
static void relay_forget_all(void) {
    portENTER_CRITICAL(&lock);
    for (unsigned g=0; g<GHOST_SLOTS; ++g) remotes[g].id=0;
    portEXIT_CRITICAL(&lock);
}
typedef bool (*relay_send_fn)(unsigned kind,uint32_t seq,unsigned from,unsigned to,const uint8_t *data,size_t n);
/* Drives keepalive state, ACKs and the pending drawing. Call every few ms
 * from the transport task; `ready` false resets the retransmit memory. */
static void relay_transmit(relay_send_fn send,int64_t now,bool ready) {
    static state_t sent[LOCAL_SLOTS]; static int64_t last_state[LOCAL_SLOTS], last_draw; static uint32_t sent_seq;
    if (!ready) { memset(sent,0,sizeof(sent)); memset(last_state,0,sizeof(last_state)); sent_seq=0; return; }
    state_t local[LOCAL_SLOTS]; uint16_t mask;
    portENTER_CRITICAL(&lock); memcpy(local,local_state,sizeof(local)); mask=remote_mask(); portEXIT_CRITICAL(&lock);
    for (unsigned i=0; i<LOCAL_SLOTS; ++i)
        if (memcmp(&local[i],&sent[i],sizeof(state_t)) || now-last_state[i]>1000000) {
            uint8_t data[92]; relay_put32(data,local[i].boot); relay_put32(data+4,local[i].generation);
            memcpy(data+8,local[i].profile,84);
            if (send(RELAY_STATE,0,i,0,data,sizeof(data))) { sent[i]=local[i]; last_state[i]=now; }
        }
    ack_t ack;
    while (xQueueReceive(acknowledgments,&ack,0)==pdTRUE) {
        uint8_t data[8]; relay_put32(data,ack.boot); relay_put32(data+4,ack.seq);
        send(RELAY_ACK,ack.seq,0,ack.to,data,sizeof(data));
    }
    drawing_t *pending;
    if (xQueuePeek(outgoing,&pending,0)==pdTRUE) {
        const state_t *s=&local[pending->from];
        bool done=(pending->acked&mask)==mask; // every live remote consumed it (or none are left)
        // ponytail: 30 s give-up so one silent remote cannot wedge the queue; per-remote queues if that matters.
        bool expired=pending->first_sent && now-pending->first_sent>DRAWING_GIVEUP_US;
        if (!s->generation || relay_get32(pending->payload+4)!=s->generation || done || expired) {
            if (expired) ESP_LOGW(TAG,"Drawing seq=%lu gave up waiting for ACKs",(unsigned long)pending->seq);
            xQueueReceive(outgoing,&pending,0); free(pending); sent_seq=0;
        } else if (pending->seq!=sent_seq || now-last_draw>DRAWING_RETRY_US) {
            if (send(RELAY_DRAWING,pending->seq,pending->from,0,pending->payload,pending->len)) {
                sent_seq=pending->seq; last_draw=now;
                if (!pending->first_sent) pending->first_sent=now;
            }
        }
    }
}
#if !PICTOCHAT_USB && !PICTOCHAT_BLE
// Direct-LAN transport. BLE and USB implementation fragments are selected below.
static bool transfer(int fd, void *buf, size_t len, bool sending) {
    uint8_t *p=buf;
    while (len) {
        int n=sending ? send(fd,p,len,0) : recv(fd,p,len,0);
        if (n<=0) return false;
        p+=n; len-=n;
    }
    return true;
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
static int lan_fd=-1;
static bool lan_send(unsigned kind,uint32_t seq,unsigned from,unsigned to,const uint8_t *body,size_t len) {
    uint8_t header[RELAY_HEADER];
    return relay_header(header,kind,seq,from,to,body,len) && transfer(lan_fd,header,sizeof(header),true) &&
           (!len || transfer(lan_fd,(void *)body,len,true));
}
static void network_task(void *arg) {
    (void)arg;
    int udp=-1, listener=-1;
    int64_t last_connect=0, last_discovery=0, last_rx=0, last_status=0;
    static uint8_t wire[RELAY_HEADER+RELAY_MAX_PAYLOAD];
    for (;;) {
        int64_t now=esp_timer_get_time();
        if (now-last_status>10000000) {
            ESP_LOGI(TAG,"Link status: ip=%u udp=%d listener=%d peer=%d heap=%u",
                     atomic_load(&have_ip),udp,listener,lan_fd,(unsigned)esp_get_free_heap_size());
            last_status=now;
        }
        if (!atomic_load(&have_ip)) {
            if (lan_fd>=0) { close(lan_fd); lan_fd=-1; }
            if (udp>=0) { close(udp); udp=-1; }
            last_rx=0; relay_forget_all(); relay_transmit(lan_send,now,false);
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
        if (online_node()==2 && lan_fd<0 && now-last_discovery>1000000) {
            struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(LINK_PORT),.sin_addr.s_addr=atomic_load(&sta_broadcast)};
            sendto(udp,"PCTR?1",6,0,(struct sockaddr *)&address,sizeof(address)); last_discovery=now;
            uint32_t configured=inet_addr(ONLINE_NODE_A_IP);
            if (configured && configured!=INADDR_NONE) {
                address.sin_addr.s_addr=configured;
                lan_fd=connect_peer(&address);
            }
        }
        if (readable(udp,0)>0) {
            char message[16]; struct sockaddr_in address; socklen_t len=sizeof(address);
            int n=recvfrom(udp,message,sizeof(message),0,(struct sockaddr *)&address,&len);
            if (online_node()==1 && n==6 && !memcmp(message,"PCTR?1",6))
                sendto(udp,"PCTR!1",6,0,(struct sockaddr *)&address,len);
            else if (online_node()==2 && lan_fd<0 && n==6 && !memcmp(message,"PCTR!1",6)) {
                address.sin_port=htons(LINK_PORT); lan_fd=connect_peer(&address);
            }
        }
        if (online_node()==1 && lan_fd<0 && listener>=0 && readable(listener,0)>0) {
            lan_fd=accept(listener,NULL,NULL); if (lan_fd>=0) socket_timeout(lan_fd);
        }
        if (lan_fd<0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (!last_rx) {
            relay_forget_all(); relay_transmit(lan_send,now,false);
            last_rx=now;
            ESP_LOGI(TAG,"Peer TCP connected node=%u",online_node());
        }
        bool ok=now-last_rx<PEER_TIMEOUT_US;
        if (ok) relay_transmit(lan_send,now,true);
        if (ok && readable(lan_fd,20)>0) {
            unsigned kind,from,to; uint32_t seq; size_t len;
            ok=transfer(lan_fd,wire,RELAY_HEADER,false) && relay_parse_header(wire,&kind,&seq,&from,&to,&len) &&
               (!len || transfer(lan_fd,wire+RELAY_HEADER,len,false));
            if (ok) { last_rx=esp_timer_get_time(); relay_receive(wire,RELAY_HEADER+len,1,last_rx); }
        }
        if (!ok) {
            close(lan_fd); lan_fd=-1; last_rx=0; relay_forget_all();
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
#if !PICTOCHAT_USB
bool online_room_wanted(int64_t now) { (void)now; return true; }
#endif
void online_start(void) {
    outgoing=xQueueCreate(2,sizeof(drawing_t *)); incoming=xQueueCreate(1,sizeof(drawing_t *));
    acknowledgments=xQueueCreate(4,sizeof(ack_t));
    configASSERT(outgoing && incoming && acknowledgments);
    boot_id=esp_random(); if (!boot_id) boot_id=1;
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
