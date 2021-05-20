#include "pilecv4j_utils.h"
#include "imagemaker.h"

extern "C" 
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/error.h"
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
}
#include <stdlib.h>
#include <vector>
#include <tuple>
#include <chrono>
#include <thread>

#undef av_err2str
#define av_err2str(errnum) av_make_error_string((char*)__builtin_alloca(AV_ERROR_MAX_STRING_SIZE),AV_ERROR_MAX_STRING_SIZE, errnum)

// =====================================================
// Definition of all of the callbacks used.
/**
 * This one is what we push decoded frames to.
 */
typedef void (*push_frame)(uint64_t frame, int32_t isRgb);

/**
 * For custom IO, this will be responsible for reading bytes from the stream
 */
typedef int32_t (*fill_buffer)(int32_t numBytesMax);

/**
 * For custom IO, this will be responsible for seeking within a stream.
 */
typedef int64_t (*seek_buffer)(int64_t offset, int whence);
// =====================================================

// =====================================================
/**
 * Status codes generated by this code. Orthogonal to AV_ERROR codes.
 */
enum Pcv4jStat {
  OK = 0,
  STREAM_IN_USE = 1,
  STREAM_BAD_STATE = 2,
  NO_VIDEO_STREAM = 3,
  NO_SUPPORTED_CODEC = 4,
  FAILED_CREATE_CODEC_CONTEXT = 5,
  FAILED_CREATE_FRAME = 6,
  FAILED_CREATE_PACKET = 7,
  LOGGING_NOT_COMPILED = 8,
  ADD_OPTION_TOO_LATE = 9
};
#define MAX_PCV4J_CODE 9
// status message descriptions corresponding to the custom codes.
static const char* pcv4jStatMessages[MAX_PCV4J_CODE + 1] = {
    "OK",
    "Can't open another stream with the same context",
    "Context not in correct state for given operation",
    "Couldn't find a video stream in the given source",
    "No supported video codecs available for the given source",
    "Failed to create a codec context",
    "Failed to create a frame",
    "Failed to create a packet",
    "Logging isn't compiled.",
    "Can't add an option after opening a stream."
};
// =====================================================

// =====================================================
// Utilities: logging
// =====================================================
/**
 * Log levels and log level names.
 */
enum LogLevel {
  TRACE=0,
  DEBUG=1,
  INFO=2,
  WARN=3,
  ERROR=4,
  FATAL=5
};

static const char* logLevelNames[] = {
    "TRACE",
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR",
    "FATAL",
    nullptr
};
#define PCV4J_MAX_LOG_LEVEL 5
// =====================================================

// ==================================
// defaults and constants
#define PCV4J_CUSTOMIO_BUFSIZE 8192
#define DEFAULT_MAX_DELAY_MILLIS 1000

static AVRational millisecondTimeBase = AVRational{1,1000};
// ==================================

// =====================================================
// =====================================================
/**
 * The StreamContext structure. This holds all of the state information associated with
 * a stream.
 */
// ==================================
/**
 * The StreamContext goes through a set of states in its lifecycle.
 * These are those states.
 */
enum StreamContextState {
  FRESH,
  OPEN,
  CODEC,
  PLAY,
  STOP
};

struct StreamContext {
  /**
   * The timebase for the selected stream. This is available after state=CODEC
   */
  AVRational streamTimebase;

  /**
   * Container context. Available after state=OPEN
   */
  AVFormatContext* formatCtx = nullptr;

  //========================================================
  // This is for using a custom data source.
  /**
   * When supplying your own data for decode, these will be set after state=OPEN.
   */
  AVIOContext* ioContext = nullptr;
  uint8_t* ioBuffer = nullptr;

  fill_buffer ioCallbackReadBuffer = nullptr;
  seek_buffer ioCallbackSeekBuffer = nullptr;

  uint8_t* ioBufferToFillFromJava = nullptr;
  //========================================================

  /**
   * Codec context. This is available after state=CODEC
   */
  AVCodecContext* codecCtx = nullptr;

  //========================================================================
  /**
   * Color converter to BGR/RGB. Available only after beginning play
   */
  SwsContext* colorCvrt = nullptr;
  AVPixelFormat lastFormatUsed;
  //========================================================================

