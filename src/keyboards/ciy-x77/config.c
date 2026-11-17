#include "config.h"
#include "kbdef.h"
#include "flash.h"
#include "watchdog.h"
#include "matrix.h"
#include "layout.h"
#include "report.h"
#include "host.h"
#include "kb.h"
#include "usb.h"

// Two 1 KiB application sectors, below the ISP marker and stock bootloader.
// The linker is limited to 0xe000 for this board.
#define BANK0 0xe000u
#define BANK1 0xe400u
#define HEADER 12u
__xdata uint8_t x77_config[X77_CONFIG_SIZE];
volatile __xdata uint8_t x77_reply[8];
bool x77_editing;
bool x77_transaction;
bool x77_saving;
uint8_t x77_preset=0xff;
static __xdata uint8_t active_keys[14];
static __xdata uint8_t header[HEADER];
static uint16_t bank,sequence,write_offset,write_bank,write_crc,transaction_offset;
static uint16_t hold_started,macro_due,transaction_started,save_indicator_until;
static uint8_t save_indicator;
static uint8_t held_preset=0xff,macro_slot=0xff,macro_step,macro_phase,macro_key,macro_mods;
static bool save_failed,hold_consumed,direct_key_down,keys_released;
bool x77_direct_requested;
extern void indicators_factory_reset(void);
extern void x77_clear_locks(void);
extern volatile uint16_t systick_ticks;
extern uint8_t matrix[18];
extern uint8_t action_layer;
extern bool host_blocked;
#include "settings.h"

uint16_t x77_now(void)
{
    uint16_t a,b;do {a=systick_ticks;b=systick_ticks;} while(a!=b);return a;
}
static uint16_t crc_byte(uint16_t crc,uint8_t value)
{
    crc^=(uint16_t)value<<8;
    for(uint8_t i=0;i<8;i++) crc=(crc&0x8000u)?(uint16_t)((crc<<1)^0x1021u):(uint16_t)(crc<<1);
    return crc;
}
static uint8_t read_byte(uint16_t address)
{
    return *((const __code uint8_t *)address);
}
static bool bank_valid(uint16_t address)
{
    if(read_byte(address)!=0x53 || read_byte(address+1)!=0x43 || read_byte(address+2)!=1 || read_byte(address+11)!=0xa5) return false;
    uint16_t length=(uint16_t)read_byte(address+3)|((uint16_t)read_byte(address+4)<<8);
    if(length!=590u && length!=X77_CONFIG_SIZE) return false;
    uint16_t crc=0xffff;
    for(uint8_t i=0;i<7;i++) crc=crc_byte(crc,read_byte(address+i));
    for(uint16_t i=0;i<length;i++) crc=crc_byte(crc,read_byte(address+HEADER+i));
    return read_byte(address+7)==(uint8_t)crc && read_byte(address+8)==(uint8_t)(crc>>8);
}
static bool config_valid(void)
{
    if(x77_config[0]!=2 || x77_config[1]>4 || x77_config[2] || x77_config[3]) return false;
    for(uint8_t cell=0;cell<108;cell++) {
        uint16_t at=X77_BINDINGS+(uint16_t)cell*2;
        uint8_t key=x77_config[at],mods=x77_config[at+1];
        if(cell==99 && (key||mods)) return false; // Fn cannot be remapped.
        if(key==0xff) {if(mods<1 || mods>X77_MACROS) return false;}
        else if(key!=0xfe && key!=0 && (key<4 || key>=160)) return false;
    }
    for(uint8_t slot=0;slot<X77_MACROS;slot++) {
        if(x77_config[X77_LENGTHS+slot]>X77_STEPS) return false;
        for(uint8_t step=0;step<X77_STEPS;step++) {
            uint8_t key=x77_config[X77_SEQUENCES+(uint16_t)slot*24+step*3];
            if(key && (key<4 || key>=160)) return false;
        }
    }
    if(x77_config[590] || x77_config[591] || x77_config[592]) return false;
    return true;
}
static uint8_t default_colour(uint8_t preset,uint8_t cell)
{
    uint8_t row=cell/18,col=cell%18;
    if(row==4) {if(col==14) col=1;else if(col>=1 && col<=10) col++;}
    bool arrows=cell==88 || cell>=105;
    if(preset==0) return row==5?10:row==2 || row==3?1:row==1?9:12;
    if(preset==1) return cell==38 || cell==55 || cell==56 || cell==57 || arrows?2:row==1 && col>=1 && col<=5?3:row==5?11:0;
    if(preset==2) return arrows?9:row==0?13:row==5?12:11;
    if(preset==3) {static const __code uint8_t colours[7]={12,13,14,2,3,4,5};return colours[(uint16_t)col*7/18];}
    {static const __code uint8_t colours[6]={12,10,9,8,7,12};return colours[row];}
}
static const __code uint8_t default_palette[48]={
    0,0,0,255,255,255,255,0,0,255,64,0,255,160,0,255,255,0,128,255,0,0,255,0,
    0,255,128,0,255,255,0,128,255,0,0,255,128,0,255,255,0,255,255,0,128,255,128,192};
