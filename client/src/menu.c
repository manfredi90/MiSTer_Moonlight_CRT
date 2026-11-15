/*
 * On-screen menu (a 320x240 canvas, shown doubled in the H264 core's overlay): the list of
 * PCs (add, rename, change address, remove; on-screen keyboard), the list of Sunshine's
 * apps, settings, pairing with the PIN on screen. Navigated with the pad (d-pad, A
 * confirms, B back) or the keyboard (arrows, Enter, Esc; text fields take typing
 * directly). At the stream's end (the exit combination or a close from the PC) it comes
 * back here.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#define _GNU_SOURCE
#include "options.h"
#include "font.h"
#include "core.h"
#include "input.h"
#include "netdiag.h"

#include <Limelight.h>
#include <client.h>
#include <errors.h>

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 320
#define H 240
#define X0 16          /* margins against the CRT's overscan */
#define Y0 12
#define ROW_H 10
#define LIST_Y 34
#define LIST_ROWS 17
#define COLS ((W - 2 * X0) / 8)

#define C_BG     0x0C1424
#define C_HEAD   0x5AA0FF
#define C_TEXT   0xE0E4EA
#define C_DIM    0x8090A8
#define C_BAR    0x1E4C9A
#define C_VALUE  0xFFD060
#define C_OK     0x60E080
#define C_ERR    0xFF7060
#define C_LINE   0x2A3A58

static uint32_t canvas[W * H];

/* ---------- drawing ---------- */

static void fill(int x, int y, int w, int h, uint32_t c) {
  for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
    for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++)
      canvas[j * W + i] = c;
}

/* Turns the most common accented letters into ASCII (the OSD font is ASCII) */
static void ascii_fold(const char *in, char *out, size_t n) {
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < n; p++) {
    if (*p < 0x80) { out[o++] = *p; continue; }
    if (*p == 0xC3 && p[1]) {
      unsigned char c = *++p;
      const char *map = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
      out[o++] = (c >= 0x80 && c <= 0xBF) ? map[c - 0x80] : '?';
      continue;
    }
    while ((p[1] & 0xC0) == 0x80) p++; /* other multibyte characters: skipped */
    out[o++] = '?';
  }
  out[o] = 0;
}

static void text_scaled(int x, int y, const char *s, uint32_t c, int scale) {
  char buf[256];
  ascii_fold(s, buf, sizeof(buf));
  for (const unsigned char *p = (const unsigned char *)buf; *p; p++, x += 8 * scale) {
    const unsigned char *g = mg_font[*p];
    for (int cx = 0; cx < 8; cx++)
      for (int cy = 0; cy < 8; cy++)
        if ((g[cx] >> cy) & 1)
          fill(x + cx * scale, y + cy * scale, scale, scale, c);
  }
}

static void text(int x, int y, const char *s, uint32_t c) { text_scaled(x, y, s, c, 1); }

static int text_len(const char *s) {
  char buf[256];
  ascii_fold(s, buf, sizeof(buf));
  return (int)strlen(buf);
}

static void text_right(int xr, int y, const char *s, uint32_t c) { text(xr - text_len(s) * 8, y, s, c); }
static void text_center(int y, const char *s, uint32_t c, int scale) {
  text_scaled((W - text_len(s) * 8 * scale) / 2, y, s, c, scale);
}

/* Shows the canvas in the core's overlay */
static void present(void) { mg_core_overlay_present(canvas, W, H); }

/* ---------- state ---------- */

typedef struct { char name[64]; int id; } app_entry;

enum { ST_CONNECT, ST_PAIR, ST_MAIN, ST_MESSAGE, ST_PCS, ST_PCOPT, ST_EDIT };
enum { IT_APP, IT_HEADER, IT_BITRATE, IT_AUDIO, IT_QUITAFTER, IT_SYNC, IT_PCRES,
       IT_QUITAPP, IT_REFRESH, IT_EXIT, IT_PCS };

/* the sync with the PC: the genlock (the core follows the PC and keeps one decoded picture in
 * reserve: one more frame, the default), the genlock low latency (the same without the reserve),
 * 60 Hz (the core stays at 60 Hz; a picture landing right at the pickup goes to the next one) */
enum { SYNC_GENLOCK, SYNC_GENLOCK_LOW, SYNC_60HZ, N_SYNC };
static const char *sync_names[N_SYNC] = { "genlock", "genlock low latency", "60 Hz" };
/* menu.cfg up to 0.4.1: "genlock +1" is today's genlock; the old plain "genlock" becomes the
 * default genlock too; guard and none (and the Italian names) become 60 Hz */
static const struct { const char *name; int mode; } sync_old[] = {
  { "genlock +1", SYNC_GENLOCK }, { "guard", SYNC_60HZ }, { "none", SYNC_60HZ },
  { "guardia", SYNC_60HZ }, { "nessuna", SYNC_60HZ },
};
static int sync_mode;

/* the PC's resolution during the stream (Sunshine sets it, with "Use resolution provided by the
 * client", and restores it): not every game has every one. The stream stays 640x480: 1280x720
 * arrives letterboxed */
static const struct { const char *name; int w, h; } pcres[] = {
  { "off", 0, 0 }, { "640x480", 640, 480 }, { "1024x768", 1024, 768 }, { "1280x960", 1280, 960 }, { "1280x720", 1280, 720 },
};
#define N_PCRES ((int)(sizeof(pcres) / sizeof(pcres[0])))
static int pcres_i;

/* the PCs with Sunshine (saved in menu.cfg as pc=address|name) */
#define MAX_PCS 16
typedef struct { char host[64]; char name[32]; } pc_entry;
static pc_entry pcs[MAX_PCS];
static int n_pcs, cur_pc = -1;
static bool pcs_saved;           /* menu.cfg already has a list (even empty): HOST is not added again */
static char last_pc[64];
static int pc_cursor;            /* in the list of PCs: 0..n_pcs-1 the PCs, then Add, Exit */
static int opt_cursor;           /* in a PC's options */
static bool opt_confirm;         /* "Remove" pressed once: confirmation is asked */

