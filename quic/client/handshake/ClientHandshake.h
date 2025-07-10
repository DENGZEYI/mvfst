/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/ExceptionWrapper.h>
#include <folly/io/IOBufQueue.h>
#include <folly/io/async/AsyncTransportCertificate.h>
#include <folly/io/async/DelayedDestruction.h>

#include <quic/QuicConstants.h>
#include <quic/QuicException.h>
#include <quic/common/Expected.h>
#include <quic/handshake/Aead.h>
#include <quic/handshake/HandshakeLayer.h>

namespace quic {

class CryptoFactory;
struct CachedServerTransportParameters;
struct ClientTransportParametersExtension;
struct QuicClientConnectionState;
struct ServerTransportParameters;

class ClientHandshake : public Handshake {
 public:
  enum class Phase { Initial, Handshake, OneRttKeysDerived, Established };

  explicit ClientHandshake(QuicClientConnectionState* conn);

  /**
   * Initiate the handshake with the supplied parameters.
   */
  [[nodiscard]] quic::Expected<void, QuicError> connect(
      Optional<std::string> hostname,
      std::shared_ptr<ClientTransportParametersExtension> transportParams);

  /**
   * Takes input bytes from the network and processes then in the handshake.
   * This can change the state of the transport which may result in ciphers
   * being initialized, bytes written out, or the write phase changing.
   */
  [[nodiscard]] virtual quic::Expected<void, QuicError> doHandshake(
      BufPtr data,
      EncryptionLevel encryptionLevel);

  /**
   * Provides facilities to get, put and remove a PSK from the cache in case the
   * handshake supports a PSK cache.
   */
  virtual void removePsk(const Optional<std::string>& /* hostname */) {}

  /**
   * Returns a reference to the CryptoFactory used internally.
   */
  virtual const CryptoFactory& getCryptoFactory() const = 0;
  /**
   * An API to get oneRttWriteCiphers on key rotation. Each call will return a
   * one rtt write cipher using the current traffic secret and advance the
   * traffic secret.
   */
  [[nodiscard]] quic::Expected<std::unique_ptr<Aead>, QuicError>
  getNextOneRttWriteCipher() override;

  /**
   * An API to get oneRttReadCiphers on key rotation. Each call will return a
   * one rtt read cipher using the current traffic secret and advance the
   * traffic secret.
   */
  [[nodiscard]] quic::Expected<std::unique_ptr<Aead>, QuicError>
  getNextOneRttReadCipher() override;

  /**
   * Triggered when we have received a handshake done frame from the server.
   */
  void handshakeConfirmed() override;

  Phase getPhase() const;

  /**
   * Was the TLS connection resumed or not.
   */
  virtual bool isTLSResumed() const = 0;

  /**
   * Edge triggered api to obtain whether or not zero rtt data was rejected.
   * If zero rtt was never attempted, then this will return std::nullopt. Once
   * the result is obtained, the result is cleared out.
   */
  Optional<bool> getZeroRttRejected();

  /**
   * If zero-rtt is rejected, this will indicate whether zero-rtt data can be
   * resent on the connection or the connection has to be closed.
   */
  Optional<bool> getCanResendZeroRtt() const;

  /**
   * API used to verify that the integrity token present in the retry packet
   * matches what we would expect
   */
  [[nodiscard]] virtual quic::Expected<bool, QuicError> verifyRetryIntegrityTag(
      const ConnectionId& originalDstConnId,
      const RetryPacket& retryPacket) = 0;

  /**
   * Returns the negotiated transport parameters chosen by the server
   */
  virtual const Optional<ServerTransportParameters>& getServerTransportParams();

  virtual int64_t getCertificateVerifyStartTimeMS() const {
    return 0;
  }

  virtual int64_t getCertificateVerifyEndTimeMS() const {
    return 0;
  }

  virtual std::optional<int32_t> getHandshakeStatus() const {
    return std::nullopt;
  }

  size_t getInitialReadBufferSize() const;
  size_t getHandshakeReadBufferSize() const;
  size_t getAppDataReadBufferSize() const;

  virtual EncryptionLevel getReadRecordLayerEncryptionLevel() = 0;

  bool waitingForData() const;

  virtual const std::shared_ptr<const folly::AsyncTransportCertificate>
  getPeerCertificate() const = 0;

  ~ClientHandshake() override = default;

 protected:
  enum class CipherKind {
    HandshakeWrite,
    HandshakeRead,
    OneRttWrite,
    OneRttRead,
    ZeroRttWrite,
  };

  void computeCiphers(CipherKind kind, ByteRange secret);

  /**
   * Various utilities for concrete implementations to use.
   */
  void waitForData();
  void writeDataToStream(EncryptionLevel encryptionLevel, BufPtr data);
  void handshakeInitiated();
  void computeZeroRttCipher();
  void computeOneRttCipher(bool earlyDataAccepted);
  void setError(QuicError error);

  /**
   * Accessor for the concrete implementation, so they can access data without
   * being able to rebind it.
   */
  QuicClientConnectionState* getClientConn();
  const QuicClientConnectionState* getClientConn() const;
  const std::shared_ptr<ClientTransportParametersExtension>&
  getClientTransportParameters() const;

  /**
   * Setters for the concrete implementation so that it can be tested.
   */
  void setZeroRttRejectedForTest(bool rejected);
  void setCanResendZeroRttForTest(bool canResendZeroRtt);

  /**
   * Given secret_n, returns secret_n+1 to be used for generating the next Aead
   * on key updates.
   */
  [[nodiscard]] virtual quic::Expected<BufPtr, QuicError> getNextTrafficSecret(
      ByteRange secret) const = 0;

  BufPtr readTrafficSecret_;
  BufPtr writeTrafficSecret_;

  Optional<bool> zeroRttRejected_;
  Optional<bool> canResendZeroRtt_;

 private:
  [[nodiscard]] virtual quic::
      Expected<Optional<CachedServerTransportParameters>, QuicError>
      connectImpl(Optional<std::string> hostname) = 0;

  virtual void processSocketData(folly::IOBufQueue& queue) = 0;
  virtual bool matchEarlyParameters() = 0;
  [[nodiscard]] virtual quic::Expected<std::unique_ptr<Aead>, QuicError>
  buildAead(CipherKind kind, ByteRange secret) = 0;
  [[nodiscard]] virtual quic::
      Expected<std::unique_ptr<PacketNumberCipher>, QuicError>
      buildHeaderCipher(ByteRange secret) = 0;

  // Represents the packet type that should be used to write the data currently
  // in the stream.
  Phase phase_{Phase::Initial};

  QuicClientConnectionState* conn_;
  std::shared_ptr<ClientTransportParametersExtension> transportParams_;

  bool waitForData_{false};
  bool earlyDataAttempted_{false};

  folly::IOBufQueue initialReadBuf_{folly::IOBufQueue::cacheChainLength()};
  folly::IOBufQueue handshakeReadBuf_{folly::IOBufQueue::cacheChainLength()};
  folly::IOBufQueue appDataReadBuf_{folly::IOBufQueue::cacheChainLength()};

  // This variable is incremented every time a read traffic secret is rotated,
  // and decremented for the write secret. Its value should be
  // between -1 and 1. A value outside of this range indicates that the
  // transport's read and write ciphers are likely out of sync.
  int8_t trafficSecretSync_{0};

  quic::Expected<void, QuicError> error_{};
};

} // namespace quic
