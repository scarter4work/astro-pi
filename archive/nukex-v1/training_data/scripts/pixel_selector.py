#!/usr/bin/env python3
"""
Pixel Selector - Class-aware pixel value selection from frame stacks

This module implements intelligent pixel selection based on:
1. Statistical distribution of values across the stack
2. ML-derived region classification
3. Per-class selection strategies

Key insight: Different astronomical features require different selection strategies.
A dark nebula pixel should NOT be rejected just because it's "low" - that's real signal!

Author: NukeX Project
"""

import numpy as np
from dataclasses import dataclass
from enum import Enum
from typing import Tuple, Optional, List, Dict, Callable
from pixel_stack_analyzer import (
    DistributionParams, DistributionType, PixelStackAnalyzer
)


class RegionClass(Enum):
    """
    Astronomical region classifications.

    These map to the 21-class segmentation model output.
    """
    BACKGROUND = 0
    STAR_CORE = 1
    STAR_HALO = 2
    BRIGHT_EMISSION = 3
    FAINT_EMISSION = 4
    DARK_NEBULA = 5
    REFLECTION_NEBULA = 6
    GALAXY_CORE = 7
    GALAXY_ARM = 8
    GALAXY_HALO = 9
    DUST_LANE = 10
    PLANETARY_NEBULA = 11
    SUPERNOVA_REMNANT = 12
    HII_REGION = 13
    STAR_FORMING = 14
    GLOBULAR_CLUSTER = 15
    OPEN_CLUSTER = 16
    ARTIFACT = 17
    SATELLITE_TRAIL = 18
    DIFFRACTION_SPIKE = 19
    TRANSITION = 20


@dataclass
class SelectionResult:
    """Result of pixel selection."""
    selected_value: float
    frame_index: int
    confidence: float
    method: str  # Description of selection method used
    rejected_count: int
    reason: str  # Why this value was selected


class SelectionStrategy:
    """
    Base class for selection strategies.

    Each strategy implements a different approach to selecting
    the "best" pixel value from a stack, optimized for a specific
    type of astronomical feature.
    """

    def __init__(self, name: str):
        self.name = name

    def select(self, values: np.ndarray,
               dist_params: DistributionParams,
               outlier_mask: np.ndarray) -> SelectionResult:
        """
        Select the best value from the stack.

        Args:
            values: Pixel values from N frames
            dist_params: Fitted distribution parameters
            outlier_mask: Boolean mask (True = outlier)

        Returns:
            SelectionResult with chosen value and metadata
        """
        raise NotImplementedError


class BackgroundStrategy(SelectionStrategy):
    """
    Strategy for background pixels.

    Goal: Clean, consistent background level
    - Select near median to avoid outliers
    - Reject high outliers aggressively (satellites, gradients)
    - Preserve low values that might indicate faint signal
    """

    def __init__(self):
        super().__init__("background")

    def select(self, values: np.ndarray,
               dist_params: DistributionParams,
               outlier_mask: np.ndarray) -> SelectionResult:

        valid_values = values[~outlier_mask]
        valid_indices = np.where(~outlier_mask)[0]

        if len(valid_values) == 0:
            # Fallback to all values
            selected_value = float(np.median(values))
            frame_idx = int(np.argmin(np.abs(values - selected_value)))
            return SelectionResult(
                selected_value=selected_value,
                frame_index=frame_idx,
                confidence=0.3,
                method="fallback_median",
                rejected_count=len(values),
                reason="All frames rejected, using median"
            )

        # For background, prefer the median of valid values
        # This gives clean, consistent background
        median_val = np.median(valid_values)

        # Find frame closest to median
        distances = np.abs(valid_values - median_val)
        best_idx = np.argmin(distances)
        selected_value = float(valid_values[best_idx])
        frame_idx = int(valid_indices[best_idx])

        confidence = len(valid_values) / len(values)

        return SelectionResult(
            selected_value=selected_value,
            frame_index=frame_idx,
            confidence=confidence,
            method="median_select",
            rejected_count=int(np.sum(outlier_mask)),
            reason=f"Selected value closest to median ({median_val:.4f})"
        )