/* the on-screen keyboard */
enum { ED_NEW_HOST, ED_NEW_NAME, ED_HOST, ED_NAME };
static int ed_kind, ed_pc;
static char ed_buf[64], ed_title[48], ed_new_host[64];
static int ed_row, ed_col;
static bool ed_shift;
static char ed_err[64];

typedef struct { int type; int app; } item;

static options *opt;
static char host_buf[64];        /* the chosen PC's address (opt->host points to it) */
static const char *keydir;
static SERVER_DATA server;
static bool server_ok;
static app_entry apps[48];
static int n_apps;
static int state = ST_CONNECT;
static item items[80];
static int n_items, cursor, scroll;
static char pin[5];
static char msg_title[64], msg_text[2][320];
static bool msg_retry;           /* in the message, A retries the connection */
static char status[160];
static uint32_t status_color;
static double status_until;
static char last_app[64];
static char cfg_path[PATH_MAX];

/* up to 10000: beyond that, large pictures take the core longer than a frame to decode (about
 * 2.7 ms + 0.57 us a byte, traces of 15/11) and the quality at 640x480 does not improve */
static const int bitrates[] = { 0, 3000, 5000, 8000, 10000 };
#define N_BITRATES (int)(sizeof(bitrates) / sizeof(bitrates[0]))

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void set_status(const char *s, uint32_t color) {
  snprintf(status, sizeof(status), "%s", s);
  status_color = color;
  status_until = now_s() + 6;
}

static void show_message(const char *title, const char *l1, const char *l2, bool retry) {
  snprintf(msg_title, sizeof(msg_title), "%s", title);
  snprintf(msg_text[0], sizeof(msg_text[0]), "%s", l1 ? l1 : "");
  snprintf(msg_text[1], sizeof(msg_text[1]), "%s", l2 ? l2 : "");
  msg_retry = retry;
  state = ST_MESSAGE;
}

/* ---------- saved settings ---------- */

static void cfg_load(void) {
  FILE *f = fopen(cfg_path, "r");
  if (!f) return;
  char line[160];
  while (fgets(line, sizeof(line), f)) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *v = eq + 1;
    v[strcspn(v, "\r\n")] = 0;
    /* res= and fps= (versions up to 0.1.5) are ignored: from the menu always 640x480 at 60 */
    if (!strcmp(line, "bitrate")) { opt->bitrate = atoi(v); if (opt->bitrate > 10000) opt->bitrate = 10000; }
    else if (!strcmp(line, "audio")) opt->audio = atoi(v) != 0;
    else if (!strcmp(line, "quitafter")) opt->quit_after = atoi(v) != 0;
    else if (!strcmp(line, "lastapp")) snprintf(last_app, sizeof(last_app), "%s", v);
    else if (!strcmp(line, "lastpc")) snprintf(last_pc, sizeof(last_pc), "%s", v);
    else if (!strcmp(line, "sync")) {
      for (int i = 0; i < N_SYNC; i++) if (!strcmp(v, sync_names[i])) sync_mode = i;
      for (size_t i = 0; i < sizeof(sync_old) / sizeof(sync_old[0]); i++) if (!strcmp(v, sync_old[i].name)) sync_mode = sync_old[i].mode;
    }
    else if (!strcmp(line, "pcres")) { pcres_i = 0; for (int i = 1; i < N_PCRES; i++) if (!strcmp(v, pcres[i].name)) pcres_i = i; }
    else if (!strcmp(line, "pcs")) pcs_saved = true;
    else if (!strcmp(line, "pc") && n_pcs < MAX_PCS) {
      char *bar = strchr(v, '|');
      if (bar) *bar = 0;
      if (!v[0]) continue;
      snprintf(pcs[n_pcs].host, sizeof(pcs[n_pcs].host), "%s", v);
      snprintf(pcs[n_pcs].name, sizeof(pcs[n_pcs].name), "%s", bar && bar[1] ? bar + 1 : v);
      n_pcs++;
    }
  }
  fclose(f);
}

static void cfg_save(void) {
  FILE *f = fopen(cfg_path, "w");
  if (!f) return;
  fprintf(f, "bitrate=%d\naudio=%d\nquitafter=%d\nsync=%s\npcres=%s\nlastapp=%s\nlastpc=%s\npcs=%d\n",
          opt->bitrate, opt->audio, opt->quit_after, sync_names[sync_mode], pcres[pcres_i].name, last_app, last_pc, n_pcs);
  for (int i = 0; i < n_pcs; i++)
    fprintf(f, "pc=%s|%s\n", pcs[i].host, pcs[i].name);
  fclose(f);
  pcs_saved = true;
}

/* ---------- background jobs (network) ---------- */

enum { JOB_NONE, JOB_CONNECT, JOB_PAIR };
static pthread_t job_thread;
static volatile int job, job_done;
static int job_result;
static char job_error[160];
static bool was_paired; /* already seen paired in this run (with the chosen PC) */
/* One job at a time (libgamestream has a single curl handle). If the PC changes while a job
 * runs (a pairing can wait for the PIN up to 60 s) its result is dropped: only the one with
 * the current PC's number counts; the connection to the new PC starts when it ends. */
static int pc_gen, job_gen;
static bool connect_pending;
static char job_host[64];

