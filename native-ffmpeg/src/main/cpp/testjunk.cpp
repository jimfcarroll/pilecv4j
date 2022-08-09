//#include <iostream>
//#include <vector>
//
//#include "utils/IMakerManager.h"
//#include "utils/log.h"
//
//extern "C" {
//#include <libavformat/avformat.h>
//#include <libavcodec/avcodec.h>
//#include <libavutil/imgutils.h>
//#include <libswscale/swscale.h>
//}
//
//#include <atomic>
//
//void initialize_avformat_context(AVFormatContext *&fctx, const char *format_name, const char* output)
//{
//  int ret = avformat_alloc_output_context2(&fctx, nullptr, format_name, output);
//  if (ret < 0)
//  {
//    std::cout << "Could not allocate output format context!" << std::endl;
//    exit(1);
//  }
//}
//
//void initialize_io_context(AVFormatContext *&fctx, const char *output)
//{
//  if (!(fctx->oformat->flags & AVFMT_NOFILE))
//  {
//    int ret = avio_open2(&fctx->pb, output, AVIO_FLAG_WRITE, nullptr, nullptr);
//    if (ret < 0)
//    {
//      std::cout << "Could not open output IO context!" << std::endl;
//      exit(1);
//    }
//  }
//}
//
//void set_codec_params(AVFormatContext *&fctx, AVCodecContext *&codec_ctx, double width, double height, int fps)
//{
//  const AVRational dst_fps = {fps, 1};
//
//  codec_ctx->codec_tag = 0;
//  codec_ctx->codec_id = AV_CODEC_ID_H264;
//  codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
//  codec_ctx->width = width;
//  codec_ctx->height = height;
//  codec_ctx->gop_size = 12;
//  codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
//  codec_ctx->framerate = dst_fps;
//  codec_ctx->time_base = av_inv_q(dst_fps);
//  if (fctx->oformat->flags & AVFMT_GLOBALHEADER)
//  {
//    codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
//  }
//}
//
//void initialize_codec_stream(AVStream *&stream, AVCodecContext *&codec_ctx, AVCodec *&codec)
//{
//  int ret = avcodec_parameters_from_context(stream->codecpar, codec_ctx);
//  if (ret < 0)
//  {
//    std::cout << "Could not initialize stream codec parameters!" << std::endl;
//    exit(1);
//  }
//
//  AVDictionary *codec_options = nullptr;
//  av_dict_set(&codec_options, "profile", "high", 0);
//  av_dict_set(&codec_options, "preset", "superfast", 0);
//  av_dict_set(&codec_options, "tune", "zerolatency", 0);
//
//  // open video encoder
//  ret = avcodec_open2(codec_ctx, codec, &codec_options);
//  if (ret < 0)
//  {
//    std::cout << "Could not open video encoder!" << std::endl;
//    exit(1);
//  }
//}
//
//SwsContext *initialize_sample_scaler(AVCodecContext *codec_ctx, double width, double height)
//{
//  pilecv4j::log(pilecv4j::TRACE, "test", "Pix fmt: %d", (int)codec_ctx->pix_fmt);
//
//  SwsContext *swsctx = sws_getContext(width, height, AV_PIX_FMT_RGB24, width, height, codec_ctx->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr);
//  if (!swsctx)
//  {
//    std::cout << "Could not initialize sample scaler!" << std::endl;
//    exit(1);
//  }
//
//  return swsctx;
//}
//
//AVFrame *allocate_frame_buffer(AVCodecContext *codec_ctx, double width, double height)
//{
//  AVFrame *frame = av_frame_alloc();
//
//  std::vector<uint8_t> framebuf(av_image_get_buffer_size(codec_ctx->pix_fmt, width, height, 1));
//  av_image_fill_arrays(frame->data, frame->linesize, framebuf.data(), codec_ctx->pix_fmt, width, height, 1);
//  frame->width = width;
//  frame->height = height;
//  frame->format = static_cast<int>(codec_ctx->pix_fmt);
//
//  return frame;
//}
//
//void write_frame(AVCodecContext *codec_ctx, AVFormatContext *fmt_ctx, AVFrame *frame, AVStream* out_stream)
//{
//  AVPacket pkt = {0};
//  av_init_packet(&pkt);
//
//  int ret = avcodec_send_frame(codec_ctx, frame);
//
//  if (ret < 0)
//  {
//    std::cout << "Error sending frame to codec context!" << std::endl;
//    exit(1);
//  }
//
//  ret = avcodec_receive_packet(codec_ctx, &pkt);
//  if (ret < 0)
//  {
//    std::cout << "Error receiving packet from codec context!" << std::endl;
//    exit(1);
//  }
//
//  pilecv4j::log(pilecv4j::TRACE, "test", "Output Packet Timing[stream %d]: pts/dts: [ %" PRId64 "/ %" PRId64 " ] duration: %" PRId64 " timebase: [ %d / %d ]",
//      (int) pkt.stream_index,
//      (int64_t)pkt.pts, (int64_t)pkt.dts,
//      (int64_t)pkt.duration,
//      (int)out_stream->time_base.num, (int)out_stream->time_base.den);
//
//  av_interleaved_write_frame(fmt_ctx, &pkt);
//  av_packet_unref(&pkt);
//}
//
//typedef uint64_t (*get_frame)();
//static get_frame callback;
//
//void stream_video(double width, double height, int fps, bool isRgb)
//{
//  av_register_all();
//  avformat_network_init();
//
//  uint64_t framecount = 0;
//
//  const char *output = "rtmp://localhost:1935/live/feedly-id";
//  int ret;
//  std::vector<uint8_t> imgbuf(height * width * 3 + 16);
//  AVFormatContext *ofmt_ctx = nullptr;
//  AVCodec *out_codec = nullptr;
//  AVStream *out_stream = nullptr;
//  AVCodecContext *out_codec_ctx = nullptr;
//
//  initialize_avformat_context(ofmt_ctx, "flv", output);
//  initialize_io_context(ofmt_ctx, output);
//
//  out_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
//  out_stream = avformat_new_stream(ofmt_ctx, out_codec);
//  out_codec_ctx = avcodec_alloc_context3(out_codec);
//
//  set_codec_params(ofmt_ctx, out_codec_ctx, width, height, fps);
//  initialize_codec_stream(out_stream, out_codec_ctx, out_codec);
//
//  out_stream->codecpar->extradata = out_codec_ctx->extradata;
//  out_stream->codecpar->extradata_size = out_codec_ctx->extradata_size;
//
//  av_dump_format(ofmt_ctx, 0, output, 1);
//
//  auto *swsctx = initialize_sample_scaler(out_codec_ctx, width, height);
//  //auto *frame = allocate_frame_buffer(out_codec_ctx, width, height);
//
//  int cur_size;
//  uint8_t *cur_ptr;
//
//  ret = avformat_write_header(ofmt_ctx, nullptr);
//  if (ret < 0)
//  {
//    std::cout << "Could not write header!" << std::endl;
//    exit(1);
//  }
//
//  bool end_of_stream = false;
//
////  // ============================
//  auto im = pilecv4j::IMakerManager::getIMaker();
////  ai::kognition::pilecv4j::RawRaster rr;
////  im->extractImageDetails(matRef, isRgb ? true : false, &rr);
////  pilecv4j::IMakerManager::Transform xform;
////  xform.h = rr.h;
////  xform.w = rr.w;
////  xform.stride = rr.stride;
////  xform.origFmt = isRgb ? ai::kognition::pilecv4j::RGB24 : ai::kognition::pilecv4j::BGR24;
////  xform.srcFmt = pilecv4j::IMakerManager::convert(xform.origFmt);
////  xform.conversion = swsctx;
////  xform.supportsCurFormat = false;
////  // ============================
//
//  do
//  {
//    // get an image -> image
//    uint64_t matRef = (*callback)();
//    ai::kognition::pilecv4j::RawRaster rr;
//    im->extractImageDetails(matRef, isRgb ? true : false, &rr);
//
//    pilecv4j::log(pilecv4j::TRACE, "test", "image data @ %" PRId64 ": %" PRId64 ", [ %d x %d ]", (uint64_t)matRef, (uint64_t)( rr.data), (int)rr.w, (int)rr.h );
//
//    auto *frame = allocate_frame_buffer(out_codec_ctx, width, height);
//    // do this
//    //const int stride[] = {static_cast<int>(image.step[0])};
//    const int stride[] = {static_cast<int>(rr.stride)};
//    // then this
//    // sws_scale(swsctx, &image.data, stride, 0, image.rows, frame->data, frame->linesize);
//    sws_scale(swsctx,(const uint8_t* const*)(&(rr.data)), stride, 0, rr.h, frame->data, frame->linesize);
//    frame->pts = framecount * av_rescale_q(1, out_codec_ctx->time_base, out_stream->time_base);
//    framecount++;
//    write_frame(out_codec_ctx, ofmt_ctx, frame, out_stream);
//
//    av_frame_free(&frame);
//    im->freeImage(matRef);
//  } while (!end_of_stream);
//
//  av_write_trailer(ofmt_ctx);
//
//  //av_frame_free(&frame);
//  avcodec_close(out_codec_ctx);
//  avio_close(ofmt_ctx->pb);
//  avformat_free_context(ofmt_ctx);
//}
//
////int main()
////{
////  // av_log_set_level(AV_LOG_DEBUG);
////  double width = 1280, height = 720;
////  int camID = 1, fps = 25;
////
////  stream_video(width, height, fps, camID, true);
////
////  return 0;
////}
////
//using namespace pilecv4j;
//
//extern "C" {
//void startme(uint64_t mat, int32_t isRgb, get_frame cb) {
//
//  auto im = IMakerManager::getIMaker();
//
//  ai::kognition::pilecv4j::RawRaster rr;
//  im->extractImageDetails(mat, isRgb ? true : false, &rr);
//  pilecv4j::log(pilecv4j::TRACE, "test", "initial image data @ %" PRId64 ": %" PRId64 ", [ %d x %d ]", (uint64_t)mat, (uint64_t)( rr.data), (int)rr.w, (int)rr.h );
//
//  callback = cb;
//  stream_video(rr.w, rr.h, 30, isRgb ? true : false);
//
//}
//}
//
//
