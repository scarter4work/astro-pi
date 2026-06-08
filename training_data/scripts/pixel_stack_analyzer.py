#!/usr/bin/env python3
"""
Pixel Stack Analyzer - Distribution fitting across frame stacks

This module analyzes pixel values across a stack of prestretched subframes,
fitting statistical distributions to understand the data characteristics
at each pixel position.

Distribution types supported:
- Gaussian: Normal distribution for stable, well-behaved pixels
- Lognormal: For positive-valued data with right skew (common in astro)
- Skewed: Generalized skewed distribution (skewnorm)
- Bimodal: Mixture of two Gaussians (e.g., star + background overlap)

Author: NukeX Project
"""

import numpy as np
from scipy import stats
from scipy.optimize import minimize
from dataclasses import dataclass
from enum import Enum
from typing import Tuple, Optional, List
import warnings


class DistributionType(Enum):
    """Supported distribution types for pixel stack analysis."""
    GAUSSIAN = "gaussian"
    LOGNORMAL = "lognormal"
    SKEWED = "skewed"
    BIMODAL = "bimodal"
    UNKNOWN = "unknown"


@dataclass
class DistributionParams:
    """Parameters describing a fitted distribution."""
    dist_type: DistributionType
    mu: float           # Location (mean)
    sigma: float        # Scale (std dev)
    skewness: float     # Skewness (0 for symmetric)
    kurtosis: float     # Excess kurtosis (0 for normal)

    # For bimodal distributions
    mu2: Optional[float] = None
    sigma2: Optional[float] = None
    weight1: Optional[float] = None  # Weight of first component

    # Goodness of fit
    aic: float = float('inf')  # Akaike Information Criterion

    def __repr__(self):
        if self.dist_type == DistributionType.BIMODAL:
            return (f"DistributionParams({self.dist_type.value}, "
                    f"mu1={self.mu:.4f}, sigma1={self.sigma:.4f}, "
                    f"mu2={self.mu2:.4f}, sigma2={self.sigma2:.4f}, "
                    f"w1={self.weight1:.2f})")
        return (f"DistributionParams({self.dist_type.value}, "
                f"mu={self.mu:.4f}, sigma={self.sigma:.4f}, "
                f"skew={self.skewness:.2f}, kurt={self.kurtosis:.2f})")


@dataclass
class PixelStackMetadata:
    """Complete metadata for a pixel position across the stack."""
    dist: DistributionParams
    selected_value: float
    source_frame: int
    confidence: float
    outlier_mask: np.ndarray  # Boolean mask, True = outlier


