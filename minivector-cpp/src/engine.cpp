#include "engine.h"
#include "exact_scan.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <tuple>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace {

constexpr uint64_t kCompactSlack = 64u * 1024u;
constexpr size_t kRowCacheLimit = 0;
constexpr size_t kRawRecordHeaderSize = 13;
constexpr uint32_t kAnnMagic = 0x4D564941;  // "MVIA"
constexpr uint32_t kAnnVersion = 1;
constexpr uint32_t kPqCentroids = 64;
constexpr size_t kAnnMinRows = 500000;
constexpr size_t kStatementChunkRows = 4096;

std::string ann_index_path(const std::string& segment_path) {
    return segment_path + ".ivf";
}

uint64_t schema_fingerprint(const Schema& schema) {
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&](const void* data, size_t size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
    };
    mix(schema.table.data(), schema.table.size());
    uint32_t pk = static_cast<uint32_t>(schema.pk);
    mix(&pk, sizeof(pk));
    uint32_t count = static_cast<uint32_t>(schema.columns.size());
    mix(&count, sizeof(count));
    for (const Column& column : schema.columns) {
        mix(column.name.data(), column.name.size());
        uint8_t kind = static_cast<uint8_t>(column.type.kind);
        mix(&kind, sizeof(kind));
        mix(&column.type.n, sizeof(column.type.n));
    }
    return hash;
}

void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
    }
}

void append_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
    }
}

void append_i64(std::vector<uint8_t>& out, int64_t value) {
    append_u64(out, static_cast<uint64_t>(value));
}

void append_f32(std::vector<uint8_t>& out, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(out, bits);
}

class AnnReader {
public:
    AnnReader(const uint8_t* data, size_t size) : cursor_(data), limit_(data + size) {}
    bool done() const { return cursor_ == limit_; }
    size_t remaining() const { return static_cast<size_t>(limit_ - cursor_); }
    uint32_t u32() {
        if (remaining() < 4) {
            fail_internal("corrupted ANN index: truncated u32");
        }
        uint32_t value = static_cast<uint32_t>(cursor_[0]) |
                         (static_cast<uint32_t>(cursor_[1]) << 8) |
                         (static_cast<uint32_t>(cursor_[2]) << 16) |
                         (static_cast<uint32_t>(cursor_[3]) << 24);
        cursor_ += 4;
        return value;
    }
    uint64_t u64() {
        if (remaining() < 8) {
            fail_internal("corrupted ANN index: truncated u64");
        }
        uint64_t value = 0;
        for (size_t index = 0; index < 8; ++index) {
            value |= static_cast<uint64_t>(cursor_[index]) << (index * 8);
        }
        cursor_ += 8;
        return value;
    }
    int64_t i64() { return static_cast<int64_t>(u64()); }
    float f32() {
        uint32_t bits = u32();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    uint8_t u8() {
        if (remaining() < 1) {
            fail_internal("corrupted ANN index: truncated u8");
        }
        return *cursor_++;
    }
    const uint8_t* take(size_t count) {
        if (remaining() < count) {
            fail_internal("corrupted ANN index: truncated array");
        }
        const uint8_t* pointer = cursor_;
        cursor_ += count;
        return pointer;
    }
private:
    const uint8_t* cursor_;
    const uint8_t* limit_;
};

uint64_t hash_bytes(const uint8_t* data, size_t size) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

size_t estimate_ann_peak(const Table& table) {
    std::optional<size_t> vector_column = table.schema.vector_col();
    if (!vector_column.has_value() || table.entries.empty()) {
        return 0;
    }
    uint64_t rows = table.entries.size();
    uint64_t dim = table.schema.columns[vector_column.value()].type.n;
    uint64_t chunks = (dim + 3) / 4;
    uint64_t bigints = 0;
    for (const Column& column : table.schema.columns) {
        if (column.type.kind == TypeKind::BigInt) {
            ++bigints;
        }
    }
    uint64_t estimate = rows * chunks;          // codes
    estimate += rows * 8;                       // pks
    estimate += rows * 8;                       // vector offsets
    estimate += rows * bigints * 8;             // filter columns
    estimate += chunks * kPqCentroids * 4 * 4;  // centroids (subdim 4)
    estimate += std::min<uint64_t>(rows, 20000) * dim * sizeof(float);
    estimate += rows * 48;                      // entries/map/allocator overhead
    estimate += 16ull * 1024 * 1024;            // process/index build headroom
    return static_cast<size_t>(
        std::min<uint64_t>(estimate, std::numeric_limits<size_t>::max()));
}

void write_all_fd(
    int fd,
    const uint8_t* data,
    size_t size,
    const std::string& context);

class AnnWriter {
public:
    explicit AnnWriter(std::string path)
        : path_(std::move(path)), temporary_(path_ + ".tmp") {
        fd_ = ::open(temporary_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) {
            fail_io("create ANN index file", errno);
        }
    }

    ~AnnWriter() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    void u32(uint32_t value) {
        append_u32(buffer_, value);
        flush_if_needed();
    }

    void u64(uint64_t value) {
        append_u64(buffer_, value);
        flush_if_needed();
    }

    void i64(int64_t value) {
        append_i64(buffer_, value);
        flush_if_needed();
    }

    void f32(float value) {
        append_f32(buffer_, value);
        flush_if_needed();
    }

    void bytes(const uint8_t* data, size_t size) {
        if (size >= 1024u * 1024u) {
            flush();
            write_all_fd(fd_, data, size, "write ANN index");
            hash_update(data, size);
            return;
        }
        buffer_.insert(buffer_.end(), data, data + size);
        flush_if_needed();
    }

    void commit() {
        flush();
        uint8_t checksum[8];
        for (size_t index = 0; index < 8; ++index) {
            checksum[index] =
                static_cast<uint8_t>((hash_ >> (index * 8)) & 0xFFu);
        }
        write_all_fd(fd_, checksum, sizeof(checksum), "write ANN checksum");
        if (::fsync(fd_) != 0) {
            fail_io("fsync ANN index", errno);
        }
        ::close(fd_);
        fd_ = -1;
        if (::rename(temporary_.c_str(), path_.c_str()) != 0) {
            int error = errno;
            std::filesystem::remove(temporary_);
            fail_io("replace ANN index", error);
        }
        fsync_dir(std::filesystem::path(path_).parent_path().string());
    }

private:
    void hash_update(const uint8_t* data, size_t size) {
        for (size_t index = 0; index < size; ++index) {
            hash_ ^= data[index];
            hash_ *= 1099511628211ull;
        }
    }

    void flush_if_needed() {
        if (buffer_.size() >= 1024u * 1024u) {
            flush();
        }
    }

    void flush() {
        if (buffer_.empty()) {
            return;
        }
        write_all_fd(fd_, buffer_.data(), buffer_.size(), "write ANN index");
        hash_update(buffer_.data(), buffer_.size());
        buffer_.clear();
    }

    std::string path_;
    std::string temporary_;
    int fd_ = -1;
    std::vector<uint8_t> buffer_;
    uint64_t hash_ = 1469598103934665603ull;
};

std::string table_file_name(const std::string& name) {
    static const char* hex = "0123456789abcdef";
    std::string file = "t_";
    for (unsigned char byte : name) {
        file.push_back(hex[byte >> 4]);
        file.push_back(hex[byte & 0x0F]);
    }
    file += ".seg";
    return file;
}

std::string table_path(const std::string& directory, const std::string& name) {
    std::string path = directory;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    return path + table_file_name(name);
}

void write_all_fd(int fd, const uint8_t* data, size_t size, const std::string& context) {
    size_t written = 0;
    while (written < size) {
        ssize_t result = ::write(fd, data + written, size - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail_io(context, errno);
        }
        written += static_cast<size_t>(result);
    }
}

constexpr uint32_t kTxnLogMagic = 0x4D565458;  // "XVTM"
constexpr size_t kTxnLogHeaderSize = 17;       // magic, type, length, hash
constexpr uint8_t kTxnLogCommit = 1;
constexpr uint8_t kTxnLogApplied = 2;
constexpr uint64_t kTxnLogMaxPayload = 512ull * 1024 * 1024;

std::string transaction_batches_path(const std::string& directory) {
    return directory + "/txn-batches.log";
}

template <typename Consume>
void for_each_transaction_chunk(const TxnOverlay& overlay, Consume consume) {
    constexpr size_t kChunkBytes = 1u << 20;
    std::vector<Op> ops;
    size_t bytes = 0;
    auto flush = [&] {
        consume(std::move(ops));
        ops.clear();
        bytes = 0;
    };
    overlay.for_each([&](int64_t pk, const TxnOverlay::EntryValue& value) {
        const size_t cost = sizeof(Op) + (value ? row_mem_size(*value) : 0);
        if (!ops.empty() && (ops.size() == kStatementChunkRows ||
                             bytes + cost > kChunkBytes)) {
            flush();
        }
        ops.push_back(Op{value ? OpKind::Update : OpKind::Delete, pk, value});
        bytes += cost;
        return true;
    });
    if (!ops.empty()) {
        flush();
    }
}

struct TxnLogData {
    std::map<uint64_t, std::vector<std::pair<std::string, std::vector<uint8_t>>>> commits;
    std::set<uint64_t> applied;
    uint64_t max_id = 0;
};

std::string txn_log_path(const std::string& directory) {
    std::string path = directory;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    return path + "txn.log";
}

uint32_t load_u32(const uint8_t* data) {
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

uint64_t load_u64(const uint8_t* data) {
    uint64_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

bool take_u32(const uint8_t*& cursor, const uint8_t* limit, uint32_t& value) {
    if (static_cast<size_t>(limit - cursor) < sizeof(value)) {
        return false;
    }
    value = load_u32(cursor);
    cursor += sizeof(value);
    return true;
}

bool take_u64(const uint8_t*& cursor, const uint8_t* limit, uint64_t& value) {
    if (static_cast<size_t>(limit - cursor) < sizeof(value)) {
        return false;
    }
    value = load_u64(cursor);
    cursor += sizeof(value);
    return true;
}

void append_txn_log_record(
    const std::string& directory,
    uint8_t type,
    const std::vector<uint8_t>& payload) {
    if (payload.size() > kTxnLogMaxPayload) {
        fail_internal("transaction log record is too large");
    }
    std::vector<uint8_t> record;
    record.reserve(kTxnLogHeaderSize + payload.size());
    append_u32(record, kTxnLogMagic);
    record.push_back(type);
    append_u32(record, static_cast<uint32_t>(payload.size()));
    append_u64(record, hash_bytes(payload.data(), payload.size()));
    record.insert(record.end(), payload.begin(), payload.end());
    std::string path = txn_log_path(directory);
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        fail_io("open transaction log", errno);
    }
    try {
        write_all_fd(fd, record.data(), record.size(), "write transaction log");
        if (::fsync(fd) != 0) {
            fail_io("fsync transaction log", errno);
        }
    } catch (...) {
        ::close(fd);
        throw;
    }
    if (::close(fd) != 0) {
        fail_io("close transaction log", errno);
    }
    fsync_dir(directory);
}

TxnLogData read_txn_log(const std::string& directory) {
    TxnLogData result;
    std::string path = txn_log_path(directory);
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return result;
        }
        fail_io("open transaction log for recovery", errno);
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        int error = errno;
        ::close(fd);
        fail_io("stat transaction log", error);
    }
    if (info.st_size < 0) {
        ::close(fd);
        fail_internal("transaction log has a negative size");
    }
    size_t size = static_cast<size_t>(info.st_size);
    std::vector<uint8_t> data(size);
    size_t filled = 0;
    while (filled < size) {
        ssize_t got = ::pread(
            fd, data.data() + filled, size - filled, static_cast<off_t>(filled));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            int error = errno;
            ::close(fd);
            fail_io("read transaction log", error);
        }
        if (got == 0) {
            break;
        }
        filled += static_cast<size_t>(got);
    }
    ::close(fd);
    data.resize(filled);

    const uint8_t* cursor = data.data();
    const uint8_t* limit = data.data() + data.size();
    while (static_cast<size_t>(limit - cursor) >= kTxnLogHeaderSize) {
        if (load_u32(cursor) != kTxnLogMagic) {
            break;
        }
        uint8_t type = cursor[4];
        uint32_t length = load_u32(cursor + 5);
        uint64_t checksum = load_u64(cursor + 9);
        if (length > kTxnLogMaxPayload ||
            static_cast<size_t>(limit - cursor - kTxnLogHeaderSize) < length) {
            break;
        }
        const uint8_t* payload = cursor + kTxnLogHeaderSize;
        if (hash_bytes(payload, length) != checksum) {
            break;
        }
        const uint8_t* payload_limit = payload + length;
        if (type == kTxnLogCommit) {
            uint64_t id = 0;
            uint32_t count = 0;
            const uint8_t* read = payload;
            if (!take_u64(read, payload_limit, id) ||
                !take_u32(read, payload_limit, count)) {
                fail_internal("corrupted transaction log: truncated commit");
            }
            if (count > 1000000u) {
                fail_internal("corrupted transaction log: too many tables");
            }
            std::vector<std::pair<std::string, std::vector<uint8_t>>> tables;
            tables.reserve(count);
            for (uint32_t index = 0; index < count; ++index) {
                uint32_t name_size = 0;
                uint64_t record_size = 0;
                if (!take_u32(read, payload_limit, name_size) ||
                    name_size == 0 ||
                    static_cast<size_t>(payload_limit - read) < name_size) {
                    fail_internal("corrupted transaction log: bad table name");
                }
                std::string name(
                    reinterpret_cast<const char*>(read),
                    static_cast<size_t>(name_size));
                read += name_size;
                if (!take_u64(read, payload_limit, record_size) ||
                    record_size > kTxnLogMaxPayload ||
                    static_cast<uint64_t>(payload_limit - read) < record_size) {
                    fail_internal("corrupted transaction log: bad record size");
                }
                std::vector<uint8_t> record(
                    read, read + static_cast<size_t>(record_size));
                read += record_size;
                tables.emplace_back(lower_ascii(name), std::move(record));
            }
            if (read != payload_limit) {
                fail_internal("corrupted transaction log: trailing commit bytes");
            }
            result.commits[id] = std::move(tables);
            result.max_id = std::max(result.max_id, id);
        } else if (type == kTxnLogApplied) {
            const uint8_t* read = payload;
            uint64_t id = 0;
            if (!take_u64(read, payload_limit, id) || read != payload_limit) {
                fail_internal("corrupted transaction log: bad applied marker");
            }
            result.applied.insert(id);
            result.max_id = std::max(result.max_id, id);
        }
        cursor = payload_limit;
    }
    return result;
}

void remove_txn_log(const std::string& directory) {
    std::error_code error;
    bool removed = std::filesystem::remove(txn_log_path(directory), error);
    if (error) {
        fail_io("remove transaction log", error.value());
    }
    if (removed) {
        fsync_dir(directory);
    }
}

const TxnOverlay* overlay_for(
    const std::optional<Txn>& txn,
    const std::string& key) {
    if (!txn.has_value()) {
        return nullptr;
    }
    auto found = txn->tables.find(key);
    if (found == txn->tables.end()) {
        return nullptr;
    }
    return &found->second;
}

bool compare_int(int64_t left, int64_t right, CmpOp op) {
    switch (op) {
        case CmpOp::Eq:
            return left == right;
        case CmpOp::Ne:
            return left != right;
        case CmpOp::Lt:
            return left < right;
        case CmpOp::Le:
            return left <= right;
        case CmpOp::Gt:
            return left > right;
        case CmpOp::Ge:
            return left >= right;
    }
    return false;
}

bool compare_str(const std::string& left, const std::string& right, CmpOp op) {
    switch (op) {
        case CmpOp::Eq:
            return left == right;
        case CmpOp::Ne:
            return left != right;
        case CmpOp::Lt:
            return left < right;
        case CmpOp::Le:
            return left <= right;
        case CmpOp::Gt:
            return left > right;
        case CmpOp::Ge:
            return left >= right;
    }
    return false;
}

bool avx2_available() {
#if defined(__x86_64__) || defined(__i386__)
    static const bool available = [] {
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    }();
    return available;
#else
    return false;
#endif
}

