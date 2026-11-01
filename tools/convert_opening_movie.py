#!/usr/bin/env python3
"""Convert the game's Indeo 5 opening movie into the mod's lossless Q99V AVI.

Why: SYSTEM\\OPENING.AVI is Intel Indeo 5, which Windows 10/11 no longer
decode (black picture, audio only). qoh99_netplay.dll registers an in-process
VfW decoder for the fourcc 'Q99V' and redirects the game's MCI open to the
sidecar this script writes, so the intro plays on every system with no codec
installed and no loss against the original.

Format (Q99V, version 1): an ordinary AVI whose video stream is tagged 'Q99V'.
Each video chunk is a 16-byte header followed by a zlib stream:

    char     magic[4]   'Q99V'
    uint8    version    1
    uint8    kind       0 = key frame, 1 = delta against the previous frame
    uint8    predictor  0 = none, 1 = sub (left neighbour), 2 = up (row above)
    uint8    reserved   0
    uint32   sequence   frame index in the stream
    uint32   rawSize    W*H*3/2 - the yuv410p planes (Y full, U and V at 1/4)

The zlib payload inflates to rawSize bytes: the predicted planes for a key
frame, or (current - previous) mod 256 for a delta. yuv410p is the Indeo 5
decoder's own output format, so nothing is lost storing it; the DLL converts
to RGB when it decodes. The audio track is the original PCM, untouched.

Usage:
    convert_opening_movie.py <SYSTEM\\OPENING.AVI> <out OPENING_LOSSLESS.AVI>
        [--keyint N] [--level L] [--ffmpeg PATH]

Requires ffmpeg (with the indeo5 decoder, which every build has) and numpy.
The written file is re-read and every frame decoded again to prove the
round trip is byte-exact before the script reports success.
"""
import argparse
import struct
import subprocess
import sys
import zlib

import numpy as np

W, H = 320, 240
CW, CH = W // 4, H // 4
RAW_SIZE = W * H + 2 * CW * CH
FPS_RATE, FPS_SCALE = 30, 1
AUDIO_RATE, AUDIO_CHANNELS, AUDIO_BITS = 22050, 2, 8
AUDIO_BYTES_PER_FRAME = AUDIO_RATE * AUDIO_CHANNELS * FPS_SCALE // FPS_RATE
FOURCC = b"Q99V"
HEADER = struct.Struct("<4sBBBBII")
AVIIF_KEYFRAME = 0x10
AVIF_HASINDEX = 0x10
AVIF_ISINTERLEAVED = 0x100

def run_ffmpeg(ffmpeg, args):
    proc = subprocess.run([ffmpeg, "-v", "error", *args], stdout=subprocess.PIPE, check=True)
    return proc.stdout

def split_planes(raw):
    y = np.frombuffer(raw[: W * H], np.uint8).reshape(H, W)
    u = np.frombuffer(raw[W * H : W * H + CW * CH], np.uint8).reshape(CH, CW)
    v = np.frombuffer(raw[W * H + CW * CH :], np.uint8).reshape(CH, CW)
    return (y, u, v)

def predict(plane, predictor):
    out = plane.astype(np.int16)
    if predictor == 1:
        out[:, 1:] -= plane[:, :-1].astype(np.int16)
    elif predictor == 2:
        out[1:, :] -= plane[:-1, :].astype(np.int16)
    return out.astype(np.uint8)

def unpredict(plane, predictor):
    out = plane.copy()
    if predictor == 1:
        for x in range(1, out.shape[1]):
            out[:, x] = (out[:, x].astype(np.int16) + out[:, x - 1].astype(np.int16)).astype(np.uint8)
    elif predictor == 2:
        for y in range(1, out.shape[0]):
            out[y, :] = (out[y, :].astype(np.int16) + out[y - 1, :].astype(np.int16)).astype(np.uint8)
    return out

