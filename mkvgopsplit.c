/*
 * mkvgopsplit - split a video-only Matroska (MKV) file into one file per
 * GOP (split at every keyframe), and join such files back into one.
 *
 * Copyright (c) 2026 Daniel Lee Witzel
 * MIT License - see LICENSE.
 *
 * Design notes (see conversation for rationale):
 *  - Only the Tracks header is duplicated into every split file. Every
 *    split file is itself a small valid Matroska file: [EBML][Segment
 *    (unknown size){[Tracks][Cluster...]}]. Because that header is
 *    byte-identical across every split file (same EBML bytes, same fixed
 *    Segment id/unknown-size marker, same Tracks bytes), joining is just:
 *    take the header from the first file once, then concatenate the
 *    "everything after Tracks" (the Cluster region) of every file in
 *    order. No re-encoding, no re-muxing library needed.
 *  - GOPs are cut on keyframes only (open GOP is fine - this is for
 *    storage/dedup, not playback). If a keyframe falls in the middle of
 *    an original Cluster, we start a fresh (unknown-size) Cluster in the
 *    new output file and rewrite just the 2-byte relative timecode field
 *    of the blocks that move into it, so absolute timing is preserved.
 *  - Assumes clusters/segment have *known* sizes in the input (true for
 *    any file finalized by a normal muxer, e.g. ffmpeg/mkvmerge output
 *    that isn't a live/streamed capture). If an unknown-size top-level
 *    element is hit, the tool reports it and stops rather than guessing.
 *  - Streams payload bytes in a fixed-size buffer; never loads whole
 *    clusters/frames into RAM regardless of file size.
 */

#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>

/* ---- EBML element IDs we care about (value includes the length-marker
 * bits, i.e. these are the raw bytes as they appear on disk) ---- */
#define ID_EBML          0x1A45DFA3u
#define ID_SEGMENT       0x18538067u
#define ID_SEEKHEAD      0x114D9B74u
#define ID_INFO          0x1549A966u
#define ID_TRACKS        0x1654AE6Bu
#define ID_TRACKENTRY    0xAEu
#define ID_TRACKNUMBER   0xD7u
#define ID_TRACKTYPE     0x83u
#define ID_CLUSTER       0x1F43B675u
#define ID_TIMECODE      0xE7u
#define ID_SIMPLEBLOCK   0xA3u
#define ID_BLOCKGROUP    0xA0u
#define ID_BLOCK         0xA1u
#define ID_REFERENCEBLOCK 0xFBu
#define ID_CUES          0x1C53BB6Bu
#define ID_VOID          0xECu
#define ID_CRC32         0xBFu

#define UNKNOWN_SIZE_MARKER 8 /* we always emit unknown-size as 8 bytes */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "mkvgopsplit: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/* ---------------- low level EBML reading ---------------- */

/* Read a vint. If keep_marker, returns the raw bytes as a big-endian
 * integer including the leading length bits (used for element IDs).
 * Otherwise strips the marker bits (used for sizes/values). *is_unknown
 * is set (if non-NULL) when this was a size field and all data bits were
 * 1 (the Matroska "unknown size" convention). Returns number of bytes
 * consumed, or -1 on EOF/error. */
static int ebml_read_vint(FILE *f, int keep_marker, uint64_t *out, int *is_unknown) {
    int b0 = fgetc(f);
    if (b0 == EOF) return -1;
    int len = 0;
    for (int i = 7; i >= 0; i--) {
        if (b0 & (1 << i)) { len = 8 - i; break; }
    }
    if (len == 0) die("invalid EBML vint (leading byte 0x00)");
    uint64_t value;
    if (keep_marker) {
        value = (uint64_t)(unsigned char)b0;
    } else {
        value = (uint64_t)(unsigned char)b0 & (0xFFu >> len);
    }
    uint64_t data_mask_all_ones = (len == 8) ? 0 : ((1ULL << (7 * len)) - 1);
    int all_ones = (!keep_marker) && ((uint64_t)((unsigned char)b0 & (0xFFu >> len)) == ((0xFFu >> len)));
    for (int i = 1; i < len; i++) {
        int b = fgetc(f);
        if (b == EOF) return -1;
        value = (value << 8) | (unsigned char)b;
        if (all_ones && (unsigned char)b != 0xFF) all_ones = 0;
    }
    if (is_unknown) *is_unknown = (!keep_marker) && all_ones && len > 0;
    (void)data_mask_all_ones;
    *out = value;
    return len;
}

/* Write a "size" vint using the minimal number of bytes that can hold
 * value (leaving the top bit of the first byte as the length marker). */
static int ebml_size_len_needed(uint64_t value) {
    for (int len = 1; len <= 8; len++) {
        uint64_t max = (len == 8) ? UINT64_MAX >> 1 : (1ULL << (7 * len)) - 2;
        if (value <= max) return len;
    }
    return 8;
}

static void ebml_write_size(FILE *f, uint64_t value, int len) {
    unsigned char buf[8];
    for (int i = len - 1; i >= 0; i--) {
        buf[i] = (unsigned char)(value & 0xFF);
        value >>= 8;
    }
    buf[0] |= (unsigned char)(1u << (8 - len));
    fwrite(buf, 1, len, f);
}

