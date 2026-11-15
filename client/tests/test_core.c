/*
 * A test of the client against a fake H264 core in memory (a thread plays the core:
 * nonce and read index, the status block with its seqlock, the heartbeat), plus the input
 * region written as the core writes it. The fake core follows the one in
 * clients/moonlight/tests/test_mister.c of owenb321/MiSTer_h264_decoder.
 *
 *   test_core STREAM.h264      one PASS / FAIL line per check, "ALL OK" at the end
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#define _GNU_SOURCE
#include <Limelight.h>
#include "core.h"
#include "input.h"
#include "h264ring.h"

#include <opus.h>

#include <math.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RING (1u << 20)

static int fails;
#define CHECK(label, cond) do { int ok_ = (cond); printf("  %s %s\n", ok_ ? "PASS" : "FAIL", label); \
                                if (!ok_) fails++; } while (0)

/* ---------- calls towards Sunshine, recorded ---------- */
enum { C_PAD, C_ARRIVE, C_KEY, C_MOVE, C_BTN, C_SCROLL, C_HSCROLL, C_HRSCROLL };
struct call { int kind, a, b, c, d, e, f, g, h, i; };
static struct call calls[4096];
static volatile int ncalls;
static pthread_mutex_t call_mx = PTHREAD_MUTEX_INITIALIZER;
static void rec(int kind, int a, int b, int c, int d, int e, int f, int g, int h, int i) {
  pthread_mutex_lock(&call_mx);
  if (ncalls < 4096) calls[ncalls++] = (struct call){ kind, a, b, c, d, e, f, g, h, i };
  pthread_mutex_unlock(&call_mx);
}
static volatile int idr_calls;
int LiGetPendingVideoFrames(void) { return 0; }
void LiRequestIdrFrame(void) { __sync_fetch_and_add(&idr_calls, 1); }
int LiSendMultiControllerEvent(short n, short mask, int buttons, unsigned char lt, unsigned char rt,
                               short lx, short ly, short rx, short ry) {
  rec(C_PAD, n, mask, buttons, lt, rt, lx, ly, rx, ry); return 0;
}
int LiSendControllerArrivalEvent(uint8_t n, uint16_t mask, uint8_t type, uint32_t buttons, uint16_t caps) {
  rec(C_ARRIVE, n, mask, type, (int)buttons, caps, 0, 0, 0, 0); return 0;
}
int LiSendKeyboardEvent2(short code, char action, char mods, char flags) {
  rec(C_KEY, code & 0xFF, action, (unsigned char)mods, flags, 0, 0, 0, 0, 0); return 0;
}
int LiSendMouseMoveEvent(short dx, short dy) { rec(C_MOVE, dx, dy, 0, 0, 0, 0, 0, 0, 0); return 0; }
int LiSendMouseButtonEvent(char action, int button) { rec(C_BTN, action, button, 0, 0, 0, 0, 0, 0, 0); return 0; }
int LiSendScrollEvent(signed char c) { rec(C_SCROLL, c, 0, 0, 0, 0, 0, 0, 0, 0); return 0; }
int LiSendHighResScrollEvent(short a) { rec(C_HRSCROLL, a, 0, 0, 0, 0, 0, 0, 0, 0); return 0; }
int LiSendHighResHScrollEvent(short a) { rec(C_HSCROLL, a, 0, 0, 0, 0, 0, 0, 0, 0); return 0; }

static void clear_calls(void) { pthread_mutex_lock(&call_mx); ncalls = 0; pthread_mutex_unlock(&call_mx); }
static int find(int kind, int a, int b) {
  for (int i = 0; i < ncalls; i++)
    if (calls[i].kind == kind && (a == -999 || calls[i].a == a) && (b == -999 || calls[i].b == b)) return i;
  return -1;
}
static int count(int kind) { int k = 0; for (int i = 0; i < ncalls; i++) k += calls[i].kind == kind; return k; }
static int last_pad(int n) { for (int i = ncalls - 1; i >= 0; i--) if (calls[i].kind == C_PAD && calls[i].a == n) return i; return -1; }