float l2_sq_scalar(const float* a, const float* b, size_t size) {
    float sum = 0.0f;
    for (size_t index = 0; index < size; ++index) {
        float diff = a[index] - b[index];
        sum += diff * diff;
    }
    return sum;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,fma"))) float l2_sq_avx2(
    const float* a,
    const float* b,
    size_t size) {
    __m256 acc0 = _mm256_setzero_ps();
    __m256 acc1 = _mm256_setzero_ps();
    __m256 acc2 = _mm256_setzero_ps();
    __m256 acc3 = _mm256_setzero_ps();
    size_t index = 0;
    while (index + 32 <= size) {
        __m256 a0 = _mm256_loadu_ps(a + index);
        __m256 b0 = _mm256_loadu_ps(b + index);
        __m256 d0 = _mm256_sub_ps(a0, b0);
        acc0 = _mm256_fmadd_ps(d0, d0, acc0);
        __m256 a1 = _mm256_loadu_ps(a + index + 8);
        __m256 b1 = _mm256_loadu_ps(b + index + 8);
        __m256 d1 = _mm256_sub_ps(a1, b1);
        acc1 = _mm256_fmadd_ps(d1, d1, acc1);
        __m256 a2 = _mm256_loadu_ps(a + index + 16);
        __m256 b2 = _mm256_loadu_ps(b + index + 16);
        __m256 d2 = _mm256_sub_ps(a2, b2);
        acc2 = _mm256_fmadd_ps(d2, d2, acc2);
        __m256 a3 = _mm256_loadu_ps(a + index + 24);
        __m256 b3 = _mm256_loadu_ps(b + index + 24);
        __m256 d3 = _mm256_sub_ps(a3, b3);
        acc3 = _mm256_fmadd_ps(d3, d3, acc3);
        index += 32;
    }
    while (index + 8 <= size) {
        __m256 av = _mm256_loadu_ps(a + index);
        __m256 bv = _mm256_loadu_ps(b + index);
        __m256 diff = _mm256_sub_ps(av, bv);
        acc0 = _mm256_fmadd_ps(diff, diff, acc0);
        index += 8;
    }
    __m256 total = _mm256_add_ps(
        _mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
    float lanes[8];
    _mm256_storeu_ps(lanes, total);
    float sum = lanes[0] + lanes[1] + lanes[2] + lanes[3] + lanes[4] + lanes[5] +
                lanes[6] + lanes[7];
    while (index < size) {
        float diff = a[index] - b[index];
        sum += diff * diff;
        ++index;
    }
    return sum;
}
#endif

float l2_sq(const std::vector<float>& a, const std::vector<float>& b) {
    size_t size = std::min(a.size(), b.size());
#if defined(__x86_64__) || defined(__i386__)
    if (avx2_available()) {
        return l2_sq_avx2(a.data(), b.data(), size);
    }
#endif
    return l2_sq_scalar(a.data(), b.data(), size);
}

float l2_sq_ptrs(const float* a, const float* b, size_t size) {
#if defined(__x86_64__) || defined(__i386__)
    if (avx2_available()) {
        return l2_sq_avx2(a, b, size);
    }
#endif
    return l2_sq_scalar(a, b, size);
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,fma"))) float l2_sq_avx2_raw(
    const uint8_t* row,
    const float* query,
    size_t size) {
    __m256 acc0 = _mm256_setzero_ps();
    __m256 acc1 = _mm256_setzero_ps();
    __m256 acc2 = _mm256_setzero_ps();
    __m256 acc3 = _mm256_setzero_ps();
    size_t index = 0;
    while (index + 32 <= size) {
        __m256 a0 = _mm256_loadu_ps(
            reinterpret_cast<const float*>(row + (index + 0) * sizeof(float)));
        __m256 b0 = _mm256_loadu_ps(query + index);
        __m256 d0 = _mm256_sub_ps(a0, b0);
        acc0 = _mm256_fmadd_ps(d0, d0, acc0);
        __m256 a1 = _mm256_loadu_ps(
            reinterpret_cast<const float*>(row + (index + 8) * sizeof(float)));
        __m256 b1 = _mm256_loadu_ps(query + index + 8);
        __m256 d1 = _mm256_sub_ps(a1, b1);
        acc1 = _mm256_fmadd_ps(d1, d1, acc1);
        __m256 a2 = _mm256_loadu_ps(
            reinterpret_cast<const float*>(row + (index + 16) * sizeof(float)));
        __m256 b2 = _mm256_loadu_ps(query + index + 16);
        __m256 d2 = _mm256_sub_ps(a2, b2);
        acc2 = _mm256_fmadd_ps(d2, d2, acc2);
        __m256 a3 = _mm256_loadu_ps(
            reinterpret_cast<const float*>(row + (index + 24) * sizeof(float)));
        __m256 b3 = _mm256_loadu_ps(query + index + 24);
        __m256 d3 = _mm256_sub_ps(a3, b3);
        acc3 = _mm256_fmadd_ps(d3, d3, acc3);
        index += 32;
    }
    while (index + 8 <= size) {
        __m256 a = _mm256_loadu_ps(
            reinterpret_cast<const float*>(row + index * sizeof(float)));
        __m256 b = _mm256_loadu_ps(query + index);
        __m256 diff = _mm256_sub_ps(a, b);
        acc0 = _mm256_fmadd_ps(diff, diff, acc0);
        index += 8;
    }
    __m256 total = _mm256_add_ps(
        _mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
    float lanes[8];
    _mm256_storeu_ps(lanes, total);
    float sum = lanes[0] + lanes[1] + lanes[2] + lanes[3] + lanes[4] + lanes[5] +
                lanes[6] + lanes[7];
    while (index < size) {
        float value = 0.0f;
        std::memcpy(&value, row + index * sizeof(float), sizeof(value));
        float diff = value - query[index];
        sum += diff * diff;
        ++index;
    }
    return sum;
}
#endif

float l2_sq_raw(
    const uint8_t* row,
    const float* query,
    size_t size,
    bool use_avx) {
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx && size >= 8) {
        return l2_sq_avx2_raw(row, query, size);
    }
#else
    (void)use_avx;
#endif
    float sum = 0.0f;
    for (size_t index = 0; index < size; ++index) {
        float value = 0.0f;
        std::memcpy(&value, row + index * sizeof(float), sizeof(value));
        float diff = value - query[index];
        sum += diff * diff;
    }
    return sum;
}

inline float l2_sq4_raw(const uint8_t* row, const float* centroid) {
    float a0 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float a3 = 0.0f;
    std::memcpy(&a0, row, sizeof(a0));
    std::memcpy(&a1, row + 4, sizeof(a1));
    std::memcpy(&a2, row + 8, sizeof(a2));
    std::memcpy(&a3, row + 12, sizeof(a3));
    float d0 = a0 - centroid[0];
    float d1 = a1 - centroid[1];
    float d2 = a2 - centroid[2];
    float d3 = a3 - centroid[3];
    return d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3;
}

inline float l2_sq4_aligned(const float* vector, const float* centroid) {
    float d0 = vector[0] - centroid[0];
    float d1 = vector[1] - centroid[1];
    float d2 = vector[2] - centroid[2];
    float d3 = vector[3] - centroid[3];
    return d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3;
}

double l2_distance_raw(
    const uint8_t* row,
    const float* query,
    size_t size) {
    double sum = 0.0, correction = 0.0;
    for (size_t index = 0; index < size; ++index) {
        float value = 0.0f;
        std::memcpy(&value, row + index * sizeof(float), sizeof(value));
        double diff = static_cast<double>(value) -
                      static_cast<double>(query[index]);
        const double term = diff * diff;
        const double next = sum + term;
        correction += sum >= term ? (sum - next) + term : (term - next) + sum;
        sum = next;
    }
    return static_cast<double>(std::sqrt(static_cast<long double>(sum) + correction));
}

float dot_scalar(const float* a, const float* b, size_t size) {
    float sum = 0.0f;
    for (size_t index = 0; index < size; ++index) {
        sum += a[index] * b[index];
    }
    return sum;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,fma"))) float dot_avx2(
    const float* a,
    const float* b,
    size_t size) {
    __m256 acc0 = _mm256_setzero_ps();
    __m256 acc1 = _mm256_setzero_ps();
    __m256 acc2 = _mm256_setzero_ps();
    __m256 acc3 = _mm256_setzero_ps();
    size_t index = 0;
    while (index + 32 <= size) {
        acc0 = _mm256_fmadd_ps(
            _mm256_loadu_ps(a + index), _mm256_loadu_ps(b + index), acc0);
        acc1 = _mm256_fmadd_ps(
            _mm256_loadu_ps(a + index + 8), _mm256_loadu_ps(b + index + 8), acc1);
        acc2 = _mm256_fmadd_ps(
            _mm256_loadu_ps(a + index + 16), _mm256_loadu_ps(b + index + 16), acc2);
        acc3 = _mm256_fmadd_ps(
            _mm256_loadu_ps(a + index + 24), _mm256_loadu_ps(b + index + 24), acc3);
        index += 32;
    }
    while (index + 8 <= size) {
        acc0 = _mm256_fmadd_ps(
            _mm256_loadu_ps(a + index), _mm256_loadu_ps(b + index), acc0);
        index += 8;
    }
    __m256 total = _mm256_add_ps(
        _mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
    float lanes[8];
    _mm256_storeu_ps(lanes, total);
    float sum = lanes[0] + lanes[1] + lanes[2] + lanes[3] + lanes[4] + lanes[5] +
                lanes[6] + lanes[7];
    while (index < size) {
        sum += a[index] * b[index];
        ++index;
    }
    return sum;
}
#endif

double l2_distance_ptr(const float* a, const float* b, size_t size) {
    return exact_scan::distance(a, b, size);
}

class RawPayloadReader {
public:
    RawPayloadReader(const uint8_t* data, size_t size)
        : cursor_(data), limit_(data + size) {}

    uint8_t u8() { return *take(1); }

    uint32_t u32() {
        const uint8_t* bytes = take(4);
        return static_cast<uint32_t>(bytes[0]) |
               (static_cast<uint32_t>(bytes[1]) << 8) |
               (static_cast<uint32_t>(bytes[2]) << 16) |
               (static_cast<uint32_t>(bytes[3]) << 24);
    }

    int64_t i64() {
        const uint8_t* bytes = take(8);
        uint64_t bits = 0;
        for (size_t index = 0; index < 8; ++index) {
            bits |= static_cast<uint64_t>(bytes[index]) << (index * 8);
        }
        return static_cast<int64_t>(bits);
    }

    const uint8_t* take(size_t count) {
        if (count > static_cast<size_t>(limit_ - cursor_)) {
            fail_internal("storage corrupted: truncated raw record payload");
        }
        const uint8_t* pointer = cursor_;
        cursor_ += count;
        return pointer;
    }

    bool done() const { return cursor_ == limit_; }

private:
    const uint8_t* cursor_;
    const uint8_t* limit_;
};

Value literal_to_value(
    const Literal& literal,
    const ColumnType& type,
    const std::string& column,
    size_t row_no) {
    switch (type.kind) {
        case TypeKind::BigInt:
            if (literal.kind == Literal::Kind::Int) {
                return Value::integer(literal.i);
            }
            throw DbError(
                1366,
                "22007",
                "Incorrect integer value for column '" + column + "' at row " +
                    std::to_string(row_no));
        case TypeKind::Varchar:
            if (literal.kind == Literal::Kind::Str) {
                if (utf8_length(literal.s) > type.n) {
                    throw DbError(
                        1406,
                        "22001",
                        "Data too long for column '" + column + "' at row " +
                            std::to_string(row_no));
                }
                return Value::string(literal.s);
            }
            throw DbError(
                1366,
                "22007",
                "Incorrect string value for column '" + column + "' at row " +
                    std::to_string(row_no));
        case TypeKind::Vector:
            if (literal.kind == Literal::Kind::Str) {
                return Value::vector(
                    parse_vector_literal(literal.s, type.n, column, row_no));
            }
            if (literal.kind == Literal::Kind::Vector) {
                return Value::vector(
                    vector_from_f64(literal.vec, type.n, column, row_no));
            }
            throw DbError(
                1366,
                "22007",
                "Incorrect vector value for column '" + column + "' at row " +
                    std::to_string(row_no));
    }
    throw DbError(1366, "22007", "Incorrect value for column '" + column + "'");
}

struct Predicate {
    struct Clause {
        size_t column;
        CmpOp op;
        Value value;
    };
    std::vector<Clause> clauses;

    bool matches(const Row& row) const {
        for (const Clause& clause : clauses) {
            const Value& cell = row.values[clause.column];
            bool result = false;
            if (cell.kind == Value::Kind::Int && clause.value.kind == Value::Kind::Int) {
                result = compare_int(cell.i, clause.value.i, clause.op);
            } else if (
                cell.kind == Value::Kind::Str && clause.value.kind == Value::Kind::Str) {
                result = compare_str(cell.s, clause.value.s, clause.op);
            } else {
                throw DbError(1366, "22007", "comparison operands have incompatible types");
            }
            if (!result) {
                return false;
            }
        }
        return true;
    }
};

Predicate resolve_predicate(const std::optional<Cond>& filter, const Schema& schema) {
    Predicate predicate;
    if (!filter.has_value()) {
        return predicate;
    }
    for (const Clause& clause : filter->clauses) {
        std::optional<size_t> index = schema.col_index(clause.column);
        if (!index.has_value()) {
            throw DbError(1054, "42S22", "Unknown column '" + clause.column + "'");
        }
        const Column& column = schema.columns[index.value()];
        Predicate::Clause resolved;
        resolved.column = index.value();
        resolved.op = clause.op;
        switch (column.type.kind) {
            case TypeKind::BigInt:
                if (clause.value.kind != Literal::Kind::Int) {
                    throw DbError(
                        1366,
                        "22007",
                        "Incorrect value for column '" + column.name + "'");
                }
                resolved.value = Value::integer(clause.value.i);
                break;
            case TypeKind::Varchar:
                if (clause.value.kind != Literal::Kind::Str) {
                    throw DbError(
                        1366,
                        "22007",
                        "Incorrect value for column '" + column.name + "'");
                }
                resolved.value = Value::string(clause.value.s);
                break;
            case TypeKind::Vector:
                throw DbError(
                    1235, "42000", "vector columns cannot be compared directly");
        }
        predicate.clauses.push_back(std::move(resolved));
    }
    return predicate;
}

bool predicate_hot_compatible(const Predicate& predicate, const Schema& schema) {
    for (const Predicate::Clause& clause : predicate.clauses) {
        if (schema.columns[clause.column].type.kind != TypeKind::BigInt) {
            return false;
        }
        if (clause.value.kind != Value::Kind::Int) {
            return false;
        }
    }
    return true;
}

bool predicate_hot_compatible_with_strings(
    const Predicate& predicate,
    const Schema& schema) {
    for (const Predicate::Clause& clause : predicate.clauses) {
        TypeKind kind = schema.columns[clause.column].type.kind;
        if (kind == TypeKind::BigInt) {
            if (clause.value.kind != Value::Kind::Int) {
                return false;
            }
        } else if (kind == TypeKind::Varchar) {
            if (clause.value.kind != Value::Kind::Str) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

bool hot_matches(const HotSet& hot, const Predicate& predicate, size_t row) {
    for (const Predicate::Clause& clause : predicate.clauses) {
        if (clause.column >= hot.int_columns.size()) {
            return false;
        }
        if (hot.int_columns[clause.column].has_value()) {
            const auto& column = hot.int_columns[clause.column].value();
            if (row >= column.size() ||
                !compare_int(column[row], clause.value.i, clause.op)) {
                return false;
            }
            continue;
        }
        if (clause.column >= hot.str_columns.size() ||
            !hot.str_columns[clause.column].has_value()) {
            return false;
        }
        const auto& column = hot.str_columns[clause.column].value();
        if (row >= column.size() ||
            !compare_str(column[row], clause.value.s, clause.op)) {
            return false;
        }
    }
    return true;
}

struct Projection {
    enum class Kind { Column, Distance };
    Kind kind = Kind::Column;
    size_t column = 0;
    std::vector<float> query;
    std::string header;
    std::optional<std::string> alias;
};

std::vector<Projection> resolve_projections(
    const std::vector<SelectItem>& items,
    const Schema& schema) {
    std::vector<Projection> projections;
    for (const SelectItem& item : items) {
        if (item.kind == SelectItemKind::Star) {
            for (size_t index = 0; index < schema.columns.size(); ++index) {
                Projection projection;
                projection.kind = Projection::Kind::Column;
                projection.column = index;
                projection.header = schema.columns[index].name;
                projections.push_back(std::move(projection));
            }
            continue;
        }
        if (item.kind == SelectItemKind::Column) {
            std::optional<size_t> index = schema.col_index(item.name);
            if (!index.has_value()) {
                throw DbError(1054, "42S22", "Unknown column '" + item.name + "'");
            }
            Projection projection;
            projection.kind = Projection::Kind::Column;
            projection.column = index.value();
            projection.alias = item.alias;
            projection.header = item.alias.has_value()
                                    ? item.alias.value()
                                    : item.name;
            projections.push_back(std::move(projection));
            continue;
        }
        std::optional<size_t> index = schema.col_index(item.column);
        if (!index.has_value()) {
            throw DbError(1054, "42S22", "Unknown column '" + item.column + "'");
        }
        const Column& column = schema.columns[index.value()];
        if (column.type.kind != TypeKind::Vector) {
            throw DbError(1235, "42000", "L2_DISTANCE requires a VECTOR column");
        }
        Projection projection;
        projection.kind = Projection::Kind::Distance;
        projection.column = index.value();
        projection.query = vector_from_f64(item.query, column.type.n, column.name, 1);
        projection.alias = item.alias;
        projection.header = item.alias.has_value()
                                ? item.alias.value()
                                : ("l2_distance(" + column.name + ")");
        projections.push_back(std::move(projection));
    }
    return projections;
}

struct OrderSpec {
    enum class Kind { Column, Distance, Projection };
    Kind kind = Kind::Column;
    size_t column = 0;
    std::vector<float> query;
    bool asc = true;
    size_t projection = 0;
};

OrderSpec resolve_order(
    const OrderBy& order,
    const std::vector<Projection>& projections,
    const Schema& schema) {
    OrderSpec spec;
    if (order.kind == OrderKind::Name) {
        for (size_t index = 0; index < projections.size(); ++index) {
            if (projections[index].alias.has_value() &&
                iequals(projections[index].alias.value(), order.name)) {
                spec.kind = OrderSpec::Kind::Projection;
                spec.projection = index;
                spec.asc = order.asc;
                return spec;
            }
        }
        std::optional<size_t> index = schema.col_index(order.name);
        if (!index.has_value()) {
            throw DbError(1054, "42S22", "Unknown column '" + order.name + "'");
        }
        if (schema.columns[index.value()].type.kind == TypeKind::Vector) {
            throw DbError(1235, "42000", "ORDER BY a VECTOR column is not supported");
        }
        spec.kind = OrderSpec::Kind::Column;
        spec.column = index.value();
        spec.asc = order.asc;
        return spec;
    }
    std::optional<size_t> index = schema.col_index(order.column);
    if (!index.has_value()) {
        throw DbError(1054, "42S22", "Unknown column '" + order.column + "'");
    }
    const Column& column = schema.columns[index.value()];
    if (column.type.kind != TypeKind::Vector) {
        throw DbError(1235, "42000", "L2_DISTANCE requires a VECTOR column");
    }
    spec.kind = OrderSpec::Kind::Distance;
    spec.column = index.value();
    spec.query = vector_from_f64(order.query, column.type.n, column.name, 1);
    spec.asc = order.asc;
    return spec;
}

struct KeyVal {
    enum class Kind { Int, Str, Float };
    Kind kind = Kind::Int;
    int64_t i = 0;
    std::string s;
    double f = 0.0;

    int compare(const KeyVal& other) const {
        if (kind != other.kind) {
            return 0;
        }
        switch (kind) {
            case Kind::Int:
                return i < other.i ? -1 : (i > other.i ? 1 : 0);
            case Kind::Str:
                return s < other.s ? -1 : (s > other.s ? 1 : 0);
            case Kind::Float:
                return f < other.f ? -1 : (f > other.f ? 1 : 0);
        }
        return 0;
    }
};

KeyVal compute_order_key(
    const Row& row,
    const OrderSpec& order,
    const std::vector<Projection>& projections) {
    if (order.kind == OrderSpec::Kind::Column) {
        const Value& value = row.values[order.column];
        if (value.kind == Value::Kind::Int) {
            KeyVal key;
            key.kind = KeyVal::Kind::Int;
            key.i = value.i;
            return key;
        }
        if (value.kind == Value::Kind::Str) {
            KeyVal key;
            key.kind = KeyVal::Kind::Str;
            key.s = value.s;
            return key;
        }
        throw DbError(1235, "42000", "ORDER BY a VECTOR column is not supported");
    }
    if (order.kind == OrderSpec::Kind::Distance) {
        const Value& value = row.values[order.column];
        if (value.kind != Value::Kind::Vec) {
            throw DbError(1105, "HY000", "distance column is not a vector");
        }
        KeyVal key;
        key.kind = KeyVal::Kind::Float;
        key.f = l2_distance(value.v, order.query);
        return key;
    }
    const Projection& projection = projections[order.projection];
    if (projection.kind == Projection::Kind::Column) {
        const Value& value = row.values[projection.column];
        if (value.kind == Value::Kind::Int) {
            KeyVal key;
            key.kind = KeyVal::Kind::Int;
            key.i = value.i;
            return key;
        }
        if (value.kind == Value::Kind::Str) {
            KeyVal key;
            key.kind = KeyVal::Kind::Str;
            key.s = value.s;
            return key;
        }
        throw DbError(1235, "42000", "ORDER BY a VECTOR column is not supported");
    }
    const Value& value = row.values[projection.column];
    if (value.kind != Value::Kind::Vec) {
        throw DbError(1105, "HY000", "distance column is not a vector");
    }
    KeyVal key;
    key.kind = KeyVal::Kind::Float;
    key.f = l2_distance(value.v, projection.query);
    return key;
}

int compare_keys_pk(
    const KeyVal& left_key,
    int64_t left_pk,
    const KeyVal& right_key,
    int64_t right_pk,
    bool asc) {
    int primary = left_key.compare(right_key);
    if (!asc) {
        primary = -primary;
    }
    if (primary != 0) {
        return primary;
    }
    return left_pk < right_pk ? -1 : (left_pk > right_pk ? 1 : 0);
}

void write_sort_record(
    std::ofstream& output,
    const KeyVal& key,
    int64_t pk) {
    uint8_t kind = static_cast<uint8_t>(key.kind);
    output.write(reinterpret_cast<const char*>(&kind), sizeof(kind));
    output.write(reinterpret_cast<const char*>(&pk), sizeof(pk));
    if (key.kind == KeyVal::Kind::Int) {
        output.write(reinterpret_cast<const char*>(&key.i), sizeof(key.i));
    } else if (key.kind == KeyVal::Kind::Float) {
        output.write(reinterpret_cast<const char*>(&key.f), sizeof(key.f));
    } else {
        uint32_t length = static_cast<uint32_t>(key.s.size());
        output.write(reinterpret_cast<const char*>(&length), sizeof(length));
        output.write(key.s.data(), static_cast<std::streamsize>(key.s.size()));
    }
    if (!output) {
        fail_internal("write external sort run failed");
    }
}

bool read_sort_record(
    std::ifstream& input,
    KeyVal& key,
    int64_t& pk) {
    uint8_t kind = 0;
    input.read(reinterpret_cast<char*>(&kind), sizeof(kind));
    if (!input) {
        return false;
    }
    if (kind > static_cast<uint8_t>(KeyVal::Kind::Float)) {
        fail_internal("external sort run is corrupted");
    }
    key.kind = static_cast<KeyVal::Kind>(kind);
    input.read(reinterpret_cast<char*>(&pk), sizeof(pk));
    if (key.kind == KeyVal::Kind::Int) {
        input.read(reinterpret_cast<char*>(&key.i), sizeof(key.i));
    } else if (key.kind == KeyVal::Kind::Float) {
        input.read(reinterpret_cast<char*>(&key.f), sizeof(key.f));
    } else {
        uint32_t length = 0;
        input.read(reinterpret_cast<char*>(&length), sizeof(length));
        if (length > 1024u * 1024u) {
            fail_internal("external sort string is too large");
        }
        key.s.resize(length);
        input.read(key.s.data(), static_cast<std::streamsize>(length));
    }
    if (!input) {
        fail_internal("external sort run is truncated");
    }
    return true;
}

std::string make_sort_path(const std::string& directory) {
    std::string pattern = directory;
    if (!pattern.empty() && pattern.back() != '/') {
        pattern.push_back('/');
    }
    pattern += "sort-XXXXXX.tmp";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    int fd = ::mkstemps(buffer.data(), 4);
    if (fd < 0) {
        fail_io("create external sort run", errno);
    }
    ::close(fd);
    return std::string(buffer.data());
}

void write_multi_sort_record(
    std::ofstream& output,
    int64_t pk,
    const std::vector<KeyVal>& keys) {
    output.write(reinterpret_cast<const char*>(&pk), sizeof(pk));
    uint32_t count = static_cast<uint32_t>(keys.size());
    output.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const KeyVal& key : keys) {
        uint8_t kind = static_cast<uint8_t>(key.kind);
        output.write(reinterpret_cast<const char*>(&kind), sizeof(kind));
        if (key.kind == KeyVal::Kind::Int) {
            output.write(reinterpret_cast<const char*>(&key.i), sizeof(key.i));
        } else if (key.kind == KeyVal::Kind::Float) {
            output.write(reinterpret_cast<const char*>(&key.f), sizeof(key.f));
        } else {
            uint32_t length = static_cast<uint32_t>(key.s.size());
            output.write(reinterpret_cast<const char*>(&length), sizeof(length));
            output.write(key.s.data(), static_cast<std::streamsize>(key.s.size()));
        }
    }
    if (!output) {
        fail_internal("write external multi-key sort run failed");
    }
}

bool read_multi_sort_record(
    std::ifstream& input,
    int64_t& pk,
    std::vector<KeyVal>& keys) {
    input.read(reinterpret_cast<char*>(&pk), sizeof(pk));
    if (!input) {
        return false;
    }
    uint32_t count = 0;
    input.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (count > 1024u * 1024u) {
        fail_internal("external multi-key sort has too many keys");
    }
    keys.clear();
    keys.resize(count);
    for (KeyVal& key : keys) {
        uint8_t kind = 0;
        input.read(reinterpret_cast<char*>(&kind), sizeof(kind));
        if (kind > static_cast<uint8_t>(KeyVal::Kind::Float)) {
            fail_internal("external multi-key sort is corrupted");
        }
        key.kind = static_cast<KeyVal::Kind>(kind);
        if (key.kind == KeyVal::Kind::Int) {
            input.read(reinterpret_cast<char*>(&key.i), sizeof(key.i));
        } else if (key.kind == KeyVal::Kind::Float) {
            input.read(reinterpret_cast<char*>(&key.f), sizeof(key.f));
        } else {
            uint32_t length = 0;
            input.read(reinterpret_cast<char*>(&length), sizeof(length));
            if (length > 1024u * 1024u) {
                fail_internal("external multi-key sort string is too large");
            }
            key.s.resize(length);
            input.read(key.s.data(), static_cast<std::streamsize>(length));
        }
    }
    if (!input) {
        fail_internal("external multi-key sort is truncated");
    }
    return true;
}

std::vector<OutputValue> project_row(
    const Row& row,
    const std::vector<Projection>& projections) {
    std::vector<OutputValue> values;
    values.reserve(projections.size());
    for (const Projection& projection : projections) {
        if (projection.kind == Projection::Kind::Column) {
            const Value& value = row.values[projection.column];
            switch (value.kind) {
                case Value::Kind::Int:
                    values.push_back(OutputValue::integer(value.i));
                    break;
                case Value::Kind::Str:
                    values.push_back(OutputValue::string(value.s));
                    break;
                case Value::Kind::Vec:
                    values.push_back(OutputValue::vector(value.v));
                    break;
            }
            continue;
        }
        const Value& value = row.values[projection.column];
        if (value.kind != Value::Kind::Vec) {
            throw DbError(1105, "HY000", "distance projection references a non-vector column");
        }
        values.push_back(OutputValue::floating(l2_distance(value.v, projection.query)));
    }
    return values;
}

size_t json_value_min_bytes(const OutputValue& value) {
    switch (value.kind) {
        case OutputValue::Kind::Int:
        case OutputValue::Kind::Float:
            return 1;
        case OutputValue::Kind::Str:
            return 2 + value.s.size();
        case OutputValue::Kind::Vec:
            return value.v.empty() ? 2 : 1 + 2 * value.v.size();
    }
    return 0;
}

void append_result_row(
    QueryResult& result,
    std::vector<OutputValue> values,
    size_t limit) {
    if (limit > 0) {
        // Reject only if even the smallest possible encoding exceeds the
        // limit. The protocol checks the exact bytes, including the envelope.
        size_t size = 2 + !result.rows.empty();
        if (!values.empty()) {
            size += values.size() - 1;
        }
        for (const OutputValue& value : values) {
            size += json_value_min_bytes(value);
        }
        if (size > limit - result.json_min_rows_bytes) {
            throw DbError(
                1105,
                "HY000",
                "response exceeds the response byte limit");
        }
        result.json_min_rows_bytes += size;
    }
    result.rows.push_back(std::move(values));
}

std::optional<QueryResult> ann_select(
    Table& table,
    const Schema& schema,
    const Predicate& predicate,
    const std::vector<Projection>& projections,
    const std::vector<std::string>& columns,
    size_t vector_column,
    const std::vector<float>& query,
    size_t limit,
    size_t rerank_override,
    size_t scan_threads,
    size_t result_limit) {
    if (limit == 0 || table.entries.size() < kAnnMinRows ||
        !predicate_hot_compatible(predicate, schema) ||
        schema.columns[vector_column].type.kind != TypeKind::Vector ||
        schema.columns[vector_column].type.n != query.size()) {
        return std::nullopt;
    }
    if (!table.ensure_ann_index() || !table.ann.has_value()) {
        return std::nullopt;
    }
    const AnnIndex& index = table.ann.value();
    if (index.dim != query.size() || index.chunks == 0 ||
        index.codes.size() != index.pks.size() * index.chunks) {
        return std::nullopt;
    }
    for (const Predicate::Clause& clause : predicate.clauses) {
        if (clause.column >= index.int_columns.size() ||
            !index.int_columns[clause.column].has_value()) {
            return std::nullopt;
        }
    }
    if (table.seg.len == 0 ||
        table.seg.len > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return std::nullopt;
    }
    const size_t rows = index.pks.size();
    size_t rerank = rerank_override > 0
                        ? rerank_override
                        : static_cast<size_t>(index.rerank);
    rerank = std::max(rerank, limit + 64);
    rerank = std::min(rerank, rows);
    if (rerank == 0) {
        return std::nullopt;
    }
    std::vector<float> tables(
        static_cast<size_t>(index.chunks) * kPqCentroids);
    for (uint32_t chunk = 0; chunk < index.chunks; ++chunk) {
        uint32_t chunk_len =
            std::min<uint32_t>(index.subdim, index.dim - chunk * index.subdim);
        const float* query_chunk = query.data() + chunk * index.subdim;
        for (uint32_t center = 0; center < kPqCentroids; ++center) {
            tables[static_cast<size_t>(chunk) * kPqCentroids + center] =
                l2_sq_ptrs(
                    query_chunk,
                    index.centroids.data() +
                        (static_cast<size_t>(chunk) * kPqCentroids + center) *
                            index.subdim,
                    chunk_len);
        }
    }
    struct ApproxCandidate {
        float approx;
        uint32_t row;
    };
    struct ApproxCompare {
        bool operator()(
            const ApproxCandidate& left,
            const ApproxCandidate& right) const {
            if (left.approx != right.approx) {
                return left.approx < right.approx;
            }
            return left.row < right.row;
        }
    };
    using ApproxHeap = std::priority_queue<
        ApproxCandidate,
        std::vector<ApproxCandidate>,
        ApproxCompare>;
    auto scan_range = [&](size_t begin, size_t end, ApproxHeap& heap) {
        for (size_t row = begin; row < end; ++row) {
            bool matches = true;
            for (const Predicate::Clause& clause : predicate.clauses) {
                const std::vector<int64_t>& values =
                    index.int_columns[clause.column].value();
                if (row >= values.size() ||
                    !compare_int(values[row], clause.value.i, clause.op)) {
                    matches = false;
                    break;
                }
            }
            if (!matches) {
                continue;
            }
            const uint8_t* code =
                index.codes.data() + row * index.chunks;
            float acc0 = 0.0f;
            float acc1 = 0.0f;
            float acc2 = 0.0f;
            float acc3 = 0.0f;
            uint32_t chunk = 0;
            for (; chunk + 4 <= index.chunks; chunk += 4) {
                acc0 += tables[
                    static_cast<size_t>(chunk + 0) * kPqCentroids +
                    code[chunk + 0]];
                acc1 += tables[
                    static_cast<size_t>(chunk + 1) * kPqCentroids +
                    code[chunk + 1]];
                acc2 += tables[
                    static_cast<size_t>(chunk + 2) * kPqCentroids +
                    code[chunk + 2]];
                acc3 += tables[
                    static_cast<size_t>(chunk + 3) * kPqCentroids +
                    code[chunk + 3]];
            }
            for (; chunk < index.chunks; ++chunk) {
                acc0 += tables[
                    static_cast<size_t>(chunk) * kPqCentroids + code[chunk]];
            }
            float approx = (acc0 + acc1) + (acc2 + acc3);
            ApproxCandidate candidate{approx, static_cast<uint32_t>(row)};
            if (heap.size() < rerank) {
                heap.push(candidate);
            } else if (ApproxCompare{}(candidate, heap.top())) {
                heap.pop();
                heap.push(candidate);
            }
        }
    };
    size_t requested_threads = scan_threads;
    if (requested_threads == 0) {
        unsigned available = std::thread::hardware_concurrency();
        requested_threads = available == 0
                                ? 1
                                : std::min<size_t>(4, available);
    }
    size_t workers = rows >= 500000
                         ? std::min<size_t>(requested_threads, 4)
                         : 1;
    std::vector<ApproxHeap> heaps;
    heaps.reserve(workers);
    for (size_t index_worker = 0; index_worker < workers; ++index_worker) {
        heaps.emplace_back();
    }
    if (workers == 1) {
        scan_range(0, rows, heaps[0]);
    } else {
        std::vector<std::thread> pool;
        bool creation_failed = false;
        size_t per_worker = (rows + workers - 1) / workers;
        try {
            for (size_t worker = 0; worker < workers; ++worker) {
                size_t begin = worker * per_worker;
                if (begin >= rows) {
                    break;
                }
                size_t end = std::min(rows, begin + per_worker);
                pool.emplace_back([&, worker, begin, end] {
                    scan_range(begin, end, heaps[worker]);
                });
            }
        } catch (...) {
            creation_failed = true;
        }
        for (std::thread& thread : pool) {
            thread.join();
        }
        if (creation_failed) {
            for (ApproxHeap& heap : heaps) {
                while (!heap.empty()) {
                    heap.pop();
                }
            }
            heaps.clear();
            heaps.emplace_back();
            scan_range(0, rows, heaps[0]);
        }
    }
    std::vector<ApproxCandidate> approx_candidates;
    for (ApproxHeap& heap : heaps) {
        while (!heap.empty()) {
            approx_candidates.push_back(heap.top());
            heap.pop();
        }
    }
    if (approx_candidates.empty()) {
        if (!predicate.clauses.empty()) {
            QueryResult result;
            result.columns = columns;
            return result;
        }
        return std::nullopt;
    }
    std::sort(
        approx_candidates.begin(),
        approx_candidates.end(),
        [](const ApproxCandidate& left, const ApproxCandidate& right) {
            if (left.approx != right.approx) {
                return left.approx < right.approx;
            }
            return left.row < right.row;
        });
    if (approx_candidates.size() > rerank) {
        approx_candidates.resize(rerank);
    }
    if (table.seg.fd < 0) {
        return std::nullopt;
    }
    struct FinalCandidate {
        double distance;
        int64_t pk;
        uint32_t row;
    };
    std::vector<FinalCandidate> finals;
    finals.reserve(approx_candidates.size());
    uint64_t vector_bytes = static_cast<uint64_t>(index.dim) * sizeof(float);
    std::vector<uint8_t> vector_buffer(static_cast<size_t>(vector_bytes));
    for (const ApproxCandidate& candidate : approx_candidates) {
        uint64_t offset = index.vector_offsets[candidate.row];
        if (offset + vector_bytes > table.seg.len) {
            return std::nullopt;
        }
        size_t read = 0;
        while (read < vector_buffer.size()) {
            ssize_t result = ::pread(
                table.seg.fd,
                vector_buffer.data() + read,
                vector_buffer.size() - read,
                static_cast<off_t>(offset + read));
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::nullopt;
            }
            if (result == 0) {
                return std::nullopt;
            }
            read += static_cast<size_t>(result);
        }
        finals.push_back(FinalCandidate{
            l2_distance_raw(
                vector_buffer.data(), query.data(), index.dim),
            index.pks[candidate.row],
            candidate.row});
    }
    std::sort(
        finals.begin(),
        finals.end(),
        [](const FinalCandidate& left, const FinalCandidate& right) {
            if (left.distance != right.distance) {
                return left.distance < right.distance;
            }
            return left.pk < right.pk;
        });
    if (finals.size() > limit) {
        finals.resize(limit);
    }
    QueryResult result;
    result.columns = columns;
    result.rows.reserve(finals.size());
    bool direct_projection = true;
    for (const Projection& projection : projections) {
        if (projection.kind == Projection::Kind::Distance) {
            if (projection.column != vector_column || projection.query != query) {
                direct_projection = false;
                break;
            }
        } else if (projection.column != schema.pk) {
            direct_projection = false;
            break;
        }
    }
    if (direct_projection) {
        for (const FinalCandidate& candidate : finals) {
            std::vector<OutputValue> values;
            values.reserve(projections.size());
            for (const Projection& projection : projections) {
                if (projection.kind == Projection::Kind::Distance) {
                    values.push_back(OutputValue::floating(candidate.distance));
                } else {
                    values.push_back(OutputValue::integer(candidate.pk));
                }
            }
            append_result_row(result, std::move(values), result_limit);
        }
        return result;
    }
    for (const FinalCandidate& candidate : finals) {
        std::optional<Row> row = table.get_effective_row(nullptr, candidate.pk);
        if (row.has_value()) {
            append_result_row(
                result, project_row(row.value(), projections), result_limit);
        }
    }
    return result;
}

class ResultSpool {
public:
    ResultSpool() {
        char pattern[] = "/tmp/minivdb-result-XXXXXX";
        int fd = ::mkstemp(pattern);
        if (fd < 0) {
            fail_io("create result spool", errno);
        }
        ::close(fd);
        path_ = pattern;
        output_.open(path_, std::ios::binary | std::ios::trunc);
        if (!output_) {
            std::remove(path_.c_str());
            fail_internal("cannot open result spool");
        }
    }

    ~ResultSpool() {
        if (!finished_ && !path_.empty()) {
            output_.close();
            std::remove(path_.c_str());
        }
    }

    void write_row(const std::vector<OutputValue>& row) {
        std::string line;
        for (size_t index = 0; index < row.size(); ++index) {
            if (index > 0) {
                line.push_back('\t');
            }
            row[index].append_text(line);
        }
        line.push_back('\n');
        output_.write(line.data(), static_cast<std::streamsize>(line.size()));
        if (!output_) {
            fail_internal("write result spool failed");
        }
        ++rows_;
    }

    std::string finish() {
        output_.flush();
        if (!output_) {
            fail_internal("flush result spool failed");
        }
        output_.close();
        finished_ = true;
        return path_;
    }

    uint64_t rows() const { return rows_; }

private:
    std::string path_;
    std::ofstream output_;
    uint64_t rows_ = 0;
    bool finished_ = false;
};

constexpr size_t kResultSpillRows = 4096;

}  // namespace

TxnOverlay::TxnOverlay(TxnOverlay&& other) noexcept
    : directory_(std::move(other.directory_)),
      table_(std::move(other.table_)),
      path_(std::move(other.path_)),
      memory_(std::move(other.memory_)),
      offsets_(std::move(other.offsets_)),
      file_(std::move(other.file_)),
      disk_(other.disk_) {
    other.disk_ = false;
    other.path_.clear();
}

TxnOverlay& TxnOverlay::operator=(TxnOverlay&& other) noexcept {
    if (this != &other) {
        clear();
        directory_ = std::move(other.directory_);
        table_ = std::move(other.table_);
        path_ = std::move(other.path_);
        memory_ = std::move(other.memory_);
        offsets_ = std::move(other.offsets_);
        file_ = std::move(other.file_);
        disk_ = other.disk_;
        other.disk_ = false;
        other.path_.clear();
    }
    return *this;
}

TxnOverlay::~TxnOverlay() {
    clear();
}

bool TxnOverlay::empty() const {
    return memory_.empty() && offsets_.empty();
}

size_t TxnOverlay::size() const {
    return disk_ ? offsets_.size() : memory_.size();
}

bool TxnOverlay::contains(int64_t pk) const {
    if (!disk_) {
        return memory_.find(pk) != memory_.end();
    }
    return offsets_.find(pk) != offsets_.end();
}

std::optional<TxnOverlay::EntryValue> TxnOverlay::get(int64_t pk) const {
    if (!disk_) {
        auto found = memory_.find(pk);
        if (found == memory_.end()) {
            return std::nullopt;
        }
        return std::optional<EntryValue>(found->second);
    }
    auto found = offsets_.find(pk);
    if (found == offsets_.end()) {
        return std::nullopt;
    }
    return read_record(file_, found->second);
}

void TxnOverlay::set(int64_t pk, EntryValue value) {
    if (!disk_ && memory_.size() >= 20000) {
        spill();
    }
    if (!disk_) {
        memory_[pk] = std::move(value);
        return;
    }
    write_record(file_, pk, value);
}

void TxnOverlay::configure(
    const std::string& directory,
    const std::string& table) {
    if (directory_.empty()) {
        directory_ = directory;
        table_ = table;
    }
}

void TxnOverlay::spill() {
    if (disk_ || memory_.empty()) {
        return;
    }
    if (path_.empty()) {
        if (directory_.empty()) {
            fail_internal("transaction overlay has no data directory");
        }
        path_ = make_sort_path(directory_);
    }
    file_.open(
        path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
    if (!file_) {
        fail_internal("open transaction overlay spill");
    }
    for (const auto& [pk, value] : memory_) {
        write_record(file_, pk, value);
    }
    memory_.clear();
    disk_ = true;
}

void TxnOverlay::for_each(
    const std::function<bool(int64_t, const EntryValue&)>& visit) const {
    if (!disk_) {
        for (const auto& [pk, value] : memory_) {
            if (!visit(pk, value)) {
                return;
            }
        }
        return;
    }
    for (const auto& [pk, offset] : offsets_) {
        EntryValue value = read_record(file_, offset);
        if (!visit(pk, value)) {
            return;
        }
    }
}

void TxnOverlay::clear() {
    memory_.clear();
    offsets_.clear();
    if (file_.is_open()) {
        file_.close();
    }
    if (!path_.empty()) {
        std::remove(path_.c_str());
    }
    path_.clear();
    disk_ = false;
}

void TxnOverlay::write_record(
    std::fstream& file,
    int64_t pk,
    const EntryValue& value) {
    file.seekp(0, std::ios::end);
    uint64_t offset = static_cast<uint64_t>(file.tellp());
    uint8_t present = value.has_value() ? 1 : 0;
    file.write(reinterpret_cast<const char*>(&present), sizeof(present));
    if (present) {
        const Row& row = value.value();
        uint32_t count = static_cast<uint32_t>(row.values.size());
        file.write(reinterpret_cast<const char*>(&count), sizeof(count));
        for (const Value& item : row.values) {
            uint8_t kind = static_cast<uint8_t>(item.kind);
            file.write(reinterpret_cast<const char*>(&kind), sizeof(kind));
            if (item.kind == Value::Kind::Int) {
                file.write(reinterpret_cast<const char*>(&item.i), sizeof(item.i));
            } else if (item.kind == Value::Kind::Str) {
                uint32_t length = static_cast<uint32_t>(item.s.size());
                file.write(reinterpret_cast<const char*>(&length), sizeof(length));
                file.write(item.s.data(), static_cast<std::streamsize>(item.s.size()));
            } else {
                uint32_t length = static_cast<uint32_t>(item.v.size());
                file.write(reinterpret_cast<const char*>(&length), sizeof(length));
                file.write(
                    reinterpret_cast<const char*>(item.v.data()),
                    static_cast<std::streamsize>(item.v.size() * sizeof(float)));
            }
        }
    }
    if (!file) {
        fail_internal("write transaction overlay failed");
    }
    offsets_[pk] = offset;
}

TxnOverlay::EntryValue TxnOverlay::read_record(
    std::fstream& file,
    uint64_t offset) const {
    file.seekg(static_cast<std::streamoff>(offset));
    uint8_t present = 0;
    file.read(reinterpret_cast<char*>(&present), sizeof(present));
    if (!file) {
        fail_internal("read transaction overlay failed");
    }
    if (present == 0) {
        return std::nullopt;
    }
    uint32_t count = 0;
    file.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (count > 1024u * 1024u) {
        fail_internal("transaction overlay row is too large");
    }
    Row row;
    row.values.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        uint8_t kind = 0;
        file.read(reinterpret_cast<char*>(&kind), sizeof(kind));
        if (kind > static_cast<uint8_t>(Value::Kind::Vec)) {
            fail_internal("transaction overlay is corrupted");
        }
        Value value;
        value.kind = static_cast<Value::Kind>(kind);
        if (value.kind == Value::Kind::Int) {
            file.read(reinterpret_cast<char*>(&value.i), sizeof(value.i));
        } else if (value.kind == Value::Kind::Str) {
            uint32_t length = 0;
            file.read(reinterpret_cast<char*>(&length), sizeof(length));
            value.s.resize(length);
            file.read(value.s.data(), static_cast<std::streamsize>(length));
        } else {
            uint32_t length = 0;
            file.read(reinterpret_cast<char*>(&length), sizeof(length));
            value.v.resize(length);
            file.read(
                reinterpret_cast<char*>(value.v.data()),
                static_cast<std::streamsize>(value.v.size() * sizeof(float)));
        }
        row.values.push_back(std::move(value));
    }
    if (!file) {
        fail_internal("transaction overlay is truncated");
    }
    return row;
}


OutputValue OutputValue::integer(int64_t value) {
    OutputValue output;
    output.kind = Kind::Int;
    output.i = value;
    return output;
}

OutputValue OutputValue::string(std::string value) {
    OutputValue output;
    output.kind = Kind::Str;
    output.s = std::move(value);
    return output;
}

OutputValue OutputValue::vector(std::vector<float> value) {
    OutputValue output;
    output.kind = Kind::Vec;
    output.v = std::move(value);
    return output;
}

OutputValue OutputValue::floating(double value) {
    OutputValue output;
    output.kind = Kind::Float;
    output.f = value;
    return output;
}

void OutputValue::append_text(std::string& out) const {
    switch (kind) {
        case Kind::Int:
            out += std::to_string(i);
            return;
        case Kind::Str:
            out += escape_tsv(s);
            return;
        case Kind::Vec:
            out += format_vector(v);
            return;
        case Kind::Float:
            out += format_f64(f);
            return;
    }
}

std::string OutputValue::to_text() const {
    std::string out;
    append_text(out);
    return out;
}

Response Response::ok() {
    Response response;
    response.kind = Kind::Ok;
    response.affected = 0;
    return response;
}

Response Response::changed(uint64_t affected) {
    Response response;
    response.kind = Kind::Ok;
    response.affected = affected;
    response.affected_present = true;
    return response;
}

Response Response::result(QueryResult result) {
    Response response;
    response.kind = Kind::Query;
    response.query = std::move(result);
    return response;
}

Table Table::recover(
    Schema schema,
    std::string path,
    size_t cache_budget,
    bool fsync,
    uint64_t compact_min) {
    struct stat info {};
    uint64_t file_len = 0;
    if (::stat(path.c_str(), &info) == 0) {
        file_len = static_cast<uint64_t>(info.st_size);
    } else if (errno != ENOENT) {
        fail_io("stat segment " + path, errno);
    }

    std::map<int64_t, Entry> entries;
    std::vector<std::pair<uint64_t, uint32_t>> records;
    uint64_t live_bytes = 0;
    std::optional<std::map<int64_t, Row>> cache = std::map<int64_t, Row>{};
    size_t cache_bytes = 0;
    bool disable_cache = false;
    bool has_stale = false;
    const size_t row_cache_budget = std::min(cache_budget, kRowCacheLimit);

    bool pending = false;
    uint64_t pending_start = 0;
    size_t pending_records = 0;
    std::vector<std::pair<int64_t, std::optional<Entry>>> pending_undo;
    uint64_t valid_len = scan_segment(
        path,
        schema,
        [&](uint64_t offset,
            uint32_t record_length,
            uint8_t flags,
            std::vector<Op> ops) {
            records.emplace_back(offset, record_length);
            crash_point("recovery_mid");
            if ((flags & kRecordFlagCommit) != 0) {
                if (pending) {
                    pending = false;
                    pending_undo.clear();
                    pending_start = 0;
                    pending_records = 0;
                }
                return;
            }
            bool provisional = (flags & kRecordFlagChunk) != 0;
            if (provisional) {
                if (!pending) {
                    pending = true;
                    pending_start = offset;
                    pending_records = records.size() - 1;
                    pending_undo.clear();
                }
                if (cache.has_value()) {
                    disable_cache = true;
                    cache.reset();
                    cache_bytes = 0;
                }
            } else if (pending) {
                fail_internal("storage corrupted: uncommitted statement record");
            }
            uint32_t op_count =
                static_cast<uint32_t>(std::max<size_t>(ops.size(), 1));
            uint32_t share = record_length / op_count;
            for (Op& op : ops) {
                if (provisional) {
                    auto found = entries.find(op.pk);
                    pending_undo.emplace_back(
                        op.pk,
                        found == entries.end()
                            ? std::nullopt
                            : std::optional<Entry>(found->second));
                }
                if (op.kind == OpKind::Delete) {
                    has_stale = true;
                    auto found = entries.find(op.pk);
                    if (found != entries.end()) {
                        live_bytes -= found->second.share;
                        entries.erase(found);
                    }
                    if (cache.has_value()) {
                        auto cached = cache->find(op.pk);
                        if (cached != cache->end()) {
                            cache_bytes -= row_mem_size(cached->second);
                            cache->erase(cached);
                        }
                    }
                    continue;
                }
                if (!op.row.has_value()) {
                    fail_internal("storage corrupted: missing row payload");
                }
                auto inserted = entries.insert_or_assign(
                    op.pk, Entry{offset, record_length, share});
                if (!inserted.second) {
                    has_stale = true;
                    live_bytes -= inserted.first->second.share;
                }
                if (op.kind == OpKind::Update) {
                    has_stale = true;
                }
                live_bytes += share;
                if (cache.has_value() && !disable_cache) {
                    size_t size = row_mem_size(op.row.value());
                    if (cache_bytes + size > row_cache_budget) {
                        disable_cache = true;
                        cache.reset();
                        cache_bytes = 0;
                    } else {
                        auto existing = cache->find(op.pk);
                        if (existing != cache->end()) {
                            cache_bytes -= row_mem_size(existing->second);
                        }
                        (*cache)[op.pk] = op.row.value();
                        cache_bytes += size;
                    }
                }
            }
        });

    if (pending) {
        for (auto iterator = pending_undo.rbegin();
             iterator != pending_undo.rend();
             ++iterator) {
            if (iterator->second.has_value()) {
                entries[iterator->first] = iterator->second.value();
            } else {
                entries.erase(iterator->first);
            }
        }
        live_bytes = 0;
        for (const auto& [pk, entry] : entries) {
            (void)pk;
            live_bytes += entry.share;
        }
        records.resize(pending_records);
        cache.reset();
        cache_bytes = 0;
        valid_len = pending_start;
    }

    if (valid_len < file_len) {
        truncate_segment(path, valid_len);
    }

    Table table;
    table.schema = std::move(schema);
    table.path = std::move(path);
    table.seg = Segment::open(table.path);
    table.entries = std::move(entries);
    table.records = std::move(records);
    table.cache = std::move(cache);
    table.cache_bytes = cache_bytes;
    table.cache_budget = cache_budget;
    table.row_cache_budget = row_cache_budget;
    table.live_bytes = live_bytes;
    table.has_stale = has_stale;
    table.fsync = fsync;
    table.compact_min = compact_min;
    if (table.should_compact()) {
        table.compact();
    }
    if (table.schema.has_vector_index()) {
        table.ensure_hot();
    }
    return table;
}

bool Table::should_compact() const {
    return seg.len > compact_min && seg.len > live_bytes * 2 + kCompactSlack;
}

bool Table::cache_covers_all() const {
    return cache.has_value() && cache->size() == entries.size();
}

bool Table::ensure_hot() {
    if (hot.has_value()) {
        return true;
    }
    std::optional<size_t> vector_column = schema.vector_col();
    if (!vector_column.has_value()) {
        return false;
    }
    const Column& column = schema.columns[vector_column.value()];
    if (column.type.kind != TypeKind::Vector) {
        return false;
    }
    HotSet set;
    set.dim = column.type.n;
    set.vector_column = vector_column.value();
    set.int_columns.resize(schema.columns.size());
    set.str_columns.resize(schema.columns.size());
    size_t bytes_per_row = static_cast<size_t>(set.dim) * sizeof(float) +
                           sizeof(int64_t) + sizeof(float);
    for (size_t index = 0; index < schema.columns.size(); ++index) {
        if (schema.columns[index].type.kind == TypeKind::BigInt) {
            set.int_columns[index] = std::vector<int64_t>{};
            bytes_per_row += sizeof(int64_t);
        }
    }
    if (cache_covers_all()) {
        size_t count = cache->size();
        set.pks.reserve(count);
        set.vectors.reserve(count * set.dim);
        set.norms.reserve(count);
        for (const auto& [pk, row] : cache.value()) {
            if (row.values.size() != schema.columns.size()) {
                return false;
            }
            set.pks.push_back(pk);
            for (size_t index = 0; index < schema.columns.size(); ++index) {
                if (set.int_columns[index].has_value()) {
                    if (row.values[index].kind != Value::Kind::Int) {
                        return false;
                    }
                    set.int_columns[index]->push_back(row.values[index].i);
                }
            }
            const Value& value = row.values[vector_column.value()];
            if (value.kind != Value::Kind::Vec || value.v.size() != set.dim) {
                return false;
            }
            set.vectors.insert(set.vectors.end(), value.v.begin(), value.v.end());
            set.norms.push_back(dot_scalar(value.v.data(), value.v.data(), set.dim));
        }
        hot = std::move(set);
        return true;
    }
    // The decoded-row cache is intentionally capped well below the vector
    // budget.  Build the columnar vector set directly from the segment when
    // the full vectors fit, but the full rows do not.
    if (entries.empty() ||
        entries.size() > cache_budget / std::max<size_t>(bytes_per_row, 1)) {
        return false;
    }
    set.pks.reserve(entries.size());
    set.vectors.reserve(entries.size() * set.dim);
    set.norms.reserve(entries.size());
    std::vector<int64_t> row_ints(schema.columns.size(), 0);
    scan_segment_raw(
        path,
        [&](uint64_t offset, uint32_t, const uint8_t* payload, size_t size) {
            RawPayloadReader reader(payload, size);
            uint32_t op_count = reader.u32();
            for (uint32_t op_index = 0; op_index < op_count; ++op_index) {
                uint8_t kind = reader.u8();
                int64_t pk = reader.i64();
                if (kind == static_cast<uint8_t>(OpKind::Delete)) {
                    continue;
                }
                if (kind != static_cast<uint8_t>(OpKind::Insert) &&
                    kind != static_cast<uint8_t>(OpKind::Update)) {
                    fail_internal("storage corrupted: unknown op kind in hot build");
                }
                const uint8_t* raw_vector = nullptr;
                for (size_t index = 0; index < schema.columns.size(); ++index) {
                    const ColumnType& type = schema.columns[index].type;
                    switch (type.kind) {
                        case TypeKind::BigInt:
                            row_ints[index] = reader.i64();
                            break;
                        case TypeKind::Varchar: {
                            uint32_t length = reader.u32();
                            reader.take(length);
                            break;
                        }
                        case TypeKind::Vector: {
                            const size_t bytes =
                                static_cast<size_t>(type.n) * sizeof(float);
                            const uint8_t* raw = reader.take(bytes);
                            if (index == vector_column.value()) {
                                raw_vector = raw;
                            }
                            break;
                        }
                    }
                }
                if (has_stale) {
                    auto found = entries.find(pk);
                    if (found == entries.end() || found->second.off != offset) {
                        continue;
                    }
                }
                if (raw_vector == nullptr) {
                    fail_internal("storage corrupted: missing vector in hot build");
                }
                set.pks.push_back(pk);
                for (size_t index = 0; index < schema.columns.size(); ++index) {
                    if (set.int_columns[index].has_value()) {
                        set.int_columns[index]->push_back(row_ints[index]);
                    }
                }
                size_t vector_offset = set.vectors.size();
                set.vectors.resize(vector_offset + set.dim);
                std::memcpy(
                    set.vectors.data() + vector_offset,
                    raw_vector,
                    static_cast<size_t>(set.dim) * sizeof(float));
                set.norms.push_back(dot_scalar(
                    set.vectors.data() + vector_offset,
                    set.vectors.data() + vector_offset,
                    set.dim));
            }
        });
    hot = std::move(set);
    return true;
}

bool Table::ensure_hot_string_column(size_t column) {
    if (!hot.has_value() || column >= schema.columns.size() ||
        schema.columns[column].type.kind != TypeKind::Varchar) {
        return false;
    }
    auto& slot = hot->str_columns[column];
    if (slot.has_value()) {
        return true;
    }
    const size_t limit = std::max<size_t>(
        1, std::min<size_t>(cache_budget, 64u * 1024u * 1024u));
    std::vector<std::string> values;
    values.reserve(hot->pks.size());
    size_t bytes = 0;
    if (cache_covers_all()) {
        for (const auto& [pk, row] : cache.value()) {
            (void)pk;
            if (row.values.size() != schema.columns.size() ||
                row.values[column].kind != Value::Kind::Str) {
                return false;
            }
            const std::string& text = row.values[column].s;
            bytes += text.size();
            if (bytes > limit) {
                return false;
            }
            values.push_back(text);
        }
    } else {
        bool too_large = false;
        scan_segment_raw(
            path,
            [&](uint64_t offset,
                uint32_t,
                const uint8_t* payload,
                size_t size) {
                if (too_large) {
                    return;
                }
                RawPayloadReader reader(payload, size);
                uint32_t op_count = reader.u32();
                for (uint32_t op_index = 0; op_index < op_count; ++op_index) {
                    uint8_t kind = reader.u8();
                    int64_t pk = reader.i64();
                    if (kind == static_cast<uint8_t>(OpKind::Delete)) {
                        continue;
                    }
                    if (kind != static_cast<uint8_t>(OpKind::Insert) &&
                        kind != static_cast<uint8_t>(OpKind::Update)) {
                        fail_internal("storage corrupted: unknown op in string cache");
                    }
                    std::string target;
                    bool target_present = false;
                    for (size_t index = 0; index < schema.columns.size(); ++index) {
                        const ColumnType& type = schema.columns[index].type;
                        switch (type.kind) {
                            case TypeKind::BigInt:
                                reader.i64();
                                break;
                            case TypeKind::Varchar: {
                                uint32_t length = reader.u32();
                                const uint8_t* text = reader.take(length);
                                if (index == column) {
                                    target.assign(
                                        reinterpret_cast<const char*>(text), length);
                                    target_present = true;
                                }
                                break;
                            }
                            case TypeKind::Vector:
                                reader.take(
                                    static_cast<size_t>(type.n) * sizeof(float));
                                break;
                        }
                    }
                    if (has_stale) {
                        auto found = entries.find(pk);
                        if (found == entries.end() ||
                            found->second.off != offset) {
                            continue;
                        }
                    }
                    if (!target_present) {
                        return;
                    }
                    bytes += target.size();
                    if (bytes > limit ||
                        values.size() >= hot->pks.size()) {
                        too_large = true;
                        return;
                    }
                    values.push_back(std::move(target));
                }
            });
        if (too_large || values.size() != hot->pks.size()) {
            return false;
        }
    }
    if (values.size() != hot->pks.size()) {
        return false;
    }
    slot = std::move(values);
    return true;
}

bool Table::ann_valid() const {
    if (!ann.has_value()) {
        return false;
    }
    const AnnIndex& index = ann.value();
    size_t rows = entries.size();
    if (index.dim == 0 || index.subdim == 0 || index.chunks == 0 ||
        index.rerank == 0 ||
        index.chunks != (index.dim + index.subdim - 1) / index.subdim ||
        index.centroids.size() !=
            static_cast<size_t>(index.chunks) * kPqCentroids * index.subdim ||
        index.codes.size() != rows * index.chunks ||
        index.pks.size() != rows ||
        index.vector_offsets.size() != rows ||
        index.int_columns.size() != schema.columns.size() ||
        index.schema_hash != schema_fingerprint(schema) ||
        index.seg_len != seg.len ||
        index.entry_count != entries.size() ||
        index.record_count != records.size()) {
        return false;
    }
    for (size_t column = 0; column < schema.columns.size(); ++column) {
        bool expected = schema.columns[column].type.kind == TypeKind::BigInt;
        if (index.int_columns[column].has_value() != expected) {
            return false;
        }
        if (expected && index.int_columns[column]->size() != rows) {
            return false;
        }
    }
    return true;
}

void Table::save_ann_index() const {
    if (!ann.has_value()) {
        fail_internal("cannot save an empty ANN index");
    }
    const AnnIndex& index = ann.value();
    AnnWriter writer(ann_index_path(path));
    writer.u32(kAnnMagic);
    writer.u32(kAnnVersion);
    writer.u32(2);  // PQ-Flat
    writer.u32(index.dim);
    writer.u32(index.subdim);
    writer.u32(index.chunks);
    writer.u32(index.rerank);
    writer.u64(index.pks.size());
    writer.u64(index.seg_len);
    writer.u64(index.record_count);
    writer.u64(index.entry_count);
    writer.u64(index.schema_hash);
    writer.u32(static_cast<uint32_t>(schema.columns.size()));
    writer.u32(0);
    for (float value : index.centroids) {
        writer.f32(value);
    }
    writer.bytes(index.codes.data(), index.codes.size());
    for (int64_t value : index.pks) {
        writer.i64(value);
    }
    for (uint64_t value : index.vector_offsets) {
        writer.u64(value);
    }
    for (size_t column = 0; column < schema.columns.size(); ++column) {
        bool present = index.int_columns[column].has_value();
        writer.bytes(
            reinterpret_cast<const uint8_t*>(&present), sizeof(present));
        if (present) {
            for (int64_t value : index.int_columns[column].value()) {
                writer.i64(value);
            }
        }
    }
    writer.commit();
}

bool Table::load_ann_index() {
    std::ifstream input(ann_index_path(path), std::ios::binary);
    if (!input) {
        return false;
    }
    input.seekg(0, std::ios::end);
    std::streamoff length = input.tellg();
    if (length < 8 || length > static_cast<std::streamoff>(2ull * 1024 * 1024 * 1024)) {
        return false;
    }
    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(length));
    input.read(reinterpret_cast<char*>(data.data()), length);
    if (!input) {
        return false;
    }
    size_t body_size = data.size() - sizeof(uint64_t);
    uint64_t stored_checksum = 0;
    for (size_t index = 0; index < sizeof(uint64_t); ++index) {
        stored_checksum |= static_cast<uint64_t>(data[body_size + index])
                           << (index * 8);
    }
    if (hash_bytes(data.data(), body_size) != stored_checksum) {
        return false;
    }
    AnnReader reader(data.data(), body_size);
    if (reader.u32() != kAnnMagic || reader.u32() != kAnnVersion ||
        reader.u32() != 2) {
        return false;
    }
    AnnIndex index;
    index.dim = reader.u32();
    index.subdim = reader.u32();
    index.chunks = reader.u32();
    index.rerank = reader.u32();
    uint64_t row_count = reader.u64();
    index.seg_len = reader.u64();
    index.record_count = reader.u64();
    index.entry_count = reader.u64();
    index.schema_hash = reader.u64();
    uint32_t column_count = reader.u32();
    reader.u32();
    if (index.dim == 0 || index.subdim == 0 || index.chunks == 0 ||
        index.rerank == 0 || index.dim > 1000000 ||
        index.subdim > 1024 || index.chunks > 1000000 ||
        index.chunks != (index.dim + index.subdim - 1) / index.subdim ||
        row_count > 100000000ull || row_count != entries.size() ||
        index.seg_len != seg.len || index.record_count != records.size() ||
        index.entry_count != entries.size() ||
        index.schema_hash != schema_fingerprint(schema) ||
        column_count != schema.columns.size()) {
        return false;
    }
    size_t centroid_count =
        static_cast<size_t>(index.chunks) * kPqCentroids * index.subdim;
    if (reader.remaining() < centroid_count * sizeof(float)) {
        return false;
    }
    index.centroids.resize(centroid_count);
    for (float& value : index.centroids) {
        value = reader.f32();
    }
    size_t code_count = static_cast<size_t>(row_count) * index.chunks;
    if (reader.remaining() < code_count) {
        return false;
    }
    const uint8_t* codes = reader.take(code_count);
    index.codes.assign(codes, codes + code_count);
    index.pks.resize(static_cast<size_t>(row_count));
    for (int64_t& value : index.pks) {
        value = reader.i64();
    }
    index.vector_offsets.resize(static_cast<size_t>(row_count));
    for (uint64_t& value : index.vector_offsets) {
        value = reader.u64();
        if (value + static_cast<uint64_t>(index.dim) * sizeof(float) >
            index.seg_len) {
            return false;
        }
    }
    index.int_columns.resize(schema.columns.size());
    for (size_t column = 0; column < schema.columns.size(); ++column) {
        bool present = reader.u8() != 0;
        bool expected = schema.columns[column].type.kind == TypeKind::BigInt;
        if (present != expected) {
            return false;
        }
        if (!present) {
            continue;
        }
        std::vector<int64_t> values(static_cast<size_t>(row_count));
        for (int64_t& value : values) {
            value = reader.i64();
        }
        index.int_columns[column] = std::move(values);
    }
    if (!reader.done()) {
        return false;
    }
    ann = std::move(index);
    return true;
}

bool Table::build_ann_index() {
    std::optional<size_t> vector_column = schema.vector_col();
    if (!vector_column.has_value() || entries.empty() ||
        schema.columns[vector_column.value()].type.kind != TypeKind::Vector) {
        return false;
    }
    const uint32_t dim = schema.columns[vector_column.value()].type.n;
    if (dim == 0) {
        return false;
    }
    const uint32_t subdim = std::min<uint32_t>(dim, 4);
    const uint32_t chunks = (dim + subdim - 1) / subdim;
    const size_t live_total = entries.size();
    const size_t sample_target = std::min<size_t>(live_total, 20000);
    std::vector<float> sample(sample_target * dim);
    size_t sample_count = 0;
    size_t live_count = 0;
    uint64_t random_state = 0x9E3779B97F4A7C15ull;
    auto next_random = [&]() {
        uint64_t value = random_state;
        value ^= value >> 12;
        value ^= value << 25;
        value ^= value >> 27;
        random_state = value;
        return value * 2685821657736338717ull;
    };
    std::vector<int64_t> row_ints(schema.columns.size(), 0);
    auto scan_rows = [&](const std::function<void(
                             int64_t,
                             uint64_t,
                             const uint8_t*,
                             const std::vector<int64_t>&)>& visit) {
        scan_segment_raw(
            path,
            [&](uint64_t offset, uint32_t, const uint8_t* payload, size_t size) {
                RawPayloadReader reader(payload, size);
                uint32_t op_count = reader.u32();
                for (uint32_t op_index = 0; op_index < op_count; ++op_index) {
                    uint8_t kind = reader.u8();
                    int64_t pk = reader.i64();
                    if (kind == static_cast<uint8_t>(OpKind::Delete)) {
                        continue;
                    }
                    if (kind != static_cast<uint8_t>(OpKind::Insert) &&
                        kind != static_cast<uint8_t>(OpKind::Update)) {
                        fail_internal("storage corrupted: unknown op in PQ build");
                    }
                    const uint8_t* vector = nullptr;
                    for (size_t column = 0; column < schema.columns.size(); ++column) {
                        const ColumnType& type = schema.columns[column].type;
                        switch (type.kind) {
                            case TypeKind::BigInt:
                                row_ints[column] = reader.i64();
                                break;
                            case TypeKind::Varchar: {
                                uint32_t length = reader.u32();
                                reader.take(length);
                                break;
                            }
                            case TypeKind::Vector: {
                                const size_t bytes =
                                    static_cast<size_t>(type.n) * sizeof(float);
                                const uint8_t* raw = reader.take(bytes);
                                if (column == vector_column.value()) {
                                    vector = raw;
                                }
                                break;
                            }
                        }
                    }
                    if (has_stale) {
                        auto found = entries.find(pk);
                        if (found == entries.end() ||
                            found->second.off != offset) {
                            continue;
                        }
                    }
                    if (vector == nullptr) {
                        fail_internal("storage corrupted: missing vector in PQ build");
                    }
                    uint64_t vector_offset =
                        offset + kRawRecordHeaderSize +
                        static_cast<uint64_t>(vector - payload);
                    visit(pk, vector_offset, vector, row_ints);
                }
            });
    };
    scan_rows([&](int64_t, uint64_t, const uint8_t* vector,
                  const std::vector<int64_t>&) {
        if (sample_count < sample_target) {
            std::memcpy(
                sample.data() + sample_count * dim,
                vector,
                static_cast<size_t>(dim) * sizeof(float));
            ++sample_count;
        } else {
            ++live_count;
            uint64_t slot = next_random() % live_count;
            if (slot < sample_target) {
                std::memcpy(
                    sample.data() + slot * dim,
                    vector,
                    static_cast<size_t>(dim) * sizeof(float));
            }
            return;
        }
        ++live_count;
    });
    if (sample_count == 0 || live_count != live_total) {
        return false;
    }
    std::vector<uint32_t> shuffle(sample_count);
    for (size_t index = 0; index < shuffle.size(); ++index) {
        shuffle[index] = static_cast<uint32_t>(index);
    }
    for (size_t index = sample_count; index > 1; --index) {
        size_t other = static_cast<size_t>(next_random() % index);
        std::swap(shuffle[index - 1], shuffle[other]);
    }
    std::vector<float> centroids(
        static_cast<size_t>(chunks) * kPqCentroids * subdim, 0.0f);
    auto train_chunk = [&](uint32_t chunk) {
        uint32_t chunk_len = std::min<uint32_t>(subdim, dim - chunk * subdim);
        for (uint32_t center = 0; center < kPqCentroids; ++center) {
            const float* source =
                sample.data() +
                static_cast<size_t>(shuffle[center % sample_count]) * dim +
                chunk * subdim;
            std::memcpy(
                centroids.data() +
                    (static_cast<size_t>(chunk) * kPqCentroids + center) * subdim,
                source,
                static_cast<size_t>(chunk_len) * sizeof(float));
        }
        std::vector<float> sums(kPqCentroids * subdim);
        std::vector<uint32_t> counts(kPqCentroids);
        for (int iteration = 0; iteration < 4; ++iteration) {
            std::fill(sums.begin(), sums.end(), 0.0f);
            std::fill(counts.begin(), counts.end(), 0);
            for (size_t point = 0; point < sample_count; ++point) {
                const float* sub =
                    sample.data() + point * dim + chunk * subdim;
                size_t best = 0;
                const float* chunk_centroids =
                    centroids.data() +
                    static_cast<size_t>(chunk) * kPqCentroids * subdim;
                float best_distance =
                    chunk_len == 4
                        ? l2_sq4_aligned(sub, chunk_centroids)
                        : l2_sq_ptrs(sub, chunk_centroids, chunk_len);
                for (uint32_t center = 1; center < kPqCentroids; ++center) {
                    float distance =
                        chunk_len == 4
                            ? l2_sq4_aligned(
                                  sub, chunk_centroids + center * subdim)
                            : l2_sq_ptrs(
                                  sub,
                                  chunk_centroids + center * subdim,
                                  chunk_len);
                    if (distance < best_distance) {
                        best_distance = distance;
                        best = center;
                    }
                }
                ++counts[best];
                for (uint32_t element = 0; element < chunk_len; ++element) {
                    sums[best * subdim + element] += sub[element];
                }
            }
            for (uint32_t center = 0; center < kPqCentroids; ++center) {
                if (counts[center] == 0) {
                    continue;
                }
                float inverse = 1.0f / static_cast<float>(counts[center]);
                for (uint32_t element = 0; element < chunk_len; ++element) {
                    centroids[
                        (static_cast<size_t>(chunk) * kPqCentroids + center) *
                            subdim +
                        element] =
                        sums[center * subdim + element] * inverse;
                }
            }
        }
    };
    size_t train_workers = 1;
    {
        unsigned available = std::thread::hardware_concurrency();
        train_workers = available == 0
                            ? 1
                            : std::min<size_t>(4, available);
        train_workers = std::min<size_t>(train_workers, chunks);
    }
    if (train_workers <= 1) {
        for (uint32_t chunk = 0; chunk < chunks; ++chunk) {
            train_chunk(chunk);
        }
    } else {
        std::vector<std::thread> pool;
        std::vector<std::exception_ptr> errors(chunks);
        bool creation_failed = false;
        try {
            for (size_t worker = 0; worker < train_workers; ++worker) {
                pool.emplace_back([&, worker] {
                    try {
                        for (uint32_t chunk = static_cast<uint32_t>(worker);
                             chunk < chunks;
                             chunk += static_cast<uint32_t>(train_workers)) {
                            train_chunk(chunk);
                        }
                    } catch (...) {
                        errors[worker] = std::current_exception();
                    }
                });
            }
        } catch (...) {
            creation_failed = true;
        }
        for (std::thread& thread : pool) {
            thread.join();
        }
        if (creation_failed) {
            for (uint32_t chunk = 0; chunk < chunks; ++chunk) {
                train_chunk(chunk);
            }
        } else {
            for (const std::exception_ptr& error : errors) {
                if (error) {
                    std::rethrow_exception(error);
                }
            }
        }
    }
    AnnIndex index;
    index.dim = dim;
    index.subdim = subdim;
    index.chunks = chunks;
    index.rerank = 2048;
    index.seg_len = seg.len;
    index.entry_count = entries.size();
    index.record_count = records.size();
    index.schema_hash = schema_fingerprint(schema);
    index.centroids = std::move(centroids);
    index.codes.reserve(live_total * chunks);
    index.int_columns.resize(schema.columns.size());
    for (size_t column = 0; column < schema.columns.size(); ++column) {
        if (schema.columns[column].type.kind == TypeKind::BigInt) {
            index.int_columns[column] = std::vector<int64_t>{};
            index.int_columns[column]->reserve(live_total);
        }
    }
    index.pks.reserve(live_total);
    index.vector_offsets.reserve(live_total);
    bool use_avx = avx2_available();
    size_t row_index = 0;
    scan_rows([&](int64_t pk, uint64_t vector_offset, const uint8_t* vector,
                  const std::vector<int64_t>& ints) {
        for (uint32_t chunk = 0; chunk < chunks; ++chunk) {
            uint32_t chunk_len =
                std::min<uint32_t>(subdim, dim - chunk * subdim);
            const uint8_t* sub =
                vector + static_cast<size_t>(chunk) * subdim * sizeof(float);
            size_t best = 0;
            const float* chunk_centroids =
                index.centroids.data() +
                static_cast<size_t>(chunk) * kPqCentroids * subdim;
            float best_distance =
                chunk_len == 4
                    ? l2_sq4_raw(sub, chunk_centroids)
                    : l2_sq_raw(
                          sub, chunk_centroids, chunk_len, use_avx);
            for (uint32_t center = 1; center < kPqCentroids; ++center) {
                float distance =
                    chunk_len == 4
                        ? l2_sq4_raw(
                              sub, chunk_centroids + center * subdim)
                        : l2_sq_raw(
                              sub,
                              chunk_centroids + center * subdim,
                              chunk_len,
                              use_avx);
                if (distance < best_distance) {
                    best_distance = distance;
                    best = center;
                }
            }
            index.codes.push_back(static_cast<uint8_t>(best));
        }
        index.pks.push_back(pk);
        index.vector_offsets.push_back(vector_offset);
        for (size_t column = 0; column < schema.columns.size(); ++column) {
            if (index.int_columns[column].has_value()) {
                index.int_columns[column]->push_back(ints[column]);
            }
        }
        ++row_index;
    });
    if (row_index != live_total) {
        return false;
    }
    ann = std::move(index);
    return true;
}

bool Table::ensure_ann_index() {
    if (ann_budget_bytes > 0 &&
        estimate_ann_peak(*this) > ann_budget_bytes) {
        return false;
    }
    if (ann_valid()) {
        return true;
    }
    ann.reset();
    try {
        if (load_ann_index()) {
            return true;
        }
    } catch (...) {
        ann.reset();
    }
    if (entries.size() < kAnnMinRows || !build_ann_index()) {
        return false;
    }
    try {
        save_ann_index();
    } catch (...) {
        ann.reset();
        return false;
    }
    return true;
}

void Table::insert_cache(int64_t pk, const Row& row) {
    if (!cache.has_value()) {
        return;
    }
    size_t size = row_mem_size(row);
    if (cache_bytes + size > row_cache_budget) {
        cache.reset();
        hot.reset();
        cache_bytes = 0;
        return;
    }
    auto existing = cache->find(pk);
    if (existing != cache->end()) {
        cache_bytes -= row_mem_size(existing->second);
    }
    (*cache)[pk] = row;
    cache_bytes += size;
}

void Table::remove_cache(int64_t pk) {
    if (!cache.has_value()) {
        return;
    }
    auto found = cache->find(pk);
    if (found != cache->end()) {
        cache_bytes -= row_mem_size(found->second);
        cache->erase(found);
    }
}

std::optional<Row> Table::get_row(int64_t pk) {
    if (cache.has_value()) {
        auto cached = cache->find(pk);
        if (cached != cache->end()) {
            return cached->second;
        }
    }
    auto found = entries.find(pk);
    if (found == entries.end()) {
        return std::nullopt;
    }
    std::vector<Op> ops = seg.read_record(found->second.off, found->second.len, schema);
    for (Op& op : ops) {
        if (op.pk == pk) {
            if (op.kind == OpKind::Delete) {
                return std::nullopt;
            }
            return op.row;
        }
    }
    return std::nullopt;
}

std::optional<Row> Table::get_effective_row(
    const TxnOverlay* overlay,
    int64_t pk) {
    if (overlay != nullptr) {
        auto found = overlay->get(pk);
        if (found.has_value()) {
            return found.value();
        }
    }
    return get_row(pk);
}

void Table::commit_ops(const std::vector<Op>& ops, bool compact_after) {
    if (ops.empty()) {
        return;
    }
    hot.reset();
    ann.reset();
    auto appended = seg.append(schema, ops, fsync);
    uint64_t offset = appended.first;
    uint32_t record_length = appended.second;
    records.emplace_back(offset, record_length);
    uint32_t op_count = static_cast<uint32_t>(std::max<size_t>(ops.size(), 1));
    uint32_t share = record_length / op_count;
    for (const Op& op : ops) {
        if (op.kind != OpKind::Insert) {
            has_stale = true;
        } else if (entries.find(op.pk) != entries.end()) {
            has_stale = true;
        }
        if (op.kind == OpKind::Delete) {
            auto found = entries.find(op.pk);
            if (found != entries.end()) {
                live_bytes -= found->second.share;
                entries.erase(found);
            }
            remove_cache(op.pk);
            continue;
        }
        if (!op.row.has_value()) {
            fail_internal("missing row in commit");
        }
        auto inserted = entries.insert_or_assign(
            op.pk, Entry{offset, record_length, share});
        if (!inserted.second) {
            live_bytes -= inserted.first->second.share;
        }
        live_bytes += share;
        insert_cache(op.pk, op.row.value());
    }
    if (compact_after && should_compact()) {
        compact();
    }
}

bool Table::begin_statement_batch(bool discard_cache) {
    if (statement_batch_active || (cache.has_value() && !discard_cache)) {
        return false;
    }
    if (discard_cache) {
        cache.reset();
        cache_bytes = 0;
    }
    statement_batch_active = true;
    statement_batch_offset = seg.len;
    statement_batch_records = records.size();
    statement_batch_undo.clear();
    return true;
}

void Table::append_statement_chunk(std::vector<Op> ops) {
    if (!statement_batch_active || ops.empty()) {
        return;
    }
    hot.reset();
    ann.reset();
    auto appended = seg.append(schema, ops, false, kRecordFlagChunk);
    uint64_t offset = appended.first;
    uint32_t record_length = appended.second;
    records.emplace_back(offset, record_length);
    crash_point("statement_chunk_after_write");
    uint32_t op_count = static_cast<uint32_t>(std::max<size_t>(ops.size(), 1));
    uint32_t share = record_length / op_count;
    for (const Op& op : ops) {
        auto found = entries.find(op.pk);
        statement_batch_undo.emplace_back(
            op.pk,
            found == entries.end()
                ? std::nullopt
                : std::optional<Entry>(found->second));
        if (op.kind != OpKind::Insert) {
            has_stale = true;
        } else if (found != entries.end()) {
            has_stale = true;
        }
        if (op.kind == OpKind::Delete) {
            if (found != entries.end()) {
                live_bytes -= found->second.share;
                entries.erase(found);
            }
            remove_cache(op.pk);
            continue;
        }
        if (!op.row.has_value()) {
            fail_internal("missing row in statement chunk");
        }
        auto inserted = entries.insert_or_assign(
            op.pk, Entry{offset, record_length, share});
        if (!inserted.second) {
            live_bytes -= inserted.first->second.share;
        }
        live_bytes += share;
        insert_cache(op.pk, op.row.value());
    }
}

void Table::finish_statement_batch(bool compact_after) {
    if (!statement_batch_active) {
        return;
    }
    crash_point("statement_before_commit");
    auto appended = seg.append(schema, {}, true, kRecordFlagCommit);
    records.emplace_back(appended.first, appended.second);
    crash_point("statement_after_commit");
    statement_batch_active = false;
    statement_batch_undo.clear();
    if (compact_after && should_compact()) {
        compact();
    }
}

void Table::abort_statement_batch() {
    if (!statement_batch_active) {
        return;
    }
    for (auto iterator = statement_batch_undo.rbegin();
         iterator != statement_batch_undo.rend();
         ++iterator) {
        if (iterator->second.has_value()) {
            entries[iterator->first] = iterator->second.value();
        } else {
            entries.erase(iterator->first);
        }
    }
    live_bytes = 0;
    for (const auto& [pk, entry] : entries) {
        (void)pk;
        live_bytes += entry.share;
    }
    cache.reset();
    hot.reset();
    ann.reset();
    records.resize(statement_batch_records);
    truncate_segment(path, statement_batch_offset);
    seg.reopen();
    statement_batch_active = false;
    statement_batch_undo.clear();
}

void Table::compact() {
    ann.reset();
    std::string temporary = path + ".tmp";
    int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail_io("create compaction file", errno);
    }
    std::vector<std::tuple<int64_t, uint64_t, uint32_t>> new_entries;
    std::vector<std::pair<uint64_t, uint32_t>> new_records;
    new_entries.reserve(entries.size());
    new_records.reserve(entries.size());
    uint64_t offset = 0;
    size_t total_entries = entries.size();
    size_t written_entries = 0;
    try {
        for (const auto& [pk, entry] : entries) {
            (void)entry;
            std::optional<Row> row = get_row(pk);
            if (!row.has_value()) {
                continue;
            }
            Op op;
            op.kind = OpKind::Insert;
            op.pk = pk;
            op.row = row;
            std::vector<Op> ops{op};
            std::vector<uint8_t> record = encode_record(schema, ops);
            write_all_fd(fd, record.data(), record.size(), "write compaction record");
            new_entries.emplace_back(pk, offset, static_cast<uint32_t>(record.size()));
            new_records.emplace_back(offset, static_cast<uint32_t>(record.size()));
            offset += record.size();
            ++written_entries;
            if (total_entries >= 2 && written_entries == total_entries / 2) {
                crash_point("checkpoint_mid");
            }
        }
        if (::fsync(fd) != 0) {
            fail_io("fsync compaction file", errno);
        }
    } catch (...) {
        ::close(fd);
        std::filesystem::remove(temporary);
        throw;
    }
    ::close(fd);
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        int error = errno;
        std::filesystem::remove(temporary);
        fail_io("replace segment with compaction", error);
    }
    crash_point("checkpoint_after_rename");
    fsync_dir(std::filesystem::path(path).parent_path().string());
    seg.reopen();
    for (const auto& [pk, new_offset, new_length] : new_entries) {
        auto found = entries.find(pk);
        if (found != entries.end()) {
            found->second.off = new_offset;
            found->second.len = new_length;
            found->second.share = new_length;
        }
    }
    live_bytes = offset;
    records = std::move(new_records);
    has_stale = false;
}

