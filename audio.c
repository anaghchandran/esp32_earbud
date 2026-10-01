#include "audio.h"
#include "app.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"

#include "driver/i2s_std.h"


#define TAG "AUDIO"

/*
 * CALL_BIT is used by the microphone task.
 * If your app.h already defines CALL_BIT, this prevents redefinition.
 */
#ifndef CALL_BIT
#define CALL_BIT BIT0
#endif


/* ============================================================
 * Ring buffers
 * ============================================================ */

#define SPK_RB_SIZE        (16 * 1024)
#define MIC_RB_SIZE        (4 * 1024)


/* ============================================================
 * Microphone
 *
 * INMP441:
 *   SCK  -> GPIO26
 *   WS   -> GPIO33
 *   SD   -> GPIO32
 *   L/R  -> GND
 *
 * 16 kHz input
 * 32-bit I2S slot
 * LEFT channel
 *
 * 120 input samples @ 16 kHz
 * = 7.5 ms
 *
 * Downsample by 2
 *
 * 60 output samples @ 8 kHz
 * = 120 bytes
 *
 * This matches the HFP requested frame size seen
 * in your log: requested=120.
 * ============================================================ */

#define MIC_SAMPLE_RATE       16000
#define CALL_SAMPLE_RATE       8000

#define MIC_INPUT_SAMPLES       120
#define HFP_OUTPUT_SAMPLES       60
#define HFP_FRAME_BYTES         (HFP_OUTPUT_SAMPLES * sizeof(int16_t))


/* ============================================================
 * Speaker
 * ============================================================ */

#define MEDIA_SAMPLE_RATE     44100

#define SPK_BITS              16
#define SPK_CHANNELS           2


/* ============================================================
 * Handles
 * ============================================================ */

static i2s_chan_handle_t s_spk_tx = NULL;
static i2s_chan_handle_t s_mic_rx = NULL;


/* ============================================================
 * Ring buffers
 * ============================================================ */

static RingbufHandle_t s_spk_rb = NULL;
static RingbufHandle_t s_mic_rb = NULL;


/* ============================================================
 * Event group
 * ============================================================ */

static EventGroupHandle_t s_audio_events = NULL;


/* ============================================================
 * Speaker protection
 * ============================================================ */

static SemaphoreHandle_t s_spk_mutex = NULL;

static bool s_spk_enabled = false;
static bool s_mic_enabled = false;


/* ============================================================
 * Current mode
 * ============================================================ */

static volatile audio_mode_t s_audio_mode = AUDIO_MODE_MEDIA;


/* ============================================================
 * Forward declarations
 * ============================================================ */

static esp_err_t speaker_i2s_init(void);
static esp_err_t mic_i2s_init(void);

static esp_err_t speaker_set_sample_rate(uint32_t sample_rate);

static void speaker_task(void *arg);
static void mic_task(void *arg);

static void ringbuffer_drain(RingbufHandle_t rb);


/* ============================================================
 * Drain ringbuffer
 * ============================================================ */

static void ringbuffer_drain(RingbufHandle_t rb)
{
    if (rb == NULL) {
        return;
    }

    while (1) {

        size_t item_len = 0;

        void *item =
            xRingbufferReceive(
                rb,
                &item_len,
                0
            );

        if (item == NULL) {
            break;
        }

        vRingbufferReturnItem(
            rb,
            item
        );
    }
}


/* ============================================================
 * Speaker I2S initialization
 * ============================================================ */

