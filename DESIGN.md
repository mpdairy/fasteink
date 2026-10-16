# fasteink: design brainstorm

Status: first version extracted from MonoPaint on 2026-10-15; MonoPaint now
uses it for all fast e-ink drawing. Written as a brainstorm on 2026-10-08.

What exists so far, against the sketch below: the C core (`core/`) with a
replaceable driver and desktop tests, the JNI shim, and the Java API
`FastEink` (capabilities, kill switch, scan order), `Session`, `Mode`
(`GRAY`, `FAST_BINARY`, intent names rather than driver numbers) and
`PanelCoordinates`. The firmware-dependent pen plane moved from MonoPaint's
`Device` into the layout table. Not yet: `Presenter`, session footprints and
`erase()`, `DirtyRegions` and `FrameGate` helpers, the probe app, a published
artifact. `present` returns `Session.PRESENTED`/`UNCHANGED`/`BUSY` and throws
for rejected pixels, as MonoPaint's callers expect, rather than a `Result` enum.

## Why

Fast region updates on Supernote panels (`/dev/ebc`, the HT driver ioctls) took
a lot of reverse engineering in MonoPaint. Now every app that wants them copies it:

- `monopaint/app/src/main/cpp/direct_eink.c` + `DirectEink.java`: current. It
  has the layout table, Nomad support, mode 9 pen plane, scan-order rotation,
  `readGray`, the Android-drawing kill switch.
- `dopecomposer/.../staffseq/DirectEink.java` + `direct_eink.c`: a fork from
  before the Nomad work. Manta only, no layout table, no mode 9, no `force`.
  It will break on a Nomad and nobody will notice until someone tries.

The knowledge that changes over time (which buffers exist, which firmware was
tested, what each mode really does, what goes wrong) should live in one place
that apps depend on.

## What can be abstracted (and what can't)

Shared between apps:

1. **Finding the driver.** Probe `/dev/ebc`, match the buffer against a table of
   known layouts, report capabilities. Never guess from `Build.MODEL` (the Manta
   says "Nomad").
2. **Panel geometry.** View → screen → panel → driver buffer transforms,
   including the Nomad's landscape scan order (hwrota=270) and app rotation.
   Today: `PanelCoordinates` + `DirectEink.bufferFromPanel()` + the quarter-turn
   copy path.
3. **Sessions.** Own a rectangle of the panel, present dirty regions from a
   Bitmap, skip unchanged pixels, force refresh for ghosts, handle EAGAIN
   backpressure without sleeping.
4. **Planes and their rules.** The gray plane (mode 7) and the binary pen plane
   (mode 9, flags 1, offset = plane size). Mode 9 accepts only opaque black/white,
   and its pixels persist across Android redraws, so whoever draws there must clean
   up. MonoPaint learned that from the Layers panel press boxes in 0.96.
5. **Fallback.** No driver, unknown layout, rejected pixels or a user kill switch:
   the caller always gets a clear "do it the Android way" answer.
6. **Device and firmware facts.** The layout table and tested firmware builds,
   kept up to date in one place.

Stays in apps:

