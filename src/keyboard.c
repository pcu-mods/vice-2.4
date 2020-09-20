/*
 * keyboard.c - Common keyboard emulation.
 *
 * Written by
 *  Andreas Boose <viceteam@t-online.de>
 *
 * Based on old code by
 *  Ettore Perazzoli <ettore@comm2000.it>
 *  Jouko Valta <jopi@stekt.oulu.fi>
 *  Andr� Fachat <fachat@physik.tu-chemnitz.de>
 *  Bernhard Kuhn <kuhn@eikon.e-technik.tu-muenchen.de>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "vice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef RAND_MAX
#include <limits.h>
#define RAND_MAX INT_MAX
#endif

#include "alarm.h"
#include "archdep.h"
#include "joystick.h"
#include "joy.h"
#include "kbd.h"
#include "keyboard.h"
#include "lib.h"
#include "log.h"
#include "machine.h"
#include "maincpu.h"
#include "network.h"
#include "resources.h"
#include "snapshot.h"
#include "sysfile.h"
#include "types.h"
#include "util.h"
#include "vice-event.h"
#include "sid/sid.h"


/* #define KEYBOARD_RAND() (rand() % machine_get_cycles_per_frame()) */
#define KEYBOARD_RAND() (1 + (int)(((float)machine_get_cycles_per_frame()) * rand() / (RAND_MAX + 1.0)))

/* Keyboard array.  */
int keyarr[KBD_ROWS];
int rev_keyarr[KBD_COLS];

/* Shift lock state.  */
int keyboard_shiftlock = 0;

/* Keyboard status to be latched into the keyboard array.  */
static int latch_keyarr[KBD_ROWS];
static int latch_rev_keyarr[KBD_COLS];

static int network_keyarr[KBD_ROWS];
static int network_rev_keyarr[KBD_COLS];

static alarm_t *keyboard_alarm;

static log_t keyboard_log = LOG_DEFAULT;

static keyboard_machine_func_t keyboard_machine_func = NULL;

static CLOCK keyboard_delay;

static int keyboard_clear = 0;

static alarm_t *restore_alarm = NULL; /* restore key alarm context */

static void keyboard_latch_matrix(CLOCK offset)
{
    if (network_connected()) {
        memcpy(keyarr, network_keyarr, sizeof(keyarr));
        memcpy(rev_keyarr, network_rev_keyarr, sizeof(rev_keyarr));
    } else {
        memcpy(keyarr, latch_keyarr, sizeof(keyarr));
        memcpy(rev_keyarr, latch_rev_keyarr, sizeof(rev_keyarr));
    }
    if (keyboard_machine_func != NULL) {
        keyboard_machine_func(keyarr);
    }
}

static int keyboard_get_latch_keyarr_value(int row, int col)
{
  if (row < 0 || col < 0) {
    return -1;
  }
  return (latch_keyarr[row] & (1 << col)) ? 1 : 0;
}

static int is_ctrl_down(void)
{
  return keyboard_get_latch_keyarr_value(7, 2);
}

static int is_left_arrow(int row, int col, int value)
{
  return (row==7 && col==1 && value!=0);
}

static int is_f(int row, int col, int value)
{
  return (row==2 && col==5 && value!=0);
}

static int is_r(int row, int col, int value)
{
  return (row==2 && col==1 && value!=0);
}

static int is_h(int row, int col, int value)
{
  return (row==3 && col==5 && value!=0);
}

static int is_s(int row, int col, int value)
{
    return (row==1 && col==5 && value!=0);
}

static int is_fslash(int row, int col, int value)
{
    return (row==6 && col==7 && value!=0);
}

static int is_w(int row, int col, int value)
{
    return (row==1 && col==1 && value!=0);
}

static int is_u(int row, int col, int value)
{
    return (row==3 && col==6 && value!=0);
}

extern BYTE mem_ram[];
void clear_debug(void);
void debug_oneshot(char* str);
void debug_draw_box(int x, int y, int w, int h);
void debug_msg(int x, int y, char* str);
void debug_msg_centred(int y, char* str);
void debug_display(void);
extern int swapping;

static int swap_joyports(void)
{
  // swap joystick ports
  // I initially tried a technique borrowed from
  // "ui_joystick2.c" - swap_joystick_ports(), but it didn't work.
  // I suspect the retro-games branch does not use resource-names
  // like 'JoyDevice*', so I tried this alternate method of
  // swapping the joystick.

  debug_oneshot("JOYSTICK SWAP");

  if (swapping == 0) swapping = 1;
  else if (swapping == 1) swapping = 0;
}

void cartridge_trigger_freeze(void);
void vsync_suspend_speed_eval(void);

int trigger_counter = 0;
int trigger_hard_reset = 0;
int trigger_soft_reset = 0;
#define MAX_COUNTER 100000

static int sid_choice = 0;
#define MAX_SID_CHOICE 2
static char sid_choices[MAX_SID_CHOICE][20] =
{
  "6581 (ReSID)",
  "8580 (ReSID)",
};

void sid_chip_selector(void)
{
  sid_choice = (sid_choice + 1) % MAX_SID_CHOICE;
  vsync_suspend_speed_eval();
  debug_oneshot(sid_choices[sid_choice]);

  switch(sid_choice)
  {
    case 0:
      resources_set_int("SidEngine", SID_ENGINE_RESID );
      resources_set_int("SidModel" , SID_MODEL_6581 );
      break;
    case 1:
      resources_set_int("SidEngine", SID_ENGINE_RESID );
      resources_set_int("SidModel" , SID_MODEL_8580 );
      break;
  }

  sound_close();
  sound_open();
}

#include "pcuversion.h"
#include "linux/input.h"
#include <fcntl.h>