/* ---------- the fake core ---------- */
static uint64_t ctrl[512];
static uint64_t *ringmem;
static pthread_mutex_t core_mx = PTHREAD_MUTEX_INITIALIZER;
static struct {
  int booted, sess, stop, dead, il;
  uint32_t seen, rw, seq, ack, fe_err, refresh, ver, raster_w, pick, skip;
  uint64_t w87;
} core;
struct sess { uint32_t start; uint8_t *b; size_t n, cap; };
static struct sess sessions[16];
static int nsess;

static uint32_t lo(unsigned w) { return (uint32_t)ctrl[w]; }
static void put(unsigned w, unsigned lsb, unsigned width, uint64_t v) {
  uint64_t m = width >= 64 ? ~0ull : ((1ull << width) - 1u);
  ctrl[w] = (ctrl[w] & ~(m << lsb)) | ((v & m) << lsb);
}
#define PUT(F, v) put(H264RING_##F##_W, H264RING_##F##_LSB, H264RING_##F##_WIDTH, (v))

static void core_status(void) {
  core.seq++;
  put(3, 0, 32, core.seq);
  __sync_synchronize();
  ctrl[0] = H264RING_MAGIC;
  ctrl[1] = (core.ver ? core.ver : H264RING_VERSION) | ((uint64_t)H264RING_CORE_WORDS << 16) | ((uint64_t)H264RING_PROG_WORD << 32) |
            ((uint64_t)H264RING_CORE2_WORD << 48);
  ctrl[2] = H264RING_RING_BYTE | ((uint64_t)RING << 32);
  put(3, 32, 32, H264RING_MAX_PAYLOAD);
  PUT(STATE, core.sess ? H264RING_S_PLAYING : H264RING_S_NO_SESSION);
  PUT(LIVE, core.sess ? H264RING_L_SESSION : 0);
  ctrl[5] = core.rw | ((uint64_t)core.ack << 32);
  PUT(FE_ERR, core.fe_err);
  PUT(DISP_PICK, core.pick);
  PUT(DISP_SKIP, core.skip);
  PUT(RASTER_IL, core.il);
  PUT(OSD_REFRESH, core.refresh);
  PUT(CAP_MAX_MBS, 3600);
  PUT(CAP_NATIVE_H, 480);
  PUT(CAP_MB_RATE, 108000);
  ctrl[87] = core.w87;
  PUT(RASTER_W, core.raster_w);
  PUT(RASTER_H, core.raster_w ? 480 : 0);
  uint32_t g = lo(H264RING_W_OVL_GEN_LO);
  PUT(OVL_GEN, g);
  PUT(OVL_STATE, (lo(H264RING_W_OVL_CTL) >> H264RING_OVL_CTL_ENABLE_BIT) & 1);
  __sync_synchronize();
  put(31, 0, 32, core.seq);
}

static void core_step(void) {
  if (core.dead)
    return;
  uint32_t wi = lo(H264RING_W_WRITE_INDEX), nonce = lo(H264RING_W_NONCE);
  if (!core.booted) {
    core.seen = nonce;
    core.booted = 1;
  } else if (nonce != core.seen) {
    core.seen = nonce;
    core.sess = 1;
    core.rw = wi & ~15u;
    core.ack = nonce;
    if (nsess < 16)
      sessions[nsess++].start = core.rw;
  } else if (core.sess && nsess > 0) {
    struct sess *s = &sessions[nsess - 1];
    while (core.rw != wi) {
      if (s->n == s->cap) {
        s->cap = s->cap ? 2 * s->cap : (1u << 20);
        s->b = realloc(s->b, s->cap);
      }
      s->b[s->n++] = (uint8_t)(ringmem[(core.rw % RING) / 8] >> (8 * (core.rw % 8)));
      core.rw++;
    }
  }
  core_status();
}

static void *core_thread(void *u) {
  while (!core.stop) {
    pthread_mutex_lock(&core_mx);
    core_step();
    pthread_mutex_unlock(&core_mx);
    usleep(100);
  }
  return NULL;
}

static void be_barrier(void *u) { __sync_synchronize(); sched_yield(); }

static void settle(void) {
  for (int i = 0; i < 2000; i++) {
    pthread_mutex_lock(&core_mx);
    int done = core.rw == lo(H264RING_W_WRITE_INDEX);
    pthread_mutex_unlock(&core_mx);
    if (done) return;
    usleep(1000);
  }
}

