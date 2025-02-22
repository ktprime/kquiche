// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quiche/quic/core/quic_alarm.h"

#include <atomic>

#include "quiche/quic/platform/api/quic_bug_tracker.h"
#include "quiche/quic/platform/api/quic_flag_utils.h"
#include "quiche/quic/platform/api/quic_flags.h"
#include "quiche/quic/platform/api/quic_stack_trace.h"

namespace quic {

QuicAlarm::QuicAlarm(QuicArenaScopedPtr<Delegate> delegate)
    : delegate_(std::move(delegate)), deadline_(QuicTime::Zero()) {}

QuicAlarm::~QuicAlarm() {
  //QUICHE_DCHECK(!IsSet());
  //QUICHE_DCHECK(IsPermanentlyCancelled());
}

void QuicAlarm::Set(QuicTime new_deadline) {
  QUICHE_DCHECK(new_deadline.IsInitialized());
#if 0
  {
    QUIC_BUG(quic_alarm_illegal_set)
        << "Set called after alarm is permanently cancelled. new_deadline:"
        << new_deadline;
    return;
  }
#endif

  deadline_ = new_deadline;
  SetImpl();
}

void QuicAlarm::PermanentCancel() {
  Cancel();
  delegate_.reset();
}

void QuicAlarm::Cancel() {
  if (deadline_.IsInitialized()) {
    CancelImpl();
    deadline_ = QuicTime::Zero();
  }
}

bool QuicAlarm::IsPermanentlyCancelled() const { return delegate_ == nullptr; }

void QuicAlarm::Update(QuicTime new_deadline, QuicTime::Delta granularity) {
  const auto delta = (new_deadline - deadline_).ToMicroseconds();
  if (std::abs(delta) <= granularity.ToMicroseconds()) {
    //deadline_ = new_deadline; //TODO3
  }
  else if (!new_deadline.IsInitialized()) {
    CancelImpl();
    deadline_ = new_deadline;
  }
  else {
    //QUICHE_DCHECK (!IsPermanentlyCancelled());
    deadline_ = new_deadline;
    SetImpl();
  }
}

bool QuicAlarm::IsSet() const { return deadline_.IsInitialized(); }

void QuicAlarm::Fire() {
  QUICHE_DCHECK(IsSet()/* && !IsPermanentlyCancelled()***/);

  deadline_ = QuicTime::Zero();
#if DEBUG
  QuicConnectionContextSwitcher context_switcher(
        delegate_->GetConnectionContext());
#endif
  auto raw_pointer = delegate_.get();
  if (raw_pointer)
      raw_pointer->OnAlarm();
}

void QuicAlarm::UpdateImpl() {
  // CancelImpl and SetImpl take the new deadline by way of the deadline_
  // member, so save and restore deadline_ before canceling.
  const QuicTime new_deadline = deadline_;

  deadline_ = QuicTime::Zero();
  CancelImpl();

  deadline_ = new_deadline;
  SetImpl();
}

}  // namespace quic