static void load_palette(void) {for(uint8_t i=0;i<48;i++) x77_config[590u+i]=default_palette[i];}
static void load_config(void)
{
    bool first=bank_valid(BANK0),second=bank_valid(BANK1);
    uint16_t a=(uint16_t)read_byte(BANK0+5)|((uint16_t)read_byte(BANK0+6)<<8);
    uint16_t b=(uint16_t)read_byte(BANK1+5)|((uint16_t)read_byte(BANK1+6)<<8);
    bank=0;sequence=0;
    if(first && (!second || (int16_t)(a-b)>=0)) {bank=BANK0;sequence=a;}
    else if(second) {bank=BANK1;sequence=b;}
    for(uint16_t i=0;i<X77_CONFIG_SIZE;i++) x77_config[i]=bank?read_byte(bank+HEADER+i):0;
    if(bank && read_byte(bank+3)==(uint8_t)590u && x77_config[0]==1) {x77_config[0]=2;load_palette();}
    if(!bank || !config_valid()) {
        for(uint16_t i=0;i<X77_CONFIG_SIZE;i++) x77_config[i]=0;
        x77_config[0]=2;load_palette();
        for(uint8_t preset=0;preset<5;preset++) for(uint8_t cell=0;cell<108;cell+=2)
            x77_config[4u+(uint16_t)preset*54+cell/2]=default_colour(preset,cell)|(default_colour(preset,cell+1)<<4);
    }
}
static void save_config(void)
{
    if(x77_saving) return;
    if(bank) {
        bool same=true;
        for(uint16_t i=0;i<X77_CONFIG_SIZE;i++) if(read_byte(bank+HEADER+i)!=x77_config[i]) {same=false;break;}
        if(same) {save_failed=false;save_indicator=1;save_indicator_until=x77_now()+3700;return;}
    }
    write_bank=bank==BANK0?BANK1:BANK0;write_offset=0;save_failed=false;
    for(uint8_t i=0;i<HEADER;i++) header[i]=0xff;
    header[0]=0x53;header[1]=0x43;header[2]=1;
    header[3]=(uint8_t)X77_CONFIG_SIZE;header[4]=X77_CONFIG_SIZE>>8;
    header[5]=(uint8_t)(sequence+1);header[6]=(uint8_t)((sequence+1)>>8);
    write_crc=0xffff;
    for(uint8_t i=0;i<7;i++) write_crc=crc_byte(write_crc,header[i]);
    for(uint16_t i=0;i<X77_CONFIG_SIZE;i++) write_crc=crc_byte(write_crc,x77_config[i]);
    header[7]=(uint8_t)write_crc;header[8]=write_crc>>8;
    x77_saving=true;
}
static void emit_overlay(void)
{
    if(host_nkro_active()) kb_send_nkro(&nkro_report);
    else kb_send_report(&keyboard_report);
}
static void cancel_macro(void)
{
    macro_slot=0xff;macro_key=macro_mods=0;emit_overlay();
}
void x77_init(void) {load_config();}

