/*
 * Output on the MiSTer's H264 core (an H.264 decoder in the FPGA, owenb321/MiSTer_h264_decoder):
 * each Sunshine decode unit becomes a VIDEO_AU record in the DDR3 ring, the Opus audio decoded
 * here an AUDIO record, the menu an overlay in Linux's framebuffer that the core lays over the
 * video. Interface: third_party/h264ring (h264ring.h).
 *
 * The handling of the session, the errors and the core's heartbeat follows the project's
 * reference Moonlight client (clients/moonlight/src/mister/mister.c):
 * - LOW_LATENCY | GRAPHICS on every access unit, without PTS: the core shows each picture as
 *   soon as it is decoded and plays the audio as soon as it arrives.
 * - A single mutex around every call into the library (it is not thread-safe); a full ring is
 *   waited on with the try_ calls and a pause outside the mutex.
 * - Decode errors (the core's counters) -> LiRequestIdrFrame. A lost session (Reset from the
 *   OSD) is reopened; a refused stream closes it.
 * - Core unloaded (another core loaded from the OSD): the status block's heartbeat stops; the
 *   program exits.
 * - The audio waits for the session's first IDR: the stream's mode (low latency) is decided by
 *   its first VIDEO_AU.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#define _GNU_SOURCE
#include "core.h"

#include "h264ring.h"
#include "h264ring_cavlc.h"

#include <opus_multistream.h>

#include <errno.h>
#include <glob.h>
#include <math.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>

#define AU_FLAGS        (H264RING_F_LOW_LATENCY | H264RING_F_GRAPHICS)
#define VIDEO_WAIT_MS   500     /* ring full for this long: the decoder is behind, the AU is dropped */
#define AUDIO_WAIT_MS   20      /* the audio is dropped rather than waited for (the core covers the gap) */
#define LAUNCH_MS       15000
#define SESSION_MS      2000
#define FLUSH_MS        1000
#define ALIVE_MS        1500    /* no heartbeat for this long: the core is gone */
#define IDR_RETRY_MS    1000
#define IDR_GRACE_MS    250     /* errors right after an IDR: they concern the pictures before it */
#define RESTART_MS      1000
#define W_EARLY         1
#define OVL_MAX_H       576

static h264ring *ring;
static pthread_mutex_t ring_mx = PTHREAD_MUTEX_INITIALIZER;
static void (*gone_cb)(void);
static void (*refused_cb)(const char *why);
static char refused_why[160];

static bool session_ok, streaming, video_started, need_idr, gone;
static uint32_t err_base, refused_base, beat;
static uint64_t beat_ms, idr_asked_ms, idr_written_ms, restart_ms;
static volatile bool restarting, osd_open;
static bool cavlc_refused;

static uint8_t *au_buf;
static size_t au_cap;

static OpusMSDecoder *opus;
static int16_t *pcm;
static int pcm_frames, pcm_channels;
static int16_t *pcm2;

static mg_core_counters n;

/* ---------- the pictures' rhythm ----------
 * In low latency the core takes, at line ~500 of each frame, the oldest decoded picture; if
 * another already waits behind it, the oldest is dropped (line 490). If the pictures arrive near
 * that moment, a delay of a few ms (Wi-Fi, decoding) makes them land now before now after: a
 * repeated frame and then a dropped picture, in bursts, every time the PC's rhythm (59.95 for
 * example) brings the arrivals onto that point.
 *
 * NOTE (0.1.5): the core closes a picture only when the start of the next one arrives (the close
 * at the record's end, "P3e", is not there yet): as long as that holds pacing cannot work (it
 * would wait for the pickup of a picture that completes only by writing the next) and it is off
 * by default.
 *
 * Pacing: a picture is written to the core only when the previous one was already taken (or
 * dropped). After a late arrival (a repeated frame) the following pictures are written right
 * after each pickup, with a whole frame to decode them: no more bursts, only the repeat that the
 * rhythm difference with the PC makes unavoidable. A picture is written anyway if a newer one
 * already waits behind it (no delay builds up) or after PACE_MAX_MS (a lost picture).
 *
 * With -stats: each skipped / repeated picture at once, and a summary every 10 s. */
#define RITMO_PERIOD_S 10
#define PACE_MAX_MS    50
static bool pacing = false;
static struct {
  bool on;
  uint32_t pick_last, skip_last, starve_last;
  uint64_t t0_ms, t_last_ms;
  /* current window */
  unsigned frames, held, held_us_max, forced, lost;
  uint64_t held_us_sum;
  uint32_t rtp_first, rtp_last;
  int64_t jit_min, jit_max;    /* arrival - the PC's time (rtp), us */
  unsigned skips, starves, picks;
} rt;
static uint32_t pace_written, pace_base;   /* written since the stream's start; taken + dropped at the start */

void mg_core_set_rhythm(bool on) { rt.on = on; }
void mg_core_set_pacing(bool on) { pacing = on; }
static bool guard_on, genlock_req;

/* THE TRACE (with -stats): one line per picture and one per core frame, in memory; written to
 * trace.csv next to the program at the stream's end (writing to the SD during the stream can
 * block). It lets a real session be replayed away from the MiSTer (client/tools/genlock_replay.py). */
static char *tr_buf;
static size_t tr_len, tr_cap;
static bool tr_on;
static void tr_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tr_printf(const char *fmt, ...) {
  static pthread_mutex_t tr_mx = PTHREAD_MUTEX_INITIALIZER;   /* the video thread and the grid's */
  if (!tr_on || !tr_buf)
    return;
  pthread_mutex_lock(&tr_mx);
  if (tr_len + 256 <= tr_cap) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tr_buf + tr_len, tr_cap - tr_len, fmt, ap);
    va_end(ap);
    if (n > 0 && tr_len + (size_t)n < tr_cap) tr_len += (size_t)n;
  }
  pthread_mutex_unlock(&tr_mx);
}
static void tr_begin(void) {
  tr_len = 0;
  if (!tr_buf) { tr_cap = 12u << 20; tr_buf = malloc(tr_cap); }
  tr_on = tr_buf != NULL;
  tr_printf("# type F (picture): w_us,recv_us,rtp,bytes,idr,phv,error,target,margin,D,trim,locked,held_us,v_us\n"
            "# type E (core frame start): s_us,seq,picks,skips,starves,Pm_us,t_ref_us,eoff_us,"
            "tm_now (27 MHz >> 10),tm_start (27 MHz),tm_emit (27 MHz),pictures started,pictures emitted\n");
}
static void tr_end(void) {
  if (!tr_on || !tr_len)
    return;
  tr_on = false;
  char path[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 32);
  if (n <= 0) return;
  path[n] = 0;
  char *sl = strrchr(path, '/');
  if (!sl) return;
  strcpy(sl + 1, "trace.csv");
  FILE *f = fopen(path, "w");
  if (!f) return;
  fwrite(tr_buf, 1, tr_len, f);
  fclose(f);
  printf("[trace] %zu bytes in %s\n", tr_len, path);
}
static char gl_problem[200];           /* why the requested genlock did not work (empty: no problem) */
const char *mg_core_genlock_problem(void) { return gl_problem; }
void mg_core_set_guard(bool on) { guard_on = on; }
void mg_core_set_genlock(bool on) { genlock_req = on; }
/* genlock +1: the core keeps one picture in reserve (word 34 [25], applied: word 45 [2]) */
static bool gl_lag_req;
static char gl_lag_problem[160];
void mg_core_set_gl_lag(bool on) { gl_lag_req = on; }
const char *mg_core_gl_lag_problem(void) { return gl_lag_problem; }

static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}


/* overlay */
static struct h264ring_fb fb;
static bool fb_ok;
static unsigned ovl_fmt, ovl_bpp, ovl_pages, ovl_page;
static uint32_t ovl_page_bytes;
static int ovl_gen, ovl_w, ovl_h;   /* ovl_w: the raster width of the last drawing */
static int ovl_max_w;
static bool ovl_shown;

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

struct h264ring *mg_core_ring(void) { return ring; }
bool mg_core_osd_open(void) { return osd_open; }

static uint32_t err_sum(const struct h264ring_status *st) {
  return (uint32_t)(H264RING_ST(st, FE_ERR) + H264RING_ST(st, RC_ERR));
}

static const char *refusal_name(unsigned code) {
  static const char *const names[] = {
    "none", "colour format (not 4:2:0)", "bit depth (not 8 bits)", "scaling list",
    "interlaced MBAFF", "CAVLC (not CABAC)", "FMO", "8x8 transform",
    "redundant pictures", "slice SP/SI", "data partitioning", "SVC/MVC",
    "size (over 128 macroblocks)",
  };
  return code < sizeof(names) / sizeof(names[0]) ? names[code] : "unknown";
}

static void core_gone_locked(void) {
  if (gone)
    return;
  gone = true;
  session_ok = false;
  printf("[core] the H264 core no longer answers (another core loaded?): exiting\n");
  if (gone_cb)
    gone_cb();
}

static int session_start_locked(void) {
  restart_ms = now_ms();
  restarting = true;
  int rc = h264ring_start_session(ring, SESSION_MS);
  if (rc && n.sessions > 0 && h264ring_alive(ring, ALIVE_MS) != 0) {
    restarting = false;
    core_gone_locked();
    return rc;
  }
  restarting = false;
  if (rc) {
    printf("[core] session not open (%s)\n", strerror(-rc));
    session_ok = false;
    return rc;
  }
  struct h264ring_status st;
  if (h264ring_status(ring, &st) == 0) {
    err_base = err_sum(&st);
    refused_base = (uint32_t)H264RING_ST(&st, FE_REFUSED);
  }
  session_ok = true;
  video_started = false;
  need_idr = true;
  n.sessions++;
  if (n.sessions > 1)
    printf("[core] session reopened (%lu)\n", n.sessions);
  return 0;
}

static void poll_status_locked(void) {
  struct h264ring_status st;
  if (!ring || gone || h264ring_status(ring, &st))
    return;
  uint32_t seq = (uint32_t)H264RING_ST(&st, SEQ_HEAD);
  if (seq != beat || !beat_ms) {
    beat = seq;
    beat_ms = now_ms();
  } else if (now_ms() - beat_ms >= ALIVE_MS) {
    core_gone_locked();
    return;
  }
  osd_open = (H264RING_ST(&st, LIVE) & H264RING_L_OSD_OPEN) != 0;
  if (H264RING_ST(&st, STATE) == H264RING_S_ERROR && session_ok) {
    printf("[core] the core stopped the session: %s at %u\n",
           h264ring_error_name((unsigned)H264RING_ST(&st, ERR)), (unsigned)H264RING_ST(&st, ERR_POS));
    session_ok = false;
  }
  if (!streaming)
    return;
  uint32_t refused = (uint32_t)H264RING_ST(&st, FE_REFUSED);
  if (refused != refused_base && session_ok) {
    printf("[core] the core refuses the stream: %s\n", refusal_name((unsigned)H264RING_ST(&st, FE_REFUSED_CODE)));
    refused_base = refused;
    n.refusals++;
    cavlc_refused = true; /* no more AUs: an IDR does not cure it */
    snprintf(refused_why, sizeof(refused_why), "The core refuses the stream: %s",
             refusal_name((unsigned)H264RING_ST(&st, FE_REFUSED_CODE)));
    if (refused_cb)
      refused_cb(refused_why);
    return;
  }
  uint32_t e = err_sum(&st);
  if (e != err_base && idr_written_ms && now_ms() - idr_written_ms < IDR_GRACE_MS) {
    n.errors_seen += e - err_base;
    err_base = e;
  } else if (e != err_base) {
    n.errors_seen += e - err_base;
    err_base = e;
    if (!need_idr || (idr_asked_ms && now_ms() - idr_asked_ms > IDR_RETRY_MS)) {
      need_idr = true;
      idr_asked_ms = now_ms();
      n.idr_asked++;
      LiRequestIdrFrame();
    }
  }
}