  /**
   * Set of user specified options (e.g. rtsp_transport = tcp). Available after state=OPEN
   */
  std::vector<std::tuple<std::string,std::string> > options;

  /**
   * Current log level for this context. Set by explicit call. Can be set in any state.
   */
  LogLevel logLevel = INFO;

  /**
   * In the container (formatCtx) which stream is the video stream. This is available after state=CODEC
   */
  int streamIndex = -1;

  /**
   * Current state.
   */
  StreamContextState state = FRESH;

  /**
   * Should we sync playback to the wall clock?
   */
  bool sync = false;

  /**
   * When sync = true, what's the max delay before we need to start discarding frames
   * by skipping calls to the frame callback in order to try to catch up.
   */
  uint64_t maxDelayMillisBeforeDroppingFrame = DEFAULT_MAX_DELAY_MILLIS;

  /**
   * flag that tells the playing loop to exit.
   */
  bool stop = false;

  int64_t whenToDisplayNextFrameMillis = -1;
  int64_t startPlayTime = -1;

  inline uint64_t addOption(const char* key, const char* val) {
    options.push_back(std::tuple<std::string, std::string>(key, val));
    return 0;
  }

  inline void buildOptions(AVDictionary** opts) {
    if (options.size() == 0) {
      *opts = nullptr;
      return;
    }

    for (auto o : options) {
      av_dict_set(opts, std::get<0>(o).c_str(), std::get<1>(o).c_str(), 0 );
    }
  }

  inline void setSync(int32_t doIt) {
    sync = doIt == 0 ? false : true;
  }

  inline ~StreamContext() {
    if (colorCvrt != nullptr)
      sws_freeContext(colorCvrt);
    if (codecCtx != nullptr)
      avcodec_free_context(&codecCtx);
    //========================================================================
    // This is to compensate for a bug (or stupidity) in FFMpeg. See :
    // https://stackoverflow.com/questions/9604633/reading-a-file-located-in-memory-with-libavformat
    // ... and search for "double free". Then ALSO follow the link to
    // https://lists.ffmpeg.org/pipermail/libav-user/2012-December/003257.html
    // Then look at aviobuf.c in the source code and search for the function
    // definition for ffio_set_buf_size. You can see that if Ffmpeg  decides
    // to shrink the buffer, it will reallocate a buffer and free the one that's
    // there already.
    if (ioContext) {
      if (ioBuffer && ioContext->buffer == ioBuffer)
        av_free(ioBuffer);
      else
        av_free(ioContext->buffer);
      av_free(ioContext);
    }
    //========================================================================
    if (ioBufferToFillFromJava)
      free(ioBufferToFillFromJava);
    if (formatCtx != nullptr)
      avformat_free_context(formatCtx);
  }

};
// =====================================================

//======================================================
/**
 * equivalent of java's System.currentTimeMillis.
 */
static inline int64_t now() {
  return static_cast<int64_t>(std::chrono::duration_cast< std::chrono::milliseconds >(
      std::chrono::system_clock::now().time_since_epoch()
  ).count());
}
// =====================================================

//========================================================================
/**
 * Logging. Can be compiled out depending on settings.
 */
#ifdef LOGGING
static inline void log(const StreamContext * const ctx, LogLevel llevel, const char *fmt, ...)
{
  if (ctx->logLevel <= llevel) {
    va_list args;
    fputs( "Ffmpeg_wrapper: [", stderr );
    fputs( logLevelNames[llevel], stderr );
    fputs( "] ", stderr);
    va_start( args, fmt );
    vfprintf( stderr, fmt, args );
    va_end( args );
    fputs( "\n", stderr );
  }
}
#else
inline static void log(...) {}
#endif

//========================================================================
/**
 * Error and status support.
 */
static const char* totallyUnknownError = "UNKNOWN ERROR";

#define MAKE_AV_STAT(x) ((uint64_t)((int32_t)x) & 0xffffffff)
#define MAKE_P_STAT(x) ((((uint64_t)((int32_t)x) & 0xffffffff)) << 32);

static inline bool isError(uint64_t stat) {
  // 0 is good and we expect this most of the time so check it first.
  if (stat == 0)
    return false;
  // if the MSBs are non zero then there's a PCV4J error
  if (stat & 0xffffffff00000000L)
    return true;
  // if the LSBs contain negative value then there's an AV error.
  //                   v
  if (stat & 0x0000000080000000L)
    return true;
  return false;
}

