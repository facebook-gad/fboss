/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/IPv6Handler.h"

#include <folly/Format.h>
#include <folly/MacAddress.h>
#include <folly/logging/xlog.h>
#include "fboss/agent/DHCPv6Handler.h"
#include "fboss/agent/FbossError.h"
#include "fboss/agent/NeighborUpdater.h"
#include "fboss/agent/Platform.h"
#include "fboss/agent/RxPacket.h"
#include "fboss/agent/SwSwitch.h"
#include "fboss/agent/SwitchStats.h"
#include "fboss/agent/TxPacket.h"
#include "fboss/agent/UDPHeader.h"
#include "fboss/agent/Utils.h"
#include "fboss/agent/packet/ICMPHdr.h"
#include "fboss/agent/packet/IPv6Hdr.h"
#include "fboss/agent/packet/NDP.h"
#include "fboss/agent/packet/PktUtil.h"
#include "fboss/agent/state/AggregatePort.h"
#include "fboss/agent/state/Interface.h"
#include "fboss/agent/state/InterfaceMap.h"
#include "fboss/agent/state/NdpResponseTable.h"
#include "fboss/agent/state/NdpTable.h"
#include "fboss/agent/state/Route.h"
#include "fboss/agent/state/RouteTable.h"
#include "fboss/agent/state/RouteTableMap.h"
#include "fboss/agent/state/RouteTableRib.h"
#include "fboss/agent/state/StateDelta.h"
#include "fboss/agent/state/SwitchState.h"
#include "fboss/agent/state/Vlan.h"
#include "fboss/agent/state/VlanMap.h"

using folly::IPAddressV6;
using folly::MacAddress;
using folly::io::Cursor;
using folly::io::RWPrivateCursor;
using std::unique_ptr;
using std::shared_ptr;