class StarCoreStrategy(SelectionStrategy):
    """
    Strategy for star core pixels.

    Goal: Preserve peak stellar signal
    - REJECT LOW OUTLIERS ONLY (clouds, bad seeing, etc.)
    - High values are REAL - never reject them!
    - Prefer the highest valid value for proper star profiles
    """

    def __init__(self):
        super().__init__("star_core")

    def select(self, values: np.ndarray,
               dist_params: DistributionParams,
               outlier_mask: np.ndarray) -> SelectionResult:

        # For star cores, we want to be extra careful about
        # only rejecting genuinely bad (low) values
        mu = dist_params.mu
        sigma = dist_params.sigma

        # Only reject values that are suspiciously LOW
        # (bad frames where star was dimmed by clouds, seeing, etc.)
        if sigma > 0:
            z_scores = (values - mu) / sigma
            # Only reject if significantly below mean
            low_outlier_mask = z_scores < -2.5
        else:
            low_outlier_mask = np.zeros_like(outlier_mask, dtype=bool)

        # Combine with provided outlier mask (which should already be low-only for stars)
        combined_mask = outlier_mask | low_outlier_mask

        valid_values = values[~combined_mask]
        valid_indices = np.where(~combined_mask)[0]

        if len(valid_values) == 0:
            # For stars, if somehow all rejected, use MAX (preserve signal)
            selected_value = float(np.max(values))
            frame_idx = int(np.argmax(values))
            return SelectionResult(
                selected_value=selected_value,
                frame_index=frame_idx,
                confidence=0.4,
                method="max_fallback",
                rejected_count=len(values),
                reason="All frames rejected, preserving maximum signal"
            )

        # For star cores, select the HIGHEST valid value
        # This preserves proper star profiles and peak signal
        best_idx = np.argmax(valid_values)
        selected_value = float(valid_values[best_idx])
        frame_idx = int(valid_indices[best_idx])

        confidence = len(valid_values) / len(values)

        return SelectionResult(
            selected_value=selected_value,
            frame_index=frame_idx,
            confidence=confidence,
            method="max_valid",
            rejected_count=int(np.sum(combined_mask)),
            reason=f"Selected highest valid value for star preservation"
        )


class DarkNebulaStrategy(SelectionStrategy):
    """
    Strategy for dark nebula pixels.

    CRITICAL FIX: Dark nebulae are DARK - low values are the SIGNAL!
    - REJECT HIGH OUTLIERS ONLY (satellites, light pollution)
    - Low values are REAL dark nebula absorption - PRESERVE them!
    - Prefer lower values to maintain dark nebula contrast
    """

    def __init__(self):
        super().__init__("dark_nebula")

    def select(self, values: np.ndarray,
               dist_params: DistributionParams,
               outlier_mask: np.ndarray) -> SelectionResult:

        mu = dist_params.mu
        sigma = dist_params.sigma

        # Only reject values that are suspiciously HIGH
        # (contamination, not real dark nebula)
        if sigma > 0:
            z_scores = (values - mu) / sigma
            # Only reject if significantly ABOVE mean
            high_outlier_mask = z_scores > 2.5
        else:
            high_outlier_mask = np.zeros_like(outlier_mask, dtype=bool)

        # Combine with provided outlier mask (which should already be high-only)
        combined_mask = outlier_mask | high_outlier_mask

        valid_values = values[~combined_mask]
        valid_indices = np.where(~combined_mask)[0]

        if len(valid_values) == 0:
            # For dark nebula, use MIN (preserve the dark signal)
            selected_value = float(np.min(values))
            frame_idx = int(np.argmin(values))
            return SelectionResult(
                selected_value=selected_value,
                frame_index=frame_idx,
                confidence=0.4,
                method="min_fallback",
                rejected_count=len(values),
                reason="All frames rejected, preserving minimum (dark) signal"
            )

        # For dark nebula, prefer LOWER valid values
        # This preserves the dark nebula absorption
        # Use 25th percentile to get dark but not extreme
        percentile_25 = np.percentile(valid_values, 25)
        distances = np.abs(valid_values - percentile_25)
        best_idx = np.argmin(distances)
        selected_value = float(valid_values[best_idx])
        frame_idx = int(valid_indices[best_idx])

        confidence = len(valid_values) / len(values)

        return SelectionResult(
            selected_value=selected_value,
            frame_index=frame_idx,
            confidence=confidence,
            method="low_percentile",
            rejected_count=int(np.sum(combined_mask)),
            reason=f"Selected low value to preserve dark nebula absorption"
        )


