/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/modules/pacing/include/paced_sender.h"

#include <assert.h>

#include "webrtc/modules/interface/module_common_types.h"
#include "webrtc/system_wrappers/interface/critical_section_wrapper.h"
#include "webrtc/system_wrappers/interface/trace_event.h"

namespace {
// Time limit in milliseconds between packet bursts.
const int kMinPacketLimitMs = 5;

// Upper cap on process interval, in case process has not been called in a long
// time.
const int kMaxIntervalTimeMs = 30;

// Max time that the first packet in the queue can sit in the queue if no
// packets are sent, regardless of buffer state. In practice only in effect at
// low bitrates (less than 320 kbits/s).
const int kMaxQueueTimeWithoutSendingMs = 30;

// Max padding bytes per second.
const int kMaxPaddingKbps = 800;

}  // namespace

namespace webrtc {

namespace paced_sender {
struct Packet {
  Packet(uint32_t ssrc, uint16_t seq_number, int64_t capture_time_ms,
         int length_in_bytes)
      : ssrc_(ssrc),
        sequence_number_(seq_number),
        capture_time_ms_(capture_time_ms),
        bytes_(length_in_bytes) {
  }
  uint32_t ssrc_;
  uint16_t sequence_number_;
  int64_t capture_time_ms_;
  int bytes_;
};

// STL list style class which prevents duplicates in the list.
class PacketList {
 public:
  PacketList() {};

  bool empty() const {
    return packet_list_.empty();
  }

  Packet front() const {
    return packet_list_.front();
  }

  void pop_front() {
    Packet& packet = packet_list_.front();
    uint16_t sequence_number = packet.sequence_number_;
    packet_list_.pop_front();
    sequence_number_set_.erase(sequence_number);
  }

  void push_back(const Packet& packet) {
    if (sequence_number_set_.find(packet.sequence_number_) ==
        sequence_number_set_.end()) {
      // Don't insert duplicates.
      packet_list_.push_back(packet);
      sequence_number_set_.insert(packet.sequence_number_);
    }
  }

 private:
  std::list<Packet> packet_list_;
  std::set<uint16_t> sequence_number_set_;
};

class IntervalBudget {
 public:
  explicit IntervalBudget(int initial_target_rate_kbps)
      : target_rate_kbps_(initial_target_rate_kbps),
        bytes_remaining_(0) {}

  void set_target_rate_kbps(int target_rate_kbps) {
    target_rate_kbps_ = target_rate_kbps;
  }

  void IncreaseBudget(int delta_time_ms) {
    int bytes = target_rate_kbps_ * delta_time_ms / 8;
    if (bytes_remaining_ < 0) {
      // We overused last interval, compensate this interval.
      bytes_remaining_ = bytes_remaining_ + bytes;
    } else {
      // If we underused last interval we can't use it this interval.
      bytes_remaining_ = bytes;
    }
  }

  void UseBudget(int bytes) {
    bytes_remaining_ = std::max(bytes_remaining_ - bytes,
                                -100 * target_rate_kbps_ / 8);
  }

  int bytes_remaining() const { return bytes_remaining_; }

