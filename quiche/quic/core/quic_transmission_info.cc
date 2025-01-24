// Copyright (c) 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quiche/quic/core/quic_transmission_info.h"

#include "absl/strings/str_cat.h"

namespace quic {

QuicTransmissionInfo::QuicTransmissionInfo()
    : sent_time(QuicTime::Zero()),
      bytes_sent(0),
      encryption_level(ENCRYPTION_INITIAL),
      transmission_type(NOT_RETRANSMISSION),
      in_flight(false),
      state(NEVER_SENT),
      has_crypto_handshake(false)
     {}

QuicTransmissionInfo::QuicTransmissionInfo(EncryptionLevel level,
                                           TransmissionType transmission_type,
                                           QuicTime sent_time,
                                           QuicPacketLength bytes_sent,
                                           bool has_crypto_handshake,
                                           QuicFrames& retransmittable_frames) noexcept
    : retransmittable_frames(std::move(retransmittable_frames)),
      sent_time(sent_time),
      bytes_sent(bytes_sent),
      encryption_level(level),
      transmission_type(transmission_type),
      in_flight(false),
      state(OUTSTANDING),
      has_crypto_handshake(has_crypto_handshake)
{}

QuicTransmissionInfo::~QuicTransmissionInfo() = default;

std::string QuicTransmissionInfo::DebugString() const {
  return absl::StrCat(
      "{sent_time: ", sent_time.ToDebuggingValue(),
      ", bytes_sent: ", bytes_sent,
//      ", encryption_level: ", EncryptionLevelToString(encryption_level),
      ", transmission_type: ", TransmissionTypeToString(transmission_type),
      ", in_flight: ", in_flight, ", state: ", state,
//      ", has_crypto_handshake: ", has_crypto_handshake,
      //", has_ack_frequency: ", has_ack_frequency,
      ", first_sent_after_loss: ", first_sent_after_loss.ToString(),
      ", largest_acked: ", largest_acked.ToString(),
      ", retransmittable_frames: ", QuicFramesToString(retransmittable_frames),
      "}");
}

}  // namespace quic
