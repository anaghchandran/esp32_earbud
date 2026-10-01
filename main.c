#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "nvs_flash.h"

#include "esp_log.h"

#include "app.h"
#include "audio.h"
#include "bt.h"
#include "ui.h"


static const char *TAG = "EARBUD";


static QueueHandle_t s_q = NULL;

static ui_state_t st;


static bool setup_incoming = false;
static bool setup_outgoing = false;


/* ============================================================
 * EVENT QUEUE
 * ============================================================ */

bool app_post(const app_event_t *e)
{
    if (s_q == NULL || e == NULL)
        return false;


    return xQueueSend(
        s_q,
        e,
        0
    ) == pdTRUE;
}


/* ============================================================
 * UPDATE CALL STATE
 * ============================================================ */

static void update_call_state(void)
{
    if (setup_incoming)
    {
        st.call = CALL_INCOMING;
    }
    else if (setup_outgoing)
    {
        st.call = CALL_OUTGOING;
    }
    else if (st.sco)
    {
        st.call = CALL_ACTIVE;
    }
    else
    {
        st.call = CALL_IDLE;
    }
}


/* ============================================================
 * APPLICATION TASK
 * ============================================================ */

static void app_task(void *arg)
{
    app_event_t e;


    memset(
        &st,
        0,
        sizeof(st)
    );


    st.call = CALL_IDLE;


    ui_render(&st);


    while (1)
    {
        if (xQueueReceive(
                s_q,
                &e,
                portMAX_DELAY) != pdTRUE)
        {
            continue;
        }


        switch (e.type)
        {
            /* ==================================================
             * A2DP
             * ================================================== */

            case EV_A2DP_CONN:

                st.a2dp = e.arg;

                ESP_LOGI(
                    TAG,
                    "A2DP %s",
                    st.a2dp ?
                    "CONNECTED" :
                    "DISCONNECTED"
                );

                break;


            case EV_A2DP_AUDIO:

                if (e.arg)
                {
                    st.playing = true;
                }
                else
                {
                    st.playing = false;
                }

                break;


            /* ==================================================
             * AVRCP
             * ================================================== */

            case EV_AVRC_CONN:

                st.avrc = e.arg;

                ESP_LOGI(
                    TAG,
                    "AVRCP %s",
                    st.avrc ?
                    "CONNECTED" :
                    "DISCONNECTED"
                );


                if (st.avrc)
                {
                    bt_avrc_query_caps();
                }

                break;


            case EV_AVRC_CAPS:

                if (st.avrc)
                {
                    /*
                     * bit 0 = track change
                     * bit 1 = play status
                     */

                    bt_avrc_subscribe(0x03);

                    bt_avrc_request_meta();
                }

                break;


            case EV_AVRC_PLAY_STATUS:

                st.playing = e.arg != 0;

                if (st.avrc)
                {
                    bt_avrc_subscribe(0x02);
                }

                break;


            case EV_AVRC_TRACK_CHANGE:

                st.title[0] = '\0';
                st.artist[0] = '\0';

                if (st.avrc)
                {
                    bt_avrc_request_meta();

                    bt_avrc_subscribe(0x01);
                }

                break;


            case EV_AVRC_META:

                if (e.arg == 1)
                {
                    strncpy(
                        st.title,
                        e.text,
                        sizeof(st.title) - 1
                    );

                    st.title[
                        sizeof(st.title) - 1
                    ] = '\0';
                }
                else if (e.arg == 2)
                {
                    strncpy(
                        st.artist,
                        e.text,
                        sizeof(st.artist) - 1
                    );

                    st.artist[
                        sizeof(st.artist) - 1
                    ] = '\0';
                }

                break;


            /* ==================================================
             * HFP
             * ================================================== */

            case EV_HF_CONN:

                st.hfp = e.arg;

                ESP_LOGI(
                    TAG,
                    "HFP %s",
                    st.hfp ?
                    "CONNECTED" :
                    "DISCONNECTED"
                );


                if (!st.hfp)
                {
                    st.sco = false;

                    setup_incoming = false;
                    setup_outgoing = false;

                    audio_set_mode(
                        AUDIO_MODE_MEDIA
                    );
                }

                update_call_state();

                break;


            case EV_HF_CALL:

                /*
                 * e.arg = 0 -> no call
                 * e.arg = 1 -> active call
                 */

                if (e.arg)
                {
                    /*
                     * Actual SCO state will be reported
                     * by EV_HF_AUDIO.
                     */
                }

                update_call_state();

                break;


            case EV_HF_SETUP:

                if (e.arg == HF_SETUP_INCOMING)
                {
                    setup_incoming = true;
                    setup_outgoing = false;
                }
                else if (e.arg == HF_SETUP_OUTGOING)
                {
                    setup_outgoing = true;
                    setup_incoming = false;
                }
                else
                {
                    setup_incoming = false;
                    setup_outgoing = false;
                }


                update_call_state();

                break;


            case EV_HF_CLIP:

                strncpy(
                    st.number,
                    e.text,
                    sizeof(st.number) - 1
                );

                st.number[
                    sizeof(st.number) - 1
                ] = '\0';

                break;


            case EV_HF_AUDIO:

                st.sco = e.arg != 0;


                if (st.sco)
                {
                    /*
                     * Only switch to call audio when
                     * HFP service-level connection exists.
                     */

                    if (st.hfp)
                    {
                        audio_set_mode(
                            AUDIO_MODE_CALL
                        );
                    }
                }
                else
                {
                    audio_set_mode(
                        AUDIO_MODE_MEDIA
                    );
                }


                update_call_state();

                break;


            default:
                break;
        }


        ui_render(&st);
    }
}


/* ============================================================
 * NVS
 * ============================================================ */

static void nvs_init(void)
{
    esp_err_t err =
        nvs_flash_init();


    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(
            TAG,
            "NVS needs erase"
        );


        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );


        ESP_ERROR_CHECK(
            nvs_flash_init()
        );
    }
    else
    {
        ESP_ERROR_CHECK(err);
    }
}


/* ============================================================
 * MAIN
 * ============================================================ */

void app_main(void)
{
    ESP_LOGI(
        TAG,
        "===================================="
    );

    ESP_LOGI(
        TAG,
        "ESP32 EAR-BUD PROJECT"
    );

    ESP_LOGI(
        TAG,
        "A2DP + AVRCP + HFP"
    );

    ESP_LOGI(
        TAG,
        "===================================="
    );


    nvs_init();


    s_q =
        xQueueCreate(
            16,
            sizeof(app_event_t)
        );


    if (s_q == NULL)
    {
        ESP_LOGE(
            TAG,
            "Application queue creation failed"
        );

        return;
    }


    ui_init();


    esp_err_t err =
        audio_init();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Audio init failed: %s",
            esp_err_to_name(err)
        );

        return;
    }


    xTaskCreate(
        app_task,
        "app_task",
        5120,
        NULL,
        5,
        NULL
    );


    bt_init();


    ESP_LOGI(
        TAG,
        "System ready"
    );
}