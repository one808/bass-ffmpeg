# bass-ffmpeg

BASS 2.4 add-on that plays **anything FFmpeg can decode** — all audio formats,
plus the **audio track of any video file** (video is skipped, audio-only
playback). Powered by a fully **static** FFmpeg build: the only dependency is
`bass.dll` (the host) — zero FFmpeg DLLs.

Built for **Windows XP and later** (including old CPUs without SSE2).

Companion to [xmp-ffmpeg](https://github.com/one808/xmp-ffmpeg-ng) (the XMPlay
version) — same FFmpeg engine, same build pipeline.

## Features

- **All formats**: 231 audio decoders (incl. PCM family), 367 demuxers —
  MP3/AAC/M4A/FLAC/WAV/WMA/APE/WavPack/TTA/TAK/MPC/Opus/Vorbis/AC3/E-AC3/DTS/
  TrueHD/MLP/AMR/GSM/ATRAC/…, game music (NSF/SPC/VGM… via libgme), codec2,
  high-quality AAC (libfdk-aac).
- **Video files play their audio**: MKV/MP4/AVI/MOV/WMV/FLV/WebM/TS/M2TS/VOB/
  3GP/RM/… demuxed and the audio stream decoded — the video stream is never
  touched.
- **Network streaming** through BASS's own file routines (buffered downloads,
  custom file systems) — anything BASS can open, this add-on can decode.
- Output as float32/16-bit/8-bit per the stream flags (BASS resamples to the
  device), byte positioning + seeking, up to 8 channels (downmix beyond).
- Static build: no FFmpeg DLLs, no C++ runtime, no libgcc runtime.

## Installation

Copy `bass-ffmpeg.dll` into the `bass` plugins folder of your BASS application
(the folder `BASS_PluginLoad` scans), or load it explicitly with
`BASS_PluginLoad("bass-ffmpeg.dll", 0)`.

Module formats (MOD/XM/S3M/IT…) are deliberately **not** claimed: BASS's own
music engine handles those, and better.

## Building

Everything is built with GitHub Actions (`.github/workflows/build.yml`) —
on every push, or manually via *Run workflow*:

1. **Cross toolchain** (i686 MinGW-w64 9.0 + GCC 10.4, win32 threads +
   winpthreads) built with the recipe from
   [ffmpeg-windows-build-helpers](https://github.com/one808/ffmpeg-windows-build-helpers)
   (pinned checkout, `xp-core2` branch) — the same toolchain that produces
   our known-good "FFmpeg for XP" builds. Cached after the first run.
2. **External codec libs** (same pinned commits as our XP FFmpeg builds):
   libfdk-aac, libcodec2, opencore-amr, game-music-emu + zlib/bzip2/lzma.
3. **FFmpeg** (pinned `n9.0.2`), configured decode-only: all demuxers/parsers/
   BSFs, audio decoders only (generated from the FFmpeg source at build time),
   no video decoding, no encoders/filters/programs, no TLS/schannel. XP-compat
   patches applied (bcrypt/CancelIoEx/wcscpy_s + wchar filename fix).
4. **The plugin**, linked statically against all of the above; a tiny
   `bass.lib` import library is generated from `bass-sdk/bass.def` with dlltool.
5. **XP audit** (`scripts/check-xp-compat.sh`): PE subsystem ≤ 5.1, no known
   Vista+-only imports, and no non-system DLL dependencies beyond `bass.dll`.

The artifact from the workflow is the finished `bass-ffmpeg.dll`, in a
versioned package (`bass-ffmpeg-<version>`).

## Releases & updates

The version lives in the `VERSION` file (single source of truth) and is
embedded into the DLL as a standard Windows **VERSIONINFO resource**. Every
successful `main` build publishes an **add-only** GitHub Release (existing
releases are never modified): tag `v<version>`, or `v<version>.<run>` when
that version already has a release.

Local build (Linux with the same apt packages as the workflow):

```sh
git clone https://github.com/one808/ffmpeg-windows-build-helpers build-helpers
git -C build-helpers checkout xp-core2
bash scripts/build-ffmpeg.sh    # ~40min first run (toolchain), cached after
bash scripts/build-plugin.sh
bash scripts/check-xp-compat.sh build/bass-ffmpeg.dll
```

## Notes

- Decoded format = source rate/channels (float32 internally; BASS converts to
  the requested stream format and resamples to the device).
- Multichannel audio is passed through up to 8 channels; more is downmixed
  to stereo.
- Video streams are never decoded — only the container is demuxed.
- Tags (ID3/APE/FLAC/MP4 atoms…) are read by BASS itself from the file.

## Credits

- [FFmpeg](https://ffmpeg.org) — decoding (LGPL/GPL, built GPL v3 here)
- [BASS](https://www.un4seen.com/) & the BASS add-on SDK by un4seen.com
- [ffmpeg-windows-build-helpers](https://github.com/one808/ffmpeg-windows-build-helpers)
  — toolchain recipe, XP patches and audit tooling

## License

GPL v3 or later (the statically linked FFmpeg is built with
`--enable-gpl --enable-version3`). See `LICENSE`.

Note: the build also links **libfdk-aac** statically, which is under the
FDK AAC license (not GPL-compatible, hence FFmpeg's `--enable-nonfree`).
Remove `--enable-libfdk-aac` from `scripts/build-ffmpeg.sh` for a strictly
GPL-pure build.
