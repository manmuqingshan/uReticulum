#include "rtreticulum/link.h"

#include "rtreticulum/cryptography/hkdf.h"
#include "rtreticulum/log.h"
#include "rtreticulum/msgpack.h"
#include "rtreticulum/packet.h"
#include "rtreticulum/transport.h"

/* Use the portable log macros from rtreticulum/log.h. DEBUGF is compiled
 * out in release builds; switch to INFOF for always-on diagnostics. */

namespace RNS {

namespace {
    constexpr size_t X25519_KEY_BYTES   = 32;
    constexpr size_t ED25519_KEY_BYTES  = 32;
    constexpr size_t SIGNATURE_BYTES    = 64;
    constexpr size_t MTU_BYTES          = 3;
    constexpr size_t REQUEST_DATA_BASE  = X25519_KEY_BYTES + ED25519_KEY_BYTES;
    constexpr size_t REQUEST_DATA_MTU   = REQUEST_DATA_BASE + MTU_BYTES;
    constexpr size_t LRPROOF_DATA_BASE  = SIGNATURE_BYTES + X25519_KEY_BYTES;
    constexpr size_t LRPROOF_DATA_MTU   = LRPROOF_DATA_BASE + MTU_BYTES;

    /* Signalling bytes: 3 bytes big-endian encoding both MTU and link mode.
     * Upper 3 bits of byte 0 = mode (shifted left 5), lower 21 bits = MTU.
     * Matches Python Reticulum's Link.signalling_bytes(). */
    void encode_signalling(Bytes& dst, uint32_t mtu,
                           uint8_t mode = Type::Link::MODE_AES256_CBC) {
        uint32_t masked = mtu & Type::Link::MTU_BYTEMASK;
        uint8_t mode_bits = (mode << 5) & Type::Link::MODE_BYTEMASK;
        dst.append(static_cast<uint8_t>(((masked >> 16) & 0x1F) | mode_bits));
        dst.append(static_cast<uint8_t>((masked >>  8) & 0xFF));
        dst.append(static_cast<uint8_t>( masked        & 0xFF));
    }

    uint32_t decode_mtu(const uint8_t* p) {
        return ((static_cast<uint32_t>(p[0]) << 16)
              | (static_cast<uint32_t>(p[1]) <<  8)
              |  static_cast<uint32_t>(p[2])) & Type::Link::MTU_BYTEMASK;
    }