class EmissionNebulaStrategy(SelectionStrategy):
    """
    Strategy for emission nebula pixels.

    Goal: Preserve faint signal while rejecting contamination
    - Reject very low outliers (clouds blocking signal)
    - Be careful with high values (could be real bright emission)
    - Favor preserving signal over aggressive rejection
    """

    def __init__(self):
        super().__init__("emission_nebula")

    def select(self, values: np.ndarray,
               dist_params: DistributionParams,
               outlier_mask: np.ndarray) -> SelectionResult:

        mu = dist_params.mu
        sigma = dist_params.sigma

        # For emission nebula, we want to preserve signal
        # Reject low outliers (clouds) more aggressively than high
        if sigma > 0:
            z_scores = (values - mu) / sigma
            # Reject low outliers (signal blocked)
            low_mask = z_scores < -2.5
            # Less aggressive on high (could be real bright emission)
            high_mask = z_scores > 3.5
            contam_mask = low_mask | high_mask
        else:
            contam_mask = np.zeros_like(outlier_mask, dtype=bool)

        combined_mask = outlier_mask | contam_mask

        valid_values = values[~combined_mask]
        valid_indices = np.where(~combined_mask)[0]

        if len(valid_values) == 0:
            # Use upper median (favor signal)
            selected_value = float(np.percentile(values, 60))
            distances = np.abs(values - selected_value)
            frame_idx = int(np.argmin(distances))
            return SelectionResult(
                selected_value=selected_value,
                frame_index=frame_idx,
                confidence=0.3,
                method="percentile_fallback",
                rejected_count=len(values),
                reason="All frames rejected, using 60th percentile"
            )

        # For emission nebula, use upper-median approach
        # Slightly favor higher values (more signal)
        percentile_60 = np.percentile(valid_values, 60)
        distances = np.abs(valid_values - percentile_60)
        best_idx = np.argmin(distances)
        selected_value = float(valid_values[best_idx])
        frame_idx = int(valid_indices[best_idx])

        confidence = len(valid_values) / len(values)

        return SelectionResult(
            selected_value=selected_value,
            frame_index=frame_idx,
            confidence=confidence,
            method="upper_percentile",
            rejected_count=int(np.sum(combined_mask)),
            reason=f"Selected from upper distribution to preserve emission signal"
        )


class StarHaloStrategy(SelectionStrategy):
    """
    Strategy for star halo pixels.

    Goal: Preserve smooth gradient around stars
    - Symmetric outlier rejection
    - Use weighted mean to preserve smooth gradients
    - Spatial consistency is important
    """

    def __init__(self):
        super().__init__("star_halo")

    def select(self, values: np.ndarray,
               dist_params: DistributionParams,
               outlier_mask: np.ndarray) -> SelectionResult:

        valid_values = values[~outlier_mask]
        valid_indices = np.where(~outlier_mask)[0]

        if len(valid_values) == 0:
            selected_value = float(np.median(values))
            frame_idx = int(np.argmin(np.abs(values - selected_value)))
            return SelectionResult(
                selected_value=selected_value,
                frame_index=frame_idx,
                confidence=0.3,
                method="median_fallback",
                rejected_count=len(values),
                reason="All frames rejected, using median"
            )

        # For star halos, use weighted mean
        # Weight by inverse distance from median
        median_val = np.median(valid_values)
        distances = np.abs(valid_values - median_val) + 0.001  # Avoid div by zero
        weights = 1.0 / distances
        weights = weights / np.sum(weights)

        weighted_mean = np.sum(valid_values * weights)

        # Find frame closest to weighted mean
        distances = np.abs(valid_values - weighted_mean)
        best_idx = np.argmin(distances)
        selected_value = float(valid_values[best_idx])
        frame_idx = int(valid_indices[best_idx])

        confidence = len(valid_values) / len(values)

        return SelectionResult(
            selected_value=selected_value,
            frame_index=frame_idx,
            confidence=confidence,
            method="weighted_mean",
            rejected_count=int(np.sum(outlier_mask)),
            reason=f"Selected for smooth gradient preservation"
        )


