#!/usr/bin/env python3
"""
Test suite for the Intelligent Pixel Selection System

This module validates the pixel selection logic with synthetic data
that has known correct answers. It tests:
1. Distribution fitting accuracy
2. Class-aware outlier detection
3. Per-class selection strategies
4. Edge cases and failure modes

Author: NukeX Project
"""

import numpy as np
import sys
from typing import Tuple, List
from dataclasses import dataclass

from pixel_stack_analyzer import (
    PixelStackAnalyzer, DistributionParams, DistributionType
)
from pixel_selector import PixelSelector, SelectionResult


@dataclass
class TestCase:
    """A single test case with expected outcome."""
    name: str
    description: str
    values: np.ndarray
    region_class: str
    expected_selection_range: Tuple[float, float]  # (min, max) acceptable
    should_reject_indices: List[int]  # Frames that SHOULD be rejected
    should_preserve_indices: List[int]  # Frames that MUST NOT be rejected


class PixelSelectionTestSuite:
    """Comprehensive test suite for pixel selection."""

    def __init__(self):
        self.analyzer = PixelStackAnalyzer()
        self.selector = PixelSelector()
        self.passed = 0
        self.failed = 0
        self.test_results = []

    def run_all_tests(self) -> bool:
        """Run all test cases and return True if all pass."""
        print("=" * 70)
        print("INTELLIGENT PIXEL SELECTION - TEST SUITE")
        print("=" * 70)

        # Distribution fitting tests
        self._test_gaussian_fitting()
        self._test_bimodal_detection()
        self._test_lognormal_fitting()

        # Outlier detection tests
        self._test_background_outlier_detection()
        self._test_star_core_outlier_detection()
        self._test_dark_nebula_outlier_detection()

        # Selection strategy tests
        self._run_selection_test_cases()

        # Edge case tests
        self._test_edge_cases()

        # Summary
        print("\n" + "=" * 70)
        print("TEST SUMMARY")
        print("=" * 70)
        print(f"  Passed: {self.passed}")
        print(f"  Failed: {self.failed}")
        print(f"  Total:  {self.passed + self.failed}")

        if self.failed > 0:
            print("\nFailed tests:")
            for name, reason in self.test_results:
                if reason:
                    print(f"  - {name}: {reason}")

        return self.failed == 0

    def _assert(self, condition: bool, test_name: str, message: str = ""):
        """Record test result."""
        if condition:
            self.passed += 1
            self.test_results.append((test_name, None))
            print(f"  [PASS] {test_name}")
        else:
            self.failed += 1
            self.test_results.append((test_name, message))
            print(f"  [FAIL] {test_name}: {message}")

    # ==================== DISTRIBUTION FITTING TESTS ====================

    def _test_gaussian_fitting(self):
        """Test Gaussian distribution fitting."""
        print("\n--- Distribution Fitting: Gaussian ---")

        # Generate known Gaussian data
        np.random.seed(42)
        true_mu, true_sigma = 0.5, 0.1
        data = np.random.normal(true_mu, true_sigma, 50)

        params = self.analyzer.fit_distribution(data)

        self._assert(
            params.dist_type == DistributionType.GAUSSIAN,
            "gaussian_type_detection",
            f"Expected GAUSSIAN, got {params.dist_type}"
        )

        self._assert(
            abs(params.mu - true_mu) < 0.05,
            "gaussian_mu_accuracy",
            f"Expected mu~{true_mu}, got {params.mu:.4f}"
        )

        self._assert(
            abs(params.sigma - true_sigma) < 0.03,
            "gaussian_sigma_accuracy",
            f"Expected sigma~{true_sigma}, got {params.sigma:.4f}"
        )

    def _test_bimodal_detection(self):
        """Test bimodal distribution detection."""
        print("\n--- Distribution Fitting: Bimodal ---")

        # Generate clear bimodal data
        np.random.seed(42)
        mode1 = np.random.normal(0.3, 0.03, 20)
        mode2 = np.random.normal(0.7, 0.03, 20)
        bimodal_data = np.concatenate([mode1, mode2])

        params = self.analyzer.fit_distribution(bimodal_data)

        self._assert(
            params.dist_type == DistributionType.BIMODAL,
            "bimodal_detection",
            f"Expected BIMODAL, got {params.dist_type}"
        )

        if params.dist_type == DistributionType.BIMODAL:
            # Check both modes are detected
            detected_modes = sorted([params.mu, params.mu2])
            self._assert(
                abs(detected_modes[0] - 0.3) < 0.1 and abs(detected_modes[1] - 0.7) < 0.1,
                "bimodal_mode_locations",
                f"Expected modes at 0.3, 0.7, got {detected_modes}"
            )

    def _test_lognormal_fitting(self):
        """Test lognormal distribution detection for right-skewed data."""
        print("\n--- Distribution Fitting: Lognormal ---")

        np.random.seed(42)
        lognormal_data = np.random.lognormal(0, 0.5, 50)
        lognormal_data = lognormal_data / np.max(lognormal_data)

        params = self.analyzer.fit_distribution(lognormal_data)

        # Should detect the right skew
        self._assert(
            params.skewness > 0.5,
            "lognormal_skewness_detection",
            f"Expected positive skewness, got {params.skewness:.3f}"
        )

    # ==================== OUTLIER DETECTION TESTS ====================

    def _test_background_outlier_detection(self):
        """Test background outlier detection - should reject high outliers."""
        print("\n--- Outlier Detection: Background ---")

        # Background with a satellite trail (high outlier)
        data = np.array([0.10, 0.11, 0.09, 0.12, 0.10, 0.85, 0.11, 0.10])
        params = self.analyzer.fit_distribution(data)
        outliers = self.analyzer.identify_outliers(data, params, 'background')

        # High outlier (0.85) should be detected
        self._assert(
            outliers[5] == True,
            "background_high_outlier_detected",
            f"High outlier at index 5 should be detected"
        )

        # Normal values should NOT be outliers
        normal_count = np.sum(~outliers) - 1  # Exclude the real outlier
        self._assert(
            normal_count >= 6,
            "background_normals_preserved",
            f"At least 6 normal values should be preserved, got {normal_count}"
        )

    def _test_star_core_outlier_detection(self):
        """Test star core outlier detection - should ONLY reject LOW outliers."""
        print("\n--- Outlier Detection: Star Core ---")

        # Star with one dim frame (low outlier) - high values are REAL!
        data = np.array([0.95, 0.94, 0.25, 0.96, 0.93, 0.95, 0.94, 0.97])
        params = self.analyzer.fit_distribution(data)
        outliers = self.analyzer.identify_outliers(data, params, 'star_core')

        # Low outlier (0.25) should be detected
        self._assert(
            outliers[2] == True,
            "star_low_outlier_detected",
            f"Low outlier (0.25) at index 2 should be detected"
        )

        # HIGH values should NOT be rejected - they are real star signal!
        high_value_idx = np.argmax(data)  # Index of 0.97
        self._assert(
            outliers[high_value_idx] == False,
            "star_high_value_preserved",
            f"High value at index {high_value_idx} should NOT be rejected!"
        )

    def _test_dark_nebula_outlier_detection(self):
        """Test dark nebula outlier detection - should ONLY reject HIGH outliers."""
        print("\n--- Outlier Detection: Dark Nebula (CRITICAL) ---")

        # Dark nebula with contamination (high outlier) - LOW values are REAL!
        data = np.array([0.15, 0.14, 0.16, 0.75, 0.15, 0.13, 0.14, 0.16])
        params = self.analyzer.fit_distribution(data)
        outliers = self.analyzer.identify_outliers(data, params, 'dark_nebula')

        # High outlier (0.75) should be detected
        self._assert(
            outliers[3] == True,
            "dark_nebula_high_outlier_detected",
            f"High outlier (0.75) at index 3 should be detected"
        )

        # LOW values should NOT be rejected - they ARE the dark nebula!
        low_value_idx = np.argmin(data)  # Index of 0.13
        self._assert(
            outliers[low_value_idx] == False,
            "dark_nebula_low_value_preserved_CRITICAL",
            f"Low value (dark nebula signal) at index {low_value_idx} must NOT be rejected!"
        )

    # ==================== SELECTION STRATEGY TESTS ====================

    def _run_selection_test_cases(self):
        """Run predefined test cases for each class."""
        print("\n--- Selection Strategy Tests ---")

        test_cases = [
            TestCase(
                name="background_clean",
                description="Clean background with one satellite",
                values=np.array([0.10, 0.11, 0.09, 0.12, 0.10, 0.85, 0.11, 0.10, 0.09]),
                region_class="background",
                expected_selection_range=(0.08, 0.13),
                should_reject_indices=[5],
                should_preserve_indices=[0, 1, 2, 3, 4, 6, 7, 8]
            ),
            TestCase(
                name="star_core_preserve_peak",
                description="Star core - must preserve peak values",
                values=np.array([0.95, 0.94, 0.25, 0.96, 0.93, 0.95, 0.94, 0.97, 0.95]),
                region_class="star_core",
                expected_selection_range=(0.90, 1.0),  # Should select HIGH value
                should_reject_indices=[2],
                should_preserve_indices=[0, 1, 3, 4, 5, 6, 7, 8]
            ),
            TestCase(
                name="dark_nebula_preserve_darkness",
                description="Dark nebula - MUST preserve low values (CRITICAL)",
                values=np.array([0.15, 0.14, 0.16, 0.75, 0.15, 0.13, 0.14, 0.16, 0.15]),
                region_class="dark_nebula",
                expected_selection_range=(0.12, 0.17),  # Should select LOW value
                should_reject_indices=[3],
                should_preserve_indices=[0, 1, 2, 4, 5, 6, 7, 8]
            ),
            TestCase(
                name="emission_nebula_preserve_signal",
                description="Emission nebula with cloud blocking",
                values=np.array([0.45, 0.48, 0.12, 0.47, 0.46, 0.49, 0.45, 0.48, 0.46]),
                region_class="emission_nebula",
                expected_selection_range=(0.43, 0.52),  # Should preserve emission
                should_reject_indices=[2],
                should_preserve_indices=[0, 1, 3, 4, 5, 6, 7, 8]
            ),
            TestCase(
                name="star_halo_symmetric",
                description="Star halo with outliers on both sides",
                values=np.array([0.35, 0.36, 0.05, 0.34, 0.37, 0.35, 0.90, 0.34, 0.36]),
                region_class="star_halo",
                expected_selection_range=(0.32, 0.40),
                should_reject_indices=[2, 6],
                should_preserve_indices=[0, 1, 3, 4, 5, 7, 8]
            ),
        ]

        for tc in test_cases:
            self._run_single_test_case(tc)

    def _run_single_test_case(self, tc: TestCase):
        """Run a single test case."""
        val, idx, result = self.selector.select_best_value(
            tc.values, region_class=tc.region_class
        )

        # Check selected value is in expected range
        in_range = tc.expected_selection_range[0] <= val <= tc.expected_selection_range[1]
        self._assert(
            in_range,
            f"{tc.name}_value_range",
            f"Selected {val:.4f}, expected in [{tc.expected_selection_range[0]:.2f}, {tc.expected_selection_range[1]:.2f}]"
        )

        # Check rejection count is reasonable
        expected_rejections = len(tc.should_reject_indices)
        self._assert(
            result.rejected_count >= expected_rejections,
            f"{tc.name}_rejection_count",
            f"Expected at least {expected_rejections} rejections, got {result.rejected_count}"
        )

    # ==================== EDGE CASE TESTS ====================

    def _test_edge_cases(self):
        """Test edge cases and failure modes."""
        print("\n--- Edge Cases ---")

        # Test 1: All identical values
        identical_data = np.array([0.5, 0.5, 0.5, 0.5, 0.5])
        val, idx, result = self.selector.select_best_value(identical_data)
        self._assert(
            val == 0.5,
            "identical_values",
            f"Should return 0.5 for identical values, got {val}"
        )

        # Test 2: Single value
        single_data = np.array([0.42])
        val, idx, result = self.selector.select_best_value(single_data)
        self._assert(
            val == 0.42,
            "single_value",
            f"Should return the only value, got {val}"
        )

        # Test 3: Two values
        two_data = np.array([0.3, 0.7])
        val, idx, result = self.selector.select_best_value(two_data)
        self._assert(
            0.25 <= val <= 0.75,
            "two_values",
            f"Should handle two values, got {val}"
        )

        # Test 4: All outliers (extreme scenario)
        extreme_data = np.array([0.01, 0.99, 0.02, 0.98, 0.01])
        val, idx, result = self.selector.select_best_value(extreme_data)
        self._assert(
            0.0 <= val <= 1.0,
            "extreme_bimodal",
            f"Should handle extreme bimodal data, got {val}"
        )

        # Test 5: NaN handling
        nan_data = np.array([0.5, np.nan, 0.52, 0.48, np.nan, 0.51])
        params = self.analyzer.fit_distribution(nan_data)
        self._assert(
            np.isfinite(params.mu),
            "nan_handling",
            f"Should handle NaN values gracefully, mu={params.mu}"
        )

        # Test 6: Zero variance
        zero_var_data = np.array([0.5, 0.5, 0.5, 0.50001, 0.49999])
        params = self.analyzer.fit_distribution(zero_var_data)
        self._assert(
            params.sigma >= 0,
            "zero_variance",
            f"Should handle near-zero variance, sigma={params.sigma}"
        )


