// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quiche/quic/core/quic_connection_stats.h"

namespace quic {

std::ostream& operator<<(std::ostream& os, const QuicConnectionStats& s) {
  //hybchanged simplicity
  //os << "{ bytes_sent: " << s.bytes_sent;
  os << " packets_sent: " << s.packets_sent;
  os << " stream_packets_sent: " << s.stream_packets_sent;
  os << " notrans_packets_sent: " << s.notrans_packets_sent;
  os << " control_packets_sent: " << s.control_packets_sent;
  os << " packets_recv: " << s.packets_received;
  os << " stream_packets_recv: " << s.stream_packets_recv;
  os << " ack_packets_recv: " << s.ack_packets_recv;
  //if (s.stream_bytes_sent > 0)
  //os << " stream_bytes_sent: " << s.stream_bytes_sent;
  if (s.packets_discarded > 0)
  os << " packets_discarded: " << s.packets_discarded;
  if (s.ping_frames_sent > 0)
    os << " ping_frames_sent: " << s.ping_frames_sent;
  if (s.max_consecutive_rto_with_forward_progress > 0)
  os << " max_consecutive_rto_with_forward_progress: " << s.max_consecutive_rto_with_forward_progress;

  //os << " bytes_received: " << s.bytes_received;

  if (s.packets_processed != s.packets_received)
  os << " packets_processed: " << s.packets_processed;

  if (s.packets_retransmitted > 0) {
    os << " stream_bytes_received: " << s.stream_bytes_received;
    os << " bytes_retransmitted: " << s.bytes_retransmitted << " packets_retransmitted: " << s.packets_retransmitted;
  }
  if (s.packet_spuriously_detected_lost + s.bytes_spuriously_retransmitted) {
  os << " packet_spuriously_detected_lost: " << s.packet_spuriously_detected_lost;
//  os << " bytes_spuriously_retransmitted: " << s.bytes_spuriously_retransmitted;
  os << " packets_spuriously_retransmitted: "
     << s.packets_spuriously_retransmitted;
  }
  if (s.packets_lost)
  os << " packets_lost: " << s.packets_lost;
  if (s.slowstart_packets_sent > 0)
  os << " slowstart_packets_sent: " << s.slowstart_packets_sent;
  if (s.slowstart_packets_lost) {
  os << " slowstart_packets_lost: " << s.slowstart_packets_lost;
//  os << " slowstart_bytes_lost: " << s.slowstart_bytes_lost;
  }
  if (s.packets_dropped)
  os << " packets_dropped: " << s.packets_dropped;
  if (s.undecryptable_packets_received_before_handshake_complete)
  os << " undecryptable_packets_received_before_handshake_complete: "
     << s.undecryptable_packets_received_before_handshake_complete;
  if (s.crypto_retransmit_count)
  os << " crypto_retransmit_count: " << s.crypto_retransmit_count;
  if (s.loss_timeout_count)
  os << " loss_timeout_count: " << s.loss_timeout_count;
  if (s.pto_count > 0)
  os << " pto_count: " << s.pto_count;
  os << " min_rtt_us: " << s.min_rtt_us << " srtt_us: " << s.srtt_us;
  os << " egress_mtu: " << s.egress_mtu << " max_egress_mtu: " << s.max_egress_mtu <<
   " ingress_mtu: " << s.ingress_mtu <<
   " estimated_bandwidth: " << s.estimated_bandwidth;
  if (s.packets_reordered + s.max_sequence_reordering) {
  os << " packets_reordered: " << s.packets_reordered;
  os << " max_sequence_reordering: " << s.max_sequence_reordering;
  os << " max_time_reordering_us: " << s.max_time_reordering_us;
  }
  if (s.tcp_loss_events)
  os << " tcp_loss_events: " << s.tcp_loss_events;
  os << " send_alarms: " << s.send_alarms;
//  os << " connection_creation_time: "
//     << s.connection_creation_time.ToDebuggingValue();
  if (s.blocked_frames_received)
  os << " blocked_frames_received: " << s.blocked_frames_received;
//  if (s.blocked_frames_sent)
//  os << " blocked_frames_sent: " << s.blocked_frames_sent;
#if QUIC_TLS_SESSION
  if (s.num_connectivity_probing_received)
  os << " num_connectivity_probing_received: " << s.num_connectivity_probing_received;
  if (s.num_path_response_received)
  os << " num_path_response_received: " << s.num_path_response_received;
  if (s.retry_packet_processed)
  os << " retry_packet_processed: yes";
  if (s.num_coalesced_packets_received)
  os << " num_coalesced_packets_received: " << s.num_coalesced_packets_received;
  if (s.num_coalesced_packets_processed)
  os << " num_coalesced_packets_processed: "<< s.num_coalesced_packets_processed;
  if (s.num_ack_aggregation_epochs)
  os << " num_ack_aggregation_epochs: " << s.num_ack_aggregation_epochs;
  if (s.key_update_count)
  os << " key_update_count: " << s.key_update_count;
  if (s.num_failed_authentication_packets_received)
  os << " num_failed_authentication_packets_received: "
     << s.num_failed_authentication_packets_received;

  if (s.num_tls_server_zero_rtt_packets_received_after_discarding_decrypter)
  os << " num_tls_server_zero_rtt_packets_received_after_discarding_decrypter: "
     << s.num_tls_server_zero_rtt_packets_received_after_discarding_decrypter;
  if (s.address_validated_via_decrypting_packet)
  os << " address_validated_via_decrypting_packet: "
     << s.address_validated_via_decrypting_packet;
  if (s.address_validated_via_token)
  os << " address_validated_via_token: " << s.address_validated_via_token;
#endif
  os << " }";

  return os;
}

}  // namespace quic
