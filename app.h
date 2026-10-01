#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 * PIN CONFIGURATION
 * ============================================================ */

/* MAX98357A Speaker - I2S0 */
#define PIN_SPK_BCLK        27
#define PIN_SPK_WS          14
#define PIN_SPK_DOUT        25

/* INMP441 Microphone - I2S1 */
#define PIN_MIC_SCK         26
#define PIN_MIC_WS          33
#define PIN_MIC_SD          32

/* SSD1306 OLED - I2C */
#define PIN_OLED_SDA        21
#define PIN_OLED_SCL        22
#define OLED_I2C_ADDR       0x3C


/* ============================================================
 * APPLICATION EVENTS
 * ============================================================ */

typedef enum
{
    EV_A2DP_CONN,
    EV_A2DP_AUDIO,

    EV_AVRC_CONN,
    EV_AVRC_CAPS,
    EV_AVRC_PLAY_STATUS,
    EV_AVRC_TRACK_CHANGE,
    EV_AVRC_META,

    EV_HF_CONN,
    EV_HF_CALL,
    EV_HF_SETUP,
    EV_HF_CLIP,
    EV_HF_AUDIO,

} app_evt_type_t;


/* HFP call setup state */

enum
{
    HF_SETUP_IDLE = 0,
    HF_SETUP_INCOMING,
    HF_SETUP_OUTGOING,
};


/* Application event */

typedef struct
{
    app_evt_type_t type;

    int32_t arg;

    char text[32];

} app_event_t;


/* Application event queue */

bool app_post(const app_event_t *e);


/* ============================================================
 * CALL STATE
 * ============================================================ */

typedef enum
{
    CALL_IDLE,
    CALL_INCOMING,
    CALL_OUTGOING,
    CALL_ACTIVE,

} call_state_t;


/* ============================================================
 * UI STATE
 * ============================================================ */

typedef struct
{
    bool a2dp;
    bool hfp;
    bool avrc;

    bool playing;
    bool sco;

    call_state_t call;

    char title[32];
    char artist[32];
    char number[32];

} ui_state_t;