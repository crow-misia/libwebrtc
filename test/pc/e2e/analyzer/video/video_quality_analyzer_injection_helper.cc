/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "test/pc/e2e/analyzer/video/video_quality_analyzer_injection_helper.h"

#include <stdio.h>

#include <memory>
#include <utility>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/strings/string_view.h"
#include "api/array_view.h"
#include "api/test/pclf/media_configuration.h"
#include "api/video/i420_buffer.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/strings/string_builder.h"
#include "system_wrappers/include/clock.h"
#include "test/pc/e2e/analyzer/video/analyzing_video_sink.h"
#include "test/pc/e2e/analyzer/video/quality_analyzing_video_decoder.h"
#include "test/pc/e2e/analyzer/video/quality_analyzing_video_encoder.h"
#include "test/pc/e2e/analyzer/video/simulcast_dummy_buffer_helper.h"
#include "test/pc/e2e/analyzer/video/video_dumping.h"
#include "test/testsupport/fixed_fps_video_frame_writer_adapter.h"
#include "test/video_renderer.h"

namespace webrtc {
namespace webrtc_pc_e2e {
namespace {

using EmulatedSFUConfigMap =
    ::webrtc::webrtc_pc_e2e::QualityAnalyzingVideoEncoder::EmulatedSFUConfigMap;

class AnalyzingFramePreprocessor
    : public test::TestVideoCapturer::FramePreprocessor {
 public:
  AnalyzingFramePreprocessor(
      absl::string_view peer_name,
      absl::string_view stream_label,
      VideoQualityAnalyzerInterface* analyzer,
      std::vector<std::unique_ptr<rtc::VideoSinkInterface<VideoFrame>>> sinks)
      : peer_name_(peer_name),
        stream_label_(stream_label),
        analyzer_(analyzer),
        sinks_(std::move(sinks)) {}
  ~AnalyzingFramePreprocessor() override = default;

  VideoFrame Preprocess(const VideoFrame& source_frame) override {
    // Copy VideoFrame to be able to set id on it.
    VideoFrame frame = source_frame;
    uint16_t frame_id =
        analyzer_->OnFrameCaptured(peer_name_, stream_label_, frame);
    frame.set_id(frame_id);

    for (auto& sink : sinks_) {
      sink->OnFrame(frame);
    }
    return frame;
  }

 private:
  const std::string peer_name_;
  const std::string stream_label_;
  VideoQualityAnalyzerInterface* const analyzer_;
  const std::vector<std::unique_ptr<rtc::VideoSinkInterface<VideoFrame>>>
      sinks_;
};

}  // namespace

VideoQualityAnalyzerInjectionHelper::VideoQualityAnalyzerInjectionHelper(
    Clock* clock,
    std::unique_ptr<VideoQualityAnalyzerInterface> analyzer,
    EncodedImageDataInjector* injector,
    EncodedImageDataExtractor* extractor)
    : clock_(clock),
      analyzer_(std::move(analyzer)),
      injector_(injector),
      extractor_(extractor) {
  RTC_DCHECK(clock_);
  RTC_DCHECK(injector_);
  RTC_DCHECK(extractor_);
}
VideoQualityAnalyzerInjectionHelper::~VideoQualityAnalyzerInjectionHelper() =
    default;

std::unique_ptr<VideoEncoderFactory>
VideoQualityAnalyzerInjectionHelper::WrapVideoEncoderFactory(
    absl::string_view peer_name,
    std::unique_ptr<VideoEncoderFactory> delegate,
    double bitrate_multiplier,
    EmulatedSFUConfigMap stream_to_sfu_config) const {
  return std::make_unique<QualityAnalyzingVideoEncoderFactory>(
      peer_name, std::move(delegate), bitrate_multiplier,
      std::move(stream_to_sfu_config), injector_, analyzer_.get());
}

std::unique_ptr<VideoDecoderFactory>
VideoQualityAnalyzerInjectionHelper::WrapVideoDecoderFactory(
    absl::string_view peer_name,
    std::unique_ptr<VideoDecoderFactory> delegate) const {
  return std::make_unique<QualityAnalyzingVideoDecoderFactory>(
      peer_name, std::move(delegate), extractor_, analyzer_.get());
}

std::unique_ptr<test::TestVideoCapturer::FramePreprocessor>
VideoQualityAnalyzerInjectionHelper::CreateFramePreprocessor(
    absl::string_view peer_name,
    const VideoConfig& config) {
  std::vector<std::unique_ptr<rtc::VideoSinkInterface<VideoFrame>>> sinks;
  if (config.input_dump_options.has_value()) {
    std::unique_ptr<test::VideoFrameWriter> writer =
        config.input_dump_options->CreateInputDumpVideoFrameWriter(
            *config.stream_label, config.GetResolution());
    sinks.push_back(std::make_unique<VideoWriter>(
        writer.get(), config.input_dump_options->sampling_modulo()));
    video_writers_.push_back(std::move(writer));
  }
  if (config.show_on_screen) {
    sinks.push_back(absl::WrapUnique(
        test::VideoRenderer::Create((*config.stream_label + "-capture").c_str(),
                                    config.width, config.height)));
  }
  sinks_helper_.AddConfig(peer_name, config);
  {
    MutexLock lock(&mutex_);
    known_video_configs_.insert({*config.stream_label, config});
  }
  return std::make_unique<AnalyzingFramePreprocessor>(
      peer_name, std::move(*config.stream_label), analyzer_.get(),
      std::move(sinks));
}

std::unique_ptr<rtc::VideoSinkInterface<VideoFrame>>
VideoQualityAnalyzerInjectionHelper::CreateVideoSink(
    absl::string_view peer_name) {
  return std::make_unique<AnalyzingVideoSink2>(peer_name, this);
}

std::unique_ptr<AnalyzingVideoSink>
VideoQualityAnalyzerInjectionHelper::CreateVideoSink(
    absl::string_view peer_name,
    const VideoSubscription& subscription,
    bool report_infra_metrics) {
  return std::make_unique<AnalyzingVideoSink>(peer_name, clock_, *analyzer_,
                                              sinks_helper_, subscription,
                                              report_infra_metrics);
}

void VideoQualityAnalyzerInjectionHelper::Start(
    std::string test_case_name,
    rtc::ArrayView<const std::string> peer_names,
    int max_threads_count) {
  analyzer_->Start(std::move(test_case_name), peer_names, max_threads_count);
  extractor_->Start(peer_names.size());
  MutexLock lock(&mutex_);
  peers_count_ = peer_names.size();
}

void VideoQualityAnalyzerInjectionHelper::RegisterParticipantInCall(
    absl::string_view peer_name) {
  analyzer_->RegisterParticipantInCall(peer_name);
  extractor_->AddParticipantInCall();
  MutexLock lock(&mutex_);
  peers_count_++;
}

void VideoQualityAnalyzerInjectionHelper::UnregisterParticipantInCall(
    absl::string_view peer_name) {
  analyzer_->UnregisterParticipantInCall(peer_name);
  extractor_->RemoveParticipantInCall();
  MutexLock lock(&mutex_);
  peers_count_--;
}

void VideoQualityAnalyzerInjectionHelper::OnStatsReports(
    absl::string_view pc_label,
    const rtc::scoped_refptr<const RTCStatsReport>& report) {
  analyzer_->OnStatsReports(pc_label, report);
}

void VideoQualityAnalyzerInjectionHelper::Stop() {
  analyzer_->Stop();
  for (const auto& video_writer : video_writers_) {
    video_writer->Close();
  }
  video_writers_.clear();
  sinks_helper_.Clear();
}

void VideoQualityAnalyzerInjectionHelper::OnFrame(absl::string_view peer_name,
                                                  const VideoFrame& frame) {
  if (IsDummyFrame(frame)) {
    // This is dummy frame, so we  don't need to process it further.
    return;
  }
  // Copy entire video frame including video buffer to ensure that analyzer
  // won't hold any WebRTC internal buffers.
  VideoFrame frame_copy = frame;
  frame_copy.set_video_frame_buffer(
      I420Buffer::Copy(*frame.video_frame_buffer()->ToI420()));
  analyzer_->OnFrameRendered(peer_name, frame_copy);

  if (frame.id() != VideoFrame::kNotSetId) {
    std::string stream_label = analyzer_->GetStreamLabel(frame.id());
    std::vector<std::unique_ptr<rtc::VideoSinkInterface<VideoFrame>>>* sinks =
        PopulateSinks(ReceiverStream(peer_name, stream_label));
    if (sinks == nullptr) {
      return;
    }
    for (auto& sink : *sinks) {
      sink->OnFrame(frame);
    }
  }
}

std::vector<std::unique_ptr<rtc::VideoSinkInterface<VideoFrame>>>*
VideoQualityAnalyzerInjectionHelper::PopulateSinks(
    const ReceiverStream& receiver_stream) {
  MutexLock lock(&mutex_);
  auto sinks_it = sinks_.find(receiver_stream);
  if (sinks_it != sinks_.end()) {
    return &sinks_it->second;
  }
  auto it = known_video_configs_.find(receiver_stream.stream_label);
  RTC_DCHECK(it != known_video_configs_.end())
      << "No video config for stream " << receiver_stream.stream_label;
  const VideoConfig& config = it->second;

  std::vector<std::unique_ptr<rtc::VideoSinkInterface<VideoFrame>>> sinks;
  if (config.output_dump_options.has_value()) {
    std::unique_ptr<test::VideoFrameWriter> writer =
        config.output_dump_options->CreateOutputDumpVideoFrameWriter(
            receiver_stream.stream_label, receiver_stream.peer_name,
            config.GetResolution());
    if (config.output_dump_use_fixed_framerate) {
      writer = std::make_unique<test::FixedFpsVideoFrameWriterAdapter>(
          config.fps, clock_, std::move(writer));
    }
    sinks.push_back(std::make_unique<VideoWriter>(
        writer.get(), config.output_dump_options->sampling_modulo()));
    video_writers_.push_back(std::move(writer));
  }
  if (config.show_on_screen) {
    sinks.push_back(absl::WrapUnique(
        test::VideoRenderer::Create((*config.stream_label + "-render").c_str(),
                                    config.width, config.height)));
  }
  sinks_.insert({receiver_stream, std::move(sinks)});
  return &(sinks_.find(receiver_stream)->second);
}

}  // namespace webrtc_pc_e2e
}  // namespace webrtc
