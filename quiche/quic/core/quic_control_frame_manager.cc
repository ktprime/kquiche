// Copyright (c) 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quiche/quic/core/quic_control_frame_manager.h"

#include <string>

#include "absl/strings/str_cat.h"
#include "quiche/quic/core/frames/quic_ack_frequency_frame.h"
#include "quiche/quic/core/frames/quic_frame.h"
#include "quiche/quic/core/frames/quic_new_connection_id_frame.h"
#include "quiche/quic/core/frames/quic_retire_connection_id_frame.h"
#include "quiche/quic/core/quic_constants.h"
#include "quiche/quic/core/quic_session.h"
#include "quiche/quic/core/quic_types.h"
#include "quiche/quic/core/quic_utils.h"
#include "quiche/quic/platform/api/quic_bug_tracker.h"
#include "quiche/quic/platform/api/quic_flag_utils.h"
#include "quiche/quic/platform/api/quic_flags.h"

namespace quic {

namespace {

// The maximum number of buffered control frames which are waiting to be ACKed
// or sent for the first time.
constexpr size_t kMaxNumControlFrames = 100 / 4;

}  // namespace

QuicControlFrameManager::QuicControlFrameManager(QuicSession* session)
    : last_control_frame_id_(kInvalidControlFrameId),
      least_unacked_(1),
      least_unsent_(1),
      delegate_(session) {}

QuicControlFrameManager::~QuicControlFrameManager() {
  while (!control_frames_.empty()) {
    DeleteFrame(&control_frames_.front());
    control_frames_.erase(control_frames_.begin());
  }
}

void QuicControlFrameManager::WriteOrBufferQuicFrame(QuicFrame frame) {
  const bool had_buffered_frames = HasBufferedFrames();
  control_frames_.emplace_back(frame);
  if (had_buffered_frames) {
    if (control_frames_.size() > kMaxNumControlFrames) {
      delegate_->OnControlFrameManagerError(
        QUIC_TOO_MANY_BUFFERED_CONTROL_FRAMES,
        absl::StrCat("More than ", kMaxNumControlFrames,
          "buffered control frames, least_unacked: ", least_unacked_,
          ", least_unsent_: ", least_unsent_));
    }
    return;
  }

  WriteBufferedFrames();
}

void QuicControlFrameManager::WriteOrBufferRstStream(
    QuicStreamId id, QuicResetStreamError error,
    QuicStreamOffset bytes_written) {
  QUIC_DVLOG(1) << "Writing RST_STREAM_FRAME";
  WriteOrBufferQuicFrame(QuicFrame(new QuicRstStreamFrame(
      ++last_control_frame_id_, id, error, bytes_written)));
}

void QuicControlFrameManager::WriteOrBufferGoAway(
    QuicErrorCode error, QuicStreamId last_good_stream_id,
    const std::string& reason) {
  QUIC_DVLOG(1) << "Writing GOAWAY_FRAME";
  WriteOrBufferQuicFrame(QuicFrame(new QuicGoAwayFrame(
      ++last_control_frame_id_, error, last_good_stream_id, reason)));
}

void QuicControlFrameManager::WriteOrBufferWindowUpdate(
    QuicStreamId id, QuicStreamOffset byte_offset) {
  QUIC_DVLOG(1) << "Writing WINDOW_UPDATE_FRAME";
  WriteOrBufferQuicFrame(QuicFrame(
      QuicWindowUpdateFrame(++last_control_frame_id_, id, byte_offset)));
}

void QuicControlFrameManager::WriteOrBufferBlocked(
    QuicStreamId id, QuicStreamOffset byte_offset) {
  QUIC_DVLOG(1) << "Writing BLOCKED_FRAME";
  WriteOrBufferQuicFrame(
      QuicFrame(QuicBlockedFrame(++last_control_frame_id_, id, byte_offset)));
}

void QuicControlFrameManager::WriteOrBufferStreamsBlocked(QuicStreamCount count,
                                                          bool unidirectional) {
#if QUIC_TLS_SESSION
  QUIC_DVLOG(1) << "Writing STREAMS_BLOCKED Frame";
  QUIC_CODE_COUNT(quic_streams_blocked_transmits);
  WriteOrBufferQuicFrame(QuicFrame(QuicStreamsBlockedFrame(
      ++last_control_frame_id_, count, unidirectional)));
#endif
}

void QuicControlFrameManager::WriteOrBufferMaxStreams(QuicStreamCount count,
                                                      bool unidirectional) {
#if QUIC_TLS_SESSION
  QUIC_DVLOG(1) << "Writing MAX_STREAMS Frame";
  QUIC_CODE_COUNT(quic_max_streams_transmits);
  WriteOrBufferQuicFrame(QuicFrame(
      QuicMaxStreamsFrame(++last_control_frame_id_, count, unidirectional)));
#endif
}

void QuicControlFrameManager::WriteOrBufferStopSending(
    QuicResetStreamError error, QuicStreamId stream_id) {
#if QUIC_TLS_SESSION
  QUIC_DVLOG(1) << "Writing STOP_SENDING_FRAME";
  WriteOrBufferQuicFrame(QuicFrame(
      QuicStopSendingFrame(++last_control_frame_id_, stream_id, error)));
#endif
}

void QuicControlFrameManager::WriteOrBufferHandshakeDone() {
  QUIC_DVLOG(1) << "Writing HANDSHAKE_DONE";
  WriteOrBufferQuicFrame(
      QuicFrame(QuicHandshakeDoneFrame(++last_control_frame_id_)));
}

void QuicControlFrameManager::WriteOrBufferAckFrequency(
    const QuicAckFrequencyFrame& ack_frequency_frame) {
#if QUIC_TLS_SESSION
  QUIC_DVLOG(1) << "Writing ACK_FREQUENCY frame";
  QuicControlFrameId control_frame_id = ++last_control_frame_id_;
  // Using the control_frame_id for sequence_number here leaves gaps in
  // sequence_number.
  WriteOrBufferQuicFrame(
      QuicFrame(new QuicAckFrequencyFrame(control_frame_id,
                                          /*sequence_number=*/control_frame_id,
                                          ack_frequency_frame.packet_tolerance,
                                          ack_frequency_frame.max_ack_delay)));
#endif
}

void QuicControlFrameManager::WriteOrBufferNewConnectionId(
    const QuicConnectionId& connection_id, uint64_t sequence_number,
    uint64_t retire_prior_to,
    const StatelessResetToken& stateless_reset_token) {
#if QUIC_TLS_SESSION
  QUIC_DVLOG(1) << "Writing NEW_CONNECTION_ID frame";
  WriteOrBufferQuicFrame(QuicFrame(new QuicNewConnectionIdFrame(
      ++last_control_frame_id_, connection_id, sequence_number,
      stateless_reset_token, retire_prior_to)));
#endif
}

void QuicControlFrameManager::WriteOrBufferRetireConnectionId(
    uint64_t sequence_number) {
#if QUIC_TLS_SESSION
  QUIC_DVLOG(1) << "Writing RETIRE_CONNECTION_ID frame";
  WriteOrBufferQuicFrame(QuicFrame(new QuicRetireConnectionIdFrame(
      ++last_control_frame_id_, sequence_number)));
#endif
}

void QuicControlFrameManager::WriteOrBufferNewToken(absl::string_view token) {
#if QUIC_TLS_SESSION
  QUIC_DVLOG(1) << "Writing NEW_TOKEN frame";
  WriteOrBufferQuicFrame(
      QuicFrame(new QuicNewTokenFrame(++last_control_frame_id_, token)));
#endif
}

void QuicControlFrameManager::OnControlFrameSent(const QuicFrame& frame) {
  QuicControlFrameId id = GetControlFrameId(frame);
  if (DCHECK_FLAG && id == kInvalidControlFrameId) {
    QUIC_BUG(quic_bug_12727_1)
        << "Send or retransmit a control frame with invalid control frame id";
    return;
  }
  if (frame.type == WINDOW_UPDATE_FRAME) {
    QuicStreamId stream_id = frame.window_update_frame.stream_id;
    auto wid = window_update_frames_.at(stream_id); //TODO2, not find set zero
    if (wid > 0 && id > wid) {
      // Consider the older window update of the same stream as acked.
      OnControlFrameIdAcked(wid);
    }
    window_update_frames_[stream_id] = id;
  }
  if (!pending_retransmissions_.empty() &&
      pending_retransmissions_.erase(id)) {
    // This is retransmitted control frame.
    return;
  }
  if (DCHECK_FLAG && id > least_unsent_) {
    QUIC_BUG(quic_bug_10517_1)
        << "Try to send control frames out of order, id: " << id
        << " least_unsent: " << least_unsent_;
    delegate_->OnControlFrameManagerError(
        QUIC_INTERNAL_ERROR, "Try to send control frames out of order");
    return;
  }
  ++least_unsent_;
}

bool QuicControlFrameManager::OnControlFrameAcked(const QuicFrame& frame) {
  QuicControlFrameId id = GetControlFrameId(frame);
  auto acked = OnControlFrameIdAcked(id);
  if (frame.type == WINDOW_UPDATE_FRAME && acked) {
    QuicStreamId stream_id = frame.window_update_frame.stream_id;
    QUICHE_DCHECK(window_update_frames_.at(stream_id) == id);
    {
      window_update_frames_.erase(stream_id);
    }
  }
  return acked;
}

void QuicControlFrameManager::OnControlFrameLost(const QuicFrame& frame) {
  QuicControlFrameId id = GetControlFrameId(frame);
#if 0
  if (DCHECK_FLAG && id == kInvalidControlFrameId) {
    // Frame does not have a valid control frame ID, ignore it.
    return;
  }
  if (DCHECK_FLAG && id >= least_unsent_) {
    QUIC_BUG(quic_bug_10517_2) << "Try to mark unsent control frame as lost";
    delegate_->OnControlFrameManagerError(
        QUIC_INTERNAL_ERROR, "Try to mark unsent control frame as lost");
    return;
  }
#endif
  if (id - least_unacked_ >= control_frames_.size() ||
      GetControlFrameId(control_frames_.at(id - least_unacked_)) ==
          kInvalidControlFrameId) {
    // This frame has already been acked.
    return;
  }
  if (pending_retransmissions_.insert_or_assign(id, true).second) {
    QUIC_BUG_IF(quic_bug_12727_2,
                pending_retransmissions_.size() > control_frames_.size())
        << "least_unacked_: " << least_unacked_
        << ", least_unsent_: " << least_unsent_;
  }
}

bool QuicControlFrameManager::IsControlFrameOutstanding(
    const QuicFrame& frame) const {
  QuicControlFrameId id = GetControlFrameId(frame);
  if (id - least_unacked_ >= control_frames_.size()) {
    return false;
  }

  return GetControlFrameId(control_frames_.at(id - least_unacked_)) != kInvalidControlFrameId;

#if 0
  if (id == kInvalidControlFrameId) {
    // Frame without a control frame ID should not be retransmitted.
    return false;
  }
  // Consider this frame is outstanding if it does not get acked.
  return id < least_unacked_ + control_frames_.size() && id >= least_unacked_ &&
         GetControlFrameId(control_frames_.at(id - least_unacked_)) !=
             kInvalidControlFrameId;
#endif
}

bool QuicControlFrameManager::HasPendingRetransmission() const {
  return !pending_retransmissions_.empty();
}

bool QuicControlFrameManager::WillingToWrite() const {
  return !pending_retransmissions_.empty() || least_unsent_ < least_unacked_ + control_frames_.size();
}

QuicFrame QuicControlFrameManager::NextPendingRetransmission() const {
  QUIC_BUG_IF(quic_bug_12727_3, pending_retransmissions_.empty())
      << "Unexpected call to NextPendingRetransmission() with empty pending "
      << "retransmission list.";
  QuicControlFrameId id = pending_retransmissions_.begin()->first;
  return control_frames_.at(id - least_unacked_);
}

void QuicControlFrameManager::OnCanWrite() {
  if (HasPendingRetransmission()) {
    // Exit early to allow streams to write pending retransmissions if any.
    WritePendingRetransmission();
    //return;
  }
  WriteBufferedFrames();
}

bool QuicControlFrameManager::RetransmitControlFrame(const QuicFrame& frame,
                                                     TransmissionType type) {
  QUICHE_DCHECK(type == PTO_RETRANSMISSION);
  QuicControlFrameId id = GetControlFrameId(frame);
#if 0
  if (id == kInvalidControlFrameId) {
    // Frame does not have a valid control frame ID, ignore it. Returns true
    // to allow writing following frames.
    return true;
  }
  if (id >= least_unsent_) {
    QUIC_BUG(quic_bug_10517_3) << "Try to retransmit unsent control frame";
    delegate_->OnControlFrameManagerError(
        QUIC_INTERNAL_ERROR, "Try to retransmit unsent control frame");
    return false;
  }
#endif
  if (id - least_unacked_ >= control_frames_.size()) {
    return true;
  }

  if (//id < least_unacked_ ||
      GetControlFrameId(control_frames_.at(id - least_unacked_)) ==
          kInvalidControlFrameId) {
    // This frame has already been acked.
    return true;
  }
  QuicFrame copy = CopyRetransmittableControlFrame(frame);
  QUIC_DVLOG(1) << "control frame manager is forced to retransmit frame: "
                << frame;
  if (delegate_->WriteControlFrame(copy, type)) {
    return true;
  }
  DeleteFrame(&copy);
  return false;
}

void QuicControlFrameManager::WriteBufferedFrames() {
  while (HasBufferedFrames()) {
    QuicFrame frame_to_send =
        control_frames_.at(least_unsent_ - least_unacked_);
    QuicFrame copy = CopyRetransmittableControlFrame(frame_to_send);
    if (!delegate_->WriteControlFrame(copy, NOT_RETRANSMISSION)) {
      // Connection is write blocked.
      DeleteFrame(&copy);
      break;
    }
    OnControlFrameSent(frame_to_send);
  }
}

void QuicControlFrameManager::WritePendingRetransmission() {
  while (HasPendingRetransmission()) {
    QuicFrame pending = NextPendingRetransmission();
    QuicFrame copy = CopyRetransmittableControlFrame(pending);
    if (!delegate_->WriteControlFrame(copy, LOSS_RETRANSMISSION)) {
      // Connection is write blocked.
      DeleteFrame(&copy);
      break;
    }
    OnControlFrameSent(pending);
  }
}

bool QuicControlFrameManager::OnControlFrameIdAcked(QuicControlFrameId id) {
  if (id - least_unacked_ >= control_frames_.size()) {
    // Frame does not have a valid control frame ID, ignore it.
    return false;
  }
#if 0
  if (id >= least_unsent_) {
    QUIC_BUG(quic_bug_10517_4) << "Try to ack unsent control frame";
    delegate_->OnControlFrameManagerError(QUIC_INTERNAL_ERROR,
                                          "Try to ack unsent control frame");
    return false;
  }
  if (id < least_unacked_ ||
      GetControlFrameId(control_frames_.at(id - least_unacked_)) ==
          kInvalidControlFrameId) {
    // This frame has already been acked.
    return false;
  }
#endif

  // Set control frame ID of acked frames to 0.
  SetControlFrameId(kInvalidControlFrameId,
                    &control_frames_[id - least_unacked_]);
  // Remove acked control frames from pending retransmissions.
  if (!pending_retransmissions_.empty())
  pending_retransmissions_.erase(id);
  // Clean up control frames queue and increment least_unacked_.
  while (!control_frames_.empty() &&
         GetControlFrameId(control_frames_.front()) == kInvalidControlFrameId) {
    if (control_frames_.front().type != WINDOW_UPDATE_FRAME)
    DeleteFrame(&control_frames_.front());
    control_frames_.erase(control_frames_.begin());
    ++least_unacked_;
  }
  return true;
}

bool QuicControlFrameManager::HasBufferedFrames() const {
  return least_unsent_ < least_unacked_ + control_frames_.size();
}

}  // namespace quic
