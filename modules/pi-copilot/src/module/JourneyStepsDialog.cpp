// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "JourneyStepsDialog.h"
#include "Utf8.h"

#include <pcl/Bitmap.h>
#include <pcl/File.h>

namespace pcl
{

namespace
{
constexpr int kColumns = 7;
constexpr int kPreviewW = 256;   // logical px: the thumbnails are 256 px on their long edge
constexpr int kPreviewH = 256;
}

JourneyStepsDialog::JourneyStepsDialog( JourneyStore& store, int64 journeyId )
{
   JourneyRow j;
   if ( !store.GetJourney( journeyId, j ) )
      throw Error( String().Format( "journey #%lld is not in the journey library (", static_cast<long long>( journeyId ) )
                   + store.DbPath() + ")" );
   Title_Label.SetText( FromU8( j.name ) + (j.kept ? String( "  (kept)" ) : String()) );

   Steps_TreeBox.SetNumberOfColumns( kColumns );
   const char* headers[] = { "#", "Image", "Process", "By", "State", "Median", "Reason" };
   for ( int c = 0; c < kColumns; ++c )
      Steps_TreeBox.SetHeaderText( c, headers[c] );
   Steps_TreeBox.EnableAlternateRowColor();
   Steps_TreeBox.EnableRootDecoration( false );
   Steps_TreeBox.SetScaledMinSize( 720, 360 );

   for ( const ImageRow& img : store.Images( journeyId ) )
   {
      // The median before a step = the image's starting median, then the last
      // measured active step's (Ruling 7: only the last step of a tick is measured,
      // so an unmeasured step shows no median).
      std::vector<ChannelStats> before = store.Stats( img.id, 0 );
      for ( const StepRow& s : store.Steps( img.id, true ) )
      {
         if ( s.params.value( "base", false ) || s.params.value( "noEffect", false ) )
            continue;
         const std::vector<ChannelStats> after = store.Stats( img.id, s.id );
         TreeBox::Node* n = new TreeBox::Node( Steps_TreeBox );
         n->SetText( 0, String( s.seq ) );
         n->SetText( 1, FromU8( img.viewId ) );
         n->SetText( 2, FromU8( s.processId ) );
         n->SetText( 3, s.actor == "copilot" ? "PI Copilot" : "you" );
         n->SetText( 4, FromU8( s.state ) );
         if ( !before.empty() && !after.empty() )
            n->SetText( 5, String().Format( "%.4f ", before[0].median ) + FromU8( "\xE2\x86\x92" )
                           + String().Format( " %.4f", after[0].median ) );
         n->SetText( 6, FromU8( s.reason ) + (s.reasonInferred && !s.reason.empty() ? String( " (inferred)" ) : String()) );
         const String thumb = store.JourneyDir( journeyId ) + String().Format( "/thumbs/%lld.jpg", static_cast<long long>( s.id ) );
         if ( File::Exists( thumb ) )
         {
            m_thumbs.push_back( thumb );
            ++m_icons;
         }
         else
            m_thumbs.push_back( String() );
         if ( !after.empty() && s.state == "active" )
            before = after;
         ++m_rows;
      }
   }
   for ( int c = 0; c < kColumns; ++c )
      Steps_TreeBox.AdjustColumnWidthToContents( c );

   Close_PushButton.SetText( "Close" );
   Close_PushButton.SetDefault();
   Close_PushButton.OnClick( (Button::click_event_handler)&JourneyStepsDialog::e_Close, *this );
   Buttons_Sizer.AddStretch();
   Buttons_Sizer.Add( Close_PushButton );

   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( Title_Label );
   Preview_BitmapBox.SetScaledFixedSize( kPreviewW, kPreviewH );
   Preview_Label.SetTextAlignment( TextAlign::HorzCenter|TextAlign::VertCenter );
   Preview_Sizer.SetSpacing( 4 );
   Preview_Sizer.Add( Preview_BitmapBox );
   Preview_Sizer.Add( Preview_Label );
   Preview_Sizer.AddStretch();
   Body_Sizer.SetSpacing( 8 );
   Body_Sizer.Add( Steps_TreeBox, 100 );
   Body_Sizer.Add( Preview_Sizer );
   Global_Sizer.Add( Body_Sizer, 100 );
   Global_Sizer.Add( Buttons_Sizer );
   SetSizer( Global_Sizer );
   SetWindowTitle( FromU8( "PI Copilot \xE2\x80\x94 Journey steps" ) );
   AdjustToContents();

   Steps_TreeBox.OnCurrentNodeUpdated( (TreeBox::node_navigation_event_handler)&JourneyStepsDialog::e_CurrentNode, *this );
   // Start on the newest step that has a thumbnail.
   int first = -1;
   for ( int r = m_rows - 1; r >= 0 && first < 0; --r )
      if ( !m_thumbs[r].IsEmpty() )
         first = r;
   if ( first >= 0 )
      Steps_TreeBox.SetCurrentNode( Steps_TreeBox.Child( first ) );
   ShowPreview( first );
}

JourneyStepsDialog::~JourneyStepsDialog()
{
   m_closing = true;
   Steps_TreeBox.Clear();
}

String JourneyStepsDialog::ThumbnailOfRow( int row ) const
{
   return (row >= 0 && row < int( m_thumbs.size() )) ? m_thumbs[row] : String();
}

void JourneyStepsDialog::ShowPreview( int row )
{
   const String thumb = ThumbnailOfRow( row );
   if ( thumb.IsEmpty() )
   {
      Preview_BitmapBox.Clear();
      Preview_Label.SetText( row < 0 ? String( "No thumbnails recorded" ) : String( "No thumbnail for this step" ) );
      return;
   }
   // A thumbnail removed meanwhile (retention) is absent, never an error.
   const Bitmap bmp = File::Exists( thumb ) ? Bitmap( thumb ) : Bitmap::Null();
   if ( bmp.IsEmpty() )
   {
      Preview_BitmapBox.Clear();
      Preview_Label.SetText( "Thumbnail not available" );
      return;
   }
   const int side = LogicalPixelsToPhysical( kPreviewW );
   Preview_BitmapBox.SetBitmap( bmp.Width() >= bmp.Height() ? bmp.ScaledToWidth( side ) : bmp.ScaledToHeight( side ) );
   Preview_Label.SetText( "Step " + Steps_TreeBox.Child( row )->Text( 0 ) + " (after)" );
}

void JourneyStepsDialog::e_CurrentNode( TreeBox& sender, TreeBox::Node& current, TreeBox::Node& )
{
   if ( m_closing )
      return;
   ShowPreview( sender.ChildIndex( &current ) );
}

String JourneyStepsDialog::CellText( int row, int col ) const
{
   if ( row < 0 || row >= Steps_TreeBox.NumberOfChildren() || col < 0 || col >= kColumns )
      return String();
   return Steps_TreeBox.Child( row )->Text( col );
}

void JourneyStepsDialog::e_Close( Button&, bool )
{
   Ok();
}

} // namespace pcl