static esp_err_t speaker_i2s_init(void)
{
    ESP_LOGI(
        TAG,
        "Initializing speaker I2S"
    );

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(
            I2S_NUM_0,
            I2S_ROLE_MASTER
        );

    /*
     * Stereo 16-bit:
     *
     * 256 frames
     * x 2 slots
     * x 2 bytes
     *
     * = 1024 bytes DMA buffer
     */
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;

    esp_err_t err =
        i2s_new_channel(
            &chan_cfg,
            &s_spk_tx,
            NULL
        );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Speaker i2s_new_channel failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    i2s_std_config_t cfg = {

        .clk_cfg =
            I2S_STD_CLK_DEFAULT_CONFIG(
                MEDIA_SAMPLE_RATE
            ),

        /*
         * MAX98357A:
         *
         * 16-bit
         * stereo
         * MSB format
         */
        .slot_cfg =
            I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_16BIT,
                I2S_SLOT_MODE_STEREO
            ),

        .gpio_cfg = {

            .mclk = I2S_GPIO_UNUSED,

            .bclk = PIN_SPK_BCLK,

            .ws = PIN_SPK_WS,

            .dout = PIN_SPK_DOUT,

            .din = I2S_GPIO_UNUSED,

            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };


    err =
        i2s_channel_init_std_mode(
            s_spk_tx,
            &cfg
        );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Speaker I2S init failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    err =
        i2s_channel_enable(
            s_spk_tx
        );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Speaker I2S enable failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    s_spk_enabled = true;

    ESP_LOGI(
        TAG,
        "Speaker I2S ready: %u Hz",
        MEDIA_SAMPLE_RATE
    );

    return ESP_OK;
}


/* ============================================================
 * Microphone I2S initialization
 * ============================================================ */

static esp_err_t mic_i2s_init(void)
{
    ESP_LOGI(
        TAG,
        "Initializing INMP441 I2S"
    );

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(
            I2S_NUM_1,
            I2S_ROLE_MASTER
        );

    /*
     * 120 frames
     * x 1 slot
     * x 4 bytes
     *
     * = 480 bytes DMA buffer
     */
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = MIC_INPUT_SAMPLES;


    esp_err_t err =
        i2s_new_channel(
            &chan_cfg,
            NULL,
            &s_mic_rx
        );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Mic i2s_new_channel failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    i2s_std_config_t cfg = {

        .clk_cfg =
            I2S_STD_CLK_DEFAULT_CONFIG(
                MIC_SAMPLE_RATE
            ),

        /*
         * INMP441 produces 24-bit audio
         * inside 32-bit I2S slots.
         *
         * L/R = GND
         * therefore use LEFT slot.
         */
        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_32BIT,
                I2S_SLOT_MODE_MONO
            ),

        .gpio_cfg = {

            .mclk = I2S_GPIO_UNUSED,

            .bclk = PIN_MIC_SCK,

            .ws = PIN_MIC_WS,

            .dout = I2S_GPIO_UNUSED,

            .din = PIN_MIC_SD,

            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };


    /*
     * L/R = GND
     *
     * INMP441 sends LEFT channel.
     */
    cfg.slot_cfg.slot_mask =
        I2S_STD_SLOT_LEFT;


    err =
        i2s_channel_init_std_mode(
            s_mic_rx,
            &cfg
        );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Mic I2S init failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    /*
     * IMPORTANT:
     *
     * Do NOT enable here.
     *
     * The microphone should run only
     * while CALL_BIT is set.
     */
    s_mic_enabled = false;


    ESP_LOGI(
        TAG,
        "INMP441 I2S ready: 16kHz / 32-bit / LEFT"
    );

    return ESP_OK;
}


/* ============================================================
 * Change speaker sample rate
 * ============================================================ */

