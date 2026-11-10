// EZTools -- per-view sky cache for EZHazeKill's Real-Time Preview (PCL-free, header-only).
// Copyright and licence: see src/EZToolsLicense.h.
//
// The RTP renders a reduced copy of the image; measuring the sky on it would give other numbers
// than Execute. So the sky is measured once on the view's FULL image and kept here, keyed by the
// view's full id and geometry. The interface invalidates a view (and its previews) whenever PCL
// reports that its image changed, so a rendition never shows a stale estimate.
//
// Thread-safe: renditions run on PixInsight's RTP thread, invalidations on the GUI thread. The
// measurement runs outside the lock; a result measured across an invalidation of its view is
// returned to that caller but not stored.

#ifndef EZTOOLS_SKYCACHE_H
#define EZTOOLS_SKYCACHE_H

#include "hazekill/HazeKill.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace eztools::hazekill
{

struct SkyKey
{
   std::string viewFullId;
   int width = 0, height = 0, channels = 0;

   bool operator==( const SkyKey& o ) const
   {
      return viewFullId == o.viewFullId && width == o.width && height == o.height && channels == o.channels;
   }
};

class SkyCache
{
public:
   // The cached estimate for key, measuring (once) on a miss. Returned by value: another thread
   // may invalidate the entry while the caller still uses it.
   SkyEstimate Get( const SkyKey& key, const std::function<SkyEstimate()>& measure )
   {
      uint64_t epoch;
      {
         std::lock_guard<std::mutex> lock( m_mutex );
         auto it = m_entries.find( key.viewFullId );
         if ( it != m_entries.end() && it->second.key == key )
            return it->second.estimate;
         ++m_measurements;
         epoch = m_epoch;
      }
      SkyEstimate e = measure();
      std::lock_guard<std::mutex> lock( m_mutex );
      if ( epoch == m_epoch )
         m_entries[key.viewFullId] = Entry{ key, e };
      return e;
   }

   // Drops viewFullId and every preview of it (ids starting with viewFullId + "->").
   void Invalidate( const std::string& viewFullId )
   {
      std::lock_guard<std::mutex> lock( m_mutex );
      ++m_epoch;
      const std::string prefix = viewFullId + "->";
      for ( auto it = m_entries.begin(); it != m_entries.end(); )
         if ( it->first == viewFullId || it->first.compare( 0, prefix.size(), prefix ) == 0 )
            it = m_entries.erase( it );
         else
            ++it;
   }

   void Clear()
   {
      std::lock_guard<std::mutex> lock( m_mutex );
      ++m_epoch;
      m_entries.clear();
   }

   // Total measure() calls, for tests.
   int Measurements() const
   {
      std::lock_guard<std::mutex> lock( m_mutex );
      return m_measurements;
   }

private:
   struct Entry
   {
      SkyKey key;
      SkyEstimate estimate;
   };
   mutable std::mutex m_mutex;
   std::map<std::string, Entry> m_entries;   // one entry per view: the latest geometry
   uint64_t m_epoch = 0;
   int m_measurements = 0;
};

} // namespace eztools::hazekill

#endif
