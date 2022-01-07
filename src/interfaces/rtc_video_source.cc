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
    //args[2] = "udp://@192.168.2.2:2222";
    args[2] = "rtsp://192.168.3.142/z3-1.sdp";
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

    Napi::Function func = DefineClass(env, "RTCVideoSource", {InstanceMethod("createTrack", &RTCVideoSource::CreateTrack), InstanceMethod("onFrame", &RTCVideoSource::OnFrame), InstanceMethod("fromFFmpeg", &RTCVideoSource::FromFFmpeg), InstanceAccessor("needsDenoising", &RTCVideoSource::GetNeedsDenoising, nullptr), InstanceAccessor("isScreencast", &RTCVideoSource::GetIsScreencast, nullptr)});

    constructor() = Napi::Persistent(func);
    constructor().SuppressDestruct();

    exports.Set("RTCVideoSource", func);
  }

} // namespace node_webrtc
