# Codec services (codec-wav, codec-flac, codec-mp3, codec-ogg, codec-aac)

Each service implements `ICodecService` from `sdk/include/sq/interfaces/codec.h` in its own DLL, under the category "Codec". The header-only helpers in `codec-wav/shared/sqcodec/` are shared as source. No service links another.

| Service | Read | Write | Backend |
|---|---|---|---|
| codec-wav | WAV, RF64/BW64, WAVE_FORMAT_EXTENSIBLE, AIFF, AIFF-C (NONE/twos/sowt/in24/in32/fl32/fl64/raw). 8/16/24/32-bit int, 32/64-bit float | the same formats; RF64 is `auto` above 4 GB | own code |
| codec-flac | FLAC | FLAC, 8–24 bit, up to 8 channels | dr_flac (decode); own encoder (FIXED+LPC, Rice, stereo decorrelation, MD5, SEEKTABLE) |
| codec-mp3 | MP3, gapless (LAME/Xing tag) | MP3, only when a LAME library is installed | dr_mp3; LAME is loaded at runtime |
| codec-ogg | Ogg Vorbis; Ogg Opus only when libopusfile is installed | none | stb_vorbis; opusfile is loaded at runtime |
| codec-aac | m4a/mp4 (audio track), ADTS .aac | m4a, ADTS. 44.1/48 kHz, 1/2/6 ch, 96–192 kbps | Windows Media Foundation (delay-loaded) |

## Metadata keys

- **`chunk:<id>`** is a raw chunk payload. The `<id>` is 4 characters; trailing spaces are kept.
  - If an id repeats, the later chunks are exposed as `chunk:<id>#2`, `#3`, and so on. Writers accept the same keys.
  - WAV and AIFF:
    - All non-structural chunks are exposed and written back unchanged.
    - Writers refuse the structural ids: `fmt `, `data`, `ds64`, `fact`, `JUNK`, `COMM`, `SSND` and `FVER`.
    - Chunks set before the first `write()` go before the audio. Chunks set later go after it.
    - If a pre-data chunk changes later, it is patched in place when the size is unchanged. Otherwise it becomes JUNK and is re-emitted after the audio.
  - FLAC: each chunk is one `APPLICATION` block with id `riff`, holding one RIFF chunk (id, little-endian size, payload).
  - MP3: the raw ID3v2 tag is `chunk:ID3 `. It is written verbatim only when no `tag:*` key and no markers are set. Otherwise a new ID3v2.4 tag is built from the tags and markers, and the frames of a raw v2.3/v2.4 tag that those do not replace (pictures, private frames, lyrics, ...) are carried over. Chapter frames are carried over only when no markers are set.
- **`tag:<name>`** holds normalized text tags:
  - WAV writes them as LIST/INFO and merges them into an existing INFO list. `originator` and `origination_date` go into `bext`.
  - AIFF writes NAME, AUTH, `(c) ` and ANNO.
  - FLAC and Vorbis use Vorbis comments. Unknown fields round-trip as `tag:<lowercase field>`.
  - MP3 writes ID3v2.4 and uses TXXX for unknown names.
- **CHAN**: if `chunk:CHAN` holds a CoreAudio layout (big-endian) with the use-bitmap tag, the WAV writer uses its bitmap as the WAVE_FORMAT_EXTENSIBLE channel mask, provided the bitmap names exactly as many channels as are written (otherwise the default mask for the channel count is used).
- **Valid bits in AIFF**: AIFF has no container width separate from the bit depth, so integer samples are packed into the smallest whole byte count (e.g. `SQ_SAMPLE_I24_IN32` and `SQ_SAMPLE_I32` with 24 valid bits are stored as 3-byte samples). WAV keeps the requested container and records the valid bits in WAVE_FORMAT_EXTENSIBLE.
- **FLAC limits**: all Vorbis comments together must fit one metadata block (16 MB); `setMetadata` refuses a tag that would overflow it.

## Markers

| Format | Stored as | Notes |
|---|---|---|
| WAV | `cue ` + LIST/`adtl` (`labl` for names, `ltxt` for region lengths) + `smpl` loops | Loops are also cue points, so their names round-trip. |
| FLAC | the same RIFF chunks, inside `riff` APPLICATION blocks | |
| AIFF | `MARK` + `INST` (sustain and release loop) | Plain regions lose their length. Loop end marks are hidden on read. |
| MP3 | ID3 `CHAP` + `CTOC` | Millisecond precision. |
| Ogg, AAC | not supported | |

## Runtime libraries (optional; never vendored or linked)

| Library | Used for | Lookup order |
|---|---|---|
| LAME | MP3 encode | setting `Services.codec-mp3.LameLibrary`, then env `SQ_LAME_LIBRARY`, then `libmp3lame.dll` / `libmp3lame-0.dll` / `lame_enc.dll` (`.so.0` / `.dylib` on other OSes) |
| libopusfile | Opus decode | setting `Services.codec-ogg.OpusfileLibrary`, then env `SQ_OPUSFILE_LIBRARY`, then `libopusfile-0.dll` / `opusfile.dll` |

## Test helpers

- `SQ_CODEC_DUMP_DIR=<dir>`: the tests write their encoded files to this directory so you can check them with external tools.
- `SQ_CODEC_FUZZ_SCALE=<n>`: multiplies the number of fuzz iterations.
- `SQ_CODEC_FUZZ_VERBOSE=1`: prints each fuzz iteration and keeps the last input.
