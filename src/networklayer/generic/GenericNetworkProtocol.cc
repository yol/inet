//
// Copyright (C) 2004 Andras Varga
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//


#include <stdlib.h>
#include <string.h>

#include "GenericNetworkProtocol.h"

#include "ARPPacket_m.h"
#include "ICMPMessage_m.h"
#include "InterfaceTableAccess.h"
#include "IPv4ControlInfo.h"
#include "IPv4Datagram.h"
#include "IPv4InterfaceData.h"
#include "IRoutingTable.h"


Define_Module(GenericNetworkProtocol);

//TODO TRANSLATE
// a multicast cimek eseten hianyoznak bizonyos NetFilter hook-ok
// a local interface-k hasznalata eseten szinten hianyozhatnak bizonyos NetFilter hook-ok

void GenericNetworkProtocol::initialize()
{
    QueueBase::initialize();

    ift = InterfaceTableAccess().get();
    rt = RoutingTableAccess().get();

    queueOutGate = gate("queueOut");

    defaultTimeToLive = par("timeToLive");
    defaultMCTimeToLive = par("multicastTimeToLive");
    fragmentTimeoutTime = par("fragmentTimeout");
    forceBroadcast = par("forceBroadcast");
    mapping.parseProtocolMapping(par("protocolMapping"));

    curFragmentId = 0;
    lastCheckTime = 0;
    fragbuf.init(icmpAccess.get());

    numMulticast = numLocalDeliver = numDropped = numUnroutable = numForwarded = 0;

    // NetFilter:
    hooks.clear();

    WATCH(numMulticast);
    WATCH(numLocalDeliver);
    WATCH(numDropped);
    WATCH(numUnroutable);
    WATCH(numForwarded);
}

void GenericNetworkProtocol::updateDisplayString()
{
    char buf[80] = "";
    if (numForwarded>0) sprintf(buf+strlen(buf), "fwd:%d ", numForwarded);
    if (numLocalDeliver>0) sprintf(buf+strlen(buf), "up:%d ", numLocalDeliver);
    if (numMulticast>0) sprintf(buf+strlen(buf), "mcast:%d ", numMulticast);
    if (numDropped>0) sprintf(buf+strlen(buf), "DROP:%d ", numDropped);
    if (numUnroutable>0) sprintf(buf+strlen(buf), "UNROUTABLE:%d ", numUnroutable);
    getDisplayString().setTagArg("t", 0, buf);
}

void GenericNetworkProtocol::endService(cPacket *msg)
{
    if (msg->getArrivalGate()->isName("transportIn"))
    {
        handleMessageFromHL( msg );
    }
    else if (dynamic_cast<ARPPacket *>(msg))
    {
        // dispatch ARP packets to ARP
        handleARP((ARPPacket *)msg);
    }
    else
    {
        IPv4Datagram *dgram = check_and_cast<IPv4Datagram *>(msg);
        InterfaceEntry *fromIE = getSourceInterfaceFrom(dgram);
        handlePacketFromNetwork(dgram, fromIE);
    }

    if (ev.isGUI())
        updateDisplayString();
}

InterfaceEntry *GenericNetworkProtocol::getSourceInterfaceFrom(cPacket *msg)
{
    cGate *g = msg->getArrivalGate();
    return g ? ift->getInterfaceByNetworkLayerGateIndex(g->getIndex()) : NULL;
}

void GenericNetworkProtocol::handlePacketFromNetwork(IPv4Datagram *datagram, InterfaceEntry *fromIE)
{
    ASSERT(datagram);
    ASSERT(fromIE);

    //
    // "Prerouting"
    //

    // check for header biterror
    if (datagram->hasBitError())
    {
        // probability of bit error in header = size of header / size of total message
        // (ignore bit error if in payload)
        double relativeHeaderLength = datagram->getHeaderLength() / (double)datagram->getByteLength();
        if (dblrand() <= relativeHeaderLength)
        {
            EV << "bit error found, sending ICMP_PARAMETER_PROBLEM\n";
            icmpAccess.get()->sendErrorMessage(datagram, ICMP_PARAMETER_PROBLEM, 0);
            return;
        }
    }

    EV << "Received datagram `" << datagram->getName() << "' with dest=" << datagram->getDestAddress() << "\n";

    if (datagramPreRoutingHook(datagram, fromIE) != GenericNetworkProtocol::Hook::ACCEPT) {
        return;
    }
    preroutingFinish(datagram, fromIE);
}

