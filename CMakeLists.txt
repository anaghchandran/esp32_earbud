idf_component_register(
    SRCS
        "main.c"
        "audio.c"
        "bt.c"
        "ui.c"

    INCLUDE_DIRS
        "."

    PRIV_REQUIRES
        bt
        nvs_flash
        esp_driver_i2s
        esp_driver_i2c
        esp_ringbuf
)