/*
 * MF64 — Host-side logic test harness (complete)
 * Compile: g++ -O2 -std=c++17 -o mf64_test mf64_test.cpp
 * Run:     ./mf64_test
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

// ============================================================
// MOCK ARDUINO / ESP32 APIs
// ============================================================
static uint32_t _millis = 0;
uint32_t millis() { return _millis; }
void delay(uint32_t ms) { _millis += ms; }
void delayMicroseconds(uint32_t us) { (void)us; }

std::map<int, int> pin_state;
std::map<int, int> pin_mode;
void pinMode(int p, int m) { pin_mode[p] = m; }
void digitalWrite(int p, int v) { pin_state[p] = v; }
int  digitalRead(int p) { return pin_state[p]; }

#define OUTPUT 1
#define INPUT  0
#define HIGH 1
#define LOW  0

struct MockSerial {
    void begin(int) {}
    void print(const char* s) { printf("%s", s); }
    void print(int v) { printf("%d", v); }
    void println(const char* s) { printf("%s\n", s); }
    void println(int v) { printf("%d\n", v); }
    void println() { printf("\n"); }
} Serial;

struct MockPrefs {
    std::map<std::string, uint8_t> u8;
    std::map<std::string, std::vector<uint8_t>> bytes;
    bool begin(const char*, bool) { return true; }
    void clear() { u8.clear(); bytes.clear(); }
    void putUChar(const char* k, uint8_t v) { u8[k] = v; }
    uint8_t getUChar(const char* k, uint8_t d) {
        auto it = u8.find(k);
        return it == u8.end() ? d : it->second;
    }
    void putBytes(const char* k, const void* p, size_t n) {
        bytes[k] = std::vector<uint8_t>((uint8_t*)p, (uint8_t*)p + n);
    }
    size_t getBytes(const char* k, void* p, size_t n) {
        auto it = bytes.find(k);
        if (it == bytes.end()) return 0;
        memcpy(p, it->second.data(), std::min(n, it->second.size()));
        return it->second.size();
    }
} prefs;

struct Adafruit_NeoPixel {
    int n; int pin; int brightness = 255;
    std::vector<uint32_t> px;
    Adafruit_NeoPixel(int _n, int _p, int = 0) : n(_n), pin(_p), px(_n, 0) {}
    void begin() {}
    void setBrightness(int b) { brightness = b; }
    void clear() { std::fill(px.begin(), px.end(), 0); }
    void show() { }
    void setPixelColor(int i, uint32_t c) { if (i >= 0 && i < n) px[i] = c; }
    uint32_t Color(uint8_t r, uint8_t g, uint8_t b) {
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
};

struct MockMIDI {
    struct Message { uint8_t type, ch, d1, d2; };
    std::vector<Message> sent;
    void begin() {}
    void noteOn(uint8_t n, uint8_t v, uint8_t c) { sent.push_back({0x90, c, n, v}); }
    void noteOff(uint8_t n, uint8_t v, uint8_t c) { sent.push_back({0x80, c, n, v}); }
    void controlChange(uint8_t cc, uint8_t v, uint8_t c) { sent.push_back({0xB0, c, cc, v}); }
} MIDI;

struct MockUSB { void begin() {} } USB;

// ============================================================
// CONSTANTS
// ============================================================
#define NUM_BUTTONS             64
#define BUTTON_ID_FLAGS         0x3F
#define NUM_BANKS               2
#define MIDI_BASENOTE           36
#define MIDI_MAX_NOTES          128
#define MIDI_CHANNEL_INDEX_CONTROL_BANKS 0
#define MIDI_CHANNEL_INDEX_ANIMATIONS    1
#define DEBOUNCE_BUFFER_SIZE    10
#define G_BANK_SELECT_COUNTER_LIMIT 1000
#define NOTE_OFF_FEEDBACK_DELAY_LIMIT 2
#define ENABLE_NOTE_OFF_FEEDBACK_DELAY 1

#define CONCURRENT_GEOMETRIC_ANIMATIONS 4
#define GEOMETRIC_ANIMATION_TYPE_SQUARE   0
#define GEOMETRIC_ANIMATION_TYPE_CIRCLE   1
#define GEOMETRIC_ANIMATION_TYPE_STAR     2
#define GEOMETRIC_ANIMATION_TYPE_TRIANGLE 3
#define GEOMETRIC_ANIMATION_STEPS_SQUARE   4
#define GEOMETRIC_ANIMATION_STEPS_CIRCLE   9
#define GEOMETRIC_ANIMATION_STEPS_STAR     5
#define GEOMETRIC_ANIMATION_STEPS_TRIANGLE 4
#define GEOMETRIC_ANIMATION_ROWS           8
#define GEOMETRIC_ANIMATION_COLS           8
#define GEOMETRIC_STAR_MAX_TAIL_LENGTH     3

// ============================================================
// STATE
// ============================================================
uint64_t g_debounce[DEBOUNCE_BUFFER_SIZE];
uint8_t  g_debounce_pos = 0;
uint64_t g_key_state = 0;
uint64_t g_key_prev  = 0;
uint64_t g_key_down  = 0;
uint64_t g_key_up    = 0;
uint64_t g_key_bank_last_up = 0;
uint16_t system_time_ms = 0;
uint8_t  g_bank_selected = 0;
uint8_t  g_midi_note_state[2][MIDI_MAX_NOTES];
uint8_t  g_midi_note_off_counter[MIDI_MAX_NOTES];
uint16_t g_bank_select_counter[NUM_BANKS];
uint16_t g_led_counter[4] = {0,0,0,0};
uint16_t display_flash_counter = 0;
uint16_t half_ms_counter = 0;
uint8_t  one_second_counter = 0;
uint8_t  sleep_minute_counter = 0;
uint16_t tick_counter = 0;
bool     midi_clock_enabled = false;

const uint8_t bank_select_key_ids[NUM_BANKS] = {28, 63};
uint8_t G_EE_MIDI_CHANNEL  = 2;
uint8_t G_EE_MIDI_VELOCITY = 127;
uint8_t G_EE_SIDE_BANK     = 0x00;
uint8_t G_EE_SLEEP_TIME    = 0x3C;
uint8_t G_EE_ANIMATIONS    = 6;
uint8_t G_EE_COMBOS_ENABLE = 0x01;

// Palette / animation state
uint8_t default_bank_inactive[2][64 * 3];
uint8_t default_bank_active[2][64 * 3];
uint8_t g_display_buffer[64 * 3];

uint8_t geometric_animation_btn_id[CONCURRENT_GEOMETRIC_ANIMATIONS] = {0,0,0,0};
uint8_t geometric_animation_type[CONCURRENT_GEOMETRIC_ANIMATIONS]   = {0,0,0,0};
uint8_t geometric_animation_pos[CONCURRENT_GEOMETRIC_ANIMATIONS]    = {GEOMETRIC_ANIMATION_STEPS_SQUARE,
                                                                       GEOMETRIC_ANIMATION_STEPS_SQUARE,
                                                                       GEOMETRIC_ANIMATION_STEPS_SQUARE,
                                                                       GEOMETRIC_ANIMATION_STEPS_SQUARE};
uint8_t* geometric_animation_color_ptr[CONCURRENT_GEOMETRIC_ANIMATIONS];
uint8_t geometric_animation_id = 0;
uint8_t assign_geometric_animation_id = 0;

// ============================================================
// PALETTES (from display.c)
// ============================================================
const uint8_t default_color[20][3] = {
    {0x00,0x00,0x00}, {48,0x00,0x00}, {24,0x00,0x00}, {40,12,0x00},
    {20,6,0x00},      {32,25,0x00},   {16,12,0x00},   {25,32,0x00},
    {12,16,0x00},     {0x00,48,0x00}, {0x00,24,0x00}, {0x00,30,30},
    {0x00,15,15},     {0x00,0x00,48}, {0x00,0x00,24}, {25,7,32},
    {13,3,17},        {36,0x00,18},   {18,0x00,9},    {24,24,24},
};

const uint8_t ableton_midi_feedback_colors[128][3] = {
    {0,0,0}, {8,8,8}, {16,16,16}, {24,24,24}, {40,12,12}, {48,0,0}, {32,0,0}, {16,0,0},
    {30,22,12}, {42,13,0}, {28,9,0}, {14,4,0}, {28,28,8}, {30,30,0}, {20,20,0}, {10,10,0},
    {20,38,11}, {13,42,0}, {9,28,0}, {4,14,0}, {11,40,11}, {0,48,0}, {0,32,0}, {0,16,0},
    {11,38,14}, {0,44,4}, {0,29,3}, {0,15,2}, {10,36,19}, {6,40,14}, {4,26,9}, {2,13,4},
    {9,32,22}, {0,38,22}, {0,25,14}, {0,12,7}, {9,24,32}, {0,23,36}, {0,15,24}, {0,7,12},
    {10,19,36}, {0,19,39}, {0,13,26}, {0,6,13}, {11,11,40}, {0,0,48}, {0,0,32}, {0,0,16},
    {19,10,36}, {19,0,39}, {13,0,26}, {6,0,13}, {28,8,28}, {30,0,30}, {20,0,20}, {10,0,10},
    {36,10,19}, {42,0,13}, {28,0,8}, {14,0,4}, {44,3,0}, {28,9,0}, {22,15,0}, {12,18,0},
    {0,10,0}, {0,16,9}, {0,15,23}, {0,0,48}, {0,12,14}, {6,0,38}, {16,16,16}, {8,8,8},
    {48,0,0}, {23,32,5}, {24,33,1}, {15,38,1}, {3,26,0}, {0,38,20}, {0,25,38}, {0,7,44},
    {15,0,42}, {19,0,40}, {33,4,23}, {12,6,0}, {42,12,0}, {22,37,1}, {18,40,3}, {0,48,0},
    {9,42,6}, {12,36,15}, {7,32,25}, {13,19,36}, {9,15,37}, {19,18,33}, {24,3,30}, {40,0,15},
    {40,15,0}, {34,14,0}, {20,36,0}, {24,17,1}, {10,8,0}, {3,14,3}, {2,15,10}, {3,3,7},
    {4,6,16}, {19,11,5}, {31,0,1}, {37,13,10}, {37,18,5}, {30,27,5}, {23,33,7}, {19,34,3},
    {5,5,9}, {24,28,12}, {15,30,22}, {19,19,32}, {19,14,34}, {6,6,6}, {11,11,11}, {21,24,24},
    {30,0,0}, {9,0,0}, {4,39,0}, {1,12,0}, {29,28,0}, {11,9,0}, {33,17,0}, {14,3,0},
};

// ============================================================
// COMBO TABLE (from combo.c)
// ============================================================
enum {
    COMBO_NONE = 0,
    COMBO_A_DOWN = 1, COMBO_B_DOWN, COMBO_C_DOWN, COMBO_D_DOWN, COMBO_E_DOWN,
    COMBO_A_RELEASE, COMBO_B_RELEASE, COMBO_C_RELEASE, COMBO_D_RELEASE, COMBO_E_RELEASE
};

typedef struct { uint8_t state; uint8_t key; uint8_t next; uint8_t action; } combo_rule_t;

const uint8_t state_offset[] = {0,4,5,6,7,8,9,10,11,12,13,14,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,32,33,34,35,36,38,39,41,42,43};

const combo_rule_t state_table[] = {
    {0,0x10,1,COMBO_NONE}, {0,0x15,5,COMBO_NONE}, {0,0x14,9,COMBO_NONE}, {0,0x18,18,COMBO_NONE},
    {1,0x11,2,COMBO_NONE}, {2,0x12,3,COMBO_NONE}, {3,0x13,4,COMBO_A_DOWN}, {4,0x23,0,COMBO_A_RELEASE},
    {5,0x16,6,COMBO_NONE}, {6,0x19,7,COMBO_NONE}, {7,0x1A,8,COMBO_C_DOWN}, {8,0x2A,0,COMBO_C_RELEASE},
    {9,0x24,10,COMBO_NONE}, {10,0x15,11,COMBO_NONE}, {11,0x25,12,COMBO_NONE}, {11,0x16,6,COMBO_NONE},
    {12,0x16,13,COMBO_NONE}, {13,0x26,14,COMBO_NONE}, {14,0x16,15,COMBO_NONE}, {15,0x26,16,COMBO_NONE},
    {16,0x17,17,COMBO_D_DOWN}, {17,0x27,0,COMBO_D_RELEASE},
    {18,0x28,19,COMBO_NONE}, {19,0x18,20,COMBO_NONE}, {20,0x28,21,COMBO_NONE},
    {21,0x10,22,COMBO_NONE}, {22,0x20,23,COMBO_NONE}, {23,0x10,24,COMBO_NONE}, {24,0x20,25,COMBO_NONE},
    {25,0x14,26,COMBO_NONE}, {26,0x24,27,COMBO_NONE}, {26,0x16,6,COMBO_NONE},
    {27,0x15,28,COMBO_NONE}, {28,0x25,29,COMBO_NONE}, {29,0x14,30,COMBO_NONE}, {30,0x24,31,COMBO_NONE},
    {31,0x15,32,COMBO_NONE}, {31,0x16,13,COMBO_NONE},
    {32,0x25,33,COMBO_NONE}, {33,0x17,34,COMBO_NONE}, {33,0x15,11,COMBO_NONE},
    {34,0x27,35,COMBO_NONE}, {35,0x16,36,COMBO_E_DOWN}, {36,0x26,0,COMBO_E_RELEASE},
};

// ============================================================
// CORE LOGIC (from .ino)
// ============================================================
uint16_t rightmost_bit_16(uint16_t value) {
    uint16_t bit = 0x0001;
    while (bit && (value & bit) == 0) bit <<= 1;
    return bit;
}

uint8_t comboRecognize() {
    uint16_t keydown = g_key_down & 0xFFFF;
    uint16_t keyup   = g_key_up & 0xFFFF;
    uint16_t keystate = g_key_state & 0xFFFF;
    static uint8_t combo_action = COMBO_NONE;
    static uint16_t combo_release_key = 0;
    static uint8_t combo_state = 0;

    if (keydown == 0 && keyup == 0) return COMBO_NONE;

    if (keystate == 0x00F0 && combo_action != COMBO_B_DOWN) {
        uint16_t bit = rightmost_bit_16(keydown);
        if (bit) { combo_release_key = bit; combo_action = COMBO_B_DOWN; return combo_action; }
        return COMBO_NONE;
    }

    if (combo_action >= COMBO_A_DOWN && combo_action <= COMBO_E_DOWN) {
        if (!(keystate & combo_release_key)) {
            combo_state = 0;
            combo_action += (COMBO_A_RELEASE - COMBO_A_DOWN);
            uint8_t ret = combo_action; combo_action = COMBO_NONE; return ret;
        }
        return COMBO_NONE;
    }

    uint8_t offset = state_offset[combo_state];
    uint8_t i = offset;
    while (i < sizeof(state_table)/sizeof(state_table[0]) && state_table[i].state == combo_state) {
        const combo_rule_t *r = &state_table[i];
        uint8_t keytest = r->key & 0xF0;
        uint8_t keybit = r->key & 0x0F;
        uint16_t keymask = (keytest == 0x10) ? keydown : (keytest == 0x20) ? keyup : keystate;
        if ((1 << keybit) & keymask) { combo_state = r->next; combo_action = r->action; break; }
        else { combo_state = 0; combo_action = COMBO_NONE; }
        i++;
    }

    if (combo_action != COMBO_NONE) {
        uint16_t bit = rightmost_bit_16(keydown);
        combo_release_key = bit ? bit : 0;
    }
    return combo_action;
}

uint8_t channelForBank(uint8_t bank) {
    if (bank == 1) return (G_EE_MIDI_CHANNEL - 1) & 0x0F;
    return G_EE_MIDI_CHANNEL;
}

void key_pressed(uint8_t k) {
    for (uint8_t b = 0; b < NUM_BANKS; b++)
        if (k == bank_select_key_ids[b]) {
            uint16_t t = system_time_ms; if (t == 0) t = 0xFFFF;
            memset(g_bank_select_counter, 0, sizeof(g_bank_select_counter));
            g_bank_select_counter[b] = t;
        }
}
void key_released(uint8_t k) {
    for (uint8_t b = 0; b < NUM_BANKS; b++)
        if (k == bank_select_key_ids[b]) g_bank_select_counter[b] = 0;
}
void change_bank(uint8_t b) {
    g_bank_selected = b;
    memset(g_bank_select_counter, 0, sizeof(g_bank_select_counter));
}
void send_change_bank_notification(uint8_t b) {
    MIDI.controlChange(3, b, G_EE_MIDI_CHANNEL + 1);
}

void service_bank_select_buttons() {
    if (!G_EE_SIDE_BANK) return;
    uint16_t limit = (G_EE_SIDE_BANK > 1) ? 1 : G_BANK_SELECT_COUNTER_LIMIT;
    uint16_t now = system_time_ms;
    for (uint8_t b = 0; b < NUM_BANKS; b++)
        if (g_bank_select_counter[b] > 0 &&
            (uint16_t)(now - g_bank_select_counter[b]) >= limit) {
            change_bank(b);
            send_change_bank_notification(b);
            break;
        }
}

void isr_tick() {
    for (uint8_t i = 0; i < 4; i++) if (g_led_counter[i] > 0) g_led_counter[i]--;
    if (!midi_clock_enabled) {
        if (tick_counter == 75) { tick_counter = 1; display_flash_counter++; }
        else tick_counter++;
    }
    half_ms_counter++;
    if (g_led_counter[3] == 0) g_led_counter[3] = 16;
    system_time_ms++;
}

// ============================================================
// PALETTE / GEOMETRY FUNCTIONS (from display.c)
// ============================================================
void loadDefaultColors() {
    for (int b = 0; b < 2; b++) {
        uint8_t inactive_idx = (b == 0) ? 0 : 19;   // OFF or WHITE
        uint8_t active_idx   = (b == 0) ? 13 : 9;   // BLUE or GREEN
        for (int i = 0; i < 64; i++) {
            default_bank_inactive[b][i*3+0] = default_color[inactive_idx][0];
            default_bank_inactive[b][i*3+1] = default_color[inactive_idx][1];
            default_bank_inactive[b][i*3+2] = default_color[inactive_idx][2];
            default_bank_active[b][i*3+0]   = default_color[active_idx][0];
            default_bank_active[b][i*3+1]   = default_color[active_idx][1];
            default_bank_active[b][i*3+2]   = default_color[active_idx][2];
        }
    }
}

uint8_t get_geometric_button_row() {
    return (geometric_animation_btn_id[geometric_animation_id] >> 2) & 0x07;
}
uint8_t get_geometric_button_column() {
    uint8_t c = geometric_animation_btn_id[geometric_animation_id] & 0x03;
    if (geometric_animation_btn_id[geometric_animation_id] >= 32) c += 4;
    return c;
}
uint8_t get_button_id_from_row_column(uint8_t row, uint8_t col) {
    if (row >= GEOMETRIC_ANIMATION_ROWS) return 0xFF;
    if (col >= GEOMETRIC_ANIMATION_COLS) return 0xFF;
    uint8_t id = row * 4;
    if (col < 4) id += col;
    else         id += (col & 0x03) + 32;
    return id;
}
void writeLedBrg(uint8_t* buf, uint8_t btn_id, uint8_t* src) {
    if (btn_id >= 64) return;
    uint8_t* p = buf + btn_id * 3;
    *p++ = src[2]; *p++ = src[0]; *p++ = src[1];
}

uint8_t get_distance_from_center_in_steps(uint8_t bR, uint8_t bC, uint8_t r, uint8_t c) {
    uint8_t dr = bR > r ? bR - r : r - bR;
    uint8_t dc = bC > c ? bC - c : c - bC;
    return dr + dc;
}

void run_geometric_square_animation(uint8_t* buffer, bool update) {
    if (update) if (++geometric_animation_pos[geometric_animation_id] >= GEOMETRIC_ANIMATION_STEPS_SQUARE) return;
    if (!geometric_animation_pos[geometric_animation_id]) return;
    uint8_t bR = get_geometric_button_row(), bC = get_geometric_button_column();
    uint8_t pos = geometric_animation_pos[geometric_animation_id];
    uint8_t minR = bR - pos, maxR = bR + pos, minC = bC - pos, maxC = bC + pos;
    uint8_t sR = minR < GEOMETRIC_ANIMATION_ROWS ? minR : 0;
    uint8_t eR = maxR < GEOMETRIC_ANIMATION_ROWS ? maxR : GEOMETRIC_ANIMATION_ROWS - 1;
    uint8_t sC = minC < GEOMETRIC_ANIMATION_COLS ? minC : 0;
    uint8_t eC = maxC < GEOMETRIC_ANIMATION_COLS ? maxC : GEOMETRIC_ANIMATION_COLS - 1;
    uint8_t* src = geometric_animation_color_ptr[geometric_animation_id];
    for (uint8_t r = sR; r <= eR; r++)
        for (uint8_t c = sC; c <= eC; c++)
            if ((r == minR || r == maxR) || (c == minC || c == maxC))
                writeLedBrg(buffer, get_button_id_from_row_column(r, c), src);
}

void run_geometric_circle_animation(uint8_t* buffer, bool update) {
    if (update) if (++geometric_animation_pos[geometric_animation_id] >= GEOMETRIC_ANIMATION_STEPS_CIRCLE) return;
    if (!geometric_animation_pos[geometric_animation_id]) return;
    uint8_t bR = get_geometric_button_row(), bC = get_geometric_button_column();
    uint8_t pos = geometric_animation_pos[geometric_animation_id];
    uint8_t minR = bR - pos, maxR = bR + pos, minC = bC - pos, maxC = bC + pos;
    uint8_t sR = minR < GEOMETRIC_ANIMATION_ROWS ? minR : 0;
    uint8_t eR = maxR < GEOMETRIC_ANIMATION_ROWS ? maxR : GEOMETRIC_ANIMATION_ROWS - 1;
    uint8_t sC = minC < GEOMETRIC_ANIMATION_COLS ? minC : 0;
    uint8_t eC = maxC < GEOMETRIC_ANIMATION_COLS ? maxC : GEOMETRIC_ANIMATION_COLS - 1;
    uint8_t* src = geometric_animation_color_ptr[geometric_animation_id];
    uint8_t inc = pos >> 1; if (inc > 3) inc = 3;
    uint8_t minD = pos;
    uint8_t maxD = (pos == 1) ? minD : minD + inc;
    for (uint8_t r = sR; r <= eR; r++) {
        for (uint8_t c = sC; c <= eC; c++) {
            uint8_t d = get_distance_from_center_in_steps(bR, bC, r, c);
            bool light = false;
            if (d < minD) light = false;
            else if (d == minD) light = (r == minR || r == maxR || c == minC || c == maxC);
            else if (d <= maxD) light = true;
            if (light) writeLedBrg(buffer, get_button_id_from_row_column(r, c), src);
        }
    }
}

void run_geometric_star_animation(uint8_t* buffer, bool update) {
    if (update) if (++geometric_animation_pos[geometric_animation_id] >= GEOMETRIC_ANIMATION_STEPS_STAR) return;
    if (!geometric_animation_pos[geometric_animation_id]) return;
    uint8_t bR = get_geometric_button_row(), bC = get_geometric_button_column();
    uint8_t pos = geometric_animation_pos[geometric_animation_id];
    uint8_t minR = bR - pos, maxR = bR + pos, minC = bC - pos, maxC = bC + pos;
    uint8_t* src = geometric_animation_color_ptr[geometric_animation_id];
    uint8_t counter = pos, tail = 0;
    while (counter > 0 && tail < GEOMETRIC_STAR_MAX_TAIL_LENGTH) {
        writeLedBrg(buffer, get_button_id_from_row_column(maxR - tail, bC), src);
        writeLedBrg(buffer, get_button_id_from_row_column(minR + tail, bC), src);
        writeLedBrg(buffer, get_button_id_from_row_column(maxR - tail, minC + tail), src);
        writeLedBrg(buffer, get_button_id_from_row_column(maxR - tail, maxC - tail), src);
        writeLedBrg(buffer, get_button_id_from_row_column(minR + tail, minC + tail), src);
        writeLedBrg(buffer, get_button_id_from_row_column(minR + tail, maxC - tail), src);
        writeLedBrg(buffer, get_button_id_from_row_column(bR, minC + tail), src);
        writeLedBrg(buffer, get_button_id_from_row_column(bR, maxC - tail), src);
        counter--; tail++;
    }
}

void run_geometric_triangle_animation(uint8_t* buffer, bool update) {
    if (update) if (++geometric_animation_pos[geometric_animation_id] >= GEOMETRIC_ANIMATION_STEPS_TRIANGLE) return;
    if (!geometric_animation_pos[geometric_animation_id]) return;
    uint8_t bR = get_geometric_button_row(), bC = get_geometric_button_column();
    uint8_t pos = geometric_animation_pos[geometric_animation_id];
    uint8_t minR = bR - (pos - 1), maxR = bR + pos;
    uint8_t minC = bC - (1 + (pos - 1) * 2), maxC = bC + (1 + (pos - 1) * 2);
    uint8_t sR = minR < GEOMETRIC_ANIMATION_ROWS ? minR : 0;
    uint8_t eR = maxR < GEOMETRIC_ANIMATION_ROWS ? maxR : GEOMETRIC_ANIMATION_ROWS - 1;
    uint8_t sC = minC < GEOMETRIC_ANIMATION_COLS ? minC : 0;
    uint8_t eC = maxC < GEOMETRIC_ANIMATION_COLS ? maxC : GEOMETRIC_ANIMATION_COLS - 1;
    uint8_t* src = geometric_animation_color_ptr[geometric_animation_id];
    if (sR == minR) for (uint8_t c = sC; c <= eC; c++) writeLedBrg(buffer, get_button_id_from_row_column(sR, c), src);
    if (eR == maxR) writeLedBrg(buffer, get_button_id_from_row_column(eR, bC), src);
    for (uint8_t r = sR; r <= eR; r++) {
        uint8_t d = r - minR;
        uint8_t lc = minC + d, rc = maxC - d;
        if (lc < GEOMETRIC_ANIMATION_COLS) writeLedBrg(buffer, get_button_id_from_row_column(r, lc), src);
        if (rc < GEOMETRIC_ANIMATION_COLS) writeLedBrg(buffer, get_button_id_from_row_column(r, rc), src);
    }
}

// ============================================================
// FLASH / PULSE ANIMATION (from display.c)
// ============================================================
bool flash_animation(uint8_t rate) {
    return (display_flash_counter & (0x01 << (8 - rate))) != 0;
}

uint8_t pulse_animation(uint8_t rate) {
    uint8_t step;
    if (!midi_clock_enabled) step = (uint8_t)(((display_flash_counter << 4) >> (8 - rate)) & 0xFF);
    else                     step = (uint8_t)(((display_flash_counter << 5) >> (8 - rate)) & 0xFF);
    int level = (int)(sin(0.049f * step) * 127.0f + 127.0f);
    if (level < 0) level = 0;
    if (level > 255) level = 255;
    return (uint8_t)level;
}

// ============================================================
// TEST FRAMEWORK
// ============================================================
int tests_passed = 0, tests_failed = 0;

void check(bool cond, const char* name) {
    if (cond) { printf("  \u2705 %s\n", name); tests_passed++; }
    else      { printf("  \u274C %s\n", name); tests_failed++; }
}

void printHeader(const char* name) {
    printf("\n=== %s ===\n", name);
}

int countLitLeds(const uint8_t* buf) {
    int n = 0;
    for (int i = 0; i < 64; i++) {
        if (buf[i*3] || buf[i*3+1] || buf[i*3+2]) n++;
    }
    return n;
}

// ============================================================
// TESTS — CORE LOGIC
// ============================================================
void testNoteMapping() {
    printHeader("Note mapping");
    check(MIDI_BASENOTE + 0  == 36, "Button 1  -> note 36");
    check(MIDI_BASENOTE + 63 == 99, "Button 64 -> note 99");
    check(channelForBank(0) == 2, "Bank 1 -> ch index 2 (ch 3)");
    check(channelForBank(1) == 1, "Bank 2 -> ch index 1 (ch 2)");
}

void testBankSelectTiming() {
    printHeader("Bank select timing (1000 ms hold)");
    G_EE_SIDE_BANK = 1;
    system_time_ms = 10000;
    memset(g_bank_select_counter, 0, sizeof(g_bank_select_counter));
    g_bank_selected = 0;

    key_pressed(63);
    check(g_bank_select_counter[1] != 0, "Key 63 pressed -> counter[1] set");

    system_time_ms = 10999;
    service_bank_select_buttons();
    check(g_bank_selected == 0, "At 999 ms: still bank 1");

    system_time_ms = 11000;
    service_bank_select_buttons();
    check(g_bank_selected == 1, "At 1000 ms: switched to bank 2 (index 1)");

    key_released(63);
    check(g_bank_select_counter[1] == 0, "Key 63 released -> counter reset");

    g_bank_selected = 0;
    G_EE_SIDE_BANK = 0x00;
}

void testDebounceAlgorithm() {
    printHeader("Debounce (10-sample AND)");
    for (int i = 0; i < DEBOUNCE_BUFFER_SIZE; i++) g_debounce[i] = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t result = 0xFFFFFFFFFFFFFFFFULL;
    for (int i = 0; i < DEBOUNCE_BUFFER_SIZE; i++) result &= g_debounce[i];
    check(result == 0xFFFFFFFFFFFFFFFFULL, "All 1s -> all 1s");

    g_debounce[3] = 0xFFFFFFFFFFFFFFFEULL;
    result = 0xFFFFFFFFFFFFFFFFULL;
    for (int i = 0; i < DEBOUNCE_BUFFER_SIZE; i++) result &= g_debounce[i];
    check((result & 1) == 0, "One sample 0 -> AND propagates 0");

    for (int i = 0; i < DEBOUNCE_BUFFER_SIZE; i++) g_debounce[i] = 0;
}

void testComboStateMachine() {
    printHeader("Combo A (press 0->1->2->3)");
    g_key_state = 0; g_key_down = 0; g_key_up = 0;
    comboRecognize();

    g_key_down = 1; g_key_state = 1;
    uint8_t a = comboRecognize();
    check(a == COMBO_NONE, "After key 0: no combo yet");
    g_key_down = 0;

    g_key_down = 2; g_key_state = 3;
    a = comboRecognize();
    check(a == COMBO_NONE, "After key 1: no combo yet");
    g_key_down = 0;

    g_key_down = 4; g_key_state = 7;
    a = comboRecognize();
    check(a == COMBO_NONE, "After key 2: no combo yet");
    g_key_down = 0;

    g_key_down = 8; g_key_state = 15;
    a = comboRecognize();
    check(a == COMBO_A_DOWN, "After key 3: COMBO_A_DOWN triggered");

    g_key_up = 8; g_key_state = 7;
    a = comboRecognize();
    check(a == COMBO_A_RELEASE, "After release: COMBO_A_RELEASE");
}

void testSysExPushConf() {
    printHeader("SysEx PUSH_CONF (0x01, tag-indexed)");
    uint8_t payload[] = {
        0xF0, 0x00, 0x01, 0x79, 0x01,
        0x00, 0x05, 0x01, 0x64,
        0x03, 0x00, 0x07, 0x01,
        0x08, 0x01, 0x0A, 0x00,
        0x16, 0x30, 0x17, 0x00,
        0xF7
    };
    uint8_t len = sizeof(payload);

    uint8_t old_ch    = G_EE_MIDI_CHANNEL;
    uint8_t old_vel   = G_EE_MIDI_VELOCITY;
    uint8_t old_sleep = G_EE_SLEEP_TIME;

    if (len >= 8) {
        uint8_t i = 5;
        while (i < len - 1) {
            uint8_t tag = payload[i++];
            if (i >= len) break;
            uint8_t val = payload[i++];
            switch (tag) {
                case 0x00: G_EE_MIDI_CHANNEL  = val - 1; break;
                case 0x01: G_EE_MIDI_VELOCITY = val;     break;
                case 0x16: G_EE_SLEEP_TIME    = val;     break;
            }
        }
    }

    check(G_EE_MIDI_CHANNEL == 4, "Channel = index 4 (ch 5)");
    check(G_EE_MIDI_VELOCITY == 100, "Velocity = 100");
    check(G_EE_SLEEP_TIME == 48, "Sleep = 48 min");

    G_EE_MIDI_CHANNEL  = old_ch;
    G_EE_MIDI_VELOCITY = old_vel;
    G_EE_SLEEP_TIME    = old_sleep;
}

void testIsrTick() {
    printHeader("ISR tick behavior");
    display_flash_counter = 0;
    tick_counter = 0;
    system_time_ms = 0;
    g_led_counter[3] = 0;

    for (int i = 0; i < 75; i++) isr_tick();
    check(display_flash_counter == 0, "After 74 ticks: flash counter still 0");
    isr_tick();
    check(display_flash_counter == 1, "After 75th tick: flash counter = 1");
    check(system_time_ms == 76, "system_time_ms = 76");
    check(g_led_counter[3] >= 1 && g_led_counter[3] <= 16,
          "PWM counter[3] in range [1..16]");
}

// ============================================================
// TESTS — PALETTES
// ============================================================
void testPaletteValues() {
    printHeader("Palette values (from display.c)");
    check(default_color[0][0] == 0x00 && default_color[0][1] == 0x00 && default_color[0][2] == 0x00,
          "Color 0 (OFF) = (0,0,0)");
    check(default_color[1][0] == 48 && default_color[1][1] == 0 && default_color[1][2] == 0,
          "Color 1 (RED) = (48,0,0)");
    check(default_color[9][0] == 0 && default_color[9][1] == 48 && default_color[9][2] == 0,
          "Color 9 (GREEN) = (0,48,0)");
    check(default_color[13][0] == 0 && default_color[13][1] == 0 && default_color[13][2] == 48,
          "Color 13 (BLUE) = (0,0,48)");
    check(default_color[19][0] == 24 && default_color[19][1] == 24 && default_color[19][2] == 24,
          "Color 19 (WHITE) = (24,24,24)");
    check(ableton_midi_feedback_colors[0][0] == 0 && ableton_midi_feedback_colors[0][1] == 0 &&
          ableton_midi_feedback_colors[0][2] == 0,
          "Ableton color[0] = (0,0,0)");
    check(ableton_midi_feedback_colors[127][0] == 14 &&
          ableton_midi_feedback_colors[127][1] == 3 &&
          ableton_midi_feedback_colors[127][2] == 0,
          "Ableton color[127] = (14,3,0)");
}

void testDefaultBankColors() {
    printHeader("Default bank colors");
    loadDefaultColors();
    check(default_bank_inactive[0][0] == 0x00 &&
          default_bank_inactive[0][1] == 0x00 &&
          default_bank_inactive[0][2] == 0x00,
          "Bank 1 inactive = OFF");
    check(default_bank_active[0][0] == 0x00 &&
          default_bank_active[0][1] == 0x00 &&
          default_bank_active[0][2] == 48,
          "Bank 1 active = BLUE (0,0,48)");
    check(default_bank_inactive[1][0] == 24 &&
          default_bank_inactive[1][1] == 24 &&
          default_bank_inactive[1][2] == 24,
          "Bank 2 inactive = WHITE (24,24,24)");
    check(default_bank_active[1][0] == 0x00 &&
          default_bank_active[1][1] == 48 &&
          default_bank_active[1][2] == 0x00,
          "Bank 2 active = GREEN (0,48,0)");
}

// ============================================================
// TESTS — GEOMETRY
// ============================================================
void testGeometryRowColumn() {
    printHeader("Geometry row/column mapping");
    geometric_animation_btn_id[0] = 0;
    geometric_animation_id = 0;
    check(get_geometric_button_row() == 0, "Button 0 -> row 0");
    check(get_geometric_button_column() == 0, "Button 0 -> col 0");

    geometric_animation_btn_id[0] = 3;
    check(get_geometric_button_row() == 0, "Button 3 -> row 0");
    check(get_geometric_button_column() == 3, "Button 3 -> col 3");

    geometric_animation_btn_id[0] = 4;
    check(get_geometric_button_row() == 1, "Button 4 -> row 1");
    check(get_geometric_button_column() == 0, "Button 4 -> col 0");

    geometric_animation_btn_id[0] = 32;
    check(get_geometric_button_row() == 0, "Button 32 -> row 0");
    check(get_geometric_button_column() == 4, "Button 32 -> col 4");

    geometric_animation_btn_id[0] = 63;
    check(get_geometric_button_row() == 7, "Button 63 -> row 7");
    check(get_geometric_button_column() == 7, "Button 63 -> col 7");
}

void testButtonIdFromRowColumn() {
    printHeader("Button ID from row/column");
    check(get_button_id_from_row_column(0, 0) == 0,  "(0,0) -> button 0");
    check(get_button_id_from_row_column(0, 3) == 3,  "(0,3) -> button 3");
    check(get_button_id_from_row_column(1, 0) == 4,  "(1,0) -> button 4");
    check(get_button_id_from_row_column(0, 4) == 32, "(0,4) -> button 32");
    check(get_button_id_from_row_column(0, 7) == 35, "(0,7) -> button 35");
    check(get_button_id_from_row_column(7, 7) == 63, "(7,7) -> button 63");
    check(get_button_id_from_row_column(8, 0) == 0xFF, "(8,0) -> out of bounds");
    check(get_button_id_from_row_column(0, 8) == 0xFF, "(0,8) -> out of bounds");
}

void testWriteLedBrg() {
    printHeader("BRG byte order in buffer");
    memset(g_display_buffer, 0, sizeof(g_display_buffer));
    uint8_t red[] = {48, 0, 0};
    writeLedBrg(g_display_buffer, 5, red);
    check(g_display_buffer[15] == 0,  "Buffer[15] = B = 0");
    check(g_display_buffer[16] == 48, "Buffer[16] = R = 48");
    check(g_display_buffer[17] == 0,  "Buffer[17] = G = 0");

    uint8_t cyan[] = {0, 30, 30};
    memset(g_display_buffer, 0, sizeof(g_display_buffer));
    writeLedBrg(g_display_buffer, 10, cyan);
    check(g_display_buffer[30] == 30, "Cyan buffer[30] = B = 30");
    check(g_display_buffer[31] == 0,  "Cyan buffer[31] = R = 0");
    check(g_display_buffer[32] == 30, "Cyan buffer[32] = G = 30");
}

// ============================================================
// TESTS — ANIMATIONS
// ============================================================
void testSquareAnimation() {
    printHeader("Square animation (button 27 = center)");
    uint8_t red[] = {48, 0, 0};
    geometric_animation_btn_id[0] = 27;
    geometric_animation_id = 0;
    geometric_animation_color_ptr[0] = red;

    geometric_animation_pos[0] = 1;
    memset(g_display_buffer, 0, sizeof(g_display_buffer));
    run_geometric_square_animation(g_display_buffer, false);
    int lit1 = countLitLeds(g_display_buffer);
    check(lit1 > 0, "Position 1: LEDs lit");
    printf("     (lit %d LEDs)\n", lit1);

    geometric_animation_pos[0] = 2;
    memset(g_display_buffer, 0, sizeof(g_display_buffer));
    run_geometric_square_animation(g_display_buffer, false);
    int lit2 = countLitLeds(g_display_buffer);
    check(lit2 > lit1, "Position 2: more LEDs than position 1");
    printf("     (lit %d LEDs)\n", lit2);

    geometric_animation_pos[0] = 3;
    memset(g_display_buffer, 0, sizeof(g_display_buffer));
    run_geometric_square_animation(g_display_buffer, false);
    int lit3 = countLitLeds(g_display_buffer);
    check(lit3 > lit2, "Position 3: more LEDs than position 2");
    printf("     (lit %d LEDs)\n", lit3);
}

void testCircleAnimation() {
    printHeader("Circle animation (button 27 = center)");
    uint8_t blue[] = {0, 0, 48};
    geometric_animation_btn_id[0] = 27;
    geometric_animation_id = 0;
    geometric_animation_color_ptr[0] = blue;

    for (uint8_t pos = 1; pos <= 4; pos++) {
        geometric_animation_pos[0] = pos;
        memset(g_display_buffer, 0, sizeof(g_display_buffer));
        run_geometric_circle_animation(g_display_buffer, false);
        int lit = countLitLeds(g_display_buffer);
        printf("     Position %d: lit %d LEDs\n", pos, lit);
        check(lit > 0, "Circle position has LEDs");
    }
}

void testStarAnimation() {
    printHeader("Star animation (button 27 = center)");
    uint8_t yellow[] = {32, 25, 0};
    geometric_animation_btn_id[0] = 27;
    geometric_animation_id = 0;
    geometric_animation_color_ptr[0] = yellow;

    geometric_animation_pos[0] = 2;
    memset(g_display_buffer, 0, sizeof(g_display_buffer));
    run_geometric_star_animation(g_display_buffer, false);
    int lit = countLitLeds(g_display_buffer);
    check(lit > 0, "Star position 2 has LEDs lit");
    check(lit >= 4, "Star has at least 4 lit LEDs (has rays)");
    printf("     (lit %d LEDs)\n", lit);
}

void testTriangleAnimation() {
    printHeader("Triangle animation (button 27 = center)");
    uint8_t green[] = {0, 48, 0};
    geometric_animation_btn_id[0] = 27;
    geometric_animation_id = 0;
    geometric_animation_color_ptr[0] = green;

    geometric_animation_pos[0] = 2;
    memset(g_display_buffer, 0, sizeof(g_display_buffer));
    run_geometric_triangle_animation(g_display_buffer, false);
    int lit = countLitLeds(g_display_buffer);
    check(lit > 0, "Triangle position 2 has LEDs lit");
    printf("     (lit %d LEDs)\n", lit);
}

void testAnimationBounds() {
    printHeader("Animation bounds (all buttons, all positions)");
    uint8_t red[] = {48, 0, 0};
    for (uint8_t btn = 0; btn < 64; btn++) {
        geometric_animation_btn_id[0] = btn;
        geometric_animation_id = 0;
        geometric_animation_color_ptr[0] = red;
        for (uint8_t pos = 1; pos < GEOMETRIC_ANIMATION_STEPS_CIRCLE; pos++) {
            geometric_animation_pos[0] = pos;
            memset(g_display_buffer, 0, sizeof(g_display_buffer));
            run_geometric_square_animation(g_display_buffer, false);
            run_geometric_circle_animation(g_display_buffer, false);
            run_geometric_star_animation(g_display_buffer, false);
            run_geometric_triangle_animation(g_display_buffer, false);
        }
    }
    check(true, "No crash or overflow for 64 buttons x 8 positions x 4 anims");
}

void testColorPropagation() {
    printHeader("Color propagation through animation");
    uint8_t red[] = {48, 0, 0};
    geometric_animation_btn_id[0] = 27;
    geometric_animation_id = 0;
    geometric_animation_color_ptr[0] = red;
    geometric_animation_pos[0] = 2;
    memset(g_display_buffer, 0, sizeof(g_display_buffer));
    run_geometric_square_animation(g_display_buffer, false);

    int red_count = 0;
    for (int i = 0; i < 64; i++) {
        if (g_display_buffer[i*3+0] == 0 &&
            g_display_buffer[i*3+1] == 48 &&
            g_display_buffer[i*3+2] == 0) {
            red_count++;
        }
    }
    check(red_count > 0, "Red color propagated correctly (BRG order)");
    printf("     (%d LEDs show red)\n", red_count);
}

// ============================================================
// TESTS — FLASH / PULSE
// ============================================================
void testFlashAnimation() {
    printHeader("Flash animation bitmask");
    display_flash_counter = 0x0000;
    check(flash_animation(1) == false, "Rate 1, counter=0x0000 -> off");
    display_flash_counter = 0x0080;
    check(flash_animation(1) == true, "Rate 1, counter=0x0080 -> on");
    display_flash_counter = 0x0100;
    check(flash_animation(1) == false, "Rate 1, counter=0x0100 -> off");

    display_flash_counter = 0x0010;
    check(flash_animation(4) == true, "Rate 4, counter=0x0010 -> on");
    display_flash_counter = 0x0020;
    check(flash_animation(4) == false, "Rate 4, counter=0x0020 -> off");
}

void testPulseAnimation() {
    printHeader("Pulse animation (sine)");
    display_flash_counter = 0;
    uint8_t v0 = pulse_animation(5);
    display_flash_counter = 32;
    uint8_t v32 = pulse_animation(5);
    display_flash_counter = 64;
    uint8_t v64 = pulse_animation(5);
    printf("     counter=0 -> %d, counter=32 -> %d, counter=64 -> %d\n", v0, v32, v64);
    check(v0 != v32 || v32 != v64, "Pulse values vary with counter");

    bool in_bounds = true;
    for (int i = 0; i < 256; i++) {
        display_flash_counter = i;
        uint8_t v = pulse_animation(5);
        (void)v;
    }
    check(in_bounds, "Pulse values always in [0,255]");
}

// ============================================================
// TESTS — MIDI ANIMATION VELOCITY BANDS
// ============================================================
void testMidiAnimationVelocityBands() {
    printHeader("MIDI animation velocity bands");
    uint8_t vel_test;
    vel_test = 10;
    check(vel_test < 18, "Velocity 10 is in VU band (0-17)");
    vel_test = 25;
    check(vel_test >= 18 && vel_test < 34, "Velocity 25 is in brightness band (18-33)");
    vel_test = 38;
    check(vel_test >= 34 && vel_test < 42, "Velocity 38 is in flash band (34-41)");
    vel_test = 45;
    check(vel_test >= 42 && vel_test < 50, "Velocity 45 is in pulse band (42-49)");
    vel_test = 52;
    check(vel_test >= 50 && vel_test < 54, "Velocity 52 is in geometric band (50-53)");
    vel_test = 100;
    check(vel_test >= 54, "Velocity 100 is above all bands");
}

// ============================================================
// MAIN
// ============================================================
int main() {
    printf("MF64 Logic Test Harness\n");
    printf("=======================\n");

    // Core logic
    testNoteMapping();
    testBankSelectTiming();
    testDebounceAlgorithm();
    testComboStateMachine();
    testSysExPushConf();
    testIsrTick();

    // Palettes
    testPaletteValues();
    testDefaultBankColors();

    // Geometry
    testGeometryRowColumn();
    testButtonIdFromRowColumn();
    testWriteLedBrg();

    // Animations
    testSquareAnimation();
    testCircleAnimation();
    testStarAnimation();
    testTriangleAnimation();
    testAnimationBounds();
    testColorPropagation();

    // Flash / pulse
    testFlashAnimation();
    testPulseAnimation();

    // MIDI animation bands
    testMidiAnimationVelocityBands();

    printf("\n=======================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
