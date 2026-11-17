#pragma once
#include <stdint.h>
#include <stdbool.h>
#define X77_CONFIG_SIZE 638u
#define X77_PRESETS 5u
#define X77_MACROS 4u
#define X77_STEPS 8u
#define X77_BINDINGS 274u
#define X77_LENGTHS 490u
#define X77_SEQUENCES 494u
extern __xdata uint8_t x77_config[X77_CONFIG_SIZE];
extern volatile __xdata uint8_t x77_reply[8];
extern bool x77_editing;
extern bool x77_direct_requested;
extern bool x77_transaction;
extern bool x77_saving;
extern uint8_t x77_preset;
uint16_t x77_now(void);
void x77_init(void);
void x77_task(void);
void x77_command(const uint8_t *packet);
bool x77_key(uint8_t row,uint8_t col,bool pressed);
void x77_preset_key(uint8_t index,bool pressed);
bool x77_custom_rgb(uint8_t cell,uint8_t *rgb);
void x77_overlay(uint8_t *mods,uint8_t *keys,bool nkro);
void x77_note_key(uint8_t row,uint8_t col);
void x77_take_onboard(void);
void x77_select_effect(uint8_t effect);

bool x77_windows_locked(void);
