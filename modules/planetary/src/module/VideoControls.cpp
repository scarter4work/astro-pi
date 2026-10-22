// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "VideoControls.h"

#include <pcl/Console.h>
#include <pcl/File.h>
#include <pcl/FileDialog.h>
#include <pcl/MessageBox.h>

#include "AsiairSidecar.h"
#include "PlanetaryVersion.h"
#include "SerFile.h"

namespace ser = planetary::ser;

namespace pcl
{

bool AskForVideo( const String& current, String& path )
{
   OpenFileDialog d;
   d.SetCaption( "Open planetary video" );
   d.SetFilters( OpenFileDialog::filter_list()
                 << FileFilter( "Planetary video", StringList() << ".ser" << ".avi" )
                 << FileFilter( "SER video", ".ser" )
                 << FileFilter( "AVI video", ".avi" ) );
   d.DisableMultipleSelections();
   const String trimmed = current.Trimmed();
   if ( !trimmed.IsEmpty() )
      d.SetInitialPath( File::ExtractDrive( trimmed ) + File::ExtractDirectory( trimmed ) );
   if ( !d.Execute() )
      return false;
   path = d.FileName();
   return true;
}

void SetUpBayerCombo( ComboBox& combo )
{
   combo.AddItem( "Auto" );
   combo.AddItem( "None (mono)" );
   combo.AddItem( "RGGB" );
   combo.AddItem( "GRBG" );
   combo.AddItem( "GBRG" );
   combo.AddItem( "BGGR" );
   combo.SetToolTip( "<p>AVI only (a SER file states its own layout). The CFA pattern of the AVI's raw "
                     "frames. Auto: the pattern in the ASIAIR sidecar (&lt;name&gt;.avi.txt); with no "
                     "pattern there, the raw CFA as mono and a warning. None (mono): the raw CFA as mono. "
                     "RGGB, GRBG, GBRG, BGGR: that pattern, overriding the sidecar.</p>" );
}

bool AviWithoutSidecar( const ser::FileInfo& info )
{
   return info.container != ser::Container::Ser && info.avi && info.avi->sidecarStatus == ser::SidecarStatus::Absent;
}

SavePatternResult SaveBayerPattern( const String& aviPath, ser::ColorId pattern, const char* process, String& failure )
{
   const String path = aviPath.Trimmed();
   const String sidecarName = File::ExtractNameAndExtension( path ) + ".txt";
   if ( MessageBox( "Write " + sidecarName + " with Bayer = " + String( ser::BayerPatternKeyword( pattern ) ) +
                    "? Auto will use it for this video from now on (SERViewer and PlanetaryStack).",
                    process, StdIcon::Question, StdButton::Yes, StdButton::No ).Execute() != StdButton::Yes )
      return SavePatternResult::Declined;
   try
   {
      ser::WriteAsiairSidecar( ToUtf8( path ), pattern, PLANETARY_VERSION_STRING );
      return SavePatternResult::Written;
   }
   catch ( ... )
   {
      failure = String( process ) + ": Save pattern: " +
                FromUtf8( planetary::preview::DescribeException( std::current_exception() ) );
   }
   Console().CriticalLn( "<end><cbr>*** " + failure.EncodedHTMLSpecialChars() );
   MessageBox( failure, process, StdIcon::Error, StdButton::Ok ).Execute();
   return SavePatternResult::Failed;
}

} // namespace pcl
