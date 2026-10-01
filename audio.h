#pragma once

#include <stdint.h>
#include "esp_err.h"


typedef enum
{
    AUDIO_MODE_MEDIA,
    AUDIO_MODE_CALL,

} audio_mode_t;


/* Initialize speaker + microphone */

esp_err_t audio_init(void);


/* Change between music and call audio */

void audio_set_mode(audio_mode_t mode);


/* A2DP PCM data */

void audio_media_write(const uint8_t *data, uint32_t len);


/* HFP incoming audio */

void audio_sco_in(const uint8_t *data, uint32_t len);


/* HFP microphone audio */

uint32_t audio_sco_out(uint8_t *buf, uint32_t sz);