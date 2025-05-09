// Copyright (c) 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quiche/quic/core/quic_idle_network_detector.h"

#include "quiche/quic/core/quic_constants.h"
#include "quiche/quic/core/quic_time.h"
#include "quiche/quic/platform/api/quic_flag_utils.h"
#include "quiche/quic/platform/api/quic_flags.h"
#include "quiche/common/platform/api/quiche_logging.h"

namespace quic {

namespace {

class AlarmDelegate : public QuicAlarm::DelegateWithContext {
 public:
  explicit AlarmDelegate(QuicIdleNetworkDetector* detector,
                         QuicConnectionContext* context)
      : QuicAlarm::DelegateWithContext(context), detector_(detector) {}
  AlarmDelegate(const AlarmDelegate&) = delete;
  AlarmDelegate& operator=(const AlarmDelegate&) = delete;

  void OnAlarm() override { detector_->OnAlarm(); }

 private:
  QuicIdleNetworkDetector* detector_;
};

}  // namespace

QuicIdleNetworkDetector::QuicIdleNetworkDetector(
    Delegate* delegate, QuicTime now, QuicConnectionArena* arena,
    QuicAlarmFactory* alarm_factory, QuicConnectionContext* context)
    : delegate_(delegate),
      start_time_(now),
      handshake_timeout_(QuicTime::Delta::Infinite()),
      time_of_last_received_packet_(now),
      time_of_first_packet_sent_after_receiving_(QuicTime::Zero()),
      idle_network_timeout_(QuicTime::Delta::Infinite()),
      alarm_(alarm_factory->CreateAlarm(
          arena->New<AlarmDelegate>(this, context), arena)) {}

void QuicIdleNetworkDetector::OnAlarm() {
  if (handshake_timeout_.IsInfinite()) {
    const QuicTime::Delta duration = delegate_->ApproximateNow() - last_network_activity_time();
    if (duration < idle_network_timeout_) {
      SetAlarm();
      return;
    }
    delegate_->OnIdleNetworkDetected();
    return;
  }
  if (idle_network_timeout_.IsInfinite()) {
    delegate_->OnHandshakeTimeout();
    return;
  }
  if (last_network_activity_time() + idle_network_timeout_ >
      start_time_ + handshake_timeout_) {
    delegate_->OnHandshakeTimeout();
    return;
  }
  delegate_->OnIdleNetworkDetected();
}

void QuicIdleNetworkDetector::SetTimeouts(
    QuicTime::Delta handshake_timeout, QuicTime::Delta idle_network_timeout) {
  handshake_timeout_ = handshake_timeout;
  idle_network_timeout_ = idle_network_timeout;

  SetAlarm();
}

void QuicIdleNetworkDetector::StopDetection() {
  alarm_->PermanentCancel();
  handshake_timeout_ = QuicTime::Delta::Infinite();
  idle_network_timeout_ = QuicTime::Delta::Infinite();
  stopped_ = true;
}

void QuicIdleNetworkDetector::OnPacketSent(QuicTime now,
                                           QuicTime::Delta pto_delay) {
  if (time_of_first_packet_sent_after_receiving_ >=
      time_of_last_received_packet_) {
    return;
  }
  QUICHE_DCHECK(time_of_first_packet_sent_after_receiving_ <= now);
  time_of_first_packet_sent_after_receiving_ = now;
//      std::max(time_of_first_packet_sent_after_receiving_, now);
  if (shorter_idle_timeout_on_sent_packet_) {
    MaybeSetAlarmOnSentPacket(pto_delay);
    return;
  }
  if (!alarm_->IsSet()) //TODO3 hybchanged. follow is set if receive packet.
    SetAlarm();
}

void QuicIdleNetworkDetector::OnPacketReceived(QuicTime now) {
  QUICHE_DCHECK(time_of_last_received_packet_ <= now);
  //if (time_of_last_received_packet_ < now)
  time_of_last_received_packet_ = now;
  if (!alarm_->IsSet())
    SetAlarm();
}

void QuicIdleNetworkDetector::SetAlarm() {
  if (false && stopped_) {
    // TODO(wub): If this QUIC_BUG fires, it indicates a problem in the
    // QuicConnection, which somehow called this function while disconnected.
    // That problem needs to be fixed.
    QUIC_BUG(quic_idle_detector_set_alarm_after_stopped)
        << "SetAlarm called after stopped";
    return;
  }
  // Set alarm to the nearer deadline.
  QuicTime new_deadline = QuicTime::Zero();
  if (false && !handshake_timeout_.IsInfinite()) {
    new_deadline = start_time_ + handshake_timeout_;
  }
  //QUICHE_DCHECK(!idle_network_timeout_.IsInfinite());
  if (true || !idle_network_timeout_.IsInfinite()) {
    if (!handshake_timeout_.IsInfinite()) {
      new_deadline = start_time_ + handshake_timeout_;
    } else {
      new_deadline = GetIdleNetworkDeadline();
    }
  }
  alarm_->Update(new_deadline, QuicTime::Delta::FromMilliseconds(300));
}

void QuicIdleNetworkDetector::MaybeSetAlarmOnSentPacket(
    QuicTime::Delta pto_delay) {
  QUICHE_DCHECK(shorter_idle_timeout_on_sent_packet_);
  if (!handshake_timeout_.IsInfinite() || !alarm_->IsSet()) {
    SetAlarm();
    return;
  }
  // Make sure connection will be alive for another PTO.
  const QuicTime deadline = alarm_->deadline();
  const QuicTime min_deadline = last_network_activity_time() + pto_delay;
  if (deadline > min_deadline) {
    return;
  }
  alarm_->Update(min_deadline, kAlarmGranularity);
}

QuicTime QuicIdleNetworkDetector::GetIdleNetworkDeadline() const {
  if (false && idle_network_timeout_.IsInfinite()) {
    return QuicTime::Zero();
  }
  return last_network_activity_time() + idle_network_timeout_;
}

}  // namespace quic
