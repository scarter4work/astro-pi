#pragma once

namespace nukex {

float median_inplace(float* data, int n);
float mad(const float* data, int n);
float biweight_location(const float* data, int n);
float biweight_midvariance(const float* data, int n);
float iqr(const float* data, int n);
float weighted_median(const float* data, const float* weights, int n);

} // namespace nukex