void Table::for_each_row_merged(
    const TxnOverlay* overlay,
    const std::function<bool(int64_t, const Row&)>& visit) {
    bool overlay_empty = overlay == nullptr || overlay->empty();
    if (overlay_empty && cache_covers_all()) {
        for (const auto& [pk, row] : cache.value()) {
            if (!visit(pk, row)) {
                return;
            }
        }
        return;
    }
    for (const auto& [pk, entry] : entries) {
        (void)entry;
        if (overlay != nullptr) {
            auto value = overlay->get(pk);
            if (value.has_value()) {
                if (value.value().has_value() &&
                    !visit(pk, value.value().value())) {
                    return;
                }
                continue;
            }
        }
        std::optional<Row> row = get_row(pk);
        if (row.has_value() && !visit(pk, row.value())) {
            return;
        }
    }
    if (overlay != nullptr) {
        overlay->for_each([&](int64_t pk, const TxnOverlay::EntryValue& value) {
            if (entries.find(pk) == entries.end() && value.has_value()) {
                return visit(pk, value.value());
            }
            return true;
        });
    }
}

void Table::for_each_row_streaming(
    const TxnOverlay* overlay,
    const std::function<bool(int64_t, const Row&)>& visit) {
    bool stopped = false;
    scan_segment(path, schema, [&](uint64_t offset, uint32_t, uint8_t, std::vector<Op> ops) {
        if (stopped) {
            return;
        }
        for (Op& op : ops) {
            if (op.kind == OpKind::Delete || !op.row.has_value()) {
                continue;
            }
            auto found = entries.find(op.pk);
            if (found == entries.end() || found->second.off != offset) {
                continue;
            }
            if (overlay != nullptr && overlay->contains(op.pk)) {
                continue;
            }
            if (!visit(op.pk, op.row.value())) {
                stopped = true;
                return;
            }
        }
    });
    if (stopped || overlay == nullptr) {
        return;
    }
    overlay->for_each([&](int64_t pk, const TxnOverlay::EntryValue& value) {
        if (value.has_value()) {
            return visit(pk, value.value());
        }
        return true;
    });
}

