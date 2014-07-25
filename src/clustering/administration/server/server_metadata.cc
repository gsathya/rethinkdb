// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/server_metadata.hpp"

/* `server_priority_le()` decides which of two servers should get priority in a name
collision. It returns `true` if the priority of the first server is less than or equal to
the priority of the second server. */
static bool server_priority_le(
        const change_tracking_map_t<peer_id_t, server_directory_metadata_t> &directory,
        machine_id_t machine1, machine_id_t machine2)
{
    ime_t startup_time_1 = 0, startup_time_2 = 0;
    for (auto it = directory.servers.begin(); it != directory.servers.end(); ++it) {
        if (it->first == machine1) {
            startup_time_1 = it->second.startup_time;
        }
        if (it->first == machine2) {
            startup_time_2 = it->second.startup_time;
        }
    }
    return (startup_time_1 > startup_time_2) ||
        ((startup_time_1 == startup_time_2) && (machine1 <= machine2));
}

/* `make_renamed_name()` generates an alternative name based on the input name.
Ordinarily it just appends `_renamed`. But if the name already has a `_renamed` suffix,
then it starts appending numbers; i.e. `_renamed2`, `_renamed3`, and so on. */
static name_string_t make_renamed_name(const name_string_t &name) {
    static const std::string suffix = "_renamed";
    std::string str = name.str();

    /* Parse the previous name. In particular, determine if it already has a suffix and
    what the number associated with that suffix is. */
    int num_end_digits = 0;
    while (num_end_digits < str.length() &&
           str[str.length()-1-num_end_digits] > '0' &&
           str[str.length()-1-num_end_digits] < '9') {
        num_end_digits++;
    }
    int suffix_start = str.length() - num_end_digits - suffix.length();
    bool has_suffix = suffix_start >= 0 &&
        (str.compare(suffix_start, suffix.length(), suffix) == 0);
    uint64_t end_digits_value;
    if (has_suffix) {
        if (num_end_digits > 0) {
            bool ok = strtou64_strict(str.substr(str.length() - num_end_digits),
                10, &end_digits_value);
            guarantee(ok, "A string made of digits should parse as an integer");
        } else {
            end_digits_value = 1;
        }
    }

    /* Generate the new name */
    std::string new_str;
    if (!has_suffix) {
        new_str = str + suffix;
    } else {
        new_str = strprintf("%s%s%" PRIu64, end_digits_value+1);
    }
    name_string_t new_name;
    bool ok = new_name.assign_value(new_str);
    guarantee(ok, "A valid name plus some letters and digits should be a valid name");

    guarantee(new_name != name);
    return new_name;
}

server_metadata_server_t::server_metadata_server_t(
        mailbox_manager_t *_mailbox_manager,
        const name_string_t &_my_initial_name,
        machine_id_t _my_machine_id,
        clone_ptr_t<watchable_t<change_tracking_map_t<peer_id_t,
            server_directory_metadata_t> > > _directory_view,
        boost::shared_ptr<semilattice_readwrite_view_t<servers_semilattice_metadata_t> >
            _semilattice_view) :
    mailbox_manager(_mailbox_manager),
    rename_mailbox(mailbox_manager,
        [this](const name_string_t &name, const mailbox_t<void()>::addr_t &addr) {
            this->on_rename_request(name, addr);
        }),
    my_machine_id(_my_machine_id),
    startup_time(get_secs()),
    directory_var(server_directory_metadata_t()),
    directory_view(_directory_view),
    semilattice_view(_semilattice_view),
    semilattice_subs([this]() {
        /* We have to call `on_semilattice_change()` in a coroutine because it might
        make changes to the semilattices. */
        auto_drainer_t::lock_t keepalive(&drainer);
        coro_t::spawn_sometime([this, keepalive /* important to capture */]() {
            on_semilattice_change();
            });
        })
{
    /* Prepare a directory entry. */
    server_directory_metadata_t dir_metadata;
    dir_metadata.machine_id = my_machine_id;
    dir_metadata.rename_addr = rename_mailbox.get_address();
    dir_metadata.startup_time = startup_time;
    directory_var.set_value(dir_metadata);

    /* If we don't have an entry in the semilattices, create one. */
    servers_semilattice_metadata_t metadata = semilattice_view->get();
    if (metadata.servers.count(_my_machine_id) == 0) {
        server_semilattice_metadata_t my_entry;
        /* Don't initialize `my_entry.name`; that's the job of `rename_me()`. */
        metadata.servers[_my_machine_id] =
            deletable_t<vclock_t<server_semilattice_metadata_t> >(
                vclock_t<server_semilattice_metadata_t>(
                    my_entry, _my_machine_id));
        semilattice_view->join(metadata);
    }

    /* Set up our name. */
    rename_me(my_name);

    /* Check for name collisions or if we were permanently removed. */
    semilattice_subs.reset(semilattice_view);
    on_semilattice_change();
}

