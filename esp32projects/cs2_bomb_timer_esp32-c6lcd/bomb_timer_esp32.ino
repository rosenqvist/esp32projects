/*
 * CS2 Bomb Timer — ESP32-C6 + ST7789 172x320 LCD
 *
 * Protocol (9-field CSV):
 *   ticking,defused,time_left,timer_length,being_defused,has_kit,hp_after,defuse_time_left,planting
 */

#include "LVGL_Driver.h"

// ============================================================================
// ESP32-C6 USB SERIAL
// ============================================================================
#if ARDUINO_USB_CDC_ON_BOOT
    #define USBPort Serial
#elif defined(ARDUINO_ESP32C6_DEV) || defined(ARDUINO_ESP32S3_DEV)
    #ifdef USBSerial
        #define USBPort USBSerial
    #else
        #define USBPort Serial
    #endif
#else
    #define USBPort Serial
#endif

// ============================================================================
// CONFIG
// ============================================================================
const int LED_PIN = 8;
const uint32_t SERIAL_TIMEOUT_MS = 2000;
const uint32_t EXPLODE_HOLD_MS   = 4000;   // hold explosion screen for 4s
const uint32_t DEFUSED_HOLD_MS   = 4000;   // hold defused screen for 4s

// ============================================================================
// STATE
// ============================================================================
enum BombState {
    STATE_IDLE,
    STATE_PLANTING,
    STATE_TICKING,
    STATE_DEFUSING,
    STATE_DEFUSE_LATE,
    STATE_DEFUSED,
    STATE_EXPLODED,
    STATE_RUN
};

struct {
    BombState state          = STATE_IDLE;
    float     time_left      = 0.0f;
    float     timer_total    = 40.0f;
    bool      being_defused  = false;
    bool      has_kit        = false;
    int       hp_after       = -1;
    float     defuse_tleft   = -1.0f;
    bool      planting       = false;
    uint32_t  last_data      = 0;
    bool      bridge_connected = false;
    // hold timers
    uint32_t  exploded_at    = 0;
    uint32_t  defused_at     = 0;
    bool      holding_explode = false;
    bool      holding_defuse  = false;
} bomb;

char serial_buf[128];
uint8_t serial_idx = 0;

// ============================================================================
// LVGL UI
// ============================================================================
static lv_obj_t* scr            = NULL;
static lv_obj_t* lbl_conn       = NULL;
static lv_obj_t* lbl_face       = NULL;
static lv_obj_t* lbl_status     = NULL;
static lv_obj_t* lbl_timer      = NULL;
static lv_obj_t* bar_progress   = NULL;
static lv_obj_t* lbl_info       = NULL;
static lv_obj_t* lbl_damage     = NULL;

// ============================================================================
// COLORS
// ============================================================================
static lv_color_t COL_BG;
static lv_color_t COL_RED;
static lv_color_t COL_ORANGE;
static lv_color_t COL_GREEN;
static lv_color_t COL_YELLOW;
static lv_color_t COL_WHITE;
static lv_color_t COL_DIM;
static lv_color_t COL_CYAN;

static uint32_t last_blink = 0;
static bool led_on = false;

// ============================================================================
// SETUP
// ============================================================================
void setup() {
    USBPort.begin(115200);
    delay(500);

    if (LED_PIN >= 0) {
        pinMode(LED_PIN, OUTPUT);
        digitalWrite(LED_PIN, LOW);
    }

    LCD_Init();
    Set_Backlight(75);
    Lvgl_Init();

    COL_BG     = lv_color_hex(0x111122);
    COL_RED    = lv_color_hex(0xff1744);
    COL_ORANGE = lv_color_hex(0xff9100);
    COL_GREEN  = lv_color_hex(0x00e676);
    COL_YELLOW = lv_color_hex(0xffea00);
    COL_WHITE  = lv_color_hex(0xffffff);
    COL_DIM    = lv_color_hex(0x555577);
    COL_CYAN   = lv_color_hex(0x00e5ff);

    ui_create();
}

