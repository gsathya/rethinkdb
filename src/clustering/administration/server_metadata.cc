// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/server_metadata.hpp"

server_metadata_server_t::server_metadata_server_t(
        mailbox_manager_t *_mm,
        const name_string_t &_my_initial_name,
        machine_id_t _my_machine_id,
        clone_ptr_t<watchable_t<change_tracking_map_t<peer_id_t,
            server_directory_metadata_t> > > _directory_view,
        boost::shared_ptr<semilattice_readwrite_view_t<servers_semilattice_metadata_t> >
            _semilattice_view) :
    mailbox_manager(_mm),
    rename_mailbox(_mm, [this](const name_string_t &name) { rename(name); })
{
    server_directory_metadata_t initial_metadata;
    initial_metadata.name = _my_initial_name;
    initial_metadata.rename_mailbox = rename_mailbox.get_address();

    servers_semilattice_metadata_t current_metadata = semilattice_view->get();
    auto it = current_metadata.servers.find(my_machine_id);
    if (it != current_metadata.servers.end()) {
        
    } else {
    }
}
