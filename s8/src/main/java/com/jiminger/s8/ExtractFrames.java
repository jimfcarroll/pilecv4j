package com.jiminger.s8;
/***********************************************************************
    Legacy Film to DVD Project
    Copyright (C) 2005 James F. Carroll

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
****************************************************************************/

import java.awt.Color;
import java.awt.image.BufferedImage;
import java.awt.image.ColorModel;
import java.awt.image.DataBuffer;
import java.awt.image.IndexColorModel;
import java.awt.image.PixelInterleavedSampleModel;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Properties;

import javax.media.jai.TiledImage;

import org.opencv.core.Core;
import org.opencv.core.CvType;
import org.opencv.core.Mat;
import org.opencv.core.Size;
import org.opencv.imgcodecs.Imgcodecs;
import org.opencv.imgproc.Imgproc;

import com.jiminger.houghspace.Transform;
import com.jiminger.image.CvRaster;
import com.jiminger.image.CvRaster.PixelAggregate;
import com.jiminger.image.ImageFile;
import com.jiminger.image.Point;
import com.jiminger.image.PolarLineFit;
import com.jiminger.image.WeightedPoint;
import com.jiminger.image.canny.EdgeDetectorOpImage;
import com.jiminger.image.jai.AddOverlayOpImage;
import com.jiminger.nr.Minimizer;
import com.jiminger.nr.MinimizerException;
import com.jiminger.util.CommandLineParser;
import com.jiminger.util.PropertiesUtils;

/*******************************************************************
 * Because I had to look this up 8000 times I decided to document it.
 *
 * Sprocket image color key.
 *  1) Clusters - blue circles.
 *  2) edges pixels belonging to a feature according to the HT - green
 *  3) Hough transform peaks - red circles
 *  4) edge pixels removed by the sprocket hole model error minimizattion - red
 *  5)   (edge pixels remaining from the minimization of (4) are REPAINTED green.
 *  6) Sprocket hole centers discovered after regression of (4) - yellow circles
 *  7) film edges surviving line fit error minimization along a sprocket hole - blue
 *  8) film edges removed as a result of line fit error minimization along a sprocket hole - red
 *  9) frame line from center of sprocket hole to the far edge along with the 
 *     far edge intersection point and sprocket edge intersection point are all - cyan
 *  
 ********************************************************************/

@SuppressWarnings("restriction")
public class ExtractFrames {
   public static final long megaBytes = 1024L * 1024L;
   public static final long defaultTileCacheSize = 300;
   public static final String frameFilenameBase = "f";

   public static int frameHeightPix = -1;
   public static int frameWidthPix = -1;

   public static double frameOversizeMult = 1.0;

   public static long tileCacheSize = defaultTileCacheSize * megaBytes;
   public static String sourceFileName = null;
   public static int resolutiondpi = 3200;
   public static int tlow = 50;
   public static int thigh = 200;
   public static float sigma = 6.0f;
   public static int houghThreshold = 150;
   public static boolean reverseImage = false;
   public static boolean correctrotation = true;
   public static boolean rescale = true;

   public static boolean dowatermark = false;
   public static int watermarkPositionX = 40;
   public static int watermarkPositionY = 40;

   public static double quantFactor = 7.0;
   public static int filmLayout = -1;
   public static int filmType = FilmSpec.superEightFilmType;
   public static int sprocketLayout;
   public static boolean writeDebugImages = true;

   public static String outputType = "JPEG";
//   public static String outputType = "BMP";
   public static String outputExt = "jpeg";
//   public static String outputExt = "bmp";

   // the following setting is the percentage of the SprocketHole 
   //  width (the narrowest part - whatever that is) that peaks need
   //  to be from one another to be considered within the same 
   //  cluster
   public static double clusterFactor = 0.2;

   public static boolean allowInterframeGeometry = true;
   
   public static BufferedImage mat2Img(Mat in) {
       BufferedImage out;
       byte[] data = new byte[in.width() * in.height() * (int)in.elemSize()];
       int type;
       in.get(0, 0, data);

       if(in.channels() == 1)
           type = BufferedImage.TYPE_BYTE_GRAY;
       else
           type = BufferedImage.TYPE_3BYTE_BGR;

       out = new BufferedImage(in.width(), in.height(), type);

       out.getRaster().setDataElements(0, 0, in.width(), in.height(), data);
       return out;
   }
   
   public static void print(String prefix, Mat im) {
	   System.out.println(prefix + " { depth=(" + CvType.ELEM_SIZE(im.type()) + ", " + im.depth() + "), channels=" + im.channels() + " HxW=" + im.height() + "x" + im.width()  + " }");
   }
   
   public static final double _256Ov2Pi = (256.0/(2.0 * Math.PI));

   public static byte angle_byte(double x, double y) {
      double xu, yu, ang;
      double ret;
      int rret;

      xu = Math.abs(x);
      yu = Math.abs(y);

      if((xu == 0) && (yu == 0)) return(0);

      ang = Math.atan(yu/xu);

      if(x >= 0){
         if(y >= 0) ret=ang;
         else ret=(2.0*Math.PI - ang);
      }
      else{
         if(y >= 0) ret=(Math.PI - ang);
         else ret=(Math.PI + ang);
      }

      rret = (int)(0.5 + (ret * _256Ov2Pi));
      if (rret >= 256)
         rret = 0;

      return byteify((int)rret);
   }
   
   public static final double[] cvrtScaleDenom = new double[ 6 ];
   
   static {
	   cvrtScaleDenom[CvType.CV_16U] = (double)((int) 0xffff);
	   cvrtScaleDenom[CvType.CV_16S] = (double)((int) 0x7fff);
	   cvrtScaleDenom[CvType.CV_8U] = (double)((int) 0xff);
	   cvrtScaleDenom[CvType.CV_8S] = (double)((int) 0x7f);
   }
   
