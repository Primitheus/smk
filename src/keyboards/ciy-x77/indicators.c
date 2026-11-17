#include "indicators.h"
#include "sh68f89.h"
#include "kbdef.h"
#include "settings.h"
#include "keyboard.h"
#include "led_effect.h"
#include "pca.h"
#include "stream_protocol.h"
#include "fast_protocol.h"
#include "lighting_layout.h"
#include "config.h"

const __code uint8_t x77_layout_id=X77_LAYOUT_ID;

#define LED_ROWS MATRIX_ROWS
#define LED_COLS MATRIX_COLS

// rows are lit three at a time, so a column takes two subframes
#define LED_GROUP_ROWS 3
#define LED_GROUPS     2

// animation step per frame, indexed by user_settings.led_speed.
static const __code uint8_t led_speeds[] = {1, 2, 4, 8};
#define LED_SPEED_LEVELS (sizeof(led_speeds))

// duty is a real PWM level here, so brightness scales the colour rather than dithering frames.
static const __code uint8_t led_brightness_scale[LED_BRIGHTNESS_LEVELS] = {32, 80, 160, 255};

#include LED_GEOMETRY_HEADER
_Static_assert(LED_GEOMETRY_ROWS == LED_ROWS && LED_GEOMETRY_COLS == LED_COLS, "generated LED geometry size does not match the key matrix");

// one byte per colour per key, in the order the PCA channels want them: blue, green, red.
static __xdata uint8_t led_fb[LED_COLS][LED_ROWS][3];

static uint8_t led_col;
static uint8_t led_group;
static uint8_t led_phase;
static uint8_t regen_row;
static uint8_t regen_col;
static uint8_t host_rgb[3];
static volatile bool host_lighting;
bool host_blocked;
static __xdata uint8_t key_age[108];
static uint16_t effects_tick;
static uint8_t ripple_row[4],ripple_col[4],ripple_age[4],ripple_next;
static bool host_per_key;
static __xdata uint8_t host_pixels[2][LED_ROWS * LED_COLS][3];
static stream_frame_state host_frame;
static fast_frame_state fast_frame;
static uint16_t host_last_tick;
extern volatile uint16_t systick_ticks;
extern bool usb_stream_poll(uint8_t *packet);
extern bool usb_fast_poll(uint8_t *packet);

static uint16_t stream_now(void)
{
    uint16_t first, second;
    do { first = systick_ticks; second = systick_ticks; } while (first != second);
    return first;
}

static void fast_task(void)
{
    static __xdata uint8_t packet[64];
    if (!usb_fast_poll(packet)) return;
    if(host_blocked || x77_editing || x77_transaction) return;
    uint8_t result = fast_accept(&fast_frame, packet);
    if (result == 3) {
        host_lighting = false;
        host_frame.receiving = false;
    } else if (result == 1 || result == 2) {
        uint8_t cell = packet[4] * 19;
        uint8_t back = host_frame.front ^ 1;
        uint8_t offset = 7;
        host_frame.receiving = false;
        for (uint8_t i = 0; i < packet[5]; i++) {
            for (uint8_t channel = 0; channel < 3; channel++) {
                host_pixels[back][cell + i][channel] = packet[offset++];
            }
        }
        if (result == 2) {
            host_frame.front = back;
            host_per_key = true;
            host_lighting = true;
            host_last_tick = stream_now();
        }
    }
}

static void stream_task(void)
{
    fast_task();
    uint8_t packet[8];
    uint16_t now = stream_now();
    bool got=usb_stream_poll(packet);
    if(got && packet[0]==11) {x77_command(packet);return;}
    if (got && !host_blocked && !x77_editing && !x77_transaction && stream_valid(packet)) {
        fast_frame.receiving = false;
        if (packet[0] == 9) {
            if (stream_pair_accept(&host_frame, packet[1])) {
                uint8_t cell = packet[1] * 2;
                uint8_t back = host_frame.front ^ 1;
                for (uint8_t i = 0; i < 3; i++) {
                    host_pixels[back][cell][i] = packet[2 + i];
                    host_pixels[back][cell + 1][i] = packet[5 + i];
                }
            }
        } else if (packet[4] == 2) {
            host_frame.next_pair = 0;
            host_frame.receiving = true;
        } else if (packet[4] == 3) {
            if (stream_commit(&host_frame)) {
                host_per_key = true;
                host_lighting = true;
                host_last_tick = now;
            }
        } else {
            host_frame.receiving = false;
            host_per_key = false;
            host_lighting = packet[4] == 1;
            host_rgb[0] = packet[5];
            host_rgb[1] = packet[6];
            host_rgb[2] = packet[7];
            host_last_tick = now;
        }
    }
    // Nominally about two seconds; no write to EEPROM on streamed updates.
    if (host_lighting && (uint16_t)(now - host_last_tick) >= 7400u) host_lighting = false;
}


