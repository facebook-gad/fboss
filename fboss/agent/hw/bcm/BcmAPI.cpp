/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/hw/bcm/BcmAPI.h"

#include "fboss/agent/FbossError.h"
#include "fboss/agent/hw/bcm/BcmPlatform.h"
#include "fboss/agent/hw/bcm/BcmUnit.h"
#include "fboss/agent/hw/bcm/BcmWarmBootHelper.h"

#include <folly/Memory.h>
#include <folly/String.h>
#include <folly/io/Cursor.h>
#include <folly/logging/xlog.h>
#include <glog/logging.h>

#include <atomic>
#include <unordered_map>

using std::make_unique;
using folly::StringPiece;
using std::string;

/*
 * bde_create() must be defined as a symbol when linking against BRCM libs.
 * It should never be invoked in our setup though. So return a error
 */
extern "C" int bde_create() {
  XLOG(ERR) << "unexpected call to bde_create(): probe invoked "
               "via diag shell command?";
  return OPENNSL_E_UNAVAIL;
}
/*
 * We don't set any default values.
 */
extern "C" void sal_config_init_defaults() {
}

namespace facebook { namespace fboss {

static std::atomic<bool> bcmInitialized{false};
static BcmAPI::HwConfigMap bcmConfig;

extern std::atomic<BcmUnit*> bcmUnits[];

std::unique_ptr<BcmAPI> BcmAPI::singleton_;

void BcmAPI::initConfig(
  const std::map<std::string, std::string>& config) {
  // Store the configuration settings
  bcmConfig.clear();
  for (const auto& entry : config) {
    bcmConfig.emplace(entry.first, entry.second);
  }
}

const char* BcmAPI::getConfigValue(StringPiece name) {
  auto it = bcmConfig.find(name);
  if (it == bcmConfig.end()) {
    return nullptr;
  }
  return it->second.c_str();
}

BcmAPI::HwConfigMap BcmAPI::getHwConfig() {
  return bcmConfig;
}

std::unique_ptr<BcmUnit> BcmAPI::initUnit(
    int deviceIndex,
    BcmPlatform* platform) {
  auto unitObj = make_unique<BcmUnit>(deviceIndex, platform);
  int unit = unitObj->getNumber();
  BcmUnit* expectedUnit{nullptr};
  if (!bcmUnits[unit].compare_exchange_strong(expectedUnit, unitObj.get(),
                                              std::memory_order_acq_rel)) {
    throw FbossError("a BcmUnit already exists for unit number ", unit);
  }
  platform->onUnitCreate(unit);
  if (platform->getWarmBootHelper()->canWarmBoot()) {
    unitObj->warmBootAttach();
  } else {
    unitObj->coldBootAttach();
  }
  platform->onUnitAttach(unit);
  return unitObj;
}

void BcmAPI::init(const std::map<std::string, std::string>& config) {
    if (bcmInitialized.load(std::memory_order_acquire)) {
      return;
    }

    initConfig(config);
    BcmAPI::initImpl();

    bcmInitialized.store(true, std::memory_order_release);
}


std::unique_ptr<BcmUnit> BcmAPI::initOnlyUnit(BcmPlatform* platform) {
  auto numDevices = BcmAPI::getNumSwitches();
  if (numDevices == 0) {
    throw FbossError("no Broadcom switching ASIC found");
  } else if (numDevices > 1) {
    throw FbossError("found more than 1 Broadcom switching ASIC");
  }
  return initUnit(0, platform);
}

void BcmAPI::unitDestroyed(BcmUnit* unit) {
  int num = unit->getNumber();
  BcmUnit* expectedUnit{unit};
  if (!bcmUnits[num].compare_exchange_strong(expectedUnit, nullptr,
                                             std::memory_order_acq_rel)) {
    XLOG(FATAL) << "inconsistency in BCM unit array for unit " << num
                << ": expected " << (void*)unit << " but found "
                << (void*)expectedUnit;
  }
  bcmInitialized.store(false, std::memory_order_release);
}

BcmUnit* BcmAPI::getUnit(int unit) {
  if (unit < 0 || unit > getMaxSwitches()) {
    throw FbossError("invalid BCM unit number ", unit);
  }
  BcmUnit* unitObj = bcmUnits[unit].load(std::memory_order_acquire);
  if (!unitObj) {
    throw FbossError("no BcmUnit created for unit number ", unit);
  }
  return unitObj;
}

}} // facebook::fboss
