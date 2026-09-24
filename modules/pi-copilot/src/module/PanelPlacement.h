// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_PanelPlacement_h
#define PICopilot_PanelPlacement_h

namespace pcl
{

struct PanelPlacement
{
   bool ok = false;   // false: screen geometry unusable, do not move/resize
   int  x = 0;
   int  y = 0;
   int  width = 0;
   int  height = 0;
};

/*
 * Flush-right, full-height default placement of the panel on the PRIMARY
 * screen, all values in physical device pixels. PCL exposes only the primary
 * screen's center (PixInsightSettings "Workspace/PrimaryScreenCenterX/Y"), so
 * this assumes that screen's origin is (0,0) and its size is 2*center; there
 * is no available-area (taskbar/panel) query, hence the fixed margins.
 * The width is clamped to what fits beside the right margin.
 */
inline PanelPlacement ComputeDefaultPanelPlacement( int screenCenterX, int screenCenterY,
                                                    int width, int topMargin, int bottomMargin,
                                                    int rightMargin )
{
   PanelPlacement p;
   const int screenW = 2*screenCenterX;
   const int screenH = 2*screenCenterY;
   const int maxW = screenW - rightMargin;
   const int h = screenH - topMargin - bottomMargin;
   if ( screenCenterX <= 0 || screenCenterY <= 0 || width <= 0 || maxW <= 0 || h <= 0 )
      return p;
   p.width = (width < maxW) ? width : maxW;
   p.height = h;
   p.x = screenW - p.width - rightMargin;
   p.y = topMargin;
   p.ok = true;
   return p;
}

} // namespace pcl

#endif // PICopilot_PanelPlacement_h
