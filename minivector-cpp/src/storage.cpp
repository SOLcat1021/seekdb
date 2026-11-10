#include "storage.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr uint32_t kRecordMagic = 0x4D564442;  // "MVDB"
constexpr size_t kHeaderLength = 13;           // magic(4) + flags(1) + len(4) + crc(4)
constexpr uint64_t kMaxRecordSize = 512ull * 1024 * 1024;
constexpr uint32_t kCatalogMagic = 0x4D564354;  // "MVCT"
constexpr uint32_t kCatalogVersion = 2;

const std::array<std::array<uint32_t, 256>, 8>& crc_tables() {
    static const std::array<std::array<uint32_t, 256>, 8> tables = [] {
        std::array<std::array<uint32_t, 256>, 8> result{};
        for (uint32_t index = 0; index < 256; ++index) {
            uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            }
            result[0][index] = value;
        }
        for (size_t table = 1; table < result.size(); ++table) {
            for (uint32_t index = 0; index < 256; ++index) {
                uint32_t value = result[table - 1][index];
                result[table][index] =
                    (value >> 8) ^ result[0][value & 0xFFu];
            }
        }
        return result;
    }();
    return tables;
}

uint32_t load_u32(const uint8_t* data) {
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

uint32_t crc32(const uint8_t* data, size_t size) {
    const auto& tables = crc_tables();
    uint32_t value = 0xFFFFFFFFu;
    while (size >= 8) {
        uint32_t first = load_u32(data) ^ value;
        uint32_t second = load_u32(data + 4);
        value = tables[7][first & 0xFFu] ^
                tables[6][(first >> 8) & 0xFFu] ^
                tables[5][(first >> 16) & 0xFFu] ^
                tables[4][(first >> 24) & 0xFFu] ^
                tables[3][second & 0xFFu] ^
                tables[2][(second >> 8) & 0xFFu] ^
                tables[1][(second >> 16) & 0xFFu] ^
                tables[0][(second >> 24) & 0xFFu];
        data += 8;
        size -= 8;
    }
    while (size > 0) {
        value = tables[0][(value ^ *data++) & 0xFFu] ^ (value >> 8);
        --size;
    }
    return ~value;
}

uint32_t crc32(const std::vector<uint8_t>& data) {
    return crc32(data.data(), data.size());
}

void write_all(int fd, const uint8_t* data, size_t size, const std::string& context) {
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

std::vector<uint8_t> read_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return {};
        }
        fail_io("open " + path, errno);
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        int error = errno;
        ::close(fd);
        fail_io("stat " + path, error);
    }
    std::vector<uint8_t> data(static_cast<size_t>(info.st_size));
    size_t offset = 0;
    while (offset < data.size()) {
        ssize_t result = ::pread(fd, data.data() + offset, data.size() - offset, static_cast<off_t>(offset));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            int error = errno;
            ::close(fd);
            fail_io("read " + path, error);
        }
        if (result == 0) {
            break;
        }
        offset += static_cast<size_t>(result);
    }
    ::close(fd);
    data.resize(offset);
    return data;
}

void append_u8(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}

void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

void append_i64(std::vector<uint8_t>& out, int64_t value) {
    uint64_t bits = static_cast<uint64_t>(value);
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<uint8_t>((bits >> shift) & 0xFF));
    }
}

[[maybe_unused]] void append_f32(std::vector<uint8_t>& out, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(out, bits);
}

void store_u32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

size_t encoded_value_size(const ColumnType& type, const Value& value) {
    switch (type.kind) {
        case TypeKind::BigInt:
            return 8;
        case TypeKind::Varchar:
            return 4 + value.s.size();
        case TypeKind::Vector:
            return static_cast<size_t>(type.n) * sizeof(float);
    }
    return 0;
}

size_t encoded_op_size(const Schema& schema, const Op& op) {
    size_t size = 1 + 8;
    if (op.kind == OpKind::Delete || !op.row.has_value() ||
        op.row->values.size() != schema.columns.size()) {
        return size;
    }
    for (size_t index = 0; index < schema.columns.size(); ++index) {
        size += encoded_value_size(schema.columns[index].type, op.row->values[index]);
    }
    return size;
}