#define EV_PRESSED  1
#define EV_RELEASED 0
#define EV_REPEAT   2

static struct input_event event;
static unsigned int scan_code = 0;
int fd = 0;

static void debug_read_keys(void)
{
  int num_bytes;
  scan_code = 0;

    num_bytes = read(fd, &event, sizeof(struct input_event));

    if (event.type == EV_KEY)
    {
      if (event.value == EV_RELEASED)
      {
        scan_code = event.code;
      }
    }

}

static void debug_pause_trap(WORD addr, void *data)
{
  //char str[256];
  char *device = "/dev/input/event1";

  fd = open(device, O_RDWR);

    vsync_suspend_speed_eval();

    while (1) {
      debug_read_keys();
      //ui_dispatch_events();
      //SDL_Delay(10);
      usleep(10000);
      if (scan_code == 1) // escape key pressed?
        break;   // then exit our pause trap and let the emulator continue...

// if (scan_code != 0)
// {
//   sprintf(str, "key=%d    ", scan_code);
//   debug_msg(0, 0, str);
//   debug_display();
// }
    }

  close(fd);
}


// pause in order to display the help menu, and assess if run/stop key is pressed, to exit the menu
void debug_pause(void)
{
  interrupt_maincpu_trigger_trap(debug_pause_trap, 0);
}

void show_help_menu(void)
{
  clear_debug();
  debug_draw_box(1, 5, 34, 16);
  debug_msg_centred(6, "C64EMU-PCU.RGL");
  debug_msg_centred(7, "--------------");
  debug_msg(2,8,"CTRL-\x1f = Joy swap");
  debug_msg(2,9,"CTRL-F = Freeze button");
  debug_msg(2,10,"CTRL-R = Soft reset");
  debug_msg(2,11,"CTRL-H = Hard reset");
  debug_msg(2,12,"CTRL-S = SID swap (6581/8580)");
  debug_msg(2,13,"CTRL-W = Toggle Warp mode");
  debug_msg(2,14,"CTRL-U = Toggle UserPort joysticks");
  debug_msg(2,15,"CTRL-/ = Help menu");

  debug_msg(2,17, "VER#: " PCU_VERSION);

  debug_msg_centred(19, "Press RUN/STOP to exit");
  vsync_suspend_speed_eval();
  debug_display();

  debug_pause();
}

static int warptoggle=0;

static void toggle_warpmode(void)
{
  if (warptoggle == 0)
  {
    warptoggle = 1;
    debug_oneshot("WARP-MODE ON");
  }
  else
  {
    warptoggle = 0;
    debug_oneshot("WARP-MODE OFF");
  }

  resources_set_int("WarpMode", warptoggle);
  sound_set_warp_mode(warptoggle);
}

static int userportjoytoggle=0;
static void toggle_userport_joysticks(void)
{
  if (userportjoytoggle == 0)
  {
    userportjoytoggle = 1;
    debug_oneshot("UserPort Joysticks ON");
  }
  else
  {
    userportjoytoggle = 0;
    debug_oneshot("UserPort Joysticks OFF");
  }

  resources_set_int("ExtraJoy", userportjoytoggle);
  resources_set_int("ExtraJoyType", 0);
}

static void assess_pcu_shortcut_keys(int row, int col, int value)
{
    // CTRL+left-arrow = swap joystick ports
    if (is_ctrl_down() && is_left_arrow(row, col, value))
    {
      swap_joyports();
    }

    // CTRL+F = freeze button
    if (is_ctrl_down() && is_f(row, col, value))
    {
      debug_oneshot("FREEZE BUTTON PRESSED");
      cartridge_trigger_freeze();
    }

    // CTRL+R = soft-reset
    if (is_ctrl_down() && is_r(row, col, value))
    {
      debug_oneshot("SOFT RESET");
      vsync_suspend_speed_eval();
      trigger_soft_reset = 1;
      trigger_counter = MAX_COUNTER;
    }

    // CTRL+H = hard-reset
    if (is_ctrl_down() && is_h(row, col, value))
    {
      debug_oneshot("HARD RESET");
      vsync_suspend_speed_eval();
      trigger_hard_reset = 1;
      trigger_counter = MAX_COUNTER;
    }

    // CTRL+S = sid-chip selector
    if (is_ctrl_down() && is_s(row, col, value))
    {
      sid_chip_selector();
    }

    // CTRL+/ = show help menu
    if (is_ctrl_down() && is_fslash(row, col, value))
    {
      show_help_menu();
    }

    // CTRL+W = toggle warp mode
    if (is_ctrl_down() && is_w(row, col, value))
    {
      toggle_warpmode();
    }

    // CTRL+U = toggle user-port joysticks
    if (is_ctrl_down() && is_u(row, col, value))
    {
      toggle_userport_joysticks();
    }
}

static int keyboard_set_latch_keyarr(int row, int col, int value)
{
    if (row < 0 || col < 0) {
        return -1;
    }
    if (value) {
        latch_keyarr[row] |= 1 << col;
        latch_rev_keyarr[col] |= 1 << row;
    } else {
        latch_keyarr[row] &= ~(1 << col);
        latch_rev_keyarr[col] &= ~(1 << row);
    }

    assess_pcu_shortcut_keys(row, col, value);

    return 0;
}

/*-----------------------------------------------------------------------*/
#ifdef COMMON_KBD
static void keyboard_key_clear_internal(void);
#endif

static void keyboard_event_record(void)
{
    event_record(EVENT_KEYBOARD_MATRIX, (void *)keyarr, sizeof(keyarr));
}

void keyboard_event_playback(CLOCK offset, void *data)
{
    int row, col;

    memcpy(latch_keyarr, data, sizeof(keyarr));

    for (row = 0; row < KBD_ROWS; row++) {
        for (col = 0; col < KBD_COLS; col++) {
            keyboard_set_latch_keyarr(row, col, latch_keyarr[row] & (1 << col));
        }
    }

    keyboard_latch_matrix(offset);
}

