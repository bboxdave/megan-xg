#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""Quick structural analyzer for MU100/XG .syx files. Splits into F0..F7 messages
and classifies each per the MU100 MIDI spec (XG/native param change, bulk dump,
dump request, universal)."""
import sys, os

MODEL = {0x4C: "XG", 0x49: "NAT1(49)", 0x59: "NAT2(59)", 0x27: "MasterTune(27)"}

def msgs(data):
    out, i, n = [], 0, len(data)
    while i < n:
        if data[i] == 0xF0:
            j = i
            while j < n and data[j] != 0xF7:
                j += 1
            out.append(data[i:j+1])
            i = j + 1
        else:
            i += 1
    return out

def classify(m):
    if len(m) < 2: return "runt"
    b = list(m)
    if b[1] == 0x7E: return "UNIVERSAL NON-RT (GM/Identity)"
    if b[1] == 0x7F: return "UNIVERSAL RT (MasterVol)"
    if b[1] != 0x43: return "non-Yamaha id=%02X" % b[1]
    cmd = b[2] & 0xF0
    dev = b[2] & 0x0F
    model = b[3]
    mname = MODEL.get(model, "model?%02X" % model)
    if cmd == 0x10:  # 1n param change
        addr = b[4:7]
        data = b[7:-1]
        return "PARAM-CHG  %-10s dev=%X addr=%02X%02X%02X size=%d data=%s" % (
            mname, dev, addr[0], addr[1], addr[2], len(data),
            " ".join("%02X" % x for x in data))
    if cmd == 0x00:  # 0n bulk dump
        bc = (b[4] << 7) | b[5]
        addr = b[6:9]
        payload = b[9:-2]   # data... (before checksum)
        cksum = b[-2]
        s = sum(b[4:-2]) & 0x7F  # bytecount+addr+data
        ok = ((s + cksum) & 0x7F) == 0
        return "BULK-DUMP  %-10s dev=%X addr=%02X%02X%02X bytecount=%d payloadlen=%d cksum=%02X %s" % (
            mname, dev, addr[0], addr[1], addr[2], bc, len(payload), cksum,
            "OK" if ok else "BADCKSUM")
    if cmd == 0x20:  # 2n dump request
        addr = b[4:7]
        return "DUMP-REQ   %-10s dev=%X addr=%02X%02X%02X" % (mname, dev, addr[0], addr[1], addr[2])
    if cmd == 0x30:  # 3n param request
        addr = b[4:7]
        return "PARAM-REQ  %-10s dev=%X addr=%02X%02X%02X" % (mname, dev, addr[0], addr[1], addr[2])
    return "yamaha cmd=%02X model=%s len=%d" % (b[2], mname, len(m))

def main():
    for path in sys.argv[1:]:
        data = open(path, "rb").read()
        ms = msgs(data)
        print("\n############", os.path.basename(path), "—", len(data), "bytes,", len(ms), "messages ############")
        from collections import Counter
        kinds = Counter()
        for k, m in enumerate(ms):
            c = classify(m)
            kinds[c.split()[0]] += 1
            if k < 40 or k >= len(ms)-3:
                print("  [%3d] len=%4d  %s" % (k, len(m), c))
            elif k == 40:
                print("  ...")
        print("  SUMMARY:", dict(kinds))

if __name__ == "__main__":
    main()