/* a session's records: type and payload */
struct rec_info { int type; uint32_t off, len; };
static int parse_records(const struct sess *s, struct rec_info *out, int cap) {
  int k = 0;
  size_t p = 0;
  while (p + 16 <= s->n && k < cap) {
    const uint8_t *h = s->b + p;
    int type = h[0];
    uint32_t len = (uint32_t)h[4] | ((uint32_t)h[5] << 8) | ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
    uint32_t n = 16u + ((len + 15u) & ~15u);
    if (type == H264RING_T_PAD) {
      p = (p + RING) & ~(size_t)(RING - 1); /* up to the ring's end */
      continue;
    }
    if (p + n > s->n) break;
    out[k++] = (struct rec_info){ type, (uint32_t)(p + 16), len };
    p += n;
  }
  return k;
}

/* ---------- the input region, as the core writes it ---------- */
static uint32_t in_seq, evc, mdx, mdy, mwh;
static uint8_t mbtn, osd;
static uint64_t pads[6];
static uint16_t q[32];
static void publish(void) {
  in_seq++;
  ctrl[71] = 0; /* in_tail first: a copy taken halfway does not match */
  __sync_synchronize();
  for (int n = 0; n < 6; n++) ctrl[49 + n] = pads[n];
  ctrl[55] = mdx | ((uint64_t)mdy << 32);
  for (int w = 0; w < 8; w++)
    ctrl[56 + w] = q[4 * w] | ((uint64_t)q[4 * w + 1] << 16) | ((uint64_t)q[4 * w + 2] << 32) | ((uint64_t)q[4 * w + 3] << 48);
  ctrl[64] = mwh | ((uint64_t)mbtn << 32) | ((uint64_t)osd << 40);
  ctrl[48] = in_seq | ((uint64_t)evc << 32);
  __sync_synchronize();
  ctrl[71] = in_seq;
}
static void ev(uint16_t e) { q[evc % 32] = e; evc++; }
static void key(unsigned ext, unsigned code, int down) { ev((uint16_t)((down ? 0x200 : 0) | (ext ? 0x100 : 0) | code)); }
static void osd_ev(int open) { osd = (uint8_t)open; ev((uint16_t)(0x8000 | open)); }
static void action(int n) { ev((uint16_t)(0xC000 | n)); }
static void mouse(int x, int y, int w, uint8_t b) {
  mdx += (uint32_t)x; mdy += (uint32_t)y; mwh += (uint32_t)w;
  if (b != mbtn) { mbtn = b; ev((uint16_t)(0x4000 | b)); }
}
static void pad(int n, unsigned buttons, int lx, int ly) {
  pads[n] = buttons | ((uint64_t)(uint8_t)lx << 32) | ((uint64_t)(uint8_t)ly << 40);
}
static void poll(void) { publish(); mg_input_poll_once(2); mg_core_tick(); }

static volatile int quits, gones, refusals;
static void on_quit(void) { quits++; }
static void on_gone(void) { gones++; }
static void on_refused(const char *why) { refusals++; }

