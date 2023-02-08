
#ifndef _ENCODER_H_
#define _ENCODER_H_

#include "utils/IMakerManager.h"

#include <atomic>
#include <string>
#include <vector>
#include <map>

extern "C" {
#include <libavformat/avformat.h>
}

#define DEFAULT_MAX_REMUX_ERRORS 20
#define DEFAULT_FPS 30

namespace pilecv4j
{
namespace ffmpeg
{

enum EncoderState {
  ENC_FRESH = 0,
  ENC_OPEN_CONTEXT,
  ENC_OPEN_STREAMS,
  ENC_READY,
  ENC_ENCODING,
  ENC_STOPPED
};

enum VideoEncoderState {
  VE_FRESH = 0,
  VE_SET_UP,
  VE_ENCODING,
  VE_STOPPED
};

class EncodingContext;
class Synchronizer;
class Muxer;

/**
 * This class is NOT thread safe. ALL calls to the VideoEncoder and the EncodingContext
 * should be done from the same thread. The one exception is EncodingContext::stop which
 * can be called from a separate thread.
 */
class VideoEncoder {
  EncodingContext* enc;
  std::string video_codec;

  std::map<std::string,std::string> options;
  VideoEncoderState state = VE_FRESH;

  // ==================================
  // valid once state becomes VE_ENABLED
  AVCodec* video_avc = nullptr;
  int video_sindex = -1;
  AVRational video_stime_base;
  AVCodecContext* video_avcc = nullptr;
  // ==================================

  int32_t maxRemuxErrorCount = DEFAULT_MAX_REMUX_ERRORS;
  int64_t framecount = 0;

  IMakerManager::Transform xform;

  // ==================================
  // encoding parameters
  int fps = DEFAULT_FPS;
  int bufferSize = -1;
  int64_t minBitrate = -1;
  int64_t maxBitrate = -1;
  // ==================================

  AVFrame* frame = nullptr;
  AVPacket output_packet = {0};

  // ==================================
  // stupid hack
  uint8_t* streams_original_extradata = nullptr;
  int streams_original_extradata_size = 0;
  bool streams_original_set = false;
  // ==================================

  Synchronizer* sync = nullptr;
public:
  inline VideoEncoder(EncodingContext* penc, const char* pvideo_codec) : enc(penc), video_codec(pvideo_codec) { }
  ~VideoEncoder();

  uint64_t addCodecOption(const char* key, const char* val);

  inline uint64_t setFps(int pfps) {
    fps = pfps;
    return 0;
  }

  inline uint64_t setBufferSize(int pbufferSize) {
    bufferSize = pbufferSize;
    return 0;
  }

  inline uint64_t setBitrate(int64_t pminBitrate, int64_t pmaxBitrate = -1) {
    minBitrate = pminBitrate;
    maxBitrate = (pmaxBitrate < 0) ? minBitrate : pmaxBitrate;
    return 0;
  }

  inline uint64_t setEncodingParameters(int pfps, int pbufferSize, int64_t pminBitrate, int64_t pmaxBitrate = -1) {
    setFps(pfps);
    setBufferSize(pbufferSize);
    setBitrate(pminBitrate, pmaxBitrate);
    return 0;
  }

  uint64_t enable(uint64_t matRef, bool isRgb, int dstW, int dstH);

  uint64_t enable(bool isRgb, int width, int height, size_t stride, int dstW, int dstH);

  uint64_t encode(uint64_t matRef, bool isRgb);

  uint64_t streaming();

  /**
   * Called from the EncodingContext after everything else in EncodingContext::ready() is done
   * as a notification. It will take the opportunity to set the time_base from the stream post
   * avformat_write_header.
   */
  uint64_t ready();

  uint64_t stop();
};


/**
 * This class is NOT thread safe. ALL calls to the VideoEncoder and the EncodingContext
 * should be done from the same thread. The one exception is EncodingContext::stop which
 * can be called from a separate thread.
 */
class EncodingContext
{
  Muxer* muxer = nullptr;
  EncoderState state = ENC_FRESH;

  friend class VideoEncoder;

  std::atomic<bool> fake_mutex;
  std::vector<VideoEncoder*> encoders;
public:
  inline EncodingContext() {
    fake_mutex = false;
  }
  ~EncodingContext();

  uint64_t setMuxer(Muxer* pmuxer);

  inline VideoEncoder* openVideoEncoder(const char* video_codec) {
    auto ret = new VideoEncoder(this, video_codec);
    encoders.push_back(ret);
    return ret;
  }

  /**
   * Tell the EncodingContext that all of the encoders are configured and ready
   */
  uint64_t ready();

  uint64_t stop();
};

}

} /* namespace pilecv4j */

#endif /* _ENCODER_H_ */