static AVPixelFormat upgradePixFormatIfNecessary(StreamContext* c, AVPixelFormat cur) {
  AVPixelFormat pixFormat;
  bool upgraded = true;
  switch (cur) {
  case AV_PIX_FMT_YUVJ420P :
      pixFormat = AV_PIX_FMT_YUV420P;
      break;
  case AV_PIX_FMT_YUVJ422P  :
      pixFormat = AV_PIX_FMT_YUV422P;
      break;
  case AV_PIX_FMT_YUVJ444P   :
      pixFormat = AV_PIX_FMT_YUV444P;
      break;
  case AV_PIX_FMT_YUVJ440P :
      pixFormat = AV_PIX_FMT_YUV440P;
      break;
  default:
      upgraded = false;
      pixFormat = cur;
      break;
  }
  if (upgraded && c->logLevel <= DEBUG)
    log(c, TRACE, "Upgrading pixel format from %d to %d", cur, pixFormat);
  return pixFormat;
}

//========================================================================
/**
 * AV compliant callbacks for custom IO.
 */
static int read_packet_from_custom_source(void *opaque, uint8_t *buf, int buf_size) {
  StreamContext const* c = (StreamContext*)opaque;
  const void* bufForCallback = c->ioBufferToFillFromJava;
  fill_buffer callback = c->ioCallbackReadBuffer;
  int numBytesRead = static_cast<int32_t>((*callback)(buf_size));
  if (numBytesRead < 0) {
    log(c, DEBUG, "java call to read bytes returned an error code: %s", av_err2str(numBytesRead));
    return numBytesRead;
  }

  log(c, TRACE, "num bytes read: %d", numBytesRead);
  if (numBytesRead != 0) {
    if (numBytesRead > buf_size) {
      log(c, ERROR, "Too many bytes (%d) written when the buffer size is only %d", numBytesRead, buf_size);
      numBytesRead = 0;
    } else {
      memcpy(buf, bufForCallback, numBytesRead);
    }
  }
  return numBytesRead == 0 ? AVERROR(EAGAIN) : numBytesRead;
}

static int64_t seek_in_custom_source(void *opaque, int64_t offset, int whence) {
  StreamContext const* c = (StreamContext*)opaque;
  seek_buffer seek = c->ioCallbackSeekBuffer;

  int64_t ret = (*seek)(offset, whence);
  log(c, DEBUG, "seeking to %ld from 0x%x, results: %ld", (long)offset, (int)whence, (long)ret);
  return ret;
}
//========================================================================

//========================================================================
// Some local helper method's definitions
//========================================================================
static uint64_t findFirstVidCodec(StreamContext* c,
    AVFormatContext* pFormatContext, AVCodec** pCodec,AVCodecParameters** pCodecParameters,
    int* video_stream_index, AVRational* timebase);
static uint64_t decode_packet(StreamContext* c, AVCodecContext *pCodecContext,
    AVFrame *pFrame, AVPacket *pPacket, push_frame callback,
    SwsContext** colorCvrt);
static uint64_t open_stream(uint64_t ctx, const char* url, fill_buffer readCallback,
    seek_buffer seekCallback);
//========================================================================

//========================================================================
// This is the bridge to lib-image that circumvents a compile dependency
static ai::kognition::pilecv4j::ImageMaker* imaker;
//========================================================================