static esp_err_t speaker_set_sample_rate(uint32_t sample_rate)
{
    if (s_spk_tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }


    if (xSemaphoreTake(
            s_spk_mutex,
            pdMS_TO_TICKS(200)
        ) != pdTRUE) {

        ESP_LOGE(
            TAG,
            "Speaker mutex timeout"
        );

        return ESP_ERR_TIMEOUT;
    }


    esp_err_t err = ESP_OK;


    /*
     * Only disable if currently running.
     *
     * This avoids:
     *
     * "channel has not been enabled yet"
     */
    if (s_spk_enabled) {

        err =
            i2s_channel_disable(
                s_spk_tx
            );

        if (err != ESP_OK &&
            err != ESP_ERR_INVALID_STATE) {

            ESP_LOGE(
                TAG,
                "Speaker disable failed: %s",
                esp_err_to_name(err)
            );

            xSemaphoreGive(
                s_spk_mutex
            );

            return err;
        }

        s_spk_enabled = false;
    }


    i2s_std_clk_config_t clk_cfg =
        I2S_STD_CLK_DEFAULT_CONFIG(
            sample_rate
        );


    err =
        i2s_channel_reconfig_std_clock(
            s_spk_tx,
            &clk_cfg
        );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Speaker clock reconfig failed: %s",
            esp_err_to_name(err)
        );

        xSemaphoreGive(
            s_spk_mutex
        );

        return err;
    }


    err =
        i2s_channel_enable(
            s_spk_tx
        );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Speaker re-enable failed: %s",
            esp_err_to_name(err)
        );

        xSemaphoreGive(
            s_spk_mutex
        );

        return err;
    }


    s_spk_enabled = true;


    ESP_LOGI(
        TAG,
        "Speaker sample rate = %lu",
        (unsigned long)sample_rate
    );


    xSemaphoreGive(
        s_spk_mutex
    );


    return ESP_OK;
}


/* ============================================================
 * Speaker task
 * ============================================================ */

static void speaker_task(void *arg)
{
    (void)arg;

    /*
     * Call mode:
     *
     * 16-bit mono
     * becomes
     * 16-bit stereo
     */
    int16_t stereo_buf[256 * 2];


    while (1) {

        size_t item_len = 0;

        uint8_t *item =
            (uint8_t *)xRingbufferReceive(
                s_spk_rb,
                &item_len,
                pdMS_TO_TICKS(100)
            );


        if (item == NULL) {
            continue;
        }


        /*
         * Take mutex before I2S write.
         *
         * This prevents speaker_task from writing
         * while speaker_set_sample_rate() is
         * disabling/reconfiguring/enabling I2S.
         */
        if (xSemaphoreTake(
                s_spk_mutex,
                pdMS_TO_TICKS(100)
            ) != pdTRUE) {

            vRingbufferReturnItem(
                s_spk_rb,
                item
            );

            continue;
        }


        bool call_mode =
            (s_audio_mode == AUDIO_MODE_CALL);

        bool enabled =
            s_spk_enabled;


        if (!enabled) {

            xSemaphoreGive(
                s_spk_mutex
            );

            vRingbufferReturnItem(
                s_spk_rb,
                item
            );

            continue;
        }


        /*
         * ----------------------------------------------------
         * CALL MODE
         * ----------------------------------------------------
         *
         * HFP downlink is expected to be:
         *
         * 16-bit mono PCM
         *
         * MAX98357A is configured:
         *
         * 16-bit stereo
         *
         * Therefore duplicate mono:
         *
         * L = sample
         * R = sample
         */
        if (call_mode) {

            size_t offset = 0;


            while (offset + 2 <= item_len) {

                size_t remaining =
                    item_len - offset;

                size_t samples =
                    remaining / 2;


                if (samples > 256) {
                    samples = 256;
                }


                int16_t *mono =
                    (int16_t *)(item + offset);


                for (size_t i = 0;
                     i < samples;
                     i++) {

                    stereo_buf[i * 2] =
                        mono[i];

                    stereo_buf[i * 2 + 1] =
                        mono[i];
                }


                size_t stereo_bytes =
                    samples *
                    2 *
                    sizeof(int16_t);


                size_t written = 0;

                esp_err_t err =
                    i2s_channel_write(
                        s_spk_tx,
                        stereo_buf,
                        stereo_bytes,
                        &written,
                        pdMS_TO_TICKS(50)
                    );


                if (err != ESP_OK) {

                    static uint32_t error_count = 0;

                    error_count++;

                    if ((error_count % 50) == 0) {

                        ESP_LOGW(
                            TAG,
                            "Call speaker write failed: %s",
                            esp_err_to_name(err)
                        );
                    }

                    break;
                }


                offset +=
                    samples * 2;
            }
        }


        /*
         * ----------------------------------------------------
         * MEDIA MODE
         * ----------------------------------------------------
         *
         * A2DP callback already supplies
         * decoded PCM data.
         */
        else {

            size_t written = 0;

            esp_err_t err =
                i2s_channel_write(
                    s_spk_tx,
                    item,
                    item_len,
                    &written,
                    pdMS_TO_TICKS(50)
                );


            if (err != ESP_OK) {

                static uint32_t error_count = 0;

                error_count++;

                if ((error_count % 50) == 0) {

                    ESP_LOGW(
                        TAG,
                        "Media speaker write failed: %s",
                        esp_err_to_name(err)
                    );
                }
            }
        }


        xSemaphoreGive(
            s_spk_mutex
        );


        vRingbufferReturnItem(
            s_spk_rb,
            item
        );
    }
}


