/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include "fboss/agent/HwSwitch.h"
#include "fboss/agent/state/StateDelta.h"
#include "fboss/agent/gen-cpp2/switch_config_types.h"
#include "fboss/agent/Constants.h"

#include <folly/Optional.h>
#include <gmock/gmock.h>

namespace facebook { namespace fboss {

class MockPlatform;

/*
 * MockPlatform is a mockable interface to a Platform. Non-critical
 * functions have stub implementations and functions that we need to
 * control for tests are mocked with gmock.
 */
class MockHwSwitch : public HwSwitch {
 public:
  explicit MockHwSwitch(MockPlatform* platform);

  MOCK_METHOD1(init, HwInitResult(Callback*));
  MOCK_METHOD1(
      stateChanged,
      std::shared_ptr<SwitchState>(const StateDelta& delta));
  MOCK_METHOD2(getAndClearNeighborHit, bool(RouterID, folly::IPAddress&));

  std::unique_ptr<TxPacket> allocatePacket(uint32_t size) override;

  // gmock currently doesn't support move-only types, so we have to
  // use some janky work-arounds.
  MOCK_METHOD1(sendPacketSwitchedAsync_, bool(TxPacket*));
  bool sendPacketSwitchedAsync(std::unique_ptr<TxPacket> pkt) noexcept override;

  MOCK_METHOD3(
      sendPacketOutOfPortAsync_,
      bool(TxPacket*, facebook::fboss::PortID, folly::Optional<uint8_t> queue));
  bool sendPacketOutOfPortAsync(
      std::unique_ptr<TxPacket> pkt,
      facebook::fboss::PortID portID,
      folly::Optional<uint8_t> queue = folly::none) noexcept override;
  MOCK_METHOD1(sendPacketSwitchedSync_, bool(TxPacket*));
  bool sendPacketSwitchedSync(std::unique_ptr<TxPacket> pkt) noexcept override;

  MOCK_METHOD2(
      sendPacketOutOfPortSync_,
      bool(TxPacket*, facebook::fboss::PortID));
  bool sendPacketOutOfPortSync(
      std::unique_ptr<TxPacket> pkt,
      facebook::fboss::PortID portID) noexcept override;

  MOCK_METHOD1(updateStats, void(SwitchStats* switchStats));
  MOCK_METHOD1(fetchL2Table, void(std::vector<L2EntryThrift>* l2Table));
  MOCK_METHOD1(gracefulExit, void(folly::dynamic& switchState));
  MOCK_CONST_METHOD0(toFollyDynamic, folly::dynamic());
  MOCK_METHOD0(initialConfigApplied, void());
  MOCK_METHOD0(clearWarmBootCache, void());
  MOCK_METHOD1(switchRunStateChanged, void(SwitchRunState newState));
  MOCK_CONST_METHOD0(exitFatal, void());
  MOCK_METHOD0(unregisterCallbacks, void());
  MOCK_CONST_METHOD1(isValidStateUpdate, bool(const StateDelta& delta));
  MOCK_CONST_METHOD1(isPortUp, bool(PortID port));
  MOCK_CONST_METHOD1(getPortMaxSpeed, cfg::PortSpeed(PortID port));
  MOCK_METHOD1(
      clearPortStats,
      void(const std::unique_ptr<std::vector<int32_t>>&));
  MOCK_CONST_METHOD0(getBootType, BootType());

 private:
  MockPlatform* platform_;

  // Forbidden copy constructor and assignment operator
  MockHwSwitch(MockHwSwitch const &) = delete;
  MockHwSwitch& operator=(MockHwSwitch const &) = delete;
};
}} // facebook::fboss