//========================================================================
// Everything here in this extern "C" section is callable from Java
//========================================================================
extern "C" {

  int32_t pcv4j_ffmpeg_init() {
    return 0;
  }

  char* pcv4j_ffmpeg_statusMessage(uint64_t status) {
    // if the MSBs have a value, then that's what we're going with.
    {
      uint32_t pcv4jCode = (status >> 32) & 0xffffffff;
      if (pcv4jCode != 0) {
        if (pcv4jCode < 0 || pcv4jCode > MAX_PCV4J_CODE)
          return strdup(totallyUnknownError);
        else
          return strdup(pcv4jStatMessages[pcv4jCode]);
      }

    }
    char* ret = new char[AV_ERROR_MAX_STRING_SIZE + 1]{0};
    av_strerror(status, ret, AV_ERROR_MAX_STRING_SIZE);
    return ret;
  }

  void pcv4j_ffmpeg_freeString(char* str) {
    if (str)
      delete[] str;
  }

  uint64_t pcv4j_ffmpeg_createContext() {
    return (uint64_t) new StreamContext();
  }

  void pcv4j_ffmpeg_deleteContext(uint64_t ctx) {
    StreamContext* c = (StreamContext*)ctx;
    delete c;
  }

  void* pcv4j_ffmpeg_customStreamBuffer(uint64_t ctx) {
    StreamContext* c = (StreamContext*)ctx;
    if (c->ioBufferToFillFromJava) {
      free(c->ioBufferToFillFromJava);
    }
    c->ioBufferToFillFromJava = (uint8_t*)malloc(PCV4J_CUSTOMIO_BUFSIZE * sizeof(uint8_t));
    return c->ioBufferToFillFromJava;
  }

  int32_t pcv4j_ffmpeg_customStreamBufferSize(uint64_t ctx) {
    return PCV4J_CUSTOMIO_BUFSIZE;
  }

  uint64_t pcv4j_ffmpeg_openCustomStream(uint64_t ctx, fill_buffer callback, seek_buffer seekCallback) {
    return open_stream(ctx,nullptr,callback,seekCallback);
  }

  uint64_t pcv4j_ffmpeg_openStream(uint64_t ctx, const char* url) {
    return open_stream(ctx,url,nullptr,nullptr);
  }

  uint64_t pcv4j_ffmpeg_findFirstVideoStream(uint64_t ctx) {
    StreamContext* c = (StreamContext*)ctx;
    if (c->state != OPEN) {
      log(c, ERROR, "StreamContext is in the wrong state. It should have been in %d but it's in %d.", (int)OPEN, (int)c->state);
      return MAKE_P_STAT(STREAM_BAD_STATE);
    }

    uint64_t stat = MAKE_AV_STAT(avformat_find_stream_info(c->formatCtx, nullptr));
    if (isError(stat))
      return stat;

    // the component that knows how to enCOde and DECode the stream
    // it's the codec (audio or video)
    // http://ffmpeg.org/doxygen/trunk/structAVCodec.html
    AVCodec *pCodec = NULL;
    // this component describes the properties of a codec used by the stream
    // https://ffmpeg.org/doxygen/trunk/structAVCodecParameters.html
    AVCodecParameters *pCodecParameters =  NULL;
    int video_stream_index = -1;

    AVFormatContext* pFormatContext = c->formatCtx;
    AVRational timebase{1,1};

    stat = findFirstVidCodec(c, pFormatContext, &pCodec, &pCodecParameters, &video_stream_index, &timebase);
    if (isError(stat))
      return stat;

    // https://ffmpeg.org/doxygen/trunk/structAVCodecContext.html
    c->codecCtx = avcodec_alloc_context3(pCodec);
    if (!c->codecCtx) {
      log(c, ERROR, "failed to allocated memory for AVCodecContext");
      return MAKE_P_STAT(FAILED_CREATE_CODEC_CONTEXT);
    }
    AVCodecContext* pCodecContext = c->codecCtx;

    // Fill the codec context based on the values from the supplied codec parameters
    // https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#gac7b282f51540ca7a99416a3ba6ee0d16
    stat = MAKE_AV_STAT(avcodec_parameters_to_context(pCodecContext, pCodecParameters));
    if (isError(stat))
      return stat;

    // Initialize the AVCodecContext to use the given AVCodec.
    // https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#ga11f785a188d7d9df71621001465b0f1d
    AVDictionary* opts = nullptr;
    c->buildOptions(&opts);
    stat = avcodec_open2(pCodecContext, pCodec, &opts);
    if (opts != nullptr)
      av_dict_free(&opts);
    if (isError(stat))
    {
      log(c, ERROR, "failed to open codec through avcodec_open2");
      return stat;
    }

    c->streamIndex = video_stream_index;
    c->streamTimebase = timebase;
    c->state = CODEC;

    return stat;
  }

  uint64_t pcv4j_ffmpeg_process_frames(uint64_t ctx, push_frame callback) {
    StreamContext* c = (StreamContext*)ctx;
    if (c->state != CODEC) {
      log(c, ERROR, "StreamContext is in the wrong state. It should have been in %d but it's in %d.", (int)CODEC, (int)c->state);
      return MAKE_P_STAT(STREAM_BAD_STATE);
    }

    c->state = PLAY;
    const bool sync = c->sync;

    // https://ffmpeg.org/doxygen/trunk/structAVFrame.html
    AVFrame *pFrame = av_frame_alloc();
    if (!pFrame)
    {
      log(c, ERROR, "failed to allocated memory for AVFrame");
      return MAKE_P_STAT(FAILED_CREATE_FRAME);
    }
    // https://ffmpeg.org/doxygen/trunk/structAVPacket.html
    AVPacket *pPacket = av_packet_alloc();
    if (!pPacket)
    {
      log(c, ERROR, "failed to allocated memory for AVPacket");
      return MAKE_P_STAT(FAILED_CREATE_PACKET);
    }

    uint64_t response = 0;

    AVCodecContext* pCodecContext = c->codecCtx;
    AVFormatContext* pFormatContext = c->formatCtx;
    const int video_stream_index = c->streamIndex;

    // fill the Packet with data from the Stream
    // https://ffmpeg.org/doxygen/trunk/group__lavf__decoding.html#ga4fdb3084415a82e3810de6ee60e46a61
    int lastResult = 0;
    SwsContext** colorCvrt = &(c->colorCvrt);

    if (sync)
      c->startPlayTime = now();

    while ((lastResult = av_read_frame(pFormatContext, pPacket)) >= 0 && !c->stop)
    {
      // if it's the video stream
      if (pPacket->stream_index == video_stream_index) {
        log(c, TRACE, "AVPacket->pts %" PRId64, pPacket->pts);
        response = decode_packet(c, pCodecContext, pFrame, pPacket, callback, colorCvrt);
        if (isError(response))
          break;
      }
      // https://ffmpeg.org/doxygen/trunk/group__lavc__packet.html#ga63d5a489b419bd5d45cfd09091cbcbc2
      av_packet_unref(pPacket);
    }

    log(c, INFO, "Last result of read was: %s", av_err2str(lastResult));
    log(c, INFO, "releasing all the resources");

    av_packet_free(&pPacket);
    av_frame_free(&pFrame);

    return lastResult;
  }

  long pcv4j_ffmpeg_set_log_level(uint64_t ctx, int32_t logLevel) {
    StreamContext* c = (StreamContext*)ctx;
    if (logLevel <= PCV4J_MAX_LOG_LEVEL && logLevel >= 0)
      c->logLevel = static_cast<LogLevel>(logLevel);
    else
      c->logLevel = FATAL;
    return 0;
  }

  void pcv4j_ffmpeg_add_option(uint64_t ctx, const char* key, const char* value) {
    StreamContext* c = (StreamContext*)ctx;
    log(c, INFO, "Setting option \"%s\" = \"%s\"",key,value);
    c->addOption(key, value);
  }

  void pcv4j_ffmpeg_set_syc(uint64_t ctx, int32_t doIt) {
    StreamContext* c = (StreamContext*)ctx;
    c->setSync(doIt);
  }

  uint64_t pcv4j_ffmpeg_stop(uint64_t ctx){
    StreamContext* c = (StreamContext*)ctx;
    if (c->state == STOP)
      return 0;
    if (c->state != PLAY) {
      log(c, ERROR, "StreamContext is in the wrong state. It should have been in %d but it's in %d.", (int)PLAY, (int)c->state);
      return MAKE_P_STAT(STREAM_BAD_STATE);
    }
    c->stop = true;
    return 0;
  }

  // exposed to java
  void pcv4j_ffmpeg_set_im_maker(uint64_t im) {
    imaker = (ai::kognition::pilecv4j::ImageMaker*)im;
  }
}
//========================================================================