   public static Mat convertToGray(Mat src) {
	   Mat workingImage = new Mat();
	   if (src.depth() != CvType.CV_8U) {
		   System.out.print("converting image to 8-bit grayscale ... ");
		   src.convertTo(workingImage, CvType.CV_8U, 255.0 / cvrtScaleDenom[src.depth()]);
		   Imgproc.cvtColor(workingImage, workingImage, Imgproc.COLOR_BGR2GRAY);
		   return workingImage;
	   } else {
		   src.copyTo(workingImage);
		   Imgproc.cvtColor(src, workingImage, Imgproc.COLOR_BGR2GRAY);
		   return workingImage;
	   }
   }
   
   /** The main method. */
   public static void main(String[] args) 
      throws IOException, InterruptedException, MinimizerException
   {
	  System.loadLibrary(Core.NATIVE_LIBRARY_NAME);
      com.jiminger.util.Timer totalTime = new com.jiminger.util.Timer();
      totalTime.start();

      // we will write out a set of bookkeeping properties
      //  for the next pass.
      Properties prop = new Properties();

      // First parse the command line and test for various
      //  settings.
      if (!commandLine(args))
         return;

      // parse the source filename
      int index = sourceFileName.lastIndexOf(".");
      if (index < 0) {
         System.err.println("\"" + sourceFileName + 
                            "\" has no extention and so I cannot create a subdirectory of the same name.");
         return;
      }

      // create the output directory (if necessary) for the individual frames
      String outDir = sourceFileName.substring(0,index);
      File dir = new File(outDir);
      dir.mkdir();
      int lastSlashIndex = sourceFileName.lastIndexOf("\\");
      if (lastSlashIndex <0)
         lastSlashIndex = sourceFileName.lastIndexOf("/");
      String destFileName = outDir + File.separator + frameFilenameBase;
      System.out.println("going to write frames to " + destFileName + "xx." + outputExt);

      String propertyFileName = outDir + File.separator + "frames.properties";

      Mat origImage = Imgcodecs.imread(sourceFileName, Imgcodecs.CV_LOAD_IMAGE_UNCHANGED);
      if (writeDebugImages)
    	  Imgcodecs.imwrite("orig.tif", origImage);
      print("origImage", origImage);
      int origImageHeight = origImage.height();
      int origImageWidth = origImage.width();

      com.jiminger.util.Timer timer = new com.jiminger.util.Timer();

      //------------------------------------------------------------
      //   This does a conversion to grayscale
      //------------------------------------------------------------
      // Create a grayscale color model.
      timer.start();
      Mat grayImage = convertToGray(origImage);
      if (writeDebugImages)
    	  Imgcodecs.imwrite("gray.bmp", grayImage);
      print("grayImage", grayImage);
      
      System.out.println("done (" + timer.stop() + ")");
      //------------------------------------------------------------

      //------------------------------------------------------------
      //   This does a canny edge detection and puts the gradient
      //    direction image in the GradientDirectionImageHolder
      //    object (if one is supplied).
      //------------------------------------------------------------
      timer.start();
      System.out.print("performing canny edge detection ... ");
      System.out.print("blurring ... ");
      Imgproc.blur(grayImage, grayImage, new Size(3, 3));
      Imgcodecs.imwrite("blur.bmp", grayImage);
      System.out.print("canny ... "); 
      Mat edgeImage = new Mat();
      Imgproc.Canny(grayImage, edgeImage, tlow, thigh, 3, true);
      System.out.println("done.");
      if (writeDebugImages)
    	  Imgcodecs.imwrite("edge.bmp", edgeImage);
      print("edge", edgeImage);
      //--------------------------------------
      
      //--------------------------------------
      //   Make the gradient images
      //--------------------------------------
      System.out.print("Making gradient image ... ");
      Mat dx = new Mat();
      Mat dy = new Mat();
      Imgproc.Sobel(grayImage, dx, CvType.CV_16S, 1, 0);
      Imgproc.Sobel(grayImage, dy, CvType.CV_16S, 0, 1);
      if (writeDebugImages) {
    	  Imgcodecs.imwrite("dx.tiff", dx);
    	  Imgcodecs.imwrite("dy.tiff", dy);
      }
      short[] dxs = new short[dx.height() * dx.width()];
      dx.get(0, 0, dxs);
      short[] dys = new short[dy.height() * dy.width()];
      byte[] dirs = new byte[dx.height() * dx.width()];
      dy.get(0, 0, dys);
      for (int pos=0; pos < dxs.length; pos++) {
    	  final double dxv = (double)dxs[pos];
    	  final double dyv = 0.0 - (double)dys[pos];
    	  dirs[pos] = angle_byte(dxv,dyv);
      }
      Mat gradientDirImage = new Mat(dx.height(), dx.width(), CvType.CV_8UC1);
      gradientDirImage.put(0,0,dirs);
      if (writeDebugImages)
    	  Imgcodecs.imwrite("gradDir.bmp", gradientDirImage);
      System.out.println("done.");
      print("dx",dx);
      print("dy",dy);
      
      CvRaster edgeRaster = CvRaster.create(edgeImage);
      CvRaster gradientDirRaster = CvRaster.create(gradientDirImage);
      
      //------------------------------------------------------------
      //  Now load up the edges of the image. This will set the values
      //   of sprocketEdge and farEdge to the appropriate value
      //------------------------------------------------------------
      timer.start();
      System.out.print("finding the edges of the film ... ");
      FilmEdge [] filmedges = FilmEdge.getEdges(filmLayout,edgeRaster,EdgeDetectorOpImage.EDGE,gradientDirRaster,true);
      FilmEdge sprocketEdge = reverseImage ? filmedges[0] : filmedges[1];
      FilmEdge farEdge = reverseImage ? filmedges[1] : filmedges[0];
      System.out.println("done (" + timer.stop() + ")");
      System.out.println("film edges:" + sprocketEdge + " " + farEdge);
      //------------------------------------------------------------

      //------------------------------------------------------------
      //  Do the hough transform of the model. First figure out the
      //   range of pixels to do the transform over. This will 
      //   be a certain distance from the edge of the image where
      //   the sprockets are.
      //------------------------------------------------------------
      timer.start();
      System.out.print("preparing hough transform ... ");
      double sprocketHoleWidthPixels = FilmSpec.inPixels(filmType,FilmSpec.widthIndex,resolutiondpi);
      double edgeToSprocketCenterPixels = 
         FilmSpec.inPixels(filmType,FilmSpec.edgeToEdgeIndex,resolutiondpi) + (sprocketHoleWidthPixels / 2.0);
      double furthestPixelToLookFor = edgeToSprocketCenterPixels + (sprocketHoleWidthPixels / 2.0) + (2.0 * quantFactor);
      double closestPixelToLookFor = edgeToSprocketCenterPixels - ((sprocketHoleWidthPixels / 2.0) + (2.0 * quantFactor));

      int rowstart = 0;
      int rowend = origImageHeight - 1;
      int colstart = 0;
      int colend = origImageWidth - 1;
      switch (sprocketLayout)
      {
         case FilmSpec.alongRight:
            colstart = sprocketEdge.mostLeft().x - ((int)(furthestPixelToLookFor + 1.0));
            colend =  sprocketEdge.mostRight().x - ((int)(closestPixelToLookFor + 1.0));
            break;
         case FilmSpec.alongLeft:
            colstart = ((int)(closestPixelToLookFor + 1.0)) + sprocketEdge.mostLeft().x;
            colend = ((int)(furthestPixelToLookFor + 1.0)) + sprocketEdge.mostRight().x;
            break;
         case FilmSpec.alongTop:
            rowstart = ((int)(closestPixelToLookFor + 1.0)) + sprocketEdge.mostTop().y;
            rowend = ((int)(furthestPixelToLookFor + 1.0)) + sprocketEdge.mostBottom().y;
            break;
         case FilmSpec.alongBottom:
            rowstart = sprocketEdge.mostTop().y - ((int)(furthestPixelToLookFor + 1.0));
            rowend = sprocketEdge.mostBottom().y - ((int)(closestPixelToLookFor + 1.0));
            break;
      }

      // Create the sprocket hole model and the Transform for the model.
      //  in order to locate the clusters in the transform space
      SprocketHoleModel sm = new SprocketHoleModel(resolutiondpi,filmType,filmLayout);
      Transform transform = new Transform(sm,quantFactor,10.0);

//      // write out the mask and the gradient mask 
//      //  for debugging purposes
//      if (writeDebugImages)
//         ImageFile.writeImageFile(transform.getMask().getMaskImage(),"tmpmask.bmp", "BMP");
//      if (writeDebugImages)
//         ImageFile.writeImageFile(transform.getGradientDirMask().getMaskImage(),"tmpgradmask.bmp", "BMP");

      // Execute the hough transform on the edge image
      System.out.print("executing hough transform (" + rowstart + "->" + 
                       rowend + "," + colstart + "->" + colend + ")" + "... ");
      Transform.HoughSpace houghSpace = transform.transform(edgeRaster,gradientDirRaster,houghThreshold,
                                                            rowstart,rowend,colstart,colend);
      if (writeDebugImages)
         ImageFile.writeImageFile(transform.getTransformImage(houghSpace),"tmpht.bmp", "BMP");
      System.out.println("done (" + timer.stop() + ")");
      //------------------------------------------------------------

      timer.start();
      System.out.print("writing transform information to debug image ... ");
      TiledImage sprocketInfoTiledImage = new TiledImage(
         0,0,origImageWidth, origImageHeight,0,0,
         new PixelInterleavedSampleModel(
            DataBuffer.TYPE_BYTE,origImageWidth,origImageHeight,1,origImageWidth, bandstride),
         getOverlayCM());

//      // This commented out code if for me to look at the resulting edges.
//      PolarLineFit.drawPolarLine(sprocketEdge.r,sprocketEdge.c,sprocketInfoTiledImage,Color.cyan);
//      PolarLineFit.drawPolarLine(farEdge.r,farEdge.c,sprocketInfoTiledImage,Color.cyan);
//      JAI.create("filestore",sprocketInfoTiledImage,"tmpsprockets.bmp", "BMP", null);
//      System.exit(0);

      System.out.println("done (" + timer.stop() + ")");

      timer.start();
      System.out.print("calculating inverse hough transform ...");
      java.util.List<Transform.HoughSpaceEntry> hse = transform.inverseTransform(houghSpace,sprocketInfoTiledImage,AddOverlayOpImage.GOVERLAY,Color.red);
      System.out.println("done (" + timer.stop() + ")");

      //------------------------------------------------------------
      // locate the clusters in the transform and prune off
      //  clusters that cant be appropriate based on known information
      //  about the image.
      //------------------------------------------------------------
      timer.start();
      System.out.print("clustering the transform results ... ");
      java.util.List<Transform.Cluster> clusters = transform.cluster(hse,clusterFactor);
      System.out.println("done (" + timer.stop() + ")");

      // eliminate clusters that fall outside of the edges
      timer.start();
      System.out.print("eliminating clusters outside edges ... ");

      furthestPixelToLookFor = 
         edgeToSprocketCenterPixels + (sprocketHoleWidthPixels / 2.0);
      closestPixelToLookFor = 
         edgeToSprocketCenterPixels - (sprocketHoleWidthPixels / 2.0);

      for (int i = clusters.size() - 1; i >= 0; i--)
      {
         Transform.Cluster cluster = (Transform.Cluster)clusters.get(i);
         double distToFarEdge = 
            PolarLineFit.perpendicularDistance(cluster, farEdge.c, farEdge.r);
         Point p = 
            PolarLineFit.closest(cluster, sprocketEdge.c, sprocketEdge.r);
         double distBetweenEdgesAtCluster = 
            PolarLineFit.perpendicularDistance(p, farEdge.c, farEdge.r);
         double distToCloseEdge = PolarLineFit.distance(cluster,p);

         if (distToFarEdge >= distBetweenEdgesAtCluster ||
             distToCloseEdge < closestPixelToLookFor ||
             distToCloseEdge > furthestPixelToLookFor)
            clusters.remove(i);
      }
      System.out.println("done (" + timer.stop() + ")");

      timer.start();
      System.out.print("pruning clusters too far out of line ... ");
      // now find the best line which passes through the clusters
      //  as a polar - determine what side of center the line
      //  falls on in order to see if the image is correct
      //  or not.
      double [] result = null;
      double maxDistanceFromLine = ((double)resolutiondpi)/64.0; // this gives 50 at 3200 dpi

      for (boolean done = false; !done;)
      {
         PolarLineFit sprocketErrFunc = new PolarLineFit(clusters,true);
         Minimizer m = new Minimizer(sprocketErrFunc);
         /*double sumSqErr =*/ m.minimize(startingPowell);
         result = m.getFinalPostion();

         double furthestDist = PolarLineFit.perpendicularDistance(sprocketErrFunc.worst,result[0],result[1]);

         // this prunes off clusters way out of line
         if (furthestDist > maxDistanceFromLine)
            clusters.remove(sprocketErrFunc.worst);
         else
            done = true;
      }

      Transform.drawClusters(clusters,sprocketInfoTiledImage,Color.blue);
      System.out.println("done (" + timer.stop() + ")");
      //------------------------------------------------------------

      //------------------------------------------------------------
      // Minimize the error in the sprocket location, scale, and
      //  rotation and trim clusters without a minimal remaining
      //  number left.
      //------------------------------------------------------------
      timer.start();
      System.out.print("finding the best fit for the sprocket holes ... ");
      List<Transform.Fit> sprockets = new ArrayList<Transform.Fit>();
      List<java.awt.Point> prunnedEdges = new ArrayList<java.awt.Point>();
      List<Transform.Cluster> removedClusters = new ArrayList<Transform.Cluster>();

      // the minimal number of pixels should at least be 
      //  the number in a long edge of a sprocket hole.
      double sprocketWidthPix = FilmSpec.inPixels(filmType,FilmSpec.widthIndex,resolutiondpi);
      double sprocketHeightPix = FilmSpec.inPixels(filmType,FilmSpec.heightIndex,resolutiondpi);
      int minNumPixels = (int)((sprocketWidthPix > sprocketHeightPix ? sprocketWidthPix : sprocketHeightPix) + 0.5);
      for (int i = clusters.size() - 1; i >= 0; i--)
      {
         prunnedEdges.clear();
         Transform.Cluster cluster = (Transform.Cluster)clusters.get(i);
         Transform.Fit fit = 
            transform.bestFit(cluster, sprocketInfoTiledImage, AddOverlayOpImage.ROVERLAY, 
                              AddOverlayOpImage.GOVERLAY, prunnedEdges);
         sprockets.add(fit);

         if (fit.edgeVals.size() < minNumPixels)
            removedClusters.add(clusters.remove(i));
      }
      System.out.println("done (" + timer.stop() + ")" + removedClusters);
      //------------------------------------------------------------

      //------------------------------------------------------------
      if (allowInterframeGeometry)
      {
         timer.start();
         System.out.print("Using interframe geometry to validate clusters ... ");

         //-------------------------------------------
         // First I need to put the Fits in rank order
         //  The rank that seems to work the best needs 
         //  to take into account the std deviation as 
         //  well as the number of edge pixels that contribute
         //  to the Fit. In this case we sort the list from
         //  lowest stdDev to highest and assign a rank to the
         //  list order. Then sort from highest to lowest contributors
         //  and add that list order to the rank. Those with the highest
         //  rank are considered more likely to be real holes and the
         //  interframeFilter assumes that the first one on the list
         //  is garunteed to be a hole.
         //-------------------------------------------
         Collections.sort(sprockets,new Transform.Fit.StdDeviationOrder());
         for (int i = 0; i < sprockets.size(); i++)
            sprockets.get(i).rank = sprockets.size() - i;

         Collections.sort(sprockets,new Transform.Fit.EdgeCountOrder());
         for (int i = 0; i < sprockets.size(); i++)
            sprockets.get(i).rank += sprockets.size() - i;

         java.util.List<Transform.Fit> verifiedSprockets = new ArrayList<Transform.Fit>();
         int imgLength = FilmSpec.isVertical(filmLayout) ? origImageHeight : origImageWidth;
         if (!FilmSpec.interframeFilter(filmType,filmLayout,resolutiondpi,imgLength,
        		 sprockets,(List<WeightedPoint>)(Object)verifiedSprockets))
         {
            // in the odd case where this fails, assume it's due to the fact that
            // the first point in the list was actually not a real sprocket hole
            for (boolean done = false; !done && sprockets.size() > 0;)
            {
               sprockets.remove(0);
               verifiedSprockets.clear();
               // try again
               done = FilmSpec.interframeFilter(filmType,filmLayout,resolutiondpi,imgLength,
                     sprockets,(List<WeightedPoint>)((List<?>)verifiedSprockets));

               if (done)
                  sprockets = verifiedSprockets;
            }
         }
         else
            sprockets = verifiedSprockets;

         System.out.println("done (" + timer.stop() + ")");
      }


      //------------------------------------------------------------
      // This is where I used to do the minimization. Now,
      //  I only want to trim the sprocket by the remaining 
      //  clusters.
      //------------------------------------------------------------

      // Put the fits in sprocket hole order
      Collections.sort(sprockets,new FilmSpec.SprocketHoleOrder(filmLayout));
      Transform.drawFits(sprockets,sprocketInfoTiledImage,Color.yellow);
      //------------------------------------------------------------

      // use the film edge and calculate the resolution average
      double resolutionsum = 0.0;

      timer.start();
      System.out.print("finding and writing output images ");

      // For 8mm it takes two successive sprocket holes to make one
      //  frame. For super-8 it takes only one.
      int numFrames = filmType == FilmSpec.eightMMFilmType ? 
         sprockets.size() - 1 : sprockets.size();

      for (int i = 0; i < numFrames; i++)
      {
         Transform.Fit fit = (Transform.Fit)sprockets.get(i);

         // if we are dealing with 8mm then we need to take successive
         //  sprocket holes and find the centroid of the two fits
         //  to find the center of the frame
         Transform.Fit frameReference = fit;
         if (filmType == FilmSpec.eightMMFilmType)
         {
            Transform.Fit fit2 = (Transform.Fit)sprockets.get(i+1);
            frameReference = new Transform.Fit(
               (fit.cr + fit2.cr)/2.0,(fit.cc + fit2.cc)/2.0,fit.scale,fit.rotation,
               null,0.0,null);
         }

         FilmEdge sprocketEdgePiece = sprocketEdge.edgePiece(frameReference.cr,frameReference.cc,(double)frameHeightPix,false);
         if (sprocketEdgePiece == null) sprocketEdgePiece = sprocketEdge.edgePiece(frameReference.cr,frameReference.cc,(double)frameHeightPix,true);
         FilmEdge farEdgePiece = farEdge.edgePiece(frameReference.cr,frameReference.cc,(double)frameHeightPix,false);
         if (farEdgePiece == null) farEdgePiece = farEdge.edgePiece(frameReference.cr,frameReference.cc,(double)frameHeightPix,true);

         if (sprocketEdgePiece != null)
         {
            sprocketEdgePiece.writeEdge(sprocketInfoTiledImage,AddOverlayOpImage.BOVERLAY);
            sprocketEdgePiece.writePruned(sprocketInfoTiledImage,AddOverlayOpImage.ROVERLAY);

            // for debug purposes draw a circle around the point along the sprocketEdgePiece
            //  that is closest to the sprocket. This line is the axis of the frame and
            //  passes through the center.
            PolarLineFit.drawCircle(PolarLineFit.closest(frameReference,sprocketEdgePiece.c, sprocketEdgePiece.r),
                                    sprocketInfoTiledImage,Color.cyan);
         }
         if (farEdgePiece != null)
         {
            farEdgePiece.writeEdge(sprocketInfoTiledImage,AddOverlayOpImage.BOVERLAY);
            farEdgePiece.writePruned(sprocketInfoTiledImage,AddOverlayOpImage.ROVERLAY);


            // for debug purposes draw a circle around the point along the far edge
            //  that is closest to the sprocket. This line is the axis of the frame and
            //  passes through the center.
            Point closest = PolarLineFit.closest(frameReference,farEdgePiece.c, farEdgePiece.r);
            PolarLineFit.drawCircle(closest,sprocketInfoTiledImage,Color.cyan);
            PolarLineFit.drawLine(frameReference,closest,sprocketInfoTiledImage,Color.cyan);
         }

         // figure out what the distance between the two edges are
         // 
         // the distance between two parallel lines in polar coords ought
         //  to be the differences between their radius'
         // since they are not perfectly parallel (which is a real problem with crappy
         //  scanners) we will measure the distance from the sprocket hole to each edge
         //  and add them.
         double calcedEdgeResDPI = -1.0;
         if (sprocketEdgePiece != null && farEdgePiece != null) {
            double distPix = PolarLineFit.perpendicularDistance(frameReference,sprocketEdgePiece.c, sprocketEdgePiece.r) +
               PolarLineFit.perpendicularDistance(frameReference,farEdgePiece.c, farEdgePiece.r);

            calcedEdgeResDPI = (distPix * FilmSpec.mmPerInch) / FilmSpec.filmAttribute(filmType,FilmSpec.filmWidthIndex);
         }

         Frame frame = new Frame(frameReference/*(Transform.Fit)sprockets.get(i)*/,
                                 sprocketEdgePiece,farEdgePiece,/*filmLayout,*/filmType);

         String frameNumStr = (i < 10) ? ("0" + i) : Integer.toString(i);

         System.out.print(".");
         Mat frameTiledImageMat = frame.cutFrame(
            origImage,resolutiondpi,frameWidthPix, frameHeightPix, reverseImage,i, frameOversizeMult,
            rescale, correctrotation);
         
         frameTiledImageMat = linearContrast(frameTiledImageMat,0, 0.3);
         
         Imgcodecs.imwrite("f" + frameNumStr + ".tif", frameTiledImageMat);
         
//         BufferedImage frameTiledImage = mat2Img(frameTiledImageMat);
//
//         if (frameTiledImage != null)
//         {
//            if (dowatermark)
//            {
//               Graphics2D fg = frameTiledImage.createGraphics();
//               fg.setColor(Color.white);
//               fg.drawString(sourceFileName + " " + frameNumStr,watermarkPositionX,watermarkPositionY);
//            }
//
////// This commented out code writes the cyan edge into the original picture and then recuts the 
//////  the frame so that it can get picked up in the final image         
//////         System.out.println (" frame " + i + " bound by " + frame.topmostRow + "," + frame.leftmostCol + "," + frame.bottommostRow + "," + frame.rightmostCol);
////            com.jiminger.util.PolarLineFit.drawPolarLine(sprocketEdgePiece.r,sprocketEdgePiece.c, origImageOverlayer,Color.cyan,
////                          0, frame.leftmostCol, origImageHeight-1, frame.rightmostCol/*, frame.topmostRow,0*/);
////
////            com.jiminger.util.PolarLineFit.drawPolarLine(farEdgePiece.r,farEdgePiece.c,origImageOverlayer,Color.cyan,
////                          0, frame.leftmostCol, origImageHeight-1,frame.rightmostCol/*, -(origImageHeight-1-frame.bottommostRow),0*/);
////
////            frameTiledImage = frame.cutFrame(
////               origImage,resolutiondpi,frameWidthPix, frameHeightPix, reverseImage,i,frameOversizeMult);
//////--------------------------------------------------------------------------
//
//            // there is no reason
//            if (!frame.isOutOfBounds())
//               ImageFile.writeImageFile(frameTiledImage, 
//                                        destFileName + frameNumStr + "." + outputExt, 
//                                        outputType);
//
//            // callculate the resolution
////            double calcedSprocketResDPI = resolutiondpi * frameReference.scale;
//            resolutionsum += calcedEdgeResDPI;
//
//            prop.setProperty("frames." + Integer.toString(i) + ".filename",
//                             frameFilenameBase + frameNumStr + "." + outputExt);
//         }
//         else
//         {
//            System.out.println("Failed to cut image for frame " + i);
//            prop.setProperty("frame." + Integer.toString(i) + ".dropped","true");
//         }

         frame.addProperties("frames." + Integer.toString(i), prop);
      }

      prop.setProperty("frames.numberofframes",Integer.toString(numFrames));

      System.out.println(" done (" + timer.stop() + ")");

      if (writeDebugImages)
         ImageFile.writeImageFile(sprocketInfoTiledImage,"tmpsprockets.bmp", "BMP");

      double calculatedResolution = (resolutionsum / (double)sprockets.size());
      System.out.println("calculated resolution based on distance between edges is an average of:" + 
                         calculatedResolution);

      PropertiesUtils.saveProps(prop,propertyFileName,"frame properties for source image " + sourceFileName);

      System.out.println("total time for entire process is " + totalTime.stop());
   }
   