void GenericNetworkProtocol::preroutingFinish(IPv4Datagram *datagram, InterfaceEntry *fromIE)
{
    IPv4Address &destAddr = datagram->getDestAddress();

    // remove control info
    delete datagram->removeControlInfo();

    // route packet

    if (fromIE->isLoopback())
    {
        reassembleAndDeliver(datagram);
    }
    else if (destAddr.isMulticast())
    {
        // check for local delivery
        // Note: multicast routers will receive IGMP datagrams even if their interface is not joined to the group
        if (fromIE->ipv4Data()->isMemberOfMulticastGroup(destAddr) ||
                (rt->isMulticastForwardingEnabled() && datagram->getTransportProtocol() == IP_PROT_IGMP))
            reassembleAndDeliver(datagram->dup());

        // don't forward if IP forwarding is off, or if dest address is link-scope
        if (!rt->isIPForwardingEnabled() || destAddr.isLinkLocalMulticast())
            delete datagram;
        else if (datagram->getTimeToLive() == 0)
        {
            EV << "TTL reached 0, dropping datagram.\n";
            delete datagram;
        }
        else
            forwardMulticastPacket(datagram, fromIE);
    }
    else
    {
        InterfaceEntry *broadcastIE = NULL;

        // check for local delivery; we must accept also packets coming from the interfaces that
        // do not yet have an IP address assigned. This happens during DHCP requests.
        if (rt->isLocalAddress(destAddr) || fromIE->ipv4Data()->getIPAddress().isUnspecified())
        {
            reassembleAndDeliver(datagram);
        }
        else if (destAddr.isLimitedBroadcastAddress() || (broadcastIE=rt->findInterfaceByLocalBroadcastAddress(destAddr)))
        {
            // broadcast datagram on the target subnet if we are a router
            if (broadcastIE && fromIE != broadcastIE && rt->isIPForwardingEnabled())
                fragmentPostRouting(datagram->dup(), broadcastIE, IPv4Address::ALLONES_ADDRESS);

            EV << "Broadcast received\n";
            reassembleAndDeliver(datagram);
        }
        else if (!rt->isIPForwardingEnabled())
        {
            EV << "forwarding off, dropping packet\n";
            numDropped++;
            delete datagram;
        }
        else
            routeUnicastPacket(datagram, fromIE, NULL/*destIE*/, IPv4Address::UNSPECIFIED_ADDRESS);
    }
}

void GenericNetworkProtocol::handleARP(ARPPacket *msg)
{
    // FIXME hasBitError() check  missing!

    // delete old control info
    delete msg->removeControlInfo();

    // dispatch ARP packets to ARP and let it know the gate index it arrived on
    InterfaceEntry *fromIE = getSourceInterfaceFrom(msg);
    ASSERT(fromIE);

    IPv4RoutingDecision *routingDecision = new IPv4RoutingDecision();
    routingDecision->setInterfaceId(fromIE->getInterfaceId());
    msg->setControlInfo(routingDecision);

    send(msg, queueOutGate);
}

void GenericNetworkProtocol::handleReceivedICMP(ICMPMessage *msg)
{
    switch (msg->getType())
    {
        case ICMP_REDIRECT: // TODO implement redirect handling
        case ICMP_DESTINATION_UNREACHABLE:
        case ICMP_TIME_EXCEEDED:
        case ICMP_PARAMETER_PROBLEM: {
            // ICMP errors are delivered to the appropriate higher layer protocol
            IPv4Datagram *bogusPacket = check_and_cast<IPv4Datagram *>(msg->getEncapsulatedPacket());
            int protocol = bogusPacket->getTransportProtocol();
            int gateindex = mapping.getOutputGateForProtocol(protocol);
            send(msg, "transportOut", gateindex);
            break;
        }
        default: {
            // all others are delivered to ICMP: ICMP_ECHO_REQUEST, ICMP_ECHO_REPLY,
            // ICMP_TIMESTAMP_REQUEST, ICMP_TIMESTAMP_REPLY, etc.
            int gateindex = mapping.getOutputGateForProtocol(IP_PROT_ICMP);
            send(msg, "transportOut", gateindex);
            break;
        }
    }
}

