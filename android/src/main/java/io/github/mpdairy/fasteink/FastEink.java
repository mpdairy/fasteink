package io.github.mpdairy.fasteink;

import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Point;
import android.graphics.Rect;
import android.os.Build;

/**
 * Whether this tablet takes fast, direct region updates, and the facts every {@link Session}
 * shares. The answer comes from the driver itself, never from a model name.
 */
public final class FastEink {
    static { System.loadLibrary("fasteink"); }
    private FastEink() { }

    private static native int[] nativeCapabilities(String firmware);
    private static native String nativeDescribe();

    /** {buffer width, buffer height, fast binary plane}; zeros and -1 when unsupported. */
    private static int[] capabilities;
    private static final int[] NONE = {0, 0, -1};
    private static volatile boolean enabled = true;

    /**
     * The app's kill switch, e.g. a "Standard Android drawing" setting. While disabled the
     * library reports no driver and makes no firmware calls, so every caller takes its Android
     * fallback.
     */
    public static void setEnabled(boolean value) { enabled = value; }
    public static boolean enabled() { return enabled; }

    private static synchronized int[] capabilities() {
        if (!enabled) return NONE;
        if (capabilities == null) {
            capabilities = nativeCapabilities(firmware());
            if (capabilities == null) capabilities = NONE;
        }
        return capabilities;
    }
    /** A known driver is present and enabled. */
    public static boolean supported() { return capabilities()[0] != 0; }
    /** {@link Mode#FAST_BINARY} works on this panel and firmware. */
    public static boolean supportsFastBinary() { int[] caps = capabilities(); return caps[0] != 0 && caps[2] >= 0; }
    /** The driver buffer in its own scan order; 0x0 when unsupported. Known buffers are in core/src/fasteink.c. */
    public static Point bufferSize() { int[] caps = capabilities(); return new Point(caps[0], caps[1]); }

    /**
     * The driver buffer's scan order from the natural portrait panel. Most buffers match the
     * panel. A landscape buffer stores panel (x,y) at (y, height-x), as observed in a Nomad's
     * shared plane and matching its firmware's hwrota=270.
     */
    public static Matrix bufferFromPanel() {
        Point size = bufferSize();
        Matrix result = new Matrix();
        if (size.x > size.y) { result.setRotate(-90); result.postTranslate(0, size.y); }
        return result;
    }

    /** The driver's raw info words as one line, for bug reports. Makes no display writes. */
    public static String describe() { return nativeDescribe(); }

    /** The installed firmware build, e.g. "Chauvet.E103.2606141001.2389_release". */
    static String firmware() { return Build.DISPLAY; }

    /**
     * Whether {@code area} of {@code bitmap} is all opaque black or white, as {@link Mode#FAST_BINARY}
     * requires. Decide from the whole patch, including its border, without flattening grays.
     */
    public static boolean opaqueBinary(Bitmap bitmap, Rect area) {
        int[] row = new int[area.width()];
        for (int y = area.top; y < area.bottom; y++) {
            bitmap.getPixels(row, 0, row.length, area.left, y, row.length, 1);
            for (int pixel : row) if (pixel != Color.BLACK && pixel != Color.WHITE) return false;
        }
        return true;
    }
}
