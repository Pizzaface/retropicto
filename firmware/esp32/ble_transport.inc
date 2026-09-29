/* BLE GATT transport: PC/phone writes RX, subscribes to TX notifications.
 * Each fragment: little-endian uint16 offset, uint16 total, binary PCTR bytes.
 * Lost fragments invalidate the packet; existing end-to-end ACK/retry recovers.
 */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#if CONFIG_IDF_TARGET_ESP32C6
#include "driver/usb_serial_jtag.h"
#endif
#include <stdio.h>
#include <stdarg.h>
#define BLE_WIRE_MAX (RELAY_HEADER+RELAY_MAX_PAYLOAD)
static const ble_uuid128_t svc_uuid=BLE_UUID128_INIT(0x01,0x00,0x00,0x00,0x51,0x50,0x49,0x50,0x80,0x00,0x00,0x00,0x54,0x43,0x49,0x50);
static const ble_uuid128_t rx_uuid=BLE_UUID128_INIT(0x02,0x00,0x00,0x00,0x51,0x50,0x49,0x50,0x80,0x00,0x00,0x00,0x54,0x43,0x49,0x50);
static const ble_uuid128_t tx_uuid=BLE_UUID128_INIT(0x03,0x00,0x00,0x00,0x51,0x50,0x49,0x50,0x80,0x00,0x00,0x00,0x54,0x43,0x49,0x50);
static uint8_t own_addr_type;
static uint16_t tx_handle;
static atomic_uint ble_connection=BLE_HS_CONN_HANDLE_NONE, ble_revision;
static atomic_bool ble_subscribed;
static QueueHandle_t ble_packets, ble_logs;
typedef struct { size_t length; uint8_t bytes[BLE_WIRE_MAX]; } ble_packet_t;
static ble_packet_t ble_partial;
static size_t ble_used,ble_total;
static uint8_t ble_tx_wire[BLE_WIRE_MAX];
typedef struct { char text[256]; } ble_log_t;
static int ble_log(const char *fmt,va_list args) {
    ble_log_t row;
    int n=vsnprintf(row.text,sizeof(row.text),fmt,args);
    row.text[sizeof(row.text)-2]='\n'; row.text[sizeof(row.text)-1]=0;
    if (ble_logs) xQueueSend(ble_logs,&row,0);
    return n;
}
static void ble_log_task(void *arg) {
    (void)arg; ble_log_t row;
    for (;;) if (xQueueReceive(ble_logs,&row,portMAX_DELAY)==pdTRUE) {
#if CONFIG_IDF_TARGET_ESP32C6
        usb_serial_jtag_write_bytes(row.text,strlen(row.text),pdMS_TO_TICKS(20));
#else
        // UART/SDK console works on boards without native USB Serial/JTAG.
        // Any console blocking stays on this low-priority task, outside radio callbacks.
        fputs(row.text,stdout);
#endif
    }
}
static int ble_access(uint16_t conn,uint16_t attr,struct ble_gatt_access_ctxt *ctxt,void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op!=BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    uint8_t data[244]; uint16_t n=0;
    if (ble_hs_mbuf_to_flat(ctxt->om,data,sizeof(data),&n) || n<5) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    size_t offset=data[0]|((unsigned)data[1]<<8), total=data[2]|((unsigned)data[3]<<8);
    if (!offset) { ble_used=0; ble_total=total; }
    if (total<RELAY_HEADER || total>BLE_WIRE_MAX || total!=ble_total || offset!=ble_used || offset+n-4>total) {
        ble_used=ble_total=0; return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    memcpy(ble_partial.bytes+ble_used,data+4,n-4); ble_used+=n-4;
    if (ble_used==total) {
        ble_packet_t *packet=malloc(sizeof(*packet));
        if (packet) {
            packet->length=total; memcpy(packet->bytes,ble_partial.bytes,total);
            if (xQueueSend(ble_packets,&packet,0)!=pdTRUE) free(packet);
        }
        ble_used=ble_total=0;
    }
    return 0;
}
/* ESP-IDF's bleprph example declares this initializer explicitly. */
void ble_store_config_init(void);
static const struct ble_gatt_svc_def ble_services[]={
    {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&svc_uuid.u,.characteristics=(struct ble_gatt_chr_def[]){
        {.uuid=&rx_uuid.u,.access_cb=ble_access,.flags=BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_NO_RSP},
        {.uuid=&tx_uuid.u,.access_cb=ble_access,.val_handle=&tx_handle,.flags=BLE_GATT_CHR_F_NOTIFY},
        {0}}}, {0}};
static void ble_advertise(void);
static int ble_event(struct ble_gap_event *event,void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) ble_advertise();
        else {
            atomic_store(&ble_connection,event->connect.conn_handle);
            atomic_fetch_add(&ble_revision,1);
            ESP_LOGI(TAG,"BLE gateway connected");
            struct ble_gap_upd_params params={.itvl_min=24,.itvl_max=40,.latency=0,.supervision_timeout=400};
            ble_gap_update_params(event->connect.conn_handle,&params);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        atomic_store(&ble_connection,BLE_HS_CONN_HANDLE_NONE); atomic_store(&ble_subscribed,false);
        atomic_fetch_add(&ble_revision,1); ble_used=ble_total=0;
        ESP_LOGW(TAG,"BLE gateway disconnected"); ble_advertise(); break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle==tx_handle) atomic_store(&ble_subscribed,event->subscribe.cur_notify);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE: ble_advertise(); break;
    }
    return 0;
}
static void ble_advertise(void) {
    struct ble_hs_adv_fields fields={0};
    fields.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128=(ble_uuid128_t *)&svc_uuid; fields.num_uuids128=1; fields.uuids128_is_complete=1;
    int rc=ble_gap_adv_set_fields(&fields); if(rc) { ESP_LOGE(TAG,"BLE advertising fields rc=%d",rc); return; }
    const char *name=ble_svc_gap_device_name();
    struct ble_hs_adv_fields scan={.name=(uint8_t *)name,.name_len=strlen(name),.name_is_complete=1};
    ble_gap_adv_rsp_set_fields(&scan);
    struct ble_gap_adv_params params={.conn_mode=BLE_GAP_CONN_MODE_UND,.disc_mode=BLE_GAP_DISC_MODE_GEN,
                                    .itvl_min=320,.itvl_max=480};
    rc=ble_gap_adv_start(own_addr_type,NULL,BLE_HS_FOREVER,&params,ble_event,NULL);
    if(rc) ESP_LOGE(TAG,"BLE advertising start rc=%d",rc);
}
static void ble_sync(void) {
    int rc=ble_hs_util_ensure_addr(0); configASSERT(!rc);
    rc=ble_hs_id_infer_auto(0,&own_addr_type); configASSERT(!rc); ble_advertise();
}
static void ble_host(void *arg) { (void)arg; nimble_port_run(); nimble_port_freertos_deinit(); }
static bool ble_send(unsigned kind,uint32_t seq,unsigned from,unsigned to,const uint8_t *payload,size_t n) {
    if (!atomic_load(&ble_subscribed) || !relay_header(ble_tx_wire,kind,seq,from,to,payload,n)) return false;
    if (n) memcpy(ble_tx_wire+RELAY_HEADER,payload,n); size_t total=RELAY_HEADER+n;
    unsigned revision=atomic_load(&ble_revision), conn=atomic_load(&ble_connection);
    uint16_t mtu=ble_att_mtu(conn); if (mtu<23) return false;
    size_t chunk=mtu-7; if(chunk>240) chunk=240;
    for(size_t offset=0;offset<total;) {
        if(atomic_load(&ble_revision)!=revision || !atomic_load(&ble_subscribed)) return false;
        size_t count=total-offset; if(count>chunk) count=chunk;
        uint8_t fragment[244]={(uint8_t)offset,(uint8_t)(offset>>8),(uint8_t)total,(uint8_t)(total>>8)};
        memcpy(fragment+4,ble_tx_wire+offset,count);
        struct os_mbuf *om=ble_hs_mbuf_from_flat(fragment,count+4); if(!om) return false;
        if(ble_gatts_notify_custom(conn,tx_handle,om)) return false;
        offset+=count; vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}
static void ble_transport_task(void *arg) {
    (void)arg;
    int64_t last_rx=0;
    unsigned revision=atomic_load(&ble_revision);
    for(;;) {
        int64_t now=esp_timer_get_time();
        if(revision!=atomic_load(&ble_revision)) {
            revision=atomic_load(&ble_revision); last_rx=0;
            relay_forget_all(); relay_transmit(ble_send,now,false);
            ble_packet_t *old; while(xQueueReceive(ble_packets,&old,0)==pdTRUE) free(old);
        }
        ble_packet_t *packet;
        while(xQueueReceive(ble_packets,&packet,0)==pdTRUE) {
            if(relay_receive(packet->bytes,packet->length,1,now)) last_rx=now;
            free(packet);
        }
        if(last_rx && now-last_rx>PEER_TIMEOUT_US) { last_rx=0; relay_forget_all(); ESP_LOGW(TAG,"BLE peer timed out"); }
        if(atomic_load(&ble_subscribed)) relay_transmit(ble_send,now,true);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
static void ble_start(void) {
    ble_packets=xQueueCreate(2,sizeof(ble_packet_t *)); ble_logs=xQueueCreate(24,sizeof(ble_log_t));
    configASSERT(ble_packets && ble_logs);
#if CONFIG_IDF_TARGET_ESP32C6
    usb_serial_jtag_driver_config_t config={.tx_buffer_size=4096,.rx_buffer_size=256};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
#endif
    esp_log_set_vprintf(ble_log);
    configASSERT(xTaskCreate(ble_log_task,"ble_logs",3072,NULL,1,NULL)==pdPASS);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb=ble_sync;
    ble_hs_cfg.sm_bonding=1;
    ble_hs_cfg.sm_sc=1;
    ble_hs_cfg.sm_io_cap=BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    ble_svc_gap_init(); ble_svc_gatt_init();
    int rc=ble_gatts_count_cfg(ble_services); configASSERT(!rc);
    rc=ble_gatts_add_svcs(ble_services); configASSERT(!rc);
    rc=ble_svc_gap_device_name_set(node==1 ? "PICTO-A" : "PICTO-B"); configASSERT(!rc);
    nimble_port_freertos_init(ble_host);
    configASSERT(xTaskCreate(ble_transport_task,"ble_relay",4096,NULL,2,NULL)==pdPASS);
    ESP_LOGI(TAG,"BLE relay ready node=%u room=%c channel=%u heap=%u",node,'A'+node-1,online_channel(),(unsigned)esp_get_free_heap_size());
}
