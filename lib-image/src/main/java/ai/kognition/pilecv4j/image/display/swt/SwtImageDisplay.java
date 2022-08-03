/*
 * Copyright 2022 Jim Carroll
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package ai.kognition.pilecv4j.image.display.swt;

import static net.dempsy.util.Functional.chain;

import java.util.function.Function;

import org.eclipse.swt.SWT;
import org.eclipse.swt.layout.GridLayout;
import org.eclipse.swt.widgets.Display;
import org.eclipse.swt.widgets.Shell;
import org.opencv.core.Mat;

import ai.kognition.pilecv4j.image.display.ImageDisplay;

public class SwtImageDisplay extends ImageDisplay {

    public static enum CanvasType {
        SCROLLABLE, RESIZABLE
    }

    private final String name;

    private Display display = null;
    private Shell shell = null;

    private SwtCanvasImageDisplay canvasWriter = null;

    private boolean setupCalled = false;

    private final Function<Shell, SwtCanvasImageDisplay> canvasHandlerMaker;

    public SwtImageDisplay(final Mat mat, final String name, final Runnable closeCallback, final KeyPressCallback kpCallback,
        final SelectCallback selectCallback, final CanvasType canvasType) {
        this.name = name;
        switch(canvasType) {
            case SCROLLABLE:
                canvasHandlerMaker = s -> new ScrollableSwtCanvasImageDisplay(shell, closeCallback, kpCallback, selectCallback);
                break;
            case RESIZABLE:
                canvasHandlerMaker = s -> new ResizableSwtCanvasImageDisplay(shell, closeCallback, kpCallback, selectCallback);
                break;
            default:
                throw new IllegalArgumentException("Cannot create an swt canvas of type " + canvasType);
        }
        if(mat != null)
            update(mat);
    }

    @Override
    public void setCloseCallback(final Runnable closeCallback) {
        canvasWriter.setCloseCallback(closeCallback);
    }

    private void setup() {
        setupCalled = true;
        display = SwtUtils.getDisplay();
        ImageDisplay.syncExec(() -> {
            shell = new Shell(display);
            if(name != null) shell.setText(name);

            // set the GridLayout on the shell
            chain(new GridLayout(), l -> l.numColumns = 1, shell::setLayout);

            canvasWriter = canvasHandlerMaker.apply(shell);

            shell.addListener(SWT.Close, e -> {
                if(!shell.isDisposed())
                    shell.dispose();
            });

            shell.open();
        });
    }

    @Override
    public synchronized void update(final Mat image) {
        if(!setupCalled)
            setup();

        canvasWriter.update(image);
    }

    @Override
    public void close() {
        if(display != null) {
            ImageDisplay.syncExec(() -> {
                if(canvasWriter != null)
                    canvasWriter.close();
                if(shell != null)
                    shell.close();
            });
        }
    }

    @Override
    public void waitUntilClosed() throws InterruptedException {
        canvasWriter.waitUntilClosed();
    }
}
