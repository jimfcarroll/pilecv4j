package ai.kognition.pilecv4j.gstreamer.guard;

import org.freedesktop.gstreamer.glib.RefCountedObject;

public class GstWrap<T extends RefCountedObject> implements AutoCloseable {
    public final T obj;
    private boolean disowned = false;

    public GstWrap(final T obj) {
        this.obj = obj;
    }

    public T disown() {
        disowned = true;
        return obj;
    }

    @Override
    public void close() {
        if(obj != null) {
            if(!disowned)
                obj.dispose();
            else
                obj.disown();
        }
    }
}
