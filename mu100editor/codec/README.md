# MU100 Editor — SysEx codec + state model (Phases 2–3)

JUCE-independent C++ for Yamaha MU100 / XG System Exclusive. Three layers; the
plugin compiles these files directly, and so does the test harness in `test/`:

```
MU100SysEx.h / .cpp        framing: parse / encode / checksum / nibblize
MU100State.h / .cpp        DeviceState: byte-exact state model (both formats -> one map)
MU100AddressMap.h / .cpp   semantic layer: named params <-> addresses + values
MU100AddressMapData.h      AUTO-GENERATED address table (from data/parameters.json)
test/sysex_test.cpp        unit + golden-file + state/map tests (no JUCE, no JSON)
test/build.ps1             compiles with MSVC cl.exe and runs the tests
```

Regenerate the address table after editing `data/parameters.json`:
```
python tools/gen_address_map.py        # -> codec/MU100AddressMapData.h
```

## Build & test
```
powershell -ExecutionPolicy Bypass -File codec/test/build.ps1
```
Compiles `sysex_test.exe` and runs it against the fixtures in
`reference/syx examples/`. Current status: **6194 assertions, 0 failures.**

## What it handles
Per the MU100 MIDI Data Format (sound-list PDF pp.34-39):

| Message | Frame |
|---|---|
| Parameter Change (XG/native) | `F0 43 1n MM aH aM aL <data..> F7` |
| Bulk Dump (XG/native) | `F0 43 0n MM bcH bcL aH aM aL <data..> ck F7` |
| Parameter Request | `F0 43 3n MM aH aM aL F7` |
| Dump Request | `F0 43 2n MM aH aM aL F7` |
| XG System On | `F0 43 1n 4C 00 00 7E 00 F7` |
| GM System On / Identity Request / Master Volume | universal 7E/7F |

`MM` (Model ID): `4C`=XG, `49`=MU100 native(1), `59`=MU100 native(2), `27`=Master Tune.

- **Checksum** (bulk): `(sum(byteCount + address + data) + checksum) & 0x7F == 0`.
  `bulkChecksum()` computes it; the parser verifies it (`Message::checksumOk`).
- **Multi-byte numeric values** are XG-nibblized (big-endian, 4 bits per data
  byte). `nibblize()` / `denibblize()`. *Effect TYPE MSB/LSB size-2 fields are
  raw 7-bit byte pairs, not nibblized — that distinction is a per-parameter
  attribute handled by the Phase-3 address-map layer, not the framing codec.*

## Format findings from the reference dumps (2026-06-22)

The five files in `reference/syx examples/` were captured by loading an XGEdit
patch into the MU100 and dumping, plus one file saved directly by XGEdit. They
reveal **two distinct on-disk formats**, both of which this codec reads and
writes byte-exact:

- **XGEdit `.syx` = a flat stream of XG Parameter Changes** (`echotech_2026_v2.syx`):
  model `4C`, **device nibble `F` (broadcast)**, led by one **XG System On**,
  then 242 parameter changes. Some addresses are re-sent (e.g. effect type
  written twice) — apply **last-wins**. This is the format we must keep loading
  for backward compatibility.
- **MU100 hardware "dumpout" = Bulk Dump blocks**, device `0`, each with byte
  count + checksum:
  - `all` — everything: XG (System/Eff1/EQ/Eff2/32 Multi Parts at `08 nn ll`,
    `nn`=`00..1F`/A-D part `0A`/plugin `10`) + native `49` System + native `59`
    Performance (`30-33`/`40`/`50-51`/`60-61`). 2945 msgs.
  - `mult-32` — XG blocks only (no native Performance). 1441 msgs.
  - `perf-all` — XG System On equivalent + native Performance only. 1504 msgs.
  - `perf-001` — a single performance: 15 native-`59` bulk blocks. 609 bytes.

