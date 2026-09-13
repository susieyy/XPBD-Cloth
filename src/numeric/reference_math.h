#pragma once

#include <array>
#include <cstddef>
#include <optional>

namespace xpbd::numeric {

struct Vec3 {
  double x = 0;
  double y = 0;
  double z = 0;
};

Vec3 operator+(Vec3 a, Vec3 b);
Vec3 operator-(Vec3 a, Vec3 b);
Vec3 operator-(Vec3 value);
Vec3 operator*(Vec3 value, double scale);
Vec3 operator*(double scale, Vec3 value);
Vec3 operator/(Vec3 value, double scale);

double dot(Vec3 a, Vec3 b);
Vec3 cross(Vec3 a, Vec3 b);
double length(Vec3 value);

double donor_area_residual(
    Vec3 p0, Vec3 p1, Vec3 p2, double rest_area, Vec3 rest_normal);
std::array<Vec3, 3> donor_area_gradients(
    Vec3 p0, Vec3 p1, Vec3 p2, Vec3 rest_normal);

std::optional<double> signed_bend_value(
    Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, double rest_angle);
std::optional<std::array<Vec3, 4>> donor_bend_gradients(
    Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3);
std::optional<std::array<Vec3, 4>> finite_difference_bend_gradients(
    Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, double rest_angle, double epsilon);

std::array<Vec3, 3> donor_wind_delta_velocity(
    const std::array<Vec3, 3>& positions,
    const std::array<Vec3, 3>& velocities,
    const std::array<double, 3>& inverse_masses,
    Vec3 wind_direction,
    double wind_force,
    double air_density,
    double drag_coefficient,
    double lift_coefficient,
    double delta_time);

double maximum_l2_error(
    const std::array<Vec3, 4>& lhs, const std::array<Vec3, 4>& rhs);

}  // namespace xpbd::numeric
