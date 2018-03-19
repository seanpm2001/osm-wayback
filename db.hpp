#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#define RAPIDJSON_HAS_STDSTRING 1
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include "rapidjson/document.h"
#pragma GCC diagnostic pop

#include "rocksdb/db.h"
#include <rocksdb/table.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/cache.h>
#include <rocksdb/write_batch.h>

#include <osmium/osm/types.hpp>
#include <osmium/visitor.hpp>

#include <chrono>

#include "pbf_json_encoding.hpp"

const std::string make_lookup(const int64_t osm_id, const int version){
  return std::to_string(osm_id) + "!" + std::to_string(version);
}

const bool STORE_GEOMETRIES = true;

class ObjectStore {
    rocksdb::DB* m_db;
    rocksdb::ColumnFamilyHandle* m_cf_ways;
    rocksdb::ColumnFamilyHandle* m_cf_nodes;
    rocksdb::ColumnFamilyHandle* m_cf_relations;
    rocksdb::WriteOptions m_write_options;

    rocksdb::WriteBatch m_buffer_batch;

    void flush_family(const std::string type, rocksdb::ColumnFamilyHandle* cf) {
        const auto start = std::chrono::steady_clock::now();
        std::cerr << std::endl << "Flushing " << type << "..." ;
        m_db->Flush(rocksdb::FlushOptions{}, cf);
        const auto end = std::chrono::steady_clock::now();
        const auto diff = end - start;
        std::cerr << "done in " << std::chrono::duration <double, std::milli> (diff).count() << " ms" << std::endl;
    }

    void compact_family(const std::string type, rocksdb::ColumnFamilyHandle* cf) {
        const auto start = std::chrono::steady_clock::now();
        std::cerr << "Compacting " << type << "...";
        m_db->CompactRange(rocksdb::CompactRangeOptions{}, cf, nullptr, nullptr);
        const auto end = std::chrono::steady_clock::now();
        const auto diff = end - start;
        std::cerr << "done in " << std::chrono::duration <double, std::milli> (diff).count() << " ms" << std::endl;
    }

    void report_count_stats() {
        uint64_t node_keys{0};
        m_db->GetIntProperty(m_cf_nodes, "rocksdb.estimate-num-keys", &node_keys);
        std::cerr << "Stored ~" << node_keys << "/" << stored_nodes_count << " nodes ";

        uint64_t way_keys{0};
        m_db->GetIntProperty(m_cf_ways, "rocksdb.estimate-num-keys", &way_keys);
        std::cerr << "~" << way_keys << "/" << stored_ways_count << " ways ";

        uint64_t relation_keys{0};
        m_db->GetIntProperty(m_cf_relations, "rocksdb.estimate-num-keys", &relation_keys);
        std::cerr << "~" << relation_keys  << "/" << stored_relations_count << " relations" << std::endl;
    }

public:
    unsigned long empty_objects_count{0};
    unsigned long stored_tags_count{0};

    unsigned long stored_nodes_count{0};
    unsigned long stored_ways_count{0};
    unsigned long stored_relations_count{0};

    unsigned long stored_objects_count() {
        return stored_nodes_count + stored_ways_count + stored_relations_count;
    }

