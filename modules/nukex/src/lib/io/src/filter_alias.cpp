#include "nukex/io/filter_alias.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>

namespace nukex {

namespace {

// The sets the shipped database carries a calibrated entry for. Anything not
// listed here has no entry, and saying so is the correct answer -- see the
// header. Kept in step with CANONICAL_DUAL in tools/import_qe_research.py and
// with FilterClassifier::known_table().
const std::map<std::set<QSolveLine>, std::string>& canonical_table() {
    static const std::map<std::set<QSolveLine>, std::string> table = {
        { {QSolveLine::Ha},                                        "Ha"     },
        { {QSolveLine::OIII},                                      "OIII"   },
        { {QSolveLine::SII},                                       "SII"    },
        { {QSolveLine::Ha,  QSolveLine::OIII},                   "HaO3"   },
        { {QSolveLine::SII, QSolveLine::OIII},                   "S2O3"   },
        { {QSolveLine::Ha,  QSolveLine::OIII, QSolveLine::SII}, "HaO3S2" },
    };
    return table;
}

constexpr int kSchemaVersion = 1;

} // namespace

std::string canonical_for_lines(const std::set<QSolveLine>& lines) {
    const auto& t = canonical_table();
    auto it = t.find(lines);
    return it == t.end() ? std::string() : it->second;
}

std::string canonical_for_lines(std::initializer_list<QSolveLine> lines) {
    return canonical_for_lines(std::set<QSolveLine>(lines));
}

std::string FilterAliasStore::normalize(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

bool FilterAliasStore::load(const std::string& path) {
    aliases_.clear();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return true;   // first run on this machine; nothing to load
    }

    std::ifstream in(path);
    if (!in) {
        return false;
    }

    try {
        nlohmann::json j;
        in >> j;
        const auto it = j.find("aliases");
        if (it != j.end() && it->is_object()) {
            for (auto& [k, v] : it->items()) {
                if (v.is_string()) {
                    aliases_[normalize(k)] = v.get<std::string>();
                }
            }
        }
    } catch (const std::exception&) {
        // A truncated or hand-edited file must not take a stack down. Report
        // it and carry on with the shipped table only.
        aliases_.clear();
        return false;
    }
    return true;
}

bool FilterAliasStore::save(const std::string& path) const {
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    nlohmann::json j;
    j["schema_version"] = kSchemaVersion;
    j["aliases"] = nlohmann::json::object();
    for (const auto& [k, v] : aliases_) {
        j["aliases"][k] = v;
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return false;
    }
    out << j.dump(2) << "\n";
    return static_cast<bool>(out);
}

std::string FilterAliasStore::lookup(const std::string& raw_filter_name) const {
    auto it = aliases_.find(normalize(raw_filter_name));
    return it == aliases_.end() ? std::string() : it->second;
}

void FilterAliasStore::set(const std::string& raw_filter_name,
                           const std::string& canonical) {
    aliases_[normalize(raw_filter_name)] = canonical;
}

} // namespace nukex
