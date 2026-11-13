// TEST-ONLY (GDG_BUILD_PROBES; never in the container/release build): the GaiaDepthGrade
// interface's state file for the GUI smoke (test/gui-smoke.sh, gui_drive.py). Every control's
// value, text, range, enabled/visible state and screen rectangle as the controls themselves report
// them, the status history and the outcome of every Prepare/Preview/Execute, written as JSON to
// $GDG_GUI_STATE on every change and twice a second. Members of GaiaDepthGradeInterface, kept out
// of the shipped translation unit.
#include "../GaiaDepthGradeInterface.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace pcl
{

namespace
{

std::string J( const String& s )
{
   std::string out = "\"";
   for ( char c : std::string( s.ToUTF8().c_str() ) )
   {
      if ( c == '"' || c == '\\' )
         out += '\\', out += c;
      else if ( c == '\n' )
         out += "\\n";
      else if ( static_cast<unsigned char>( c ) < 0x20 )
         out += ' ';
      else
         out += c;
   }
   return out + "\"";
}

std::string Num( double v )
{
   char b[64];
   std::snprintf( b, sizeof( b ), "%.10g", v );
   return b;
}

std::string Rect( const Control& c )
{
   const pcl::Point p = c.LocalToGlobal( pcl::Point( 0, 0 ) );
   return "[" + std::to_string( p.x ) + "," + std::to_string( p.y ) + "," + std::to_string( c.Width() ) + "," +
          std::to_string( c.Height() ) + "]";
}

/// IsEnabled() is Qt's effective state: false for a child of a disabled control.
std::string Geometry( const Control& c )
{
   return "\"rect\":" + Rect( c ) + ",\"enabled\":" + (c.IsEnabled() ? "true" : "false") +
          ",\"visible\":" + (c.IsVisible() ? "true" : "false");
}

std::string ValueFields( const NumericEdit& c )
{
   return "\"value\":" + Num( c.Value() ) + ",\"label\":" + J( c.label.Text() ) + ",\"text\":" + J( c.edit.Text() ) +
          ",\"range\":[" + Num( c.LowerBound() ) + "," + Num( c.UpperBound() ) + "]," + Geometry( c ) +
          ",\"edit\":" + Rect( c.edit );
}

std::string Value( const char* name, const NumericEdit& c )
{
   return std::string( "\"" ) + name + "\":{" + ValueFields( c ) + "}";
}

std::string Value( const char* name, const NumericControl& c )
{
   return std::string( "\"" ) + name + "\":{" + ValueFields( c ) + ",\"slider\":" + Rect( c.slider ) + "}";
}

std::string Check( const char* name, const CheckBox& c )
{
   return std::string( "\"" ) + name + "\":{\"checked\":" + (c.IsChecked() ? "true" : "false") + ",\"text\":" + J( c.Text() ) +
          ",\"toolTip\":" + J( c.ToolTip() ) + "," + Geometry( c ) + "}";
}

std::string Button_( const char* name, const PushButton& c )
{
   return std::string( "\"" ) + name + "\":{\"text\":" + J( c.Text() ) + ",\"toolTip\":" + J( c.ToolTip() ) + "," +
          Geometry( c ) + "}";
}

std::string Plain( const char* name, const Control& c )
{
   return std::string( "\"" ) + name + "\":{" + Geometry( c ) + "}";
}

std::string Bool( bool b )
{
   return b ? "true" : "false";
}

uint64 BitmapSum( const Bitmap& b )
{
   if ( b.IsNull() )
      return 0;
   uint64 s = 0;
   for ( int y = 0; y < b.Height(); ++y )
   {
      const RGBA* row = b.ScanLine( y );
      for ( int x = 0; x < b.Width(); ++x )
         s += (row[x] & 0xffffff) * uint64( 1 + (x + 7*y) % 13 );
   }
   return s;
}

std::string BitmapInfo( const Bitmap& b )
{
   return "[" + std::to_string( b.IsNull() ? 0 : b.Width() ) + "," + std::to_string( b.IsNull() ? 0 : b.Height() ) + "," +
          std::to_string( BitmapSum( b ) ) + "]";
}

} // namespace

bool GaiaDepthGradeInterface::SessionMatchesSettings() const
{
   return !m_prepared || (m_session && !PrepareOptionsDiffer( m_session->options, instance.Options() ));
}

void GaiaDepthGradeInterface::RecordAction( const char* action, bool ok )
{
   m_actionLog << String( action ) + (ok ? " ok" : " error");
   WriteState();
}

void GaiaDepthGradeInterface::WriteState() const
{
   const char* path = std::getenv( "GDG_GUI_STATE" );
   if ( path == nullptr || *path == '\0' || GUI == nullptr )
      return;
   std::string j = "{";
   j += "\"stamp\":" + Num( std::chrono::duration<double>( std::chrono::system_clock::now().time_since_epoch() ).count() );
   j += ",\"target\":" + J( String( m_targetId ) );
   const View listed = GUI->Target_ViewList.CurrentView();
   j += ",\"targetList\":" + J( listed.IsNull() ? String() : String( listed.Id() ) );
   j += ",\"prepared\":" + Bool( m_prepared );
   j += ",\"sessionMatchesSettings\":" + Bool( SessionMatchesSettings() );
   j += ",\"previewStale\":" + Bool( m_previewStale );
   j += ",\"busy\":" + Bool( m_busy );
   j += ",\"status\":" + J( GUI->Status_Label.Text() );
   j += ",\"qa\":" + J( GUI->QA_Label.Text() );
   j += ",\"statusHistory\":[";
   for ( size_type i = m_statusLog.Length() > 80 ? m_statusLog.Length() - 80 : 0, n = 0; i < m_statusLog.Length(); ++i, ++n )
      j += (n ? "," : "") + J( m_statusLog[i] );
   j += "]";
   j += ",\"statusCount\":" + std::to_string( m_statusLog.Length() );
   j += ",\"actions\":[";
   for ( size_type i = 0; i < m_actionLog.Length(); ++i )
      j += (i ? "," : "") + J( m_actionLog[i] );
   j += "]";
   j += ",\"windowTitle\":" + J( WindowTitle() );
   j += ",\"inset\":[" + std::to_string( m_insetCx ) + "," + std::to_string( m_insetCy ) + "]";
   j += ",\"previewBitmap\":" + BitmapInfo( m_previewBmp );
   j += ",\"insetBitmap\":" + BitmapInfo( m_insetBmp );
   if ( m_session )
   {
      const gdg::InsetRegion r = gdg::inset_region( m_session->width, m_session->height, m_insetCx, m_insetCy, kInsetSize );
      j += ",\"insetRegion\":[" + std::to_string( r.x ) + "," + std::to_string( r.y ) + "," + std::to_string( r.w ) + "," +
           std::to_string( r.h ) + "]";
      j += ",\"session\":{\"width\":" + std::to_string( m_session->width ) + ",\"height\":" + std::to_string( m_session->height ) +
           ",\"nDetected\":" + std::to_string( m_session->prep.qa.n_detected ) + ",\"nMatched\":" +
           std::to_string( m_session->prep.qa.n_matched ) + "}";
   }
   j += ",\"labels\":{\"live\":" + J( GUI->Live_Label.Text() ) + ",\"prep\":" + J( GUI->Prep_Label.Text() ) +
        ",\"inset\":" + J( GUI->Inset_Label.Text() ) + ",\"target\":" + J( GUI->Target_Label.Text() ) + "}";
   j += ",\"controls\":{";
   j += Value( "brightness", GUI->Brightness_NumericControl ) + "," + Value( "size", GUI->Size_NumericControl ) + "," +
        Value( "contrast", GUI->Contrast_NumericControl ) + "," + Value( "saturation", GUI->Saturation_NumericControl ) + "," +
        Value( "haloSigma", GUI->HaloSigma_NumericControl ) + "," + Value( "pLow", GUI->PLow_NumericControl ) + "," +
        Value( "pHigh", GUI->PHigh_NumericControl ) + "," + Check( "nebula", GUI->Nebula_CheckBox ) + "," +
        Value( "nebulaStrength", GUI->NebulaStrength_NumericControl ) + "," +
        Value( "nebulaAtmos", GUI->NebulaAtmos_NumericControl ) + "," +
        Value( "nebulaStructure", GUI->NebulaStructure_NumericControl ) + "," +
        Value( "detect", GUI->Detect_NumericEdit ) + "," + Value( "match", GUI->Match_NumericEdit ) + "," +
        Check( "offline", GUI->Offline_CheckBox ) + "," + Value( "haloGain", GUI->HaloGain_NumericControl ) + "," +
        Value( "detectFWHM", GUI->DetectFWHM_NumericEdit ) + "," + Value( "minMatchRate", GUI->MinMatchRate_NumericEdit ) + "," +
        Value( "neutral", GUI->Neutral_NumericEdit ) + "," + Check( "magLimitEnabled", GUI->MagLimit_CheckBox ) + "," +
        Value( "magLimit", GUI->MagLimit_NumericEdit ) + "," + Value( "parallaxSNR", GUI->ParallaxSNR_NumericEdit ) + "," +
        "\"haloMinRefs\":{\"value\":" + std::to_string( GUI->HaloMinRefs_SpinBox.Value() ) + "," +
        Geometry( GUI->HaloMinRefs_SpinBox ) + "}," + Value( "haloRadiusSigma", GUI->HaloRadius_NumericEdit ) + "," +
        Value( "haloMoffatBeta", GUI->HaloBeta_NumericEdit ) + "," +
        "\"splitMode\":{\"item\":" + std::to_string( GUI->Split_ComboBox.CurrentItem() ) + "," + Geometry( GUI->Split_ComboBox ) +
        "}," + "\"outputMode\":{\"item\":" + std::to_string( GUI->Output_ComboBox.CurrentItem() ) + "," +
        Geometry( GUI->Output_ComboBox ) + "}," + "\"advanced\":{\"expanded\":" +
        Bool( GUI->Advanced_Control.IsVisible() ) + "," + Geometry( GUI->Advanced_SectionBar ) + "}," +
        Plain( "advancedPanel", GUI->Advanced_Control ) + "," +
        Button_( "prepare", GUI->Prepare_PushButton ) + "," + Button_( "preview", GUI->Preview_PushButton ) + "," +
        Button_( "execute", GUI->Execute_PushButton ) + "," + Button_( "close", GUI->Close_PushButton ) + "," +
        Button_( "browse", GUI->Browse_PushButton ) + "," + Plain( "previewControl", GUI->Preview_Control ) + "," +
        Plain( "insetControl", GUI->Inset_Control ) + "," + Plain( "targetList", GUI->Target_ViewList ) + "," +
        Plain( "status", GUI->Status_Label ) + "," + Plain( "qaLabel", GUI->QA_Label );
   j += "}";
   j += ",\"interfaceVisible\":" + Bool( IsVisible() );
   j += "}\n";
   const std::string tmp = std::string( path ) + ".tmp";
   {
      std::ofstream f( tmp, std::ios::binary | std::ios::trunc );
      f << j;
   }
   std::rename( tmp.c_str(), path );
}

void GaiaDepthGradeInterface::e_Timer( Timer& )
{
   WriteState();
}

} // namespace pcl
