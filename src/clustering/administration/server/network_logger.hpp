// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef CLUSTERING_ADMINISTRATION_NETWORK_LOGGER_HPP_
#define CLUSTERING_ADMINISTRATION_NETWORK_LOGGER_HPP_

#include <map>
#include <set>
#include <string>

#include "clustering/administration/machine_metadata.hpp"
#include "clustering/administration/metadata.hpp"

/* This class is responsible for writing log messages when a peer connects or
disconnects from us */

class network_logger_t {
public:
    network_logger_t(
        peer_id_t us,
        clone_ptr_t<watchable_t<change_tracking_map_t<peer_id_t,
            server_directory_metadata_t> > > _directory_view,
        boost::shared_ptr<semilattice_readwrite_view_t<servers_semilattice_metadata_t> >
            _semilattice_view);

private:
    void on_change();
    std::string pretty_print_machine(machine_id_t id);

    peer_id_t us;
    clone_ptr_t<watchable_t<change_tracking_map_t<peer_id_t,
        server_directory_metadata_t> > > directory_view;
    boost::shared_ptr<semilattice_readwrite_view_t<servers_semilattice_metadata_t> >
        semilattice_view;

    watchable_t<change_tracking_map_t<peer_id_t, server_directory_metadata_t> >
        ::subscription_t directory_subscription;
    semilattice_read_view_t<servers_semilattice_metadata_t>::subscription_t
        semilattice_subscription;

    /* Whenever the directory changes, we compare the directory to
    `connected_servers` and `connected_proxies` to see what machines have
    connected or disconnected. */
    std::set<machine_id_t> connected_servers;
    std::set<peer_id_t> connected_proxies;
};

#endif /* CLUSTERING_ADMINISTRATION_NETWORK_LOGGER_HPP_ */
