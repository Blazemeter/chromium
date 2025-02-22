// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/third_party/quic/core/quic_crypto_stream.h"

#include "net/third_party/quic/core/crypto/crypto_handshake.h"
#include "net/third_party/quic/core/crypto/crypto_utils.h"
#include "net/third_party/quic/core/quic_connection.h"
#include "net/third_party/quic/core/quic_session.h"
#include "net/third_party/quic/core/quic_utils.h"
#include "net/third_party/quic/platform/api/quic_flag_utils.h"
#include "net/third_party/quic/platform/api/quic_flags.h"
#include "net/third_party/quic/platform/api/quic_logging.h"
#include "net/third_party/quic/platform/api/quic_string.h"
#include "net/third_party/quic/platform/api/quic_string_piece.h"

namespace quic {

#define ENDPOINT                                                   \
  (session()->perspective() == Perspective::IS_SERVER ? "Server: " \
                                                      : "Client:"  \
                                                        " ")

QuicCryptoStream::QuicCryptoStream(QuicSession* session)
    : QuicStream(QuicUtils::GetCryptoStreamId(
                     session->connection()->transport_version()),
                 session,
                 /*is_static=*/true,
                 BIDIRECTIONAL),
      substreams_{{this, ENCRYPTION_NONE},
                  {this, ENCRYPTION_ZERO_RTT},
                  {this, ENCRYPTION_FORWARD_SECURE}} {
  // The crypto stream is exempt from connection level flow control.
  DisableConnectionFlowControlForThisStream();
}

QuicCryptoStream::~QuicCryptoStream() {}

// static
QuicByteCount QuicCryptoStream::CryptoMessageFramingOverhead(
    QuicTransportVersion version) {
  return QuicPacketCreator::StreamFramePacketOverhead(
      version, PACKET_8BYTE_CONNECTION_ID, PACKET_0BYTE_CONNECTION_ID,
      /*include_version=*/true,
      /*include_diversification_nonce=*/true,
      version > QUIC_VERSION_43 ? PACKET_4BYTE_PACKET_NUMBER
                                : PACKET_1BYTE_PACKET_NUMBER,
      VARIABLE_LENGTH_INTEGER_LENGTH_1, VARIABLE_LENGTH_INTEGER_LENGTH_2,
      /*offset=*/0);
}

void QuicCryptoStream::OnCryptoFrame(const QuicCryptoFrame& frame) {
  QUIC_BUG_IF(session()->connection()->transport_version() < QUIC_VERSION_47)
      << "Versions less than 47 shouldn't receive CRYPTO frames";
  EncryptionLevel level = session()->connection()->last_decrypted_level();
  substreams_[level].sequencer.OnCryptoFrame(frame);
}

void QuicCryptoStream::OnStreamFrame(const QuicStreamFrame& frame) {
  if (session()->connection()->transport_version() >= QUIC_VERSION_47) {
    QUIC_PEER_BUG
        << "Crypto data received in stream frame instead of crypto frame";
    CloseConnectionWithDetails(QUIC_INVALID_STREAM_DATA,
                               "Unexpected stream frame");
  }
  QuicStream::OnStreamFrame(frame);
}

void QuicCryptoStream::OnDataAvailable() {
  EncryptionLevel level = session()->connection()->last_decrypted_level();
  if (session()->connection()->transport_version() < QUIC_VERSION_47) {
    // Versions less than 47 only support QUIC crypto, which ignores the
    // EncryptionLevel passed into CryptoMessageParser::ProcessInput (and
    // OnDataAvailableInSequencer).
    OnDataAvailableInSequencer(sequencer(), level);
    return;
  }
  OnDataAvailableInSequencer(&substreams_[level].sequencer, level);
}

void QuicCryptoStream::OnDataAvailableInSequencer(
    QuicStreamSequencer* sequencer,
    EncryptionLevel level) {
  struct iovec iov;
  while (sequencer->GetReadableRegion(&iov)) {
    QuicStringPiece data(static_cast<char*>(iov.iov_base), iov.iov_len);
    if (!crypto_message_parser()->ProcessInput(data, level)) {
      CloseConnectionWithDetails(crypto_message_parser()->error(),
                                 crypto_message_parser()->error_detail());
      return;
    }
    sequencer->MarkConsumed(iov.iov_len);
    if (handshake_confirmed() &&
        crypto_message_parser()->InputBytesRemaining() == 0) {
      // If the handshake is complete and the current message has been fully
      // processed then no more handshake messages are likely to arrive soon
      // so release the memory in the stream sequencer.
      sequencer->ReleaseBufferIfEmpty();
    }
  }
}

bool QuicCryptoStream::ExportKeyingMaterial(QuicStringPiece label,
                                            QuicStringPiece context,
                                            size_t result_len,
                                            QuicString* result) const {
  if (!handshake_confirmed()) {
    QUIC_DLOG(ERROR) << "ExportKeyingMaterial was called before forward-secure"
                     << "encryption was established.";
    return false;
  }
  return CryptoUtils::ExportKeyingMaterial(
      crypto_negotiated_params().subkey_secret, label, context, result_len,
      result);
}

void QuicCryptoStream::WriteCryptoData(EncryptionLevel level,
                                       QuicStringPiece data) {
  if (session()->connection()->transport_version() < QUIC_VERSION_47) {
    // The QUIC crypto handshake takes care of setting the appropriate
    // encryption level before writing data. Since that is the only handshake
    // supported in versions less than 47, |level| can be ignored here.
    WriteOrBufferData(data, /* fin */ false, /* ack_listener */ nullptr);
    return;
  }
  if (data.empty()) {
    QUIC_BUG << "Empty crypto data being written";
    return;
  }
  // Append |data| to the send buffer for this encryption level.
  struct iovec iov(QuicUtils::MakeIovec(data));
  QuicStreamSendBuffer* send_buffer = &substreams_[level].send_buffer;
  QuicStreamOffset offset = send_buffer->stream_offset();
  send_buffer->SaveStreamData(&iov, /*iov_count=*/1, /*iov_offset=*/0,
                              data.length());
  if (kMaxStreamLength - offset < data.length()) {
    QUIC_BUG << "Writing too much crypto handshake data";
    // TODO(nharper): Switch this to an IETF QUIC error code, possibly
    // INTERNAL_ERROR?
    CloseConnectionWithDetails(QUIC_STREAM_LENGTH_OVERFLOW,
                               "Writing too much crypto handshake data");
  }

  // Set long header type based on the encryption level.
  if (level != ENCRYPTION_FORWARD_SECURE) {
    QuicStreamOffset fake_offset = level == ENCRYPTION_NONE ? 0 : 1;
    // Implementations of GetLongHeaderType either don't care at all about the
    // offset, or only care whether or not it's 0. However, they do care that it
    // is an absolute offset from the start of unencrypted crypto data, not the
    // offset at a particular encryption level.
    QuicLongHeaderType type = GetLongHeaderType(fake_offset);
    session()->connection()->SetLongHeaderType(type);
  }
  EncryptionLevel current_level = session()->connection()->encryption_level();
  session()->connection()->SetDefaultEncryptionLevel(level);
  size_t bytes_consumed =
      session()->connection()->SendCryptoData(level, data.length(), offset);
  session()->connection()->SetDefaultEncryptionLevel(current_level);

  send_buffer->OnStreamDataConsumed(bytes_consumed);
}

void QuicCryptoStream::OnSuccessfulVersionNegotiation(
    const ParsedQuicVersion& version) {}

bool QuicCryptoStream::OnCryptoFrameAcked(const QuicCryptoFrame& frame,
                                          QuicTime::Delta ack_delay_time) {
  QuicByteCount newly_acked_length = 0;
  if (!substreams_[frame.level].send_buffer.OnStreamDataAcked(
          frame.offset, frame.data_length, &newly_acked_length)) {
    CloseConnectionWithDetails(QUIC_INTERNAL_ERROR,
                               "Trying to ack unsent crypto data.");
    return false;
  }
  return newly_acked_length > 0;
}

void QuicCryptoStream::NeuterUnencryptedStreamData() {
  if (session()->connection()->transport_version() < QUIC_VERSION_47) {
    for (const auto& interval : bytes_consumed_[ENCRYPTION_NONE]) {
      QuicByteCount newly_acked_length = 0;
      send_buffer().OnStreamDataAcked(
          interval.min(), interval.max() - interval.min(), &newly_acked_length);
    }
    return;
  }
  QuicStreamSendBuffer* send_buffer = &substreams_[ENCRYPTION_NONE].send_buffer;
  // TODO(nharper): Consider adding a Clear() method to QuicStreamSendBuffer to
  // replace the following code.
  QuicIntervalSet<QuicStreamOffset> to_ack = send_buffer->bytes_acked();
  to_ack.Complement(0, send_buffer->stream_offset());
  for (const auto& interval : to_ack) {
    QuicByteCount newly_acked_length = 0;
    send_buffer->OnStreamDataAcked(
        interval.min(), interval.max() - interval.min(), &newly_acked_length);
  }
}

void QuicCryptoStream::OnStreamDataConsumed(size_t bytes_consumed) {
  if (session()->connection()->transport_version() >= QUIC_VERSION_47) {
    QUIC_BUG << "Stream data consumed when CRYPTO frames should be in use";
  }
  if (bytes_consumed > 0) {
    bytes_consumed_[session()->connection()->encryption_level()].Add(
        stream_bytes_written(), stream_bytes_written() + bytes_consumed);
  }
  QuicStream::OnStreamDataConsumed(bytes_consumed);
}

bool QuicCryptoStream::HasPendingCryptoRetransmission() {
  if (session()->connection()->transport_version() < QUIC_VERSION_47) {
    return false;
  }
  for (EncryptionLevel level :
       {ENCRYPTION_NONE, ENCRYPTION_ZERO_RTT, ENCRYPTION_FORWARD_SECURE}) {
    if (substreams_[level].send_buffer.HasPendingRetransmission()) {
      return true;
    }
  }
  return false;
}

void QuicCryptoStream::WritePendingCryptoRetransmission() {
  QUIC_BUG_IF(session()->connection()->transport_version() < QUIC_VERSION_47)
      << "Versions less than 47 don't write CRYPTO frames";
  EncryptionLevel current_encryption_level =
      session()->connection()->encryption_level();
  for (EncryptionLevel level :
       {ENCRYPTION_NONE, ENCRYPTION_ZERO_RTT, ENCRYPTION_FORWARD_SECURE}) {
    QuicStreamSendBuffer* send_buffer = &substreams_[level].send_buffer;
    session()->connection()->SetDefaultEncryptionLevel(level);
    while (send_buffer->HasPendingRetransmission()) {
      auto pending = send_buffer->NextPendingRetransmission();
      size_t bytes_consumed = session()->connection()->SendCryptoData(
          level, pending.length, pending.offset);
      send_buffer->OnStreamDataRetransmitted(pending.offset, bytes_consumed);
    }
  }
  session()->connection()->SetDefaultEncryptionLevel(current_encryption_level);
}

void QuicCryptoStream::WritePendingRetransmission() {
  while (HasPendingRetransmission()) {
    StreamPendingRetransmission pending =
        send_buffer().NextPendingRetransmission();
    QuicIntervalSet<QuicStreamOffset> retransmission(
        pending.offset, pending.offset + pending.length);
    EncryptionLevel retransmission_encryption_level = ENCRYPTION_NONE;
    // Determine the encryption level to write the retransmission
    // at. The retransmission should be written at the same encryption level
    // as the original transmission.
    for (size_t i = 0; i < NUM_ENCRYPTION_LEVELS; ++i) {
      if (retransmission.Intersects(bytes_consumed_[i])) {
        retransmission_encryption_level = static_cast<EncryptionLevel>(i);
        retransmission.Intersection(bytes_consumed_[i]);
        break;
      }
    }
    pending.offset = retransmission.begin()->min();
    pending.length =
        retransmission.begin()->max() - retransmission.begin()->min();
    EncryptionLevel current_encryption_level =
        session()->connection()->encryption_level();
    // Set appropriate encryption level.
    session()->connection()->SetDefaultEncryptionLevel(
        retransmission_encryption_level);
    QuicConsumedData consumed = session()->WritevData(
        this, id(), pending.length, pending.offset, NO_FIN);
    QUIC_DVLOG(1) << ENDPOINT << "stream " << id()
                  << " tries to retransmit stream data [" << pending.offset
                  << ", " << pending.offset + pending.length
                  << ") with encryption level: "
                  << retransmission_encryption_level
                  << ", consumed: " << consumed;
    OnStreamFrameRetransmitted(pending.offset, consumed.bytes_consumed,
                               consumed.fin_consumed);
    // Restore encryption level.
    session()->connection()->SetDefaultEncryptionLevel(
        current_encryption_level);
    if (consumed.bytes_consumed < pending.length) {
      // The connection is write blocked.
      break;
    }
  }
}

bool QuicCryptoStream::RetransmitStreamData(QuicStreamOffset offset,
                                            QuicByteCount data_length,
                                            bool /*fin*/) {
  QuicIntervalSet<QuicStreamOffset> retransmission(offset,
                                                   offset + data_length);
  // Determine the encryption level to send data. This only needs to be once as
  // [offset, offset + data_length) is guaranteed to be in the same packet.
  EncryptionLevel send_encryption_level = ENCRYPTION_NONE;
  for (size_t i = 0; i < NUM_ENCRYPTION_LEVELS; ++i) {
    if (retransmission.Intersects(bytes_consumed_[i])) {
      send_encryption_level = static_cast<EncryptionLevel>(i);
      break;
    }
  }
  retransmission.Difference(bytes_acked());
  EncryptionLevel current_encryption_level =
      session()->connection()->encryption_level();
  for (const auto& interval : retransmission) {
    QuicStreamOffset retransmission_offset = interval.min();
    QuicByteCount retransmission_length = interval.max() - interval.min();
    // Set appropriate encryption level.
    session()->connection()->SetDefaultEncryptionLevel(send_encryption_level);
    QuicConsumedData consumed = session()->WritevData(
        this, id(), retransmission_length, retransmission_offset, NO_FIN);
    QUIC_DVLOG(1) << ENDPOINT << "stream " << id()
                  << " is forced to retransmit stream data ["
                  << retransmission_offset << ", "
                  << retransmission_offset + retransmission_length
                  << "), with encryption level: " << send_encryption_level
                  << ", consumed: " << consumed;
    OnStreamFrameRetransmitted(retransmission_offset, consumed.bytes_consumed,
                               consumed.fin_consumed);
    // Restore encryption level.
    session()->connection()->SetDefaultEncryptionLevel(
        current_encryption_level);
    if (consumed.bytes_consumed < retransmission_length) {
      // The connection is write blocked.
      return false;
    }
  }

  return true;
}

uint64_t QuicCryptoStream::crypto_bytes_read() const {
  if (session()->connection()->transport_version() < QUIC_VERSION_47) {
    return stream_bytes_read();
  }
  return substreams_[ENCRYPTION_NONE].sequencer.NumBytesConsumed() +
         substreams_[ENCRYPTION_ZERO_RTT].sequencer.NumBytesConsumed() +
         substreams_[ENCRYPTION_FORWARD_SECURE].sequencer.NumBytesConsumed();
}

uint64_t QuicCryptoStream::BytesReadOnLevel(EncryptionLevel level) const {
  return substreams_[level].sequencer.NumBytesConsumed();
}

bool QuicCryptoStream::WriteCryptoFrame(EncryptionLevel level,
                                        QuicStreamOffset offset,
                                        QuicByteCount data_length,
                                        QuicDataWriter* writer) {
  QUIC_BUG_IF(session()->connection()->transport_version() < QUIC_VERSION_47)
      << "Versions less than 47 don't write CRYPTO frames (2)";
  return substreams_[level].send_buffer.WriteStreamData(offset, data_length,
                                                        writer);
}

void QuicCryptoStream::OnCryptoFrameLost(QuicCryptoFrame* crypto_frame) {
  QUIC_BUG_IF(session()->connection()->transport_version() < QUIC_VERSION_47)
      << "Versions less than 47 don't lose CRYPTO frames";
  substreams_[crypto_frame->level].send_buffer.OnStreamDataLost(
      crypto_frame->offset, crypto_frame->data_length);
}

void QuicCryptoStream::RetransmitData(QuicCryptoFrame* crypto_frame) {
  QUIC_BUG_IF(session()->connection()->transport_version() < QUIC_VERSION_47)
      << "Versions less than 47 don't retransmit CRYPTO frames";
  QuicIntervalSet<QuicStreamOffset> retransmission(
      crypto_frame->offset, crypto_frame->offset + crypto_frame->data_length);
  QuicStreamSendBuffer* send_buffer =
      &substreams_[crypto_frame->level].send_buffer;
  retransmission.Difference(send_buffer->bytes_acked());
  if (retransmission.Empty()) {
    return;
  }
  EncryptionLevel current_encryption_level =
      session()->connection()->encryption_level();
  for (const auto& interval : retransmission) {
    size_t retransmission_offset = interval.min();
    size_t retransmission_length = interval.max() - interval.min();
    session()->connection()->SetDefaultEncryptionLevel(crypto_frame->level);
    size_t bytes_consumed = session()->connection()->SendCryptoData(
        crypto_frame->level, retransmission_length, retransmission_offset);
    send_buffer->OnStreamDataRetransmitted(retransmission_offset,
                                           bytes_consumed);
  }
  session()->connection()->SetDefaultEncryptionLevel(current_encryption_level);
}

bool QuicCryptoStream::IsFrameOutstanding(EncryptionLevel level,
                                          size_t offset,
                                          size_t length) const {
  if (session()->connection()->transport_version() < QUIC_VERSION_47) {
    // This only happens if a client was originally configured for a version
    // greater than 45, but received a version negotiation packet and is
    // attempting to retransmit for a version less than 47. Outside of tests,
    // this is a misconfiguration of the client, and this connection will be
    // doomed. Return false here to avoid trying to retransmit CRYPTO frames on
    // the wrong transport version.
    return false;
  }
  return substreams_[level].send_buffer.IsStreamDataOutstanding(offset, length);
}

bool QuicCryptoStream::IsWaitingForAcks() const {
  if (session()->connection()->transport_version() < QUIC_VERSION_47) {
    return QuicStream::IsWaitingForAcks();
  }
  for (EncryptionLevel level :
       {ENCRYPTION_NONE, ENCRYPTION_ZERO_RTT, ENCRYPTION_FORWARD_SECURE}) {
    if (substreams_[level].send_buffer.stream_bytes_outstanding()) {
      return true;
    }
  }
  return false;
}

QuicCryptoStream::CryptoSubstream::CryptoSubstream(
    QuicCryptoStream* crypto_stream,
    EncryptionLevel)
    : sequencer(crypto_stream),
      send_buffer(crypto_stream->session()
                      ->connection()
                      ->helper()
                      ->GetStreamSendBufferAllocator()) {}

#undef ENDPOINT  // undef for jumbo builds
}  // namespace quic
