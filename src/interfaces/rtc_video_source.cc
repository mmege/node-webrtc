/* Copyright (c) 2019 The node-webrtc project authors. All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be found
 * in the LICENSE.md file in the root of the source tree. All contributing
 * project authors may be found in the AUTHORS file in the root of the source
 * tree.
 */
#include "src/interfaces/rtc_video_source.h"

#include <webrtc/api/peer_connection_interface.h>
#include <webrtc/api/video/i420_buffer.h>
#include <webrtc/api/video/video_frame.h>
#include <webrtc/rtc_base/ref_counted_object.h>
#include <webrtc/rtc_base/logging.h>

#include "src/converters.h"
#include "src/converters/absl.h"
#include "src/converters/arguments.h"
#include "src/converters/napi.h"
#include "src/dictionaries/webrtc/video_frame_buffer.h"
#include "src/functional/maybe.h"
#include "src/interfaces/media_stream_track.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
}

#include <chrono>
#include <ctime>

namespace node_webrtc
{

  Napi::FunctionReference &RTCVideoSource::constructor()
  {
    static Napi::FunctionReference constructor;
    return constructor;
  }

  RTCVideoSource::RTCVideoSource(const Napi::CallbackInfo &info)
      : Napi::ObjectWrap<RTCVideoSource>(info)
  {
    New(info);
  }

  Napi::Value RTCVideoSource::New(const Napi::CallbackInfo &info)
  {
    auto env = info.Env();

    if (!info.IsConstructCall())
    {
      Napi::TypeError::New(env, "Use the new operator to construct an RTCVideoSource.").ThrowAsJavaScriptException();
      return info.Env().Undefined();
    }

    CONVERT_ARGS_OR_THROW_AND_RETURN_NAPI(info, maybeInit, Maybe<RTCVideoSourceInit>)
    auto init = maybeInit.FromMaybe(RTCVideoSourceInit());

    auto needsDenoising = init.needsDenoising
                              .Map([](auto needsDenoising)
                                   { return absl::optional<bool>(needsDenoising); })
                              .FromMaybe(absl::optional<bool>());

    _source = new rtc::RefCountedObject<RTCVideoTrackSource>(init.isScreencast, needsDenoising);

    return info.Env().Undefined();
  }

  Napi::Value RTCVideoSource::CreateTrack(const Napi::CallbackInfo &)
  {
    // TODO(mroberts): Again, we have some implicit factory we are threading around. How to handle?
    auto factory = PeerConnectionFactory::GetOrCreateDefault();
    auto track = factory->factory()->CreateVideoTrack(rtc::CreateRandomUuid(), _source);
    return MediaStreamTrack::wrap()->GetOrCreate(factory, track)->Value();
  }

