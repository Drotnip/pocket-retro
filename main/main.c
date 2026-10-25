/*
 * Pocket Retro — a Galaga-style vertical shooter for the LilyGO T-Display C5.
 *
 * Controls (the board has exactly two buttons, and that is all this game
 * needs because the ship shoots by itself):
 *   - Left button  = move ship left
 *   - Right button = move ship right
 *   - Ship fires automatically (auto-fire is always ON)
 *   - On the title screen and the game-over screen, press EITHER button
 *     to start / restart.
 *
 * Game rules: 3 lives, score for every enemy destroyed, formation gets
 * faster every level. If an enemy reaches your ship's line, you lose a life.
 *
 * ---------------------------------------------------------------------------
 * PIN MAP — verified against LilyGO's official T-Display-C5 repository
 * (README "Pin Mapping" table, github.com/Xinyuan-LilyGO/T-Display-C5).
 * Display: 1.9" ST7789, 170 x 320, RGB565, 4-wire SPI.
 *
 *   LCD_CS   = GPIO 26   LCD_SCK  = GPIO 7    LCD_MOSI = GPIO 9
 *   LCD_DC   = GPIO 8    LCD_RST  = GPIO 23   LCD_BLK  = GPIO 25
 *   BTN_LEFT = GPIO 0    (User button)
 *   BTN_RIGHT= GPIO 28   (BOOT button)
 *
 * NOTE — a pin conflict inside LilyGO's own repo: their lcd.ino example
 * lists LCD_CS = 3 and LCD_RST = 0. Those two values collide with the
 * README table's I2C clock (GPIO 3) and the User button (GPIO 0), and
 * cannot both be true alongside the factory firmware, which uses the
 * button on GPIO 0 and I2C for the battery chip. This build uses the
 * README table (CS = 26, RST = 23). If your screen stays black, try the
 * lcd.ino values as a fallback (see README.md "If the screen stays
 * black").
 *
 * NOTE — left/right: if pressing the left button moves the ship right,
 * swap the two BTN_ lines below. Which physical button is "left" depends
 * on how you hold the board; see README.md "If left and right feel
 * swapped".
 * ---------------------------------------------------------------------------
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font5x7.h"

static const char *TAG = "pocket-retro";

/* ---------------- Display pins (LilyGO T-Display-C5 README table) -------- */
#define LCD_PIN_CS    26
#define LCD_PIN_SCK   7
#define LCD_PIN_MOSI  9
#define LCD_PIN_DC    8
#define LCD_PIN_RST   23
#define LCD_PIN_BL    25

/* ---------------- Button pins -------------------------------------------- */
/* Swap these two lines if left/right feel backwards on your board. */
#define BTN_LEFT_GPIO   0   /* User button */
#define BTN_RIGHT_GPIO  28  /* BOOT button */

/* ---------------- Screen size -------------------------------------------- */
#define SCR_W  170
#define SCR_H  320

/* ---------------- Colors (RGB565) ---------------------------------------- */
#define COL_BLACK   0x0000
#define COL_WHITE   0xFFFF
#define COL_GREEN   0x07E0
#define COL_RED     0xF800
#define COL_YELLOW  0xFFE0
#define COL_ORANGE  0xFD20
#define COL_CYAN    0x07FF

/* ---------------- Game tuning -------------------------------------------- */
#define SHIP_W       16
#define SHIP_H       10
#define SHIP_Y       302                 /* top edge of the ship */
#define SHIP_SPEED   5                   /* pixels per frame */
#define ENEMY_W      14
#define ENEMY_H      8
#define ENEMY_COLS   5
#define ENEMY_ROWS   3
#define ENEMY_X0     10                  /* formation left edge, offset 0 */
#define ENEMY_DX     30                  /* horizontal spacing */
#define ENEMY_Y0     20                  /* formation top edge */
#define ENEMY_DY     26                  /* vertical spacing */
#define BULLET_W     3
#define BULLET_H     8
#define BULLET_SPEED 8
#define MAX_BULLETS  8
#define FIRE_EVERY   11                  /* frames between shots (~180 ms) */
#define FRAME_MS     33                  /* ~30 frames per second */
#define START_LIVES  3

/* ---------------- Framebuffer -------------------------------------------- */
/* One full screen, 170 x 320 pixels x 2 bytes = 108,800 bytes, kept in
 * internal RAM so the SPI driver can read it directly. Every frame the
 * game draws into this buffer, then sends the whole buffer to the screen
 * in one go. */
