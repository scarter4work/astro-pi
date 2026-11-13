// Model checks shared by render_cavity_2d, build_glb and the orbit (PCL-free): the members of a
// CavityModel are public, so each consumer re-checks what the constructor cannot guarantee.
#pragma once

#include "Model.h"

#include <string>

namespace gdg::recon {

/// std::invalid_argument("<who>: the cavity model holds a non-finite <z|emissivity|dust> value at
/// (x, y)") for the first non-finite pixel (rows in order; within a pixel z, then dust, then
/// emissivity). The planes must already share a shape and have 1 / 1 / 3 channels
/// (validate_shapes). Rows run in parallel; reentrant.
void require_finite_model(const CavityModel& model, const std::string& who);

/// std::invalid_argument("apex (<x>, <y>) is outside the model grid (<nx>x<ny>) by more than its
/// size") unless -nx <= x <= 2 nx and -ny <= y <= 2 ny (nx x ny = the z plane); a non-finite apex
/// throws "<who>: the apex must be finite". Beyond that bound the apex distance R, hence the depth
/// scale D = depth_ratio * R, no longer describes the model.
void require_apex_in_grid(const CavityModel& model, const std::string& who);

}  // namespace gdg::recon
