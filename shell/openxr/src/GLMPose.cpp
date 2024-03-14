#include "GLMPose.h"
#include <cstring>

#ifndef deg2rad
#define deg2rad(a)  ((a)*(M_PI/180))
#endif
#ifndef rad2deg
#define rad2deg(a)  ((a)*(180/M_PI))
#endif

namespace igl::shell::openxr {

XrVector3f convert_to_xr(const glm::vec3 &input) {
    XrVector3f output;
    std::memcpy(&output, &input, sizeof(output));
    return output;
}

glm::vec3 convert_to_glm(const XrVector3f &input) {
    glm::vec3 output;
    output.x = input.x;
    output.y = input.y;
    output.z = input.z;
    return output;
}

XrQuaternionf convert_to_xr(const glm::fquat &input) {
    XrQuaternionf output;
    output.x = input.x;
    output.y = input.y;
    output.z = input.z;
    output.w = input.w;
    return output;
}

glm::fquat convert_to_glm(const XrQuaternionf &input) {
    glm::fquat output;
    output.x = input.x;
    output.y = input.y;
    output.z = input.z;
    output.w = input.w;
    return output;
}

glm::mat4 convert_to_rotation_matrix(const glm::fquat &rotation) {
    glm::mat4 rotation_matrix = glm::mat4_cast(rotation);
    return rotation_matrix;
}

// GLMPose

void GLMPose::clear() {
    translation_ = glm::vec3(0.0f, 0.0f, 0.0f);
    rotation_ = default_rotation;
    scale_ = glm::vec3(1.0f, 1.0f, 1.0f);
    euler_angles_degrees_ = glm::vec3(0.0f, 0.0f, 0.0f);
}

glm::mat4 GLMPose::to_matrix() const {
    glm::mat4 translation_matrix = glm::translate(glm::mat4(1), translation_);
    glm::mat4 rotation_matrix = glm::mat4_cast(rotation_);
    glm::mat4 scale_matrix = glm::scale(scale_);
    return translation_matrix * rotation_matrix * scale_matrix;
}

void GLMPose::update_rotation_from_euler() {
    glm::vec3 euler_angles_radians(deg2rad(euler_angles_degrees_.x),
                                   deg2rad(euler_angles_degrees_.y),
                                   deg2rad(euler_angles_degrees_.z));
    rotation_ = glm::fquat(euler_angles_radians);
}


void GLMPose::transform(const GLMPose &glm_pose) {
    glm::vec3 rotated_translation = rotation_ * glm_pose.translation_;
    translation_ += rotated_translation;
    rotation_ = glm::normalize(rotation_ * glm_pose.rotation_);
}

GLMPose convert_to_glm(const XrVector3f &position, const XrQuaternionf &rotation,
                       const XrVector3f &scale) {
    GLMPose glm_pose;
    glm_pose.translation_ = convert_to_glm(position);
    glm_pose.rotation_ = convert_to_glm(rotation);
    glm_pose.scale_ = convert_to_glm(scale);
    return glm_pose;
}

GLMPose convert_to_glm_pose(const XrPosef &xr_pose) {
    GLMPose glm_pose;
    glm_pose.translation_ = convert_to_glm(xr_pose.position);
    glm_pose.rotation_ = convert_to_glm(xr_pose.orientation);
    return glm_pose;
}

XrPosef convert_to_xr_pose(const GLMPose &glm_pose) {
    // No scale
    XrPosef xr_pose;
    xr_pose.position = convert_to_xr(glm_pose.translation_);
    xr_pose.orientation = convert_to_xr(glm_pose.rotation_);
    return xr_pose;
}

} // namespace igl::shell::openxr

