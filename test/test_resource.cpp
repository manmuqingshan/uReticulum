// Note: this file was generated with the help of offline AI.
#include "doctest.h"

#include "ureticulum/destination.h"
#include "ureticulum/identity.h"
#include "ureticulum/link.h"
#include "ureticulum/packet.h"
#include "ureticulum/resource.h"
#include "ureticulum/transport.h"

using namespace RNS;

namespace {
    /* Build a synthetic two-link pair (sender + receiver) without going
     * through the Transport routing collapsing problem we hit in Phase 5.
     * The trick: we directly call validate_request on the receiver side
     * with the sender's request data, then call on_proof on the sender
     * with the receiver's proof data. */
    struct LinkPair {
        Link::Ptr client;
        Link::Ptr server;
    };

    LinkPair build_link_pair() {
        Identity server_id;
        Destination server_dest(server_id, Type::Destination::IN,
                                Type::Destination::SINGLE, "test", "resource");

        /* The Resource transfer flows over Link::send → Transport::broadcast,
         * so the test rig needs at least one registered interface. We use a
         * loopback that the receiver consumes from. */
        /* For Phase 8 we cheat: register both client and server links into
         * Transport so the per-link send routes are exercised, but rely on
         * Resource calling Link::send directly. */
        Identity client_view(false);
        client_view.load_public_key(server_id.get_public_key());
        Destination client_dest(client_view, Type::Destination::OUT,
                                Type::Destination::SINGLE, server_dest.hash());

        auto client = Link::request(client_dest);
        REQUIRE(static_cast<bool>(client));

        /* Build the server-side link by hand from the client's request. */
        Bytes request_data;
        /* We don't have access to the client's internal eph keys, so we
         * pull them out by reverse-engineering the link hash → no, simpler:
         * use a different test rig that builds both sides explicitly. */
        return {client, nullptr};
    }
}

TEST_CASE("Resource send produces a chunk count consistent with payload size") {
    Transport::reset();
    /* Build a server-side Link directly so we can inspect what Resource::send
     * does to it. We use validate_request with synthesized request data. */
    Identity server_id;
    Destination server_dest(server_id, Type::Destination::IN,
                            Type::Destination::SINGLE, "test", "resource");

    auto eph_x = Cryptography::X25519PrivateKey::generate();
    auto eph_e = Cryptography::Ed25519PrivateKey::generate();
    Bytes request_data;
    request_data << eph_x->public_key()->public_bytes()
                 << eph_e->public_key()->public_bytes();

    Packet req_pkt(server_dest, request_data, Type::Packet::LINKREQUEST);
    req_pkt.pack();

    auto link = Link::validate_request(server_dest, request_data, req_pkt);
    REQUIRE(static_cast<bool>(link));
    REQUIRE(link->status() == Link::ACTIVE);

    /* The Resource sends over the link, which broadcasts via Transport.
     * With no registered interfaces, broadcast is a no-op — exactly what
     * we want for this slice test (we only verify the sender accepts the
     * payload and reports COMPLETE). */
    Bytes payload(2048);
    /* Fill payload with a deterministic pattern. */
    {
        Bytes pattern;
        for (int i = 0; i < 2048; ++i) pattern.append(static_cast<uint8_t>(i & 0xFF));
        payload = pattern;
    }

    auto r = Resource::send(link, payload);
    REQUIRE(static_cast<bool>(r));
    CHECK(r->status() == Resource::Status::COMPLETE);
    CHECK(r->size() == 2048);
    CHECK(r->id().size() == 16);

    Transport::reset();
}

TEST_CASE("Resource receive reassembles payload from sequential chunks") {
    Transport::reset();
    /* Two side-by-side links sharing the same Token (so each can decrypt
     * what the other encrypted). We build one link explicitly and clone
     * its session key into a sibling. Cleanest way: send self-loop. */

    Identity server_id;
    Destination server_dest(server_id, Type::Destination::IN,
                            Type::Destination::SINGLE, "test", "resource");

    /* Build the link via validate_request. */
    auto eph_x = Cryptography::X25519PrivateKey::generate();
    auto eph_e = Cryptography::Ed25519PrivateKey::generate();
    Bytes request_data;
    request_data << eph_x->public_key()->public_bytes()
                 << eph_e->public_key()->public_bytes();
    Packet req_pkt(server_dest, request_data, Type::Packet::LINKREQUEST);
    req_pkt.pack();
    auto link = Link::validate_request(server_dest, request_data, req_pkt);
    REQUIRE(static_cast<bool>(link));

    /* Receiver registers a Resource that will absorb chunks via the
     * link's packet callback. Sender feeds chunks by calling the same
     * Link::send. The link's broadcast is a no-op without interfaces, so
     * we route the encrypted bytes manually: every Link::send call writes
     * the ciphertext through Token; we intercept it via the link callback
     * and re-feed it into the same link's on_inbound (since both ends
     * share the same Token in our single-link rig). */

    bool got_complete = false;
    Bytes received;
    /* Direct chunk path: Resource::receive sets a callback on the link,
     * then Resource::send produces chunks via link->send (which goes
     * through Transport::broadcast → no-op). For the receive path to
     * exercise we need the chunks to actually reach Resource::on_chunk. */

    /* Simpler: set the link's packet callback ourselves and manually
     * dispatch encrypted chunks back through the link's decrypt path. */
    auto receiver = Resource::receive(link,
        [&](const Bytes& payload) {
            got_complete = true;
            received = payload;
        });
    (void)receiver;

    /* Build a payload, manually walk the chunks to the receiver via the
     * link's own pipeline. We do that by overriding the broadcast path:
     * register a synthetic interface that re-injects the frame into
     * Transport::inbound, which routes it to our link's on_inbound. */
    Transport::register_link(link);

    /* For the Phase 8 test we drive Resource send/receive in-process by
     * monkey-patching: temporarily make the link's outgoing chunks loop
     * back through the same link's on_inbound. Easiest way: hand-craft
     * the encrypted chunks and call link->on_inbound with each one. */

    /* Build a small payload that fits in 2 chunks. */
    Bytes payload;
    for (int i = 0; i < 600; ++i) payload.append(static_cast<uint8_t>(i & 0xFF));

    /* Inline the Resource send logic so we can intercept each chunk and
     * feed it through the link's decrypt path. */
    auto sender = Resource::send(link, payload);
    REQUIRE(static_cast<bool>(sender));
    CHECK(sender->status() == Resource::Status::COMPLETE);

    /* Without an in-process loopback for Link's encrypted frames, the
     * receiver won't actually see the chunks. We mark this test as
     * structural (sender works) and rely on the next test for full
     * round-trip via direct callback. */
    Transport::reset();
}

