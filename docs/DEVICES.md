# Tablets and planes

Everything here was observed on the devices. The data lives in `LAYOUTS` in
`core/src/fasteink.c`; this page says why. MonoPaint's `docs/DIRECT_GRAY.md`
keeps the full experiment history.

## Driver

`/dev/ebc`, opened with ordinary app permissions.

- `0x48545201` fills 32 info words: `info[1]` is `width | height << 16`,
  `info[2]` the stride, `info[3]` one plane's size.
- The device maps three planes plus a header, `(size * 3 + 95) & ~63` bytes.
- `0x48545701` submits a 24-byte region update: left, top, right, bottom,
  plane offset, mode, flags. `EAGAIN`/`EINTR` mean the queue is busy.

The struct and request numbers are device interface declarations, confirmed
against Atelier's region update, not vendor implementation code.

## Layouts

| Tablet | Buffer | Stride | Plane size | Scan order |
| --- | --- | --- | --- | --- |
| Manta | 1920×2560 | 1920 | 4915200 | Portrait, matches the panel |
| Nomad | 1872×1404 | 1872 | 2628288 | Landscape (firmware hwrota=270): panel (x,y) at (y, 1404−x), observed 2026-10-05 |

The Manta reports its model as "Supernote Nomad", so only the buffer identifies it.

## Modes

| `Mode` | Driver | What it does |
| --- | --- | --- |
| `GRAY` | mode 7, flags 0, plane 0 | Gray drawing. Frames queue behind the pen on the Nomad, and on the Manta from 3.30.45 |
| `FAST_BINARY` | mode 9, flags 1 | The firmware's binary pen path. Opaque black/white only. Pixels persist through Android redraws and gray refreshes |
| `GRAY_FLAGGED` | mode 7, flags 1 | Diagnostic; seen in installed display clients |

Which plane `FAST_BINARY` uses depends on the firmware build stamp (the
`yyMMddHHmm` in `Build.DISPLAY`):

| | Manta | Nomad |
| --- | --- | --- |
| Before 2610091001 | unsupported (stays gray) | plane 1 |
| From 2610091001 (3.30.45) | plane 0 (confirmed by hand 2026-10-12) | plane 0 |

From 3.30.45 the firmware's `libeinkutils.so` sends its own mode 9 requests to
the main plane. On the Nomad, plane 1 then leaves black boxes where closed
panels were: the kernel records a pen-write rectangle and the next full UI
update fills it from Android's black framebuffer.

## Known problems

- The shared planes are scratch the compositor also writes. Reading them back
  is not a record of what the panel shows.
- Tight cropping of regions to the changed pixels coincided with edge artifacts.

## Adding a tablet

Add a `LAYOUTS` row with the stride and size the info query reports, and the
plane fast binary updates use on each firmware, each observed on the device.
Say in a comment how and when. Then add a row above.
