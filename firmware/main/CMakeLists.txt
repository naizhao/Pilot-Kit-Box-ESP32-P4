idf_component_register(
    SRCS
        "main.c"
        "sdr_task.c"
        "dsp_task.c"
        "cpr_decode.c"
        "aircraft_state.c"
        "gdl90.c"
        "ble_gatt.c"
        "record_sink.c"
        "record_sink_uart.c"
        "record_sink_file.c"
        "record_sink_ble.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp32-rtl-sdr            # our patched RTL-SDR driver component
        espressif__usb           # USB host library (was core `usb` pre-IDF v6.0)
        esp_ringbuf              # FreeRTOS ring buffer (xRingbuffer*)
        esp_timer                # esp_timer_get_time()
        joltwallet__littlefs     # on-board ts-format dump partition
        bt                       # NimBLE host (controller proxied via hosted)
        espressif__esp_hosted    # P4 ↔ C6 hosted transport (BT/Wi-Fi remote)
)