class DefaultStrategy(SelectionStrategy):
    """
    Default strategy for unknown or transition regions.

    Uses median-based selection with symmetric outlier rejection.
    """

    def __init__(self):
        super().__init__("default")

    def select(self, values: np.ndarray,
               dist_params: DistributionParams,
               outlier_mask: np.ndarray) -> SelectionResult:

        valid_values = values[~outlier_mask]
        valid_indices = np.where(~outlier_mask)[0]

        if len(valid_values) == 0:
            selected_value = float(np.median(values))
            frame_idx = int(np.argmin(np.abs(values - selected_value)))
            return SelectionResult(
                selected_value=selected_value,
                frame_index=frame_idx,
                confidence=0.3,
                method="median_fallback",
                rejected_count=len(values),
                reason="All frames rejected, using median"
            )

        median_val = np.median(valid_values)
        distances = np.abs(valid_values - median_val)
        best_idx = np.argmin(distances)
        selected_value = float(valid_values[best_idx])
        frame_idx = int(valid_indices[best_idx])

        confidence = len(valid_values) / len(values)

        return SelectionResult(
            selected_value=selected_value,
            frame_index=frame_idx,
            confidence=confidence,
            method="median_select",
            rejected_count=int(np.sum(outlier_mask)),
            reason=f"Default median selection"
        )


class PixelSelector:
    """
    Main pixel selector that dispatches to class-specific strategies.

    This is the core decision-making engine that selects the "best"
    pixel value from a stack based on the region classification.
    """

    def __init__(self):
        self.analyzer = PixelStackAnalyzer()

        # Strategy registry
        self.strategies: Dict[str, SelectionStrategy] = {
            'background': BackgroundStrategy(),
            'star_core': StarCoreStrategy(),
            'star_halo': StarHaloStrategy(),
            'dark_nebula': DarkNebulaStrategy(),
            'dust_lane': DarkNebulaStrategy(),  # Same as dark nebula
            'emission_nebula': EmissionNebulaStrategy(),
            'bright_emission': EmissionNebulaStrategy(),
            'faint_emission': EmissionNebulaStrategy(),
            'reflection_nebula': EmissionNebulaStrategy(),
            'galaxy_core': StarCoreStrategy(),  # Treat like star cores
            'galaxy_arm': EmissionNebulaStrategy(),
            'default': DefaultStrategy(),
        }

    def select_best_value(self, values: np.ndarray,
                         dist_params: Optional[DistributionParams] = None,
                         region_class: str = 'default') -> Tuple[float, int, SelectionResult]:
        """
        Select the best pixel value from a stack.

        Args:
            values: Array of pixel values from N frames
            dist_params: Pre-computed distribution params (optional)
            region_class: Classification of this pixel's region

        Returns:
            Tuple of (selected_value, frame_index, full_result)
        """
        values = np.asarray(values, dtype=np.float64)

        # Fit distribution if not provided
        if dist_params is None:
            dist_params = self.analyzer.fit_distribution(values)

        # Get class-specific outlier mask
        outlier_mask = self.analyzer.identify_outliers(
            values, dist_params, region_class
        )

        # Get appropriate strategy
        strategy = self.strategies.get(
            region_class.lower(),
            self.strategies['default']
        )

        # Execute selection
        result = strategy.select(values, dist_params, outlier_mask)

        return result.selected_value, result.frame_index, result

    def process_stack(self, stack: np.ndarray,
                     class_map: Optional[np.ndarray] = None) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
        """
        Process an entire image stack.

        Args:
            stack: 3D array (N_frames, height, width)
            class_map: Optional 2D class labels (height, width)

        Returns:
            Tuple of:
            - selected_values: 2D array of selected pixel values
            - frame_indices: 2D array of which frame was selected
            - confidence: 2D array of selection confidence
        """
        n_frames, height, width = stack.shape

        selected_values = np.zeros((height, width), dtype=np.float32)
        frame_indices = np.zeros((height, width), dtype=np.int16)
        confidence = np.zeros((height, width), dtype=np.float32)

        for y in range(height):
            for x in range(width):
                values = stack[:, y, x]

                # Get class for this pixel
                if class_map is not None:
                    class_type = self._class_id_to_name(class_map[y, x])
                else:
                    class_type = 'default'

                # Select best value
                val, idx, result = self.select_best_value(values, region_class=class_type)

                selected_values[y, x] = val
                frame_indices[y, x] = idx
                confidence[y, x] = result.confidence

        return selected_values, frame_indices, confidence

    def _class_id_to_name(self, class_id: int) -> str:
        """Map class ID to class name."""
        class_mapping = {
            0: 'background',
            1: 'star_core',
            2: 'star_halo',
            3: 'bright_emission',
            4: 'faint_emission',
            5: 'dark_nebula',
            6: 'reflection_nebula',
            7: 'galaxy_core',
            8: 'galaxy_arm',
            9: 'galaxy_halo',
            10: 'dust_lane',
            11: 'planetary_nebula',
            12: 'supernova_remnant',
            13: 'hii_region',
            14: 'star_forming',
            15: 'globular_cluster',
            16: 'open_cluster',
            17: 'artifact',
            18: 'satellite_trail',
            19: 'diffraction_spike',
            20: 'transition',
        }
        return class_mapping.get(class_id, 'default')