// ============================================================================
// LOOP
// ============================================================================
void loop() {
    read_serial();
    bomb.bridge_connected = (millis() - bomb.last_data) < SERIAL_TIMEOUT_MS;
    update_led();
    ui_update();
    Timer_Loop();
}

// ============================================================================
// SERIAL
// ============================================================================
void read_serial() {
    while (USBPort.available()) {
        char c = USBPort.read();
        if (c == '\n' || c == '\r') {
            if (serial_idx > 0) {
                serial_buf[serial_idx] = '\0';
                parse_line(serial_buf);
                serial_idx = 0;
            }
        } else if (serial_idx < sizeof(serial_buf) - 1) {
            serial_buf[serial_idx++] = c;
        }
    }
}

// ============================================================================
// PARSE — 9 fields
// ============================================================================
void parse_line(const char* line) {
    String s = String(line);
    int idx[8];
    idx[0] = s.indexOf(',');
    for (int i = 1; i < 8; i++) {
        idx[i] = s.indexOf(',', idx[i-1] + 1);
        if (idx[i] < 0) return;
    }

    int   ticking       = s.substring(0, idx[0]).toInt();
    int   defused       = s.substring(idx[0]+1, idx[1]).toInt();
    float time_left     = s.substring(idx[1]+1, idx[2]).toFloat();
    float timer_len     = s.substring(idx[2]+1, idx[3]).toFloat();
    int   being_defused = s.substring(idx[3]+1, idx[4]).toInt();
    int   has_kit       = s.substring(idx[4]+1, idx[5]).toInt();
    int   hp_after      = s.substring(idx[5]+1, idx[6]).toInt();
    float defuse_tleft  = s.substring(idx[6]+1, idx[7]).toFloat();
    int   planting      = s.substring(idx[7]+1).toInt();

    bomb.last_data      = millis();
    bomb.time_left      = time_left;
    bomb.timer_total    = timer_len;
    bomb.being_defused  = (being_defused == 1);
    bomb.has_kit        = (has_kit == 1);
    bomb.hp_after       = hp_after;
    bomb.defuse_tleft   = defuse_tleft;
    bomb.planting       = (planting == 1);

    // --- state machine ---
    BombState new_state = STATE_IDLE;

    if (ticking == 1 && defused == 0) {
        if (time_left <= 0.01f) {
            new_state = STATE_EXPLODED;
        }
        else if (bomb.being_defused) {
            if (bomb.defuse_tleft >= 0 && bomb.defuse_tleft < time_left) {
                new_state = STATE_DEFUSING;
            } else {
                new_state = STATE_DEFUSE_LATE;
            }
        }
        else if (time_left < 5.0f) {
            new_state = STATE_RUN;
        }
        else {
            new_state = STATE_TICKING;
        }
    }
    else if (defused == 1) {
        new_state = STATE_DEFUSED;
        bomb.time_left = 0;
    }
    else if (bomb.planting) {
        new_state = STATE_PLANTING;
    }
    else {
        new_state = STATE_IDLE;
        bomb.time_left = 0;
    }

    // --- hold logic: keep showing exploded/defused for a few seconds ---
    if (new_state == STATE_EXPLODED && bomb.state != STATE_EXPLODED) {
        bomb.exploded_at = millis();
        bomb.holding_explode = true;
    }
    if (new_state == STATE_DEFUSED && bomb.state != STATE_DEFUSED) {
        bomb.defused_at = millis();
        bomb.holding_defuse = true;
    }

    // if data says idle but we're holding an end state, keep it
    if (new_state == STATE_IDLE || new_state == STATE_PLANTING) {
        if (bomb.holding_explode && (millis() - bomb.exploded_at < EXPLODE_HOLD_MS)) {
            new_state = STATE_EXPLODED;
        } else {
            bomb.holding_explode = false;
        }

        if (bomb.holding_defuse && (millis() - bomb.defused_at < DEFUSED_HOLD_MS)) {
            new_state = STATE_DEFUSED;
        } else {
            bomb.holding_defuse = false;
        }
    }

    // once a new round really starts (ticking again), clear holds
    if (new_state == STATE_TICKING || new_state == STATE_PLANTING) {
        bomb.holding_explode = false;
        bomb.holding_defuse = false;
    }

    bomb.state = new_state;
}

