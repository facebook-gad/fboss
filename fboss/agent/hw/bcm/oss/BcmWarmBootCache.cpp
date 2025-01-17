/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/hw/bcm/BcmWarmBootCache.h"

#include "fboss/agent/hw/bcm/BcmAclEntry.h"

namespace facebook { namespace fboss {

void BcmWarmBootCache::populateAcls(
  const int /*groupId*/,
  AclEntry2AclStat& /*stats*/,
  Priority2BcmAclEntryHandle& /*acls*/) {}

void BcmWarmBootCache::populateAclStats(const BcmAclEntryHandle /*acl*/,
  AclEntry2AclStat& /*stats*/) {}

void BcmWarmBootCache::removeBcmAcl(BcmAclEntryHandle /*handle*/) {}

void BcmWarmBootCache::removeBcmAclStat(BcmAclStatHandle /*handle*/) {}

void BcmWarmBootCache::detachBcmAclStat(BcmAclEntryHandle /*acl handle*/,
  BcmAclStatHandle /*stat handle*/) {}

void BcmWarmBootCache::populateMirrors() {}

void BcmWarmBootCache:: populateMirroredPorts() {}

void BcmWarmBootCache::populateMirroredPort(opennsl_gport_t /*port*/) {}
void BcmWarmBootCache::populateMirroredAcl(BcmAclEntryHandle /*handle*/) {}
void BcmWarmBootCache::stopUnclaimedPortMirroring(
    opennsl_gport_t /*port*/,
    MirrorDirection /*direction*/,
    BcmMirrorHandle /*mirror*/) {}
void BcmWarmBootCache::stopUnclaimedAclMirroring(
    BcmAclEntryHandle /*aclEntry*/,
    MirrorDirection /*direction*/,
    BcmMirrorHandle /*mirror*/) {}
void BcmWarmBootCache::removeUnclaimedMirror(BcmMirrorHandle /*mirror*/) {}
void BcmWarmBootCache::populateIngressQosMaps() {}
void BcmWarmBootCache::populateLabelSwitchActions() {}
void BcmWarmBootCache::removeUnclaimedLabelSwitchActions() {}
}} // facebook::fboss
