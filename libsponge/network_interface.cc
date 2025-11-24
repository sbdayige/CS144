#include "network_interface.hh"

#include "arp_message.hh"
#include "ethernet_frame.hh"

#include <iostream>

// Dummy implementation of a network interface
// Translates from {IP datagram, next hop address} to link-layer frame, and from link-layer frame to IP datagram

// For Lab 5, please replace with a real implementation that passes the
// automated checks run by `make check_lab5`.

// You will need to add private members to the class declaration in `network_interface.hh`

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

//! \param[in] ethernet_address Ethernet (what ARP calls "hardware") address of the interface
//! \param[in] ip_address IP (what ARP calls "protocol") address of the interface
NetworkInterface::NetworkInterface(const EthernetAddress &ethernet_address, const Address &ip_address)
    : _ethernet_address(ethernet_address), _ip_address(ip_address) {
    cerr << "DEBUG: Network interface has Ethernet address " << to_string(_ethernet_address) << " and IP address "
         << ip_address.ip() << "\n";
}

EthernetFrame NetworkInterface::broadcast_frame(uint32_t ip) {
    ARPMessage arp_msg;
    arp_msg.opcode = ARPMessage::OPCODE_REQUEST;
    arp_msg.target_ip_address = ip;
    arp_msg.sender_ethernet_address = _ethernet_address;
    arp_msg.sender_ip_address = _ip_address.ipv4_numeric();
    arp_msg.target_ethernet_address = {};

    EthernetFrame ef;
    ef.header() = EthernetHeader{ETHERNET_BROADCAST,_ethernet_address,EthernetHeader::TYPE_ARP};
    ef.payload() = arp_msg.serialize();

    return ef;
}

//! \param[in] dgram the IPv4 datagram to be sent
//! \param[in] next_hop the IP address of the interface to send it to (typically a router or default gateway, but may also be another host if directly connected to the same network as the destination)
//! (Note: the Address type can be converted to a uint32_t (raw 32-bit IP address) with the Address::ipv4_numeric() method.)
void NetworkInterface::send_datagram(const InternetDatagram &dgram, const Address &next_hop) {
    const uint32_t next_hop_ip = next_hop.ipv4_numeric();

    // Check if we have a valid ARP entry
    const auto arp_it = _arp_table.find(next_hop_ip);
    if (arp_it != _arp_table.end() && arp_it->second.ttl > _timer) {
        EthernetFrame frame;
        frame.header() = {arp_it->second.eth_addr, _ethernet_address, EthernetHeader::TYPE_IPv4};
        frame.payload() = dgram.serialize();
        _frames_out.push(frame);
    } else {
        // Queue the datagram
        _waiting_datagrams.emplace_back(next_hop, dgram);

        // Send ARP request if needed (not sent recently)
        const auto wait_it = _waiting_arps.find(next_hop_ip);
        if (wait_it == _waiting_arps.end() || _timer >= wait_it->second + ARP_RESPONSE_TTL) {
            _frames_out.push(broadcast_frame(next_hop_ip));
            _waiting_arps[next_hop_ip] = _timer;
        }
    }
}

//! \param[in] frame the incoming Ethernet frame
optional<InternetDatagram> NetworkInterface::recv_frame(const EthernetFrame &frame) {
    // Ignore frames not destined for us (and not broadcast)
    if (frame.header().dst != _ethernet_address && frame.header().dst != ETHERNET_BROADCAST) {
        return {};
    }

    if (frame.header().type == EthernetHeader::TYPE_IPv4) {
        InternetDatagram dgram;
        if (dgram.parse(frame.payload()) == ParseResult::NoError) {
            return dgram;
        }
    } else if (frame.header().type == EthernetHeader::TYPE_ARP) {
        ARPMessage arp_msg;
        if (arp_msg.parse(frame.payload()) == ParseResult::NoError) {
            const uint32_t sender_ip = arp_msg.sender_ip_address;
            const EthernetAddress sender_eth = arp_msg.sender_ethernet_address;

            // Learn mapping from sender (request or reply)
            _arp_table[sender_ip] = {sender_eth, _timer + ARP_ENTRY_TTL};

            // Send any waiting datagrams for this IP
            for (auto it = _waiting_datagrams.begin(); it != _waiting_datagrams.end();) {
                if (it->first.ipv4_numeric() == sender_ip) {
                    send_datagram(it->second, it->first);
                    it = _waiting_datagrams.erase(it);
                } else {
                    ++it;
                }
            }

            // If we learned the mapping, we don't need to retransmit ARP request anymore
            _waiting_arps.erase(sender_ip);

            // If it's an ARP request for our IP, send a reply
            if (arp_msg.opcode == ARPMessage::OPCODE_REQUEST &&
                arp_msg.target_ip_address == _ip_address.ipv4_numeric()) {
                ARPMessage reply;
                reply.opcode = ARPMessage::OPCODE_REPLY;
                reply.sender_ethernet_address = _ethernet_address;
                reply.sender_ip_address = _ip_address.ipv4_numeric();
                reply.target_ethernet_address = sender_eth;
                reply.target_ip_address = sender_ip;

                EthernetFrame reply_frame;
                reply_frame.header() = {sender_eth, _ethernet_address, EthernetHeader::TYPE_ARP};
                reply_frame.payload() = reply.serialize();
                _frames_out.push(reply_frame);
            }
        }
    }
    return {};
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void NetworkInterface::tick(const size_t ms_since_last_tick) {
    _timer += ms_since_last_tick;

    // Retransmit pending ARP requests
    for (auto &pair : _waiting_arps) {
        if (_timer >= pair.second + ARP_RESPONSE_TTL) {
            _frames_out.push(broadcast_frame(pair.first));
            pair.second = _timer;
        }
    }
}
