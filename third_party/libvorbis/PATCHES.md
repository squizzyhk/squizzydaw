# libvorbis patches

Upstream libvorbis 1.3.7 with one local fix.

## lib/block.c: `_vds_shared_init` error path clears the decode codebooks

When a codebook fails `vorbis_book_init_decode()` (hostile/corrupt setup header), the
`abort_books` path destroyed the static books but kept `ci->fullbooks`, with every book after the
failing one still zeroed. vorbisfile retries `vorbis_synthesis_init()` on the next seek; that retry
skipped the codebook setup (`fullbooks` already set) and decoded with `dim == 0` books, which is
an integer division by zero in `res2_inverse()` (process crash). Found by the codec-ogg
decoder-level fuzz test (`codec_ogg`, CRC-fixed corrupted payloads).

The patch frees and clears `ci->fullbooks` in `abort_books`, so a retry fails cleanly again
(`book_param` is already gone) and the stream reports an error instead of crashing.
