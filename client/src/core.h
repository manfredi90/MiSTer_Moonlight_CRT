#ifndef MG_CORE_H
#define MG_CORE_H

#include <Limelight.h>
#include <stdbool.h>
#include <stdint.h>

struct h264ring;

/* Loads the H264 core (unless it is already running), attaches to its ring and
 * opens a session that stays open for the whole run: the menu needs it too, since
 * the core shows the overlay only during a session. */
int  mg_core_open(void (*on_gone)(void), void (*on_refused)(const char *why));
/* The core file to load, without date and extension (default: MiSTer_moonlight/core/H264
 * if present, otherwise _Other/H264) */
void mg_core_set_rbf(const char *rbf);
/* For the tests: attaches to a ring that is already set up (a fake core in memory) */
int  mg_core_attach(struct h264ring *r, void (*on_gone)(void), void (*on_refused)(const char *why));
void mg_core_close(void);
/* For the tests: a framebuffer in memory instead of /dev/fb0 */
void mg_core_use_fb(void *virt, uint32_t phys, uint32_t len);
struct h264ring *mg_core_ring(void);

/* Re-reads the core's state: heartbeat (core unloaded?), OSD open, decode errors
 * (asks for an IDR), session lost (reopens it). Every ~50 ms. */
void mg_core_poll(void);
/* Diagnostics of the pictures' rhythm (-stats): every ~2 ms during the stream, from the
 * input thread; prints the skipped/repeated pictures and a summary every 10 s */
void mg_core_set_rhythm(bool on);
/* Pacing: a picture goes to the core only after the previous one was taken (default: off) */
void mg_core_set_pacing(bool on);
/* The phase guard (core with P3e): a picture that would land right at the pickup goes to the next one */
void mg_core_set_guard(bool on);
/* The genlock (this repository's core, interface mm 1.5): the core's phase follows the arrivals */
void mg_core_set_genlock(bool on);
/* With the genlock: the core keeps one picture in reserve (one more frame, like the guard) */
void mg_core_set_gl_lag(bool on);
/* After a session with genlock +1: why the reserve was missing ("" if it was there) */
const char *mg_core_gl_lag_problem(void);
/* After a session with the genlock requested: why it did not work ("" if it did) */
const char *mg_core_genlock_problem(void);
void mg_core_tick(void);
bool mg_core_osd_open(void);

/* The core's frame rate and the fps to ask Sunshine for (x100) */
typedef struct {
  int refresh_x100;   /* 5994, 6000, 5000; 5991 / 4999 a 240p / 288p */
  bool interlaced;    /* 480i/576i sull'uscita analogica */
  bool p240;          /* 240p/288p during the stream (core in core/, 240p Playback) */
  int raster_h;       /* 480 or 576 */
  int raster_w;       /* 720 (original core) or 640 (core in core/) */
  unsigned max_mbs, mb_rate;
} mg_core_info;
void mg_core_get_info(mg_core_info *ci);

/* Start and end of a stream: empties the ring and resets the decoder (black screen
 * until the first IDR), hides/shows the menu overlay. */
void mg_core_stream_begin(void);
void mg_core_stream_end(void);

/* Overlay: copies the canvas (0xRRGGBB, w x h) doubled and shows it */
int  mg_core_overlay_present(const uint32_t *canvas, int w, int h);
void mg_core_overlay_hide(void);

extern DECODER_RENDERER_CALLBACKS mg_video_callbacks;
extern AUDIO_RENDERER_CALLBACKS mg_audio_callbacks;

typedef struct {
  unsigned long au, au_bytes, au_dropped, idr_asked, errors_seen, sessions, refusals;
  unsigned long aud, aud_dropped, aud_early, aud_plc, au_rewritten;
} mg_core_counters;
void mg_core_get_counters(mg_core_counters *c);
void mg_core_print_counters(void);

#endif
