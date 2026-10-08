#!/usr/bin/env python3
"""Generate OpenDisplay MTT user_edid.bin (EDID 1.3, 128 bytes).

Bakes the landscape hello modes as detailed timing descriptors and a fixed
manufacturer/product/serial so Windows keeps display arrangement across
reconnects. CustomEdid=true loads this file from C:\\VirtualDisplayDriver\\.

Usage:
  python tools/gen_user_edid.py                  # write assets/mtt/user_edid.bin
  python tools/gen_user_edid.py -o path.bin      # custom output
  python tools/gen_user_edid.py --decode path    # print modes/identity

Identity (fixed, never randomize):
  Manufacturer: MTT
  Product:      0x1337
  Serial:       0x4F445731  ("ODW1")
  Name:         OpenDisplay (via preferred DTD only — all 4 slots are timings)

Modes @ 60 Hz (DTD order; first = preferred):
  2352x1632, 2384x1664, 1760x1216, 1168x816
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# --- Fixed identity (do not randomize) ---
MANUFACTURER = "MTT"
PRODUCT_CODE = 0x1337
SERIAL = 0x4F445731  # "ODW1" as ASCII in LE serial field
WEEK = 1
YEAR = 2026  # stored as year - 1990

# Landscape modes currently in live vdd_settings.xml / BuildIpadModeList for
# 11" hello. First entry is the preferred (native) timing.
MODES = [
    (2352, 1632, 60),
    (2384, 1664, 60),
    (1760, 1216, 60),
    (1168, 816, 60),
]

# ~11" class panel mm (aspect ≈ 1.44)
H_MM = 225
V_MM = 156


def manufacturer_id(letters: str) -> bytes:
    letters = letters.upper()
    assert len(letters) == 3 and letters.isalpha()
    a, b, c = (ord(ch) - ord("A") + 1 for ch in letters)
    val = (a << 10) | (b << 5) | c
    return struct.pack(">H", val)


def cvt_rb_params(h_active: int, v_active: int, refresh: int = 60):
    """CVT reduced-blanking-ish params; pick v_back so clock ≈ refresh."""
    h_front, h_sync, h_blank = 48, 32, 160
    v_front, v_sync = 3, 10
    h_total = h_active + h_blank
    best = None
    for v_back in range(6, 120):
        v_blank = v_front + v_sync + v_back
        v_total = v_active + v_blank
        clock_10khz = round(h_total * v_total * refresh / 10000.0)
        if not (1 <= clock_10khz <= 65535):
            continue
        actual = (clock_10khz * 10000.0) / (h_total * v_total)
        err = abs(actual - refresh)
        if best is None or err < best[0]:
            best = (err, clock_10khz, h_front, h_sync, h_blank, v_front, v_sync, v_back, v_blank)
            if err < 0.01:
                break
    if best is None:
        raise RuntimeError(f"no timing for {h_active}x{v_active}@{refresh}")
    _, clock_10khz, h_front, h_sync, h_blank, v_front, v_sync, v_back, v_blank = best
    return {
        "clock_10khz": clock_10khz,
        "h_active": h_active,
        "h_blank": h_blank,
        "h_front": h_front,
        "h_sync": h_sync,
        "v_active": v_active,
        "v_blank": v_blank,
        "v_front": v_front,
        "v_sync": v_sync,
        "v_back": v_back,
    }


def detailed_timing(h: int, v: int, hz: int = 60) -> bytes:
    p = cvt_rb_params(h, v, hz)
    b = bytearray(18)
    b[0] = p["clock_10khz"] & 0xFF
    b[1] = (p["clock_10khz"] >> 8) & 0xFF
    b[2] = p["h_active"] & 0xFF
    b[3] = p["h_blank"] & 0xFF
    b[4] = ((p["h_active"] >> 8) << 4) | ((p["h_blank"] >> 8) & 0x0F)
    b[5] = p["v_active"] & 0xFF
    b[6] = p["v_blank"] & 0xFF
    b[7] = ((p["v_active"] >> 8) << 4) | ((p["v_blank"] >> 8) & 0x0F)
    b[8] = p["h_front"] & 0xFF
    b[9] = p["h_sync"] & 0xFF
    b[10] = ((p["v_front"] & 0x0F) << 4) | (p["v_sync"] & 0x0F)
    b[11] = (
        ((p["h_front"] >> 8) << 6)
        | ((p["h_sync"] >> 8) << 4)
        | ((p["v_front"] >> 4) << 2)
        | ((p["v_sync"] >> 4) & 0x03)
    )
    b[12] = H_MM & 0xFF
    b[13] = V_MM & 0xFF
    b[14] = ((H_MM >> 8) << 4) | ((V_MM >> 8) & 0x0F)
    b[15] = 0  # h border
    b[16] = 0  # v border
    # digital separate sync, h/v positive
    b[17] = 0x1E
    return bytes(b)


def checksum(block: bytes | bytearray) -> int:
    assert len(block) >= 127
    return (256 - (sum(block[:127]) % 256)) % 256


def build_edid() -> bytes:
    e = bytearray(128)
    e[0:8] = bytes([0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00])
    e[8:10] = manufacturer_id(MANUFACTURER)
    e[10:12] = struct.pack("<H", PRODUCT_CODE)
    e[12:16] = struct.pack("<I", SERIAL)
    e[16] = WEEK
    e[17] = YEAR - 1990
    e[18] = 0x01  # EDID 1.3
    e[19] = 0x03
    # Digital input, DisplayPort-ish bit depth unspecified, DFPs compliant
    e[20] = 0x80
    e[21] = max(1, H_MM // 10)  # cm
    e[22] = max(1, V_MM // 10)
    e[23] = 0x78  # gamma 2.2
    # features: preferred timing mode, continuous freq not claimed, RGB
    e[24] = 0x0A
    # sRGB-ish chromaticity (standard template)
    e[25:35] = bytes([0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54])
    e[35:38] = bytes([0x00, 0x00, 0x00])  # no established timings
    # Standard timings unused
    for i in range(38, 54):
        e[i] = 0x01

    assert len(MODES) <= 4, "base EDID only has 4 DTD slots"
    for i, (w, h, hz) in enumerate(MODES):
        off = 54 + i * 18
        e[off : off + 18] = detailed_timing(w, h, hz)

    e[126] = 0  # no extension blocks
    e[127] = checksum(e)
    assert sum(e) % 256 == 0
    return bytes(e)


def decode_dtd(d: bytes) -> dict | None:
    clock = d[0] | (d[1] << 8)
    if clock == 0:
        return None
    ha = d[2] | ((d[4] >> 4) << 8)
    hb = d[3] | ((d[4] & 0x0F) << 8)
    va = d[5] | ((d[7] >> 4) << 8)
    vb = d[6] | ((d[7] & 0x0F) << 8)
    ht = ha + hb
    vt = va + vb
    hz = (clock * 10000) / (ht * vt) if ht and vt else 0
    return {"w": ha, "h": va, "hz": round(hz, 2), "clock_mhz": clock / 100.0}


def decode(path: Path) -> None:
    data = path.read_bytes()
    print(f"file: {path} ({len(data)} bytes)")
    assert data[0:8] == bytes([0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00])
    mid = (data[8] << 8) | data[9]
    def unpack(v):
        return chr(((v >> 10) & 0x1F) + 64) + chr(((v >> 5) & 0x1F) + 64) + chr((v & 0x1F) + 64)
    prod = data[10] | (data[11] << 8)
    serial = data[12] | (data[13] << 8) | (data[14] << 16) | (data[15] << 24)
    print(f"manufacturer: {unpack(mid)}  product: 0x{prod:04X}  serial: 0x{serial:08X}")
    print(f"edid: {data[18]}.{data[19]}  week={data[16]} year={1990+data[17]}")
    print(f"checksum ok: {sum(data[:128]) % 256 == 0}")
    for i in range(4):
        d = decode_dtd(data[54 + i * 18 : 72 + i * 18])
        if d:
            pref = " (preferred)" if i == 0 else ""
            print(f"  DTD{i}: {d['w']}x{d['h']} @{d['hz']}Hz  clock={d['clock_mhz']:.2f}MHz{pref}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", type=Path, default=None)
    ap.add_argument("--decode", type=Path, default=None)
    args = ap.parse_args()
    if args.decode:
        decode(args.decode)
        return 0
    out = args.output
    if out is None:
        root = Path(__file__).resolve().parents[1]
        out = root / "assets" / "mtt" / "user_edid.bin"
    out.parent.mkdir(parents=True, exist_ok=True)
    data = build_edid()
    out.write_bytes(data)
    print(f"wrote {out} ({len(data)} bytes)")
    decode(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