struct Reader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;

    const uint8_t* take(size_t count) {
        if (pos + count > size) {
            fail_internal("storage corrupted: truncated record payload");
        }
        const uint8_t* pointer = data + pos;
        pos += count;
        return pointer;
    }

    uint8_t u8() { return *take(1); }

    uint32_t u32() {
        const uint8_t* pointer = take(4);
        return static_cast<uint32_t>(pointer[0]) |
               (static_cast<uint32_t>(pointer[1]) << 8) |
               (static_cast<uint32_t>(pointer[2]) << 16) |
               (static_cast<uint32_t>(pointer[3]) << 24);
    }

    int64_t i64() {
        const uint8_t* pointer = take(8);
        uint64_t bits = 0;
        for (int index = 0; index < 8; ++index) {
            bits |= static_cast<uint64_t>(pointer[index]) << (index * 8);
        }
        return static_cast<int64_t>(bits);
    }

    float f32() {
        uint32_t bits = u32();
        float value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    bool done() const { return pos >= size; }
};

void encode_value(std::vector<uint8_t>& out, const ColumnType& type, const Value& value) {
    switch (type.kind) {
        case TypeKind::BigInt:
            if (value.kind != Value::Kind::Int) {
                fail_internal("storage corrupted: BIGINT value mismatch");
            }
            append_i64(out, value.i);
            return;
        case TypeKind::Varchar:
            if (value.kind != Value::Kind::Str) {
                fail_internal("storage corrupted: VARCHAR value mismatch");
            }
            if (value.s.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
                fail_internal("VARCHAR value too large to encode");
            }
            append_u32(out, static_cast<uint32_t>(value.s.size()));
            out.insert(out.end(), value.s.begin(), value.s.end());
            return;
        case TypeKind::Vector:
            if (value.kind != Value::Kind::Vec || value.v.size() != type.n) {
                fail_internal("storage corrupted: VECTOR value mismatch");
            }
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            {
                const uint8_t* bytes =
                    reinterpret_cast<const uint8_t*>(value.v.data());
                out.insert(out.end(), bytes, bytes + value.v.size() * sizeof(float));
            }
#else
            for (float element : value.v) {
                append_f32(out, element);
            }
#endif
            return;
    }
    fail_internal("storage corrupted: unknown column type");
}

Value decode_value(Reader& reader, const ColumnType& type) {
    switch (type.kind) {
        case TypeKind::BigInt:
            return Value::integer(reader.i64());
        case TypeKind::Varchar: {
            uint32_t length = reader.u32();
            if (length > 128u * 1024u * 1024u) {
                fail_internal("storage corrupted: VARCHAR length out of range");
            }
            const uint8_t* bytes = reader.take(length);
            return Value::string(std::string(reinterpret_cast<const char*>(bytes), length));
        }
        case TypeKind::Vector: {
            std::vector<float> values;
            values.reserve(type.n);
            for (uint32_t index = 0; index < type.n; ++index) {
                values.push_back(reader.f32());
            }
            return Value::vector(std::move(values));
        }
    }
    fail_internal("storage corrupted: unknown column type");
}

void encode_op(std::vector<uint8_t>& out, const Schema& schema, const Op& op) {
    append_u8(out, static_cast<uint8_t>(op.kind));
    append_i64(out, op.pk);
    if (op.kind == OpKind::Delete) {
        return;
    }
    if (!op.row.has_value() || op.row->values.size() != schema.columns.size()) {
        fail_internal("storage corrupted: row does not match schema");
    }
    for (size_t index = 0; index < schema.columns.size(); ++index) {
        encode_value(out, schema.columns[index].type, op.row->values[index]);
    }
}