static uint16_t framebuffer[SCR_W * SCR_H];

static esp_lcd_panel_handle_t panel = NULL;

/* ---------------- Small drawing helpers ---------------------------------- */

static inline void put_pixel(int x, int y, uint16_t color)
{
    if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H) {
        framebuffer[y * SCR_W + x] = color;
    }
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCR_W) { w = SCR_W - x; }
    if (y + h > SCR_H) { h = SCR_H - y; }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int row = 0; row < h; row++) {
        uint16_t *dst = &framebuffer[(y + row) * SCR_W + x];
        for (int col = 0; col < w; col++) {
            dst[col] = color;
        }
    }
}

/* Draw a sprite given as rows of 'X' (paint) and '.' (skip). */
static void draw_sprite(int x, int y, const char *rows[], int nrows, uint16_t color)
{
    for (int r = 0; r < nrows; r++) {
        const char *row = rows[r];
        for (int c = 0; row[c] != '\0'; c++) {
            if (row[c] == 'X') {
                put_pixel(x + c, y + r, color);
            }
        }
    }
}

/* Width in pixels of a string drawn at the given scale (1 = normal). */
static int text_width(const char *s, int scale)
{
    int w = 0;
    while (*s) {
        w += 6 * scale;
        s++;
    }
    return w > 0 ? w - scale : 0; /* no trailing gap after last char */
}

static void draw_char(int x, int y, char c, int scale, uint16_t color)
{
    const uint8_t *g = font5x7_get(c);
    if (g == NULL) {
        return; /* blank space */
    }
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                if (scale == 1) {
                    put_pixel(x + col, y + row, color);
                } else {
                    fill_rect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
    }
}

static void draw_text(int x, int y, const char *s, int scale, uint16_t color)
{
    int cx = x;
    while (*s) {
        draw_char(cx, y, *s, scale, color);
        cx += 6 * scale;
        s++;
    }
}

static void draw_text_centered(int y, const char *s, int scale, uint16_t color)
{
    int w = text_width(s, scale);
    draw_text((SCR_W - w) / 2, y, s, scale, color);
}

static void push_frame(void)
{
    esp_lcd_panel_draw_bitmap(panel, 0, 0, SCR_W, SCR_H, framebuffer);
}

/* ---------------- Sprites ------------------------------------------------- */

/* Player ship, 16 wide x 10 tall. */
static const char *ship_rows[SHIP_H] = {
    ".......XX.......",
    "......XXXX......",
    "......XXXX......",
    ".....XXXXXX.....",
    "....XXXXXXXX....",
    "...XXXXXXXXXX...",
    "..XXXXXXXXXXXX..",
    ".XXXXXXXXXXXXXX.",
    "XXXXXXXXXXXXXXXX",
    "X.XXXXXXXXXXXX.X",
};

/* Enemy bug, 14 wide x 8 tall. */
static const char *enemy_rows[ENEMY_H] = {
    "..X........X..",
    "...X......X...",
    "..XXXXXXXXXX..",
    ".XX.XXXXXX.XX.",
    "XXXXXXXXXXXXXX",
    "X.XXXXXXXXXX.X",
    "X.X.XXXXXX.X.X",
    "...XX....XX...",
};

/* Points per enemy row: top row is worth the most. */
static const int row_points[ENEMY_ROWS] = {30, 20, 10};
static const uint16_t row_colors[ENEMY_ROWS] = {COL_RED, COL_ORANGE, COL_YELLOW};

/* ---------------- Game state ---------------------------------------------- */

typedef enum {
    ST_TITLE,   /* "POCKET RETRO" — press a button to start */
    ST_PLAY,    /* playing */
    ST_OVER,    /* game over — press a button to restart */
} game_state_t;

typedef struct {
    int x, y;
    int active;
} bullet_t;

static game_state_t state = ST_TITLE;
static int ship_x = (SCR_W - SHIP_W) / 2;
static int lives = START_LIVES;
static int score = 0;
static int level = 1;
static int frame = 0;

/* Formation: enemies are either alive (1) or destroyed (0). Their screen
 * position is base_x + formation_x + col * ENEMY_DX, base_y + row * ENEMY_DY.
 * formation_x slides left/right; the whole formation drops when it hits
 * a screen edge. */
