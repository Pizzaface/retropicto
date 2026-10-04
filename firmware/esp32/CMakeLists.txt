set(pictochat_requires esp_wifi esp_netif nvs_flash esp_timer lwip)
if(CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED)
    list(APPEND pictochat_requires esp_driver_usb_serial_jtag)
endif()
idf_component_register(
    SRCS "main.c" "online.c" "../../lib/pictochat/src/session.c" "../../lib/pictochat/src/frame.c" "../../lib/pictochat/src/room.c"
    INCLUDE_DIRS "../../lib/pictochat/include"
    REQUIRES ${pictochat_requires}
)
