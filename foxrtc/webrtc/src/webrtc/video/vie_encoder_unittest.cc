/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <utility>

#include "webrtc/base/logging.h"
#include "webrtc/test/encoder_settings.h"
#include "webrtc/test/fake_encoder.h"
#include "webrtc/test/frame_generator.h"
#include "webrtc/test/gtest.h"
#include "webrtc/video/send_statistics_proxy.h"
#include "webrtc/video/vie_encoder.h"

namespace webrtc {

class ViEEncoderTest : public ::testing::Test {
 public:
  static const int kDefaultTimeoutMs = 30 * 1000;

  ViEEncoderTest()
      : video_send_config_(VideoSendStream::Config(nullptr)),
        codec_width_(320),
        codec_height_(240),
        fake_encoder_(),
        stats_proxy_(Clock::GetRealTimeClock(),
                     video_send_config_,
                     webrtc::VideoEncoderConfig::ContentType::kRealtimeVideo),
        sink_(&fake_encoder_) {}

  void SetUp() override {
    video_send_config_ = VideoSendStream::Config(nullptr);
    video_send_config_.encoder_settings.encoder = &fake_encoder_;
    video_send_config_.encoder_settings.payload_name = "FAKE";
    video_send_config_.encoder_settings.payload_type = 125;

    VideoEncoderConfig video_encoder_config;
    test::FillEncoderConfiguration(1, &video_encoder_config);
    vie_encoder_.reset(new ViEEncoder(
        1 /* number_of_cores */, &stats_proxy_,
        video_send_config_.encoder_settings, nullptr /* pre_encode_callback */,
        nullptr /* overuse_callback */, nullptr /* encoder_timing */));
    vie_encoder_->SetSink(&sink_);
    vie_encoder_->SetSource(&video_source_);
    vie_encoder_->SetStartBitrate(10000);
    vie_encoder_->ConfigureEncoder(std::move(video_encoder_config), 1440);
  }

  VideoFrame CreateFrame(int64_t ntp_ts, rtc::Event* destruction_event) const {
    class TestBuffer : public webrtc::I420Buffer {
     public:
      TestBuffer(rtc::Event* event, int width, int height)
          : I420Buffer(width, height), event_(event) {}

     private:
      friend class rtc::RefCountedObject<TestBuffer>;
      ~TestBuffer() override {
        if (event_)
          event_->Set();
      }
      rtc::Event* const event_;
    };

    VideoFrame frame(new rtc::RefCountedObject<TestBuffer>(
                         destruction_event, codec_width_, codec_height_),
                     99, 99, kVideoRotation_0);
    frame.set_ntp_time_ms(ntp_ts);
    return frame;
  }

  class TestEncoder : public test::FakeEncoder {
   public:
    TestEncoder()
        : FakeEncoder(Clock::GetRealTimeClock()),
          continue_encode_event_(false, false) {}

    VideoCodec codec_config() {
      rtc::CritScope lock(&crit_);
      return config_;
    }

    void BlockNextEncode() {
      rtc::CritScope lock(&crit_);
      block_next_encode_ = true;
    }

    void ContinueEncode() { continue_encode_event_.Set(); }

    void CheckLastTimeStampsMatch(int64_t ntp_time_ms,
                                  uint32_t timestamp) const {
      rtc::CritScope lock(&crit_);
      EXPECT_EQ(timestamp_, timestamp);
      EXPECT_EQ(ntp_time_ms_, ntp_time_ms);
    }

   private:
    int32_t Encode(const VideoFrame& input_image,
                   const CodecSpecificInfo* codec_specific_info,
                   const std::vector<FrameType>* frame_types) override {
      bool block_encode;
      {
        rtc::CritScope lock(&crit_);
        EXPECT_GT(input_image.timestamp(), timestamp_);
        EXPECT_GT(input_image.ntp_time_ms(), ntp_time_ms_);
        EXPECT_EQ(input_image.timestamp(), input_image.ntp_time_ms() * 90);

        timestamp_ = input_image.timestamp();
        ntp_time_ms_ = input_image.ntp_time_ms();
        block_encode = block_next_encode_;
        block_next_encode_ = false;
      }
      int32_t result =
          FakeEncoder::Encode(input_image, codec_specific_info, frame_types);
      if (block_encode)
        EXPECT_TRUE(continue_encode_event_.Wait(kDefaultTimeoutMs));
      return result;
    }



    rtc::CriticalSection crit_;
    bool block_next_encode_ = false;
    rtc::Event continue_encode_event_;
    uint32_t timestamp_ = 0;
    int64_t ntp_time_ms_ = 0;
  };