std::vector<Op> decode_payload(const Schema& schema, const uint8_t* data, size_t size) {
    Reader reader{data, size};
    uint32_t count = reader.u32();
    if (count > 100000000u) {
        fail_internal("storage corrupted: op count out of range");
    }
    std::vector<Op> ops;
    ops.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        uint8_t kind = reader.u8();
        Op op;
        switch (kind) {
            case 0:
                op.kind = OpKind::Insert;
                break;
            case 1:
                op.kind = OpKind::Update;
                break;
            case 2:
                op.kind = OpKind::Delete;
                break;
            default:
                fail_internal("storage corrupted: unknown op kind");
        }
        op.pk = reader.i64();
        if (op.kind != OpKind::Delete) {
            Row row;
            row.values.reserve(schema.columns.size());
            for (const Column& column : schema.columns) {
                row.values.push_back(decode_value(reader, column.type));
            }
            op.row = std::move(row);
        }
        ops.push_back(std::move(op));
    }
    if (!reader.done()) {
        fail_internal("storage corrupted: trailing bytes in record payload");
    }
    return ops;
}

}  // namespace

Segment::Segment(Segment&& other) noexcept
    : fd(other.fd), path(std::move(other.path)), len(other.len) {
    other.fd = -1;
    other.len = 0;
}

Segment& Segment::operator=(Segment&& other) noexcept {
    if (this != &other) {
        if (fd >= 0) {
            ::close(fd);
        }
        fd = other.fd;
        path = std::move(other.path);
        len = other.len;
        other.fd = -1;
        other.len = 0;
    }
    return *this;
}

Segment::~Segment() {
    if (fd >= 0) {
        ::close(fd);
    }
}

Segment Segment::open(const std::string& file_path) {
    int descriptor = ::open(file_path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (descriptor < 0) {
        fail_io("open segment " + file_path, errno);
    }
    struct stat info {};
    if (::fstat(descriptor, &info) != 0) {
        int error = errno;
        ::close(descriptor);
        fail_io("stat segment " + file_path, error);
    }
    Segment segment;
    segment.fd = descriptor;
    segment.path = file_path;
    segment.len = static_cast<uint64_t>(info.st_size);
    return segment;
}

void Segment::reopen() {
    int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (descriptor < 0) {
        fail_io("reopen segment " + path, errno);
    }
    struct stat info {};
    if (::fstat(descriptor, &info) != 0) {
        int error = errno;
        ::close(descriptor);
        fail_io("stat segment " + path, error);
    }
    if (fd >= 0) {
        ::close(fd);
    }
    fd = descriptor;
    len = static_cast<uint64_t>(info.st_size);
}

std::pair<uint64_t, uint32_t> Segment::append(
    const Schema& schema,
    const std::vector<Op>& ops,
    bool fsync_enabled,
    uint8_t flags) {
    if (ops.empty() && flags == 0) {
        return {len, 0};
    }
    std::vector<uint8_t> record = encode_record(schema, ops);
    if (record.size() < kHeaderLength) {
        fail_internal("commit record is shorter than its header");
    }
    record[4] = flags;
    uint64_t offset = len;
    crash_point("wal_before_write");
    if (crash_point_hit("wal_partial_write")) {
        size_t half = record.size() / 2;
        write_all(fd, record.data(), half, "append record");
        std::_Exit(137);
    }
    write_all(fd, record.data(), record.size(), "append record");
    crash_point("wal_after_write_before_fsync");
    if (fsync_enabled) {
        if (::fdatasync(fd) != 0) {
            fail_io("fsync segment", errno);
        }
    }
    crash_point("wal_after_fsync");
    len += record.size();
    return {offset, static_cast<uint32_t>(record.size())};
}

std::vector<Op> Segment::read_record(
    uint64_t offset,
    uint32_t length,
    const Schema& schema) const {
    std::vector<uint8_t> buffer(length);
    size_t read = 0;
    while (read < buffer.size()) {
        ssize_t result = ::pread(
            fd, buffer.data() + read, buffer.size() - read, static_cast<off_t>(offset + read));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail_io("read record", errno);
        }
        if (result == 0) {
            fail_internal("storage corrupted: short record read");
        }
        read += static_cast<size_t>(result);
    }
    return decode_record(schema, buffer.data(), buffer.size());
}

