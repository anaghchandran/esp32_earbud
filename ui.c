#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"

#include "esp_log.h"

#include "app.h"
#include "ui.h"


static const char *TAG = "UI";


/* ============================================================
 * OLED CONFIGURATION
 * ============================================================ */

#define OLED_WIDTH          128
#define OLED_HEIGHT         64
#define OLED_PAGES          8

#define OLED_I2C_TIMEOUT_MS 100


static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_oled = NULL;

static bool s_oled_ready = false;


/* ============================================================
 * OLED FRAME BUFFER
 * ============================================================ */

static uint8_t oled_buffer[
    OLED_WIDTH * OLED_HEIGHT / 8
];


/* ============================================================
 * SIMPLE 5x7 FONT
 *
 * Each character is 5 columns wide.
 * One extra pixel column is used as spacing.
 * ============================================================ */

static const uint8_t font5x7[][5] =
{
    /* SPACE */
    [0] = {0x00, 0x00, 0x00, 0x00, 0x00},

    /* A */
    ['A'] = {0x7E, 0x09, 0x09, 0x09, 0x7E},

    /* B */
    ['B'] = {0x7F, 0x49, 0x49, 0x49, 0x36},

    /* C */
    ['C'] = {0x3E, 0x41, 0x41, 0x41, 0x22},

    /* D */
    ['D'] = {0x7F, 0x41, 0x41, 0x22, 0x1C},

    /* E */
    ['E'] = {0x7F, 0x49, 0x49, 0x49, 0x41},

    /* F */
    ['F'] = {0x7F, 0x09, 0x09, 0x09, 0x01},

    /* G */
    ['G'] = {0x3E, 0x41, 0x49, 0x49, 0x7A},

    /* H */
    ['H'] = {0x7F, 0x08, 0x08, 0x08, 0x7F},

    /* I */
    ['I'] = {0x00, 0x41, 0x7F, 0x41, 0x00},

    /* J */
    ['J'] = {0x20, 0x40, 0x41, 0x3F, 0x01},

    /* K */
    ['K'] = {0x7F, 0x08, 0x14, 0x22, 0x41},

    /* L */
    ['L'] = {0x7F, 0x40, 0x40, 0x40, 0x40},

    /* M */
    ['M'] = {0x7F, 0x02, 0x0C, 0x02, 0x7F},

    /* N */
    ['N'] = {0x7F, 0x04, 0x08, 0x10, 0x7F},

    /* O */
    ['O'] = {0x3E, 0x41, 0x41, 0x41, 0x3E},

    /* P */
    ['P'] = {0x7F, 0x09, 0x09, 0x09, 0x06},

    /* Q */
    ['Q'] = {0x3E, 0x41, 0x51, 0x21, 0x5E},

    /* R */
    ['R'] = {0x7F, 0x09, 0x19, 0x29, 0x46},

    /* S */
    ['S'] = {0x46, 0x49, 0x49, 0x49, 0x31},

    /* T */
    ['T'] = {0x01, 0x01, 0x7F, 0x01, 0x01},

    /* U */
    ['U'] = {0x3F, 0x40, 0x40, 0x40, 0x3F},

    /* V */
    ['V'] = {0x1F, 0x20, 0x40, 0x20, 0x1F},

    /* W */
    ['W'] = {0x3F, 0x40, 0x38, 0x40, 0x3F},

    /* X */
    ['X'] = {0x63, 0x14, 0x08, 0x14, 0x63},

    /* Y */
    ['Y'] = {0x07, 0x08, 0x70, 0x08, 0x07},

    /* Z */
    ['Z'] = {0x61, 0x51, 0x49, 0x45, 0x43},


    /* 0 */
    ['0'] = {0x3E, 0x51, 0x49, 0x45, 0x3E},

    /* 1 */
    ['1'] = {0x00, 0x42, 0x7F, 0x40, 0x00},

    /* 2 */
    ['2'] = {0x42, 0x61, 0x51, 0x49, 0x46},

    /* 3 */
    ['3'] = {0x21, 0x41, 0x45, 0x4B, 0x31},

    /* 4 */
    ['4'] = {0x18, 0x14, 0x12, 0x7F, 0x10},

    /* 5 */
    ['5'] = {0x27, 0x45, 0x45, 0x45, 0x39},

    /* 6 */
    ['6'] = {0x3C, 0x4A, 0x49, 0x49, 0x30},

    /* 7 */
    ['7'] = {0x01, 0x71, 0x09, 0x05, 0x03},

    /* 8 */
    ['8'] = {0x36, 0x49, 0x49, 0x49, 0x36},

    /* 9 */
    ['9'] = {0x06, 0x49, 0x49, 0x29, 0x1E},


    /* punctuation */

    ['-'] = {0x08, 0x08, 0x08, 0x08, 0x08},

    ['.'] = {0x00, 0x60, 0x60, 0x00, 0x00},

    [':'] = {0x00, 0x36, 0x36, 0x00, 0x00},

    ['/'] = {0x20, 0x10, 0x08, 0x04, 0x02},

    ['?'] = {0x02, 0x01, 0x51, 0x09, 0x06},
};