void GenericNetworkProtocol::handleMessageFromHL(cPacket *msg)
{
    // if no interface exists, do not send datagram
    if (ift->getNumInterfaces() == 0)
    {
        EV << "No interfaces exist, dropping packet\n";
        numDropped++;
        delete msg;
        return;
    }

    // encapsulate and send
    IPv4Datagram *datagram = dynamic_cast<IPv4Datagram *>(msg);
    IPv4ControlInfo *controlInfo = NULL;
    //FIXME dubious code, remove? how can the HL tell IP whether it wants tunneling or forwarding?? --Andras
    if (datagram) // if HL sends an IPv4Datagram, route the packet
    {
        // Dsr routing, Dsr is a HL protocol and send IPv4Datagram
        if (datagram->getTransportProtocol()==IP_PROT_DSR)
        {
            controlInfo = check_and_cast<IPv4ControlInfo*>(datagram->removeControlInfo());
        }
    }
    else
    {
        // encapsulate
        controlInfo = check_and_cast<IPv4ControlInfo*>(msg->removeControlInfo());
        datagram = encapsulate(msg, controlInfo);
    }

    // extract requested interface and next hop
    InterfaceEntry *destIE = NULL;
    IPv4Address nextHopAddress = IPv4Address::UNSPECIFIED_ADDRESS;
    bool multicastLoop = true;
    if (controlInfo!=NULL)
    {
        destIE = ift->getInterfaceById(controlInfo->getInterfaceId());
        nextHopAddress = controlInfo->getNextHopAddr();
        multicastLoop = controlInfo->getMulticastLoop();
    }

    if (controlInfo)
        datagram->setControlInfo(controlInfo);

    if (datagramLocalOutHook(datagram, destIE) != GenericNetworkProtocol::Hook::ACCEPT) {
        return;
    }

    datagramLocalOut(datagram, destIE);
}

void GenericNetworkProtocol::datagramLocalOut(IPv4Datagram* datagram, InterfaceEntry* destIE)
{
    IPv4ControlInfo *controlInfo = dynamic_cast<IPv4ControlInfo*>(datagram->getControlInfo());
    IPv4Address nextHopAddress = IPv4Address::UNSPECIFIED_ADDRESS;
    bool multicastLoop = true;

    if (controlInfo!=NULL)
    {
        nextHopAddress = controlInfo->getNextHopAddr();
        multicastLoop = controlInfo->getMulticastLoop();
    }

    delete datagram->removeControlInfo();

    // send
    IPv4Address &destAddr = datagram->getDestAddress();

    EV << "Sending datagram `" << datagram->getName() << "' with dest=" << destAddr << "\n";

    if (datagram->getDestAddress().isMulticast())
    {
        destIE = determineOutgoingInterfaceForMulticastDatagram(datagram, destIE);

        // loop back a copy
        if (multicastLoop && (!destIE || !destIE->isLoopback()))
        {
            InterfaceEntry *loopbackIF = ift->getFirstLoopbackInterface();
            if (loopbackIF)
                fragmentPostRouting(datagram->dup(), loopbackIF, destAddr);
        }

        if (destIE)
        {
            numMulticast++;
            fragmentPostRouting(datagram, destIE, destAddr);
        }
        else
        {
            EV << "No multicast interface, packet dropped\n";
            numUnroutable++;
            delete datagram;
        }
    }
    else // unicast and broadcast
    {
        // check for local delivery
        if (rt->isLocalAddress(destAddr))
        {
            EV << "local delivery\n";
            if (destIE)
                EV << "datagram destination address is local, ignoring destination interface specified in the control info\n";

            destIE = ift->getFirstLoopbackInterface();
            ASSERT(destIE);
            fragmentPostRouting(datagram, destIE, destAddr);
        }
        else if (destAddr.isLimitedBroadcastAddress() || rt->isLocalBroadcastAddress(destAddr))
            routeLocalBroadcastPacket(datagram, destIE);
        else
            routeUnicastPacket(datagram, NULL, destIE, nextHopAddress);
    }
}

/* Choose the outgoing interface for the muticast datagram:
 *   1. use the interface specified by MULTICAST_IF socket option (received in the control info)
 *   2. lookup the destination address in the routing table
 *   3. if no route, choose the interface according to the source address
 *   4. or if the source address is unspecified, choose the first MULTICAST interface
 */
InterfaceEntry *GenericNetworkProtocol::determineOutgoingInterfaceForMulticastDatagram(IPv4Datagram *datagram, InterfaceEntry *multicastIFOption)
{
    InterfaceEntry *ie = NULL;
    if (multicastIFOption)
    {
        ie = multicastIFOption;
        EV << "multicast packet routed by socket option via output interface " << ie->getName() << "\n";
    }
    if (!ie)
    {
        IPv4Route *route = rt->findBestMatchingRoute(datagram->getDestAddress());
        if (route)
            ie = route->getInterface();
        if (ie)
            EV << "multicast packet routed by routing table via output interface " << ie->getName() << "\n";
    }
    if (!ie)
    {
        ie = rt->getInterfaceByAddress(datagram->getSrcAddress());
        if (ie)
            EV << "multicast packet routed by source address via output interface " << ie->getName() << "\n";
    }
    if (!ie)
    {
        ie = ift->getFirstMulticastInterface();
        if (ie)
            EV << "multicast packet routed via the first multicast interface " << ie->getName() << "\n";
    }
    return ie;
}