def encode_frame(seq, planes, prev, key_interval, level):
    candidates = []
    for predictor in (1, 2, 0):
        body = b"".join(predict(p, predictor).tobytes() for p in planes)
        candidates.append((len(zlib.compress(body, level)), 0, predictor, body))
    force_key = prev is None or seq % key_interval == 0
    if not force_key:
        delta = b"".join(
            (c.astype(np.int16) - p.astype(np.int16)).astype(np.uint8).tobytes()
            for c, p in zip(planes, prev)
        )
        candidates.append((len(zlib.compress(delta, level)), 1, 0, delta))
    size, kind, predictor, body = min(candidates, key=lambda c: c[0])
    payload = HEADER.pack(FOURCC, 1, kind, predictor, 0, seq, RAW_SIZE) + zlib.compress(body, level)
    return payload, kind == 0

def decode_frame(payload, prev):
    magic, version, kind, predictor, _res, seq, raw_size = HEADER.unpack_from(payload, 0)
    if magic != FOURCC or version != 1 or raw_size != RAW_SIZE:
        raise ValueError("bad frame header")
    body = zlib.decompress(payload[HEADER.size :])
    if len(body) != RAW_SIZE:
        raise ValueError("bad frame size")
    planes = split_planes(body)
    if kind == 0:
        return tuple(unpredict(p, predictor) for p in planes), seq
    if prev is None:
        raise ValueError("delta frame without a previous frame")
    return tuple((c.astype(np.int16) + p.astype(np.int16)).astype(np.uint8) for c, p in zip(planes, prev)), seq

def chunk(tag, data):
    pad = b"\0" if len(data) & 1 else b""
    return tag + struct.pack("<I", len(data)) + data + pad

def riff_list(tag, body):
    return b"LIST" + struct.pack("<I", len(body) + 4) + tag + body

def write_avi(path, frames, keyflags, audio):
    n = len(frames)
    audio_chunks = [audio[i * AUDIO_BYTES_PER_FRAME : (i + 1) * AUDIO_BYTES_PER_FRAME] for i in range(n)]
    audio_len = sum(len(a) for a in audio_chunks)
    max_video = max(len(f) for f in frames)
    max_chunk = max(max_video, AUDIO_BYTES_PER_FRAME) + 8
    us_per_frame = 1000000 * FPS_SCALE // FPS_RATE
    bytes_per_sec = int((sum(len(f) for f in frames) + audio_len) / (n / FPS_RATE))

    avih = struct.pack(
        "<IIIIIIIIIIIIII", us_per_frame, bytes_per_sec, 0, AVIF_HASINDEX | AVIF_ISINTERLEAVED,
        n, 0, 2, max_chunk, W, H, 0, 0, 0, 0)
    strh_v = struct.pack(
        "<4s4sIHHIIIIIIIIhhhh", b"vids", FOURCC, 0, 0, 0, 0, FPS_SCALE, FPS_RATE, 0, n,
        max_video + 8, 0xFFFFFFFF, 0, 0, 0, W, H)
    bih = struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, struct.unpack("<I", FOURCC)[0], W * H * 3, 0, 0, 0, 0)
    strl_v = riff_list(b"strl", chunk(b"strh", strh_v) + chunk(b"strf", bih))
    block_align = AUDIO_CHANNELS * AUDIO_BITS // 8
    strh_a = struct.pack(
        "<4s4sIHHIIIIIIIIhhhh", b"auds", b"\0\0\0\0", 0, 0, 0, 0, block_align,
        AUDIO_RATE * block_align, 0, audio_len // block_align, AUDIO_BYTES_PER_FRAME + 8,
        0xFFFFFFFF, block_align, 0, 0, 0, 0)
    wfx = struct.pack("<HHIIHH", 1, AUDIO_CHANNELS, AUDIO_RATE, AUDIO_RATE * block_align, block_align, AUDIO_BITS)
    strl_a = riff_list(b"strl", chunk(b"strh", strh_a) + chunk(b"strf", wfx))
    hdrl = riff_list(b"hdrl", chunk(b"avih", avih) + strl_v + strl_a)

    movi_parts = []
    idx = []
    offset = 4  # index offsets are relative to the 'movi' fourcc
    for i in range(n):
        v = chunk(b"00dc", frames[i])
        idx.append(struct.pack("<4sIII", b"00dc", AVIIF_KEYFRAME if keyflags[i] else 0, offset, len(frames[i])))
        movi_parts.append(v)
        offset += len(v)
        if audio_chunks[i]:
            a = chunk(b"01wb", audio_chunks[i])
            idx.append(struct.pack("<4sIII", b"01wb", AVIIF_KEYFRAME, offset, len(audio_chunks[i])))
            movi_parts.append(a)
            offset += len(a)
    movi = riff_list(b"movi", b"".join(movi_parts))
    body = b"AVI " + hdrl + movi + chunk(b"idx1", b"".join(idx))
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)

