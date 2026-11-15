#ifndef MG_OPTIONS_H
#define MG_OPTIONS_H

#include <client.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  const char *action;
  char *host;
  const char *app;
  const char *keydir;
  int width, height; /* risoluzione chiesta a Sunshine */
  int bitrate;       /* kbps, 0 = automatic */
  int fps;           /* 0 = the core's frame rate, 30 = half */
  int pads;
  int remote;
  int packet_size;
  bool audio;
  bool quit_after;
  bool stats;
  bool debug_input;
  bool stick_dpad;
  int cpu;
  int pin;
  bool no_menu_cfg;
  bool pacing;
  bool guard;
  bool genlock;
  int pc_w, pc_h;       /* the PC's resolution during the stream (0: unchanged) */
  bool gl_lag;          /* with the genlock: one picture in reserve in the core (one frame of latency) */
  const char *core_rbf;
} options;

/* A streaming session with the core already attached. Returns 0 if the stream
 * started (and then ended), -1 if it did not start; 'msg' receives the reason. */
int mg_stream_session(PSERVER_DATA server, options *o, int app_id, char *msg, size_t msglen);
/* Asks the running session to end (like the key combination) */
void mg_request_quit(void);
/* true if the program must exit (SIGTERM/SIGINT, core unloaded) */
bool mg_exiting(void);

/* menu.c */
int mg_menu_run(options *o, const char *keydir);

/* the resolutions offered in the menu */
typedef struct { int w, h; const char *desc; } mg_res;
int mg_res_count(void);
const mg_res *mg_res_at(int i);
int mg_res_find(int w, int h); /* index, -1 if absent */

#endif
