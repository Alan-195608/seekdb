#include "engine.hpp"

#include <iomanip>
#include <iostream>
#include <locale>

namespace {

// Development wire protocol. The official driver adapter belongs here.
void escape(std::ostream& out, std::string_view text) {
    for (const char c : text) {
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '\t': out << "\\t"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\0': out << "\\0"; break;
            default: out.put(c);
        }
    }
}

void respond(std::ostream& out, const mini::Engine& engine, const mini::Result& result) {
    if (!result.selection) {
        out << "OK\t" << result.affected << '\n';
        return;
    }
    const auto& selection = *result.selection;
    const auto& table = *selection.table;
    out << "RESULT\t" << selection.row_count << '\t' << selection.expressions.size() << '\n';
    for (size_t i = 0; i < selection.expressions.size(); ++i) {
        if (i) out.put('\t');
        escape(out, selection.expressions[i].name);
    }
    out.put('\n');
    engine.emit_rows(selection, [&](const mini::Row& row) {
        for (size_t i = 0; i < selection.expressions.size(); ++i) {
            if (i) out.put('\t');
            const auto& expression = selection.expressions[i];
            if (expression.column == mini::distance_column) {
                out << engine.distance(row, expression.query);
                continue;
            }
            const auto item = engine.field_value(table, row, expression.column);
            if (const auto* value = std::get_if<int64_t>(&item)) out << *value;
            else if (const auto* text = std::get_if<std::string_view>(&item)) escape(out, *text);
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
        out.put('\n');
    });
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
        std::cout << "READY\n" << std::flush;
        std::string line;
        while (std::getline(std::cin, line)) {
            try {
                mini::Parser p(line);
                if (p.empty()) continue;
                if (p.take("quit") || p.take("exit")) {
                    p.finish();
                    engine.close();
                    std::cout << "OK\t0\n" << std::flush;
                    return 0;
                }
                if (p.take("mode")) {
                    const bool ann = p.take("ann");
                    if (!ann) p.need("exact");
                    p.finish();
                    engine.mode(ann);
                    std::cout << "OK\t0\n";
                } else respond(std::cout, engine, engine.execute(p));
            } catch (const mini::SqlError& error) {
                std::cout << "ERR\t" << error.code << '\t';
                escape(std::cout, error.what());
                std::cout.put('\n');
            }
            std::cout.flush();
        }
        engine.close();
    } catch (const std::exception& error) {
        std::cerr << "minivectordb: " << error.what() << '\n';
        return 1;
    }
}
