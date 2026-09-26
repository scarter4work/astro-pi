// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_MasterFacts_h
#define PICopilot_MasterFacts_h

#include "HistoryReader.h"
#include "JourneyTypes.h"

#include <pcl/FITSHeaderKeyword.h>
#include <pcl/String.h>

#include <string>
#include <vector>

namespace pcl
{

struct MasterEvidence
{
   bool        isMaster = false;
   std::string why;   // e.g. "keyword IMAGETYP='Master Light'", "history begins with ImageIntegration"
};

bool IsIntegrationProcess( const std::string& processId );   // ImageIntegration, DrizzleIntegration, FastIntegration

// Ruling 1, rules 1-4 in order (rule 5, Copilot-created, is the tracker's).
MasterEvidence DetectMaster( const std::vector<std::string>& historyProcessIds, const FITSKeywordArray& keywords );

// Ruling 29 (P35). True when this window is an AUXILIARY output of an
// integration run (rejection low/high map, slope map, drizzle weights): its
// first non-Script step is an integration process whose recorded
// integrationImageId is non-empty and names another window. Always false when
// Task 1 measured PICopilotJourneyIntegrationIdInHistory == false (the README
// then documents that such maps become their own journeys). Task 7 ignores
// auxiliary windows: never a master, never linked.
bool IsIntegrationAuxiliary( const std::string& viewId, const std::vector<HistoryStep>& history );

// A keyword's value, delimiters stripped, trimmed, Latin-1 -> UTF-8. "" when
// absent or when the name is a redacted keyword (they are never read).
std::string KeywordText( const FITSKeywordArray& keywords, const char* name );

// Acquisition facts of a master (spec §5; mapping in the implementation).
AcquisitionFacts ExtractAcquisition( const FITSKeywordArray& keywords, const std::vector<HistoryStep>& history,
                                     const String& filePath, const std::string& viewId );

// "<W>x<H>x<C>:<f|i><bits>:" + 16 hex FNV-1a-64 over the base history
// identities and the STABLE keywords (IMAGETYP, OBJECT, FILTER, INSTRUME,
// TELESCOP, EXPTIME, EXPOSURE, DATE-OBS, NCOMBINE, STACKCNT, XBINNING and
// HISTORY lines starting "ImageIntegration."). Redacted keywords never enter.
// Geometry and sample format live ONLY in the prefix: a 1-channel and a
// 3-channel master with the same history/keywords share the same hash suffix.
// A fingerprint is therefore always compared as the WHOLE string (as
// JourneyStore::FindResumableByFingerprint does), never by its hash part.
std::string MasterFingerprint( int width, int height, int channels, int bitsPerSample, bool floatSample,
                               const std::vector<std::string>& baseIdentities, const FITSKeywordArray& keywords );

// Ruling 10.
std::string DeriveTarget( const FITSKeywordArray& keywords, const String& filePath, const std::string& viewId );
std::string DeriveJourneyName( const std::string& target, const std::string& filter, int masterCount, const std::string& dateIso );
std::string StripKind( const std::string& filter, int masterCount );
std::string SafeFolderName( const std::string& name );

} // namespace pcl

#endif // PICopilot_MasterFacts_h
