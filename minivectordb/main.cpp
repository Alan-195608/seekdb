#include "engine.hpp"

#include <jansson.h>

#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>

namespace {

void write_string(std::ostream& out, std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    out.put('"');
    for (const unsigned char c : text) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\t': out << "\\t"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            default:
                if (c < 0x20) out << "\\u00" << hex[c >> 4] << hex[c & 15];
                else out.put(static_cast<char>(c));
        }
    }
    out.put('"');
}

std::string_view string_value(const json_t* value) {
    if (!json_is_string(value)) return {};
    return {json_string_value(value), json_string_length(value)};
}

using Json = std::unique_ptr<json_t, decltype(&json_decref)>;

void response_id(std::ostream& out, json_t* id) {
    out << "{\"id\":";
    if (!id) {
        out << "null";
        return;
    }
    const int status = json_dump_callback(id, [](const char* data, size_t size, void* target) {
        auto& stream = *static_cast<std::ostream*>(target);
        stream.write(data, size);
        return stream ? 0 : -1;
    }, &out, JSON_ENCODE_ANY | JSON_COMPACT);
    if (status != 0) throw std::runtime_error("could not write response id");
}

void respond(std::ostream& out, json_t* id, const mini::Engine& engine, const mini::Result& result) {
    response_id(out, id);
    out << ",\"ok\":true";
    if (!result.selection) {
        out << ",\"affected_rows\":" << result.affected << "}\n";
        return;
    }
    const auto& selection = *result.selection;
    const auto& table = *selection.table;
    out << ",\"columns\":[";
    for (size_t i = 0; i < selection.expressions.size(); ++i) {
        if (i) out.put(',');
        write_string(out, selection.expressions[i].name);
    }
    out << "],\"rows\":[";
    bool first = true;
    engine.emit_rows(selection, [&](const mini::Row& row) {
        if (!first) out.put(',');
        first = false;
        out.put('[');
        for (size_t i = 0; i < selection.expressions.size(); ++i) {
            if (i) out.put(',');
            const auto& expression = selection.expressions[i];
            if (expression.column == mini::distance_column) {
                out << engine.distance(row, expression.query);
                continue;
            }
            const auto item = engine.field_value(table, row, expression.column);
            if (const auto* value = std::get_if<int64_t>(&item)) out << *value;
            else if (const auto* text = std::get_if<std::string_view>(&item)) write_string(out, *text);
            else {
                const auto* vector = std::get<const char*>(item);
                const size_t width = table.columns[expression.column].width;
                out.put('[');
                for (size_t j = 0; j < width; ++j) {
                    if (j) out.put(',');
                    out << mini::load<float>(vector + j * sizeof(float));
                }
                out.put(']');
            }
        }
        out.put(']');
    });
    out << "]}\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 1 && (argc != 3 || std::string_view(argv[1]) != "--data")) {
        std::cerr << "usage: minivectordb [--data DIRECTORY]\n";
        return 2;
    }
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    std::cout.imbue(std::locale::classic());
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10);
    try {
        mini::Engine engine(argc == 3 ? argv[2] : "/data");
        std::cout << "{\"type\":\"ready\"}\n" << std::flush;
        std::string line;
        while (std::getline(std::cin, line)) {
            const Json request(json_loadb(line.data(), line.size(),
                                          JSON_REJECT_DUPLICATES | JSON_ALLOW_NUL, nullptr),
                               json_decref);
            auto* id = json_object_get(request.get(), "id");
            const auto op = string_value(json_object_get(request.get(), "op"));
            try {
                if (!id) throw mini::SqlError("INVALID_REQUEST", "expected a request with an id");
                if (op == "shutdown") {
                    engine.close();
                    response_id(std::cout, id);
                    std::cout << ",\"ok\":true}\n" << std::flush;
                    return 0;
                }
                auto* sql = json_object_get(request.get(), "sql");
                if (op != "execute" || !json_is_string(sql)) {
                    throw mini::SqlError("INVALID_REQUEST", "expected execute with a SQL string");
                }
                auto* mode = json_object_get(request.get(), "search_mode");
                const auto search_mode = string_value(mode);
                if (mode && search_mode != "exact" && search_mode != "ann") {
                    throw mini::SqlError("INVALID_REQUEST", "expected exact or ann search_mode");
                }
                engine.mode(search_mode == "ann");
                mini::Parser parser(string_value(sql));
                respond(std::cout, id, engine, engine.execute(parser));
            } catch (const mini::SqlError& error) {
                response_id(std::cout, id);
                std::cout << ",\"ok\":false,\"error\":{\"code\":";
                write_string(std::cout, mini::lower(error.code));
                std::cout << ",\"message\":";
                write_string(std::cout, error.what());
                std::cout << "}}\n";
            }
            std::cout.flush();
        }
        engine.close();
    } catch (const std::exception& error) {
        std::cerr << "minivectordb: " << error.what() << '\n';
        return 1;
    }
}