/* ============================================================
 * OLED COMMAND
 * ============================================================ */

static esp_err_t oled_cmd(uint8_t cmd)
{
    uint8_t data[2];

    data[0] = 0x00;
    data[1] = cmd;

    return i2c_master_transmit(
        s_oled,
        data,
        sizeof(data),
        OLED_I2C_TIMEOUT_MS
    );
}


/* ============================================================
 * OLED DATA
 * ============================================================ */

static esp_err_t oled_data(
    const uint8_t *data,
    size_t len)
{
    if (len > 128)
        return ESP_ERR_INVALID_SIZE;

    uint8_t packet[129];

    packet[0] = 0x40;

    memcpy(
        &packet[1],
        data,
        len
    );

    return i2c_master_transmit(
        s_oled,
        packet,
        len + 1,
        OLED_I2C_TIMEOUT_MS
    );
}


/* ============================================================
 * OLED CLEAR
 * ============================================================ */

static void oled_clear(void)
{
    memset(
        oled_buffer,
        0,
        sizeof(oled_buffer)
    );
}


/* ============================================================
 * SET PIXEL
 * ============================================================ */

static void oled_pixel(
    int x,
    int y,
    bool on)
{
    if (x < 0 || x >= OLED_WIDTH)
        return;

    if (y < 0 || y >= OLED_HEIGHT)
        return;


    int index =
        x + (y / 8) * OLED_WIDTH;

    uint8_t mask =
        1 << (y & 7);


    if (on)
        oled_buffer[index] |= mask;
    else
        oled_buffer[index] &= ~mask;
}


/* ============================================================
 * DRAW CHARACTER
 * ============================================================ */

static void oled_char(
    int x,
    int y,
    char c)
{
    const uint8_t *glyph = NULL;


    if ((unsigned char)c < 128)
    {
        glyph = font5x7[
            (unsigned char)c
        ];
    }


    if (glyph == NULL)
        return;


    /*
     * Characters in the table that were not explicitly
     * initialized are zero, which gives a blank character.
     */

    for (int col = 0; col < 5; col++)
    {
        uint8_t bits = glyph[col];


        for (int row = 0; row < 7; row++)
        {
            if (bits & (1 << row))
            {
                oled_pixel(
                    x + col,
                    y + row,
                    true
                );
            }
        }
    }
}


/* ============================================================
 * DRAW STRING
 * ============================================================ */

static void oled_text(
    int x,
    int y,
    const char *text)
{
    if (text == NULL)
        return;


    while (*text)
    {
        oled_char(
            x,
            y,
            *text
        );

        x += 6;


        if (x >= OLED_WIDTH)
            break;


        text++;
    }
}


/* ============================================================
 * DRAW HORIZONTAL LINE
 * ============================================================ */

static void oled_hline(
    int y,
    int x1,
    int x2)
{
    if (y < 0 || y >= OLED_HEIGHT)
        return;


    if (x1 < 0)
        x1 = 0;

    if (x2 >= OLED_WIDTH)
        x2 = OLED_WIDTH - 1;


    for (int x = x1; x <= x2; x++)
    {
        oled_pixel(
            x,
            y,
            true
        );
    }
}


/* ============================================================
 * FLUSH FRAMEBUFFER
 * ============================================================ */

static esp_err_t oled_flush(void)
{
    if (!s_oled_ready)
        return ESP_ERR_INVALID_STATE;


    for (int page = 0;
         page < OLED_PAGES;
         page++)
    {
        esp_err_t err;


        err = oled_cmd(
            0xB0 | page
        );

        if (err != ESP_OK)
            return err;


        err = oled_cmd(
            0x00
        );

        if (err != ESP_OK)
            return err;


        err = oled_cmd(
            0x10
        );

        if (err != ESP_OK)
            return err;


        err = oled_data(
            &oled_buffer[
                page * OLED_WIDTH
            ],
            OLED_WIDTH
        );

        if (err != ESP_OK)
            return err;
    }


    return ESP_OK;
}


/* ============================================================
 * OLED INITIALIZATION
 * ============================================================ */