Database Database::open(
    const std::string& directory,
    size_t cache_budget,
    bool fsync,
    uint64_t compact_min) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw DbError(1105, "HY000", "create data directory: " + error.message());
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".tmp") {
            std::filesystem::remove(entry.path(), error);
        }
    }
    Database database;
    database.dir = directory;
    database.cache_budget = cache_budget;
    database.fsync = fsync;
    database.compact_min = compact_min;
    std::vector<Schema> schemas = load_catalog(directory + "/catalog.bin");
    for (Schema& schema : schemas) {
        std::string key = lower_ascii(schema.table);
        std::string path = table_path(directory, schema.table);
        Table table = Table::recover(
            std::move(schema), path, cache_budget, fsync, compact_min);
        database.tables.emplace(key, std::move(table));
    }
    database.recover_cross_transactions();
    return database;
}

size_t Database::table_count() const {
    return tables.size();
}

uint64_t Database::row_count() const {
    uint64_t total = 0;
    for (const auto& [key, table] : tables) {
        (void)key;
        total += table.entries.size();
    }
    return total;
}

void Database::preload_ann_indexes() {
    if (!ann_mode) {
        return;
    }
    for (auto& [key, table] : tables) {
        (void)key;
        if (!table.schema.has_vector_index()) {
            continue;
        }
        if (table.entries.size() < kAnnMinRows) {
            continue;
        }
        if (table.ann_budget_bytes > 0 &&
            estimate_ann_peak(table) > table.ann_budget_bytes) {
            continue;
        }
        try {
            table.load_ann_index();
        } catch (...) {
            table.ann.reset();
        }
    }
}