class PixelStackAnalyzer:
    """
    Analyzes pixel values across a stack of frames at each (x,y) position.

    This is the core engine for understanding the statistical characteristics
    of pixel stacks, which informs both classification and selection.
    """

    # Minimum frames needed for reliable distribution fitting
    MIN_FRAMES_FOR_FIT = 5

    # Outlier detection thresholds by class type
    OUTLIER_THRESHOLDS = {
        'background': 2.5,      # Tight rejection for background
        'star_core': 3.5,       # Loose - don't reject bright values
        'star_halo': 3.0,       # Moderate
        'emission_nebula': 3.0, # Moderate - preserve signal
        'dark_nebula': 3.0,     # Moderate - preserve low values
        'galaxy_core': 3.0,
        'default': 3.0
    }

    def __init__(self, sigma_clip: float = 3.0, min_valid_ratio: float = 0.5):
        """
        Initialize the analyzer.

        Args:
            sigma_clip: Default sigma threshold for outlier detection
            min_valid_ratio: Minimum fraction of frames that must be valid
        """
        self.sigma_clip = sigma_clip
        self.min_valid_ratio = min_valid_ratio

    def fit_distribution(self, values: np.ndarray) -> DistributionParams:
        """
        Fit the best distribution to a set of pixel values across frames.

        Args:
            values: Array of pixel values from N frames at one position

        Returns:
            DistributionParams with best-fit parameters
        """
        values = np.asarray(values, dtype=np.float64)
        values = values[np.isfinite(values)]

        if len(values) < self.MIN_FRAMES_FOR_FIT:
            return self._create_fallback_params(values)

        # Calculate basic statistics
        mu = np.mean(values)
        sigma = np.std(values)
        if sigma < 1e-10:
            sigma = 1e-10  # Prevent division by zero

        skewness = stats.skew(values)
        kurtosis = stats.kurtosis(values)  # Excess kurtosis

        # Try different distribution fits and select best by AIC
        candidates = []

        # 1. Gaussian fit
        gaussian_params = self._fit_gaussian(values)
        candidates.append(gaussian_params)

        # 2. Lognormal fit (only if all values positive)
        if np.all(values > 0):
            lognormal_params = self._fit_lognormal(values)
            candidates.append(lognormal_params)

        # 3. Skewed normal fit
        skewed_params = self._fit_skewed(values)
        candidates.append(skewed_params)

        # 4. Bimodal fit (if kurtosis suggests bimodality)
        if kurtosis < -0.5 or len(values) >= 10:
            bimodal_params = self._fit_bimodal(values)
            if bimodal_params is not None:
                candidates.append(bimodal_params)

        # Select best by AIC
        best = min(candidates, key=lambda p: p.aic)

        # Update statistics
        best.skewness = skewness
        best.kurtosis = kurtosis

        return best

    def _fit_gaussian(self, values: np.ndarray) -> DistributionParams:
        """Fit Gaussian distribution."""
        mu = np.mean(values)
        sigma = np.std(values)

        # Calculate AIC: AIC = 2k - 2ln(L)
        # For Gaussian: ln(L) = -n/2 * ln(2*pi*sigma^2) - sum((x-mu)^2)/(2*sigma^2)
        n = len(values)
        if sigma > 0:
            log_likelihood = -n/2 * np.log(2 * np.pi * sigma**2) - \
                             np.sum((values - mu)**2) / (2 * sigma**2)
        else:
            log_likelihood = -np.inf

        aic = 2 * 2 - 2 * log_likelihood  # k=2 parameters

        return DistributionParams(
            dist_type=DistributionType.GAUSSIAN,
            mu=mu,
            sigma=sigma,
            skewness=0.0,
            kurtosis=0.0,
            aic=aic
        )

    def _fit_lognormal(self, values: np.ndarray) -> DistributionParams:
        """Fit lognormal distribution to positive values."""
        try:
            # Shift if needed to ensure positivity
            min_val = np.min(values)
            shift = 0
            if min_val <= 0:
                shift = -min_val + 1e-6

            log_values = np.log(values + shift)
            mu_log = np.mean(log_values)
            sigma_log = np.std(log_values)

            # Back-transform to get distribution parameters
            mu = np.exp(mu_log + sigma_log**2 / 2) - shift
            sigma = np.sqrt((np.exp(sigma_log**2) - 1) * np.exp(2*mu_log + sigma_log**2))

            # Calculate AIC
            n = len(values)
            if sigma_log > 0:
                log_likelihood = np.sum(stats.lognorm.logpdf(values + shift,
                                                             s=sigma_log,
                                                             scale=np.exp(mu_log)))
            else:
                log_likelihood = -np.inf

            aic = 2 * 2 - 2 * log_likelihood

            return DistributionParams(
                dist_type=DistributionType.LOGNORMAL,
                mu=mu,
                sigma=sigma,
                skewness=0.0,  # Will be updated
                kurtosis=0.0,  # Will be updated
                aic=aic
            )
        except Exception:
            # Fall back to Gaussian if lognormal fails
            return self._fit_gaussian(values)

    def _fit_skewed(self, values: np.ndarray) -> DistributionParams:
        """Fit skewed normal distribution."""
        try:
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                # Fit skewnorm distribution
                a, loc, scale = stats.skewnorm.fit(values)

            # Calculate AIC
            n = len(values)
            log_likelihood = np.sum(stats.skewnorm.logpdf(values, a, loc=loc, scale=scale))
            aic = 2 * 3 - 2 * log_likelihood  # k=3 parameters

            return DistributionParams(
                dist_type=DistributionType.SKEWED,
                mu=loc,
                sigma=scale,
                skewness=a,  # Shape parameter (not exact skewness)
                kurtosis=0.0,  # Will be updated
                aic=aic
            )
        except Exception:
            return self._fit_gaussian(values)

    def _fit_bimodal(self, values: np.ndarray) -> Optional[DistributionParams]:
        """
        Fit mixture of two Gaussians using EM-like approach.

        This is important for detecting pixels where star and background overlap,
        or transition regions.
        """
        try:
            n = len(values)
            if n < 10:
                return None

            # Initialize with k-means style split
            sorted_vals = np.sort(values)
            split_idx = n // 2

            # Initial parameters
            mu1 = np.mean(sorted_vals[:split_idx])
            mu2 = np.mean(sorted_vals[split_idx:])
            sigma1 = np.std(sorted_vals[:split_idx])
            sigma2 = np.std(sorted_vals[split_idx:])
            weight1 = 0.5

            # Simple EM iterations
            for _ in range(20):
                # E-step: calculate responsibilities
                if sigma1 < 1e-10:
                    sigma1 = 1e-10
                if sigma2 < 1e-10:
                    sigma2 = 1e-10

                p1 = weight1 * stats.norm.pdf(values, mu1, sigma1)
                p2 = (1 - weight1) * stats.norm.pdf(values, mu2, sigma2)
                total = p1 + p2 + 1e-10
                r1 = p1 / total
                r2 = p2 / total

                # M-step: update parameters
                n1 = np.sum(r1)
                n2 = np.sum(r2)

                if n1 > 1:
                    mu1 = np.sum(r1 * values) / n1
                    sigma1 = np.sqrt(np.sum(r1 * (values - mu1)**2) / n1)
                if n2 > 1:
                    mu2 = np.sum(r2 * values) / n2
                    sigma2 = np.sqrt(np.sum(r2 * (values - mu2)**2) / n2)

                weight1 = n1 / n

            # Check if bimodal is significantly better
            # Calculate log-likelihood
            if sigma1 < 1e-10 or sigma2 < 1e-10:
                return None

            log_likelihood = np.sum(np.log(
                weight1 * stats.norm.pdf(values, mu1, sigma1) +
                (1 - weight1) * stats.norm.pdf(values, mu2, sigma2) + 1e-10
            ))

            aic = 2 * 5 - 2 * log_likelihood  # k=5 parameters

            # Only return if the two modes are separated enough
            mode_separation = abs(mu2 - mu1) / (sigma1 + sigma2)
            if mode_separation < 0.5:
                return None

            return DistributionParams(
                dist_type=DistributionType.BIMODAL,
                mu=mu1,
                sigma=sigma1,
                skewness=0.0,
                kurtosis=0.0,
                mu2=mu2,
                sigma2=sigma2,
                weight1=weight1,
                aic=aic
            )
        except Exception:
            return None

    def _create_fallback_params(self, values: np.ndarray) -> DistributionParams:
        """Create basic params when insufficient data."""
        if len(values) == 0:
            return DistributionParams(
                dist_type=DistributionType.UNKNOWN,
                mu=0.0,
                sigma=1.0,
                skewness=0.0,
                kurtosis=0.0
            )

        return DistributionParams(
            dist_type=DistributionType.GAUSSIAN,
            mu=float(np.mean(values)),
            sigma=float(np.std(values)) if len(values) > 1 else 0.1,
            skewness=0.0,
            kurtosis=0.0
        )

    def identify_outliers(self, values: np.ndarray,
                         dist_params: DistributionParams,
                         class_type: str = 'default') -> np.ndarray:
        """
        Identify outliers based on distribution and class type.

        This is CLASS-AWARE outlier detection:
        - For background: reject high outliers (gradients, satellites)
        - For star cores: reject LOW outliers only (preserve bright)
        - For dark nebula: reject HIGH outliers only (preserve dark)
        - For emission nebula: symmetric rejection but loose

        Args:
            values: Array of pixel values from N frames
            dist_params: Fitted distribution parameters
            class_type: Region classification (background, star_core, etc.)

        Returns:
            Boolean mask where True = outlier
        """
        values = np.asarray(values, dtype=np.float64)
        n = len(values)
        outlier_mask = np.zeros(n, dtype=bool)

        if n < 3:
            return outlier_mask

        # Get threshold for this class
        threshold = self.OUTLIER_THRESHOLDS.get(class_type,
                                                 self.OUTLIER_THRESHOLDS['default'])

        mu = dist_params.mu
        sigma = dist_params.sigma

        # For very low sigma, use MAD-based approach
        if sigma < 1e-10:
            mad = np.median(np.abs(values - np.median(values)))
            if mad > 1e-10:
                sigma = mad * 1.4826  # Convert MAD to sigma equivalent
            else:
                return outlier_mask

        # Calculate z-scores
        z_scores = (values - mu) / sigma

        # Apply class-aware rejection
        if class_type == 'background':
            # Reject high outliers more aggressively (satellites, gradients)
            # But also reject very low (bad frames)
            outlier_mask = (z_scores > threshold) | (z_scores < -threshold * 1.5)

        elif class_type == 'star_core':
            # CRITICAL: Only reject LOW outliers
            # High values are real star signal!
            # Use a more aggressive threshold for star cores to catch dim frames
            outlier_mask = z_scores < -2.0  # More aggressive for star cores

        elif class_type == 'dark_nebula':
            # CRITICAL: Only reject HIGH outliers
            # Low values are real dark nebula absorption!
            outlier_mask = z_scores > threshold

        elif class_type in ['emission_nebula', 'bright_emission']:
            # Favor preserving signal, only reject obvious contamination
            # Reject low values (clouds blocking signal) more than high
            outlier_mask = (z_scores < -threshold) | (z_scores > threshold * 1.2)

        elif class_type == 'star_halo':
            # Symmetric rejection, moderate threshold
            # Also use MAD-based detection for robustness
            mad = np.median(np.abs(values - np.median(values)))
            if mad > 1e-10:
                mad_scores = np.abs(values - np.median(values)) / mad
                outlier_mask = (np.abs(z_scores) > threshold) | (mad_scores > 4.0)
            else:
                outlier_mask = np.abs(z_scores) > threshold

        else:
            # Default: symmetric rejection
            outlier_mask = np.abs(z_scores) > threshold

        # Never mark more than 40% as outliers
        if np.sum(outlier_mask) > 0.4 * n:
            # Keep only the most extreme outliers
            sorted_indices = np.argsort(np.abs(z_scores))[::-1]
            max_outliers = int(0.4 * n)
            outlier_mask = np.zeros(n, dtype=bool)
            outlier_mask[sorted_indices[:max_outliers]] = True

        return outlier_mask

    def analyze_stack(self, stack: np.ndarray,
                     region_classes: Optional[np.ndarray] = None) -> List[PixelStackMetadata]:
        """
        Analyze a full image stack.

        Args:
            stack: 3D array of shape (N_frames, height, width)
            region_classes: Optional 2D array of class labels (height, width)

        Returns:
            List of PixelStackMetadata for each pixel position
        """
        n_frames, height, width = stack.shape
        results = []

        for y in range(height):
            for x in range(width):
                values = stack[:, y, x]

                # Get class for this pixel
                if region_classes is not None:
                    class_type = self._class_id_to_name(region_classes[y, x])
                else:
                    class_type = 'default'

                # Fit distribution
                dist_params = self.fit_distribution(values)

                # Identify outliers
                outlier_mask = self.identify_outliers(values, dist_params, class_type)

                # Select best value (simple version - will be replaced by PixelSelector)
                valid_values = values[~outlier_mask]
                if len(valid_values) > 0:
                    selected_value = float(np.median(valid_values))
                    source_frame = int(np.argmin(np.abs(values - selected_value)))
                    confidence = 1.0 - np.sum(outlier_mask) / n_frames
                else:
                    selected_value = float(np.median(values))
                    source_frame = n_frames // 2
                    confidence = 0.5

                results.append(PixelStackMetadata(
                    dist=dist_params,
                    selected_value=selected_value,
                    source_frame=source_frame,
                    confidence=confidence,
                    outlier_mask=outlier_mask
                ))

        return results

    def _class_id_to_name(self, class_id: int) -> str:
        """Map class ID to class name for outlier detection."""
        class_mapping = {
            0: 'background',
            1: 'star_core',
            2: 'star_halo',
            3: 'emission_nebula',
            4: 'dark_nebula',
            5: 'reflection_nebula',
            6: 'galaxy_core',
            7: 'galaxy_arm',
            8: 'dust_lane',
            9: 'transition',
            10: 'bright_emission',
            11: 'faint_emission',
            # Add more as needed
        }
        return class_mapping.get(class_id, 'default')


