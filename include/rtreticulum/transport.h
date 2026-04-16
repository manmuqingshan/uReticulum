#pragma once

#include <functional>
#include <map>
#include <set>
#include <vector>

#include "rtreticulum/bytes.h"
#include "rtreticulum/destination.h"
#include "rtreticulum/hal.h"
#include "rtreticulum/interface.h"

namespace RNS {

    class Identity;
    class Link;
    class Packet;

    class Transport {
    public:
        struct PathEntry {
            Bytes      next_hop;
            uint8_t    hops             = 0;
            double     timestamp        = 0;
            std::shared_ptr<InterfaceImpl> via_interface;
        };

        using AnnounceCallback = std::function<void(const Bytes& destination_hash,
                                                    const Identity& announced_identity,
                                                    const Bytes& app_data)>;
        using LinkRequestCallback = std::function<std::shared_ptr<Link>(
            const Destination& destination, const Bytes& request_data, const Packet& packet)>;

        static void register_interface(std::shared_ptr<InterfaceImpl> iface);
        static void deregister_interface(const std::shared_ptr<InterfaceImpl>& iface);
        static const std::vector<std::shared_ptr<InterfaceImpl>>& interfaces();

        static void        register_destination(const Destination& dest);
        static void        deregister_destination(const Destination& dest);
        static Destination find_destination_from_hash(const Bytes& hash);

        static void                  register_link(const std::shared_ptr<Link>& link);
        static void                  deregister_link(const std::shared_ptr<Link>& link);
        static std::shared_ptr<Link> find_link(const Bytes& link_hash);

        static void set_link_request_handler(LinkRequestCallback cb);

        static void broadcast(const Bytes& raw, const std::shared_ptr<InterfaceImpl>& skip = nullptr);
        static void inbound(const Bytes& raw, const Interface& iface);

        static bool          has_path(const Bytes& destination_hash);
        static uint8_t       hops_to(const Bytes& destination_hash);
        static const PathEntry* lookup_path(const Bytes& destination_hash);
        static void          clear_paths();
        static const std::map<Bytes, PathEntry>& path_table() { return _path_table; }

        static void on_announce(AnnounceCallback cb);
        static void reset();

        /* Lazy-init accessor for the recursive mutex protecting the static
         * state below. Public so the in-file Lock RAII helper can reach it
         * without being a friend. */
        static ur_recursive_mutex_t* mutex();

    private:
        /* HAL-backed recursive mutex. Recursive because the
         * inbound→process_announce→broadcast→loopback peer→handle_incoming→
         * Transport::inbound chain re-enters on the same task when running
         * with the in-process LoopbackInterface. Backed by the HAL (not
         * std::recursive_mutex) because newlib-nano on bare-metal ARM does
         * not ship <mutex>. */
        static ur_recursive_mutex_t* _mutex;

        static std::vector<std::shared_ptr<InterfaceImpl>>      _interfaces;
        static std::map<Bytes, Destination>                     _destinations;
        static std::map<Bytes, PathEntry>                       _path_table;
        static std::map<Bytes, std::shared_ptr<Link>>           _links;
        static std::set<Bytes>                                  _seen_announces;
        static AnnounceCallback                                 _on_announce;
        static LinkRequestCallback                              _on_link_request;

        static bool process_announce(const Packet& packet, const Interface& iface);
    };

}