def run_specific_test(test_name: str = None):
    """Run specific test or all tests."""
    suite = PixelSelectionTestSuite()

    if test_name == "dark_nebula":
        print("\n" + "=" * 70)
        print("FOCUSED TEST: Dark Nebula Selection")
        print("=" * 70)
        suite._test_dark_nebula_outlier_detection()

        # Additional detailed dark nebula test
        print("\n--- Detailed Dark Nebula Selection ---")
        selector = PixelSelector()

        # Case 1: Classic dark nebula with one contaminated frame
        print("\nCase 1: Dark nebula with satellite contamination")
        data = np.array([0.15, 0.14, 0.16, 0.75, 0.15, 0.13, 0.14, 0.16, 0.15])
        val, idx, result = selector.select_best_value(data, region_class='dark_nebula')
        print(f"  Data: {data}")
        print(f"  Selected: {val:.4f} from frame {idx}")
        print(f"  Expected: ~0.13-0.16 (dark signal)")
        print(f"  PASS" if 0.12 <= val <= 0.17 else "  FAIL - Wrong selection!")

        # Case 2: Very dark nebula with multiple contaminations
        print("\nCase 2: Very dark nebula with multiple contaminations")
        data2 = np.array([0.05, 0.06, 0.55, 0.05, 0.04, 0.06, 0.45, 0.05, 0.06])
        val2, idx2, result2 = selector.select_best_value(data2, region_class='dark_nebula')
        print(f"  Data: {data2}")
        print(f"  Selected: {val2:.4f} from frame {idx2}")
        print(f"  Expected: ~0.04-0.06 (very dark signal)")
        print(f"  PASS" if 0.03 <= val2 <= 0.08 else "  FAIL - Wrong selection!")

    else:
        suite.run_all_tests()


if __name__ == '__main__':
    if len(sys.argv) > 1:
        run_specific_test(sys.argv[1])
    else:
        suite = PixelSelectionTestSuite()
        success = suite.run_all_tests()
        sys.exit(0 if success else 1)
