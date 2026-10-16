package io.github.mpdairy.fasteink;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Point;
import android.graphics.Rect;
import android.graphics.RectF;
import android.view.View;

/**
 * Owns a rectangle of the panel and presents a Bitmap's damaged regions to it directly, using
 * ordinary app permissions. Opening throws {@link IllegalStateException} when the fast path is
 * unavailable, and so do failed updates; callers then fall back to Android drawing.
 */
public final class Session implements AutoCloseable {
    static { System.loadLibrary("fasteink"); }
    private static native long nativeOpen(Bitmap background, int x, int y, int mode, String firmware);
    private static native int nativePresent(long handle, Bitmap bitmap, int left, int top,
                                            int right, int bottom, boolean force);
    private static native void nativeClose(long handle);
    private static native int nativeReadGray(long handle, int x, int y);
    private static native void nativeRotate(Bitmap source,Bitmap target,int quarter,int left,int top,int right,int bottom);

    /** {@link #present} results. */
    public static final int PRESENTED = 1, UNCHANGED = 0,
            /** The display queue is busy. Keep the damage and retry on the next sample; never sleep on the pen thread. */
            BUSY = -1;

    private long handle;
    private final int x, y;
    private Matrix sourceToBuffer;
    private Bitmap buffer;
    private Canvas bufferCanvas;
    private final RectF mapped = new RectF();
    private final Rect bufferDirty = new Rect();
    private int sourceWidth, sourceHeight;
    private int bufferTurn=-1;

    /** A session for {@code background}, drawn into {@code owner} through {@code bitmapToView}. */
    public static Session forView(View owner, Bitmap background, Matrix bitmapToView, Mode mode) {
        Matrix toPanel = new Matrix(bitmapToView);
        toPanel.postConcat(PanelCoordinates.fromView(owner));
        return forPanel(background,toPanel,mode);
    }
    /** A session for pixels placed by panel coordinates, e.g. after their view has closed. */
    public static Session forPanel(Bitmap background, Matrix bitmapToPanel, Mode mode) {
        Matrix toBuffer = new Matrix(bitmapToPanel);
        toBuffer.postConcat(FastEink.bufferFromPanel());
        return new Session(background,toBuffer,mode);
    }
    private Session(Bitmap background, Matrix toPanel, Mode mode) {
        if (!FastEink.enabled()) throw new IllegalStateException("Fast e-ink is disabled");
        float[] values = new float[9]; toPanel.getValues(values);
        for (float value : values) if (!Float.isFinite(value)) throw new IllegalStateException("Invalid panel transform");
        // A rotated control must still own a rectangular patch, never surrounding UI pixels.
        if (!toPanel.rectStaysRect()) throw new IllegalStateException("Non-rectangular panel transform");
        RectF bounds = new RectF(0,0,background.getWidth(),background.getHeight());
        toPanel.mapRect(bounds);
        Rect pixels = new Rect(); bounds.roundOut(pixels);
        Point size = FastEink.bufferSize();
        if (pixels.isEmpty() || pixels.width()>size.x || pixels.height()>size.y)
            throw new IllegalStateException("Panel patch outside supported display");
        x=pixels.left; y=pixels.top;
        sourceWidth=background.getWidth(); sourceHeight=background.getHeight();
        sourceToBuffer=new Matrix(toPanel); sourceToBuffer.postTranslate(-x,-y);
        // Page and panel orientation cancel. Normal landscape pages take this zero-copy path.
        if (sourceToBuffer.isIdentity()) {
            sourceToBuffer=null;
            handle=nativeOpen(background,x,y,mode.nativeValue(),FastEink.firmware());
            return;
        }
        buffer=Bitmap.createBitmap(pixels.width(),pixels.height(),Bitmap.Config.ARGB_8888);
        bufferCanvas=new Canvas(buffer);
        bufferCanvas.drawColor(Color.WHITE);
        bufferTurn=quarterTurn(sourceToBuffer,sourceWidth,sourceHeight);
        copyToBuffer(background,new Rect(0,0,buffer.getWidth(),buffer.getHeight()));
        try { handle=nativeOpen(buffer,x,y,mode.nativeValue(),FastEink.firmware()); }
        catch (RuntimeException | LinkageError error) { buffer.recycle(); throw error; }
    }