// the nine compare channels, in the order the per-column table lists them: three rows of
// blue, green, red. Group 0 drives rows 0-2 and group 1 rows 3-5.
#define PCA_P3_PINS (uint8_t)(_P3_3 | _P3_4 | _P3_5 | _P3_7)
#define PCA_P4_PINS (uint8_t)(_P4_0 | _P4_2 | _P4_3)
#define PCA_P6_PINS (uint8_t)(_P6_0 | _P6_1)
#define LED_ENABLES (uint8_t)(_P4_4 | _P4_5)

// P7.0 gates the backlight supply and is active low; left high nothing lights at all.
#define LED_SUPPLY_P7_0 _P7_0

// the nine channels in the order the hardware lays them out: three consecutive rows, each
// blue, green then red.
static void pca_load(const __xdata uint8_t *bgr0, const __xdata uint8_t *bgr1, const __xdata uint8_t *bgr2)
{
    pca_hold();

    P1CPH0 = 0;
    P1CPL0 = bgr0[0];
    P1CPH1 = 0;
    P1CPL1 = bgr0[1];
    P1CPH2 = 0;
    P1CPL2 = bgr0[2];
    P2CPH0 = 0;
    P2CPL0 = bgr1[0];
    P2CPH1 = 0;
    P2CPL1 = bgr1[1];
    P3CPH0 = 0;
    P3CPL0 = bgr1[2];
    P3CPH1 = 0;
    P3CPL1 = bgr2[0];
    P0CPH1 = 0;
    P0CPL1 = bgr2[1];
    P0CPH0 = 0;
    P0CPL0 = bgr2[2];

    pca_release();
}

void indicators_init(void)
{
    sfr_page_0();
    P3CR |= PCA_P3_PINS;
    P3 = KB_P3_IDLE;
    P4CR |= (uint8_t)(PCA_P4_PINS | LED_ENABLES);
    kb_p4_shadow = KB_P4_IDLE;
    P4           = kb_p4_shadow;

    uint8_t saved_page = INSCON;
    sfr_page_1();

    P6CR |= PCA_P6_PINS;
    P6 &= (uint8_t)~PCA_P6_PINS;
    P7CR |= LED_SUPPLY_P7_0;
    P7 &= (uint8_t)~LED_SUPPLY_P7_0;

    pca_init();

    INSCON = saved_page;
}

void indicators_start(void)
{
    led_col   = 0;
    led_group = 0;
    led_phase = 0;
    regen_row = 0;
    regen_col = 0;

    user_settings.led_effect = FX_RADIAL;
    settings_load();
    if (user_settings.led_effect > 14) {
        user_settings.led_effect = FX_RADIAL;
    }
    if(user_settings.led_effect>=10) x77_preset=user_settings.led_effect-10;
    for(uint8_t i=0;i<108;i++) key_age[i]=255;
    for(uint8_t i=0;i<4;i++) ripple_age[i]=255;
    if (user_settings.led_brightness >= LED_BRIGHTNESS_LEVELS) {
        user_settings.led_brightness = LED_BRIGHTNESS_LEVELS - 1;
    }
    if (user_settings.led_speed >= LED_SPEED_LEVELS) {
        user_settings.led_speed = 2;
    }
}

void indicators_next_effect(void)
{
    x77_take_onboard();x77_preset=0xff;
    uint8_t fx=user_settings.led_effect;
    user_settings.led_effect=fx>=10 || fx==4?0:fx==9?4:fx==3?5:fx+1;
    settings_save();
}

void indicators_set_effect(uint8_t fx)
{
    if (fx > 9) {
        return;
    }
    x77_take_onboard();x77_preset=0xff;
    user_settings.led_effect = fx;
    settings_save();
}

void indicators_step_brightness(bool up)
{
    if (up) {
        if (user_settings.led_brightness + 1u >= LED_BRIGHTNESS_LEVELS) {
            return;
        }
        user_settings.led_brightness++;
    } else {
        if (user_settings.led_brightness == 0) {
            return;
        }
        user_settings.led_brightness--;
    }
    settings_save();
}

void indicators_step_speed(bool up)
{
    if (up) {
        if (user_settings.led_speed + 1u >= LED_SPEED_LEVELS) {
            return;
        }
        user_settings.led_speed++;
    } else {
        if (user_settings.led_speed == 0) {
            return;
        }
        user_settings.led_speed--;
    }
    settings_save();
}

// eight steps round the wheel
#define LED_COLOR_STEP 32u