static uint64_t open_stream(uint64_t ctx, const char* url, fill_buffer readCallback, seek_buffer seekCallback) {
  StreamContext* c = (StreamContext*)ctx;
  if (c->state != FRESH) {
    log(c, ERROR, "StreamContext is in the wrong state. It should have been in %d but it's in %d.", (int)FRESH, (int)c->state);
    return MAKE_P_STAT(STREAM_BAD_STATE);
  }

  if (c->formatCtx)
    return MAKE_P_STAT(STREAM_IN_USE);

  c->formatCtx = avformat_alloc_context();

  if (readCallback) { // this means we have custom io
    // setup the StreamContext for the custom IO
    c->ioCallbackReadBuffer = readCallback;
    c->ioCallbackSeekBuffer = seekCallback;
    c->ioBuffer = (uint8_t*)av_malloc(PCV4J_CUSTOMIO_BUFSIZE * sizeof(uint8_t));
    c->ioContext = avio_alloc_context(c->ioBuffer,PCV4J_CUSTOMIO_BUFSIZE,0,c,
        read_packet_from_custom_source,
        nullptr,
        seekCallback == nullptr ? nullptr : seek_in_custom_source);

    // setup the AVFormatContext for the custom io
    c->formatCtx->pb = c->ioContext;
  }

  AVDictionary* opts = nullptr;
  c->buildOptions(&opts);
  uint64_t ret =  MAKE_AV_STAT(avformat_open_input(&c->formatCtx, url, nullptr, opts == nullptr ? nullptr : &opts));
  if (opts != nullptr)
    av_dict_free(&opts);

  if (!isError(ret))
    c->state = OPEN;

  return ret;
}

