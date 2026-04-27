#include "nukex/calibration/qe_database.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

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

LoadResult QEDatabase::load_shipped(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return {false, "QE database missing or unreadable: " + path};
    }
    return parse_and_merge(path, /*is_override*/ false);
}

LoadResult QEDatabase::load_override(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return {false, "QE override file missing or unreadable: " + path};
    }
    return parse_and_merge(path, /*is_override*/ true);
}

LoadResult QEDatabase::parse_and_merge(const std::string& path, bool is_override) {
    using nlohmann::json;
    std::ifstream f(path);
    if (!f.is_open()) {
        return {false, std::string(is_override ? "QE override" : "QE database") + " missing: " + path};
    }
    json doc;
    try {
        f >> doc;
    } catch (const json::parse_error& e) {
        std::ostringstream oss;
        oss << (is_override ? "QE override" : "QE database")
            << " is malformed at line " << e.byte
            << " (parser: " << e.what() << ")";
        return {false, oss.str()};
    }

    if (doc.contains("cameras") && doc["cameras"].is_object()) {
        for (auto it = doc["cameras"].begin(); it != doc["cameras"].end(); ++it) {
            const std::string& name = it.key();
            const json& cam_json    = it.value();
            CameraQE cam;
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
            // Override semantics: replace whole camera record.
            // (Coarse but reflects spec: "override wins on key collision".)
            cameras_[name] = std::move(cam);
        }
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

bool QEDatabase::has_camera(const std::string& name) const {
    return cameras_.find(name) != cameras_.end();
}

bool QEDatabase::has_filter(const std::string& name) const {
    return filters_.find(name) != filters_.end();
}

QEConfidence QEDatabase::confidence(const std::string& camera) const {
    auto it = cameras_.find(camera);
    if (it == cameras_.end()) return QEConfidence::UNKNOWN;
    return it->second.confidence;
}

double QEDatabase::lookup_camera_qe(const std::string& camera,
                                    double             wavelength_nm,
                                    Photosite          photosite) const {
    auto it = cameras_.find(camera);
    if (it == cameras_.end()) return 0.0;
    const auto& curve = it->second.qe_by_wavelength;
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
