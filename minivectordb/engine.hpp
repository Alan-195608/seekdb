#pragma once

#include "store.hpp"

#include <optional>
#include <ostream>
#include <queue>
#include <ranges>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#endif

namespace mini {

using Field = std::variant<int64_t, std::string_view, const char*>;
constexpr size_t distance_column = SIZE_MAX;

struct Row {
    int64_t key;
    Offset offset;
    double distance = 0;
};

inline Field field(const Table& table, const Row& row, const char* data, size_t column) {
    if (column == table.primary) return row.key;
    if (column == table.vector_column) return data;
    data += table.vector_bytes();
    for (size_t i = 0; i <= column; ++i) {
        if (i == table.primary || i == table.vector_column) continue;
        if (table.columns[i].kind == Kind::Integer) {
            if (i == column) return load<int64_t>(data);
            data += sizeof(int64_t);
        } else {
            const size_t size = load<uint32_t>(data);
            data += sizeof(uint32_t);
            if (i == column) return std::string_view(data, size);
            data += size;
        }
    }
    throw std::logic_error("invalid field");
}

inline double portable_distance(const char* data, const Vector& query) {
    // Independent double-precision lanes help the compiler vectorize the reduction.
    double a = 0, b = 0, c = 0, d = 0;
    size_t i = 0;
    for (; i + 4 <= query.size(); i += 4) {
        const double x = double(load<float>(data + (i + 0) * sizeof(float))) - query[i + 0];
        const double y = double(load<float>(data + (i + 1) * sizeof(float))) - query[i + 1];
        const double z = double(load<float>(data + (i + 2) * sizeof(float))) - query[i + 2];
        const double w = double(load<float>(data + (i + 3) * sizeof(float))) - query[i + 3];
        a += x * x;
        b += y * y;
        c += z * z;
        d += w * w;
    }
    for (; i < query.size(); ++i) {
        const double delta = double(load<float>(data + i * sizeof(float))) - query[i];
        a += delta * delta;
    }
    return (a + b) + (c + d);
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("avx2")))
inline double avx_distance(const char* data, const Vector& query) {
    __m256d sum = _mm256_setzero_pd();
    size_t i = 0;
    for (; i + 4 <= query.size(); i += 4) {
        __m128 values;
        std::memcpy(&values, data + i * sizeof(float), sizeof(values));
        const auto delta = _mm256_sub_pd(_mm256_cvtps_pd(values),
            _mm256_cvtps_pd(_mm_loadu_ps(query.data() + i)));
        sum = _mm256_add_pd(sum, _mm256_mul_pd(delta, delta));
    }
    double lanes[4];
    _mm256_storeu_pd(lanes, sum);
    for (; i < query.size(); ++i) {
        const double delta = double(load<float>(data + i * sizeof(float))) - query[i];
        lanes[0] += delta * delta;
    }
    return (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
}
#endif

inline double squared_distance(const char* data, const Vector& query) {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    if (__builtin_cpu_supports("avx2")) return avx_distance(data, query);
#endif
    return portable_distance(data, query);
}

enum class Compare { Equal, Less, LessEqual, Greater, GreaterEqual };

struct Filter {
    size_t column;
    Compare op;
    Value value;
};

struct Expression {
    size_t column = distance_column;
    std::string name;
    Vector query;
};

struct Selection {
    const Table* table = nullptr;
    std::vector<Expression> expressions;
    std::vector<Row> rows;
    std::optional<std::vector<Filter>> streaming_filter;
    size_t row_count = 0;
};

struct Result {
    uint64_t affected = 0;
    std::optional<Selection> selection;
};

class Engine {
    Store store_;
    bool active_ = false;
    bool ann_ = false;

    static uint32_t width(Parser& p) {
        p.need("(");
        const auto value = p.integer();
        p.need(")");
        if (value <= 0 || value > UINT32_MAX) invalid("invalid column width");
        return static_cast<uint32_t>(value);
    }

    void create(Parser& p) {
        if (p.take("vector")) {
            p.need("index");
            auto name = p.identifier();
            p.need("on");
            auto& table = store_.table(p.identifier());
            p.need("(");
            const auto col = table.column(p.identifier());
            p.need(")");
            p.finish();
            if (col != table.vector_column) invalid("index requires a VECTOR column");
            store_.create_index(table, std::move(name));
            return;
        }
        p.need("table");
        auto table = std::make_unique<Table>();
        table->name = p.identifier();
        std::optional<std::string> primary;
        const auto set_primary = [&](std::string name) {
            if (primary) invalid("one primary key is required");
            primary = std::move(name);
        };
        p.need("(");
        do {
            if (p.take("primary")) {
                p.need("key");
                p.need("(");
                set_primary(p.identifier());
                p.need(")");
                continue;
            }
            auto name = p.identifier();
            if (std::ranges::any_of(table->columns, [&](const auto& col) { return col.name == name; })) {
                invalid("duplicate column");
            }
            Column column{name, Kind::Integer, 0};
            if (p.take("varchar")) {
                column.kind = Kind::String;
                column.width = width(p);
            } else if (p.take("vector")) {
                if (table->vector_column != SIZE_MAX) invalid("one VECTOR column per table");
                table->vector_column = table->columns.size();
                column.kind = Kind::Vector;
                column.width = width(p);
            } else p.need("bigint");
            if (p.take("primary")) {
                p.need("key");
                set_primary(name);
            }
            if (p.take("not")) p.need("null");
            table->columns.push_back(std::move(column));
        } while (p.take(","));
        p.need(")");
        p.finish();
        if (!primary) invalid("missing primary key");
        table->primary = table->column(*primary);
        if (table->columns[table->primary].kind != Kind::Integer) invalid("primary key requires BIGINT");
        store_.create(std::move(table));
    }

    uint64_t insert(Parser& p) {
        p.need("into");
        auto& table = store_.table(p.identifier());
        std::vector<size_t> columns;
        if (p.take("(")) {
            std::vector<bool> seen(table.columns.size());
            do {
                const auto col = table.column(p.identifier());
                if (seen[col]) invalid("duplicate INSERT column");
                seen[col] = true;
                columns.push_back(col);
            } while (p.take(","));
            p.need(")");
            if (columns.size() != table.columns.size()) invalid("INSERT requires every column");
        } else {
            for (size_t i = 0; i < table.columns.size(); ++i) columns.push_back(i);
        }
        p.need("values");
        std::vector<Value> row(table.columns.size());
        uint64_t affected = 0;
        do {
            p.need("(");
            for (size_t i = 0; i < columns.size(); ++i) {
                if (i) p.need(",");
                row[columns[i]] = p.value(table.columns[columns[i]]);
            }
            p.need(")");
            store_.set(table, std::get<int64_t>(row[table.primary]), row, true);
            ++affected;
        } while (p.take(","));
        p.finish();
        return affected;
    }

    static std::vector<Filter> filters(Parser& p, const Table& table) {
        std::vector<Filter> result;
        if (p.take("where")) {
            do {
                const auto col = table.column(p.identifier());
                if (col == table.vector_column) invalid("vector comparison unsupported");
                Compare op;
                if (p.take("=")) op = Compare::Equal;
                else if (p.take("<")) op = Compare::Less;
                else if (p.take("<=")) op = Compare::LessEqual;
                else if (p.take(">")) op = Compare::Greater;
                else if (p.take(">=")) op = Compare::GreaterEqual;
                else syntax("expected comparison");
                // Comparison literals can exceed the declared VARCHAR length.
                Column type = table.columns[col];
                if (type.kind == Kind::String) type.width = UINT32_MAX;
                result.push_back({col, op, p.value(type)});
            } while (p.take("and"));
        }
        return result;
    }

    bool matches(const Table& table, const Row& row, const std::vector<Filter>& filters) const {
        const auto* data = store_.row(row.offset);
        for (const auto& f : filters) {
            const auto candidate = field(table, row, data, f.column);
            int order;
            if (const auto* integer = std::get_if<int64_t>(&candidate)) {
                const auto expected = std::get<int64_t>(f.value);
                order = (*integer > expected) - (*integer < expected);
            } else order = std::get<std::string_view>(candidate).compare(std::get<std::string>(f.value));
            switch (f.op) {
                case Compare::Equal: if (order != 0) return false; break;
                case Compare::Less: if (order >= 0) return false; break;
                case Compare::LessEqual: if (order > 0) return false; break;
                case Compare::Greater: if (order <= 0) return false; break;
                case Compare::GreaterEqual: if (order < 0) return false; break;
            }
        }
        return true;
    }

    template<typename Visit>
    void visit(const Table& table, const std::vector<Filter>& filters, Visit&& callback) const {
        for (const auto& f : filters) {
            if (f.column == table.primary && f.op == Compare::Equal) {
                const auto it = table.rows.find(std::get<int64_t>(f.value));
                if (it != table.rows.end() && it->second) {
                    const Row row{it->first, it->second};
                    if (matches(table, row, filters)) callback(row);
                }
                return;
            }
        }
        for (const auto& [key, offset] : table.rows) {
            const Row row{key, offset};
            if (offset && matches(table, row, filters) && !callback(row)) break;
        }
    }

    Value copy_field(const Table& table, const Row& row, size_t column) const {
        const auto item = field(table, row, store_.row(row.offset), column);
        if (const auto* integer = std::get_if<int64_t>(&item)) return *integer;
        if (const auto* string = std::get_if<std::string_view>(&item)) return std::string(*string);
        Vector vector(table.columns[column].width);
        std::memcpy(vector.data(), std::get<const char*>(item), table.vector_bytes());
        return vector;
    }

    uint64_t change(Parser& p, bool update) {
        if (!update) p.need("from");
        auto& table = store_.table(p.identifier());
        std::vector<std::optional<Value>> assignments(table.columns.size());
        if (update) {
            p.need("set");
            do {
                const auto col = table.column(p.identifier());
                if (col == table.primary || assignments[col]) invalid("invalid UPDATE column");
                p.need("=");
                assignments[col] = p.value(table.columns[col]);
            } while (p.take(","));
        }
        const auto conditions = filters(p, table);
        p.finish();
        uint64_t affected = 0;
        // Replacing offsets or marking tombstones does not invalidate hash iterators.
        visit(table, conditions, [&](Row row) {
            if (update) {
                std::vector<Value> values;
                values.reserve(table.columns.size());
                for (size_t i = 0; i < table.columns.size(); ++i) {
                    values.push_back(assignments[i] ? *assignments[i] : copy_field(table, row, i));
                }
                store_.set(table, row.key, values, false);
            } else store_.erase(table, row.key);
            ++affected;
            return true;
        });
        return affected;
    }

    // Resolve projection names after FROM supplies a schema. Keeping the original
    // literal tokens in a Parser snapshot also supports reordered vector columns.
    struct Projection {
        std::string column;
        std::string alias;
        std::optional<Parser> vector_literal;
    };

    static Projection expression(Parser& p) {
        Projection result;
        if (p.take("l2_distance")) {
            p.need("(");
            result.column = p.identifier();
            p.need(",");
            result.vector_literal = p;
            p.skip_vector_literal();
            p.need(")");
        } else result.column = p.identifier();
        if (p.take("as")) result.alias = p.identifier();
        return result;
    }

    static Expression resolve(Projection& spec, const Table& table) {
        const auto col = table.column(spec.column);
        if (!spec.vector_literal) return {col, spec.alias.empty() ? spec.column : spec.alias, {}};
        if (col != table.vector_column) invalid("L2_DISTANCE requires a VECTOR column");
        return {distance_column, spec.alias.empty() ? "l2_distance" : spec.alias,
                spec.vector_literal->vector(table.columns[col].width)};
    }

    Selection select(Parser& p) const {
        const bool star = p.take("*");
        std::vector<Projection> projections;
        if (!star) {
            do { projections.push_back(expression(p)); } while (p.take(","));
        }
        p.need("from");
        const auto& table = store_.table(p.identifier());
        Selection result;
        result.table = &table;
        if (star) {
            for (size_t i = 0; i < table.columns.size(); ++i) {
                result.expressions.push_back({i, table.columns[i].name, {}});
            }
        } else {
            for (auto& spec : projections) result.expressions.push_back(resolve(spec, table));
        }
        auto conditions = filters(p, table);
        std::optional<Expression> order;
        bool desc = false;
        if (p.take("order")) {
            p.need("by");
            auto spec = expression(p);
            if (!spec.alias.empty()) syntax("invalid ORDER BY alias");
            if (!spec.vector_literal) {
                for (const auto& column : result.expressions) {
                    if (column.name == spec.column) { order = column; break; }
                }
            }
            if (!order) order = resolve(spec, table);
            if (order->column == table.vector_column) invalid("vector ordering unsupported");
            desc = p.take("desc");
            if (!desc) p.take("asc");
        }
        size_t limit = SIZE_MAX;
        if (p.take("limit")) {
            const auto n = p.integer();
            if (n < 0) invalid("negative LIMIT");
            limit = n;
        }
        p.finish();
        if (!limit) return result;
        if (!order && limit == SIZE_MAX) {
            visit(table, conditions, [&](Row) { ++result.row_count; return true; });
            result.streaming_filter = std::move(conditions);
            return result;
        }

        const auto before = [&](const Row& a, const Row& b) {
            int comparison = 0;
            if (order) {
                const auto col = order->column;
                if (col == distance_column) comparison = (a.distance > b.distance) - (a.distance < b.distance);
                else {
                    const auto lhs = field(table, a, store_.row(a.offset), col);
                    const auto rhs = field(table, b, store_.row(b.offset), col);
                    if (const auto* value = std::get_if<int64_t>(&lhs)) {
                        const auto other = std::get<int64_t>(rhs);
                        comparison = (*value > other) - (*value < other);
                    } else comparison = std::get<std::string_view>(lhs).compare(std::get<std::string_view>(rhs));
                }
            }
            return comparison ? (desc ? comparison > 0 : comparison < 0) : a.key < b.key;
        };
        const bool bounded = limit < table.rows.size();
        auto& rows = result.rows;
        if (bounded) rows.reserve(limit);
        const auto collect = [&](Row row) {
            if (order && order->column == distance_column) {
                row.distance = squared_distance(store_.row(row.offset), order->query);
            }
            if (!bounded) rows.push_back(row);
            else if (rows.size() < limit) {
                rows.push_back(row);
                std::push_heap(rows.begin(), rows.end(), before);
            } else if (before(row, rows.front())) {
                std::pop_heap(rows.begin(), rows.end(), before);
                rows.back() = row;
                std::push_heap(rows.begin(), rows.end(), before);
            }
            return true;
        };
        const bool primary_lookup = std::ranges::any_of(conditions, [&](const auto& filter) {
            return filter.column == table.primary && filter.op == Compare::Equal;
        });
        if (ann_ && order && order->column == distance_column && !desc &&
            bounded && !primary_lookup && !table.index_name.empty()) {
            store_.rebuild_index(table);
            const double target = order->query.front();
            auto right = std::lower_bound(table.index.begin(), table.index.end(), target,
                [](const auto& point, double value) { return point.first < value; });
            auto left = right;
            const double infinity = std::numeric_limits<double>::infinity();
            while (left != table.index.begin() || right != table.index.end()) {
                const double left_delta = left == table.index.begin() ? infinity : target - std::prev(left)->first;
                const double right_delta = right == table.index.end() ? infinity : right->first - target;
                const double delta = std::min(left_delta, right_delta);
                // One coordinate is a lower bound on Euclidean distance. Strict
                // comparison preserves the primary-key tie break at the boundary.
                if (rows.size() == limit && delta * delta > rows.front().distance) break;
                const auto offset = left_delta < right_delta ? (--left)->second : (right++)->second;
                const Row row{store_.key_at(offset), offset};
                if (matches(table, row, conditions)) collect(row);
            }
        } else visit(table, conditions, collect);
        std::sort(rows.begin(), rows.end(), before);
        result.row_count = rows.size();
        return result;
    }

public:
    explicit Engine(const std::filesystem::path& directory) : store_(directory) {}
    void mode(bool ann) { ann_ = ann; }

    Result execute(Parser& p) {
        if (p.take("begin")) {
            p.finish();
            if (active_) throw SqlError("TRANSACTION", "transaction already active");
            store_.begin();
            active_ = true;
            return {};
        }
        if (p.take("commit")) {
            p.finish();
            if (!active_) throw SqlError("TRANSACTION", "no active transaction");
            store_.commit();
            active_ = false;
            return {};
        }
        if (p.take("rollback")) {
            p.finish();
            if (!active_) throw SqlError("TRANSACTION", "no active transaction");
            store_.rollback();
            active_ = false;
            return {};
        }
        if (p.take("select")) return {0, select(p)};

        if (!active_) store_.begin();
        const auto point = store_.savepoint();
        Result result;
        try {
            if (p.take("create")) create(p);
            else if (p.take("insert")) result.affected = insert(p);
            else if (p.take("update")) result.affected = change(p, true);
            else if (p.take("delete")) result.affected = change(p, false);
            else throw SqlError("UNSUPPORTED_SQL", "unsupported statement");
        } catch (const SqlError&) {
            store_.restore(point);
            throw;
        }
        if (active_) store_.flush();
        else store_.commit();
        return result;
    }

    void close() {
        if (active_) store_.rollback();
        active_ = false;
    }

    template<typename Emit>
    void emit_rows(const Selection& selection, Emit&& emit) const {
        if (selection.streaming_filter) {
            visit(*selection.table, *selection.streaming_filter, [&](Row row) {
                emit(row);
                return true;
            });
        } else {
            for (const auto& row : selection.rows) emit(row);
        }
    }

    Field field_value(const Table& table, const Row& row, size_t column) const {
        return field(table, row, store_.row(row.offset), column);
    }

    double distance(const Row& row, const Vector& query) const {
        return std::sqrt(squared_distance(store_.row(row.offset), query));
    }
};

} // namespace mini
