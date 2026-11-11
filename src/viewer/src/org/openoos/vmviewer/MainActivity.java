package org.openoos.vmviewer;

import android.app.Activity;
import android.os.Bundle;
import android.graphics.*;
import android.view.*;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.ArrayList;

/** Displays the VM's published scanout, with no invented guest graphics. */
public final class MainActivity extends Activity {
    private volatile FrameSession session;
    private TouchClient touch;
    private Screen screen;
    private String token;
    private int port;
    private long generation;
    private final Object imageLock = new Object();
    private Bitmap bitmap;
    private String message = "Waiting for guest display";

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().getWindowInsetsController().hide(WindowInsets.Type.systemBars());
        getWindow().getDecorView().getWindowInsetsController().setSystemBarsBehavior(
            WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        token = getIntent().getStringExtra("token");
        port = getIntent().getIntExtra("port", 27183);
        if (token != null && token.matches("[0-9a-f]{64}"))
            getPreferences(0).edit().putString("token", token).putInt("port", port).apply();
        else {
            token = getPreferences(0).getString("token", "");
            port = getPreferences(0).getInt("port", 27183);
        }
        screen = new Screen();
        screen.setContentDescription("iPadOS guest display");
        setContentView(screen);
    }

    @Override public void onResume() {
        super.onResume();
        FrameSession next=new FrameSession();
        session=next;next.reader.start();
        touch=new TouchClient(token,port+1);
    }

    @Override public void onPause() {
        FrameSession previous;
        synchronized(imageLock){previous=session;session=null;}
        if(previous!=null)previous.close();
        if(touch!=null){touch.close();touch=null;}
        screen.tracked.clear();
        super.onPause();
    }

    private final class FrameSession {
        private Socket socket;
        private boolean closed;
        final Thread reader=new Thread(()->readFrames(this),"guest-scanout");
        synchronized boolean attach(Socket s) throws IOException {
            if(closed){s.close();return false;}socket=s;return true;
        }
        synchronized void detach(Socket s){if(socket==s)socket=null;}
        synchronized void close(){
            closed=true;reader.interrupt();
            try{if(socket!=null)socket.close();}catch(IOException ignored){}
        }
    }
    private void readFrames(FrameSession self) {
        byte[] raw = new byte[1536 * 2048 * 4];
        int[] pixels = new int[1536 * 2048];
        while (session == self && !Thread.currentThread().isInterrupted()) {
            long delay = 50;
            long started = android.os.SystemClock.elapsedRealtime();
            try (Socket s = new Socket()) {
                if(!self.attach(s))break;
                s.connect(new InetSocketAddress("127.0.0.1", port), 1500);
                s.setSoTimeout(4000);
                DataOutputStream out = new DataOutputStream(s.getOutputStream());
                out.write(token.getBytes(StandardCharsets.US_ASCII));
                out.writeLong(generation);
                out.flush();
                DataInputStream in = new DataInputStream(new BufferedInputStream(s.getInputStream(), 65536));
                if (in.readInt() != 0x46524d31) throw new IOException("Invalid display stream");
                int width = in.readInt(), height = in.readInt(), bytes = in.readInt();
                long next = in.readLong();
                if (width != 1536 || height != 2048 || (bytes != 0 && bytes != raw.length))
                    throw new IOException("Invalid frame size");
                if (bytes != 0) {
                    in.readFully(raw);
                    for (int i = 0, j = 0; i < pixels.length; ++i, j += 4)
                        pixels[i] = 0xff000000 | ((raw[j+2] & 255) << 16) | ((raw[j+1] & 255) << 8) | (raw[j] & 255);
                    synchronized (imageLock) {
                        if (session != self) break;
                        if (bitmap == null) bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
                        bitmap.setPixels(pixels, 0, width, 0, 0, width, height);
                        message = null;
                        generation = next;
                    }
                    android.util.Log.i("GuestScanout", "frame=" + generation + " size=" + width + "x" + height
                            + " receive_convert_ms=" + (android.os.SystemClock.elapsedRealtime() - started));
                } else synchronized (imageLock) { if (session==self && bitmap != null) message = null; }
                screen.postInvalidate();
                self.detach(s);
            } catch (IOException | RuntimeException e) {
                delay = 750;
                if (session == self) {
                    synchronized (imageLock) { if(session==self)message = "Guest display disconnected"; }
                    screen.postInvalidate();
                    android.util.Log.w("GuestScanout", e.toString());
                }
            }
            try { Thread.sleep(delay); } catch (InterruptedException e) { break; }
        }
    }

    private final class Screen extends View {
        private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
        final HashSet<Integer> tracked=new HashSet<>();
        Screen() { super(MainActivity.this); }
        private RectF displayRect(){
            float scale=Math.min(getWidth()/1536f,getHeight()/2048f);
            float w=1536*scale,h=2048*scale;
            return new RectF((getWidth()-w)/2,(getHeight()-h)/2,(getWidth()+w)/2,(getHeight()+h)/2);
        }
        @Override public boolean onTouchEvent(MotionEvent e){
            int action=e.getActionMasked(),index=e.getActionIndex();
            RectF r=displayRect();
            if(action==MotionEvent.ACTION_DOWN) {
                tracked.clear();
                if(!r.contains(e.getX(),e.getY()) || touch==null)return false;
            }
            if(action==MotionEvent.ACTION_DOWN || action==MotionEvent.ACTION_POINTER_DOWN){
                if(r.contains(e.getX(index),e.getY(index)) && tracked.size()<10)
                    tracked.add(e.getPointerId(index));
            }
            if(action==MotionEvent.ACTION_UP || action==MotionEvent.ACTION_POINTER_UP)
                tracked.remove(e.getPointerId(index));
            if(action==MotionEvent.ACTION_CANCEL)tracked.clear();
            ArrayList<Integer> values=new ArrayList<>();
            for(int i=0;i<e.getPointerCount();++i)if(tracked.contains(e.getPointerId(i))){
                values.add(e.getPointerId(i));
                values.add(Math.round(Math.max(0,Math.min(1,(e.getX(i)-r.left)/r.width()))*32767));
                values.add(Math.round(Math.max(0,Math.min(1,(e.getY(i)-r.top)/r.height()))*32767));
            }
            int[] contacts=new int[values.size()];
            for(int i=0;i<contacts.length;++i)contacts[i]=values.get(i);
            if(touch==null || !touch.offer(contacts,action!=MotionEvent.ACTION_MOVE))tracked.clear();
            if(action==MotionEvent.ACTION_UP)performClick();
            return true;
        }
        @Override public boolean performClick(){super.performClick();return true;}
        @Override protected void onDraw(Canvas c) {
            c.drawColor(Color.BLACK);
            synchronized (imageLock) {
                if (bitmap != null) {
                    c.drawBitmap(bitmap, null, displayRect(), paint);
                }
                if (message != null) {
                    paint.setColor(Color.WHITE);
                    paint.setTextAlign(Paint.Align.CENTER);
                    paint.setTextSize(18 * getResources().getDisplayMetrics().scaledDensity);
                    c.drawText(message, getWidth()/2f, getHeight()/2f, paint);
                }
            }
        }
    }
}
