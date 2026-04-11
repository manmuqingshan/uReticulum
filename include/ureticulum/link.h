#pragma once

#include <functional>
#include <memory>

#include "ureticulum/bytes.h"
#include "ureticulum/cryptography/ed25519.h"
#include "ureticulum/cryptography/token.h"
#include "ureticulum/cryptography/x25519.h"
#include "ureticulum/destination.h"
#include "ureticulum/identity.h"
#include "ureticulum/type.h"

namespace RNS {

    class Packet;

    /* Phase 5 cut: minimal Link with handshake + symmetric data plane.
     *
     * Wire format (single-MTU, no MTU discovery):
     *   LINKREQUEST data : ephemeral_x25519_pub (32) || ephemeral_ed25519_pub (32)
     *   LRPROOF data     : signature (64) || responder_ephemeral_x25519_pub (32)
     *
     * Out of scope for Phase 5: link MTU discovery, RTT measurement,
     * keep-alive packets, watchdog timeouts, ratchets, link teardown
     * negotiation, request/response RPC. Each of those is a separate
     * later-phase work item. */
    class Link : public std::enable_shared_from_this<Link> {
    public:
        using Ptr                 = std::shared_ptr<Link>;
        using EstablishedCallback = std::function<void(const Link& link)>;
        using PacketCallback      = std::function<void(const Bytes& plaintext, const Link& link)>;

        enum Status { PENDING, HANDSHAKE, ACTIVE, CLOSED };

        /* Initiator side: build a link request to a known destination and
         * register the link with Transport. The link enters PENDING; it
         * transitions to ACTIVE once an LRPROOF arrives. */
        static Ptr request(const Destination& destination,
                           EstablishedCallback on_established = nullptr,
                           PacketCallback      on_packet      = nullptr);

        /* Responder side: validate an incoming LINKREQUEST against a local
         * destination and, if valid, build the link, derive keys, and emit
         * the LRPROOF. Returns the new link or nullptr on failure. */
        static Ptr validate_request(const Destination& owner,
                                    const Bytes&       request_data,
                                    const Packet&      request_packet);

        /* Send an encrypted application payload over the link. */
        void send(const Bytes& plaintext);

        /* Called by Transport when a frame addressed to this link's hash
         * arrives. Decrypts and dispatches to the packet callback. */
        void on_inbound(const Packet& packet);

        /* Called by Transport when an LRPROOF arrives that matches this
         * (initiator-side) link's pending hash. Completes the handshake. */
        void on_proof(const Packet& proof_packet);

        const Bytes&  hash()      const { return _hash; }
        Status        status()    const { return _status; }
        bool          initiator() const { return _initiator; }

        void set_packet_callback(PacketCallback cb)            { _on_packet = std::move(cb); }
        void set_established_callback(EstablishedCallback cb)  { _on_established = std::move(cb); }

    private:
        Link(const Destination& destination, bool initiator);
        void derive_keys();
        bool open_session_from_peer_pub(const Bytes& peer_x25519_pub);

        Destination _destination;
        bool        _initiator     = false;
        Status      _status        = PENDING;

        Cryptography::X25519PrivateKey::Ptr  _eph_x25519_prv;
        Cryptography::Ed25519PrivateKey::Ptr _eph_ed25519_prv;
        Bytes _eph_x25519_pub;
        Bytes _eph_ed25519_pub;

        Bytes _peer_x25519_pub;
        Bytes _shared_session_key;
        Bytes _hash;          /* link_id, used as destination hash on the wire */

        std::shared_ptr<Cryptography::Token> _token;

        EstablishedCallback _on_established;
        PacketCallback      _on_packet;
    };

}