static void ebml_write_unknown_size(FILE *f) {
    unsigned char buf[8] = {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    fwrite(buf, 1, 8, f);
}

/* Like ebml_write_uint_elem, but always encodes exactly `width` content
 * bytes (zero-padded), so the element's total on-disk length never
 * depends on the value. Used for the per-file Timecode element so that
 * `join` can patch its value in place later without touching anything
 * else in the file. */
static void ebml_write_uint_fixed(FILE *f, uint32_t id, uint64_t value, int width) {
    unsigned char idbuf[4];
    int idlen = (id > 0xFFFFFF) ? 4 : (id > 0xFFFF) ? 3 : (id > 0xFF) ? 2 : 1;
    for (int i = 0; i < idlen; i++) idbuf[i] = (unsigned char)(id >> (8 * (idlen - 1 - i)));
    fwrite(idbuf, 1, idlen, f);
    ebml_write_size(f, (uint64_t)width, ebml_size_len_needed((uint64_t)width));
    unsigned char vbuf[8];
    uint64_t vv = value;
    for (int i = width - 1; i >= 0; i--) { vbuf[i] = (unsigned char)(vv & 0xFF); vv >>= 8; }
    fwrite(vbuf, 1, width, f);
}
#define GOP_TIMECODE_WIDTH 8

static uint64_t read_uint_be(FILE *f, uint64_t size) {
    uint64_t v = 0;
    for (uint64_t i = 0; i < size; i++) {
        int b = fgetc(f);
        if (b == EOF) die("unexpected EOF reading uint field");
        v = (v << 8) | (unsigned char)b;
    }
    return v;
}

/* Read an EBML vint (with marker) fully into buf, returning its length.
 * Used to pass the Track Number field of a Block/SimpleBlock through
 * unchanged without needing to interpret its value. */
static int raw_vint_bytes(FILE *f, unsigned char *buf, int bufcap) {
    int b0 = fgetc(f);
    if (b0 == EOF) die("unexpected EOF reading vint");
    int len = 0;
    for (int i = 7; i >= 0; i--) if (b0 & (1 << i)) { len = 8 - i; break; }
    if (len == 0 || len > bufcap) die("invalid/oversized vint in block header");
    buf[0] = (unsigned char)b0;
    for (int i = 1; i < len; i++) {
        int b = fgetc(f);
        if (b == EOF) die("unexpected EOF reading vint");
        buf[i] = (unsigned char)b;
    }
    return len;
}

static void copy_bytes(FILE *in, FILE *out, uint64_t n) {
    unsigned char buf[1 << 16];
    while (n > 0) {
        size_t chunk = n > sizeof(buf) ? sizeof(buf) : (size_t)n;
        size_t got = fread(buf, 1, chunk, in);
        if (got != chunk) die("unexpected EOF while copying %llu bytes", (unsigned long long)n);
        fwrite(buf, 1, chunk, out);
        n -= chunk;
    }
}

static void skip_bytes(FILE *f, uint64_t n) {
    if (fseeko(f, (off_t)n, SEEK_CUR) != 0) die("seek failed while skipping %llu bytes", (unsigned long long)n);
}

/* ---------------- progress bar ---------------- */
static struct timespec g_pb_last;
static void progress_bar(uint64_t done, uint64_t total, const char *label) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = (now.tv_sec - g_pb_last.tv_sec) + (now.tv_nsec - g_pb_last.tv_nsec) / 1e9;
    int finished = total > 0 && done >= total;
    if (dt < 0.1 && !finished) return;
    g_pb_last = now;
    double frac = total ? (double)done / (double)total : 1.0;
    if (frac > 1.0) frac = 1.0;
    int width = 40;
    int filled = (int)(frac * width);
    fprintf(stderr, "\r%s [", label);
    for (int i = 0; i < width; i++) fputc(i < filled ? '=' : ' ', stderr);
    fprintf(stderr, "] %5.1f%%", frac * 100.0);
    if (finished) fputc('\n', stderr);
    fflush(stderr);
}

/* Read one element id (raw, with markers) at current position; returns
 * byte length of id, or -1 at EOF. */
static int read_id(FILE *f, uint32_t *id) {
    uint64_t v;
    int len = ebml_read_vint(f, 1, &v, NULL);
    if (len < 0) return -1;
    *id = (uint32_t)v;
    return len;
}

static int read_size(FILE *f, uint64_t *size, int *unknown) {
    return ebml_read_vint(f, 0, size, unknown);
}

/* ---------------- block (SimpleBlock / BlockGroup) copy ---------------- */

/* Copies one SimpleBlock or BlockGroup element (id+size already consumed
 * by caller; file position is at the start of its content, length
 * content_size) from `in` to `out`, adding tc_offset (in TimecodeScale
 * ticks) to the block's relative timecode. tc_offset==0 takes a fast
 * verbatim-copy path. `id`/`content_size` are re-written as the element
 * header (unchanged, since content length never changes). */
