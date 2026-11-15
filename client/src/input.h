#ifndef MG_INPUT_H
#define MG_INPUT_H

#include <stdbool.h>

/* In a stream the controls go to Sunshine, in the menu they become navigation keys */
enum { MG_INPUT_STREAM = 0, MG_INPUT_MENU = 1 };
enum { MG_KEY_NONE = 0, MG_KEY_UP, MG_KEY_DOWN, MG_KEY_LEFT, MG_KEY_RIGHT, MG_KEY_OK, MG_KEY_BACK,
       MG_KEY_DEL,              /* the keyboard's Backspace (outside text fields it counts as BACK) */
       MG_KEY_ENTER,            /* the keyboard's Enter, the pad's Start: like OK, but in text fields it confirms */
       MG_KEY_CHAR = 0x100 };   /* + an ASCII character typed on the keyboard */

/* Reads the pads, keyboard and mouse that Main passes to the H264 core (the core
 * copies them into its control block: Main keeps /dev/input for itself). on_quit: the
 * exit combination or "Program menu" from the MiSTer's OSD. */
void mg_input_start(int pads, void (*on_quit)(void));
void mg_input_set_mode(int m);
/* The menu's next key (MG_KEY_NONE if nothing arrives within timeout_ms) */
int  mg_input_menu_key(int timeout_ms);
void mg_input_stop(void);
void mg_input_set_debug(bool on);
/* false: drops the digital directions the MiSTer derives from the left stick */
void mg_input_set_stick_dpad(bool on);

/* moonlight-common-c's rumble callback */
void mg_rumble(unsigned short controller, unsigned short low, unsigned short high);
void mg_rumble_stop_all(void);

/* For the tests: like mg_input_start but without a thread, and one loop pass (ms elapsed) */
void mg_input_setup(int pads, void (*on_quit)(void));
void mg_input_poll_once(unsigned ms);

#endif
