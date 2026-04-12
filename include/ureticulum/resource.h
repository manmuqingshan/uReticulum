#pragma once

#include <functional>
#include <map>
#include <memory>

#include "ureticulum/bytes.h"
#include "ureticulum/link.h"

namespace RNS {

    /* uReticulum Resource — chunked transfer of an arbitrary Bytes payload
     * over an established Link. This is a custom implementation that does
     * not yet match upstream Reticulum's wire format (which itself isn't
     * standardised in the C++ port). When upstream lands, the wire format
     * will be replaced; the public API should stay close.
     *
     * Wire format:
     *   First chunk:  [TAG_HEAD (1)] [resource_id (16)] [total_size (4 BE)]
     *                 [chunk_count (4 BE)] [chunk_index (4 BE)] [data]
     *   Other chunks: [TAG_BODY (1)] [resource_id (16)] [chunk_index (4 BE)] [data]
     *
     * The header chunk carries the resource size and chunk count so the
     * receiver can pre-allocate. Body chunks carry only the index. The
     * receiver completes the resource when it has seen every index from
     * 0 to chunk_count-1.
     *
     * Out of scope: ACKs, retries, windowing, hash maps, sender resume.
     * The Link itself is the reliability boundary — if the underlying
     * transport drops a chunk, the resource fails. */
    class Resource : public std::enable_shared_from_this<Resource> {
    public:
        using Ptr              = std::shared_ptr<Resource>;
        using CompleteCallback = std::function<void(const Bytes& payload)>;
        using ProgressCallback = std::function<void(size_t bytes_received, size_t total_bytes)>;

        enum class Status { IDLE, SENDING, RECEIVING, COMPLETE, FAILED };

        /* Sender side. Splits payload into chunks sized to fit the link's
         * negotiated MTU and sends them in order. Returns the resource. */
        static Ptr send(const Link::Ptr& link, const Bytes& payload);

        /* Receiver side. The Link's packet callback dispatches into this. */
        static Ptr receive(const Link::Ptr& link,
                           CompleteCallback on_complete,
                           ProgressCallback on_progress = nullptr);

        Status        status() const { return _status; }
        const Bytes&  id()     const { return _id; }
        size_t        size()   const { return _total_size; }

    private:
        Resource(const Link::Ptr& link);
        void on_chunk(const Bytes& chunk);

        Link::Ptr        _link;
        Bytes            _id;
        Status           _status      = Status::IDLE;
        size_t           _total_size  = 0;
        size_t           _chunk_count = 0;
        size_t           _bytes_received = 0;
        std::map<uint32_t, Bytes> _chunks;
        CompleteCallback _on_complete;
        ProgressCallback _on_progress;
    };

}
