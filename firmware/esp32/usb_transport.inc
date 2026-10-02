/* Included by online.c: PCTR v2 frames as "@PCTR <hex>\n" lines over the native
 * USB Serial/JTAG port. The bridge (phone or PC) assigns remote peer ids. */
#include <stdio.h>
#include <stdarg.h>
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
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
static bool usb_send(unsigned kind,uint32_t seq,unsigned from,unsigned to,const uint8_t *data,size_t n) {
    uint8_t header[RELAY_HEADER];
    if (!relay_header(header,kind,seq,from,to,data,n)) return false;
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
static void usb_info(void) {
    char text[112], build[17];
    const esp_app_desc_t *app=esp_app_get_description();
    for (int i=0;i<8;i++) snprintf(build+2*i,3,"%02x",app->app_elf_sha256[i]);
    int n=snprintf(text,sizeof(text),"\n@INFO room=%c channel=%u local=%u ghosts=%u build=%s\n",'A'+node-1,online_channel(),LOCAL_SLOTS,GHOST_SLOTS,build);
    usb_write(text,(size_t)n);
}
static bool usb_line(size_t size,int64_t now) {
    if (size==6 && !memcmp(usb_rx_line,"@INFO?",6)) { usb_info(); return true; }
    if (size==7 && !memcmp(usb_rx_line,"@ROOM ",6) && usb_rx_line[6]>='A' && usb_rx_line[6]<='D') {
        bool ok=online_room_save((unsigned)(usb_rx_line[6]-'A'+1));
        usb_write(ok ? "\n@ROOM OK\n" : "\n@ROOM FAIL\n",ok ? 10 : 12);
        if (ok) { vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
        return ok;
    }
    if (size<6+2*RELAY_HEADER || memcmp(usb_rx_line,"@PCTR ",6) || (size-6)%2) return false;
    size_t bytes=(size-6)/2;
    if (bytes>sizeof(usb_wire)) return false;
    for (size_t i=0;i<bytes;i++) {
        int a=hex_digit(usb_rx_line[6+2*i]),b=hex_digit(usb_rx_line[7+2*i]);
        if (a<0 || b<0) return false;
        usb_wire[i]=(uint8_t)((a<<4)|b);
    }
    return relay_receive(usb_wire,bytes,0,now);
}
static void usb_task(void *arg) {
    (void)arg;
    size_t used=0; bool overflow=false;
    for (;;) {
        uint8_t input[256];
        int count=usb_serial_jtag_read_bytes(input,sizeof(input),pdMS_TO_TICKS(2));
        int64_t now=esp_timer_get_time();
        for (int i=0;i<count;i++) {
            if (input[i]=='\n') {
                if (!overflow) usb_line(used,now);
                used=0; overflow=false;
            } else if (input[i]!='\r') {
                if (used<USB_LINE_MAX) usb_rx_line[used++]=(char)input[i]; else overflow=true;
            }
        }
        relay_transmit(usb_send,now,true);
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
    ESP_LOGI(TAG,"USB relay ready node=%u room=%c channel=%u local=%u ghosts=%u",node,'A'+node-1,online_channel(),LOCAL_SLOTS,GHOST_SLOTS);
    usb_info();
}