// ============================================================================
// HELPERS
// ============================================================================
lv_color_t get_zone_color(float ratio) {
    if (ratio > 0.5f)  return COL_GREEN;
    if (ratio > 0.25f) return COL_ORANGE;
    return COL_RED;
}

lv_color_t get_hp_color(int hp) {
    if (hp < 0)  return COL_DIM;
    if (hp == 0) return COL_RED;
    if (hp < 40) return COL_ORANGE;
    return COL_GREEN;
}

// ============================================================================
// LED
// ============================================================================
uint32_t get_blink_interval() {
    if (bomb.time_left <= 0) return 80;
    if (bomb.time_left > 20.0f) return 1200;
    if (bomb.time_left > 10.0f) return 600;
    if (bomb.time_left > 5.0f)  return 300;
    float t = bomb.time_left / 5.0f;
    return (uint32_t)(80.0f + 120.0f * t);
}

void update_led() {
    uint32_t now = millis();

    switch (bomb.state) {
        case STATE_IDLE:
        case STATE_DEFUSED:
            Set_Backlight(70);
            if (LED_PIN >= 0) digitalWrite(LED_PIN, LOW);
            led_on = false;
            break;

        case STATE_PLANTING:
            Set_Backlight(80);
            // slow pulse
            if (now - last_blink >= 800) {
                last_blink = now;
                led_on = !led_on;
                if (LED_PIN >= 0) digitalWrite(LED_PIN, led_on ? HIGH : LOW);
            }
            break;

        case STATE_TICKING:
        case STATE_RUN: {
            Set_Backlight(85);
            uint32_t interval = get_blink_interval();
            if (now - last_blink >= interval) {
                last_blink = now;
                led_on = !led_on;
                if (LED_PIN >= 0) digitalWrite(LED_PIN, led_on ? HIGH : LOW);
            }
            break;
        }

        case STATE_DEFUSING:
            Set_Backlight(80);
            if (now - last_blink >= 700) {
                last_blink = now;
                led_on = !led_on;
                if (LED_PIN >= 0) digitalWrite(LED_PIN, led_on ? HIGH : LOW);
            }
            break;

        case STATE_DEFUSE_LATE:
            Set_Backlight(90);
            if (now - last_blink >= 150) {
                last_blink = now;
                led_on = !led_on;
                if (LED_PIN >= 0) digitalWrite(LED_PIN, led_on ? HIGH : LOW);
            }
            break;

        case STATE_EXPLODED:
            Set_Backlight(90);
            if (now - last_blink >= 120) {
                last_blink = now;
                led_on = !led_on;
                if (LED_PIN >= 0) digitalWrite(LED_PIN, led_on ? HIGH : LOW);
            }
            break;
    }
}

// ============================================================================
// UI 
// ============================================================================

// pick the largest available font for the face
#if LV_FONT_MONTSERRAT_48
    #define FACE_FONT &lv_font_montserrat_48
#elif LV_FONT_MONTSERRAT_36
    #define FACE_FONT &lv_font_montserrat_36
#else
    #define FACE_FONT LV_FONT_DEFAULT
#endif