static uint64_t findFirstVidCodec(StreamContext* c, AVFormatContext* pFormatContext,
    AVCodec** pCodec,AVCodecParameters** pCodecParameters,
    int* rvsi, AVRational* timebase) {

  // if there's no streams, there's no stream.
  if (pFormatContext->streams == nullptr)
    return MAKE_P_STAT(NO_VIDEO_STREAM);

  int video_stream_index = -1;
  bool foundUnsupportedCode = false;
  int logLevel = c->logLevel;

  // loop though all the streams and print its main information.
  // when we find the first video stream, record the information
  // from it.
  for (int i = 0; i < pFormatContext->nb_streams; i++)
  {
    AVCodecParameters *pLocalCodecParameters =  NULL;

    AVStream* lStream = pFormatContext->streams[i];

    // minimally validate the stream
    if (lStream == nullptr) {
      log(c, WARN, "AVStream is missing from stream array [%d]", i);
      continue;
    }

    pLocalCodecParameters = lStream->codecpar;
    if (logLevel <= DEBUG) {
      log(c, DEBUG, "AVStream->time_base before open coded %d/%d", lStream->time_base.num, lStream->time_base.den);
      log(c, DEBUG, "AVStream->r_frame_rate before open coded %d/%d", lStream->r_frame_rate.num, lStream->r_frame_rate.den);
      log(c, DEBUG, "AVStream->start_time %" PRId64, lStream->start_time);
      log(c, DEBUG, "AVStream->duration %" PRId64, lStream->duration);

      log(c, INFO, "finding the proper decoder (CODEC)");
    }

    AVCodec *pLocalCodec = NULL;

    // finds the registered decoder for a codec ID
    // https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga19a0ca553277f019dd5b0fec6e1f9dca
    pLocalCodec = avcodec_find_decoder(pLocalCodecParameters->codec_id);

    if (pLocalCodec==NULL) {
      log(c, ERROR, "ERROR unsupported codec!");
      foundUnsupportedCode = true;
      continue;
    }

    // when the stream is a video we store its index, codec parameters and codec, etc.
    if (pLocalCodecParameters->codec_type == AVMEDIA_TYPE_VIDEO) {
      if (video_stream_index == -1) {
        video_stream_index = i;
        *pCodec = pLocalCodec;
        *pCodecParameters = pLocalCodecParameters;
        *timebase = pFormatContext->streams[video_stream_index]->time_base;
      }

      log(c, DEBUG, "Video Codec: resolution %d x %d", pLocalCodecParameters->width, pLocalCodecParameters->height);
    } else if (pLocalCodecParameters->codec_type == AVMEDIA_TYPE_AUDIO) {
      log(c, DEBUG, "Audio Codec: %d channels, sample rate %d", pLocalCodecParameters->channels, pLocalCodecParameters->sample_rate);
    }

    // print its name, id and bitrate
    log(c, INFO, "\tCodec %s ID %d bit_rate %lld", pLocalCodec->name, pLocalCodec->id, pLocalCodecParameters->bit_rate);
  }

  if (video_stream_index == -1) {
    if (foundUnsupportedCode) {
      return MAKE_P_STAT(NO_SUPPORTED_CODEC);
    } else {
      return MAKE_P_STAT(NO_VIDEO_STREAM);
    }
  }

  *rvsi = video_stream_index;

  return 0;
}