void GenericNetworkProtocol::routeUnicastPacket(IPv4Datagram *datagram, InterfaceEntry *fromIE, InterfaceEntry *destIE, IPv4Address destNextHopAddr)
{
    IPv4Address destAddr = datagram->getDestAddress();

    EV << "Routing datagram `" << datagram->getName() << "' with dest=" << destAddr << ": ";

    IPv4Address nextHopAddr;
    // if output port was explicitly requested, use that, otherwise use GenericNetworkProtocol routing
    if (destIE)
    {
        EV << "using manually specified output interface " << destIE->getName() << "\n";
        // and nextHopAddr remains unspecified
        if (!destNextHopAddr.isUnspecified())
           nextHopAddr = destNextHopAddr;  // Manet DSR routing explicit route
        // special case ICMP reply
        else if (destIE->isBroadcast())
        {
            // if the interface is broadcast we must search the next hop
            const IPv4Route *re = rt->findBestMatchingRoute(destAddr);
            if (re && re->getInterface() == destIE)
                nextHopAddr = re->getGateway();
        }
    }
    else
    {
        // use GenericNetworkProtocol routing (lookup in routing table)
        const IPv4Route *re = rt->findBestMatchingRoute(destAddr);
        if (re)
        {
            destIE = re->getInterface();
            nextHopAddr = re->getGateway();
        }
    }

    if (!destIE) // no route found
    {
        EV << "unroutable, sending ICMP_DESTINATION_UNREACHABLE\n";
        numUnroutable++;
        icmpAccess.get()->sendErrorMessage(datagram, ICMP_DESTINATION_UNREACHABLE, 0);
    }
    else // fragment and send
    {
        if (fromIE != NULL) if (datagramForwardHook(datagram, fromIE, destIE, nextHopAddr) != GenericNetworkProtocol::Hook::ACCEPT) {
            return;
        }

        EV << "output interface is " << destIE->getName() << ", next-hop address: " << nextHopAddr << "\n";
        numForwarded++;
        fragmentPostRouting(datagram, destIE, nextHopAddr);
    }
}

void GenericNetworkProtocol::routeLocalBroadcastPacket(IPv4Datagram *datagram, InterfaceEntry *destIE)
{
    // The destination address is 255.255.255.255 or local subnet broadcast address.
    // We always use 255.255.255.255 as nextHopAddress, because it is recognized by ARP,
    // and mapped to the broadcast MAC address.
    if (destIE!=NULL)
    {
        fragmentPostRouting(datagram, destIE, IPv4Address::ALLONES_ADDRESS);
    }
    else if (forceBroadcast)
    {
        // forward to each interface including loopback
        for (int i = 0; i<ift->getNumInterfaces(); i++)
        {
            InterfaceEntry *ie = ift->getInterface(i);
            fragmentPostRouting(datagram->dup(), ie, IPv4Address::ALLONES_ADDRESS);
        }
        delete datagram;
    }
    else
    {
        numDropped++;
        delete datagram;
    }
}

InterfaceEntry *GenericNetworkProtocol::getShortestPathInterfaceToSource(IPv4Datagram *datagram)
{
    return rt->getInterfaceForDestAddr(datagram->getSrcAddress());
}

void GenericNetworkProtocol::forwardMulticastPacket(IPv4Datagram *datagram, InterfaceEntry *fromIE)
{
    ASSERT(fromIE);
    const IPv4Address &origin = datagram->getSrcAddress();
    const IPv4Address &destAddr = datagram->getDestAddress();
    ASSERT(destAddr.isMulticast());

    EV << "Forwarding multicast datagram `" << datagram->getName() << "' with dest=" << destAddr << "\n";

    numMulticast++;

    const IPv4MulticastRoute *route = rt->findBestMatchingMulticastRoute(origin, destAddr);
    if (!route)
    {
        EV << "No route, packet dropped.\n";
        numUnroutable++;
        delete datagram;
    }
    else if (route->getParent() && fromIE != route->getParent())
    {
        EV << "Did not arrive on parent interface, packet dropped.\n";
        numDropped++;
        delete datagram;
    }
    // backward compatible: no parent means shortest path interface to source (RPB routing)
    else if (!route->getParent() && fromIE != getShortestPathInterfaceToSource(datagram))
    {
        EV << "Did not arrive on shortest path, packet dropped.\n";
        numDropped++;
        delete datagram;
    }
    else
    {
        numForwarded++;
        // copy original datagram for multiple destinations
        const IPv4MulticastRoute::ChildInterfaceVector &children = route->getChildren();
        for (unsigned int i=0; i<children.size(); i++)
        {
            InterfaceEntry *destIE = children[i]->getInterface();
            if (destIE != fromIE)
            {
                int ttlThreshold = destIE->ipv4Data()->getMulticastTtlThreshold();
                if (datagram->getTimeToLive() <= ttlThreshold)
                    EV << "Not forwarding to " << destIE->getName() << " (ttl treshold reached)\n";
                else if (children[i]->isLeaf() && !destIE->ipv4Data()->hasMulticastListener(destAddr))
                    EV << "Not forwarding to " << destIE->getName() << " (no listeners)\n";
                else
                {
                    EV << "Forwarding to " << destIE->getName() << "\n";
                    fragmentPostRouting(datagram->dup(), destIE, destAddr);
                }
            }
        }
        // only copies sent, delete original datagram
        delete datagram;
    }
}

