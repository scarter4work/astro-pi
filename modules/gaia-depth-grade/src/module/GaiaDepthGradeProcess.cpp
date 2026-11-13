#include "GaiaDepthGradeProcess.h"

#include "GaiaDepthGradeInstance.h"
#include "GaiaDepthGradeParameters.h"

#include <pcl/ErrorHandler.h>

namespace pcl
{

GaiaDepthGradeProcess* TheGaiaDepthGradeProcess = nullptr;

GaiaDepthGradeProcess::GaiaDepthGradeProcess()
{
   TheGaiaDepthGradeProcess = this;
   CreateGaiaDepthGradeParameters( this );
}

IsoString GaiaDepthGradeProcess::Id() const
{
   return "GaiaDepthGrade";
}

IsoString GaiaDepthGradeProcess::Categories() const
{
   return "scarter4work";
}

uint32 GaiaDepthGradeProcess::Version() const
{
   return 0x100;
}

String GaiaDepthGradeProcess::Description() const
{
   return String::UTF8ToUTF16(
      "<html>"
      "<p><b>GaiaDepthGrade</b> grades the stars of an image by their measured Gaia distances, and optionally "
      "the nebula by a structure-driven, Gaia-calibrated depth. Physically motivated, not per-pixel correct "
      "for gas.</p>"
      "<p>Executed on an image, it works on a duplicate (<i>id</i>_gdgwork; the image itself is never "
      "written unless <i>outputMode</i> is ReplaceTarget): it plate-solves the duplicate from its metadata "
      "(or blindly, with an astrometric index selected in the Gaia process preferences), splits it into stars "
      "and starless layers with StarXTerminator (or uses the two views named by <i>starsViewId</i> and "
      "<i>starlessViewId</i> when <i>splitMode</i> is SuppliedViews), detects the stars, matches them to Gaia "
      "DR3 and grades each by its distance: brightness, size with a star-shine halo, local contrast and "
      "saturation. With <i>nebulaEnabled</i>, the starless layer is graded too. The layers are recombined by a "
      "screen blend into a new 32-bit window <i>id</i>_depthgraded, tagged DEPTHTAG.</p>"
      "<p><i>catalog</i> Offline (the default) uses the local Gaia DR3 database PixInsight's Gaia process is "
      "configured with (parallax distances; no network connection). Online uses the Bailer-Jones distances "
      "of the ESA Gaia archive (https://gea.esac.esa.int/, cached per field); a machine without a local "
      "database then also plate-solves against the archive.</p>"
      "<p>The QA of the last run is in the read-only parameters outNDetected, outNMatched, outMatchRate, "
      "outMedianOffset (-1 when nothing matched) and outLowMatch (match rate below <i>minMatchRate</i>: the "
      "grade is unreliable).</p>"
      "</html>" );
}

ProcessInterface* GaiaDepthGradeProcess::DefaultInterface() const
{
   return nullptr; // the dialog arrives with GaiaDepthGradeInterface (plan Task 17)
}

ProcessImplementation* GaiaDepthGradeProcess::Create() const
{
   return new GaiaDepthGradeInstance( this );
}

ProcessImplementation* GaiaDepthGradeProcess::Clone( const ProcessImplementation& p ) const
{
   const GaiaDepthGradeInstance* instance = dynamic_cast<const GaiaDepthGradeInstance*>( &p );
   if ( instance == nullptr )
      throw Error( "GaiaDepthGrade: internal error - cannot clone a foreign instance" );
   return new GaiaDepthGradeInstance( *instance );
}

bool GaiaDepthGradeProcess::CanProcessViews() const
{
   return true;
}

bool GaiaDepthGradeProcess::CanProcessGlobal() const
{
   return false;
}

} // namespace pcl