   private static class Hist {
	   public final int[] h = new int[0xffff + 1];
	   public int max = 0;
   }
   
   private static Mat applyCdf(int[] cdf, CvRaster src) {
	   int maxPixVal = cdf.length - 1; // should be either 0xff or 0xffff
	   if (maxPixVal != 0xff && maxPixVal != 0xffff)
		   throw new RuntimeException();
	   double maxPixValD = (double)maxPixVal;
       double max = cdf[maxPixVal];
       double[] scale = new double[maxPixVal + 1];
       double factor = maxPixValD / max;
       for (int idx = 0; idx < scale.length; idx++)
      	 scale[idx] = (double)cdf[idx] * factor;
       
       final CvRaster tmp = CvRaster.create(src.rows, src.cols, CvType.CV_32SC3);
       
       int maxChannel = 0;
       for (int r = 0; r < src.rows; r++) {
      	 for (int c = 0; c < src.cols; c++) {
      		 short[] sbgr = (short[])src.get(r, c);
          	 double intensity = ((int)sbgr[0] & maxPixVal);
          	 for (int idx=1; idx<sbgr.length;idx++) intensity += (double)((int)sbgr[idx] & maxPixVal);
          	 intensity /= 3.0;
          	 double boost = scale[(int)Math.round(intensity)] / intensity;
          	 int[] newpix = new int[3];
          	 for (int idx = 0; idx < 3; idx++) {
          		 int cv = (int) Math.round(((int)sbgr[idx] & maxPixVal) * boost);
          		 if (maxChannel < cv) maxChannel = cv;
          		 newpix[idx] = cv;
          	 }
          	 tmp.set(r, c, newpix);
      	 }
       }
       
       // now rescale so maxChannel is 0xffff.
       double rescale = maxPixValD / (double) maxChannel;
       for (int r = 0; r < tmp.rows; r++) {
    	   for (int c = 0; c < tmp.cols; c++) {
    		   int[] bgr = (int[])tmp.get(r, c);
    		   short[] sbgr = new short[3];
    		   for (int i =0; i < 3; i++) sbgr[i] = (short)((int)Math.round((double)bgr[i] * rescale) & 0xffff); 
    		   src.set(r, c, sbgr);
    	   }
       }
       
       return src.toMat();
   }
   