static int enemy_alive[ENEMY_ROWS][ENEMY_COLS];
static float formation_x = 0;
static int formation_dir = 1;   /* +1 = moving right, -1 = moving left */
static int formation_y = ENEMY_Y0;

static bullet_t bullets[MAX_BULLETS];
static int fire_timer = 0;
static int banner_timer = 0;    /* frames left showing "LEVEL n" */
static int invuln_timer = 0;    /* frames of blink protection after a hit */

static void reset_formation(void)
{
    for (int r = 0; r < ENEMY_ROWS; r++) {
        for (int c = 0; c < ENEMY_COLS; c++) {
            enemy_alive[r][c] = 1;
        }
    }
    formation_x = 0;
    formation_dir = 1;
    formation_y = ENEMY_Y0;
}

static void start_game(void)
{
    ship_x = (SCR_W - SHIP_W) / 2;
    lives = START_LIVES;
    score = 0;
    level = 1;
    frame = 0;
    fire_timer = 0;
    invuln_timer = 0;
    banner_timer = 90;          /* show "LEVEL 1" for ~3 seconds */
    for (int i = 0; i < MAX_BULLETS; i++) {
        bullets[i].active = 0;
    }
    reset_formation();
    state = ST_PLAY;
}

static int enemies_left(void)
{
    int n = 0;
    for (int r = 0; r < ENEMY_ROWS; r++) {
        for (int c = 0; c < ENEMY_COLS; c++) {
            if (enemy_alive[r][c]) {
                n++;
            }
        }
    }
    return n;
}

/* Left/right edges of the living formation, before formation_x is added. */
static void formation_bounds(int *left, int *right)
{
    int l = SCR_W, r = -1;
    for (int row = 0; row < ENEMY_ROWS; row++) {
        for (int col = 0; col < ENEMY_COLS; col++) {
            if (!enemy_alive[row][col]) {
                continue;
            }
            int x = ENEMY_X0 + col * ENEMY_DX;
            if (x < l) {
                l = x;
            }
            if (x + ENEMY_W > r) {
                r = x + ENEMY_W;
            }
        }
    }
    *left = l;
    *right = r;
}

static void lose_life(void)
{
    lives--;
    for (int i = 0; i < MAX_BULLETS; i++) {
        bullets[i].active = 0;
    }
    if (lives <= 0) {
        state = ST_OVER;
        return;
    }
    /* Same level, fresh formation, ship blinks briefly for protection. */
    reset_formation();
    ship_x = (SCR_W - SHIP_W) / 2;
    invuln_timer = 90;
    banner_timer = 0;
}

/* ---------------- Buttons -------------------------------------------------- */

/* Buttons are wired so that pressing one pulls its pin LOW. A pin reads 1
 * when the button is up, 0 when it is pressed. */
