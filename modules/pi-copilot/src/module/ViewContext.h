// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ViewContext_h
#define PICopilot_ViewContext_h

#include <pcl/View.h>

#include <nlohmann/json.hpp>

#include <string>

namespace pcl
{

// FITS keyword caps for the context sent to the model (token cost).
constexpr int PICopilotMaxFitsKeywords   = 60;
constexpr int PICopilotMaxFitsValueChars = 80;

// FITS keywords never sent to the model (privacy): observing-site location
// and observer identity. These exact names, any case -- and, fail-closed,
// every keyword whose name (any case, HIERARCH/long names included) names a
// place or a person (IsRedactedFitsKeyword).
// Redacted keywords do not consume a PICopilotMaxFitsKeywords slot; they are
// counted in the context's "fitsKeywordsRedacted" (NOT in
// "fitsKeywordsOmitted", which counts only cap overflow).
constexpr const char* PICopilotRedactedFitsKeywords[] =
{
   "SITELAT", "SITELONG", "SITEELEV",
   "OBSGEO-B", "OBSGEO-L", "OBSGEO-H",
   "LAT-OBS", "LONG-OBS", "ALT-OBS",
   "OBSERVER"
};

// True iff the keyword may hold a location or an identity: a name in
// PICopilotRedactedFitsKeywords (any case), a name containing SITE (not in
// COMPOSITE/POSITION), OBSGEO, GEOLAT/GEOLON/GEOELEV/GEOALT, GEODETIC,
// LATITUD, LONGITUD, ELEVATIO, GPS, OBSERVER, OBSERVAT, LOCATION, ADDRESS,
// AUTHOR, EMAIL or TELEPHON (any case: "Observation:Location:Latitude"), or
// -- for FITS-style names without lowercase letters -- a separate token LAT,
// LON, LONG, LNG, ELEV, ALT, CITY, COUNTRY, TOWN, PLACE, OWNER, USER,
// USERNAME, CONTACT, OBSNAME, PI, PINAME, OPERATOR or ORIGIN (tokens split at
// anything that is not a letter or digit: "LAT-OBS", "HIERARCH ESO TEL
// GEOLAT", "GPS LONG"). Object coordinates and altitude ("OBJCTRA",
// "OBJCTALT", "CENTALT") are kept.
bool IsRedactedFitsKeyword( const IsoString& name );

// COMMENT, HISTORY and blank-name keywords: free text, never sent.
bool IsCommentaryFitsKeyword( const IsoString& name );

// THE ALLOWLIST (security review of 0.2.2.0: a blocklist keeps losing): the
// keywords whose VALUES may reach the model, because processing needs them --
// what NukeX, PixInsight's own processes (WBPP, Debayer, ImageSolver,
// SPCC/PCC, astrometry) and PI Copilot's journey facts read, and what
// NINA/SGP/SharpCap/ASIAIR write for them:
//  image structure  SIMPLE BITPIX NAXIS NAXISn EXTEND BZERO BSCALE IMAGETYP FRAME
//                   FRAMETYP BAYERPAT XBAYROFF YBAYROFF PEDESTAL CALSTAT ROWORDER
//                   IMAGEW IMAGEH XORGSUBF YORGSUBF
//  target, filter   OBJECT FILTER
//  exposure         EXPTIME EXPOSURE LIVETIME DATE-OBS DATE-AVG DATE-END DATE-LOC
//                   MJD-OBS MJD-AVG
//  camera           GAIN EGAIN OFFSET CCD-TEMP SET-TEMP XBINNING YBINNING CCDXBIN
//                   CCDYBIN XPIXSZ YPIXSZ PIXSIZE INSTRUME READOUTM READNOIS
//                   RDNOISE
//  optics           TELESCOP FOCALLEN APERTURE APTDIA FOCRATIO FOCUSPOS FOCPOS
//  pointing         RA DEC OBJCTRA OBJCTDEC (never ALT/AZ, hour angle, LST or
//                   airmass: with the time they locate the observer)
//  astrometry       EQUINOX EPOCH RADESYS RADECSYS CTYPEn CRVALn CRPIXn CDELTn
//                   CROTAn CUNITn CDi_j PCi_j PIXSCALE SCALE PLTSOLVD and the SIP
//                   coefficients A_/B_/AP_/BP_ (ORDER, i_j)
//  integration      NCOMBINE STACKCNT
// Every other keyword reaches the model by NAME only (no value), and only
// when IsRedactedFitsKeyword() passes it. Allowlisted values still go
// through FitsValueWithoutPaths() (second layer). Any case.
bool IsFitsValueAllowed( const IsoString& name );

// Text with every filesystem path reduced to its file name, FAIL-CLOSED
// (privacy: a path names the user's account and folders):
//  - an absolute path starts with '/', a double backslash (network share),
//    "~/" or "~\", or a drive ("X:\", "X:/"), at the start of the text or
//    after ANY character that is not a letter or digit; it runs to the next
//    path and is cut back to what follows its last '/' or '\'; a "file:"
//    scheme before it goes too;
//  - a relative path ("data/M31/raw", "M31/light.fits") is reduced the same way;
//  - what is kept must look like a file name with an extension: a folder name,
//    or a path cut short mid-name, becomes "[path]".
// Kept: "1/2", "2026/10/13", "N/A", "Ha/OIII", "$T/2" and plain text. Pure.
std::string FitsValueWithoutPaths( const std::string& value );

// The same, line by line (console output, messages).
std::string TextWithoutFolders( const std::string& text );

// Every string (keys included) of a JSON value through TextWithoutFolders.
nlohmann::json JsonWithoutFolders( const nlohmann::json& v );

// Only the absolute-path pass (paths relative to a known tree, such as a
// script's place under src/scripts, are kept): line by line, and for every
// string of a JSON value.
std::string    TextWithoutAbsolutePaths( const std::string& text );
nlohmann::json JsonWithoutAbsolutePaths( const nlohmann::json& v );

// What the context carries for the image's file: the file name only
// (File::ExtractNameAndExtension), never the directory -- a local path
// usually contains the user's account name. Empty for an empty path.
String ViewContextFileName( const String& filePath );

/*
 * JSON description of a view for the model: identity (view ids and the
 * file NAME, see ViewContextFileName), geometry, robust per-channel
 * statistics of the REAL (usually linear) data, and the first
 * PICopilotMaxFitsKeywords FITS keywords that are not redacted
 * (PICopilotRedactedFitsKeywords), each value with its paths reduced to file
 * names (FitsValueWithoutPaths; counted in "fitsValuesPathsScrubbed"). There is deliberately no process
 * history: PCL exposes no API for it.
 *
 * Root thread only. Reads the image under AutoViewWriteLock (blocks writers,
 * readers unaffected) and never modifies it: statistics use explicit
 * (rect, channel, channel) arguments -- no SelectChannel() calls.
 * Throws pcl::Error for a null view, a view with no image, or a complex image.
 */
nlohmann::json BuildViewContext( const View& view );

} // namespace pcl

#endif // PICopilot_ViewContext_h