    /** Presents {@code dirty} of {@code bitmap}, unless its pixels are unchanged; {@link #PRESENTED}, {@link #UNCHANGED} or {@link #BUSY}. */
    public synchronized int present(Bitmap bitmap, Rect dirty) {
        return present(bitmap, dirty, false);
    }
    /** Presents even unchanged pixels: the physical panel may retain fast-ink ghosts. */
    public synchronized int refresh(Bitmap bitmap, Rect dirty) {
        return present(bitmap, dirty, true);
    }
    private int present(Bitmap bitmap, Rect dirty, boolean force) {
        if (handle==0) return BUSY;
        if (buffer != null) {
            if (bitmap.getWidth()!=sourceWidth || bitmap.getHeight()!=sourceHeight)
                throw new IllegalStateException("Display source dimensions changed");
            mapped.set(dirty); sourceToBuffer.mapRect(mapped); mapped.roundOut(bufferDirty);
            if (!bufferDirty.intersect(0,0,buffer.getWidth(),buffer.getHeight())) return UNCHANGED;
            copyToBuffer(bitmap,bufferDirty);
            return nativePresent(handle,buffer,bufferDirty.left,bufferDirty.top,bufferDirty.right,bufferDirty.bottom,force);
        }
        return nativePresent(handle,bitmap,dirty.left,dirty.top,dirty.right,dirty.bottom,force);
    }
    private void copyToBuffer(Bitmap source,Rect dirty) {
        // Control captures can have transparent corners; retain Canvas's blending
        // for those. Document/viewport rasters explicitly contain opaque pixels.
        if(bufferTurn>=0 && !source.hasAlpha())nativeRotate(source,buffer,bufferTurn,dirty.left,dirty.top,dirty.right,dirty.bottom);
        else {
            int save=bufferCanvas.save();bufferCanvas.clipRect(dirty);
            bufferCanvas.drawBitmap(source,sourceToBuffer,null);bufferCanvas.restoreToCount(save);
        }
    }
    private static int quarterTurn(Matrix transform,int width,int height) {
        float[] actual=new float[9],expected=new float[9];transform.getValues(actual);
        for(int turn=0;turn<4;turn++) {
            Matrix rotation=new Matrix();rotation.setRotate(turn*90);
            if(turn==1)rotation.postTranslate(height,0);
            else if(turn==2)rotation.postTranslate(width,height);
            else if(turn==3)rotation.postTranslate(0,width);
            rotation.getValues(expected);boolean matches=true;
            for(int i=0;i<9;i++)if(Math.abs(actual[i]-expected[i])>.00001f)matches=false;
            if(matches)return turn;
        }
        return -1;
    }
    @Override public synchronized void close() {
        if (handle==0) return;
        nativeClose(handle); handle=0;
        if (buffer != null) { buffer.recycle(); buffer=null; bufferCanvas=null; }
    }
    /** The 4-bit gray the session's plane holds under a source pixel, for device checks. */
    public synchronized int readGray(int localX, int localY) {
        if (sourceToBuffer != null) {
            float[] point={localX+.5f,localY+.5f}; sourceToBuffer.mapPoints(point);
            return nativeReadGray(handle,x+(int)Math.floor(point[0]),y+(int)Math.floor(point[1]));
        }
        return nativeReadGray(handle,x+localX,y+localY);
    }
    /** A source rectangle in the driver buffer's coordinates, which are not the portrait panel's on a landscape buffer. */
    public Rect bufferRegion(Rect source) {
        RectF area=new RectF(source);
        if (sourceToBuffer!=null) sourceToBuffer.mapRect(area);
        Rect result=new Rect(); area.roundOut(result); result.offset(x,y); return result;
    }
}
