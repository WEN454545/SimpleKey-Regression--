# ESP32-S3 三模键盘固件 - 主组件 CMakeLists.txt
# 作者：Regression
# 版本：1.0.0
# 日期：2026-09-18

idf_component_register(
    SRCS 
        "main.c"
        "matrix_driver.c"
        "encoder_driver.c"
        "led_driver.c"
        "usb_hid.c"
        "ble_hid.c"
        "hid_manager.c"
        "keymap.c"
        "macro.c"
        "profile_store.c"
        "serial_cmd.c"
        "cfg_frame.c"
        "power_mgr.c"
        "sleep_mgr.c"
        "wifi_ap.c"
        "net_input.c"
    INCLUDE_DIRS 
        "."
        "configs"
    REQUIRES 
        driver
        esp_timer
        nvs_flash
        bt
        esp_hid
        esp_adc
        esp_wifi
        esp_netif
        esp_event
        lwip
    PRIV_REQUIRES
        espressif__tinyusb
        espressif__esp_tinyusb
)
