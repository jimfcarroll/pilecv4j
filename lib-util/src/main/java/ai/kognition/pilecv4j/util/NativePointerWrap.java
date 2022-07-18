package ai.kognition.pilecv4j.util;

import com.sun.jna.Native;
import com.sun.jna.Pointer;

/**
 * This class is a simple helper class that will automatically free native memory
 * pointed to by a JNA Pointer on close. It's basically a JNA Pointer guard class.
 */
public class NativePointerWrap implements AutoCloseable {

    public final Pointer ptr;

    public NativePointerWrap(final Pointer ptr) {
        this.ptr = ptr;
    }

    @Override
    public void close() {
        if(ptr != null)
            Native.free(Pointer.nativeValue(ptr));
    }
}