static void *job_main(void *arg) {
  int ret = GS_OK;
  job_error[0] = 0;
  if (job == JOB_PAIR) {
    ret = gs_pair(&server, pin);
    if (ret != GS_OK)
      snprintf(job_error, sizeof(job_error), "%s", gs_error ? gs_error : "unknown error");
  } else {
    double t0 = now_s();
    ret = gs_init(&server, job_host, 47989, keydir, 0, true);
    /* At start the MiSTer's network may not be ready yet (the core just loaded and
     * the Wi-Fi just reconfigured): a few attempts before showing the error. */
    for (int i = 0; i < 4 && ret != GS_OK && !mg_exiting() && job_gen == pc_gen; i++) {
      printf("[menu] connection failed after %.1f s (%s): retrying (%d)\n", now_s() - t0,
             gs_error ? gs_error : "?", i + 1);
      if (i == 0)
        mg_net_diag("connection to Sunshine failed", job_host);
      sleep(2);
      t0 = now_s();
      ret = gs_init(&server, job_host, 47989, keydir, 0, true);
    }
    if (ret == GS_OK && !server.paired)
      printf("[menu] serverinfo: not paired according to HTTP (running app %d); HTTPS failed after %.1f s: %s\n",
             server.currentGame, now_s() - t0, gs_https_error[0] ? gs_https_error : "?");
    /* Right after a stream Sunshine may not answer on the HTTPS port: libgamestream
     * then falls back to HTTP, which always says "not paired". If the pairing was
     * there, a few retries before asking for the PIN. */
    for (int i = 0; i < 5 && ret == GS_OK && !server.paired && was_paired && !mg_exiting() && job_gen == pc_gen; i++) {
      printf("[menu] Sunshine answers \"not paired\": retrying (%d)\n", i + 1);
      sleep(2);
      t0 = now_s();
      ret = gs_init(&server, job_host, 47989, keydir, 0, true);
      if (ret == GS_OK && !server.paired)
        printf("[menu]   still not: HTTPS failed after %.1f s: %s\n", now_s() - t0, gs_https_error);
    }
    if (ret == GS_OK && server.paired && job_gen == pc_gen)
      was_paired = true;
    if (ret != GS_OK) {
      snprintf(job_error, sizeof(job_error), "%s", gs_error ? gs_error : "no answer");
    } else if (server.paired && job_gen == pc_gen) {
      PAPP_LIST list = NULL;
      if (gs_applist(&server, &list) == GS_OK) {
        /* libgamestream builds the list backwards: we turn it around */
        int n = 0;
        for (PAPP_LIST l = list; l; l = l->next) n++;
        if (n > 48) n = 48;
        n_apps = n;
        for (PAPP_LIST l = list; l && n > 0; l = l->next) {
          n--;
          snprintf(apps[n].name, sizeof(apps[n].name), "%s", l->name);
          apps[n].id = l->id;
        }
      } else {
        ret = GS_FAILED;
        snprintf(job_error, sizeof(job_error), "app list: %s", gs_error ? gs_error : "error");
      }
    }
  }
  job_result = ret;
  job_done = 1;
  return NULL;
}

static void start_job(int j) {
  if (job != JOB_NONE) {
    if (j == JOB_CONNECT && job_gen != pc_gen) connect_pending = true;   /* after the old one */
    return;
  }
  job = j;
  job_done = 0;
  job_gen = pc_gen;
  snprintf(job_host, sizeof(job_host), "%s", host_buf);
  pthread_create(&job_thread, NULL, job_main, NULL);
}

static void rebuild_items(void);

static void finish_job(void) {
  if (job == JOB_NONE || !job_done) return;
  pthread_join(job_thread, NULL);
  int j = job;
  job = JOB_NONE;
  if (job_gen != pc_gen) {                   /* the PC changed: the result does not count */
    printf("[menu] result for %s dropped (PC changed)\n", job_host);
    if (connect_pending) { connect_pending = false; start_job(JOB_CONNECT); }
    return;
  }
  if (j == JOB_CONNECT) {
    if (job_result != GS_OK) {
      char l1[160];
      snprintf(l1, sizeof(l1), "Sunshine does not answer at %s", host_buf);
      server_ok = false;
      if (state != ST_MESSAGE)
        show_message("Connection", l1, job_error, true);
    } else if (!server.paired && gs_https_error[0] && (was_paired || server.currentGame != 0)) {
      /* With an app open Sunshine would refuse the pairing anyway ("The computer is
       * currently in a game"): the problem is the HTTPS connection, not the pairing. */
      char l2[320];
      snprintf(l2, sizeof(l2), "Error: %s. If it persists, close the app on the PC (in Sunshine: Troubleshooting, Force Close) and retry.", gs_https_error);
      server_ok = false;
      show_message("Connection", "Sunshine does not answer on the secure connection", l2, true);
    } else if (!server.paired) {
      srandom(getpid() ^ (unsigned)time(NULL));
      snprintf(pin, sizeof(pin), "%04ld", random() % 10000);
      state = ST_PAIR;
      start_job(JOB_PAIR);
    } else {
      server_ok = true;
      rebuild_items();
      if (state == ST_CONNECT) state = ST_MAIN; /* an open message stays until A is pressed */
    }
  } else if (j == JOB_PAIR) {
    if (job_result == GS_OK) {
      set_status("Paired", C_OK);
      state = ST_CONNECT;
      start_job(JOB_CONNECT);
    } else {
      show_message("Pairing failed", job_error, "", true);
    }
  }
}

/* ---------- list ---------- */

static void rebuild_items(void) {
  int old_type = n_items ? items[cursor].type : -1;
  int old_app = n_items ? items[cursor].app : -1;
  n_items = 0;
  for (int i = 0; i < n_apps; i++)
    items[n_items++] = (item){ IT_APP, i };
  if (!n_apps)
    items[n_items++] = (item){ IT_HEADER, -2 }; /* "no app" */
  items[n_items++] = (item){ IT_HEADER, -1 };
  items[n_items++] = (item){ IT_BITRATE, -1 };
  items[n_items++] = (item){ IT_AUDIO, -1 };
  items[n_items++] = (item){ IT_QUITAFTER, -1 };
  items[n_items++] = (item){ IT_SYNC, -1 };
  items[n_items++] = (item){ IT_PCRES, -1 };
  if (server.currentGame)
    items[n_items++] = (item){ IT_QUITAPP, -1 };
  items[n_items++] = (item){ IT_REFRESH, -1 };
  items[n_items++] = (item){ IT_PCS, -1 };
  items[n_items++] = (item){ IT_EXIT, -1 };

  /* puts the cursor back where it was, or on the last app used */
  cursor = 0;
  for (int i = 0; i < n_items; i++) {
    if (old_type >= 0 && items[i].type == old_type && items[i].app == old_app) { cursor = i; break; }
    if (old_type < 0 && items[i].type == IT_APP && !strcmp(apps[items[i].app].name, last_app)) { cursor = i; break; }
  }
  if (items[cursor].type == IT_HEADER)
    cursor++;
}

