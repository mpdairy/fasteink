# fasteink

A small library for fast, direct region updates on e-ink tablets (Supernote
Manta and Nomad so far), so apps don't each reverse-engineer and copy the driver
interface. Extracted from [MonoPaint](https://github.com/mpdairy/monopaint),
which uses it for all of its fast e-ink drawing. MIT licensed.

```
android/   Android library (Java API + thin JNI shim), io.github.mpdairy.fasteink
core/      Plain C core: no JNI, no Android. Driver access, layouts, sessions
docs/      What the driver does on each tablet and firmware
scripts/   test_core.sh: desktop checks of the core against a fake driver
```

## Using it from an app

Add it as a submodule, then include its Android module:

```sh
git submodule add https://github.com/mpdairy/fasteink.git fasteink
```

```groovy
// settings.gradle
include ':fasteink'
project(':fasteink').projectDir = file('fasteink/android')
// root build.gradle needs: id 'com.android.library' version '8.7.3' apply false
```

and in the app module:

```groovy
dependencies { implementation project(':fasteink') }
```

```java
FastEink.setEnabled(!userWantsStandardDrawing); // no firmware calls at all when off
if (FastEink.supported()) {
    Session session = Session.forView(view, bitmap, bitmapToView, Mode.GRAY);
    // After drawing into bitmap:
    if (session.present(bitmap, dirty) == Session.BUSY) { /* keep dirty, retry on the next sample */ }
    session.close();
}
```

- `Mode.GRAY` is normal drawing. `Mode.FAST_BINARY` is for pen feedback and
  control presses in exact opaque black and white, when
  `FastEink.supportsFastBinary()`. Its pixels outlive Android redraws, so erase
  them through `FAST_BINARY` again (`Session.forPanel` with a white bitmap).
- Opening a session, or a failed update, throws `IllegalStateException`. Fall
  back to `View.invalidate()` then.
- `present` skips unchanged regions; `refresh` sends them anyway, to clear ghosts.
- `PanelCoordinates` maps view pixels to the panel's fixed portrait frame, and
  `FastEink.bufferFromPanel()` maps that to the driver's scan order.

## Building and testing

JDK 21, Android SDK 34, NDK 27.0.12077973, CMake 3.22.1:

```sh
ANDROID_HOME=$HOME/Android/Sdk ./gradlew :fasteink:assembleDebug
scripts/test_core.sh   # needs a desktop C compiler; uses ASan and UBSan
```

## Rules

- Identify the panel by the driver's own answer, never the model string (the
  Manta says "Nomad").
- Unknown layout means fall back. Never guess an ABI.
- Only the layout table in `core/src/fasteink.c` names a tablet.
- Never sleep on the pen thread; `BUSY` means try again on the next sample.
- Reject non-binary pixels before any write to the pen plane.
- Don't crop the region to the exact changed pixels (edge artifacts in MonoPaint 0.7).
- Make no firmware calls at all when disabled.

See [docs/DEVICES.md](docs/DEVICES.md) for the tablets and [DESIGN.md](DESIGN.md)
for where this is going.