void Database::set_ann_budget(size_t bytes) {
    ann_budget = bytes;
    for (auto& [key, table] : tables) {
        (void)key;
        table.ann_budget_bytes = bytes;
    }
}

void Database::save_catalog() {
    std::vector<Schema> schemas;
    schemas.reserve(tables.size());
    for (const auto& [key, table] : tables) {
        (void)key;
        schemas.push_back(table.schema);
    }
    ::save_catalog(dir + "/catalog.bin", schemas);
}

Response Database::execute_large_insert(
    const std::string& sql,
    std::optional<Txn>& txn) {
    if (!txn.has_value() && autocommit) {
        Table* batch_table = nullptr;
        std::string batch_key;
        bool started = false;
        uint64_t total = 0;
        try {
            bool split = for_each_insert_chunk(
                sql,
                4096,
                [&](const std::string& chunk) {
                    Stmt statement = parse_statement(chunk);
                    if (statement.kind != StmtKind::Insert) {
                        return false;
                    }
                    std::string key = lower_ascii(statement.table);
                    auto found = tables.find(key);
                    if (found == tables.end()) {
                        throw DbError(
                            1146,
                            "42S02",
                            "Table 'minivdb." + statement.table +
                                "' doesn't exist");
                    }
                    if (batch_table == nullptr) {
                        batch_table = &found->second;
                        batch_key = key;
                        if (!batch_table->begin_statement_batch()) {
                            batch_table = nullptr;
                            return false;
                        }
                        started = true;
                    } else if (batch_key != key) {
                        fail_internal(
                            "large INSERT chunks span multiple tables");
                    }
                    Response response = insert(statement, txn, batch_table);
                    total += response.affected;
                    return true;
                });
            if (!split) {
                if (started) {
                    batch_table->abort_statement_batch();
                }
                return execute(parse_statement(sql), txn);
            }
            if (started) {
                batch_table->finish_statement_batch();
            }
            return Response::changed(total);
        } catch (...) {
            if (started && batch_table != nullptr) {
                try {
                    batch_table->abort_statement_batch();
                } catch (...) {
                }
            }
            throw;
        }
    }

    // The statement belongs to a transaction. Stage it in a spillable txn
    // overlay so a duplicate or parse error in a later chunk cannot leave
    // earlier chunks visible, and the full INSERT text is never materialized
    // as one parsed statement.
    std::optional<Txn> staging;
    std::string staging_key;
    uint64_t total = 0;
    bool split = for_each_insert_chunk(
        sql,
        4096,
        [&](const std::string& chunk) {
            Stmt statement = parse_statement(chunk);
            if (statement.kind != StmtKind::Insert) {
                return false;
            }
            std::string key = lower_ascii(statement.table);
            if (tables.find(key) == tables.end()) {
                throw DbError(
                    1146,
                    "42S02",
                    "Table 'minivdb." + statement.table + "' doesn't exist");
            }
            if (!staging.has_value()) {
                staging = Txn{};
                staging_key = key;
            } else if (staging_key != key) {
                fail_internal("large INSERT chunks span multiple tables");
            }
            const TxnOverlay* prior = overlay_for(txn, key);
            Response response =
                insert(statement, staging, nullptr, prior);
            total += response.affected;
            return true;
        });
    if (!split) {
        return execute(parse_statement(sql), txn);
    }
    if (!staging.has_value()) {
        return Response::changed(total);
    }
    auto staged = staging->tables.find(staging_key);
    if (staged == staging->tables.end()) {
        return Response::changed(total);
    }

    if (!txn.has_value()) {
        txn = Txn{};
    }
    auto& target = txn->tables[staging_key];
    target.configure(dir, staging_key);
    staged->second.for_each(
        [&](int64_t pk, const TxnOverlay::EntryValue& value) {
            target.set(pk, value);
            return true;
        });
    return Response::changed(total);
}