std::vector<uint8_t> encode_record(const Schema& schema, const std::vector<Op>& ops) {
    if (ops.size() > std::numeric_limits<uint32_t>::max()) {
        fail_internal("commit record has too many operations");
    }
    size_t capacity = kHeaderLength + sizeof(uint32_t);
    for (const Op& op : ops) {
        capacity += encoded_op_size(schema, op);
    }
    std::vector<uint8_t> record;
    record.reserve(capacity);
    append_u32(record, kRecordMagic);
    append_u8(record, 0);
    append_u32(record, 0);
    append_u32(record, 0);
    append_u32(record, static_cast<uint32_t>(ops.size()));
    for (const Op& op : ops) {
        encode_op(record, schema, op);
    }
    size_t payload_length = record.size() - kHeaderLength;
    if (payload_length > kMaxRecordSize) {
        fail_internal("commit record is too large");
    }
    uint32_t checksum =
        crc32(record.data() + kHeaderLength, payload_length);
    store_u32(record.data() + 5, static_cast<uint32_t>(payload_length));
    store_u32(record.data() + 9, checksum);
    return record;
}

std::vector<Op> decode_record(const Schema& schema, const uint8_t* data, size_t size) {
    if (size < kHeaderLength) {
        fail_internal("storage corrupted: record shorter than header");
    }
    Reader reader{data, size};
    uint32_t magic = reader.u32();
    if (magic != kRecordMagic) {
        fail_internal("storage corrupted: bad record magic");
    }
    reader.u8();
    uint32_t payload_length = reader.u32();
    uint32_t stored_crc = reader.u32();
    if (kHeaderLength + payload_length != size) {
        fail_internal("storage corrupted: record length mismatch");
    }
    const uint8_t* payload = reader.take(payload_length);
    if (crc32(payload, payload_length) != stored_crc) {
        fail_internal("storage corrupted: record checksum mismatch");
    }
    return decode_payload(schema, payload, payload_length);
}

uint64_t scan_segment(
    const std::string& path,
    const Schema& schema,
    const std::function<void(uint64_t, uint32_t, uint8_t, std::vector<Op>)>& on_record) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return 0;
        }
        fail_io("open segment for scan " + path, errno);
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        int error = errno;
        ::close(fd);
        fail_io("stat segment for scan " + path, error);
    }
    uint64_t file_size = static_cast<uint64_t>(info.st_size);
    uint64_t offset = 0;
    while (offset < file_size) {
        uint8_t header[kHeaderLength];
        size_t filled = 0;
        while (filled < kHeaderLength) {
            ssize_t result = ::pread(
                fd, header + filled, kHeaderLength - filled, static_cast<off_t>(offset + filled));
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                int error = errno;
                ::close(fd);
                fail_io("scan segment", error);
            }
            if (result == 0) {
                break;
            }
            filled += static_cast<size_t>(result);
        }
        if (filled < kHeaderLength) {
            break;
        }
        Reader reader{header, kHeaderLength};
        uint32_t magic = reader.u32();
        if (magic != kRecordMagic) {
            break;
        }
        reader.u8();
        uint32_t payload_length = reader.u32();
        uint32_t stored_crc = reader.u32();
        uint8_t flags = 0;
        {
            Reader header_reader{header, kHeaderLength};
            header_reader.u32();
            flags = header_reader.u8();
        }
        if (payload_length > kMaxRecordSize) {
            break;
        }
        std::vector<uint8_t> payload(payload_length);
        size_t read = 0;
        while (read < payload.size()) {
            ssize_t result = ::pread(
                fd,
                payload.data() + read,
                payload.size() - read,
                static_cast<off_t>(offset + kHeaderLength + read));
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                int error = errno;
                ::close(fd);
                fail_io("scan segment payload", error);
            }
            if (result == 0) {
                break;
            }
            read += static_cast<size_t>(result);
        }
        if (read < payload.size()) {
            break;
        }
        if (crc32(payload) != stored_crc) {
            break;
        }
        std::vector<Op> ops = decode_payload(schema, payload.data(), payload.size());
        uint32_t record_length = static_cast<uint32_t>(kHeaderLength + payload_length);
        on_record(offset, record_length, flags, std::move(ops));
        offset += record_length;
    }
    ::close(fd);
    return offset;
}

