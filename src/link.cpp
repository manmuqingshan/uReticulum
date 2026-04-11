#include "ureticulum/link.h"

#include "ureticulum/cryptography/hkdf.h"
#include "ureticulum/log.h"
#include "ureticulum/packet.h"
#include "ureticulum/transport.h"

namespace RNS {

namespace {
    constexpr size_t X25519_KEY_BYTES  = 32;
    constexpr size_t ED25519_KEY_BYTES = 32;
    constexpr size_t SIGNATURE_BYTES   = 64;
    constexpr size_t REQUEST_DATA_LEN  = X25519_KEY_BYTES + ED25519_KEY_BYTES;
    constexpr size_t LRPROOF_DATA_LEN  = SIGNATURE_BYTES + X25519_KEY_BYTES;
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

Link::Ptr Link::request(const Destination& destination,
                        EstablishedCallback on_established,
                        PacketCallback      on_packet) {
    auto link = std::shared_ptr<Link>(new Link(destination, true));
    link->_on_established = std::move(on_established);
    link->_on_packet      = std::move(on_packet);
    link->_status         = HANDSHAKE;

    Bytes request_data;
    request_data << link->_eph_x25519_pub << link->_eph_ed25519_pub;

    /* Link id is the truncated hash of the LINKREQUEST data — both sides
     * can derive it independently. This is the destination hash that the
     * matching LRPROOF will be addressed to. */
    link->_hash = Identity::truncated_hash(request_data);

    Packet pkt(destination, request_data, Type::Packet::LINKREQUEST);
    pkt.pack();

    Transport::register_link(link);
    Transport::broadcast(pkt.raw());
    return link;
}

Link::Ptr Link::validate_request(const Destination& owner,
                                 const Bytes&       request_data,
                                 const Packet&      /*request_packet*/) {
    if (request_data.size() != REQUEST_DATA_LEN) {
        DEBUGF("Link::validate_request: bad data len %zu", request_data.size());
        return nullptr;
    }

    auto link = std::shared_ptr<Link>(new Link(owner, false));
    Bytes peer_x25519_pub  = request_data.left(X25519_KEY_BYTES);
    Bytes peer_ed25519_pub = request_data.mid(X25519_KEY_BYTES, ED25519_KEY_BYTES);
    (void)peer_ed25519_pub;  /* Phase 5 doesn't yet validate initiator signature. */

    /* link_hash is derived identically on both sides from the LINKREQUEST data. */
    link->_hash = Identity::truncated_hash(request_data);

    if (!link->open_session_from_peer_pub(peer_x25519_pub)) return nullptr;
    link->_status = ACTIVE;

    /* Build LRPROOF: signature(link_hash || responder_eph_x25519_pub)
     *               || responder_eph_x25519_pub
     * Signature is by the destination owner's identity. */
    Bytes signed_material;
    signed_material << link->_hash << link->_eph_x25519_pub;
    Bytes signature = owner.identity().sign(signed_material);

    Bytes proof_data;
    proof_data << signature << link->_eph_x25519_pub;

    /* The LRPROOF packet's destination_hash is the link hash, not the owner
     * destination's hash. Register the link with Transport so the next inbound
     * frame for this hash routes here. */
    Transport::register_link(link);

    /* Manually construct the proof packet because Packet::pack() won't know
     * to use the link hash as destination. We hand-build the raw frame:
     *   flags(1) || hops(1) || link_hash(16) || context(1) || proof_data
     */
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
    if (d.size() != LRPROOF_DATA_LEN) {
        DEBUGF("Link::on_proof: bad LRPROOF size %zu", d.size());
        return;
    }
    Bytes signature        = d.left(SIGNATURE_BYTES);
    Bytes peer_x25519_pub  = d.mid(SIGNATURE_BYTES, X25519_KEY_BYTES);

    if (!open_session_from_peer_pub(peer_x25519_pub)) return;

    /* Verify destination identity signed (link_hash || peer_x25519_pub). */
    Bytes signed_material;
    signed_material << _hash << peer_x25519_pub;
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

void Link::on_inbound(const Packet& packet) {
    if (_status != ACTIVE || !_token) return;
    if (packet.packet_type() != Type::Packet::DATA) return;
    Bytes plaintext = _token->decrypt(packet.data());
    if (plaintext.empty()) return;
    if (_on_packet) _on_packet(plaintext, *this);
}

}