static void copy_block_like(FILE *in, FILE *out, uint32_t id, uint64_t content_size, int64_t tc_offset) {
    long content_start = ftello(in);

    if (tc_offset == 0) {
        /* fast path: whole element unchanged */
        unsigned char idbuf[4];
        int idlen = (id > 0xFF) ? 4 : 1; /* both SimpleBlock/BlockGroup/Block are 1-byte ids */
        idlen = 1;
        idbuf[0] = (unsigned char)id;
        fwrite(idbuf, 1, idlen, out);
        int slen = ebml_size_len_needed(content_size);
        ebml_write_size(out, content_size, slen);
        copy_bytes(in, out, content_size);
        return;
    }

    if (id == ID_SIMPLEBLOCK) {
        unsigned char vint[8];
        int vlen = raw_vint_bytes(in, vint, sizeof(vint));
        int hi = fgetc(in), lo = fgetc(in), flags = fgetc(in);
        if (hi == EOF || lo == EOF || flags == EOF) die("unexpected EOF in SimpleBlock header");
        int16_t rel_tc = (int16_t)(((unsigned)hi << 8) | (unsigned)lo);
        int64_t new_tc = (int64_t)rel_tc + tc_offset;
        if (new_tc < -32768 || new_tc > 32767) {
            fprintf(stderr, "\nmkvgopsplit: warning: relative timecode overflow at split boundary, clamping\n");
            if (new_tc < -32768) new_tc = -32768;
            if (new_tc > 32767) new_tc = 32767;
        }
        uint64_t consumed_header = vlen + 3;
        uint64_t payload_len = content_size - consumed_header;

        fputc((unsigned char)ID_SIMPLEBLOCK, out);
        ebml_write_size(out, content_size, ebml_size_len_needed(content_size));
        fwrite(vint, 1, vlen, out);
        unsigned char tcbuf[2] = { (unsigned char)((new_tc >> 8) & 0xFF), (unsigned char)(new_tc & 0xFF) };
        fwrite(tcbuf, 1, 2, out);
        fputc(flags, out);
        copy_bytes(in, out, payload_len);
        return;
    }

    if (id == ID_BLOCKGROUP) {
        /* Copy header, then walk children, rewriting the nested Block's
         * timecode and passing everything else through verbatim. */
        fputc((unsigned char)ID_BLOCKGROUP, out);
        ebml_write_size(out, content_size, ebml_size_len_needed(content_size));
        uint64_t remaining = content_size;
        while (remaining > 0) {
            uint32_t cid; uint64_t csize; int cunk;
            long before = ftello(in);
            int idlen = read_id(in, &cid);
            if (idlen < 0) die("unexpected EOF inside BlockGroup");
            int slen = read_size(in, &csize, &cunk);
            if (slen < 0 || cunk) die("bad/unknown size inside BlockGroup");
            long header_len = ftello(in) - before;
            if (cid == ID_BLOCK) {
                unsigned char vint[8];
                int vlen = raw_vint_bytes(in, vint, sizeof(vint));
                int hi = fgetc(in), lo = fgetc(in), flags = fgetc(in);
                if (hi == EOF || lo == EOF || flags == EOF) die("unexpected EOF in Block header");
                int16_t rel_tc = (int16_t)(((unsigned)hi << 8) | (unsigned)lo);
                int64_t new_tc = (int64_t)rel_tc + tc_offset;
                if (new_tc < -32768 || new_tc > 32767) {
                    fprintf(stderr, "\nmkvgopsplit: warning: relative timecode overflow at split boundary, clamping\n");
                    if (new_tc < -32768) new_tc = -32768;
                    if (new_tc > 32767) new_tc = 32767;
                }
                uint64_t consumed = vlen + 3;
                uint64_t payload_len = csize - consumed;
                fputc((unsigned char)ID_BLOCK, out);
                ebml_write_size(out, csize, ebml_size_len_needed(csize));
                fwrite(vint, 1, vlen, out);
                unsigned char tcbuf[2] = { (unsigned char)((new_tc >> 8) & 0xFF), (unsigned char)(new_tc & 0xFF) };
                fwrite(tcbuf, 1, 2, out);
                fputc(flags, out);
                copy_bytes(in, out, payload_len);
            } else {
                /* pass through unchanged: id + size + content */
                fseeko(in, before, SEEK_SET);
                unsigned char hdrbuf[16];
                long hcount = header_len;
                if (fread(hdrbuf, 1, hcount, in) != (size_t)hcount) die("re-read failed inside BlockGroup");
                fwrite(hdrbuf, 1, hcount, out);
                copy_bytes(in, out, csize);
            }
            remaining -= (header_len + csize);
        }
        (void)content_start;
        return;
    }

    die("copy_block_like called with unexpected id 0x%X", id);
}

/* Scan a BlockGroup's children (without disturbing later re-reads: caller
 * must fseeko back afterward) to see whether it has a ReferenceBlock
 * (=> not a keyframe). */