   public static PixelAggregate<Object, Hist> histogram(int maxPixVal) {
	   return (h, pixel, row, col) -> {
    	 short[] sbgr = (short[])pixel;
    	 final int b = ((int)sbgr[0] & maxPixVal);
    	 final int g = ((int)sbgr[1] & maxPixVal);
    	 final int r = ((int)sbgr[2] & maxPixVal);
    	 int intensity = r + g + b;
    	 h.h[(int)Math.round((double)intensity / 3.0)]++;
    	 if (h.max < r) h.max = r;
    	 if (h.max < g) h.max = g;
    	 if (h.max < b) h.max = b;
    	 return h;
     };
   }

   public static Mat linearContrast(Mat src, double lowerpct, double upperpct) {
       CvRaster raster = CvRaster.create(src);
       final int maxPixVal = 0xffff;
       
       System.out.print("|");
       Hist cdf = raster.reduce(new Hist(), histogram(maxPixVal));
       
       // convert hist to cdf
       for (int idx = 1; idx < cdf.h.length; idx++) 
      	 cdf.h[idx] = cdf.h[idx - 1] + cdf.h[idx];

       int pixelCount = raster.rows * raster.cols;
       
       int lowerBoundCount = (int) Math.round((double)pixelCount * lowerpct);
       int upperBoundCount = pixelCount - (int) Math.round((double)pixelCount * upperpct);
       
       int lowerBound = -1;
       int upperBound = -1;
       for (int i = 0; i < cdf.h.length; i++) {
    	   if (cdf.h[i] >= lowerBoundCount) {
    		   lowerBound = i;
    		   break;
    	   }
       }
       
       for (int i = cdf.h.length - 1; i >= 0; i--) {
    	   if (cdf.h[i] <= upperBoundCount) {
    		   upperBound = i;
    		   break;
    	   }
       }
       
       // line goes from lowerBound, 0.0 -> upperBound, maxPixVal
       double range = (double)(upperBound - lowerBound);
       double maxPixValD = (double)maxPixVal;
       int[] mapping = new int[cdf.h.length];
       for (int i = 0; i < mapping.length; i++) {
    	   if (i <= lowerBound) mapping[i] = 0;
    	   else if (i >= upperBound) mapping[i] = maxPixVal;
    	   else {
    		   double x = (double) (i - lowerBound);
    		   double v = (x / range) * maxPixValD;
    		   mapping[i] = (int)Math.round(v);
    	   }
       }

       System.out.print("+");
       return applyCdf(mapping,raster);
   }

