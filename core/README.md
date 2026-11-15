# H264 core for MiSTer Moonlight

The MiSTer H264 core (owenb321/MiSTer_h264_decoder, GPL-3.0-or-later) as extended by
MisterZine Jellyfin (matijaerceg/misterzine-jellyfin, `third_party/MiSTer_h264_decoder` at
b522f4f18b9b6f4688f572c32eea841ce8d99dc2): HPS interface 1.4, Output Mode, and **240p Playback**, which shows a playing stream
on a progressive 240p raster (288p at 50 Hz) at a constant line length, 59.914 Hz (49.993 Hz).

Changes here:

- **640-pixel raster, exactly 60.000 Hz, constant line lengths.** The raster's dot is 24.759 MHz
  (`clk_vid` 74.277 MHz, `h264/vid_pll.sv`) and every mode is 412,650 dots a refresh:
  480p 786 x 525, 480i 1,572 x 525 (two fields, the half-line interlace), 240p 1,575 x 262
  (`h264/sync_gen.sv`). 640 active pixels: a 640x480 stream is shown 1:1, a 320x240 one exactly
  x2, no horizontal scaling. The display's grid (`h264/disp_ctl.sv`) is 450,000 clk_sys cycles
  for every mode; the fit (`h264/disp_fit.sv`) works on square pixels; the composite sync
  (`h264/csync_smpte.sv`) keeps the SMPTE pulse widths on the new line.
- **No 50 Hz, no native HDMI raster, no refresh matching, no film speed** (60.000 Hz without the
  genlock, the PC's rate with it). Output Mode
  is 15kHz (480i) or 31kHz (480p); at 15kHz, 15kHz Mode 240p (upstream's 240p Playback) switches a
  playing stream to 240p. HDMI still gets the picture through the MiSTer scaler.
- No *Deinterlace* entry: it only applies to field-coded streams, which Sunshine never sends (the
  field path stays, at Weave). No *HDMI Deinterlace* entry either (mm-0.4.1): the scaler weaves 480i;
  for HDMI, Output Mode 31kHz.
- **Genlock (interface mm 1.5).** Program word 34 gains GL_EN [24], GL_TRIM [23:16] (signed, in
  2^-16 of the pixel clock, positive = slower) and GL_LAG [25]; status word 45 mirrors them and the
  PLL's state. While GL_EN, `h264/vid_pll_tune.sv` rewrites the video PLL's fractional divider
  (`h264/vid_pll.sv`, a physical Cyclone V PLL so its reconfiguration ports exist), gliding a quarter
  unit every 10 ms: the raster keeps its lines and lengths, the whole refresh runs at the PC's rate.
  GL_LAG keeps one decoded picture in reserve (`h264/disp_ctl.sv` drops the head only when two wait
  behind it): one frame of latency, no repeat when a large picture decodes late. The client measures
  the PC's rate from the RTP timestamps and steers the phase so each picture is ready just before its
  pickup (`-genlock`, `-genlock-lag`).
- **The picture closes at its record's end (P3e).** On a LOW_LATENCY stream the reader's `es_au_end`
  crosses to the decoder, which closes the picture there instead of at the next picture's first slice.
- The core is named H264 (`_Other/H264_*.rbf`), the pad's J line is upstream's twelve
  Xbox-style buttons (the input region carries them as they are), the OSD's *Send Home to the
  program* and *Program menu* entries are back, `CORE_VERSION` is `mm-<version>`.

The client asks Sunshine for 60 fps (60.00), and draws its menu 640 wide.

Checked in simulation (`bench/h264`): `sync_gen_mm_tb` (all three modes against figures written
out independently: line lengths, active area, sync, 412,650 dots a refresh, the vsync edges a
refresh apart), `csync_field_tb` (480i: both fields' sync waveforms, the 262.5-line spacing),
`disp_fit_tb` (the RTL against `tools/disp_fit_ref.py`, the model against ffmpeg's scaler on
square pixels), `disp_sched_tb` (the display scheduler on the 60 Hz grid with the raster on its
own dot).

Built by `.github/workflows/core.yml` with Quartus 17.0.2 Lite, several fitter seeds in
parallel, the one with the best timing kept (artifact `H264_core`).