static int blockgroup_has_reference(FILE *f, uint64_t content_size) {
    uint64_t remaining = content_size;
    int has_ref = 0;
    while (remaining > 0) {
        uint32_t cid; uint64_t csize; int cunk;
        long before = ftello(f);
        int idlen = read_id(f, &cid);
        if (idlen < 0) die("unexpected EOF inside BlockGroup (scan)");
        int slen = read_size(f, &csize, &cunk);
        if (slen < 0 || cunk) die("bad/unknown size inside BlockGroup (scan)");
        long header_len = ftello(f) - before;
        if (cid == ID_REFERENCEBLOCK) has_ref = 1;
        skip_bytes(f, csize);
        remaining -= (header_len + csize);
    }
    return has_ref;
}

/* ---------------- in-memory EBML walk (used only on the small,
 * already-buffered Tracks element) ---------------- */

static int mem_read_vint(const unsigned char *buf, size_t len, size_t *pos, int keep_marker, uint64_t *out) {
    if (*pos >= len) return -1;
    int b0 = buf[*pos];
    int l = 0;
    for (int i = 7; i >= 0; i--) if (b0 & (1 << i)) { l = 8 - i; break; }
    if (l == 0 || *pos + l > len) return -1;
    uint64_t value = keep_marker ? (uint64_t)(unsigned char)b0 : ((uint64_t)(unsigned char)b0 & (0xFFu >> l));
    for (int i = 1; i < l; i++) value = (value << 8) | buf[*pos + i];
    *pos += l;
    *out = value;
    return l;
}

static uint64_t find_video_track_number(const unsigned char *buf, size_t len) {
    size_t pos = 0;
    uint64_t id, size;
    if (mem_read_vint(buf, len, &pos, 1, &id) < 0) die("malformed Tracks element");
    if (mem_read_vint(buf, len, &pos, 0, &size) < 0) die("malformed Tracks element size");
    size_t content_end = pos + size; if (content_end > len) content_end = len;
    uint64_t first_track_num = 0; int any = 0;
    uint64_t video_num = 0; int found_video = 0;
    while (pos < content_end) {
        uint64_t eid; if (mem_read_vint(buf, len, &pos, 1, &eid) < 0) break;
        uint64_t esize; if (mem_read_vint(buf, len, &pos, 0, &esize) < 0) break;
        size_t econtent_start = pos;
        if (eid == ID_TRACKENTRY) {
            size_t p2 = econtent_start;
            size_t tend = econtent_start + esize; if (tend > len) tend = len;
            uint64_t tnum = 0, ttype = 0; int have_tnum = 0, have_ttype = 0;
            while (p2 < tend) {
                uint64_t cid; if (mem_read_vint(buf, len, &p2, 1, &cid) < 0) break;
                uint64_t csize; if (mem_read_vint(buf, len, &p2, 0, &csize) < 0) break;
                size_t cstart = p2;
                if (cid == ID_TRACKNUMBER) { uint64_t v = 0; for (uint64_t k = 0; k < csize; k++) v = (v << 8) | buf[cstart + k]; tnum = v; have_tnum = 1; }
                else if (cid == ID_TRACKTYPE) { uint64_t v = 0; for (uint64_t k = 0; k < csize; k++) v = (v << 8) | buf[cstart + k]; ttype = v; have_ttype = 1; }
                p2 = cstart + csize;
            }
            if (have_tnum) {
                any = 1;
                if (first_track_num == 0) first_track_num = tnum;
                if (have_ttype && ttype == 1 && !found_video) { found_video = 1; video_num = tnum; }
            }
        }
        pos = econtent_start + esize;
    }
    if (found_video) return video_num;
    if (any) return first_track_num;
    die("no track entries found in Tracks element");
    return 0;
}

/* ---------------- header parsing ---------------- */

typedef struct { unsigned char *ebml_bytes; size_t ebml_len; unsigned char *info_bytes; size_t info_len; unsigned char *tracks_bytes; size_t tracks_len; uint64_t video_track_number; } MkvHeaderFull;