    ObjectStore(const std::string index_dir, const bool create) {
        rocksdb::Options db_options;
        db_options.allow_mmap_writes = false;
        db_options.max_background_flushes = 4;
        db_options.PrepareForBulkLoad();

        db_options.target_file_size_base = 512 * 1024 * 1024;

        m_write_options = rocksdb::WriteOptions();
        m_write_options.disableWAL = true;
        m_write_options.sync = false;

        rocksdb::BlockBasedTableOptions table_options;
        table_options.filter_policy = std::shared_ptr<const rocksdb::FilterPolicy>(rocksdb::NewBloomFilterPolicy(10));
        db_options.table_factory.reset(NewBlockBasedTableFactory(table_options));

        rocksdb::Status s;

        if(create) {
            // always clear out the previous tag index first
            rocksdb::DestroyDB(index_dir, db_options);
            db_options.create_if_missing = true;
            s = rocksdb::DB::Open(db_options, index_dir, &m_db);
            s = m_db->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), "nodes", &m_cf_nodes);
            assert(s.ok());
            s = m_db->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), "ways", &m_cf_ways);
            assert(s.ok());
            s = m_db->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), "relations", &m_cf_relations);
            assert(s.ok());
        } else {
            db_options.error_if_exists = false;
            db_options.create_if_missing = false;
            std::cerr << "Open without create";
            // open DB with two column families
            std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
            // have to open default column family
            column_families.push_back(rocksdb::ColumnFamilyDescriptor(
            rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions()));

            // specifiy the existing column family
            column_families.push_back(rocksdb::ColumnFamilyDescriptor( "nodes", rocksdb::ColumnFamilyOptions()));
            column_families.push_back(rocksdb::ColumnFamilyDescriptor( "ways", rocksdb::ColumnFamilyOptions()));
            column_families.push_back(rocksdb::ColumnFamilyDescriptor( "relations", rocksdb::ColumnFamilyOptions()));

            std::vector<rocksdb::ColumnFamilyHandle*> handles;

            s = rocksdb::DB::OpenForReadOnly(db_options, index_dir, column_families, &handles, &m_db);
            assert(s.ok());

            m_cf_nodes = handles[1];
            m_cf_ways = handles[2];
            m_cf_relations = handles[3];
        }
    }

    rocksdb::Status get_tags(const int64_t osm_id, const int osm_type, const int version, std::string* value) {
        const auto lookup = make_lookup(osm_id, version);

        // Node
        if(osm_type== 1) {
            return m_db->Get(rocksdb::ReadOptions(), m_cf_nodes, lookup, value);
        // Way
        } else if (osm_type == 2) {
            return m_db->Get(rocksdb::ReadOptions(), m_cf_ways, lookup, value);
        // Relation
        } else {
            return m_db->Get(rocksdb::ReadOptions(), m_cf_relations, lookup, value);
        }
    }

/*
    Store PBF Objects in RocksDB
*/
    void store_pbf_node(const osmium::Node&node) {
      std::string lookup = make_lookup( node.id(), node.version() );

      if ( store_pbf_object( osmwayback::encode_node(node), lookup, m_cf_nodes) ){
          stored_nodes_count++;
      }

      //PBF Nodes always include geometries, flush in bulks of 5M
      if (stored_nodes_count != 0 && (stored_nodes_count % 5000000) == 0) {
          flush_family("nodes", m_cf_nodes);
          report_count_stats();
      }
    }

    void store_pbf_way(const osmium::Way&way) {
      std::string lookup = make_lookup( way.id(), way.version() );

      if ( store_pbf_object( osmwayback::encode_way(way), lookup, m_cf_ways) ){
          stored_ways_count++;
      }

      //PBF Ways... flush in bulks of 2M
      if (stored_ways_count != 0 && (stored_ways_count % 2000000) == 0) {
          flush_family("ways", m_cf_ways);
          report_count_stats();
      }
    }



