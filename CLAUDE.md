# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and Install

```sh
make          # build send2tv binary
make test     # build and run unit tests
make install  # install to $HOME/.bin/send2tv
make clean    # remove binary and object files
```

Dependencies are linked via `pkg-config`: libavformat, libavcodec, libavutil, libavdevice, libavfilter, libswscale, libswresample (all from FFmpeg), plus libpthread.

The `ffmpeg-8.0.1/` directory contains FFmpeg headers used only by the test build (`tests.c`), which compiles without linking most FFmpeg libs (`-Wl,--unresolved-symbols=ignore-all`).

## Configuration

`~/.send2tv.conf` is a simple `key=value` file. Keys: `host`, `audiodev`, `bitrate`, `port`, `transcode` (yes/no), `verbose` (yes/no), `codec` (h264/hevc/auto), `mac`. The config is loaded at startup; `-h host` on the command line overrides it. The MAC address for Wake-on-LAN is auto-saved to the config on first discovery.

## Architecture

The program is structured as four cooperating layers:

**`send2tv.c` — main / orchestration.** Parses config and args, discovers the TV (SSDP via `upnp.c`), resolves web URLs via yt-dlp (`ytdlp_resolve()`), opens media or starts screen capture (`media.c`), starts the HTTP server (`httpd.c`), sends UPnP AVTransport commands, then runs the interactive playback loop (arrow-key seeking, `q`/`Q`).

**`media.c` — FFmpeg pipeline.** Two modes:
- *File mode* (`MODE_FILE`): `media_probe()` inspects the file and decides whether to pass through directly or transcode. `media_open_transcode()` sets up decode → (optional VAAPI filter graph) → encode → AVIO write into a pipe. The transcode thread (`media_transcode_thread`) runs in a background pthread.
- *Screen mode* (`MODE_SCREEN`): `media_open_screen()` captures from X11 (`x11grab`) and sndio audio concurrently, encoding to H.264 MPEG-TS into a pipe (`media_capture_thread`).

In both modes, the read end of the pipe is what `httpd.c` streams to the TV. An FFmpeg interrupt callback (`ffmpeg_interrupt_cb`) checks `ctx->running && running` so Ctrl+C aborts blocking I/O immediately.

**`httpd.c` — HTTP server.** A minimal single-client HTTP/1.1 server (one accept loop, one pthread). Handles `GET` with `Range:` support for direct file serving, and streams from the pipe for transcoded/captured content. Sends DLNA headers (`transferMode.dlna.org`, `contentFeatures.dlna.org`) built by `dlna.c`.

**`upnp.c` — UPnP/SSDP/SOAP.** SSDP M-SEARCH discovery, device description XML parsing, SOAP calls for AVTransport (`SetAVTransportURI`, `Play`, `Stop`, `Seek`, `GetPositionInfo`), Samsung-specific ARP MAC lookup, Wake-on-LAN, app listing/launching, and capability querying.

**`dlna.c`** — single function `build_dlna_features()` that constructs the `DLNA.ORG_PN/OP/CI/FLAGS` string.

## Key Design Points

- **Signal safety**: `SIGINT`/`SIGTERM` set the global `running = 0` and kill the yt-dlp child PID. `SA_RESTART` is intentionally cleared so that blocking syscalls return `EINTR` and the main loop exits cleanly.
- **yt-dlp support**: If a file argument looks like an HTTP/HTTPS URL, `ytdlp_resolve()` forks `yt-dlp --print '%(title)s\n%(url)s'` to get a direct stream URL, then `media_probe()` opens that URL via FFmpeg's network I/O.
- **VAAPI**: Hardware-accelerated video encoding is attempted when the codec is H.264 or HEVC; the filter graph uses `hwupload,scale_vaapi` to move frames to the GPU. Falls back to software if VAAPI init fails.
- **5.1 channel remapping**: Named presets in `channelmap_presets[]` correct channel ordering differences between codecs (AC3, DTS, AAC variants). Applying a preset forces transcoding.
- **Transcode decision** (`media_probe`): passthrough is used when the container/codec is natively supported by Samsung TVs (table in `video_container_ok()`/`audio_codec_ok()`). Transcoding produces MPEG-TS with AAC audio and H.264 or HEVC video.
