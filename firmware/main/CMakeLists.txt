idf_component_register(
    SRCS
        "main.c"
        "sdr_task.c"
        "dsp_task.c"
        "cpr_decode.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp32-rtl-sdr   # our patched RTL-SDR driver component
        espressif__usb  # USB host library (was core `usb` pre-IDF v6.0)
        esp_ringbuf     # FreeRTOS ring buffer (xRingbuffer*)
        esp_timer       # esp_timer_get_time()
)