static void rhythm_reset_window(void) {
  rt.frames = rt.held = rt.held_us_max = rt.forced = rt.lost = 0;
  rt.held_us_sum = 0;
  rt.jit_min = INT64_MAX;
  rt.jit_max = INT64_MIN;
  rt.skips = rt.starves = rt.picks = 0;
}

/* Every ~2 ms from the input thread during the stream: the hitches, at once */
void mg_core_tick(void) {
  if (!rt.on || !streaming || restarting)
    return;
  struct h264ring_status st;
  pthread_mutex_lock(&ring_mx);
  int ok = ring && !gone && h264ring_status(ring, &st) == 0;
  pthread_mutex_unlock(&ring_mx);
  if (!ok)
    return;
  uint32_t pick = (uint32_t)H264RING_ST(&st, DISP_PICK), skip = (uint32_t)H264RING_ST(&st, DISP_SKIP);
  uint32_t starve = (uint32_t)H264RING_ST(&st, DISP_STARVE);
  if (!rt.t0_ms) {
    rt.t0_ms = rt.t_last_ms = now_ms();
    rt.pick_last = pick; rt.skip_last = skip; rt.starve_last = starve;
    rhythm_reset_window();
  }
  double t = (now_ms() - rt.t0_ms) / 1000.0;
  rt.picks += pick - rt.pick_last;
  rt.pick_last = pick;
  if (skip != rt.skip_last) {
    printf("[rhythm] %7.2f s: %u pictures dropped (two ready in the same frame)\n", t, skip - rt.skip_last);
    rt.skips += skip - rt.skip_last;
    rt.skip_last = skip;
  }
  if ((uint16_t)(starve - rt.starve_last)) {
    printf("[rhythm] %7.2f s: %u frames without a new picture (repeated)\n", t, (uint16_t)(starve - rt.starve_last));
    rt.starves += (uint16_t)(starve - rt.starve_last);
    rt.starve_last = starve;
  }
}

static void guard_report(void);
static void rhythm_au(PDECODE_UNIT du, uint64_t held_us, bool forced) {
  if (!rt.on || !rt.t0_ms)
    return;
  int64_t jit = (int64_t)du->receiveTimeUs - (int64_t)((uint64_t)du->rtpTimestamp * 1000000ull / 90000ull);
  if (!rt.frames)
    rt.rtp_first = du->rtpTimestamp;
  rt.frames++;
  rt.rtp_last = du->rtpTimestamp;
  if (jit < rt.jit_min) rt.jit_min = jit;
  if (jit > rt.jit_max) rt.jit_max = jit;
  if (held_us) {
    rt.held++;
    rt.held_us_sum += held_us;
    if (held_us > rt.held_us_max) rt.held_us_max = (unsigned)held_us;
  }
  if (forced) rt.forced++;
  if (now_ms() - rt.t_last_ms >= RITMO_PERIOD_S * 1000u && rt.frames > 10) {
    double t = (now_ms() - rt.t0_ms) / 1000.0;
    rt.t_last_ms = now_ms();
    double drtp = (double)(uint32_t)(rt.rtp_last - rt.rtp_first);
    double fps = drtp > 0 ? (rt.frames - 1) * 90000.0 / drtp : 0;
    printf("[rhythm] %7.1f s: %u pictures, the PC at %.3f fps, arrival +-%.1f ms; pacing %s: %u held "
           "(mean %.1f ms, max %.1f), %u written with a newer one behind, %u lost; "
           "core: %u picks, %u dropped, %u repeated\n",
           t, rt.frames, fps, (rt.jit_max - rt.jit_min) / 2000.0, guard_on ? "guard" : pacing ? "on" : "off", rt.held,
           rt.held ? rt.held_us_sum / 1000.0 / rt.held : 0.0, rt.held_us_max / 1000.0, rt.forced, rt.lost,
           rt.picks, rt.skips, rt.starves);
    guard_report();
    rhythm_reset_window();
  }
}

/* Waits until the core took or dropped the previous picture (pacing) */
static void pace_wait(uint64_t *held_us, bool *forced) {
  *held_us = 0;
  *forced = false;
  if (!pacing)
    return;
  uint64_t t0 = now_us();
  for (;;) {
    struct h264ring_status st;
    pthread_mutex_lock(&ring_mx);
    int ok = ring && session_ok && streaming && h264ring_status(ring, &st) == 0;
    pthread_mutex_unlock(&ring_mx);
    if (!ok)
      return;
    uint32_t done = (uint32_t)H264RING_ST(&st, DISP_PICK) + (uint32_t)H264RING_ST(&st, DISP_SKIP) - pace_base;
    int32_t pending = (int32_t)(pace_written - done);
    if (pending <= 0) {
      if (pending < 0)
        pace_base += (uint32_t)pending; /* more picks counted than writes (restart): realign */
      break;
    }
    uint64_t dt = now_us() - t0;
    if (LiGetPendingVideoFrames() > 0) {
      *forced = true; /* another one arrived: no delay builds up, the core drops the old one */
      break;
    }
    if (dt >= PACE_MAX_MS * 1000u) {
      /* a picture never taken (lost by the decoder): no longer waited for */
      pace_base -= (uint32_t)pending;
      if (rt.on) rt.lost++;
      break;
    }
    usleep(300);
  }
  *held_us = now_us() - t0;
  if (*held_us < 500)
    *held_us = 0;
}

/* ---------- the phase guard (-guard; core with P3e) ----------
 * With the PC at exactly 60.000 Hz the arrivals stay a long time at the same place relative to
 * the core's pickup: far from it, all smooth; near it, every network delay of a few ms makes a
 * picture land now in one frame now in the next (one repeated and one dropped, continuously).
 *
 * The guard measures the core's grid on the ARM's clock: the status block is rewritten every
 * frame (SEQ_HEAD +1, at the frame's start), the pickup's evaluation falls at 428,142 / 450,000
 * of the frame (425,947 at 240p; h264/disp_ctl.sv). A thread reads SEQ_HEAD every ~0.4 ms and a
 * PLL derives the grid from it. On it: the pictures' mean arrival phase phi, their spread s, and
 * D, the time the core takes to decode a picture (learned: a picture written in time for a pickup
 * that misses it raises D).
 *
 * While the arrivals' cloud (phi +- s) stays far from the decode deadline (P - D, with D's
 * uncertainty) the guard does nothing. If it comes near, it sets a cut c just before the cloud:
 * the pictures arriving after c are held until the next pickup and written right after it, so
 * they all go, surely, to the following pickup (one more frame of latency, only while the phase
 * is there); those before c go to the next one. The pickup a picture goes to then depends on
 * where it lands relative to c, far from the cloud, and not on chance. With hysteresis: the cut
 * moves if the cloud comes near it, and the guard turns off if the cloud stays far from the
 * deadline with a wide margin for a few seconds; with arrivals spread over more than half a
 * frame no cut helps and it stays off. */
#define G_POLL_US     400
#define G_XH          1200        /* the genlock: the delays of the last 1200 pictures (20 s) */
#define G_Q_MAX_US    8000.0      /* ... and the margin at most half a frame */
#define G_FE          1800        /* the genlock: the PC's rate over the last 1800 pictures (30 s) */
#define G_LQ          32          /* the latency: the queues of the written / emitted pictures */
#define G_LX          3600        /* ... x over the last 3600 pictures (60 s) */
#define G_LL          31          /* ... L: the median of the last 31 picks */
#define G_FE_MIN_S    10.0        /* ... used from 10 s of measurement (before, the 4 s startup estimate) */
#define G_FE_MAX_US   40.0        /* ... good within +-40 us a frame (the PLL reaches +-32) */
#define G_FE_MAX_SD_US 1500.0     /* ... and with the phase within 1.5 ms (rms) of the line */
/* Holding early pictures is off: two versions (until after / until before the previous pickup)
 * made things worse on the MiSTer (traces of 14/11), and the model of the core's timings that
 * justified them does not hold with the second trace. The code stays, off. */
#define G_GL_HOLD     0
#define G_MARGIN_US   1000.0
#define G_D0_US       10500.0     /* initial D: 1,200 macroblocks at ~110,000 a second */
#define G_D_MIN_US    5000.0
#define G_D_MAX_US    15000.0     /* the guard (its zones assume D < one frame) */
#define G_D_MAX_GL_US 15000.0     /* the genlock: with 22 ms (to give large pictures time) D
                                   * rose to the maximum in the startup burst and the small pictures,
                                   * ready in a few ms, collided (trace 4 of 14/11): back to 15
                                   * until the decode times measured by the core say more */
static struct {
  pthread_t th;
  volatile bool run;
  bool th_ok;
  pthread_mutex_t mx;
  /* the grid (protected by mx) */
  double P, eoff;                  /* the period and the evaluation's position in the frame, us */
  double t_ref;                    /* a frame's start, us (CLOCK_MONOTONIC) */
  double Pm;                       /* the measured frame period (changes with the genlock), us */
  uint64_t w45;                    /* the last word 45: [25] PLL read, [26] moving, [63:32] K */
  bool gl_pll_fail;                /* the core did not read its PLL: the genlock is useless, the guard stays */
  double gl_stuck_t0;              /* since when the core has been stuck on a PLL write (0: it is not) */
  int gl_lag_st;                   /* the reserve: 0 not asked, 1 asked, 2 applied, 3 refused */
  double gl_lag_t0;
  unsigned samples, outliers;
  /* the status block at the last frame */
  uint32_t pick, starve;
  bool have_snap;
  /* the arrivals */
  double ex, ey;                   /* circular mean (EMA) */
  double mad;                      /* mean absolute deviation from the mean phase, us */
  unsigned arrivals;
  double D;                        /* estimated decode time, us */
  int mode;                        /* 0 cut at the deadline, 1 fixed cut */
  double cut, fixed_cut;
  unsigned good_run;               /* pictures with the preferred cut free (to go back to it) */
  /* pictures written awaiting the outcome: the write's instant and the expected pickup */
  struct { double wr, ev; bool idr; } pend[4];
  unsigned npend;
  /* THE TRUE LATENCY, picture by picture (from the core's counters, words 23, 85, 31 and the picks):
   * the written pictures (v: the nominal arrival from the RTP timestamp) wait to be emitted by the
   * decoder, the emitted ones to be taken or dropped, in order. x = ready - v (decoding plus
   * network), L = pickup - v (the latency from the nominal arrival to the screen) */
  double lq_w[G_LQ], lq_e[G_LQ];   /* written: v; emitted: v */
  unsigned lq_wh, lq_wn, lq_eh, lq_en;
  uint32_t lq_oe, lq_pk, lq_sk;
  bool lq_have;
  unsigned lq_wmin, lq_wmin_n;     /* the written queue, its minimum over 120 frames */
  double lx[G_LX]; unsigned lx_i, lx_n;      /* x */
  double ll[G_LL]; unsigned ll_i, ll_n;      /* L */
  double lx50, lx999, lL;          /* the statistics (every half second) */
  double lsum, lr[4];
  unsigned lcnt, lr_i, lr_n;
  bool lstat, lag_ctl;
  double lag_Lt;
  unsigned lq_resync;
  /* counters for -stats */
  unsigned held, misses, switches, hits;
  double held_us_sum;
  /* the genlock (this repository's core, interface mm 1.5) */
  int gl_state;                    /* 0 off, 1 on trial (does the core apply it?), 2 active, 3 unsupported */
  double gl_t0;                    /* when it was asked (for the trial) */
  int gl_trim, gl_trim_sent;       /* the core's rate in 2^-16 (+ slower) */
  double gl_integ;                 /* the integral part: us a frame (the rate difference with the PC) */
  double gl_L;                     /* a line's duration, us */
  double gl_target, gl_err;        /* the arrivals' target phase and the error, us */
  double gl_err_abs;               /* for -stats: mean absolute error */
  unsigned gl_n;
  /* the core's frame rate, measured on the ARM's clock (per second: min / max) */
  double r_t0, r_min, r_max;
  /* the PC rate estimate at the genlock's start (FLL) */
  bool fll_done;
  /* the PC's rhythm (RTP timestamp) for the genlock */
  bool rtp_have, q_init, gl_locked, d_have, gl_relock, p240, rtp_prev_have;
  double rtp_prev_us;
  uint32_t rtp_last;
  int64_t rtp64;
  double d_ref, dmin, dmin_t, q, q_tgt, gl_bad_t0, gl_phv, gl_eema;
  float xh[G_XH];
  unsigned xh_i, xh_n, xh_cnt, gl_good;
  double fll_start, fll_t0, fll_prev, fll_phu, fll_sx, fll_sy, fll_sxx, fll_sxy;
  unsigned fll_n;
  /* the PC's rate measured continuously (replaces the integral): the slope of
   * Y = the RTP timestamp's phase (unwrapped) + the phase the trim applied to the core removed */
  double fe_x[G_FE], fe_y[G_FE];
  unsigned fe_i, fe_n, fe_cnt;
  bool fe_have, fe_valid;
  double fe_prev, fe_phu, fe_C, fe_ta, fe_w, fe_t0, fe_F, fe_sd, fe_logged;
  unsigned fe_rejected;
  unsigned r_n;
} g = { .mx = PTHREAD_MUTEX_INITIALIZER };
static void lat_written(double v);
static void lat_track(const struct h264ring_status *st, double s);

