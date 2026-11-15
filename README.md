# MiSTer Moonlight

A [Moonlight](https://moonlight-stream.org) client for [Sunshine](https://github.com/LizardByte/Sunshine)
that runs on the MiSTer FPGA's ARM and plays the stream through the
[H264 core](https://github.com/owenb321/MiSTer_h264_decoder), which decodes H.264 in the FPGA.
It is aimed at CRTs. With the core in `core/` the picture is 640 square pixels wide and the
raster follows the PC's own frame rate (genlock); the stock H264 core (480i at 59.94 Hz) works too.

- On-screen menu (drawn in the core's overlay plane): the list of PCs, Sunshine's apps, bitrate,
  audio, sync, the PC's resolution, pairing with an on-screen PIN. The stream is always 640x480
  at 60 fps.
- Sync with the PC, the core's video PLL following the PC's frame rate (see *Settings*).
- Pads, keyboard and mouse through the core (Main keeps `/dev/input` while a core runs),
  rumble on the MiSTer's pads, mouse mode on the pad.

## Layout

| Folder | What |
| --- | --- |
| `client/` | the program (C, cross-compiled for the Cortex-A9, static) |
| `core/` | a build of the H264 core: 640-pixel picture, 240p output, genlock |

## Install

1. Copy the release zip's `Scripts` and `MiSTer_moonlight` folders to the root of the SD card
   (`/media/fat`), and the H264 core (`H264_*.rbf`) into `MiSTer_moonlight/core`. The core lives
   with Moonlight, not in `_Other`: loaded on its own from the MiSTer's menu it would only show a
   black screen (`_Other/H264` is still used when `MiSTer_moonlight/core` is empty).
2. Run **Scripts → mister_moonlight**: the list of PCs opens. Choose *+ Add PC* and type the IP
   address of the PC with Sunshine (on-screen keyboard with the pad, or a real keyboard), then a
   name. If `mister_moonlight.conf` has `HOST=...`, that PC enters the list by itself.
3. The menu shows a PIN to enter in Sunshine's web page (PIN tab).

`MiSTer_moonlight/mister_moonlight.conf` is created from `mister_moonlight.conf.example` on the
first run; `HOST` there is required only with `MENU=0`.

In Sunshine, leave **CAVLC** off: the core decodes CABAC; a CAVLC stream is rewritten on the
ARM, which only keeps up at low bitrates.

## Controls

| | |
| --- | --- |
| Menu | d-pad or arrows, A / Enter / Start confirm, B / Esc back (from the list of PCs: exit) |
| List of PCs | A connects, right opens the PC's options (rename, change address, remove); from the apps, B goes back to the list |
| On-screen keyboard | A types the chosen key, Start / Enter confirm, B / Esc cancel; a real keyboard types directly |
| Back to the menu | Select+Start+LB+RB, Ctrl+Alt+Shift+Q, or *Program menu* in the MiSTer OSD |
| Guide | Select+Start, or *Send Home to the program* in the MiSTer OSD |
| Mouse mode | hold Start alone for 1 s (a short rumble confirms): left stick or d-pad = pointer, right stick = wheel, A/B/X = left/right/middle button |

## Settings

In the menu: bitrate (auto = 10 Mbps, up to 10 Mbps), audio, closing the app on exit, sync, the
PC's resolution. The choices are kept in `MiSTer_moonlight/menu.cfg`, with the list of PCs. The
stream is always 640x480 at 60 pictures a second (what the core shows 1:1); on the command line
`-width`, `-height` and `-fps` remain for tests.

**Sync** (with this repository's core):
- *genlock*: the core moves its video PLL and the raster runs at the PC's rate (59.94 Hz too),
  with no added frame of latency. It locks within 10-20 s of the start and after a 240p/480i
  switch.
- *genlock +1*: the same, and the core keeps one decoded picture in reserve (about 10 ms more
  latency). For high bitrates: a picture above ~20 KB takes the core more than a frame to decode,
  and without the reserve the previous picture is repeated.
- *guard*: a picture that would land right at the pickup goes to the next one (up to one more
  frame); *none*: nothing.

If the genlock does not work (a core without it), the menu says so at the stream's end.

**PC resolution** (off, 640x480, 1024x768, 1280x960, 1280x720): during the stream Sunshine sets
the desktop to that resolution and restores its own at the end. With a 16:9 desktop the 640x480
stream arrives letterboxed (Sunshine adds the bars); with a 4:3 resolution the picture fills the
screen. The stream stays 640x480 (so 1280x720 is still letterboxed). It needs a recent Sunshine
(one from a few months before November 2026 did not change the resolution) with **Advanced
Display Options > Resolution: "Use resolution provided by the client"**; the PC's monitor (or a
virtual display) must accept the resolution. On the command line: `-pcres 1024x768`.

The log is `MiSTer_moonlight/last_stream.log`, written to the SD when the program exits. With
`-stats` in `EXTRA` it also reports every 10 s the PC's frame rate and the repeated or dropped
frames, and `trace.csv` is written at the stream's end (for tests only).

## This repository's core

The core in `core/` (the `H264_core` artefact of the GitHub Actions) has a **640-pixel** wide
picture with equal lines: a 640x480 stream arrives 1:1, a 320x240 one exactly x2, without any
horizontal scaling. Without the genlock it runs at exactly 60.000 Hz; with it, at the PC's rate.

In the core's OSD: *Output Mode* 15kHz (480i) or 31kHz (480p); at 15kHz, *15kHz Mode* 240p
switches the stream to 240p (the menu stays at 480i; the 640x480 stream on alternate lines).
No 50 Hz.

With the original core (H264 v1.1.0) the client works too: Sunshine receives 59.94 fps and the
menu is widened to its 720-pixel picture.

## Build

On Linux (Ubuntu 24.04: `gcc-arm-linux-gnueabihf cmake ninja-build pkg-config`, plus
`qemu-user` and `ffmpeg` for the test):

    client/scripts/build.sh

It builds the static dependencies (OpenSSL, curl, expat, Opus), the program, runs the test
against a fake core in memory, and writes the SD-card zip.

## Credits and licences

- Program: GPL-3.0-or-later (`LICENSE`).
- `client/third_party/moonlight-common-c`, `client/third_party/libgamestream`:
  [moonlight-stream](https://github.com/moonlight-stream), GPL-3.0.
- `client/third_party/h264ring`: the H264 core's HPS library, MIT
  (owenb321/MiSTer_h264_decoder v1.1.0, `hps/`). The session, error and input handling follow
  that project's own Moonlight client (`clients/moonlight/src/mister`).