void x77_task(void)
{
    uint16_t now=x77_now();
    bool down=false;for(uint8_t i=0;i<18;i++) if(matrix[i]&0x3f) down=true;
    if(down) keys_released=false;
    else if(!keys_released) {
        keys_released=true;clear_keys();clear_mods();
        for(uint8_t i=0;i<14;i++) active_keys[i]=0;
        send_keyboard_report();emit_overlay();
    }
    if(save_indicator && (int16_t)(now-save_indicator_until)>=0) save_indicator=0;
    if(x77_transaction && (uint16_t)(now-transaction_started)>=37000u) {load_config();x77_transaction=false;}
    if(held_preset!=0xff && !hold_consumed && (uint16_t)(now-hold_started)>=3700u && !x77_saving && !x77_transaction) {
        x77_editing=true;hold_consumed=true;
        clear_keys();clear_mods();
        for(uint8_t i=0;i<14;i++) active_keys[i]=0;
        cancel_macro();send_keyboard_report();
    }
    if(x77_saving) {
        if(write_offset==0) flash_erase(FLASH_CODE,write_bank);
        for(uint8_t n=0;n<4 && write_offset<HEADER+X77_CONFIG_SIZE;n++,write_offset++) {
            // SSP bytes must be programmed only once between erases. Leave the
            // commit byte untouched until the rest of the record is verified.
            if(write_offset==11) continue;
            uint8_t value=write_offset<HEADER?header[write_offset]:x77_config[write_offset-HEADER];
            flash_program_byte(FLASH_CODE,write_bank+write_offset,value);
        }
        watchdog_kick();
        if(write_offset==HEADER+X77_CONFIG_SIZE) {
            bool good=true;
            for(uint16_t i=0;i<X77_CONFIG_SIZE;i++) if(read_byte(write_bank+HEADER+i)!=x77_config[i]) good=false;
            for(uint8_t i=0;i<11;i++) if(read_byte(write_bank+i)!=header[i]) good=false;
            if(good) flash_program_byte(FLASH_CODE,write_bank+11,0xa5);
            if(good && bank_valid(write_bank)) {bank=write_bank;sequence++;}
            else save_failed=true;
            save_indicator=save_failed?2:1;save_indicator_until=now+3700;
            x77_saving=false;
        }
    }
    if(macro_slot!=0xff && (int16_t)(now-macro_due)>=0) {
        uint16_t at=X77_SEQUENCES+(uint16_t)macro_slot*24+macro_step*3;
        if(!macro_phase) {
            if(macro_step>=x77_config[X77_LENGTHS+macro_slot]) {cancel_macro();return;}
            macro_key=x77_config[at];macro_mods=x77_config[at+1];macro_phase=1;macro_due=now+148;
        } else {
            macro_key=macro_mods=0;macro_phase=0;macro_step++;
            macro_due=now+(uint16_t)(x77_config[at+2]?x77_config[at+2]:1)*37u;
        }
        emit_overlay();
    }
    if(!usb_is_configured() && macro_slot!=0xff) cancel_macro();
}

void x77_preset_key(uint8_t index,bool pressed)
{
    if(x77_saving || x77_transaction) return;
    if(pressed) {
        if(x77_editing) {
            if(index==x77_preset) {x77_editing=false;save_config();}
            return;
        }
        x77_take_onboard();x77_preset=index;x77_config[1]=index;
        user_settings.led_effect=10+index;settings_mark_dirty();
        held_preset=index;hold_started=x77_now();hold_consumed=false;
    } else if(held_preset==index) held_preset=0xff;
}

bool x77_key(uint8_t row,uint8_t col,bool pressed)
{
    uint8_t cell=row*18+col,mask=1u<<(cell&7),at=cell>>3;
    if(pressed) x77_note_key(row,col);
    // Fn+0 is always available outside the colour editor.
    if(row==1 && col==10 && !pressed && direct_key_down) {direct_key_down=false;return false;}
    if(pressed && action_layer && row==1 && col==10 && !x77_editing && !x77_transaction && !x77_saving) {
        host_blocked=false;x77_direct_requested=true;direct_key_down=true;
        return false;
    }
    if(pressed && row==0 && col==0 && macro_slot!=0xff) {cancel_macro();return true;}
    if(cell==99) {if(!pressed) held_preset=0xff;return true;}
    if(x77_editing) {
        // Let Fn+CM reach its save handler, but consume normal typing in the editor.
        if(action_layer && row==1 && col>=1 && col<=5) return true;
        if(pressed && !x77_saving) {
            uint16_t address=4u+(uint16_t)x77_preset*54+cell/2;
            uint8_t shift=(cell&1)*4,value=(x77_config[address]>>shift)&15;
            value=(value+1)&15;
            x77_config[address]=(x77_config[address]&~(15u<<shift))|(value<<shift);
        }
        return false;
    }
    if(!pressed && (active_keys[at]&mask)) {active_keys[at]&=~mask;emit_overlay();return false;}
    if(x77_transaction || x77_saving || action_layer || !pressed) return true;
    uint16_t binding=X77_BINDINGS+(uint16_t)cell*2;
    uint8_t key=x77_config[binding],mods=x77_config[binding+1];
    if(!key && !mods) return true;
    active_keys[at]|=mask;
    if(key==0xff) {
        cancel_macro();macro_slot=mods-1;macro_step=macro_phase=0;macro_due=x77_now();
    }
    emit_overlay();return false;
}

