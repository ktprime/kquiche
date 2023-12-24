// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quiche/quic/core/quic_connection_id.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <string>

#include "absl/strings/escaping.h"
#include "openssl/siphash.h"
#include "quiche/quic/core/crypto/quic_random.h"
#include "quiche/quic/core/quic_types.h"
#include "quiche/quic/platform/api/quic_bug_tracker.h"
#include "quiche/quic/platform/api/quic_flag_utils.h"
#include "quiche/quic/platform/api/quic_flags.h"
#include "quiche/quic/platform/api/quic_logging.h"
#include "quiche/common/quiche_endian.h"

namespace quic {

namespace {

// QuicConnectionIdHasher can be used to generate a stable connection ID hash
// function that will return the same value for two equal connection IDs for
// the duration of process lifetime. It is meant to be used as input to data
// structures that do not outlast process lifetime. A new key is generated once
// per process to prevent attackers from crafting connection IDs in such a way
// that they always land in the same hash bucket.
class QuicConnectionIdHasher {
 public:
  inline QuicConnectionIdHasher()
      : QuicConnectionIdHasher(QuicRandom::GetInstance()) {}

  explicit inline QuicConnectionIdHasher(QuicRandom* random) {
    random->RandBytes(&sip_hash_key_, sizeof(sip_hash_key_));
  }

  inline size_t Hash(const char* input, size_t input_len) const {
    return static_cast<size_t>(SIPHASH_24(
        sip_hash_key_, reinterpret_cast<const uint8_t*>(input), input_len));
  }

 private:
  uint64_t sip_hash_key_[2];
};

}  // namespace

static_assert(sizeof(QuicConnectionId) <= 16, "bad size");

QuicConnectionId::QuicConnectionId(const char* data, uint8_t length) {
  length_ = length;
  QUICHE_DCHECK(length <= sizeof(data_short_));
  if (length_ == kQuicDefaultConnectionIdLength) {
    data_short_ = *((uint64_t*)data);
  } else if (length > 0) {
    memcpy((void*)data_short_, data, std::min(length, (uint8_t)sizeof(data_short_)));
  }
}
QuicConnectionId::QuicConnectionId(const QuicConnectionId& other)
    : QuicConnectionId(other.data(), other.length()) {}

QuicConnectionId& QuicConnectionId::operator=(const QuicConnectionId& other) {
  data_short_ = other.data_short_; //x86
  length_ = other.length_;
  return *this;
}

const char* QuicConnectionId::data() const {
  return (char*)&data_short_;
}

char* QuicConnectionId::mutable_data() {
  return (char*)&data_short_;
}

uint8_t QuicConnectionId::length() const { return length_; }

void QuicConnectionId::set_length(uint8_t length) {
  length_ = length;
}

bool QuicConnectionId::IsEmpty() const { return length_ == 0; }

size_t QuicConnectionId::Hash() const {
  static const QuicConnectionIdHasher hasher = QuicConnectionIdHasher();
  return hasher.Hash(data(), length_);
}

std::string QuicConnectionId::ToString() const {
  if (IsEmpty()) {
    return std::string("0");
  }
  return absl::BytesToHexString(absl::string_view(data(), length_));
}

std::ostream& operator<<(std::ostream& os, const QuicConnectionId& v) {
  os << v.ToString();
  return os;
}

bool QuicConnectionId::operator==(const QuicConnectionId& v) const {
  return data_short_ == v.data_short_ && length_ == v.length_;
}

bool QuicConnectionId::operator!=(const QuicConnectionId& v) const {
  return !(v == *this);
}

bool QuicConnectionId::operator<(const QuicConnectionId& v) const {
  if (length_ < v.length_) {
    return true;
  }
  if (length_ > v.length_) {
    return false;
  }
  return memcmp(data(), v.data(), length_) < 0;
}

QuicConnectionId EmptyQuicConnectionId() { return QuicConnectionId(); }

static_assert(kQuicDefaultConnectionIdLength == sizeof(uint64_t),
              "kQuicDefaultConnectionIdLength changed");
static_assert(kQuicDefaultConnectionIdLength == 8,
              "kQuicDefaultConnectionIdLength changed");

}  // namespace quic