void keyboard_restore_event_playback(CLOCK offset, void *data)
{
    machine_set_restore_key((int)(*(DWORD *)data));
}
    
static void keyboard_latch_handler(CLOCK offset, void *data)
{
    alarm_unset(keyboard_alarm);
    alarm_context_update_next_pending(keyboard_alarm->context);

    keyboard_latch_matrix(offset);

    keyboard_event_record();
}

void keyboard_event_delayed_playback(void *data)
{
    int row, col;

    memcpy(network_keyarr, data, sizeof(network_keyarr));

    for (row = 0; row < KBD_ROWS; row++) {
        for (col = 0; col < KBD_COLS; col++) {
            if (network_keyarr[row] & (1 << col)) {
                network_rev_keyarr[col] |= 1 << row;
            } else {
                network_rev_keyarr[col] &= ~(1 << row);
            }
        }
    }

    if (keyboard_clear == 1) {
#ifdef COMMON_KBD
        keyboard_key_clear_internal();
#endif
        keyboard_clear = 0;
    }

    alarm_set(keyboard_alarm, maincpu_clk + keyboard_delay);
}
/*-----------------------------------------------------------------------*/

void keyboard_set_keyarr(int row, int col, int value)
{
    if (keyboard_set_latch_keyarr(row, col, value) < 0) {
        return;
    }

    alarm_set(keyboard_alarm, maincpu_clk + KEYBOARD_RAND());
}

void keyboard_clear_keymatrix(void)
{
    memset(keyarr, 0, sizeof(keyarr));
    memset(rev_keyarr, 0, sizeof(rev_keyarr));
    memset(latch_keyarr, 0, sizeof(latch_keyarr));
    memset(latch_rev_keyarr, 0, sizeof(latch_rev_keyarr));
}

void keyboard_register_machine(keyboard_machine_func_t func)
{
    keyboard_machine_func = func;
}

void keyboard_register_delay(unsigned int delay)
{
    keyboard_delay = delay;
}

void keyboard_register_clear(void)
{
    keyboard_clear = 1;
}
/*-----------------------------------------------------------------------*/

#ifdef COMMON_KBD

enum shift_type {
    NO_SHIFT = 0,             /* Key is not shifted. */
    VIRTUAL_SHIFT = (1 << 0), /* The key needs a shift on the real machine. */
    LEFT_SHIFT = (1 << 1),    /* Key is left shift. */
    RIGHT_SHIFT = (1 << 2),   /* Key is right shift. */
    ALLOW_SHIFT = (1 << 3),   /* Allow key to be shifted. */
    DESHIFT_SHIFT = (1 << 4), /* Although SHIFT might be pressed, do not
                                 press shift on the real machine. */
    ALLOW_OTHER = (1 << 5),   /* Allow another key code to be assigned if
                                 SHIFT is pressed. */
    SHIFT_LOCK = (1 << 6),    /* Key is shift lock. */

    ALT_MAP  = (1 << 8)       /* Key is used for an alternative keyboard
                                 mapping */
};

struct keyboard_conv_s {
    signed long sym;
    int row;
    int column;
    enum shift_type shift;
    char *comment;
};
typedef struct keyboard_conv_s keyboard_conv_t;

/* Is the resource code ready to load the keymap?  */
static int load_keymap_ok;

/* Memory size of array in sizeof(keyconv_t), 0 = static.  */
static int keyc_mem = 0;

/* Number of convs used in sizeof(keyconv_t).  */
static int keyc_num = 0;

/* Two possible restore keys.  */
static signed long key_ctrl_restore1 = -1;
static signed long key_ctrl_restore2 = -1;

/* 40/80 column key.  */
static signed long key_ctrl_column4080 = -1;
static key_ctrl_column4080_func_t key_ctrl_column4080_func = NULL;

/* CAPS (ASCII/DIN) key.  */
static signed long key_ctrl_caps = -1;
static key_ctrl_caps_func_t key_ctrl_caps_func = NULL;

/* Special key to swap to alt set and Is an alternative mapping active? */
static signed long key_ctrl_alt1 = -1;
static signed long key_ctrl_alt2 = -1;
static int key_alternative = 0;

static keyboard_conv_t *keyconvmap = NULL;

static int kbd_lshiftrow;
static int kbd_lshiftcol;
static int kbd_rshiftrow;
static int kbd_rshiftcol;

#define KEY_NONE   0
#define KEY_RSHIFT 1
#define KEY_LSHIFT 2

static int vshift = KEY_NONE;
static int shiftl = KEY_NONE;

/*-----------------------------------------------------------------------*/

static int left_shift_down, right_shift_down, virtual_shift_down;
static int key_latch_row, key_latch_column;

static void keyboard_key_deshift(void)
{
    keyboard_set_latch_keyarr(kbd_lshiftrow, kbd_lshiftcol, 0);
    keyboard_set_latch_keyarr(kbd_rshiftrow, kbd_rshiftcol, 0);
}

static void keyboard_key_shift(void)
{
    if (left_shift_down > 0
        || (virtual_shift_down > 0 && vshift == KEY_LSHIFT)
        || (keyboard_shiftlock > 0 && shiftl == KEY_LSHIFT)) {
        keyboard_set_latch_keyarr(kbd_lshiftrow, kbd_lshiftcol, 1);
    }
    if (right_shift_down > 0
        || (virtual_shift_down > 0 && vshift == KEY_RSHIFT)
        || (keyboard_shiftlock > 0 && shiftl == KEY_RSHIFT)) {
        keyboard_set_latch_keyarr(kbd_rshiftrow, kbd_rshiftcol, 1);
    }
}