def read_back_video(path):
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"AVI ":
        raise ValueError("not an AVI")
    movi = data.find(b"LIST", 12)
    frames = []
    while movi != -1:
        size = struct.unpack_from("<I", data, movi + 4)[0]
        if data[movi + 8 : movi + 12] == b"movi":
            pos = movi + 12
            end = movi + 8 + size
            while pos + 8 <= end:
                tag = data[pos : pos + 4]
                csize = struct.unpack_from("<I", data, pos + 4)[0]
                if tag == b"00dc":
                    frames.append(data[pos + 8 : pos + 8 + csize])
                pos += 8 + csize + (csize & 1)
            break
        movi = data.find(b"LIST", movi + 8 + size)
    return frames

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source")
    ap.add_argument("output")
    ap.add_argument("--keyint", type=int, default=30, help="force a key frame every N frames (default 30)")
    ap.add_argument("--level", type=int, default=9, help="deflate level 1-9 (default 9)")
    ap.add_argument("--ffmpeg", default="ffmpeg")
    args = ap.parse_args()

    print("decoding video (yuv410p)...", flush=True)
    raw = run_ffmpeg(args.ffmpeg, ["-i", args.source, "-map", "0:v", "-f", "rawvideo", "-pix_fmt", "yuv410p", "-"])
    if len(raw) % RAW_SIZE:
        sys.exit(f"unexpected raw video size {len(raw)}; is the source 320x240?")
    n = len(raw) // RAW_SIZE
    print("decoding audio (pcm_u8)...", flush=True)
    audio = run_ffmpeg(args.ffmpeg, ["-i", args.source, "-map", "0:a", "-f", "u8", "-acodec", "pcm_u8",
                                     "-ac", str(AUDIO_CHANNELS), "-ar", str(AUDIO_RATE), "-"])
    print(f"{n} frames, {len(audio)} audio bytes ({len(audio) / (AUDIO_RATE * AUDIO_CHANNELS):.1f} s)", flush=True)

    sources = [split_planes(raw[i * RAW_SIZE : (i + 1) * RAW_SIZE]) for i in range(n)]
    frames, keys = [], []
    prev = None
    for i, planes in enumerate(sources):
        payload, is_key = encode_frame(i, planes, prev, args.keyint, args.level)
        frames.append(payload)
        keys.append(is_key)
        prev = planes
        if i % 300 == 0:
            print(f"  encoded {i}/{n}", flush=True)
    write_avi(args.output, frames, keys, audio)

    print("verifying round trip...", flush=True)
    prev = None
    back = read_back_video(args.output)
    if len(back) != n:
        sys.exit(f"read back {len(back)} frames, expected {n}")
    for i, payload in enumerate(back):
        planes, seq = decode_frame(payload, prev)
        if seq != i:
            sys.exit(f"frame {i} carries sequence {seq}")
        for got, want in zip(planes, sources[i]):
            if not np.array_equal(got, want):
                sys.exit(f"frame {i} is not byte-exact after decode")
        prev = planes
    video_bytes = sum(len(f) for f in frames)
    print(f"OK: {args.output}: {n} frames ({sum(keys)} key), video {video_bytes / 1e6:.1f} MB, "
          f"audio {len(audio) / 1e6:.1f} MB, lossless round trip verified")

if __name__ == "__main__":
    main()