static uint64_t decode_packet(StreamContext* c, AVCodecContext *pCodecContext, AVFrame *pFrame, AVPacket *pPacket, push_frame callback, SwsContext** colorCvrt)
{
  // Supply raw packet data as input to a decoder
  // https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga58bc4bf1e0ac59e27362597e467efff3
  int response = avcodec_send_packet(pCodecContext, pPacket);

  if (response < 0 && response != AVERROR_INVALIDDATA) {
    log(c, ERROR, "Error while sending a packet to the decoder: %s", av_err2str(response));
    return MAKE_AV_STAT(response);
  }

  const int logLevel = c->logLevel;
  bool sync = c->sync;

  while (response >= 0)
  {
    // Return decoded output data (into a frame) from a decoder
    // https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga11e6542c4e66d3028668788a1a74217c
    response = avcodec_receive_frame(pCodecContext, pFrame);
    if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
      break;
    } else if (response < 0) {
      log(c, ERROR, "Error while receiving a frame from the decoder: %s", av_err2str(response));
      return MAKE_AV_STAT(response);
    }

    if (response >= 0) {

      int64_t pts = pFrame->best_effort_timestamp;
      int64_t timeToDiplayFrame = sync ?
          (av_rescale_q(pts, c->streamTimebase, millisecondTimeBase) + c->startPlayTime) :
          -1;

      AVPixelFormat curFormat = (AVPixelFormat)pFrame->format;
      if (logLevel <= TRACE) {
        log(c, TRACE,
            "Frame %d (type=%c, size=%d bytes, format=%d) pts %d (clock millis: %d), timebase %d/%d, key_frame %d [DTS %d]",
            pCodecContext->frame_number,
            av_get_picture_type_char(pFrame->pict_type),
            pFrame->pkt_size,
            curFormat,
            pts,
            timeToDiplayFrame,
            c->streamTimebase.num, c->streamTimebase.den,
            pFrame->key_frame,
            pFrame->coded_picture_number
        );
      }

      const int32_t w = pFrame->width;
      const int32_t h = pFrame->height;
      int32_t isRgb;
      uint64_t mat;

      curFormat = upgradePixFormatIfNecessary(c, curFormat);
      if (curFormat != AV_PIX_FMT_RGB24 && curFormat != AV_PIX_FMT_BGR24) {
        // use the existing setup if it's there already.
        SwsContext* swCtx = *colorCvrt;
        if (swCtx == nullptr || c->lastFormatUsed != curFormat) {
          c->lastFormatUsed = curFormat;
          if (swCtx)
            sws_freeContext(swCtx);

          *colorCvrt = swCtx =
              sws_getContext(
                w,
                h,
                curFormat,
                w,
                h,
                AV_PIX_FMT_RGB24,
                SWS_BILINEAR,NULL,NULL,NULL
              );
        }

        int32_t stride = 3 * w;
        ai::kognition::pilecv4j::MatAndData matPlus = imaker->allocateImage(h,w);
        mat = matPlus.mat;
        uint8_t* matData = (uint8_t*)matPlus.data;
        uint8_t *rgb24[1] = { matData };
        int rgb24_stride[1] = { stride };
        sws_scale(swCtx,pFrame->data, pFrame->linesize, 0, h, rgb24, rgb24_stride);
        isRgb = 1;
      } else {
        mat = imaker->allocateImageWithCopyOfData(h,w,w * 3,pFrame->data[0]);
        isRgb = (curFormat == AV_PIX_FMT_RGB24) ? 1 : 0;
      }

      bool skipIt = false;
      if (sync) {
        int64_t curTime = now();
        if (curTime < timeToDiplayFrame) {
          log(c, TRACE, "Sleeping for %d", (timeToDiplayFrame - curTime));
          std::this_thread::sleep_for(std::chrono::milliseconds(timeToDiplayFrame - curTime));
        }
        else if ((curTime - timeToDiplayFrame) > c->maxDelayMillisBeforeDroppingFrame) {
          log(c, DEBUG, "Throwing away frame because it's %d milliseconds late.",(curTime - timeToDiplayFrame) );
          skipIt=true;
        }
      }
      if (!skipIt)
        (*callback)(mat, isRgb);
      imaker->freeImage(mat);
    }
  }
  return 0;
}