   public static Mat equalize(Mat src) {
       CvRaster raster = CvRaster.create(src);
       
       final int maxPixVal = 0xffff;
       
       System.out.print("|");
       Hist cdf = raster.reduce(new Hist(), histogram(maxPixVal));
       System.out.print("+");
       
       // convert hist to cdf
       for (int idx = 1; idx < cdf.h.length; idx++) 
      	 cdf.h[idx] = cdf.h[idx - 1] + cdf.h[idx];
       
       return applyCdf(cdf.h, raster);
   }

   public static Mat sigmoidNormalize(Mat src) {
       CvRaster raster = CvRaster.create(src);
       
       System.out.print("|");
       final Hist dfunc = raster.reduce(new Hist(), histogram(0xffff));
       System.out.print("+");
       
       // convert hist to cdf
       for (int idx = 1; idx < dfunc.h.length; idx++) 
      	 dfunc.h[idx] = dfunc.h[idx - 1] + dfunc.h[idx];
       
       double max = (double) dfunc.h[dfunc.h.length - 1];
       
       // minimize the error to a sigmoid
	   final int[] mapping = new int[dfunc.h.length];
       Minimizer.Func sigmoidError = p -> {
    	   double sumerr2 = 0.0;
		   double mn = sigmoid(0.0,p[0], p[1]);
		   double mx = sigmoid((double)0xffff, p[0],p[1]);
    	   for (int i = 0; i < mapping.length; i++) {
    		   double sigmoid = sigmoid((double)i, p[0], p[1]);
    		   double val = (sigmoid - mn) / (mx - mn);
    		   mapping[i] = (int)Math.round(val * max);
    		   double err = (val * max) - (double)dfunc.h[i];
    		   double err2 = err * err;
    		   sumerr2 += err2;
    	   }
    	   return sumerr2;
       };
       
       Minimizer mm = new Minimizer(sigmoidError);
       double finalerr = mm.minimize(new double[] { (double)0xffff / 6.0, (double)0xffff / 2.0 });
       System.out.print("(m=" + mm.getFinalPostion()[0] + ",b=" + mm.getFinalPostion()[1] + ")");
       
       return applyCdf(mapping,raster);
   }
   
