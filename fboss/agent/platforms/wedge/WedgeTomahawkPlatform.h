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

#include "fboss/agent/platforms/wedge/WedgePlatform.h"

#include <folly/Optional.h>

namespace facebook {
namespace fboss {

class PlatformProductInfo;

class WedgeTomahawkPlatform : public WedgePlatform {
 public:
  explicit WedgeTomahawkPlatform(
    std::unique_ptr<PlatformProductInfo> productInfo)
      : WedgePlatform(std::move(productInfo)) {}

  uint32_t getMMUBufferBytes() const override {
    // All WedgeTomahawk platforms have 16MB MMU buffer
    return 16 * 1024 * 1024;
  }
  uint32_t getMMUCellBytes() const override {
    // All WedgeTomahawk platforms have 208 byte cells
    return 208;
  }
  bool isCosSupported() const override {
    return true;
  }
  bool v6MirrorTunnelSupported() const override {
    return false;
  }
  uint32_t maxLabelStackDepth() const override {
    return 3;
  }
  const PortQueue& getDefaultPortQueueSettings(
    cfg::StreamType streamType) const override;
  const PortQueue& getDefaultControlPlaneQueueSettings(
    cfg::StreamType streamType) const override;

  bool useQueueGportForCos() const override {
    return true;
  }
};

} // namespace fboss
} // namespace facebook