static MkvHeaderFull parse_header(FILE *in, off_t *cluster_start_out) {
    MkvHeaderFull h; memset(&h, 0, sizeof(h));
    long id_start = ftello(in);
    uint32_t id; uint64_t size; int unk;
    if (read_id(in, &id) < 0 || id != ID_EBML) die("not a Matroska file (missing EBML header)");
    if (read_size(in, &size, &unk) < 0) die("EOF reading EBML header size");
    if (unk) die("EBML header has unknown size (unsupported)");
    long content_start = ftello(in);
    long elem_total = (content_start - id_start) + (long)size;
    fseeko(in, id_start, SEEK_SET);
    h.ebml_bytes = malloc(elem_total);
    if (!h.ebml_bytes) die("out of memory");
    if (fread(h.ebml_bytes, 1, elem_total, in) != (size_t)elem_total) die("EOF reading EBML header");
    h.ebml_len = elem_total;

    if (read_id(in, &id) < 0 || id != ID_SEGMENT) die("expected Segment element after EBML header");
    uint64_t seg_size; int seg_unk;
    if (read_size(in, &seg_size, &seg_unk) < 0) die("EOF reading Segment size");
    long seg_content_start = ftello(in);
    long seg_content_end = seg_unk ? -1 : (seg_content_start + (long)seg_size);

    int have_tracks = 0;
    for (;;) {
        long epos = ftello(in);
        if (seg_content_end >= 0 && epos >= seg_content_end) die("reached end of Segment before finding a video Cluster");
        uint32_t eid; uint64_t esize; int eunk;
        if (read_id(in, &eid) < 0) die("EOF before finding a video Cluster");
        if (read_size(in, &esize, &eunk) < 0) die("EOF reading element size");
        long econtent_start = ftello(in);
        if (eid == ID_TRACKS) {
            if (eunk) die("Tracks element has unknown size (unsupported)");
            long elen = (econtent_start - epos) + (long)esize;
            fseeko(in, epos, SEEK_SET);
            h.tracks_bytes = malloc(elen);
            if (!h.tracks_bytes) die("out of memory");
            if (fread(h.tracks_bytes, 1, elen, in) != (size_t)elen) die("EOF reading Tracks element");
            h.tracks_len = elen;
            have_tracks = 1;
            h.video_track_number = find_video_track_number(h.tracks_bytes, h.tracks_len);
        } else if (eid == ID_INFO) {
            if (eunk) die("Info element has unknown size (unsupported)");
            long elen = (econtent_start - epos) + (long)esize;
            fseeko(in, epos, SEEK_SET);
            h.info_bytes = malloc(elen);
            if (!h.info_bytes) die("out of memory");
            if (fread(h.info_bytes, 1, elen, in) != (size_t)elen) die("EOF reading Info element");
            h.info_len = elen;
        } else if (eid == ID_CLUSTER) {
            if (!have_tracks) die("found a Cluster before the Tracks element (unsupported ordering)");
            *cluster_start_out = epos;
            return h;
        } else {
            if (eunk) die("unsupported unknown-size element (id 0x%X) before the first Cluster", eid);
            fseeko(in, econtent_start + (long)esize, SEEK_SET);
        }
    }
}

static void write_segment_header(FILE *f) {
    unsigned char id[4] = {0x18, 0x53, 0x80, 0x67};
    fwrite(id, 1, 4, f);
    ebml_write_unknown_size(f);
}

static void mkdir_if_needed(const char *dir) {
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        die("cannot create output directory %s: %s", dir, strerror(errno));
    }
}

static void join_path(char *out, size_t cap, const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    if (dlen > 0 && dir[dlen - 1] == '/')
        snprintf(out, cap, "%s%s", dir, name);
    else
        snprintf(out, cap, "%s/%s", dir, name);
}

/* Strip directory and extension: "/a/b/show.mkv" -> "show" */
static char *basename_noext(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    char *out = malloc(len + 1);
    if (!out) die("out of memory");
    memcpy(out, base, len);
    out[len] = '\0';
    return out;
}

static FILE *open_output(const char *out_dir, const char *base_name, int idx, MkvHeaderFull *hdr, char *name_out, size_t name_cap) {
    char fname[512];
    snprintf(fname, sizeof(fname), "%s-%06d.mkv", base_name, idx);
    join_path(name_out, name_cap, out_dir, fname);
    FILE *f = fopen(name_out, "wb");
    if (!f) die("cannot create output file %s: %s", name_out, strerror(errno));
    fwrite(hdr->ebml_bytes, 1, hdr->ebml_len, f);
    write_segment_header(f);
    if (hdr->info_bytes) fwrite(hdr->info_bytes, 1, hdr->info_len, f);
    fwrite(hdr->tracks_bytes, 1, hdr->tracks_len, f);
    return f;
}

/* ---------------- keyframe / timecode peeking (non-consuming) ---------------- */

static int peek_simpleblock_keyframe(FILE *f) {
    long save = ftello(f);
    unsigned char vint[8]; raw_vint_bytes(f, vint, sizeof(vint));
    fgetc(f); fgetc(f);
    int flags = fgetc(f);
    fseeko(f, save, SEEK_SET);
    return (flags & 0x80) != 0;
}

static int64_t peek_block_rel_tc(FILE *f, uint32_t id, uint64_t content_size) {
    long save = ftello(f);
    int64_t result = 0;
    if (id == ID_SIMPLEBLOCK) {
        unsigned char vint[8]; raw_vint_bytes(f, vint, sizeof(vint));
        int hi = fgetc(f), lo = fgetc(f);
        result = (int16_t)(((unsigned)hi << 8) | (unsigned)lo);
    } else {
        uint64_t remaining = content_size;
        while (remaining > 0) {
            uint32_t cid; uint64_t csize2; int cunk;
            long before = ftello(f);
            int idlen = read_id(f, &cid);
            if (idlen < 0) die("EOF scanning BlockGroup for Block");
            int slen = read_size(f, &csize2, &cunk);
            if (slen < 0 || cunk) die("bad size scanning BlockGroup");
            long header_len = ftello(f) - before;
            if (cid == ID_BLOCK) {
                unsigned char vint[8]; raw_vint_bytes(f, vint, sizeof(vint));
                int hi = fgetc(f), lo = fgetc(f);
                result = (int16_t)(((unsigned)hi << 8) | (unsigned)lo);
                break;
            } else {
                skip_bytes(f, csize2);
            }
            remaining -= (header_len + csize2);
        }
    }
    fseeko(f, save, SEEK_SET);
    return result;
}

