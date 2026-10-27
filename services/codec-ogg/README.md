# codec-ogg

Ogg codec service (`ICodecService`, class `sq::codec::kOggCodecClassId`, category `Codec`).

| Format | Read | Write | Implementation |
|---|---|---|---|
| `.ogg` / `.oga` Vorbis | yes, sample-accurate seek | yes | libvorbis 1.3.7 + libvorbisenc + libvorbisfile, libogg 1.3.5 (`third_party/libvorbis`, `third_party/libogg`, BSD-3-Clause, statically linked) |
| `.opus` | seam: needs a user-installed libopusfile (`Services.codec-ogg.OpusfileLibrary`, env `SQ_OPUSFILE_LIBRARY`, default names) | no | libopus/libopusenc are not vendored; `createWriter("opus")` and `supports("opus", .., SQ_CODEC_WRITE)` report unsupported |

## Writer

- Options: `quality` (double 0..1, default 0.5; Vorbis VBR quality -0.1..1.0), `bitrate_kbps`
  (int > 0: managed average bitrate instead). 1..8 channels (Vorbis channel order), 1 kHz..384 kHz
  integer rates; combinations the encoder has no mode for return `SQ_E_UNSUPPORTED`.
- Metadata: `tag:<name>` -> Vorbis comments (normalised names as in `codec.h`, unknown names
  upper-cased), accepted any time before `finalize()`. `chunk:*` and markers: `SQ_E_UNSUPPORTED`.
- Deterministic output (fixed stream serial): the same input and options give the same bytes.
- Encoded pages stay in memory (compressed size) until `finalize()`, so tags set after the audio
  still go into the comment header; the audio pages are renumbered if the header needs more pages.
  Past 32 MB of encoded audio the headers are committed and the rest streams to the output; tags
  set after that point return `SQ_E_UNSUPPORTED`.
- Path-opened writers delete their file when released without `finalize()`.

## Reader

- The stream is read into memory, cut at the first page that is incomplete, fails its CRC or
  belongs to another logical stream (chained files decode their first link only), and the Vorbis
  header packets are sanity-checked before libvorbisfile sees them.
- stb_vorbis was the decoder before the writer landed; it produced wrong output for 5.1 streams in
  this reader, so decoding moved to libvorbisfile (the reference decoder).
- `third_party/libvorbis/PATCHES.md`: one local fix (decoder init retry after a failed codebook
  setup decoded with zeroed books: division by zero), found by this service's fuzz test.

## Tests

- `codec_ogg`: fixture decode (length, comments, sample-accurate seeks); write -> read round trip
  (exact length, SNR > 20 dB at the default quality, no time shift, tags incl. UTF-8, seeks on the
  encoded file); quality/bitrate/sample-rate/5.1 coverage; determinism; late tags with header page
  renumbering; the spill path (byte-identical to the buffered path); writer contract; abort deletes
  the file; truncation/bit-flip fuzz and CRC-fixed decoder-level fuzz; regression fixture
  `tests/data/bad_books.ogg`.
- `codec_ogg_opus`: Opus decode through libopusfile. Without the library it checks the clean
  `SQ_E_UNSUPPORTED` path and is reported **Skipped** (exit 77).
