#include "nukex/calibration/qe_database.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <sstream>

namespace nukex {

namespace {

// Which side of the mono/colour split a name or a DB "type" field lands on.
enum class SensorKind { UNKNOWN, MONO, OSC };

// Every digit in a string, in order: "IMX585" -> "585", "Panasonic-MN34230"
// -> "34230". Used to compare a sensor designation against the number a
// vendor put in a product name.
std::string digits_of(const std::string& s) {
    std::string d;
    for (unsigned char c : s)
        if (std::isdigit(c)) d.push_back(static_cast<char>(c));
    return d;
}

// Split a normalised product name into its trailing number and the vendor's
// mono/colour marker. The marker sits immediately after the number -- ZWO
// writes MM/MC, others a bare M/C -- and anything past it ("pro") is noise.
//   "atr585m"     -> ("585",  MONO)
//   "asi2600mcpro"-> ("2600", OSC)
//   "asi585"      -> false: no marker, so mono vs colour is a coin flip.
// The LAST run of digits in a normalised name, with the offset just past
// it: "asi2600mcpro" -> "2600", "qhy5iii462c" -> "462" (not "5462" -- a
// vendor's series digits are not part of the model number).
std::string trailing_number(const std::string& key, size_t* number_end = nullptr) {
    size_t end   = 0;
    bool   found = false;
    for (size_t i = key.size(); i-- > 0; ) {
        if (std::isdigit(static_cast<unsigned char>(key[i]))) {
            end = i + 1; found = true; break;
        }
    }
    if (!found) return {};
    size_t start = end;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(key[start - 1]))) --start;
    if (number_end) *number_end = end;
    return key.substr(start, end - start);
}

bool split_sensor_marker(const std::string& key,
                         std::string&       number,
                         SensorKind&        kind) {
    size_t end = 0;
    number = trailing_number(key, &end);
    if (number.empty()) return false;

    const std::string suffix = key.substr(end);
    if      (suffix.rfind("mm", 0) == 0) kind = SensorKind::MONO;
    else if (suffix.rfind("mc", 0) == 0) kind = SensorKind::OSC;
    else if (suffix.rfind("m",  0) == 0) kind = SensorKind::MONO;
    else if (suffix.rfind("c",  0) == 0) kind = SensorKind::OSC;
    else return false;
    return true;
}

SensorKind parse_sensor_kind(const std::string& type) {
    std::string t;
    for (unsigned char c : type)
        if (std::isalpha(c)) t.push_back(static_cast<char>(std::tolower(c)));
    if (t == "mono") return SensorKind::MONO;
    if (t == "osc")  return SensorKind::OSC;
    return SensorKind::UNKNOWN;
}

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

    if (doc.contains("cameras") && doc["cameras"].is_object()) {
        // Parsed into a local map first so a collision detected partway
        // through this document leaves cameras_ (and any earlier load)
        // completely unaffected — a failed load must not half-apply.
        std::unordered_map<std::string, CameraQE> parsed_cameras;
        std::unordered_map<std::string, std::string> normalized_to_raw;
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

            const std::string norm = normalize_camera_key(name);
            auto seen = normalized_to_raw.find(norm);
            if (seen != normalized_to_raw.end() && seen->second != name) {
                std::ostringstream oss;
                oss << context << ": camera keys '" << seen->second << "' and '" << name
                    << "' both normalise to '" << norm << "'";
                return {false, oss.str()};
            }
            normalized_to_raw[norm] = name;
            parsed_cameras[norm]    = std::move(cam);
        }
        // No collision within this document — override semantics: replace
        // whole camera record on key collision against earlier loads.
        // (Coarse but reflects spec: "override wins on key collision".)
        for (auto& kv : parsed_cameras) {
            cameras_[kv.first] = std::move(kv.second);
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

    // Both load_shipped and load_override land here, and an override can add
    // cameras, so the index is rebuilt on every successful merge rather than
    // once at construction.
    rebuild_sensor_index();
    return {true, ""};
}

std::string QEDatabase::normalize_camera_key(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string QEDatabase::resolve_camera(const std::string& instrume) const {
    const std::string key = normalize_camera_key(instrume);
    if (key.empty()) return {};
    if (cameras_.count(key)) return key;
    // Longest contained key wins; on a length tie, the lexicographically
    // smaller key wins, so the result does not depend on unordered_map's
    // iteration order.
    std::string best;
    for (const auto& kv : cameras_) {
        if (key.find(kv.first) == std::string::npos) continue;
        if (kv.first.size() > best.size() ||
            (kv.first.size() == best.size() && kv.first < best)) {
            best = kv.first;
        }
    }
    if (!best.empty()) return best;

    // Third tier: the silicon. A rebadged camera ("ATR585M") carries no DB
    // product key, but its sensor number and mono/colour marker name a
    // sensor whose QE curve is already in the DB under some other vendor's
    // product name. Vendor branding is not QE-relevant; the sensor is. The
    // comparison is against the entry's `sensor` field, never its product
    // key, because the two disagree constantly -- ASI2600MM is an IMX571.
    std::string number;
    SensorKind  want = SensorKind::UNKNOWN;
    if (!split_sensor_marker(key, number, want) || want == SensorKind::UNKNOWN)
        return {};

    auto it = sensor_index_.find(sensor_index_key(number, want == SensorKind::MONO));
    return (it == sensor_index_.end()) ? std::string{} : it->second;
}

void QEDatabase::rebuild_sensor_index() {
    sensor_index_.clear();
    for (const auto& kv : cameras_) {
        // The generic row is a caller's explicit fallback choice, never a
        // resolution result -- reaching it by resolution would hide an
        // unknown camera behind a confident-looking answer.
        if (kv.first == kGenericOSCCamera) continue;

        const SensorKind kind = parse_sensor_kind(kv.second.type);
        if (kind == SensorKind::UNKNOWN) continue;
        const bool mono = (kind == SensorKind::MONO);

        // Register the vendor's product number AND the sensor's own
        // designation, so both "2600" and "571" reach the ASI2600MM row.
        // Indexing by (number, mono/colour) rather than by number alone is
        // what makes this correct instead of merely convenient: ZWO's
        // ASI294MC is an IMX294 while the ASI294MM is an IMX492, so one
        // product number denotes different silicon on each side of the
        // split, and no rule over the digits could resolve both.
        const std::string numbers[2] = { trailing_number(kv.first),
                                         digits_of(kv.second.sensor) };
        for (const std::string& n : numbers) {
            if (n.empty()) continue;
            const std::string ik = sensor_index_key(n, mono);
            auto slot = sensor_index_.find(ik);
            if (slot == sensor_index_.end()) sensor_index_.emplace(ik, kv.first);
            // Deterministic on ties, matching the contained-key tier above.
            else if (kv.first < slot->second) slot->second = kv.first;
        }
    }
}

std::string QEDatabase::sensor_index_key(const std::string& number, bool mono) {
    return number + (mono ? "|m" : "|c");
}

bool QEDatabase::has_camera(const std::string& name) const {
    return cameras_.find(normalize_camera_key(name)) != cameras_.end();
}

bool QEDatabase::has_filter(const std::string& name) const {
    return filters_.find(name) != filters_.end();
}

QEConfidence QEDatabase::confidence(const std::string& camera) const {
    auto it = cameras_.find(normalize_camera_key(camera));
    if (it == cameras_.end()) return QEConfidence::UNKNOWN;
    return it->second.confidence;
}

double QEDatabase::lookup_camera_qe(const std::string& camera,
                                    double             wavelength_nm,
                                    Photosite          photosite) const {
    auto it = cameras_.find(normalize_camera_key(camera));
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