/* ============================================================
 * Microphone task
 * ============================================================ */

static void mic_task(void *arg)
{
    (void)arg;


    /*
     * Exactly:
     *
     * 120 x 32-bit samples
     *
     * = 480 bytes
     */
    int32_t raw[MIC_INPUT_SAMPLES];


    /*
     * Output:
     *
     * 60 x 16-bit samples
     *
     * = 120 bytes
     */
    int16_t hfp_frame[HFP_OUTPUT_SAMPLES];


    uint32_t timeout_count = 0;
    uint32_t frame_count = 0;


    while (1) {

        /*
         * Wait until CALL mode.
         */
        xEventGroupWaitBits(
            s_audio_events,
            CALL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY
        );


        /*
         * Enable RX channel.
         *
         * READY -> RUNNING
         */
        if (!s_mic_enabled) {

            esp_err_t err =
                i2s_channel_enable(
                    s_mic_rx
                );


            if (err != ESP_OK) {

                ESP_LOGE(
                    TAG,
                    "MIC I2S enable failed: %s",
                    esp_err_to_name(err)
                );

                vTaskDelay(
                    pdMS_TO_TICKS(100)
                );

                continue;
            }


            s_mic_enabled = true;


            ESP_LOGI(
                TAG,
                "Microphone START"
            );

            ESP_LOGI(
                TAG,
                "MIC I2S ENABLED"
            );
        }


        /*
         * Clear frame counter when a call starts.
         */
        timeout_count = 0;
        frame_count = 0;


        /*
         * Read microphone continuously
         * while CALL_BIT is active.
         */
        while (
            xEventGroupGetBits(
                s_audio_events
            ) & CALL_BIT
        ) {

            size_t bytes_read = 0;


            esp_err_t err =
                i2s_channel_read(
                    s_mic_rx,
                    raw,
                    sizeof(raw),
                    &bytes_read,
                    pdMS_TO_TICKS(100)
                );


            /*
             * ------------------------------------------------
             * Timeout
             * ------------------------------------------------
             */
            if (err == ESP_ERR_TIMEOUT) {

                timeout_count++;


                /*
                 * DO NOT print every timeout.
                 *
                 * Otherwise serial output itself
                 * can become a problem.
                 */
                if ((timeout_count % 100) == 0) {

                    ESP_LOGW(
                        TAG,
                        "Mic I2S timeout count=%lu",
                        (unsigned long)timeout_count
                    );
                }


                continue;
            }


            /*
             * ------------------------------------------------
             * Other I2S error
             * ------------------------------------------------
             */
            if (err != ESP_OK) {

                ESP_LOGE(
                    TAG,
                    "Mic I2S read failed: %s",
                    esp_err_to_name(err)
                );

                continue;
            }


            /*
             * ------------------------------------------------
             * Successful read
             * ------------------------------------------------
             */
            size_t sample_count =
                bytes_read / sizeof(int32_t);


            if (sample_count < 2) {
                continue;
            }


            /*
             * Debug occasionally.
             *
             * This tells us immediately whether
             * the INMP441 is actually producing data.
             */
            frame_count++;


            if ((frame_count % 50) == 0) {

                ESP_LOGI(
                    TAG,
                    "MIC RAW: bytes=%u sample0=%ld sample1=%ld sample2=%ld sample3=%ld",
                    (unsigned)bytes_read,
                    (long)raw[0],
                    (long)raw[1],
                    (long)raw[2],
                    (long)raw[3]
                );
            }


            /*
             * ------------------------------------------------
             * 16 kHz -> 8 kHz
             * ------------------------------------------------
             *
             * Use simple 2:1 averaging.
             *
             * Input:
             *
             *   16k sample A
             *   16k sample B
             *
             * Output:
             *
             *   average(A,B)
             *
             * This gives approximately:
             *
             *   8 kHz
             */
            size_t output_index = 0;


            for (
                size_t i = 0;
                i + 1 < sample_count;
                i += 2
            ) {

                /*
                 * INMP441 data is delivered
                 * in a 32-bit slot.
                 *
                 * Extract the upper 16 bits.
                 */
                int32_t sample_a =
                    raw[i] >> 16;

                int32_t sample_b =
                    raw[i + 1] >> 16;


                /*
                 * Average two samples.
                 */
                int32_t avg =
                    (sample_a + sample_b) / 2;


                /*
                 * Clamp to signed 16-bit.
                 */
                if (avg > 32767) {
                    avg = 32767;
                }

                if (avg < -32768) {
                    avg = -32768;
                }


                hfp_frame[output_index++] =
                    (int16_t)avg;


                /*
                 * 60 samples = 120 bytes
                 */
                if (output_index ==
                    HFP_OUTPUT_SAMPLES) {

                    BaseType_t ok =
                        xRingbufferSend(
                            s_mic_rb,
                            hfp_frame,
                            HFP_FRAME_BYTES,
                            pdMS_TO_TICKS(10)
                        );


                    if (ok != pdTRUE) {

                        static uint32_t rb_error_count = 0;

                        rb_error_count++;

                        if ((rb_error_count % 50) == 0) {

                            ESP_LOGW(
                                TAG,
                                "MIC ringbuffer full"
                            );
                        }
                    }


                    else {

                        static uint32_t ready_count = 0;

                        ready_count++;

                        if ((ready_count % 50) == 0) {

                            ESP_LOGI(
                                TAG,
                                "HFP MIC FRAME READY: %u bytes",
                                HFP_FRAME_BYTES
                            );
                        }
                    }


                    output_index = 0;
                }
            }
        }


        /*
         * ----------------------------------------------------
         * Call ended.
         * ----------------------------------------------------
         */

        if (s_mic_enabled) {

            esp_err_t err =
                i2s_channel_disable(
                    s_mic_rx
                );


            if (err != ESP_OK &&
                err != ESP_ERR_INVALID_STATE) {

                ESP_LOGW(
                    TAG,
                    "Microphone disable failed: %s",
                    esp_err_to_name(err)
                );
            }


            s_mic_enabled = false;


            ESP_LOGI(
                TAG,
                "MIC I2S DISABLED"
            );


            ESP_LOGI(
                TAG,
                "Microphone STOP"
            );
        }


        /*
         * Remove any partially generated
         * microphone data from the previous call.
         */
        ringbuffer_drain(
            s_mic_rb
        );
    }
}