   static double sigmoid(double x, double m, double b) {
	   double mt = 0.0 - (x - b) / m;
	   return 1.0 / (1.0 + Math.pow(Math.E, mt));
   }

   static double [] startingPowell = { 512.0, 512.0 };
   private static int [] bandstride = { 0 };


   static private boolean commandLine(String[] args)
   {
      CommandLineParser cl = new CommandLineParser(args);

      // see if we are asking for help
      if (cl.getProperty("help") != null || 
          cl.getProperty("-help") != null)
      {
         usage();
         return false;
      }

      sourceFileName = cl.getProperty("f");
      if (sourceFileName == null)
      {
         usage();
         return false;
      }

      System.out.println("---------------------------------------------------");
      System.out.println("Extracting frames from " + sourceFileName);

      String tmps = cl.getProperty("r");
      if (tmps != null)
         resolutiondpi = Integer.parseInt(tmps);

      System.out.println("   Using approximate resolution of " + resolutiondpi + " dpi");

      tmps = cl.getProperty("ot");
      if (tmps != null)
      {
         outputExt = tmps.toLowerCase();
         outputType = tmps.toUpperCase();
         System.out.println("   Frames will be output into " + outputType + " files.");
      }

      tmps = cl.getProperty("8mm");
      if (tmps != null)
         filmType = FilmSpec.eightMMFilmType;

      tmps = cl.getProperty("fw");
      if (tmps != null)
         frameWidthPix = Integer.parseInt(tmps);

      tmps = cl.getProperty("fh");
      if (tmps != null)
         frameHeightPix = Integer.parseInt(tmps);

      tmps = cl.getProperty("over");
      if (tmps != null)
         frameOversizeMult = Double.parseDouble(tmps);

      rescale = cl.getProperty("norescale") == null ? true : false;
      correctrotation = cl.getProperty("norotation") == null ? true : false;

      tmps = cl.getProperty("wm");
      if (tmps != null)
         dowatermark = 
            tmps.equalsIgnoreCase("true") || tmps.equalsIgnoreCase("t") ||
            tmps.equalsIgnoreCase("yes") || tmps.equalsIgnoreCase("y") ||
            tmps.equalsIgnoreCase("1");


      if (frameWidthPix < 0)
         frameWidthPix = (int)((FilmSpec.inPixels(filmType,FilmSpec.frameWidthIndex,resolutiondpi) * frameOversizeMult) + 0.5);

      if (frameHeightPix < 0)
         frameHeightPix = (int)((FilmSpec.inPixels(filmType,FilmSpec.frameHeightIndex,resolutiondpi) * frameOversizeMult) + 0.5);

      System.out.println("   Extracting a final frame size of width " + frameWidthPix + " and height " + frameHeightPix);
      System.out.println("     and going " + ((frameOversizeMult * 100.0) - 100.0) + " percentage beyond the frame boundaries");
      System.out.println("  " + (!rescale ? " NOT" : "" ) + " rescaling the original image into the destination.");
      System.out.println("  " + (!correctrotation ? " NOT" : "" ) + " rotating the original image into the destination.");

      tmps = cl.getProperty("cs");
      if (tmps != null)
      {
         tileCacheSize = Long.parseLong(tmps) * megaBytes;
         System.out.println("   using a tile cache size of " + tileCacheSize);
      }

//      tmps = cl.getProperty("clamp");
//      if (tmps != null)
//         clampValue = Integer.parseInt(tmps);
//      System.out.println("   Clamping grayscale image at " + clampValue);

      tmps = cl.getProperty("tl");
      if (tmps != null)
         tlow = Integer.parseInt(tmps);

      tmps = cl.getProperty("th");
      if (tmps != null)
         thigh = Integer.parseInt(tmps);

      System.out.println("   Canny details:");
      System.out.println("     using a canny hysteresis high threshold of " + thigh);
      System.out.println("       percent of max and a low threshold of " + tlow + " percent of high.");

      tmps = cl.getProperty("sigma");
      if (tmps != null)
         sigma = Float.parseFloat(tmps);

      System.out.println("     using a std deviation of " + sigma + " pixels for gaussian smoothing.");

      tmps = cl.getProperty("ht");
      if (tmps != null)
         houghThreshold = Integer.parseInt(tmps);

      System.out.println("   Hough Transform voting threshold is " + houghThreshold);

      reverseImage = cl.getProperty("rev") == null ? false : true;

      filmLayout = cl.getProperty("lr") != null ? FilmSpec.lr : 
         (cl.getProperty("rl") != null ? FilmSpec.rl :
          (cl.getProperty("tb") != null ? FilmSpec.tb :
           (cl.getProperty("bt") != null ? FilmSpec.bt : -1)));

      if (filmLayout == -1)
      {
         usage();
         return false;
      }


      sprocketLayout = FilmSpec.sprocketLayout(filmType,filmLayout,reverseImage);

      System.out.println("   Film Layout:");
      System.out.println("      Film format is " + (filmType == FilmSpec.eightMMFilmType ? "regular 8mm." : "super 8"));
      System.out.println("      " + FilmSpec.filmLayoutDesc[filmLayout]);
      if (reverseImage)
         System.out.println("      Film is mirrored.");
      if (reverseImage)
	  System.out.println("      This means the sprocket holes are expected to be along the " + 
			     FilmSpec.sprocketLayoutFromFilmLayoutRevDesc[filmLayout] + " side of the image.");
      else
	  System.out.println("      This means the sprocket holes are expected to be along the " + 
			     FilmSpec.sprocketLayoutFromFilmLayoutDesc[filmLayout] + " side of the film image.");
      System.out.println("---------------------------------------------------");

      tmps = cl.getProperty("di");
      if (tmps != null)
         writeDebugImages = 
            tmps.equalsIgnoreCase("true") || tmps.equalsIgnoreCase("t") ||
            tmps.equalsIgnoreCase("yes") || tmps.equalsIgnoreCase("y") ||
            tmps.equalsIgnoreCase("1");

      return true;
   }

