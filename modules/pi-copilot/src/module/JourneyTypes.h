// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_JourneyTypes_h
#define PICopilot_JourneyTypes_h

#include "StepStats.h"   // ChannelStats

#include <pcl/Defs.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace pcl
{

// ASCII lower-case copy (byte-wise; UTF-8 multibyte bytes pass through
// unchanged). The one shared helper: JourneyStore (T5), MasterFacts (T6) and
// JourneyTools (T10) use it; none keeps a local copy (P22).
inline std::string AsciiLower( std::string s )
{
   std::transform( s.begin(), s.end(), s.begin(), []( unsigned char c ) { return char( std::tolower( c ) ); } );
   return s;
}

// Acquisition facts of a master (spec §5 table acquisition). Never holds a
// redacted keyword's value (they are not read at all; Task 6).
struct AcquisitionFacts
{
   std::string           target;
   std::string           filter;
   std::string           camera;
   std::optional<double> gain;
   std::optional<double> offset;
   std::optional<double> sensorTempC;
   std::optional<double> subExposureS;
   std::optional<int>    subCount;
   std::optional<double> totalIntegrationS;
   std::string           sessionDate;   // YYYY-MM-DD or ""
};

struct JourneyRow
{
   int64       id = 0;
   std::string created, updated, name, target;
   bool        kept = false;
   std::string keptAt;
   int64       endImageId = 0;
   std::string status;                  // "recording" | "ended" (Ruling 24)
   int64       continuesJourneyId = 0;  // a "(continued)" journey: the kept journey it continues (Ruling 26); 0 = none
};

struct ImageRow
{
   int64       id = 0;
   int64       journeyId = 0;
   std::string viewId, filePath, fingerprint;
   bool        isMaster = false;
   std::string created;
   std::string owner;                   // "<pid>:<process start>" of the PixInsight instance recording it; "" = none (m6)
};

struct StepRow
{
   int64          id = 0;
   int64          imageId = 0;
   int            seq = 0;
   std::string    processId;
   nlohmann::json params = nlohmann::json::object();   // params_json (see JourneyStore.h; "base": true = pre-join history, T7)
   std::string    started;
   double         durationS = -1;
   std::string    actor = "user";       // "user" | "copilot"
   std::string    reason;
   bool           reasonInferred = false;
   std::string    state = "active";     // "active" | "undone" | "superseded"
   int            historyIndex = 0;     // the view's active step count when recorded
};

struct LinkRow
{
   int64       fromImageId = 0;
   int64       toImageId = 0;
   int64       viaStepId = 0;           // 0 = none
   std::string evidence;                // "copilot" | "timing" | "reference"
};

struct GapRow
{
   int64       journeyId = 0;
   int64       imageId = 0;
   int         afterSeq = 0;
   std::string reason;
};

} // namespace pcl

#endif // PICopilot_JourneyTypes_h
