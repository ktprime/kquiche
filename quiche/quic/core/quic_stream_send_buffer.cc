// Copyright (c) 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quiche/quic/core/quic_stream_send_buffer.h"

#include <algorithm>

#include "quiche/quic/core/quic_data_writer.h"
#include "quiche/quic/core/quic_interval.h"
#include "quiche/quic/core/quic_utils.h"
#include "quiche/quic/platform/api/quic_bug_tracker.h"
#include "quiche/quic/platform/api/quic_flag_utils.h"
#include "quiche/quic/platform/api/quic_flags.h"
#include "quiche/quic/platform/api/quic_logging.h"
#include "quiche/common/platform/api/quiche_mem_slice.h"

namespace quic {

namespace {

struct CompareOffset {
  bool operator()(const BufferedSlice& slice, QuicStreamOffset offset) const {
    return slice.offset + slice.slice.length() < offset;
  }
};

}  // namespace

BufferedSlice::BufferedSlice(quiche::QuicheMemSlice mem_slice,
                             QuicStreamOffset offset)
    : slice(std::move(mem_slice)), offset(offset) {}

BufferedSlice::BufferedSlice(BufferedSlice&& other) = default;

BufferedSlice& BufferedSlice::operator=(BufferedSlice&& other) = default;

BufferedSlice::~BufferedSlice() {}

QuicInterval<std::size_t> BufferedSlice::interval() const {
  const std::size_t length = slice.length();
  return QuicInterval<std::size_t>(offset, offset + length);
}

bool StreamPendingRetransmission::operator==(
    const StreamPendingRetransmission& other) const {
  return offset == other.offset && length == other.length;
}

QuicStreamSendBuffer::QuicStreamSendBuffer(
    quiche::QuicheBufferAllocator* allocator)
    : current_end_offset_(0),
      stream_offset_(0),
      stream_bytes_start_(0),
      stream_bytes_written_(0),
      stream_bytes_outstanding_(0)
{
  bytes_acked_.AddEmpty(0);
}

QuicStreamSendBuffer::~QuicStreamSendBuffer() {
  for (size_t i = 0; i < blocks_.size(); ++i) {
    if (blocks_[i] != nullptr) {
      delete (blocks_[i]);
      blocks_[i] = nullptr;
    }
  }
  blocks_.clear();
}

void QuicStreamSendBuffer::SaveStreamData(std::string_view data) {
  QUICHE_DCHECK(!data.empty());

  // Latch the maximum data slice size.
  constexpr QuicByteCount max_data_slice_size = kBlockSizeBytes;
  const auto cindex = GetBlockIndex(stream_offset_ + data.length());
  while (cindex >= blocks_.size()) {
    blocks_.push_back(new BufferBlock);
  }

  const auto offset = GetInBlockOffset(stream_offset_);
  auto index  = GetBlockIndex(stream_offset_);
  stream_offset_ += data.length();
  current_end_offset_ = std::max(current_end_offset_, stream_offset_);
  if (offset + data.length() <= max_data_slice_size) {
    memcpy(blocks_[index]->buffer + offset, data.data(), data.length());
    return;
  }

  memcpy(blocks_[index]->buffer + offset, data.data(), max_data_slice_size - offset);
  data = data.substr(max_data_slice_size - offset);

  for (auto csize = 0; csize < data.size(); csize += max_data_slice_size) {
    const auto slice_size = std::min(max_data_slice_size, data.size() - csize);
    memcpy(blocks_[++index]->buffer, data.data() + csize, slice_size);
  }
}

void QuicStreamSendBuffer::SaveMemSlice(quiche::QuicheMemSlice slice) {
  QUIC_DVLOG(2) << "Save slice offset " << stream_offset_ << " length "
                << slice.length();

  SaveStreamData(std::string_view(slice.data(), slice.length()));
}

QuicByteCount QuicStreamSendBuffer::SaveMemSliceSpan(
    absl::Span<quiche::QuicheMemSlice> span) {
  QuicByteCount total = 0;
  for (quiche::QuicheMemSlice& slice : span) {
    QUICHE_DCHECK(slice.length());
    if (false && slice.length() == 0) {
      // Skip empty slices.
      continue;
    }
    total += slice.length();
    SaveMemSlice(std::move(slice));
  }
  return total;
}

void QuicStreamSendBuffer::OnStreamDataConsumed(size_t bytes_consumed) {
  stream_bytes_written_ += bytes_consumed;
  stream_bytes_outstanding_ += bytes_consumed;
}

bool QuicStreamSendBuffer::WriteStreamData(QuicStreamOffset stream_offset,
                                            QuicByteCount data_length,
                                            QuicDataWriter* writer) {
  QUIC_BUG_IF(quic_bug_12823_1, current_end_offset_ < stream_offset)
    << "Tried to write data out of sequence. last_offset_end:"
    << current_end_offset_ << ", offset:" << stream_offset;
    // The iterator returned from |interval_deque_| will automatically advance
    // the internal write index for the QuicIntervalDeque. The incrementing is
    // done in operator++.
  const auto offset = GetInBlockOffset(stream_offset);
  auto index = GetBlockIndex(stream_offset);
  QUICHE_DCHECK(index <= blocks_.size());
  constexpr QuicByteCount max_data_slice_size = kBlockSizeBytes;

  current_end_offset_ = std::max(current_end_offset_, stream_offset + data_length);
  const auto available_bytes_in_slice = max_data_slice_size - offset;
  if (data_length <= available_bytes_in_slice) {
    return writer->WriteBytes(blocks_[index]->buffer + offset, data_length);
  }

  writer->WriteBytes(blocks_[index]->buffer + offset, available_bytes_in_slice);
  data_length -= available_bytes_in_slice;
  QUICHE_DCHECK(data_length <= max_data_slice_size);
  //if (data_length <= max_data_slice_size)
  {
    return writer->WriteBytes(blocks_[++index]->buffer, data_length);
  }

  QuicByteCount csize = 0;
  for (; csize + max_data_slice_size <= data_length; csize += max_data_slice_size) {
    writer->WriteBytes(blocks_[++index]->buffer, max_data_slice_size);
  }
  if (csize < data_length) {
    writer->WriteBytes(blocks_[++index]->buffer, data_length - csize);
  }

  return false;
}

bool QuicStreamSendBuffer::OnStreamDataAcked(
    QuicStreamOffset offset, QuicByteCount data_length,
    QuicByteCount* newly_acked_length) {
//  *newly_acked_length = 0;
  QUICHE_DCHECK(data_length && *newly_acked_length == 0);
  if (false && data_length == 0) {
    return true;
  }

  const size_t ending_offset = offset + data_length;
  QuicInterval<QuicStreamOffset> off(offset, ending_offset);
  const auto& lmax = bytes_acked_.rbegin()->max();
  if (offset == lmax) {
    // Optimization for the normal case.
    const_cast<size_t&>(lmax) = ending_offset;
    *newly_acked_length = data_length;
    stream_bytes_outstanding_ -= data_length;
    if (!pending_retransmissions_.Empty())
      pending_retransmissions_.Difference(off);
    return FreeMemSlices(offset, ending_offset);
  }
  else if (offset > lmax) {
    // Optimization for the typical case, hole happend.
    if (bytes_acked_.Size() >= kMaxPacketGap) {
      // This frame is going to create more intervals than allowed. Stop processing.
      return false;
    }
    bytes_acked_.AppendBack(off);
    if (!pending_retransmissions_.Empty())
      pending_retransmissions_.Difference(off);
    *newly_acked_length = data_length;
    stream_bytes_outstanding_ -= data_length;
    return true;
  }
  else if (bytes_acked_.IsDisjoint(off)) {
    // Optimization for the typical case, maybe update pending.
    bytes_acked_.AddInter(off);
    *newly_acked_length = data_length;
    stream_bytes_outstanding_ -= data_length;
    if (!pending_retransmissions_.Empty())
      pending_retransmissions_.Difference(off);
    return FreeMemSlices(offset, ending_offset);
  }
  // Exit if dupliacted
  else if (bytes_acked_.Contains(off)) {
    return true;
  }

  // Execute the slow path if newly acked data fill in existing holes.
  QuicIntervalSet<QuicStreamOffset> newly_acked(off);
  newly_acked.Difference(bytes_acked_);
  for (const auto& interval : newly_acked) {
    *newly_acked_length += (interval.max() - interval.min());
  }
  if (stream_bytes_outstanding_ < *newly_acked_length) {
    return false;
  }
  stream_bytes_outstanding_ -= *newly_acked_length;
  bytes_acked_.AddInter(off);
  if (!pending_retransmissions_.Empty())
    pending_retransmissions_.Difference(off);
  QUICHE_DCHECK(!newly_acked.Empty());
  //if (newly_acked.Empty()) {
    //return true;
  //}
  return true;// FreeMemSlices(newly_acked.begin()->min(), newly_acked.rbegin()->max());
}

void QuicStreamSendBuffer::OnStreamDataLost(QuicStreamOffset offset,
                                            QuicByteCount data_length) {
  QUICHE_DCHECK(data_length);
  if (false && data_length == 0) {
    return;
  }

  QuicIntervalSet<QuicStreamOffset> bytes_lost(offset, offset + data_length);
  bytes_lost.Difference(bytes_acked_);
  if (false && pending_retransmissions_.Empty()) {
    pending_retransmissions_ = std::move(bytes_lost);
    return;
  }

  for (const auto& lost : bytes_lost) {
    pending_retransmissions_.AddOptimizedForAppend(lost.min(), lost.max());
  }
}

void QuicStreamSendBuffer::OnStreamDataRetransmitted(
    QuicStreamOffset offset, QuicByteCount data_length) {
  if (data_length == 0 || pending_retransmissions_.Empty()) {
    //printf("\trertans %d == %ld ======================\n", (int)data_length, offset);
    return;
  }
  pending_retransmissions_.Difference(offset, offset + data_length);
}

bool QuicStreamSendBuffer::HasPendingRetransmission() const {
  return !pending_retransmissions_.Empty();
}

StreamPendingRetransmission QuicStreamSendBuffer::NextPendingRetransmission()
    const {
  QUICHE_DCHECK(HasPendingRetransmission());
  //if (HasPendingRetransmission())
  {
    const auto pending = pending_retransmissions_.begin();
    return {pending->min(), pending->max() - pending->min()};
  }
  QUIC_BUG(quic_bug_10853_3)
      << "NextPendingRetransmission is called unexpected with no "
         "pending retransmissions.";
  return {0, 0};
}

bool QuicStreamSendBuffer::FreeMemSlices(QuicStreamOffset start, QuicStreamOffset end) {
  if (end < stream_bytes_start_ + kBlockSizeBytes)
    return true;

  for (int i = 0; i < (int)blocks_.size(); i++) {
    if (bytes_acked_.Contains(stream_bytes_start_, stream_bytes_start_ + kBlockSizeBytes)) {
      stream_bytes_start_ += kBlockSizeBytes;
      if (blocks_.size() > kSmallBlocks) {
        delete blocks_[0];
      } else {
        blocks_.emplace_back(blocks_[0]);//bugs TODO?
      }
      blocks_.erase(blocks_.begin());
    } else
      break;
  }

  return true;
}

bool QuicStreamSendBuffer::IsStreamDataOutstanding(
    QuicStreamOffset offset, QuicByteCount data_length) const {
  QUICHE_DCHECK(data_length);
  return //data_length > 0 &&
         !bytes_acked_.Contains(offset, offset + data_length);
}

}  // namespace quic
