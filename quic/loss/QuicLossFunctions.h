/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Chrono.h>
#include <quic/QuicConstants.h>
#include <quic/codec/Types.h>
#include <quic/common/Expected.h>
#include <quic/common/Optional.h>
#include <quic/common/TimeUtil.h>
#include <quic/congestion_control/CongestionController.h>
#include <quic/flowcontrol/QuicFlowController.h>
#include <quic/logging/QLoggerConstants.h>
#include <quic/observer/SocketObserverTypes.h>
#include <quic/state/QuicStateFunctions.h>
#include <quic/state/SimpleFrameFunctions.h>
#include <quic/state/StateData.h>

namespace quic {

// Forward-declaration
bool hasAckDataToWrite(const QuicConnectionStateBase& conn);
WriteDataReason hasNonAckDataToWrite(const QuicConnectionStateBase& conn);

std::chrono::microseconds calculatePTO(const QuicConnectionStateBase& conn);

/**
 * Whether conn is having persistent congestion.
 *
 * Persistent congestion requires a time period much longer than crypto timer.
 * This means no handshake packet should be in a persistent congestion range.
 * Thus persistent congestion is per pnSpace, and it's AppData space only.
 *
 */
bool isPersistentCongestion(
    OptionalMicros pto,
    TimePoint lostPeriodStart,
    TimePoint lostPeriodEnd,
    const CongestionController::AckEvent& ack) noexcept;

inline std::ostream& operator<<(
    std::ostream& os,
    const LossState::AlarmMethod& alarmMethod) {
  switch (alarmMethod) {
    case LossState::AlarmMethod::EarlyRetransmitOrReordering:
      os << "EarlyRetransmitOrReordering";
      break;
    case LossState::AlarmMethod::PTO:
      os << "PTO";
      break;
  }
  return os;
}

/**
 * Returns the absolute deadline at which the loss timer should fire,
 * together with the method that caused it.
 */
inline std::pair<TimePoint, LossState::AlarmMethod> computeLossTimerDeadline(
    const QuicConnectionStateBase& conn) {
  const auto lastSent = conn.lossState.lastRetransmittablePacketSentTime;
  if (auto [lossTime, _space] = earliestLossTimer(conn); lossTime) {
    // 1. Early–retransmit / reordering timer
    return {*lossTime, LossState::AlarmMethod::EarlyRetransmitOrReordering};
  } else {
    // 2. PTO timer.
    auto pto = calculatePTO(conn);
    pto *= 1ULL << std::min(conn.lossState.ptoCount, (uint32_t)31);
    return {lastSent + pto, LossState::AlarmMethod::PTO};
  }
}

/**
 * Returns the number of milliseconds still left until the loss timer
 * expires and the method used to set it.
 */
template <class ClockType = Clock>
std::pair<std::chrono::milliseconds, LossState::AlarmMethod>
calculateAlarmDuration(const QuicConnectionStateBase& conn) {
  auto [deadline, method] = computeLossTimerDeadline(conn);
  auto now = ClockType::now();
  auto remaining = deadline > now
      ? folly::chrono::ceil<std::chrono::milliseconds>(deadline - now)
      : std::chrono::milliseconds(0);
  return {remaining, method};
}

/*
 * This function should be invoked after some event that is possible to change
 * the loss detection timer, for example, write happened, timeout happened or
 * packets are acked.
 */
template <class Timeout, class ClockType = Clock>
void setLossDetectionAlarm(QuicConnectionStateBase& conn, Timeout& timeout) {
  /*
   * We might have new data or lost data to send even if we don't have any
   * outstanding packets. When we get a PTO event, it is possible that only
   * cloned packets might be outstanding. Since cwnd might be set to min cwnd,
   * we might not be able to send data. However we might still have data sitting
   * in the buffers which is unsent or known to be lost. We should set a timer
   * in this case to be able to send this data on the next PTO.
   */
  bool hasDataToWrite = hasAckDataToWrite(conn) ||
      (hasNonAckDataToWrite(conn) != WriteDataReason::NO_WRITE);
  auto totalPacketsOutstanding = conn.outstandings.numOutstanding();
  /*
   * We have this condition to disambiguate the case where we have.
   * (1) All outstanding packets that are clones that are processed and there is
   * no data to write. (2) All outstanding are clones that are processed and
   * there is data to write. If there are only clones with no data, then we
   * don't need to set the timer. This will free up the evb. However after a PTO
   * verified event, clones take up space in cwnd. If we have data left to
   * write, we would not be able to write them since we could be blocked by
   * cwnd. So we must set the loss timer so that we can write this data with the
   * slack packet space for the clones.
   */
  if (!hasDataToWrite && conn.outstandings.clonedPacketIdentifiers.empty() &&
      totalPacketsOutstanding == conn.outstandings.numClonedPackets()) {
    VLOG(10) << __func__ << " unset alarm pure ack or processed packets only"
             << " outstanding=" << totalPacketsOutstanding
             << " handshakePackets="
             << conn.outstandings.packetCount[PacketNumberSpace::Handshake]
             << " " << conn;
    conn.pendingEvents.setLossDetectionAlarm = false;
    timeout.cancelLossTimeout();
    return;
  }
  /**
   * Either previous timer or an Ack can clear the lossTime without setting a
   * new one, for example, if such timer or ack marks everything as loss, or
   * every as acked. In that case, if an early retransmit timer is already set,
   * we should clear it.
   */
  if (conn.lossState.currentAlarmMethod ==
          LossState::AlarmMethod::EarlyRetransmitOrReordering &&
      !earliestLossTimer(conn).first) {
    VLOG(10) << __func__
             << " unset alarm due to invalidated early retran timer";
    timeout.cancelLossTimeout();
  }
  if (!conn.pendingEvents.setLossDetectionAlarm) {
    VLOG_IF(10, !timeout.isLossTimeoutScheduled())
        << __func__ << " alarm not scheduled"
        << " outstanding=" << totalPacketsOutstanding << " initialPackets="
        << conn.outstandings.packetCount[PacketNumberSpace::Initial]
        << " handshakePackets="
        << conn.outstandings.packetCount[PacketNumberSpace::Handshake] << " "
        << nodeToString(conn.nodeType) << " " << conn;
    return;
  }
  timeout.cancelLossTimeout();
  auto alarmDuration = calculateAlarmDuration<ClockType>(conn);
  conn.lossState.currentAlarmMethod = alarmDuration.second;
  VLOG(10) << __func__ << " setting transmission"
           << " alarm=" << alarmDuration.first.count() << "ms"
           << " method=" << conn.lossState.currentAlarmMethod
           << " haDataToWrite=" << hasDataToWrite
           << " outstanding=" << totalPacketsOutstanding
           << " outstanding clone=" << conn.outstandings.numClonedPackets()
           << " clonedPacketIdentifiers="
           << conn.outstandings.clonedPacketIdentifiers.size()
           << " initialPackets="
           << conn.outstandings.packetCount[PacketNumberSpace::Initial]
           << " handshakePackets="
           << conn.outstandings.packetCount[PacketNumberSpace::Handshake] << " "
           << nodeToString(conn.nodeType) << " " << conn;
  timeout.scheduleLossTimeout(alarmDuration.first);
  conn.pendingEvents.setLossDetectionAlarm = false;
}

/**
 * Processes outstandings for loss.
 * Returns true if the loss timer should be set, false otherwise.
 * Returns QuicError if the LossVisitor fails.
 */
[[nodiscard]] quic::Expected<bool, QuicError> processOutstandingsForLoss(
    QuicConnectionStateBase& conn,
    PacketNum largestAcked,
    const PacketNumberSpace& pnSpace,
    const InlineMap<StreamId, PacketNum, 20>& largestDsrAckedSequenceNumber,
    const Optional<PacketNum>& largestNonDsrAckedSequenceNumber,
    const TimePoint& lossTime,
    const std::chrono::microseconds& rttSample,
    const LossVisitor& lossVisitor, // Visitor now returns Expected
    std::chrono::microseconds& delayUntilLost,
    CongestionController::LossEvent& lossEvent,
    Optional<SocketObserverInterface::LossEvent>& observerLossEvent);

/*
 * Detects losses based on ACKs or timeout.
 * Returns a LossEvent on success (possibly empty), or a QuicError if
 * processing encountered an error (e.g., from the lossVisitor).
 */
[[nodiscard]] quic::
    Expected<Optional<CongestionController::LossEvent>, QuicError>
    detectLossPackets(
        QuicConnectionStateBase& conn,
        const AckState& ackState,
        const LossVisitor& lossVisitor,
        const TimePoint lossTime,
        const PacketNumberSpace pnSpace,
        const CongestionController::AckEvent* ackEvent = nullptr);

/*
 * Function invoked when PTO alarm fires. Handles errors internally.
 */
[[nodiscard]] quic::Expected<void, QuicError> onPTOAlarm(
    QuicConnectionStateBase& conn);

/*
 * Function invoked when loss detection timer fires
 */
template <class ClockType = Clock>
[[nodiscard]] quic::Expected<void, QuicError> onLossDetectionAlarm(
    QuicConnectionStateBase& conn,
    const LossVisitor& lossVisitor) {
  auto now = ClockType::now();
  if (conn.outstandings.packets.empty()) {
    VLOG(10) << "Transmission alarm fired with no outstanding packets " << conn;
    return {};
  }
  if (conn.lossState.currentAlarmMethod ==
      LossState::AlarmMethod::EarlyRetransmitOrReordering) {
    auto lossTimeAndSpace = earliestLossTimer(conn);
    CHECK(lossTimeAndSpace.first);
    auto lossEventResult = detectLossPackets(
        conn,
        getAckState(conn, lossTimeAndSpace.second),
        lossVisitor,
        now,
        lossTimeAndSpace.second);
    if (!lossEventResult.has_value()) {
      return quic::make_unexpected(lossEventResult.error());
    }
    auto& lossEvent = lossEventResult.value();
    if (conn.congestionController && lossEvent) {
      DCHECK(lossEvent->largestLostSentTime && lossEvent->smallestLostSentTime);
      conn.congestionController->onPacketAckOrLoss(
          nullptr, lossEvent.has_value() ? &lossEvent.value() : nullptr);
    }
  } else {
    auto result = onPTOAlarm(conn);
    if (!result.has_value()) {
      return quic::make_unexpected(result.error());
    }
  }
  conn.pendingEvents.setLossDetectionAlarm =
      conn.outstandings.numOutstanding() > 0;
  VLOG(10) << __func__ << " setLossDetectionAlarm="
           << conn.pendingEvents.setLossDetectionAlarm
           << " outstanding=" << conn.outstandings.numOutstanding()
           << " initialPackets="
           << conn.outstandings.packetCount[PacketNumberSpace::Initial]
           << " handshakePackets="
           << conn.outstandings.packetCount[PacketNumberSpace::Handshake] << " "
           << conn;
  return {};
}

/*
 * Process streams in a RegularQuicWritePacket for loss.
 * This is the canonical implementation often used *by* the LossVisitor.
 * Returns folly::unit on success, or QuicError if accessing stream state fails.
 */
[[nodiscard]] quic::Expected<void, QuicError> markPacketLoss(
    QuicConnectionStateBase& conn,
    RegularQuicWritePacket& packet,
    bool processed);

/*
 * Handles ACK processing related to loss detection.
 * Returns a LossEvent on success (possibly empty), or QuicError if processing
 * failed.
 */
[[nodiscard]] quic::
    Expected<Optional<CongestionController::LossEvent>, QuicError>
    handleAckForLoss(
        QuicConnectionStateBase& conn,
        const LossVisitor& lossVisitor, // Visitor now returns Expected
        CongestionController::AckEvent& ack,
        PacketNumberSpace pnSpace);

/**
 * Force marks zero rtt packets as lost during zero rtt rejection.
 * Returns folly::unit on success, or QuicError if marking fails.
 */
template <class ClockType = Clock>
[[nodiscard]] quic::Expected<void, QuicError> markZeroRttPacketsLost(
    QuicConnectionStateBase& conn,
    const LossVisitor& lossVisitor) {
  CongestionController::LossEvent lossEvent(ClockType::now());

  auto iter = getFirstOutstandingPacket(conn, PacketNumberSpace::AppData);
  while (iter != conn.outstandings.packets.end()) {
    DCHECK_EQ(
        iter->packet.header.getPacketNumberSpace(), PacketNumberSpace::AppData);
    auto isZeroRttPacket =
        iter->packet.header.getProtectionType() == ProtectionType::ZeroRtt;
    if (isZeroRttPacket) {
      auto& pkt = *iter;
      bool processed = pkt.maybeClonedPacketIdentifier &&
          !conn.outstandings.clonedPacketIdentifiers.count(
              *pkt.maybeClonedPacketIdentifier);

      auto visitorResult = lossVisitor(conn, pkt.packet, processed);
      if (!visitorResult.has_value()) {
        return quic::make_unexpected(visitorResult.error());
      }

      if (pkt.maybeClonedPacketIdentifier) {
        conn.outstandings.clonedPacketIdentifiers.erase(
            *pkt.maybeClonedPacketIdentifier);
        CHECK(conn.outstandings.clonedPacketCount[PacketNumberSpace::AppData]);
        --conn.outstandings.clonedPacketCount[PacketNumberSpace::AppData];
      }
      lossEvent.addLostPacket(pkt);
      if (!processed) {
        CHECK(conn.outstandings.packetCount[PacketNumberSpace::AppData]);
        --conn.outstandings.packetCount[PacketNumberSpace::AppData];
      }
      iter = conn.outstandings.packets.erase(iter);
      iter = getNextOutstandingPacket(conn, PacketNumberSpace::AppData, iter);
    } else {
      iter =
          getNextOutstandingPacket(conn, PacketNumberSpace::AppData, iter + 1);
    }
  }

  conn.lossState.rtxCount += lossEvent.lostPackets;
  if (conn.congestionController && lossEvent.largestLostPacketNum.has_value()) {
    conn.congestionController->onRemoveBytesFromInflight(lossEvent.lostBytes);
  }
  VLOG(10) << __func__ << " marked=" << lossEvent.lostPackets;
  return {};
}

} // namespace quic
