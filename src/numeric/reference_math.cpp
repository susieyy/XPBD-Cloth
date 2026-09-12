#include "reference_math.h"

#include <algorithm>
#include <cmath>

namespace xpbd::numeric {

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator-(Vec3 value) { return {-value.x, -value.y, -value.z}; }
Vec3 operator*(Vec3 value, double scale) {
  return {value.x * scale, value.y * scale, value.z * scale};
}
Vec3 operator*(double scale, Vec3 value) { return value * scale; }
Vec3 operator/(Vec3 value, double scale) { return value * (1.0 / scale); }

double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) {
  return {
      a.y * b.z - a.z * b.y,
      a.z * b.x - a.x * b.z,
      a.x * b.y - a.y * b.x,
  };
}
double length(Vec3 value) { return std::sqrt(dot(value, value)); }

namespace {

std::optional<Vec3> normalized(Vec3 value) {
  const double value_length = length(value);
  if (!std::isfinite(value_length) || value_length < 1e-8) return std::nullopt;
  return value / value_length;
}

Vec3& component(std::array<Vec3, 4>& values, std::size_t index) {
  return values[index];
}

double& component(Vec3& value, std::size_t axis) {
  if (axis == 0) return value.x;
  if (axis == 1) return value.y;
  return value.z;
}

}  // namespace

double donor_area_residual(
    Vec3 p0, Vec3 p1, Vec3 p2, double rest_area, Vec3 rest_normal) {
  return 0.5 * dot(cross(p1 - p0, p2 - p0), rest_normal) - rest_area;
}

std::array<Vec3, 3> donor_area_gradients(
    Vec3 p0, Vec3 p1, Vec3 p2, Vec3 rest_normal) {
  const Vec3 e1 = p1 - p0;
  const Vec3 e2 = p2 - p0;
  const Vec3 g1 = 0.5 * cross(rest_normal, e2);
  const Vec3 g2 = 0.5 * cross(e1, rest_normal);
  return {-g1 - g2, g1, g2};
}

std::optional<double> signed_bend_value(
    Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, double rest_angle) {
  const Vec3 edge = p1 - p0;
  const auto edge_direction = normalized(edge);
  const auto n1 = normalized(cross(edge, p2 - p0));
  const auto n2 = normalized(cross(edge, p3 - p0));
  if (!edge_direction || !n1 || !n2) return std::nullopt;
  const double cosine = std::clamp(dot(*n1, *n2), -1.0, 1.0);
  const double sine = dot(*edge_direction, cross(*n1, *n2));
  return std::atan2(sine, cosine) - rest_angle;
}

std::optional<std::array<Vec3, 4>> donor_bend_gradients(
    Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3) {
  const Vec3 edge = p1 - p0;
  const double a1 = length(cross(edge, p2 - p0));
  const double a2 = length(cross(edge, p3 - p0));
  const auto n1 = normalized(cross(edge, p2 - p0));
  const auto n2 = normalized(cross(edge, p3 - p0));
  if (length(edge) < 1e-8 || a1 < 1e-8 || a2 < 1e-8 || !n1 || !n2) {
    return std::nullopt;
  }
  const double cosine = std::clamp(dot(*n1, *n2), -1.0, 1.0);
  const Vec3 q2 = (cross(edge, *n2) + cross(*n1, edge) * cosine) / a1;
  const Vec3 q3 = (cross(edge, *n1) + cross(*n2, edge) * cosine) / a2;
  const Vec3 q1 =
      -(cross(p2 - p0, *n2) + cross(*n1, p2 - p0) * cosine) / a1
      -(cross(p3 - p0, *n1) + cross(*n2, p3 - p0) * cosine) / a2;
  const Vec3 q0 = -q1 - q2 - q3;
  return std::array<Vec3, 4>{q0, q1, q2, q3};
}

std::optional<std::array<Vec3, 4>> finite_difference_bend_gradients(
    Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, double rest_angle, double epsilon) {
  if (!(epsilon > 0)) return std::nullopt;
  const std::array<Vec3, 4> input{p0, p1, p2, p3};
  std::array<Vec3, 4> result{};
  for (std::size_t vertex = 0; vertex < input.size(); ++vertex) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      auto plus = input;
      auto minus = input;
      component(component(plus, vertex), axis) += epsilon;
      component(component(minus, vertex), axis) -= epsilon;
      const auto plus_value = signed_bend_value(
          plus[0], plus[1], plus[2], plus[3], rest_angle);
      const auto minus_value = signed_bend_value(
          minus[0], minus[1], minus[2], minus[3], rest_angle);
      if (!plus_value || !minus_value) return std::nullopt;
      component(component(result, vertex), axis) =
          (*plus_value - *minus_value) / (2.0 * epsilon);
    }
  }
  return result;
}

std::array<Vec3, 3> donor_wind_delta_velocity(
    const std::array<Vec3, 3>& positions,
    const std::array<Vec3, 3>& velocities,
    const std::array<double, 3>& inverse_masses,
    Vec3 wind_direction,
    double wind_force,
    double air_density,
    double drag_coefficient,
    double lift_coefficient,
    double delta_time) {
  std::array<Vec3, 3> result{};
  const Vec3 face_normal = cross(positions[1] - positions[0], positions[2] - positions[0]);
  const double area2 = length(face_normal);
  const auto wind_normal = normalized(wind_direction);
  if (area2 < 1e-8 || !wind_normal) return result;
  const Vec3 normal = face_normal / area2;
  const double area = 0.5 * area2;
  const Vec3 air_velocity = *wind_normal * wind_force;
  const Vec3 triangle_velocity = (velocities[0] + velocities[1] + velocities[2]) / 3.0;
  const Vec3 relative = air_velocity - triangle_velocity;
  const double speed_squared = dot(relative, relative);
  if (speed_squared < 1e-10) return result;
  const double normal_speed = dot(relative, normal);
  const Vec3 facing_normal = normal * (normal_speed >= 0 ? 1.0 : -1.0);
  const Vec3 force = 0.5 * air_density * area *
      ((drag_coefficient - lift_coefficient) * std::abs(normal_speed) * relative
       + lift_coefficient * speed_squared * facing_normal);
  const Vec3 shared_force = force / 3.0;
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = delta_time * inverse_masses[index] * shared_force;
  }
  return result;
}

double maximum_l2_error(
    const std::array<Vec3, 4>& lhs, const std::array<Vec3, 4>& rhs) {
  double maximum = 0;
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    maximum = std::max(maximum, length(lhs[index] - rhs[index]));
  }
  return maximum;
}

}  // namespace xpbd::numeric
