// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_JourneyStepsDialog_h
#define PICopilot_JourneyStepsDialog_h

#include "JourneyStore.h"

#include <pcl/BitmapBox.h>
#include <pcl/Dialog.h>
#include <pcl/Label.h>
#include <pcl/PushButton.h>
#include <pcl/Sizer.h>
#include <pcl/TreeBox.h>

#include <vector>

namespace pcl
{

// Read-only list of a journey's recorded steps (spec §7): number, image,
// process, who did it, state, median before -> after, reason (inferred ones
// marked); the selected step's thumbnail is shown beside the list (a row icon
// cannot be used: TreeBox rows keep a text line's height, so a 48 px icon was
// squashed to a dark bar -- seen in the GUI smoke). Base steps (history the
// image had when it joined) and noEffect steps are not listed, like every
// count and recipe (JourneyStore::StepCount, JourneyExport IsBase). A missing
// thumbnail is absent, never an error (JourneyStore::PruneUnkept residual).
// Everything is read in the constructor; only text and thumbnail paths are kept.
// Root thread only (Controls + JourneyStore). Store read failures throw
// (pcl::Error naming the database): the caller shows them.
class JourneyStepsDialog : public Dialog
{
public:

   JourneyStepsDialog( JourneyStore& store, int64 journeyId );
   // Empties the list while every member is alive: removing the nodes moves
   // the current node, and that event must never reach a half-destroyed dialog
   // (measured in the GUI smoke: SIGSEGV when the tree emptied itself after the
   // preview controls were already destroyed).
   ~JourneyStepsDialog() override;

   int RowCount() const { return m_rows; }     // self-test
   int IconCount() const { return m_icons; }   // self-test: rows that have a thumbnail
   String ThumbnailOfRow( int row ) const;     // self-test; "" = none
   String CellText( int row, int col ) const;  // self-test; "" when out of range
   String TitleText() const { return Title_Label.Text(); }
   String PreviewText() const { return Preview_Label.Text(); }   // self-test

private:

   VerticalSizer   Global_Sizer;
   Label           Title_Label;
   HorizontalSizer Body_Sizer;
   TreeBox         Steps_TreeBox;
   VerticalSizer   Preview_Sizer;
   BitmapBox       Preview_BitmapBox;
   Label           Preview_Label;
   HorizontalSizer Buttons_Sizer;
   PushButton      Close_PushButton;

   int m_rows = 0;
   int m_icons = 0;
   bool m_closing = false;
   std::vector<String> m_thumbs;   // per row, "" = no thumbnail

   void ShowPreview( int row );
   void e_Close( Button& sender, bool checked );
   void e_CurrentNode( TreeBox& sender, TreeBox::Node& current, TreeBox::Node& oldCurrent );
};

} // namespace pcl

#endif // PICopilot_JourneyStepsDialog_h
