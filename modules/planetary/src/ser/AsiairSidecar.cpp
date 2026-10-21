#include "AsiairSidecar.h"

#include <cctype>

namespace planetary::ser {

namespace {

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool AllDigits(const std::string& s, size_t pos, size_t n) {
    if (pos + n > s.size()) return false;
    for (size_t i = pos; i < pos + n; ++i)
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    return true;
}

int Num(const std::string& s, size_t pos, size_t n) { return std::stoi(s.substr(pos, n)); }

}  // namespace

std::optional<AsiairSidecar> ParseAsiairSidecar(const std::string& text,
                                                std::vector<std::string>& warnings,
                                                const std::string& name) {
    AsiairSidecar sc;
    const std::string body = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? text.substr(3) : text;
    bool sawSection = false, sawKey = false;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();
        const std::string line = Trim(body.substr(pos, nl - pos));  // also drops a trailing CR
        pos = nl + 1;
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            if (!sawSection) sc.camera = Trim(line.substr(1, line.size() - 2));
            sawSection = true;
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        sawKey = true;
        const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
        if (key == "Colour Format") sc.colourFormat = value;
        else if (key == "Bayer") sc.bayer = value;
        else if (key == "Flip") sc.flip = value;
        else if (key == "Gain") sc.gain = value;
        else if (key == "Exposure") sc.exposure = value;
        else if (key == "Capture Area Size") sc.captureArea = value;
    }
    if (!sawSection && !sawKey) {
        warnings.push_back("sidecar " + name + " could not be read; ignored");
        return std::nullopt;
    }
    return sc;
}

ColorId ResolveBayer(BayerChoice choice, const std::optional<AsiairSidecar>& sidecar,
                     std::string& source, std::vector<std::string>& warnings,
                     const std::string& sidecarName) {
    switch (choice) {
        case BayerChoice::None: source = "chosen"; return ColorId::Mono;
        case BayerChoice::RGGB: source = "chosen"; return ColorId::BayerRGGB;
        case BayerChoice::GRBG: source = "chosen"; return ColorId::BayerGRBG;
        case BayerChoice::GBRG: source = "chosen"; return ColorId::BayerGBRG;
        case BayerChoice::BGGR: source = "chosen"; return ColorId::BayerBGGR;
        case BayerChoice::Auto: break;
    }

    const char* names[] = {"RG", "GR", "GB", "BG"};
    const ColorId ids[] = {ColorId::BayerRGGB, ColorId::BayerGRBG, ColorId::BayerGBRG,
                           ColorId::BayerBGGR};
    const char* patterns[] = {"RGGB", "GRBG", "GBRG", "BGGR"};
    int found = -1;
    if (sidecar && !sidecar->bayer.empty()) {
        for (int i = 0; i < 4; ++i)
            if (sidecar->bayer == names[i]) found = i;
        if (found < 0) {
            warnings.push_back("sidecar Bayer value " + sidecar->bayer + " is not recognised" +
                               kRawCfaAsMonoTail);
            source = "none";
            return ColorId::Mono;
        }
    }
    if (found < 0) {
        source = "none";
        if (!sidecar)
            warnings.push_back(std::string("no Bayer pattern known (no .avi.txt sidecar)") + kRawCfaAsMonoTail);
        else
            warnings.push_back((sidecarName.empty() ? std::string("the sidecar")
                                                    : "sidecar " + sidecarName) +
                               " has no Bayer value" + kRawCfaAsMonoTail);
        return ColorId::Mono;
    }

    if (sidecar->colourFormat != "RAW8" && !sidecar->colourFormat.empty())
        warnings.push_back("sidecar Colour Format is " + sidecar->colourFormat +
                           ", not RAW8; using its Bayer value anyway");

    if (sidecar->flip.rfind("Vert", 0) == 0) {
        const int rotated = 3 - found;  // RGGB<->BGGR, GRBG<->GBRG
        source = std::string("sidecar ") + names[found] + ", Flip=Vert \xE2\x86\x92 " +
                 patterns[rotated];
        // An inference, not a fact (Ruling 10): of the 40 Flip=Vert planet files measured (33
        // captures), 35 carry the rotated phase, 3 keep the sidecar's own and 2 are unmeasurable;
        // nothing found in the headers or sidecars tells them apart.
        warnings.push_back(std::string("Bayer pattern inferred from Flip=Vert (") + patterns[rotated] +
                           "); some ASIAIR captures are " + patterns[found] +
                           " -- if red and blue look swapped, choose " + patterns[found] +
                           " under Bayer pattern");
        return ids[rotated];
    }
    source = "sidecar";
    return ids[found];
}

std::optional<std::string> FilenameStart(const std::string& fileName) {
    // YYYY-MM-DD-HHMMSS-
    if (!AllDigits(fileName, 0, 4) || fileName.size() < 18 || fileName[4] != '-' ||
        !AllDigits(fileName, 5, 2) || fileName[7] != '-' || !AllDigits(fileName, 8, 2) ||
        fileName[10] != '-' || !AllDigits(fileName, 11, 6) || fileName[17] != '-')
        return std::nullopt;
    const int mo = Num(fileName, 5, 2), d = Num(fileName, 8, 2);
    const int h = Num(fileName, 11, 2), mi = Num(fileName, 13, 2), s = Num(fileName, 15, 2);
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59) return std::nullopt;
    return fileName.substr(0, 10) + " " + fileName.substr(11, 2) + ":" + fileName.substr(13, 2) +
           ":" + fileName.substr(15, 2);
}

bool ParseCaptureArea(const std::string& text, int& w, int& h) {
    size_t i = 0;
    auto skip = [&] { while (i < text.size() && text[i] == ' ') ++i; };
    auto num = [&](int& out) {
        skip();
        const size_t b = i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
        if (i == b || i - b > 9) return false;
        out = std::stoi(text.substr(b, i - b));
        return true;
    };
    if (!num(w)) return false;
    skip();
    if (i >= text.size() || text[i++] != '*') return false;
    if (!num(h)) return false;
    skip();
    return i == text.size();
}

}  // namespace planetary::ser
