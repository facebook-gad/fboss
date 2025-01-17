/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/sai/switch/SaiSwitch.h"
#include "fboss/agent/hw/sai/api/HostifApi.h"
#include "fboss/agent/hw/sai/api/SaiApiTable.h"
#include "fboss/agent/hw/sai/switch/SaiHostifManager.h"
#include "fboss/agent/hw/sai/switch/SaiManagerTable.h"
#include "fboss/agent/hw/sai/switch/SaiNeighborManager.h"
#include "fboss/agent/hw/sai/switch/SaiPortManager.h"
#include "fboss/agent/hw/sai/switch/SaiRouteManager.h"
#include "fboss/agent/hw/sai/switch/SaiRouterInterfaceManager.h"
#include "fboss/agent/hw/sai/switch/SaiRxPacket.h"
#include "fboss/agent/hw/sai/switch/SaiSwitchManager.h"
#include "fboss/agent/hw/sai/switch/SaiTxPacket.h"
#include "fboss/agent/hw/sai/switch/SaiVlanManager.h"
#include "fboss/agent/platforms/sai/SaiPlatform.h"
#include "fboss/agent/state/DeltaFunctions.h"
#include "fboss/agent/state/Port.h"
#include "fboss/agent/state/StateDelta.h"
#include "fboss/agent/state/SwitchState.h"

#include <iomanip>
#include <memory>
#include <sstream>