  class TestSink : public ViEEncoder::EncoderSink {
   public:
    explicit TestSink(TestEncoder* test_encoder)
        : test_encoder_(test_encoder), encoded_frame_event_(false, false) {}

    void WaitForEncodedFrame(int64_t expected_ntp_time) {
      uint32_t timestamp = 0;
      EXPECT_TRUE(encoded_frame_event_.Wait(kDefaultTimeoutMs));
      {
        rtc::CritScope lock(&crit_);
        timestamp = timestamp_;
      }
      test_encoder_->CheckLastTimeStampsMatch(expected_ntp_time, timestamp);
    }

    void SetExpectNoFrames() {
      rtc::CritScope lock(&crit_);
      expect_frames_ = false;
    }

    int number_of_reconfigurations() {
      rtc::CritScope lock(&crit_);
      return number_of_reconfigurations_;
    }

    int last_min_transmit_bitrate() {
      rtc::CritScope lock(&crit_);
      return min_transmit_bitrate_bps_;
    }

   private:
    int32_t Encoded(const EncodedImage& encoded_image,
                    const CodecSpecificInfo* codec_specific_info,
                    const RTPFragmentationHeader* fragmentation) override {
      rtc::CritScope lock(&crit_);
      EXPECT_TRUE(expect_frames_);
      timestamp_ = encoded_image._timeStamp;
      encoded_frame_event_.Set();
      return 0;
    }

    void OnEncoderConfigurationChanged(std::vector<VideoStream> streams,
                                       int min_transmit_bitrate_bps) override {
      rtc::CriticalSection crit_;
      ++number_of_reconfigurations_;
      min_transmit_bitrate_bps_ = min_transmit_bitrate_bps;
    }

    rtc::CriticalSection crit_;
    TestEncoder* test_encoder_;
    rtc::Event encoded_frame_event_;
    uint32_t timestamp_ = 0;
    bool expect_frames_ = true;
    int number_of_reconfigurations_ = 0;
    int min_transmit_bitrate_bps_ = 0;
  };