# Example usage and interactive testing
if __name__ == '__main__':
    print("=" * 70)
    print("Pixel Selector - Class-Aware Selection Demo")
    print("=" * 70)

    selector = PixelSelector()

    # Test case 1: Background with high outlier (satellite)
    print("\n--- Test 1: Background with satellite contamination ---")
    background_data = np.array([0.10, 0.11, 0.09, 0.12, 0.10, 0.11, 0.85, 0.10, 0.09])
    val, idx, result = selector.select_best_value(background_data, region_class='background')
    print(f"  Data: {background_data}")
    print(f"  Selected: {val:.4f} from frame {idx}")
    print(f"  Method: {result.method}")
    print(f"  Reason: {result.reason}")
    print(f"  Rejected: {result.rejected_count} frames")

    # Test case 2: Star core - should preserve high values!
    print("\n--- Test 2: Star core with one dim frame ---")
    star_data = np.array([0.95, 0.94, 0.25, 0.96, 0.93, 0.95, 0.94, 0.92, 0.95])
    val, idx, result = selector.select_best_value(star_data, region_class='star_core')
    print(f"  Data: {star_data}")
    print(f"  Selected: {val:.4f} from frame {idx}")
    print(f"  Method: {result.method}")
    print(f"  Reason: {result.reason}")
    print(f"  Expected: Should select high value (~0.95-0.96), NOT reject them!")

    # Test case 3: Dark nebula - CRITICAL: should preserve LOW values!
    print("\n--- Test 3: Dark nebula with contamination ---")
    dark_data = np.array([0.15, 0.14, 0.16, 0.75, 0.15, 0.13, 0.14, 0.16, 0.15])
    val, idx, result = selector.select_best_value(dark_data, region_class='dark_nebula')
    print(f"  Data: {dark_data}")
    print(f"  Selected: {val:.4f} from frame {idx}")
    print(f"  Method: {result.method}")
    print(f"  Reason: {result.reason}")
    print(f"  Expected: Should select LOW value (~0.13-0.15), NOT reject them!")

    # Test case 4: Emission nebula with faint signal
    print("\n--- Test 4: Emission nebula with cloud pass ---")
    emission_data = np.array([0.45, 0.48, 0.12, 0.47, 0.46, 0.49, 0.45, 0.48, 0.46])
    val, idx, result = selector.select_best_value(emission_data, region_class='emission_nebula')
    print(f"  Data: {emission_data}")
    print(f"  Selected: {val:.4f} from frame {idx}")
    print(f"  Method: {result.method}")
    print(f"  Reason: {result.reason}")
    print(f"  Expected: Should preserve emission signal (~0.45-0.49)")

    # Test case 5: Star halo - gradient preservation
    print("\n--- Test 5: Star halo with outliers ---")
    halo_data = np.array([0.35, 0.36, 0.05, 0.34, 0.37, 0.35, 0.90, 0.34, 0.36])
    val, idx, result = selector.select_best_value(halo_data, region_class='star_halo')
    print(f"  Data: {halo_data}")
    print(f"  Selected: {val:.4f} from frame {idx}")
    print(f"  Method: {result.method}")
    print(f"  Reason: {result.reason}")

    print("\n" + "=" * 70)
    print("All tests completed!")
    print("=" * 70)