static void change_setting(int type, int dir) {
  switch (type) {
  case IT_BITRATE: {
    int i = 0;
    while (i < N_BITRATES && bitrates[i] != opt->bitrate) i++;
    if (i == N_BITRATES) i = 0;
    opt->bitrate = bitrates[(i + dir + N_BITRATES) % N_BITRATES];
    break;
  }
  case IT_AUDIO: opt->audio = !opt->audio; break;
  case IT_QUITAFTER: opt->quit_after = !opt->quit_after; break;
  case IT_SYNC: sync_mode = (sync_mode + dir + N_SYNC) % N_SYNC; break;
  case IT_PCRES: pcres_i = (pcres_i + dir + N_PCRES) % N_PCRES; break;
  default: return;
  }
  cfg_save();
}

static void item_label(const item *it, char *label, size_t ln, char *value, size_t vn) {
  value[0] = 0;
  switch (it->type) {
  case IT_APP:
    snprintf(label, ln, "%s", apps[it->app].name);
    if (server.currentGame == apps[it->app].id) snprintf(value, vn, "running");
    break;
  case IT_HEADER: snprintf(label, ln, "%s", it->app == -2 ? "(no app in Sunshine)" : ""); break;
  case IT_BITRATE:
    snprintf(label, ln, "Bitrate");
    if (opt->bitrate) snprintf(value, vn, "< %d kbps >", opt->bitrate);
    else snprintf(value, vn, "< auto (%d kbps) >",
                  mg_auto_bitrate(sync_mode != SYNC_60HZ, sync_mode == SYNC_GENLOCK, sync_mode == SYNC_60HZ));
    break;
  case IT_AUDIO: snprintf(label, ln, "Audio"); snprintf(value, vn, "< %s >", opt->audio ? "on" : "off"); break;
  case IT_QUITAFTER: snprintf(label, ln, "Close the app on exit"); snprintf(value, vn, "< %s >", opt->quit_after ? "yes" : "no"); break;
  case IT_SYNC: snprintf(label, ln, "Sync"); snprintf(value, vn, "< %s >", sync_names[sync_mode]); break;
  case IT_PCRES: snprintf(label, ln, "PC resolution"); snprintf(value, vn, "< %s >", pcres[pcres_i].name); break;
  case IT_QUITAPP: snprintf(label, ln, "Close the app open on the PC"); break;
  case IT_REFRESH: snprintf(label, ln, "Refresh list"); break;
  case IT_PCS: snprintf(label, ln, "Change PC"); break;
  case IT_EXIT: snprintf(label, ln, "Exit"); break;
  }
}

/* ---------- screens ---------- */

static void draw_frame(const char *footer) {
  fill(0, 0, W, H, C_BG);
  text(X0, Y0, "MOONLIGHT", C_HEAD);
  const char *who = state == ST_PCS || state == ST_EDIT ? "PCs with Sunshine"
                  : state == ST_PCOPT && ed_pc >= 0 && ed_pc < n_pcs ? pcs[ed_pc].host
                  : cur_pc >= 0 && cur_pc < n_pcs ? pcs[cur_pc].name : host_buf;
  char wb[40];
  snprintf(wb, sizeof(wb), "%.*s", COLS - 11, who);
  text_right(W - X0, Y0, wb, C_DIM);
  fill(X0, Y0 + 12, W - 2 * X0, 1, C_LINE);
  fill(X0, H - Y0 - 12, W - 2 * X0, 1, C_LINE);
  if (status[0] && now_s() < status_until)
    text(X0, H - Y0 - 24, status, status_color);
  if (footer)
    text(X0, H - Y0 - 8, footer, C_DIM);
}

static void draw_main(void) {
  draw_frame("A start/ok  < > change  B PCs");
  if (cursor < scroll) scroll = cursor;
  if (cursor >= scroll + LIST_ROWS) scroll = cursor - LIST_ROWS + 1;
  for (int r = 0; r < LIST_ROWS && scroll + r < n_items; r++) {
    const item *it = &items[scroll + r];
    int y = LIST_Y + r * ROW_H;
    char label[80], value[64];
    item_label(it, label, sizeof(label), value, sizeof(value));
    if (it->type == IT_HEADER) {
      if (it->app == -1) {
        fill(X0, y + 4, W - 2 * X0, 1, C_LINE);
        fill(X0 + 8, y, 14 * 8, 8, C_BG);
        text(X0 + 8, y, " Settings ", C_DIM);
      } else {
        text(X0 + 8, y, label, C_DIM);
      }
      continue;
    }
    bool sel = scroll + r == cursor;
    if (sel) fill(X0 - 4, y - 1, W - 2 * X0 + 8, ROW_H, C_BAR);
    /* name cut to leave room for the value */
    int maxc = COLS - 1 - (value[0] ? text_len(value) + 1 : 0);
    if (text_len(label) > maxc && maxc > 3) { label[maxc - 1] = '.'; label[maxc] = 0; }
    text(X0 + 4, y, label, sel ? 0xFFFFFF : C_TEXT);
    if (value[0])
      text_right(W - X0 - 4, y, value, it->type == IT_APP ? C_OK : C_VALUE);
  }
  if (scroll > 0) text_right(W - X0, LIST_Y - 10, "^", C_DIM);
  if (scroll + LIST_ROWS < n_items) text_right(W - X0, LIST_Y + LIST_ROWS * ROW_H - 2, "v", C_DIM);
}

static void draw_connect(void) {
  draw_frame("B back");
  char l[96];
  if (job != JOB_NONE && job_gen != pc_gen)
    snprintf(l, sizeof(l), "Waiting for the previous request to end...");
  else if (cur_pc >= 0 && cur_pc < n_pcs && strcmp(pcs[cur_pc].name, host_buf))
    snprintf(l, sizeof(l), "Connecting to %s (%s)...", pcs[cur_pc].name, host_buf);
  else
    snprintf(l, sizeof(l), "Connecting to %s...", host_buf);
  text_center(100, l, C_TEXT, 1);
  static const char spin[] = "|/-\\";
  char s[2] = { spin[(int)(now_s() * 6) % 4], 0 };
  text_center(120, s, C_HEAD, 2);
}