extern "C" {
#include <sai.h>
}
namespace facebook {
namespace fboss {

static SaiSwitch* hwSwitch;

using facebook::fboss::DeltaFunctions::forEachAdded;

// We need this global SaiSwitch* to support registering SAI callbacks
// which can then use SaiSwitch to do their work. The current callback
// facility in SAI does not support passing user data to come back
// with the callback.
// N.B., if we want to have multiple SaiSwitches in a device with multiple
// cards being managed by one instance of FBOSS, this will need to be
// extended, presumably into an array keyed by switch id.
static SaiSwitch* __gSaiSwitch;

void __gPacketRxCallback(
    sai_object_id_t switch_id,
    sai_size_t buffer_size,
    const void* buffer,
    uint32_t attr_count,
    const sai_attribute_t* attr_list) {
  __gSaiSwitch->packetRxCallback(
      switch_id, buffer_size, buffer, attr_count, attr_list);
}

HwInitResult SaiSwitch::init(Callback* callback) noexcept {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return initLocked(lock, callback);
}

void SaiSwitch::unregisterCallbacks() noexcept {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  unregisterCallbacksLocked(lock);
}

std::shared_ptr<SwitchState> SaiSwitch::stateChanged(const StateDelta& delta) {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return stateChangedLocked(lock, delta);
}

bool SaiSwitch::isValidStateUpdate(const StateDelta& delta) const {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return isValidStateUpdateLocked(lock, delta);
}

std::unique_ptr<TxPacket> SaiSwitch::allocatePacket(uint32_t size) {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return allocatePacketLocked(lock, size);
}

bool SaiSwitch::sendPacketSwitchedAsync(
    std::unique_ptr<TxPacket> pkt) noexcept {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return sendPacketSwitchedAsyncLocked(lock, std::move(pkt));
}

bool SaiSwitch::sendPacketOutOfPortAsync(
    std::unique_ptr<TxPacket> pkt,
    PortID portID,
    folly::Optional<uint8_t> queue) noexcept {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return sendPacketOutOfPortAsyncLocked(lock, std::move(pkt), portID, queue);
}

bool SaiSwitch::sendPacketSwitchedSync(std::unique_ptr<TxPacket> pkt) noexcept {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return sendPacketSwitchedSyncLocked(lock, std::move(pkt));
}

bool SaiSwitch::sendPacketOutOfPortSync(
    std::unique_ptr<TxPacket> pkt,
    PortID portID) noexcept {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return sendPacketOutOfPortSyncLocked(lock, std::move(pkt), portID);
}

void SaiSwitch::updateStats(SwitchStats* switchStats) {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  updateStatsLocked(lock, switchStats);
}

void SaiSwitch::fetchL2Table(std::vector<L2EntryThrift>* l2Table) {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  fetchL2TableLocked(lock, l2Table);
}

void SaiSwitch::gracefulExit(folly::dynamic& switchState) {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  gracefulExitLocked(lock, switchState);
}

folly::dynamic SaiSwitch::toFollyDynamic() const {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return toFollyDynamicLocked(lock);
}

void SaiSwitch::initialConfigApplied() {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  initialConfigAppliedLocked(lock);
}

void SaiSwitch::clearWarmBootCache() {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  clearWarmBootCacheLocked(lock);
}

void SaiSwitch::switchRunStateChanged(SwitchRunState newState) {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  switchRunStateChangedLocked(lock, newState);
}

void SaiSwitch::exitFatal() const {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  exitFatalLocked(lock);
}

bool SaiSwitch::isPortUp(PortID port) const {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return isPortUpLocked(lock, port);
}

bool SaiSwitch::getAndClearNeighborHit(RouterID vrf, folly::IPAddress& ip) {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return getAndClearNeighborHitLocked(lock, vrf, ip);
}

void SaiSwitch::clearPortStats(
    const std::unique_ptr<std::vector<int32_t>>& ports) {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  clearPortStatsLocked(lock, ports);
}

cfg::PortSpeed SaiSwitch::getPortMaxSpeed(PortID port) const {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return getPortMaxSpeedLocked(lock, port);
}

/*
 * This signature matches the SAI callback signature and will be invoked
 * immediately by the non-method SAI callback function.
 */
void SaiSwitch::packetRxCallback(
    sai_object_id_t switch_id,
    sai_size_t buffer_size,
    const void* buffer,
    uint32_t attr_count,
    const sai_attribute_t* attr_list) {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  packetRxCallbackLocked(
      lock, switch_id, buffer_size, buffer, attr_count, attr_list);
}

BootType SaiSwitch::getBootType() const {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return getBootTypeLocked(lock);
}

const SaiManagerTable* SaiSwitch::managerTable() const {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return managerTableLocked(lock);
}

SaiManagerTable* SaiSwitch::managerTable() {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return managerTableLocked(lock);
}

const SaiApiTable* SaiSwitch::apiTable() const {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return apiTableLocked(lock);
}

SaiApiTable* SaiSwitch::apiTable() {
  std::lock_guard<std::mutex> lock(saiSwitchMutex_);
  return apiTableLocked(lock);
}

// Begin Locked functions with actual SaiSwitch functionality

HwInitResult SaiSwitch::initLocked(
    const std::lock_guard<std::mutex>& lock,
    Callback* callback) noexcept {
  HwInitResult ret;
  ret.bootType = BootType::COLD_BOOT;
  bootType_ = BootType::COLD_BOOT;

  saiApiTable_ = std::make_unique<SaiApiTable>();
  managerTable_ =
      std::make_unique<SaiManagerTable>(apiTableLocked(lock), platform_);
  switchId_ = managerTable_->switchManager().getSwitchSaiId();
  callback_ = callback;

  auto state = std::make_shared<SwitchState>();
  ret.switchState = state;
  __gSaiSwitch = this;
  return ret;
}

void SaiSwitch::packetRxCallbackLocked(
    const std::lock_guard<std::mutex>& lock,
    sai_object_id_t switch_id,
    sai_size_t buffer_size,
    const void* buffer,
    uint32_t attr_count,
    const sai_attribute_t* attr_list) {
  sai_object_id_t saiPortId = 0;
  for (auto index = 0; index < attr_count; index++) {
    const sai_attribute_t* attr = &attr_list[index];
    switch (attr->id) {
      case SAI_HOSTIF_PACKET_ATTR_INGRESS_PORT:
        saiPortId = attr->value.oid;
        break;
      case SAI_HOSTIF_PACKET_ATTR_INGRESS_LAG:
      case SAI_HOSTIF_PACKET_ATTR_HOSTIF_TRAP_ID:
        break;
      default:
        XLOG(INFO) << "invalid attribute received";
    }
  }
  CHECK_NE(saiPortId, 0);
  const auto& portManager = managerTableLocked(lock)->portManager();
  PortID swPortId = portManager.getPortID(saiPortId);
  const SaiPort* port = portManager.getPort(swPortId);
  auto vlanId = port->getPortVlan();
  auto rxPacket =
      std::make_unique<SaiRxPacket>(buffer_size, buffer, swPortId, vlanId);
  callback_->packetReceived(std::move(rxPacket));
}

void SaiSwitch::unregisterCallbacksLocked(
    const std::lock_guard<std::mutex>& /* lock */) noexcept {}

std::shared_ptr<SwitchState> SaiSwitch::stateChangedLocked(
    const std::lock_guard<std::mutex>& lock,
    const StateDelta& delta) {
  managerTableLocked(lock)->vlanManager().processVlanDelta(
      delta.getVlansDelta());
  managerTableLocked(lock)->routerInterfaceManager().processInterfaceDelta(
      delta);
  managerTableLocked(lock)->neighborManager().processNeighborDelta(delta);
  managerTableLocked(lock)->routeManager().processRouteDelta(delta);
  managerTableLocked(lock)->hostifManager().processHostifDelta(delta);
  return delta.newState();
}

bool SaiSwitch::isValidStateUpdateLocked(
    const std::lock_guard<std::mutex>& /* lock */,
    const StateDelta& /* delta */) const {
  return true;
}

std::unique_ptr<TxPacket> SaiSwitch::allocatePacketLocked(
    const std::lock_guard<std::mutex>& /* lock */,
    uint32_t size) {
  return std::make_unique<SaiTxPacket>(size);
}

bool SaiSwitch::sendPacketSwitchedAsyncLocked(
    const std::lock_guard<std::mutex>& lock,
    std::unique_ptr<TxPacket> pkt) noexcept {
  return sendPacketSwitchedSyncLocked(lock, std::move(pkt));
}

bool SaiSwitch::sendPacketOutOfPortAsyncLocked(
    const std::lock_guard<std::mutex>& lock,
    std::unique_ptr<TxPacket> pkt,
    PortID portID,
    folly::Optional<uint8_t> /* queue */) noexcept {
  return sendPacketOutOfPortSyncLocked(lock, std::move(pkt), portID);
}

bool SaiSwitch::sendPacketSwitchedSyncLocked(
    const std::lock_guard<std::mutex>& lock,
    std::unique_ptr<TxPacket> pkt) noexcept {
  HostifApiParameters::TxPacketAttributes::TxType txType(
      SAI_HOSTIF_TX_TYPE_PIPELINE_LOOKUP);
  HostifApiParameters::TxPacketAttributes attributes{{txType, 0}};
  HostifApiParameters::HostifApiPacket txPacket{
      reinterpret_cast<void*>(pkt->buf()->writableData()),
      pkt->buf()->length()};
  auto& hostifApi = saiApiTable_->hostifApi();
  hostifApi.send(attributes.attrs(), switchId_, txPacket);
  return true;
}

bool SaiSwitch::sendPacketOutOfPortSyncLocked(
    const std::lock_guard<std::mutex>& lock,
    std::unique_ptr<TxPacket> pkt,
    PortID portID) noexcept {
  auto port = managerTableLocked(lock)->portManager().getPort(portID);
  if (!port) {
    throw FbossError("Failed to send packet on invalid port: ", portID);
  }
  HostifApiParameters::HostifApiPacket txPacket{
      reinterpret_cast<void*>(pkt->buf()->writableData()),
      pkt->buf()->length()};
  HostifApiParameters::TxPacketAttributes::EgressPortOrLag egressPort(
      port->id());
  HostifApiParameters::TxPacketAttributes::TxType txType(
      SAI_HOSTIF_TX_TYPE_PIPELINE_BYPASS);
  HostifApiParameters::TxPacketAttributes attributes{{txType, egressPort}};
  auto& hostifApi = saiApiTable_->hostifApi();
  hostifApi.send(attributes.attrs(), switchId_, txPacket);
  return true;
}

void SaiSwitch::updateStatsLocked(
    const std::lock_guard<std::mutex>& /* lock */,
    SwitchStats* /* switchStats */) {}

void SaiSwitch::fetchL2TableLocked(
    const std::lock_guard<std::mutex>& /* lock */,
    std::vector<L2EntryThrift>* /* l2Table */) {}

void SaiSwitch::gracefulExitLocked(
    const std::lock_guard<std::mutex>& /* lock */,
    folly::dynamic& /* switchState */) {}

folly::dynamic SaiSwitch::toFollyDynamicLocked(
    const std::lock_guard<std::mutex>& /* lock */) const {
  return folly::dynamic::object();
}

void SaiSwitch::initialConfigAppliedLocked(
    const std::lock_guard<std::mutex>& /* lock */) {}

void SaiSwitch::clearWarmBootCacheLocked(
    const std::lock_guard<std::mutex>& /* lock */) {}

void SaiSwitch::switchRunStateChangedLocked(
    const std::lock_guard<std::mutex>& lock,
    SwitchRunState newState) {
  switch (newState) {
    case SwitchRunState::INITIALIZED: {
      auto& switchApi = apiTableLocked(lock)->switchApi();
      switchApi.registerRxCallback(switchId_, __gPacketRxCallback);
    } break;
    default:
      break;
  }
}

void SaiSwitch::exitFatalLocked(
    const std::lock_guard<std::mutex>& /* lock */) const {}

bool SaiSwitch::isPortUpLocked(
    const std::lock_guard<std::mutex>& /* lock */,
    PortID /* port */) const {
  return true;
}

cfg::PortSpeed SaiSwitch::getPortMaxSpeedLocked(
    const std::lock_guard<std::mutex>& /* lock */,
    PortID /* port */) const {
  // TODO (srikrishnagopu): Use the read-only attribute
  // SAI_PORT_ATTR_SUPPORTED_SPEED to query the list of supported speeds
  // and return the maximum supported speed.
  return cfg::PortSpeed::HUNDREDG;
}

bool SaiSwitch::getAndClearNeighborHitLocked(
    const std::lock_guard<std::mutex>& /* lock */,
    RouterID /* vrf */,
    folly::IPAddress& /* ip */) {
  return true;
}

void SaiSwitch::clearPortStatsLocked(
    const std::lock_guard<std::mutex>& /* lock */,
    const std::unique_ptr<std::vector<int32_t>>& /* ports */) {}

BootType SaiSwitch::getBootTypeLocked(
    const std::lock_guard<std::mutex>& /* lock */) const {
  return bootType_;
}

const SaiManagerTable* SaiSwitch::managerTableLocked(
    const std::lock_guard<std::mutex>& /* lock */) const {
  return managerTable_.get();
}

SaiManagerTable* SaiSwitch::managerTableLocked(
    const std::lock_guard<std::mutex>& /* lock */) {
  return managerTable_.get();
}

const SaiApiTable* SaiSwitch::apiTableLocked(
    const std::lock_guard<std::mutex>& /* lock */) const {
  return saiApiTable_.get();
}

SaiApiTable* SaiSwitch::apiTableLocked(
    const std::lock_guard<std::mutex>& /* lock */) {
  return saiApiTable_.get();
}

} // namespace fboss
} // namespace facebook
