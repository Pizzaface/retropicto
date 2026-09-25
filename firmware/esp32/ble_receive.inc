/* Runs only in the BLE transport task, never in the GATT callback. */
static bool ble_receive(uint8_t *wire,size_t bytes,int64_t *last_rx,uint32_t *epoch) {
    if (bytes<RELAY_HEADER) return false;
    unsigned kind; uint32_t seq; size_t len;
    if (!relay_parse_header(wire,&kind,&seq,&len) || bytes!=RELAY_HEADER+len ||
        relay_hash(wire+RELAY_HEADER,len)!=relay_get32(wire+12)) return false;
    uint8_t *payload=wire+RELAY_HEADER;
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
        if (fresh) ESP_LOGI(TAG,"BLE peer connected node=%u",node);
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
