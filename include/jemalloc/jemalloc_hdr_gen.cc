#include <fstream>
#include <iostream>
#include <regex>
#include <string>

static bool should_exclude(const std::string& line) {
    static const std::regex pattern("Generated from .* by configure");
    return std::regex_search(line, pattern);
}

static std::string strip_trailing_spaces(std::string line) {
    while (!line.empty() && line.back() == ' ') {
        line.pop_back();
    }
    return line;
}

static bool append_filtered(const std::string& src_path, std::ofstream& out) {
    std::ifstream in(src_path);
    if (!in) {
        std::cerr << "Error: could not open " << src_path << "\n";
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!should_exclude(line)) {
            out << strip_trailing_spaces(line) << "\n";
        }
    }
    out << "\n";
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <output> <src...>\n";
        return 1;
    }

    const std::string out_path = argv[1];
    std::ofstream out(out_path);
    if (!out) {
        std::cerr << "Error: could not open output file " << out_path << "\n";
        return 1;
    }

    out << "#ifndef JEMALLOC_H_\n";
    out << "#define JEMALLOC_H_\n";
    out << "#ifdef __cplusplus\n";
    out << "extern \"C\" {\n";
    out << "#endif\n";
    out << "\n";

    for (int i = 2; i < argc; ++i) {
        if (!append_filtered(argv[i], out)) {
            return 1;
        }
    }

    out << "#ifdef __cplusplus\n";
    out << "}\n";
    out << "#endif\n";
    out << "#endif /* JEMALLOC_H_ */\n";

    return 0;
}