static double g_wrap(double x, double P) {
  x = fmod(x, P);
  return x < 0 ? x + P : x;
}
static int g_fcmp(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}
static double g_cdist(double a, double b, double P) {   /* circular distance */
  double d = fabs(g_wrap(a - b, P));
  return d > P / 2 ? P - d : d;
}

/* the grid's thread */
static void *guard_thread(void *arg) {
  (void)arg;
  volatile uint64_t *ctrl = NULL;
  pthread_mutex_lock(&ring_mx);
  if (ring)
    ctrl = ((struct h264ring_backend *)ring)->ctrl;   /* be is struct h264ring's first field */
  pthread_mutex_unlock(&ring_mx);
  if (!ctrl)
    return NULL;
  uint32_t last = (uint32_t)ctrl[H264RING_SEQ_HEAD_W];
  double t_prev = (double)now_us();
  while (g.run) {
    usleep(G_POLL_US);
    double t = (double)now_us();
    uint32_t seq = (uint32_t)ctrl[H264RING_SEQ_HEAD_W];
    if (seq != last) {
      double s = 0.5 * (t_prev + t);   /* the change fell between the two reads */
      pthread_mutex_lock(&g.mx);
      if (!g.r_t0) { g.r_t0 = s; g.r_n = 0; }
      g.r_n += (uint32_t)(seq - last);
      if (s - g.r_t0 >= 1e6) {
        double hz = g.r_n * 1e6 / (s - g.r_t0);
        if (!g.r_min || hz < g.r_min) g.r_min = hz;
        if (hz > g.r_max) g.r_max = hz;
        g.r_t0 = s; g.r_n = 0;
      }
      pthread_mutex_unlock(&g.mx);
      struct h264ring_status st;
      bool snap = false;
      pthread_mutex_lock(&ring_mx);
      if (ring && !gone)
        snap = h264ring_status(ring, &st) == 0;
      pthread_mutex_unlock(&ring_mx);
      pthread_mutex_lock(&g.mx);
      /* second-order PLL on the frames' starts: phase (t_ref) and period (Pm), which with the genlock
       * glides along with the core's video PLL (not derived from the trim: the core gets there by degrees) */
      if (!g.samples) {
        g.t_ref = s;
        g.Pm = g.P;
      } else {
        double Pe = g.Pm;
        double k = floor((s - g.t_ref) / Pe + 0.5);
        double err = s - (g.t_ref + k * Pe);
        if (fabs(err) > 2500 && g.samples > 60) {
          if (++g.outliers > 30) {                                   /* lost: start over */
            g.samples = 0, g.outliers = 0;
            if (g.gl_state == 2) { g.gl_relock = true; printf("[genlock] frame grid lost: re-locking\n"); }
          }
        } else {
          g.outliers = 0;
          g.t_ref += k * Pe + err * (g.samples < 60 ? 0.125 : 0.03);
          if (k >= 1) {
            g.Pm += err / k * (g.samples < 60 ? 0.004 : 0.0005);
            if (g.Pm > g.P * 1.005) g.Pm = g.P * 1.005;
            if (g.Pm < g.P * 0.995) g.Pm = g.P * 0.995;
          }
        }
      }
      if (g.samples < 100000) g.samples++;
      if (snap) {
        uint32_t pick = (uint32_t)H264RING_ST(&st, DISP_PICK);
        uint32_t starve = (uint32_t)H264RING_ST(&st, DISP_STARVE) & 0xffffu;
        if (g.have_snap && g.samples > 60) {
          /* the evaluation just passed: the frame started at s, minus a period, plus eoff */
          double ev = s - g.P + g.eoff;
          bool starved = ((starve - g.starve) & 0xffffu) != 0;
          bool picked = pick != g.pick;
          unsigned j = 0;
          for (unsigned i = 0; i < g.npend; i++) {
            if (fabs(g.pend[i].ev - ev) < g.P / 2) {
              double slack = g.pend[i].ev - g.pend[i].wr;   /* the time it had to decode */
              if (starved && !picked) {
                g.misses++;
                if (!g.pend[i].idr && slack + 500 > g.D) g.D = fmin(g.gl_state == 2 ? G_D_MAX_GL_US : G_D_MAX_US, slack + 500);
              } else if (picked) {
                g.hits++;
                if (!g.pend[i].idr && slack < g.D + 2000) g.D = fmax(G_D_MIN_US, g.D - (g.gl_state == 2 ? 1 : 5));
              }
            } else if (g.pend[i].ev > ev) {
              g.pend[j++] = g.pend[i];                        /* not yet */
            }
          }
          g.npend = j;
        }
        lat_track(&st, s);
        g.pick = pick;
        g.starve = starve;
        g.have_snap = true;
        tr_printf("E,%.0f,%u,%u,%u,%u,%.1f,%.0f,%.0f,%u,%u,%u,%u,%u\n", s, seq, pick, (uint32_t)H264RING_ST(&st, DISP_SKIP), starve,
                    g.Pm, g.t_ref, g.eoff, (unsigned)(st.w[31] >> 42), (unsigned)(st.w[45] >> 32), (unsigned)(st.w[85] >> 32),
                    (unsigned)(st.w[15] & 0xffffffffu), (unsigned)(st.w[23] >> 32));
        /* the genlock: does the core apply it? (word 45: [24] GL_EN applied, [1] word 34 refused) */
        g.w45 = st.w[45];
        /* 480i <-> 240p from the OSD: the pickup falls elsewhere in the frame and the raster restarts with
         * another phase; the locked genlock (limited correction) would take 20 s: re-lock */
        if ((st.w[1] & 0xFFFFu) >= 0x103u) {
          /* [15] the physical raster is now 240p (not [4] && [13], the request and interlaced: in a
           * session they gave 480i with the core at 240p and missed the switch, which came only
           * as a lost grid, 15/11) */
          bool p240n = (st.w[87] >> 15) & 1u;
          if (p240n != g.p240) {
            g.p240 = p240n;
            g.eoff = g.P * (p240n ? 425947.0 : 428142.0) / 450000.0;
            g.gl_relock = true;
            printf("[genlock] raster %s: re-locking\n", p240n ? "240p" : "480i");
          }
        }
        /* the genlock active but the core's PLL not read after 4 s: it cannot move, back to the guard */
        if (g.gl_state == 2 && !((g.w45 >> 25) & 1) && s - g.gl_t0 > 4000000 && !g.gl_pll_fail) {
          g.gl_pll_fail = true;
          g.gl_state = 3;
          snprintf(gl_problem, sizeof(gl_problem), "the core does not read its video PLL (state %u, reads 0x%08x)",
                   (unsigned)(g.w45 >> 27) & 15u, (unsigned)(g.w45 >> 32));
          printf("[genlock] %s: %s\n", gl_problem, guard_on ? "the guard stays" : "no sync");
        }
        /* ... or read, but every write stays pending (waitrequest high) for 3 s: the PLL does not answer */
        unsigned pst = (unsigned)(g.w45 >> 27) & 15u;
        if (g.gl_state == 2 && (pst == 8 || pst == 9) && ((g.w45 >> 31) & 1)) {
          if (!g.gl_stuck_t0) g.gl_stuck_t0 = s;
          else if (s - g.gl_stuck_t0 > 3000000 && !g.gl_pll_fail) {
            g.gl_pll_fail = true;
            g.gl_state = 3;
            snprintf(gl_problem, sizeof(gl_problem), "the core's video PLL does not take writes (state %u)", pst);
            printf("[genlock] %s: %s\n", gl_problem, guard_on ? "the guard stays" : "no sync");
          }
        } else {
          g.gl_stuck_t0 = 0;
        }
        if (g.gl_lag_st == 1) {
          if ((g.w45 >> 2) & 1) {
            g.gl_lag_st = 2;
            printf("[genlock] the core keeps one picture in reserve (one more frame)\n");
          } else if (((g.w45 >> 1) & 1) || s - g.gl_lag_t0 > 1500000) {
            g.gl_lag_st = 3;
            snprintf(gl_lag_problem, sizeof(gl_lag_problem), "the loaded core has no reserve (genlock +1)");
            printf("[genlock] %s: plain genlock without the reserve\n", gl_lag_problem);
            pthread_mutex_lock(&ring_mx);
            if (ring) h264ring_set_gl_lag(ring, 0);
            pthread_mutex_unlock(&ring_mx);
          }
        }
        if (g.gl_state == 1) {
          uint64_t w45 = st.w[45];
          if ((w45 >> 24) & 1) {
            g.gl_state = 2;
            printf("[genlock] the core applies it\n");
            /* the reserve only now: a core that does not know it refuses the whole word 34 (and would hold
             * the trim too), so the genlock is already applied and the reserve is dropped if refused */
            if (gl_lag_req) {
              g.gl_lag_st = 1;
              g.gl_lag_t0 = s;
              pthread_mutex_lock(&ring_mx);
              if (ring) h264ring_set_gl_lag(ring, 1);
              pthread_mutex_unlock(&ring_mx);
            }
          } else if (((w45 >> 1) & 1) || s - g.gl_t0 > 1500000) {
            g.gl_state = 3;    /* core without genlock: the request is dropped (word 34 would be refused) */
          }
        }
      }
      pthread_mutex_unlock(&g.mx);
      last = seq;
    }
    t_prev = t;
  }
  return NULL;
}

