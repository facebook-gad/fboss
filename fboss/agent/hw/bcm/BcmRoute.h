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

extern "C" {
#include <opennsl/types.h>
#include <opennsl/l3.h>
}

#include <folly/dynamic.h>
#include <folly/IPAddress.h>
#include "fboss/agent/types.h"
#include "fboss/agent/state/Route.h"
#include "fboss/agent/state/RouteNextHopEntry.h"

#include <boost/container/flat_map.hpp>

namespace facebook { namespace fboss {

class BcmSwitch;
class BcmHost;
class BcmMultiPathNextHop;

/**
 * BcmRoute represents a L3 route object.
 */
class BcmRoute {
 public:
  BcmRoute(
      BcmSwitch* hw,
      opennsl_vrf_t vrf,
      const folly::IPAddress& addr,
      uint8_t len);
  ~BcmRoute();
  void program(const RouteNextHopEntry& fwd);
  static bool deleteLpmRoute(int unit,
                             opennsl_vrf_t vrf,
                             const folly::IPAddress& prefix,
                             uint8_t prefixLength);
  static void initL3RouteFromArgs(opennsl_l3_route_t* rt,
                                  opennsl_vrf_t vrf,
                                  const folly::IPAddress& prefix,
                                  uint8_t prefixLength);

  opennsl_if_t getEgressId() const {
    return egressId_;
  }

  /* used for tests */
  std::shared_ptr<BcmMultiPathNextHop> getNextHop() const {
    return nextHopHostReference_;
  }

 private:
  std::shared_ptr<BcmHost> programHostRoute(
      opennsl_if_t egressId,
      const RouteNextHopEntry& fwd,
      bool replace);
  void programLpmRoute(opennsl_if_t egressId, const RouteNextHopEntry& fwd);
  /*
   * Check whether we can use the host route table. BCM platforms
   * support this from TD2 onwards
   */
  bool isHostRoute() const;
  bool canUseHostTable() const;
  // no copy or assign
  BcmRoute(const BcmRoute &) = delete;
  BcmRoute& operator=(const BcmRoute &) = delete;
  BcmSwitch* hw_;
  opennsl_vrf_t vrf_;
  folly::IPAddress prefix_;
  uint8_t len_;
  RouteNextHopEntry fwd_{RouteNextHopEntry::Action::DROP,
                         AdminDistance::MAX_ADMIN_DISTANCE};
  bool added_{false}; // if the route added to HW or not
  opennsl_if_t egressId_{-1};
  void initL3RouteT(opennsl_l3_route_t* rt) const;
  std::shared_ptr<BcmMultiPathNextHop>
      nextHopHostReference_; // reference to nexthops
  std::shared_ptr<BcmHost> hostRouteEntry_; // for host routes
};

class BcmRouteTable {
 public:
  explicit BcmRouteTable(BcmSwitch* hw);
  ~BcmRouteTable();
  // throw an error if not found
  BcmRoute* getBcmRoute(
      opennsl_vrf_t vrf, const folly::IPAddress& prefix, uint8_t len) const;
  // return nullptr if not found
  BcmRoute* getBcmRouteIf(
      opennsl_vrf_t vrf, const folly::IPAddress& prefix, uint8_t len) const;

  /*
   * The following functions will modify the object. They rely on the global
   * HW update lock in BcmSwitch::lock_ for the protection.
   */
  template<typename RouteT>
  void addRoute(opennsl_vrf_t vrf, const RouteT *route);
  template<typename RouteT>
  void deleteRoute(opennsl_vrf_t vrf, const RouteT *route);
 private:
  struct Key {
    folly::IPAddress network;
    uint8_t mask;
    opennsl_vrf_t vrf;
    bool operator<(const Key& k2) const;
  };

  BcmSwitch* hw_;

  boost::container::flat_map<Key, std::unique_ptr<BcmRoute>> fib_;
};

}}