static int keyboard_key_pressed_matrix(int row, int column, int shift)
{
    if (row >= 0) {
        key_latch_row = row;
        key_latch_column = column;

        if (shift == NO_SHIFT || shift & DESHIFT_SHIFT) {
            keyboard_key_deshift();
        } else {
            if (shift & VIRTUAL_SHIFT) {
                virtual_shift_down = 1;
            }
            if (shift & LEFT_SHIFT) {
                left_shift_down = 1;
            }
            if (shift & RIGHT_SHIFT) {
                right_shift_down = 1;
            }
            if (shift & SHIFT_LOCK) {
                keyboard_shiftlock = 1;
            }
            keyboard_key_shift();
        }

        return 1;
    }

    return 0;
}

/*
    restore key handling. restore key presses are distributed randomly
    across a frame.

    FIXME: when network play is active this is not the case yet
*/

static int restore_raw = 0;
static int restore_delayed = 0;
static int restore_quick_release = 0;

static void restore_alarm_triggered(CLOCK offset, void *data)
{
    DWORD event_data;
    alarm_unset(restore_alarm);

    event_data = (DWORD)restore_delayed;
    machine_set_restore_key(restore_delayed);
    event_record(EVENT_KEYBOARD_RESTORE, (void*)&event_data, sizeof(DWORD));
    restore_delayed = 0;

    if (restore_quick_release) {
        restore_quick_release = 0;
        alarm_set(restore_alarm, maincpu_clk + KEYBOARD_RAND());
    }
}

static void keyboard_restore_pressed(void)
{
    DWORD event_data;
    event_data = (DWORD)1;
    if (network_connected()) {
        network_event_record(EVENT_KEYBOARD_RESTORE,
                (void*)&event_data, sizeof(DWORD));
    } else {
        if (restore_raw == 0) {
            restore_delayed = 1;
            restore_quick_release = 0;
            alarm_set(restore_alarm, maincpu_clk + KEYBOARD_RAND());
        }
    }
    restore_raw = 1;
}

static void keyboard_restore_released(void)
{
    DWORD event_data;
    event_data = (DWORD)0;
    if (network_connected()) {
        network_event_record(EVENT_KEYBOARD_RESTORE,
                (void*)&event_data, sizeof(DWORD));
    } else {
        if (restore_raw == 1) {
            if (restore_delayed) {
                restore_quick_release = 1;
            } else {
                alarm_set(restore_alarm, maincpu_clk + KEYBOARD_RAND());
            }
        }
    }
    restore_raw = 0;
}

void show_val(int val)
{
  int k;
  int loc = 0x400;  // start of screen mem;
  char str[256];
  sprintf(str, "%d", val);
  for (k = 0; k < 40; k++)
  {
    if (k < strlen(str))
    {
      mem_ram[loc+k] = str[k];
    }
    else
    {
      mem_ram[loc+k] = 32; // insert spaces
    }
  }
}

void show_str(char* str)
{
  int k;
  int loc = 0x400;  // start of screen mem;
  for (k = 0; k < strlen(str); k++)
  {
    if (k < strlen(str))
    {
      mem_ram[loc+k] = str[k];
    }
  }
}
void keyboard_key_pressed(signed long key)
{
    int i, latch;

    if (event_playback_active()) {
        return;
    }

    printf("key = %d\n", (int)key);
    // show_val((int)key);

    /* Restore */
    if (((key == key_ctrl_restore1) || (key == key_ctrl_restore2))
        && machine_has_restore_key())
    {
        keyboard_restore_pressed();
        return;
    }
    /* Alt */
    if ((key == key_ctrl_alt1) || (key == key_ctrl_alt2))
    {
        keyboard_alternative_set(1);
        return;
    }

    if (key == key_ctrl_column4080) {
        if (key_ctrl_column4080_func != NULL)
            key_ctrl_column4080_func();
        return;
    }

    if (key == key_ctrl_caps) {
        if (key_ctrl_caps_func != NULL)
            key_ctrl_caps_func();
        return;
    }

    for (i = 0; i < JOYSTICK_NUM; ++i) {
        if (joystick_port_map[i] == JOYDEV_NUMPAD
         || joystick_port_map[i] == JOYDEV_KEYSET1
         || joystick_port_map[i] == JOYDEV_KEYSET2) {
            if (joystick_check_set(key, joystick_port_map[i] - JOYDEV_NUMPAD, 1+i)) {
                return;
            }
        }
    }

    if (keyconvmap == NULL) {
        return;
    }

    latch = 0;

    for (i = 0; i < keyc_num; ++i) {
        if (key == keyconvmap[i].sym) {
            if ((keyconvmap[i].shift & ALT_MAP) && !key_alternative) {
                continue;
            }

            if (keyboard_key_pressed_matrix(keyconvmap[i].row,
                                            keyconvmap[i].column,
                                            keyconvmap[i].shift)) {
                latch = 1;
                if (!(keyconvmap[i].shift & ALLOW_OTHER)
                    || (right_shift_down + left_shift_down) == 0) {
                    break;
                }
            }
        }
    }

    if (latch) {
        keyboard_set_latch_keyarr(key_latch_row, key_latch_column, 1);
        if (network_connected()) {
            CLOCK keyboard_delay = KEYBOARD_RAND();
            network_event_record(EVENT_KEYBOARD_DELAY,
                    (void *)&keyboard_delay, sizeof(keyboard_delay));
            network_event_record(EVENT_KEYBOARD_MATRIX, 
                    (void *)latch_keyarr, sizeof(latch_keyarr));
        } else {
            alarm_set(keyboard_alarm, maincpu_clk + KEYBOARD_RAND());
        }
    }
}

