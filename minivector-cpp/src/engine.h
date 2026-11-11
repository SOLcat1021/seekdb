#pragma once

#include "sql.h"
#include "storage.h"
#include "util.h"

#include <functional>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct Entry {
    uint64_t off = 0;
    uint32_t len = 0;
    uint32_t share = 0;
};

class TxnOverlay {
public:
    using EntryValue = std::optional<Row>;
    TxnOverlay() = default;
    TxnOverlay(const TxnOverlay&) = delete;
    TxnOverlay& operator=(const TxnOverlay&) = delete;
    TxnOverlay(TxnOverlay&& other) noexcept;
    TxnOverlay& operator=(TxnOverlay&& other) noexcept;
    ~TxnOverlay();

    bool empty() const;
    size_t size() const;
    bool contains(int64_t pk) const;
    // Outer optional means "there is an overlay entry"; inner optional means
    // "row present or deleted".
    std::optional<EntryValue> get(int64_t pk) const;
    void set(int64_t pk, EntryValue value);
    void configure(const std::string& directory, const std::string& table);
    void for_each(
        const std::function<bool(int64_t, const EntryValue&)>& visit) const;
    void clear();

private:
    void spill();
    void write_record(std::fstream& file, int64_t pk, const EntryValue& value);
    EntryValue read_record(std::fstream& file, uint64_t offset) const;

    std::string directory_;
    std::string table_;
    std::string path_;
    std::map<int64_t, EntryValue> memory_;
    std::map<int64_t, uint64_t> offsets_;
    mutable std::fstream file_;
    bool disk_ = false;
};

struct Txn {
    std::map<std::string, TxnOverlay> tables;
};

struct HotSet {
    size_t dim = 0;
    size_t vector_column = 0;
    std::vector<int64_t> pks;
    std::vector<float> vectors;
    std::vector<float> norms;
    std::vector<std::optional<std::vector<int64_t>>> int_columns;
    std::vector<std::optional<std::vector<std::string>>> str_columns;
};

struct AnnIndex {
    uint32_t dim = 0;
    uint32_t subdim = 0;
    uint32_t chunks = 0;
    uint32_t rerank = 0;
    uint64_t seg_len = 0;
    uint64_t entry_count = 0;
    uint64_t record_count = 0;
    uint64_t schema_hash = 0;
    std::vector<float> centroids;  // chunks * 256 * subdim
    std::vector<uint8_t> codes;    // rows * chunks
    std::vector<int64_t> pks;
    std::vector<uint64_t> vector_offsets;
    std::vector<std::optional<std::vector<int64_t>>> int_columns;
};

struct OutputValue {
    enum class Kind { Int, Str, Vec, Float };
    Kind kind = Kind::Int;
    int64_t i = 0;
    std::string s;
    std::vector<float> v;
    double f = 0.0;

    static OutputValue integer(int64_t value);
    static OutputValue string(std::string value);
    static OutputValue vector(std::vector<float> value);
    static OutputValue floating(double value);
    void append_text(std::string& out) const;
    std::string to_text() const;
};

struct QueryResult {
    std::vector<std::string> columns;
    std::vector<std::vector<OutputValue>> rows;
    size_t json_min_rows_bytes = 0;
    std::string spill_path;
    uint64_t spill_rows = 0;
};

struct Response {
    enum class Kind { Query, Ok };
    Kind kind = Kind::Ok;
    QueryResult query;
    uint64_t affected = 0;
    bool affected_present = false;

    static Response ok();
    static Response changed(uint64_t affected);
    static Response result(QueryResult result);
};

class Table {
public:
    Schema schema;
    std::string path;
    Segment seg;
    std::map<int64_t, Entry> entries;
    std::vector<std::pair<uint64_t, uint32_t>> records;
    std::optional<std::map<int64_t, Row>> cache;
    std::optional<HotSet> hot;
    std::optional<AnnIndex> ann;
    size_t cache_bytes = 0;
    size_t cache_budget = 64u * 1024u * 1024u;
    size_t row_cache_budget = 16u * 1024u * 1024u;
    size_t ann_budget_bytes = 0;
    uint64_t live_bytes = 0;
    bool fsync = true;
    bool has_stale = false;
    bool statement_batch_active = false;
    uint64_t statement_batch_offset = 0;
    size_t statement_batch_records = 0;
    std::vector<std::pair<int64_t, std::optional<Entry>>> statement_batch_undo;
    uint64_t compact_min = 8u * 1024u * 1024u;

    static Table recover(
        Schema schema,
        std::string path,
        size_t cache_budget,
        bool fsync,
        uint64_t compact_min);

    bool ensure_hot();
    bool ensure_hot_string_column(size_t column);
    bool ann_valid() const;
    bool load_ann_index();
    void save_ann_index() const;
    bool build_ann_index();
    bool ensure_ann_index();
    bool begin_statement_batch(bool discard_cache = false);
    void append_statement_chunk(std::vector<Op> ops);
    void finish_statement_batch(bool compact_after = true);
    void abort_statement_batch();
    bool should_compact() const;
    bool cache_covers_all() const;
    void insert_cache(int64_t pk, const Row& row);
    void remove_cache(int64_t pk);
    std::optional<Row> get_row(int64_t pk);
    std::optional<Row> get_effective_row(
        const TxnOverlay* overlay,
        int64_t pk);
    void commit_ops(const std::vector<Op>& ops, bool compact_after = true);
    void compact();

    void for_each_row_merged(
        const TxnOverlay* overlay,
        const std::function<bool(int64_t, const Row&)>& visit);
    void for_each_row_streaming(
        const TxnOverlay* overlay,
        const std::function<bool(int64_t, const Row&)>& visit);
};

class Database {
public:
    std::string dir;
    std::map<std::string, Table> tables;
    size_t cache_budget = 64u * 1024u * 1024u;
    size_t threads = 0;
    size_t ann_rerank = 0;
    size_t ann_budget = 0;
    bool ann_mode = false;
    bool spool_results = true;
    size_t result_limit_bytes = 0;
    uint64_t next_txn_id = 1;
    bool fsync = true;
    uint64_t compact_min = 8u * 1024u * 1024u;
    bool autocommit = true;
    bool affected_rows_matched = false;

    static Database open(
        const std::string& directory,
        size_t cache_budget,
        bool fsync,
        uint64_t compact_min);

    size_t table_count() const;
    uint64_t row_count() const;
    void preload_ann_indexes();
    void set_ann_budget(size_t bytes);
    Response execute_large_insert(
        const std::string& sql,
        std::optional<Txn>& txn);
    Response execute(const Stmt& statement, std::optional<Txn>& txn);

private:
    void recover_cross_transactions();
    void apply_transaction_batches(bool recovering);
    void save_catalog();
    Response create_table(const Stmt& statement, std::optional<Txn>& txn);
    Response create_index(const Stmt& statement, std::optional<Txn>& txn);
    Response insert(
        const Stmt& statement,
        std::optional<Txn>& txn,
        Table* batch_table = nullptr,
        const TxnOverlay* prior_overlay = nullptr);
    Response update(const Stmt& statement, std::optional<Txn>& txn);
    Response remove(const Stmt& statement, std::optional<Txn>& txn);
    Response select(const Stmt& statement, std::optional<Txn>& txn);
    Response commit(std::optional<Txn>& txn);
    void apply_ops(
        const std::string& key,
        std::optional<Txn>& txn,
        std::vector<Op> ops,
        Table* batch_table = nullptr);
};