void server_metadata_server_t::rename_me(const name_string_t &new_name) {
    ASSERT_FINITE_CORO_WAITING;
    if (new_name != my_name) {
        my_name = new_name;
        servers_semilattice_metadata_t metadata = semilattice_view->get();
        deletable_t<vclock_t<server_semilattice_metadata_t> > *entry =
            &metadata.servers.at(my_machine_id);
        if (!entry->is_deleted()) {
            server_semilattice_metadata_t my_entry;
            my_entry.name = new_name;
            *entry->get_mutable() = entry->get_ref()->make_new_version(
                my_entry, my_machine_id);
        }
    }
}

void server_metadata_server_t::on_rename_request(const name_string_t &new_name,
                                                 mailbox_t<void()>::addr_t ack_addr) {
    rename_me(new_name);
    on_semilattice_change();   /* check if we caused a name collision */

    /* Send an acknowledgement to the server that initiated the request */
    auto_drainer_t::lock_t keepalive(&drainer);
    coro_t::spawn_sometime([this, keepalive /* important to capture */, ack_addr]() {
        send(this->mailbox_manager, ack_addr);
    });
}

void server_metadata_server_t::on_semilattice_change() {
    ASSERT_FINITE_CORO_WAITING;
    servers_semilattice_metadata_t sl_metadata = semilattice_view->get();
    guarantee(metadata.servers.count(my_machine_id) == 1);

    /* Check if we've been permanently removed */
    if (sl_metadata.servers.at(my_machine_id).is_deleted()) {
        permanently_removed_cond.pulse_if_not_already_pulsed();
        return;
    }

    /* Check for any name collisions */
    bool must_rename_myself = false;
    std::multiset<name_string_t> names_in_use;
    for (auto it = sl_metadata.servers.begin();
              it != sl_metadata.servers.end;
            ++it) {
        if (it->second.is_deleted()) continue;
        name_string_t name = it->second.get_ref().get_ref().name;
        names_in_use.insert(name);
        if (it->first == my_machine_id) {
            guarantee(name == my_name);
        } else if (name == my_name) {
            directory_view->apply_read(
                [&](const change_tracking_map_t<peer_id_t,
                        server_directory_metadata_t> *directory) {
                    must_rename_myself = server_priority_le(
                        *directory, my_machine_id, it->first);
                });
        }
    }

    if (must_rename_myself) {
        /* There's a name collision, and we're the one being renamed. */
        name_string_t new_name = my_name;
        while (names_in_use.count(new_name) != 0) {
            /* Keep trying new names until we find one that isn't in use. If our original
            name is `foo`, repeated calls to `make_renamed_name` will yield
            `foo_renamed`, `foo_renamed2`, `foo_renamed3`, etc. */
            new_name = make_renamed_name(new_name);
        }
        rename_me(new_name);
    }
}

server_metadata_client_t::server_metadata_client_t(
        mailbox_manager_t *_mailbox_manager,
        clone_ptr_t<watchable_t<change_tracking_map_t<peer_id_t,
            server_directory_metadata_t> > > _directory_view,
        boost::shared_ptr<semilattice_readwrite_view_t<servers_semilattice_metadata_t> >
            _semilattice_view) :
    mailbox_manager(_mailbox_manager),
    directory_view(_directory_view),
    semilattice_view(_semilattice_view)
    directory_subs([this]() { this->recompute_name_map(); }),
    semilattice_subs([this]() { this->recompute_name_map(); })
{
    watchable_t<servers_directory_metadata_t>::freeze_t freeze(directory_view);
    directory_subs.reset(directory_view, &freeze);
    semilattice_subs.reset(semilattice_view);
    recompute_name_map();
}

