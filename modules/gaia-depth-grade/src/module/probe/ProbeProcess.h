// GaiaDepthGradeProbe: a TEST-ONLY process, compiled only with -DGDG_BUILD_PROBES=ON (never in
// the container / release build). It lets the in-PI probes (test/run-probe.sh) call the module's
// PCL adapters (PclSky, ImageBridge, LocalGaiaSource, OnlineGaiaSource) inside a real
// PixInsight and write their results as JSON:
//
//   var P = new GaiaDepthGradeProbe;
//   P.probeCase = "sky_adapter_convention"; P.probeArgs = "..."; P.outputPath = "/tmp/x.json";
//   P.executeOn( view ) or P.executeGlobal();
#ifndef __GDG_ProbeProcess_h
#define __GDG_ProbeProcess_h

#include <pcl/MetaParameter.h>
#include <pcl/MetaProcess.h>
#include <pcl/ProcessImplementation.h>

namespace pcl
{

class GDGProbeProcess : public MetaProcess
{
public:
   GDGProbeProcess();
   IsoString Id() const override;
   IsoString Categories() const override;
   uint32 Version() const override;
   String Description() const override;
   ProcessImplementation* Create() const override;
   ProcessImplementation* Clone( const ProcessImplementation& ) const override;
   bool CanProcessViews() const override;
   bool CanProcessGlobal() const override;
   bool NeedsValidation() const override { return false; }
};

class GDGProbeString : public MetaString
{
public:
   GDGProbeString( MetaProcess* p, const char* id ) : MetaString( p ), m_id( id ) {}
   IsoString Id() const override { return m_id; }
private:
   IsoString m_id;
};

class GDGProbeInstance : public ProcessImplementation
{
public:
   GDGProbeInstance( const MetaProcess* );
   GDGProbeInstance( const GDGProbeInstance& );
   void Assign( const ProcessImplementation& ) override;
   bool CanExecuteOn( const View&, String& whyNot ) const override;
   bool CanExecuteGlobal( String& whyNot ) const override;
   bool ExecuteOn( View& ) override;
   bool ExecuteGlobal() override;
   void* LockParameter( const MetaParameter*, size_type tableRow ) override;
   bool AllocateParameter( size_type sizeOrLength, const MetaParameter*, size_type tableRow ) override;
   size_type ParameterLength( const MetaParameter*, size_type tableRow ) const override;

   String p_case;
   String p_args;
   String p_output;

private:
   bool Finish( const IsoString& json );
};

// Probe cases (probe/Probe*.cpp). Each returns a JSON object (without the outer "ok"/"case"
// fields, which the instance adds) or throws.
IsoString ProbeSkyAdapterConvention( View& view, const String& args );
IsoString ProbeLocalGaia( const String& args );
IsoString ProbeOnline( const String& args );

extern GDGProbeProcess* TheGDGProbeProcess;

} // namespace pcl

#endif // __GDG_ProbeProcess_h
