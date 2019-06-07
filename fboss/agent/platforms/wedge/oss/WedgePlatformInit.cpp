/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/platforms/wedge/WedgePlatformInit.h"

#include "fboss/agent/platforms/wedge/WedgePlatform.h"

namespace facebook { namespace fboss {
std::unique_ptr<WedgePlatform> createFBWedgePlatform(
    std::unique_ptr<PlatformProductInfo> /*productInfo*/) {
  return std::unique_ptr<WedgePlatform>{};
}
}} // facebook::fboss