  Napi::Value RTCVideoSource::FromFFmpeg(const Napi::CallbackInfo &info)
  {
    CONVERT_ARGS_OR_THROW_AND_RETURN_NAPI(info, args_list, std::string)

    uv_process_options_t options = {0};
    uint8_t r;

    RTC_LOG_F(LS_WARNING) << "Entry point\n";

    uv_loop_init(&_loop);
    _frame_buffer_cursor = 0;
    _size_of_buffer_frame = 1280 * 720 * 12 / 8;

    static std::function<void(uv_handle_t * handle, size_t suggested_size, uv_buf_t * buf)> AllocFrameBufferBounce;
    static std::function<void(uv_stream_t * stream, ssize_t nread, const uv_buf_t *buf)> ReadFrameBufferBounce;
    // static std::function<void(void *)> runLoop;

    /* Create the connected pair */
    uv_pipe(_pipe_fds, 0, 0);
    fcntl(_pipe_fds[1], F_SETPIPE_SZ, _size_of_buffer_frame);
    fcntl(_pipe_fds[1], F_GETPIPE_SZ);
    RTC_LOG(LS_WARNING) << "Get pipes fds: " << _pipe_fds[0] << " " << _pipe_fds[1];

    /* read from the pipe with uv */
    uv_pipe_init(&_loop, &_pipe_handle, 0);
    uv_pipe_open(&_pipe_handle, _pipe_fds[0]);
    RTC_LOG(LS_WARNING) << "Reading pipe side has been init and registered, opened to the loop";

    /* Launch ffmpeg as subprocess */
    char *args[17];
    args[0] = "ffmpeg";
    args[1] = "-i";
    args[2] = "udp://@192.168.3.74:2222";
    //args[2] = "rtsp://192.168.3.142/z3-1.sdp";
    args[3] = "-f";
    args[4] = "rawvideo";
    args[5] = "-pix_fmt";
    args[6] = "yuv420p";
    args[7] = "-r";
    args[8] = "15";
    args[9] = "-fflags";
    args[10] = "nobuffer";
    args[11] = "-flags";
    args[12] = "low_delay";
    args[13] = "-s";
    args[14] = "1280x720";
    args[15] = "pipe:1";
    args[16] = NULL;
    RTC_LOG(LS_WARNING) << "define args";

    options.file = "ffmpeg";
    options.args = args;

    options.stdio_count = 3;
    uv_stdio_container_t child_stdio[3];
    child_stdio[0].flags = UV_IGNORE;
    child_stdio[1].flags = UV_INHERIT_FD;
    child_stdio[1].data.fd = _pipe_fds[1];
    child_stdio[2].flags = UV_INHERIT_FD;
    child_stdio[2].data.fd = 2;
    options.stdio = child_stdio;

    /* uv_pipe_open() takes ownership of the file descriptor. */

    auto AllocFrameBuffer = [](uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
    {
      AllocFrameBufferBounce(handle, suggested_size, buf);
    };
    AllocFrameBufferBounce = [&](uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
    {
      if (_frame_buffer_cursor == 0)
      {
        RTC_LOG(LS_VERBOSE) << "Allocate a new I420 Frame Buffer";
        _video_frame_buffer = webrtc::I420Buffer::Create(1280, 720);
        buf->len = _size_of_buffer_frame;
        buf->base = reinterpret_cast<char *>(RTCVideoSource::_video_frame_buffer->MutableDataY());
      }
      else
      {
        buf->len = _size_of_buffer_frame - _frame_buffer_cursor;
        buf->base = reinterpret_cast<char *>(RTCVideoSource::_video_frame_buffer->MutableDataY()) + _frame_buffer_cursor;
      }
    };

    auto ReadFrameBuffer = [](uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
    {
      ReadFrameBufferBounce(stream, nread, buf);
    };
    ReadFrameBufferBounce = [&](uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
    {
      _frame_buffer_cursor += nread;
      if(_frame_buffer_cursor < _size_of_buffer_frame)
      {
        return;
      }
      RTC_LOG(LS_VERBOSE) << "Size of buffer frame:" << _frame_buffer_cursor;
      _frame_buffer_cursor = 0;
      if (nread < 0)
      {
        if (nread == UV_EOF)
        {
          // end of fil
          uv_close(reinterpret_cast<uv_handle_t *>(stream), NULL);
        }
      }
      else if (nread > 0)
      {
        auto now = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
        uint64_t nowInUs = now.time_since_epoch().count();

        webrtc::VideoFrame::Builder builder;
        auto frame = builder
                         .set_timestamp_us(nowInUs)
                         .set_video_frame_buffer(_video_frame_buffer)
                         .build();
        _source->PushFrame(frame);
      }
    };

    uv_read_start(reinterpret_cast<uv_stream_t *>(&_pipe_handle), AllocFrameBuffer, ReadFrameBuffer);

    RTC_LOG(LS_WARNING) << "Prepare to spawn ffmpeg";
    if ((r = uv_spawn(&_loop, &_child_req, &options)))
    {
      RTC_LOG(LS_ERROR) << uv_strerror(r);
      return info.Env().Undefined();
    }
    RTC_LOG(LS_WARNING) << "ffmpeg has been spawned";

    uv_thread_create(
        &_loop_event_thread, [](void *arg)
        {
    RTC_LOG(LS_WARNING) << "Launch loop";
    uv_run(reinterpret_cast<uv_loop_t *>(arg), UV_RUN_DEFAULT);
    RTC_LOG(LS_WARNING) << "Loop stopped"; },
        &_loop);
    return info.Env().Undefined();
  }

  Napi::Value RTCVideoSource::FromLibAvFormat(const Napi::CallbackInfo &info){
    uint32_t ret = 0;
    static std::function<void(void *arg)> ReadFrameBufferBounce;
    auto now = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
    char err_buff[64];

    /* open input and allocate format context */
    if (avformat_open_input(&fmt_ctx, "udp://@192.168.3.74:2222", NULL, NULL) < 0)
    {
      RTC_LOG(LS_ERROR) << "Could not open input";
      return info.Env().Undefined();
    }

    /* retrieve stream information */
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0)
    {
      RTC_LOG(LS_ERROR) << "Could not find stream information";
      return info.Env().Undefined();
    }

    video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if(video_stream_idx < 0)
    {
      RTC_LOG(LS_ERROR) << "Could not find " << av_get_media_type_string(AVMEDIA_TYPE_VIDEO) << " stream in input";
      return info.Env().Undefined();
    }

    video_stream = fmt_ctx->streams[video_stream_idx];

    /* find decoder for the stream */
    const AVCodec *dec = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!dec)
    {
      RTC_LOG(LS_ERROR) << "Failed to find " << av_get_media_type_string(AVMEDIA_TYPE_VIDEO) << " codec";
      return info.Env().Undefined();
    }

    /* Allocate a codec context for the decoder */
    video_dec_ctx = avcodec_alloc_context3(dec);
    if (!video_dec_ctx)
    {
      RTC_LOG(LS_ERROR) << "Failed to allocate the " << av_get_media_type_string(AVMEDIA_TYPE_VIDEO) << "codec context";
      return info.Env().Undefined();
    }

    /* Copy codec parameters from input stream to output codec context */
    if ((ret = avcodec_parameters_to_context(video_dec_ctx, video_stream->codecpar)) < 0)
    {
      RTC_LOG(LS_ERROR) << "Failed to copy" << av_get_media_type_string(AVMEDIA_TYPE_VIDEO) << "codec parameters to decoder context";
      return info.Env().Undefined();
    }

    /* Init the decoders */
    AVDictionary *opts = NULL;
    if ((ret = avcodec_open2(video_dec_ctx, dec, &opts)) < 0)
    {
      RTC_LOG(LS_ERROR) << "Failed to open" << av_get_media_type_string(AVMEDIA_TYPE_VIDEO) << "codec";
      return info.Env().Undefined();
    }

    /* allocate image where the decoded image will be put */
    width = video_dec_ctx->width;
    height = video_dec_ctx->height;
    pix_fmt = video_dec_ctx->pix_fmt;
    video_dst_bufsize = av_image_alloc(video_dst_data, video_dst_linesize,
                         video_dec_ctx->width, video_dec_ctx->height, video_dec_ctx->pix_fmt, 1);
    if (video_dst_bufsize < 0) {
        RTC_LOG(LS_ERROR) << "Could not allocate raw video buffer";
    }

    ReadFrameBufferBounce = [&](void *arg)
    {
      AVFrame *frame = av_frame_alloc();
      if (!frame)
      {
        RTC_LOG(LS_ERROR) << "Could not allocate frame";
        return;
      }

      /* initialize packet, set data to NULL, let the demuxer fill it */
      AVPacket pkt;
      av_init_packet(&pkt);
      pkt.data = NULL;
      pkt.size = 0;

      /* read frames from the source url*/
      while (av_read_frame(fmt_ctx, &pkt) >= 0)
      {
        /* Submit packet to decoder*/
        int ret = avcodec_send_packet(video_dec_ctx, &pkt);
        if (ret < 0) {
          av_make_error_string(err_buff, 64, ret);
          RTC_LOG(LS_ERROR) << "Error submitting a packet for decoding" << err_buff;
          break;
        }

        /* Decode Frame (One per each pkt with VIDEO case)*/
        ret = avcodec_receive_frame(video_dec_ctx, frame);
        if (ret < 0) {
          // those two return values are special and mean there is no output
          // frame available, but there were no errors during decoding
          if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
            continue;
          av_make_error_string(err_buff, 64, ret);
          RTC_LOG(LS_ERROR) << "Error during decoding" << err_buff;
          break;
        }

        /* Create the WebRTC image buffer */
        rtc::scoped_refptr<webrtc::I420Buffer> video_frame_buffer = webrtc::I420Buffer::Create(frame->width, frame->height);
        int w = video_frame_buffer->width();
        int h = video_frame_buffer->height();
        uint8_t *dst = video_frame_buffer->MutableDataY();

        /* copy decoded frame to destination buffer:
         * this is required since rawvideo expects non aligned data */
        ret = av_image_copy_to_buffer(video_frame_buffer->MutableDataY(), video_dst_bufsize,
                                (const uint8_t **)(frame->data), frame->linesize,
                                pix_fmt, width, height, 1);
        if(ret < 0){
          av_make_error_string(err_buff, 64, ret);
          RTC_LOG(LS_ERROR) << "Error during decoding" << err_buff;
        }
        if(ret != video_dst_bufsize)
        {
          RTC_LOG(LS_ERROR) << "Copy of frame size does not correspond to attended frame size";
        }

        webrtc::VideoFrame::Builder webRTCFramebuilder;
        auto webRTCframe = webRTCFramebuilder
                         .set_timestamp_us(now.time_since_epoch().count())
                         .set_video_frame_buffer(video_frame_buffer)
                         .build();
        _source->PushFrame(webRTCframe);

        av_frame_unref(frame);
        av_packet_unref(&pkt);
      }
    };

    uv_thread_create(
        &_decode_thread, [](void *arg)
        { ReadFrameBufferBounce(arg); },
        NULL);

    return info.Env().Undefined();
  }

  Napi::Value RTCVideoSource::OnFrame(const Napi::CallbackInfo &info)
  {
    CONVERT_ARGS_OR_THROW_AND_RETURN_NAPI(info, buffer, rtc::scoped_refptr<webrtc::I420Buffer>)

    auto now = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
    uint64_t nowInUs = now.time_since_epoch().count();

    webrtc::VideoFrame::Builder builder;
    auto frame = builder
                     .set_timestamp_us(nowInUs)
                     .set_video_frame_buffer(buffer)
                     .build();
    _source->PushFrame(frame);
    return info.Env().Undefined();
  }

  Napi::Value RTCVideoSource::GetNeedsDenoising(const Napi::CallbackInfo &info)
  {
    CONVERT_OR_THROW_AND_RETURN_NAPI(info.Env(), _source->needs_denoising(), result, Napi::Value)
    return result;
  }

  Napi::Value RTCVideoSource::GetIsScreencast(const Napi::CallbackInfo &info)
  {
    CONVERT_OR_THROW_AND_RETURN_NAPI(info.Env(), _source->is_screencast(), result, Napi::Value)
    return result;
  }

  void RTCVideoSource::Init(Napi::Env env, Napi::Object exports)
  {
    Napi::HandleScope scope(env);

    Napi::Function func = DefineClass(env, "RTCVideoSource", {InstanceMethod("createTrack", &RTCVideoSource::CreateTrack), InstanceMethod("onFrame", &RTCVideoSource::OnFrame), InstanceMethod("fromFFmpeg", &RTCVideoSource::FromFFmpeg), InstanceMethod("fromLibAvFormat", &RTCVideoSource::FromLibAvFormat), InstanceAccessor("needsDenoising", &RTCVideoSource::GetNeedsDenoising, nullptr), InstanceAccessor("isScreencast", &RTCVideoSource::GetIsScreencast, nullptr)});

    constructor() = Napi::Persistent(func);
    constructor().SuppressDestruct();

    exports.Set("RTCVideoSource", func);
  }

} // namespace node_webrtc
