First release.

MiSTer Moonlight CRT: a Moonlight client for Sunshine running on the MiSTer's ARM, with a modified H264 core (included) that decodes the stream in the FPGA, aimed at CRTs (15 kHz or 31 kHz).

- On-screen menu: PCs, Sunshine apps, pairing with an on-screen PIN, bitrate, audio, sync, PC resolution.
- Genlock: the core's video PLL follows the PC's frame rate (59.94 Hz too). Sync modes: genlock (default, one picture in reserve), genlock low latency, 60 Hz.
- 640x480 stream shown 1:1, 240p and 480i output.
- Pads, keyboard and mouse forwarded to the PC, rumble.

Install: copy the zip's `Scripts` and `MiSTer_moonlight_crt` folders to the root of the SD card and run Scripts → mister_moonlight_crt. Needs a recent Sunshine; see the README for the settings.
