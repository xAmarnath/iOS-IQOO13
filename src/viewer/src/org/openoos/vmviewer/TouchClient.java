package org.openoos.vmviewer;

import java.io.*;
import java.net.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;

/** Separate ordered input channel; moves may coalesce but contact edges never do. */
final class TouchClient implements AutoCloseable {
    private static final class Event {
        final byte[] frame; final boolean edge;
        Event(byte[] frame, boolean edge) { this.frame=frame; this.edge=edge; }
    }
    private final ArrayDeque<Event> queue=new ArrayDeque<>();
    private final String token;
    private final int port;
    private boolean closing;
    private volatile boolean ready;
    private Socket socket;
    private byte[] latest=frame(new int[0]);
    private final Thread worker;
    TouchClient(String token,int port) {
        this.token=token; this.port=port;
        worker=new Thread(this::run,"guest-touch");worker.start();
    }
    static byte[] frame(int[] contacts) {
        if(contacts.length%3!=0 || contacts.length>30)throw new IllegalArgumentException();
        ByteBuffer b=ByteBuffer.allocate(136).order(ByteOrder.LITTLE_ENDIAN);
        b.putInt(0x31544347).putInt(1).putInt(1).putInt(contacts.length/3);
        for(int i:contacts)b.putInt(i);
        return b.array();
    }
    synchronized boolean offer(int[] contacts,boolean edge) {
        if(closing || !ready)return false;
        byte[] f=frame(contacts);
        if(!edge && !queue.isEmpty() && !queue.peekLast().edge)queue.removeLast();
        if(queue.size()>=64) {
            ready=false;queue.clear();latest=frame(new int[0]);
            try { if(socket!=null)socket.close(); }catch(IOException ignored){}
            return false;
        }
        latest=f;queue.addLast(new Event(f,edge));notifyAll();return true;
    }
    @Override public synchronized void close() {
        closing=true;ready=false;queue.clear();latest=frame(new int[0]);
        queue.add(new Event(latest,true));notifyAll();
    }
    private void send(Socket s,byte[] frame) throws IOException {
        s.getOutputStream().write(frame);s.getOutputStream().flush();
        byte[] ack=new byte[4];new DataInputStream(s.getInputStream()).readFully(ack);
        if(ByteBuffer.wrap(ack).order(ByteOrder.LITTLE_ENDIAN).getInt()!=1)
            throw new IOException("Guest touch was not acknowledged");
    }
    private void run() {
        for(;;) {
            synchronized(this){if(closing)return;}
            try(Socket s=new Socket()) {
                synchronized(this){socket=s;}
                s.connect(new InetSocketAddress("127.0.0.1",port),1500);
                s.setSoTimeout(3500);s.setTcpNoDelay(true);
                s.getOutputStream().write(token.getBytes(StandardCharsets.US_ASCII));
                send(s,frame(new int[0])); // Reconnection begins with a confirmed release.
                synchronized(this){if(!closing)ready=true;}
                for(;;) {
                    byte[] next;boolean last;
                    synchronized(this) {
                        if(queue.isEmpty())wait(200);
                        last=closing;
                        next=!queue.isEmpty()?queue.removeFirst().frame:latest;
                    }
                    send(s,next); // Heartbeat also prevents stationary fingers timing out.
                    if(last)return;
                }
            }catch(IOException e) {
                System.err.println("GuestTouch: "+e);
            }catch(InterruptedException e){Thread.currentThread().interrupt();return;}
            finally {synchronized(this){ready=false;socket=null;queue.clear();latest=frame(new int[0]);}}
            synchronized(this){if(closing)return;}
            try{Thread.sleep(500);}catch(InterruptedException e){Thread.currentThread().interrupt();return;}
        }
    }
}