   private static void usage()
   {
      System.out.println(
         "usage: java [javaargs] ExtractFrames -f filename -lr|-rl|-tb|-bt [-r " + resolutiondpi + 
         "] [-cs " + defaultTileCacheSize + "] [-tl " + tlow + "] [-th " + thigh + "] [-sigma " + 
         sigma + "] [-ht " + houghThreshold + "] [-rev] [-8mm] [-fw #pix] [-fh #pix] [-over " + 
         frameOversizeMult + "] [-norotation] [-norescale] [-wm] [-ot jpeg] [-di]");
      System.out.println();
      System.out.println("  -f this is how you supply filename of the source image.");
      System.out.println("  -lr|-rl|-tb|-bt specifies the orientation of the image. This orientation can be:");
      System.out.println("     left to right");
      System.out.println("     right to left");
      System.out.println("     top to bottom");
      System.out.println("     bottom to top");
      System.out.println("        the orientation mut be supplied.");
      System.out.println("  -rev means the strip was scanned inverted so the mirror image appears.");
      System.out.println("  -r specify the resolution (in dpi) of the source image.");
      System.out.println("  -cs image tile cache size in mega bytes.");
      System.out.println("  -th high threshold % for the canny edge detector.");
      System.out.println("  -tl low threshold % for the canny edge detector.");
      System.out.println("  -sigma std dev for the canny edge detector.");
      System.out.println("  -ht threshold for the pseudo-generalized hough transform.");
      System.out.println("  -8mm is specified to set film model for regular 8mm file. Default is super 8.");
      System.out.println("  -fw Is the final frame width in pixels. This defaults to a calculated");
      System.out.println("      value based on the supplied resolution and the film specifications.");
      System.out.println("  -fw Is the final frame height in pixels. This defaults to a calculated");
      System.out.println("      value based on the supplied resolution and the film specifications.");
      System.out.println("  -over is a multiplication factor for exceeding the bounds of the frame.");
      System.out.println("  -norotation this prevents the rotation of the original source into the destination frame");
      System.out.println("      This will avoid pixellation issues but may cause jitters at strip transitions.");
      System.out.println("  -norescale this prevents the scaling of the original source into the destination frame");
      System.out.println("      This will avoid pixellation issues but may cause jitters at strip transitions.");
      System.out.println("  -wm set the reference image and frame into the final frame images as a watermark.");
      System.out.println("  -ot set the output file type (and extention).");
      System.out.println("  -di writes out tmp*.bmp debug images.");
//      System.out.println("  -clamp The grayscale image has a lower clamp applied. You can change this level.");
   }