/* ============================================================
 * Public: audio_init()
 * ============================================================ */

esp_err_t audio_init(void)
{
    ESP_LOGI(
        TAG,
        "Audio initialization"
    );


    /*
     * Event group
     */
    s_audio_events =
        xEventGroupCreate();

    if (s_audio_events == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to create audio event group"
        );

        return ESP_ERR_NO_MEM;
    }


    /*
     * Speaker mutex
     */
    s_spk_mutex =
        xSemaphoreCreateMutex();

    if (s_spk_mutex == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to create speaker mutex"
        );

        return ESP_ERR_NO_MEM;
    }


    /*
     * Speaker ringbuffer
     */
    s_spk_rb =
        xRingbufferCreate(
            SPK_RB_SIZE,
            RINGBUF_TYPE_BYTEBUF
        );

    if (s_spk_rb == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to create speaker ringbuffer"
        );

        return ESP_ERR_NO_MEM;
    }


    /*
     * Microphone ringbuffer
     */
    s_mic_rb =
        xRingbufferCreate(
            MIC_RB_SIZE,
            RINGBUF_TYPE_BYTEBUF
        );

    if (s_mic_rb == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to create microphone ringbuffer"
        );

        return ESP_ERR_NO_MEM;
    }


    /*
     * Initialize speaker
     */
    esp_err_t err =
        speaker_i2s_init();

    if (err != ESP_OK) {
        return err;
    }


    /*
     * Initialize microphone.
     *
     * It remains READY.
     *
     * It is NOT enabled until CALL_BIT.
     */
    err =
        mic_i2s_init();

    if (err != ESP_OK) {
        return err;
    }


    /*
     * Start speaker task.
     */
    BaseType_t task_ok =
        xTaskCreate(
            speaker_task,
            "speaker_task",
            4096,
            NULL,
            6,
            NULL
        );

    if (task_ok != pdPASS) {

        ESP_LOGE(
            TAG,
            "Failed to create speaker task"
        );

        return ESP_ERR_NO_MEM;
    }


    /*
     * Start microphone task.
     */
    task_ok =
        xTaskCreate(
            mic_task,
            "mic_task",
            4096,
            NULL,
            6,
            NULL
        );

    if (task_ok != pdPASS) {

        ESP_LOGE(
            TAG,
            "Failed to create microphone task"
        );

        return ESP_ERR_NO_MEM;
    }


    s_audio_mode =
        AUDIO_MODE_MEDIA;


    ESP_LOGI(
        TAG,
        "Audio initialized"
    );


    ESP_LOGI(
        TAG,
        "Speaker = GPIO %d / %d / %d",
        PIN_SPK_BCLK,
        PIN_SPK_WS,
        PIN_SPK_DOUT
    );


    ESP_LOGI(
        TAG,
        "Mic = GPIO %d / %d / %d",
        PIN_MIC_SCK,
        PIN_MIC_WS,
        PIN_MIC_SD
    );


    return ESP_OK;
}