static esp_err_t oled_init(void)
{
    i2c_master_bus_config_t bus_cfg =
    {
        .i2c_port = I2C_NUM_0,

        .sda_io_num = PIN_OLED_SDA,
        .scl_io_num = PIN_OLED_SCL,

        .clk_source = I2C_CLK_SRC_DEFAULT,

        .glitch_ignore_cnt = 7,

        .flags =
        {
            .enable_internal_pullup = true,
        },
    };


    esp_err_t err =
        i2c_new_master_bus(
            &bus_cfg,
            &s_i2c_bus
        );


    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "I2C bus creation failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    i2c_device_config_t dev_cfg =
    {
        .dev_addr_length =
            I2C_ADDR_BIT_LEN_7,

        .device_address =
            OLED_I2C_ADDR,

        .scl_speed_hz =
            400000,
    };


    err =
        i2c_master_bus_add_device(
            s_i2c_bus,
            &dev_cfg,
            &s_oled
        );


    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "OLED device creation failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    /*
     * SSD1306 initialization
     */

    const uint8_t init_commands[] =
    {
        0xAE,       /* Display OFF */

        0xD5, 0x80, /* Display clock */

        0xA8, 0x3F, /* Multiplex */

        0xD3, 0x00, /* Display offset */

        0x40,       /* Start line */

        0x8D, 0x14, /* Charge pump ON */

        0x20, 0x00, /* Horizontal addressing */

        0xA1,       /* Segment remap */

        0xC8,       /* COM scan direction */

        0xDA, 0x12, /* COM pins */

        0x81, 0xCF, /* Contrast */

        0xD9, 0xF1, /* Pre-charge */

        0xDB, 0x40, /* VCOM */

        0xA4,       /* Display follows RAM */

        0xA6,       /* Normal display */

        0xAF        /* Display ON */
    };


    for (size_t i = 0;
         i < sizeof(init_commands);
         i++)
    {
        err =
            oled_cmd(
                init_commands[i]
            );


        if (err != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "OLED init command failed: %s",
                esp_err_to_name(err)
            );

            return err;
        }
    }


    s_oled_ready = true;


    oled_clear();

    oled_flush();


    ESP_LOGI(
        TAG,
        "SSD1306 OLED initialized"
    );


    return ESP_OK;
}


/* ============================================================
 * DRAW HEADER
 * ============================================================ */

static void draw_header(
    const char *text)
{
    oled_text(
        0,
        0,
        text
    );

    oled_hline(
        9,
        0,
        127
    );
}


/* ============================================================
 * DRAW MUSIC SCREEN
 * ============================================================ */

static void draw_music_screen(
    const ui_state_t *st)
{
    draw_header(
        "EARBUD A2DP"
    );


    if (!st->a2dp)
    {
        oled_text(
            0,
            18,
            "DISCONNECTED"
        );

        oled_text(
            0,
            32,
            "WAIT PHONE"
        );

        return;
    }


    if (st->playing)
    {
        oled_text(
            0,
            18,
            "PLAYING"
        );
    }
    else
    {
        oled_text(
            0,
            18,
            "PAUSED"
        );
    }


    if (st->title[0])
    {
        oled_text(
            0,
            32,
            st->title
        );
    }
    else
    {
        oled_text(
            0,
            32,
            "NO TITLE"
        );
    }


    if (st->artist[0])
    {
        oled_text(
            0,
            46,
            st->artist
        );
    }
    else
    {
        oled_text(
            0,
            46,
            "NO ARTIST"
        );
    }
}


/* ============================================================
 * DRAW INCOMING CALL
 * ============================================================ */

static void draw_incoming_call(
    const ui_state_t *st)
{
    draw_header(
        "INCOMING CALL"
    );


    oled_text(
        0,
        18,
        "PHONE CALL"
    );


    if (st->number[0])
    {
        oled_text(
            0,
            32,
            st->number
        );
    }
    else
    {
        oled_text(
            0,
            32,
            "UNKNOWN"
        );
    }


    oled_text(
        0,
        48,
        "ANSWER / REJECT"
    );
}


/* ============================================================
 * DRAW OUTGOING CALL
 * ============================================================ */

static void draw_outgoing_call(
    const ui_state_t *st)
{
    draw_header(
        "OUTGOING CALL"
    );


    oled_text(
        0,
        18,
        "CALLING"
    );


    if (st->number[0])
    {
        oled_text(
            0,
            32,
            st->number
        );
    }
}


/* ============================================================
 * DRAW ACTIVE CALL
 * ============================================================ */

static void draw_active_call(
    const ui_state_t *st)
{
    draw_header(
        "PHONE CALL"
    );


    oled_text(
        0,
        18,
        "CALL ACTIVE"
    );


    if (st->sco)
    {
        oled_text(
            0,
            32,
            "AUDIO CONNECTED"
        );
    }
    else
    {
        oled_text(
            0,
            32,
            "AUDIO WAIT"
        );
    }


    if (st->number[0])
    {
        oled_text(
            0,
            48,
            st->number
        );
    }
}


/* ============================================================
 * DRAW UI
 * ============================================================ */

void ui_render(
    const ui_state_t *st)
{
    if (!s_oled_ready)
        return;


    if (st == NULL)
        return;


    oled_clear();


    switch (st->call)
    {
        case CALL_INCOMING:

            draw_incoming_call(st);

            break;


        case CALL_OUTGOING:

            draw_outgoing_call(st);

            break;


        case CALL_ACTIVE:

            draw_active_call(st);

            break;


        case CALL_IDLE:

        default:

            draw_music_screen(st);

            break;
    }


    esp_err_t err =
        oled_flush();


    if (err != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "OLED flush failed: %s",
            esp_err_to_name(err)
        );
    }
}


/* ============================================================
 * UI INITIALIZATION
 * ============================================================ */

void ui_init(void)
{
    esp_err_t err =
        oled_init();


    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "OLED initialization failed"
        );

        s_oled_ready = false;

        return;
    }


    ESP_LOGI(
        TAG,
        "UI initialized"
    );
}