bool server_metadata_client_t::rename_server(const name_string_t &old_name,
        const name_string_t &new_name, signal_t *interruptor, std::string *error_out) {

    /* We can produce this error message for several different reasons, so it's stored in
    a local variable instead of typed out multiple times. */
    std::string lost_contact_message = strprintf("Cannot rename server `%s` because we "
        "lost contact with it.", old_name.c_str())

    /* Look up `new_name` and make sure it's not currently in use. Also check if
    `old_name` is present; that information helps provide better error messages. */
    bool old_name_in_sl = false, new_name_in_sl = false;
    servers_semilattice_metadata_t sl_metadata = semilattice_view->get();
    for (auto it = sl_metadata.servers.begin(); it != sl_metadata.servers.end(); ++it) {
        if (!it->second.is_deleted()) {
            it_name = it->second.get_ref().get_ref().name;
            if (it_name == old_name) old_name_present = true;
            if (it_name == new_name) new_name_present = true;
        }
    }

    /* Look up `old_name` and figure out which peer it corresponds to */
    peer_id_t peer;
    name_map.apply_read([&](const std::map<name_string_t, peer_id_t> *map) {
        auto it = map->find(old_name);
        if (it != map->end()) {
            peer = it->second;
        }
    });
    if (peer.is_nil()) {
        *error_out = old_name_in_sl
            ? lost_contact_message
            : strprintf("Server `%s` does not exist.", old_name.c_str());
        return false;
    }

    if (old_name == new_name) {
        /* This is a no-op */
        return true;
    }

    if (new_name_in_sl) {
        *error_out = strprintf("Server `%s` already exists.", new_name.c_str());
        return false;
    }

    server_directory_metadata_t::rename_mailbox_t::addr_t rename_addr;
    directory_view->apply_read(
        [&](const servers_directory_metadata_t *dir_metadata) {
            auto it = dir_metadata->get_inner().find(peer);
            if (it != dir_metadata->get_inner().end()) {
                rename_addr = it->second.rename_addr;
            }
        });
    if (rename_addr.is_nil()) {
        *error_out = lost_contact_message;
        return false;
    }

    cond_t got_reply;
    mailbox_t<void()> ack_mailbox([&]() { got_reply.pulse(); });
    disconnect_watcher_t disconnect_watcher(mailbox_manager, rename_addr.get_peer());
    send(mailbox_manager, rename_addr, new_name, ack_mailbox.get_addr());
    wait_any_t waiter(&got_reply, &disconnect_watcher);
    wait_interruptible(&waiter, interruptor);

    if (!got_reply.is_pulsed()) {
        *error_out = lost_contact_message;
        return false;
    }

    return true; 
}

bool server_metadata_client_t::permanently_remove_server(const name_string_t &name,
        std::string *error_out) {
    std::set<machine_id_t> machine_ids;
    servers_semilattice_metadata_t sl_metadata = semilattice_view->get();
    for (auto it = sl_metadata.servers.begin(); it != sl_metadata.servers.end(); ++it) {
        if (!it->second.is_deleted()) {
            if (it->second.get_ref().get_ref().name == name) {
                machine_ids.insert(it->first);
            }
        }
    }
    if (machine_ids.empty()) {
        *error_out = strprintf("Server `%s` does not exist.", name.c_str());
        return false;
    }
    
    bool is_visible = false;
    directory_view->apply_read(
        [&](const servers_directory_metadata_t *dir_metadata) {
            for (auto it = dir_metadata->get_inner().first();
                      it != dir_metadata->get_inner().end();
                    ++it) {
                if (machine_ids.count(it->second.machine_id) != 0) {
                    is_visible = true;
                    break;
                }
            }
        }); 
    if (is_visible) {
        *error_out = strprintf("Server `%s` is currently connected to the cluster, so "
            "it should not be permanently removed.", name.c_str());
        return false;
    }

    for (const machine_id_t &machine_id : machine_ids) {
        sl_metadata.servers.at(machine_id).mark_deleted();
    }
    semilattice_view->join(sl_metadata);
    return true;
}

void server_metadata_client_t::recompute_name_map() {
    std::map<name_string_t, peer_id_t> new_name_map;
    bool name_collision = false;
    servers_semilattice_metadata_t sl_metadata = semilattice_view->get();
    directory_view->apply_read([&](const servers_directory_metadata_t *dir_metadata) {
        for (auto dir_it = dir_metadata->begin();
                  dir_it != dir_metadata->end();
                ++dir_it) {
            auto sl_it = sl_metadata.servers->find(dir_it->second.machine_id);
            if (sl_it == sl_metadata.servers->end()) {
                /* This situation is unlikely, but it could occur briefly during startup
                */
                continue;
            }
            if (sl_it->second.is_deleted()) {
                continue;
            }
            name_string_t name = sl_it->second.get_ref().get_ref().name;
            auto res = new_name_map.insert(std::make_pair(name, it->first));
            if (res.second) {
                name_collision = true;
                break;
            }
        }
    });
    if (!name_collision) {
        name_map.set_value(new_name_map);
    }
}