namespace facebook { namespace fboss {

template <typename BodyFn>
std::unique_ptr<TxPacket> createICMPv6Pkt(
    SwSwitch* sw,
    folly::MacAddress dstMac,
    folly::MacAddress srcMac,
    VlanID vlan,
    const folly::IPAddressV6& dstIP,
    const folly::IPAddressV6& srcIP,
    ICMPv6Type icmp6Type,
    ICMPv6Code icmp6Code,
    uint32_t bodyLength,
    BodyFn serializeBody) {
  IPv6Hdr ipv6(srcIP, dstIP);
  ipv6.trafficClass = 0xe0; // CS7 precedence (network control)
  ipv6.payloadLength = ICMPHdr::SIZE + bodyLength;
  ipv6.nextHeader = static_cast<uint8_t>(IP_PROTO::IP_PROTO_IPV6_ICMP);
  ipv6.hopLimit = 255;

  ICMPHdr icmp6(
      static_cast<uint8_t>(icmp6Type), static_cast<uint8_t>(icmp6Code), 0);

  uint32_t pktLen = icmp6.computeTotalLengthV6(bodyLength);
  auto pkt = sw->allocatePacket(pktLen);
  RWPrivateCursor cursor(pkt->buf());
  icmp6.serializeFullPacket(&cursor, dstMac, srcMac, vlan,
                               ipv6, bodyLength, serializeBody);
  return pkt;
}

struct IPv6Handler::ICMPHeaders {
  folly::MacAddress dst;
  folly::MacAddress src;
  const IPv6Hdr* ipv6;
  const ICMPHdr* icmp6;
};

IPv6Handler::IPv6Handler(SwSwitch* sw)
    : AutoRegisterStateObserver(sw, "IPv6Handler"),
      sw_(sw) {
}

void IPv6Handler::stateUpdated(const StateDelta& delta) {
  for (const auto& entry : delta.getIntfsDelta()) {
    if (!entry.getOld()) {
      intfAdded(delta.newState().get(), entry.getNew().get());
    } else if (!entry.getNew()) {
      intfDeleted(entry.getOld().get());
    } else {
      // TODO: We could add an intfChanged() method to re-use the existing
      // IPv6RouteAdvertiser object.
      intfDeleted(entry.getOld().get());
      intfAdded(delta.newState().get(), entry.getNew().get());
    }
  }
}

bool IPv6Handler::raEnabled(const Interface* intf) const {
  return intf->getNdpConfig().routerAdvertisementSeconds > 0;
}

void IPv6Handler::intfAdded(const SwitchState* state, const Interface* intf) {
  // If IPv6 router advertisement isn't enabled on this interface, ignore it.
  if (!raEnabled(intf)) {
    return;
  }

  IPv6RouteAdvertiser adv(sw_, state, intf);
  auto ret = routeAdvertisers_.emplace(intf->getID(), std::move(adv));
  CHECK(ret.second);
}

void IPv6Handler::intfDeleted(const Interface* intf) {
  if (!raEnabled(intf)) {
    return;
  }
  auto numErased = routeAdvertisers_.erase(intf->getID());
  CHECK_EQ(numErased, 1);
}

void IPv6Handler::handlePacket(unique_ptr<RxPacket> pkt,
                               MacAddress dst,
                               MacAddress src,
                               Cursor cursor) {
  const uint32_t l3Len = pkt->getLength() - (cursor - Cursor(pkt->buf()));
  IPv6Hdr ipv6(cursor);  // note: advances our cursor object
  XLOG(DBG4) << "IPv6 (" << l3Len
             << " bytes)"
                " port: "
             << pkt->getSrcPort() << " vlan: " << pkt->getSrcVlan()
             << " src: " << ipv6.srcAddr.str() << " (" << src << ")"
             << " dst: " << ipv6.dstAddr.str() << " (" << dst << ")"
             << " nextHeader: " << static_cast<int>(ipv6.nextHeader);

  // Additional data (such as FCS) may be appended after the IP payload
  auto payload = folly::IOBuf::wrapBuffer(cursor.data(), ipv6.payloadLength);
  cursor.reset(payload.get());

  // retrieve the current switch state
  auto state = sw_->getState();
  PortID port = pkt->getSrcPort();

  // NOTE: DHCPv6 solicit packet from client has hoplimit set to 1,
  // we need to handle it before send the ICMPv6 TTL exceeded
  if (ipv6.nextHeader == static_cast<uint8_t>(IP_PROTO::IP_PROTO_UDP)) {
    UDPHeader udpHdr;
    Cursor udpCursor(cursor);
    udpHdr.parse(sw_, port, &udpCursor);
    XLOG(DBG4) << "DHCP UDP packet, source port :" << udpHdr.srcPort
               << " destination port: " << udpHdr.dstPort;
    if (DHCPv6Handler::isForDHCPv6RelayOrServer(udpHdr)) {
      DHCPv6Handler::handlePacket(sw_, std::move(pkt), src, dst, ipv6,
          udpHdr, udpCursor);
      return;
    }
  }

  // Get the Interface to which this packet should be forwarded in host
  // TODO:
  // 1. Assume VRF 0 now
  // 2. Only if v6 address has been assigned to an interface. For link local
  //    address that is supposed to be generated by default, we do not handle
  //    it now.
  std::shared_ptr<Interface> intf{nullptr};
  auto interfaceMap = state->getInterfaces();
  if (ipv6.dstAddr.isMulticast()) {
    // Forward multicast packet directly to corresponding host interface
    // and let Linux handle it. In software we consume ICMPv6 Multicast
    // packets for function of NDP protocol, rest all are forwarded to host.
    intf = interfaceMap->getInterfaceInVlanIf(pkt->getSrcVlan());
  } else if (ipv6.dstAddr.isLinkLocal()) {
    // Forward link-local packet directly to corresponding host interface
    // provided desAddr is assigned to that interface.
    intf = interfaceMap->getInterfaceInVlanIf(pkt->getSrcVlan());
    if (intf && ! (intf->hasAddress(ipv6.dstAddr))) {
      intf = nullptr;
    }
  } else {
    // Else loopup host interface based on destAddr
    intf = interfaceMap->getInterfaceIf(RouterID(0), ipv6.dstAddr);
  }

  // If the packet is destined to us, accept packets
  // with a hop limit of 1. Else we need to forward
  // this packet so the hop limit should be at least 1
  auto minHopLimit = intf ? 0 : 1;
  if (ipv6.hopLimit <= minHopLimit) {
    XLOG(DBG4) << "Rx IPv6 Packet with hop limit exceeded";
    sw_->portStats(port)->pktDropped();
    sw_->portStats(port)->ipv6HopExceeded();
    // Look up cpu mac from platform
    MacAddress cpuMac = sw_->getPlatform()->getLocalMac();
    sendICMPv6TimeExceeded(pkt->getSrcVlan(), cpuMac, cpuMac, ipv6, cursor);
    return;
  }

  if (intf) {
    // packets destined for us
    // Anything not handled by the controller, we will forward it to the host,
    // i.e. ping, ssh, bgp...
    PortID portID = pkt->getSrcPort();
    if (ipv6.payloadLength > intf->getMtu()) {
      // Generate PTB as interface to dst intf has MTU smaller than payload
      sendICMPv6PacketTooBig(
          portID, pkt->getSrcVlan(), src, dst, ipv6, intf->getMtu(), cursor);
      sw_->portStats(portID)->pktDropped();
      return;
    }
    if (ipv6.nextHeader == static_cast<uint8_t>(IP_PROTO::IP_PROTO_IPV6_ICMP)) {
      pkt = handleICMPv6Packet(std::move(pkt), dst, src, ipv6, cursor);
      if (pkt == nullptr) {
        // packet has been handled
        return;
      }
    }

    if (sw_->sendPacketToHost(intf->getID(), std::move(pkt))) {
      sw_->portStats(portID)->pktToHost(l3Len);
    } else {
      sw_->portStats(portID)->pktDropped();
    }
    return;
  }

  // Don't send solicitations for multicast or broadcast addresses.
  if (!ipv6.dstAddr.isMulticast() && !ipv6.dstAddr.isLinkLocalBroadcast()) {
    // If IP is not multicast or linklocal broadcast, we need to resolve the IP
    // for this packet.
    // TODO: Add rate limiting so we don't generate too many requests for the
    // same IP.  Following the rules in RFC 4861 should be sufficient.
    resolveDestAndHandlePacket(ipv6, std::move(pkt), dst, src, cursor);
  }
}

unique_ptr<RxPacket> IPv6Handler::handleICMPv6Packet(
    unique_ptr<RxPacket> pkt,
    MacAddress dst,
    MacAddress src,
    const IPv6Hdr& ipv6,
    Cursor cursor) {
  ICMPHdr icmp6(cursor); // note: advances our cursor object

  // Validate the checksum, and drop the packet if it is not valid
  if (!icmp6.validateChecksum(ipv6, cursor)) {
    XLOG(DBG3) << "bad ICMPv6 checksum";
    sw_->portStats(pkt)->pktDropped();
    return nullptr;
  }

  ICMPHeaders hdr{dst, src, &ipv6, &icmp6};
  ICMPv6Type type = static_cast<ICMPv6Type>(icmp6.type);
  switch (type) {
    case ICMPv6Type::ICMPV6_TYPE_NDP_ROUTER_SOLICITATION:
      handleRouterSolicitation(std::move(pkt), hdr, cursor);
      return nullptr;
    case ICMPv6Type::ICMPV6_TYPE_NDP_ROUTER_ADVERTISEMENT:
      handleRouterAdvertisement(std::move(pkt), hdr, cursor);
      return nullptr;
    case ICMPv6Type::ICMPV6_TYPE_NDP_NEIGHBOR_SOLICITATION:
      handleNeighborSolicitation(std::move(pkt), hdr, cursor);
      return nullptr;
    case ICMPv6Type::ICMPV6_TYPE_NDP_NEIGHBOR_ADVERTISEMENT:
      handleNeighborAdvertisement(std::move(pkt), hdr, cursor);
      return nullptr;
    case ICMPv6Type::ICMPV6_TYPE_NDP_REDIRECT_MESSAGE:
      sw_->portStats(pkt)->ipv6NdpPkt();
      // TODO: Do we need to bother handling this yet?
      sw_->portStats(pkt)->pktDropped();
      return nullptr;
    default:
      break;
  }

  return pkt;
}

void IPv6Handler::handleRouterSolicitation(unique_ptr<RxPacket> pkt,
                                           const ICMPHeaders& hdr,
                                           Cursor cursor) {
  sw_->portStats(pkt)->ipv6NdpPkt();
  if (!checkNdpPacket(hdr, pkt.get())) {
    return;
  }

  cursor.skip(4); // 4 reserved bytes

  auto state = sw_->getState();
  auto vlan = state->getVlans()->getVlanIf(pkt->getSrcVlan());
  if (!vlan) {
    sw_->portStats(pkt)->pktDropped();
    return;
  }

  auto intf = state->getInterfaces()->getInterfaceIf(vlan->getInterfaceID());
  if (!intf) {
    sw_->portStats(pkt)->pktDropped();
    return;
  }

  MacAddress dstMac = hdr.src;
  try {
    auto ndpOptions = NDPOptions(cursor);
    if (ndpOptions.sourceLinkLayerAddress) {
      dstMac = *ndpOptions.sourceLinkLayerAddress;
    }
  } catch (const HdrParseError& e) {
    XLOG(WARNING) << e.what();
    sw_->portStats(pkt)->pktDropped();
    return;
  }

  // Send the response
  IPAddressV6 dstIP = hdr.ipv6->srcAddr;
  if (dstIP.isZero()) {
    dstIP = IPAddressV6("ff01::1");
  }

  XLOG(DBG4) << "sending router advertisement in response to solicitation from "
             << dstIP.str() << " (" << dstMac << ")";

  uint32_t pktLen = IPv6RouteAdvertiser::getPacketSize(intf.get());
  auto resp = sw_->allocatePacket(pktLen);
  RWPrivateCursor respCursor(resp->buf());
  IPv6RouteAdvertiser::createAdvertisementPacket(
    intf.get(), &respCursor, dstMac, dstIP);
  // Based on the router solicidtation and advertisement mechanism, the
  // advertisement should send back to who request such solicidation. Besides,
  // right now, only servers send RSW router solicidation. It's kinda safe to
  // send router advertisement back to the src port.
  sw_->sendNetworkControlPacketAsync(
    std::move(resp),
    PortDescriptor::fromRxPacket(*pkt.get()));
}

void IPv6Handler::handleRouterAdvertisement(
    unique_ptr<RxPacket> pkt,
    const ICMPHeaders& hdr,
    Cursor /*cursor*/) {
  sw_->portStats(pkt)->ipv6NdpPkt();
  if (!checkNdpPacket(hdr, pkt.get())) {
    return;
  }

  if (!hdr.ipv6->srcAddr.isLinkLocal()) {
    XLOG(DBG6) << "bad IPv6 router advertisement: source address must be "
                  "link-local: "
               << hdr.ipv6->srcAddr;
    sw_->portStats(pkt)->ipv6NdpBad();
    return;
  }

  XLOG(DBG3) << "dropping IPv6 router advertisement from " << hdr.ipv6->srcAddr;
  sw_->portStats(pkt)->pktDropped();
}

void IPv6Handler::handleNeighborSolicitation(unique_ptr<RxPacket> pkt,
                                             const ICMPHeaders& hdr,
                                             Cursor cursor) {
  sw_->portStats(pkt)->ipv6NdpPkt();
  if (!checkNdpPacket(hdr, pkt.get())) {
    return;
  }

  cursor.skip(4); // 4 reserved bytes
  IPAddressV6 targetIP = PktUtil::readIPv6(&cursor);
  if (targetIP.isMulticast()) {
    XLOG(DBG6) << "bad IPv6 neighbor solicitation request: target is "
                  "multicast: "
               << targetIP;
    sw_->portStats(pkt)->ipv6NdpBad();
    return;
  }
  XLOG(DBG4) << "got neighbor solicitation for " << targetIP.str();

  auto state = sw_->getState();
  auto vlan = state->getVlans()->getVlanIf(pkt->getSrcVlan());
  if (!vlan) {
    // Hmm, we don't actually have this VLAN configured.
    // Perhaps the state has changed since we received the packet.
    sw_->portStats(pkt)->pktDropped();
    return;
  }

  // exctract NDP options to update cache only  with value of
  // Source LinkLayer Address option, if present
  NDPOptions ndpOptions;
  try {
    ndpOptions.tryParse(cursor);
  } catch (const HdrParseError& e) {
    XLOG(DBG6) << e.what();
    sw_->portStats(pkt)->ipv6NdpBad();
    return;
  }

  auto updater = sw_->getNeighborUpdater();
  auto type = ICMPv6Type::ICMPV6_TYPE_NDP_NEIGHBOR_SOLICITATION;

  if ((!ndpOptions.sourceLinkLayerAddress.hasValue() &&
       hdr.ipv6->dstAddr.isMulticast()) ||
      (ndpOptions.sourceLinkLayerAddress.hasValue() &&
       hdr.ipv6->srcAddr.isZero())) {
    /* rfc 4861 -  must not be included when the source IP address is the
      unspecified address.  must be included in multicast solicitations a
    */
    XLOG(DBG6) << "bad IPv6 neighbor solicitation request:"
               << " either multicast solicitation is missing source link layer"
               << " option or notification has source link layer address but"
               << " source is unspecified";
    sw_->portStats(pkt)->ipv6NdpBad();
    return;
  }

  if (!AggregatePort::isIngressValid(state, pkt)) {
    XLOG(INFO) << "Dropping invalid NS ingressing on port " << pkt->getSrcPort()
               << " on vlan " << vlan << " for " << targetIP;
    return;
  }

  auto entry = vlan->getNdpResponseTable()->getEntry(targetIP);
  auto srcPortDescriptor = PortDescriptor::fromRxPacket(*pkt.get());
  if (ndpOptions.sourceLinkLayerAddress.hasValue()) {
    /* rfc 4861 - if the source address is not the unspecified address and,
    on link layers that have addresses, the solicitation includes a Source
    Link-Layer Address option, then the recipient should create or update
    the Neighbor Cache entry for the IP Source Address of the solicitation.
    */
    if (!entry) {
      // if this IP address not is in NDP response table.
      updater->receivedNdpNotMine(
          vlan->getID(),
          hdr.ipv6->srcAddr,
          ndpOptions.sourceLinkLayerAddress.value(),
          srcPortDescriptor,
          type,
          0);
      return;
    }

    updater->receivedNdpMine(
        vlan->getID(),
        hdr.ipv6->srcAddr,
        ndpOptions.sourceLinkLayerAddress.value(),
        srcPortDescriptor,
        type,
        0);
  }
  // TODO: It might be nice to support duplicate address detection, and track
  // whether our IP is tentative or not.

  // Send the response. To reply the neighbor solicitation, we can use the
  // src port of such packet to send back the neighbor advertisement.
  sendNeighborAdvertisement(
      pkt->getSrcVlan(),
      entry.value().mac,
      targetIP,
      hdr.src,
      hdr.ipv6->srcAddr,
      srcPortDescriptor);
}

void IPv6Handler::handleNeighborAdvertisement(unique_ptr<RxPacket> pkt,
                                              const ICMPHeaders& hdr,
                                              Cursor cursor) {
  sw_->portStats(pkt)->ipv6NdpPkt();
  if (!checkNdpPacket(hdr, pkt.get())) {
    return;
  }

  auto flags = cursor.read<uint32_t>();
  IPAddressV6 targetIP = PktUtil::readIPv6(&cursor);

  MacAddress targetMac = hdr.src;
  try {
    auto ndpOptions = NDPOptions(cursor);
    if (ndpOptions.targetLinkLayerAddress) {
      targetMac = *ndpOptions.targetLinkLayerAddress;
    }
  } catch (const HdrParseError& e) {
    XLOG(DBG3) << e.what();
    sw_->portStats(pkt)->ipv6NdpBad();
    return;
  }

  if (targetMac.isMulticast() || targetMac.isBroadcast()) {
    XLOG(DBG3) << "ignoring IPv6 neighbor advertisement for " << targetIP
               << "with multicast MAC " << targetMac;
    sw_->portStats(pkt)->pktDropped();
    return;
  }

  auto state = sw_->getState();
  auto vlan = state->getVlans()->getVlanIf(pkt->getSrcVlan());
  if (!vlan) {
    // Hmm, we don't actually have this VLAN configured.
    // Perhaps the state has changed since we received the packet.
    sw_->portStats(pkt)->pktDropped();
    return;
  }

  XLOG(DBG4) << "got neighbor advertisement for " << targetIP << " ("
             << targetMac << ")";

  auto updater = sw_->getNeighborUpdater();
  auto type = ICMPv6Type::ICMPV6_TYPE_NDP_NEIGHBOR_ADVERTISEMENT;

  // Check to see if this IP address is in our NDP response table.
  auto entry = vlan->getNdpResponseTable()->getEntry(hdr.ipv6->dstAddr);
  if (!entry) {
    updater->receivedNdpNotMine(vlan->getID(), targetIP, hdr.src,
                                PortDescriptor::fromRxPacket(*pkt.get()),
                                type, flags);
    return;
  }

  updater->receivedNdpMine(vlan->getID(), targetIP, hdr.src,
                           PortDescriptor::fromRxPacket(*pkt.get()),
                           type, flags);
}


void IPv6Handler::sendICMPv6TimeExceeded(VlanID srcVlan,
                              MacAddress dst,
                              MacAddress src,
                              IPv6Hdr& v6Hdr,
                              folly::io::Cursor cursor) {
  auto state = sw_->getState();

  /*
   * The payload of ICMPv6TimeExceeded consists of:
   *  - The unused field of the ICMP header;
   *  - The original IPv6 header and its payload.
   */
  uint32_t icmpPayloadLength =
      ICMPHdr::ICMPV6_UNUSED_LEN + IPv6Hdr::SIZE + cursor.totalLength();
  // This payload and the IPv6/ICMPv6 headers must fit the IPv6 MTU
  icmpPayloadLength =
      std::min(icmpPayloadLength, IPV6_MIN_MTU - IPv6Hdr::SIZE - ICMPHdr::SIZE);

  auto serializeBody = [&](RWPrivateCursor* sendCursor) {
    // ICMPv6 unused field
    sendCursor->writeBE<uint32_t>(0);
    v6Hdr.serialize(sendCursor);
    auto remainingLength =
        icmpPayloadLength - ICMPHdr::ICMPV6_UNUSED_LEN - IPv6Hdr::SIZE;
    sendCursor->push(cursor, remainingLength);
  };

  IPAddressV6 srcIp = getSwitchVlanIPv6(state, srcVlan);
  auto icmpPkt = createICMPv6Pkt(
      sw_,
      dst,
      src,
      srcVlan,
      v6Hdr.srcAddr,
      srcIp,
      ICMPv6Type::ICMPV6_TYPE_TIME_EXCEEDED,
      ICMPv6Code::ICMPV6_CODE_TIME_EXCEEDED_HOPLIMIT_EXCEEDED,
      icmpPayloadLength,
      serializeBody);
  XLOG(DBG4) << "sending ICMPv6 Time Exceeded with srcMac  " << src
             << " dstMac: " << dst << " vlan: " << srcVlan
             << " dstIp: " << v6Hdr.srcAddr.str() << " srcIP: " << srcIp.str()
             << " bodyLength: " << icmpPayloadLength;
  sw_->sendPacketSwitchedAsync(std::move(icmpPkt));
}

void IPv6Handler::sendICMPv6PacketTooBig(
    PortID srcPort,
    VlanID srcVlan,
    folly::MacAddress dst,
    folly::MacAddress src,
    IPv6Hdr& v6Hdr,
    int expectedMtu,
    folly::io::Cursor cursor) {
  auto state = sw_->getState();

  // payload serialization function
  // 4 bytes expected MTU + ipv6 header + as much payload as possible to fit MTU
  // this is upper limit of bodyLength
  uint32_t bodyLengthLimit = IPV6_MIN_MTU - ICMPHdr::computeTotalLengthV6(0);
  // this is when we add the whole input L3 packet
  uint32_t fullPacketLength = ICMPHdr::ICMPV6_MTU_LEN + IPv6Hdr::SIZE
                              + cursor.totalLength();
  auto bodyLength = std::min(bodyLengthLimit, fullPacketLength);

  auto serializeBody = [&](RWPrivateCursor* sendCursor) {
    sendCursor->writeBE<uint32_t>(expectedMtu);
    v6Hdr.serialize(sendCursor);
    auto remainingLength = bodyLength - IPv6Hdr::SIZE -
                           ICMPHdr::ICMPV6_UNUSED_LEN;
    sendCursor->push(cursor, remainingLength);
  };

  IPAddressV6 srcIp = getSwitchVlanIPv6(state, srcVlan);
  auto icmpPkt = createICMPv6Pkt(sw_, dst, src, srcVlan,
                             v6Hdr.srcAddr, srcIp,
                             ICMPv6Type::ICMPV6_TYPE_PACKET_TOO_BIG,
                             ICMPv6Code::ICMPV6_CODE_PACKET_TOO_BIG,
                             bodyLength, serializeBody);

  XLOG(DBG4) << "sending ICMPv6 Packet Too Big with srcMac  " << src
          << " dstMac: " << dst
          << " vlan: " << srcVlan
          << " dstIp: " << v6Hdr.srcAddr.str()
          << " srcIP: " << srcIp.str()
          << " bodyLength: " << bodyLength;
  sw_->sendPacketSwitchedAsync(std::move(icmpPkt));
  sw_->portStats(srcPort)->pktTooBig();
}

bool IPv6Handler::checkNdpPacket(const ICMPHeaders& hdr,
                                 const RxPacket* pkt) const {
  // Validation common for all NDP packets
  if (hdr.ipv6->hopLimit != 255) {
    XLOG(DBG3) << "bad IPv6 NDP request (" << hdr.icmp6->type
               << "): hop limit should be 255, received value is "
               << static_cast<int>(hdr.ipv6->hopLimit);
    sw_->portStats(pkt)->ipv6NdpBad();
    return false;
  }
  if (hdr.icmp6->code != 0) {
    XLOG(DBG3) << "bad IPv6 NDP request (" << hdr.icmp6->type
               << "): code should be 0, received value is " << hdr.icmp6->code;
    sw_->portStats(pkt)->ipv6NdpBad();
    return false;
  }

  return true;
}

void IPv6Handler::sendMulticastNeighborSolicitation(
    SwSwitch* sw,
    const IPAddressV6& targetIP,
    const MacAddress& srcMac,
    const VlanID& vlanID) {
  IPAddressV6 solicitedNodeAddr = targetIP.getSolicitedNodeAddress();
  MacAddress dstMac = MacAddress::createMulticast(solicitedNodeAddr);
  // For now, we always use our link local IP as the source.
  IPAddressV6 srcIP(IPAddressV6::LINK_LOCAL, srcMac);

  NDPOptions ndpOptions;
  ndpOptions.sourceLinkLayerAddress.emplace(srcMac);

  XLOG(DBG4) << "sending neighbor solicitation for " << targetIP << " on vlan "
             << vlanID;

  sendNeighborSolicitation(
      sw,
      solicitedNodeAddr,
      dstMac,
      srcIP,
      srcMac,
      targetIP,
      vlanID,
      folly::Optional<PortDescriptor>(),
      ndpOptions);
}

/* unicast neighbor solicitation */
void IPv6Handler::sendUnicastNeighborSolicitation(
    SwSwitch* sw,
    const folly::IPAddressV6& targetIP,
    const folly::MacAddress& targetMac,
    const folly::IPAddressV6& srcIP,
    const folly::MacAddress& srcMac,
    const VlanID& vlanID,
    const folly::Optional<PortDescriptor>& portDescriptor) {
  auto state = sw->getState();
  auto vlan = state->getVlans()->getVlanIf(vlanID);
  if (!Interface::isIpAttached(targetIP, vlan->getInterfaceID(), state)) {
    XLOG(DBG2) << "unicast neighbor solicitation not sent, neighbor address: "
               << targetIP << ", is not in the subnets of interface: "
               << vlan->getInterfaceID() << " for vlan:" << vlanID;
    return;
  }

  XLOG(DBG4) << "sending unicast neighbor solicitation to " << targetIP << "("
             << targetMac << ")"
             << " on vlan " << vlanID << " from " << srcIP << "(" << srcMac
             << ")";

  return sendNeighborSolicitation(
      sw, targetIP, targetMac, srcIP, srcMac, targetIP, vlanID, portDescriptor);
}

void IPv6Handler::sendMulticastNeighborSolicitation(
    SwSwitch* sw,
    const IPAddressV6& targetIP,
    const shared_ptr<Vlan>& vlan) {
  auto state = sw->getState();
  auto intfID = vlan->getInterfaceID();

  auto intf = state->getInterfaces()->getInterfaceIf(intfID);
  if (!intf) {
    XLOG(DBG0) << "Cannot find interface " << intfID;
    return;
  }

  sendMulticastNeighborSolicitation(
      sw, targetIP, intf->getMac(), vlan->getID());
}

void IPv6Handler::resolveDestAndHandlePacket(
    IPv6Hdr hdr,
    unique_ptr<RxPacket> pkt,
    MacAddress dst,
    MacAddress src,
    Cursor cursor) {
  // Right now this either responds with PTB or generate neighbor soliciations
  auto ingressPort = pkt->getSrcPort();
  auto targetIP = hdr.dstAddr;
  auto state = sw_->getState();

  auto route = sw_->longestMatch(state, targetIP, RouterID(0));
  if (!route || !route->isResolved()) {
    sw_->portStats(ingressPort)->ipv6DstLookupFailure();
    // No way to reach targetIP
    return;
  }

  auto interfaces = state->getInterfaces();
  auto nexthops = route->getForwardInfo().getNextHopSet();

  for (auto nexthop : nexthops) {
    // get interface needed to reach next hop
    auto intf = interfaces->getInterfaceIf(nexthop.intf());
    if (intf) {
      // what should be source & destination of packet
      auto source = intf->getAddressToReach(nexthop.addr())->first.asV6();
      auto target = route->isConnected() ? targetIP : nexthop.addr().asV6();

      if (source == target) {
        // This packet is for us.  Don't generate PTB or NDP request.
        continue;
      }

      if (hdr.payloadLength > intf->getMtu()) {
        // Generate PTB as interface to next hop has MTU smaller than payload
        sendICMPv6PacketTooBig(
            ingressPort,
            pkt->getSrcVlan(),
            src,
            dst,
            hdr,
            intf->getMtu(),
            cursor);
        sw_->portStats(ingressPort)->pktDropped();
        return;
      } else {
        // Check if destination is unknown, in which case trigger NDP
        auto vlanID = intf->getVlanID();
        auto vlan = state->getVlans()->getVlanIf(vlanID);
        if (vlan) {
          auto entry = vlan->getNdpTable()->getEntryIf(target);
          if (nullptr == entry) {
            // No entry in NDP table, create a neighbor solicitation packet
            sendMulticastNeighborSolicitation(
                sw_, target, intf->getMac(), vlan->getID());
            // Notify the updater that we sent a solicitation out
            sw_->getNeighborUpdater()->sentNeighborSolicitation(vlanID, target);
          } else {
            XLOG(DBG5) << "not sending neighbor solicitation for "
                       << target.str() << ", "
                       << ((entry->isPending()) ? "pending" : "")
                       << " entry already exists";
          }
        }
      }
    }
  }
  sw_->portStats(pkt)->pktDropped();
} // namespace fboss

void IPv6Handler::sendMulticastNeighborSolicitations(
    PortID ingressPort,
    const folly::IPAddressV6& targetIP) {
  // Don't send solicitations for multicast or broadcast addresses.
  if (targetIP.isMulticast() || targetIP.isLinkLocalBroadcast()) {
    return;
  }

  auto state = sw_->getState();

  auto route = sw_->longestMatch(state, targetIP, RouterID(0));
  if (!route || !route->isResolved()) {
    sw_->portStats(ingressPort)->ipv6DstLookupFailure();
    // No way to reach targetIP
    return;
  }

  auto intfs = state->getInterfaces();
  auto nhs = route->getForwardInfo().getNextHopSet();
  for (auto nh : nhs) {
    auto intf = intfs->getInterfaceIf(nh.intf());
    if (intf) {
      auto source = intf->getAddressToReach(nh.addr())->first.asV6();
      auto target = route->isConnected() ? targetIP : nh.addr().asV6();
      if (source == target) {
        // This packet is for us.  Don't send NDP requests to ourself.
        continue;
      }

      auto vlanID = intf->getVlanID();
      auto vlan = state->getVlans()->getVlanIf(vlanID);
      if (vlan) {
        auto entry = vlan->getNdpTable()->getEntryIf(target);
        if (entry == nullptr) {
          // No entry in NDP table, create a neighbor solicitation packet
          sendMulticastNeighborSolicitation(
              sw_, target, intf->getMac(), vlan->getID());

          // Notify the updater that we sent a solicitation out
          sw_->getNeighborUpdater()->sentNeighborSolicitation(vlanID, target);
        } else {
          XLOG(DBG5) << "not sending neighbor solicitation for " << target.str()
                     << ", " << ((entry->isPending()) ? "pending" : "")
                     << " entry already exists";
        }
      }
    }
  }
}

void IPv6Handler::floodNeighborAdvertisements() {
  for (const auto& intf: *sw_->getState()->getInterfaces()) {
    for (const auto& addrEntry: intf->getAddresses()) {
      if (!addrEntry.first.isV6()) {
        continue;
      }
      sendNeighborAdvertisement(intf->getVlanID(), intf->getMac(),
          addrEntry.first.asV6(), MacAddress::BROADCAST, IPAddressV6());
    }
  }
}

void IPv6Handler::sendNeighborAdvertisement(
    VlanID vlan,
    MacAddress srcMac,
    IPAddressV6 srcIP,
    MacAddress dstMac,
    IPAddressV6 dstIP,
    const folly::Optional<PortDescriptor>& portDescriptor) {
  XLOG(DBG4) << "sending neighbor advertisement to " << dstIP.str() << " ("
             << dstMac << "): for " << srcIP << " (" << srcMac << ")";

  uint32_t flags =
      NeighborAdvertisementFlags::ROUTER | NeighborAdvertisementFlags::OVERRIDE;
  if (dstIP.isZero()) {
    // TODO: add a constructor that doesn't require string processing
    dstIP = IPAddressV6("ff01::1");
  } else {
    flags |= NeighborAdvertisementFlags::SOLICITED;
  }

  NDPOptions ndpOptions;
  ndpOptions.targetLinkLayerAddress.emplace(srcMac);

  uint32_t bodyLength = ICMPHdr::ICMPV6_UNUSED_LEN + IPAddressV6::byteCount() +
      ndpOptions.computeTotalLength();

  auto serializeBody = [&](RWPrivateCursor* cursor) {
    cursor->writeBE<uint32_t>(flags);
    cursor->push(srcIP.bytes(), IPAddressV6::byteCount());
    ndpOptions.serialize(cursor);
  };

  auto pkt = createICMPv6Pkt(sw_, dstMac, srcMac, vlan, dstIP, srcIP,
                             ICMPv6Type::ICMPV6_TYPE_NDP_NEIGHBOR_ADVERTISEMENT,
                             ICMPv6Code::ICMPV6_CODE_NDP_MESSAGE_CODE,
                             bodyLength, serializeBody);
  sw_->sendNetworkControlPacketAsync(std::move(pkt), portDescriptor);
}

void IPv6Handler::sendNeighborSolicitation(
    SwSwitch* sw,
    const folly::IPAddressV6& dstIP,
    const folly::MacAddress& dstMac,
    const folly::IPAddressV6& srcIP,
    const folly::MacAddress& srcMac,
    const folly::IPAddressV6& neighborIP,
    const VlanID& vlanID,
    const folly::Optional<PortDescriptor>& portDescriptor,
    const NDPOptions& ndpOptions) {
  auto state = sw->getState();

  uint32_t bodyLength = ICMPHdr::ICMPV6_UNUSED_LEN + IPAddressV6::byteCount() +
      ndpOptions.computeTotalLength();

  auto serializeBody = [&](RWPrivateCursor* cursor) {
    cursor->writeBE<uint32_t>(0); // reserved
    cursor->push(neighborIP.bytes(), IPAddressV6::byteCount());
    ndpOptions.serialize(cursor);
  };

  auto pkt = createICMPv6Pkt(
      sw,
      dstMac,
      srcMac,
      vlanID,
      dstIP,
      srcIP,
      ICMPv6Type::ICMPV6_TYPE_NDP_NEIGHBOR_SOLICITATION,
      ICMPv6Code::ICMPV6_CODE_NDP_MESSAGE_CODE,
      bodyLength,
      serializeBody);
  sw->sendNetworkControlPacketAsync(std::move(pkt), portDescriptor);
}
}} // facebook::fboss