   static private ColorModel getOverlayCM()
   {
      byte [] r = new byte[256];
      byte [] g = new byte[256];
      byte [] b = new byte[256];

      r[intify(EdgeDetectorOpImage.EDGE)] = 
         g[intify(EdgeDetectorOpImage.EDGE)] = 
         b[intify(EdgeDetectorOpImage.EDGE)] = -1;

      r[intify(AddOverlayOpImage.ROVERLAY)] = -1;
      g[intify(AddOverlayOpImage.GOVERLAY)] = -1;
      b[intify(AddOverlayOpImage.BOVERLAY)] = -1;

      r[intify(AddOverlayOpImage.YOVERLAY)] = -1;
      g[intify(AddOverlayOpImage.YOVERLAY)] = -1;

      r[intify(AddOverlayOpImage.COVERLAY)] = byteify(Color.cyan.getRed());
      g[intify(AddOverlayOpImage.COVERLAY)] = byteify(Color.cyan.getGreen());
      b[intify(AddOverlayOpImage.COVERLAY)] = byteify(Color.cyan.getBlue());

      r[intify(AddOverlayOpImage.MOVERLAY)] = byteify(Color.magenta.getRed());
      g[intify(AddOverlayOpImage.MOVERLAY)] = byteify(Color.magenta.getGreen());
      b[intify(AddOverlayOpImage.MOVERLAY)] = byteify(Color.magenta.getBlue());

      r[intify(AddOverlayOpImage.OOVERLAY)] = byteify(Color.orange.getRed());
      g[intify(AddOverlayOpImage.OOVERLAY)] = byteify(Color.orange.getGreen());
      b[intify(AddOverlayOpImage.OOVERLAY)] = byteify(Color.orange.getBlue());

      r[intify(AddOverlayOpImage.GRAYOVERLAY)] = byteify(Color.gray.getRed());
      g[intify(AddOverlayOpImage.GRAYOVERLAY)] = byteify(Color.gray.getGreen());
      b[intify(AddOverlayOpImage.GRAYOVERLAY)] = byteify(Color.gray.getBlue());

      return new IndexColorModel(8,256,r,g,b);
   }

   private static byte byteify(int i) {
      return i > 127 ? (byte)(i - 256) : (byte)i;
   }

   private static int intify(byte b) {
      return (b < 0) ? ((int)b) + 256 : (int)b;
   }

}