def compute_stack_statistics(values: np.ndarray) -> dict:
    """
    Compute comprehensive statistics for a pixel stack.

    Useful for debugging and understanding pixel behavior.
    """
    values = np.asarray(values, dtype=np.float64)
    values = values[np.isfinite(values)]

    if len(values) == 0:
        return {'n': 0}

    return {
        'n': len(values),
        'mean': float(np.mean(values)),
        'median': float(np.median(values)),
        'std': float(np.std(values)),
        'min': float(np.min(values)),
        'max': float(np.max(values)),
        'range': float(np.max(values) - np.min(values)),
        'skewness': float(stats.skew(values)) if len(values) > 2 else 0.0,
        'kurtosis': float(stats.kurtosis(values)) if len(values) > 3 else 0.0,
        'iqr': float(np.percentile(values, 75) - np.percentile(values, 25)),
        'mad': float(np.median(np.abs(values - np.median(values)))),  # Median absolute deviation
    }


# Example usage and testing
if __name__ == '__main__':
    # Test distribution fitting
    analyzer = PixelStackAnalyzer()

    # Test 1: Gaussian data
    print("=" * 60)
    print("Test 1: Gaussian distribution")
    gaussian_data = np.random.normal(0.5, 0.1, 20)
    params = analyzer.fit_distribution(gaussian_data)
    print(f"  Data: mean={np.mean(gaussian_data):.3f}, std={np.std(gaussian_data):.3f}")
    print(f"  Fit:  {params}")

    # Test 2: Lognormal data (positive skew)
    print("\n" + "=" * 60)
    print("Test 2: Lognormal distribution")
    lognormal_data = np.random.lognormal(0, 0.5, 20)
    lognormal_data = lognormal_data / np.max(lognormal_data)  # Normalize
    params = analyzer.fit_distribution(lognormal_data)
    print(f"  Data: mean={np.mean(lognormal_data):.3f}, std={np.std(lognormal_data):.3f}")
    print(f"  Fit:  {params}")

    # Test 3: Bimodal data
    print("\n" + "=" * 60)
    print("Test 3: Bimodal distribution")
    bimodal_data = np.concatenate([
        np.random.normal(0.3, 0.05, 10),
        np.random.normal(0.7, 0.05, 10)
    ])
    params = analyzer.fit_distribution(bimodal_data)
    print(f"  Data: mean={np.mean(bimodal_data):.3f}, std={np.std(bimodal_data):.3f}")
    print(f"  Fit:  {params}")

    # Test 4: Outlier detection for different classes
    print("\n" + "=" * 60)
    print("Test 4: Class-aware outlier detection")

    # Data with one high outlier and one low outlier
    test_data = np.array([0.5, 0.51, 0.49, 0.52, 0.48, 0.50, 0.51, 0.15, 0.85])
    params = analyzer.fit_distribution(test_data)

    print(f"  Test data: {test_data}")
    print(f"  Distribution: {params}")

    for class_type in ['background', 'star_core', 'dark_nebula', 'default']:
        outliers = analyzer.identify_outliers(test_data, params, class_type)
        outlier_vals = test_data[outliers]
        print(f"  {class_type:15s}: outliers = {outlier_vals}")

    print("\n" + "=" * 60)
    print("All tests completed!")
