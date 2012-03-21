/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 *
 * WEBRTC VP8 wrapper interface
 */

#ifndef WEBRTC_MODULES_VIDEO_CODING_CODECS_VP8_H_
#define WEBRTC_MODULES_VIDEO_CODING_CODECS_VP8_H_

#include "video_codec_interface.h"

// VPX forward declaration
typedef struct vpx_codec_ctx vpx_codec_ctx_t;
typedef struct vpx_codec_ctx vpx_dec_ctx_t;
typedef struct vpx_codec_enc_cfg vpx_codec_enc_cfg_t;
typedef struct vpx_image vpx_image_t;
typedef struct vpx_ref_frame vpx_ref_frame_t;
struct vpx_codec_cx_pkt;

namespace webrtc
{
class TemporalLayers;
class ReferencePictureSelection;

class VP8Encoder : public VideoEncoder {
 public:
  static VP8Encoder* Create();

  virtual ~VP8Encoder();

  // Free encoder memory.
  //
  // Return value                : WEBRTC_VIDEO_CODEC_OK if OK, < 0 otherwise.
  virtual int Release();

  // Initialize the encoder with the information from the codecSettings
  //
  // Input:
  //          - codec_settings    : Codec settings
  //          - number_of_cores   : Number of cores available for the encoder
  //          - max_payload_size  : The maximum size each payload is allowed
  //                                to have. Usually MTU - overhead.
  //
  // Return value                 : Set bit rate if OK
  //                                <0 - Errors:
  //                                  WEBRTC_VIDEO_CODEC_ERR_PARAMETER
  //                                  WEBRTC_VIDEO_CODEC_ERR_SIZE
  //                                  WEBRTC_VIDEO_CODEC_LEVEL_EXCEEDED
  //                                  WEBRTC_VIDEO_CODEC_MEMORY
  //                                  WEBRTC_VIDEO_CODEC_ERROR
  virtual int InitEncode(const VideoCodec* codec_settings,
                         int number_of_cores,
                         uint32_t max_payload_size);

  // Encode an I420 image (as a part of a video stream). The encoded image
  // will be returned to the user through the encode complete callback.
  //
  // Input:
  //          - input_image       : Image to be encoded
  //          - frame_types       : Frame type to be generated by the encoder.
  //
  // Return value                 : WEBRTC_VIDEO_CODEC_OK if OK
  //                                <0 - Errors:
  //                                  WEBRTC_VIDEO_CODEC_ERR_PARAMETER
  //                                  WEBRTC_VIDEO_CODEC_MEMORY
  //                                  WEBRTC_VIDEO_CODEC_ERROR
  //                                  WEBRTC_VIDEO_CODEC_TIMEOUT

  virtual int Encode(const RawImage& input_image,
                     const CodecSpecificInfo* codec_specific_info,
                     const VideoFrameType* frame_types);

  // Register an encode complete callback object.
  //
  // Input:
  //          - callback         : Callback object which handles encoded images.
  //
  // Return value                : WEBRTC_VIDEO_CODEC_OK if OK, < 0 otherwise.
  virtual int RegisterEncodeCompleteCallback(EncodedImageCallback* callback);

  // Inform the encoder of the new packet loss rate and the round-trip time of
  // the network.
  //
  //          - packet_loss : Fraction lost
  //                          (loss rate in percent = 100 * packetLoss / 255)
  //          - rtt         : Round-trip time in milliseconds
  // Return value           : WEBRTC_VIDEO_CODEC_OK if OK
  //                          <0 - Errors: WEBRTC_VIDEO_CODEC_ERROR
  //
  virtual int SetChannelParameters(uint32_t packet_loss, int rtt);

  // Inform the encoder about the new target bit rate.
  //
  //          - new_bitrate_kbit : New target bit rate
  //          - frame_rate       : The target frame rate
  //
  // Return value                : WEBRTC_VIDEO_CODEC_OK if OK, < 0 otherwise.
  virtual int SetRates(uint32_t new_bitrate_kbit, uint32_t frame_rate);

 private:
  VP8Encoder();

  // Call encoder initialize function and set control settings.
  int InitAndSetControlSettings(const VideoCodec* inst);

  // Update frame size for codec.
  int UpdateCodecFrameSize(WebRtc_UWord32 input_image_width,
                           WebRtc_UWord32 input_image_height);

  void PopulateCodecSpecific(CodecSpecificInfo* codec_specific,
                             const vpx_codec_cx_pkt& pkt);