static void draw_pair(void) {
  draw_frame("B back");
  text_center(46, "Pairing with Sunshine", C_HEAD, 1);
  text_center(70, "On the PC open Sunshine's web page", C_TEXT, 1);
  text_center(82, "(https://localhost:47990), PIN tab,", C_TEXT, 1);
  text_center(94, "and enter this code:", C_TEXT, 1);
  text_center(116, pin, C_VALUE, 4);
  text_center(162, "Waiting...", C_DIM, 1);
}

/* Writes a centred text wrapping on spaces; returns the next y */
static int text_wrapped(int y, const char *s, uint32_t c) {
  char buf[256];
  ascii_fold(s, buf, sizeof(buf));
  const int maxc = COLS;
  char *p = buf;
  while (*p) {
    while (*p == ' ') p++;
    int n = (int)strlen(p);
    if (n > maxc) {
      n = maxc;
      while (n > 0 && p[n] != ' ') n--;
      if (n == 0) n = maxc;
    }
    char line[64];
    snprintf(line, sizeof(line), "%.*s", n, p);
    text_center(y, line, c, 1);
    y += ROW_H + 2;
    p += n;
  }
  return y;
}

static void draw_message(void) {
  draw_frame(msg_retry ? "A retry  B back" : "A ok");
  text_center(76, msg_title, C_ERR, 1);
  int y = text_wrapped(100, msg_text[0], C_TEXT);
  text_wrapped(y + 4, msg_text[1], C_DIM);
}

static void draw_starting(const char *name) {
  draw_frame(NULL);
  char l[96];
  snprintf(l, sizeof(l), "Starting %s", name);
  text_center(104, l, C_TEXT, 1);
  char r[64];
  int ri = mg_res_find(opt->width, opt->height);
  if (ri >= 0) snprintf(r, sizeof(r), "%s", mg_res_at(ri)->desc);
  else snprintf(r, sizeof(r), "%dx%d", opt->width, opt->height);
  text_center(120, r, C_DIM, 1);
}

/* ---------- list of PCs ---------- */

static void pick_pc(int i);

static void draw_pcs(void) {
  draw_frame("A connect  > options  B exit");
  int n = n_pcs + 2;
  int top = pc_cursor - LIST_ROWS + 1 > 0 ? pc_cursor - LIST_ROWS + 1 : 0;
  for (int r = 0; r < LIST_ROWS && top + r < n; r++) {
    int i = top + r;
    int y = LIST_Y + r * ROW_H;
    bool sel = i == pc_cursor;
    if (sel) fill(X0 - 4, y - 1, W - 2 * X0 + 8, ROW_H, C_BAR);
    if (i < n_pcs) {
      char lab[40], val[64];
      int maxv = COLS / 2;
      snprintf(val, sizeof(val), "%.*s", maxv, pcs[i].host);
      snprintf(lab, sizeof(lab), "%.*s", COLS - 2 - text_len(val), pcs[i].name);
      text(X0 + 4, y, lab, sel ? 0xFFFFFF : C_TEXT);
      text_right(W - X0 - 4, y, val, C_DIM);
    } else if (i == n_pcs) {
      text(X0 + 4, y, "+ Add PC", sel ? 0xFFFFFF : C_OK);
    } else {
      text(X0 + 4, y, "Exit", sel ? 0xFFFFFF : C_TEXT);
    }
  }
  if (!n_pcs)
    text_wrapped(LIST_Y + 4 * ROW_H, "Add the PC with Sunshine by typing its IP address (shown in Sunshine, or by ipconfig on the PC).", C_DIM);
}

static const char *pcopt_labels[] = { "Connect", "Rename", "Change address", "Remove", "Back" };
#define N_PCOPT 5

static void draw_pcopt(void) {
  draw_frame("A ok  B back");
  char t[64];
  snprintf(t, sizeof(t), "%.36s", pcs[ed_pc].name);
  text_center(LIST_Y, t, C_HEAD, 1);
  for (int i = 0; i < N_PCOPT; i++) {
    int y = LIST_Y + 20 + i * (ROW_H + 4);
    bool sel = i == opt_cursor;
    if (sel) fill(X0 + 60, y - 2, W - 2 * X0 - 120, ROW_H + 2, C_BAR);
    const char *l = i == 3 && opt_confirm ? "Confirm: remove" : pcopt_labels[i];
    text_center(y, l, i == 3 && opt_confirm ? C_ERR : sel ? 0xFFFFFF : C_TEXT, 1);
  }
}

