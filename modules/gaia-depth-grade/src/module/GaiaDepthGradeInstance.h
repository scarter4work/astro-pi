// A GaiaDepthGrade instance (plan Task 16): the parameters and the headless ExecuteOn (Prepare,
// render, output in one call), which process icons, ImageContainer batches, scripts and the
// interface's Apply run.
#ifndef __GaiaDepthGradeInstance_h
#define __GaiaDepthGradeInstance_h

#include "PrepareSession.h"

#include <pcl/MetaParameter.h>
#include <pcl/ProcessImplementation.h>

namespace pcl
{

class GaiaDepthGradeInstance : public ProcessImplementation
{
public:

   GaiaDepthGradeInstance( const MetaProcess* );
   GaiaDepthGradeInstance( const GaiaDepthGradeInstance& );

   void Assign( const ProcessImplementation& ) override;
   bool IsHistoryUpdater( const View& ) const override;
   UndoFlags UndoMode( const View& ) const override;
   bool CanExecuteOn( const View&, String& whyNot ) const override;
   bool ExecuteOn( View& ) override;
   void* LockParameter( const MetaParameter*, size_type tableRow ) override;
   bool AllocateParameter( size_type sizeOrLength, const MetaParameter*, size_type tableRow ) override;
   size_type ParameterLength( const MetaParameter*, size_type tableRow ) const override;

   /// The run options these parameters describe.
   GradeOptions Options() const;
   /// The engine settings alone.
   gdg::GradeSettings Settings() const;
   /// Sets every engine parameter from `s` (the interface's controls).
   void SetSettings( const gdg::GradeSettings& s );
   /// Records a run's QA in the read-only outputs.
   void SetQA( const gdg::QA& qa );

   double   p_brightness, p_size, p_contrast, p_saturation, p_haloGain, p_haloSigma, p_pLow, p_pHigh;
   pcl_bool p_nebulaEnabled;
   double   p_nebulaStrength, p_nebulaAtmos, p_nebulaStructure;
   double   p_detectSigma, p_detectFWHM, p_matchTolerance, p_minMatchRate, p_neutralStrength;
   pcl_enum p_catalog;
   pcl_bool p_magLimitEnabled;
   double   p_magLimit, p_parallaxSNR;
   int32    p_haloMinRefs;
   double   p_haloRadiusSigma, p_haloMoffatBeta;
   pcl_enum p_splitMode;
   String   p_starsViewId, p_starlessViewId;
   pcl_enum p_outputMode;
   String   p_solveIndexFile;
   StringList p_gaiaDatabaseFiles;

   int32    o_nDetected = 0, o_nMatched = 0;
   double   o_matchRate = 0, o_medianOffset = 0;
   pcl_bool o_lowMatch = false;
};

/// The process' console log: info lines as "[GDG] ...", warnings as "[GDG] ** Warning: ...".
gdg::Log ConsoleLog();

} // namespace pcl

#endif // __GaiaDepthGradeInstance_h