static void guard_start(void) {
  if ((!guard_on && !genlock_req) || g.th_ok)
    return;
  if (rt.on) tr_begin(); else tr_on = false;      /* -stats: the trace, with the genlock or with the guard */
  mg_core_info ci;
  mg_core_get_info(&ci);
  pthread_mutex_lock(&g.mx);
  unsigned hz100 = ci.refresh_x100 ? ci.refresh_x100 : 6000;
  g.P = 1e8 / hz100;
  g.eoff = g.P * (ci.p240 ? 425947.0 : 428142.0) / 450000.0;
  g.samples = g.outliers = 0;
  g.have_snap = false;
  g.ex = g.ey = 0;
  g.mad = 0;
  g.arrivals = 0;
  if (g.D < G_D_MIN_US) g.D = G_D0_US;   /* kept from one stream to the next */
  if (!genlock_req && g.D > G_D_MAX_US) g.D = G_D_MAX_US;   /* after a genlock: the guard wants D < one frame */
  g.mode = 0;
  g.good_run = 0;
  g.npend = 0;
  g.lq_wh = g.lq_wn = g.lq_eh = g.lq_en = 0; g.lq_have = false; g.lq_wmin = 99; g.lq_wmin_n = 0;
  g.lx_i = g.lx_n = g.ll_i = g.ll_n = 0; g.lstat = false; g.lq_resync = 0; g.lx50 = g.lx999 = g.lL = 0;
  g.lag_ctl = false; g.lag_Lt = 0; g.lsum = 0; g.lcnt = g.lr_i = g.lr_n = 0;
  g.held = g.misses = g.switches = g.hits = 0;
  g.held_us_sum = 0;
  g.gl_state = genlock_req ? 1 : 0;
  g.gl_t0 = (double)now_us();
  g.gl_pll_fail = false;
  g.fll_done = false;
  g.fll_start = g.fll_t0 = g.fll_prev = g.fll_phu = g.fll_sx = g.fll_sy = g.fll_sxx = g.fll_sxy = 0;
  g.fe_have = g.fe_valid = false; g.fe_n = g.fe_i = g.fe_cnt = 0; g.fe_F = g.fe_sd = 0; g.fe_ta = 0;
  g.fe_logged = 0; g.fe_rejected = 0;
  g.fll_n = 0;
  g.rtp_have = g.q_init = g.gl_locked = g.d_have = g.gl_relock = g.rtp_prev_have = false;
  g.p240 = ci.p240;
  g.gl_eema = 5000;
  g.d_ref = g.dmin = g.dmin_t = g.q = g.q_tgt = g.gl_bad_t0 = g.gl_phv = 0;
  g.xh_i = g.xh_n = g.xh_cnt = g.gl_good = 0;
  gl_problem[0] = 0;
  gl_lag_problem[0] = 0;
  g.gl_lag_st = 0;
  g.gl_stuck_t0 = 0;
  g.gl_trim = g.gl_trim_sent = 0;
  g.gl_integ = 0;
  g.gl_L = g.P / ((ci.interlaced || ci.p240) ? 262.5 : 525.0);
  g.gl_err_abs = 0;
  g.gl_n = 0;
  g.r_t0 = g.r_min = g.r_max = 0;
  pthread_mutex_unlock(&g.mx);
  if (genlock_req) {
    pthread_mutex_lock(&ring_mx);
    if (ring) h264ring_set_genlock(ring, 1, 0);
    pthread_mutex_unlock(&ring_mx);
  }
  g.run = true;
  g.th_ok = pthread_create(&g.th, NULL, guard_thread, NULL) == 0;
}