/* the keyboard: 4 rows of 10 characters and one of special keys */
static const char *kb_rows[2][4] = {
  { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._:" },
  { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM._:" },
};
static const char *kb_special[] = { "Shift", "Space", "Del", "OK" };
#define KB_CW 24
#define KB_RH 16
#define KB_X ((W - 10 * KB_CW) / 2)
#define KB_Y 92

static void draw_edit(void) {
  draw_frame("A type  Start confirm  B cancel");
  text_center(LIST_Y, ed_title, C_HEAD, 1);
  /* the field */
  int fw = (COLS - 2) * 8;
  fill((W - fw) / 2 - 4, LIST_Y + 18, fw + 8, 14, C_LINE);
  char shown[64];
  int maxc = COLS - 3;
  int len = (int)strlen(ed_buf);
  snprintf(shown, sizeof(shown), "%s%s", len > maxc ? ed_buf + len - maxc : ed_buf,
           ((int)(now_s() * 2) & 1) ? "_" : " ");
  text((W - fw) / 2, LIST_Y + 21, shown, C_VALUE);
  if (ed_err[0])
    text_center(LIST_Y + 38, ed_err, C_ERR, 1);
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 10; c++) {
      int x = KB_X + c * KB_CW, y = KB_Y + r * KB_RH;
      bool sel = ed_row == r && ed_col == c;
      fill(x + 1, y, KB_CW - 2, KB_RH - 2, sel ? C_BAR : C_LINE);
      char ch[2] = { kb_rows[ed_shift][r][c], 0 };
      text(x + (KB_CW - 8) / 2, y + (KB_RH - 10) / 2 + 1, ch, sel ? 0xFFFFFF : C_TEXT);
    }
  int y = KB_Y + 4 * KB_RH + 2;
  int sw = 10 * KB_CW / 4;
  for (int i = 0; i < 4; i++) {
    int x = KB_X + i * sw;
    bool sel = ed_row == 4 && ed_col == i;
    uint32_t bg = sel ? C_BAR : (i == 0 && ed_shift) ? 0x3A5A30 : C_LINE;
    fill(x + 1, y, sw - 2, KB_RH - 2, bg);
    text(x + (sw - text_len(kb_special[i]) * 8) / 2, y + 4, kb_special[i], sel ? 0xFFFFFF : i == 3 ? C_OK : C_TEXT);
  }
}

static void start_edit(int kind, int pc, const char *initial) {
  ed_kind = kind;
  ed_pc = pc;
  snprintf(ed_buf, sizeof(ed_buf), "%s", initial ? initial : "");
  snprintf(ed_title, sizeof(ed_title), "%s",
           kind == ED_NEW_HOST || kind == ED_HOST ? "The PC's IP address" : "The PC's name");
  ed_err[0] = 0;
  ed_row = 0; ed_col = 0;
  ed_shift = false;
  state = ST_EDIT;
}

static bool host_ok(const char *h) {
  if (!h[0] || strlen(h) > 63) return false;
  for (const char *p = h; *p; p++)
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          *p == '.' || *p == '-' || *p == '_' || *p == ':'))
      return false;
  return true;
}

static int find_pc(const char *h) {
  for (int i = 0; i < n_pcs; i++)
    if (!strcasecmp(pcs[i].host, h)) return i;
  return -1;
}

static void edit_done(void) {
  /* leading and trailing spaces off */
  char *b = ed_buf;
  while (*b == ' ') b++;
  memmove(ed_buf, b, strlen(b) + 1);
  for (int n = (int)strlen(ed_buf); n > 0 && ed_buf[n - 1] == ' '; n--) ed_buf[n - 1] = 0;
  switch (ed_kind) {
  case ED_NEW_HOST:
  case ED_HOST: {
    if (!host_ok(ed_buf)) { snprintf(ed_err, sizeof(ed_err), "Invalid address"); return; }
    int dup = find_pc(ed_buf);
    if (dup >= 0 && dup != ed_pc) { snprintf(ed_err, sizeof(ed_err), "Already there: %.40s", pcs[dup].name); return; }
    if (ed_kind == ED_NEW_HOST) {
      snprintf(ed_new_host, sizeof(ed_new_host), "%s", ed_buf);
      start_edit(ED_NEW_NAME, -1, "");
      return;
    }
    bool same_name = !strcmp(pcs[ed_pc].name, pcs[ed_pc].host);
    if (same_name) snprintf(pcs[ed_pc].name, sizeof(pcs[ed_pc].name), "%s", ed_buf);
    snprintf(pcs[ed_pc].host, sizeof(pcs[ed_pc].host), "%s", ed_buf);
    if (ed_pc == cur_pc) cur_pc = -1, server_ok = false;   /* it must reconnect */
    cfg_save();
    set_status("Address changed", C_OK);
    state = ST_PCOPT;
    return;
  }
  case ED_NEW_NAME: {
    if (n_pcs >= MAX_PCS) { state = ST_PCS; set_status("Too many PCs", C_ERR); return; }
    pc_entry *e = &pcs[n_pcs];
    snprintf(e->host, sizeof(e->host), "%s", ed_new_host);
    snprintf(e->name, sizeof(e->name), "%.31s", ed_buf[0] ? ed_buf : ed_new_host);
    pc_cursor = n_pcs++;
    cfg_save();
    printf("[menu] PC added: %s (%s)\n", e->name, e->host);
    pick_pc(pc_cursor);
    return;
  }
  case ED_NAME:
    snprintf(pcs[ed_pc].name, sizeof(pcs[ed_pc].name), "%.31s", ed_buf[0] ? ed_buf : pcs[ed_pc].host);
    cfg_save();
    set_status("Name changed", C_OK);
    state = ST_PCOPT;
    return;
  }
}

static void edit_cancel(void) {
  state = ed_kind == ED_HOST || ed_kind == ED_NAME ? ST_PCOPT : ST_PCS;
}

static void edit_type(int c) {
  size_t n = strlen(ed_buf);
  ed_err[0] = 0;
  if (n + 1 < sizeof(ed_buf) && (ed_kind == ED_NAME || ed_kind == ED_NEW_NAME ? n < 31 : 1))
    ed_buf[n] = (char)c, ed_buf[n + 1] = 0;
}

static void edit_key(int k) {
  if (k >= MG_KEY_CHAR) { edit_type(k - MG_KEY_CHAR); return; }
  switch (k) {
  case MG_KEY_UP:
  case MG_KEY_DOWN: {
    int r = (ed_row + (k == MG_KEY_UP ? 4 : 1)) % 5;
    if (ed_row == 4) ed_col = ed_col * 10 / 4 + 1;   /* from the special keys to the letters */
    else if (r == 4) ed_col = ed_col * 4 / 10;       /* and back */
    ed_row = r;
    break;
  }
  case MG_KEY_LEFT: ed_col = ed_row == 4 ? (ed_col + 3) % 4 : (ed_col + 9) % 10; break;
  case MG_KEY_RIGHT: ed_col = ed_row == 4 ? (ed_col + 1) % 4 : (ed_col + 1) % 10; break;
  case MG_KEY_DEL: { size_t n = strlen(ed_buf); if (n) ed_buf[n - 1] = 0; ed_err[0] = 0; break; }
  case MG_KEY_BACK: edit_cancel(); break;
  case MG_KEY_ENTER: edit_done(); break;
  case MG_KEY_OK:
    if (ed_row < 4) edit_type(kb_rows[ed_shift][ed_row][ed_col]);
    else if (ed_col == 0) ed_shift = !ed_shift;
    else if (ed_col == 1) edit_type(' ');
    else if (ed_col == 2) { size_t n = strlen(ed_buf); if (n) ed_buf[n - 1] = 0; ed_err[0] = 0; }
    else edit_done();
    break;
  }
  if (ed_row == 4 && ed_col > 3) ed_col = 3;
  if (ed_row < 4 && ed_col > 9) ed_col = 9;
}

