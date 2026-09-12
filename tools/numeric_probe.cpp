#include "reference_math.h"

#include <array>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view kRevision = "c7019d35414e75c77ede2c3500a762837bcab6a6";

void emit(std::string_view case_name, std::string_view metric, double value) {
  std::cout << "{\"schemaVersion\":1,\"sourceRevision\":\"" << kRevision
            << "\",\"implementation\":\"donor-cpu-transcription\",\"case\":\""
            << case_name << "\",\"metric\":\"" << metric << "\",\"value\":"
            << std::setprecision(17) << value << "}\n";
}

}  // namespace

int main() {
  using namespace xpbd::numeric;
  const Vec3 p0{0, 0, 0};
  const Vec3 p1{0, 0, -1};
  const Vec3 p2{0, 1, 0};
  emit("area-rigid-rotation-90", "residual",
       donor_area_residual(p0, p1, p2, 0.5, {0, 0, 1}));

  const Vec3 b0{0, 0, 0};
  const Vec3 b1{1, 0, 0};
  const Vec3 b2{0.2, 0.8, 0.1};
  const Vec3 b3{0.3, -0.2, 0.9};
  const auto source = donor_bend_gradients(b0, b1, b2, b3);
  const auto finite = finite_difference_bend_gradients(b0, b1, b2, b3, 0.2, 1e-6);
  if (!source || !finite) return 2;
  emit("bend-gradient", "maximum-l2-error", maximum_l2_error(*source, *finite));

  const auto wind = donor_wind_delta_velocity(
      {{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}},
      {{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}},
      {{1, 1, 1}}, {0, 0, 1}, 1, 1.2, 4, 5, 1.0 / 600.0);
  emit("wind-single-triangle", "particle-0-delta-v-z", wind[0].z);
  emit("defaults", "substeps", 10);
  emit("defaults", "iterations", 4);
  emit("defaults", "bend-compliance", 500);
  return 0;
}
