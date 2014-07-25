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
we do, each server also has an entry in the semilattices. This entry should only be
modified by the server itself, except in one case: if the user orders another server to
permanently remove it. */

/* Only the code in `clustering/administration/server/` should manipulate these types
directly. Other code should use the interfaces provided by `server_metadata_client_t` and
`server_metadata_server_t`. */

class server_directory_metadata_t {
public:
    /* The server's machine ID. This never changes. */
    machine_id_t machine_id;

    /* The address to send name-change orders. */
    typedef mailbox_t<void(name_string_t, mailbox_t<void()>::addr_t)> rename_mailbox_t;
    rename_mailbox_t::addr_t rename_addr;

    /* The time when the server started up. Used for resolving name collisions; the
    server that's been alive longer gets to keep the name. */
    time_t startup_time;
};

RDB_DECLARE_SERIALIZABLE(server_directory_metadata_t);

class server_semilattice_metadata_t {
public:
    name_string_t name;
};

RDB_DECLARE_SERIALIZABLE(server_semilattice_metadata_t);

class servers_semilattice_metadata_t {
public:
    /* Because this name is never changed except by the server that it describes, it's
    impossible for this vector clock to be in conflict. */
    typedef std::map<machine_id_t, deletable_t<vclock_t<server_semilattice_metadata_t
        > > > server_map_t;
    server_map_t servers;
};

RDB_DECLARE_SERIALIZABLE(servers_semilattice_metadata_t);
RDB_DECLARE_SEMILATTICE_JOINABLE(servers_semilattice_metadata_t);
RDB_DECLARE_EQUALITY_COMPARABLE(servers_semilattice_metadata_t);

#endif /* CLUSTERING_ADMINISTRATION_SERVER_METADATA_HPP_ */