uint64_t scan_segment_raw_range(
    const std::string& path,
    uint64_t start,
    uint64_t end,
    const std::function<void(uint64_t, uint32_t, const uint8_t*, size_t)>& on_record) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return start;
        }
        fail_io("open segment for raw scan " + path, errno);
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        int error = errno;
        ::close(fd);
        fail_io("stat segment for raw scan " + path, error);
    }
    uint64_t file_size = static_cast<uint64_t>(info.st_size);
    if (start > file_size) {
        start = file_size;
    }
    if (end > file_size) {
        end = file_size;
    }
    if (end < start) {
        end = start;
    }
    std::vector<uint8_t> window(1u * 1024u * 1024u);
    size_t window_pos = 0;
    size_t window_len = 0;
    uint64_t window_offset = start;
    auto ensure = [&](size_t need) -> bool {
        while (window_pos + need > window_len) {
            if (window_pos > 0) {
                size_t remain = window_len - window_pos;
                if (remain > 0) {
                    std::memmove(window.data(), window.data() + window_pos, remain);
                }
                window_offset += window_pos;
                window_len = remain;
                window_pos = 0;
            }
            if (need > window.size()) {
                window.resize(need);
            }
            size_t want = window.size() - window_len;
            if (want == 0) {
                return false;
            }
            ssize_t result = ::pread(
                fd,
                window.data() + window_len,
                want,
                static_cast<off_t>(window_offset + window_len));
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                int error = errno;
                ::close(fd);
                fail_io("raw scan segment", error);
            }
            if (result == 0) {
                return false;
            }
            window_len += static_cast<size_t>(result);
        }
        return true;
    };
    uint64_t offset = start;
    while (offset + kHeaderLength <= end) {
        if (!ensure(kHeaderLength)) {
            break;
        }
        const uint8_t* header = window.data() + window_pos;
        Reader reader{header, kHeaderLength};
        if (reader.u32() != kRecordMagic) {
            break;
        }
        reader.u8();
        uint32_t payload_length = reader.u32();
        if (payload_length > kMaxRecordSize ||
            offset + kHeaderLength + payload_length > end) {
            break;
        }
        size_t record_length = kHeaderLength + payload_length;
        if (!ensure(record_length)) {
            break;
        }
        on_record(
            offset,
            static_cast<uint32_t>(record_length),
            window.data() + window_pos + kHeaderLength,
            payload_length);
        window_pos += record_length;
        offset += record_length;
    }
    ::close(fd);
    return offset;
}

uint64_t scan_segment_raw(
    const std::string& path,
    const std::function<void(uint64_t, uint32_t, const uint8_t*, size_t)>& on_record) {
    return scan_segment_raw_range(
        path, 0, std::numeric_limits<uint64_t>::max(), on_record);
}

void truncate_segment(const std::string& path, uint64_t valid_len) {
    int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) {
        fail_io("open segment for truncate " + path, errno);
    }
    if (::ftruncate(fd, static_cast<off_t>(valid_len)) != 0) {
        int error = errno;
        ::close(fd);
        fail_io("truncate segment", error);
    }
    if (::fsync(fd) != 0) {
        int error = errno;
        ::close(fd);
        fail_io("fsync truncated segment", error);
    }
    ::close(fd);
}

