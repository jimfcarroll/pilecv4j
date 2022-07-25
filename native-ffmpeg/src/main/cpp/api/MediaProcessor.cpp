/*
 * MediaProcessor.cpp
 *
 *  Created on: Jul 6, 2022
 *      Author: jim
 */

#include "api/MediaProcessor.h"

#include <stdint.h>

#include "utils/log.h"
#include "common/kog_exports.h"

namespace pilecv4j {
namespace ffmpeg
{

#define COMPONENT "MPRC"

inline static void llog(LogLevel llevel, const char *fmt, ...) {
  va_list args;
  va_start( args, fmt );
  log( llevel, COMPONENT, fmt, args );
  va_end( args );
}

static const AVRational UNKNOWN_TIME_BASE{0, 1};

uint64_t MediaProcessor::open_codec(AVStream* pStream, AVDictionary** opts, AVCodecContext** codecCtxPtr) {

  *codecCtxPtr = nullptr;

  AVCodecParameters *pCodecParameters = pStream->codecpar;
  AVCodec* pCodec = avcodec_find_decoder(pCodecParameters->codec_id);
  if (pCodec==NULL) {
    llog(ERROR, "Unsupported codec, ID %d",(int)pCodecParameters->codec_id);
    return MAKE_P_STAT(UNSUPPORTED_CODEC);
  }

  // https://ffmpeg.org/doxygen/trunk/structAVCodecContext.html
  AVCodecContext* codecCtx = avcodec_alloc_context3(pCodec);
  *codecCtxPtr = codecCtx;
  if (!codecCtx) {
    llog(ERROR, "failed to allocated memory for AVCodecContext");
    return MAKE_P_STAT(FAILED_CREATE_CODEC_CONTEXT);
  }

  // See the accepted answer here: https://stackoverflow.com/questions/40275242/libav-ffmpeg-copying-decoded-video-timestamps-to-encoder
  AVRational demuxTimeBase = pStream->time_base;
  if (!(demuxTimeBase.num == UNKNOWN_TIME_BASE.num && demuxTimeBase.den == UNKNOWN_TIME_BASE.den)) {
    llog(TRACE, "initializing decode codec context time_base to: %d/%d (this may be reset when the codec is open)", (int)(demuxTimeBase.num), (int)(demuxTimeBase.den));
    codecCtx->time_base = demuxTimeBase;
  }

  // Fill the codec context based on the values from the supplied codec parameters
  // https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#gac7b282f51540ca7a99416a3ba6ee0d16
  uint64_t stat = MAKE_AV_STAT(avcodec_parameters_to_context(codecCtx, pCodecParameters));
  if (isError(stat))
    return stat;

  // Initialize the AVCodecContext to use the given AVCodec.
  // https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#ga11f785a188d7d9df71621001465b0f1d
  stat = avcodec_open2(codecCtx, pCodec, opts);

  if (isEnabled(TRACE))
    llog(TRACE, "decode codec context time_base: %d/%d (after open)", codecCtx->time_base.num, codecCtx->time_base.den);

  if (isError(stat)) {
    llog(ERROR, "failed to open codec through avcodec_open2");
    return stat;
  }

  return stat;
}

//========================================================================
// Everything here in this extern "C" section is callable from Java
//========================================================================
extern "C" {

  KAI_EXPORT void pcv4j_ffmpeg2_mediaProcessor_destroy(uint64_t uriSource) {
    if (isEnabled(TRACE))
      llog(TRACE, "destroying vid processor %" PRId64, uriSource);

    MediaProcessor* ret = (MediaProcessor*)uriSource;
    if (ret != nullptr)
      delete ret;
  }

}

}
}