  VideoSendStream::Config video_send_config_;
  int codec_width_;
  int codec_height_;
  TestEncoder fake_encoder_;
  SendStatisticsProxy stats_proxy_;
  TestSink sink_;
  test::FrameForwarder video_source_;
  std::unique_ptr<ViEEncoder> vie_encoder_;
};

TEST_F(ViEEncoderTest, EncodeOneFrame) {
  const int kTargetBitrateBps = 100000;
  vie_encoder_->OnBitrateUpdated(kTargetBitrateBps, 0, 0);
  rtc::Event frame_destroyed_event(false, false);
  video_source_.IncomingCapturedFrame(CreateFrame(1, &frame_destroyed_event));
  sink_.WaitForEncodedFrame(1);
  EXPECT_TRUE(frame_destroyed_event.Wait(kDefaultTimeoutMs));
  vie_encoder_->Stop();
}

TEST_F(ViEEncoderTest, DropsFramesBeforeFirstOnBitrateUpdated) {
  // Dropped since no target bitrate has been set.
  rtc::Event frame_destroyed_event(false, false);
  video_source_.IncomingCapturedFrame(CreateFrame(1, &frame_destroyed_event));
  EXPECT_TRUE(frame_destroyed_event.Wait(kDefaultTimeoutMs));

  const int kTargetBitrateBps = 100000;
  vie_encoder_->OnBitrateUpdated(kTargetBitrateBps, 0, 0);

  video_source_.IncomingCapturedFrame(CreateFrame(2, nullptr));
  sink_.WaitForEncodedFrame(2);
  vie_encoder_->Stop();
}

TEST_F(ViEEncoderTest, DropsFramesWhenRateSetToZero) {
  const int kTargetBitrateBps = 100000;
  vie_encoder_->OnBitrateUpdated(kTargetBitrateBps, 0, 0);
  video_source_.IncomingCapturedFrame(CreateFrame(1, nullptr));
  sink_.WaitForEncodedFrame(1);

  vie_encoder_->OnBitrateUpdated(0, 0, 0);
  // Dropped since bitrate is zero.
  video_source_.IncomingCapturedFrame(CreateFrame(2, nullptr));

  vie_encoder_->OnBitrateUpdated(kTargetBitrateBps, 0, 0);
  video_source_.IncomingCapturedFrame(CreateFrame(3, nullptr));
  sink_.WaitForEncodedFrame(3);
  vie_encoder_->Stop();
}

TEST_F(ViEEncoderTest, DropsFramesWithSameOrOldNtpTimestamp) {
  const int kTargetBitrateBps = 100000;
  vie_encoder_->OnBitrateUpdated(kTargetBitrateBps, 0, 0);
  video_source_.IncomingCapturedFrame(CreateFrame(1, nullptr));
  sink_.WaitForEncodedFrame(1);

  // This frame will be dropped since it has the same ntp timestamp.
  video_source_.IncomingCapturedFrame(CreateFrame(1, nullptr));

  video_source_.IncomingCapturedFrame(CreateFrame(2, nullptr));
  sink_.WaitForEncodedFrame(2);
  vie_encoder_->Stop();
}

TEST_F(ViEEncoderTest, DropsFrameAfterStop) {
  const int kTargetBitrateBps = 100000;
  vie_encoder_->OnBitrateUpdated(kTargetBitrateBps, 0, 0);

  video_source_.IncomingCapturedFrame(CreateFrame(1, nullptr));
  sink_.WaitForEncodedFrame(1);

  vie_encoder_->Stop();
  sink_.SetExpectNoFrames();
  rtc::Event frame_destroyed_event(false, false);
  video_source_.IncomingCapturedFrame(CreateFrame(2, &frame_destroyed_event));
  EXPECT_TRUE(frame_destroyed_event.Wait(kDefaultTimeoutMs));
}

TEST_F(ViEEncoderTest, DropsPendingFramesOnSlowEncode) {
  const int kTargetBitrateBps = 100000;
  vie_encoder_->OnBitrateUpdated(kTargetBitrateBps, 0, 0);

  fake_encoder_.BlockNextEncode();
  video_source_.IncomingCapturedFrame(CreateFrame(1, nullptr));
  sink_.WaitForEncodedFrame(1);
  // Here, the encoder thread will be blocked in the TestEncoder waiting for a
  // call to ContinueEncode.
  video_source_.IncomingCapturedFrame(CreateFrame(2, nullptr));
  video_source_.IncomingCapturedFrame(CreateFrame(3, nullptr));
  fake_encoder_.ContinueEncode();
  sink_.WaitForEncodedFrame(3);

  vie_encoder_->Stop();
}

TEST_F(ViEEncoderTest, ConfigureEncoderTriggersOnEncoderConfigurationChanged) {
  const int kTargetBitrateBps = 100000;
  vie_encoder_->OnBitrateUpdated(kTargetBitrateBps, 0, 0);

  // Capture a frame and wait for it to synchronize with the encoder thread.
  video_source_.IncomingCapturedFrame(CreateFrame(1, nullptr));
  sink_.WaitForEncodedFrame(1);
  // The encoder will have been configured twice. First time before the first
  // frame has been received. Then a second time when the resolution is known.
  EXPECT_EQ(2, sink_.number_of_reconfigurations());

  VideoEncoderConfig video_encoder_config;
  test::FillEncoderConfiguration(1, &video_encoder_config);
  video_encoder_config.min_transmit_bitrate_bps = 9999;
  vie_encoder_->ConfigureEncoder(std::move(video_encoder_config), 1440);

  // Capture a frame and wait for it to synchronize with the encoder thread.
  video_source_.IncomingCapturedFrame(CreateFrame(2, nullptr));
  sink_.WaitForEncodedFrame(2);
  EXPECT_EQ(3, sink_.number_of_reconfigurations());
  EXPECT_EQ(9999, sink_.last_min_transmit_bitrate());

  vie_encoder_->Stop();
}

TEST_F(ViEEncoderTest, FrameResolutionChangeReconfigureEncoder) {
  const int kTargetBitrateBps = 100000;
  vie_encoder_->OnBitrateUpdated(kTargetBitrateBps, 0, 0);

  // Capture a frame and wait for it to synchronize with the encoder thread.
  video_source_.IncomingCapturedFrame(CreateFrame(1, nullptr));
  sink_.WaitForEncodedFrame(1);
  // The encoder will have been configured twice. First time before the first
  // frame has been received. Then a second time when the resolution is known.
  EXPECT_EQ(2, sink_.number_of_reconfigurations());
  EXPECT_EQ(codec_width_, fake_encoder_.codec_config().width);
  EXPECT_EQ(codec_height_, fake_encoder_.codec_config().height);

  codec_width_ *= 2;
  codec_height_ *= 2;
  // Capture a frame with a higher resolution and wait for it to synchronize
  // with the encoder thread.
  video_source_.IncomingCapturedFrame(CreateFrame(2, nullptr));
  sink_.WaitForEncodedFrame(2);
  EXPECT_EQ(codec_width_, fake_encoder_.codec_config().width);
  EXPECT_EQ(codec_height_, fake_encoder_.codec_config().height);
  EXPECT_EQ(3, sink_.number_of_reconfigurations());

  vie_encoder_->Stop();
}

}  // namespace webrtc
