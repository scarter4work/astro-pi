// NukeX — HTTPS fetcher for the QE camera-database updater.
//
// nukex4_calibration must not link PCL, so QEUpdater takes an injected
// Fetcher and this is the only implementation that touches the network.
// Unit tests inject a fake instead and never open a socket.

#ifndef NUKEX_MODULE_QEFETCHER_H
#define NUKEX_MODULE_QEFETCHER_H

#include "nukex/calibration/qe_update.hpp"

#include <pcl/Control.h>
#include <pcl/NetworkTransfer.h>

#include <string>

namespace pcl
{

class QEFetcher : public nukex::Fetcher
{
public:

   QEFetcher() = default;

   // pcl::Control's destructor is noexcept(false), and the Sink member
   // below is one, so QEFetcher's implicit destructor would be
   // noexcept(false) too -- looser than nukex::Fetcher's noexcept(true)
   // destructor, which is ill-formed. Stating noexcept here keeps the
   // library interface free of a PCL quirk and confines the consequence
   // (terminate if PCL's UI teardown ever throws, which it does not do on
   // a normal destruction path) to this one module class.
   ~QEFetcher() noexcept override {}

   // Never throws. An observatory laptop with no network is the normal
   // case, not an error condition, so failures come back as
   // { ok = false, error } for the caller to treat as OFFLINE.
   nukex::FetchResult get( const std::string& url ) override;

   // Total wall-clock ceiling for one transfer. The routine poll must not
   // be able to hang the interface opening.
   static constexpr int kTimeoutSeconds = 20;

   // Refuse absurd payloads before they reach memory. The manifest is well
   // under a kilobyte and the database is tens of kilobytes; anything at
   // this scale is a wrong URL or a hostile server, not our data.
   static constexpr size_type kMaxBytes = 8u*1024u*1024u;

private:

   // NetworkTransfer's download callback is typed
   //   bool (Control::*)( NetworkTransfer&, const void*, fsize_type )
   // and OnDownloadDataAvailable() takes a Control& receiver, so the sink
   // must BE a Control. QEFetcher holds one rather than inheriting from it:
   // Control's destructor is noexcept(false) while nukex::Fetcher's is
   // noexcept(true), so deriving from both would give QEFetcher a
   // destructor with a looser exception specification than the interface it
   // overrides -- which is ill-formed. Composition sidesteps that entirely.
   class Sink : public Control
   {
   public:
      IsoString data;
      bool      overflow = false;

      void Reset() { data.Clear(); overflow = false; }

      bool OnData( NetworkTransfer& sender, const void* buffer, fsize_type size );
   };

   Sink m_sink;
};

} // namespace pcl

#endif // NUKEX_MODULE_QEFETCHER_H
