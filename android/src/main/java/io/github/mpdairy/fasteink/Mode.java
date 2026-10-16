package io.github.mpdairy.fasteink;

/** What a {@link Session}'s updates are for. Each maps to the driver's own modes. */
public enum Mode {
    /** Normal gray drawing (Supernote mode 7). */
    GRAY,
    /**
     * Pen feedback and control presses in exact opaque black and white (Supernote mode 9),
     * when {@link FastEink#supportsFastBinary()}. Its pixels persist through Android redraws
     * and gray refreshes, so whoever draws here must erase them through this mode again.
     */
    FAST_BINARY,
    /** Diagnostic only: mode 7 with request flags 1, as some installed display clients send it. */
    GRAY_FLAGGED;

    // Matches fe_mode in core/include/fasteink.h.
    int nativeValue() { return ordinal(); }
}
