// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quiche/quic/core/quic_packets.h"

#include <utility>

#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "quiche/quic/core/quic_connection_id.h"
#include "quiche/quic/core/quic_types.h"
#include "quiche/quic/core/quic_utils.h"
#include "quiche/quic/core/quic_versions.h"
#include "quiche/quic/platform/api/quic_flag_utils.h"
#include "quiche/quic/platform/api/quic_flags.h"

namespace quic {

QuicConnectionId GetServerConnectionIdAsRecipient(
    const QuicPacketHeader& header, Perspective perspective) {
  if (QUIC_SERVER_SESSION == Perspective::IS_SERVER || perspective == Perspective::IS_SERVER) {
    return header.destination_connection_id;
  }
  return header.source_connection_id;
}

QuicConnectionId GetClientConnectionIdAsRecipient(
    const QuicPacketHeader& header, Perspective perspective) {
  if (QUIC_SERVER_SESSION == Perspective::IS_CLIENT || perspective == Perspective::IS_CLIENT) {
    return header.destination_connection_id;
  }
  return header.source_connection_id;
}

QuicConnectionId GetServerConnectionIdAsSender(const QuicPacketHeader& header,
                                               Perspective perspective) {
  if (QUIC_SERVER_SESSION == Perspective::IS_CLIENT || perspective == Perspective::IS_CLIENT) {
    return header.destination_connection_id;
  }
  return header.source_connection_id;
}

QuicConnectionIdIncluded GetServerConnectionIdIncludedAsSender(
    const QuicPacketHeader& header, Perspective perspective) {
  if (QUIC_SERVER_SESSION == Perspective::IS_CLIENT || perspective == Perspective::IS_CLIENT) {
    return header.destination_connection_id_included;
  }
  return header.source_connection_id_included;
}

QuicConnectionId GetClientConnectionIdAsSender(const QuicPacketHeader& header,
                                               Perspective perspective) {
  if (QUIC_SERVER_SESSION == Perspective::IS_CLIENT || perspective == Perspective::IS_CLIENT) {
    return header.source_connection_id;
  }
  return header.destination_connection_id;
}

QuicConnectionIdIncluded GetClientConnectionIdIncludedAsSender(
    const QuicPacketHeader& header, Perspective perspective) {
  if (QUIC_SERVER_SESSION == Perspective::IS_CLIENT || perspective == Perspective::IS_CLIENT) {
    return header.source_connection_id_included;
  }
  return header.destination_connection_id_included;
}

uint8_t GetIncludedConnectionIdLength(
    QuicConnectionId connection_id,
    QuicConnectionIdIncluded connection_id_included) {
  QUICHE_DCHECK(connection_id_included == CONNECTION_ID_PRESENT ||
                connection_id_included == CONNECTION_ID_ABSENT);
  return connection_id_included == CONNECTION_ID_PRESENT
             ? connection_id.length()
             : 0;
}

uint8_t GetIncludedDestinationConnectionIdLength(
    const QuicPacketHeader& header) {
  return GetIncludedConnectionIdLength(
      header.destination_connection_id,
      header.destination_connection_id_included);
}

uint8_t GetIncludedSourceConnectionIdLength(const QuicPacketHeader& header) {
  return GetIncludedConnectionIdLength(header.source_connection_id,
                                       header.source_connection_id_included);
}

size_t GetPacketHeaderSize(QuicTransportVersion version,
                           const QuicPacketHeader& header) {
  return GetPacketHeaderSize(
      version, GetIncludedDestinationConnectionIdLength(header),
      GetIncludedSourceConnectionIdLength(header), header.version_flag,
      header.nonce != nullptr, header.packet_number_length,
      header.retry_token_length_length, header.retry_token.length(),
      header.length_length);
}

size_t GetPacketHeaderSize(
    QuicTransportVersion version, uint8_t destination_connection_id_length,
    uint8_t source_connection_id_length, bool include_version,
    bool include_diversification_nonce,
    QuicPacketNumberLength packet_number_length,
    quiche::QuicheVariableLengthIntegerLength retry_token_length_length,
    QuicByteCount retry_token_length,
    quiche::QuicheVariableLengthIntegerLength length_length) {
  if (VersionHasIetfInvariantHeader(version)) {
    if (include_version) {
      // Long header.
      size_t size = kPacketHeaderTypeSize + kConnectionIdLengthSize +
                    destination_connection_id_length +
                    source_connection_id_length + packet_number_length +
                    kQuicVersionSize;
      if (include_diversification_nonce) {
        size += kDiversificationNonceSize;
      }
      if (VersionHasLengthPrefixedConnectionIds(version)) {
        size += kConnectionIdLengthSize;
      }
      QUICHE_DCHECK(
          QuicVersionHasLongHeaderLengths(version) ||
          retry_token_length_length + retry_token_length + length_length == 0);
      if (QuicVersionHasLongHeaderLengths(version)) {
        size += retry_token_length_length + retry_token_length + length_length;
      }
      return size;
    }
    // Short header.
    return kPacketHeaderTypeSize + destination_connection_id_length +
           packet_number_length;
  }
  // Google QUIC versions <= 43 can only carry one connection ID.
  QUICHE_DCHECK(destination_connection_id_length == 0 ||
                source_connection_id_length == 0);
  return kPublicFlagsSize + destination_connection_id_length +
         source_connection_id_length +
         (include_version ? kQuicVersionSize : 0) + packet_number_length +
         (include_diversification_nonce ? kDiversificationNonceSize : 0);
}

size_t GetStartOfEncryptedData(QuicTransportVersion version,
                               const QuicPacketHeader& header) {
  return GetPacketHeaderSize(version, header);
}

size_t GetStartOfEncryptedData(
    QuicTransportVersion version, uint8_t destination_connection_id_length,
    uint8_t source_connection_id_length, bool include_version,
    bool include_diversification_nonce,
    QuicPacketNumberLength packet_number_length,
    quiche::QuicheVariableLengthIntegerLength retry_token_length_length,
    QuicByteCount retry_token_length,
    quiche::QuicheVariableLengthIntegerLength length_length) {
  // Encryption starts before private flags.
  return GetPacketHeaderSize(
      version, destination_connection_id_length, source_connection_id_length,
      include_version, include_diversification_nonce, packet_number_length,
      retry_token_length_length, retry_token_length, length_length);
}

QuicPacketHeader::QuicPacketHeader():
#if 0
    destination_connection_id(EmptyQuicConnectionId()),
    source_connection_id(EmptyQuicConnectionId()),
    destination_connection_id_included(CONNECTION_ID_PRESENT),
    source_connection_id_included(CONNECTION_ID_ABSENT),
    reset_flag(false),
    version_flag(false),
    has_possible_stateless_reset_token(false),
    packet_number_length(PACKET_4BYTE_PACKET_NUMBER),
#endif
    version(UnsupportedQuicVersion()),
#if 0
    form(GOOGLE_QUIC_PACKET),
    long_packet_type(INITIAL),
    length_length(quiche::VARIABLE_LENGTH_INTEGER_LENGTH_0),
    possible_stateless_reset_token({}),
    retry_token_length_length(quiche::VARIABLE_LENGTH_INTEGER_LENGTH_0),
    retry_token(absl::string_view()),
    remaining_packet_length(0),
#endif
    nonce(nullptr) {}

QuicPacketHeader::QuicPacketHeader(bool read)
  :
//  : destination_connection_id(EmptyQuicConnectionId()),
//  source_connection_id(EmptyQuicConnectionId()),
  destination_connection_id_included(CONNECTION_ID_PRESENT),
  source_connection_id_included(CONNECTION_ID_ABSENT),
  reset_flag(false),
  version_flag(false),
  has_possible_stateless_reset_token(false),
  packet_number_length(PACKET_4BYTE_PACKET_NUMBER),
  version(UnsupportedQuicVersion()),
  form(GOOGLE_QUIC_PACKET),
  long_packet_type(INITIAL),
  length_length(quiche::VARIABLE_LENGTH_INTEGER_LENGTH_0),
  //possible_stateless_reset_token({}),
  retry_token_length_length(quiche::VARIABLE_LENGTH_INTEGER_LENGTH_0),
  //retry_token(absl::string_view()),
  remaining_packet_length(0),
  nonce(nullptr) {
}

QuicPacketHeader::QuicPacketHeader(const QuicPacketHeader& other) = default;

QuicPacketHeader::~QuicPacketHeader() {}

QuicPacketHeader& QuicPacketHeader::operator=(const QuicPacketHeader& other) =
    default;

QuicPublicResetPacket::QuicPublicResetPacket()
    : connection_id(EmptyQuicConnectionId()), nonce_proof(0) {}

QuicPublicResetPacket::QuicPublicResetPacket(QuicConnectionId connection_id)
    : connection_id(connection_id), nonce_proof(0) {}

QuicVersionNegotiationPacket::QuicVersionNegotiationPacket()
    : connection_id(EmptyQuicConnectionId()) {}

QuicVersionNegotiationPacket::QuicVersionNegotiationPacket(
    QuicConnectionId connection_id)
    : connection_id(connection_id) {}

QuicVersionNegotiationPacket::QuicVersionNegotiationPacket(
    const QuicVersionNegotiationPacket& other) = default;

QuicVersionNegotiationPacket::~QuicVersionNegotiationPacket() {}

QuicIetfStatelessResetPacket::QuicIetfStatelessResetPacket()
    : stateless_reset_token({}) {}

QuicIetfStatelessResetPacket::QuicIetfStatelessResetPacket(
    const QuicPacketHeader& header, StatelessResetToken token)
    : header(header), stateless_reset_token(token) {}

QuicIetfStatelessResetPacket::QuicIetfStatelessResetPacket(
    const QuicIetfStatelessResetPacket& other) = default;

QuicIetfStatelessResetPacket::~QuicIetfStatelessResetPacket() {}

std::ostream& operator<<(std::ostream& os, const QuicPacketHeader& header) {
  os << "{ destination_connection_id: " << header.destination_connection_id
     << " ("
     << (header.destination_connection_id_included == CONNECTION_ID_PRESENT
             ? "present"
             : "absent")
     << "), source_connection_id: " << header.source_connection_id << " ("
     << (header.source_connection_id_included == CONNECTION_ID_PRESENT
             ? "present"
             : "absent")
     << "), packet_number_length: "
     << static_cast<int>(header.packet_number_length)
     << ", reset_flag: " << header.reset_flag
     << ", version_flag: " << header.version_flag;
  if (header.version_flag) {
    os << ", version: " << ParsedQuicVersionToString(header.version);
    if (header.long_packet_type != INVALID_PACKET_TYPE) {
      os << ", long_packet_type: "
         << QuicUtils::QuicLongHeaderTypetoString(header.long_packet_type);
    }
    if (header.retry_token_length_length !=
        quiche::VARIABLE_LENGTH_INTEGER_LENGTH_0) {
      os << ", retry_token_length_length: "
         << static_cast<int>(header.retry_token_length_length);
    }
    if (header.retry_token.length() != 0) {
      os << ", retry_token_length: " << header.retry_token.length();
    }
    if (header.length_length != quiche::VARIABLE_LENGTH_INTEGER_LENGTH_0) {
      os << ", length_length: " << static_cast<int>(header.length_length);
    }
    if (header.remaining_packet_length != 0) {
      os << ", remaining_packet_length: " << header.remaining_packet_length;
    }
  }
  if (header.nonce != nullptr) {
    os << ", diversification_nonce: "
       << absl::BytesToHexString(
              absl::string_view(header.nonce->data(), header.nonce->size()));
  }
  os << ", packet_number: " << header.packet_number << " }\n";
  return os;
}

QuicData::QuicData(const char* buffer, size_t length)
    : buffer_(buffer), length_(length), owns_buffer_(false) {}

QuicData::QuicData(const char* buffer, size_t length, bool owns_buffer)
    : buffer_(buffer), length_(length), owns_buffer_(owns_buffer) {}

QuicData::QuicData(absl::string_view packet_data)
    : buffer_(packet_data.data()),
      length_(packet_data.length()),
      owns_buffer_(false) {}

QuicData::~QuicData() {
  if (owns_buffer_) {
    delete[] const_cast<char*>(buffer_);
  }
}

QuicPacket::QuicPacket(
    char* buffer, size_t length, bool owns_buffer,
    uint8_t destination_connection_id_length,
    uint8_t source_connection_id_length, bool includes_version,
    bool includes_diversification_nonce,
    QuicPacketNumberLength packet_number_length,
    quiche::QuicheVariableLengthIntegerLength retry_token_length_length,
    QuicByteCount retry_token_length,
    quiche::QuicheVariableLengthIntegerLength length_length)
    : QuicData(buffer, length, owns_buffer),
      buffer_(buffer),
      destination_connection_id_length_(destination_connection_id_length),
      source_connection_id_length_(source_connection_id_length),
      length_length_(length_length),
      includes_version_(includes_version),
      includes_diversification_nonce_(includes_diversification_nonce),
      packet_number_length_(packet_number_length),
      retry_token_length_length_(retry_token_length_length),
      retry_token_length_(retry_token_length) {}

QuicPacket::QuicPacket(QuicTransportVersion /*version*/, char* buffer,
                       size_t length, bool owns_buffer,
                       const QuicPacketHeader& header)
    : QuicPacket(buffer, length, owns_buffer,
                 GetIncludedDestinationConnectionIdLength(header),
                 GetIncludedSourceConnectionIdLength(header),
                 header.version_flag, header.nonce != nullptr,
                 header.packet_number_length, header.retry_token_length_length,
                 header.retry_token.length(), header.length_length) {}

QuicEncryptedPacket::QuicEncryptedPacket(const char* buffer, size_t length)
    : QuicData(buffer, length) {}

QuicEncryptedPacket::QuicEncryptedPacket(const char* buffer, size_t length,
                                         bool owns_buffer)
    : QuicData(buffer, length, owns_buffer) {}

QuicEncryptedPacket::QuicEncryptedPacket(absl::string_view data)
    : QuicData(data) {}

std::unique_ptr<QuicEncryptedPacket> QuicEncryptedPacket::Clone() const {
  char* buffer = new char[this->length()];
  memcpy(buffer, this->data(), this->length());
  return std::make_unique<QuicEncryptedPacket>(buffer, this->length(), true);
}

std::ostream& operator<<(std::ostream& os, const QuicEncryptedPacket& s) {
  os << s.length() << "-byte data";
  return os;
}

QuicReceivedPacket::QuicReceivedPacket(const char* buffer, size_t length,
                                       QuicTime receipt_time)
    : QuicReceivedPacket(buffer, length, receipt_time,
                         false /* owns_buffer */) {}

QuicReceivedPacket::QuicReceivedPacket(const char* buffer, size_t length,
                                       QuicTime receipt_time, bool owns_buffer)
    : QuicReceivedPacket(buffer, length, receipt_time, owns_buffer, 0 /* ttl */,
                         true /* ttl_valid */) {}

QuicReceivedPacket::QuicReceivedPacket(const char* buffer, size_t length,
                                       QuicTime receipt_time, bool owns_buffer,
                                       int ttl, bool ttl_valid)
    : quic::QuicReceivedPacket(buffer, length, receipt_time, owns_buffer, ttl,
                               ttl_valid, nullptr /* packet_headers */,
                               0 /* headers_length */,
                               false /* owns_header_buffer */) {}

QuicReceivedPacket::QuicReceivedPacket(const char* buffer, size_t length,
                                       QuicTime receipt_time, bool owns_buffer,
                                       int ttl, bool ttl_valid,
                                       char* packet_headers,
                                       size_t headers_length,
                                       bool owns_header_buffer)
    : QuicEncryptedPacket(buffer, length, owns_buffer),
      receipt_time_(receipt_time),
      packet_headers_(packet_headers),
      ttl_(ttl_valid ? ttl : -1),
      headers_length_(headers_length),
      owns_header_buffer_(owns_header_buffer) {}

QuicReceivedPacket::~QuicReceivedPacket() {
  if (DCHECK_FLAG && owns_header_buffer_) {
    delete[] static_cast<char*>(packet_headers_);
  }
}

std::unique_ptr<QuicReceivedPacket> QuicReceivedPacket::Clone() const {
  char* buffer = new char[this->length()];
  memcpy(buffer, this->data(), this->length());
  if (this->packet_headers()) {
    char* headers_buffer = new char[this->headers_length()];
    memcpy(headers_buffer, this->packet_headers(), this->headers_length());
    return std::make_unique<QuicReceivedPacket>(
        buffer, this->length(), receipt_time(), true, ttl(), ttl() >= 0,
        headers_buffer, this->headers_length(), true);
  }

  return std::make_unique<QuicReceivedPacket>(
      buffer, this->length(), receipt_time(), true, ttl(), ttl() >= 0);
}

std::ostream& operator<<(std::ostream& os, const QuicReceivedPacket& s) {
  os << s.length() << "-byte data";
  return os;
}

absl::string_view QuicPacket::AssociatedData(
    QuicTransportVersion version) const {
  return absl::string_view(
      data(),
      GetStartOfEncryptedData(version, destination_connection_id_length_,
                              source_connection_id_length_, includes_version_,
                              includes_diversification_nonce_,
                              packet_number_length_, retry_token_length_length_,
                              retry_token_length_, length_length_));
}

absl::string_view QuicPacket::Plaintext(QuicTransportVersion version) const {
  const size_t start_of_encrypted_data = GetStartOfEncryptedData(
      version, destination_connection_id_length_, source_connection_id_length_,
      includes_version_, includes_diversification_nonce_, packet_number_length_,
      retry_token_length_length_, retry_token_length_, length_length_);
  return absl::string_view(data() + start_of_encrypted_data,
                           length() - start_of_encrypted_data);
}

SerializedPacket::SerializedPacket(QuicPacketNumber packet_number,
                                   QuicPacketNumberLength packet_number_length,
                                   const char* encrypted_buffer,
                                   QuicPacketLength encrypted_length)
    : encrypted_buffer(encrypted_buffer),
      encrypted_length(encrypted_length),
      //has_crypto_handshake(NOT_HANDSHAKE),
      packet_number(packet_number),
      packet_number_length(packet_number_length),
      encryption_level(ENCRYPTION_INITIAL),
      transmission_type(NOT_RETRANSMISSION),
//      has_ack(false),
      fate(SEND_TO_WRITER),
      frame_types(0)
{}

SerializedPacket::SerializedPacket(SerializedPacket&& other) noexcept
     : retransmittable_frames(std::move(other.retransmittable_frames)),
      //has_crypto_handshake(other.has_crypto_handshake),
      packet_number(other.packet_number),
      packet_number_length(other.packet_number_length),
      encryption_level(other.encryption_level),
//      has_ack(other.has_ack),
      transmission_type(other.transmission_type),
      fate(other.fate),
      frame_types(other.frame_types),
      largest_acked(other.largest_acked),
      peer_address(other.peer_address),
      bytes_not_retransmitted(other.bytes_not_retransmitted) {
    if (!other.nonretransmittable_frames.empty())
      nonretransmittable_frames = std::move(other.nonretransmittable_frames);
    if (release_encrypted_buffer && encrypted_buffer != nullptr) {
      release_encrypted_buffer(encrypted_buffer);
    }
    encrypted_buffer = other.encrypted_buffer;
    encrypted_length = other.encrypted_length;
    release_encrypted_buffer = std::move(other.release_encrypted_buffer);
    other.release_encrypted_buffer = nullptr;
}

constexpr int DEL_FRAME_TYPES =
//    1 << ACK_FRAME        |
    1 << ACK_FRAME_COPY   |
    1 << RST_STREAM_FRAME |
    1 << CONNECTION_CLOSE_FRAME |
    1 << GOAWAY_FRAME  |
    1 << MESSAGE_FRAME |
    1 << CRYPTO_FRAME
#if QUIC_TLS_SESSION
  | 1 << NEW_CONNECTION_ID_FRAME |
    1 << RETIRE_CONNECTION_ID_FRAME |
    1 << NEW_TOKEN_FRAME |
    1 << ACK_FREQUENCY_FRAME
#endif
  ;

SerializedPacket::~SerializedPacket() {
  if (DCHECK_FLAG && release_encrypted_buffer && encrypted_buffer != nullptr) {
    release_encrypted_buffer(encrypted_buffer);
  }

  if (0 == (DEL_FRAME_TYPES & frame_types))
    return;

  //QUICHE_DCHECK(retransmittable_frames.empty());
  if (!retransmittable_frames.empty()) {
    DeleteFrames(&retransmittable_frames);
  }
  for (auto& frame : nonretransmittable_frames) {
    if (frame.type == ACK_FRAME  && !(frame_types & (1 << ACK_FRAME_COPY))) {
      // Do not delete ack frame if the packet does not own a copy of it.
      continue;
    }
    if (DEL_FRAME_TYPES & frame.type)
      DeleteFrame(&frame);
  }
}

SerializedPacket* CopySerializedPacket(const SerializedPacket& serialized,
                                       quiche::QuicheBufferAllocator* allocator,
                                       bool copy_buffer) {
  SerializedPacket* copy = new SerializedPacket(
      serialized.packet_number, serialized.packet_number_length,
      serialized.encrypted_buffer, serialized.encrypted_length);
//  copy->has_ack = serialized.has_ack;
  //copy->has_crypto_handshake = serialized.has_crypto_handshake;
  copy->encryption_level = serialized.encryption_level;
  copy->transmission_type = serialized.transmission_type;
  copy->largest_acked = serialized.largest_acked;
  copy->frame_types = serialized.frame_types;
  copy->fate = serialized.fate;
  copy->peer_address = serialized.peer_address;
  copy->bytes_not_retransmitted = serialized.bytes_not_retransmitted;

  if (copy_buffer) {
    copy->encrypted_buffer = CopyBuffer(serialized);
    copy->release_encrypted_buffer = [](const char* p) { delete[] p; };
  }
  // Copy underlying frames.
  copy->retransmittable_frames =
      CopyQuicFrames(allocator, serialized.retransmittable_frames);
  QUICHE_DCHECK(copy->nonretransmittable_frames.empty());
  for (const auto& frame : serialized.nonretransmittable_frames) {
    if (frame.type == ACK_FRAME) {
      copy->frame_types |= (1 << ACK_FRAME) | (1 << ACK_FRAME_COPY);
    }
    copy->nonretransmittable_frames.push_back(CopyQuicFrame(allocator, frame));
  }
  return copy;
}

char* CopyBuffer(const SerializedPacket& packet) {
  return CopyBuffer(packet.encrypted_buffer, packet.encrypted_length);
}

char* CopyBuffer(const char* encrypted_buffer,
                 QuicPacketLength encrypted_length) {
  char* dst_buffer = new char[encrypted_length];
  memcpy(dst_buffer, encrypted_buffer, encrypted_length);
  return dst_buffer;
}

ReceivedPacketInfo::ReceivedPacketInfo(const QuicSocketAddress& self_address,
                                       const QuicSocketAddress& peer_address,
                                       const QuicReceivedPacket& packet)
    : self_address(self_address),
      peer_address(peer_address),
      packet(packet),
      form(GOOGLE_QUIC_PACKET),
      long_packet_type(INVALID_PACKET_TYPE),
      version_flag(false),
      use_length_prefix(false),
      version_label(0),
      version(ParsedQuicVersion::Unsupported()),
      destination_connection_id(EmptyQuicConnectionId()),
      source_connection_id(EmptyQuicConnectionId()) {}

ReceivedPacketInfo::~ReceivedPacketInfo() {}

std::string ReceivedPacketInfo::ToString() const {
  std::string output =
      absl::StrCat("{ self_address: ", self_address.ToString(),
                   ", peer_address: ", peer_address.ToString(),
                   ", packet_length: ", packet.length(),
                   ", header_format: ", form, ", version_flag: ", version_flag);
  if (version_flag) {
    absl::StrAppend(&output, ", version: ", ParsedQuicVersionToString(version));
  }
  absl::StrAppend(
      &output,
      ", destination_connection_id: ", destination_connection_id.ToString(),
      ", source_connection_id: ", source_connection_id.ToString(), " }\n");
  return output;
}

std::ostream& operator<<(std::ostream& os,
                         const ReceivedPacketInfo& packet_info) {
  os << packet_info.ToString();
  return os;
}

}  // namespace quic