static int keyboard_key_released_matrix(int row, int column, int shift)
{
    int skip_release = 0;

    if (row >= 0) {
        key_latch_row = row;
        key_latch_column = column;

        if (shift & VIRTUAL_SHIFT) {
            virtual_shift_down = 0;
        }
        if (shift & LEFT_SHIFT) {
            left_shift_down = 0;
            if (keyboard_shiftlock && (shiftl == KEY_LSHIFT)) {
                skip_release = 1;
            }
        }
        if (shift & RIGHT_SHIFT) {
            right_shift_down = 0;
            if (keyboard_shiftlock && (shiftl == KEY_RSHIFT)) {
                skip_release = 1;
            }
        }
        if (shift & SHIFT_LOCK) {
            keyboard_shiftlock = 0;
            if (((shiftl == KEY_RSHIFT) && right_shift_down)
             || ((shiftl == KEY_LSHIFT) && left_shift_down)) {
                skip_release = 1;
            }
        }

        /* Map shift keys. */
        if (right_shift_down > 0
            || (virtual_shift_down > 0 && vshift == KEY_RSHIFT)
            || (keyboard_shiftlock > 0 && shiftl == KEY_RSHIFT)) {
            keyboard_set_latch_keyarr(kbd_rshiftrow, kbd_rshiftcol, 1);
        } else {
            keyboard_set_latch_keyarr(kbd_rshiftrow, kbd_rshiftcol, 0);
        }

        if (left_shift_down > 0
            || (virtual_shift_down > 0 && vshift == KEY_LSHIFT)
            || (keyboard_shiftlock > 0 && shiftl == KEY_LSHIFT)) {
            keyboard_set_latch_keyarr(kbd_lshiftrow, kbd_lshiftcol, 1);
        } else {
            keyboard_set_latch_keyarr(kbd_lshiftrow, kbd_lshiftcol, 0);
        }

        return !skip_release;
    }

    return 0;
}

void keyboard_key_released(signed long key)
{
    int i, latch;

    if (event_playback_active()) {
        return;
    }

    /* Restore */
    if (((key == key_ctrl_restore1) || (key == key_ctrl_restore2))
        && machine_has_restore_key()) {
        keyboard_restore_released();
        return;
    }
    /* Alt */
    if ((key == key_ctrl_alt1) || (key == key_ctrl_alt2) )
    {
        keyboard_alternative_set(0);
        return;
    }

    for (i = 0; i < JOYSTICK_NUM; ++i) {
        if (joystick_port_map[i] == JOYDEV_NUMPAD
         || joystick_port_map[i] == JOYDEV_KEYSET1
         || joystick_port_map[i] == JOYDEV_KEYSET2) {
            if (joystick_check_clr(key, joystick_port_map[i] - JOYDEV_NUMPAD, 1+i)) {
                return;
            }
        }
    }

    if (keyconvmap == NULL) {
        return;
    }

    latch = 0;

    for (i = 0; i < keyc_num; i++) {
        if (key == keyconvmap[i].sym) {
            if ((keyconvmap[i].shift & ALT_MAP) && !key_alternative) {
                continue;
            }

            if (keyboard_key_released_matrix(keyconvmap[i].row,
                                             keyconvmap[i].column,
                                             keyconvmap[i].shift)) {
                latch = 1;
                keyboard_set_latch_keyarr(keyconvmap[i].row,
                                          keyconvmap[i].column, 0);
                if (!(keyconvmap[i].shift & ALLOW_OTHER)
                    /*|| (right_shift_down + left_shift_down) == 0*/) {
                    break;
                }
            }
        }
    }

    if (latch) {
        if (network_connected()) {
            CLOCK keyboard_delay = KEYBOARD_RAND();
            network_event_record(EVENT_KEYBOARD_DELAY,
                        (void *)&keyboard_delay, sizeof(keyboard_delay));
            network_event_record(EVENT_KEYBOARD_MATRIX, 
                        (void *)latch_keyarr, sizeof(latch_keyarr));
        } else {
            alarm_set(keyboard_alarm, maincpu_clk + KEYBOARD_RAND());
        }
    }
}

static void keyboard_key_clear_internal(void)
{
    keyboard_clear_keymatrix();
    joystick_clear_all();
    virtual_shift_down = left_shift_down = right_shift_down = keyboard_shiftlock = 0;
    joystick_joypad_clear();
}

void keyboard_key_clear(void)
{
    if (event_playback_active()) {
        return;
    }

    if (network_connected()) {
        network_event_record(EVENT_KEYBOARD_CLEAR, NULL, 0);
        return;
    }

    keyboard_key_clear_internal();
}

void keyboard_set_keyarr_any(int row, int col, int value)
{
    signed long sym;

    if (row < 0) {
        if (row == -3 && col == 0) {
            sym = key_ctrl_restore1;
        } else if (row == -3 && col == 1) {
            sym = key_ctrl_restore2;
        } else if (row == -4 && col == 0) {
            sym = key_ctrl_column4080;
        } else if (row == -4 && col == 1) {
            sym = key_ctrl_caps;
        } else {
            return;
        }

        if (value) {
            keyboard_key_pressed(sym);
        } else {
            keyboard_key_released(sym);
        }

    } else {
        keyboard_set_keyarr(row, col, value);
    }
}

/*-----------------------------------------------------------------------*/

void keyboard_alternative_set(int alternative)
{
    key_alternative = alternative;
}

/*-----------------------------------------------------------------------*/

static void keyboard_keyconvmap_alloc(void)
{
#define KEYCONVMAP_SIZE_MIN 150

    keyconvmap = lib_malloc(KEYCONVMAP_SIZE_MIN * sizeof(keyboard_conv_t));
    keyc_num = 0;
    keyc_mem = KEYCONVMAP_SIZE_MIN - 1;
    keyconvmap[0].sym = ARCHDEP_KEYBOARD_SYM_NONE;
}

