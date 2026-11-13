// AutoContrast core (PCL-free): loop state and its session.json record (design §2.1, §6.10) — optimize/session.py.
//
// The module runs the loop in process, so the Python batched protocol (begin_batch/step_batch, pending
// instructions, per-branch cursors) is intentionally dropped (sidecar-only). session.json is still written after
// every iteration, in Python's schema, so a crashed run leaves an inspectable record.
#pragma once

#include "core/Json.h"
#include "core/fingerprint/Extract.h"
#include "core/optimize/Beam.h"

#include <string>
#include <vector>

namespace ac {

struct Session {
   std::string session_id, work_dir, source_path, proxy_id, reference_id;
   FingerprintData reference_fp;
   double pixel_scale_arcsec = 0.0, psf_fwhm_arcsec = 0.0;
   std::string palette_class;
   int n_scales = 0;
   BeamConfig config;
   int iteration = 0;
   std::vector<Branch> branches;
   Branch best;
   double baseline_distance = 0.0;
   std::vector<double> distance_history;
   bool converged = false;
   std::string convergence_reason;
   Json guardrail_log = Json::array();

   // Session.save's payload (session.py:104-123): keys and order exactly as Python, `proxy_id` under
   // "proxy_path" and each branch's image id under "image_path", config with max_attempts resolved (Python's
   // __post_init__ does the same). The dropped batched fields are written as Python's empty defaults for schema
   // compatibility: pending [], cursors [], candidates [], max_dim null, candidate_suffix ".png".
   // Serialise with dump_python(to_json(), 2) to get Python's json.dumps(payload, indent=2) text.
   Json to_json() const;
};

// _branch_to_dict (session.py:25-35): recipe, image_path, distance, alive, fingerprint (null when unmeasured).
Json branch_to_json(const Branch& b);

} // namespace ac