static void overlay_key(uint8_t key,uint8_t *keys,bool nkro)
{
    if(key<4 || key>=160) return;
    if(nkro) keys[key>>3]|=1u<<(key&7);
    else {
        for(uint8_t i=0;i<6;i++) if(keys[i]==key) return;
        for(uint8_t i=0;i<6;i++) if(!keys[i]) {keys[i]=key;return;}
    }
}
void x77_overlay(uint8_t *mods,uint8_t *keys,bool nkro)
{
    *mods|=macro_mods;overlay_key(macro_key,keys,nkro);
    for(uint8_t cell=0;cell<108;cell++) if(active_keys[cell>>3]&(1u<<(cell&7))) {
        uint16_t at=X77_BINDINGS+(uint16_t)cell*2;
        uint8_t key=x77_config[at];
        if(key<160) {*mods|=x77_config[at+1];overlay_key(key,keys,nkro);}
    }
}

bool x77_custom_rgb(uint8_t cell,uint8_t *rgb)
{
    if(x77_preset>=5) return false;
    uint8_t value=(x77_config[4u+(uint16_t)x77_preset*54+cell/2]>>((cell&1)*4))&15;
    for(uint8_t i=0;i<3;i++) rgb[i]=x77_config[590u+(uint16_t)value*3+i];
    if(cell==1u*18+x77_preset+1) {
        uint16_t now=x77_now();
        if(x77_editing && (now&1024u)) for(uint8_t i=0;i<3;i++) rgb[i]=128;
        else if(x77_saving) {rgb[0]=255;rgb[1]=128;rgb[2]=0;}
        else if(save_indicator && (int16_t)(now-save_indicator_until)<0) {
            rgb[0]=save_indicator==2?255:0;rgb[1]=save_indicator==1?255:0;rgb[2]=0;
        }
    }
    return true;
}

void x77_command(const uint8_t *p)
{
    uint8_t command=p[1]&15,status=0;
    if(command==2 || command==3) transaction_started=x77_now();
    uint16_t offset=(uint16_t)p[2]|((uint16_t)p[3]<<8);
    uint8_t data[3]={0,0,0};
    if(command==6) {data[0]=x77_saving;data[1]=user_settings.led_effect|(x77_editing?128:0);data[2]=save_failed | (host_blocked?2:0) | (x77_direct_requested?4:0) | (x77_windows_locked()?8:0);}
    else if(x77_saving || x77_editing) status=1;
    else if(command==1) {
        if(offset>=X77_CONFIG_SIZE) status=2;
        else for(uint8_t i=0;i<3 && offset+i<X77_CONFIG_SIZE;i++) data[i]=x77_config[offset+i];
    } else if(command==3) {
        bool held=false;for(uint8_t i=0;i<18;i++) if(matrix[i]&0x3f) held=true;
        if(held || x77_transaction) status=1;
        else {x77_transaction=true;transaction_offset=0;cancel_macro();}
    } else if(command==2) {
        if(!x77_transaction || offset!=transaction_offset || offset>=X77_CONFIG_SIZE) status=2;
        else for(uint8_t i=0;i<3 && transaction_offset<X77_CONFIG_SIZE;i++) x77_config[transaction_offset++]=p[4+i];
    } else if(command==4) {
        if(!x77_transaction || transaction_offset!=X77_CONFIG_SIZE || !config_valid()) status=2;
        else {x77_transaction=false;save_config();}
    } else if(command==5) {load_config();x77_transaction=false;}
    else if(command==7 && !x77_transaction && p[4]<10) x77_select_effect(p[4]);
    else if(command==8 && !x77_transaction && p[4]<5) {x77_preset_key(p[4],true);x77_preset_key(p[4],false);}
    else if(command==9 && !x77_transaction) {host_blocked=false;x77_direct_requested=false;}
    else if(command==10 && offset<108) {
        uint16_t code=keymaps[1][offset/18][offset%18];
        data[0]=(uint8_t)code;data[1]=code>>8;
        if(offset==28) {data[0]=0xfe;data[1]=0xff;} // Fn+0: Direct
    }
    else if(command==12 && !x77_transaction) {indicators_factory_reset();if(p[4]==1) x77_clear_locks();}
    else status=2;
    x77_reply[0]=11;x77_reply[2]=p[2];x77_reply[3]=p[3];
    for(uint8_t i=0;i<3;i++) x77_reply[4+i]=data[i];
    x77_reply[7]=status;x77_reply[1]=p[1]; // publish tag last
}