static int dcmp(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

/* (under g.mx) a picture written: v = the nominal arrival (0 if unknown) */
static void lat_written(double v) {
  if (g.lq_wn == G_LQ) { g.lq_wh = (g.lq_wh + 1) % G_LQ; g.lq_wn--; }
  g.lq_w[(g.lq_wh + g.lq_wn++) % G_LQ] = v;
}

/* (under g.mx, grid thread) a core status read at the start of frame s */
static void lat_track(const struct h264ring_status *st, double s) {
  uint32_t oe = (uint32_t)(st->w[23] >> 32);
  uint32_t pk = (uint32_t)H264RING_ST(st, DISP_PICK), sk = (uint32_t)H264RING_ST(st, DISP_SKIP);
  if (!g.lq_have) { g.lq_have = true; g.lq_oe = oe; g.lq_pk = pk; g.lq_sk = sk; return; }
  uint32_t de = oe - g.lq_oe, dp = pk - g.lq_pk, ds = sk - g.lq_sk;
  g.lq_oe = oe; g.lq_pk = pk; g.lq_sk = sk;
  if (de > 8 || dp > 8 || ds > 8) {          /* out of step (should not happen): start over */
    g.lq_wn = g.lq_en = 0; g.lq_resync++;
    return;
  }
  /* the last emission's instant: tm_emit (27 MHz) against tm_now (27 MHz >> 10) */
  int64_t dt = (int64_t)(int32_t)((uint32_t)(st->w[85] >> 32) - (uint32_t)((st->w[31] >> 42) << 10));
  double t_emit = s + dt / 27.0;
  for (uint32_t j = 0; j < de; j++) {
    if (!g.lq_wn) break;                     /* written before the counting started */
    double v = g.lq_w[g.lq_wh];
    g.lq_wh = (g.lq_wh + 1) % G_LQ; g.lq_wn--;
    if (g.lq_en == G_LQ) { g.lq_eh = (g.lq_eh + 1) % G_LQ; g.lq_en--; }
    g.lq_e[(g.lq_eh + g.lq_en++) % G_LQ] = v;
    if (de == 1 && v > 0) {                  /* the instant is this one's alone */
      g.lx[g.lx_i] = t_emit - v;
      g.lx_i = (g.lx_i + 1) % G_LX;
      if (g.lx_n < G_LX) g.lx_n++;
    }
  }
  for (uint32_t j = 0; j < ds && g.lq_en; j++) { g.lq_eh = (g.lq_eh + 1) % G_LQ; g.lq_en--; }
  double ev = s - g.Pm + g.eoff;             /* the pickup just passed */
  for (uint32_t j = 0; j < dp && g.lq_en; j++) {
    double v = g.lq_e[g.lq_eh];
    g.lq_eh = (g.lq_eh + 1) % G_LQ; g.lq_en--;
    if (v > 0) {
      g.ll[g.ll_i] = ev - v;
      g.ll_i = (g.ll_i + 1) % G_LL;
      if (g.ll_n < G_LL) g.ll_n++;
    }
  }
  /* a picture written and never emitted (a decoder error) would shift every pair by one: if the
   * written queue never gets empty in 2 s, the oldest is removed */
  if (g.lq_wn < g.lq_wmin) g.lq_wmin = g.lq_wn;
  if (++g.lq_wmin_n >= 120) {
    if (g.lq_wmin >= 1 && g.lq_wn) { g.lq_wh = (g.lq_wh + 1) % G_LQ; g.lq_wn--; g.lq_resync++; }
    g.lq_wmin = 99; g.lq_wmin_n = 0;
  }
  /* the statistics, every 30 frames */
  if (g.lq_wmin_n % 30 == 0 && g.lx_n >= 600 && g.ll_n == G_LL) {
    static double tmp[G_LX];
    memcpy(tmp, g.lx, g.lx_n * sizeof(double));
    qsort(tmp, g.lx_n, sizeof(double), dcmp);
    g.lx50 = tmp[g.lx_n / 2];
    g.lx999 = tmp[(unsigned)(g.lx_n * 0.999)];
    double tl[G_LL];
    memcpy(tl, g.ll, sizeof(tl));
    qsort(tl, G_LL, sizeof(double), dcmp);
    g.lL = tl[G_LL / 2];
    g.lstat = g.lL > 0 && g.lL < 100000;
    /* the mean latency while locked, without the last 2 s (as the stream closes the last pictures
     * come out late: at the stream's end the median gave 54-56 ms instead of 20) */
    if (g.lstat && g.gl_locked) {
      if (g.lr_n == 4) { g.lsum += g.lr[g.lr_i]; g.lcnt++; } else g.lr_n++;
      g.lr[g.lr_i] = g.lL;
      g.lr_i = (g.lr_i + 1) % 4;
    }
  }
}

static void guard_stop(void) {
  if (!g.th_ok)
    return;
  g.run = false;
  pthread_join(g.th, NULL);
  g.th_ok = false;
  tr_end();
  if (g.gl_state == 2 && g.lcnt)
    printf("[genlock] at stream end: mean latency %.1f ms from arrival to screen, while locked (%.0f s; ready after arrival: median %.1f ms)%s\n",
           g.lsum / g.lcnt / 1000, g.lcnt * 0.5, g.lx50 / 1000, g.lag_ctl ? ", genlock +1" : "");
  if (g.gl_state == 2)
    printf("[genlock] at stream end: PC rate %s %+.2f us a frame (%.4f Hz), phase %.2f ms off the line%s\n",
           g.fe_valid ? "measured" : "estimated", g.fe_valid ? g.fe_F : g.gl_integ, 1e6 / (16666.667 + (g.fe_valid ? g.fe_F : g.gl_integ)),
           g.fe_sd / 1000, g.fe_rejected ? ", with measurements rejected" : "");
  if (g.gl_state) {
    /* outside the stream the raster goes back to exactly 60.000 Hz */
    pthread_mutex_lock(&ring_mx);
    if (ring) { h264ring_set_genlock(ring, 0, 0); h264ring_set_gl_lag(ring, 0); }
    pthread_mutex_unlock(&ring_mx);
    g.gl_state = 0;
    g.gl_lag_st = 0;
  }
}

/* Before writing a picture: hold it until the next pickup if it lands after the cut */
static void guard_wait(bool idr, uint64_t *held_us, PDECODE_UNIT du) {
  uint32_t rtp = du->rtpTimestamp;
  *held_us = 0;
  if (!g.th_ok)
    return;
  double w = (double)now_us();
  pthread_mutex_lock(&g.mx);
  if (g.samples < 90) {
    lat_written(0);
    pthread_mutex_unlock(&g.mx);
    return;
  }
  double P = g.P;
  double e0 = g.t_ref + g.eoff;              /* an evaluation */
  double a = g_wrap(w - e0, P);              /* how long since the last evaluation */
  double e_next = w - a + P;
  /* the arrivals' mean phase and spread */
  double ang = 2 * M_PI * a / P;
  double al = g.arrivals < 64 ? 1.0 / (g.arrivals + 1) : 1.0 / 64;
  g.ex += (cos(ang) - g.ex) * al;
  g.ey += (sin(ang) - g.ey) * al;
  double phi = g_wrap(atan2(g.ey, g.ex) * P / (2 * M_PI), P);
  g.mad += (g_cdist(a, phi, P) - g.mad) * al;
  g.arrivals++;
  /* the genlock: the pickup is brought to land right after the picture is ready, by moving the rate
   * of the core's video PLL (GL_TRIM, 2^-16 = 0.25 us a frame; the core glides there, 25 units a
   * second, without line jumps). An estimate of the PC's rate, then a phase loop on the PC's
   * rhythm (below). */
  int gl_send = -1000;
  if (g.gl_state == 3) {
    g.gl_state = 0;
    gl_send = 1001;                          /* drop the request */
    if (!gl_problem[0])
      snprintf(gl_problem, sizeof(gl_problem), "the loaded core has no genlock");
    printf("[genlock] %s: %s\n", gl_problem, guard_on ? "the guard stays" : "no sync");
  }
  /* THE PC'S RHYTHM, NOT THE ARRIVALS. The arrivals move with the pictures' size and with the
   * Wi-Fi (a fast scroll: large pictures, arrivals late by several ms): chasing them moves the
   * core's rate (on the MiSTer: from 59.94 to 60.07 and 59.88 at every scroll). The genlock follows
   * instead each picture's RTP timestamp (the PC's clock, 90 kHz): v = rtp + the base delay (the
   * 3rd percentile of the last 20 s's delays, which also follows the drift between the two clocks)
   * is the arrival the picture would have without queues or size. The margin is the 97th
   * percentile of the delays minus the base, and it measures only after the startup burst (from
   * the rate estimate); while locking it follows at once, once locked it changes by at most
   * 0.15 ms a second (the phase follows 0.18). */
  double phv = 0, v_abs = 0;
  bool have_v = false;
  if (g.gl_state == 2) {
    if (!g.rtp_have) { g.rtp64 = rtp; g.rtp_have = true; }
    else g.rtp64 += (int32_t)(rtp - g.rtp_last);
    g.rtp_last = rtp;
    double rtp_us = (double)g.rtp64 * (1e6 / 90000.0);
    double d = w - rtp_us;
    double dt_q = g.dmin_t ? (w - g.dmin_t) / 1e6 : 0;
    g.dmin_t = w;
    /* a timestamp jump (the stream's start, the network just ready): start over */
    if (!g.d_have || fabs(d - (g.d_ref + g.dmin)) > 500000) {
      if (g.d_have) printf("[genlock] jump of %.0f ms between timestamps and arrivals: the measurement restarts\n",
                           (d - (g.d_ref + g.dmin)) / 1000);
      g.d_have = true; g.d_ref = d; g.dmin = 0; g.xh_i = g.xh_n = g.xh_cnt = 0; g.q_init = false;
      g.fe_have = false; g.fe_n = g.fe_i = g.fe_cnt = 0;
    }
    /* in the delays, only the pictures one frame after the previous (RTP timestamp): on a still desktop
     * Sunshine sends pictures only when the screen changes, and the one after a pause arrives with a
     * delay that is not the network's (15/11, the first 20 s on the desktop before opening the game:
     * margin 48 ms with every picture, 4-6 ms with these) */
    bool reg_step = g.rtp_prev_have && fabs(rtp_us - g.rtp_prev_us - P) < 0.25 * P;
    g.rtp_prev_us = rtp_us;
    g.rtp_prev_have = true;
    if (reg_step) {
      g.xh[g.xh_i] = (float)(d - g.d_ref);
      g.xh_i = (g.xh_i + 1) % G_XH;
      if (g.xh_n < G_XH) g.xh_n++;
    }
    if (++g.xh_cnt >= 30 && g.xh_n >= 60) {
      /* every half second, on the delays of the last 1200 pictures (20 s): the base is the 3rd
       * percentile (not the minimum: a single odd timestamp would skew it for minutes -- on the
       * MiSTer, just switched on, a 48 ms margin), the margin the 97th minus the base (the 99.5th,
       * tried, widened the margin to almost a frame with the worst Wi-Fi and made pictures collide) */
      static float tmp[G_XH];
      memcpy(tmp, g.xh, sizeof(float) * g.xh_n);
      qsort(tmp, g.xh_n, sizeof(float), g_fcmp);
      g.dmin = tmp[(unsigned)(g.xh_n * 0.03)];
      g.q_tgt = tmp[(unsigned)(g.xh_n * 0.97)] - g.dmin;
      /* at most half a frame: arrivals spread over more than that cannot be fixed by any phase, and a
       * margin larger than a frame only makes the target spin (it is modulo a frame) */
      if (g.q_tgt > G_Q_MAX_US) g.q_tgt = G_Q_MAX_US;
      g.xh_cnt = 0;
      if (!g.q_init) { g.q = g.q_tgt; g.q_init = true; }
      /* once locked the margin falls by 0.15 ms a second; if the target is far lower (the startup's
       * irregular delays out of the 20 s window) it jumps there and re-locks: the first session after
       * Sunshine's start on 15/11 (the PC at 36-57 fps for 30 s) had a 48 ms margin that fell over
       * minutes, and the target, spinning, crossed the pickup: 75-86 pictures dropped every 10 s with
       * the PC already regular */
      if (g.gl_locked && g.q - g.q_tgt > 2000) {
        printf("[genlock] margin down from %.1f to %.1f ms: re-locking\n", g.q / 1000, g.q_tgt / 1000);
        g.q = g.q_tgt;
        g.gl_locked = false; g.gl_good = 0; g.gl_bad_t0 = 0; g.gl_eema = 5000;
      }
    }
    if (!g.gl_locked) {
      double qs = 1000.0 * dt_q;           /* while locking the margin follows quickly, 1 ms a second */
      if (g.q < g.q_tgt) g.q = fmin(g.q_tgt, g.q + qs); else g.q = fmax(g.q_tgt, g.q - qs);
    } else {
      double qs = 150.0 * dt_q;            /* locked: below the 0.18 ms a second the phase can follow */
      if (g.q < g.q_tgt) g.q = fmin(g.q_tgt, g.q + qs); else g.q = fmax(g.q_tgt, g.q - qs);
    }
    v_abs = rtp_us + g.d_ref + g.dmin;
    phv = g_wrap(v_abs - e0, P);
    have_v = g.q_init;
    /* THE PC'S RATE, MEASURED CONTINUOUSLY. The RTP timestamp's phase against the pickups (without
     * the dmin base, which jumps at each update) changes by (the PC's period - the core's period)
     * a frame; the core's period is the nominal one plus the applied trim (which the core reaches
     * by gliding, 25 units a second: it is simulated). Y = the unwrapped phase + the integral of
     * the applied trim therefore has slope (the PC's period - nominal), whatever the loop does: it
     * is the genlock's rate part, measured instead of integrated from the phase error. The former
     * integral, at every phase wrap (on the 5000 kbps trace of 15/11: the phase slipped a frame
     * every 25-30 s, bursts of 40-80 repeats), changed sign and charged the wrong way: it put the
     * PC 29.7 us a frame slower, it was 15.8. On the traces of 15/11 the slope over 30 s gives
     * 15.8-16.1 us a frame +-0.2 (over 10 s +-1, over 4 s +-3.5). */
    if (g.fll_done) {
      double ph = g_wrap(rtp_us + g.d_ref - e0, P);
      if (!g.fe_have) {
        g.fe_have = true; g.fe_phu = 0; g.fe_C = 0; g.fe_t0 = w; g.fe_ta = g.gl_trim_sent;
        g.fe_n = g.fe_i = g.fe_cnt = 0;
      } else {
        g.fe_phu += g_wrap(ph - g.fe_prev + P / 2, P) - P / 2;
        double dts = (w - g.fe_w) / 1e6;
        g.fe_C += g.fe_ta * P / 65536.0 * (w - g.fe_w) / P;
        double st = 25.0 * dts, tt = g.gl_trim_sent;
        g.fe_ta = g.fe_ta < tt ? fmin(tt, g.fe_ta + st) : fmax(tt, g.fe_ta - st);
      }
      g.fe_prev = ph;
      g.fe_w = w;
      g.fe_x[g.fe_i] = (w - g.fe_t0) / 1e6;
      g.fe_y[g.fe_i] = g.fe_phu + g.fe_C;
      g.fe_i = (g.fe_i + 1) % G_FE;
      if (g.fe_n < G_FE) g.fe_n++;
      if (++g.fe_cnt >= 30 && g.fe_n > 300) {
        g.fe_cnt = 0;
        double sx = 0, sy = 0, sxx = 0, sxy = 0, n = g.fe_n, xmin = 1e30, xmax = -1e30;
        for (unsigned i = 0; i < g.fe_n; i++) {
          double x = g.fe_x[i], y = g.fe_y[i];
          sx += x; sy += y; sxx += x * x; sxy += x * y;
          if (x < xmin) xmin = x;
          if (x > xmax) xmax = x;
        }
        double den = n * sxx - sx * sx;
        if (den > 0 && xmax - xmin >= G_FE_MIN_S) {
          double slope = (n * sxy - sx * sy) / den;            /* us of phase a second */
          double F = slope * P / 1e6;                          /* us a frame */
          double resid = 0, b = (sy - slope * sx) / n;
          for (unsigned i = 0; i < g.fe_n; i++) { double r = g.fe_y[i] - (b + slope * g.fe_x[i]); resid += r * r; }
          g.fe_sd = sqrt(resid / n);
          /* a good measurement: the phase lies on the line (on the traces of 15/11, rms 0.5-0.7 ms in
           * steady state, under 1 ms from 10 s after the start) and the rate is within the PLL's range
           * (+-127 units, 32 us a frame). Otherwise it is rejected and the previous one stays: two
           * sessions on the evening of 15/11 (no trace) had measured -373 and -32 us a frame, with the PC at +13 */
          if (fabs(F) > G_FE_MAX_US || g.fe_sd > G_FE_MAX_SD_US) {
            if (!g.fe_rejected++)
              printf("[genlock] rate measurement rejected: %+.1f us a frame, the phase %.1f ms off the line\n",
                     F, g.fe_sd / 1000);
          } else {
            if (!g.fe_valid)
              printf("[genlock] PC rate measured over %.0f s: %+.2f us a frame (%.4f Hz; the startup estimate was %+.1f)\n",
                     xmax - xmin, F, 1e6 / (P + F), g.gl_integ);
            else if (fabs(F - g.fe_logged) > 3)
              printf("[genlock] PC rate now %+.2f us a frame (%.4f Hz)\n", F, 1e6 / (P + F));
            if (!g.fe_valid || fabs(F - g.fe_logged) > 3) g.fe_logged = F;
            g.fe_F = F;
            g.fe_valid = true;
          }
        }
      }
    }
  }
  /* first the rate (FLL): with the core at nominal, the slope of v's phase tells how much slower or
   * faster the PC is (us a frame); 4 s, after Sunshine's startup burst (arrivals spread under
   * 1.5 ms; over 2.5 ms it starts over; after 15 s it starts from 0). It goes into the integral. */
  if (g.gl_state == 2 && have_v && !g.fll_done) {
    if (!g.fll_start) g.fll_start = w;
    if (g.fll_n || g.fll_t0) g.fll_phu += g_wrap(phv - g.fll_prev + P / 2, P) - P / 2;
    g.fll_prev = phv;
    if (g.mad > 2500) {
      g.fll_n = 0; g.fll_t0 = 0; g.fll_sx = g.fll_sy = g.fll_sxx = g.fll_sxy = 0;
    } else if (g.mad < 1500) {
      if (!g.fll_t0) { g.fll_t0 = w; g.fll_phu = 0; }
      double xs = (w - g.fll_t0) / 1e6;
      g.fll_n++; g.fll_sx += xs; g.fll_sy += g.fll_phu; g.fll_sxx += xs * xs; g.fll_sxy += xs * g.fll_phu;
      if (xs > 4.0 && g.fll_n > 150) {
        double n = g.fll_n, den = n * g.fll_sxx - g.fll_sx * g.fll_sx;
        double slope = den > 0 ? (n * g.fll_sxy - g.fll_sx * g.fll_sy) / den : 0;   /* us of phase a second */
        g.gl_integ = slope * P / 1e6;                                               /* us a frame */
        if (g.gl_integ > 30) g.gl_integ = 30;
        if (g.gl_integ < -30) g.gl_integ = -30;
        g.fll_done = true;
        printf("[genlock] PC rate: %+.1f us a frame against the core (%.4f Hz), measured in %.1f s\n",
               g.gl_integ, 1e6 / (P + g.gl_integ), (w - g.fll_start) / 1e6);
        /* the startup burst's delays do not count for the margin (on the MiSTer: 50 ms, falling by
         * 0.15 ms a second, with one picture in five lost for minutes): start over from here */
        g.xh_i = g.xh_n = g.xh_cnt = 0;
        g.q_init = false;
      }
    }
    if (!g.fll_done && w - g.fll_start > 15e6) {
      g.fll_done = true;
      printf("[genlock] PC rate not measured (irregular arrivals for 15 s): starting from 0\n");
      g.xh_i = g.xh_n = g.xh_cnt = 0;          /* here too the startup's delays do not count */
      g.q_init = false;
    }
  } else if (g.gl_state == 2 && have_v) {
    /* then the phase: target P - D - margin - 0.5 ms (a frame later if it does not fit). LOCKING: the
     * rate moves as much as needed (e/150, integral 8e-6). LOCKED (error under 0.8 ms for 3 s): the
     * phase correction is limited to +-3 us a frame (+-0.018 % around the PC's rate) and the
     * integral is slow (1e-6): the core stays at the PC's rate even when the margin or the decoding
     * change; the phase gets there slowly. Error over 5 ms for 10 s: re-lock. */
    double tg = P - g.D - g.q - 500;
    if (tg < 300) tg += P;
    tg = g_wrap(tg, P);
    double e = g_wrap(phv - tg + P / 2, P) - P / 2;
    /* GENLOCK +1: the target is the true latency L (pickup - nominal arrival, measured picture by
     * picture), not the phase: with the reserve the core keeps one more picture after the startup
     * burst or a late picture, and the phase alone stayed a frame above (36 ms on the trace of
     * 15/11). The target: the typical decoding (median, network included) plus one frame and 1 ms:
     * the reserve for the typical pictures, each picture has up to one frame more than usual (the
     * slowest, 99.9 %, stays 8-10 ms below on the traces; the 99.9 % is not in the target: for a
     * minute the startup burst, 50-60 ms, still counts). On the traces of 15/11: 29.9 ms at 10000
     * kbps, 26.2 at 5000. The error is L - target, not wrapped: one frame too many is removed by
     * sliding the phase (the core a little faster for a few seconds), without dropping pictures */
    if (g.gl_lag_st == 2 && g.lstat) {
      double Lt = g.lx50 + P + 1000;
      if (!g.lag_ctl)
        printf("[genlock] +1: latency %.1f ms, target %.1f ms (ready after arrival: median %.1f, 99.9%% %.1f ms)\n",
               g.lL / 1000, Lt / 1000, g.lx50 / 1000, g.lx999 / 1000);
      g.lag_ctl = true;
      g.lag_Lt = Lt;
      e = Lt - g.lL;
    }
    double pc = e / 150.0;
    /* the rate: the continuous measurement as soon as it exists (and the integral no longer moves with the error) */
    if (g.fe_valid) {
      double dF = g.fe_F - g.gl_integ;
      if (dF > 0.5) dF = 0.5;                 /* at most 0.5 us a frame per picture (at the start) */
      if (dF < -0.5) dF = -0.5;
      g.gl_integ += dF;
    }
    if (g.gl_relock) {
      g.gl_relock = false;
      g.fe_have = false;                      /* the pickups' phase jumps (raster changed): measure again */
      if (g.gl_locked) { g.gl_locked = false; g.gl_good = 0; g.gl_bad_t0 = 0; g.gl_eema = 5000; }
    }
    if (g.gl_locked) {
      if (!g.fe_valid) g.gl_integ += e * 1e-6;
      if (pc > 3) pc = 3;
      if (pc < -3) pc = -3;
      if (fabs(e) > 5000) {
        if (!g.gl_bad_t0) g.gl_bad_t0 = w;
        else if (w - g.gl_bad_t0 > 10e6) {
          g.gl_locked = false; g.gl_good = 0; g.gl_bad_t0 = 0; g.gl_eema = 5000;
          printf("[genlock] phase lost (error %.1f ms for 10 s): re-locking\n", e / 1000);
        }
      } else {
        g.gl_bad_t0 = 0;
      }
    } else {
      if (!g.fe_valid) g.gl_integ += e * 8e-6;
      /* locked when the MEAN error of the last ~2 s is under 1 ms (not every sample under 0.8: with
       * the Wi-Fi it sometimes never happened, and the locking mode, which moves the rate as much as
       * needed, stayed for the whole session) */
      g.gl_eema += (fabs(e) - g.gl_eema) / 120.0;
      if (++g.gl_good > 180 && g.gl_eema < 1000 + 0.1 * g.q) {
        g.gl_locked = true;
        printf("[genlock] locked: core at %.4f Hz, margin %.1f ms\n", g.Pm > 0 ? 1e6 / g.Pm : 0.0, g.q / 1000);
      }
    }
    if (g.gl_integ > 30) g.gl_integ = 30;
    if (g.gl_integ < -30) g.gl_integ = -30;
    double corr = pc + g.gl_integ;            /* us a frame */
    int trim = (int)lround(corr * 65536.0 / P);
    if (trim > 127) trim = 127;
    if (trim < -127) trim = -127;
    g.gl_trim = trim;
    g.gl_target = tg;
    g.gl_err = e;
    g.gl_phv = phv;
    g.gl_err_abs += fabs(e);
    g.gl_n++;
    if (trim != g.gl_trim_sent) { gl_send = trim; g.gl_trim_sent = trim; }
  }
  /* the danger zone: around the deadline (P - D: after it, the picture is not ready for the next
   * pickup; D varies from picture to picture, the zone is wide accordingly) */
  double d = P - g.D;
  double zc = d + 500;
  double h = 3 * g.mad + G_MARGIN_US;        /* the arrivals' cloud, with a margin */
  double zw = h + 1500;
  if (g.gl_state) {
    if (g.mode) g.switches++;
    g.mode = 0;                              /* with the genlock (or while trying it) the guard keeps quiet */
  } else if (!guard_on) {
    g.mode = 0;
  } else if (g.arrivals > 60) {
    if (2 * h > 0.8 * P) {
      if (g.mode) g.switches++;
      g.mode = 0;                            /* arrivals too spread: no cut helps */
    } else if (g.mode == 0 && g_cdist(phi, zc, P) < 0.8 * zw) {
      g.mode = 1; g.fixed_cut = g_wrap(phi - h, P); g.switches++; g.good_run = 0;
    } else if (g.mode == 1) {
      if (g_cdist(phi, g.fixed_cut, P) < 0.6 * h) {
        if (g_cdist(phi, zc, P) > zw) g.mode = 0;
        else g.fixed_cut = g_wrap(phi - h, P);
        g.switches++;
        g.good_run = 0;
      } else if (g_cdist(phi, zc, P) > 1.3 * zw) {
        if (++g.good_run > 180) { g.mode = 0; g.switches++; g.good_run = 0; }
      } else {
        g.good_run = 0;
      }
    }
  }
  /* mode 0 (arrivals far from the deadline): nothing to do. Mode 1: a cut just before the cloud,
   * which goes entirely to the following pickup */
  double c = g.mode == 0 ? d : g.fixed_cut;
  g.cut = c;
  /* the expected pickup: the first after the picture is ready (a + D; with the genlock D can exceed
   * a frame: with "a < d ? e_next : e_next + P" a picture ready after two pickups counted as missed
   * and D rose by itself to the maximum) */
  double hold_until = 0, target = w - a + ceil((a + g.D) / P) * P;
  if (g.mode == 1) {
    if (c <= d) {
      if (a < c) target = e_next;
      else { target = e_next + P; hold_until = e_next + 200; }
    } else {
      if (a < c) { target = e_next + P; hold_until = e_next + 200; }
      else { target = e_next + 2 * P; hold_until = e_next + P + 200; }
    }
  }
  /* the genlock: each picture has its pickup (the one after v + D + margin). Arriving much earlier
   * it would be ready a frame early and collide with the previous one (one dropped, one repeated):
   * it is held, but only until 0.25 us a byte minus 1 ms BEFORE the previous pickup (a low bound of
   * its decode time), not until after that pickup: so a large picture has more than a frame to
   * decode. On the trace of 14/11 (3 min, Wi-Fi), replayed with the decode time estimated from the
   * trace itself (5 models): from 70-80 repeats to 1-8, no collision; holding until after the
   * previous pickup caused almost all of them (pictures over 19 KB want more than 15.7 ms).
   * client/tools/genlock_replay.py */
  if (G_GL_HOLD && g.gl_state == 2 && g.fll_done && have_v) {
    double tr = v_abs + g.D + g.q + 500;
    double ei = e0 + ceil((tr - e0) / P) * P;
    double rel = ei - P - (0.25 * du->fullLength - 1000);
    if (w < rel && rel - w < P)
      hold_until = rel;
  }
  double tr_e = g.gl_err, tr_tg = g.gl_target, tr_q = g.q, tr_D = g.D;
  int tr_trim = g.gl_trim_sent;
  bool tr_lk = g.gl_locked, tr_gl = g.gl_state == 2;
  pthread_mutex_unlock(&g.mx);
  if (gl_send != -1000) {
    pthread_mutex_lock(&ring_mx);
    if (ring) h264ring_set_genlock(ring, gl_send != 1001, gl_send == 1001 ? 0 : gl_send);
    pthread_mutex_unlock(&ring_mx);
  }
  if (hold_until > w) {
    for (;;) {
      double t = (double)now_us();
      if (t >= hold_until || !g.run || !streaming)
        break;
      double left = hold_until - t;
      usleep(left > 2000 ? 1000 : (useconds_t)(left > 50 ? left : 50));
    }
    *held_us = (uint64_t)((double)now_us() - w);
  }
  (void)tr_gl;
  tr_printf("F,%.0f,%llu,%u,%d,%d,%.0f,%.0f,%.0f,%.0f,%.0f,%d,%d,%llu,%.0f\n", w, (unsigned long long)du->receiveTimeUs,
              rtp, du->fullLength, idr, have_v ? phv : -1.0, tr_e, tr_tg, tr_q, tr_D, tr_trim, tr_lk,
              (unsigned long long)*held_us, v_abs);
  pthread_mutex_lock(&g.mx);
  lat_written(have_v ? v_abs : 0);
  if (g.npend == 4) {
    memmove(&g.pend[0], &g.pend[1], 3 * sizeof(g.pend[0]));
    g.npend = 3;
  }
  g.pend[g.npend].wr = (double)now_us();
  g.pend[g.npend].ev = target;
  g.pend[g.npend].idr = idr;
  g.npend++;
  if (*held_us) { g.held++; g.held_us_sum += (double)*held_us; }
  pthread_mutex_unlock(&g.mx);
}

static void guard_report(void) {
  if (!g.th_ok)
    return;
  pthread_mutex_lock(&g.mx);
  double P = g.P;
  double phi = g_wrap(atan2(g.ey, g.ex) * P / (2 * M_PI), P);
  if (g.gl_state == 2)
    printf("[genlock] %s: PC rhythm %.1f ms from the pickup (target %.1f, mean error %.2f ms), margin %.1f ms "
           "(arrival spread %.1f ms), decoding %.1f ms, latency ~%.1f ms; trim %+d (%.1f us a frame from the PC), core at %.4f Hz, %u missed\n",
           g.gl_locked ? "locked" : "locking", g.gl_phv / 1000, g.gl_target / 1000, g.gl_n ? g.gl_err_abs / g.gl_n / 1000 : 0.0,
           g.q / 1000, g.mad / 1000, g.D / 1000, (g.D + g.q + 500) / 1000,
           g.gl_trim, g.gl_integ, g.Pm > 0 ? 1e6 / g.Pm : 0.0, g.misses);
  if (g.gl_state == 2)
    printf("[genlock] PC rate: %s %+.2f us a frame (%.4f Hz), phase %.2f ms off the line\n",
           g.fe_valid ? "measured" : "startup estimate (measurement not ready yet)", g.fe_valid ? g.fe_F : g.gl_integ,
           1e6 / (P + (g.fe_valid ? g.fe_F : g.gl_integ)), g.fe_sd / 1000);
  if (g.gl_lag_st)
    printf("[genlock] reserve (genlock +1): %s\n", g.gl_lag_st == 2 ? "applied" : g.gl_lag_st == 3 ? "refused by the core" : "asked, never applied");
  if (g.gl_state == 2 || g.gl_pll_fail)
    printf("[genlock] video PLL: %s%s%s, state %u%s\n",
           (g.w45 >> 25) & 1 ? "read" : "NOT read (the core does not move it)",
           (g.w45 >> 7) & 1 ? " (the Fitter's fixed values: the read gave nothing)" : "",
           (g.w45 >> 26) & 1 ? ", moving" : "", (unsigned)(g.w45 >> 27) & 15u,
           (g.w45 >> 31) & 1 ? ", waitrequest high" : "");
  else
    printf("[guard] arrivals %.1f ms from the pickup (spread %.1f ms), decoding %.1f ms, cut at %.1f ms (%s); "
           "%u held (mean %.1f ms), %u missed, %u cut changes\n",
           phi / 1000, g.mad / 1000, g.D / 1000, g.cut / 1000, g.mode ? "on" : "off (arrivals far)",
           g.held, g.held ? g.held_us_sum / 1000 / g.held : 0.0, g.misses, g.switches);
  if (g.r_min)
    printf("[core] frames at %.3f-%.3f Hz (per second, on the ARM's clock)\n", g.r_min, g.r_max);
  g.r_min = g.r_max = 0;
  g.gl_err_abs = 0;
  g.gl_n = 0;
  g.held = g.misses = g.switches = g.hits = 0;
  g.held_us_sum = 0;
  pthread_mutex_unlock(&g.mx);
}

void mg_core_poll(void) {
  if (restarting)
    return;
  pthread_mutex_lock(&ring_mx);
  poll_status_locked();
  /* outside the stream (menu): a lost session is reopened at once, otherwise
   * the overlay disappears */
  if (ring && !gone && !session_ok && !streaming && now_ms() - restart_ms >= RESTART_MS)
    session_start_locked();
  pthread_mutex_unlock(&ring_mx);
}

void mg_core_get_info(mg_core_info *ci) {
  struct h264ring_status st;
  memset(ci, 0, sizeof(*ci));
  pthread_mutex_lock(&ring_mx);
  int got = ring && h264ring_status(ring, &st) == 0;
  pthread_mutex_unlock(&ring_mx);
  if (!got)
    memset(&st, 0, sizeof(st));
  unsigned iface = (unsigned)(st.w[1] & 0xFFFFu);
  if (iface >= 0x103u) {
    /* the core in core/ (interface 1.3+): Output Mode and 240p Playback in word 87.
     * 240p/288p has constant-length lines: 27 MHz / (1720 x 262) = 59.914 Hz,
     * 27 MHz / (1728 x 312.5) = 49.993 Hz */
    uint64_t w87 = st.w[87];
    unsigned std = (unsigned)(w87 >> 11) & 3u;
    bool il = (w87 >> 13) & 1u, p240 = (w87 >> 4) & 1u;
    ci->p240 = p240 && il;
    if (ci->p240)
      ci->refresh_x100 = std == 2 ? 4999 : std == 1 ? 6000 : 5991;   /* 1: the core in core/ (60.000 Hz) */
    else
      ci->refresh_x100 = std == 2 ? 5000 : std == 1 ? 6000 : 5994;
    ci->interlaced = il && !ci->p240;
  } else {
    unsigned refresh = (unsigned)H264RING_ST(&st, OSD_REFRESH);
    unsigned std = (unsigned)H264RING_ST(&st, RASTER_STD);
    /* Refresh Rate: 0 Auto (follows the stream's rhythm: asking Sunshine for 59.94
     * it stays 59.94), 1 59.94, 2 60, 3 50 */
    if (refresh == 3 || (refresh == 0 && std == 2))
      ci->refresh_x100 = 5000;
    else if (refresh == 2)
      ci->refresh_x100 = 6000;
    else
      ci->refresh_x100 = 5994;
    ci->interlaced = H264RING_ST(&st, RASTER_IL) != 0;
  }
  ci->raster_h = ci->refresh_x100 == 5000 || ci->refresh_x100 == 4999 ? 576 : 480;
  struct h264ring_raster rs;
  h264ring_raster(&st, &rs);
  ci->raster_w = rs.mode == H264RING_RASTER_FITTED && rs.w ? (int)rs.w : 720;
  struct h264ring_caps caps;
  h264ring_caps(&st, &caps);
  ci->max_mbs = caps.max_mbs;
  ci->mb_rate = caps.mb_rate;
}

/* ---------- overlay ---------- */

static bool fb_mapped; /* from h264ring_fb_map (not a test framebuffer) */

static void overlay_layout(void) {
  ovl_max_w = 720;       /* the widest raster: 720 (original core); 640 the one in core/ */
  ovl_w = 720;
  ovl_h = OVL_MAX_H;
  static const struct { unsigned fmt, bpp, pages; } tries[] = {
    { H264RING_OVL_FMT_8888, 4, 2 }, { H264RING_OVL_FMT_1555, 2, 2 },
    { H264RING_OVL_FMT_8888, 4, 1 }, { H264RING_OVL_FMT_1555, 2, 1 },
  };
  for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
    uint32_t pb = (uint32_t)ovl_max_w * tries[i].bpp * (uint32_t)ovl_h;
    if (pb * tries[i].pages <= fb.len) {
      ovl_fmt = tries[i].fmt;
      ovl_bpp = tries[i].bpp;
      ovl_pages = tries[i].pages;
      ovl_page_bytes = pb;
      fb_ok = true;
      break;
    }
  }
  if (!fb_ok) {
    printf("[core] framebuffer too small (%u bytes): no on-screen menu\n", (unsigned)fb.len);
    if (fb_mapped)
      h264ring_fb_unmap(&fb);
    fb_mapped = false;
    return;
  }
  memset(fb.virt, 0, (size_t)ovl_page_bytes * ovl_pages);
  printf("[core] overlay up to %dx%d, %u bytes per pixel, %u pages (framebuffer %u bytes)\n", ovl_max_w, ovl_h, ovl_bpp,
         ovl_pages, (unsigned)fb.len);
}

