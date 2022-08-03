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

package ai.kognition.pilecv4j.image;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotEquals;
import static org.opencv.imgcodecs.Imgcodecs.IMREAD_UNCHANGED;

import java.io.File;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.opencv.core.CvType;
import org.opencv.imgcodecs.Imgcodecs;

import net.dempsy.util.Functional;
import net.dempsy.util.QuietCloseable;

import ai.kognition.pilecv4j.image.CvRaster.BytePixelSetter;
import ai.kognition.pilecv4j.image.CvRaster.GetChannelValueAsInt;
import ai.kognition.pilecv4j.image.display.ImageDisplay;
import ai.kognition.pilecv4j.image.display.ImageDisplay.Implementation;

public class CvRasterTest {

    public final static boolean SHOW; /// can only set this to true when building on a machine with a display

    static {
        final String sysOpSHOW = System.getProperty("pilecv4j.SHOW");
        final boolean sysOpSet = sysOpSHOW != null;
        boolean show = ("".equals(sysOpSHOW) || Boolean.parseBoolean(sysOpSHOW));
        if(!sysOpSet)
            show = Boolean.parseBoolean(System.getenv("PILECV4J_SHOW"));
        SHOW = show;
    }

    static {
        CvMat.initOpenCv();
    }

    private static String testImagePath = new File(
        CvRasterTest.class.getClassLoader().getResource("test-images/expected-8bit-grey.darkToLight.bmp").getFile())
            .getAbsolutePath();

    private static int IMAGE_WIDTH_HEIGHT = 256;

    @Rule public TemporaryFolder tempDir = new TemporaryFolder();

    @Test
    public void testMove() throws Exception {
        try(final CvMat cvmat = scopedGetAndMove();) {
            assertEquals(IMAGE_WIDTH_HEIGHT, cvmat.rows());
            assertEquals(IMAGE_WIDTH_HEIGHT, cvmat.cols());

            // meeger attempt to call the finalizer on the original Mat
            // created in scopedGetAndMove using Imgcodecs.
            for(int i = 0; i < 10; i++) {
                System.gc();
                Thread.sleep(100);
            }
        }
    }

    /*
     * This is here because it's part of the above testing of the
     * Mat 'move' functionality.
     */
    private static CvMat scopedGetAndMove() {
        try(final CvMat ret = CvMat.move(Imgcodecs.imread(testImagePath, IMREAD_UNCHANGED));) {
            assertEquals(IMAGE_WIDTH_HEIGHT, ret.rows());
            assertEquals(IMAGE_WIDTH_HEIGHT, ret.cols());
            // A true std::move was applied to mat so we really shouldn't
            // access it afterward.
            return ret.returnMe();
        }
    }

    @Test
    public void testShow() throws Exception {
        if(SHOW) {
            try(final CvMat raster = ImageFile.readMatFromFile(testImagePath);
                QuietCloseable c = new ImageDisplay.Builder().implementation(Implementation.SWT)
                    .show(raster).windowName("Test").build();

                QuietCloseable c2 = new ImageDisplay.Builder().implementation(Implementation.HIGHGUI)
                    .show(raster).windowName("Test").build();

            ) {
                Thread.sleep(3000);
            }
        }
    }

    @Test
    public void testSimpleCreate() throws Exception {
        final String expectedFileLocation = testImagePath;

        try(final CvMat mat = new CvMat(IMAGE_WIDTH_HEIGHT, IMAGE_WIDTH_HEIGHT, CvType.CV_8UC1)) {
            mat.rasterAp(raster -> {
                raster.apply((BytePixelSetter)(r, c) -> new byte[] {(byte)(((r + c) >> 1) & 0xff)});

                try(final CvMat expected = Functional.uncheck(() -> ImageFile.readMatFromFile(expectedFileLocation));) {
                    expected.rasterAp(e -> assertEquals(e, raster));
                }
            });
        }
    }

    @Test
    public void testEqualsAndNotEquals() throws Exception {
        final String expectedFileLocation = testImagePath;

        try(final CvMat omat = new CvMat(IMAGE_WIDTH_HEIGHT, IMAGE_WIDTH_HEIGHT, CvType.CV_8UC1)) {
            omat.rasterAp(ra -> ra.apply((BytePixelSetter)(r, c) -> {
                if(r == 134 && c == 144)
                    return new byte[] {-1};
                return new byte[] {(byte)(((r + c) >> 1) & 0xff)};
            }));

            try(final CvMat emat = ImageFile.readMatFromFile(expectedFileLocation);) {
                emat.rasterAp(expected -> omat.rasterAp(raster -> {
                    assertNotEquals(expected, raster);

                    // correct the pixel
                    raster.set(134, 144, new byte[] {(byte)(((134 + 144) >> 1) & 0xff)});
                    assertEquals(expected, raster);
                }));
            }
        }
    }

    @Test
    public void testReduce() throws Exception {
        try(final CvMat mat = new CvMat(255, 255, CvType.CV_8UC1);) {
            mat.rasterAp(raster -> {
                raster.apply((BytePixelSetter)(r, c) -> new byte[] {(byte)c});
            });

            final GetChannelValueAsInt valueFetcher = CvRaster.channelValueFetcher(mat.type());
            final long sum = CvMat.rasterOp(mat,
                raster -> raster.reduce(Long.valueOf(0), (prev, pixel, row, col) -> Long.valueOf(prev.longValue() + valueFetcher.get(pixel, 0))));

            long expected = 0;
            for(int i = 0; i < 255; i++)
                expected = expected + i;
            expected *= 255;

            assertEquals(expected, sum);
        }
    }
}