/* ---------------- split ---------------- */

static void split_body(FILE *in, off_t cluster_start, uint64_t total_size, MkvHeaderFull *hdr, const char *out_dir, const char *base_name) {
    fseeko(in, cluster_start, SEEK_SET);
    char name[4160];
    int file_index = 1;
    FILE *out = open_output(out_dir, base_name, file_index, hdr, name, sizeof(name));

    char tcmap_path[4160], tcmap_name[560];
    snprintf(tcmap_name, sizeof(tcmap_name), "%s.tcmap", base_name);
    join_path(tcmap_path, sizeof(tcmap_path), out_dir, tcmap_name);
    FILE *tcmap = fopen(tcmap_path, "w");
    if (!tcmap) die("cannot create timecode map %s: %s", tcmap_path, strerror(errno));

    int cur_open = 0;
    int64_t cur_base_tc = 0;
    int global_first_block = 1;
    uint64_t frame_count = 0;
    clock_gettime(CLOCK_MONOTONIC, &g_pb_last);

    for (;;) {
        uint32_t id; uint64_t size; int unk;
        long epos = ftello(in);
        (void)epos;
        int idlen = read_id(in, &id);
        if (idlen < 0) break; /* EOF: done */
        if (read_size(in, &size, &unk) < 0) die("EOF reading top-level element size");
        long content_start = ftello(in);
        if (unk) die("unsupported unknown-size top-level element (id 0x%X) while splitting", id);
        if (id != ID_CLUSTER) {
            fseeko(in, content_start + (long)size, SEEK_SET);
            continue;
        }
        long cluster_content_end = content_start + (long)size;
        uint64_t cluster_abs_tc = 0; int have_cluster_tc = 0;

        while (ftello(in) < cluster_content_end) {
            uint32_t cid; uint64_t csize; int cunk;
            if (read_id(in, &cid) < 0) die("EOF inside Cluster");
            if (read_size(in, &csize, &cunk) < 0) die("EOF inside Cluster reading size");
            if (cunk) die("unsupported unknown-size element inside Cluster");
            long ccontent_start = ftello(in);

            if (cid == ID_TIMECODE) {
                cluster_abs_tc = read_uint_be(in, csize);
                have_cluster_tc = 1;
            } else if (cid == ID_SIMPLEBLOCK || cid == ID_BLOCKGROUP) {
                if (!have_cluster_tc) { have_cluster_tc = 1; cluster_abs_tc = 0; }
                int is_keyframe;
                if (cid == ID_SIMPLEBLOCK) {
                    is_keyframe = peek_simpleblock_keyframe(in);
                } else {
                    is_keyframe = !blockgroup_has_reference(in, csize);
                    fseeko(in, ccontent_start, SEEK_SET);
                }
                int64_t block_rel_tc = peek_block_rel_tc(in, cid, csize);
                int64_t abs_tc = (int64_t)cluster_abs_tc + block_rel_tc;

                if (is_keyframe && !global_first_block) {
                    fclose(out);
                    file_index++;
                    out = open_output(out_dir, base_name, file_index, hdr, name, sizeof(name));
                    cur_open = 0;
                }
                if (!cur_open) {
                    cur_base_tc = abs_tc;
                    unsigned char cid4[4] = {0x1F, 0x43, 0xB6, 0x75};
                    fwrite(cid4, 1, 4, out);
                    ebml_write_unknown_size(out);
                    /* Normalize this GOP file's internal clock to start at
                     * 0 so identical content produces identical bytes
                     * regardless of where in the source it occurred (needed
                     * for content-based dedup). The true original offset is
                     * recorded in the .tcmap sidecar so `join` can restore
                     * the real absolute timeline. */
                    ebml_write_uint_fixed(out, ID_TIMECODE, 0, GOP_TIMECODE_WIDTH);
                    fprintf(tcmap, "%d %llu\n", file_index, (unsigned long long)cur_base_tc);
                    cur_open = 1;
                }
                int64_t tc_offset = (int64_t)cluster_abs_tc - cur_base_tc;
                copy_block_like(in, out, cid, csize, tc_offset);
                frame_count++;
                global_first_block = 0;
                progress_bar((uint64_t)ftello(in), total_size, "split");
            } else {
                fseeko(in, ccontent_start + (long)csize, SEEK_SET);
            }
        }
        fseeko(in, cluster_content_end, SEEK_SET);
    }
    fclose(out);
    fclose(tcmap);
    progress_bar(total_size, total_size, "split");
    double avg_gop = file_index > 0 ? (double)frame_count / (double)file_index : 0.0;
    fprintf(stderr, "mkvgopsplit: wrote %d GOP file(s), %llu frame(s), track #%llu, avg %.1f frames/GOP\n",
            file_index, (unsigned long long)frame_count, (unsigned long long)hdr->video_track_number, avg_gop);
}

/* ---------------- directory scan for join ---------------- */

typedef struct { long index; char *path; } GopEntry;