void GenericNetworkProtocol::reassembleAndDeliver(IPv4Datagram *datagram)
{
    EV << "Local delivery\n";

    if (datagram->getSrcAddress().isUnspecified())
        EV << "Received datagram '%s' without source address filled in" << datagram->getName() << "\n";

    // reassemble the packet (if fragmented)
    if (datagram->getFragmentOffset()!=0 || datagram->getMoreFragments())
    {
        EV << "Datagram fragment: offset=" << datagram->getFragmentOffset()
           << ", MORE=" << (datagram->getMoreFragments() ? "true" : "false") << ".\n";

        // erase timed out fragments in fragmentation buffer; check every 10 seconds max
        if (simTime() >= lastCheckTime + 10)
        {
            lastCheckTime = simTime();
            fragbuf.purgeStaleFragments(simTime()-fragmentTimeoutTime);
        }

        datagram = fragbuf.addFragment(datagram, simTime());
        if (!datagram)
        {
            EV << "No complete datagram yet.\n";
            return;
        }
        EV << "This fragment completes the datagram.\n";
    }

    if (datagramLocalInHook(datagram, getSourceInterfaceFrom(datagram)) != GenericNetworkProtocol::Hook::ACCEPT) {
        return;
    }

    reassembleAndDeliverFinish(datagram);
}

void GenericNetworkProtocol::reassembleAndDeliverFinish(IPv4Datagram *datagram)
{
    // decapsulate and send on appropriate output gate
    int protocol = datagram->getTransportProtocol();

    if (protocol==IP_PROT_ICMP)
    {
        // incoming ICMP packets are handled specially
        handleReceivedICMP(check_and_cast<ICMPMessage *>(decapsulate(datagram)));
        numLocalDeliver++;
    }
    else if (protocol==IP_PROT_IP)
    {
        // tunnelled IP packets are handled separately
        send(decapsulate(datagram), "preRoutingOut");  //FIXME There is no "preRoutingOut" gate in the GenericNetworkProtocol module.
    }
    else
    {
        // JcM Fix: check if the transportOut port are connected, otherwise
        // discard the packet
        int gateindex = mapping.getOutputGateForProtocol(protocol);

        if (gate("transportOut", gateindex)->isPathOK())
        {
            send(decapsulate(datagram), "transportOut", gateindex);
            numLocalDeliver++;
        }
        else
        {
            EV << "L3 Protocol not connected. discarding packet" << endl;
            icmpAccess.get()->sendErrorMessage(datagram, ICMP_DESTINATION_UNREACHABLE, ICMP_DU_PROTOCOL_UNREACHABLE);
        }
    }
}

cPacket *GenericNetworkProtocol::decapsulate(IPv4Datagram *datagram)
{
    // decapsulate transport packet
    InterfaceEntry *fromIE = getSourceInterfaceFrom(datagram);
    cPacket *packet = datagram->decapsulate();

    // create and fill in control info
    IPv4ControlInfo *controlInfo = new IPv4ControlInfo();
    controlInfo->setProtocol(datagram->getTransportProtocol());
    controlInfo->setSrcAddr(datagram->getSrcAddress());
    controlInfo->setDestAddr(datagram->getDestAddress());
    controlInfo->setTypeOfService(datagram->getTypeOfService());
    controlInfo->setInterfaceId(fromIE ? fromIE->getInterfaceId() : -1);
    controlInfo->setTimeToLive(datagram->getTimeToLive());

    // original GenericNetworkProtocol datagram might be needed in upper layers to send back ICMP error message
    controlInfo->setOrigDatagram(datagram);

    // attach control info
    packet->setControlInfo(controlInfo);

    return packet;
}

void GenericNetworkProtocol::fragmentPostRouting(IPv4Datagram *datagram, InterfaceEntry *ie, IPv4Address nextHopAddr)
{
    if (datagramPostRoutingHook(datagram, getSourceInterfaceFrom(datagram), ie, nextHopAddr) != GenericNetworkProtocol::Hook::ACCEPT) {
        return;
    }

    fragmentAndSend(datagram, ie, nextHopAddr);
}

