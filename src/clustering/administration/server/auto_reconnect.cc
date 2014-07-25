// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/server/auto_reconnect.hpp"

#include "errors.hpp"
#include <boost/bind.hpp>

#include "arch/timing.hpp"
#include "concurrency/wait_any.hpp"

auto_reconnector_t::auto_reconnector_t(
        connectivity_cluster_t *connectivity_cluster_,
        connectivity_cluster_t::run_t *connectivity_cluster_run_,
        clone_ptr_t<watchable_t<change_tracking_map_t<peer_id_t,
            server_directory_metadata_t> > > _directory_view,
        boost::shared_ptr<semilattice_readwrite_view_t<servers_semilattice_metadata_t> >
            _semilattice_view
        ) :
    connectivity_cluster(connectivity_cluster_),
    connectivity_cluster_run(connectivity_cluster_run_),
    directory_view(_directory_view),
    semilattice_view(_semilattice_view),
    directory_subs(boost::bind(&auto_reconnector_t::on_connect_or_disconnect, this))
{
    watchable_t<change_tracking_map_t<peer_id_t, server_semilattice_metadata_t> >
        ::freeze_t freeze(directory_view);
    directory_subs.reset(directory_view, &freeze);
    on_connect_or_disconnect();
}

void auto_reconnector_t::on_connect_or_disconnect() {
    std::map<peer_id_t, server_directory_metadata_t> map =
        directory_view->get().get_inner();
    for (auto it = map.begin(); it != map.end(); it++) {
        if (connected_peers.find(it->first) == connected_peers.end()) {
            auto_drainer_t::lock_t connection_keepalive;
            connectivity_cluster_t::connection_t *connection =
                connectivity_cluster->get_connection(it->first, &connection_keepalive);
            guarantee(connection != NULL);
            connected_peers.insert(std::make_pair(
                it->first,
                std::make_pair(
                    it->second.machine_id,
                    connection->get_peer_address())));
        }
    }
    for (auto it = connected_peers.begin(); it != connected_peers.end();) {
        if (map.find(it->first) == map.end()) {
            auto jt = it;
            ++jt;
            coro_t::spawn_sometime(boost::bind(
                &auto_reconnector_t::try_reconnect, this,
                it->second.first, it->second.second, auto_drainer_t::lock_t(&drainer)));
            connected_peers.erase(it);
            it = jt;
        } else {
            it++;
        }
    }
}

static const int initial_backoff_ms = 50;
static const int max_backoff_ms = 1000 * 15;
static const double backoff_growth_rate = 1.5;

void auto_reconnector_t::try_reconnect(machine_id_t machine,
                                       peer_address_t last_known_address,
                                       auto_drainer_t::lock_t keepalive) {

    cond_t cond;
    semilattice_read_view_t<servers_semilattice_metadata_t>::subscription_t subs(
        boost::bind(&auto_reconnector_t::pulse_if_machine_declared_dead, this, machine, &cond),
        machine_metadata);
    pulse_if_machine_declared_dead(machine, &cond);
    watchable_t<change_tracking_map_t<peer_id_t, server_directory_metadata_t> >
        ::subscription_t subs2(
            boost::bind(&auto_reconnector_t::pulse_if_machine_reconnected,
                this, machine, &cond));
    {
        watchable_t<change_tracking_map_t<peer_id_t, server_directory_metadata_t> >
            ::freeze_t freeze(directory_view);
        subs2.reset(directory_view, &freeze);
        pulse_if_machine_reconnected(machine, &cond);
    }

    wait_any_t interruptor(&cond, keepalive.get_drain_signal());

    int backoff_ms = initial_backoff_ms;
    try {
        while (true) {
            connectivity_cluster_run->join(last_known_address);
            signal_timer_t timer;
            timer.start(backoff_ms);
            wait_interruptible(&timer, &interruptor);
            guarantee(backoff_ms * backoff_growth_rate > backoff_ms, "rounding screwed it up");
            backoff_ms = std::min(static_cast<int>(backoff_ms * backoff_growth_rate), max_backoff_ms);
        }
    } catch (const interrupted_exc_t &) {
        /* ignore; this is how we escape the loop */
    }
}

void auto_reconnector_t::pulse_if_machine_declared_dead(machine_id_t machine, cond_t *c) {
    servers_semilattice_metadata_t md = semilattice_view->get();
    auto it = md.servers.find(machine);
    if (it == md.servers.end()) {
        /* The only way we could have gotten here is if a machine connected
        for the first time and then immediately disconnected, and its directory
        got through but its semilattices didn't. Don't try to reconnect to it
        because there's no way to declare it dead. */
        if (!c->is_pulsed()) {
            c->pulse();
        }
    } else if (it->second.is_deleted()) {
        if (!c->is_pulsed()) {
            c->pulse();
        }
    }
}

void auto_reconnector_t::pulse_if_machine_reconnected(machine_id_t machine, cond_t *c) {
    std::map<peer_id_t, server_directory_metadata_t> map =
        machine_id_translation_table->get().get_inner();
    for (auto it = map.begin(); it != map.end(); it++) {
        if (it->second.machine_id == machine) {
            if (!c->is_pulsed()) {
                c->pulse();
            }
            break;
        }
    }
}

