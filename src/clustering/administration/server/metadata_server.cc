// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/server/metadata_server.hpp"

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