    uint8_t decode_mode(const uint8_t* p) {
        return (p[0] & Type::Link::MODE_BYTEMASK) >> 5;
    }
}

Link::Link(const Destination& destination, bool initiator)
    : _destination(destination), _initiator(initiator) {
    _eph_x25519_prv  = Cryptography::X25519PrivateKey::generate();
    _eph_ed25519_prv = Cryptography::Ed25519PrivateKey::generate();
    _eph_x25519_pub  = _eph_x25519_prv->public_key()->public_bytes();
    _eph_ed25519_pub = _eph_ed25519_prv->public_key()->public_bytes();
}

bool Link::open_session_from_peer_pub(const Bytes& peer_x25519_pub) {
    _peer_x25519_pub = peer_x25519_pub;
    Bytes shared = _eph_x25519_prv->exchange(_peer_x25519_pub);

    /* Link id was already computed from the LINKREQUEST data (the hash
     * derivation is handled by the caller); we only derive the session key
     * here. */
    _shared_session_key = Cryptography::hkdf(
        64, shared, _hash, Bytes{Bytes::NONE});
    _token = std::make_shared<Cryptography::Token>(_shared_session_key);
    return true;
}

void Link::derive_keys() {
    /* Hook for future ratchet/key-rotation support. Phase 5 is single-shot. */
}

Bytes Link::link_id_from_lr_packet(const Packet& packet, size_t data_len) {
    /* Python Reticulum derives the link_id from the packet's hashable_part,
     * stripping any trailing bytes beyond ECPUBSIZE (the X25519 + Ed25519
     * public keys). This means MTU negotiation bytes don't affect the hash,
     * so both sides always agree on the link_id. */
    Bytes hashable = packet.get_hashable_part();
    if (data_len > REQUEST_DATA_BASE) {
        size_t diff = data_len - REQUEST_DATA_BASE;
        if (hashable.size() > diff) {
            hashable = hashable.left(hashable.size() - diff);
        }
    }
    return Identity::truncated_hash(hashable);
}

Link::Ptr Link::request(const Destination& destination,
                        EstablishedCallback on_established,
                        PacketCallback      on_packet,
                        uint32_t            advertised_mtu) {
    auto link = std::shared_ptr<Link>(new Link(destination, true));
    link->_on_established = std::move(on_established);
    link->_on_packet      = std::move(on_packet);
    link->_status         = HANDSHAKE;
    link->_local_mtu      = advertised_mtu;
    link->_mtu            = advertised_mtu ? advertised_mtu : Type::Reticulum::MTU;

    Bytes request_data;
    request_data << link->_eph_x25519_pub << link->_eph_ed25519_pub;
    if (advertised_mtu) encode_signalling(request_data, advertised_mtu);

    Packet pkt(destination, request_data, Type::Packet::LINKREQUEST);
    pkt.pack();

    /* Link id is derived from the packed packet's hashable_part — same
     * derivation as the responder and Python Reticulum. */
    link->_hash = link_id_from_lr_packet(pkt, request_data.size());

    Transport::register_link(link);
    Transport::broadcast(pkt.raw());
    return link;
}

Link::Ptr Link::validate_request(const Destination& owner,
                                 const Bytes&       request_data,
                                 const Packet&      request_packet) {
    bool has_mtu;
    if      (request_data.size() == REQUEST_DATA_BASE) has_mtu = false;
    else if (request_data.size() == REQUEST_DATA_MTU)  has_mtu = true;
    else {
        DEBUGF("Link::validate_request: bad data len %zu", request_data.size());
        return nullptr;
    }

    auto link = std::shared_ptr<Link>(new Link(owner, false));
    Bytes peer_x25519_pub  = request_data.left(X25519_KEY_BYTES);
    Bytes peer_ed25519_pub = request_data.mid(X25519_KEY_BYTES, ED25519_KEY_BYTES);
    (void)peer_ed25519_pub;  /* Initiator-side identity proof is a later phase. */

    if (has_mtu) {
        uint32_t peer_mtu = decode_mtu(request_data.data() + REQUEST_DATA_BASE);
        link->_mtu = peer_mtu;
        /* The responder will negotiate min(local, peer) once the firmware
         * sets _local_mtu via a future API. For now responder accepts the
         * initiator's proposed MTU verbatim. */
    }

    link->_hash = link_id_from_lr_packet(request_packet, request_data.size());

    if (!link->open_session_from_peer_pub(peer_x25519_pub)) return nullptr;
    link->_status = ACTIVE;

    /* Build LRPROOF: signed_data = link_id || eph_x25519_pub
     *                             || identity_ed25519_pub || signalling_bytes
     *               proof_data   = signature || eph_x25519_pub || signalling_bytes
     * Python Reticulum uses the destination identity's Ed25519 pub (not a
     * random ephemeral) in the signed data, since the responder's sig_prv
     * is the identity's long-term Ed25519 signing key. */
    Bytes signalling;
    if (has_mtu) encode_signalling(signalling, link->_mtu);

    Bytes signed_material;
    signed_material << link->_hash << link->_eph_x25519_pub
                    << owner.identity().signingPublicKey() << signalling;
    Bytes signature = owner.identity().sign(signed_material);

    Bytes proof_data;
    proof_data << signature << link->_eph_x25519_pub << signalling;

    Transport::register_link(link);

    /* Manually construct the proof packet because Packet::pack() won't know
     * to use the link hash as destination. */
    Bytes raw;
    uint8_t flags = (Type::Packet::HEADER_1 << 6)
                  | (Type::Transport::BROADCAST << 4)
                  | (Type::Destination::LINK << 2)
                  | Type::Packet::PROOF;
    raw << flags << uint8_t{0} << link->_hash << uint8_t{Type::Packet::LRPROOF} << proof_data;
    Transport::broadcast(raw);
    return link;
}

void Link::on_proof(const Packet& proof_packet) {
    if (_status != HANDSHAKE || !_initiator) return;
    const Bytes& d = proof_packet.data();
    bool has_mtu;
    if      (d.size() == LRPROOF_DATA_BASE) has_mtu = false;
    else if (d.size() == LRPROOF_DATA_MTU)  has_mtu = true;
    else {
        DEBUGF("Link::on_proof: bad LRPROOF size %zu", d.size());
        return;
    }
    Bytes signature       = d.left(SIGNATURE_BYTES);
    Bytes peer_x25519_pub = d.mid(SIGNATURE_BYTES, X25519_KEY_BYTES);
    Bytes signalling;
    if (has_mtu) {
        signalling = d.mid(LRPROOF_DATA_BASE, MTU_BYTES);
        _mtu = decode_mtu(d.data() + LRPROOF_DATA_BASE);
    }

    if (!open_session_from_peer_pub(peer_x25519_pub)) return;

    /* Verify: signed_data = link_id || peer_x25519_pub
     *                     || destination_ed25519_pub || signalling_bytes
     * The responder uses the destination identity's Ed25519 pub (not an
     * ephemeral key), matching Python Reticulum's prove() method. */
    Bytes peer_sig_pub = _destination.identity().signingPublicKey();
    Bytes signed_material;
    signed_material << _hash << peer_x25519_pub << peer_sig_pub << signalling;
    if (!_destination.identity().validate(signature, signed_material)) {
        DEBUG("Link::on_proof: invalid signature");
        _status = CLOSED;
        return;
    }

    _status = ACTIVE;
    if (_on_established) _on_established(*this);
}

void Link::send(const Bytes& plaintext) {
    if (_status != ACTIVE || !_token) return;
    Bytes ciphertext = _token->encrypt(plaintext);

    /* DATA-over-Link wire frame: flags || hops(0) || link_hash || context(NONE) || ciphertext */
    Bytes raw;
    uint8_t flags = (Type::Packet::HEADER_1 << 6)
                  | (Type::Transport::BROADCAST << 4)
                  | (Type::Destination::LINK << 2)
                  | Type::Packet::DATA;
    raw << flags << uint8_t{0} << _hash << uint8_t{Type::Packet::CONTEXT_NONE} << ciphertext;
    Transport::broadcast(raw);
}

void Link::send_with_context(const Bytes& plaintext, uint8_t context) {
    if (_status != ACTIVE || !_token) return;
    Bytes ciphertext = _token->encrypt(plaintext);
    Bytes raw;
    uint8_t flags = (Type::Packet::HEADER_1 << 6)
                  | (Type::Transport::BROADCAST << 4)
                  | (Type::Destination::LINK << 2)
                  | Type::Packet::DATA;
    raw << flags << uint8_t{0} << _hash << uint8_t{context} << ciphertext;
    Transport::broadcast(raw);
}

void Link::on_inbound(const Packet& packet) {
    if (_status != ACTIVE || !_token) return;
    if (packet.packet_type() != Type::Packet::DATA) return;
    Bytes plaintext = _token->decrypt(packet.data());
    if (plaintext.empty()) {
        DEBUGF("on_inbound: decrypt failed, data=%zu", packet.data().size());
        return;
    }

    uint8_t ctx = packet.context();
    DEBUGF("on_inbound: ctx=%u pt=%zu", (unsigned)ctx, plaintext.size());
    if (ctx == Type::Packet::REQUEST) {
        handle_request(plaintext, packet);
    } else {
        if (_on_packet) _on_packet(plaintext, *this);
    }
}

void Link::handle_request(const Bytes& plaintext, const Packet& packet) {
    /* request_id = truncated hash of the REQUEST packet's hashable_part,
     * matching Python Reticulum's packet.getTruncatedHash(). */
    Bytes request_id = Identity::truncated_hash(packet.get_hashable_part());

    /* Unpack msgpack: [timestamp, path_hash, data] */
    MsgPack::Reader r(plaintext);
    size_t arr = r.read_array_header();
    if (arr < 3) return;
    double requested_at = r.read_float64();
    Bytes path_hash     = r.read_bin();
    Bytes request_data;
    if (!r.read_nil()) request_data = r.read_bin();

    /* Look up handler in destination's request_handlers map. */
    auto& handlers = _destination.request_handlers();
    auto it = handlers.find(path_hash);
    if (it == handlers.end()) return;

    const auto& handler = it->second;
    Bytes response = handler.generator(
        handler.path, request_data, request_id,
        _hash, Identity{Type::NONE}, requested_at);

    if (response.empty()) return;

    /* Pack response: [request_id, response_bytes] */
    Bytes packed;
    MsgPack::pack_array_header(packed, 2);
    MsgPack::pack_bin(packed, request_id);
    MsgPack::pack_bin(packed, response);

    send_with_context(packed, Type::Packet::RESPONSE);
}

}