/* ============================================================
 * Public: audio_set_mode()
 * ============================================================ */

void audio_set_mode(audio_mode_t mode)
{
    if (mode == s_audio_mode) {
        return;
    }


    if (mode == AUDIO_MODE_CALL) {

        ESP_LOGI(
            TAG,
            "AUDIO MODE -> CALL"
        );


        /*
         * Remove old media data.
         *
         * We don't want old A2DP audio
         * to be played during the call.
         */
        ringbuffer_drain(
            s_spk_rb
        );


        /*
         * Remove old mic data.
         */
        ringbuffer_drain(
            s_mic_rb
        );


        /*
         * Change speaker to 8 kHz.
         */
        esp_err_t err =
            speaker_set_sample_rate(
                CALL_SAMPLE_RATE
            );


        if (err != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Failed to switch speaker to CALL mode"
            );

            return;
        }


        /*
         * Set mode AFTER speaker is ready.
         */
        s_audio_mode =
            AUDIO_MODE_CALL;


        /*
         * Start microphone task.
         */
        xEventGroupSetBits(
            s_audio_events,
            CALL_BIT
        );


        ESP_LOGI(
            TAG,
            "CALL audio active"
        );
    }


    else {

        ESP_LOGI(
            TAG,
            "AUDIO MODE -> MEDIA"
        );


        /*
         * Stop microphone task first.
         */
        xEventGroupClearBits(
            s_audio_events,
            CALL_BIT
        );


        /*
         * Remove call audio from speaker buffer.
         */
        ringbuffer_drain(
            s_spk_rb
        );


        /*
         * Change speaker back to media rate.
         */
        esp_err_t err =
            speaker_set_sample_rate(
                MEDIA_SAMPLE_RATE
            );


        if (err != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Failed to switch speaker to MEDIA mode"
            );

            return;
        }


        s_audio_mode =
            AUDIO_MODE_MEDIA;


        ESP_LOGI(
            TAG,
            "MEDIA audio active"
        );
    }
}


