// AutoContrast core (PCL-free): palette-class derivation and the chroma-family gate — fingerprint/palette.py.
#pragma once

#include <set>
#include <string>
#include <vector>

namespace ac {

// §4.3 palette classes.
extern const std::set<std::string> kPaletteClasses;

// Palette class from the FITS FILTER keyword(s): empty -> "RGB" (one-shot colour), any unrecognised name ->
// "unknown". Names are str.strip()ped and lower()ed as Python does (ASCII case mapping; the Unicode
// whitespace Python strips is stripped too).
std::string palette_class_from_filters(const std::vector<std::string>& filters);

// Palette class from a gallery's published bands (upper()-cased; empty -> "unknown").
std::string palette_class_from_gallery_bands(const std::vector<std::string>& bands);

// §2.3: two palettes may share chroma guidance iff both belong to the same family (broadband / sho / hoo).
// "L-only" and "unknown" belong to no family, so they are never compatible, not even with themselves.
bool palette_chroma_compatible(const std::string& a, const std::string& b);

} // namespace ac
