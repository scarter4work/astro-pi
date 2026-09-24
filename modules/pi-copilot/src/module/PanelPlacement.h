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
 * screen, all values in physical device pixels.
 *
 * PCL exposes only the primary screen's CENTER (Cx,Cy) in GLOBAL desktop
 * coordinates (PixInsightSettings "Workspace/PrimaryScreenCenterX/Y",
 * GlobalSettings.h:233-234) -- not its origin, size or available area. On a
 * multi-monitor desktop the primary screen's origin (ox,oy) need not be
 * (0,0): a 2560x1440 primary to the right of a 1920 px monitor has Cx = 3200,
 * and 2*Cx = 6400 is far off every screen. So the placement is anchored to
 * the center, never to 2*center, using half-extents that are at most the
 * real ones in the common cases:
 *
 *   halfW = min( Cx, Cy*16/9 )     halfH = min( Cy, Cx*9/16 )
 *
 * Why these bounds:
 *  - Cx - ox is the real half-width and ox >= 0 for the primary screen
 *    (Windows/macOS put it at (0,0); X11 desktop coordinates are
 *    non-negative), so Cx and Cy are upper bounds of the real half-extents,
 *    exact when the origin is 0 on that axis.
 *  - Side-by-side monitors (oy = 0) make Cy exact; then Cy*16/9 is the
 *    real half-width of any primary with aspect >= 16:9, whatever ox is.
 *    Symmetrically, stacked monitors (ox = 0) make Cx exact and Cx*9/16 is
 *    the real half-height of any primary with aspect <= 16:9.
 *  - A single 16:9 primary at the origin (the common case) gets exactly
 *    flush-right and full-height.
 * Known imprecision, never negative/unbounded coordinates:
 *  - Primary wider than 16:9 at the origin (ultrawide): the panel sits
 *    Cx - Cy*16/9 px left of the right edge (still fully on screen).
 *  - Primary narrower than 16:9 at the origin (16:10, 4:3): the panel is
 *    shorter than full height by 2*(Cy - Cx*9/16) px (still on screen).
 *  - Primary narrower than 16:9 AND offset horizontally (e.g. 16:10 right of
 *    another monitor): halfW overestimates by Cy*(16/9 - W/H); the panel's
 *    right part may overhang the primary's right edge by that much (~140 px
 *    at 1600 px height) -- its title bar and left part stay on the primary.
 *    Without an origin query this cannot be told apart from an ultrawide.
 *
 * The result always satisfies, for the estimated extents:
 *   Cx - halfW <= x,  x + width + rightMargin <= Cx + halfW,
 *   Cy - halfH + topMargin == y,  y + height + bottomMargin == Cy + halfH,
 * and x >= 0, y >= 0 (as halfW <= Cx and halfH <= Cy). The width is clamped
 * to what fits beside the right margin; there is no available-area
 * (taskbar/panel) query, hence the fixed margins.
 */
inline PanelPlacement ComputeDefaultPanelPlacement( int screenCenterX, int screenCenterY,
                                                    int width, int topMargin, int bottomMargin,
                                                    int rightMargin )
{
   PanelPlacement p;
   if ( screenCenterX <= 0 || screenCenterY <= 0 || width <= 0
     || topMargin < 0 || bottomMargin < 0 || rightMargin < 0 )
      return p;
   // 64-bit intermediates: Cy*16 and Cx*9 must not overflow int.
   const long long cx = screenCenterX;
   const long long cy = screenCenterY;
   const long long halfW = (cx < cy*16/9) ? cx : cy*16/9;
   const long long halfH = (cy < cx*9/16) ? cy : cx*9/16;
   const long long maxW = 2*halfW - rightMargin;
   const long long h = 2*halfH - topMargin - bottomMargin;
   if ( maxW <= 0 || h <= 0 )
      return p;
   const long long w = (width < maxW) ? width : maxW;
   p.width = int( w );
   p.height = int( h );
   p.x = int( cx + halfW - w - rightMargin );
   p.y = int( cy - halfH + topMargin );
   p.ok = true;
   return p;
}

} // namespace pcl

#endif // PICopilot_PanelPlacement_h