static void keyboard_keyconvmap_free(void)
{
    lib_free(keyconvmap);
    keyconvmap = NULL;
}

static void keyboard_keyconvmap_realloc(void)
{
    keyc_mem += keyc_mem / 2;
    keyconvmap = lib_realloc(keyconvmap, (keyc_mem + 1) * sizeof(keyboard_conv_t));
}

/*-----------------------------------------------------------------------*/

static int keyboard_parse_keymap(const char *filename);

static void keyboard_keyword_lshift(void)
{
    char *p;

    p = strtok(NULL, " \t,");
    if (p != NULL) {
        kbd_lshiftrow = atoi(p);
        p = strtok(NULL, " \t,");
        if (p != NULL) {
            kbd_lshiftcol = atoi(p);
        }
    }
}

static void keyboard_keyword_rshift(void)
{
    char *p;

    p = strtok(NULL, " \t,");
    if (p != NULL) {
        kbd_rshiftrow = atoi(p);
        p = strtok(NULL, " \t,");
        if (p != NULL) {
            kbd_rshiftcol = atoi(p);
        }
    }
}

static int keyboard_keyword_vshiftl(void)
{
    char *p;

    p = strtok(NULL, " \t,\r");

    if (!strcmp(p, "RSHIFT")) {
        return KEY_RSHIFT;
    } else if (!strcmp(p, "LSHIFT")) {
        return KEY_LSHIFT;
    } else {
        return KEY_NONE;
    }
}

static void keyboard_keyword_vshift(void)
{
    vshift = keyboard_keyword_vshiftl();
}

static void keyboard_keyword_shiftl(void)
{
    shiftl = keyboard_keyword_vshiftl();
}

static void keyboard_keyword_clear(void)
{
    keyc_num = 0;
    keyconvmap[0].sym = ARCHDEP_KEYBOARD_SYM_NONE;
    key_ctrl_restore1 = -1;
    key_ctrl_restore2 = -1;
    key_ctrl_caps = -1;
    key_ctrl_column4080 = -1;
    vshift = KEY_NONE;
    shiftl = KEY_NONE;
}

static void keyboard_keyword_include(void)
{
    char *key;

    key = strtok(NULL, " \t");
    keyboard_parse_keymap(key);
}

static void keyboard_keysym_undef(signed long sym)
{
    int i;

    if (sym >= 0) {
        for (i = 0; i < keyc_num; i++) {
            if (keyconvmap[i].sym == sym) {
                if (keyc_num) {
                    keyconvmap[i] = keyconvmap[--keyc_num];
                }
                keyconvmap[keyc_num].sym = ARCHDEP_KEYBOARD_SYM_NONE;
                break;
            }
        }
    }
}

static void keyboard_keyword_undef(void)
{
    char *key;

    /* TODO: this only unsets from the main table, not for joysticks */
    key = strtok(NULL, " \t");
    keyboard_keysym_undef(kbd_arch_keyname_to_keynum(key));
}

static void keyboard_parse_keyword(char *buffer)
{
    char *key;

    key = strtok(buffer + 1, " \t:");

    if (!strcmp(key, "LSHIFT")) {
        keyboard_keyword_lshift();
    } else if (!strcmp(key, "RSHIFT")) {
        keyboard_keyword_rshift();
    } else if (!strcmp(key, "VSHIFT")) {
        keyboard_keyword_vshift();
    } else if (!strcmp(key, "SHIFTL")) {
        keyboard_keyword_shiftl();
    } else if (!strcmp(key, "CLEAR")) {
        keyboard_keyword_clear();
    } else if (!strcmp(key, "INCLUDE")) {
        keyboard_keyword_include();
    } else if (!strcmp(key, "UNDEF")) {
        keyboard_keyword_undef();
    }

    joystick_joypad_clear();
}

static void keyboard_parse_set_pos_row(signed long sym, int row, int col,
                                       int shift)
{
    int i;

    for (i = 0; i < keyc_num; i++) {
        if (sym == keyconvmap[i].sym
            && !(keyconvmap[i].shift & ALLOW_OTHER)
            && !(keyconvmap[i].shift & ALT_MAP)) {
            keyconvmap[i].row = row;
            keyconvmap[i].column = col;
            keyconvmap[i].shift = shift;
            break;
        }
    }

    /* Not in table -> add.  */
    if (i >= keyc_num) {
        /* Table too small -> realloc.  */
        if (keyc_num >= keyc_mem) {
            keyboard_keyconvmap_realloc();
        }

        if (keyc_num < keyc_mem) {
            keyconvmap[keyc_num].sym = sym;
            keyconvmap[keyc_num].row = row;
            keyconvmap[keyc_num].column = col;
            keyconvmap[keyc_num].shift = shift;
            keyconvmap[++keyc_num].sym = ARCHDEP_KEYBOARD_SYM_NONE;
        }
    }
}

static int keyboard_parse_set_neg_row(signed long sym, int row, int col)
{
    if (row == -3 && col == 0) {
        key_ctrl_restore1 = sym;
    } else
    if (row == -3 && col == 1) {
        key_ctrl_restore2 = sym;
    } else
    if (row == -4 && col == 0) {
        key_ctrl_column4080 = sym;
    } else
    if (row == -4 && col == 1) {
        key_ctrl_caps = sym;
    } else
    if (row == -6 && col == 0) { // Left Alt
        key_ctrl_alt1 = sym;
    } else 
    if (row == -6 && col == 1) { // Right Alt
        key_ctrl_alt2 = sym;
    } else {
        return -1;
    }
    return 0;
}

