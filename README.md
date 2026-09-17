# mkvgopsplit

A small, dependency-free C tool that splits a video-only Matroska (`.mkv`)
file into one file per GOP (Group of Pictures), and joins such files back
into a single playable file.

**This tool targets video-only, single-track MKV files.** It doesn't
carry audio, subtitles, or multiple video tracks through a split/join
round trip — see [Limitations](#limitations) for exactly what it does
when a file has more than one track.

Built for storage/deduplication workflows — split an episode into
per-GOP chunks, hash/dedup them against a shared dictionary, then
reassemble a full episode later by joining the chunks back in order.

## Features

- **No libavformat / libmatroska dependency** — parses just enough raw
  EBML/Matroska structure to do the job (Tracks header + Cluster/Block
  keyframe flags), the same way the rest of this pipeline is built.
- **Open GOP is fine.** This is for storage/dedup, not playback
  correctness across a single split file's boundaries, so no extra
  padding/re-encoding is done to force closed GOPs.
- **Dedup-friendly by construction.** Every split file's internal
  clock is normalized to start at 0, so two GOPs with identical frame
  content produce byte-identical `.mkv` files regardless of where in
  the source they occurred — verified by splitting a source with the
  same encoded GOP repeated at two different timeline positions and
  confirming the two output files matched byte-for-byte.
- **Lossless.** Verified pixel-for-pixel and timestamp-for-timestamp
  identical to the source after a split → join round trip, including
  the edge case where a source Cluster spans multiple GOPs (keyframe
  falls mid-cluster).
- **Flat memory use regardless of file size.** Frame payloads are
  streamed through a fixed-size buffer; nothing scales with input
  file size or frame size. Peak RSS was under 2 MB splitting/joining a
  35 MB / 120-GOP test file.
- **Progress bar** on stderr for both `split` and `join`.
- **Reports average GOP size** (frames per GOP, not byte size) after a
  split, whether or not GOPs are uniform length.

## Build

```sh
gcc -O2 -march=native -o mkvgopsplit mkvgopsplit.c
```

(`-march=native` since the convention in this pipeline is build machine
== run machine; a plain `-O2` build works fine too.)

## Usage

```sh
mkvgopsplit split <input.mkv> <output_dir> [base_name]
mkvgopsplit join  <input_dir> <base_name> <output_dir>
```

- `split` writes `<output_dir>/<base_name>-000001.mkv`,
  `-000002.mkv`, ... (one file per GOP, 6-digit zero-padded index,
  widens automatically past 999999 rather than truncating), plus a
  single `<output_dir>/<base_name>.tcmap` sidecar (plain text,
  `<index> <original_tick>` per line) recording each GOP's true
  original position in the source. `base_name` defaults to
  `<input.mkv>`'s filename without its extension if omitted.
- `join` scans `<input_dir>` for files matching
  `<base_name>-NNNN.mkv` (any digit width), sorts them numerically,
  reads the matching `<base_name>.tcmap` to recover correct absolute
  timing, and writes `<output_dir>/<base_name>.mkv`. This means you
  can point it at a directory containing thousands of GOP files from
  many different sources and it'll only pick up the ones for the
  `base_name` you asked for — no manual globbing needed.

**Keep each source's `.tcmap` file alongside its GOP files** — `join`
needs it to restore correct playback timing and will refuse (with a
clear error) if it's missing. If your dedup process discards
duplicate GOP files, keep the `.tcmap` for every source video even
though some of the `.mkv` files it references may have been
deduplicated away elsewhere.

Both `output_dir` arguments are created automatically if they don't
already exist.

### Example

```sh
$ mkvgopsplit split show_s01e01.mkv ./gops
split [========================================] 100.0%
mkvgopsplit: wrote 214 GOP file(s), 34512 frame(s), track #1, avg 161.3 frames/GOP

$ mkvgopsplit join ./gops show_s01e01 ./rebuilt
join  [========================================] 100.0%
mkvgopsplit: joined 214 file(s) into ./rebuilt/show_s01e01.mkv
```

## Example use cases

Two illustrative scenarios beyond the storage/dedup pipeline this was
built for — neither requires anything from this tool beyond `split`
itself.

- **Splitting before compression to expose exact-duplicate GOPs across
  files.** `split` gives you one file per GOP; pass those (plain, or
  through a similarity/dedup pass like nilsimsa hashing first) to
  `zstd`/`xz`/`7z` instead of compressing the whole source in one
  piece. Most content won't have literal duplicate GOPs to find, so
  this won't do much in the general case — but it's a real win when it
  does apply: e.g. a set of VOB episode files that each spliced in the
  same shared MPEG-2 sequence. Buried inside otherwise-different
  full-length files, that shared sequence is easy for a compressor to
  miss entirely; split into individual GOPs first, it's an exact,
  easy-to-find duplicate. Results depend entirely on whether the source
  actually has duplicate content to find - shared opening/closing
  sequences across episodes being the common real-world case. A test
  against two 480p animated (cartoon) episodes with computer-assisted,
  relatively clean animation and shared open/close sequences found
  ~2% duplicate GOPs on both an FFV1 and an MPEG-2 encode of the same
  episodes; the exact figure shifted slightly between the two since
  MPEG-2's GOP boundaries don't land on quite the same frames as
  FFV1's, but it landed in the same ballpark either way. Small, but
  enough to measurably shrink the final `7z`/`xz` output. Worth trying,
  not worth counting on.

- **Pulling single frames out of an all-intra lossless source.** Point
  `split` at a lossless, all-intra file (FFV1 is the obvious case) that
  isn't already flagged per-frame-keyframe by its muxer, and each
  output file is one independently valid, decodable frame — no need to
  decode the whole stream just to look at one frame. The resulting
  per-frame files are individually bigger than the source (each one
  duplicates the EBML/Tracks header the whole stream would otherwise
  only pay for once), but still far smaller than dumping to raw YUV,
  since the frame data itself is still FFV1-compressed. Handy for
  eyeballing a specific frame's exact decoded output, or converting it
  straight to PNG/JPEG with one line:
  `ffmpeg -i show_s01e01-000042.mkv frame.png`.

- **Isolating a troublesome sample for a codec bug report.** If an
  encoder or decoder misbehaves at one specific point in a much larger
  file, `split` lets you hand a developer just the GOP(s) where it
  happens instead of the whole source. Each output file is already an
  independently valid, playable `.mkv` on its own (see
  [How it works](#how-it-works)), so there's no re-muxing or trimming
  step before sending it — just find the offending GOP's index and
  attach that one file.

## How it works

Every split file is itself a small, independently valid Matroska file:

```
[EBML][Segment (unknown size) { [Tracks] [Cluster (Timecode=0, ...)] }]
```

The `EBML` header and `Tracks` element are copied verbatim into every
split file, so that header is byte-identical across all of them.
Each file's single `Cluster` has its `Timecode` normalized to `0` —
the true original offset is written instead to the `.tcmap` sidecar —
so that two GOPs with identical encoded frame content produce
byte-identical files no matter where in the source they occurred.
That's what makes whole-file hashing a valid dedup strategy here.

Splits happen at every keyframe, detected directly from the container
(the `SimpleBlock` keyframe flag, or absence of a `ReferenceBlock` for
`BlockGroup`) — no decoding required. If a keyframe happens to fall in
the middle of a source `Cluster`, the affected blocks' relative
timecodes are rewritten so they stay correct relative to the new
file's `0`-based `Cluster`. This path is exercised and tested (see
Testing notes below), though in practice most muxers already start a
new cluster at every keyframe, so it's rarely hit.

`join` reads the `.tcmap`, patches each file's `Cluster` `Timecode`
back to its true original value, and streams every other byte
(all the blocks) through completely unchanged — their relative
timecodes are already correct since they're relative to their own
`Cluster`'s timecode, not to the file as a whole.

## Limitations

- Assumes the input's `Segment` and `Cluster` elements have **known**
  (finite) sizes, which is true for any file finalized by a normal
  muxer (ffmpeg, mkvmerge, etc.). A live/streamed capture that was
  never finalized may use Matroska's "unknown size" convention; the
  tool detects this and reports a clear error rather than guessing.
- Assumes a single video track (video-only MKV, as produced elsewhere
  in this pipeline). If multiple tracks are present, it picks the
  first video track (`TrackType == 1`) and ignores the rest.

## Testing notes

Round-tripped (split → join) against synthetic H.264/MKV files built
with ffmpeg, including:
- uniform-length GOPs and highly irregular/scene-cut-driven GOP
  lengths (2 to 80 frames in the same file),
- B-frames (non-monotonic decode/presentation order),
- clusters intentionally *not* aligned to keyframes (forces the
  mid-cluster timecode-rewrite path),
- an all-intra encode (every frame its own GOP) and a single-GOP clip,
- a 120-GOP / 35 MB file, to check memory stayed flat,
- a source with the same encoded GOP occurring at two different
  timeline positions, to confirm the two resulting split files are
  byte-identical (the dedup case) while `join` still reconstructs
  correct absolute timing for both occurrences.

In every case, the rejoined file's decoded YUV output and per-frame
presentation timestamps were verified byte-identical to the original
source.

## License

MIT — see [LICENSE](LICENSE). Use it, share it, modify it, no strings attached.
