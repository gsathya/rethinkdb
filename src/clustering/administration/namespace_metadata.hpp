// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef CLUSTERING_ADMINISTRATION_NAMESPACE_METADATA_HPP_
#define CLUSTERING_ADMINISTRATION_NAMESPACE_METADATA_HPP_

#include <map>
#include <set>
#include <string>
#include <utility>

#include "clustering/administration/database_metadata.hpp"
#include "clustering/administration/datacenter_metadata.hpp"
#include "clustering/administration/http/json_adapters.hpp"
#include "clustering/administration/persistable_blueprint.hpp"
#include "clustering/generic/nonoverlapping_regions.hpp"
#include "clustering/reactor/blueprint.hpp"
#include "clustering/reactor/directory_echo.hpp"
#include "clustering/reactor/reactor_json_adapters.hpp"
#include "clustering/reactor/metadata.hpp"
#include "containers/cow_ptr.hpp"
#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "http/json/json_adapter.hpp"
#include "rpc/semilattice/joins/deletable.hpp"
#include "rpc/semilattice/joins/macros.hpp"
#include "rpc/semilattice/joins/map.hpp"
#include "rpc/semilattice/joins/vclock.hpp"
#include "rpc/serialize_macros.hpp"


/* This is the metadata for a single namespace of a specific protocol. */

/* If you change this data structure, you must also update
`clustering/administration/issues/vector_clock_conflict.hpp`. */

class ack_expectation_t {
public:
    ack_expectation_t() : expectation_(0), hard_durability_(true) { }

    explicit ack_expectation_t(uint32_t expectation, bool hard_durability) :
        expectation_(expectation),
        hard_durability_(hard_durability) { }

    uint32_t expectation() const { return expectation_; }
    bool is_hardly_durable() const { return hard_durability_; }

    RDB_DECLARE_ME_SERIALIZABLE;

    bool operator==(ack_expectation_t other) const;

private:
    uint32_t expectation_;
    bool hard_durability_;
};

RDB_SERIALIZE_OUTSIDE(ack_expectation_t);

void debug_print(printf_buffer_t *buf, const ack_expectation_t &x);

class table_config_t {
public:
    /* `regions`, `replica_server_names`, and `director_server_names` have exactly the same number of elements; they are
    aligned with each other. That is, the shard whose boundaries are the first element in `regions` has the the replicas
    in `replica_server_names[0]`, and its director candidates are in `director_server_names[0]`. */
    nonoverlapping_regions_t regions;
    std::vector<std::set<name_string_t> > replica_server_names;
    std::vector<std::set<name_string_t> > director_server_names;
};

RDB_DECLARE_SERIALIZABLE(table_config_t);

class namespace_semilattice_metadata_t {
public:
    namespace_semilattice_metadata_t() { }

    vclock_t<name_string_t> name;
    vclock_t<std::string> primary_key; //TODO this should actually never be changed...
    vclock_t<database_id_t> database;

    vclock_t<table_config_t> table_config;
    vclock_t<name_string_t> director_server_name;
};

RDB_DECLARE_SERIALIZABLE(namespace_semilattice_metadata_t);

RDB_DECLARE_SEMILATTICE_JOINABLE(namespace_semilattice_metadata_t);

RDB_DECLARE_EQUALITY_COMPARABLE(namespace_semilattice_metadata_t);

/* This is the metadata for all of the namespaces of a specific protocol. */
class namespaces_semilattice_metadata_t {
public:
    typedef std::map<namespace_id_t, deletable_t<namespace_semilattice_metadata_t> > namespace_map_t;
    namespace_map_t namespaces;
};

RDB_DECLARE_SERIALIZABLE(namespaces_semilattice_metadata_t);
RDB_DECLARE_SEMILATTICE_JOINABLE(namespaces_semilattice_metadata_t);
RDB_DECLARE_EQUALITY_COMPARABLE(namespaces_semilattice_metadata_t);

class namespaces_directory_metadata_t {
public:
    namespaces_directory_metadata_t() { }
    namespaces_directory_metadata_t(const namespaces_directory_metadata_t &other) {
        *this = other;
    }
    namespaces_directory_metadata_t(namespaces_directory_metadata_t &&other) {
        *this = std::move(other);
    }
    namespaces_directory_metadata_t &operator=(const namespaces_directory_metadata_t &other) {
        reactor_bcards = other.reactor_bcards;
        return *this;
    }
    namespaces_directory_metadata_t &operator=(namespaces_directory_metadata_t &&other) {
        reactor_bcards = std::move(other.reactor_bcards);
        return *this;
    }

    /* This used to say `reactor_business_card_t` instead of
    `cow_ptr_t<reactor_business_card_t>`, but that
    was extremely slow because the size of the data structure grew linearly with
    the number of tables and so copying it became a major cost. Using a
    `boost::shared_ptr` instead makes it significantly faster. */
    typedef std::map<namespace_id_t, directory_echo_wrapper_t<cow_ptr_t<reactor_business_card_t> > > reactor_bcards_map_t;

    reactor_bcards_map_t reactor_bcards;
};

RDB_DECLARE_SERIALIZABLE(namespaces_directory_metadata_t);
RDB_DECLARE_EQUALITY_COMPARABLE(namespaces_directory_metadata_t);

#endif /* CLUSTERING_ADMINISTRATION_NAMESPACE_METADATA_HPP_ */
