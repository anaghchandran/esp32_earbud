#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_hf_client_api.h"

#include "app.h"
#include "audio.h"
#include "bt.h"

#define DEVICE_NAME "ESP32_EARBUD"

static const char *TAG = "BT";

static esp_bd_addr_t s_hf_peer;
static bool          s_hf_peer_valid;
static uint8_t       s_tl;

static uint8_t next_tl(void)
{
    return (s_tl++) & 0x0F;
}

static void post(app_evt_type_t type, int32_t arg, const char *text)
{
    app_event_t e = { .type = type, .arg = arg };

    if (text)
    {
        strlcpy(e.text, text, sizeof(e.text));
    }

    app_post(&e);
}


/* ============================================================
 * GAP
 * ============================================================ */

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event)
    {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS)
            {
                ESP_LOGI(TAG, "Authentication successful");
            }
            else
            {
                ESP_LOGE(TAG, "Authentication failed");
            }
            break;

        case ESP_BT_GAP_CFM_REQ_EVT:
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;

        case ESP_BT_GAP_PIN_REQ_EVT:
        {
            esp_bt_pin_code_t pin = { '0', '0', '0', '0' };
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
            break;
        }

        default:
            break;
    }
}


/* ============================================================
 * A2DP sink
 * ============================================================ */

static void a2dp_data_cb(const uint8_t *data, uint32_t len)
{
    audio_media_write(data, len);
}

static void a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event)
    {
        case ESP_A2D_CONNECTION_STATE_EVT:
            post(EV_A2DP_CONN,
                 param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED, NULL);
            break;

        case ESP_A2D_AUDIO_STATE_EVT:
            post(EV_A2DP_AUDIO,
                 param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED, NULL);
            break;

        case ESP_A2D_AUDIO_CFG_EVT:
            ESP_LOGI(TAG, "A2DP codec type: %d", param->audio_cfg.mcc.type);
            break;

        default:
            break;
    }
}


/* ============================================================
 * AVRCP controller
 * ============================================================ */

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *p)
{
    switch (event)
    {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            post(EV_AVRC_CONN, p->conn_stat.connected, NULL);
            break;

        case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        {
            int mask = 0;

            if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST,
                    &p->get_rn_caps_rsp.evt_set, ESP_AVRC_RN_TRACK_CHANGE))
            {
                mask |= 1;
            }

            if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST,
                    &p->get_rn_caps_rsp.evt_set, ESP_AVRC_RN_PLAY_STATUS_CHANGE))
            {
                mask |= 2;
            }

            post(EV_AVRC_CAPS, mask, NULL);
            break;
        }

        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
            if (p->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE)
            {
                post(EV_AVRC_PLAY_STATUS,
                     p->change_ntf.event_parameter.playback == ESP_AVRC_PLAYBACK_PLAYING,
                     NULL);
            }
            else if (p->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE)
            {
                post(EV_AVRC_TRACK_CHANGE, 0, NULL);
            }
            break;

        case ESP_AVRC_CT_METADATA_RSP_EVT:
        {
            /* attr_text is not NUL-terminated and only valid inside this callback */
            app_event_t e = { .type = EV_AVRC_META, .arg = p->meta_rsp.attr_id };
            size_t n = p->meta_rsp.attr_length;

            if (n > sizeof(e.text) - 1)
            {
                n = sizeof(e.text) - 1;
            }

            memcpy(e.text, p->meta_rsp.attr_text, n);
            e.text[n] = '\0';
            app_post(&e);
            break;
        }

        default:
            break;
    }
}

void bt_avrc_query_caps(void)
{
    esp_avrc_ct_send_get_rn_capabilities_cmd(next_tl());
}