  int GetEncodedFrame(const RawImage& input_image);

  int GetEncodedPartitions(const RawImage& input_image);

  // Determine maximum target for Intra frames
  //
  // Input:
  //    - optimal_buffer_size : Optimal buffer size
  // Return Value             : Max target size for Intra frames represented as
  //                            percentage of the per frame bandwidth
  uint32_t MaxIntraTarget(uint32_t optimal_buffer_size);

  EncodedImage encoded_image_;
  EncodedImageCallback* encoded_complete_callback_;
  VideoCodec codec_;
  bool inited_;
  uint32_t timestamp_;
  uint16_t picture_id_;
  bool feedback_mode_;
  int cpu_speed_;
  uint32_t rc_max_intra_target_;
  int token_partitions_;
  ReferencePictureSelection* rps_;
  TemporalLayers* temporal_layers_;
  vpx_codec_ctx_t* encoder_;
  vpx_codec_enc_cfg_t* config_;
  vpx_image_t* raw_;
};  // end of VP8Encoder class


class VP8Decoder : public VideoDecoder {
 public:
  static VP8Decoder* Create();

  virtual ~VP8Decoder();

  // Initialize the decoder.
  //
  // Return value         :  WEBRTC_VIDEO_CODEC_OK.
  //                        <0 - Errors:
  //                                  WEBRTC_VIDEO_CODEC_ERROR
  virtual int InitDecode(const VideoCodec* inst, int number_of_cores);

  // Decode encoded image (as a part of a video stream). The decoded image
  // will be returned to the user through the decode complete callback.
  //
  // Input:
  //          - input_image         : Encoded image to be decoded
  //          - missing_frames      : True if one or more frames have been lost
  //                                  since the previous decode call.
  //          - fragmentation       : Specifies the start and length of each VP8
  //                                  partition.
  //          - codec_specific_info : pointer to specific codec data
  //          - render_time_ms      : Render time in Ms
  //
  // Return value                 : WEBRTC_VIDEO_CODEC_OK if OK
  //                                <0 - Errors:
  //                                      WEBRTC_VIDEO_CODEC_ERROR
  //                                      WEBRTC_VIDEO_CODEC_ERR_PARAMETER
  virtual int Decode(const EncodedImage& input_image,
                     bool missing_frames,
                     const RTPFragmentationHeader* fragmentation,
                     const CodecSpecificInfo* codec_specific_info,
                     int64_t /*render_time_ms*/);

  // Register a decode complete callback object.
  //
  // Input:
  //          - callback         : Callback object which handles decoded images.
  //
  // Return value                : WEBRTC_VIDEO_CODEC_OK if OK, < 0 otherwise.
  virtual int RegisterDecodeCompleteCallback(DecodedImageCallback* callback);

  // Free decoder memory.
  //
  // Return value                : WEBRTC_VIDEO_CODEC_OK if OK
  //                               <0 - Errors:
  //                                      WEBRTC_VIDEO_CODEC_ERROR
  virtual int Release();

  // Reset decoder state and prepare for a new call.
  //
  // Return value         : WEBRTC_VIDEO_CODEC_OK.
  //                        <0 - Errors:
  //                                  WEBRTC_VIDEO_CODEC_UNINITIALIZED
  //                                  WEBRTC_VIDEO_CODEC_ERROR
  virtual int Reset();

  // Create a copy of the codec and its internal state.
  //
  // Return value                : A copy of the instance if OK, NULL otherwise.
  virtual VideoDecoder* Copy();

 private:
  VP8Decoder();

  // Copy reference image from this _decoder to the _decoder in copyTo. Set
  // which frame type to copy in _refFrame->frame_type before the call to
  // this function.
  int CopyReference(VP8Decoder* copy);

  int DecodePartitions(const EncodedImage& input_image,
                       const RTPFragmentationHeader* fragmentation);

  int ReturnFrame(const vpx_image_t* img, uint32_t timeStamp);

  RawImage decoded_image_;
  DecodedImageCallback* decode_complete_callback_;
  bool inited_;
  bool feedback_mode_;
  vpx_dec_ctx_t* decoder_;
  VideoCodec codec_;
  EncodedImage last_keyframe_;
  int image_format_;
  vpx_ref_frame_t* ref_frame_;
  int propagation_cnt_;
  bool latest_keyframe_complete_;
  bool mfqe_enabled_;
};  // end of VP8Decoder class
}  // namespace webrtc

#endif // WEBRTC_MODULES_VIDEO_CODING_CODECS_VP8_H_