void GenericNetworkProtocol::fragmentAndSend(IPv4Datagram *datagram, InterfaceEntry *ie, IPv4Address nextHopAddr)
{
    // fill in source address
    if (datagram->getSrcAddress().isUnspecified())
        datagram->setSrcAddress(ie->ipv4Data()->getIPAddress());

    // hop counter decrement; but not if it will be locally delivered
    if (!ie->isLoopback())
        datagram->setTimeToLive(datagram->getTimeToLive()-1);

    // hop counter check
    if (datagram->getTimeToLive() < 0)
    {
        // drop datagram, destruction responsibility in ICMP
        EV << "datagram TTL reached zero, sending ICMP_TIME_EXCEEDED\n";
        icmpAccess.get()->sendErrorMessage(datagram, ICMP_TIME_EXCEEDED, 0);
        numDropped++;
        return;
    }

    int mtu = ie->getMTU();

    // check if datagram does not require fragmentation
    if (datagram->getByteLength() <= mtu)
    {
        sendDatagramToOutput(datagram, ie, nextHopAddr);
        return;
    }

    // if "don't fragment" bit is set, throw datagram away and send ICMP error message
    if (datagram->getDontFragment())
    {
        EV << "datagram larger than MTU and don't fragment bit set, sending ICMP_DESTINATION_UNREACHABLE\n";
        icmpAccess.get()->sendErrorMessage(datagram, ICMP_DESTINATION_UNREACHABLE,
                                                     ICMP_FRAGMENTATION_ERROR_CODE);
        numDropped++;
        return;
    }

    // optimization: do not fragment and reassemble on the loopback interface
    if (ie->isLoopback())
    {
        sendDatagramToOutput(datagram, ie, nextHopAddr);
        return;
    }

    // FIXME some IP options should not be copied into each fragment, check their COPY bit
    int headerLength = datagram->getHeaderLength();
    int payloadLength = datagram->getByteLength() - headerLength;
    int fragmentLength = ((mtu - headerLength) / 8) * 8; // payload only (without header)
    int offsetBase = datagram->getFragmentOffset();

    int noOfFragments = (payloadLength + fragmentLength - 1)/ fragmentLength;
    EV << "Breaking datagram into " << noOfFragments << " fragments\n";

    // create and send fragments
    std::string fragMsgName = datagram->getName();
    fragMsgName += "-frag";

    for (int offset=0; offset < payloadLength; offset+=fragmentLength)
    {
        bool lastFragment = (offset+fragmentLength >= payloadLength);
        // length equal to fragmentLength, except for last fragment;
        int thisFragmentLength = lastFragment ? payloadLength - offset : fragmentLength;

        // FIXME is it ok that full encapsulated packet travels in every datagram fragment?
        // should better travel in the last fragment only. Cf. with reassembly code!
        IPv4Datagram *fragment = (IPv4Datagram *) datagram->dup();
        fragment->setName(fragMsgName.c_str());

        // "more fragments" bit is unchanged in the last fragment, otherwise true
        if (!lastFragment)
            fragment->setMoreFragments(true);

        fragment->setByteLength(headerLength + thisFragmentLength);
        fragment->setFragmentOffset(offsetBase + offset);

        sendDatagramToOutput(fragment, ie, nextHopAddr);
    }

    delete datagram;
}

IPv4Datagram *GenericNetworkProtocol::encapsulate(cPacket *transportPacket, IPv4ControlInfo *controlInfo)
{
    IPv4Datagram *datagram = createIPv4Datagram(transportPacket->getName());
    datagram->setByteLength(IP_HEADER_BYTES);
    datagram->encapsulate(transportPacket);

    // set source and destination address
    IPv4Address dest = controlInfo->getDestAddr();
    datagram->setDestAddress(dest);

    IPv4Address src = controlInfo->getSrcAddr();

    // when source address was given, use it; otherwise it'll get the address
    // of the outgoing interface after routing
    if (!src.isUnspecified())
    {
        // if interface parameter does not match existing interface, do not send datagram
        if (rt->getInterfaceByAddress(src)==NULL)
            throw cRuntimeError("Wrong source address %s in (%s)%s: no interface with such address",
                      src.str().c_str(), transportPacket->getClassName(), transportPacket->getFullName());

        datagram->setSrcAddress(src);
    }

    // set other fields
    datagram->setTypeOfService(controlInfo->getTypeOfService());

    datagram->setIdentification(curFragmentId++);
    datagram->setMoreFragments(false);
    datagram->setDontFragment(controlInfo->getDontFragment());
    datagram->setFragmentOffset(0);

    short ttl;
    if (controlInfo->getTimeToLive() > 0)
        ttl = controlInfo->getTimeToLive();
    else if (datagram->getDestAddress().isLinkLocalMulticast())
        ttl = 1;
    else if (datagram->getDestAddress().isMulticast())
        ttl = defaultMCTimeToLive;
    else
        ttl = defaultTimeToLive;
    datagram->setTimeToLive(ttl);
    datagram->setTransportProtocol(controlInfo->getProtocol());

    // setting GenericNetworkProtocol options is currently not supported

    return datagram;
}