- What to draw: brushes, dot shading, dithering policy, UI.
- When to switch planes (e.g. MonoPaint's preview-in-mode-9 then repaint in mode 7).
  The library can offer this as a helper later, but apps decide.
- Input. `NativePen` (the firmware pen bridge, adapted from AnimInk) is a
  separate, riskier thing: reflection into firmware classes. Possibly a second
  optional module later, not part of the core.

## Layers

```
┌───────────────────────────────────────────────┐
│ fasteink-android (Java, AAR)                  │  apps use this
│  FastEink, Capabilities, Surface, Session     │
│  geometry (view/panel/buffer), fallback       │
├───────────────────────────────────────────────┤
│ JNI shim (thin, no logic)                     │
├───────────────────────────────────────────────┤
│ libfasteink (plain C, no JNI, no Android)     │  NDK/Rust/C apps can use
│  backend table: supernote_ht, (later others)  │  this directly
│  layouts, open/present/close, diff, planes    │
└───────────────────────────────────────────────┘
```

Keeping the C core free of JNI means it can be unit-tested on a desktop against
a fake driver (a memory buffer + a recorded ioctl log), and non-Java apps can
link it.

## API sketch (Java)

Names describe intent, not raw driver numbers. `7`/`9` never show up in app code.

```java
// Once per process. Never throws; unsupported devices get Capabilities.NONE.
Capabilities caps = FastEink.capabilities(context);
caps.supported();          // any fast path at all
caps.planes();             // {GRAY} on Manta, {GRAY, BINARY} on Nomad
caps.bufferSize();         // driver buffer, e.g. 1872x1404
caps.firmwareTested();     // (layout, Build.DISPLAY) is in the tested table
caps.describe();           // one line for bug reports / settings screens

FastEink.setEnabled(false);  // app-level kill switch ("Standard Android" setting)

// A session owns a rectangle of the panel, given how a Bitmap maps onto a View.
Session s = FastEink.open(view, bitmap, bitmapToView, Plane.GRAY);
Session s = FastEink.openPanel(bitmap, bitmapToPanel, Plane.BINARY); // view already gone

Result r = s.present(bitmap, dirtyRect);   // PRESENTED, UNCHANGED, BUSY, REJECTED, UNSUPPORTED
s.refresh(bitmap, dirtyRect);              // force, to clear ghosts
s.close();
```

`Result` instead of exceptions for the normal "can't do it fast" cases, so the
caller pattern is:

```java
if (s.present(bmp, dirty) != Result.PRESENTED && r != Result.UNCHANGED) view.invalidate(dirty);
```

or a helper that does exactly that:

```java
Presenter p = Presenter.forView(view, bmp, matrix, Plane.GRAY); // falls back to invalidate()
p.present(dirty);
```

### Binary plane cleanup as a first-class thing

Because BINARY pixels outlive Android redraws, the session should record the
union of what it drew and offer:

```java
s.footprint();   // panel area this session ever drew on
s.erase();       // white over the footprint, through the same plane
```

MonoPaint does this by hand in `SelectionFeedback` + `DrawingPad`. In the library
an app can't forget it.

### Helpers worth including (opt-in)

- `DirtyRegions`: coalesce damage into patches (MonoPaint has one).
- `Binary.isOpaqueBinary(bitmap, rect)`: check before choosing BINARY
  (MonoPaint's `SelectionFeedback.opaqueBinary`).
- `FrameGate`: one present per display frame (MonoPaint's shape-preview
  coalescing).

## C core sketch

```c
typedef struct fe_caps { int width, height, stride; unsigned planes; const char *backend; } fe_caps;
typedef struct fe_session fe_session;

int  fe_probe(fe_caps *out);                                 // 0 = supported
int  fe_open(fe_session **out, int plane, int x, int y,
             int w, int h, const uint8_t *gray4, size_t pitch); // seeds "previous"
int  fe_present(fe_session *, const uint8_t *gray4, size_t pitch,
                int l, int t, int r, int b, int force);        // FE_PRESENTED/UNCHANGED/BUSY/REJECTED
void fe_close(fe_session *);
```

The core takes gray pixels, not Android Bitmaps; the JNI shim locks the Bitmap
and passes a pointer. One open question is whether the core takes RGBA (like
today, `src[0]>>4`) or 8-bit gray. RGBA avoids a copy on the hot path.

## Backends

Start with one: `supernote_ht` (`/dev/ebc`, `0x48545201` info, `0x48545701`
region update, the 24-byte `Update` struct, the `LAYOUTS` table).

### Other tablets

From general knowledge, not tested on hardware. Other e-ink devices get fast
updates in roughly four ways:

| Family | How fast updates work | Fit |
| --- | --- | --- |
| Supernote | An ordinary app can open `/dev/ebc`, write the shared plane and ioctl a region | What we have. Unusually open |
| Onyx Boox (Android) | Official Onyx SDK: `EpdController` update modes (A2/DU/GC16) on a View, plus `TouchHelper` for raw pen drawing | Good backend candidate. Vendor-supported, documented, more stable than ioctls |
| Other Android e-ink (Bigme, Meebook, Hisense, ...) | Usually hidden framework methods on View or a system "eink manager", sometimes Rockchip EBC underneath | Device by device. Probably reflection, which breaks easily |
| Linux e-readers (Kobo, Kindle, reMarkable, PocketBook) | Framebuffer ioctls (`MXCFB_SEND_UPDATE` with a waveform mode); reMarkable 2 does it in software | Already covered by [FBInk](https://github.com/NiLuJe/FBInk). Don't duplicate it. Our Java layer doesn't apply |

Many of these tablets use Rockchip chips with an EBC driver, but vendors change
the driver interface and SELinux usually blocks apps from the device node.
Supernote's open `/dev/ebc` is the exception, not the rule. Whether any other
Rockchip tablet takes the same `HT` ioctls is unknown; the probe app would tell us.

**What this means for the API:** "plane" is a Supernote word. The shared
contract should be about intent, so every backend can map it:

| Intent | Supernote | Boox | Linux mxcfb |
| --- | --- | --- | --- |
| `FAST_BINARY`: pen feedback, UI presses | mode 9 pen plane | A2 | A2 / DU |
| `GRAY`: normal drawing | mode 7 | GC16 / regal | GC16 / GL16 |
| `CLEAN`: clear ghosts | forced mode 7 refresh | full GC16 | full flashing update |

So `Plane.GRAY` / `Plane.BINARY` in the sketch above should become an `Update`
or `Intent` enum, and "BINARY pixels persist and need erasing" becomes a
capability flag (`caps.binaryPersists()`), not an assumption.

The likely second backend is Boox: biggest market, official SDK, nothing to
reverse-engineer. Build it when someone has a Boox to test on.

## Keeping it up to date

The point of the repo. Most changes will be data, not code:

- `layouts`: buffer size, stride, plane size, scan order, which planes exist.
  Added only from observations on the device, with date and how it was observed.
- `tested firmware`: (layout, `Build.DISPLAY`) pairs the device checks passed on.
- `docs/DEVICES.md`, `docs/PLANES.md`: what each mode does, known problems
  (mode 7 queues frames on the Nomad, mode 9 persists, shared planes aren't a
  record of what was shown, tight cropping caused edge artifacts in 0.7...).
  Move MonoPaint's DIRECT_GRAY.md history here.

Versioning: semver on the API. A new layout or tested firmware is a patch
release, so apps can pick it up with a version bump and nothing else.

A small **probe app** in the repo: dumps the info ioctl, the match result,
firmware, and runs a visible test pattern on each plane. Someone with a new
device or firmware runs it and pastes the output into an issue. That is how the
table grows without us owning every tablet.

## Distribution

Now (2026-10-15): its own repository, used as a git submodule at `fasteink/`
by MonoPaint, Dope Composer and inkbreach, each including `android/` as a
`:fasteink` module. Later options, roughly in order of effort:

1. Gradle composite build / `includeBuild("../fasteink")`. Fine for our own apps
   in this folder, no publishing.
2. JitPack from a GitHub tag. Easy for outsiders.
3. Maven Central. Only if people actually use it.

Prebuilt `.so` for arm64-v8a (and armeabi-v7a?) inside the AAR.

## Rules to carry over from MonoPaint

- Identify by the driver's own answer, never the model string.
- Unknown layout means fall back. Never guess an ABI.
- No app code names a tablet. Only the layout table does.
- Never sleep on the pen thread; BUSY means try on the next sample.
- Reject non-binary pixels before any write to the pen plane.
- Don't crop the region to the exact changed pixels (edge artifacts in 0.7).
- The library makes no firmware calls at all when disabled.

## Open questions

- Name. `fasteink`? Something Supernote-specific since that's all it supports
  for now?
- Java only, or also a Kotlin-friendly surface? (MonoPaint and dopecomposer are Java.)
- Does `NativePen` belong here as an optional `fasteink-pen` module, or stay in
  apps? It's the other half of "fast drawing" but has very different risk.
- Licensing: MIT like MonoPaint. The ioctl struct is a device interface
  declaration, not vendor code; keep saying so in the source. AnimInk notice
  only matters if the pen module moves here.
- Test strategy: desktop tests of the C core against a fake driver, plus device
  checks in the probe app run on a Manta and a Nomad before each release.
- Migration: MonoPaint's `DirectEink` becomes a thin wrapper over the library
  first, then goes away. dopecomposer drops its fork and gains Nomad support for
  free.
