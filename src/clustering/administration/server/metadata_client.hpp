// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef CLUSTERING_ADMINISTRATION_SERVER_METADATA_CLIENT_HPP_
#define CLUSTERING_ADMINISTRATION_SERVER_METADATA_CLIENT_HPP_

#include "clustering/administration/server/server_metadata.hpp"

class auto_reconnector_t;
class network_logger_t;
class last_seen_tracker_t;

/* A `server_metadata_client_t` keeps track of a set of servers. Among its jobs are:
 1. Translating server names to peer IDs
 2. Renaming and permanently removing servers when the user requests it
 3. Automatically trying to reconnect to servers that we've lost contact with
 4. Reporting issues in the issue tracker for missing servers
*/

class server_metadata_client_t : public home_thread_mixin_t {
public:
    typedef change_tracking_map_t<peer_id_t, server_directory_metadata_t>
        servers_directory_metadata_t;

    server_metadata_client_t(
        connectivity_cluster_t *_connectivity_cluster,
        connectivity_cluster_t::run_t *_connectivity_cluster_run,
        mailbox_manager_t *_mailbox_manager,
        clone_ptr_t<watchable_t<servers_directory_metadata_t> > _directory_view,
        boost::shared_ptr<semilattice_readwrite_view_t<servers_semilattice_metadata_t> >
            _semilattice_view);

    /* All of the currently-visible peers, indexed by name. Warning: in the event of a
    name collision, this watchable will not update until the name collision has been
    resolved. The name collision should be resolved automatically in a fraction of a
    second, so this shouldn't be a big deal in practice, but be aware of it. */
    clone_ptr_t<watchable_t<std::map<name_string_t, peer_id_t> > > get_name_map() {
        return name_map.get_watchable();
    }

    /* `rename_server` changes the name of the peer named `old_name` to `new_name`. On
    success, returns `true`. On failure, returns `false` and sets `*error_out` to an
    informative message. */
    bool rename_server(const name_string_t &old_name, const name_string_t &new_name,
        signal_t *interruptor, std::string *error_out);

    /* `permanently_remove_server` permanently removes the server with the given name,
    provided that it is not currently visible. On success, returns `true`. On failure,
    returns `false` and sets `*error_out` to an informative message. */
    bool permanently_remove_server(const name_string_t &name, std::string *error_out);

private:
    void recompute_name_map();

    mailbox_manager_t *mailbox_manager;
    clone_ptr_t<watchable_t<servers_directory_metadata_t> > directory_view;
    boost::shared_ptr<semilattice_readwrite_view_t<servers_semilattice_metadata_t> >
        semilattice_view;

    watchable_variable_t<std::map<name_string_t, peer_id_t> > name_map;

    watchable_t<servers_directory_metadata_t>::subscription_t directory_subs;
    semilattice_readwrite_view_t<servers_semilattice_metadata_t>::subscription_t
        semilattice_subs;

    scoped_ptr_t<auto_reconnector_t> auto_reconnector;
    scoped_ptr_t<network_logger_t> network_logger;
};

#endif /* CLUSTERING_ADMINISTRATION_SERVER_METADATA_CLIENT_HPP_ */