/* picks PC i and connects */
static void pick_pc(int i) {
  if (i != cur_pc || !server_ok) {
    pc_gen++;
    was_paired = false;
    server_ok = false;
    n_apps = 0;
    n_items = 0;
    cur_pc = i;
    snprintf(host_buf, sizeof(host_buf), "%s", pcs[i].host);
    opt->host = host_buf;
    snprintf(last_pc, sizeof(last_pc), "%s", pcs[i].host);
    cfg_save();
    printf("[menu] PC chosen: %s (%s)\n", pcs[i].name, pcs[i].host);
    state = ST_CONNECT;
    start_job(JOB_CONNECT);
  } else {
    state = ST_MAIN;                         /* already connected: straight to the apps */
  }
}

static void back_to_pcs(void) {
  state = ST_PCS;
  if (cur_pc >= 0) pc_cursor = cur_pc;
}

static int render_logged;
static void render(void) {
  int logged = render_logged;
  if (logged < 3) render_logged++;
  if (logged < 3) printf("[menu] drawing screen %d\n", state);
  switch (state) {
  case ST_CONNECT: draw_connect(); break;
  case ST_PAIR: draw_pair(); break;
  case ST_MAIN: draw_main(); break;
  case ST_MESSAGE: draw_message(); break;
  case ST_PCS: draw_pcs(); break;
  case ST_PCOPT: draw_pcopt(); break;
  case ST_EDIT: draw_edit(); break;
  }
  present();
}

/* ---------- actions ---------- */

static void launch(int app) {
  snprintf(last_app, sizeof(last_app), "%s", apps[app].name);
  cfg_save();
  draw_starting(apps[app].name);
  present();
  char msg[200];
  opt->genlock = sync_mode == SYNC_GENLOCK || sync_mode == SYNC_GENLOCK_LOW;
  opt->gl_lag = sync_mode == SYNC_GENLOCK;
  opt->guard = sync_mode == SYNC_60HZ;
  opt->pc_w = pcres[pcres_i].w;
  opt->pc_h = pcres[pcres_i].h;
  printf("[menu] starting %s (sync: %s, PC resolution: %s)\n", apps[app].name, sync_names[sync_mode], pcres[pcres_i].name);
  int r = mg_stream_session(&server, opt, apps[app].id, msg, sizeof(msg));
  if (mg_exiting()) return;
  printf("[menu] back to the menu\n");
  render_logged = 0;
  mg_input_set_mode(MG_INPUT_MENU);
  if (r != 0) {
    show_message("Start failed", msg, "", false);
  } else if (opt->genlock && mg_core_genlock_problem()[0]) {
    /* the requested genlock did not work: the stream had no sync, the user must know */
    char l1[240];
    snprintf(l1, sizeof(l1), "The genlock did not work: %s.", mg_core_genlock_problem());
    show_message("Genlock not active", l1,
                 "The stream had no sync. For now choose Sync: 60 Hz in the settings.", false);
  } else if (opt->gl_lag && mg_core_gl_lag_problem()[0]) {
    char l1[240];
    snprintf(l1, sizeof(l1), "The reserve did not work: %s.", mg_core_gl_lag_problem());
    show_message("Genlock reserve not active", l1, "The stream had the genlock low latency (without the extra frame).", false);
  } else {
    state = ST_MAIN;
    if (msg[0]) set_status(msg, C_DIM);
  }
  /* refreshes the running app and the list */
  start_job(JOB_CONNECT);
}