static void buttons_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BTN_LEFT_GPIO) | (1ULL << BTN_RIGHT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static int button_down(int gpio)
{
    return gpio_get_level(gpio) == 0;
}

/* True only on the frame a button is first pressed (not while held). */
static int any_button_pressed_edge(void)
{
    static int prev_left = 0, prev_right = 0;
    static int64_t last_edge_us = 0;
    int left = button_down(BTN_LEFT_GPIO);
    int right = button_down(BTN_RIGHT_GPIO);
    int edge = (left && !prev_left) || (right && !prev_right);
    prev_left = left;
    prev_right = right;
    if (!edge) {
        return 0;
    }
    /* Ignore extra edges for 150 ms (button bounce / shaky fingers). */
    int64_t now = esp_timer_get_time();
    if (now - last_edge_us < 150000) {
        return 0;
    }
    last_edge_us = now;
    return 1;
}

/* ---------------- Update (one frame of gameplay) --------------------------- */

static void update_play(void)
{
    frame++;

    /* Ship movement: hold a button to keep moving. */
    if (button_down(BTN_LEFT_GPIO) && !button_down(BTN_RIGHT_GPIO)) {
        ship_x -= SHIP_SPEED;
    } else if (button_down(BTN_RIGHT_GPIO) && !button_down(BTN_LEFT_GPIO)) {
        ship_x += SHIP_SPEED;
    }
    if (ship_x < 2) {
        ship_x = 2;
    }
    if (ship_x > SCR_W - SHIP_W - 2) {
        ship_x = SCR_W - SHIP_W - 2;
    }

    /* Auto-fire: the ship shoots by itself, no fire button needed. */
    if (fire_timer > 0) {
        fire_timer--;
    }
    if (fire_timer == 0) {
        for (int i = 0; i < MAX_BULLETS; i++) {
            if (!bullets[i].active) {
                bullets[i].active = 1;
                bullets[i].x = ship_x + SHIP_W / 2 - BULLET_W / 2;
                bullets[i].y = SHIP_Y - BULLET_H;
                break;
            }
        }
        /* Fire a little faster on higher levels, down to a floor. */
        fire_timer = FIRE_EVERY - (level - 1);
        if (fire_timer < 8) {
            fire_timer = 8;
        }
    }

    /* Move bullets up; switch off any that leave the top. */
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) {
            continue;
        }
        bullets[i].y -= BULLET_SPEED;
        if (bullets[i].y + BULLET_H < 12) {
            bullets[i].active = 0;
        }
    }

    /* Formation movement: slide sideways, drop when hitting an edge.
     * (Skipped when nothing is alive, so the edges stay sane.) */
    if (enemies_left() > 0) {
        float fspeed = 1.0f + (float)(level - 1) * 0.4f;
        if (fspeed > 3.0f) {
            fspeed = 3.0f;
        }
        formation_x += formation_dir * fspeed;
        int fl, fr;
        formation_bounds(&fl, &fr);
        if (fl + (int)formation_x < 4) {
            formation_x = (float)(4 - fl);
            formation_dir = 1;
            formation_y += 6 + (level - 1) * 2;
        } else if (fr + (int)formation_x > SCR_W - 4) {
            formation_x = (float)(SCR_W - 4 - fr);
            formation_dir = -1;
            formation_y += 6 + (level - 1) * 2;
        }
    }

    /* Bullet hits: a bullet overlapping an enemy destroys it. */
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) {
            continue;
        }
        int bx = bullets[i].x, by = bullets[i].y;
        for (int r = 0; r < ENEMY_ROWS && bullets[i].active; r++) {
            for (int c = 0; c < ENEMY_COLS && bullets[i].active; c++) {
                if (!enemy_alive[r][c]) {
                    continue;
                }
                int ex = ENEMY_X0 + (int)formation_x + c * ENEMY_DX;
                int ey = formation_y + r * ENEMY_DY;
                if (bx < ex + ENEMY_W && bx + BULLET_W > ex &&
                    by < ey + ENEMY_H && by + BULLET_H > ey) {
                    enemy_alive[r][c] = 0;
                    bullets[i].active = 0;
                    score += row_points[r];
                }
            }
        }
    }

    /* All enemies destroyed: next level, faster formation. */
    if (enemies_left() == 0) {
        level++;
        score += 100;           /* level-clear bonus */
        reset_formation();
        for (int i = 0; i < MAX_BULLETS; i++) {
            bullets[i].active = 0;
        }
        banner_timer = 90;
        return;
    }

    /* An enemy reaching the ship's line costs a life. */
    if (invuln_timer == 0) {
        for (int r = 0; r < ENEMY_ROWS; r++) {
            for (int c = 0; c < ENEMY_COLS; c++) {
                if (!enemy_alive[r][c]) {
                    continue;
                }
                int ey = formation_y + r * ENEMY_DY;
                if (ey + ENEMY_H >= SHIP_Y) {
                    lose_life();
                    return;
                }
            }
        }
    }

    if (banner_timer > 0) {
        banner_timer--;
    }
    if (invuln_timer > 0) {
        invuln_timer--;
    }
}

/* ---------------- Render --------------------------------------------------- */

static void render_hud(void)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "SCORE %d", score);
    draw_text(2, 2, buf, 1, COL_WHITE);
    snprintf(buf, sizeof(buf), "LIVES %d", lives);
    draw_text(SCR_W - text_width(buf, 1) - 2, 2, buf, 1, COL_WHITE);
}

