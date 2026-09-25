// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ProcessCatalog_h
#define PICopilot_ProcessCatalog_h

#include <pcl/ProcessParameter.h>
#include <pcl/String.h>

#include <nlohmann/json.hpp>

namespace pcl
{

constexpr size_type PICopilotMaxProcessDescriptionChars = 2000;

// The compiled-in data/process-summaries.json, parsed once.
const nlohmann::json& CompiledProcessSummaries();

// Grounding for the model (exposed as tools in increment 4). Native
// introspection of the installed process registry (Process::AllProcesses,
// ProcessParameter), merged with the one-line compiled-in summaries.
// Root thread only. Exceptions other than "unknown process id" propagate.
nlohmann::json ListProcesses();
nlohmann::json DescribeProcess( const IsoString& id );

// The element list and default of an enumeration parameter -- the ONE source
// both describe_process and apply_process use, so they always agree.
//
// Native ProcessParameter::EnumerationElements() first. For some parameters
// the core cannot report the element identifiers to a foreign module
// ("GetParameterElementIdentifier(): API function error"; observed for SCNR
// colorToRemove and PixelMath newImageColorSpace/newImageSampleFormat), so
// the fallback splits the work: the element COUNT and VALUES still come from
// the core natively (raw API table; only the identifier call is broken), and
// the core JavaScript runtime (PJSR) maps each of those values to its id --
// the value is assigned to the parameter of a fresh instance and the element
// id the core itself writes for it in toSource() is recorded. The default id
// is read from a default instance's toSource(). Only native element values
// are ever assigned: an arbitrary number can crash the core in toSource().
// Resolved per (process, parameter) once and cached. Root thread only.
// Throws pcl::Error naming the parameter when neither route works.
struct EnumerationInfo
{
   ProcessParameter::enumeration_element_list elements;   // id + value (+ aliases, native only)
   IsoString                                  defaultId;  // empty if the core reports none
   bool                                       viaScript = false;
};

const EnumerationInfo& EnumerationInfoOf( const ProcessParameter& p );

// describe_process's "default" for an enumeration. ProcessParameter::
// DefaultValue() is the default element's INDEX, never a value the model can
// pass back: info.defaultId when known, else the id of element number
// `defaultIndex`, else null (unresolvable: no default is emitted, and the
// parameter says so in "defaultNote").
nlohmann::json EnumerationDefaultJson( const EnumerationInfo& info, int defaultIndex );

} // namespace pcl

#endif // PICopilot_ProcessCatalog_h