static void keyboard_parse_entry(char *buffer)
{
    char *key, *p;
    signed long sym;
    int row, col;
    int shift = 0;

    key = strtok(buffer, " \t:");

    sym = kbd_arch_keyname_to_keynum(key);

    if (sym < 0) {
        log_error(keyboard_log, "Could not find key `%s'!", key);
        return;
    }

    p = strtok(NULL, " \t,");
    if (p != NULL) {
        row = strtol(p, NULL, 10);
        p = strtok(NULL, " \t,");
        if (p != NULL) {
            col = atoi(p);
            p = strtok(NULL, " \t");
            if (p != NULL || row < 0) {
                if (p != NULL) {
                    shift = atoi(p);
                }

                if (row >= 0) {
                    keyboard_parse_set_pos_row(sym, row, col, shift);
                } else {
                    if (keyboard_parse_set_neg_row(sym, row, col) < 0) {
                        log_error(keyboard_log,
                            "Bad row/column value (%d/%d) for keysym `%s'.",
                            row, col, key);
                    }
                }
            }
        }
    }
}


static int keyboard_parse_keymap(const char *filename)
{
    FILE *fp;
    char *complete_path;
    char buffer[1000];

    fp = sysfile_open(filename, &complete_path, MODE_READ_TEXT);

    if (fp == NULL) {
        return -1;
    }

    log_message(keyboard_log, "Loading keymap `%s'.", complete_path);

    do {
        buffer[0] = 0;
        if (fgets(buffer, 999, fp)) {
            char *p;

            if (strlen(buffer) == 0) {
                break;
            }

            buffer[strlen(buffer) - 1] = 0; /* remove newline */
            /* remove comments */
            if ((p = strchr(buffer, '#'))) {
                *p=0;
            }

            switch(*buffer) {
              case 0:
                break;
              case '!':
                /* keyword handling */
                keyboard_parse_keyword(buffer);
                break;
              default:
                /* table entry handling */
                keyboard_parse_entry(buffer);
                break;
            }
        }
    } while (!feof(fp));
    fclose(fp);

    lib_free(complete_path);

    return 0;
}

static int keyboard_keymap_load(const char *filename)
{
    if (filename == NULL) {
        return -1;
    }

    if (keyconvmap != NULL) {
        keyboard_keyconvmap_free();
    }

    keyboard_keyconvmap_alloc();

    return keyboard_parse_keymap(filename);
}

/*-----------------------------------------------------------------------*/

void keyboard_set_map_any(signed long sym, int row, int col, int shift)
{
    if (row >= 0) {
        keyboard_parse_set_pos_row(sym, row, col, shift);
    } else {
        keyboard_parse_set_neg_row(sym, row, col);
    }
}

void keyboard_set_unmap_any(signed long sym)
{
    keyboard_keysym_undef(sym);
}

int keyboard_keymap_dump(const char *filename)
{
    FILE *fp;
    int i;

    if (filename == NULL) {
        return -1;
    }

    fp = fopen(filename, MODE_WRITE_TEXT);

    if (fp == NULL) {
        return -1;
    }

    fprintf(fp, "# VICE keyboard mapping file\n"
            "#\n"
            "# A Keyboard map is read in as patch to the current map.\n"
            "#\n"
            "# File format:\n"
            "# - comment lines start with '#'\n"
            "# - keyword lines start with '!keyword'\n"
            "# - normal line has 'keysym/scancode row column shiftflag'\n"
            "#\n"
            "# Keywords and their lines are:\n"
            "# '!CLEAR'               clear whole table\n"
            "# '!INCLUDE filename'    read file as mapping file\n"
            "# '!LSHIFT row col'      left shift keyboard row/column\n"
            "# '!RSHIFT row col'      right shift keyboard row/column\n"
            "# '!VSHIFT shiftkey'     virtual shift key (RSHIFT or LSHIFT)\n"
            "# '!SHIFTL shiftkey'     shift lock key (RSHIFT or LSHIFT)\n"
            "# '!UNDEF keysym'        remove keysym from table\n"
            "#\n"
            "# Shiftflag can have the values:\n"
            "# 0      key is not shifted for this keysym/scancode\n"
            "# 1      key is shifted for this keysym/scancode\n"
            "# 2      left shift\n"
            "# 4      right shift\n"
            "# 8      key can be shifted or not with this keysym/scancode\n"
            "# 16     deshift key for this keysym/scancode\n"
            "# 32     another definition for this keysym/scancode follows\n"
            "# 64     shift lock\n"
            "# 256    key is used for an alternative keyboard mapping\n"
            "#\n"
            "# Negative row values:\n"
            "# 'keysym -1 n' joystick #1, direction n\n"
            "# 'keysym -2 n' joystick #2, direction n\n"
            "# 'keysym -3 0' first RESTORE key\n"
            "# 'keysym -3 1' second RESTORE key\n"
            "# 'keysym -4 0' 40/80 column key\n"
            "# 'keysym -4 1' CAPS (ASCII/DIN) key\n"
            "# 'keysym -6 0' Left ALT key\n"
            "# 'keysym -6 1' Right ALT key\n"
            "#\n\n"
        );
    fprintf(fp, "!CLEAR\n");
    fprintf(fp, "!LSHIFT %d %d\n", kbd_lshiftrow, kbd_lshiftcol);
    fprintf(fp, "!RSHIFT %d %d\n", kbd_rshiftrow, kbd_rshiftcol);
    if (vshift != KEY_NONE) {
        fprintf(fp, "!VSHIFT %s\n",
                (vshift == KEY_RSHIFT) ? "RSHIFT" : "LSHIFT");
    }
    if (shiftl != KEY_NONE) {
        fprintf(fp, "!SHIFTL %s\n",
                (shiftl == KEY_RSHIFT) ? "RSHIFT" : "LSHIFT");
    }
    fprintf(fp, "\n");

    for (i = 0; keyconvmap[i].sym != ARCHDEP_KEYBOARD_SYM_NONE; i++) {
        fprintf(fp, "%s %d %d %d\n",
                kbd_arch_keynum_to_keyname(keyconvmap[i].sym),
                keyconvmap[i].row, keyconvmap[i].column,
                keyconvmap[i].shift);
    }
    fprintf(fp, "\n");

    if (key_ctrl_restore1 != -1 || key_ctrl_restore2 != -1) {
        fprintf(fp, "#\n"
                "# Restore key mappings\n"
                "#\n");
        if (key_ctrl_restore1 != -1) {
            fprintf(fp, "%s -3 0\n",
                    kbd_arch_keynum_to_keyname(key_ctrl_restore1));
        }
        if (key_ctrl_restore2 != -1) {
            fprintf(fp, "%s -3 1\n",
                    kbd_arch_keynum_to_keyname(key_ctrl_restore2));
        }
        fprintf(fp, "\n");
    }

    if (key_ctrl_column4080 != -1) {
        fprintf(fp, "#\n"
                "# 40/80 column key mapping\n"
                "#\n");
        fprintf(fp, "%s -4 0\n",
                kbd_arch_keynum_to_keyname(key_ctrl_restore1));
        fprintf(fp, "\n");
    }

    if (key_ctrl_caps != -1) {
        fprintf(fp, "#\n"
                "# CAPS (ASCII/DIN) key mapping\n"
                "#\n");
        fprintf(fp, "%s -4 1\n",
                kbd_arch_keynum_to_keyname(key_ctrl_restore1));
        fprintf(fp, "\n");
    }
    if (key_ctrl_alt1 != -1 || key_ctrl_alt2 != -1) {
        fprintf(fp, "#\n"
                "# Alt key mappings\n"
                "#\n");
        if (key_ctrl_alt1 != -1) {
            fprintf(fp, "%s -6 0\n",
                    kbd_arch_keynum_to_keyname(key_ctrl_alt1));
        }
        if (key_ctrl_alt2 != -1) {
            fprintf(fp, "%s -6 1\n",
                    kbd_arch_keynum_to_keyname(key_ctrl_alt2));
        }
        fprintf(fp, "\n");
    }

    fclose(fp);

    return 0;
}