/* ============================================================
 * Public: A2DP media input
 * ============================================================ */

void audio_media_write(
    const uint8_t *data,
    uint32_t len
)
{
    if (data == NULL || len == 0) {
        return;
    }


    /*
     * Don't queue A2DP audio during a call.
     */
    if (s_audio_mode != AUDIO_MODE_MEDIA) {
        return;
    }


    if (s_spk_rb == NULL) {
        return;
    }


    BaseType_t ok =
        xRingbufferSend(
            s_spk_rb,
            data,
            len,
            0
        );


    if (ok != pdTRUE) {

        static uint32_t rb_error_count = 0;

        rb_error_count++;

        if ((rb_error_count % 50) == 0) {

            ESP_LOGW(
                TAG,
                "Media speaker ringbuffer full"
            );
        }
    }
}


/* ============================================================
 * Public: HFP incoming audio
 *
 * Phone -> ESP32 -> speaker
 * ============================================================ */

void audio_sco_in(
    const uint8_t *data,
    uint32_t len
)
{
    if (data == NULL || len == 0) {
        return;
    }


    /*
     * Only accept HFP audio during CALL mode.
     */
    if (s_audio_mode != AUDIO_MODE_CALL) {
        return;
    }


    if (s_spk_rb == NULL) {
        return;
    }


    BaseType_t ok =
        xRingbufferSend(
            s_spk_rb,
            data,
            len,
            0
        );


    if (ok != pdTRUE) {

        static uint32_t rb_error_count = 0;

        rb_error_count++;

        if ((rb_error_count % 50) == 0) {

            ESP_LOGW(
                TAG,
                "Call speaker ringbuffer full"
            );
        }
    }
}


/* ============================================================
 * Public: HFP outgoing microphone audio
 *
 * ESP32 microphone -> HFP -> phone
 *
 * Your current HFP callback requests:
 *
 *     requested=120
 *
 * So we return exactly 120 bytes.
 * ============================================================ */

uint32_t audio_sco_out(
    uint8_t *buf,
    uint32_t sz
)
{
    if (buf == NULL || sz == 0) {
        return 0;
    }


    if (s_audio_mode != AUDIO_MODE_CALL) {
        return 0;
    }


    if (s_mic_rb == NULL) {
        return 0;
    }


    /*
     * Current HFP requested frame size
     * observed in your log:
     *
     * requested=120
     */
    if (sz != HFP_FRAME_BYTES) {

        ESP_LOGW(
            TAG,
            "HFP requested unexpected size=%lu",
            (unsigned long)sz
        );

        return 0;
    }


    size_t item_len = 0;


    /*
     * Very short wait.
     *
     * Normally the microphone should already
     * have a frame available.
     */
    uint8_t *item =
        (uint8_t *)xRingbufferReceive(
            s_mic_rb,
            &item_len,
            pdMS_TO_TICKS(2)
        );


    /*
     * No microphone packet ready.
     */
    if (item == NULL) {

        static uint32_t empty_count = 0;

        empty_count++;


        if ((empty_count % 50) == 0) {

            ESP_LOGW(
                TAG,
                "HFP MIC OUT: requested=%lu item=0 len=0",
                (unsigned long)sz
            );
        }


        return 0;
    }


    /*
     * Packet size must be exactly 120 bytes.
     */
    if (item_len != sz) {

        ESP_LOGW(
            TAG,
            "HFP MIC OUT wrong item size=%u expected=%lu",
            (unsigned)item_len,
            (unsigned long)sz
        );


        vRingbufferReturnItem(
            s_mic_rb,
            item
        );


        return 0;
    }


    memcpy(
        buf,
        item,
        sz
    );


    vRingbufferReturnItem(
        s_mic_rb,
        item
    );


    static uint32_t out_count = 0;

    out_count++;


    if ((out_count % 50) == 0) {

        ESP_LOGI(
            TAG,
            "HFP MIC OUT: requested=%lu len=%u",
            (unsigned long)sz,
            (unsigned)item_len
        );
    }


    return sz;
}