static void overlay_setup(void) {
  int rc = h264ring_fb_map(&fb);
  if (rc) {
    printf("[core] framebuffer not available (%s): no on-screen menu\n", strerror(-rc));
    return;
  }
  fb_mapped = true;
  overlay_layout();
}

void mg_core_use_fb(void *virt, uint32_t phys, uint32_t len) {
  memset(&fb, 0, sizeof(fb));
  fb.virt = virt;
  fb.phys = phys;
  fb.len = len;
  fb.fd = -1;
  fb_mapped = false;
  overlay_layout();
}

/* a pixel in the overlay's format (8888: bytes B, G, R, A = 0xAARRGGBB; 1555: A R G B) */
static uint32_t ovl_px(uint32_t c) {
  if (ovl_bpp == 4)
    return c | 0xFF000000u;
  return 0x8000 | (((c >> 19) & 0x1F) << 10) | (((c >> 11) & 0x1F) << 5) | ((c >> 3) & 0x1F);
}

/* a canvas line widened to the whole raster (320 -> 720: x2.25, interpolated as the core does
 * with the video: on the CRT no alternating 2- and 3-pixel wide columns are visible) */
static void ovl_line(uint8_t *dst, const uint32_t *src, int w) {
  if (ovl_w == 2 * w) {          /* 640 raster (core/): exactly x2, no interpolation */
    for (int x = 0; x < w; x++) {
      uint32_t v = ovl_px(src[x]);
      if (ovl_bpp == 4) {
        ((uint32_t *)dst)[2 * x] = v;
        ((uint32_t *)dst)[2 * x + 1] = v;
      } else {
        ((uint16_t *)dst)[2 * x] = (uint16_t)v;
        ((uint16_t *)dst)[2 * x + 1] = (uint16_t)v;
      }
    }
    return;
  }
  for (int x = 0; x < ovl_w; x++) {
    int pos = (int)(((2 * x + 1) * w * 256LL) / (2 * ovl_w)) - 128; /* the pixel's centre, 1/256 */
    if (pos < 0) pos = 0;
    int i = pos >> 8, f = pos & 255;
    if (i >= w - 1) { i = w - 1; f = 0; }
    uint32_t a = src[i], b = f ? src[i + 1] : a, c = 0;
    for (int sh = 0; sh < 24; sh += 8) {
      uint32_t ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
      c |= ((ca * (256 - f) + cb * f + 128) >> 8) << sh;
    }
    if (ovl_bpp == 4)
      ((uint32_t *)dst)[x] = ovl_px(c);
    else
      ((uint16_t *)dst)[x] = (uint16_t)ovl_px(c);
  }
}

