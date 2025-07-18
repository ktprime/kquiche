// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quiche/quic/core/quic_sent_packet_manager.h"

#include <algorithm>
#include <cstddef>
#include <string>

#include "quiche/quic/core/congestion_control/general_loss_algorithm.h"
#include "quiche/quic/core/congestion_control/pacing_sender.h"
#include "quiche/quic/core/congestion_control/send_algorithm_interface.h"
#include "quiche/quic/core/crypto/crypto_protocol.h"
#include "quiche/quic/core/frames/quic_ack_frequency_frame.h"
#include "quiche/quic/core/proto/cached_network_parameters_proto.h"
#include "quiche/quic/core/quic_connection_stats.h"
#include "quiche/quic/core/quic_constants.h"
#include "quiche/quic/core/quic_packet_number.h"
#include "quiche/quic/core/quic_transmission_info.h"
#include "quiche/quic/core/quic_types.h"
#include "quiche/quic/core/quic_utils.h"
#include "quiche/quic/platform/api/quic_bug_tracker.h"
#include "quiche/quic/platform/api/quic_flag_utils.h"
#include "quiche/quic/platform/api/quic_flags.h"
#include "quiche/quic/platform/api/quic_logging.h"
#include "quiche/common/print_elements.h"

namespace quic {

namespace {
constexpr int64_t kDefaultRetransmissionTimeMs = 500;

// Ensure the handshake timer isnt't faster than 10ms.
// This limits the tenth retransmitted packet to 10s after the initial CHLO.
constexpr int64_t kMinHandshakeTimeoutMs = 10;

// Sends up to two tail loss probes before firing an RTO,
// per draft RFC draft-dukkipati-tcpm-tcp-loss-probe.
constexpr size_t kDefaultMaxTailLossProbes = 2;

// The multiplier for calculating PTO timeout before any RTT sample is
// available.
constexpr int kPtoMultiplierWithoutRttSamples = 3;

// Returns true of retransmissions of the specified type should retransmit
// the frames directly (as opposed to resulting in a loss notification).
inline bool ShouldForceRetransmission(TransmissionType transmission_type) {
  return transmission_type == PTO_RETRANSMISSION ||
         transmission_type == HANDSHAKE_RETRANSMISSION;
}

// If pacing rate is accurate, > 2 burst token is not likely to help first ACK
// to arrive earlier, and overly large burst token could cause incast packet
// losses.
constexpr uint32_t kConservativeUnpacedBurst = 2;

// The default number of PTOs to trigger path degrading.
constexpr uint32_t kNumProbeTimeoutsForPathDegradingDelay = 4;

}  // namespace

#define ENDPOINT                                                         \
  (unacked_packets_.perspective() == Perspective::IS_SERVER ? "Server: " \
                                                            : "Client: ")

QuicSentPacketManager::QuicSentPacketManager(
    Perspective perspective, const QuicClock* clock, QuicRandom* random,
    QuicConnectionStats* stats, CongestionControlType congestion_control_type)
    : unacked_packets_(perspective),
      clock_(clock),
      random_(random),
      stats_(stats),
      //debug_delegate_(nullptr),
      network_change_visitor_(nullptr),
      initial_congestion_window_(kInitialCongestionWindow),
      loss_algorithm_(&uber_loss_algorithm_),
      consecutive_crypto_retransmission_count_(0),
      pending_timer_transmission_count_(0),
      using_pacing_(false),
      conservative_handshake_retransmits_(false),
      handshake_finished_(false),
      largest_mtu_acked_(0),
      peer_max_ack_delay_(
          QuicTime::Delta::FromMilliseconds(kDefaultDelayedAckTimeMs)),
      rtt_updated_(false),
      acked_packets_iter_(last_ack_frame_.packets.rbegin()),
      consecutive_pto_count_(0),
#if QUIC_TLS_SESSION
      handshake_mode_disabled_(false),
#endif
      rtt_packet_state_(0),
      ignore_pings_(false),
      ignore_ack_delay_(false),
      num_ptos_for_path_degrading_(kNumProbeTimeoutsForPathDegradingDelay) {
  SetSendAlgorithm(congestion_control_type);
}

QuicSentPacketManager::~QuicSentPacketManager() {
  delete send_algorithm_;
}

void QuicSentPacketManager::SetFromConfig(const QuicConfig& config) {
  const Perspective perspective = unacked_packets_.perspective();
  if (config.HasReceivedInitialRoundTripTimeUs() &&
      config.ReceivedInitialRoundTripTimeUs() > 0) {
    if (!config.HasClientSentConnectionOption(kNRTT, perspective)) {
      SetInitialRtt(QuicTime::Delta::FromMicroseconds(
                        config.ReceivedInitialRoundTripTimeUs()),
                    /*trusted=*/false);
    }
  } else if (config.HasInitialRoundTripTimeUsToSend() &&
             config.GetInitialRoundTripTimeUsToSend() > 0) {
    SetInitialRtt(QuicTime::Delta::FromMicroseconds(
                      config.GetInitialRoundTripTimeUsToSend()),
                  /*trusted=*/false);
  }
  if (config.HasReceivedMaxAckDelayMs()) {
    peer_max_ack_delay_ =
        QuicTime::Delta::FromMilliseconds(config.ReceivedMaxAckDelayMs());
  }
  if (GetQuicReloadableFlag(quic_can_send_ack_frequency) &&
      perspective == Perspective::IS_SERVER) {
    if (config.HasReceivedMinAckDelayMs()) {
      peer_min_ack_delay_ =
          QuicTime::Delta::FromMilliseconds(config.ReceivedMinAckDelayMs());
    }
    if (config.HasClientSentConnectionOption(kAFF1, perspective)) {
      use_smoothed_rtt_in_ack_delay_ = true;
    }
  }
  if (config.HasClientSentConnectionOption(kMAD0, perspective)) {
    ignore_ack_delay_ = true;
  }

  // Configure congestion control.
  if (config.HasClientRequestedIndependentOption(kTBBR, perspective)) {
    SetSendAlgorithm(kBBR);
  }
  if (GetQuicReloadableFlag(quic_allow_client_enabled_bbr_v2) &&
      config.HasClientRequestedIndependentOption(kB2ON, perspective)) {
    QUIC_RELOADABLE_FLAG_COUNT(quic_allow_client_enabled_bbr_v2);
    SetSendAlgorithm(kBBRv2);
  }

  if (config.HasClientRequestedIndependentOption(kRENO, perspective)) {
    SetSendAlgorithm(kRenoBytes);
  } else if (config.HasClientRequestedIndependentOption(kBYTE, perspective) ||
             (GetQuicReloadableFlag(quic_default_to_bbr) &&
              config.HasClientRequestedIndependentOption(kQBIC, perspective))) {
    SetSendAlgorithm(kCubicBytes);
  }

  // Initial window.
  if (config.HasClientRequestedIndependentOption(kIW03, perspective)) {
    initial_congestion_window_ = 3;
    send_algorithm_->SetInitialCongestionWindowInPackets(3);
  }
  if (config.HasClientRequestedIndependentOption(kIW10, perspective)) {
    initial_congestion_window_ = 10;
    send_algorithm_->SetInitialCongestionWindowInPackets(10);
  }
  if (config.HasClientRequestedIndependentOption(kIW20, perspective)) {
    initial_congestion_window_ = 20;
    send_algorithm_->SetInitialCongestionWindowInPackets(20);
  }
  if (config.HasClientRequestedIndependentOption(kIW50, perspective)) {
    initial_congestion_window_ = 50;
    send_algorithm_->SetInitialCongestionWindowInPackets(50);
  }
  if (config.HasClientRequestedIndependentOption(kBWS5, perspective)) {
    initial_congestion_window_ = 10;
    send_algorithm_->SetInitialCongestionWindowInPackets(10);
  }

  if (config.HasClientRequestedIndependentOption(kIGNP, perspective)) {
    ignore_pings_ = true;
  }

  using_pacing_ = !GetQuicFlag(quic_disable_pacing_for_perf_tests);
  // Configure loss detection.
  if (config.HasClientRequestedIndependentOption(kILD0, perspective)) {
    uber_loss_algorithm_.SetReorderingShift(kDefaultIetfLossDelayShift);
    uber_loss_algorithm_.DisableAdaptiveReorderingThreshold();
  }
  if (config.HasClientRequestedIndependentOption(kILD1, perspective)) {
    uber_loss_algorithm_.SetReorderingShift(kDefaultLossDelayShift);
    uber_loss_algorithm_.DisableAdaptiveReorderingThreshold();
  }
  if (config.HasClientRequestedIndependentOption(kILD2, perspective)) {
    uber_loss_algorithm_.EnableAdaptiveReorderingThreshold();
    uber_loss_algorithm_.SetReorderingShift(kDefaultIetfLossDelayShift);
  }
  if (config.HasClientRequestedIndependentOption(kILD3, perspective)) {
    uber_loss_algorithm_.SetReorderingShift(kDefaultLossDelayShift);
    uber_loss_algorithm_.EnableAdaptiveReorderingThreshold();
  }
  if (config.HasClientRequestedIndependentOption(kILD4, perspective)) {
    uber_loss_algorithm_.SetReorderingShift(kDefaultLossDelayShift);
    uber_loss_algorithm_.EnableAdaptiveReorderingThreshold();
    uber_loss_algorithm_.EnableAdaptiveTimeThreshold();
  }
  if (config.HasClientRequestedIndependentOption(kRUNT, perspective)) {
    uber_loss_algorithm_.DisablePacketThresholdForRuntPackets();
  }
  if (config.HasClientSentConnectionOption(kCONH, perspective)) {
    conservative_handshake_retransmits_ = true;
  }
  send_algorithm_->SetFromConfig(config, perspective);
  loss_algorithm_->SetFromConfig(config, perspective);

  if (network_change_visitor_ != nullptr) {
    network_change_visitor_->OnCongestionChange();
  }

  if (debug_delegate_ != nullptr) {
    DebugDelegate::SendParameters parameters{};
    parameters.congestion_control_type =
        send_algorithm_->GetCongestionControlType();
    parameters.use_pacing = using_pacing_;
    parameters.initial_congestion_window = initial_congestion_window_;
    debug_delegate_->OnConfigProcessed(parameters);
  }
}

void QuicSentPacketManager::ApplyConnectionOptions(
    const QuicTagVector& connection_options) {
  absl::optional<CongestionControlType> cc_type;
  if (ContainsQuicTag(connection_options, kB2ON)) {
    cc_type = kBBRv2;
  } else if (ContainsQuicTag(connection_options, kTBBR)) {
    cc_type = kBBR;
  } else if (ContainsQuicTag(connection_options, kRENO)) {
    cc_type = kRenoBytes;
  } else if (ContainsQuicTag(connection_options, kQBIC)) {
    cc_type = kCubicBytes;
  }

  if (cc_type.has_value()) {
    SetSendAlgorithm(*cc_type);
  }

  send_algorithm_->ApplyConnectionOptions(connection_options);
}

void QuicSentPacketManager::ResumeConnectionState(
    const CachedNetworkParameters& cached_network_params,
    bool max_bandwidth_resumption) {
#if QUIC_SERVER_SESSION
  QuicBandwidth bandwidth = QuicBandwidth::FromBytesPerSecond(
      max_bandwidth_resumption
          ? cached_network_params.max_bandwidth_estimate_bytes_per_second()
          : cached_network_params.bandwidth_estimate_bytes_per_second());
  QuicTime::Delta rtt =
      QuicTime::Delta::FromMilliseconds(cached_network_params.min_rtt_ms());
  // This calls the old AdjustNetworkParameters interface, and fills certain
  // fields in SendAlgorithmInterface::NetworkParams
  // (e.g., quic_bbr_fix_pacing_rate) using GFE flags.
  SendAlgorithmInterface::NetworkParams params(
      bandwidth, rtt, /*allow_cwnd_to_decrease = */ false);
  // The rtt is trusted because it's a min_rtt measured from a previous
  // connection with the same network path between client and server.
  params.is_rtt_trusted = true;
  AdjustNetworkParameters(params);
#endif
}

void QuicSentPacketManager::AdjustNetworkParameters(
    const SendAlgorithmInterface::NetworkParams& params) {
  const QuicBandwidth& bandwidth = params.bandwidth;
  const QuicTime::Delta& rtt = params.rtt;

  if (!rtt.IsZero()) {
    if (params.is_rtt_trusted) {
      // Always set initial rtt if it's trusted.
      SetInitialRtt(rtt, /*trusted=*/true);
    } else if (rtt_stats_.initial_rtt() ==
               QuicTime::Delta::FromMilliseconds(kInitialRttMs)) {
      // Only set initial rtt if we are using the default. This avoids
      // overwriting a trusted initial rtt by an untrusted one.
      SetInitialRtt(rtt, /*trusted=*/false);
    }
  }

  const QuicByteCount old_cwnd = send_algorithm_->GetCongestionWindow();
  if (GetQuicReloadableFlag(quic_conservative_bursts) && using_pacing_ &&
      !bandwidth.IsZero()) {
    QUIC_RELOADABLE_FLAG_COUNT(quic_conservative_bursts);
    pacing_sender_.SetBurstTokens(kConservativeUnpacedBurst);
  }
  send_algorithm_->AdjustNetworkParameters(params);
  if (debug_delegate_ != nullptr) {
    debug_delegate_->OnAdjustNetworkParameters(
        bandwidth, rtt.IsZero() ? rtt_stats_.MinOrInitialRtt() : rtt, old_cwnd,
        send_algorithm_->GetCongestionWindow());
  }
}

void QuicSentPacketManager::SetLossDetectionTuner(
    std::unique_ptr<LossDetectionTunerInterface> tuner) {
  uber_loss_algorithm_.SetLossDetectionTuner(std::move(tuner));
}

void QuicSentPacketManager::OnConfigNegotiated() {
  loss_algorithm_->OnConfigNegotiated();
}

void QuicSentPacketManager::OnConnectionClosed() {
  loss_algorithm_->OnConnectionClosed();
}

void QuicSentPacketManager::SetHandshakeConfirmed() {
  if (!handshake_finished_) {
    handshake_finished_ = true;
    NeuterHandshakePackets();
  }
}

void QuicSentPacketManager::PostProcessNewlyAckedPackets(
    QuicPacketNumber ack_packet_number, EncryptionLevel ack_decrypted_level,
    QuicTime ack_receive_time, bool rtt_updated,
    QuicByteCount prior_bytes_in_flight) {
  unacked_packets_.NotifyAggregatedStreamFrameAcked(
      last_ack_frame_.ack_delay_time);
  InvokeLossDetection(ack_receive_time);
  MaybeInvokeCongestionEvent(rtt_updated, prior_bytes_in_flight,
                             ack_receive_time);
  unacked_packets_.RemoveObsoletePackets();
#if DCHECK_FLAG
  sustained_bandwidth_recorder_.RecordEstimate(
      send_algorithm_->InRecovery(), send_algorithm_->InSlowStart(),
      send_algorithm_->BandwidthEstimate(), ack_receive_time, clock_->ApproximateNow(),
      rtt_stats_.smoothed_rtt());
#endif
  // Anytime we are making forward progress and have a new RTT estimate, reset
  // the backoff counters.
  if (consecutive_pto_count_) {
    // Records the max consecutive PTO before forward progress has been made.
    if (consecutive_pto_count_ >
        stats_->max_consecutive_rto_with_forward_progress) {
      stats_->max_consecutive_rto_with_forward_progress =
          consecutive_pto_count_;
    }
    // Reset all retransmit counters any time a new packet is acked.
    if (rtt_updated)
      consecutive_pto_count_ = 0; //TODO3. on fixed output bandwidth, it's easy closed by 5rto blackhole detected.
    else
      consecutive_pto_count_ -= 1;
    consecutive_crypto_retransmission_count_ = 0;
  }

  if (debug_delegate_ != nullptr) {
    debug_delegate_->OnIncomingAck(
        ack_packet_number, ack_decrypted_level, last_ack_frame_, ack_receive_time,
        LargestAcked(last_ack_frame_), rtt_updated, GetLeastUnacked());
  }
  // Remove packets below least unacked from all_packets_acked_ and
  // last_ack_frame_.
  last_ack_frame_.packets.RemoveUpTo(unacked_packets_.GetLeastUnacked());
  QUICHE_CHECK(last_ack_frame_.received_packet_times.empty());
#if QUIC_TLS_SESSION
  if (!last_ack_frame_.received_packet_times.empty())
  last_ack_frame_.received_packet_times.clear();
#endif
}

void QuicSentPacketManager::MaybeInvokeCongestionEvent(
    bool rtt_updated, QuicByteCount prior_in_flight, QuicTime event_time) {
  if (!rtt_updated && packets_acked_.empty() && packets_lost_.empty()) {
    return;
  }
  const bool overshooting_detected =
      stats_->overshooting_detected_with_network_parameters_adjusted;
  if (using_pacing_) {
    pacing_sender_.OnCongestionEvent(rtt_updated, prior_in_flight, event_time,
                                     packets_acked_, packets_lost_);
  } else {
    send_algorithm_->OnCongestionEvent(rtt_updated, prior_in_flight, event_time,
                                       packets_acked_, packets_lost_);
  }
  if (false && !overshooting_detected &&
      stats_->overshooting_detected_with_network_parameters_adjusted) {
    debug_delegate_->OnOvershootingDetected();
  }
  packets_acked_.clear();
  packets_lost_.clear();
  if (false && network_change_visitor_ != nullptr) {
    network_change_visitor_->OnCongestionChange();
  }
}

void QuicSentPacketManager::MarkInitialPacketsForRetransmission() {
  if (unacked_packets_.empty()) {
    return;
  }
  QuicPacketNumber packet_number = unacked_packets_.GetLeastUnacked();
  QuicPacketNumber largest_sent_packet = unacked_packets_.largest_sent_packet();
  for (; packet_number <= largest_sent_packet; ++packet_number) {
    QuicTransmissionInfo* transmission_info =
        unacked_packets_.GetMutableTransmissionInfo(packet_number);
    if (transmission_info->encryption_level == ENCRYPTION_INITIAL) {
      if (transmission_info->in_flight) {
        unacked_packets_.RemoveFromInFlight(transmission_info);
      }
      if (unacked_packets_.HasRetransmittableFrames(*transmission_info)) {
        MarkForRetransmission(packet_number, ALL_INITIAL_RETRANSMISSION);
      }
    }
  }
}

void QuicSentPacketManager::MarkZeroRttPacketsForRetransmission() {
  if (unacked_packets_.empty()) {
    return;
  }
  QuicPacketNumber packet_number = unacked_packets_.GetLeastUnacked();
  QuicPacketNumber largest_sent_packet = unacked_packets_.largest_sent_packet();
  for (; packet_number <= largest_sent_packet; ++packet_number) {
    QuicTransmissionInfo* transmission_info =
        unacked_packets_.GetMutableTransmissionInfo(packet_number);
    if (transmission_info->encryption_level == ENCRYPTION_ZERO_RTT) {
      if (transmission_info->in_flight) {
        // Remove 0-RTT packets and packets of the wrong version from flight,
        // because neither can be processed by the peer.
        unacked_packets_.RemoveFromInFlight(transmission_info);
      }
      if (unacked_packets_.HasRetransmittableFrames(*transmission_info)) {
        MarkForRetransmission(packet_number, ALL_ZERO_RTT_RETRANSMISSION);
      }
    }
  }
}

void QuicSentPacketManager::NeuterUnencryptedPackets() {
  for (QuicPacketNumber packet_number :
       unacked_packets_.NeuterUnencryptedPackets()) {
    send_algorithm_->OnPacketNeutered(packet_number);
  }
  if (handshake_mode_disabled_) {
    consecutive_pto_count_ = 0;
    uber_loss_algorithm_.ResetLossDetection(INITIAL_DATA);
  }
}

void QuicSentPacketManager::NeuterHandshakePackets() {
  for (QuicPacketNumber packet_number :
       unacked_packets_.NeuterHandshakePackets()) {
    send_algorithm_->OnPacketNeutered(packet_number);
  }
  if (handshake_mode_disabled_) {
    consecutive_pto_count_ = 0;
    uber_loss_algorithm_.ResetLossDetection(HANDSHAKE_DATA);
  }
}

bool QuicSentPacketManager::ShouldAddMaxAckDelay(
    PacketNumberSpace space) const {
  // Do not include max_ack_delay when PTO is armed for Initial or Handshake
  // packet number spaces.
  return !supports_multiple_packet_number_spaces() || space == APPLICATION_DATA;
}

QuicTime QuicSentPacketManager::GetEarliestPacketSentTimeForPto(
    PacketNumberSpace* packet_number_space) const {
  QUICHE_DCHECK(supports_multiple_packet_number_spaces());
  QuicTime earliest_sent_time = QuicTime::Zero();
  for (int8_t i = 0; i < NUM_PACKET_NUMBER_SPACES; ++i) {
    const QuicTime sent_time = unacked_packets_.GetLastInFlightPacketSentTime(
        static_cast<PacketNumberSpace>(i));
    if (!handshake_finished_ && i == APPLICATION_DATA) {
      // Do not arm PTO for application data until handshake gets confirmed.
      continue;
    }
    if (!sent_time.IsInitialized() || (earliest_sent_time.IsInitialized() &&
                                       earliest_sent_time <= sent_time)) {
      continue;
    }
    earliest_sent_time = sent_time;
    *packet_number_space = static_cast<PacketNumberSpace>(i);
  }

  return earliest_sent_time;
}

void QuicSentPacketManager::MarkForRetransmission(
    QuicPacketNumber packet_number, TransmissionType transmission_type) {
  QuicTransmissionInfo* transmission_info =
      unacked_packets_.GetMutableTransmissionInfo(packet_number);
  // Packets without retransmittable frames can only be marked for loss
  // retransmission.
  QUIC_BUG_IF(quic_bug_12552_2, transmission_type != LOSS_RETRANSMISSION &&
                                    !unacked_packets_.HasRetransmittableFrames(
                                        *transmission_info))
      << "packet number " << packet_number
      << " transmission_type: " << transmission_type << " transmission_info "
      << transmission_info->DebugString();
  if (ShouldForceRetransmission(transmission_type)) {
    if (!unacked_packets_.RetransmitFrames(
            QuicFrames(transmission_info->retransmittable_frames),
            transmission_type)) {
      // Do not set packet state if the data is not fully retransmitted.
      // This should only happen if packet payload size decreases which can be
      // caused by:
      // 1) connection tries to opportunistically retransmit data
      // when sending a packet of a different packet number space, or
      // 2) path MTU decreases, or
      // 3) packet header size increases (e.g., packet number length
      // increases).
      QUIC_CODE_COUNT(quic_retransmit_frames_failed);
      return;
    }
    QUIC_CODE_COUNT(quic_retransmit_frames_succeeded);
    // Get the latest transmission_info here as it can be invalidated after
    // HandleRetransmission adding new sent packets into unacked_packets_.
    transmission_info =
      unacked_packets_.GetMutableTransmissionInfo(packet_number);
  } else {
    auto has_lost = unacked_packets_.NotifyFramesLost(*transmission_info, transmission_type);
    if (!has_lost) {
      --stats_->packets_lost;
      //printf("pn_id = %5ld %s", (long)packet_number.ToInt64(), transmission_info->DebugString().data());
      //transmission_info->state = QuicUtils::RetransmissionTypeToPacketState(transmission_type);
      transmission_info->state = ACKED;
      return;
    }

    if (!transmission_info->retransmittable_frames.empty()) {
      if (transmission_type == LOSS_RETRANSMISSION) { //TODO3 need check?
        // Record the first packet sent after loss, which allows to wait 1
        // more RTT before giving up on this lost packet.
        transmission_info->first_sent_after_loss =
            unacked_packets_.largest_sent_packet() + 1;
      } else {
        // Clear the recorded first packet sent after loss when version or
        // encryption changes.
        transmission_info->first_sent_after_loss.Clear(); //TODO3
      }
    }
  }

  // Update packet state according to transmission type.
  transmission_info->state =
      QuicUtils::RetransmissionTypeToPacketState(transmission_type);
}

void QuicSentPacketManager::RecordOneSpuriousRetransmission(
    const QuicTransmissionInfo& info) {
  stats_->bytes_spuriously_retransmitted += info.bytes_sent;
  ++stats_->packets_spuriously_retransmitted;
  if (debug_delegate_ != nullptr) {
    debug_delegate_->OnSpuriousPacketRetransmission(info.transmission_type,
                                                    info.bytes_sent);
  }
}

void QuicSentPacketManager::MarkPacketHandled(QuicPacketNumber packet_number,
                                              QuicTransmissionInfo* info,
                                              QuicTime ack_receive_time,
                                              QuicTime::Delta ack_delay_time,
                                              QuicTime receive_timestamp) {
#if QUIC_TLS_SESSION
    for (const auto& frame : info->retransmittable_frames) {
      if (frame.type == ACK_FREQUENCY_FRAME) {
        OnAckFrequencyFrameAcked(*frame.ack_frequency_frame);
      }
    }
#endif
  // Try to aggregate acked stream frames if acked packet is not a
  // retransmission.
  if (info->transmission_type == NOT_RETRANSMISSION) {
    unacked_packets_.MaybeAggregateAckedStreamFrame(*info, ack_delay_time,
                                                    receive_timestamp);
  } else {
    unacked_packets_.NotifyAggregatedStreamFrameAcked(ack_delay_time);
    const bool new_data_acked = unacked_packets_.NotifyFramesAcked(
        *info, ack_delay_time, receive_timestamp);
    if (!new_data_acked) {
      // Record as a spurious retransmission if this packet is a
      // retransmission and no new data gets acked.
      QUIC_DVLOG(1) << "Detect spurious retransmitted packet " << packet_number
                    << " transmission type: " << info->transmission_type;
      RecordOneSpuriousRetransmission(*info);
    }
  }
  if (info->state == LOST) {
    // Record as a spurious loss as a packet previously declared lost gets
    // acked.
    const PacketNumberSpace packet_number_space =
        unacked_packets_.GetPacketNumberSpace(info->encryption_level);
    const QuicPacketNumber previous_largest_acked =
        supports_multiple_packet_number_spaces()
            ? unacked_packets_.GetLargestAckedOfPacketNumberSpace(
                  packet_number_space)
            : unacked_packets_.largest_acked();
    QUIC_DVLOG(1) << "Packet " << packet_number
                  << " was detected lost spuriously, "
                     "previous_largest_acked: "
                  << previous_largest_acked;
    loss_algorithm_->SpuriousLossDetected(unacked_packets_, rtt_stats_,
                                          ack_receive_time, packet_number,
                                          previous_largest_acked);
    ++stats_->packet_spuriously_detected_lost;
    //--stats_->packets_lost;
  }

  if (false && //network_change_visitor_ != nullptr &&
      info->bytes_sent > largest_mtu_acked_) {
    largest_mtu_acked_ = info->bytes_sent;
    network_change_visitor_->OnPathMtuIncreased(largest_mtu_acked_);
  }
  if (info->in_flight)
  unacked_packets_.RemoveFromInFlight(info);
  unacked_packets_.RemoveRetransmittability(info);
  info->state = ACKED;
}

QuicAckFrequencyFrame QuicSentPacketManager::GetUpdatedAckFrequencyFrame()
    const {
  QuicAckFrequencyFrame frame;
  if (!CanSendAckFrequency()) {
    QUIC_BUG(quic_bug_10750_1)
        << "New AckFrequencyFrame is created while it shouldn't.";
    return frame;
  }

  QUIC_RELOADABLE_FLAG_COUNT_N(quic_can_send_ack_frequency, 1, 3);
  frame.packet_tolerance = kMaxRetransmittablePacketsBeforeAck;
  auto rtt = use_smoothed_rtt_in_ack_delay_ ? rtt_stats_.SmoothedOrInitialRtt()
                                            : rtt_stats_.MinOrInitialRtt();
  frame.max_ack_delay = rtt * kAckDecimationDelay;
  frame.max_ack_delay = std::max(frame.max_ack_delay, peer_min_ack_delay_);
  // TODO(haoyuewang) Remove this once kDefaultMinAckDelayTimeMs is updated to
  // 5 ms on the client side.
  frame.max_ack_delay =
      std::max(frame.max_ack_delay,
               QuicTime::Delta::FromMilliseconds(kDefaultMinAckDelayTimeMs));
  return frame;
}

bool QuicSentPacketManager::OnPacketSent(
    SerializedPacket* mutable_packet, QuicTime sent_time,
    TransmissionType transmission_type,
    HasRetransmittableData has_retransmittable_data, bool measure_rtt) {
  const SerializedPacket& packet = *mutable_packet;
  QuicPacketNumber packet_number = packet.packet_number;
  QUICHE_DCHECK_LE(FirstSendingPacketNumber(), packet_number);
  //QUICHE_DCHECK(!unacked_packets_.IsUnacked(packet_number));
  QUIC_BUG_IF(quic_bug_10750_2, packet.encrypted_length == 0)
      << "Cannot send empty packets.";
  if (pending_timer_transmission_count_ > 0) {
    --pending_timer_transmission_count_;
  }

  bool in_flight = has_retransmittable_data == HAS_RETRANSMITTABLE_DATA;
  if (false && ignore_pings_ && mutable_packet->retransmittable_frames.size() == 1 &&
      mutable_packet->retransmittable_frames[0].type == PING_FRAME) {
    // Dot not use PING only packet for RTT measure or congestion control.
    in_flight = false;
    measure_rtt = false;
  }
  if (using_pacing_) {
    //if (packet.transmission_type == NOT_RETRANSMISSION) //hybchanged.TODO2 do not add LOSS_RETRANSMISSION ?
    pacing_sender_.OnPacketSent(sent_time, unacked_packets_.bytes_in_flight(),
                                packet_number, packet.encrypted_length,
                                has_retransmittable_data);
  } else {
    send_algorithm_->OnPacketSent(sent_time, unacked_packets_.bytes_in_flight(),
                                  packet_number, packet.encrypted_length,
                                  has_retransmittable_data);
  }

  // Deallocate message data in QuicMessageFrame immediately after packet
  // sent.
  if (DCHECK_FLAG && packet.frame_types & (1 << MESSAGE_FRAME)) { //TODO3 reopen it if msg is OK
    for (const auto& frame : mutable_packet->retransmittable_frames) {
      if (frame.type == MESSAGE_FRAME) {
        frame.message_frame->message_data.clear();
        frame.message_frame->message_length = 0;
      }
    }
  }
#if QUIC_TLS_SESSION
  if (packet.frame_types & (1 << ACK_FREQUENCY_FRAME)) {
    for (const auto& frame : packet.retransmittable_frames) {
      if (frame.type == ACK_FREQUENCY_FRAME) {
        OnAckFrequencyFrameSent(*frame.ack_frequency_frame);
      }
    }
  }
#endif
  unacked_packets_.AddSentPacket(mutable_packet, transmission_type, sent_time,
                                 in_flight, measure_rtt);
  // Reset the retransmission timer anytime a pending packet is sent.
  return in_flight;
}

QuicSentPacketManager::RetransmissionTimeoutMode
QuicSentPacketManager::OnRetransmissionTimeout() {
  QUICHE_DCHECK(unacked_packets_.HasInFlightPackets() ||
                (handshake_mode_disabled_ && !handshake_finished_));
  //QUICHE_DCHECK_EQ(0u, pending_timer_transmission_count_);
  // Handshake retransmission, timer based loss detection, TLP, and RTO are
  // implemented with a single alarm. The handshake alarm is set when the
  // handshake has not completed, the loss alarm is set when the loss detection
  // algorithm says to, and the TLP and  RTO alarms are set after that.
  // The TLP alarm is always set to run for under an RTO.
  QUICHE_DCHECK(unacked_packets_.HasInFlightPackets() ||
    (handshake_mode_disabled_ && !handshake_finished_));
  if (loss_algorithm_->GetLossTimeout() != QuicTime::Zero()) {
    ++stats_->loss_timeout_count;
    QuicByteCount prior_in_flight = unacked_packets_.bytes_in_flight();
    const QuicTime now = clock_->Now();
    InvokeLossDetection(now);
    MaybeInvokeCongestionEvent(false, prior_in_flight, now);
    return LOSS_MODE;
  } else if (!handshake_finished_ && !handshake_mode_disabled_ && unacked_packets_.HasPendingCryptoPackets()) {
    QUICHE_DCHECK(!handshake_mode_disabled_);
    ++stats_->crypto_retransmit_count;
    RetransmitCryptoPackets();
    return HANDSHAKE_MODE;
  } else {
    ++stats_->pto_count;
    if (handshake_mode_disabled_ && !handshake_finished_) {
      ++stats_->crypto_retransmit_count;
    }
    ++consecutive_pto_count_;
    pending_timer_transmission_count_ = 1;
    return PTO_MODE;
  }
}

void QuicSentPacketManager::RetransmitCryptoPackets() {
  QUICHE_DCHECK_EQ(HANDSHAKE_MODE, GetRetransmissionMode());
  ++consecutive_crypto_retransmission_count_;
  bool packet_retransmitted = false;
  //std::vector<QuicPacketNumber> crypto_retransmissions;
  absl::InlinedVector<QuicPacketNumber, 2> crypto_retransmissions;
  if (!unacked_packets_.empty()) {
    QuicPacketNumber packet_number = unacked_packets_.GetLeastUnacked();
    QuicPacketNumber largest_sent_packet =
        unacked_packets_.largest_sent_packet();
    for (; packet_number <= largest_sent_packet; ++packet_number) {
      QuicTransmissionInfo* transmission_info =
          unacked_packets_.GetMutableTransmissionInfo(packet_number);
      // Only retransmit frames which are in flight, and therefore have been
      // sent.
      if (!transmission_info->in_flight ||
          transmission_info->state != OUTSTANDING ||
          !transmission_info->has_crypto_handshake ||
          !unacked_packets_.HasRetransmittableFrames(*transmission_info)) {
        continue;
      }
      packet_retransmitted = true;
      crypto_retransmissions.push_back(packet_number);
      ++pending_timer_transmission_count_;
    }
  }
  QUICHE_CHECK(packet_retransmitted)
    ;//<< "No crypto packets found to retransmit.";
  for (QuicPacketNumber retransmission : crypto_retransmissions) {
    MarkForRetransmission(retransmission, HANDSHAKE_RETRANSMISSION);
  }
}

#if 0
bool QuicSentPacketManager::MaybeRetransmitOldestPacket(TransmissionType type) {
  if (!unacked_packets_.empty()) {
    QuicPacketNumber packet_number = unacked_packets_.GetLeastUnacked();
    QuicPacketNumber largest_sent_packet =
        unacked_packets_.largest_sent_packet();
    for (; packet_number <= largest_sent_packet; ++packet_number) {
      QuicTransmissionInfo* transmission_info =
          unacked_packets_.GetMutableTransmissionInfo(packet_number);
      // Only retransmit frames which are in flight, and therefore have been
      // sent.
      if (!transmission_info->in_flight ||
          transmission_info->state != OUTSTANDING ||
          !unacked_packets_.HasRetransmittableFrames(*transmission_info)) {
        continue;
      }
      MarkForRetransmission(packet_number, type);
      return true;
    }
  }
  QUIC_DVLOG(1)
      << "No retransmittable packets, so RetransmitOldestPacket failed.";
  return false;
}
#endif

void QuicSentPacketManager::MaybeSendProbePacket() {
  if (pending_timer_transmission_count_ == 0 || unacked_packets_.empty()) {
    return;
  }
  PacketNumberSpace packet_number_space = INITIAL_DATA;
  if (supports_multiple_packet_number_spaces()) {
    // Find out the packet number space to send probe packets.
    if (!GetEarliestPacketSentTimeForPto(&packet_number_space)
             .IsInitialized()) {
      QUIC_BUG_IF(quic_earliest_sent_time_not_initialized,
                  unacked_packets_.perspective() == Perspective::IS_SERVER)
          << "earliest_sent_time not initialized when trying to send PTO "
             "retransmissions";
      return;
    }
  }
  absl::InlinedVector<QuicPacketNumber, 2> probing_packets;
  QuicPacketNumber packet_number = unacked_packets_.GetLeastUnacked();
  QuicPacketNumber largest_sent_packet =
      unacked_packets_.largest_sent_packet();
  for (; packet_number <= largest_sent_packet; ++packet_number) {
    QuicTransmissionInfo* transmission_info =
        unacked_packets_.GetMutableTransmissionInfo(packet_number);
    if (transmission_info->state == OUTSTANDING &&
        unacked_packets_.HasRetransmittableFrames(*transmission_info) &&
        (!supports_multiple_packet_number_spaces() ||
          unacked_packets_.GetPacketNumberSpace(
              transmission_info->encryption_level) == packet_number_space)) {
      QUICHE_DCHECK(transmission_info->in_flight);
      probing_packets.push_back(packet_number);
      if (probing_packets.size() == pending_timer_transmission_count_) {
        break;
      }
    }
  }

  for (QuicPacketNumber retransmission : probing_packets) {
    QUIC_DVLOG(1) << ENDPOINT << "Marking " << retransmission
                  << " for probing retransmission";
    MarkForRetransmission(retransmission, PTO_RETRANSMISSION);
  }
  // It is possible that there is not enough outstanding data for probing.
}

void QuicSentPacketManager::EnableIetfPtoAndLossDetection() {
  // Disable handshake mode
#if QUIC_TLS_SESSION
  handshake_mode_disabled_ = true;
#endif
}

void QuicSentPacketManager::RetransmitDataOfSpaceIfAny(
    PacketNumberSpace space) {
  QUICHE_DCHECK(supports_multiple_packet_number_spaces());
  if (!unacked_packets_.GetLastInFlightPacketSentTime(space).IsInitialized()) {
    // No in flight data of space.
    return;
  }
  if (unacked_packets_.empty()) {
    return;
  }
  QuicPacketNumber packet_number = unacked_packets_.GetLeastUnacked();
  QuicPacketNumber largest_sent_packet = unacked_packets_.largest_sent_packet();
  for (; packet_number <= largest_sent_packet; ++packet_number) {
    QuicTransmissionInfo* transmission_info =
        unacked_packets_.GetMutableTransmissionInfo(packet_number);
    if (transmission_info->state == OUTSTANDING &&
        unacked_packets_.HasRetransmittableFrames(*transmission_info) &&
        unacked_packets_.GetPacketNumberSpace(
            transmission_info->encryption_level) == space) {
      QUICHE_DCHECK(transmission_info->in_flight);
      if (pending_timer_transmission_count_ == 0) {
        pending_timer_transmission_count_ = 1;
      }
      MarkForRetransmission(packet_number, PTO_RETRANSMISSION);
      return;
    }
  }
}

QuicSentPacketManager::RetransmissionTimeoutMode
QuicSentPacketManager::GetRetransmissionMode() const {
  QUICHE_DCHECK(unacked_packets_.HasInFlightPackets() ||
                (handshake_mode_disabled_ && !handshake_finished_));
  if (loss_algorithm_->GetLossTimeout() != QuicTime::Zero()) {
    return LOSS_MODE;
  }
  if (!handshake_finished_ && !handshake_mode_disabled_ &&
      unacked_packets_.HasPendingCryptoPackets()) {
    return HANDSHAKE_MODE;
  }
  return PTO_MODE;
}

void QuicSentPacketManager::InvokeLossDetection(QuicTime time) {
  if (!packets_acked_.empty()) {
    QUICHE_DCHECK_LE(packets_acked_.front().packet_number,
                     packets_acked_.back().packet_number);
    largest_newly_acked_ = packets_acked_.back().packet_number;
  }
  LossDetectionInterface::DetectionStats detection_stats =
      loss_algorithm_->DetectLosses(unacked_packets_, time, rtt_stats_,
                                    largest_newly_acked_, packets_acked_,
                                    &packets_lost_);

  if (detection_stats.sent_packets_max_sequence_reordering >
      stats_->sent_packets_max_sequence_reordering) {
    stats_->sent_packets_max_sequence_reordering =
        detection_stats.sent_packets_max_sequence_reordering;
  }

  stats_->sent_packets_num_borderline_time_reorderings +=
      detection_stats.sent_packets_num_borderline_time_reorderings;

  stats_->total_loss_detection_response_time +=
      detection_stats.total_loss_detection_response_time;

  for (const LostPacket& packet : packets_lost_) {
    QuicTransmissionInfo* info =
        unacked_packets_.GetMutableTransmissionInfo(packet.packet_number);
    ++stats_->packets_lost;
    if (debug_delegate_ != nullptr) {
      debug_delegate_->OnPacketLoss(packet.packet_number,
                                    info->encryption_level, LOSS_RETRANSMISSION,
                                    time);
    }
    unacked_packets_.RemoveFromInFlight(info);

    MarkForRetransmission(packet.packet_number, LOSS_RETRANSMISSION);
  }
}

bool QuicSentPacketManager::MaybeUpdateRTT(QuicPacketNumber largest_acked,
                                           QuicTime::Delta ack_delay_time,
                                           QuicTime ack_receive_time) {
  // We rely on ack_delay_time to compute an RTT estimate, so we
  // only update rtt when the largest observed gets acked and the acked packet
  // is not useless.
  if (!unacked_packets_.IsUnacked(largest_acked)) {
    return false;
  }
  // We calculate the RTT based on the highest ACKed packet number, the lower
  // packet numbers will include the ACK aggregation delay.
  const QuicTransmissionInfo& transmission_info =
      unacked_packets_.GetTransmissionInfo(largest_acked);
  // Ensure the packet has a valid sent time.
  if (DCHECK_FLAG && transmission_info.sent_time == QuicTime::Zero()) {
    QUIC_BUG(quic_bug_10750_4)
        << "Acked packet has zero sent time, largest_acked:" << largest_acked;
    return false;
  }
  if (DCHECK_FLAG && transmission_info.state == NOT_CONTRIBUTING_RTT) {
    return false;
  }
  if (transmission_info.sent_time >= ack_receive_time) {
    return false;
  }

  QuicTime::Delta send_delta = ack_receive_time - transmission_info.sent_time;
#if DCHECK_FLAG
  const bool min_rtt_available = !rtt_stats_.min_rtt().IsZero();
#endif
  rtt_stats_.UpdateRtt(send_delta, ack_delay_time, ack_receive_time);
#if DCHECK_FLAG
  if (!min_rtt_available && !rtt_stats_.min_rtt().IsZero()) {
    loss_algorithm_->OnMinRttAvailable();
  }
#endif

  return true;
}

QuicTime::Delta QuicSentPacketManager::TimeUntilSend(QuicTime now) const {
  // The TLP logic is entirely contained within QuicSentPacketManager, so the
  // send algorithm does not need to be consulted.
  QUICHE_DCHECK(pending_timer_transmission_count_ == 0);

  if (using_pacing_) {
    return pacing_sender_.TimeUntilSend(now,
                                        unacked_packets_.bytes_in_flight());
  }

  return send_algorithm_->CanSend(unacked_packets_.bytes_in_flight())
    ? QuicTime::Delta::Zero()
    //: send_algorithm_->PacingRate(unacked_packets_.bytes_in_flight()).TransferTime(unacked_packets_.bytes_in_flight() - send_algorithm_->GetCongestionWindow());
    : QuicTime::Delta::FromSeconds(5);// QuicTime::Delta::Infinite();
}

const QuicTime QuicSentPacketManager::GetRetransmissionTime() const {
  if (unacked_packets_.bytes_in_flight() == 0 /* && //TODO3 no need for server
      PeerCompletedAddressValidation()**/) {
    return QuicTime::Zero();
  }
  QUICHE_DCHECK(pending_timer_transmission_count_ == 0);
  if (DCHECK_FLAG && pending_timer_transmission_count_ > 0) {
    // Do not set the timer if there is any credit left.
    return QuicTime::Zero();
  }

  if (loss_algorithm_->GetLossTimeout() != QuicTime::Zero()) { //LOSS_MODE
    return loss_algorithm_->GetLossTimeout();
  }
  if (DCHECK_FLAG && !handshake_finished_ && unacked_packets_.HasPendingCryptoPackets() && !handshake_mode_disabled_) {
    return unacked_packets_.GetLastCryptoPacketSentTime() + GetCryptoRetransmissionDelay();
  }

  //PTO_MODE mode
  if (!supports_multiple_packet_number_spaces()) {
    QUICHE_DCHECK(unacked_packets_.HasInFlightPackets());
    if (consecutive_pto_count_ == 0) {
      // Arm 1st PTO with earliest in flight sent time, and make sure at
      // least kFirstPtoSrttMultiplier * RTT has been passed since last
      // in flight packet.
      if (false && unacked_packets_.packets_in_flight() <= 2) {
        return unacked_packets_.GetLastInFlightPacketSentTime()
            + rtt_stats_.smoothed_rtt() * kFirstPtoSrttMultiplier / 2
            + rtt_stats_.mean_deviation() * kPtoRttvarMultiplier;
      }

      QuicTime delay_first = unacked_packets_.GetFirstInFlightTransmissionInfo()->sent_time +
                    GetProbeTimeoutDelay(NUM_PACKET_NUMBER_SPACES);
      QuicTime delay_last  = unacked_packets_.GetLastInFlightPacketSentTime() +
                    rtt_stats_.smoothed_rtt() * kFirstPtoSrttMultiplier / 2;
      return std::max(delay_first, delay_last);
    }
    // Ensure PTO never gets set to a time in the past.
    return unacked_packets_.GetLastInFlightPacketSentTime() + GetProbeTimeoutDelay(NUM_PACKET_NUMBER_SPACES);
  }

  PacketNumberSpace packet_number_space = NUM_PACKET_NUMBER_SPACES;
  // earliest_right_edge is the earliest sent time of the last in flight
  // packet of all packet number spaces.
  QuicTime earliest_right_edge =
      GetEarliestPacketSentTimeForPto(&packet_number_space);
  if (!earliest_right_edge.IsInitialized()) {
    // Arm PTO from now if there is no in flight packets.
    earliest_right_edge = clock_->ApproximateNow();
  }
  if (packet_number_space == APPLICATION_DATA &&
      consecutive_pto_count_ == 0) {
    const QuicTransmissionInfo* first_application_info =
        unacked_packets_.GetFirstInFlightTransmissionInfoOfSpace(
            APPLICATION_DATA);
    if (first_application_info != nullptr) {
      // Arm 1st PTO with earliest in flight sent time, and make sure at
      // least kFirstPtoSrttMultiplier * RTT has been passed since last
      // in flight packet. Only do this for application data.
      return
          std::max(
              first_application_info->sent_time + GetProbeTimeoutDelay(packet_number_space),
              earliest_right_edge + kFirstPtoSrttMultiplier * rtt_stats_.smoothed_rtt() / 2);
    }
  }
  return earliest_right_edge + GetProbeTimeoutDelay(packet_number_space);
}

const QuicTime::Delta QuicSentPacketManager::GetPathDegradingDelay() const {
  QUICHE_DCHECK_GT(num_ptos_for_path_degrading_, 0);
  return num_ptos_for_path_degrading_ * GetPtoDelay();
}

const QuicTime::Delta QuicSentPacketManager::GetNetworkBlackholeDelay(
    int8_t num_rtos_for_blackhole_detection) const {
  return GetNConsecutiveRetransmissionTimeoutDelay(
      kDefaultMaxTailLossProbes + num_rtos_for_blackhole_detection);
}

QuicTime::Delta QuicSentPacketManager::GetMtuReductionDelay(
    int8_t num_rtos_for_blackhole_detection) const {
  return GetNetworkBlackholeDelay(num_rtos_for_blackhole_detection / 2);
}

const QuicTime::Delta QuicSentPacketManager::GetCryptoRetransmissionDelay()
    const {
  // This is equivalent to the TailLossProbeDelay, but slightly more aggressive
  // because crypto handshake messages don't incur a delayed ack time.
  QuicTime::Delta srtt = rtt_stats_.SmoothedOrInitialRtt();
  int64_t delay_ms;
  if (conservative_handshake_retransmits_) {
    // Using the delayed ack time directly could cause conservative handshake
    // retransmissions to actually be more aggressive than the default.
    delay_ms = std::max(peer_max_ack_delay_.ToMilliseconds(),
                        static_cast<int64_t>(2 * srtt.ToMilliseconds()));
  } else {
    delay_ms = std::max(kMinHandshakeTimeoutMs,
                        static_cast<int64_t>(1.5 * srtt.ToMilliseconds()));
  }
  return QuicTime::Delta::FromMilliseconds(
      delay_ms << consecutive_crypto_retransmission_count_);
}

const QuicTime::Delta QuicSentPacketManager::GetProbeTimeoutDelay(
    PacketNumberSpace space) const {
  if (DCHECK_FLAG && rtt_stats_.smoothed_rtt().IsZero()) {
    // Respect kMinHandshakeTimeoutMs to avoid a potential amplification attack.
    QUIC_BUG_IF(quic_bug_12552_6, rtt_stats_.initial_rtt().IsZero());
    return std::max(rtt_stats_.initial_rtt() * kPtoMultiplierWithoutRttSamples,
                    QuicTime::Delta::FromMilliseconds(kMinHandshakeTimeoutMs)) *
           (1 << consecutive_pto_count_);
  }
  QuicTime::Delta pto_delay =
      rtt_stats_.smoothed_rtt() +
      kPtoRttvarMultiplier * rtt_stats_.mean_deviation() +
               kAlarmGranularity +
      (ShouldAddMaxAckDelay(space) ? peer_max_ack_delay_
                                   : QuicTime::Delta::Zero());
  return pto_delay * (1 << consecutive_pto_count_);
}

QuicTime::Delta QuicSentPacketManager::GetSlowStartDuration() const {
  if (send_algorithm_->GetCongestionControlType() == kBBR ||
      send_algorithm_->GetCongestionControlType() == kBBRv2) {
    return stats_->slowstart_duration.GetTotalElapsedTime(
        clock_->ApproximateNow());
  }
  return QuicTime::Delta::Infinite();
}

QuicByteCount QuicSentPacketManager::GetAvailableCongestionWindowInBytes()
    const {
  QuicByteCount congestion_window = GetCongestionWindowInBytes();
  QuicByteCount bytes_in_flight = GetBytesInFlight();
  return congestion_window - std::min(congestion_window, bytes_in_flight);
}

std::string QuicSentPacketManager::GetDebugState() const {
  return send_algorithm_->GetDebugState();
}

void QuicSentPacketManager::SetSendAlgorithm(
    CongestionControlType congestion_control_type) {
  if (send_algorithm_ &&
      send_algorithm_->GetCongestionControlType() == congestion_control_type) {
    return;
  }

  SetSendAlgorithm(SendAlgorithmInterface::Create(
      clock_, &rtt_stats_, &unacked_packets_, congestion_control_type, random_,
      stats_, initial_congestion_window_, send_algorithm_));
}

void QuicSentPacketManager::SetSendAlgorithm(
    SendAlgorithmInterface* send_algorithm) {
  delete send_algorithm_;
  send_algorithm_ = send_algorithm;
  pacing_sender_.set_sender(send_algorithm);
}

SendAlgorithmInterface*
QuicSentPacketManager::OnConnectionMigration(bool reset_send_algorithm) {
  consecutive_pto_count_ = 0;
  rtt_stats_.OnConnectionMigration();
  if (!reset_send_algorithm) {
    send_algorithm_->OnConnectionMigration();
    return nullptr;
  }

  auto old_send_algorithm = send_algorithm_;
  SetSendAlgorithm(old_send_algorithm->GetCongestionControlType());
  // Treat all in flight packets sent to the old peer address as lost and
  // retransmit them.
  QuicPacketNumber packet_number = unacked_packets_.GetLeastUnacked();
  for (auto it = unacked_packets_.begin(); it != unacked_packets_.end();
       ++it, ++packet_number) {
    if (it->in_flight) {
      // Proactively retransmit any packet which is in flight on the old path.
      // As a result, these packets will not contribute to congestion control.
      unacked_packets_.RemoveFromInFlight(packet_number);
      // Retransmitting these packets with PATH_CHANGE_RETRANSMISSION will mark
      // them as useless, thus not contributing to RTT stats.
      if (unacked_packets_.HasRetransmittableFrames(packet_number)) {
        MarkForRetransmission(packet_number, PATH_RETRANSMISSION);
        QUICHE_DCHECK_EQ(it->state, NOT_CONTRIBUTING_RTT);
      }
    }
    it->state = NOT_CONTRIBUTING_RTT;
  }
  return old_send_algorithm;
}

void QuicSentPacketManager::OnAckFrameStart(QuicPacketNumber largest_acked,
                                            QuicTime::Delta ack_delay_time,
                                            QuicTime ack_receive_time) {
  QUICHE_DCHECK(packets_acked_.empty());
  QUICHE_DCHECK_GE(unacked_packets_.largest_sent_packet(), largest_acked);
  // Ignore peer_max_ack_delay and use received ack_delay during
  // handshake when supporting multiple packet number spaces.
  if (!supports_multiple_packet_number_spaces() || handshake_finished_) {
    if (DCHECK_FLAG && ack_delay_time > peer_max_ack_delay()) {
      ack_delay_time = peer_max_ack_delay();
    }
    if (DCHECK_FLAG && ignore_ack_delay_) {
      ack_delay_time = QuicTime::Delta::Zero();
    }
  }
  rtt_updated_ =
      MaybeUpdateRTT(largest_acked, ack_delay_time, ack_receive_time);
  last_ack_frame_.ack_delay_time = ack_delay_time;
  acked_packets_iter_ = last_ack_frame_.packets.rbegin();
}

void QuicSentPacketManager::OnAckRange(QuicPacketNumber start,
                                       QuicPacketNumber end) {
  // Drop ack ranges which ack packets below least_unacked.
  QuicPacketNumber least_unacked = unacked_packets_.GetLeastUnacked();
  if (//!last_ack_frame_.largest_acked.IsInitialized() ||
      end.ToInt64() > last_ack_frame_.largest_acked.ToInt64() + 1) {
    // Largest acked increases.
    unacked_packets_.IncreaseLargestAcked(last_ack_frame_.largest_acked = end - 1);
    //last_ack_frame_.largest_acked = end - 1;
  }
  else if (end.ToInt64() <= least_unacked.ToInt64()) {
    return;
  }

  start = std::max(start, least_unacked);
  do {
    QuicPacketNumber newly_acked_start = start;
    if (acked_packets_iter_ != last_ack_frame_.packets.rend()) {
      newly_acked_start = std::max(start, acked_packets_iter_->max());
    }
    for (QuicPacketNumber acked = end - 1; newly_acked_start <= acked;
         --acked) {
      // Check if end is above the current range. If so add newly acked packets
      // in descending order.
      packets_acked_.emplace_back(acked, 0, QuicTime::Zero());
      if (false && acked == FirstSendingPacketNumber()) {
        break;
      }
    }
    if (acked_packets_iter_ == last_ack_frame_.packets.rend() ||
        start > acked_packets_iter_->min()) {
      // Finish adding all newly acked packets.
      return;
    }
    end = std::min(end, acked_packets_iter_->min());
    ++acked_packets_iter_;
  } while (start < end);
}

void QuicSentPacketManager::OnAckTimestamp(QuicPacketNumber packet_number,
                                           QuicTime timestamp) {
  last_ack_frame_.received_packet_times.push_back({packet_number, timestamp});
  for (AckedPacket& packet : packets_acked_) {
    if (packet.packet_number == packet_number) {
      packet.receive_timestamp = timestamp;
      return;
    }
  }
}

AckResult QuicSentPacketManager::OnAckFrameEnd(
    QuicTime ack_receive_time, QuicPacketNumber ack_packet_number,
    EncryptionLevel ack_decrypted_level) {
  QuicByteCount prior_bytes_in_flight = unacked_packets_.bytes_in_flight();
  // Reverse packets_acked_ so that it is in ascending order.
  if (packets_acked_.size() > 1)
    std::reverse(packets_acked_.begin(), packets_acked_.end());

  for (AckedPacket& acked_packet : packets_acked_) {
    QuicTransmissionInfo* info =
        unacked_packets_.GetMutableTransmissionInfo(acked_packet.packet_number);
    if (!QuicUtils::IsAckable(info->state)) {
      continue;
      if (info->state == ACKED) {
        QUIC_BUG(quic_bug_10750_5)
            << "Trying to ack an already acked packet: "
            << acked_packet.packet_number
            << ", last_ack_frame_: " << last_ack_frame_
            << ", least_unacked: " << unacked_packets_.GetLeastUnacked()
            << ", packets_acked_: " << quiche::PrintElements(packets_acked_);
      } else {
        QUIC_PEER_BUG(quic_peer_bug_10750_6)
            << "Received " << ack_decrypted_level
            << " ack for unackable packet: " << acked_packet.packet_number
            << " with state: "
            << QuicUtils::SentPacketStateToString(info->state);
        if (supports_multiple_packet_number_spaces()) {
          if (info->state == NEVER_SENT) {
            return UNSENT_PACKETS_ACKED;
          }
          return UNACKABLE_PACKETS_ACKED;
        }
      }
      continue;
    }
    QUIC_DVLOG(1) << ENDPOINT << "Got an " << ack_decrypted_level
                  << " ack for packet " << acked_packet.packet_number
                  << " , state: "
                  << QuicUtils::SentPacketStateToString(info->state);
    const PacketNumberSpace packet_number_space =
        unacked_packets_.GetPacketNumberSpace(info->encryption_level);
    if (supports_multiple_packet_number_spaces() &&
        QuicUtils::GetPacketNumberSpace(ack_decrypted_level) !=
            packet_number_space) {
      return PACKETS_ACKED_IN_WRONG_PACKET_NUMBER_SPACE;
    }
    last_ack_frame_.packets.Add(acked_packet.packet_number);

    rtt_packet_state_ |= (1 << info->encryption_level);

    largest_packet_peer_knows_is_acked_.UpdateMax(info->largest_acked);
    if (supports_multiple_packet_number_spaces()) {
      largest_packets_peer_knows_is_acked_[packet_number_space].UpdateMax(
          info->largest_acked);
    }
    // If data is associated with the most recent transmission of this
    // packet, then inform the caller.
    if (info->in_flight) {
      acked_packet.bytes_acked = info->bytes_sent;
    } else {
      // Unackable packets are skipped earlier.
      largest_newly_acked_ = acked_packet.packet_number;
    }
    unacked_packets_.MaybeUpdateLargestAckedOfPacketNumberSpace(
        packet_number_space, acked_packet.packet_number);
    MarkPacketHandled(acked_packet.packet_number, info, ack_receive_time,
                      last_ack_frame_.ack_delay_time,
                      acked_packet.receive_timestamp);
  }
  const bool acked_new_packet = !packets_acked_.empty();
  PostProcessNewlyAckedPackets(ack_packet_number, ack_decrypted_level,
                               ack_receive_time, rtt_updated_,
                               prior_bytes_in_flight);

  return acked_new_packet ? PACKETS_NEWLY_ACKED : NO_PACKETS_NEWLY_ACKED;
}

void QuicSentPacketManager::SetDebugDelegate(DebugDelegate* debug_delegate) {
  //debug_delegate_ = debug_delegate;
}

void QuicSentPacketManager::OnApplicationLimited() {
  if (using_pacing_) {
    pacing_sender_.OnApplicationLimited();
  }
  send_algorithm_->OnApplicationLimited(unacked_packets_.bytes_in_flight());
  if (debug_delegate_ != nullptr) {
    debug_delegate_->OnApplicationLimited();
  }
}

NextReleaseTimeResult QuicSentPacketManager::GetNextReleaseTime() const {
  if (!using_pacing_) {
    return {QuicTime::Zero(), false};
  }

  return pacing_sender_.GetNextReleaseTime();
}

void QuicSentPacketManager::SetInitialRtt(QuicTime::Delta rtt, bool trusted) {
  const QuicTime::Delta min_rtt = QuicTime::Delta::FromMicroseconds(
      trusted ? kMinTrustedInitialRoundTripTimeUs
              : kMinUntrustedInitialRoundTripTimeUs);
  QuicTime::Delta max_rtt =
      QuicTime::Delta::FromMicroseconds(kMaxInitialRoundTripTimeUs);
  rtt_stats_.set_initial_rtt(std::max(min_rtt, std::min(max_rtt, rtt)));
}

void QuicSentPacketManager::EnableMultiplePacketNumberSpacesSupport() {
  EnableIetfPtoAndLossDetection();
  unacked_packets_.EnableMultiplePacketNumberSpacesSupport();
}

QuicPacketNumber QuicSentPacketManager::GetLargestAckedPacket(
    EncryptionLevel decrypted_packet_level) const {
  QUICHE_DCHECK(supports_multiple_packet_number_spaces());
  return unacked_packets_.GetLargestAckedOfPacketNumberSpace(
      QuicUtils::GetPacketNumberSpace(decrypted_packet_level));
}

QuicPacketNumber QuicSentPacketManager::GetLeastPacketAwaitedByPeer(
    EncryptionLevel encryption_level) const {
  QuicPacketNumber largest_acked;
  if (supports_multiple_packet_number_spaces()) {
    largest_acked = GetLargestAckedPacket(encryption_level);
  } else {
    largest_acked = GetLargestObserved();
  }
  //QUICHE_DCHECK(largest_acked.IsInitialized());
  if (false && !largest_acked.IsInitialized()) {
    // If no packets have been acked, return the first sent packet to ensure
    // we use a large enough packet number length.
    return FirstSendingPacketNumber();
  }
  QuicPacketNumber least_awaited = QuicPacketNumber(1) + largest_acked.ToUint64();
  QuicPacketNumber least_unacked = GetLeastUnacked();
  if (least_unacked < least_awaited) {
    QUICHE_DCHECK(least_unacked.IsInitialized());
    least_awaited = least_unacked;
  }
  return least_awaited;
}

QuicPacketNumber QuicSentPacketManager::GetLargestPacketPeerKnowsIsAcked(
    EncryptionLevel decrypted_packet_level) const {
  QUICHE_DCHECK(supports_multiple_packet_number_spaces());
  return largest_packets_peer_knows_is_acked_[QuicUtils::GetPacketNumberSpace(
      decrypted_packet_level)];
}

QuicTime::Delta
QuicSentPacketManager::GetNConsecutiveRetransmissionTimeoutDelay(
    int num_timeouts) const {
  QuicTime::Delta total_delay = QuicTime::Delta::Zero();
  const QuicTime::Delta srtt = rtt_stats_.SmoothedOrInitialRtt();
  int num_tlps =
      std::min(num_timeouts, static_cast<int>(kDefaultMaxTailLossProbes));
  num_timeouts -= num_tlps;
  if (num_tlps > 0) {
    const QuicTime::Delta tlp_delay = std::max(
        2 * srtt,
        unacked_packets_.HasMultipleInFlightPackets()
            ? QuicTime::Delta::FromMilliseconds(kMinTailLossProbeTimeoutMs)
            : (1.5 * srtt +
               (QuicTime::Delta::FromMilliseconds(kMinRetransmissionTimeMs) *
                0.5)));
    total_delay = total_delay + num_tlps * tlp_delay;
  }
  if (num_timeouts == 0) {
    return total_delay;
  }

  const QuicTime::Delta retransmission_delay =
      rtt_stats_.smoothed_rtt().IsZero()
          ? QuicTime::Delta::FromMilliseconds(kDefaultRetransmissionTimeMs)
          : std::max(
                srtt + 4 * rtt_stats_.mean_deviation(),
                QuicTime::Delta::FromMilliseconds(kMinRetransmissionTimeMs));
  total_delay = total_delay + ((1 << num_timeouts) - 1) * retransmission_delay;
  return total_delay;
}

bool QuicSentPacketManager::PeerCompletedAddressValidation() const {
  if (!handshake_mode_disabled_ ||
      unacked_packets_.perspective() == Perspective::IS_SERVER) {
    return true;
  }

  // To avoid handshake deadlock due to anti-amplification limit, client needs
  // to set PTO timer until server successfully processed any HANDSHAKE packet.
  return handshake_finished_ || (handshake_packet_acked());
}

bool QuicSentPacketManager::IsLessThanThreePTOs(QuicTime::Delta timeout) const {
  return timeout < 3 * GetPtoDelay();
}

QuicTime::Delta QuicSentPacketManager::GetPtoDelay() const {
  return GetProbeTimeoutDelay(APPLICATION_DATA);
}

void QuicSentPacketManager::OnAckFrequencyFrameSent(
    const QuicAckFrequencyFrame& ack_frequency_frame) {
  in_use_sent_ack_delays_.emplace_back(ack_frequency_frame.max_ack_delay,
                                       ack_frequency_frame.sequence_number);
  if (ack_frequency_frame.max_ack_delay > peer_max_ack_delay_) {
    peer_max_ack_delay_ = ack_frequency_frame.max_ack_delay;
  }
}

void QuicSentPacketManager::OnAckFrequencyFrameAcked(
    const QuicAckFrequencyFrame& ack_frequency_frame) {
  int stale_entry_count = 0;
  for (auto it = in_use_sent_ack_delays_.cbegin();
       it != in_use_sent_ack_delays_.cend(); ++it) {
    if (it->second < ack_frequency_frame.sequence_number) {
      ++stale_entry_count;
    } else {
      break;
    }
  }
  if (stale_entry_count > 0) {
    in_use_sent_ack_delays_.pop_front_n(stale_entry_count);
  }
  if (in_use_sent_ack_delays_.empty()) {
    QUIC_BUG(quic_bug_10750_7) << "in_use_sent_ack_delays_ is empty.";
    return;
  }
  peer_max_ack_delay_ = std::max_element(in_use_sent_ack_delays_.cbegin(),
                                         in_use_sent_ack_delays_.cend())
                            ->first;
}

#undef ENDPOINT  // undef for jumbo builds
}  // namespace quic
