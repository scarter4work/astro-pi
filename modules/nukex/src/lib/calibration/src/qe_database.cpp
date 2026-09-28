#include "nukex/calibration/qe_database.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace nukex {

namespace {

QEConfidence parse_confidence(const std::string& s) {
    if (s == "high")   return QEConfidence::HIGH;
    if (s == "medium") return QEConfidence::MEDIUM;
    if (s == "low")    return QEConfidence::LOW;
    return QEConfidence::UNKNOWN;
}

Photosite parse_photosite_key(const std::string& key) {
    if (key == "R")            return Photosite::R;
    if (key == "G")            return Photosite::G;
    if (key == "B")            return Photosite::B;
    if (key == "Gr" || key == "Gb") return Photosite::G;
    return Photosite::MONO_PEAK;
}

} // namespace

namespace {

std::pair<size_t, size_t> line_col_for_byte(const std::string& text, size_t byte) {
    size_t line = 1, col = 1;
    const size_t end = std::min(byte, text.size());
    for (size_t i = 0; i < end; ++i) {
        if (text[i] == '\n') { ++line; col = 1; } else { ++col; }
    }
    return {line, col};
}

LoadResult slurp(const std::string& path, const char* context, std::string& out_text) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return {false, std::string(context) + " missing or unreadable: " + path};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    out_text = ss.str();
    return {true, ""};
}

} // namespace

LoadResult QEDatabase::load_embedded() {
    return parse_and_merge(embedded_qe_database_json(), "QE database (embedded)");
}

LoadResult QEDatabase::load_shipped(const std::string& path) {
    std::string text;
    auto r = slurp(path, "QE database", text);
    if (!r.ok) return r;
    return parse_and_merge(text, "QE database");
}

LoadResult QEDatabase::load_override(const std::string& path) {
    std::string text;
    auto r = slurp(path, "QE override", text);
    if (!r.ok) return r;
    return parse_and_merge(text, "QE override");
}

LoadResult QEDatabase::parse_and_merge(const std::string& text, const char* context) {
    using nlohmann::json;
    json doc;
    try {
        doc = json::parse(text);
    } catch (const json::parse_error& e) {
        auto [line, col] = line_col_for_byte(text, e.byte);
        std::ostringstream oss;
        oss << context << " is malformed at line " << line << " col " << col
            << " (parser: " << e.what() << ")";
        return {false, oss.str()};
    }

    // Cameras are parsed and validated in full before anything is merged, so
    // a rejected document leaves the database exactly as it was.
    struct ParsedCamera {
        std::string              name;
        CameraQE                 cam;
        std::vector<std::string> alias_keys;   // normalized
    };
    std::vector<ParsedCamera> parsed_cameras;

    if (doc.contains("cameras") && doc["cameras"].is_object()) {
        // normalized key -> camera name that claimed it in THIS document.
        std::unordered_map<std::string, std::string> doc_keys;
        auto claim = [&](const std::string& key, const std::string& raw,
                         const std::string& cam_name, std::string& err) {
            if (key.empty()) {
                err = std::string(context) + ": camera '" + cam_name + "' has name/alias '" +
                      raw + "' with no alphanumeric characters";
                return false;
            }
            auto [pos, inserted] = doc_keys.emplace(key, cam_name);
            if (!inserted && pos->second != cam_name) {
                err = std::string(context) + ": camera name/alias '" + raw +
                      "' (normalized '" + key + "') is claimed by both '" + pos->second +
                      "' and '" + cam_name + "' -- ambiguous camera identification";
                return false;
            }
            return true;
        };

        for (auto it = doc["cameras"].begin(); it != doc["cameras"].end(); ++it) {
            const std::string& name = it.key();
            const json& cam_json    = it.value();
            std::string err;
            if (!claim(normalize_camera_key(name), name, name, err)) return {false, err};

            ParsedCamera pc;
            pc.name = name;
            if (cam_json.contains("aliases")) {
                const json& aj = cam_json["aliases"];
                if (!aj.is_array()) {
                    return {false, std::string(context) + ": camera '" + name +
                                   "' has non-array 'aliases'"};
                }
                for (const auto& a : aj) {
                    if (!a.is_string()) {
                        return {false, std::string(context) + ": camera '" + name +
                                       "' has a non-string alias"};
                    }
                    const std::string raw = a.get<std::string>();
                    const std::string key = normalize_camera_key(raw);
                    if (!claim(key, raw, name, err)) return {false, err};
                    pc.alias_keys.push_back(key);
                }
            }

            CameraQE& cam = pc.cam;
            if (cam_json.contains("sensor")) cam.sensor = cam_json["sensor"].get<std::string>();
            if (cam_json.contains("type"))   cam.type   = cam_json["type"].get<std::string>();
            if (cam_json.contains("bayer"))  cam.bayer  = cam_json["bayer"].get<std::string>();
            if (cam_json.contains("confidence")) {
                cam.confidence = parse_confidence(cam_json["confidence"].get<std::string>());
            }
            if (cam_json.contains("qe") && cam_json["qe"].is_object()) {
                for (auto wlit = cam_json["qe"].begin(); wlit != cam_json["qe"].end(); ++wlit) {
                    int wl = std::stoi(wlit.key());
                    std::map<Photosite, double> per_site;
                    for (auto pit = wlit.value().begin(); pit != wlit.value().end(); ++pit) {
                        per_site[parse_photosite_key(pit.key())] = pit.value().get<double>();
                    }
                    cam.qe_by_wavelength[wl] = per_site;
                }
            }
            parsed_cameras.push_back(std::move(pc));
        }
    }

    for (auto& pc : parsed_cameras) {
        // Override semantics: "override wins on key collision". A later
        // document naming an EXISTING camera id (case/punctuation-insensitive)
        // replaces that whole record. A later document whose camera name
        // matches only an existing ALIAS becomes its own record and the alias
        // key is re-pointed to it (the later document wins that name without
        // rewriting the aliased camera's data for its other names).
        const std::string key = normalize_camera_key(pc.name);
        std::string target = pc.name;
        auto existing = camera_keys_.find(key);
        if (existing != camera_keys_.end() &&
            normalize_camera_key(existing->second) == key) {
            target = existing->second;
        }
        cameras_[target]  = std::move(pc.cam);
        camera_keys_[key] = target;
        for (const auto& ak : pc.alias_keys) camera_keys_[ak] = target;
    }

    if (doc.contains("filters") && doc["filters"].is_object()) {
        for (auto it = doc["filters"].begin(); it != doc["filters"].end(); ++it) {
            const std::string& name = it.key();
            const json& fjson = it.value();
            FilterPassband fp;
            if (fjson.contains("type")) fp.type = fjson["type"].get<std::string>();
            if (fjson.contains("lines") && fjson["lines"].is_array()) {
                for (const auto& line : fjson["lines"]) {
                    EmissionLine el;
                    if (line.contains("name")) el.name = line["name"].get<std::string>();
                    if (line.contains("wavelength_nm")) el.wavelength_nm = line["wavelength_nm"].get<double>();
                    if (line.contains("fwhm_nm"))      el.fwhm_nm       = line["fwhm_nm"].get<double>();
                    fp.lines.push_back(el);
                }
            }
            filters_[name] = std::move(fp);
        }
    }

    return {true, ""};
}