static int gop_entry_cmp(const void *a, const void *b) {
    long ia = ((const GopEntry *)a)->index, ib = ((const GopEntry *)b)->index;
    return (ia > ib) - (ia < ib);
}

/* Matches "<base>-<digits>.mkv" exactly; on match returns 1 and sets *index. */
static int match_gop_filename(const char *filename, const char *base) {
    size_t blen = strlen(base);
    if (strncmp(filename, base, blen) != 0) return 0;
    if (filename[blen] != '-') return 0;
    const char *p = filename + blen + 1;
    const char *digits_start = p;
    while (isdigit((unsigned char)*p)) p++;
    if (p == digits_start) return 0;
    if (strcmp(p, ".mkv") != 0) return 0;
    return 1;
}

static int scan_gop_files(const char *in_dir, const char *base_name, GopEntry **out_entries) {
    DIR *d = opendir(in_dir);
    if (!d) die("cannot open input directory %s: %s", in_dir, strerror(errno));
    GopEntry *entries = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!match_gop_filename(de->d_name, base_name)) continue;
        size_t blen = strlen(base_name);
        long idx = strtol(de->d_name + blen + 1, NULL, 10);
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            entries = realloc(entries, cap * sizeof(GopEntry));
            if (!entries) die("out of memory");
        }
        char full[4160];
        join_path(full, sizeof(full), in_dir, de->d_name);
        entries[n].index = idx;
        entries[n].path = strdup(full);
        n++;
    }
    closedir(d);
    if (n == 0) die("no files matching \"%s-NNNN.mkv\" found in %s", base_name, in_dir);
    qsort(entries, n, sizeof(GopEntry), gop_entry_cmp);
    *out_entries = entries;
    return (int)n;
}

/* ---------------- .tcmap sidecar (original absolute GOP start times) ---------------- */

typedef struct { long index; uint64_t tick; } TcMapEntry;

static int tcmap_cmp(const void *a, const void *b) {
    long ia = ((const TcMapEntry *)a)->index, ib = ((const TcMapEntry *)b)->index;
    return (ia > ib) - (ia < ib);
}

static int tcmap_find_cmp(const void *key, const void *elem) {
    long ik = *(const long *)key, ie = ((const TcMapEntry *)elem)->index;
    return (ik > ie) - (ik < ie);
}

static TcMapEntry *read_tcmap(const char *dir, const char *base_name, size_t *out_n) {
    char path[4160], name[560];
    snprintf(name, sizeof(name), "%s.tcmap", base_name);
    join_path(path, sizeof(path), dir, name);
    FILE *f = fopen(path, "r");
    if (!f) die("cannot find timecode map %s (created by `split` alongside the GOP files; "
                "needed to restore correct absolute timing on join)", path);
    TcMapEntry *entries = NULL;
    size_t n = 0, cap = 0;
    long idx; unsigned long long tick;
    while (fscanf(f, "%ld %llu", &idx, &tick) == 2) {
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            entries = realloc(entries, cap * sizeof(TcMapEntry));
            if (!entries) die("out of memory");
        }
        entries[n].index = idx;
        entries[n].tick = (uint64_t)tick;
        n++;
    }
    fclose(f);
    qsort(entries, n, sizeof(TcMapEntry), tcmap_cmp);
    *out_n = n;
    return entries;
}

static uint64_t tcmap_lookup(TcMapEntry *entries, size_t n, long index) {
    TcMapEntry *found = bsearch(&index, entries, n, sizeof(TcMapEntry), tcmap_find_cmp);
    if (!found) die("no timecode map entry for GOP index %ld (map may be stale or incomplete)", index);
    return found->tick;
}

/* ---------------- join ---------------- */

