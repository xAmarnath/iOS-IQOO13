package org.openoos.vmviewer;

import android.app.Activity;
import android.os.Bundle;
import android.opengl.GLSurfaceView;
import android.view.*;
import android.graphics.RectF;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;
import java.util.HashSet;
import java.util.ArrayList;

/** Native latest-frame presentation. Touch uses the existing ordered channel. */
public final class StreamActivity extends Activity {
    static { System.loadLibrary("gueststream"); }
    private static native long nativeCreate(String endpoint);
    private static native void nativeSurface(long handle);
    private static native void nativeSize(long handle,int width,int height);
    private static native void nativeDraw(long handle);
    private static native void nativeClose(long handle);
    private long handle;
    private Screen screen;
    private TouchClient touch;
    private String token;
    private int port;
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        token=getIntent().getStringExtra("token");
        String endpoint=getIntent().getStringExtra("endpoint");
        port=getIntent().getIntExtra("port",27183);
        if(token==null || !token.matches("[0-9a-f]{64}") || endpoint==null || !endpoint.matches("[a-z0-9-]{16,90}")) {finish();return;}
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().getWindowInsetsController().hide(WindowInsets.Type.systemBars());
        getWindow().getDecorView().getWindowInsetsController().setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        Display display=getDisplay();
        if(display!=null) {
            Display.Mode current=display.getMode(),best=current;
            for(Display.Mode mode:display.getSupportedModes())
                if(mode.getPhysicalWidth()==current.getPhysicalWidth() && mode.getPhysicalHeight()==current.getPhysicalHeight()
                    && mode.getRefreshRate()<=120.5f && mode.getRefreshRate()>best.getRefreshRate())best=mode;
            WindowManager.LayoutParams p=getWindow().getAttributes();p.preferredDisplayModeId=best.getModeId();getWindow().setAttributes(p);
        }
        handle=nativeCreate(endpoint);
        if(handle==0){finish();return;}
        screen=new Screen();setContentView(screen);
    }
    @Override public void onResume(){super.onResume();if(screen!=null){screen.onResume();touch=new TouchClient(token,port+1);}}
    @Override public void onPause(){if(touch!=null){touch.close();touch=null;}if(screen!=null){screen.tracked.clear();screen.onPause();}super.onPause();}
    @Override public void onDestroy(){if(handle!=0){nativeClose(handle);handle=0;}super.onDestroy();}
    private final class Screen extends GLSurfaceView implements GLSurfaceView.Renderer {
        final HashSet<Integer> tracked=new HashSet<>();
        Screen(){super(StreamActivity.this);setContentDescription("iPadOS guest display");setEGLContextClientVersion(2);setPreserveEGLContextOnPause(true);setRenderer(this);}
        public void onSurfaceCreated(GL10 gl,EGLConfig config){nativeSurface(handle);}
        public void onSurfaceChanged(GL10 gl,int w,int h){nativeSize(handle,w,h);}
        public void onDrawFrame(GL10 gl){nativeDraw(handle);}
        private RectF displayRect(){float scale=Math.min(getWidth()/1536f,getHeight()/2048f),w=1536*scale,h=2048*scale;return new RectF((getWidth()-w)/2,(getHeight()-h)/2,(getWidth()+w)/2,(getHeight()+h)/2);}
        @Override public boolean onTouchEvent(MotionEvent e){
            int action=e.getActionMasked(),index=e.getActionIndex();RectF r=displayRect();
            if(action==MotionEvent.ACTION_DOWN){tracked.clear();if(!r.contains(e.getX(),e.getY())||touch==null)return false;}
            if(action==MotionEvent.ACTION_DOWN || action==MotionEvent.ACTION_POINTER_DOWN)
                if(r.contains(e.getX(index),e.getY(index)) && tracked.size()<10)tracked.add(e.getPointerId(index));
            if(action==MotionEvent.ACTION_UP || action==MotionEvent.ACTION_POINTER_UP)tracked.remove(e.getPointerId(index));
            if(action==MotionEvent.ACTION_CANCEL)tracked.clear();
            ArrayList<Integer> values=new ArrayList<>();
            for(int i=0;i<e.getPointerCount();++i)if(tracked.contains(e.getPointerId(i))){
                values.add(e.getPointerId(i));values.add(Math.round(Math.max(0,Math.min(1,(e.getX(i)-r.left)/r.width()))*32767));
                values.add(Math.round(Math.max(0,Math.min(1,(e.getY(i)-r.top)/r.height()))*32767));
            }
            int[] contacts=new int[values.size()];for(int i=0;i<contacts.length;++i)contacts[i]=values.get(i);
            if(touch==null || !touch.offer(contacts,action!=MotionEvent.ACTION_MOVE))tracked.clear();
            if(action==MotionEvent.ACTION_UP)performClick();return true;
        }
        @Override public boolean performClick(){super.performClick();return true;}
    }
}