Response Database::execute(const Stmt& statement, std::optional<Txn>& txn) {
    if (!statement.table.empty()) {
        std::string table_key = lower_ascii(statement.table);
        if (table_key.rfind("__mvdb", 0) == 0) {
            if (statement.kind == StmtKind::CreateTable) {
                throw DbError(
                    1235,
                    "42000",
                    "table names starting with '__mvdb' are reserved");
            }
            throw DbError(
                1146,
                "42S02",
                "Table 'minivdb." + statement.table + "' doesn't exist");
        }
    }
    switch (statement.kind) {
        case StmtKind::Begin:
            if (txn.has_value()) {
                throw DbError(1105, "HY000", "a transaction is already in progress");
            }
            txn = Txn{};
            return Response::ok();
        case StmtKind::Commit:
            return commit(txn);
        case StmtKind::Rollback:
            txn.reset();
            return Response::ok();
        case StmtKind::SetAutocommit:
            if (statement.autocommit && txn.has_value()) {
                commit(txn);
            }
            autocommit = statement.autocommit;
            return Response::ok();
        case StmtKind::Noop:
            return Response::ok();
        case StmtKind::CreateTable:
            return create_table(statement, txn);
        case StmtKind::CreateIndex:
            return create_index(statement, txn);
        case StmtKind::Insert:
            return insert(statement, txn);
        case StmtKind::Update:
            return update(statement, txn);
        case StmtKind::Delete:
            return remove(statement, txn);
        case StmtKind::Select:
            return select(statement, txn);
    }
    throw DbError(1235, "42000", "unsupported statement");
}

Response Database::create_table(const Stmt& statement, std::optional<Txn>& txn) {
    if (txn.has_value()) {
        throw DbError(1235, "42000", "CREATE TABLE is not allowed inside a transaction");
    }
    std::string key = lower_ascii(statement.table);
    if (tables.find(key) != tables.end()) {
        if (statement.if_not_exists) {
            return Response::ok();
        }
        throw DbError(1050, "42S01", "Table '" + statement.table + "' already exists");
    }
    std::set<std::string> names;
    for (const ColumnDef& column : statement.columns) {
        std::string lower = lower_ascii(column.name);
        if (!names.insert(lower).second) {
            throw DbError(1060, "42S21", "Duplicate column name '" + column.name + "'");
        }
    }
    std::vector<std::string> inline_primary;
    for (const ColumnDef& column : statement.columns) {
        if (column.primary) {
            inline_primary.push_back(column.name);
        }
    }
    if (inline_primary.size() > 1 ||
        (!inline_primary.empty() && statement.table_pk.has_value())) {
        throw DbError(1068, "42000", "Multiple primary key defined");
    }
    std::string pk_name;
    if (!inline_primary.empty()) {
        pk_name = inline_primary.front();
    } else if (statement.table_pk.has_value()) {
        pk_name = statement.table_pk.value();
    } else {
        throw DbError(1235, "42000", "a BIGINT PRIMARY KEY column is required");
    }
    std::optional<size_t> pk_index;
    for (size_t index = 0; index < statement.columns.size(); ++index) {
        if (iequals(statement.columns[index].name, pk_name)) {
            pk_index = index;
            break;
        }
    }
    if (!pk_index.has_value()) {
        throw DbError(1054, "42S22", "Unknown column '" + pk_name + "'");
    }
    if (statement.columns[pk_index.value()].type.kind != TypeKind::BigInt) {
        throw DbError(1235, "42000", "the primary key column must be BIGINT");
    }
    size_t vector_count = 0;
    for (const ColumnDef& column : statement.columns) {
        if (column.type.kind == TypeKind::Vector) {
            ++vector_count;
        }
    }
    if (vector_count > 1) {
        throw DbError(1235, "42000", "a table may have at most one VECTOR column");
    }
    Schema schema;
    schema.table = statement.table;
    schema.pk = pk_index.value();
    for (const ColumnDef& column : statement.columns) {
        schema.columns.push_back(Column{column.name, column.type});
    }
    std::string path = table_path(dir, statement.table);
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail_io("create table file " + path, errno);
    }
    if (::fsync(fd) != 0) {
        int error = errno;
        ::close(fd);
        fail_io("fsync table file", error);
    }
    ::close(fd);
    fsync_dir(dir);
    Table table;
    table.schema = std::move(schema);
    table.path = path;
    table.seg = Segment::open(path);
    table.cache = std::map<int64_t, Row>{};
    table.cache_budget = cache_budget;
    table.row_cache_budget = std::min(cache_budget, kRowCacheLimit);
    table.ann_budget_bytes = ann_budget;
    table.fsync = fsync;
    table.compact_min = compact_min;
    tables.emplace(key, std::move(table));
    save_catalog();
    return Response::ok();
}

Response Database::create_index(const Stmt& statement, std::optional<Txn>& txn) {
    if (txn.has_value()) {
        throw DbError(1235, "42000", "CREATE INDEX is not allowed inside a transaction");
    }
    std::string key = lower_ascii(statement.table);
    auto found = tables.find(key);
    if (found == tables.end()) {
        throw DbError(
            1146, "42S02", "Table 'minivdb." + statement.table + "' doesn't exist");
    }
    Table& table = found->second;
    std::optional<size_t> index = table.schema.col_index(statement.column);
    if (!index.has_value()) {
        throw DbError(1054, "42S22", "Unknown column '" + statement.column + "'");
    }
    if (table.schema.columns[index.value()].type.kind != TypeKind::Vector) {
        throw DbError(1235, "42000", "only VECTOR columns can have a vector index");
    }
    std::string index_name = lower_ascii(statement.name);
    for (const std::string& existing : table.schema.vector_indexes) {
        if (iequals(existing, index_name)) {
            if (statement.if_not_exists) {
                return Response::ok();
            }
            throw DbError(
                1061, "42000", "Duplicate key name '" + statement.name + "'");
        }
    }
    bool first_index = table.schema.vector_indexes.empty();
    if (first_index) {
        table.ann.reset();
        // In ANN mode the PQ sidecar is the query structure; do not also keep
        // the columnar exact cache resident. Exact requests build it lazily.
        if (!ann_mode) {
            table.ensure_hot();
        }
    }
    // Additional names are aliases for the same physical vector index.
    table.schema.vector_indexes.push_back(index_name);
    save_catalog();
    if (first_index && ann_mode) {
        try {
            table.ensure_ann_index();
        } catch (...) {
            table.ann.reset();
        }
    }
    return Response::ok();
}

Response Database::insert(
    const Stmt& statement,
    std::optional<Txn>& txn,
    Table* batch_table,
    const TxnOverlay* prior_overlay) {
    std::string key = lower_ascii(statement.table);
    auto found = tables.find(key);
    if (found == tables.end()) {
        throw DbError(
            1146, "42S02", "Table 'minivdb." + statement.table + "' doesn't exist");
    }
    Table& table = found->second;
    const Schema& schema = table.schema;
    std::vector<size_t> indices;
    if (statement.insert_columns.has_value()) {
        std::set<size_t> seen;
        for (const std::string& name : statement.insert_columns.value()) {
            std::optional<size_t> index = schema.col_index(name);
            if (!index.has_value()) {
                throw DbError(1054, "42S22", "Unknown column '" + name + "'");
            }
            if (!seen.insert(index.value()).second) {
                throw DbError(1110, "42000", "Column '" + name + "' specified twice");
            }
            indices.push_back(index.value());
        }
    } else {
        for (size_t index = 0; index < schema.columns.size(); ++index) {
            indices.push_back(index);
        }
    }
    bool pk_present = false;
    for (size_t index : indices) {
        if (index == schema.pk) {
            pk_present = true;
        }
    }
    if (!pk_present) {
        throw DbError(
            1364,
            "HY000",
            "Field '" + schema.columns[schema.pk].name +
                "' doesn't have a default value");
    }
    const auto* overlay = overlay_for(txn, key);
    std::vector<Op> ops;
    std::set<int64_t> seen_pks;
    for (size_t row_no = 0; row_no < statement.rows.size(); ++row_no) {
        const std::vector<Literal>& literals = statement.rows[row_no];
        if (literals.size() != indices.size()) {
            throw DbError(
                1136,
                "21S01",
                "Column count doesn't match value count at row " +
                    std::to_string(row_no + 1));
        }
        std::vector<Value> values(schema.columns.size(), Value::integer(0));
        std::vector<bool> present(schema.columns.size(), false);
        for (size_t index = 0; index < literals.size(); ++index) {
            size_t column = indices[index];
            values[column] = literal_to_value(
                literals[index],
                schema.columns[column].type,
                schema.columns[column].name,
                row_no + 1);
            present[column] = true;
        }
        for (size_t index = 0; index < schema.columns.size(); ++index) {
            if (!present[index]) {
                throw DbError(
                    1364,
                    "HY000",
                    "Field '" + schema.columns[index].name +
                        "' doesn't have a default value");
            }
        }
        if (values[schema.pk].kind != Value::Kind::Int) {
            throw DbError(1105, "HY000", "primary key is not BIGINT");
        }
        int64_t pk = values[schema.pk].i;
        if (!seen_pks.insert(pk).second) {
            throw DbError(
                1062, "23000", "Duplicate entry '" + std::to_string(pk) + "' for key 'PRIMARY'");
        }
        std::optional<TxnOverlay::EntryValue> staged;
        if (overlay != nullptr) {
            staged = overlay->get(pk);
        }
        std::optional<TxnOverlay::EntryValue> prior;
        if (prior_overlay != nullptr) {
            prior = prior_overlay->get(pk);
        }
        bool occupied = false;
        if (staged.has_value()) {
            occupied = staged->has_value();
        } else if (prior.has_value()) {
            occupied = prior->has_value();
        } else {
            occupied = table.get_effective_row(nullptr, pk).has_value();
        }
        if (occupied) {
            throw DbError(
                1062, "23000", "Duplicate entry '" + std::to_string(pk) + "' for key 'PRIMARY'");
        }
        Op op;
        op.kind = OpKind::Insert;
        op.pk = pk;
        op.row = Row{std::move(values)};
        ops.push_back(std::move(op));
    }
    uint64_t affected = ops.size();
    apply_ops(key, txn, std::move(ops), batch_table);
    return Response::changed(affected);
}

Response Database::update(const Stmt& statement, std::optional<Txn>& txn) {
    std::string key = lower_ascii(statement.table);
    auto found = tables.find(key);
    if (found == tables.end()) {
        throw DbError(
            1146, "42S02", "Table 'minivdb." + statement.table + "' doesn't exist");
    }
    Table& table = found->second;
    const Schema& schema = table.schema;
    Predicate predicate = resolve_predicate(statement.filter, schema);
    std::vector<std::pair<size_t, Value>> assignments;
    std::set<size_t> seen;
    for (const auto& [name, literal] : statement.sets) {
        std::optional<size_t> index = schema.col_index(name);
        if (!index.has_value()) {
            throw DbError(1054, "42S22", "Unknown column '" + name + "'");
        }
        if (index.value() == schema.pk) {
            throw DbError(1235, "42000", "updating the primary key is not supported");
        }
        if (!seen.insert(index.value()).second) {
            throw DbError(1110, "42000", "Column '" + name + "' specified twice");
        }
        assignments.emplace_back(
            index.value(),
            literal_to_value(
                literal,
                schema.columns[index.value()].type,
                schema.columns[index.value()].name,
                1));
    }
    const auto* overlay = overlay_for(txn, key);
    if (!txn.has_value() && autocommit &&
        !table.cache.has_value() &&
        table.entries.size() >= kStatementChunkRows &&
        table.begin_statement_batch()) {
        uint64_t changed = 0;
        uint64_t matched_count = 0;
        std::vector<Op> chunk;
        try {
            auto stream_visit = [&](int64_t pk, const Row& row) {
                if (!predicate.matches(row)) {
                    return true;
                }
                ++matched_count;
                Row updated = row;
                for (const auto& [column, value] : assignments) {
                    updated.values[column] = value;
                }
                if (updated.values != row.values) {
                    ++changed;
                    Op op;
                    op.kind = OpKind::Update;
                    op.pk = pk;
                    op.row = std::move(updated);
                    chunk.push_back(std::move(op));
                    if (chunk.size() >= kStatementChunkRows) {
                        table.append_statement_chunk(std::move(chunk));
                        chunk.clear();
                    }
                }
                return true;
            };
            table.for_each_row_streaming(nullptr, stream_visit);
            if (!chunk.empty()) {
                table.append_statement_chunk(std::move(chunk));
            }
            if (changed > 0) {
                table.finish_statement_batch();
            } else {
                table.abort_statement_batch();
            }
        } catch (...) {
            try {
                table.abort_statement_batch();
            } catch (...) {
            }
            throw;
        }
        uint64_t affected = affected_rows_matched ? matched_count : changed;
        return Response::changed(affected);
    }

    std::vector<std::pair<int64_t, Row>> matched;
    auto visit = [&](int64_t pk, const Row& row) {
        if (predicate.matches(row)) {
            matched.emplace_back(pk, row);
        }
        return true;
    };
    if (table.cache.has_value()) {
        table.for_each_row_merged(overlay, visit);
    } else {
        table.for_each_row_streaming(overlay, visit);
    }
    std::vector<Op> ops;
    uint64_t changed = 0;
    for (auto& [pk, original] : matched) {
        Row updated = original;
        for (const auto& [column, value] : assignments) {
            updated.values[column] = value;
        }
        if (updated.values != original.values) {
            ++changed;
            Op op;
            op.kind = OpKind::Update;
            op.pk = pk;
            op.row = std::move(updated);
            ops.push_back(std::move(op));
        }
    }
    uint64_t affected = affected_rows_matched
                            ? static_cast<uint64_t>(matched.size())
                            : changed;
    apply_ops(key, txn, std::move(ops));
    return Response::changed(affected);
}

Response Database::remove(const Stmt& statement, std::optional<Txn>& txn) {
    std::string key = lower_ascii(statement.table);
    auto found = tables.find(key);
    if (found == tables.end()) {
        throw DbError(
            1146, "42S02", "Table 'minivdb." + statement.table + "' doesn't exist");
    }
    Table& table = found->second;
    Predicate predicate = resolve_predicate(statement.filter, table.schema);
    const auto* overlay = overlay_for(txn, key);
    if (!txn.has_value() && autocommit &&
        !table.cache.has_value() &&
        table.entries.size() >= kStatementChunkRows &&
        table.begin_statement_batch()) {
        uint64_t affected = 0;
        std::vector<Op> chunk;
        try {
            auto stream_visit = [&](int64_t pk, const Row& row) {
                if (!predicate.matches(row)) {
                    return true;
                }
                Op op;
                op.kind = OpKind::Delete;
                op.pk = pk;
                chunk.push_back(std::move(op));
                ++affected;
                if (chunk.size() >= kStatementChunkRows) {
                    table.append_statement_chunk(std::move(chunk));
                    chunk.clear();
                }
                return true;
            };
            table.for_each_row_streaming(nullptr, stream_visit);
            if (!chunk.empty()) {
                table.append_statement_chunk(std::move(chunk));
            }
            if (affected > 0) {
                table.finish_statement_batch();
            } else {
                table.abort_statement_batch();
            }
        } catch (...) {
            try {
                table.abort_statement_batch();
            } catch (...) {
            }
            throw;
        }
        return Response::changed(affected);
    }

    std::vector<int64_t> pks;
    auto visit = [&](int64_t pk, const Row& row) {
        if (predicate.matches(row)) {
            pks.push_back(pk);
        }
        return true;
    };
    if (table.cache.has_value()) {
        table.for_each_row_merged(overlay, visit);
    } else {
        table.for_each_row_streaming(overlay, visit);
    }
    std::vector<Op> ops;
    ops.reserve(pks.size());
    for (int64_t pk : pks) {
        Op op;
        op.kind = OpKind::Delete;
        op.pk = pk;
        ops.push_back(std::move(op));
    }
    uint64_t affected = ops.size();
    apply_ops(key, txn, std::move(ops));
    return Response::changed(affected);
}