 private:
  int target_rate_kbps_;
  int bytes_remaining_;
};
}  // namespace paced_sender

PacedSender::PacedSender(Callback* callback, int target_bitrate_kbps,
                         float pace_multiplier)
    : callback_(callback),
      pace_multiplier_(pace_multiplier),
      enabled_(false),
      paused_(false),
      critsect_(CriticalSectionWrapper::CreateCriticalSection()),
      media_budget_(new paced_sender::IntervalBudget(
          pace_multiplier_ * target_bitrate_kbps)),
      padding_budget_(new paced_sender::IntervalBudget(kMaxPaddingKbps)),
      // No padding until UpdateBitrate is called.
      pad_up_to_bitrate_budget_(new paced_sender::IntervalBudget(0)),
      time_last_update_(TickTime::Now()),
      capture_time_ms_last_queued_(0),
      capture_time_ms_last_sent_(0),
      high_priority_packets_(new paced_sender::PacketList),
      normal_priority_packets_(new paced_sender::PacketList),
      low_priority_packets_(new paced_sender::PacketList) {
  UpdateBytesPerInterval(kMinPacketLimitMs);
}

PacedSender::~PacedSender() {
}

void PacedSender::Pause() {
  CriticalSectionScoped cs(critsect_.get());
  paused_ = true;
}

void PacedSender::Resume() {
  CriticalSectionScoped cs(critsect_.get());
  paused_ = false;
}

void PacedSender::SetStatus(bool enable) {
  CriticalSectionScoped cs(critsect_.get());
  enabled_ = enable;
}

bool PacedSender::Enabled() const {
  CriticalSectionScoped cs(critsect_.get());
  return enabled_;
}

void PacedSender::UpdateBitrate(int target_bitrate_kbps,
                                int pad_up_to_bitrate_kbps) {
  CriticalSectionScoped cs(critsect_.get());
  media_budget_->set_target_rate_kbps(pace_multiplier_ * target_bitrate_kbps);
  pad_up_to_bitrate_budget_->set_target_rate_kbps(pad_up_to_bitrate_kbps);
}

bool PacedSender::SendPacket(Priority priority, uint32_t ssrc,
    uint16_t sequence_number, int64_t capture_time_ms, int bytes) {
  CriticalSectionScoped cs(critsect_.get());

  if (!enabled_) {
    UpdateMediaBytesSent(bytes);
    return true;  // We can send now.
  }
  if (capture_time_ms < 0) {
    capture_time_ms = TickTime::MillisecondTimestamp();
  }
  if (paused_) {
    // Queue all packets when we are paused.
    switch (priority) {
      case kHighPriority:
        high_priority_packets_->push_back(paced_sender::Packet(ssrc,
                                                               sequence_number,
                                                               capture_time_ms,
                                                               bytes));
        break;
      case kNormalPriority:
        if (capture_time_ms > capture_time_ms_last_queued_) {
          capture_time_ms_last_queued_ = capture_time_ms;
          TRACE_EVENT_ASYNC_BEGIN1("webrtc_rtp", "PacedSend", capture_time_ms,
                                   "capture_time_ms", capture_time_ms);
        }
      case kLowPriority:
        // Queue the low priority packets in the normal priority queue when we
        // are paused to avoid starvation.
        normal_priority_packets_->push_back(paced_sender::Packet(
            ssrc, sequence_number, capture_time_ms, bytes));
        break;
    }
    return false;
  }
  paced_sender::PacketList* packet_list;
  switch (priority) {
    case kHighPriority:
      packet_list = high_priority_packets_.get();
      break;
    case kNormalPriority:
      packet_list = normal_priority_packets_.get();
      break;
    case kLowPriority:
      packet_list = low_priority_packets_.get();
      break;
  }
  if (packet_list->empty() &&
      media_budget_->bytes_remaining() > 0) {
    UpdateMediaBytesSent(bytes);
    return true;  // We can send now.
  }
  packet_list->push_back(paced_sender::Packet(ssrc, sequence_number,
                                              capture_time_ms, bytes));
  return false;
}

int PacedSender::QueueInMs() const {
  CriticalSectionScoped cs(critsect_.get());
  int64_t now_ms = TickTime::MillisecondTimestamp();
  int64_t oldest_packet_capture_time = now_ms;
  if (!high_priority_packets_->empty()) {
    oldest_packet_capture_time = std::min(
        oldest_packet_capture_time,
        high_priority_packets_->front().capture_time_ms_);
  }
  if (!normal_priority_packets_->empty()) {
    oldest_packet_capture_time = std::min(
        oldest_packet_capture_time,
        normal_priority_packets_->front().capture_time_ms_);
  }
  if (!low_priority_packets_->empty()) {
    oldest_packet_capture_time = std::min(
        oldest_packet_capture_time,
        low_priority_packets_->front().capture_time_ms_);
  }
  return now_ms - oldest_packet_capture_time;
}

int32_t PacedSender::TimeUntilNextProcess() {
  CriticalSectionScoped cs(critsect_.get());
  int64_t elapsed_time_ms =
      (TickTime::Now() - time_last_update_).Milliseconds();
  if (elapsed_time_ms <= 0) {
    return kMinPacketLimitMs;
  }
  if (elapsed_time_ms >= kMinPacketLimitMs) {
    return 0;
  }
  return kMinPacketLimitMs - elapsed_time_ms;
}

int32_t PacedSender::Process() {
  TickTime now = TickTime::Now();
  CriticalSectionScoped cs(critsect_.get());
  int elapsed_time_ms = (now - time_last_update_).Milliseconds();
  time_last_update_ = now;
  if (!paused_ && elapsed_time_ms > 0) {
    uint32_t delta_time_ms = std::min(kMaxIntervalTimeMs, elapsed_time_ms);
    UpdateBytesPerInterval(delta_time_ms);
    uint32_t ssrc;
    uint16_t sequence_number;
    int64_t capture_time_ms;
    Priority priority;
    bool last_packet;
    while (GetNextPacket(&ssrc, &sequence_number, &capture_time_ms,
                         &priority, &last_packet)) {
      if (priority == kNormalPriority) {
        if (capture_time_ms > capture_time_ms_last_sent_) {
          capture_time_ms_last_sent_ = capture_time_ms;
        } else if (capture_time_ms == capture_time_ms_last_sent_ &&
                   last_packet) {
          TRACE_EVENT_ASYNC_END0("webrtc_rtp", "PacedSend", capture_time_ms);
        }
      }
      critsect_->Leave();
      callback_->TimeToSendPacket(ssrc, sequence_number, capture_time_ms);
      critsect_->Enter();
    }
    if (high_priority_packets_->empty() &&
        normal_priority_packets_->empty() &&
        low_priority_packets_->empty() &&
        padding_budget_->bytes_remaining() > 0 &&
        pad_up_to_bitrate_budget_->bytes_remaining() > 0) {
      int padding_needed = std::min(
          padding_budget_->bytes_remaining(),
          pad_up_to_bitrate_budget_->bytes_remaining());
      critsect_->Leave();
      int bytes_sent = callback_->TimeToSendPadding(padding_needed);
      critsect_->Enter();
      media_budget_->UseBudget(bytes_sent);
      padding_budget_->UseBudget(bytes_sent);
      pad_up_to_bitrate_budget_->UseBudget(bytes_sent);
    }
  }
  return 0;
}

// MUST have critsect_ when calling.
void PacedSender::UpdateBytesPerInterval(uint32_t delta_time_ms) {
  media_budget_->IncreaseBudget(delta_time_ms);
  padding_budget_->IncreaseBudget(delta_time_ms);
  pad_up_to_bitrate_budget_->IncreaseBudget(delta_time_ms);
}

// MUST have critsect_ when calling.
bool PacedSender::GetNextPacket(uint32_t* ssrc, uint16_t* sequence_number,
                                int64_t* capture_time_ms, Priority* priority,
                                bool* last_packet) {
  if (media_budget_->bytes_remaining() <= 0) {
    // All bytes consumed for this interval.
    // Check if we have not sent in a too long time.
    if ((TickTime::Now() - time_last_send_).Milliseconds() >
        kMaxQueueTimeWithoutSendingMs) {
      if (!high_priority_packets_->empty()) {
        *priority = kHighPriority;
        GetNextPacketFromList(high_priority_packets_.get(), ssrc,
                              sequence_number, capture_time_ms, last_packet);
        return true;
      }
      if (!normal_priority_packets_->empty()) {
        *priority = kNormalPriority;
        GetNextPacketFromList(normal_priority_packets_.get(), ssrc,
                              sequence_number, capture_time_ms, last_packet);
        return true;
      }
    }
    return false;
  }
  if (!high_priority_packets_->empty()) {
    *priority = kHighPriority;
    GetNextPacketFromList(high_priority_packets_.get(), ssrc, sequence_number,
                          capture_time_ms, last_packet);
    return true;
  }
  if (!normal_priority_packets_->empty()) {
    *priority = kNormalPriority;
    GetNextPacketFromList(normal_priority_packets_.get(), ssrc,
                          sequence_number, capture_time_ms, last_packet);
    return true;
  }
  if (!low_priority_packets_->empty()) {
    *priority = kLowPriority;
    GetNextPacketFromList(low_priority_packets_.get(), ssrc, sequence_number,
                          capture_time_ms, last_packet);
    return true;
  }
  return false;
}

void PacedSender::GetNextPacketFromList(paced_sender::PacketList* packets,
    uint32_t* ssrc, uint16_t* sequence_number, int64_t* capture_time_ms,
    bool* last_packet) {
  paced_sender::Packet packet = packets->front();
  UpdateMediaBytesSent(packet.bytes_);
  *sequence_number = packet.sequence_number_;
  *ssrc = packet.ssrc_;
  *capture_time_ms = packet.capture_time_ms_;
  packets->pop_front();
  *last_packet = packets->empty() ||
      packets->front().capture_time_ms_ > *capture_time_ms;
}

// MUST have critsect_ when calling.
void PacedSender::UpdateMediaBytesSent(int num_bytes) {
  time_last_send_ = TickTime::Now();
  media_budget_->UseBudget(num_bytes);
  pad_up_to_bitrate_budget_->UseBudget(num_bytes);
}

}  // namespace webrtc
