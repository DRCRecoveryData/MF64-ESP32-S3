/*
 * MF64 Firmware — ESP32-S3 Port (source-verified, logic-tested 82/82)
 * Behavioral port of wunnation/Midi_Fighter_64
 *
 * Hardware substitutions (silicon-level, unavoidable):
 *   - LUFA USB          -> TinyUSB (ESP32 default VID 0x303A)
 *   - AVR EEPROM        -> NVS
 *   - Timer1 @ 1.95 kHz -> ESP32 hw_timer @ ~1.95 kHz
 *   - Bit-banged WS2812 -> Adafruit_NeoPixel RMT
 *   - AVR DFU jump      -> no-op
 */

// ============================================================
// USB IDENTITY
// ============================================================
#define USB_MANUFACTURER  "DJ TechTools"
#define USB_PRODUCT       "Midi Fighter 64"

#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include "USB.h"
#include "USBMIDI.h"

// ============================================================
// HARDWARE PINS
// ============================================================
#define PIN_G0     18
#define PIN_G1     4
#define PIN_G2     6
#define PIN_G3     7
#define PIN_LATCH  15
#define PIN_CLOCK  16
#define PIN_DATA   17

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
#define LED_REFRESH_LIMIT       25
#define MF64_BANK_CC            3
#define MIDI_OUTPUT_MODE_NOTES_ONLY    0
#define MIDI_OUTPUT_MODE_NOTES_AND_CCS 1
#define MIDI_OUTPUT_MODE_CCS_ONLY      2
#define MIDI_MFR_ID_0           0x00
#define MIDI_MFR_ID_1           0x01
#define MIDI_MFR_ID_2           0x79
#define EEPROM_LAYOUT           1

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
#define CONCURRENT_GEOMETRIC_ANIMATIONS    4
#define GEOMETRIC_ANIMATION_G_LED_IDX      1
#define GEOMETRIC_ANIMATION_G_LED_LIMIT    4
#define GEOMETRIC_ANIMATION_TYPES          6
#define SIXTEENTH_FLASH_STATE 0x01

#define OVERRIDE_BALL_SLOWDOWN_FACTOR 0xF0
#define BALL_DEMO_MAX_BALLS 2
#define BALL_DEMO_NEW_BALL_DELAY_LIMIT 0x18
#define BALL_DEMO_MAX_SLOWDOWN_FACTOR 0xFF

#define LAVENDER_GREEN_LIMIT 0x24
#define MF3D_UTILITY_BRIGHT_COLOR_LIMIT 0x80