void ui_create() {
    scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);

    // connection — tiny top left
    lbl_conn = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_conn, COL_DIM, 0);
    lv_label_set_text(lbl_conn, "...");
    lv_obj_align(lbl_conn, LV_ALIGN_TOP_LEFT, 4, 2);

    // face — BIG, top center
    lbl_face = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_face, COL_DIM, 0);
    lv_obj_set_style_text_font(lbl_face, FACE_FONT, 0);
    lv_label_set_text(lbl_face, "");
    lv_obj_set_style_text_align(lbl_face, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_face, LCD_WIDTH);
    lv_obj_align(lbl_face, LV_ALIGN_TOP_MID, 0, 15);

    // status text
    lbl_status = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_status, COL_DIM, 0);
    lv_label_set_text(lbl_status, "STANDBY");
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_status, LCD_WIDTH);
    #if LV_FONT_MONTSERRAT_14
        lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
    #endif
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 70);

    // big timer
    lbl_timer = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_timer, COL_WHITE, 0);
    lv_label_set_text(lbl_timer, "--:--");
    lv_obj_set_style_text_align(lbl_timer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_timer, LCD_WIDTH);
    #if LV_FONT_MONTSERRAT_36
        lv_obj_set_style_text_font(lbl_timer, &lv_font_montserrat_36, 0);
    #endif
    lv_obj_align(lbl_timer, LV_ALIGN_CENTER, 0, -5);

    // progress bar
    bar_progress = lv_bar_create(scr);
    lv_obj_set_size(bar_progress, LCD_WIDTH - 24, 10);
    lv_bar_set_range(bar_progress, 0, 1000);
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x222244), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_progress, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_progress, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_progress, 4, LV_PART_INDICATOR);
    lv_obj_align(bar_progress, LV_ALIGN_CENTER, 0, 28);

    // info line
    lbl_info = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_info, COL_DIM, 0);
    lv_label_set_text(lbl_info, "");
    lv_obj_set_style_text_align(lbl_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_info, LCD_WIDTH);
    #if LV_FONT_MONTSERRAT_14
        lv_obj_set_style_text_font(lbl_info, &lv_font_montserrat_14, 0);
    #endif
    lv_obj_align(lbl_info, LV_ALIGN_CENTER, 0, 48);

    // HP / survival — bottom
    lbl_damage = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_damage, COL_DIM, 0);
    lv_label_set_text(lbl_damage, "");
    lv_obj_set_style_text_align(lbl_damage, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_damage, LCD_WIDTH);
    #if LV_FONT_MONTSERRAT_14
        lv_obj_set_style_text_font(lbl_damage, &lv_font_montserrat_14, 0);
    #endif
    lv_obj_align(lbl_damage, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// ============================================================================
// TIMER TEXT
// ============================================================================
void set_timer_text(float t) {
    if (t < 0) t = 0;
    int sec    = (int)t;
    int tenths = ((int)(t * 10)) % 10;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d.%d", sec / 60, sec % 60, tenths);
    lv_label_set_text(lbl_timer, buf);
}

// ============================================================================
// UI UPDATE
// ============================================================================
void ui_update() {
    // connection dot
    if (bomb.bridge_connected) {
        lv_label_set_text(lbl_conn, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(lbl_conn, COL_GREEN, 0);
    } else {
        lv_label_set_text(lbl_conn, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(lbl_conn, COL_RED, 0);
    }

    int progress = 0;
    if (bomb.timer_total > 0 && bomb.time_left > 0) {
        progress = (int)((bomb.time_left / bomb.timer_total) * 1000.0f);
        if (progress > 1000) progress = 1000;
    }
    float ratio = (bomb.timer_total > 0) ? (bomb.time_left / bomb.timer_total) : 0;
    char hp_buf[40];

    switch (bomb.state) {

        // ==== IDLE ====
        case STATE_IDLE: {
            const char* faces[] = { "-_-", "=_=", "-_-", "._." };
            lv_label_set_text(lbl_face, faces[(millis() / 2000) % 4]);
            lv_obj_set_style_text_color(lbl_face, COL_DIM, 0);

            lv_label_set_text(lbl_status, "STANDBY");
            lv_obj_set_style_text_color(lbl_status, COL_DIM, 0);
            lv_label_set_text(lbl_timer, "--:--");
            lv_obj_set_style_text_color(lbl_timer, COL_DIM, 0);
            lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar_progress, COL_DIM, LV_PART_INDICATOR);
            lv_label_set_text(lbl_info, "Waiting...");
            lv_obj_set_style_text_color(lbl_info, COL_DIM, 0);
            lv_label_set_text(lbl_damage, "");
            lv_obj_set_style_bg_color(scr, COL_BG, 0);
            break;
        }

        // ==== PLANTING ====
        case STATE_PLANTING: {
            lv_label_set_text(lbl_face, "o_O");
            lv_obj_set_style_text_color(lbl_face, COL_ORANGE, 0);

            lv_label_set_text(lbl_status, "BOMB BEING PLANTED!");
            lv_obj_set_style_text_color(lbl_status, COL_ORANGE, 0);
            lv_label_set_text(lbl_timer, ". . .");
            lv_obj_set_style_text_color(lbl_timer, COL_ORANGE, 0);
            lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar_progress, COL_ORANGE, LV_PART_INDICATOR);
            lv_label_set_text(lbl_info, "");
            lv_label_set_text(lbl_damage, "");
            lv_obj_set_style_bg_color(scr, COL_BG, 0);
            break;
        }

        // ==== TICKING ====
        case STATE_TICKING: {
            lv_color_t zone = get_zone_color(ratio);

            // one consistent face during ticking — alert but neutral
            lv_label_set_text(lbl_face, "o_o");
            lv_obj_set_style_text_color(lbl_face, zone, 0);

            lv_label_set_text(lbl_status, "BOMB PLANTED");
            lv_obj_set_style_text_color(lbl_status, zone, 0);

            set_timer_text(bomb.time_left);
            lv_obj_set_style_text_color(lbl_timer, (ratio < 0.25f) ? COL_RED : COL_WHITE, 0);

            lv_bar_set_value(bar_progress, progress, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar_progress, zone, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(scr, COL_BG, 0);

            lv_label_set_text(lbl_info, "");

            if (bomb.hp_after >= 0) {
                if (bomb.hp_after > 0)
                    snprintf(hp_buf, sizeof(hp_buf), "HP: %d - SURVIVE", bomb.hp_after);
                else
                    snprintf(hp_buf, sizeof(hp_buf), "HP: 0 - LETHAL");
                lv_label_set_text(lbl_damage, hp_buf);
                lv_obj_set_style_text_color(lbl_damage, get_hp_color(bomb.hp_after), 0);
            } else {
                lv_label_set_text(lbl_damage, "");
            }
            break;
        }

        // ==== DEFUSING (will make it) ====
        case STATE_DEFUSING: {
            lv_label_set_text(lbl_face, "^_^");
            lv_obj_set_style_text_color(lbl_face, COL_CYAN, 0);

            lv_label_set_text(lbl_status, "DEFUSING...");
            lv_obj_set_style_text_color(lbl_status, COL_CYAN, 0);

            if (bomb.defuse_tleft >= 0)
                set_timer_text(bomb.defuse_tleft);
            else
                set_timer_text(bomb.time_left);
            lv_obj_set_style_text_color(lbl_timer, COL_CYAN, 0);

            lv_bar_set_value(bar_progress, progress, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar_progress, COL_CYAN, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(scr, COL_BG, 0);

            if (bomb.has_kit) {
                lv_label_set_text(lbl_info, "KIT 5s - WILL DEFUSE");
            } else {
                lv_label_set_text(lbl_info, "NO KIT 10s - WILL DEFUSE");
            }
            lv_obj_set_style_text_color(lbl_info, COL_GREEN, 0);

            if (bomb.hp_after >= 0) {
                if (bomb.hp_after > 0)
                    snprintf(hp_buf, sizeof(hp_buf), "HP: %d - SURVIVE", bomb.hp_after);
                else
                    snprintf(hp_buf, sizeof(hp_buf), "HP: 0 - LETHAL");
                lv_label_set_text(lbl_damage, hp_buf);
                lv_obj_set_style_text_color(lbl_damage, get_hp_color(bomb.hp_after), 0);
            } else {
                lv_label_set_text(lbl_damage, "");
            }
            break;
        }

        // ==== DEFUSE LATE (won't make it) ====
        case STATE_DEFUSE_LATE: {
            lv_label_set_text(lbl_face, ">_<");
            lv_obj_set_style_text_color(lbl_face, COL_RED, 0);

            lv_label_set_text(lbl_status, "DEFUSING - NO TIME!");
            lv_obj_set_style_text_color(lbl_status, COL_RED, 0);

            set_timer_text(bomb.time_left);
            lv_obj_set_style_text_color(lbl_timer, COL_RED, 0);

            lv_bar_set_value(bar_progress, progress, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar_progress, COL_RED, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(scr, COL_BG, 0);

            if (bomb.has_kit)
                lv_label_set_text(lbl_info, "KIT 5s - WON'T MAKE IT");
            else
                lv_label_set_text(lbl_info, "NO KIT - WON'T MAKE IT");
            lv_obj_set_style_text_color(lbl_info, COL_RED, 0);

            if (bomb.hp_after >= 0) {
                if (bomb.hp_after > 0)
                    snprintf(hp_buf, sizeof(hp_buf), "HP: %d - SURVIVE", bomb.hp_after);
                else
                    snprintf(hp_buf, sizeof(hp_buf), "HP: 0 - LETHAL");
                lv_label_set_text(lbl_damage, hp_buf);
                lv_obj_set_style_text_color(lbl_damage, get_hp_color(bomb.hp_after), 0);
            } else {
                lv_label_set_text(lbl_damage, "");
            }
            break;
        }

        // ==== RUN ====
        case STATE_RUN: {
            lv_label_set_text(lbl_face, "O_O");
            lv_obj_set_style_text_color(lbl_face, COL_RED, 0);

            lv_label_set_text(lbl_status, "!! RUN !!");
            lv_obj_set_style_text_color(lbl_status, COL_RED, 0);

            set_timer_text(bomb.time_left);
            lv_obj_set_style_text_color(lbl_timer, COL_RED, 0);

            lv_bar_set_value(bar_progress, progress, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar_progress, COL_RED, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(scr, COL_BG, 0);

            lv_label_set_text(lbl_info, "GET OUT NOW");
            lv_obj_set_style_text_color(lbl_info, COL_RED, 0);

            if (bomb.hp_after >= 0) {
                if (bomb.hp_after > 0)
                    snprintf(hp_buf, sizeof(hp_buf), "HP: %d - SURVIVE", bomb.hp_after);
                else
                    snprintf(hp_buf, sizeof(hp_buf), "HP: 0 - LETHAL");
                lv_label_set_text(lbl_damage, hp_buf);
                lv_obj_set_style_text_color(lbl_damage, get_hp_color(bomb.hp_after), 0);
            } else {
                lv_label_set_text(lbl_damage, "");
            }
            break;
        }

        // ==== DEFUSED ====
        case STATE_DEFUSED:
            lv_label_set_text(lbl_face, "^o^");
            lv_obj_set_style_text_color(lbl_face, COL_GREEN, 0);

            lv_label_set_text(lbl_status, "DEFUSED");
            lv_obj_set_style_text_color(lbl_status, COL_GREEN, 0);
            lv_label_set_text(lbl_timer, "00:00.0");
            lv_obj_set_style_text_color(lbl_timer, COL_GREEN, 0);
            lv_bar_set_value(bar_progress, 1000, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar_progress, COL_GREEN, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(scr, COL_BG, 0);
            lv_label_set_text(lbl_info, "CT WIN");
            lv_obj_set_style_text_color(lbl_info, COL_GREEN, 0);
            lv_label_set_text(lbl_damage, "");
            break;

        // ==== EXPLODED ====
        case STATE_EXPLODED:
            lv_label_set_text(lbl_face, "X_X");
            lv_obj_set_style_text_color(lbl_face, COL_RED, 0);

            lv_label_set_text(lbl_status, "BOOM");
            lv_obj_set_style_text_color(lbl_status, COL_RED, 0);
            lv_label_set_text(lbl_timer, "00:00.0");
            lv_obj_set_style_text_color(lbl_timer, COL_ORANGE, 0);
            lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar_progress, COL_RED, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(scr, COL_BG, 0);
            lv_label_set_text(lbl_info, "T WIN");
            lv_obj_set_style_text_color(lbl_info, COL_RED, 0);
            lv_label_set_text(lbl_damage, "");
            break;
    }
}
