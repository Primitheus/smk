"""Exercise the firmware state machine with a simulated flash device."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class ConfigTests(unittest.TestCase):
    def test_led_output_for_onboard_modes(self):
        compiler=shutil.which('gcc')
        if not compiler:self.skipTest('GCC is required')
        source=(ROOT/'src/keyboards/ciy-x77/indicators.c').read_text()
        function=source[source.index('bool indicators_update_step('):source.index('void indicators_post_update(')]
        harness=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define FX_OFF 4
#define LED_GROUP_ROWS 3
#define LED_GROUPS 2
#define LED_COLS 18
typedef int keyboard_state_t;
static bool host_lighting;
static bool x77_windows_locked(void) {return false;}
static struct {uint8_t led_effect;} user_settings;
static uint8_t INSCON,led_group,led_col,led_fb[18][6][3];
static unsigned loads,drives;
static void sfr_page_1(void) {INSCON=1;}
static void pca_load(uint8_t *a,uint8_t *b,uint8_t *c) {(void)a;(void)b;(void)c;loads++;}
static void led_drive(uint8_t col,uint8_t group) {(void)col;(void)group;drives++;}
'''
        checks=r'''
int main(void) {
 for(unsigned host=0;host<2;host++) for(unsigned mode=0;mode<15;mode++) {
  host_lighting=host;user_settings.led_effect=mode;loads=drives=0;
  led_col=led_group=0;INSCON=7;
  for(unsigned step=0;step<36;step++) indicators_update_step(0,0);
  unsigned expected=host || mode!=FX_OFF?36:0;
  assert(loads==expected && drives==expected && INSCON==7);
  assert(led_col==0 && led_group==0);
 }
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary);(path/'test.c').write_text(harness+function+checks)
            subprocess.run([compiler,'-std=c11','-Wall','-Wextra','-Werror',str(path/'test.c'),'-o',str(path/'test.exe')],check=True)
            subprocess.run([str(path/'test.exe')],check=True)

    def test_save_recovery_editor_and_macros(self):
        compiler=shutil.which('gcc')
        if not compiler:self.skipTest('GCC is required')
        source=(ROOT/'src/keyboards/ciy-x77/config.c').read_text()
        source=re.sub(r'^#include .*\n','',source,flags=re.M)
        source=source.replace('return *((const __code uint8_t *)address);','return flash_memory[address];')
        header=(ROOT/'src/keyboards/ciy-x77/config.h').read_text().replace('#pragma once','')
        prefix=r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#define __xdata
#define __code
#define FLASH_CODE 0
static uint8_t flash_memory[65536];
static bool programmed[65536];
volatile uint16_t systick_ticks;
uint8_t matrix[18],action_layer;
bool host_blocked;
struct {uint8_t led_effect;} user_settings;
typedef struct {uint8_t mods,keys[6];} report_keyboard_t;
typedef struct {uint8_t mods,bits[20];} report_nkro_t;
report_keyboard_t keyboard_report;
report_nkro_t nkro_report;
static bool configured=true;
void watchdog_kick(void) {}
bool usb_is_configured(void) {return configured;}
bool host_nkro_active(void) {return false;}
void kb_send_report(void *r) {(void)r;}
void kb_send_nkro(void *r) {(void)r;}
void clear_keys(void) {memset(keyboard_report.keys,0,6);}
void clear_mods(void) {keyboard_report.mods=0;}
void send_keyboard_report(void) {}
void settings_mark_dirty(void) {}
void indicators_factory_reset(void) {}
void x77_clear_locks(void) {}
bool x77_windows_locked(void) {return false;}
static const uint16_t keymaps[2][6][18]={0};
void x77_take_onboard(void) {host_blocked=true;}
void x77_select_effect(uint8_t effect) {user_settings.led_effect=effect;}
void x77_note_key(uint8_t row,uint8_t col) {(void)row;(void)col;}
void flash_erase(int window,uint16_t address) {
 assert(window==FLASH_CODE && (address==0xe000 || address==0xe400));
 memset(flash_memory+address,0,1024);
 memset(programmed+address,0,1024*sizeof(bool));
}
void flash_program_byte(int window,uint16_t address,uint8_t value) {
 assert(window==FLASH_CODE && address>=0xe000 && address<0xe800);
 assert(!programmed[address]);programmed[address]=true;
 flash_memory[address]=value;
}
'''
        checks=r'''
static void complete_save(void) {for(int i=0;i<200 && x77_saving;i++) x77_task();assert(!x77_saving && !save_failed);}
int main(void) {
 memset(flash_memory,0,sizeof(flash_memory));x77_init();assert(x77_config[0]==2);
 assert(default_colour(0,0)==12 && default_colour(1,38)==2 && default_colour(1,0)==0 && default_colour(2,88)==9 && default_colour(4,0)==12);
 x77_config[4]=0x23;save_config();complete_save();assert(bank_valid(BANK0));
 uint8_t original[65536];memcpy(original,flash_memory,sizeof(original));
 // Cut power after each possible incremental write, before the commit marker.
 for(int step=0;step<163;step++) {
  memcpy(flash_memory,original,sizeof(original));load_config();x77_saving=false;
  x77_config[4]=0x45;save_config();
  for(int i=0;i<step;i++) x77_task();
  x77_saving=false;load_config();assert(x77_config[4]==0x23);
 }
 memcpy(flash_memory,original,sizeof(original));load_config();x77_config[4]=0x45;save_config();complete_save();
 load_config();assert(x77_config[4]==0x45 && bank==BANK1);
 flash_memory[BANK1+HEADER+20]^=1;load_config();assert(x77_config[4]==0x23 && bank==BANK0);
 // Upgrade a legacy 590-byte record without losing its saved key colours.
 memcpy(flash_memory,original,sizeof(original));
 flash_memory[BANK0+3]=(uint8_t)590;flash_memory[BANK0+4]=590>>8;flash_memory[BANK0+HEADER]=1;
 uint16_t crc=0xffff;for(int i=0;i<7;i++) crc=crc_byte(crc,flash_memory[BANK0+i]);
 for(int i=0;i<590;i++) crc=crc_byte(crc,flash_memory[BANK0+HEADER+i]);
 flash_memory[BANK0+7]=(uint8_t)crc;flash_memory[BANK0+8]=crc>>8;
 assert(bank_valid(BANK0));load_config();assert(x77_config[0]==2 && x77_config[4]==0x23);
 assert(x77_config[593]==255 && x77_config[594]==255 && x77_config[595]==255);
 save_config();complete_save();load_config();assert(x77_config[4]==0x23 && x77_config[0]==2);
 x77_config[599]=18;x77_config[600]=52;x77_config[601]=86;save_config();complete_save();load_config();
 assert(x77_config[599]==18 && x77_config[600]==52 && x77_config[601]==86);
 // Key colour editing consumes typing and survives a save/reload.
 for(int i=0;i<54;i++) x77_config[4+2*54+i]=0;
 x77_preset_key(2,true);systick_ticks=4000;x77_task();assert(x77_editing);
 x77_preset_key(2,false);action_layer=0;
 assert(!x77_key(4,14,true));assert(!x77_key(4,14,false));
 assert((x77_config[4+2*54+43]&15)==1);
 action_layer=1;x77_preset_key(2,true);assert(!x77_editing);complete_save();
 load_config();assert((x77_config[4+2*54+43]&15)==1);
 // Shortcut modifiers merge without removing physical keys/modifiers.
 action_layer=0;x77_config[X77_BINDINGS+2]=6;x77_config[X77_BINDINGS+3]=1;
 assert(!x77_key(0,1,true));uint8_t mods=2,keys[6]={4,0,0,0,0,0};
 x77_overlay(&mods,keys,false);assert(mods==3 && keys[0]==4 && keys[1]==6);
 assert(!x77_key(0,1,false));mods=2;memset(keys,0,6);x77_overlay(&mods,keys,false);assert(mods==2 && keys[0]==0);
 // Finite macro playback and Escape cancellation release only synthetic state.
 x77_config[X77_BINDINGS+2]=255;x77_config[X77_BINDINGS+3]=1;
 x77_config[X77_LENGTHS]=1;x77_config[X77_SEQUENCES]=6;x77_config[X77_SEQUENCES+1]=1;
 x77_key(0,1,true);x77_task();assert(macro_key==6 && macro_mods==1);
 assert(x77_key(0,0,true));assert(macro_key==0 && macro_mods==0 && macro_slot==255);
 // Direct shortcut consumes its own release even after Fn is released.
 action_layer=1;assert(!x77_key(1,10,true));assert(x77_direct_requested && !host_blocked);
 action_layer=0;assert(!x77_key(1,10,false));assert(x77_key(1,10,false));
 uint8_t claim[8]={11,9,0,0,0,0,0,0};x77_command(claim);assert(!x77_direct_requested);
 // An all-released matrix clears stale physical and remapped modifiers.
 matrix[0]=1;x77_task();keyboard_report.mods=3;active_keys[0]=2;
 matrix[0]=0;x77_task();assert(!keyboard_report.mods && !active_keys[0]);
 // Invalid transactions cannot commit; Fn remains reserved.
 x77_config[X77_BINDINGS+99*2]=4;assert(!config_valid());load_config();
 uint8_t p[8]={11,3,0,0,0,0,0,0};matrix[0]=1;x77_command(p);assert(x77_reply[7]==1);
 matrix[0]=0;x77_command(p);assert(x77_transaction);p[1]=4;x77_command(p);assert(x77_reply[7]==2);
 p[1]=5;x77_command(p);assert(!x77_transaction);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary);(path/'test.c').write_text(prefix+header+source+checks)
            subprocess.run([compiler,'-std=c11','-Wall','-Wextra','-Werror',str(path/'test.c'),'-o',str(path/'test.exe')],check=True)
            subprocess.run([str(path/'test.exe')],check=True)

if __name__=='__main__':unittest.main()