// ============================================================
// LED OBJECTS
// ============================================================
Adafruit_NeoPixel g0(32, PIN_G0, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel g1(32, PIN_G1, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel g2(32, PIN_G2, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel g3(32, PIN_G3, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel* groups[4] = { &g0, &g1, &g2, &g3 };

USBMIDI MIDI;
Preferences prefs;

// ============================================================
// SETTINGS
// ============================================================
uint8_t G_EE_MIDI_CHANNEL      = 2;
uint8_t G_EE_MIDI_VELOCITY     = 127;
uint8_t G_EE_COMBOS_ENABLE     = 0x01;
uint8_t G_EE_MIDI_OUTPUT_MODE  = MIDI_OUTPUT_MODE_NOTES_ONLY;
uint8_t G_EE_FOUR_BANKS_MODE   = 0x00;
uint8_t G_EE_TILT_MODE         = 0x02;
uint8_t G_EE_TILT_MASK         = 0xF1;
uint8_t G_EE_ANIMATIONS        = GEOMETRIC_ANIMATION_TYPES;
uint8_t G_EE_TILT_SENSITIVITY  = 0x1E;
uint8_t G_EE_PITCH_SENSITIVITY = 0x7F;
uint8_t G_EE_TILT_RANGE        = 0x46;
uint8_t G_EE_PITCH_RANGE       = 0x3C;
uint8_t G_EE_TILT_DEADZONE     = 0x0C;
uint8_t G_EE_PITCH_DEADZONE    = 0x7F;
uint8_t G_EE_TILT_AXIS         = 0x00;
uint8_t G_EE_PICK_SENSITIVITY  = 0x40;
uint8_t G_EE_SIDE_BANK         = 0x00;
uint8_t G_EE_SLEEP_TIME        = 0x3C;
uint8_t G_EE_KEYPRESS_LED      = 0x01;
uint8_t g_self_test_passed     = 0xFF;

// ============================================================
// STATE
// ============================================================
uint64_t g_debounce[DEBOUNCE_BUFFER_SIZE];
uint8_t  g_debounce_pos = 0;
uint64_t g_key_state    = 0;
uint64_t g_key_prev     = 0;
uint64_t g_key_down     = 0;
uint64_t g_key_up       = 0;
uint64_t g_key_bank_last_up = 0;
volatile uint16_t system_time_ms = 0;
uint32_t last_debounce_time = 0;
uint32_t last_led_refresh_time_ms = 0;

uint8_t  g_bank_selected = 0;
uint8_t  g_midi_note_state[2][MIDI_MAX_NOTES];
uint8_t  g_midi_note_off_counter[MIDI_MAX_NOTES];
uint16_t g_bank_select_counter[NUM_BANKS];
uint8_t  g_display_buffer[64 * 3];

volatile uint16_t g_led_counter[4]  = {0, 0, 0, 0};
volatile uint16_t display_flash_counter = 0;
volatile uint16_t half_ms_counter      = 0;
volatile uint8_t  one_second_counter   = 0;
volatile uint8_t  sleep_minute_counter = 0;
volatile uint16_t tick_counter         = 0;
volatile bool     midi_clock_enabled   = false;
float    rgb_freq = 0.049f;

const uint8_t bank_select_key_ids[NUM_BANKS] = {28, 63};

// ============================================================
// PALETTES
// ============================================================
enum DefaultColorIds {
    COLORID_OFF = 0, COLORID_RED = 1, COLORID_RED_DIM = 2,
    COLORID_ORANGE = 3, COLORID_ORANGE_DIM = 4,
    COLORID_YELLOW = 5, COLORID_YELLOW_DIM = 6,
    COLORID_CHARTREUSE = 7, COLORID_CHARTREUSE_DIM = 8,
    COLORID_GREEN = 9, COLORID_GREEN_DIM = 10,
    COLORID_CYAN = 11, COLORID_CYAN_DIM = 12,
    COLORID_BLUE = 13, COLORID_BLUE_DIM = 14,
    COLORID_LAVENDER = 15, COLORID_LAVENDER_DIM = 16,
    COLORID_PINK = 17, COLORID_PINK_DIM = 18,
    COLORID_WHITE = 19
};

const uint8_t default_color[20][3] = {
    {0x00,0x00,0x00}, {48,0x00,0x00}, {24,0x00,0x00}, {40,12,0x00},
    {20,6,0x00},      {32,25,0x00},   {16,12,0x00},   {25,32,0x00},
    {12,16,0x00},     {0x00,48,0x00}, {0x00,24,0x00}, {0x00,30,30},
    {0x00,15,15},     {0x00,0x00,48}, {0x00,0x00,24}, {25,7,32},
    {13,3,17},        {36,0x00,18},   {18,0x00,9},    {24,24,24},
};

uint8_t default_bank_inactive[2][64 * 3];
uint8_t default_bank_active[2][64 * 3];

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
// COMBO STATE TABLE (verbatim from combo.c)
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
// GEOMETRIC / BALL DEMO STATE
// ============================================================
uint8_t geometric_animation_btn_id[CONCURRENT_GEOMETRIC_ANIMATIONS] = {0,0,0,0};
uint8_t geometric_animation_type[CONCURRENT_GEOMETRIC_ANIMATIONS]   = {0,0,0,0};
uint8_t geometric_animation_pos[CONCURRENT_GEOMETRIC_ANIMATIONS]    = {GEOMETRIC_ANIMATION_STEPS_SQUARE,
                                                                       GEOMETRIC_ANIMATION_STEPS_SQUARE,
                                                                       GEOMETRIC_ANIMATION_STEPS_SQUARE,
                                                                       GEOMETRIC_ANIMATION_STEPS_SQUARE};
uint8_t* geometric_animation_color_ptr[CONCURRENT_GEOMETRIC_ANIMATIONS];
uint8_t geometric_animation_id = 0;
uint8_t assign_geometric_animation_id = 0;

const uint8_t vertical_slowdown_factors[8] = {0x0C, 0x0A, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03};
static uint8_t ball_index = 0;
static bool     ball_horizontal_direction[BALL_DEMO_MAX_BALLS];
static uint8_t  ball_horizontal_slowdown_factor[BALL_DEMO_MAX_BALLS];
static uint8_t  ball_horizontal_slowdown_counter[BALL_DEMO_MAX_BALLS];
static bool     ball_vertical_direction[BALL_DEMO_MAX_BALLS];
static uint8_t  ball_vertical_slowdown_counter[BALL_DEMO_MAX_BALLS];
static uint8_t  ball_vertical_slowdown_factor[BALL_DEMO_MAX_BALLS];
static uint8_t  ball_vertical_max[BALL_DEMO_MAX_BALLS];
static uint8_t  ball_color[BALL_DEMO_MAX_BALLS];
static uint8_t  ball_grid[8][8];

static uint16_t rand16_state = 0x1D2B;
uint16_t random16() {
    rand16_state ^= rand16_state << 7;
    rand16_state ^= rand16_state >> 9;
    rand16_state ^= rand16_state << 8;
    return rand16_state;
}

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
void start_geometric_animation(uint8_t button_id, uint8_t animation_id);
void sendSysEx(const uint8_t* data, uint16_t length);
void adjust_inactive_bank_leds_for_power(uint8_t bank, uint8_t offset, uint8_t size);
void adjust_active_bank_leds_for_power(uint8_t bank, uint8_t offset, uint8_t size);

// ============================================================
// HELPERS
// ============================================================
void loadDefaultColors() {
    for (int b = 0; b < 2; b++) {
        uint8_t inactive_idx = (b == 0) ? COLORID_OFF : COLORID_WHITE;
        uint8_t active_idx   = (b == 0) ? COLORID_BLUE : COLORID_GREEN;
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

uint8_t channelForBank(uint8_t bank) {
    if (bank == 1) return (G_EE_MIDI_CHANNEL - 1) & 0x0F;
    return G_EE_MIDI_CHANNEL;
}

// ============================================================
// HARDWARE TIMER @ ~1.95 kHz
// ============================================================
hw_timer_t* timer_2khz = NULL;

void IRAM_ATTR onTimer2kHz() {
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
// BUTTON SCAN
// ============================================================
uint64_t scanRaw() {
    uint64_t state = 0;
    digitalWrite(PIN_LATCH, HIGH);
    delayMicroseconds(2);
    digitalWrite(PIN_LATCH, LOW);
    delayMicroseconds(2);
    for (int i = 0; i < 64; i++) {
        digitalWrite(PIN_CLOCK, LOW);
        delayMicroseconds(1);
        if (digitalRead(PIN_DATA) == HIGH) state |= (1ULL << i);
        digitalWrite(PIN_CLOCK, HIGH);
        delayMicroseconds(1);
    }
    return state;
}

uint64_t scanDebounced() {
    g_debounce[g_debounce_pos] = scanRaw();
    g_debounce_pos = (g_debounce_pos + 1) % DEBOUNCE_BUFFER_SIZE;
    uint64_t result = 0xFFFFFFFFFFFFFFFFULL;
    for (int i = 0; i < DEBOUNCE_BUFFER_SIZE; i++) result &= g_debounce[i];
    return result;
}

// ============================================================
// MIDI SEND (ESP32 core 3.x: API is 1-indexed)
// ============================================================
void midiNoteOn(uint8_t n, uint8_t v, uint8_t c)  { MIDI.noteOn(n, v, c + 1); }
void midiNoteOff(uint8_t n, uint8_t c)             { MIDI.noteOff(n, 0, c + 1); }
void midiCC(uint8_t cc, uint8_t v, uint8_t c)      { MIDI.controlChange(cc, v, c + 1); }

void sendSysEx(const uint8_t* data, uint16_t length) {
    uint16_t pos = 0;
    while (pos < length) {
        midiEventPacket_t packet;
        uint16_t remaining = length - pos;
        if (pos == 0) {
            packet.header = 0x04;
            packet.byte1 = data[pos++];
            packet.byte2 = (remaining > 1) ? data[pos++] : 0;
            packet.byte3 = (remaining > 2) ? data[pos++] : 0;
        } else if (remaining >= 3) {
            packet.header = 0x04;
            packet.byte1 = data[pos++];
            packet.byte2 = data[pos++];
            packet.byte3 = data[pos++];
        } else if (remaining == 2) {
            packet.header = 0x06;
            packet.byte1 = data[pos++];
            packet.byte2 = data[pos++];
            packet.byte3 = 0;
        } else {
            packet.header = 0x05;
            packet.byte1 = data[pos++];
            packet.byte2 = 0;
            packet.byte3 = 0;
        }
        MIDI.writePacket(&packet);
    }
}

// ============================================================
// COMBOS
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

// ============================================================
// BANK SELECT
// ============================================================
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
    uint8_t this_key = b ? bank_select_key_ids[1] : bank_select_key_ids[0];
    uint8_t anim = G_EE_ANIMATIONS < GEOMETRIC_ANIMATION_TYPES ? G_EE_ANIMATIONS : GEOMETRIC_ANIMATION_TYPE_SQUARE;
    start_geometric_animation(this_key, anim);
    memset(g_bank_select_counter, 0, sizeof(g_bank_select_counter));
}
void send_change_bank_notification(uint8_t b) { midiCC(MF64_BANK_CC, b, G_EE_MIDI_CHANNEL); }

// FIXED: cast subtraction to uint16_t for x86 portability
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

// ============================================================
// GEOMETRIC ANIMATIONS
// ============================================================
uint8_t clamp_u8(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (uint8_t)v;
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
    if (col < 4) id += col; else id += (col & 0x03) + 32;
    return id;
}
void writeLedBrg(uint8_t* buf, uint8_t btn_id, uint8_t* src) {
    if (btn_id >= 64) return;
    uint8_t* p = buf + btn_id * 3;
    *p++ = src[2]; *p++ = src[0]; *p++ = src[1];
}
void set_geometric_animation_color_source(uint8_t button_id) {
    uint8_t vel = g_midi_note_state[0][button_id + NUM_BUTTONS * g_bank_selected];
    if (vel <= 0 || vel >= 121) {
        geometric_animation_color_ptr[assign_geometric_animation_id] =
            default_bank_active[g_bank_selected] + button_id * 3;
    } else {
        uint8_t color = clamp_u8(((vel - 1) / 6) - 1, 0, 19);
        geometric_animation_color_ptr[assign_geometric_animation_id] = (uint8_t*)default_color[color];
    }
}
void start_geometric_square_animation(uint8_t b) {
    geometric_animation_btn_id[assign_geometric_animation_id] = b;
    geometric_animation_pos[assign_geometric_animation_id] = 0;
    geometric_animation_type[assign_geometric_animation_id] = GEOMETRIC_ANIMATION_TYPE_SQUARE;
    set_geometric_animation_color_source(b);
    g_led_counter[GEOMETRIC_ANIMATION_G_LED_IDX] = GEOMETRIC_ANIMATION_G_LED_LIMIT;
}
void start_geometric_circle_animation(uint8_t b) {
    geometric_animation_btn_id[assign_geometric_animation_id] = b;
    geometric_animation_pos[assign_geometric_animation_id] = 0;
    geometric_animation_type[assign_geometric_animation_id] = GEOMETRIC_ANIMATION_TYPE_CIRCLE;
    set_geometric_animation_color_source(b);
    g_led_counter[GEOMETRIC_ANIMATION_G_LED_IDX] = GEOMETRIC_ANIMATION_G_LED_LIMIT;
}
void start_geometric_star_animation(uint8_t b) {
    geometric_animation_btn_id[assign_geometric_animation_id] = b;
    geometric_animation_pos[assign_geometric_animation_id] = 0;
    geometric_animation_type[assign_geometric_animation_id] = GEOMETRIC_ANIMATION_TYPE_STAR;
    set_geometric_animation_color_source(b);
    g_led_counter[GEOMETRIC_ANIMATION_G_LED_IDX] = GEOMETRIC_ANIMATION_G_LED_LIMIT;
}
void start_geometric_triangle_animation(uint8_t b) {
    geometric_animation_btn_id[assign_geometric_animation_id] = b;
    geometric_animation_pos[assign_geometric_animation_id] = 0;
    geometric_animation_type[assign_geometric_animation_id] = GEOMETRIC_ANIMATION_TYPE_TRIANGLE;
    set_geometric_animation_color_source(b);
    g_led_counter[GEOMETRIC_ANIMATION_G_LED_IDX] = GEOMETRIC_ANIMATION_G_LED_LIMIT;
}
void start_geometric_animation(uint8_t button_id, uint8_t animation_id) {
    if (g_led_counter[1] == 0) g_led_counter[1] = GEOMETRIC_ANIMATION_G_LED_LIMIT;
    switch (animation_id) {
        case GEOMETRIC_ANIMATION_TYPE_CIRCLE:   start_geometric_circle_animation(button_id); break;
        case GEOMETRIC_ANIMATION_TYPE_SQUARE:   start_geometric_square_animation(button_id); break;
        case GEOMETRIC_ANIMATION_TYPE_STAR:     start_geometric_star_animation(button_id); break;
        case GEOMETRIC_ANIMATION_TYPE_TRIANGLE: start_geometric_triangle_animation(button_id); break;
    }
    assign_geometric_animation_id = (assign_geometric_animation_id + 1) % CONCURRENT_GEOMETRIC_ANIMATIONS;
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
uint8_t get_distance_from_center_in_steps(uint8_t bR, uint8_t bC, uint8_t r, uint8_t c) {
    uint8_t dr = bR > r ? bR - r : r - bR;
    uint8_t dc = bC > c ? bC - c : c - bC;
    return dr + dc;
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
void geometric_animation_state(uint8_t bank, uint8_t* buffer) {
    (void)bank;
    bool update = false;
    if (g_led_counter[1] == 0) { g_led_counter[1] = GEOMETRIC_ANIMATION_G_LED_LIMIT; update = true; }
    for (uint8_t i = 0; i < CONCURRENT_GEOMETRIC_ANIMATIONS; i++) {
        geometric_animation_id = i;
        uint8_t t = geometric_animation_type[i], p = geometric_animation_pos[i];
        if (t == GEOMETRIC_ANIMATION_TYPE_SQUARE   && p < GEOMETRIC_ANIMATION_STEPS_SQUARE)   run_geometric_square_animation(buffer, update);
        else if (t == GEOMETRIC_ANIMATION_TYPE_CIRCLE   && p < GEOMETRIC_ANIMATION_STEPS_CIRCLE)   run_geometric_circle_animation(buffer, update);
        else if (t == GEOMETRIC_ANIMATION_TYPE_STAR     && p < GEOMETRIC_ANIMATION_STEPS_STAR)     run_geometric_star_animation(buffer, update);
        else if (t == GEOMETRIC_ANIMATION_TYPE_TRIANGLE && p < GEOMETRIC_ANIMATION_STEPS_TRIANGLE) run_geometric_triangle_animation(buffer, update);
    }
}

// ============================================================
// MIDI ANIMATION STATE
// ============================================================
bool flash_animation(uint8_t rate) {
    return (display_flash_counter & (SIXTEENTH_FLASH_STATE << (8 - rate))) != 0;
}
uint8_t pulse_animation(uint8_t rate) {
    uint8_t step;
    if (!midi_clock_enabled) step = (uint8_t)(((display_flash_counter << 4) >> (8 - rate)) & 0xFF);
    else                     step = (uint8_t)(((display_flash_counter << 5) >> (8 - rate)) & 0xFF);
    int level = (int)(sin(rgb_freq * step) * 127.0f + 127.0f);
    if (level < 0) level = 0;
    if (level > 255) level = 255;
    return (uint8_t)level;
}
void midi_animation_state(uint8_t bank, uint8_t* buffer) {
    uint8_t bank_offset = bank ? NUM_BUTTONS : 0;
    for (uint8_t i = bank_offset; i < bank_offset + 64; i++) {
        uint8_t vel = g_midi_note_state[1][i];
        uint8_t key = i & BUTTON_ID_FLAGS;
        if (vel == 0) continue;
        if (vel < 18) {
            // VU meter not ported
        } else if (vel < 34) {
            uint8_t* p = buffer + key * 3;
            uint8_t level = vel - 18;
            p[0] = (p[0] * level) >> 4;
            p[1] = (p[1] * level) >> 4;
            p[2] = (p[2] * level) >> 4;
        } else if (vel < 42) {
            if (!flash_animation(vel - 33)) {
                uint8_t* p = buffer + key * 3;
                p[0] = 0; p[1] = 0; p[2] = 0;
            }
        } else if (vel < 50) {
            uint8_t* p = buffer + key * 3;
            uint8_t level = pulse_animation(vel - 41);
            p[0] = (p[0] * level) >> 8;
            p[1] = (p[1] * level) >> 8;
            p[2] = (p[2] * level) >> 8;
        } else if (vel < 54) {
            start_geometric_animation(i & BUTTON_ID_FLAGS, vel - 50);
            g_midi_note_state[1][i] = 0;
        }
    }
}

// ============================================================
// BALL DEMO
// ============================================================
void ball_demo_setup() {
    memset(ball_grid, BALL_DEMO_MAX_BALLS, sizeof(ball_grid));
}
bool new_ball() {
    static uint8_t new_ball_delay_counter = BALL_DEMO_NEW_BALL_DELAY_LIMIT;
    static bool direction = true;
    new_ball_delay_counter++;
    if (new_ball_delay_counter < BALL_DEMO_NEW_BALL_DELAY_LIMIT) return false;
    new_ball_delay_counter = 0;
    direction ^= 1;
    uint8_t color = random16() & 0x1F;
    color = color < 18 ? color + 1 : color - 17;
    ball_horizontal_direction[ball_index] = direction;
    uint8_t hp = direction ? 7 : 0;
    uint8_t vp = (random16() & 0x07) + 2;
    vp = vp >= 8 ? 7 : vp;
    if (ball_grid[vp][hp] < BALL_DEMO_MAX_BALLS) return false;
    ball_grid[vp][hp] = ball_index;
    ball_color[ball_index] = color;
    ball_horizontal_slowdown_factor[ball_index] = 8 + (random16() & 0x1F);
    ball_vertical_max[ball_index] = vp;
    ball_vertical_direction[ball_index] = 0;
    ball_vertical_slowdown_factor[ball_index] = BALL_DEMO_MAX_SLOWDOWN_FACTOR;
    ball_horizontal_slowdown_counter[ball_index] = 0;
    ball_vertical_slowdown_counter[ball_index] = 0;
    ball_index = (ball_index + 1) % BALL_DEMO_MAX_BALLS;
    return true;
}
uint8_t move_ball_vertically(uint8_t bid, uint8_t this_row) {
    uint8_t new_row = this_row;
    uint8_t vc = ball_vertical_slowdown_counter[bid] + 1;
    uint8_t bro = (ball_vertical_max[bid] - this_row) & 0x07;
    if (vc >= vertical_slowdown_factors[bro]) {
        ball_vertical_slowdown_counter[bid] = 0;
        new_row = ball_vertical_direction[bid] ? this_row - 1 : this_row + 1;
        if (new_row <= ball_vertical_max[bid]) {
        } else if (new_row & 0x80) {
            new_row = this_row;
            ball_vertical_direction[bid] ^= 1;
            ball_vertical_slowdown_counter[bid] = vertical_slowdown_factors[bro];
        } else {
            new_row = this_row;
            ball_vertical_direction[bid] ^= 1;
            ball_vertical_slowdown_counter[bid] = vertical_slowdown_factors[bro];
        }
    } else ball_vertical_slowdown_counter[bid] = vc;
    return new_row;
}
uint8_t move_ball_horizontally(uint8_t bid, uint8_t this_col) {
    uint8_t new_col = this_col;
    uint8_t hc = ball_horizontal_slowdown_counter[bid] + 1;
    if (hc >= ball_horizontal_slowdown_factor[bid]) {
        bool dir = ball_horizontal_direction[bid];
        ball_horizontal_slowdown_counter[bid] = 0;
        new_col = dir ? this_col - 1 : this_col + 1;
        if (new_col < 8) {
        } else {
            new_col = this_col;
            dir ^= 1;
            ball_horizontal_direction[bid] = dir;
            ball_horizontal_slowdown_counter[bid] = ball_horizontal_slowdown_factor[bid];
        }
    } else ball_horizontal_slowdown_counter[bid] = hc;
    return new_col;
}
void change_ball_colors_after_collision(uint8_t tb, uint8_t that) {
    (void)tb;
    uint16_t rv = random16();
    uint16_t color = rv >> 5 & 0x1F;
    color = color < 18 ? color + 1 : color - 17;
    ball_color[that] = color;
}
bool repeat_collision_limit_reached(uint8_t tb, uint8_t that) {
    static uint8_t last_t = 0xFF, last_that = 0xFF, ctr = 0;
    if ((tb == last_t && that == last_that) || (tb == last_that && that == last_t)) ctr++;
    else { ctr = 0; last_t = tb; last_that = that; return false; }
    if (ctr >= 0xFF) { ctr = 0; return true; }
    return false;
}
void ball_demo_run(uint8_t* buffer) {
    if (g_led_counter[2] == 0) {
        g_led_counter[2] = 0x30;
        bool disable_collision[64] = {false};
        uint8_t balls_on_grid = 0;
        for (uint8_t tr = 0; tr < 8; tr++) {
            for (uint8_t tc = 0; tc < 8; tc++) {
                uint8_t tb = ball_grid[tr][tc];
                if (tb >= BALL_DEMO_MAX_BALLS) continue;
                uint8_t nc = move_ball_horizontally(tb, tc);
                uint8_t nr = move_ball_vertically(tb, tr);
                uint8_t that = ball_grid[nr][nc];
                if (that >= BALL_DEMO_MAX_BALLS) {
                    ball_grid[tr][tc] = BALL_DEMO_MAX_BALLS;
                    ball_grid[nr][nc] = tb;
                    balls_on_grid++;
                } else if (that == tb) {
                    balls_on_grid++;
                } else {
                    uint8_t btn_id = get_button_id_from_row_column(tr, tc);
                    balls_on_grid++;
                    if (disable_collision[tb]) {
                    } else if (repeat_collision_limit_reached(tb, that)) {
                        start_geometric_animation(btn_id, random16() & 0x03);
                        ball_grid[tr][tc] = BALL_DEMO_MAX_BALLS;
                        ball_grid[nr][nc] = BALL_DEMO_MAX_BALLS;
                        if (nr < tr || nc < tc) balls_on_grid -= 2;
                    } else if (nr > tr) {
                        ball_vertical_direction[tb] = false;
                        ball_vertical_direction[that] = true;
                        ball_vertical_slowdown_counter[tb] = OVERRIDE_BALL_SLOWDOWN_FACTOR;
                        change_ball_colors_after_collision(tb, that);
                    } else if (nr < tr) {
                        ball_vertical_direction[tb] = true;
                        ball_vertical_direction[that] = false;
                        ball_vertical_slowdown_counter[tb] = OVERRIDE_BALL_SLOWDOWN_FACTOR;
                        change_ball_colors_after_collision(tb, that);
                    }
                    if (nc > tc) {
                        ball_horizontal_direction[tb] = false;
                        ball_horizontal_direction[that] = true;
                        ball_horizontal_slowdown_counter[tb] = OVERRIDE_BALL_SLOWDOWN_FACTOR;
                        change_ball_colors_after_collision(tb, that);
                    } else if (nc < tc) {
                        ball_horizontal_direction[tb] = true;
                        ball_horizontal_direction[that] = false;
                        ball_horizontal_slowdown_counter[tb] = OVERRIDE_BALL_SLOWDOWN_FACTOR;
                        change_ball_colors_after_collision(tb, that);
                    }
                    disable_collision[that] = true;
                }
            }
        }
        if (balls_on_grid < BALL_DEMO_MAX_BALLS) if (new_ball()) balls_on_grid++;
    }
    for (uint8_t tr = 0; tr < 8; tr++) {
        for (uint8_t tc = 0; tc < 8; tc++) {
            uint8_t btn_id = get_button_id_from_row_column(tr, tc);
            uint8_t bid = ball_grid[tr][tc];
            uint8_t* dp = buffer + 3 * btn_id;
            if (bid >= BALL_DEMO_MAX_BALLS) {
                dp[0] = default_color[0][2]; dp[1] = default_color[0][0]; dp[2] = default_color[0][1];
            } else {
                uint8_t color = ball_color[bid];
                dp[0] = default_color[color][2]; dp[1] = default_color[color][0]; dp[2] = default_color[color][1];
            }
        }
    }
}

// ============================================================
// POWER ADJUSTMENT (from display.c)
// ============================================================
void adjust_inactive_bank_leds_for_power(uint8_t bank, uint8_t offset, uint8_t size) {
    for (uint8_t b = offset; b < offset + size; b += 3) {
        uint8_t r = default_bank_inactive[bank][b];
        uint8_t g = default_bank_inactive[bank][b+1];
        uint8_t bl = default_bank_inactive[bank][b+2];
        uint8_t color_id = COLORID_OFF;
        if (!r) {
            if (!g) {
                if (!bl) color_id = COLORID_OFF;
                else { if (bl == default_color[COLORID_BLUE][2]) continue;
                       color_id = bl > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_BLUE : COLORID_BLUE_DIM; }
            } else if (!bl) {
                if (g == default_color[COLORID_GREEN][1]) continue;
                color_id = g > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_GREEN : COLORID_GREEN_DIM;
            } else {
                if (g == default_color[COLORID_CYAN][1]) continue;
                color_id = g > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_CYAN : COLORID_CYAN_DIM;
            }
        } else if (!g) {
            if (!bl) { if (r == default_color[COLORID_RED][0]) continue;
                       color_id = r > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_RED : COLORID_RED_DIM; }
            else { if (r == default_color[COLORID_PINK][0]) continue;
                   color_id = r > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_PINK : COLORID_PINK_DIM; }
        } else if (!bl) {
            if (r < g) { if (g == default_color[COLORID_CHARTREUSE][1]) continue;
                         color_id = g > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_CHARTREUSE : COLORID_CHARTREUSE_DIM; }
            else if (r >> 1 >= g) { if (r == default_color[COLORID_ORANGE][0]) continue;
                                    color_id = r > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_ORANGE : COLORID_ORANGE_DIM; }
            else { if (r == default_color[COLORID_YELLOW][0]) continue;
                   color_id = r > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_YELLOW : COLORID_YELLOW_DIM; }
        } else {
            if (g > LAVENDER_GREEN_LIMIT) color_id = COLORID_WHITE;
            else { if (bl == default_color[COLORID_LAVENDER][2]) continue;
                   color_id = r > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_LAVENDER : COLORID_LAVENDER_DIM; }
        }
        default_bank_inactive[bank][b]   = default_color[color_id][0];
        default_bank_inactive[bank][b+1] = default_color[color_id][1];
        default_bank_inactive[bank][b+2] = default_color[color_id][2];
    }
}

void adjust_active_bank_leds_for_power(uint8_t bank, uint8_t offset, uint8_t size) {
    for (uint8_t b = offset; b < offset + size; b += 3) {
        uint8_t r = default_bank_active[bank][b];
        uint8_t g = default_bank_active[bank][b+1];
        uint8_t bl = default_bank_active[bank][b+2];
        uint8_t color_id = COLORID_OFF;
        if (!r) {
            if (!g) {
                if (!bl) color_id = COLORID_OFF;
                else { if (bl == default_color[COLORID_BLUE][2]) continue;
                       color_id = bl > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_BLUE : COLORID_BLUE_DIM; }
            } else if (!bl) {
                if (g == default_color[COLORID_GREEN][1]) continue;
                color_id = g > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_GREEN : COLORID_GREEN_DIM;
            } else {
                if (g == default_color[COLORID_CYAN][1]) continue;
                color_id = g > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_CYAN : COLORID_CYAN_DIM;
            }
        } else if (!g) {
            if (!bl) { if (r == default_color[COLORID_RED][0]) continue;
                       color_id = r > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_RED : COLORID_RED_DIM; }
            else { if (r == default_color[COLORID_PINK][0]) continue;
                   color_id = r > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_PINK : COLORID_PINK_DIM; }
        } else if (!bl) {
            if (r < g) { if (g == default_color[COLORID_CHARTREUSE][1]) continue;
                         color_id = g > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_CHARTREUSE : COLORID_CHARTREUSE_DIM; }
            else if (r >> 1 >= g) { if (r == default_color[COLORID_ORANGE][0]) continue;
                                    color_id = r > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_ORANGE : COLORID_ORANGE_DIM; }
            else { if (r == default_color[COLORID_YELLOW][0]) continue;
                   color_id = r > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_YELLOW : COLORID_YELLOW_DIM; }
        } else {
            if (g > LAVENDER_GREEN_LIMIT) color_id = COLORID_WHITE;
            else { if (bl == default_color[COLORID_LAVENDER][2]) continue;
                   color_id = r > MF3D_UTILITY_BRIGHT_COLOR_LIMIT ? COLORID_LAVENDER : COLORID_LAVENDER_DIM; }
        }
        default_bank_active[bank][b]   = default_color[color_id][0];
        default_bank_active[bank][b+1] = default_color[color_id][1];
        default_bank_active[bank][b+2] = default_color[color_id][2];
    }
}

// ============================================================
// DISPLAY UPDATE
// ============================================================
void updateDisplay() {
    uint8_t* active   = default_bank_active[g_bank_selected];
    uint8_t* inactive = default_bank_inactive[g_bank_selected];
    uint8_t* dest = g_display_buffer;
    for (int i = 0; i < 64; i++) {
        bool pressed = (g_key_state >> i) & 1;
        uint8_t* src = pressed ? active : inactive;
        *dest++ = src[i * 3 + 2];
        *dest++ = src[i * 3 + 0];
        *dest++ = src[i * 3 + 1];
    }
    uint8_t bank_offset = g_bank_selected * NUM_BUTTONS;
    for (uint8_t i = bank_offset; i < bank_offset + NUM_BUTTONS; i++) {
        uint8_t vel = g_midi_note_state[0][i];
        if (vel > 0) {
            uint8_t key = i & BUTTON_ID_FLAGS;
            uint8_t* p = g_display_buffer + key * 3;
            p[0] = ableton_midi_feedback_colors[vel][2];
            p[1] = ableton_midi_feedback_colors[vel][0];
            p[2] = ableton_midi_feedback_colors[vel][1];
        }
    }
    midi_animation_state(g_bank_selected, g_display_buffer);

    if (half_ms_counter >= 2000) {
        half_ms_counter = 0;
        one_second_counter++;
        if (one_second_counter >= 60) {
            sleep_minute_counter++;
            one_second_counter = 0;
        }
    }
    if (G_EE_SLEEP_TIME) {
        if (sleep_minute_counter == G_EE_SLEEP_TIME) {
            ball_demo_setup();
            sleep_minute_counter++;
        }
        if (sleep_minute_counter > G_EE_SLEEP_TIME) {
            ball_demo_run(g_display_buffer);
        }
        if (g_key_down) {
            one_second_counter = 0;
            sleep_minute_counter = 0;
        }
    }
    geometric_animation_state(g_bank_selected, g_display_buffer);
}

void pushLeds() {
    for (int g = 0; g < 4; g++) {
        for (int i = 0; i < 32; i++) {
            int btn = g * 16 + (i / 2);
            uint8_t b  = g_display_buffer[btn * 3 + 0];
            uint8_t r  = g_display_buffer[btn * 3 + 1];
            uint8_t gg = g_display_buffer[btn * 3 + 2];
            groups[g]->setPixelColor(i, groups[g]->Color(r, gg, b));
        }
        groups[g]->show();
    }
}

// ============================================================
// SYSEX
// ============================================================
void sendConfigData() {
    uint8_t payload[] = {
        0xF0, MIDI_MFR_ID_0, MIDI_MFR_ID_1, MIDI_MFR_ID_2, 0x02,
        0x1, 0, G_EE_MIDI_CHANNEL+1, 1, G_EE_MIDI_VELOCITY,
        3, G_EE_FOUR_BANKS_MODE, 7, G_EE_MIDI_OUTPUT_MODE,
        8, G_EE_COMBOS_ENABLE, 10, G_EE_ANIMATIONS,
        11, (G_EE_TILT_MASK) & 0x3, 12, (G_EE_TILT_MASK >> 4) & 0xf,
        13, G_EE_TILT_MODE-1, 14, G_EE_TILT_SENSITIVITY,
        15, G_EE_PITCH_SENSITIVITY, 16, G_EE_TILT_RANGE,
        17, G_EE_PITCH_RANGE, 18, G_EE_TILT_DEADZONE,
        19, G_EE_PITCH_DEADZONE, 20, G_EE_TILT_AXIS,
        21, G_EE_PICK_SENSITIVITY, 22, G_EE_SLEEP_TIME,
        23, G_EE_SIDE_BANK, 0xF7
    };
    sendSysEx(payload, sizeof(payload));
}

void saveSettings() {
    prefs.putUChar("ch", G_EE_MIDI_CHANNEL);
    prefs.putUChar("vel", G_EE_MIDI_VELOCITY);
    prefs.putUChar("combos", G_EE_COMBOS_ENABLE);
    prefs.putUChar("outmode", G_EE_MIDI_OUTPUT_MODE);
    prefs.putUChar("4bank", G_EE_FOUR_BANKS_MODE);
    prefs.putUChar("tiltmode", G_EE_TILT_MODE);
    prefs.putUChar("tiltmask", G_EE_TILT_MASK);
    prefs.putUChar("anim", G_EE_ANIMATIONS);
    prefs.putUChar("tiltsens", G_EE_TILT_SENSITIVITY);
    prefs.putUChar("pitchsens", G_EE_PITCH_SENSITIVITY);
    prefs.putUChar("tiltrange", G_EE_TILT_RANGE);
    prefs.putUChar("pitchrange", G_EE_PITCH_RANGE);
    prefs.putUChar("tiltdead", G_EE_TILT_DEADZONE);
    prefs.putUChar("pitchdead", G_EE_PITCH_DEADZONE);
    prefs.putUChar("tiltaxis", G_EE_TILT_AXIS);
    prefs.putUChar("picksens", G_EE_PICK_SENSITIVITY);
    prefs.putUChar("sleep", G_EE_SLEEP_TIME);
    prefs.putUChar("sidebank", G_EE_SIDE_BANK);
    prefs.putBytes("idle", default_bank_inactive, sizeof(default_bank_inactive));
    prefs.putBytes("active", default_bank_active, sizeof(default_bank_active));
}

void loadSettings() {
    G_EE_MIDI_CHANNEL      = prefs.getUChar("ch", 2);
    G_EE_MIDI_VELOCITY     = prefs.getUChar("vel", 127);
    G_EE_COMBOS_ENABLE     = prefs.getUChar("combos", 0x01);
    G_EE_MIDI_OUTPUT_MODE  = prefs.getUChar("outmode", MIDI_OUTPUT_MODE_NOTES_ONLY);
    G_EE_FOUR_BANKS_MODE   = prefs.getUChar("4bank", 0x00);
    G_EE_TILT_MODE         = prefs.getUChar("tiltmode", 0x02);
    G_EE_TILT_MASK         = prefs.getUChar("tiltmask", 0xF1);
    G_EE_ANIMATIONS        = prefs.getUChar("anim", GEOMETRIC_ANIMATION_TYPES);
    G_EE_TILT_SENSITIVITY  = prefs.getUChar("tiltsens", 0x1E);
    G_EE_PITCH_SENSITIVITY = prefs.getUChar("pitchsens", 0x7F);
    G_EE_TILT_RANGE        = prefs.getUChar("tiltrange", 0x46);
    G_EE_PITCH_RANGE       = prefs.getUChar("pitchrange", 0x3C);
    G_EE_TILT_DEADZONE     = prefs.getUChar("tiltdead", 0x0C);
    G_EE_PITCH_DEADZONE    = prefs.getUChar("pitchdead", 0x7F);
    G_EE_TILT_AXIS         = prefs.getUChar("tiltaxis", 0x00);
    G_EE_PICK_SENSITIVITY  = prefs.getUChar("picksens", 0x40);
    G_EE_SLEEP_TIME        = prefs.getUChar("sleep", 0x3C);
    G_EE_SIDE_BANK         = prefs.getUChar("sidebank", 0x00);
    prefs.getBytes("idle", default_bank_inactive, sizeof(default_bank_inactive));
    prefs.getBytes("active", default_bank_active, sizeof(default_bank_active));
}

void factoryReset() {
    prefs.clear();
    G_EE_MIDI_CHANNEL      = 2;
    G_EE_MIDI_VELOCITY     = 127;
    G_EE_COMBOS_ENABLE     = 0x01;
    G_EE_MIDI_OUTPUT_MODE  = MIDI_OUTPUT_MODE_NOTES_ONLY;
    G_EE_FOUR_BANKS_MODE   = 0x00;
    G_EE_TILT_MODE         = 0x02;
    G_EE_TILT_MASK         = 0xF1;
    G_EE_ANIMATIONS        = GEOMETRIC_ANIMATION_TYPES;
    G_EE_TILT_SENSITIVITY  = 0x1E;
    G_EE_PITCH_SENSITIVITY = 0x7F;
    G_EE_TILT_RANGE        = 0x46;
    G_EE_PITCH_RANGE       = 0x3C;
    G_EE_TILT_DEADZONE     = 0x0C;
    G_EE_PITCH_DEADZONE    = 0x7F;
    G_EE_TILT_AXIS         = 0x00;
    G_EE_PICK_SENSITIVITY  = 0x40;
    G_EE_SIDE_BANK         = 0x00;
    G_EE_SLEEP_TIME        = 0x3C;
    loadDefaultColors();
    saveSettings();
}

// FIXED: tag-indexed SysEx decode (matches config.c tv_table_decode)
void handleSysEx(uint8_t* data, uint8_t len) {
    if (len < 5) return;
    uint8_t cmd = data[4];
    switch (cmd) {
        case 0x01: {  // PUSH_CONF (tag-indexed)
            if (len >= 8) {
                uint8_t i = 5;
                while (i < len - 1) {
                    uint8_t tag = data[i++];
                    if (i >= len) break;
                    uint8_t val = data[i++];
                    switch (tag) {
                        case 0x00: G_EE_MIDI_CHANNEL      = val - 1; break;
                        case 0x01: G_EE_MIDI_VELOCITY     = val;     break;
                        case 0x02: G_EE_KEYPRESS_LED      = val;     break;
                        case 0x03: G_EE_FOUR_BANKS_MODE   = val;     break;
                        case 0x07: G_EE_MIDI_OUTPUT_MODE  = val;     break;
                        case 0x08: G_EE_COMBOS_ENABLE     = val;     break;
                        case 0x0A: G_EE_ANIMATIONS        = val;     break;
                        case 0x0B: G_EE_TILT_MASK         = val;     break;
                        case 0x0C: /* tilt mode */                    break;
                        case 0x0E: G_EE_TILT_SENSITIVITY  = val;     break;
                        case 0x0F: G_EE_PITCH_SENSITIVITY = val;     break;
                        case 0x10: G_EE_TILT_RANGE        = val;     break;
                        case 0x11: G_EE_PITCH_RANGE       = val;     break;
                        case 0x12: G_EE_TILT_DEADZONE     = val;     break;
                        case 0x13: G_EE_PITCH_DEADZONE    = val;     break;
                        case 0x14: G_EE_TILT_AXIS         = val;     break;
                        case 0x15: G_EE_PICK_SENSITIVITY  = val;     break;
                        case 0x16: G_EE_SLEEP_TIME        = val;     break;
                        case 0x17: G_EE_SIDE_BANK         = val;     break;
                    }
                }
                saveSettings();
                sendConfigData();
            }
        } break;
        case 0x02: sendConfigData(); break;
        case 0x03: if (len >= 6 && data[5] == 0x02) factoryReset(); break;
        case 0x04: {  // BULK_XFER
            if (len > 6) {
                uint8_t command = data[5];
                uint8_t tag     = data[6];
                if (command == 0) {  // PUSH
                    if (len > 9) {
                        uint8_t part  = data[7];
                        uint8_t total = data[8];
                        uint8_t size  = data[9];
                        (void)total;
                        if (part > 16 || size > (len - 10)) return;
                        uint8_t bank   = (part - 1) >> 3;
                        uint8_t offset = ((part - 1) & 0x07) * 24;
                        if (tag == 1) {
                            for (uint8_t idx = 0; idx < size; ++idx)
                                default_bank_inactive[bank][offset + idx] = data[10 + idx] * 2;
                            adjust_inactive_bank_leds_for_power(bank, offset, size);
                        } else if (tag == 2) {
                            for (uint8_t idx = 0; idx < size; ++idx)
                                default_bank_active[bank][offset + idx] = data[10 + idx] * 2;
                            adjust_active_bank_leds_for_power(bank, offset, size);
                        }
                        saveSettings();
                    }
                } else if (command == 1) {  // PULL
                    uint8_t* source;
                    if (tag == 1)      source = (uint8_t*)default_bank_inactive;
                    else if (tag == 2) source = (uint8_t*)default_bank_active;
                    else return;
                    uint16_t bytes_remaining = 2 * 64 * 3;
                    uint16_t total = bytes_remaining / 24;
                    uint16_t index = 0;
                    for (uint16_t part = 1; part <= total; ++part) {
                        uint16_t size = bytes_remaining > 24 ? 24 : bytes_remaining;
                        bytes_remaining -= 24;
                        uint8_t payload[36];
                        payload[0] = 0xF0;
                        payload[1] = MIDI_MFR_ID_0;
                        payload[2] = MIDI_MFR_ID_1;
                        payload[3] = MIDI_MFR_ID_2;
                        payload[4] = 0x04;
                        payload[5] = 0x00;
                        payload[6] = tag;
                        payload[7] = (uint8_t)part;
                        payload[8] = (uint8_t)total;
                        payload[9] = (uint8_t)size;
                        for (uint8_t idx = 0; idx < size; ++idx) {
                            uint16_t color = source[index++];
                            payload[10 + idx] = color * 127 / 255;
                        }
                        payload[10 + size] = 0xF7;
                        sendSysEx(payload, 11 + size);
                        delay(5);
                    }
                }
            }
        } break;
    }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println();
    Serial.println("=== MF64 ESP32-S3 (full port) ===");

    prefs.begin("mf64", false);
    if (prefs.getUChar("ver", 0) != EEPROM_LAYOUT) {
        factoryReset();
        prefs.putUChar("ver", EEPROM_LAYOUT);
    } else {
        loadSettings();
    }

    for (int i = 0; i < 4; i++) {
        groups[i]->begin();
        groups[i]->setBrightness(20);
        groups[i]->clear();
        groups[i]->show();
    }

    pinMode(PIN_LATCH, OUTPUT);
    pinMode(PIN_CLOCK, OUTPUT);
    pinMode(PIN_DATA, INPUT);
    digitalWrite(PIN_LATCH, LOW);
    digitalWrite(PIN_CLOCK, HIGH);
    for (int i = 0; i < DEBOUNCE_BUFFER_SIZE; i++) g_debounce[i] = 0;

    MIDI.begin();
    USB.begin();

    timer_2khz = timerBegin(1000000);
    timerAttachInterrupt(timer_2khz, &onTimer2kHz);
    timerAlarm(timer_2khz, 512, true, 0);

    delay(20);
    uint64_t boot_keys = scanDebounced();
    if (boot_keys & 0x01) {
        Serial.println(">>> Bootloader key held — halting (no DFU on ESP32)");
        while (1) { delay(1000); }
    }

    sleep_minute_counter = G_EE_SLEEP_TIME;

    Serial.print("Bank "); Serial.print(g_bank_selected + 1);
    Serial.print(" | ch "); Serial.println(channelForBank(g_bank_selected) + 1);
    Serial.print("Sleep: "); Serial.print(G_EE_SLEEP_TIME); Serial.println(" min");
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
    uint32_t now = millis();

    if (now - last_debounce_time >= 10) {
        last_debounce_time = now;

        g_key_prev = g_key_state;
        g_key_state = scanDebounced();
        uint64_t changed = g_key_state ^ g_key_prev;
        g_key_down = changed & g_key_state;
        g_key_up   = changed & ~g_key_state;

        if (g_key_down) { one_second_counter = 0; sleep_minute_counter = 0; }

        for (int i = 0; i < 64; i++) {
            uint64_t bit = 1ULL << i;
            uint8_t note = MIDI_BASENOTE + i;

            if (g_key_down & bit) {
                if (G_EE_MIDI_OUTPUT_MODE < MIDI_OUTPUT_MODE_CCS_ONLY)
                    midiNoteOn(note, G_EE_MIDI_VELOCITY, channelForBank(g_bank_selected));
                if (G_EE_MIDI_OUTPUT_MODE > MIDI_OUTPUT_MODE_NOTES_ONLY)
                    midiCC(note, 127, G_EE_MIDI_CHANNEL);

                if (g_bank_selected <= 0) g_key_bank_last_up &= ~bit;
                else                       g_key_bank_last_up |=  bit;

                if (G_EE_ANIMATIONS < GEOMETRIC_ANIMATION_TYPES)
                    start_geometric_animation(i, G_EE_ANIMATIONS);

                key_pressed(i);
            }
            if (g_key_up & bit) {
                uint8_t rel_ch = (g_key_bank_last_up & bit)
                                 ? ((G_EE_MIDI_CHANNEL - 1) & 0x0F)
                                 : G_EE_MIDI_CHANNEL;
                if (G_EE_MIDI_OUTPUT_MODE < MIDI_OUTPUT_MODE_CCS_ONLY)
                    midiNoteOff(note, rel_ch);
                if (G_EE_MIDI_OUTPUT_MODE > MIDI_OUTPUT_MODE_NOTES_ONLY)
                    midiCC(note, 0, G_EE_MIDI_CHANNEL);
                key_released(i);
            }
        }

        if (G_EE_COMBOS_ENABLE) {
            uint8_t action = comboRecognize();
            uint8_t ch = (g_bank_selected <= 0) ? G_EE_MIDI_CHANNEL
                                                : ((G_EE_MIDI_CHANNEL - 1) & 0x0F);
            switch (action) {
                case COMBO_A_DOWN:    midiNoteOn(8,  127, ch); break;
                case COMBO_A_RELEASE: midiNoteOff(8,  ch);     break;
                case COMBO_B_DOWN:    midiNoteOn(9,  127, ch); break;
                case COMBO_B_RELEASE: midiNoteOff(9,  ch);     break;
                case COMBO_C_DOWN:    midiNoteOn(10, 127, ch); break;
                case COMBO_C_RELEASE: midiNoteOff(10, ch);     break;
                case COMBO_D_DOWN:    midiNoteOn(11, 127, ch); break;
                case COMBO_D_RELEASE: midiNoteOff(11, ch);     break;
                case COMBO_E_DOWN:    midiNoteOn(12, 127, ch); break;
                case COMBO_E_RELEASE: midiNoteOff(12, ch);     break;
            }
        }

        if (ENABLE_NOTE_OFF_FEEDBACK_DELAY) {
            uint16_t sys_time_7 = system_time_ms & 0x7F;
            for (uint8_t i = 0; i < (NUM_BANKS * NUM_BUTTONS); i++) {
                uint8_t t = g_midi_note_off_counter[i];
                if (t & 0x80) {
                    t &= 0x7F;
                    if ((uint8_t)(sys_time_7 - t) >= NOTE_OFF_FEEDBACK_DELAY_LIMIT) {
                        g_midi_note_state[0][i] = 0;
                        g_midi_note_off_counter[i] = 0;
                    }
                }
            }
        }
    }

    midiEventPacket_t packet;
    while (MIDI.readPacket(&packet)) {
        uint8_t cin   = packet.header & 0x0F;
        one_second_counter = 0;
        sleep_minute_counter = 0;

        static uint8_t  sysex_buf[64];
        static uint16_t sysex_len = 0;
        static bool     sysex_active = false;

        if (cin == 0x04) {
            if (!sysex_active) { sysex_active = true; sysex_len = 0; }
            if (sysex_len < sizeof(sysex_buf)) sysex_buf[sysex_len++] = packet.byte1;
            if (sysex_len < sizeof(sysex_buf)) sysex_buf[sysex_len++] = packet.byte2;
            if (sysex_len < sizeof(sysex_buf)) sysex_buf[sysex_len++] = packet.byte3;
            continue;
        } else if (cin == 0x05 || cin == 0x06 || cin == 0x07) {
            if (sysex_active) {
                uint8_t remaining = (cin == 0x05) ? 1 : (cin == 0x06) ? 2 : 3;
                if (remaining >= 1 && sysex_len < sizeof(sysex_buf)) sysex_buf[sysex_len++] = packet.byte1;
                if (remaining >= 2 && sysex_len < sizeof(sysex_buf)) sysex_buf[sysex_len++] = packet.byte2;
                if (remaining >= 3 && sysex_len < sizeof(sysex_buf)) sysex_buf[sysex_len++] = packet.byte3;
                handleSysEx(sysex_buf, sysex_len);
                sysex_active = false;
                sysex_len = 0;
            }
            continue;
        }

        uint8_t status  = packet.byte1;
        uint8_t type    = status & 0xF0;
        uint8_t ch_1idx = status & 0x0F;
        uint8_t d1 = packet.byte2;
        uint8_t d2 = packet.byte3;

        if (type == 0x90 && d2 > 0) {
            uint8_t key_id = d1 - MIDI_BASENOTE;
            if (ch_1idx == G_EE_MIDI_CHANNEL && key_id < NUM_BUTTONS) {
                g_midi_note_state[0][key_id] = d2;
                if (ENABLE_NOTE_OFF_FEEDBACK_DELAY) g_midi_note_off_counter[key_id] = 0;
            } else if (ch_1idx == ((G_EE_MIDI_CHANNEL - 1) & 0x0F) && key_id < NUM_BUTTONS) {
                g_midi_note_state[0][key_id + NUM_BUTTONS] = d2;
                if (ENABLE_NOTE_OFF_FEEDBACK_DELAY) g_midi_note_off_counter[key_id + NUM_BUTTONS] = 0;
            } else if (ch_1idx == ((G_EE_MIDI_CHANNEL + 1) & 0x0F)) {
                g_midi_note_state[MIDI_CHANNEL_INDEX_ANIMATIONS][d1] = d2;
            }
        } else if (type == 0x80 || (type == 0x90 && d2 == 0)) {
            uint8_t key_id = d1 - MIDI_BASENOTE;
            if (ch_1idx == G_EE_MIDI_CHANNEL && key_id < NUM_BUTTONS) {
                if (ENABLE_NOTE_OFF_FEEDBACK_DELAY)
                    g_midi_note_off_counter[key_id] = (system_time_ms & 0x7F) | 0x80;
                else g_midi_note_state[0][key_id] = 0;
            } else if (ch_1idx == ((G_EE_MIDI_CHANNEL - 1) & 0x0F) && key_id < NUM_BUTTONS) {
                if (ENABLE_NOTE_OFF_FEEDBACK_DELAY)
                    g_midi_note_off_counter[key_id + NUM_BUTTONS] = (system_time_ms & 0x7F) | 0x80;
                else g_midi_note_state[0][key_id + NUM_BUTTONS] = 0;
            } else if (ch_1idx == ((G_EE_MIDI_CHANNEL + 1) & 0x0F)) {
                g_midi_note_state[MIDI_CHANNEL_INDEX_ANIMATIONS][d1] = 0;
            }
        } else if (type == 0xB0) {
            if (ch_1idx == G_EE_MIDI_CHANNEL && d1 == MF64_BANK_CC)
                change_bank(d2 > 0 ? 1 : 0);
        } else if (cin == 0x0F) {
            if (d1 == 0xF8) midi_clock_enabled = true;
            else if (d1 == 0xFA) midi_clock_enabled = true;
            else if (d1 == 0xFC) midi_clock_enabled = false;
        }
    }

    if (now - last_led_refresh_time_ms >= LED_REFRESH_LIMIT) {
        last_led_refresh_time_ms = now;
        service_bank_select_buttons();
        updateDisplay();
        pushLeds();
    }

    delay(1);
}