static void join_files(const char *out_path, GopEntry *entries, int n_inputs, TcMapEntry *tcmap, size_t tcmap_n) {
    FILE *out = fopen(out_path, "wb");
    if (!out) die("cannot create %s", out_path);
    uint64_t total_size = 0;
    for (int i = 0; i < n_inputs; i++) {
        struct stat st;
        if (stat(entries[i].path, &st) == 0) total_size += (uint64_t)st.st_size;
    }
    uint64_t done = 0;
    clock_gettime(CLOCK_MONOTONIC, &g_pb_last);

    for (int i = 0; i < n_inputs; i++) {
        const char *path = entries[i].path;
        FILE *in = fopen(path, "rb");
        if (!in) die("cannot open %s", path);
        off_t cluster_start;
        MkvHeaderFull h = parse_header(in, &cluster_start);
        fseeko(in, 0, SEEK_END);
        long fsize = ftello(in);
        if (i == 0) {
            fwrite(h.ebml_bytes, 1, h.ebml_len, out);
            write_segment_header(out);
            if (h.info_bytes) fwrite(h.info_bytes, 1, h.info_len, out);
            fwrite(h.tracks_bytes, 1, h.tracks_len, out);
        }
        fseeko(in, cluster_start, SEEK_SET);

        /* Each GOP file is exactly one Cluster (unknown size, runs to
         * EOF) whose Timecode was normalized to 0 by `split`. Copy the
         * Cluster's id+size through unchanged, patch its Timecode value
         * to the file's true original offset, then stream every
         * remaining byte (all the blocks) through completely unchanged -
         * their relative timecodes stay correct automatically since
         * they're relative to the Cluster's own Timecode. */
        uint32_t cid; uint64_t csize; int cunk;
        long cluster_id_pos = ftello(in);
        if (read_id(in, &cid) < 0 || cid != ID_CLUSTER)
            die("%s: expected a Cluster where the header parser left off", path);
        if (read_size(in, &csize, &cunk) < 0) die("%s: EOF reading Cluster size", path);
        long after_cluster_header = ftello(in);
        fseeko(in, cluster_id_pos, SEEK_SET);
        unsigned char clhdr[16];
        long clhdrlen = after_cluster_header - cluster_id_pos;
        if (fread(clhdr, 1, clhdrlen, in) != (size_t)clhdrlen) die("%s: re-read of Cluster header failed", path);
        fwrite(clhdr, 1, clhdrlen, out);

        uint32_t tid; uint64_t tsize; int tunk;
        if (read_id(in, &tid) < 0 || tid != ID_TIMECODE || read_size(in, &tsize, &tunk) < 0 || tsize != GOP_TIMECODE_WIDTH)
            die("%s: expected a %d-byte Timecode element right after the Cluster "
                "(file predates timecode-normalization support? re-split with the current tool)",
                path, GOP_TIMECODE_WIDTH);

        uint64_t offset = tcmap_lookup(tcmap, tcmap_n, entries[i].index);
        fputc((unsigned char)ID_TIMECODE, out);
        ebml_write_size(out, GOP_TIMECODE_WIDTH, ebml_size_len_needed(GOP_TIMECODE_WIDTH));
        unsigned char tbuf[GOP_TIMECODE_WIDTH];
        uint64_t v = offset;
        for (int k = GOP_TIMECODE_WIDTH - 1; k >= 0; k--) { tbuf[k] = (unsigned char)(v & 0xFF); v >>= 8; }
        fwrite(tbuf, 1, GOP_TIMECODE_WIDTH, out);
        skip_bytes(in, tsize); /* skip past the original (0) Timecode value we just read past */

        long remain_start = ftello(in);
        uint64_t remain = (uint64_t)fsize - (uint64_t)remain_start;
        copy_bytes(in, out, remain);

        done += (uint64_t)fsize;
        progress_bar(done, total_size, "join ");
        free(h.ebml_bytes);
        free(h.info_bytes);
        free(h.tracks_bytes);
        fclose(in);
    }
    progress_bar(total_size, total_size, "join ");
    fclose(out);
    fprintf(stderr, "mkvgopsplit: joined %d file(s) into %s\n", n_inputs, out_path);
}


/* ---------------- main ---------------- */

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage:\n"
        "  %s split <input.mkv> <output_dir> [base_name]\n"
        "      writes <output_dir>/<base_name>-000001.mkv, -000002.mkv, ...\n"
        "      plus <output_dir>/<base_name>.tcmap (original timing, needed by join).\n"
        "      base_name defaults to <input.mkv>'s filename without extension.\n"
        "  %s join  <input_dir> <base_name> <output_dir>\n"
        "      finds <input_dir>/<base_name>-NNNN.mkv (any count, any digit width)\n"
        "      and writes <output_dir>/<base_name>.mkv\n",
        argv0, argv0);
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 2) usage(argv[0]);
    if (strcmp(argv[1], "split") == 0) {
        if (argc != 4 && argc != 5) usage(argv[0]);
        const char *input_path = argv[2];
        const char *out_dir = argv[3];
        char *derived = NULL;
        const char *base_name = (argc == 5) ? argv[4] : (derived = basename_noext(input_path));
        mkdir_if_needed(out_dir);
        FILE *in = fopen(input_path, "rb");
        if (!in) die("cannot open %s", input_path);
        struct stat st;
        if (stat(input_path, &st) != 0) die("cannot stat %s", input_path);
        off_t cluster_start;
        MkvHeaderFull h = parse_header(in, &cluster_start);
        split_body(in, cluster_start, (uint64_t)st.st_size, &h, out_dir, base_name);
        fclose(in);
        free(derived);
    } else if (strcmp(argv[1], "join") == 0) {
        if (argc != 5) usage(argv[0]);
        const char *in_dir = argv[2];
        const char *base_name = argv[3];
        const char *out_dir = argv[4];
        mkdir_if_needed(out_dir);
        GopEntry *entries;
        int n = scan_gop_files(in_dir, base_name, &entries);
        size_t tcmap_n;
        TcMapEntry *tcmap = read_tcmap(in_dir, base_name, &tcmap_n);
        char out_name[512], out_path[4160];
        snprintf(out_name, sizeof(out_name), "%s.mkv", base_name);
        join_path(out_path, sizeof(out_path), out_dir, out_name);
        join_files(out_path, entries, n, tcmap, tcmap_n);
        for (int i = 0; i < n; i++) free(entries[i].path);
        free(entries);
        free(tcmap);
    } else {
        usage(argv[0]);
    }
    return 0;
}