IPv4Datagram *GenericNetworkProtocol::createIPv4Datagram(const char *name)
{
    return new IPv4Datagram(name);
}

void GenericNetworkProtocol::sendDatagramToOutput(IPv4Datagram *datagram, InterfaceEntry *ie, IPv4Address nextHopAddr)
{
    if (ie->isLoopback())
    {
        // no interface module for loopback, forward packet internally
        // FIXME shouldn't be arrival(datagram) ?
        handlePacketFromNetwork(datagram, ie);
    }
    else
    {
        // send out datagram to ARP, with control info attached
        delete datagram->removeControlInfo();
        IPv4RoutingDecision *routingDecision = new IPv4RoutingDecision();
        routingDecision->setInterfaceId(ie->getInterfaceId());
        routingDecision->setNextHopAddr(nextHopAddr);
        datagram->setControlInfo(routingDecision);
        send(datagram, queueOutGate);
    }
}

// NetFilter:

void GenericNetworkProtocol::registerHook(int priority, GenericNetworkProtocol::Hook* hook) {
    Enter_Method("registerHook()");
    hooks.insert(std::pair<int, GenericNetworkProtocol::Hook*>(priority, hook));
}

void GenericNetworkProtocol::unregisterHook(int priority, GenericNetworkProtocol::Hook* hook) {
    Enter_Method("unregisterHook()");
    for (HookList::iterator iter = hooks.begin(); iter != hooks.end(); iter++) {
        if ((iter->first == priority) && (iter->second == hook)) {
            hooks.erase(iter);
            return;
        }
    }
}

void GenericNetworkProtocol::reinjectDatagram(const IPv4Datagram* datagram, GenericNetworkProtocol::Hook::Result verdict) {
    Enter_Method("reinjectDatagram()");
    for (DatagramQueueForHooks::iterator iter = queuedDatagramsForHooks.begin(); iter != queuedDatagramsForHooks.end(); iter++) {
        if (iter->datagram == datagram) {
            IPv4Datagram* datagram = iter->datagram;
            if (verdict == GenericNetworkProtocol::Hook::DROP) {
                delete datagram;
            } else {
                switch (iter->hook) {
                    case QueuedDatagramForHook::LOCALOUT:
                        datagramLocalOut(datagram, iter->outIE);
                        break;
                    case QueuedDatagramForHook::PREROUTING:
                        preroutingFinish(datagram, iter->inIE);
                        break;
                    case QueuedDatagramForHook::POSTROUTING:
                        fragmentAndSend(datagram, iter->outIE, iter->nextHopAddr);
                        break;
                    case QueuedDatagramForHook::LOCALIN:
                        reassembleAndDeliverFinish(datagram);
                        break;
                    case QueuedDatagramForHook::FORWARD:
                        throw cRuntimeError("Re-injection of datagram queued for this hook not implemented");
                        break;
                    default:
                        throw cRuntimeError("Unknown hook ID: %d", (int)(iter->hook));
                        break;
                }
            }

            queuedDatagramsForHooks.erase(iter);
            return;
        }
    }
}

GenericNetworkProtocol::Hook::Result GenericNetworkProtocol::datagramPreRoutingHook(IPv4Datagram* datagram, InterfaceEntry* inIE) {
    for (HookList::iterator iter = hooks.begin(); iter != hooks.end(); iter++) {
        GenericNetworkProtocol::Hook::Result r = iter->second->datagramPreRoutingHook(datagram, inIE);
        switch(r)
        {
            case GenericNetworkProtocol::Hook::ACCEPT: break;   // continue iteration
            case GenericNetworkProtocol::Hook::DROP:   delete datagram; return r;
            case GenericNetworkProtocol::Hook::QUEUE:  queuedDatagramsForHooks.push_back(QueuedDatagramForHook(datagram, inIE, NULL, IPv4Address::UNSPECIFIED_ADDRESS, QueuedDatagramForHook::PREROUTING)); return r;
            case GenericNetworkProtocol::Hook::STOLEN: return r;
            default: throw cRuntimeError("Unknown Hook::Result value: %d", (int)r);
        }
    }
    return GenericNetworkProtocol::Hook::ACCEPT;
}

