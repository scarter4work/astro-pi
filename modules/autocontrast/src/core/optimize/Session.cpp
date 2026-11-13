#include "core/optimize/Session.h"

namespace ac {

Json branch_to_json(const Branch& b)
{
   Json j = Json::object();
   j["recipe"] = b.recipe.to_json();
   j["image_path"] = b.image_id;
   j["distance"] = b.distance;
   j["alive"] = b.alive;
   j["fingerprint"] = b.fingerprint ? b.fingerprint->to_json() : Json(nullptr);
   return j;
}

Json Session::to_json() const
{
   Json cfg = Json::object();   // dataclasses.asdict(BeamConfig): field order
   cfg["width"] = config.width;
   cfg["top_k"] = config.top_k;
   cfg["iteration_cap"] = config.iteration_cap;
   cfg["convergence_window"] = config.convergence_window;
   cfg["epsilon"] = config.epsilon;
   cfg["epsilon_improve"] = config.epsilon_improve;
   cfg["max_attempts"] = config.resolved_max_attempts();

   Json br = Json::array();
   for (const Branch& b : branches)
      br.push_back(branch_to_json(b));
   Json hist = Json::array();
   for (double d : distance_history)
      hist.push_back(d);

   Json j = Json::object();
   j["session_id"] = session_id;
   j["work_dir"] = work_dir;
   j["source_path"] = source_path;
   j["proxy_path"] = proxy_id;
   j["reference_id"] = reference_id;
   j["reference_fp"] = reference_fp.to_json();
   j["pixel_scale_arcsec"] = pixel_scale_arcsec;
   j["psf_fwhm_arcsec"] = psf_fwhm_arcsec;
   j["palette_class"] = palette_class;
   j["n_scales"] = n_scales;
   j["config"] = std::move(cfg);
   j["iteration"] = iteration;
   j["branches"] = std::move(br);
   j["best"] = branch_to_json(best);
   j["baseline_distance"] = baseline_distance;
   j["distance_history"] = std::move(hist);
   j["converged"] = converged;
   j["convergence_reason"] = convergence_reason;
   j["guardrail_log"] = guardrail_log;
   // Batched-protocol fields (sidecar-only, dropped in the module): Python's empty defaults.
   j["pending"] = Json::array();
   j["cursors"] = Json::array();
   j["candidates"] = Json::array();
   j["max_dim"] = nullptr;
   j["candidate_suffix"] = ".png";
   return j;
}

} // namespace ac