void indicators_step_color(bool forward)
{
    user_settings.led_color = (uint8_t)(user_settings.led_color + (forward ? LED_COLOR_STEP : (uint8_t)-LED_COLOR_STEP));
    settings_save();
}

void indicators_factory_reset(void)
{
    x77_take_onboard();x77_preset=0xff;
    user_settings.led_effect     = FX_RADIAL;
    user_settings.led_brightness = LED_BRIGHTNESS_LEVELS - 1;
    user_settings.led_speed      = 2;
    user_settings.led_color      = 0;
    settings_save();
}

static void led_scale_vivid(uint8_t bright, uint8_t out[3])
{
    uint8_t peak = out[0];
    if (out[1] > peak) {
        peak = out[1];
    }
    if (out[2] > peak) {
        peak = out[2];
    }
    if (peak == 0) {
        return;
    }
    for (uint8_t i = 0; i < 3; i++) {
        out[i] = (uint8_t)(((uint16_t)out[i] * bright) / peak);
    }
}

// as led_effect_rgb, but with the board's colour offset folded into the wheel index so the
// colour keys shift the whole animation round the wheel. FX_SOLID stays white.
static bool effect_rgb(uint8_t row, uint8_t col, uint8_t bright, uint8_t out[3])
{
    if(row==5 && col==1 && x77_windows_locked()) {out[0]=255;out[1]=out[2]=0;return true;}
    if (host_lighting) {
        if (host_per_key) {
            uint8_t cell = row * LED_COLS + col;
            for (uint8_t i = 0; i < 3; i++) out[i] = host_pixels[host_frame.front][cell][i];
        } else {
            out[0] = host_rgb[0]; out[1] = host_rgb[1]; out[2] = host_rgb[2];
        }
        return true;
    }
    uint8_t cell=row*18+col;
    if(x77_custom_rgb(cell,out)) {
        for(uint8_t i=0;i<3;i++) out[i]=(uint16_t)out[i]*bright/255u;
        return true;
    }
    const led_effect_t fx = (led_effect_t)user_settings.led_effect;
    if((uint8_t)fx>=5) {
        uint8_t level=255,index=user_settings.led_color;
        uint8_t physical=lighting_column(row,col);
        if((uint8_t)fx==5) level=led_phase<128?led_phase*2:(255-led_phase)*2;
        else if((uint8_t)fx==6) index+=led_phase;
        else if((uint8_t)fx==7) level=key_age[cell]>=85?0:255-key_age[cell]*3;
        else if((uint8_t)fx==8) {
            level=0;
            for(uint8_t n=0;n<4;n++) if(ripple_age[n]<85) {
                uint8_t dx=physical>ripple_col[n]?physical-ripple_col[n]:ripple_col[n]-physical;
                uint8_t dy=row>ripple_row[n]?row-ripple_row[n]:ripple_row[n]-row;
                int16_t delta=(int16_t)(dx+dy)*4-ripple_age[n];if(delta<0) delta=-delta;
                uint8_t strength=delta<8?(uint16_t)(8-delta)*(255-ripple_age[n]*3)/8:0;
                if(strength>level) level=strength;
            }
        } else if((uint8_t)fx==9) {
            uint8_t position=(led_phase/7)%34;if(position>17) position=34-position;
            uint8_t distance=position>physical?position-physical:physical-position;
            level=distance>3?0:255-distance*64;
        }
        led_color_wheel(index,out);
        for(uint8_t i=0;i<3;i++) out[i]=(uint16_t)out[i]*level/255u;
        for(uint8_t i=0;i<3;i++) out[i]=(uint16_t)out[i]*bright/255u;
        return true;
    }

    if (fx >= FX_OFF) {
        out[0]=out[1]=out[2]=0;
        return true;
    }
    if (fx == FX_SOLID) {
        out[0] = out[1] = out[2] = bright;
        return true;
    }

    led_color_wheel((uint8_t)(led_effect_index(fx, row, lighting_column(row, col), led_phase) + user_settings.led_color), out);
    led_scale_vivid(bright, out);
    return true;
}

// Re-evaluating the effect is by far the most expensive thing here, so it runs from the
// main loop rather than the subframe interrupt: at a 0.25 ms subframe it does not fit in
// the interrupt, and overrunning it starves USB until the device stops enumerating.
static void led_regen_one(void)
{
    uint8_t rgb[3];
    if (effect_rgb(regen_row, regen_col, led_brightness_scale[user_settings.led_brightness], rgb)) {
        led_fb[regen_col][regen_row][0] = rgb[2]; // blue
        led_fb[regen_col][regen_row][1] = rgb[1]; // green
        led_fb[regen_col][regen_row][2] = rgb[0]; // red
    }

    if (++regen_col >= LED_COLS) {
        regen_col = 0;
        if (++regen_row >= LED_ROWS) {
            regen_row = 0;
            led_phase = (uint8_t)(led_phase + led_speeds[user_settings.led_speed]);
        }
    }
}

