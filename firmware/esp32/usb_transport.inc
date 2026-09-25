/* Included by online.c: same room-owner queues, USB instead of LAN sockets. */
#include <stdio.h>
#include <stdarg.h>
#include "driver/usb_serial_jtag.h"
#define USB_WIRE_MAX (RELAY_HEADER+RELAY_MAX_PAYLOAD)
#define USB_LINE_MAX (6+2*USB_WIRE_MAX)
static QueueHandle_t usb_logs;
static char usb_rx_line[USB_LINE_MAX+1], usb_tx_line[USB_LINE_MAX+2];
static uint8_t usb_wire[USB_WIRE_MAX];
typedef struct { char text[256]; } usb_log_t;
static int usb_log(const char *fmt, va_list args) {
    usb_log_t row;
    int n=vsnprintf(row.text,sizeof(row.text),fmt,args);
    row.text[sizeof(row.text)-2]='\n'; row.text[sizeof(row.text)-1]=0;
    if (usb_logs) xQueueSend(usb_logs,&row,0); // Logs may drop; never block radio work.
    return n;
}
static bool usb_write(const void *data,size_t n) {
    const uint8_t *p=data;
    while (n) {
        // The driver enqueues each call atomically into its 4096-byte ring.
        // A drawing line can exceed 20 KB, so it must be submitted in chunks.
        size_t chunk=n>1024 ? 1024 : n;
        int sent=usb_serial_jtag_write_bytes(p,chunk,pdMS_TO_TICKS(100));
        if (sent<=0) return false;
        p+=sent; n-=sent;
    }
    return true;
}
static bool usb_packet(unsigned kind,uint32_t seq,const uint8_t *data,size_t n) {
    uint8_t header[RELAY_HEADER];
    if (!relay_header(header,kind,seq,data,n)) return false;
    static const char hex[]="0123456789abcdef";
    memcpy(usb_tx_line,"\n@PCTR ",7);
    for (size_t i=0;i<RELAY_HEADER+n;i++) {
        uint8_t value=i<RELAY_HEADER ? header[i] : data[i-RELAY_HEADER];
        usb_tx_line[7+2*i]=hex[value>>4]; usb_tx_line[8+2*i]=hex[value&15];
    }
    size_t length=7+2*(RELAY_HEADER+n);
    usb_tx_line[length++]='\n';
    return usb_write(usb_tx_line,length);
}
static int hex_digit(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static bool usb_receive(size_t size,int64_t *last_rx,uint32_t *epoch) {
    if (size<6+2*RELAY_HEADER || memcmp(usb_rx_line,"@PCTR ",6) || (size-6)%2) return false;
    size_t bytes=(size-6)/2;
    if (bytes>sizeof(usb_wire)) return false;
    for (size_t i=0;i<bytes;i++) {
        int a=hex_digit(usb_rx_line[6+2*i]),b=hex_digit(usb_rx_line[7+2*i]);
        if (a<0 || b<0) return false;
        usb_wire[i]=(uint8_t)((a<<4)|b);
    }
    unsigned kind; uint32_t seq; size_t len;
    if (!relay_parse_header(usb_wire,&kind,&seq,&len) || bytes!=RELAY_HEADER+len ||
        relay_hash(usb_wire+RELAY_HEADER,len)!=relay_get32(usb_wire+12)) return false;
    uint8_t *payload=usb_wire+RELAY_HEADER;
    if (kind==RELAY_STATE) {
        state_t received={.boot=relay_get32(payload),.generation=relay_get32(payload+4)};
        memcpy(received.profile,payload+8,84);
        if (!received.boot || (received.generation && (received.profile[0]!=3 || received.profile[1]>1))) return false;
        portENTER_CRITICAL(&lock);
        bool fresh=!link_up || remote_state.boot!=received.boot;
        if (fresh) { if (++*epoch==0) ++*epoch; link_epoch=*epoch; }
        remote_state=received; link_up=true;
        portEXIT_CRITICAL(&lock);
        *last_rx=esp_timer_get_time();
        if (fresh) ESP_LOGI(TAG,"USB peer connected node=%u",node);
    } else if (kind==RELAY_ACK) {
        portENTER_CRITICAL(&lock); uint32_t own_boot=local_state.boot; portEXIT_CRITICAL(&lock);
        drawing_t *pending;
        if (xQueuePeek(outgoing,&pending,0)==pdTRUE && relay_get32(payload)==own_boot &&
            seq==pending->seq && relay_get32(payload+4)==seq) {
            xQueueReceive(outgoing,&pending,0); free(pending);
            ESP_LOGI(TAG,"Peer consumed drawing seq=%lu",(unsigned long)seq);
        }
    } else if (kind==RELAY_DRAWING && seq) {
        drawing_t *packet=malloc(sizeof(*packet));
        if (!packet) return false;
        packet->seq=seq; packet->len=len; packet->epoch=*epoch;
        memcpy(packet->payload,payload,len);
        if (xQueueSend(incoming,&packet,0)!=pdTRUE) free(packet); // Retry without ACK.
    }
    return true;
}
static void usb_task(void *arg) {
    (void)arg;
    size_t used=0; bool overflow=false;
    int64_t last_rx=0,last_state=0,last_draw=0;
    uint32_t epoch=0,sent_seq=0;
    state_t sent_state={0};
    for (;;) {
        uint8_t input[256];
        int count=usb_serial_jtag_read_bytes(input,sizeof(input),pdMS_TO_TICKS(2));
        for (int i=0;i<count;i++) {
            if (input[i]=='\n') {
                if (!overflow) usb_receive(used,&last_rx,&epoch);
                used=0; overflow=false;
            } else if (input[i]!='\r') {
                if (used<USB_LINE_MAX) usb_rx_line[used++]=(char)input[i]; else overflow=true;
            }
        }
        int64_t now=esp_timer_get_time();
        if (last_rx && now-last_rx>6000000) {
            portENTER_CRITICAL(&lock); link_up=false; portEXIT_CRITICAL(&lock);
            last_rx=0;
            ESP_LOGW(TAG,"USB peer timed out; removing remote ghost");
        }
        portENTER_CRITICAL(&lock); state_t state=local_state; portEXIT_CRITICAL(&lock);
        if (memcmp(&state,&sent_state,sizeof(state)) || now-last_state>1000000) {
            uint8_t data[92]; relay_put32(data,state.boot); relay_put32(data+4,state.generation);
            memcpy(data+8,state.profile,84);
            if (usb_packet(RELAY_STATE,0,data,sizeof(data))) { sent_state=state; last_state=now; }
        }
        ack_t ack;
        while (xQueueReceive(acknowledgments,&ack,0)==pdTRUE) {
            if (ack.epoch!=epoch) continue;
            uint8_t data[8]; relay_put32(data,ack.boot); relay_put32(data+4,ack.seq);
            usb_packet(RELAY_ACK,ack.seq,data,sizeof(data));
        }
        drawing_t *pending;
        if (xQueuePeek(outgoing,&pending,0)==pdTRUE) {
            if (!state.generation || relay_get32(pending->payload+4)!=state.generation) {
                xQueueReceive(outgoing,&pending,0); free(pending); sent_seq=0;
            } else if (last_rx && (pending->seq!=sent_seq || now-last_draw>4000000)) {
                if (usb_packet(RELAY_DRAWING,pending->seq,pending->payload,pending->len)) {
                    sent_seq=pending->seq; last_draw=now;
                }
            }
        }
        usb_log_t row;
        for (unsigned i=0;i<4 && xQueueReceive(usb_logs,&row,0)==pdTRUE;i++) {
            usb_write(row.text,strlen(row.text)); usb_write("\n",1);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
static void usb_start(void) {
    usb_logs=xQueueCreate(24,sizeof(usb_log_t)); configASSERT(usb_logs);
    usb_serial_jtag_driver_config_t config={.tx_buffer_size=4096,.rx_buffer_size=16384};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    esp_log_set_vprintf(usb_log);
    BaseType_t created=xTaskCreate(usb_task,"usb_relay",6144,NULL,2,NULL);
    configASSERT(created==pdPASS);
    ESP_LOGI(TAG,"USB relay ready node=%u room=%c channel=%u",node,'A'+node-1,online_channel());
}
