// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef CLUSTERING_ADMINISTRATION_SERVER_METADATA_HPP_
#define CLUSTERING_ADMINISTRATION_SERVER_METADATA_HPP_

/* Each server has a name and tags, which are passed on the command line when the server
starts. These can be modified at runtime but are not persisted to disk. Each server
manages its own metadata. If the user wants to modify the metadata, the server that
receives the command will forward it to the server whose metadata is being modified. Each
server will broadcast its current metadata via the directory.

Name conflicts are resolved automatically by renaming one of the servers.

Eventually, we will get rid of the concept of "permanently removing" a server. But until
we do, each server also has an entry in the semilattices. When a server is alive, no
other server should alter its semilattice entry. But if a server is inaccessible, other
servers can modify the entry for two reasons:
 1. If the user wants to permanently remove a server, then other servers may delete its
    entry from the semilattices.
 2. If the inaccessible server is involved in a name conflict, the other servers may
    change the name stored on its semilattice entry.
*/

/* These metadata types should only be accessed directly by the
`server_metadata_server_t` and `server_metadata_client_t`; all other accesses should go
through those two. */

class server_directory_metadata_t {
public:
    /* `true` if the server was permanently removed, but then came back to the cluster.
    If this is `true` then the other fields are meaningless. */
    bool was_permanently_removed;

    /* The server's current name. */
    name_string_t name;

    /* The address to send name-change orders. */
    mailbox_t<void(name_string_t)>::addr_t rename_mailbox;
};

RDB_DECLARE_SERIALIZABLE(server_directory_metadata_t);

class server_semilattice_metadata_t {
public:
    name_string_t name;
};

RDB_DECLARE_SERIALIZABLE(server_semilattice_metadata_t);

class servers_semilattice_metadata_t {
public:
    typedef std::map<machine_id_t, deletable_t<vclock_t<server_semilattice_metadata_t
        > > > server_map_t;
    server_map_t servers;
};

RDB_DECLARE_SERIALIZABLE(servers_semilattice_metadata_t);
RDB_DECLARE_SEMILATTICE_JOINABLE(servers_semilattice_metadata_t);
RDB_DECLARE_EQUALITY_COMPARABLE(servers_semilattice_metadata_t);

/* Each server constructs a `server_metadata_server_t` to publish its metadata, handle
name-change requests, and so on. It also takes care of automatically renaming itself if
necessary to resolve a name conflict. */

class server_metadata_server_t : public home_thread_mixin_t {
public:
    server_metadata_server_t(
        mailbox_manager_t *mm,
        const name_string_t &my_initial_name,
        machine_id_t my_machine_id,
        clone_ptr_t<watchable_t<change_tracking_map_t<peer_id_t,
            server_directory_metadata_t> > > directory_view,
        boost::shared_ptr<semilattice_readwrite_view_t<servers_semilattice_metadata_t> >
            semilattice_view
        );

    clone_ptr_t<watchable_t<server_directory_metadata_t> > get_directory();

    /* This errors if we were permanently removed. Check first with
    `get_is_permanently_removed()`. */
    name_string_t get_my_name();

    bool get_is_permanently_removed();

private:
    void rename(name_string_t new_name);

    mailbox_manager_t *mailbox_manager;
    mailbox_t<void(name_string_t)> rename_mailbox;
    watchable_variable_t<server_directory_metadata_t> directory_var;

    watchable_t<change_tracking_map_t<peer_id_t, server_directory_metadata_t> >::
        subscription_t directory_subs;
    semilattice_readwrite_view_t<servers_semilattice_metadata_t>::subscription_t
};

/* Each server and each proxy constructs a `server_metadata_client_t` to read the
metadata, send name-change requests to the right server, and so on. It also
automatically renames entries in the semilattices if necessary to resolve a name
conflict. */

class server_metadata_client_t : public home_thread_mixin_t {
public:
    server_metadata_client_t(
        mailbox_manager_t *mm,
        clone_ptr_t<watchable_t<change_tracking_map_t<peer_id_t,
            server_directory_metadata_t> > > directory_view);

    /* All of the currently-visible peers, indexed by name. Name conflicts will never
    appear in this map. */
    clone_ptr_t<watchable_t<std::map<name_string_t, peer_id_t> > > get_name_map();

    /* `rename_server` changes the name of the peer named `old` to `new`. On success,
    returns `true`. On failure, returns `false` and sets `*error_out` to an informative
    message. */
    bool rename_server(const name_string_t &old, const name_string_t &new,
        signal_t *interruptor, std::string *error_out);

    /* Peers that are missing but still appear 
};

#endif /* CLUSTERING_ADMINISTRATION_SERVER_METADATA_HPP_ */