std::string QEDatabase::normalize_camera_key(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x80 && std::isalnum(u)) {
            out.push_back(static_cast<char>(std::tolower(u)));
        }
    }
    return out;
}

std::string QEDatabase::resolve_camera_id(const std::string& name) const {
    if (cameras_.count(name)) return name;          // exact id
    const std::string key = normalize_camera_key(name);
    if (key.empty()) return {};
    auto it = camera_keys_.find(key);
    return it == camera_keys_.end() ? std::string{} : it->second;
}

const CameraQE* QEDatabase::find_camera(const std::string& name) const {
    const std::string id = resolve_camera_id(name);
    if (id.empty()) return nullptr;
    auto it = cameras_.find(id);
    return it == cameras_.end() ? nullptr : &it->second;
}

bool QEDatabase::has_camera(const std::string& name) const {
    return find_camera(name) != nullptr;
}

bool QEDatabase::has_filter(const std::string& name) const {
    return filters_.find(name) != filters_.end();
}

QEConfidence QEDatabase::confidence(const std::string& camera) const {
    const CameraQE* cam = find_camera(camera);
    return cam ? cam->confidence : QEConfidence::UNKNOWN;
}

double QEDatabase::lookup_camera_qe(const std::string& camera,
                                    double             wavelength_nm,
                                    Photosite          photosite) const {
    const CameraQE* cam = find_camera(camera);
    if (!cam) return 0.0;
    const auto& curve = cam->qe_by_wavelength;
    if (curve.empty()) return 0.0;

    auto upper = curve.upper_bound(static_cast<int>(wavelength_nm + 0.5));
    if (upper == curve.begin()) {
        auto p = upper->second.find(photosite);
        return (p == upper->second.end()) ? 0.0 : p->second;
    }
    if (upper == curve.end()) {
        auto last = std::prev(upper);
        auto p = last->second.find(photosite);
        return (p == last->second.end()) ? 0.0 : p->second;
    }
    auto lower = std::prev(upper);
    auto pl = lower->second.find(photosite);
    auto pu = upper->second.find(photosite);
    if (pl == lower->second.end() || pu == upper->second.end()) return 0.0;
    double t = (wavelength_nm - lower->first) / static_cast<double>(upper->first - lower->first);
    return pl->second + t * (pu->second - pl->second);
}

FilterPassband QEDatabase::lookup_filter(const std::string& name) const {
    auto it = filters_.find(name);
    if (it == filters_.end()) return {};
    return it->second;
}

} // namespace nukex
