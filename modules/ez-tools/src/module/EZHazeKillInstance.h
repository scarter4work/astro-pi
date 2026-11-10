// EZTools -- EZHazeKill process instance.
// Copyright and licence: see src/EZToolsLicense.h.

#ifndef __EZHazeKillInstance_h
#define __EZHazeKillInstance_h

#include <pcl/ImageVariant.h>
#include <pcl/MetaParameter.h>
#include <pcl/ProcessImplementation.h>

#include "hazekill/HazeKill.h"

namespace pcl
{

class EZHazeKillInstance : public ProcessImplementation
{
public:
   EZHazeKillInstance( const MetaProcess* );
   EZHazeKillInstance( const EZHazeKillInstance& );

   void Assign( const ProcessImplementation& ) override;
   bool CanExecuteOn( const View&, String& whyNot ) const override;
   bool ExecuteOn( View& ) override;
   void* LockParameter( const MetaParameter*, size_type tableRow ) override;

   eztools::hazekill::Settings Settings() const;

   // The sky of the whole image (nominal channels only; alpha is ignored), measured at its own
   // resolution. Integer images are read as [0,1] doubles. Never throws: a refusal is ok=false.
   static eztools::hazekill::SkyEstimate MeasureImage( const ImageVariant& );

   // MeasureImage + BuildModel. False (with the reason in error) for a refused image.
   static bool ComputeModel( const ImageVariant&, const eztools::hazekill::Settings&, eztools::hazekill::Model&,
                             String& error );

   // Applies the model in place to the nominal channels, in the image's own sample type (integer
   // images are computed in double and rounded on write).
   static void ApplyModel( ImageVariant&, const eztools::hazekill::Model& );

   // Writes Report(model) to the console; the stretched-image note as a note line.
   static void WriteReport( const eztools::hazekill::Model& );

private:
   double p_amount;
   double p_neutralize;
   double p_targetLevel;

   friend class EZHazeKillInterface;
};

} // namespace pcl

#endif