void indicators_pwm_disable(void)
{
    sfr_page_0();
    kb_p4_shadow &= (uint8_t)~LED_ENABLES;
    P4 = kb_p4_shadow;
}

// the P4 bits this owns; the rest of the port, notably the EEPROM write protect, is left
// exactly as the shadow has it. Rebuilding the whole byte here re-asserted write protect
// from under a settings save and left a half-written record behind.
#define LED_P4_OWNED (uint8_t)(PCA_P4_PINS | LED_ENABLES | KB_C_P4_MASK)

static void led_drive(uint8_t col, uint8_t group)
{
    uint8_t mask = kb_col_masks[col];
    uint8_t p4   = (uint8_t)((kb_p4_shadow & ~LED_P4_OWNED) | KB_C_P4_MASK | (group ? _P4_5 : _P4_4));

    sfr_page_0();
    P0 = (col < KB_C_P1_FIRST) ? (uint8_t)~mask : 0xFF;
    P1 = (col >= KB_C_P1_FIRST && col < KB_C_P4_FIRST) ? (uint8_t)~mask : 0xFF;
    if (col >= KB_C_P4_FIRST) {
        p4 &= (uint8_t)~mask;
    }

    kb_p4_shadow = p4;
    P4           = p4;
}

// deliberately not restoring the drive here: the next subframe tick is a quarter of a
// millisecond away and lights the *next* cell, whereas relighting the last one would give
// that single cell the whole scan slot on top of its own subframe and leave it visibly
// brighter than its neighbours.
void indicators_pwm_enable(void) {}

void indicators_pre_update(void)
{
    indicators_pwm_disable();
}

bool indicators_update_step(keyboard_state_t *keyboard, uint8_t current_step)
{
    (void)current_step;
    (void)keyboard;

    if (!host_lighting && user_settings.led_effect == FX_OFF && !x77_windows_locked()) {
        return false;
    }

    const uint8_t base = (uint8_t)(led_group * LED_GROUP_ROWS);

    uint8_t saved_page = INSCON;
    sfr_page_1();
    pca_load(led_fb[led_col][base], led_fb[led_col][base + 1], led_fb[led_col][base + 2]);
    INSCON = saved_page;

    // one column low, then the group enable, so the drive settles before anything lights
    led_drive(led_col, led_group);

    bool wrapped = false;
    if (++led_group >= LED_GROUPS) {
        led_group = 0;
        if (++led_col >= LED_COLS) {
            led_col = 0;
            wrapped = true;
        }
    }

    return wrapped;
}

void indicators_post_update(void) {}

// HID keyboard output report bits, in the order the boot protocol defines them.
#define USB_LED_NUM_LOCK    (1u << 0)
#define USB_LED_CAPS_LOCK   (1u << 1)
#define USB_LED_SCROLL_LOCK (1u << 2)

// the lock LEDs are plain GPIO on P3 and sink, so they light on a low. P3 also carries four
// PCA channels, so it is never read back: a read samples live PWM on those pins and writing
// the value back latches the channel on.
void indicators_render(void)
{
    stream_task();
    uint16_t now=x77_now();
    if((uint16_t)(now-effects_tick)>=296u/led_speeds[user_settings.led_speed]) {
        effects_tick=now;
        for(uint8_t i=0;i<108;i++) if(key_age[i]<255) key_age[i]++;
        for(uint8_t i=0;i<4;i++) if(ripple_age[i]<255) ripple_age[i]++;
    }
    led_regen_one();

    uint8_t state = keyboard_state.led_state;
    uint8_t low   = 0;

    if (state & USB_LED_NUM_LOCK) {
        low |= LED_NUM_P3_0;
    }
    if (state & USB_LED_CAPS_LOCK) {
        low |= LED_CAPS_P3_1;
    }
    if (state & USB_LED_SCROLL_LOCK) {
        low |= LED_SCROLL_P3_2;
    }

    uint8_t saved_page = INSCON;
    sfr_page_0();
    P3     = (uint8_t)(KB_P3_IDLE & ~low);
    INSCON = saved_page;
}

void x77_note_key(uint8_t row,uint8_t col)
{
    key_age[row*18+col]=0;
    ripple_row[ripple_next]=row;ripple_col[ripple_next]=lighting_column(row,col);ripple_age[ripple_next]=0;
    ripple_next=(ripple_next+1)&3;
}
void x77_take_onboard(void) {host_lighting=false;host_blocked=true;x77_direct_requested=false;}
void x77_select_effect(uint8_t effect) {indicators_set_effect(effect);}