static uint32_t rumble_word(int padn) { return (lo(H264RING_W_RUMBLE + padn / 2) >> (16 * (padn % 2))) & 0xFFFF; }

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: test_core STREAM.h264\n");
    return 2;
  }
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 2; }
  fseek(f, 0, SEEK_END);
  long fn = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = malloc((size_t)fn);
  if (fread(buf, 1, (size_t)fn, f) != (size_t)fn) return 2;
  fclose(f);
  size_t nau = h264ring_split_annexb(buf, (size_t)fn, NULL, 0);
  struct h264ring_au *au = calloc(nau, sizeof *au);
  h264ring_split_annexb(buf, (size_t)fn, au, nau);
  bool *is_idr = calloc(nau, sizeof(bool));
  for (size_t i = 0; i < nau; i++) {
    const uint8_t *p = buf + au[i].off;
    for (uint32_t k = 0; k + 3 < au[i].len; k++)
      if (p[k] == 0 && p[k + 1] == 0 && p[k + 2] == 1 && (p[k + 3] & 0x1F) == 5) { is_idr[i] = true; break; }
  }
  printf("test_core: %zu access units, the first %s IDR\n", nau, is_idr[0] ? "e'" : "not");

  ringmem = calloc(RING / 8, 8);
  pthread_t ct;
  core_status();
  pthread_create(&ct, NULL, core_thread, NULL);
  usleep(2000);

  struct h264ring_backend be;
  memset(&be, 0, sizeof be);
  be.ctrl = ctrl; be.ring = ringmem; be.ring_size = RING; be.barrier = be_barrier;
  h264ring *r = NULL;
  printf("Attaching and menu\n");
  CHECK("attaches to the fake core", h264ring_attach(&r, &be) == 0 && r);
  if (!r) return 1;
  CHECK("opens a session for the menu", mg_core_attach(r, on_gone, on_refused) == 0 && nsess == 1);

  /* overlay */
  uint32_t fb_len = 4u << 20;
  uint8_t *fbmem = calloc(1, fb_len);
  mg_core_use_fb(fbmem, H264RING_FB_BYTE, fb_len);
  static uint32_t canvas[320 * 240];
  for (int i = 0; i < 320 * 240; i++) canvas[i] = (uint32_t)(i * 2654435761u) & 0xFFFFFF;
  CHECK("shows the menu in the overlay", mg_core_overlay_present(canvas, 320, 240) == 0);
  uint32_t base1 = lo(H264RING_W_OVL_BASE);
  CHECK("descriptor: on, the whole 720x480 raster, 8888",
        (lo(H264RING_W_OVL_CTL) >> H264RING_OVL_CTL_ENABLE_BIT & 1) && lo(H264RING_W_OVL_SIZE) == ((480u << 16) | 720u) &&
        ((lo(H264RING_W_OVL_CTL) >> H264RING_OVL_CTL_FMT_LSB) & 3) == H264RING_OVL_FMT_8888 && lo(H264RING_W_OVL_STRIDE) == 2880);
  {
    /* canvas: uniform colour columns in 32-pixel blocks */
    static uint32_t bands[320 * 240];
    for (int y = 0; y < 240; y++)
      for (int x = 0; x < 320; x++) bands[y * 320 + x] = (x / 32) * 0x101010u + (y < 120 ? 0x800000u : 0);
    mg_core_overlay_present(bands, 320, 240);
    const uint32_t *pg = (const uint32_t *)(fbmem + (lo(H264RING_W_OVL_BASE) - H264RING_FB_BYTE));
    int ok = 1;
    for (int y = 0; y < 480 && ok; y += 5)
      for (int x = 0; x < 720 && ok; x++) {
        int sx = x * 320 / 720;
        if (sx % 32 < 2 || sx % 32 > 29) continue; /* near the boundary: blended */
        ok = pg[y * 720 + x] == (bands[(y / 2) * 320 + sx] | 0xFF000000u);
      }
    CHECK("the canvas covers the whole width (320 -> 720), doubled lines, opaque", ok);
    CHECK("edges: the first and last columns are the canvas's",
          pg[0] == (bands[0] | 0xFF000000u) && pg[719] == (bands[319] | 0xFF000000u) &&
          pg[479 * 720 + 719] == (bands[239 * 320 + 319] | 0xFF000000u));
    uint32_t mid = pg[100 * 720 + (32 * 720 + 160) / 320]; /* on the boundary between two bands: in between */
    CHECK("at the boundary between two columns the colour is interpolated",
          (mid & 0xFF) > (bands[100 * 320 + 31] & 0xFF) && (mid & 0xFF) < (bands[100 * 320 + 32] & 0xFF));
  }
  mg_core_overlay_present(canvas, 320, 240);
  base1 = lo(H264RING_W_OVL_BASE);
  usleep(5000);
  mg_core_overlay_present(canvas, 320, 240);
  CHECK("the next drawing goes to the other page", lo(H264RING_W_OVL_BASE) != base1);

  /* the core in core/: a 640-wide raster (word 80) -> 640 overlay, exactly x2 without blending */
  core.raster_w = 640; usleep(2000);
  mg_core_overlay_present(canvas, 320, 240);
  {
    const uint32_t *pg = (const uint32_t *)(fbmem + (lo(H264RING_W_OVL_BASE) - H264RING_FB_BYTE));
    int ok = lo(H264RING_W_OVL_SIZE) == ((480u << 16) | 640u) && lo(H264RING_W_OVL_STRIDE) == 2560;
    for (int y = 0; y < 480 && ok; y += 3)
      for (int x = 0; x < 640 && ok; x++)
        ok = pg[y * 640 + x] == (canvas[(y / 2) * 320 + x / 2] | 0xFF000000u);
    CHECK("640 raster: 640x480 overlay, each canvas pixel exactly x2", ok);
  }
  core.raster_w = 0; usleep(2000);
  mg_core_overlay_present(canvas, 320, 240);
  CHECK("raster 720 again: 720 overlay", lo(H264RING_W_OVL_SIZE) == ((480u << 16) | 720u));

  mg_core_info ci;
  mg_core_get_info(&ci);
  CHECK("Refresh Rate Auto: Sunshine is asked for 59.94", ci.refresh_x100 == 5994 && ci.max_mbs == 3600);
  core.refresh = 2; usleep(2000);
  mg_core_get_info(&ci);
  CHECK("Refresh Rate 60 Hz: 60 is asked", ci.refresh_x100 == 6000);
  core.refresh = 3; usleep(2000);
  mg_core_get_info(&ci);
  CHECK("Refresh Rate 50 Hz: 50 is asked (576 lines)", ci.refresh_x100 == 5000 && ci.raster_h == 576);
  mg_core_overlay_present(canvas, 320, 240);
  {
    const uint32_t *pg = (const uint32_t *)(fbmem + (lo(H264RING_W_OVL_BASE) - H264RING_FB_BYTE));
    CHECK("at 576 lines the overlay is 720x576, the menu centred",
          lo(H264RING_W_OVL_SIZE) == ((576u << 16) | 720u) && pg[47 * 720 + 300] == (canvas[0] | 0xFF000000u) &&
          pg[48 * 720] == (canvas[0] | 0xFF000000u) && pg[47 * 720] == (canvas[0] | 0xFF000000u));
  }
  core.refresh = 0; core.il = 1; usleep(2000);
  mg_core_get_info(&ci);
  CHECK("interlaced raster read", ci.interlaced && ci.refresh_x100 == 5994);
  core.ver = 0x104; core.w87 = (1u << 13) | (1u << 4); usleep(2000);
  mg_core_get_info(&ci);
  CHECK("core in core/ with 240p Playback: 59.91 is asked", ci.p240 && !ci.interlaced && ci.refresh_x100 == 5991);
  core.w87 = (1u << 13) | (2u << 11) | (1u << 4); usleep(2000);
  mg_core_get_info(&ci);
  CHECK("... at 50 Hz (288p): 49.99", ci.p240 && ci.refresh_x100 == 4999);
  core.w87 = (1u << 13) | (1u << 11) | (1u << 4); usleep(2000);
  mg_core_get_info(&ci);
  CHECK("this repository's core (60.000 Hz) with 240p: 60 is asked", ci.p240 && ci.refresh_x100 == 6000);
  core.w87 = (1u << 13) | (1u << 11); usleep(2000);
  mg_core_get_info(&ci);
  CHECK("... and without 240p: 480i at 60", !ci.p240 && ci.interlaced && ci.refresh_x100 == 6000);
  core.w87 = (1u << 13); usleep(2000);
  mg_core_get_info(&ci);
  CHECK("... without 240p: 480i at 59.94", !ci.p240 && ci.interlaced && ci.refresh_x100 == 5994);
  core.ver = 0; core.w87 = 0; usleep(2000);

  /* menu: keys */
  mg_input_setup(2, on_quit);
  mg_input_set_mode(MG_INPUT_MENU);
  pad(0, H264RING_PAD_A, 0, 0); poll();
  CHECK("a key already pressed at start does not count", mg_input_menu_key(0) == MG_KEY_NONE);
  pad(0, 0, 0, 0); poll();
  pad(1, H264RING_PAD_DOWN, 0, 0); poll();
  CHECK("menu: pad 2's d-pad down", mg_input_menu_key(0) == MG_KEY_DOWN);
  pad(1, 0, 0, 0); poll();
  key(1, 0x75, 1); key(1, 0x75, 0); poll();
  CHECK("menu: arrow up", mg_input_menu_key(0) == MG_KEY_UP);
  key(0, 0x5A, 1); key(0, 0x5A, 0); poll();
  CHECK("menu: Enter", mg_input_menu_key(0) == MG_KEY_ENTER);
  key(0, 0x76, 1); key(0, 0x76, 0); poll();
  CHECK("menu: Esc", mg_input_menu_key(0) == MG_KEY_BACK);
  pad(0, H264RING_PAD_B, 0, 0); poll(); pad(0, 0, 0, 0); poll();
  CHECK("menu: B = back", mg_input_menu_key(0) == MG_KEY_BACK);
  pad(0, H264RING_PAD_RIGHT, 0, 0); poll();
  int k1 = mg_input_menu_key(0);
  usleep(450000); poll();
  int k2 = mg_input_menu_key(0);
  pad(0, 0, 0, 0); poll();
  CHECK("menu: right held repeats", k1 == MG_KEY_RIGHT && k2 == MG_KEY_RIGHT);
  CHECK("in the menu nothing goes to the PC", ncalls == 0);

  /* ---- stream ---- */
  printf("Stream\n");
  mg_core_set_rhythm(true);
  mg_core_stream_begin();
  usleep(2000);
  CHECK("stream start: new session (flush), overlay off", nsess == 2 &&
        !((lo(H264RING_W_OVL_CTL) >> H264RING_OVL_CTL_ENABLE_BIT) & 1));
  mg_input_set_mode(MG_INPUT_STREAM);
  clear_calls(); poll();
  CHECK("announces 2 Xbox pads with rumble", count(C_ARRIVE) == 2 && calls[find(C_ARRIVE, 0, 3)].e == LI_CCAP_RUMBLE);
  pad(0, H264RING_PAD_A | H264RING_PAD_LT, 0, 0); clear_calls(); poll();
  { int i = last_pad(0);
    CHECK("pad 1: A and LT", i >= 0 && calls[i].c == A_FLAG && calls[i].d == 255 && calls[i].b == 3); }
  pad(0, 0, 0, 0); poll();
  pad(1, H264RING_PAD_UP, 0, -100); clear_calls(); poll();
  { int i = last_pad(1);
    CHECK("pad 2: stick up, the d-pad the MiSTer derives from it removed", i >= 0 && calls[i].c == 0 && calls[i].g == 25800); }
  pad(1, 0, 0, 0); poll();
  clear_calls();
  key(0, 0x1C, 1); key(0, 0x1C, 0); key(1, 0x75, 1); key(1, 0x75, 0); key(0, 0x77, 0); poll();
  CHECK("keyboard: A down and up", find(C_KEY, 'A', KEY_ACTION_DOWN) >= 0 && find(C_KEY, 'A', KEY_ACTION_UP) >= 0);
  { int i = find(C_KEY, 0x26, KEY_ACTION_DOWN);
    CHECK("keyboard: arrow up with the extended flag", i >= 0 && (calls[i].c & MODIFIER_EXTENDED)); }
  CHECK("release without a press ignored (NumLock)", find(C_KEY, 0x90, -999) < 0);
  clear_calls();
  mouse(5, 3, 1, 1); poll();
  CHECK("mouse: movement (Y inverted), left button, wheel",
        find(C_MOVE, 5, -3) >= 0 && find(C_BTN, BUTTON_ACTION_PRESS, BUTTON_LEFT) >= 0 && find(C_SCROLL, -1, -999) >= 0);
  mouse(0, 0, 0, 0); poll();

  /* rumble */
  mg_rumble(0, 0xFF00, 0x8000);
  mg_rumble(1, 0x1000, 0);
  poll();
  CHECK("rumble of pads 1 and 2 written to the core", rumble_word(0) == 0xFF80 && rumble_word(1) == 0x1000);
  mg_rumble_stop_all(); poll();
  CHECK("rumble stopped", rumble_word(0) == 0 && rumble_word(1) == 0);

  /* mouse mode */
  clear_calls();
  pad(0, H264RING_PAD_START, 0, 0); poll();
  usleep(1050000); poll();
  CHECK("Start held 1 s: rumble pulse", rumble_word(0) == 0x6060);
  pad(0, 0, 0, 0); poll();
  clear_calls();
  pad(0, H264RING_PAD_A, 127, 0); poll(); usleep(20000); poll();
  CHECK("mouse mode: A = left button, stick = pointer, no pad",
        find(C_BTN, BUTTON_ACTION_PRESS, BUTTON_LEFT) >= 0 && find(C_MOVE, -999, -999) >= 0 && last_pad(0) < 0);
  pad(0, 0, 0, 0); poll();
  usleep(200000); poll();
  CHECK("pulse over", rumble_word(0) == 0);
  pad(0, H264RING_PAD_START, 0, 0); poll(); usleep(1050000); poll();
  pad(0, 0, 0, 0); poll();
  clear_calls();
  pad(0, H264RING_PAD_A, 0, 0); poll();
  CHECK("Start held again: the pad is back", last_pad(0) >= 0 && calls[last_pad(0)].c == A_FLAG);
  pad(0, 0, 0, 0); poll();

  /* Guide and OSD */
  clear_calls();
  pad(0, H264RING_PAD_BACK | H264RING_PAD_START, 0, 0); poll();
  CHECK("Select+Start = Guide", last_pad(0) >= 0 && calls[last_pad(0)].c == SPECIAL_FLAG);
  pad(0, 0, 0, 0); poll();
  key(0, 0x1C, 1); poll();
  clear_calls();
  osd_ev(1); poll();
  CHECK("OSD open: the held key is released", find(C_KEY, 'A', KEY_ACTION_UP) >= 0);
  clear_calls();
  pad(0, H264RING_PAD_X, 0, 0); key(0, 0x32, 1); poll();
  CHECK("with the OSD open nothing passes", ncalls == 0);
  pad(0, 0, 0, 0); key(0, 0x32, 0);
  action(0); osd_ev(0); poll();
  { int i = last_pad(0);
    CHECK("Send Home from the OSD: Guide on pad 1", i >= 0 && (calls[i].c & SPECIAL_FLAG)); }
  usleep(150000); poll(); poll();
  { int i = last_pad(0);
    CHECK("... and then released", i >= 0 && calls[i].c == 0); }
  CHECK("no release of B (pressed with the OSD open)", find(C_KEY, 'B', KEY_ACTION_UP) < 0);
  quits = 0;
  osd_ev(1); action(1); osd_ev(0); poll();
  CHECK("Program menu from the OSD: back to the menu", quits == 1);

  /* exits */
  quits = 0;
  pad(0, H264RING_PAD_BACK | H264RING_PAD_START | H264RING_PAD_LB | H264RING_PAD_RB, 0, 0); poll();
  CHECK("Select+Start+LB+RB: exit", quits == 1);
  pad(0, 0, 0, 0); poll();
  quits = 0; clear_calls();
  key(0, 0x14, 1); key(0, 0x11, 1); key(0, 0x12, 1); key(0, 0x15, 1); key(0, 0x15, 0); poll();
  CHECK("Ctrl+Alt+Shift+Q: Q does not go to the PC and does not exit at once", find(C_KEY, 'Q', -999) < 0 && quits == 0);
  key(0, 0x14, 0); key(0, 0x11, 0); key(0, 0x12, 0); poll();
  CHECK("... exits when the modifiers come back up", quits == 1);

  /* ---- video and audio ---- */
  printf("Video and audio\n");
  CHECK("setup H.264 640x480", mg_video_callbacks.setup(VIDEO_FORMAT_H264, 640, 480, 60, NULL, 0) == 0);
  CHECK("refuses HEVC", mg_video_callbacks.setup(VIDEO_FORMAT_H265, 640, 480, 60, NULL, 0) != 0);
  CHECK("refuses 1920x1080 (beyond the core's macroblocks)",
        mg_video_callbacks.setup(VIDEO_FORMAT_H264, 1920, 1080, 30, NULL, 0) != 0);
  OPUS_MULTISTREAM_CONFIGURATION cfg = { 48000, 2, 1, 1, 240, { 0, 1 } };
  CHECK("Opus audio 48 kHz stereo", mg_audio_callbacks.init(AUDIO_CONFIGURATION_STEREO, &cfg, NULL, 0) == 0);
  int err;
  OpusEncoder *enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
  int16_t pcm[240 * 2];
  unsigned char pkt[1500];
  double ph = 0;
  for (int k = 0; k < 240; k++) { ph += 2 * M_PI * 440 / 48000; pcm[2 * k] = pcm[2 * k + 1] = (int16_t)(8000 * sin(ph)); }
  int plen = opus_encode(enc, pcm, 240, pkt, sizeof pkt);
  mg_audio_callbacks.decodeAndPlaySample((char *)pkt, plen);
  mg_core_counters cn;
  mg_core_get_counters(&cn);
  CHECK("audio before the first IDR: not written", cn.aud_early == 1 && cn.aud == 0);

  size_t first_p = 0;
  while (first_p < nau && is_idr[first_p]) first_p++;
  LENTRY e;
  DECODE_UNIT du;