GenericNetworkProtocol::Hook::Result GenericNetworkProtocol::datagramLocalInHook(IPv4Datagram* datagram, InterfaceEntry* inIE) {
    for (HookList::iterator iter = hooks.begin(); iter != hooks.end(); iter++) {
        GenericNetworkProtocol::Hook::Result r = iter->second->datagramLocalInHook(datagram, inIE);
        switch(r)
        {
            case GenericNetworkProtocol::Hook::ACCEPT: break;   // continue iteration
            case GenericNetworkProtocol::Hook::DROP:   delete datagram; return r;
            case GenericNetworkProtocol::Hook::QUEUE:  queuedDatagramsForHooks.push_back(QueuedDatagramForHook(datagram, inIE, NULL, IPv4Address::UNSPECIFIED_ADDRESS, QueuedDatagramForHook::LOCALIN)); return r;
            case GenericNetworkProtocol::Hook::STOLEN: return r;
            default: throw cRuntimeError("Unknown Hook::Result value: %d", (int)r);
        }
    }
    return GenericNetworkProtocol::Hook::ACCEPT;
}

GenericNetworkProtocol::Hook::Result GenericNetworkProtocol::datagramForwardHook(IPv4Datagram* datagram, InterfaceEntry* inIE, InterfaceEntry* outIE, IPv4Address& nextHopAddr) {
    for (HookList::iterator iter = hooks.begin(); iter != hooks.end(); iter++) {
        GenericNetworkProtocol::Hook::Result r = iter->second->datagramForwardHook(datagram, inIE, outIE, nextHopAddr);
        switch(r)
        {
            case GenericNetworkProtocol::Hook::ACCEPT: break;   // continue iteration
            case GenericNetworkProtocol::Hook::DROP:   delete datagram; return r;
            case GenericNetworkProtocol::Hook::QUEUE:  queuedDatagramsForHooks.push_back(QueuedDatagramForHook(datagram, inIE, outIE, nextHopAddr, QueuedDatagramForHook::FORWARD)); return r;
            case GenericNetworkProtocol::Hook::STOLEN: return r;
            default: throw cRuntimeError("Unknown Hook::Result value: %d", (int)r);
        }
    }
    return GenericNetworkProtocol::Hook::ACCEPT;
}

GenericNetworkProtocol::Hook::Result GenericNetworkProtocol::datagramPostRoutingHook(IPv4Datagram* datagram, InterfaceEntry* inIE, InterfaceEntry* outIE, IPv4Address& nextHopAddr) {
    for (HookList::iterator iter = hooks.begin(); iter != hooks.end(); iter++) {
        GenericNetworkProtocol::Hook::Result r = iter->second->datagramPostRoutingHook(datagram, inIE, outIE, nextHopAddr);
        switch(r)
        {
            case GenericNetworkProtocol::Hook::ACCEPT: break;   // continue iteration
            case GenericNetworkProtocol::Hook::DROP:   delete datagram; return r;
            case GenericNetworkProtocol::Hook::QUEUE:  queuedDatagramsForHooks.push_back(QueuedDatagramForHook(datagram, inIE, outIE, nextHopAddr, QueuedDatagramForHook::POSTROUTING)); return r;
            case GenericNetworkProtocol::Hook::STOLEN: return r;
            default: throw cRuntimeError("Unknown Hook::Result value: %d", (int)r);
        }
    }
    return GenericNetworkProtocol::Hook::ACCEPT;
}

GenericNetworkProtocol::Hook::Result GenericNetworkProtocol::datagramLocalOutHook(IPv4Datagram* datagram, InterfaceEntry* outIE) {
    for (HookList::iterator iter = hooks.begin(); iter != hooks.end(); iter++) {
        GenericNetworkProtocol::Hook::Result r = iter->second->datagramLocalOutHook(datagram, outIE);
        switch(r)
        {
            case GenericNetworkProtocol::Hook::ACCEPT: break;   // continue iteration
            case GenericNetworkProtocol::Hook::DROP:   delete datagram; return r;
            case GenericNetworkProtocol::Hook::QUEUE:  queuedDatagramsForHooks.push_back(QueuedDatagramForHook(datagram, NULL, outIE, IPv4Address::UNSPECIFIED_ADDRESS, QueuedDatagramForHook::LOCALOUT)); return r;
            case GenericNetworkProtocol::Hook::STOLEN: return r;
            default: throw cRuntimeError("Unknown Hook::Result value: %d", (int)r);
        }
    }
    return GenericNetworkProtocol::Hook::ACCEPT;
}