Response Database::select(const Stmt& statement, std::optional<Txn>& txn) {
    std::string key = lower_ascii(statement.table);
    auto found = tables.find(key);
    if (found == tables.end()) {
        throw DbError(
            1146, "42S02", "Table 'minivdb." + statement.table + "' doesn't exist");
    }
    Table& table = found->second;
    const Schema& schema = table.schema;
    const size_t result_limit = result_limit_bytes;
    const auto* overlay = overlay_for(txn, key);
    std::vector<Projection> projections = resolve_projections(statement.items, schema);
    std::vector<std::string> columns;
    columns.reserve(projections.size());
    for (const Projection& projection : projections) {
        columns.push_back(projection.header);
    }
    if (columns.empty()) {
        columns.push_back("result");
    }
    Predicate predicate = resolve_predicate(statement.filter, schema);
    std::vector<OrderSpec> resolved_orders;
    resolved_orders.reserve(statement.orders.size());
    for (const OrderBy& order_by : statement.orders) {
        resolved_orders.push_back(resolve_order(order_by, projections, schema));
    }
    bool pk_only_projection = !projections.empty();
    for (const Projection& projection : projections) {
        if (projection.kind != Projection::Kind::Column ||
            projection.column != schema.pk) {
            pk_only_projection = false;
            break;
        }
    }
    auto project_pk = [&](int64_t pk) {
        std::vector<OutputValue> values;
        values.reserve(projections.size());
        for (size_t index = 0; index < projections.size(); ++index) {
            values.push_back(OutputValue::integer(pk));
        }
        return values;
    };
    std::optional<OrderSpec> order;
    if (resolved_orders.size() == 1) {
        order = resolved_orders.front();
    }
    if (statement.limit.has_value() && statement.limit.value() == 0) {
        QueryResult result;
        result.columns = columns;
        return Response::result(std::move(result));
    }
    size_t limit = statement.limit.has_value()
                       ? static_cast<size_t>(statement.limit.value())
                       : std::numeric_limits<size_t>::max();

    // With a large table the decoded-row cache is disabled; scan the segment
    // sequentially instead of doing one random record read per row.
    auto visit_rows = [&](const std::function<bool(int64_t, const Row&)>& visit) {
        if (table.cache.has_value()) {
            table.for_each_row_merged(overlay, visit);
        } else {
            table.for_each_row_streaming(overlay, visit);
        }
    };

    if (resolved_orders.size() > 1) {
        // Multi-key ORDER BY is an isolated fallback. The single-key paths
        // below keep their existing HotSet/ANN/bounded-heap fast paths.
        struct MultiCandidate {
            std::vector<KeyVal> keys;
            int64_t pk = 0;
        };
        struct MultiCompare {
            const std::vector<OrderSpec>* orders;
            bool operator()(
                const MultiCandidate& left,
                const MultiCandidate& right) const {
                for (size_t index = 0; index < orders->size(); ++index) {
                    int value = left.keys[index].compare(right.keys[index]);
                    if (value != 0) {
                        return (*orders)[index].asc ? value < 0 : value > 0;
                    }
                }
                return left.pk < right.pk;
            }
        };
        auto make_candidate = [&](int64_t pk, const Row& row) {
            MultiCandidate candidate;
            candidate.pk = pk;
            candidate.keys.reserve(resolved_orders.size());
            for (const OrderSpec& spec : resolved_orders) {
                candidate.keys.push_back(
                    compute_order_key(row, spec, projections));
            }
            return candidate;
        };
        std::vector<MultiCandidate> candidates;
        MultiCompare compare{&resolved_orders};
        if (statement.limit.has_value()) {
            std::priority_queue<
                MultiCandidate,
                std::vector<MultiCandidate>,
                MultiCompare>
                heap(compare);
            visit_rows([&](int64_t pk, const Row& row) {
                if (!predicate.matches(row)) {
                    return true;
                }
                MultiCandidate candidate = make_candidate(pk, row);
                if (heap.size() < limit) {
                    heap.push(std::move(candidate));
                } else if (compare(candidate, heap.top())) {
                    heap.pop();
                    heap.push(std::move(candidate));
                }
                return true;
            });
            candidates.reserve(heap.size());
            while (!heap.empty()) {
                candidates.push_back(std::move(heap.top()));
                heap.pop();
            }
        } else {
            struct SortRuns {
                std::vector<std::string> paths;
                ~SortRuns() {
                    for (const std::string& path : paths) {
                        std::remove(path.c_str());
                    }
                }
            } runs;
            constexpr size_t kExternalRows = 100000;
            std::vector<MultiCandidate> buffer;
            auto flush = [&]() {
                if (buffer.empty()) {
                    return;
                }
                std::sort(buffer.begin(), buffer.end(), compare);
                std::string path = make_sort_path(dir);
                std::ofstream output(path, std::ios::binary | std::ios::trunc);
                if (!output) {
                    std::remove(path.c_str());
                    fail_internal("create external multi-key sort run");
                }
                for (const MultiCandidate& item : buffer) {
                    write_multi_sort_record(output, item.pk, item.keys);
                }
                output.close();
                runs.paths.push_back(path);
                buffer.clear();
            };
            visit_rows([&](int64_t pk, const Row& row) {
                if (predicate.matches(row)) {
                    buffer.push_back(make_candidate(pk, row));
                    if (buffer.size() >= kExternalRows) {
                        flush();
                    }
                }
                return true;
            });
            QueryResult result;
            result.columns = columns;
            auto emit = [&](const MultiCandidate& item) {
                if (pk_only_projection) {
                    append_result_row(result, project_pk(item.pk), result_limit);
                    return;
                }
                std::optional<Row> row =
                    table.get_effective_row(overlay, item.pk);
                if (row.has_value()) {
                    append_result_row(
                        result,
                        project_row(row.value(), projections),
                        result_limit);
                }
            };
            if (runs.paths.empty()) {
                std::sort(buffer.begin(), buffer.end(), compare);
                result.rows.reserve(buffer.size());
                for (const MultiCandidate& item : buffer) {
                    emit(item);
                }
                return Response::result(std::move(result));
            }
            flush();
            struct RunItem {
                std::vector<KeyVal> keys;
                int64_t pk;
                size_t run;
            };
            auto run_compare = [&](const RunItem& left, const RunItem& right) {
                for (size_t index = 0; index < resolved_orders.size(); ++index) {
                    int value = left.keys[index].compare(right.keys[index]);
                    if (value != 0) {
                        return resolved_orders[index].asc ? value > 0 : value < 0;
                    }
                }
                return left.pk > right.pk;
            };
            struct RunCompare {
                std::function<bool(const RunItem&, const RunItem&)> less;
                bool operator()(const RunItem& left, const RunItem& right) const {
                    return less(left, right);
                }
            };
            std::priority_queue<RunItem, std::vector<RunItem>, RunCompare> heap(
                RunCompare{run_compare});
            std::vector<std::ifstream> inputs;
            inputs.reserve(runs.paths.size());
            for (size_t index = 0; index < runs.paths.size(); ++index) {
                inputs.emplace_back(runs.paths[index], std::ios::binary);
                if (!inputs.back()) {
                    fail_internal("open external multi-key sort run");
                }
                RunItem item;
                if (read_multi_sort_record(inputs.back(), item.pk, item.keys)) {
                    item.run = index;
                    heap.push(std::move(item));
                }
            }
            while (!heap.empty()) {
                RunItem item = heap.top();
                heap.pop();
                emit(MultiCandidate{std::move(item.keys), item.pk});
                RunItem next;
                if (read_multi_sort_record(
                        inputs[item.run], next.pk, next.keys)) {
                    next.run = item.run;
                    heap.push(std::move(next));
                }
            }
            return Response::result(std::move(result));
        }
        std::sort(candidates.begin(), candidates.end(), compare);
        QueryResult result;
        result.columns = columns;
        result.rows.reserve(candidates.size());
        for (const MultiCandidate& candidate : candidates) {
            if (pk_only_projection) {
                append_result_row(result, project_pk(candidate.pk), result_limit);
                continue;
            }
            std::optional<Row> row =
                table.get_effective_row(overlay, candidate.pk);
            if (row.has_value()) {
                append_result_row(
                    result,
                    project_row(row.value(), projections),
                    result_limit);
            }
        }
        return Response::result(std::move(result));
    }

    if (!order.has_value()) {
        // Primary-key equality is a point lookup; avoid scanning the segment.
        const Predicate::Clause* pk_clause = nullptr;
        for (const Predicate::Clause& clause : predicate.clauses) {
            if (clause.column == schema.pk && clause.op == CmpOp::Eq &&
                clause.value.kind == Value::Kind::Int) {
                pk_clause = &clause;
                break;
            }
        }
        if (pk_clause != nullptr) {
            QueryResult result;
            result.columns = columns;
            std::optional<Row> row =
                table.get_effective_row(overlay, pk_clause->value.i);
            if (row.has_value() && predicate.matches(row.value())) {
                append_result_row(
                    result,
                    project_row(row.value(), projections),
                    result_limit);
            }
            return Response::result(std::move(result));
        }
    }

    if (!order.has_value()) {
        QueryResult result;
        result.columns = columns;
        std::optional<ResultSpool> spool;
        uint64_t count = 0;
        visit_rows([&](int64_t, const Row& row) {
            if (predicate.matches(row)) {
                std::vector<OutputValue> values = project_row(row, projections);
                if (!spool_results ||
                    (!spool && result.rows.size() < kResultSpillRows)) {
                    append_result_row(result, std::move(values), result_limit);
                } else {
                    if (!spool) {
                        spool.emplace();
                        for (const auto& pending : result.rows) {
                            spool->write_row(pending);
                        }
                        result.rows.clear();
                    }
                    spool->write_row(values);
                }
                ++count;
            }
            return count < limit;
        });
        if (spool.has_value()) {
            result.spill_path = spool->finish();
            result.spill_rows = count;
        }
        return Response::result(std::move(result));
    }

    bool asc = order->asc;

    struct DistanceTarget {
        size_t column;
        const std::vector<float>* query;
    };
    std::optional<DistanceTarget> distance_target;
    if (order->kind == OrderSpec::Kind::Distance) {
        distance_target = DistanceTarget{order->column, &order->query};
    } else if (order->kind == OrderSpec::Kind::Projection) {
        const Projection& projection = projections[order->projection];
        if (projection.kind == Projection::Kind::Distance) {
            distance_target = DistanceTarget{projection.column, &projection.query};
        }
    }

    if (ann_mode && asc && statement.limit.has_value() &&
        distance_target.has_value() && table.schema.has_vector_index() &&
        (overlay == nullptr || overlay->empty())) {
        try {
            std::optional<QueryResult> ann_result = ann_select(
                table,
                schema,
                predicate,
                projections,
                columns,
                distance_target->column,
                *distance_target->query,
                limit,
                ann_rerank,
                threads,
                result_limit);
            if (ann_result.has_value()) {
                return Response::result(std::move(ann_result.value()));
            }
        } catch (...) {
            // The ANN index is a cache; any index-specific failure falls back
            // to the exact paths below.
        }
    }

    auto ensure_hot_for_predicate = [&]() {
        if (!table.ensure_hot() || !table.hot.has_value()) {
            return false;
        }
        for (const Predicate::Clause& clause : predicate.clauses) {
            if (schema.columns[clause.column].type.kind != TypeKind::Varchar) {
                continue;
            }
            if (!table.ensure_hot_string_column(clause.column)) {
                return false;
            }
        }
        return true;
    };

    if (statement.limit.has_value() && limit <= 1000000 &&
        distance_target.has_value() && (overlay == nullptr || overlay->empty()) &&
        predicate_hot_compatible_with_strings(predicate, schema) &&
        ensure_hot_for_predicate() && table.hot.has_value() &&
        table.hot->dim == distance_target->query->size()) {
        // Exact bounded TopK. A direct-distance f32 screen rejects only rows
        // outside a conservative rounding bound; the heap uses f64 scores.
        const HotSet& hot = table.hot.value();
        size_t keep = limit;
        struct HotCandidate {
            double distance;
            int64_t pk;
            size_t index;
        };
        struct HotCompare {
            bool asc;
            bool operator()(
                const HotCandidate& left,
                const HotCandidate& right) const {
                if (left.distance != right.distance) {
                    return asc ? left.distance < right.distance
                               : left.distance > right.distance;
                }
                return left.pk < right.pk;
            }
        };
        const std::vector<float>& query = *distance_target->query;
        bool use_avx = avx2_available();
        const exact_scan::Bound bound(hot.dim);
        std::vector<HotCandidate> candidates;
        if (hot.pks.size() >= 50000) {
            // Large vector sets use bounded per-worker heaps instead of an
            // all-candidate array, and split the scan across workers.
            using HotHeap = std::priority_queue<
                HotCandidate,
                std::vector<HotCandidate>,
                HotCompare>;
            auto scan_part = [&](size_t begin,
                                 size_t end,
                                 std::vector<HotCandidate>& output) {
                HotHeap heap(HotCompare{asc});
                for (size_t index = begin; index < end; ++index) {
                    if (!predicate.clauses.empty() &&
                        !hot_matches(hot, predicate, index)) {
                        continue;
                    }
                    const float* row = hot.vectors.data() + index * hot.dim;
                    float approx = l2_sq_raw(reinterpret_cast<const uint8_t*>(row),
                                             query.data(), hot.dim, use_avx);
                    if (heap.size() == keep &&
                        bound.rejects(approx, heap.top().distance, asc)) continue;
                    HotCandidate candidate{
                        l2_distance_ptr(row, query.data(), hot.dim), hot.pks[index], index};
                    if (heap.size() < keep) {
                        heap.push(candidate);
                    } else if (HotCompare{asc}(candidate, heap.top())) {
                        heap.pop();
                        heap.push(candidate);
                    }
                }
                output.reserve(heap.size());
                while (!heap.empty()) {
                    output.push_back(heap.top());
                    heap.pop();
                }
            };
            size_t requested_threads = threads;
            if (requested_threads == 0) {
                unsigned available = std::thread::hardware_concurrency();
                requested_threads = available == 0
                                        ? 1
                                        : std::min<size_t>(4, available);
            }
            size_t workers = std::min(requested_threads, hot.pks.size());
            if (workers <= 1) {
                scan_part(0, hot.pks.size(), candidates);
            } else {
                std::vector<std::vector<HotCandidate>> parts(workers);
                std::vector<std::thread> pool;
                bool creation_failed = false;
                size_t total = hot.pks.size();
                size_t per_worker = (total + workers - 1) / workers;
                try {
                    for (size_t worker = 0; worker < workers; ++worker) {
                        size_t begin = worker * per_worker;
                        if (begin >= total) {
                            break;
                        }
                        size_t end = std::min(total, begin + per_worker);
                        pool.emplace_back([&, worker, begin, end] {
                            scan_part(begin, end, parts[worker]);
                        });
                    }
                } catch (...) {
                    creation_failed = true;
                }
                for (std::thread& thread : pool) {
                    thread.join();
                }
                if (creation_failed) {
                    candidates.clear();
                    scan_part(0, hot.pks.size(), candidates);
                } else {
                    for (const auto& part : parts) {
                        candidates.insert(
                            candidates.end(), part.begin(), part.end());
                    }
                }
            }
        } else {
            candidates.reserve(std::min(keep, hot.pks.size()));
            const HotCompare compare{asc};
            for (size_t index = 0; index < hot.pks.size(); ++index) {
                if (!predicate.clauses.empty() &&
                    !hot_matches(hot, predicate, index)) {
                    continue;
                }
                const float* row = hot.vectors.data() + index * hot.dim;
                float approx = l2_sq_raw(reinterpret_cast<const uint8_t*>(row),
                                         query.data(), hot.dim, use_avx);
                if (candidates.size() == keep &&
                    bound.rejects(approx, candidates.front().distance, asc)) continue;
                HotCandidate candidate{
                    l2_distance_ptr(row, query.data(), hot.dim), hot.pks[index], index};
                if (candidates.size() < keep) {
                    candidates.push_back(candidate);
                    std::push_heap(candidates.begin(), candidates.end(), compare);
                } else if (compare(candidate, candidates.front())) {
                    std::pop_heap(candidates.begin(), candidates.end(), compare);
                    candidates.back() = candidate;
                    std::push_heap(candidates.begin(), candidates.end(), compare);
                }
            }
        }
        std::sort(candidates.begin(), candidates.end(), HotCompare{asc});
        struct HotFinal {
            double distance;
            int64_t pk;
            size_t index;
        };
        std::vector<HotFinal> finals;
        finals.reserve(candidates.size());
        for (const HotCandidate& candidate : candidates) {
            finals.push_back(HotFinal{
                candidate.distance,
                candidate.pk,
                candidate.index});
        }
        std::sort(
            finals.begin(),
            finals.end(),
            [asc](const HotFinal& left, const HotFinal& right) {
                if (left.distance != right.distance) {
                    return asc ? left.distance < right.distance
                               : left.distance > right.distance;
                }
                return left.pk < right.pk;
            });
        if (finals.size() > limit) {
            finals.resize(limit);
        }
        QueryResult result;
        result.columns = columns;
        result.rows.reserve(finals.size());
        bool direct_projection = true;
        for (const Projection& projection : projections) {
            if (projection.kind == Projection::Kind::Column) {
                if (!hot.int_columns[projection.column].has_value()) {
                    direct_projection = false;
                    break;
                }
            }
        }
        if (direct_projection) {
            for (const HotFinal& item : finals) {
                std::vector<OutputValue> values;
                values.reserve(projections.size());
                for (const Projection& projection : projections) {
                    if (projection.kind == Projection::Kind::Distance) {
                        const float* row =
                            hot.vectors.data() + item.index * hot.dim;
                        values.push_back(OutputValue::floating(
                            projection.query == *distance_target->query
                                ? item.distance
                                : l2_distance_ptr(
                                      row, projection.query.data(), hot.dim)));
                    } else {
                        values.push_back(OutputValue::integer(
                            (*hot.int_columns[projection.column])[item.index]));
                    }
                }
                append_result_row(result, std::move(values), result_limit);
            }
            return Response::result(std::move(result));
        }
        for (const HotFinal& item : finals) {
            std::optional<Row> row = table.get_effective_row(overlay, item.pk);
            if (row.has_value()) {
                append_result_row(
                    result,
                    project_row(row.value(), projections),
                    result_limit);
            }
        }
        return Response::result(std::move(result));
    }

    if (statement.limit.has_value() && limit <= 1000000 &&
        distance_target.has_value() &&
        (overlay == nullptr || overlay->empty()) &&
        !table.cache_covers_all() &&
        predicate_hot_compatible(predicate, schema) &&
        schema.columns[distance_target->column].type.kind == TypeKind::Vector &&
        distance_target->query->size() ==
            schema.columns[distance_target->column].type.n) {
        // Bounded-memory exact TopK for tables larger than the decoded-row
        // cache: scan record payloads and touch only the vector column and
        // BIGINT filter columns.  No Row/Value allocations and no CRC pass.
        const size_t vector_column = distance_target->column;
        const uint32_t dim = schema.columns[vector_column].type.n;
        const float* query = distance_target->query->data();
        const bool use_avx = avx2_available();
        const size_t keep = limit;
        const exact_scan::Bound bound(dim);
        struct StreamCandidate {
            double exact;
            int64_t pk;
        };
        struct StreamCompare {
            bool asc;
            bool operator()(
                const StreamCandidate& left,
                const StreamCandidate& right) const {
                if (left.exact != right.exact) {
                    return asc ? left.exact < right.exact
                               : left.exact > right.exact;
                }
                return left.pk < right.pk;
            }
        };
        using StreamHeap = std::priority_queue<
            StreamCandidate,
            std::vector<StreamCandidate>,
            StreamCompare>;
        auto process_payload = [&](uint64_t offset,
                                   const uint8_t* payload,
                                   size_t size,
                                   StreamHeap& heap,
                                   std::vector<int64_t>& int_values) {
            RawPayloadReader reader(payload, size);
            uint32_t op_count = reader.u32();
            for (uint32_t op_index = 0; op_index < op_count; ++op_index) {
                uint8_t kind = reader.u8();
                int64_t pk = reader.i64();
                if (kind == static_cast<uint8_t>(OpKind::Delete)) {
                    continue;
                }
                if (kind != static_cast<uint8_t>(OpKind::Insert) &&
                    kind != static_cast<uint8_t>(OpKind::Update)) {
                    fail_internal("storage corrupted: unknown raw op kind");
                }
                const uint8_t* raw_vector = nullptr;
                for (size_t column = 0; column < schema.columns.size(); ++column) {
                    const ColumnType& type = schema.columns[column].type;
                    switch (type.kind) {
                        case TypeKind::BigInt:
                            int_values[column] = reader.i64();
                            break;
                        case TypeKind::Varchar: {
                            uint32_t length = reader.u32();
                            reader.take(length);
                            break;
                        }
                        case TypeKind::Vector: {
                            const size_t bytes =
                                static_cast<size_t>(type.n) * sizeof(float);
                            const uint8_t* raw = reader.take(bytes);
                            if (column == vector_column) {
                                raw_vector = raw;
                            }
                            break;
                        }
                    }
                }
                if (table.has_stale) {
                    auto found = table.entries.find(pk);
                    if (found == table.entries.end() ||
                        found->second.off != offset) {
                        continue;
                    }
                }
                bool matches = true;
                for (const Predicate::Clause& clause : predicate.clauses) {
                    if (!compare_int(
                            int_values[clause.column],
                            clause.value.i,
                            clause.op)) {
                        matches = false;
                        break;
                    }
                }
                if (!matches) {
                    continue;
                }
                if (raw_vector == nullptr) {
                    fail_internal("storage corrupted: missing raw vector");
                }
                const float approx = l2_sq_raw(raw_vector, query, dim, use_avx);
                if (heap.size() == keep &&
                    bound.rejects(approx, heap.top().exact, asc)) continue;
                StreamCandidate candidate{l2_distance_raw(raw_vector, query, dim), pk};
                if (heap.size() < keep) {
                    heap.push(candidate);
                } else if (StreamCompare{asc}(candidate, heap.top())) {
                    heap.pop();
                    heap.push(candidate);
                }
            }
        };
        auto scan_range = [&](uint64_t start, uint64_t end, StreamHeap& heap) {
            std::vector<int64_t> int_values(schema.columns.size(), 0);
            scan_segment_raw_range(
                table.path,
                start,
                end,
                [&](uint64_t offset,
                    uint32_t,
                    const uint8_t* payload,
                    size_t size) {
                    process_payload(offset, payload, size, heap, int_values);
                });
        };
        size_t requested_threads = threads;
        if (requested_threads == 0) {
            unsigned available = std::thread::hardware_concurrency();
            requested_threads = available == 0
                                    ? 1
                                    : std::min<size_t>(4, available);
        }
        size_t workers = 1;
        if (requested_threads > 1 && table.entries.size() >= 150000 &&
            table.records.size() >= 2) {
            workers = std::min(requested_threads, table.records.size());
        }
        std::vector<StreamHeap> heaps;
        heaps.reserve(workers);
        for (size_t index = 0; index < workers; ++index) {
            heaps.emplace_back(StreamCompare{asc});
        }
        if (workers == 1) {
            scan_range(0, std::numeric_limits<uint64_t>::max(), heaps[0]);
        } else {
            std::vector<std::thread> pool;
            std::vector<std::exception_ptr> errors(workers);
            bool creation_failed = false;
            size_t total = table.records.size();
            size_t per_worker = (total + workers - 1) / workers;
            try {
                for (size_t worker = 0; worker < workers; ++worker) {
                    size_t begin = worker * per_worker;
                    if (begin >= total) {
                        break;
                    }
                    size_t finish = std::min(total, begin + per_worker);
                    uint64_t start = table.records[begin].first;
                    const auto& last = table.records[finish - 1];
                    uint64_t end = last.first + last.second;
                    pool.emplace_back([&, worker, start, end] {
                        try {
                            scan_range(start, end, heaps[worker]);
                        } catch (...) {
                            errors[worker] = std::current_exception();
                        }
                    });
                }
            } catch (...) {
                creation_failed = true;
            }
            for (std::thread& thread : pool) {
                thread.join();
            }
            if (creation_failed) {
                for (StreamHeap& heap : heaps) {
                    while (!heap.empty()) {
                        heap.pop();
                    }
                }
                heaps.clear();
                heaps.emplace_back(StreamCompare{asc});
                scan_range(
                    0,
                    std::numeric_limits<uint64_t>::max(),
                    heaps[0]);
            } else {
                for (const std::exception_ptr& error : errors) {
                    if (error) {
                        std::rethrow_exception(error);
                    }
                }
            }
        }
        std::vector<StreamCandidate> candidates;
        for (StreamHeap& heap : heaps) {
            while (!heap.empty()) {
                candidates.push_back(heap.top());
                heap.pop();
            }
        }
        std::sort(
            candidates.begin(),
            candidates.end(),
            [asc](const StreamCandidate& left, const StreamCandidate& right) {
                if (left.exact != right.exact) {
                    return asc ? left.exact < right.exact
                               : left.exact > right.exact;
                }
                return left.pk < right.pk;
            });
        if (candidates.size() > limit) {
            candidates.resize(limit);
        }
        QueryResult result;
        result.columns = columns;
        result.rows.reserve(candidates.size());
        bool direct_projection = true;
        for (const Projection& projection : projections) {
            if (projection.kind == Projection::Kind::Distance) {
                if (projection.column != vector_column ||
                    projection.query != *distance_target->query) {
                    direct_projection = false;
                    break;
                }
            } else if (projection.column != schema.pk) {
                direct_projection = false;
                break;
            }
        }
        if (direct_projection) {
            for (const StreamCandidate& candidate : candidates) {
                std::vector<OutputValue> values;
                values.reserve(projections.size());
                for (const Projection& projection : projections) {
                    if (projection.kind == Projection::Kind::Distance) {
                        values.push_back(OutputValue::floating(candidate.exact));
                    } else {
                        values.push_back(OutputValue::integer(candidate.pk));
                    }
                }
                append_result_row(result, std::move(values), result_limit);
            }
            return Response::result(std::move(result));
        }
        for (const StreamCandidate& candidate : candidates) {
            std::optional<Row> row =
                table.get_effective_row(nullptr, candidate.pk);
            if (row.has_value()) {
                append_result_row(
                    result,
                    project_row(row.value(), projections),
                    result_limit);
            }
        }
        return Response::result(std::move(result));
    }

    if (statement.limit.has_value() && limit <= 1000000 &&
        distance_target.has_value()) {
        // Transaction/row fallback also retains exact scores, with safe screening.
        size_t keep = limit;
        const exact_scan::Bound bound(distance_target->query->size());
        struct DistanceCandidate {
            double distance;
            int64_t pk;
        };
        struct DistanceCompare {
            bool asc;
            bool operator()(
                const DistanceCandidate& left,
                const DistanceCandidate& right) const {
                if (left.distance != right.distance) {
                    return asc ? left.distance < right.distance
                               : left.distance > right.distance;
                }
                return left.pk < right.pk;
            }
        };
        std::priority_queue<
            DistanceCandidate,
            std::vector<DistanceCandidate>,
            DistanceCompare>
            heap(DistanceCompare{asc});
        visit_rows([&](int64_t pk, const Row& row) {
            if (!predicate.matches(row)) {
                return true;
            }
            const Value& value = row.values[distance_target->column];
            if (value.kind != Value::Kind::Vec) {
                throw DbError(1105, "HY000", "distance column is not a vector");
            }
            const float approx = l2_sq(value.v, *distance_target->query);
            if (heap.size() == keep &&
                bound.rejects(approx, heap.top().distance, asc)) return true;
            DistanceCandidate candidate{l2_distance(value.v, *distance_target->query), pk};
            if (heap.size() < keep) {
                heap.push(candidate);
            } else if (DistanceCompare{asc}(candidate, heap.top())) {
                heap.pop();
                heap.push(candidate);
            }
            return true;
        });
        struct FinalCandidate {
            double distance;
            int64_t pk;
            Row row;
        };
        std::vector<FinalCandidate> finals;
        finals.reserve(heap.size());
        while (!heap.empty()) {
            DistanceCandidate candidate = heap.top();
            heap.pop();
            std::optional<Row> row = table.get_effective_row(overlay, candidate.pk);
            if (!row.has_value()) {
                continue;
            }
            const Value& value = row->values[distance_target->column];
            if (value.kind != Value::Kind::Vec) {
                throw DbError(1105, "HY000", "distance column is not a vector");
            }
            finals.push_back(FinalCandidate{
                l2_distance(value.v, *distance_target->query),
                candidate.pk,
                std::move(row.value())});
        }
        std::sort(
            finals.begin(),
            finals.end(),
            [asc](const FinalCandidate& left, const FinalCandidate& right) {
                if (left.distance != right.distance) {
                    return asc ? left.distance < right.distance
                               : left.distance > right.distance;
                }
                return left.pk < right.pk;
            });
        if (finals.size() > limit) {
            finals.resize(limit);
        }
        QueryResult result;
        result.columns = columns;
        result.rows.reserve(finals.size());
        for (const FinalCandidate& item : finals) {
            append_result_row(
                result,
                project_row(item.row, projections),
                result_limit);
        }
        return Response::result(std::move(result));
    }

    if (statement.limit.has_value() && limit <= 1000000) {
        // Bounded Top-K: compute only the order key per row and project the
        // final K rows, instead of materializing every matching row.
        size_t keep = std::min<size_t>(limit + 64, 1000000);
        struct Candidate {
            KeyVal key;
            int64_t pk;
        };
        struct Compare {
            bool asc;
            bool operator()(const Candidate& left, const Candidate& right) const {
                return compare_keys_pk(
                           left.key, left.pk, right.key, right.pk, asc) < 0;
            }
        };
        std::priority_queue<Candidate, std::vector<Candidate>, Compare> heap(
            Compare{asc});
        visit_rows([&](int64_t pk, const Row& row) {
            if (!predicate.matches(row)) {
                return true;
            }
            Candidate candidate{compute_order_key(row, order.value(), projections), pk};
            if (heap.size() < keep) {
                heap.push(candidate);
            } else if (
                compare_keys_pk(
                    candidate.key,
                    candidate.pk,
                    heap.top().key,
                    heap.top().pk,
                    asc) < 0) {
                heap.pop();
                heap.push(candidate);
            }
            return true;
        });
        std::vector<Candidate> candidates;
        candidates.reserve(heap.size());
        while (!heap.empty()) {
            candidates.push_back(heap.top());
            heap.pop();
        }
        std::sort(
            candidates.begin(),
            candidates.end(),
            [asc](const Candidate& left, const Candidate& right) {
                return compare_keys_pk(
                           left.key, left.pk, right.key, right.pk, asc) < 0;
            });
        if (candidates.size() > limit) {
            candidates.resize(limit);
        }
        QueryResult result;
        result.columns = columns;
        result.rows.reserve(candidates.size());
        for (const Candidate& candidate : candidates) {
            if (pk_only_projection) {
                append_result_row(result, project_pk(candidate.pk), result_limit);
                continue;
            }
            std::optional<Row> row = table.get_effective_row(overlay, candidate.pk);
            if (row.has_value()) {
                append_result_row(
                    result,
                    project_row(row.value(), projections),
                    result_limit);
            }
        }
        return Response::result(std::move(result));
    }

    struct Collected {
        KeyVal key;
        int64_t pk;
    };
    struct SortRuns {
        std::vector<std::string> paths;
        ~SortRuns() {
            for (const std::string& path : paths) {
                std::remove(path.c_str());
            }
        }
    } runs;
    constexpr size_t kExternalSortRows = 100000;
    std::vector<Collected> collected;
    auto compare_collected = [asc](const Collected& left, const Collected& right) {
        return compare_keys_pk(
                   left.key, left.pk, right.key, right.pk, asc) < 0;
    };
    auto flush_run = [&]() {
        if (collected.empty()) {
            return;
        }
        std::sort(collected.begin(), collected.end(), compare_collected);
        std::string path = make_sort_path(dir);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::remove(path.c_str());
            fail_internal("create external sort run");
        }
        for (const Collected& item : collected) {
            write_sort_record(output, item.key, item.pk);
        }
        if (!output) {
            std::remove(path.c_str());
            fail_internal("write external sort run");
        }
        output.close();
        runs.paths.push_back(path);
        collected.clear();
    };
    visit_rows([&](int64_t pk, const Row& row) {
        if (!predicate.matches(row)) {
            return true;
        }
        collected.push_back(Collected{
            compute_order_key(row, order.value(), projections), pk});
        if (collected.size() >= kExternalSortRows) {
            flush_run();
        }
        return true;
    });

    QueryResult result;
    result.columns = std::move(columns);
    auto emit = [&](const Collected& item) {
        if (pk_only_projection) {
            append_result_row(result, project_pk(item.pk), result_limit);
            return;
        }
        std::optional<Row> row = table.get_effective_row(overlay, item.pk);
        if (row.has_value()) {
            append_result_row(
                result,
                project_row(row.value(), projections),
                result_limit);
        }
    };

    if (runs.paths.empty()) {
        std::sort(collected.begin(), collected.end(), compare_collected);
        if (collected.size() > limit) {
            collected.resize(limit);
        }
        result.rows.reserve(collected.size());
        for (const Collected& item : collected) {
            emit(item);
        }
        return Response::result(std::move(result));
    }

    flush_run();
    struct RunItem {
        KeyVal key;
        int64_t pk;
        size_t run;
    };
    struct RunCompare {
        bool asc;
        bool operator()(const RunItem& left, const RunItem& right) const {
            return compare_keys_pk(
                       left.key, left.pk, right.key, right.pk, asc) > 0;
        }
    };
    std::vector<std::ifstream> inputs;
    inputs.reserve(runs.paths.size());
    std::priority_queue<RunItem, std::vector<RunItem>, RunCompare> heap(
        RunCompare{asc});
    for (size_t index = 0; index < runs.paths.size(); ++index) {
        inputs.emplace_back(runs.paths[index], std::ios::binary);
        if (!inputs.back()) {
            fail_internal("open external sort run");
        }
        RunItem item;
        if (read_sort_record(inputs.back(), item.key, item.pk)) {
            item.run = index;
            heap.push(std::move(item));
        }
    }
    while (!heap.empty()) {
        RunItem item = heap.top();
        heap.pop();
        emit(Collected{std::move(item.key), item.pk});
        RunItem next;
        if (read_sort_record(inputs[item.run], next.key, next.pk)) {
            next.run = item.run;
            heap.push(std::move(next));
        }
    }
    return Response::result(std::move(result));
}