std::vector<Schema> load_catalog(const std::string& path) {
    std::vector<uint8_t> data = read_file(path);
    if (data.empty()) {
        return {};
    }
    if (data.size() < 8) {
        fail_internal("storage corrupted: catalog file too short");
    }
    Reader reader{data.data(), data.size()};
    uint32_t magic = reader.u32();
    uint32_t version = reader.u32();
    if (magic != kCatalogMagic || (version != 1 && version != 2)) {
        fail_internal("storage corrupted: unsupported catalog file");
    }
    uint32_t table_count = reader.u32();
    std::vector<Schema> schemas;
    schemas.reserve(table_count);
    for (uint32_t table_index = 0; table_index < table_count; ++table_index) {
        uint32_t name_length = reader.u32();
        if (name_length > 1024u) {
            fail_internal("storage corrupted: table name too long");
        }
        const uint8_t* name_bytes = reader.take(name_length);
        Schema schema;
        schema.table.assign(reinterpret_cast<const char*>(name_bytes), name_length);
        schema.pk = reader.u32();
        if (version == 1) {
            if (reader.u8() != 0) {
                // v1 only stored a boolean; keep a synthetic legacy name so
                // the physical index remains enabled and new names can be
                // added without rebuilding it.
                schema.vector_indexes.push_back("__legacy_vector_index");
            }
        } else {
            uint32_t index_count = reader.u32();
            if (index_count > 1024u) {
                fail_internal("storage corrupted: too many vector indexes");
            }
            schema.vector_indexes.reserve(index_count);
            for (uint32_t index = 0; index < index_count; ++index) {
                uint32_t index_name_length = reader.u32();
                if (index_name_length == 0 || index_name_length > 1024u) {
                    fail_internal("storage corrupted: invalid vector index name");
                }
                const uint8_t* index_name = reader.take(index_name_length);
                schema.vector_indexes.emplace_back(
                    reinterpret_cast<const char*>(index_name),
                    index_name_length);
            }
        }
        uint32_t column_count = reader.u32();
        if (column_count > 1024u) {
            fail_internal("storage corrupted: too many columns");
        }
        schema.columns.reserve(column_count);
        for (uint32_t column_index = 0; column_index < column_count; ++column_index) {
            uint32_t column_name_length = reader.u32();
            const uint8_t* column_name_bytes = reader.take(column_name_length);
            Column column;
            column.name.assign(
                reinterpret_cast<const char*>(column_name_bytes), column_name_length);
            uint8_t tag = reader.u8();
            uint32_t n = reader.u32();
            switch (tag) {
                case 1:
                    column.type = ColumnType::bigint();
                    break;
                case 2:
                    column.type = ColumnType::varchar(n);
                    break;
                case 3:
                    column.type = ColumnType::vector(n);
                    break;
                default:
                    fail_internal("storage corrupted: unknown column type in catalog");
            }
            schema.columns.push_back(std::move(column));
        }
        if (schema.pk >= schema.columns.size()) {
            fail_internal("storage corrupted: invalid primary key in catalog");
        }
        schemas.push_back(std::move(schema));
    }
    return schemas;
}

void save_catalog(const std::string& path, const std::vector<Schema>& schemas) {
    std::vector<uint8_t> data;
    append_u32(data, kCatalogMagic);
    append_u32(data, kCatalogVersion);
    append_u32(data, static_cast<uint32_t>(schemas.size()));
    for (const Schema& schema : schemas) {
        append_u32(data, static_cast<uint32_t>(schema.table.size()));
        data.insert(data.end(), schema.table.begin(), schema.table.end());
        append_u32(data, static_cast<uint32_t>(schema.pk));
        append_u32(data, static_cast<uint32_t>(schema.vector_indexes.size()));
        for (const std::string& index_name : schema.vector_indexes) {
            append_u32(data, static_cast<uint32_t>(index_name.size()));
            data.insert(data.end(), index_name.begin(), index_name.end());
        }
        append_u32(data, static_cast<uint32_t>(schema.columns.size()));
        for (const Column& column : schema.columns) {
            append_u32(data, static_cast<uint32_t>(column.name.size()));
            data.insert(data.end(), column.name.begin(), column.name.end());
            append_u8(data, static_cast<uint8_t>(column.type.kind));
            append_u32(data, column.type.n);
        }
    }
    atomic_write(path, data);
}

void atomic_write(const std::string& path, const std::vector<uint8_t>& data) {
    std::string temporary = path + ".tmp";
    int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail_io("create temp file " + temporary, errno);
    }
    try {
        write_all(fd, data.data(), data.size(), "write temp file");
        if (::fsync(fd) != 0) {
            fail_io("fsync temp file", errno);
        }
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        fail_io("rename temp file", errno);
    }
    size_t slash = path.find_last_of('/');
    std::string directory = slash == std::string::npos ? "." : path.substr(0, slash);
    fsync_dir(directory);
}

void fsync_dir(const std::string& directory) {
    int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        fail_io("open data directory " + directory, errno);
    }
    if (::fsync(fd) != 0) {
        int error = errno;
        ::close(fd);
        fail_io("fsync data directory", error);
    }
    ::close(fd);
}