static void ovl_fill(uint8_t *dst, uint32_t c) {
  for (int x = 0; x < ovl_w; x++) {
    if (ovl_bpp == 4)
      ((uint32_t *)dst)[x] = ovl_px(c);
    else
      ((uint16_t *)dst)[x] = (uint16_t)ovl_px(c);
  }
}

/* The canvas (w x h) on the whole raster: as wide as the raster (720 pixels at 13.5 MHz, the
 * CRT's full width), doubled lines, centred vertically in 576 lines (above and below the
 * colour of its corner): nothing of the core underneath shows */
int mg_core_overlay_present(const uint32_t *canvas, int w, int h) {
  if (!fb_ok || !ring || w < 2 || w > ovl_max_w || h * 2 > OVL_MAX_H)
    return -1;
  mg_core_info ci;
  mg_core_get_info(&ci);
  /* as wide as the core's raster (word 80): 720 on the original core, 640 on the one in core/ */
  ovl_w = ci.raster_w >= 2 * w && ci.raster_w <= ovl_max_w ? ci.raster_w : ovl_max_w;
  int oh = ci.raster_h > 0 && ci.raster_h <= OVL_MAX_H ? ci.raster_h : 480;
  if (h * 2 > oh)
    oh = h * 2;
  /* the page to draw must not be the one the core is reading */
  unsigned page = ovl_pages > 1 ? ovl_page ^ 1 : 0;
  if (ovl_pages > 1 && ovl_gen > 0) {
    pthread_mutex_lock(&ring_mx);
    h264ring_overlay_wait(ring, ovl_gen, 50);
    pthread_mutex_unlock(&ring_mx);
  }
  uint8_t *base = (uint8_t *)fb.virt + (size_t)page * ovl_page_bytes;
  size_t stride = (size_t)ovl_w * ovl_bpp;
  int y0 = (oh - 2 * h) / 2;
  uint32_t border = canvas[0];
  static uint8_t line[2048 * 4];
  for (int y = 0; y < oh; y++) {
    int sy = (y - y0) >> 1;
    bool in = y >= y0 && sy < h;
    /* two equal lines in a row: the second is copied from the first */
    if (in && ((y - y0) & 1)) {
      memcpy(base + (size_t)y * stride, line, stride);
      continue;
    }
    if (in)
      ovl_line(line, canvas + (size_t)sy * w, w);
    else
      ovl_fill(line, border);
    memcpy(base + (size_t)y * stride, line, stride);
  }
  int ovl_h_now = oh;
  __sync_synchronize();
  struct h264ring_overlay o;
  memset(&o, 0, sizeof(o));
  o.base = fb.phys + page * ovl_page_bytes;
  o.width = (uint16_t)ovl_w;
  o.height = (uint16_t)ovl_h_now;
  o.stride = (uint16_t)stride;
  o.format = (uint8_t)ovl_fmt;
  o.band_first = 0;
  o.band_last = (uint16_t)(ovl_h_now - 1);
  pthread_mutex_lock(&ring_mx);
  int gen = h264ring_overlay_show(ring, &o);
  pthread_mutex_unlock(&ring_mx);
  if (gen < 0) {
    printf("[core] overlay refused (%s)\n", strerror(-gen));
    return gen;
  }
  if (!ovl_shown)
    printf("[core] on-screen menu visible\n");
  ovl_gen = gen;
  ovl_page = page;
  ovl_shown = true;
  return 0;
}

void mg_core_overlay_hide(void) {
  if (!ring || !ovl_shown)
    return;
  pthread_mutex_lock(&ring_mx);
  int gen = h264ring_overlay_hide(ring);
  pthread_mutex_unlock(&ring_mx);
  if (gen > 0)
    ovl_gen = gen;
  ovl_shown = false;
}

/* ---------- opening ---------- */

int mg_core_attach(h264ring *r, void (*on_gone)(void), void (*on_refused)(const char *why)) {
  gone_cb = on_gone;
  refused_cb = on_refused;
  pthread_mutex_lock(&ring_mx);
  ring = r;
  gone = false;
  int rc = session_start_locked();
  pthread_mutex_unlock(&ring_mx);
  return rc;
}

static const char *core_rbf; /* NULL: MiSTer_moonlight/core/H264 if present, otherwise _Other/H264 */
void mg_core_set_rbf(const char *rbf) { core_rbf = (rbf && *rbf) ? rbf : NULL; }

/* the core lives with Moonlight (MiSTer_moonlight/core): from the MiSTer's menu, on its own, it
 * would only show a black screen. _Other/H264 stays for whoever put it there */
static const char *core_default(void) {
  glob_t gb;
  bool here = glob("/media/fat/MiSTer_moonlight/core/H264_*.rbf", 0, NULL, &gb) == 0 && gb.gl_pathc > 0;
  globfree(&gb);
  return here ? "MiSTer_moonlight/core/H264" : NULL;
}

int mg_core_open(void (*on_gone)(void), void (*on_refused)(const char *why)) {
  if (!core_rbf) core_rbf = core_default();
  printf("[core] loading the H264 core%s%s...\n", core_rbf ? " from " : "", core_rbf ? core_rbf : "");
  int rc = h264ring_launch_core(core_rbf, LAUNCH_MS);
  if (rc) {
    printf("[core] the H264 core does not start (%s): %s_*.rbf is needed on the SD\n", strerror(-rc),
           core_rbf ? core_rbf : "MiSTer_moonlight/core/H264");
    return rc;
  }
  h264ring *r;
  rc = h264ring_open_devmem(&r, 0);
  if (rc) {
    printf("[core] attaching to the core failed (%s)%s\n", strerror(-rc),
           rc == -EBUSY ? ": another program is using it" : "");
    return rc;
  }
  rc = mg_core_attach(r, on_gone, on_refused);
  if (rc)
    return rc;
  overlay_setup();
  mg_core_info ci;
  mg_core_get_info(&ci);
  struct h264ring_status st;
  pthread_mutex_lock(&ring_mx);
  unsigned iface = h264ring_status(ring, &st) == 0 ? (unsigned)(st.w[1] & 0xFFFFu) : 0;
  pthread_mutex_unlock(&ring_mx);
  printf("[core] attached (interface %u.%u): frames %.2f Hz, %s, up to %u macroblocks (%u a second)\n",
         iface >> 8, iface & 0xFF, ci.refresh_x100 / 100.0,
         ci.p240 ? "240p during the stream" : ci.interlaced ? "interlaced" : "progressive", ci.max_mbs, ci.mb_rate);
  return 0;
}

