#pragma once

#include "util.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

enum class OpKind : uint8_t { Insert = 0, Update = 1, Delete = 2 };

constexpr uint8_t kRecordFlagChunk = 1;
constexpr uint8_t kRecordFlagCommit = 2;

struct Op {
    OpKind kind;
    int64_t pk;
    std::optional<Row> row;
};

struct Segment {
    int fd = -1;
    std::string path;
    uint64_t len = 0;

    Segment() = default;
    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;
    Segment(Segment&& other) noexcept;
    Segment& operator=(Segment&& other) noexcept;
    ~Segment();

    static Segment open(const std::string& path);
    void reopen();
    std::pair<uint64_t, uint32_t> append(
        const Schema& schema,
        const std::vector<Op>& ops,
        bool fsync,
        uint8_t flags = 0);
    std::vector<Op> read_record(uint64_t offset, uint32_t length, const Schema& schema) const;
};

std::vector<uint8_t> encode_record(const Schema& schema, const std::vector<Op>& ops);
std::vector<Op> decode_record(const Schema& schema, const uint8_t* data, size_t size);

uint64_t scan_segment(
    const std::string& path,
    const Schema& schema,
    const std::function<void(uint64_t, uint32_t, uint8_t, std::vector<Op>)>& on_record);

// Read record payloads without decoding rows or verifying CRC.  This is used
// by the bounded-memory vector scan: only the vector/filter columns are
// interpreted, and the segment is trusted because recovery already validated
// every record up to the committed length.
uint64_t scan_segment_raw(
    const std::string& path,
    const std::function<void(uint64_t, uint32_t, const uint8_t*, size_t)>& on_record);

uint64_t scan_segment_raw_range(
    const std::string& path,
    uint64_t start,
    uint64_t end,
    const std::function<void(uint64_t, uint32_t, const uint8_t*, size_t)>& on_record);

void truncate_segment(const std::string& path, uint64_t valid_len);

std::vector<Schema> load_catalog(const std::string& path);
void save_catalog(const std::string& path, const std::vector<Schema>& schemas);
void atomic_write(const std::string& path, const std::vector<uint8_t>& data);
void fsync_dir(const std::string& directory);