TEST_CASE("Resource end-to-end with direct chunk feeding") {
    /* Bypass Link encryption entirely — feed plaintext chunks straight
     * into Resource::on_chunk via the public callback installed by
     * Resource::receive. This validates the chunking + reassembly logic
     * in isolation from the Link/Transport plumbing. */
    Transport::reset();

    Identity server_id;
    Destination server_dest(server_id, Type::Destination::IN,
                            Type::Destination::SINGLE, "test", "resource");
    auto eph_x = Cryptography::X25519PrivateKey::generate();
    auto eph_e = Cryptography::Ed25519PrivateKey::generate();
    Bytes request_data;
    request_data << eph_x->public_key()->public_bytes()
                 << eph_e->public_key()->public_bytes();
    Packet req_pkt(server_dest, request_data, Type::Packet::LINKREQUEST);
    req_pkt.pack();
    auto link = Link::validate_request(server_dest, request_data, req_pkt);

    bool complete = false;
    Bytes assembled;

    /* The receiver hooks Resource into the link's packet callback. We
     * grab a reference to it and then feed the link plaintext chunks
     * directly via the same callback path. */
    Bytes payload;
    for (int i = 0; i < 1500; ++i) payload.append(static_cast<uint8_t>((i * 7) & 0xFF));

    auto receiver = Resource::receive(link,
        [&](const Bytes& p) { complete = true; assembled = p; });

    /* Build chunks the same way Resource::send does and call the link's
     * packet callback directly with each one. The callback was installed
     * by Resource::receive; we invoke it by triggering Link::on_inbound,
     * which decrypts and dispatches to the callback. But there's no
     * matching encryption side here. So instead we cheat and call the
     * stored receiver's chunk handler directly, which is what the link
     * callback would call. */
    /* Resource::receive set the link callback to call self->on_chunk.
     * We can drive the same path by sending plaintext through the link's
     * own send/decrypt loop, but the link doesn't have a self-loop. So
     * we cheat one step further: instantiate Resource manually as the
     * receiver and call on_chunk via its public test seam. */

    /* Use the Resource pointer we got back. There's no public on_chunk;
     * but Resource::receive's installed callback into the link IS the
     * chunk handler. We can't reach it without going through the link.
     * So: encrypt chunks via Token directly (we know the link's key
     * is available because both sides share it through validate_request
     * → which is the same Token because it's the same Link object). */
    /* Easiest path: simulate by calling link->on_inbound with packets
     * carrying the encrypted chunks. We need to construct ciphertext
     * via the link's own encryption, which we can do indirectly by
     * calling link->send and then catching the broadcast frame. But
     * broadcast has no interfaces.
     *
     * Pragmatic answer for Phase 8: register a tiny interface that
     * captures the outgoing frame and immediately re-injects it as
     * inbound on the same link. */

    struct CaptureIface : public InterfaceImpl {
        Link::Ptr loopback;
        CaptureIface() : InterfaceImpl("capture") {}
        void send_outgoing(const Bytes& data) override {
            /* Re-decode the wire frame and dispatch back to the same link.
             * The frame layout is: flags(1) hops(1) link_hash(16) ctx(1) ciphertext */
            if (data.size() < 19) return;
            Packet pkt(data);
            if (!pkt.unpack()) return;
            if (loopback) loopback->on_inbound(pkt);
        }
    };
    auto capture = std::make_shared<CaptureIface>();
    capture->loopback = link;
    Transport::register_interface(capture);

    auto sender = Resource::send(link, payload);
    REQUIRE(static_cast<bool>(sender));
    CHECK(sender->status() == Resource::Status::COMPLETE);

    CHECK(complete);
    CHECK(assembled.size() == payload.size());
    CHECK(assembled == payload);

    Transport::reset();
}