static void render_play(void)
{
    /* Enemies. */
    for (int r = 0; r < ENEMY_ROWS; r++) {
        for (int c = 0; c < ENEMY_COLS; c++) {
            if (!enemy_alive[r][c]) {
                continue;
            }
            int ex = ENEMY_X0 + (int)formation_x + c * ENEMY_DX;
            int ey = formation_y + r * ENEMY_DY;
            draw_sprite(ex, ey, enemy_rows, ENEMY_H, row_colors[r]);
        }
    }

    /* Bullets. */
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (bullets[i].active) {
            fill_rect(bullets[i].x, bullets[i].y, BULLET_W, BULLET_H, COL_CYAN);
        }
    }

    /* Ship. After a hit it blinks while protected: visible 8 frames,
     * hidden 8 frames, until the protection timer runs out. */
    int show_ship = 1;
    if (invuln_timer > 0 && ((frame / 8) % 2) == 1) {
        show_ship = 0;
    }
    if (show_ship) {
        draw_sprite(ship_x, SHIP_Y, ship_rows, SHIP_H, COL_GREEN);
    }

    render_hud();

    /* "LEVEL n" banner at the start of a level. */
    if (banner_timer > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "LEVEL %d", level);
        draw_text_centered(150, buf, 2, COL_WHITE);
    }
}

static void render_title(void)
{
    draw_text_centered(60, "POCKET", 2, COL_GREEN);
    draw_text_centered(90, "RETRO", 2, COL_YELLOW);
    draw_sprite((SCR_W - SHIP_W) / 2, 150, ship_rows, SHIP_H, COL_GREEN);
    draw_text_centered(200, "PRESS A BUTTON", 1, COL_WHITE);
    draw_text_centered(214, "TO START", 1, COL_WHITE);
    draw_text_centered(260, "LEFT + RIGHT TO MOVE", 1, COL_CYAN);
    draw_text_centered(274, "SHIP SHOOTS ITSELF", 1, COL_CYAN);
}

static void render_over(void)
{
    draw_text_centered(80, "GAME OVER", 2, COL_RED);
    char buf[24];
    snprintf(buf, sizeof(buf), "SCORE %d", score);
    draw_text_centered(140, buf, 1, COL_WHITE);
    draw_text_centered(200, "PRESS A BUTTON", 1, COL_WHITE);
    draw_text_centered(214, "TO PLAY AGAIN", 1, COL_WHITE);
}

static void render(void)
{
    memset(framebuffer, 0, sizeof(framebuffer)); /* black background */
    switch (state) {
    case ST_TITLE:
        render_title();
        break;
    case ST_PLAY:
        render_play();
        break;
    case ST_OVER:
        render_over();
        break;
    }
    push_frame();
}

/* ---------------- Display setup -------------------------------------------- */

static void display_init(void)
{
    /* Backlight on. */
    gpio_config_t bl_cfg = {
        .pin_bit_mask = (1ULL << LCD_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(LCD_PIN_BL, 1);

    spi_bus_config_t bus_config = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,             /* display never sends data back */
        .sclk_io_num = LCD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* One full screen of RGB565 pixels, plus a few bytes of slack. */
        .max_transfer_sz = SCR_W * SCR_H * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = 40000000,           /* 40 MHz, as in LilyGO's factory demo */
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                             &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        /* BGR element order: matches LilyGO's own lcd.ino for this board
         * (it sets the panel's BGR bit in the MADCTL register). */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    /* Portrait 170 x 320. Horizontal-mirror fix: on real hardware the
     * picture was mirrored left-to-right (text read backwards, letters
     * upright) with USB-C at the bottom. Toggling mirror-X only
     * (MX=1 -> MX=0) un-mirrors it; mirror-Y is left exactly as it was
     * (MY=0) because the image was never upside down. */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 35, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    ESP_LOGI(TAG, "ST7789 display ready (%dx%d)", SCR_W, SCR_H);
}

/* ---------------- Main ------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "Pocket Retro starting");
    display_init();
    buttons_init();

    /* Show the title screen first; the game starts on a button press. */
    state = ST_TITLE;
    render();

    int64_t frame_start_us;
    while (1) {
        frame_start_us = esp_timer_get_time();

        if (state == ST_TITLE) {
            if (any_button_pressed_edge()) {
                start_game();
            }
            render();
        } else if (state == ST_PLAY) {
            update_play();
            if (state == ST_PLAY) {   /* update may have ended the game */
                render();
            } else {
                render();             /* show the game-over screen */
            }
        } else { /* ST_OVER */
            if (any_button_pressed_edge()) {
                start_game();
                render();
            } else {
                /* Redraw occasionally so the screen stays live. */
                render();
            }
        }

        /* Hold a steady ~30 frames per second. */
        int64_t elapsed_ms = (esp_timer_get_time() - frame_start_us) / 1000;
        if (elapsed_ms < FRAME_MS) {
            vTaskDelay(pdMS_TO_TICKS(FRAME_MS - elapsed_ms));
        }
    }
}
