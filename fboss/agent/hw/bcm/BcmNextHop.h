// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

extern "C" {
#include <opennsl/l3.h>
#include <opennsl/types.h>
}

#include "fboss/agent/hw/bcm/BcmHost.h"
#include "fboss/agent/hw/bcm/BcmHostKey.h"
#include "fboss/lib/RefMap.h"

namespace facebook {
namespace fboss {

class BcmSwitch;
class BcmHostReference;

class BcmNextHop {
 public:
  virtual ~BcmNextHop() {}
  virtual opennsl_if_t getEgressId() const = 0;
  virtual void programToCPU(opennsl_if_t intf) = 0;
  virtual bool isProgrammed() const = 0;
};

class BcmL3NextHop : public BcmNextHop {
 public:
  BcmL3NextHop(BcmSwitch* hw, BcmHostKey key);

  ~BcmL3NextHop() override {}

  opennsl_if_t getEgressId() const override;

  void programToCPU(opennsl_if_t intf) override;

  bool isProgrammed() const override;

 private:
  BcmSwitch* hw_;
  BcmHostKey key_;
  std::unique_ptr<BcmHostReference> hostReference_;
};

class BcmMplsNextHop : public BcmNextHop {
 public:
  BcmMplsNextHop(BcmSwitch* hw, BcmLabeledHostKey key);

  ~BcmMplsNextHop() override;

  opennsl_if_t getEgressId() const override;

  void programToCPU(opennsl_if_t intf) override;

  bool isProgrammed() const override;

  void program(BcmHostKey bcmHostKey);

  BcmHostKey getBcmHostKey() {
    return BcmHostKey(key_.getVrf(), key_.addr(), key_.intfID());
  }

  opennsl_gport_t getGPort();

 private:
  std::unique_ptr<BcmEgress> createEgress();
  void setPort(opennsl_port_t port);
  void setTrunk(opennsl_trunk_t trunk);

  BcmSwitch* hw_;
  BcmLabeledHostKey key_;
  folly::Optional<PortDescriptor> egressPort_;
  std::unique_ptr<BcmEgress> mplsEgress_;
};

template <typename NextHopKeyT, typename NextHopT>
class BcmNextHopTable {
 public:
  using MapT = FlatRefMap<NextHopKeyT, NextHopT>;
  explicit BcmNextHopTable(BcmSwitch* hw) : hw_(hw) {}
  const NextHopT* getNextHopIf(const NextHopKeyT& key) const;
  const NextHopT* getNextHop(const NextHopKeyT& key) const;
  std::shared_ptr<NextHopT> referenceOrEmplaceNextHop(const NextHopKeyT& key);
  const MapT& getNextHops() const {
    return nexthops_;
  }

 private:
  BcmSwitch* hw_;
  MapT nexthops_;
};

using BcmL3NextHopTable = BcmNextHopTable<BcmHostKey, BcmL3NextHop>;
using BcmMplsNextHopTable = BcmNextHopTable<BcmLabeledHostKey, BcmMplsNextHop>;
} // namespace fboss
} // namespace facebook