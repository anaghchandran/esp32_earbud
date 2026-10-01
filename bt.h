#pragma once


typedef enum
{
    BT_MEDIA_PLAY,
    BT_MEDIA_PAUSE,
    BT_MEDIA_NEXT,
    BT_MEDIA_PREV,

} bt_media_cmd_t;


/* Bluetooth initialization */

void bt_init(void);


/* ============================================================
 * AVRCP
 * ============================================================ */

void bt_media(bt_media_cmd_t cmd);

void bt_avrc_query_caps(void);

void bt_avrc_subscribe(int mask);

void bt_avrc_request_meta(void);


/* ============================================================
 * HFP
 * ============================================================ */

void bt_hf_answer(void);

void bt_hf_hangup(void);

void bt_hf_connect_audio(void);