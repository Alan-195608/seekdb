#pragma once

#include "sql.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <system_error>
#include <unordered_map>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mini {

using Offset = uint64_t;

inline void check(int status, const char* operation) {
    if (status < 0) throw std::system_error(errno, std::generic_category(), operation);
}

template<typename T>
inline T load(const char* address) {
    T value;
    std::memcpy(&value, address, sizeof(value));
    return value;
}

template<typename T>
inline void put(std::string& buffer, T value) {
    buffer.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

inline void put_string(std::string& buffer, std::string_view value) {
    if (value.size() > UINT32_MAX) invalid("string too long");
    put(buffer, static_cast<uint32_t>(value.size()));
    buffer.append(value);
}

class Reader {
    const char* data_;
    size_t remaining_;

public:
    Reader(const char* data, size_t size) : data_(data), remaining_(size) {}
    const char* take(size_t size) {
        if (size > remaining_) throw std::runtime_error("invalid committed log record");
        const auto* result = data_;
        data_ += size;
        remaining_ -= size;
        return result;
    }
    template<typename T> T get() { return load<T>(take(sizeof(T))); }
    std::string_view string() {
        const auto size = get<uint32_t>();
        return {take(size), size};
    }
    size_t remaining() const { return remaining_; }
};

struct Table {
    std::string name;
    std::vector<Column> columns;
    size_t primary = 0;
    size_t vector_column = SIZE_MAX;
    std::string index_name;
    mutable bool index_dirty = true;
    mutable std::vector<std::pair<float, Offset>> index;
    uint32_t id = 0;
    std::unordered_map<int64_t, Offset> rows;

    size_t column(const std::string& name) const {
        for (size_t i = 0; i < columns.size(); ++i) if (columns[i].name == name) return i;
        throw SqlError("UNKNOWN_COLUMN", name);
    }

    size_t vector_bytes() const {
        return vector_column == SIZE_MAX ? 0 : size_t(columns[vector_column].width) * sizeof(float);
    }
};

class Store {
    static constexpr uint64_t magic = 0x31424456494e494d;
    static constexpr size_t frame_header = 16;
    enum class Op : uint8_t { Create, Set, Delete, Index };

    struct Undo {
        Op op;
        uint32_t table;
        int64_t key = 0;
        Offset old = 0;
        bool existed = false;
    };

    int fd_ = -1;
    const char* mapping_ = nullptr;
    size_t capacity_ = 0;
    Offset disk_end_ = 0;
    Offset transaction_ = 0;
    std::string pending_;
    std::vector<Undo> undo_;
    std::vector<std::unique_ptr<Table>> tables_;
    std::unordered_map<std::string, uint32_t> names_;

    void map(size_t required) {
        if (required <= capacity_) return;
        const size_t capacity = std::max({size_t(1 << 20), required, capacity_ * 2});
        const void* address = mmap(nullptr, capacity, PROT_READ, MAP_SHARED, fd_, 0);
        if (address == MAP_FAILED) check(-1, "mmap");
        if (mapping_) munmap(const_cast<char*>(mapping_), capacity_);
        mapping_ = static_cast<const char*>(address);
        capacity_ = capacity;
    }

    void write_at(std::string_view bytes, Offset offset) {
        while (!bytes.empty()) {
            const auto n = pwrite(fd_, bytes.data(), bytes.size(), offset);
            if (n < 0 && errno == EINTR) continue;
            check(n < 0 ? -1 : 0, "pwrite");
            if (n == 0) throw std::runtime_error("empty pwrite");
            bytes.remove_prefix(n);
            offset += n;
        }
    }

    void sync() {
        int status;
        do {
#ifdef __APPLE__
            status = fsync(fd_);
#else
            status = fdatasync(fd_);
#endif
        } while (status < 0 && errno == EINTR);
        check(status, "sync log");
    }

    Offset end() const { return disk_end_ + pending_.size(); }

    Offset append(std::string_view record) {
        if (record.size() > UINT32_MAX) invalid("record too large");
        const Offset offset = end();
        put(pending_, static_cast<uint32_t>(record.size()));
        pending_.append(record);
        if (pending_.size() >= 1 << 20) flush();
        return offset;
    }

    void install(std::unique_ptr<Table> table) {
        table->id = static_cast<uint32_t>(tables_.size());
        names_.emplace(table->name, table->id);
        tables_.push_back(std::move(table));
    }

    void replay(Offset begin, Offset end) {
        while (begin < end) {
            Reader remaining(mapping_ + begin, end - begin);
            const auto size = remaining.get<uint32_t>();
            Reader record(remaining.take(size), size);
            const auto op = record.get<Op>();
            if (op == Op::Create) {
                auto table = std::make_unique<Table>();
                table->name = record.string();
                table->primary = record.get<uint32_t>();
                const auto count = record.get<uint32_t>();
                for (uint32_t i = 0; i < count; ++i) {
                    std::string name(record.string());
                    const auto kind = record.get<Kind>();
                    const auto width = record.get<uint32_t>();
                    if (kind == Kind::Vector) table->vector_column = i;
                    table->columns.push_back({std::move(name), kind, width});
                }
                if (table->primary >= table->columns.size() || names_.contains(table->name)) {
                    throw std::runtime_error("invalid committed table schema");
                }
                install(std::move(table));
            } else {
                auto& table = *tables_.at(record.get<uint32_t>());
                if (op == Op::Index) table.index_name = record.string();
                else if (op == Op::Delete) table.rows.erase(record.get<int64_t>());
                else if (op == Op::Set) table.rows[record.get<int64_t>()] = begin;
                else throw std::runtime_error("unknown committed log operation");
            }
            begin += sizeof(size) + size;
        }
    }

    void recover(Offset size, const std::filesystem::path& directory) {
        if (!size) {
            std::string header;
            put(header, magic);
            write_at(header, 0);
            sync();
            const int dir = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            check(dir, "open data directory");
            int status;
            do { status = fsync(dir); } while (status < 0 && errno == EINTR);
            const int saved_errno = errno;
            close(dir);
            errno = saved_errno;
            check(status, "sync data directory");
            size = sizeof(magic);
        }
        map(size);
        if (size < sizeof(magic) || load<uint64_t>(mapping_) != magic) {
            throw std::runtime_error("unknown log format");
        }
        disk_end_ = sizeof(magic);
        while (size - disk_end_ >= frame_header) {
            const auto length = load<uint64_t>(mapping_ + disk_end_);
            const auto inverse = load<uint64_t>(mapping_ + disk_end_ + sizeof(length));
            if (!length || inverse != ~length || length > size - disk_end_ - frame_header) break;
            const auto start = disk_end_ + frame_header;
            replay(start, start + length);
            disk_end_ = start + length;
        }
        if (disk_end_ != size) check(ftruncate(fd_, disk_end_), "truncate uncommitted tail");
    }

public:
    struct Savepoint { Offset end; size_t undo_count; };

    explicit Store(const std::filesystem::path& directory) {
        std::filesystem::create_directories(directory);
        fd_ = open((directory / "mini.wal").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        check(fd_, "open log");
        try {
            check(flock(fd_, LOCK_EX | LOCK_NB), "lock log");
            struct stat info {};
            check(fstat(fd_, &info), "stat log");
            recover(info.st_size, directory);
            for (const auto& table : tables_) {
                if (!table->index_name.empty()) rebuild_index(*table);
            }
        } catch (...) {
            if (mapping_) munmap(const_cast<char*>(mapping_), capacity_);
            close(fd_);
            throw;
        }
    }

    ~Store() {
        if (mapping_) munmap(const_cast<char*>(mapping_), capacity_);
        close(fd_);
    }
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    Table& table(const std::string& name) {
        const auto it = names_.find(name);
        if (it == names_.end()) throw SqlError("UNKNOWN_TABLE", name);
        return *tables_[it->second];
    }

    const Table& table(const std::string& name) const {
        return const_cast<Store*>(this)->table(name);
    }

    void begin() {
        transaction_ = disk_end_;
    }

    void flush() {
        if (pending_.empty()) return;
        write_at(pending_, disk_end_);
        disk_end_ = end();
        pending_.clear();
        map(disk_end_);
    }

    Savepoint savepoint() const { return {end(), undo_.size()}; }

    void restore(Savepoint point) {
        while (undo_.size() > point.undo_count) {
            const auto entry = undo_.back();
            undo_.pop_back();
            auto& table = *tables_[entry.table];
            if (entry.op == Op::Create) {
                names_.erase(table.name);
                tables_.pop_back();
            } else if (entry.op == Op::Index) {
                table.index_name.clear();
                table.index.clear();
                table.index_dirty = true;
            } else if (entry.existed) {
                table.rows[entry.key] = entry.old;
                table.index_dirty = true;
            } else {
                table.rows.erase(entry.key);
                table.index_dirty = true;
            }
        }
        if (point.end >= disk_end_) {
            pending_.resize(point.end - disk_end_);
        } else {
            check(ftruncate(fd_, point.end), "rollback log");
            disk_end_ = point.end;
            pending_.clear();
        }
    }

    void rollback() {
        restore({transaction_, 0});
        if (undo_.capacity() > 65536) std::vector<Undo>().swap(undo_);
    }

    void commit() {
        if (undo_.empty()) return;
        flush();
        const uint64_t size = disk_end_ - transaction_ - frame_header;
        std::string header;
        put(header, size);
        put(header, ~size);
        write_at(header, transaction_);
        sync();
        for (const auto& entry : undo_) {
            if (entry.op != Op::Delete) continue;
            auto& rows = tables_[entry.table]->rows;
            const auto it = rows.find(entry.key);
            if (it != rows.end() && !it->second) rows.erase(it);
        }
        undo_.clear();
        if (undo_.capacity() > 65536) std::vector<Undo>().swap(undo_);
    }

    void create(std::unique_ptr<Table> table) {
        if (names_.contains(table->name)) throw SqlError("DUPLICATE_TABLE", table->name);
        std::string record;
        put(record, Op::Create);
        put_string(record, table->name);
        put(record, static_cast<uint32_t>(table->primary));
        put(record, static_cast<uint32_t>(table->columns.size()));
        for (const auto& column : table->columns) {
            put_string(record, column.name);
            put(record, column.kind);
            put(record, column.width);
        }
        ensure_frame();
        append(record);
        undo_.push_back({Op::Create, static_cast<uint32_t>(tables_.size())});
        install(std::move(table));
    }

    void create_index(Table& table, std::string name) {
        for (const auto& existing : tables_) {
            if (existing->index_name == name) throw SqlError("DUPLICATE_INDEX", name);
        }
        if (!table.index_name.empty()) throw SqlError("DUPLICATE_INDEX", table.index_name);
        std::string record;
        put(record, Op::Index);
        put(record, table.id);
        put_string(record, name);
        ensure_frame();
        append(record);
        undo_.push_back({Op::Index, table.id});
        table.index_name = std::move(name);
        rebuild_index(table);
    }

    void set(Table& table, int64_t key, const std::vector<Value>& values, bool insert) {
        const auto it = table.rows.find(key);
        if (insert && it != table.rows.end() && it->second) throw SqlError("DUPLICATE_KEY", std::to_string(key));
        const Offset old = it == table.rows.end() ? 0 : it->second;
        std::string record;
        record.reserve(32 + table.vector_bytes());
        put(record, Op::Set);
        put(record, table.id);
        put(record, key);
        if (table.vector_column != SIZE_MAX) {
            const auto& vector = std::get<Vector>(values[table.vector_column]);
            record.append(reinterpret_cast<const char*>(vector.data()), table.vector_bytes());
        }
        for (size_t i = 0; i < values.size(); ++i) {
            if (i == table.primary || i == table.vector_column) continue;
            if (const auto* number = std::get_if<int64_t>(&values[i])) put(record, *number);
            else put_string(record, std::get<std::string>(values[i]));
        }
        ensure_frame();
        const Offset offset = append(record);
        undo_.push_back({Op::Set, table.id, key, old, it != table.rows.end()});
        table.rows[key] = offset;
        table.index_dirty = true;
    }

    void erase(Table& table, int64_t key) {
        const auto it = table.rows.find(key);
        std::string record;
        put(record, Op::Delete);
        put(record, table.id);
        put(record, key);
        ensure_frame();
        append(record);
        undo_.push_back({Op::Delete, table.id, key, it->second, true});
        // Keep a tombstone through the transaction so rollback never has to allocate.
        it->second = 0;
        table.index_dirty = true;
    }

    void rebuild_index(const Table& table) const {
        if (!table.index_dirty) return;
        table.index.clear();
        table.index.reserve(table.rows.size());
        for (const auto& [key, offset] : table.rows) {
            if (offset) table.index.emplace_back(load<float>(row(offset)), offset);
        }
        std::sort(table.index.begin(), table.index.end());
        table.index_dirty = false;
    }

    int64_t key_at(Offset offset) const {
        return load<int64_t>(row(offset) - sizeof(int64_t));
    }

    const char* row(Offset offset) const {
        return mapping_ + offset + sizeof(uint32_t) + sizeof(Op) + sizeof(uint32_t) + sizeof(int64_t);
    }

private:
    void ensure_frame() {
        if (end() == transaction_) pending_.append(frame_header, '\0');
    }
};

} // namespace mini