void bt_avrc_subscribe(int mask)
{
    if (mask & 1)
    {
        esp_avrc_ct_send_register_notification_cmd(next_tl(), ESP_AVRC_RN_TRACK_CHANGE, 0);
    }

    if (mask & 2)
    {
        esp_avrc_ct_send_register_notification_cmd(next_tl(), ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
    }
}

void bt_avrc_request_meta(void)
{
    esp_avrc_ct_send_metadata_cmd(next_tl(),
                                  ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST);
}

void bt_media(bt_media_cmd_t cmd)
{
    uint8_t key;

    switch (cmd)
    {
        case BT_MEDIA_PLAY:  key = ESP_AVRC_PT_CMD_PLAY;     break;
        case BT_MEDIA_PAUSE: key = ESP_AVRC_PT_CMD_PAUSE;    break;
        case BT_MEDIA_NEXT:  key = ESP_AVRC_PT_CMD_FORWARD;  break;
        case BT_MEDIA_PREV:  key = ESP_AVRC_PT_CMD_BACKWARD; break;
        default: return;
    }

    /* A passthrough command is a press followed by a release */
    esp_avrc_ct_send_passthrough_cmd(next_tl(), key, ESP_AVRC_PT_CMD_STATE_PRESSED);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_avrc_ct_send_passthrough_cmd(next_tl(), key, ESP_AVRC_PT_CMD_STATE_RELEASED);
}


/* ============================================================
 * HFP client (hands-free unit)
 * ============================================================ */

static void hf_data_in_cb(const uint8_t *buf, uint32_t len)
{
    audio_sco_in(buf, len);

    /* Tell the stack it may fetch the next outgoing (mic) packet */
    esp_hf_client_outgoing_data_ready();
}

static uint32_t hf_data_out_cb(uint8_t *buf, uint32_t len)
{
    return audio_sco_out(buf, len);
}

static void hf_cb(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *p)
{
    switch (event)
    {
        case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
            if (p->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED)
            {
                memcpy(s_hf_peer, p->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
                s_hf_peer_valid = true;
                post(EV_HF_CONN, 1, NULL);
            }
            else if (p->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED)
            {
                post(EV_HF_CONN, 0, NULL);
            }
            break;

        case ESP_HF_CLIENT_AUDIO_STATE_EVT:
            post(EV_HF_AUDIO,
                 p->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED ||
                 p->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC,
                 NULL);
            break;

        case ESP_HF_CLIENT_CIND_CALL_EVT:
            post(EV_HF_CALL,
                 p->call.status == ESP_HF_CALL_STATUS_CALL_IN_PROGRESS, NULL);
            break;

        case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT:
        {
            int s = HF_SETUP_IDLE;

            if (p->call_setup.status == ESP_HF_CALL_SETUP_STATUS_INCOMING)
            {
                s = HF_SETUP_INCOMING;
            }
            else if (p->call_setup.status == ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING ||
                     p->call_setup.status == ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING)
            {
                s = HF_SETUP_OUTGOING;
            }

            post(EV_HF_SETUP, s, NULL);
            break;
        }

        case ESP_HF_CLIENT_CLIP_EVT:
            post(EV_HF_CLIP, 0, p->clip.number);
            break;

        default:
            break;
    }
}

void bt_hf_answer(void)
{
    esp_hf_client_answer_call();
}

void bt_hf_hangup(void)
{
    esp_hf_client_reject_call();    /* AT+CHUP: rejects ringing call or ends active call */
}

void bt_hf_connect_audio(void)
{
    if (s_hf_peer_valid)
    {
        esp_hf_client_connect_audio(s_hf_peer);
    }
}


/* ============================================================
 * Init
 * ============================================================ */

void bt_init(void)
{
    ESP_LOGI(TAG, "Initializing Bluetooth (Classic only)...");

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

#if CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY
    /* Classic-only build: give the unused BLE RAM back (must be before init) */
    esp_bt_mem_release(ESP_BT_MODE_BLE);
#endif

    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));

    /* Enable in the mode the controller was configured for (bt_cfg.mode).
     * A mismatch (e.g. CLASSIC_BT while menuconfig says BTDM) returns
     * ESP_ERR_INVALID_ARG. */
    ESP_ERROR_CHECK(esp_bt_controller_enable(bt_cfg.mode));

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bd_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(DEVICE_NAME));

    /* Class of Device: Audio/Video, wearable headset. Phones then treat us
     * as a headset (icon + HFP routing). Numeric values on purpose:
     * service = audio 0x100 | telephony 0x200 | rendering 0x20 | capturing 0x40 */
    esp_bt_cod_t cod = {0};
    cod.major   = 0x04;
    cod.minor   = 0x01;
    cod.service = 0x100 | 0x200 | 0x20 | 0x40;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL));

    /* AVRCP must be initialised BEFORE A2DP (this removes the
     * "A2DP Enable without AVRC" warning from the old log) */
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    ESP_ERROR_CHECK(esp_avrc_ct_register_callback(avrc_ct_cb));

    /* HFP client, HCI audio data path */
    ESP_ERROR_CHECK(esp_hf_client_register_callback(hf_cb));
    ESP_ERROR_CHECK(esp_hf_client_init());
    ESP_ERROR_CHECK(esp_hf_client_register_data_callback(hf_data_in_cb, hf_data_out_cb));

    /* A2DP sink */
    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dp_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(a2dp_data_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_init());

    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                             ESP_BT_GENERAL_DISCOVERABLE));

    ESP_LOGI(TAG, "Bluetooth ready, waiting for phone as '%s'", DEVICE_NAME);
}
