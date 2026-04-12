#include "ureticulum/resource.h"

#include <stdexcept>

#include "ureticulum/cryptography/random.h"
#include "ureticulum/identity.h"
#include "ureticulum/log.h"

namespace RNS {

namespace {
    constexpr uint8_t TAG_HEAD = 0xA0;
    constexpr uint8_t TAG_BODY = 0xA1;
    constexpr size_t  ID_BYTES = 16;
    /* Conservative chunk payload sized to fit comfortably inside the
     * Reticulum MTU after Link encryption overhead. The link layer adds
     * Token framing (~48 bytes) and the wire frame header (~19 bytes), so
     * we cap chunk payload at 256 bytes to leave headroom. */
    constexpr size_t  CHUNK_PAYLOAD = 256;

    void put_u32(Bytes& dst, uint32_t v) {
        dst.append(static_cast<uint8_t>((v >> 24) & 0xFF));
        dst.append(static_cast<uint8_t>((v >> 16) & 0xFF));
        dst.append(static_cast<uint8_t>((v >>  8) & 0xFF));
        dst.append(static_cast<uint8_t>( v        & 0xFF));
    }

    uint32_t get_u32(const uint8_t* p) {
        return (static_cast<uint32_t>(p[0]) << 24)
             | (static_cast<uint32_t>(p[1]) << 16)
             | (static_cast<uint32_t>(p[2]) <<  8)
             |  static_cast<uint32_t>(p[3]);
    }
}

Resource::Resource(const Link::Ptr& link) : _link(link) {}

Resource::Ptr Resource::send(const Link::Ptr& link, const Bytes& payload) {
    if (!link || link->status() != Link::ACTIVE)
        throw std::runtime_error("Resource::send requires an ACTIVE Link");

    auto r        = Ptr(new Resource(link));
    r->_id        = Cryptography::random(ID_BYTES);
    r->_total_size = payload.size();
    r->_status    = Status::SENDING;

    /* Pick chunk size — bounded by the link MTU and our cap. */
    size_t chunk_size = CHUNK_PAYLOAD;
    if (link->mtu() > 0 && link->mtu() < chunk_size + 100) {
        /* Leave 100 bytes headroom for Link/Token/header overhead. */
        chunk_size = (link->mtu() > 100) ? (link->mtu() - 100) : 64;
    }

    size_t chunk_count = payload.empty() ? 0 : ((payload.size() + chunk_size - 1) / chunk_size);
    r->_chunk_count = chunk_count;

    if (chunk_count == 0) {
        /* Empty payload still sends a header so the receiver completes. */
        Bytes head;
        head.append(TAG_HEAD);
        head.append(r->_id.data(), ID_BYTES);
        put_u32(head, 0);
        put_u32(head, 0);
        put_u32(head, 0);
        link->send(head);
        r->_status = Status::COMPLETE;
        return r;
    }

    /* First chunk carries the header fields. */
    Bytes head;
    head.append(TAG_HEAD);
    head.append(r->_id.data(), ID_BYTES);
    put_u32(head, static_cast<uint32_t>(payload.size()));
    put_u32(head, static_cast<uint32_t>(chunk_count));
    put_u32(head, 0);
    size_t first_len = payload.size() < chunk_size ? payload.size() : chunk_size;
    head.append(payload.data(), first_len);
    link->send(head);

    /* Remaining body chunks. */
    for (size_t i = 1; i < chunk_count; ++i) {
        size_t off = i * chunk_size;
        size_t len = (off + chunk_size > payload.size()) ? (payload.size() - off) : chunk_size;
        Bytes body;
        body.append(TAG_BODY);
        body.append(r->_id.data(), ID_BYTES);
        put_u32(body, static_cast<uint32_t>(i));
        body.append(payload.data() + off, len);
        link->send(body);
    }

    r->_status = Status::COMPLETE;  /* fire-and-forget; receiver tracks its own state */
    return r;
}

Resource::Ptr Resource::receive(const Link::Ptr& link,
                                CompleteCallback on_complete,
                                ProgressCallback on_progress) {
    if (!link)
        throw std::runtime_error("Resource::receive requires a Link");
    auto r = Ptr(new Resource(link));
    r->_status      = Status::RECEIVING;
    r->_on_complete = std::move(on_complete);
    r->_on_progress = std::move(on_progress);

    /* Hook into the link's packet callback. The receiver doesn't know the
     * resource id ahead of time — it captures the first chunk that arrives
     * and locks onto that id for the rest of the transfer. */
    auto self = r;
    link->set_packet_callback([self](const Bytes& plaintext, const Link&) {
        self->on_chunk(plaintext);
    });
    return r;
}

void Resource::on_chunk(const Bytes& chunk) {
    if (_status != Status::RECEIVING) return;
    if (chunk.size() < 1 + ID_BYTES) return;
    uint8_t tag = chunk.data()[0];

    if (tag == TAG_HEAD) {
        if (chunk.size() < 1 + ID_BYTES + 12) return;
        Bytes id(chunk.data() + 1, ID_BYTES);
        if (_id.empty()) _id = id;
        else if (_id != id) return;  /* not for us */

        const uint8_t* p = chunk.data() + 1 + ID_BYTES;
        _total_size  = get_u32(p);
        _chunk_count = get_u32(p + 4);
        uint32_t idx = get_u32(p + 8);

        size_t header_size = 1 + ID_BYTES + 12;
        Bytes payload(chunk.data() + header_size, chunk.size() - header_size);
        _chunks[idx] = payload;
        _bytes_received += payload.size();
    }
    else if (tag == TAG_BODY) {
        if (chunk.size() < 1 + ID_BYTES + 4) return;
        Bytes id(chunk.data() + 1, ID_BYTES);
        if (_id.empty() || _id != id) return;

        const uint8_t* p = chunk.data() + 1 + ID_BYTES;
        uint32_t idx = get_u32(p);

        size_t header_size = 1 + ID_BYTES + 4;
        Bytes payload(chunk.data() + header_size, chunk.size() - header_size);
        _chunks[idx] = payload;
        _bytes_received += payload.size();
    }
    else {
        return;
    }

    if (_on_progress) _on_progress(_bytes_received, _total_size);

    /* Complete when we have every chunk. */
    if (_chunk_count > 0 && _chunks.size() == _chunk_count) {
        Bytes assembled;
        for (size_t i = 0; i < _chunk_count; ++i) {
            auto it = _chunks.find(static_cast<uint32_t>(i));
            if (it == _chunks.end()) { _status = Status::FAILED; return; }
            assembled.append(it->second);
        }
        if (assembled.size() != _total_size) {
            _status = Status::FAILED;
            return;
        }
        _status = Status::COMPLETE;
        if (_on_complete) _on_complete(assembled);
    }
    else if (_chunk_count == 0 && _total_size == 0) {
        _status = Status::COMPLETE;
        if (_on_complete) _on_complete(Bytes{});
    }
}

}