void mg_core_close(void) {
  pthread_mutex_lock(&ring_mx);
  if (ring) {
    if (ovl_shown)
      h264ring_overlay_hide(ring);
    h264ring_close(ring);
  }
  ring = NULL;
  session_ok = false;
  pthread_mutex_unlock(&ring_mx);
  if (fb_ok && fb_mapped)
    h264ring_fb_unmap(&fb);
  fb_ok = fb_mapped = false;
  ovl_shown = false;
}

/* ---------- stream ---------- */

void mg_core_stream_begin(void) {
  mg_core_overlay_hide();
  pthread_mutex_lock(&ring_mx);
  if (ring && !gone) {
    if (!session_ok)
      session_start_locked();
    else if (h264ring_flush(ring, FLUSH_MS) != 0)
      session_start_locked();
    struct h264ring_status st;
    if (h264ring_status(ring, &st) == 0) {
      err_base = err_sum(&st);
      refused_base = (uint32_t)H264RING_ST(&st, FE_REFUSED);
    }
  }
  video_started = false;
  need_idr = true;
  idr_asked_ms = idr_written_ms = 0;
  cavlc_refused = false;
  rt.t0_ms = 0;
  {
    struct h264ring_status st;
    pace_written = 0;
    pace_base = 0;
    if (ring && h264ring_status(ring, &st) == 0)
      pace_base = (uint32_t)H264RING_ST(&st, DISP_PICK) + (uint32_t)H264RING_ST(&st, DISP_SKIP);
  }
  streaming = true;
  pthread_mutex_unlock(&ring_mx);
  guard_start();
}

void mg_core_stream_end(void) {
  guard_stop();
  pthread_mutex_lock(&ring_mx);
  streaming = false;
  if (ring && !gone && session_ok && h264ring_flush(ring, FLUSH_MS) != 0)
    session_ok = false;
  pthread_mutex_unlock(&ring_mx);
  mg_core_poll(); /* reopens the session if the flush lost it */
}

static int write_record(bool audio, const void *p, uint32_t len, bool idr, unsigned wait_ms) {
  uint64_t t0 = now_ms();
  for (;;) {
    int rc;
    if (audio && restarting)
      return -ENOTCONN;
    pthread_mutex_lock(&ring_mx);
    if (!ring || !session_ok || !streaming) {
      rc = -ENOTCONN;
    } else if (audio && !video_started) {
      rc = W_EARLY;
    } else {
      rc = audio ? h264ring_try_write_audio(ring, p, len, -1)
                 : h264ring_try_write_au(ring, p, len, -1, AU_FLAGS);
      if (rc == 0 && idr) {
        video_started = true;
        idr_written_ms = now_ms();
      }
      if (rc == -ENOTCONN || rc == -EPROTO)
        session_ok = false;
    }
    pthread_mutex_unlock(&ring_mx);
    if (rc != -EAGAIN)
      return rc;
    if (now_ms() - t0 >= wait_ms)
      return -ETIMEDOUT;
    usleep(1000);
  }
}

static int video_setup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags) {
  if (!(videoFormat & VIDEO_FORMAT_MASK_H264)) {
    printf("[core] the core decodes only H.264\n");
    return -1;
  }
  if (!ring) {
    printf("[core] not attached to the core\n");
    return -1;
  }
  mg_core_info ci;
  mg_core_get_info(&ci);
  long mbs = (long)((width + 15) / 16) * ((height + 15) / 16);
  if (mbs > (long)ci.max_mbs) {
    printf("[core] %dx%d is %ld macroblocks: the core decodes at most %u\n", width, height, mbs, ci.max_mbs);
    return -1;
  }
  if (mbs * redrawRate > (long)ci.mb_rate * 101 / 100)
    printf("[core] warning: %dx%d at %d fps exceeds the core's %u macroblocks a second\n", width, height,
           redrawRate, ci.mb_rate);
  printf("[core] video %dx%d at %d fps\n", width, height, redrawRate);
  return 0;
}

static void video_cleanup(void) {
  free(au_buf);
  au_buf = NULL;
  au_cap = 0;
  pthread_mutex_lock(&ring_mx);
  if (ring) {
    struct h264ring_cavlc_stats cs;
    h264ring_rewrite_stats(ring, &cs);
    if (cs.units_rewritten)
      printf("[core] CAVLC: %llu access units rewritten to CABAC on the ARM (turn CAVLC off in Sunshine)\n",
             (unsigned long long)cs.units_rewritten);
  }
  pthread_mutex_unlock(&ring_mx);
}

static int video_submit(PDECODE_UNIT du) {
  bool idr = du->frameType == FRAME_TYPE_IDR;

  pthread_mutex_lock(&ring_mx);
  if (ring && !session_ok && !gone && now_ms() - restart_ms >= RESTART_MS)
    session_start_locked();
  bool wait_idr = need_idr;
  bool ok = session_ok && !cavlc_refused;
  if (idr) {
    need_idr = false;
    idr_asked_ms = 0;
  }
  pthread_mutex_unlock(&ring_mx);

  if (!ok || (wait_idr && !idr)) {
    pthread_mutex_lock(&ring_mx);
    poll_status_locked();
    pthread_mutex_unlock(&ring_mx);
    n.au_dropped++;
    return cavlc_refused ? DR_OK : DR_NEED_IDR;
  }

  if ((size_t)du->fullLength > au_cap) {
    size_t cap = (size_t)du->fullLength * 2;
    uint8_t *b = realloc(au_buf, cap);
    if (!b) {
      n.au_dropped++;
      return DR_NEED_IDR;
    }
    au_buf = b;
    au_cap = cap;
  }
  size_t len = 0;
  for (PLENTRY e = du->bufferList; e != NULL; e = e->next) {
    memcpy(au_buf + len, e->data, (size_t)e->length);
    len += (size_t)e->length;
  }

  uint64_t held_us;
  bool forced = false;
  if (guard_on || genlock_req)               /* the arrivals are needed by the genlock alone too */
    guard_wait(idr, &held_us, du);
  else
    pace_wait(&held_us, &forced);
  int rc = write_record(false, au_buf, (uint32_t)len, idr, VIDEO_WAIT_MS);
  if (rc == 0)
    pace_written++;
  if (rc == -EOPNOTSUPP) {
    pthread_mutex_lock(&ring_mx);
    printf("[core] the stream cannot be given to the core (%s)\n", h264ring_rewrite_why(ring));
    snprintf(refused_why, sizeof(refused_why), "Stream not suited to the core: %s", h264ring_rewrite_why(ring));
    cavlc_refused = true;
    pthread_mutex_unlock(&ring_mx);
    if (refused_cb)
      refused_cb(refused_why);
    n.refusals++;
    n.au_dropped++;
    return DR_OK;
  }
  if (rc == -EBADMSG) {
    pthread_mutex_lock(&ring_mx);
    printf("[core] access unit %lu not rewritten (%s)\n", n.au, h264ring_rewrite_why(ring));
    need_idr = true;
    pthread_mutex_unlock(&ring_mx);
    n.au_dropped++;
    return DR_NEED_IDR;
  }
  if (rc) {
    pthread_mutex_lock(&ring_mx);
    if (rc != -ENOTCONN && !gone)
      printf("[core] access unit %lu not written (%s)\n", n.au, strerror(-rc));
    need_idr = true;
    poll_status_locked();
    pthread_mutex_unlock(&ring_mx);
    n.au_dropped++;
    return DR_NEED_IDR;
  }
  n.au++;
  n.au_bytes += len;

  rhythm_au(du, held_us, forced);
  pthread_mutex_lock(&ring_mx);
  {
    struct h264ring_cavlc_stats cs;
    h264ring_rewrite_stats(ring, &cs);
    if (cs.units_rewritten && !n.au_rewritten)
      printf("[core] the stream is CAVLC: rewriting to CABAC on the ARM (slow: turn CAVLC off in Sunshine)\n");
    n.au_rewritten = (unsigned long)cs.units_rewritten;
  }
  poll_status_locked();
  pthread_mutex_unlock(&ring_mx);
  return DR_OK;
}

DECODER_RENDERER_CALLBACKS mg_video_callbacks = {
  .setup = video_setup,
  .cleanup = video_cleanup,
  .submitDecodeUnit = video_submit,
  .capabilities = 0,
};

/* ---------- audio: Opus -> 48 kHz stereo PCM to the core ---------- */

static int audio_init(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION cfg, void *context, int arFlags) {
  if (cfg->sampleRate != H264RING_AUD_RATE) {
    printf("[core] audio at %d Hz: the core plays at 48 kHz\n", cfg->sampleRate);
    return -1;
  }
  int err;
  opus = opus_multistream_decoder_create(cfg->sampleRate, cfg->channelCount, cfg->streams, cfg->coupledStreams,
                                         cfg->mapping, &err);
  if (!opus) {
    printf("[core] Opus decoder: error %d\n", err);
    return -1;
  }
  pcm_frames = cfg->samplesPerFrame > 0 ? cfg->samplesPerFrame : 960;
  pcm_channels = cfg->channelCount;
  pcm = malloc(sizeof(int16_t) * (size_t)pcm_channels * (size_t)pcm_frames);
  pcm2 = malloc(sizeof(int16_t) * 2 * (size_t)pcm_frames);
  return pcm && pcm2 ? 0 : -1;
}

static void audio_cleanup(void) {
  if (opus)
    opus_multistream_decoder_destroy(opus);
  opus = NULL;
  free(pcm);
  free(pcm2);
  pcm = pcm2 = NULL;
}

static void audio_sample(char *data, int length) {
  /* data NULL: a lost packet, Opus conceals it and the core's queue stays full */
  int frames = opus_multistream_decode(opus, (const unsigned char *)data, length, pcm, pcm_frames, 0);
  if (frames <= 0) {
    n.aud_dropped++;
    return;
  }
  if (!data)
    n.aud_plc++;
  const int16_t *out = pcm;
  if (pcm_channels != 2) {
    for (int i = 0; i < frames; i++) {
      pcm2[2 * i] = pcm[i * pcm_channels];
      pcm2[2 * i + 1] = pcm[i * pcm_channels + (pcm_channels > 1 ? 1 : 0)];
    }
    out = pcm2;
  }
  int rc = write_record(true, out, (uint32_t)frames * 4u, false, AUDIO_WAIT_MS);
  if (rc == 0)
    n.aud++;
  else if (rc == W_EARLY)
    n.aud_early++;
  else
    n.aud_dropped++;
}

AUDIO_RENDERER_CALLBACKS mg_audio_callbacks = {
  .init = audio_init,
  .cleanup = audio_cleanup,
  .decodeAndPlaySample = audio_sample,
  .capabilities = CAPABILITY_DIRECT_SUBMIT,
};

void mg_core_get_counters(mg_core_counters *c) { *c = n; }

void mg_core_print_counters(void) {
  printf("[core] %lu access units (%lu bytes, %lu rewritten from CAVLC), %lu dropped; %lu decoder errors, "
         "%lu IDRs asked; %lu sessions; audio %lu blocks, %lu dropped, %lu before the video, %lu concealed\n",
         n.au, n.au_bytes, n.au_rewritten, n.au_dropped, n.errors_seen, n.idr_asked, n.sessions, n.aud,
         n.aud_dropped, n.aud_early, n.aud_plc);
}