/*-----------------------------------------------------------------------*/

int keyboard_set_keymap_index(int val, void *param)
{
    const char *name, *resname;

    resname = machine_keymap_res_name_list[val];

    if (resources_get_string(resname, &name) < 0)
        return -1;

    if (load_keymap_ok) {
        if (keyboard_keymap_load(name) >= 0) {
            machine_keymap_index = val;
            return 0;
        } else {
            log_error(keyboard_log, "Cannot load keymap `%s'.",
                      name ? name : "(null)");
        }
        return -1;
    }

    machine_keymap_index = val;
    return 0;
}

int keyboard_set_keymap_file(const char *val, void *param)
{
    int oldindex, newindex;

    newindex = vice_ptr_to_int(param);

    if (newindex >= machine_num_keyboard_mappings()) {
        return -1;
    }

    if (resources_get_int("KeymapIndex", &oldindex) < 0) {
        return -1;
    }

    if (util_string_set(&machine_keymap_file_list[newindex], val)) {
        return 0;
    }

    /* reset oldindex -> reload keymap file if this keymap is active */
    if (oldindex == newindex) {
        resources_set_int("KeymapIndex", oldindex);
    }

    return 0;
}

/*-----------------------------------------------------------------------*/

void keyboard_register_column4080_key(key_ctrl_column4080_func_t func)
{
    key_ctrl_column4080_func = func;
}

void keyboard_register_caps_key(key_ctrl_caps_func_t func)
{
    key_ctrl_caps_func = func;
}
#endif

/*-----------------------------------------------------------------------*/

void keyboard_init(void)
{
    keyboard_log = log_open("Keyboard");

    keyboard_alarm = alarm_new(maincpu_alarm_context, "Keyboard",
                               keyboard_latch_handler, NULL);
#ifdef COMMON_KBD
    restore_alarm = alarm_new(maincpu_alarm_context, "Restore",
                                restore_alarm_triggered, NULL);

    kbd_arch_init();

    load_keymap_ok = 1;
    keyboard_set_keymap_index(machine_keymap_index, NULL);
#endif
}

void keyboard_shutdown(void)
{
#ifdef COMMON_KBD
    keyboard_keyconvmap_free();
#endif
}

/*--------------------------------------------------------------------------*/
int keyboard_snapshot_write_module(snapshot_t *s)
{
    snapshot_module_t *m;

    m = snapshot_module_create(s, "KEYBOARD", 1, 0);
    if (m == NULL)
       return -1;

    if (0
        || SMW_DWA(m, (DWORD *)keyarr, KBD_ROWS) < 0
        || SMW_DWA(m, (DWORD *)rev_keyarr, KBD_COLS) < 0) {
        snapshot_module_close(m);
        return -1;
    }

    if (snapshot_module_close(m) < 0) {
        return -1;
    }

    return 0;
}

int keyboard_snapshot_read_module(snapshot_t *s)
{
    BYTE major_version, minor_version;
    snapshot_module_t *m;

    m = snapshot_module_open(s, "KEYBOARD",
                             &major_version, &minor_version);
    if (m == NULL) {
        return 0;
    }

    if (0
        || SMR_DWA(m, (DWORD *)keyarr, KBD_ROWS) < 0
        || SMR_DWA(m, (DWORD *)rev_keyarr, KBD_COLS) < 0) {
        snapshot_module_close(m);
        return -1;
    }

    snapshot_module_close(m);
    return 0;
}