/*
    Store JSON Objects in RocksDB

    (Less efficient for large areas, but useful for debugging)
*/

    void store_json_node(const osmium::Node& node) {
        rapidjson::Document json;

        //If there are no tags, do things differently
        if (node.tags().empty()) {
            //If we're not storing geometries, skip.
            if (!STORE_GEOMETRIES){
                empty_objects_count++;
                return;
            //We are storing at least geometries, so we need basic attributes
            }else{
                //If it's version 1, it should match a changeset, skip all properties
                if (node.version()==1){
                    //No tags & version 1: store only changeset INFO
                    json = osmwayback::extract_primary_properties(node);
                }else{
                    json = osmwayback::extract_osm_properties(node);
                }
            }
        } else {
            //There are tags, so get everything
            // rapidjson::Document json;
            json = osmwayback::extract_osm_properties(node);
        }

        std::string lookup = make_lookup( node.id(), node.version() );

        //If the node was not deleted, then store it's coordinates (if desired)
        if( !node.deleted() && STORE_GEOMETRIES){
            try{
              rapidjson::Document::AllocatorType& a = json.GetAllocator();
              rapidjson::Value coordinates(rapidjson::kArrayType);
              coordinates.PushBack(node.location().lon(), a);
              coordinates.PushBack(node.location().lat(), a);
              json.AddMember("g", coordinates, a); //g for geometry

            } catch (const osmium::invalid_location& ex) {
              //Catch invlid locations, not sure why this would happen... but it could
              std::cerr<< ex.what() << std::endl;
            }
        }

        if(store_json_object(json, lookup, m_cf_nodes)) {
            stored_nodes_count++;
        }
        if (stored_nodes_count != 0 && (stored_nodes_count % 4000000) == 0) {
            flush_family("nodes", m_cf_nodes);
            report_count_stats();
        }
    }

    void store_json_way(const osmium::Way& way) {
        //Get basic properties, initialize json
        rapidjson::Document json;
        json = osmwayback::extract_osm_properties(way);

        std::string lookup = make_lookup( way.id(), way.version() );

        //Store the node refs
        if( !way.deleted() && STORE_GEOMETRIES){
          try{
            rapidjson::Document::AllocatorType& a = json.GetAllocator();
            rapidjson::Value nodes(rapidjson::kArrayType);

            //iterate over the array
            for (const osmium::NodeRef& nr : way.nodes()) {
              nodes.PushBack(nr.ref(), a);
            }

            json.AddMember("r", nodes, a); //r for references

          } catch (const std::exception& ex) {
            //Not sure what might get thrown here
            std::cerr<< ex.what() << std::endl;
          }
        }

        if(store_json_object(json, lookup, m_cf_ways)) {
            stored_ways_count++;
        }

        if (stored_ways_count != 0 && (stored_ways_count % 2000000) == 0) {
            flush_family("ways", m_cf_ways);
            report_count_stats();
        }
    }

    void store_json_relation(const osmium::Relation& relation) {
        //Get basic properties, initialize json
        rapidjson::Document json;
        json = osmwayback::extract_osm_properties(relation);

        std::string lookup = make_lookup( relation.id(), relation.version() );

        if(store_json_object(json, lookup, m_cf_relations)) {
            stored_relations_count++;
        }
        if (stored_relations_count != 0 && (stored_relations_count % 1000000) == 0) {
            flush_family("relations", m_cf_relations);
            report_count_stats();
        }
    }


/*
    Store objects to RocksDB
*/
    //Store PBF Object
    bool store_pbf_object( const std::string value, const std::string lookup, rocksdb::ColumnFamilyHandle* cf ) {

        rocksdb::Status stat = m_buffer_batch.Put(cf, lookup, value);

        //Write in chunks of 2000
        if (m_buffer_batch.Count() > 2000) {
            m_db->Write(m_write_options, &m_buffer_batch);
            m_buffer_batch.Clear();
        }
        return true;
    }

    //Store the rapidjson object to rocksdb
    bool store_json_object( const rapidjson::Document& doc, const std::string lookup, rocksdb::ColumnFamilyHandle* cf ) {

        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);

        rocksdb::Status stat = m_buffer_batch.Put(cf, lookup, buffer.GetString());

        //Write in chunks of 1000
        if (m_buffer_batch.Count() > 1000) {
            m_db->Write(m_write_options, &m_buffer_batch);
            m_buffer_batch.Clear();
        }

        return true;
    }

    void flush() {
        m_db->Write(m_write_options, &m_buffer_batch);
        m_buffer_batch.Clear();

        flush_family("nodes",       m_cf_nodes);
        flush_family("ways",        m_cf_ways);
        flush_family("relations",   m_cf_relations);

        compact_family("nodes",     m_cf_nodes);
        compact_family("ways",      m_cf_ways);
        compact_family("relations", m_cf_relations);

        report_count_stats();
    }
};
