#include "rtreticulum/packet.h"

#include <stdexcept>

#include "rtreticulum/identity.h"

namespace RNS {

namespace TP = Type::Packet;

Packet::Packet(const Destination& destination, const Bytes& data,
               TP::types packet_type, TP::context_types context,
               Type::Transport::types transport_type)
    : _object(std::make_shared<Object>()) {
    _object->_destination    = destination;
    _object->_packet_type    = packet_type;
    _object->_context        = context;
    _object->_transport_type = transport_type;
    _object->_data           = data;
    _object->_destination_type = destination.type();
    if (_object->_data.size() > Type::Reticulum::MDU)
        _object->_data.resize(Type::Reticulum::MDU);
    _object->_flags = pack_flags();
}

Packet::Packet(const Bytes& raw_frame) : _object(std::make_shared<Object>()) {
    _object->_raw         = raw_frame;
    _object->_packed      = true;
    _object->_from_packed = true;
}

uint8_t Packet::pack_flags() const {
    return (_object->_header_type     << 6)
         | (_object->_context_flag    << 5)
         | (_object->_transport_type  << 4)
         | (_object->_destination_type << 2)
         |  _object->_packet_type;
}

void Packet::unpack_flags(uint8_t flags) {
    _object->_header_type      = static_cast<TP::header_types>((flags & 0b01000000) >> 6);
    _object->_context_flag     = static_cast<TP::context_flags>((flags & 0b00100000) >> 5);
    _object->_transport_type   = static_cast<Type::Transport::types>((flags & 0b00010000) >> 4);
    _object->_destination_type = static_cast<Type::Destination::types>((flags & 0b00001100) >> 2);
    _object->_packet_type      = static_cast<TP::types>(flags & 0b00000011);
}

void Packet::pack() {
    if (!_object->_destination)
        throw std::logic_error("Packet destination is required");

    _object->_destination_hash = _object->_destination.hash();
    Bytes header;
    header << _object->_flags;
    header << _object->_hops;
    header << _object->_destination_hash;

    Bytes ciphertext;
    if (_object->_packet_type == TP::DATA) {
        ciphertext = _object->_destination.encrypt(_object->_data);
    } else {
        /* ANNOUNCE / LINKREQUEST / PROOF are not encrypted. Phase 4 fills
         * in announce-specific signing logic. */
        ciphertext = _object->_data;
    }

    header << static_cast<uint8_t>(_object->_context);
    _object->_raw = header + ciphertext;
    _object->_packet_hash = Identity::full_hash(get_hashable_part());
    _object->_packed = true;
}

bool Packet::unpack() {
    if (_object->_raw.size() < 2 + Type::Reticulum::DESTINATION_LENGTH + 1)
        return false;
    const uint8_t* p = _object->_raw.data();
    size_t len = _object->_raw.size();
    _object->_flags = p[0];
    _object->_hops  = p[1];
    unpack_flags(_object->_flags);

    constexpr size_t DST_LEN = Type::Reticulum::DESTINATION_LENGTH;
    size_t off;

    if (_object->_header_type == TP::HEADER_2) {
        /* Transport-relayed packet: transport_id + destination_hash */
        if (len < 2 + 2 * DST_LEN + 1) return false;
        _object->_transport_id     = Bytes(p + 2, DST_LEN);
        _object->_destination_hash = Bytes(p + 2 + DST_LEN, DST_LEN);
        off = 2 + 2 * DST_LEN;
    } else {
        _object->_destination_hash = Bytes(p + 2, DST_LEN);
        off = 2 + DST_LEN;
    }

    _object->_context = static_cast<TP::context_types>(p[off]);
    off += 1;
    _object->_data = Bytes(p + off, len - off);
    _object->_packet_hash = Identity::full_hash(get_hashable_part());
    return true;
}

Bytes Packet::get_hash() const {
    return _object->_packet_hash;
}

Bytes Packet::get_hashable_part() const {
    /* Hash covers everything in raw[] except the hops counter (byte 1) and
     * the upper nibble of byte 0 (IFAC flag, header type, transport type).
     * Only the lower nibble (destination type + packet type) participates
     * in the hash — matching Python Reticulum's raw[0] & 0x0F.
     * For HEADER_2, skip the transport_id (first DST_LEN bytes after hops). */
    Bytes hashable;
    if (_object->_raw.size() < 2) return hashable;
    uint8_t first = _object->_raw.data()[0] & 0b00001111;
    hashable.append(first);
    if (_object->_header_type == Type::Packet::HEADER_2) {
        /* Skip transport_id: raw[2..2+DST_LEN], keep rest */
        constexpr size_t DST_LEN = Type::Reticulum::DESTINATION_LENGTH;
        size_t skip = 2 + DST_LEN;
        if (_object->_raw.size() > skip) {
            hashable.append(_object->_raw.data() + skip, _object->_raw.size() - skip);
        }
    } else {
        hashable.append(_object->_raw.data() + 2, _object->_raw.size() - 2);
    }
    return hashable;
}

}
