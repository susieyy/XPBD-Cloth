#include "reference_math.h"

#include <cmath>
#include <iostream>

namespace {

int failures = 0;

void expect_near(double actual, double expected, double tolerance, const char* message) {
  if (std::abs(actual - expected) <= tolerance) return;
  std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
  ++failures;
}

void expect_true(bool condition, const char* message) {
  if (condition) return;
  std::cerr << message << '\n';
  ++failures;
}

}  // namespace

int main() {
  using namespace xpbd::numeric;
  expect_near(
      donor_area_residual({0, 0, 0}, {0, 0, -1}, {0, 1, 0}, 0.5, {0, 0, 1}),
      -0.5, 1e-12,
      "donor fixed-normal area must expose the rigid-rotation discrepancy");

  const auto source = donor_bend_gradients(
      {0, 0, 0}, {1, 0, 0}, {0.2, 0.8, 0.1}, {0.3, -0.2, 0.9});
  const auto finite = finite_difference_bend_gradients(
      {0, 0, 0}, {1, 0, 0}, {0.2, 0.8, 0.1}, {0.3, -0.2, 0.9}, 0.2, 1e-6);
  expect_true(source.has_value() && finite.has_value(), "bend fixture must be non-degenerate");
  if (source && finite) {
    const double error = maximum_l2_error(*source, *finite);
    expect_true(error > 1e-4, "donor bend gradient discrepancy must remain observable");
    expect_true(error < 0.1, "bend fixture must remain in the intended numeric regime");
  }

  const auto wind = donor_wind_delta_velocity(
      {{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}},
      {{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}},
      {{1, 1, 1}}, {0, 0, 1}, 1, 1.2, 4, 5, 1.0 / 600.0);
  expect_near(wind[0].z, 1.0 / 1500.0, 1e-12,
              "donor aerodynamic impulse must match the shader fixture");
  expect_near(wind[1].z, wind[0].z, 1e-12, "wind impulse must be shared equally");
  expect_near(wind[2].z, wind[0].z, 1e-12, "wind impulse must be shared equally");

  return failures == 0 ? 0 : 1;
}