Both formats reduce to the same `{model, address} -> value bytes` map, which the
Phase-3 state model will build on (`tools/analyze_syx.py` dumps the structure of
any `.syx` for inspection).

## State model (`MU100State` / `DeviceState`)
The byte-exact backbone: **one 7-bit value per (model, 21-bit address)**. Both
on-disk formats fold into the same map via `DeviceState::load()` — XGEdit
parameter-change streams *and* MU100 bulk dumps — so a file in either format
becomes the same editable state and the editor reads/writes addresses without
caring how they were framed. Re-export with `toBulkDumps()` or `toParamChanges()`
(reloading either reproduces the state exactly). Multi-byte *interpretation* is
deliberately not here, which keeps it robust against any parameter-map quirks.

## Address-map / semantic layer (`MU100AddressMap`)
Maps named parameters ↔ resolved (model, address) using the generated table:
- `resolve(param, instance)` fills address templates (`nn` part 0..31, `0n` A/D,
  `pp` perf, `3n`) for a given instance.
- `get(state, param, instance)` / `set(...)` read/write parameter values,
  decoding per encoding: **SINGLE** (1 byte), **NIBBLE** (MASTER TUNE, size 4),
  **BYTEPAIR** (effect TYPE/PARAMETER MSB+LSB, two 7-bit bytes; combined =
  `(b0<<7)|b1`, raw bytes also exposed).
- `identify(message)` maps a parameter-change message back to its param + instance.
- `makeParamChange(...)` builds the message that transmits a value to the device.
- Effect-type-*dependent* display scaling of byte-pair params is **not** here —
  that belongs with the Phase-4 effect screens, backed by `effects.json`.

Two-dimension template params (Drum Setup high=`3n` setup# + mid=`rr` note;
Internal Performance `3n`+`pp`) are resolved by passing both indices —
`resolve/get/set/makeParamChange(..., instance, instance2)` — where `instance`
is the finer (least-significant) templated byte and `instance2` the coarser; e.g.
`resolve(drumPan, note, setupNumber)`. `identify()` recovers both.

`MU100AddressMapData.h` is generated by `tools/gen_address_map.py` from
`data/parameters.json` (553 params baked in; only 2 skipped = the read-only
SYSTEM INFORMATION rows, which are device info, not editable). Defaults in
`parameters.json` were back-filled from a factory-reset dump by
`tools/backfill_defaults.py` (394/550; the factory dump supersedes the PDF's
floating Default column, which was shifted on effect rows).

## Validation
`sysex_test.cpp` (**6224 assertions, 0 failures**):
- nibblize/denibblize round-trip (incl. 0..0xFFFF sweep), checksum law,
  encoder bytes vs spec (XG System On, GM On, Identity, dump request, param change).
- For each fixture: message count, kind histogram, **all checksums valid**, and
  the decisive **byte-exact round-trip** — every message re-encodes to its
  original bytes, and the whole file reconstructs identically. Proves the encoder
  emits exactly what XGEdit and the MU100 firmware emit.
- State model: factory dump loads to 59,396 addresses; every original bulk byte
  recoverable; state survives `toBulkDumps`/`toParamChanges` → reload identically.
- Address map: known factory values decode correctly (MASTER VOLUME 7F, MASTER
  TUNE 0x0400 via nibble, EQ/PAN centers, PAN across part templates), `identify`
  recovers param+instance, set/get round-trips.
- Drum Setup 2-D addressing: `resolve(note, setup)` → correct `3n`/`rr`/low
  address, setup# lands in the high nibble, `identify` recovers note+setup,
  per-note set/get is independent.

## Not yet (Phase 4 — GUI, and effect value semantics)
- Effect-type-dependent value scaling / display (from `effects.json` + `tables.json`).
- A few MULTI PART low-offsets to confirm against the live emulator per screen
  (e.g. part VOLUME at `08/nn/0F` read `00` from the factory dump, which looked
  odd) — natural Phase-5 verification.