static bool handle_key(int k) {
  if (k == MG_KEY_NONE) return true;
  if (state == ST_EDIT) { edit_key(k); return true; }
  /* outside text fields: Backspace = back, Space = ok, the other letters nothing */
  if (k == MG_KEY_DEL) k = MG_KEY_BACK;
  else if (k == MG_KEY_ENTER) k = MG_KEY_OK;
  else if (k == MG_KEY_CHAR + ' ') k = MG_KEY_OK;
  else if (k >= MG_KEY_CHAR) return true;
  switch (state) {
  case ST_CONNECT:
  case ST_PAIR:
    if (k == MG_KEY_BACK) { pc_gen++; server_ok = false; cur_pc = -1; back_to_pcs(); }
    break;
  case ST_MESSAGE:
    if (k == MG_KEY_OK) {
      if (msg_retry) { state = ST_CONNECT; start_job(JOB_CONNECT); }
      else state = server_ok ? ST_MAIN : ST_PCS;
    } else if (k == MG_KEY_BACK) {
      if (msg_retry || !server_ok) back_to_pcs();
      else state = ST_MAIN;
    }
    break;
  case ST_PCS: {
    int n = n_pcs + 2;
    if (pc_cursor >= n) pc_cursor = n - 1;
    if (k == MG_KEY_UP) pc_cursor = (pc_cursor + n - 1) % n;
    else if (k == MG_KEY_DOWN) pc_cursor = (pc_cursor + 1) % n;
    else if (k == MG_KEY_BACK) return false;
    else if (k == MG_KEY_RIGHT && pc_cursor < n_pcs) {
      ed_pc = pc_cursor; opt_cursor = 0; opt_confirm = false; state = ST_PCOPT;
    } else if (k == MG_KEY_OK) {
      if (pc_cursor < n_pcs) pick_pc(pc_cursor);
      else if (pc_cursor == n_pcs) {
        if (n_pcs >= MAX_PCS) set_status("List full: remove a PC", C_ERR);
        else start_edit(ED_NEW_HOST, -1, "");
      } else return false;
    }
    break;
  }
  case ST_PCOPT:
    if (k == MG_KEY_UP) { opt_cursor = (opt_cursor + N_PCOPT - 1) % N_PCOPT; opt_confirm = false; }
    else if (k == MG_KEY_DOWN) { opt_cursor = (opt_cursor + 1) % N_PCOPT; opt_confirm = false; }
    else if (k == MG_KEY_BACK || k == MG_KEY_LEFT) { state = ST_PCS; pc_cursor = ed_pc; }
    else if (k == MG_KEY_OK) {
      switch (opt_cursor) {
      case 0: pc_cursor = ed_pc; pick_pc(ed_pc); break;
      case 1: start_edit(ED_NAME, ed_pc, pcs[ed_pc].name); break;
      case 2: start_edit(ED_HOST, ed_pc, pcs[ed_pc].host); break;
      case 3:
        if (!opt_confirm) { opt_confirm = true; break; }
        printf("[menu] PC removed: %s (%s)\n", pcs[ed_pc].name, pcs[ed_pc].host);
        if (ed_pc == cur_pc) { cur_pc = -1; server_ok = false; pc_gen++; }
        else if (cur_pc > ed_pc) cur_pc--;
        memmove(&pcs[ed_pc], &pcs[ed_pc + 1], (size_t)(n_pcs - ed_pc - 1) * sizeof(pcs[0]));
        n_pcs--;
        cfg_save();
        set_status("PC removed", C_DIM);
        opt_confirm = false;
        state = ST_PCS;
        pc_cursor = ed_pc < n_pcs ? ed_pc : n_pcs;
        break;
      default: state = ST_PCS; pc_cursor = ed_pc; break;
      }
    }
    break;
  case ST_MAIN: {
    int t = items[cursor].type;
    if (k == MG_KEY_UP || k == MG_KEY_DOWN) {
      int d = k == MG_KEY_UP ? -1 : 1;
      int c = cursor;
      do { c = (c + d + n_items) % n_items; } while (items[c].type == IT_HEADER && c != cursor);
      cursor = c;
    } else if (k == MG_KEY_LEFT || k == MG_KEY_RIGHT) {
      change_setting(t, k == MG_KEY_LEFT ? -1 : 1);
    } else if (k == MG_KEY_BACK) {
      back_to_pcs();
    } else if (k == MG_KEY_OK) {
      if (t == IT_APP) {
        if (job != JOB_NONE) set_status("Wait for the refresh...", C_DIM);
        else launch(items[cursor].app);
      }
      else if (t == IT_EXIT) return false;
      else if (t == IT_PCS) back_to_pcs();
      else if (t == IT_REFRESH) { start_job(JOB_CONNECT); set_status("Refreshing...", C_DIM); }
      else if (t == IT_QUITAPP) {
        set_status(gs_quit_app(&server) == GS_OK ? "App closed on the PC" : "Close failed", C_DIM);
        server.currentGame = 0;
        start_job(JOB_CONNECT);
      } else change_setting(t, 1);
    }
    break;
  }
  }
  return true;
}

int mg_menu_run(options *o, const char *kd) {
  opt = o;
  keydir = kd;

  /* menu.cfg next to the executable */
  char exe[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (len > 0) {
    exe[len] = 0;
    char *slash = strrchr(exe, '/');
    if (slash) *slash = 0;
    snprintf(cfg_path, sizeof(cfg_path), "%s/menu.cfg", exe);
  } else {
    snprintf(cfg_path, sizeof(cfg_path), "menu.cfg");
  }
  /* the sync: from menu.cfg if there (sync=), otherwise from the options (-genlock,
   * -genlock-low, -60hz); without them the genlock (this repository's core has it; with another
   * core the menu says so at the stream's end) */
  sync_mode = o->genlock ? (o->gl_lag ? SYNC_GENLOCK : SYNC_GENLOCK_LOW) : o->guard ? SYNC_60HZ : SYNC_GENLOCK;
  pcres_i = 0;
  for (int i = 1; i < N_PCRES; i++) if (pcres[i].w == o->pc_w && pcres[i].h == o->pc_h) pcres_i = i;
  if (!o->no_menu_cfg)
    cfg_load();
  /* the core is 640 pixels and 60 Hz: from the menu always 640x480 at 60 pictures a second */
  o->width = 640;
  o->height = 480;
  o->fps = 0;

  /* the list of PCs: the configuration's HOST enters the first time (then the menu manages it) */
  if (o->host && o->host[0] && host_ok(o->host) && find_pc(o->host) < 0 && !pcs_saved && n_pcs < MAX_PCS) {
    snprintf(pcs[n_pcs].host, sizeof(pcs[n_pcs].host), "%s", o->host);
    snprintf(pcs[n_pcs].name, sizeof(pcs[n_pcs].name), "PC");
    n_pcs++;
    if (!last_pc[0]) snprintf(last_pc, sizeof(last_pc), "%s", o->host);
    if (!o->no_menu_cfg) cfg_save();
  }
  pc_cursor = 0;
  int lp = find_pc(last_pc);
  if (lp >= 0) pc_cursor = lp;
  else if (!n_pcs) pc_cursor = 0;            /* "Add PC" */

  mg_input_start(o->pads, mg_request_quit);
  mg_input_set_mode(MG_INPUT_MENU);

  state = ST_PCS;
  double last_present = 0;
  bool dirty = true;
  while (!mg_exiting()) {
    int k = mg_input_menu_key(dirty ? 0 : 100);
    if (k != MG_KEY_NONE) dirty = true;
    if (!handle_key(k)) break;
    if (job != JOB_NONE && job_done) { finish_job(); dirty = true; }
    double t = now_s();
    /* redraws when needed, and anyway now and then (animation, timed messages) */
    if (dirty || t - last_present > (state == ST_CONNECT ? 0.15 : state == ST_EDIT ? 0.5 : 1.0)) {
      render();
      last_present = t;
      dirty = false;
    }
  }
  /* a network job still running will end on its own: we don't wait for it */
  mg_input_stop();
  mg_core_overlay_hide();
  return 0;
}
