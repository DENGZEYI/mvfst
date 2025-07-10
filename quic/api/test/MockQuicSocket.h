/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/portability/GMock.h>

#include <quic/api/QuicSocket.h>
#include <quic/dsr/Types.h>

namespace quic {

class MockQuicSocket : public QuicSocket {
 public:
  using SharedBuf = std::shared_ptr<folly::IOBuf>;

  MockQuicSocket() = default;

  MockQuicSocket(
      folly::EventBase* /*eventBase*/,
      ConnectionSetupCallback* setupCb,
      ConnectionCallback* connCb)
      : setupCb_(setupCb), connCb_(connCb) {}

  MOCK_METHOD(bool, good, (), (const));
  MOCK_METHOD(bool, replaySafe, (), (const));
  MOCK_METHOD(bool, error, (), (const));
  MOCK_METHOD(void, close, (Optional<QuicError>));
  MOCK_METHOD(void, closeGracefully, ());
  MOCK_METHOD(void, closeNow, (Optional<QuicError>));
  MOCK_METHOD(Optional<quic::ConnectionId>, getClientConnectionId, (), (const));
  MOCK_METHOD(const TransportSettings&, getTransportSettings, (), (const));
  MOCK_METHOD(Optional<quic::ConnectionId>, getServerConnectionId, (), (const));
  MOCK_METHOD(
      Optional<quic::ConnectionId>,
      getClientChosenDestConnectionId,
      (),
      (const));
  MOCK_METHOD(const folly::SocketAddress&, getPeerAddress, (), (const));
  MOCK_METHOD(const folly::SocketAddress&, getOriginalPeerAddress, (), (const));
  MOCK_METHOD(const folly::SocketAddress&, getLocalAddress, (), (const));
  MOCK_METHOD(
      Optional<std::vector<TransportParameter>>,
      getPeerTransportParams,
      (),
      (const));
  MOCK_METHOD(
      (Optional<std::vector<uint8_t>>),
      getExportedKeyingMaterial,
      (const std::string&, const Optional<ByteRange>&, uint16_t),
      (const));
  MOCK_METHOD(std::shared_ptr<QuicEventBase>, getEventBase, (), (const));
  MOCK_METHOD(
      (quic::Expected<size_t, LocalErrorCode>),
      getStreamReadOffset,
      (StreamId),
      (const));
  MOCK_METHOD(
      (quic::Expected<size_t, LocalErrorCode>),
      getStreamWriteOffset,
      (StreamId),
      (const));
  MOCK_METHOD(
      (quic::Expected<size_t, LocalErrorCode>),
      getStreamWriteBufferedBytes,
      (StreamId),
      (const));
  MOCK_METHOD(QuicSocket::TransportInfo, getTransportInfo, (), (const));
  MOCK_METHOD(
      (quic::Expected<QuicSocket::StreamTransportInfo, LocalErrorCode>),
      getStreamTransportInfo,
      (StreamId),
      (const));
  MOCK_METHOD(Optional<std::string>, getAppProtocol, (), (const));
  MOCK_METHOD(void, setReceiveWindow, (StreamId, size_t));
  MOCK_METHOD(void, setSendBuffer, (StreamId, size_t, size_t));
  MOCK_METHOD(uint64_t, getConnectionBufferAvailable, (), (const));
  MOCK_METHOD(
      (quic::Expected<FlowControlState, LocalErrorCode>),
      getConnectionFlowControl,
      (),
      (const));
  MOCK_METHOD(
      (quic::Expected<FlowControlState, LocalErrorCode>),
      getStreamFlowControl,
      (StreamId),
      (const));
  MOCK_METHOD(
      (quic::Expected<uint64_t, LocalErrorCode>),
      getMaxWritableOnStream,
      (StreamId),
      (const));
  MOCK_METHOD(void, unsetAllReadCallbacks, ());
  MOCK_METHOD(void, unsetAllPeekCallbacks, ());
  MOCK_METHOD(void, unsetAllDeliveryCallbacks, ());
  MOCK_METHOD(void, cancelDeliveryCallbacksForStream, (StreamId));
  MOCK_METHOD(
      void,
      cancelDeliveryCallbacksForStream,
      (StreamId, uint64_t offset));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      setConnectionFlowControlWindow,
      (uint64_t));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      setStreamFlowControlWindow,
      (StreamId, uint64_t));
  MOCK_METHOD(void, setTransportSettings, (TransportSettings));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      setMaxPacingRate,
      (uint64_t));

  quic::Expected<void, LocalErrorCode>
  setKnob(uint64_t knobSpace, uint64_t knobId, BufPtr knobBlob) override {
    SharedBuf sharedBlob(knobBlob.release());
    return setKnob(knobSpace, knobId, sharedBlob);
  }

  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      setKnob,
      (uint64_t, uint64_t, SharedBuf));
  MOCK_METHOD(bool, isKnobSupported, (), (const));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      setStreamPriority,
      (StreamId, PriorityQueue::Priority));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      setPriorityQueue,
      (std::unique_ptr<PriorityQueue> queue));
  MOCK_METHOD(
      (quic::Expected<PriorityQueue::Priority, LocalErrorCode>),
      getStreamPriority,
      (StreamId));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      setReadCallback,
      (StreamId, ReadCallback*, Optional<ApplicationErrorCode> err));
  MOCK_METHOD(
      void,
      setConnectionSetupCallback,
      (folly::MaybeManagedPtr<ConnectionSetupCallback>));
  MOCK_METHOD(
      void,
      setConnectionCallback,
      (folly::MaybeManagedPtr<ConnectionCallback>));

  void setEarlyDataAppParamsFunctions(
      std::function<bool(const Optional<std::string>&, const BufPtr&)>
          validator,
      std::function<BufPtr()> getter) override {
    earlyDataAppParamsValidator_ = std::move(validator);
    earlyDataAppParamsGetter_ = std::move(getter);
  }

  MOCK_METHOD((quic::Expected<void, LocalErrorCode>), pauseRead, (StreamId));
  MOCK_METHOD((quic::Expected<void, LocalErrorCode>), resumeRead, (StreamId));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      stopSending,
      (StreamId, ApplicationErrorCode));

  quic::Expected<std::pair<BufPtr, bool>, LocalErrorCode> read(
      StreamId id,
      size_t maxRead) override {
    auto res = readNaked(id, maxRead);
    if (res.hasError()) {
      return quic::make_unexpected(res.error());
    } else {
      return std::pair<BufPtr, bool>(
          BufPtr(res.value().first), res.value().second);
    }
  }

  using ReadResult =
      quic::Expected<std::pair<folly::IOBuf*, bool>, LocalErrorCode>;
  MOCK_METHOD(ReadResult, readNaked, (StreamId, size_t));
  MOCK_METHOD(
      (quic::Expected<StreamId, LocalErrorCode>),
      createBidirectionalStream,
      (bool));
  MOCK_METHOD(
      (quic::Expected<StreamId, LocalErrorCode>),
      createUnidirectionalStream,
      (bool));
  MOCK_METHOD(uint64_t, getNumOpenableBidirectionalStreams, (), (const));
  MOCK_METHOD(uint64_t, getNumOpenableUnidirectionalStreams, (), (const));
  MOCK_METHOD((bool), isClientStream, (StreamId), (noexcept));
  MOCK_METHOD((bool), isServerStream, (StreamId), (noexcept));
  MOCK_METHOD((StreamInitiator), getStreamInitiator, (StreamId), (noexcept));
  MOCK_METHOD((bool), isBidirectionalStream, (StreamId), (noexcept));
  MOCK_METHOD((bool), isUnidirectionalStream, (StreamId), (noexcept));
  MOCK_METHOD(
      (StreamDirectionality),
      getStreamDirectionality,
      (StreamId),
      (noexcept));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      notifyPendingWriteOnConnection,
      (ConnectionWriteCallback*));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      notifyPendingWriteOnStream,
      (StreamId, StreamWriteCallback*));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      unregisterStreamWriteCallback,
      (StreamId));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      registerTxCallback,
      (const StreamId, const uint64_t, ByteEventCallback*));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      registerByteEventCallback,
      (const ByteEvent::Type,
       const StreamId,
       const uint64_t,
       ByteEventCallback*));
  MOCK_METHOD(
      void,
      cancelByteEventCallbacksForStream,
      (const StreamId id, const Optional<uint64_t>& offset));
  MOCK_METHOD(
      void,
      cancelByteEventCallbacksForStream,
      (const ByteEvent::Type,
       const StreamId id,
       const Optional<uint64_t>& offset));
  MOCK_METHOD(void, cancelAllByteEventCallbacks, ());
  MOCK_METHOD(void, cancelByteEventCallbacks, (const ByteEvent::Type));
  MOCK_METHOD(
      size_t,
      getNumByteEventCallbacksForStream,
      (const StreamId id),
      (const));
  MOCK_METHOD(
      size_t,
      getNumByteEventCallbacksForStream,
      (const ByteEvent::Type, const StreamId),
      (const));

  quic::Expected<void, LocalErrorCode> writeChain(
      StreamId id,
      BufPtr data,
      bool eof,
      ByteEventCallback* cb) override {
    SharedBuf sharedData(data.release());
    return writeChain(id, sharedData, eof, cb);
  }

  MOCK_METHOD(
      WriteResult,
      writeChain,
      (StreamId, SharedBuf, bool, ByteEventCallback*));
  MOCK_METHOD(
      WriteResult,
      writeBufMeta,
      (StreamId, const BufferMeta&, bool, ByteEventCallback*));
  MOCK_METHOD(
      WriteResult,
      setDSRPacketizationRequestSender,
      (StreamId, std::unique_ptr<DSRPacketizationRequestSender>));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      registerDeliveryCallback,
      (StreamId, uint64_t, ByteEventCallback*));
  MOCK_METHOD(Optional<LocalErrorCode>, shutdownWrite, (StreamId));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      resetStream,
      (StreamId, ApplicationErrorCode));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      updateReliableDeliveryCheckpoint,
      (StreamId));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      resetStreamReliably,
      (StreamId, ApplicationErrorCode));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      maybeResetStreamFromReadError,
      (StreamId, QuicErrorCode));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      setPingCallback,
      (PingCallback*));
  MOCK_METHOD(void, sendPing, (std::chrono::milliseconds));
  MOCK_METHOD(const QuicConnectionStateBase*, getState, (), (const));
  MOCK_METHOD(bool, isDetachable, ());
  MOCK_METHOD(void, attachEventBase, (std::shared_ptr<QuicEventBase>));
  MOCK_METHOD(void, detachEventBase, ());
  MOCK_METHOD(Optional<LocalErrorCode>, setControlStream, (StreamId));

  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      setPeekCallback,
      (StreamId, PeekCallback*));

  MOCK_METHOD((quic::Expected<void, LocalErrorCode>), pausePeek, (StreamId));
  MOCK_METHOD((quic::Expected<void, LocalErrorCode>), resumePeek, (StreamId));

  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      peek,
      (StreamId,
       const std::function<
           void(StreamId, const folly::Range<PeekIterator>&)>&));

  MOCK_METHOD(
      (quic::Expected<void, std::pair<LocalErrorCode, Optional<uint64_t>>>),
      consume,
      (StreamId, uint64_t, size_t));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      consume,
      (StreamId, size_t));

  MOCK_METHOD(void, setCongestionControl, (CongestionControlType));

  MOCK_METHOD(void, addPacketProcessor, (std::shared_ptr<PacketProcessor>));

  MOCK_METHOD(
      void,
      setThrottlingSignalProvider,
      (std::shared_ptr<ThrottlingSignalProvider>));

  ConnectionSetupCallback* setupCb_{nullptr};
  ConnectionCallback* connCb_{nullptr};

  std::function<bool(const Optional<std::string>&, const BufPtr&)>
      earlyDataAppParamsValidator_;
  std::function<BufPtr()> earlyDataAppParamsGetter_;

  MOCK_METHOD(
      void,
      resetNonControlStreams,
      (ApplicationErrorCode, folly::StringPiece));
  MOCK_METHOD(QuicConnectionStats, getConnectionsStats, (), (const));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      setDatagramCallback,
      (DatagramCallback*));
  MOCK_METHOD(uint16_t, getDatagramSizeLimit, (), (const));

  quic::Expected<void, LocalErrorCode> writeDatagram(BufPtr data) override {
    SharedBuf sharedData(data.release());
    return writeDatagram(sharedData);
  }

  MOCK_METHOD(WriteResult, writeDatagram, (SharedBuf));
  MOCK_METHOD(
      (quic::Expected<std::vector<ReadDatagram>, LocalErrorCode>),
      readDatagrams,
      (size_t));
  MOCK_METHOD(
      (quic::Expected<std::vector<BufPtr>, LocalErrorCode>),
      readDatagramBufs,
      (size_t));
  MOCK_METHOD(
      SocketObserverContainer*,
      getSocketObserverContainer,
      (),
      (const));
  MOCK_METHOD(
      (quic::Expected<StreamGroupId, LocalErrorCode>),
      createBidirectionalStreamGroup,
      ());
  MOCK_METHOD(
      (quic::Expected<StreamGroupId, LocalErrorCode>),
      createUnidirectionalStreamGroup,
      ());
  MOCK_METHOD(
      (quic::Expected<StreamId, LocalErrorCode>),
      createBidirectionalStreamInGroup,
      (StreamGroupId));
  MOCK_METHOD(
      (quic::Expected<StreamId, LocalErrorCode>),
      createUnidirectionalStreamInGroup,
      (StreamGroupId));
  MOCK_METHOD(
      (quic::Expected<void, LocalErrorCode>),
      setStreamGroupRetransmissionPolicy,
      (StreamGroupId, std::optional<QuicStreamGroupRetransmissionPolicy>),
      (noexcept));
  MOCK_METHOD(
      (const std::shared_ptr<const folly::AsyncTransportCertificate>),
      getPeerCertificate,
      (),
      (const));
  MOCK_METHOD((uint64_t), maxWritableOnConn, (), (const));
};
} // namespace quic