#define SUBMIT(i) (memset(&e, 0, sizeof e), e.data = (char *)buf + au[i].off, e.length = (int)au[i].len, \
                   memset(&du, 0, sizeof du), du.frameType = is_idr[i] ? FRAME_TYPE_IDR : FRAME_TYPE_PFRAME, \
                   du.fullLength = (int)au[i].len, du.bufferList = &e, mg_video_callbacks.submitDecodeUnit(&du))
  CHECK("a P before the IDR: dropped, asks for the IDR", first_p < nau && SUBMIT(first_p) == DR_NEED_IDR);
  int bad = 0;
  size_t sent = 0;
  for (size_t i = 0; i < nau; i++) {
    if (SUBMIT(i) != DR_OK) bad++;
    sent++;
    if (i % 4 == 0) mg_audio_callbacks.decodeAndPlaySample((char *)pkt, plen);
    if (i % 40 == 39) mg_audio_callbacks.decodeAndPlaySample(NULL, 0);
  }
  CHECK("every access unit accepted", bad == 0);
  settle();
  {
    static struct rec_info ri[20000];
    int nr = parse_records(&sessions[1], ri, 20000);
    size_t v = 0, a = 0;
    int same = 1;
    for (int i = 0; i < nr; i++) {
      if (ri[i].type == H264RING_T_VIDEO_AU) {
        if (v < nau && (ri[i].len != au[v].len || memcmp(sessions[1].b + ri[i].off, buf + au[v].off, au[v].len)))
          same = 0;
        v++;
      } else if (ri[i].type == H264RING_T_AUDIO) {
        a++;
        if (ri[i].len != 240 * 4) same = 0;
      }
    }
    printf("    (%d records: %zu video, %zu audio)\n", nr, v, a);
    CHECK("in the ring the access units, identical and in order", v == nau && same);
    CHECK("and the audio after the first IDR (lost packets too, concealed)", a > 0);
  }
  mg_core_get_counters(&cn);
  CHECK("counters: AUs and audio", cn.au == nau && cn.aud > 0 && cn.aud_plc > 0);

  /* a new stream (two more sessions), from an IDR */
  mg_core_stream_end();
  mg_core_stream_begin();
  usleep(2000);
  {
    size_t idr0 = 0;
    while (idr0 < nau && !is_idr[idr0]) idr0++;
    for (size_t i = idr0; i < idr0 + 3 && i < nau; i++) SUBMIT(i);
  }

  /* decode error */
  idr_calls = 0;
  usleep(300000); /* beyond the tolerance after the last IDR */
  core.fe_err += 3;
  usleep(3000);
  mg_core_poll();
  CHECK("decoder error: asks for an IDR", idr_calls == 1);
  CHECK("the P frames after the error are dropped until the IDR", SUBMIT(first_p) == DR_NEED_IDR);
  size_t idr2 = 0;
  for (size_t i = nau; i-- > 0;) if (is_idr[i]) { idr2 = i; break; }
  CHECK("the IDR restarts", SUBMIT(idr2) == DR_OK);

  /* end of the stream */
  mg_audio_callbacks.cleanup();
  mg_video_callbacks.cleanup();
  mg_input_set_mode(MG_INPUT_MENU);
  mg_core_stream_end();
  usleep(3000);
  CHECK("stream end: new session (black screen), still open for the menu", nsess == 5);
  CHECK("the menu is back in the overlay", mg_core_overlay_present(canvas, 320, 240) == 0 &&
        ((lo(H264RING_W_OVL_CTL) >> H264RING_OVL_CTL_ENABLE_BIT) & 1));

  /* core unloaded */
  printf("Core unloaded\n");
  core.dead = 1;
  for (int i = 0; i < 40 && !gones; i++) { mg_core_poll(); usleep(50000); }
  CHECK("heartbeat stopped: the program exits", gones == 1);

  core.stop = 1;
  pthread_join(ct, NULL);
  printf(fails ? "test_core: %d FAILED\n" : "test_core: ALL OK\n", fails);
  return fails ? 1 : 0;
}