Response Database::commit(std::optional<Txn>& txn) {
    if (!txn.has_value()) {
        return Response::ok();
    }
    Txn transaction = std::move(txn.value());
    txn.reset();
    bool published = false;
    try {
        std::vector<std::pair<Table*, const TxnOverlay*>> pending;
        for (auto& [key, overlay] : transaction.tables) {
            if (overlay.empty()) {
                continue;
            }
            auto found = tables.find(key);
            if (found == tables.end()) {
                throw DbError(
                    1146,
                    "42S02",
                    "Table 'minivdb." + key + "' doesn't exist");
            }
            pending.emplace_back(&found->second, &overlay);
        }
        if (pending.empty()) {
            return Response::ok();
        }
        if (pending.size() == 1) {
            auto [table, overlay] = pending.front();
            if (!table->begin_statement_batch(true)) {
                fail_internal("cannot start single-table commit");
            }
            try {
                for_each_transaction_chunk(*overlay, [&](std::vector<Op> ops) {
                    table->append_statement_chunk(std::move(ops));
                });
                table->finish_statement_batch(false);
            } catch (...) {
                table->abort_statement_batch();
                throw;
            }
            return Response::ok();
        }

        // Publish the complete redo file before touching any table. The
        // temporary file and table overlays are disposable before this point.
        const std::string path = transaction_batches_path(dir);
        const std::string temporary = path + ".tmp";
        crash_point("cross_txn_before_log");
        int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fail_io("create transaction batches", errno);
        }
        try {
            for (const auto& [table, overlay] : pending) {
                const std::string key = lower_ascii(table->schema.table);
                for_each_transaction_chunk(*overlay, [&](const std::vector<Op>& ops) {
                    const auto record = encode_record(table->schema, ops);
                    std::vector<uint8_t> header;
                    append_u32(header, static_cast<uint32_t>(key.size()));
                    append_u32(header, static_cast<uint32_t>(record.size()));
                    header.insert(header.end(), key.begin(), key.end());
                    write_all_fd(fd, header.data(), header.size(), "write transaction batch header");
                    write_all_fd(fd, record.data(), record.size(), "write transaction batch");
                    crash_point("cross_txn_log_chunk");
                });
            }
            if (::fsync(fd) != 0) {
                fail_io("sync transaction batches", errno);
            }
        } catch (...) {
            ::close(fd);
            throw;
        }
        if (::close(fd) != 0) {
            fail_io("close transaction batches", errno);
        }
        if (::rename(temporary.c_str(), path.c_str()) != 0) {
            fail_io("publish transaction batches", errno);
        }
        published = true;
        fsync_dir(dir);
        crash_point("cross_txn_after_log");
        transaction.tables.clear();
        apply_transaction_batches(false);
    } catch (...) {
        if (published) {
            // Recovery must finish the durable commit before serving reads.
            std::fputs("minivector: restart required to finish durable commit\n", stderr);
            std::_Exit(1);
        }
        txn = std::move(transaction);
        throw;
    }
    return Response::ok();
}

void Database::apply_transaction_batches(bool recovering) {
    const std::string path = transaction_batches_path(dir);
    if (!std::filesystem::exists(path)) {
        return;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail_internal("cannot open transaction batches");
    }
    Table* previous = nullptr;
    auto finish_table = [&] {
        if (!previous->fsync && ::fdatasync(previous->seg.fd) != 0) {
            fail_io("sync replayed transaction batches", errno);
        }
        crash_point(recovering ? "cross_txn_recovery_after_table" : "cross_txn_after_table");
    };
    for (;;) {
        uint8_t header[8];
        input.read(reinterpret_cast<char*>(header), sizeof(header));
        if (input.eof() && input.gcount() == 0) {
            break;
        }
        if (!input) {
            fail_internal("truncated transaction batch header");
        }
        const uint32_t key_size = load_u32(header);
        const uint32_t record_size = load_u32(header + 4);
        if (key_size == 0 || key_size > 4096 ||
            record_size < kRawRecordHeaderSize ||
            record_size > kTxnLogMaxPayload + kRawRecordHeaderSize) {
            fail_internal("invalid transaction batch length");
        }
        std::string key(key_size, '\0');
        input.read(key.data(), key.size());
        auto found = tables.find(key);
        if (!input || found == tables.end()) {
            fail_internal("transaction batch references missing table");
        }
        Table* table = &found->second;
        if (previous && table != previous) {
            finish_table();
        }
        previous = table;
        std::vector<uint8_t> record(record_size);
        input.read(reinterpret_cast<char*>(record.data()), record.size());
        if (!input) {
            fail_internal("truncated transaction batch record");
        }
        // These operations contain final row images or tombstones. Replaying
        // an already applied batch replaces the same logical rows.
        table->commit_ops(decode_record(table->schema, record.data(), record.size()), false);
    }
    if (!previous) {
        fail_internal("empty transaction batch log");
    }
    finish_table();
    if (!recovering) {
        crash_point("cross_txn_after_tables");
    }
    std::filesystem::remove(path);
    fsync_dir(dir);
    crash_point(recovering ? "cross_txn_recovery_after_applied" : "cross_txn_after_applied");
}

void Database::recover_cross_transactions() {
    TxnLogData log = read_txn_log(dir);
    next_txn_id = std::max(next_txn_id, log.max_id + 1);
    for (const auto& [id, transaction] : log.commits) {
        if (log.applied.find(id) != log.applied.end()) {
            continue;
        }
        for (const auto& [key, record] : transaction) {
            auto found = tables.find(key);
            if (found == tables.end()) {
                fail_internal(
                    "transaction log references missing table '" + key + "'");
            }
            std::vector<Op> ops = decode_record(
                found->second.schema, record.data(), record.size());
            if (!ops.empty()) {
                found->second.commit_ops(ops);
            }
            crash_point("cross_txn_recovery_after_table");
        }
        std::vector<uint8_t> applied;
        append_u64(applied, id);
        append_txn_log_record(dir, kTxnLogApplied, applied);
        crash_point("cross_txn_recovery_after_applied");
    }
    remove_txn_log(dir);
    apply_transaction_batches(true);
}

void Database::apply_ops(
    const std::string& key,
    std::optional<Txn>& txn,
    std::vector<Op> ops,
    Table* batch_table) {
    if (ops.empty()) {
        return;
    }
    if (batch_table != nullptr) {
        batch_table->append_statement_chunk(std::move(ops));
        return;
    }
    if (!txn.has_value() && !autocommit) {
        txn = Txn{};
    }
    if (txn.has_value()) {
        auto& overlay = txn.value().tables[key];
        overlay.configure(dir, key);
        for (Op& op : ops) {
            overlay.set(op.pk, std::move(op.row));
        }
        return;
    }
    auto found = tables.find(key);
    if (found == tables.end()) {
        throw DbError(1146, "42S02", "Table 'minivdb." + key + "' doesn't exist");
    }
    found->second.commit_ops(ops);
}
