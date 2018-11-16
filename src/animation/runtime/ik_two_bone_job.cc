//----------------------------------------------------------------------------//
//                                                                            //
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//                                                                            //
// Copyright (c) 2017 Guillaume Blanc                                         //
//                                                                            //
// Permission is hereby granted, free of charge, to any person obtaining a    //
// copy of this software and associated documentation files (the "Software"), //
// to deal in the Software without restriction, including without limitation  //
// the rights to use, copy, modify, merge, publish, distribute, sublicense,   //
// and/or sell copies of the Software, and to permit persons to whom the      //
// Software is furnished to do so, subject to the following conditions:       //
//                                                                            //
// The above copyright notice and this permission notice shall be included in //
// all copies or substantial portions of the Software.                        //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR //
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   //
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    //
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER //
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    //
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        //
// DEALINGS IN THE SOFTWARE.                                                  //
//                                                                            //
//----------------------------------------------------------------------------//

#include "ozz/animation/runtime/ik_two_bone_job.h"

#include <cassert>

#include "ozz/base/log.h"
#include "ozz/base/maths/math_ex.h"
#include "ozz/base/maths/simd_quaternion.h"

using namespace ozz::math;

namespace ozz {
namespace animation {
IKTwoBoneJob::IKTwoBoneJob()
    : handle(math::simd_float4::zero()),
      pole_vector(math::simd_float4::y_axis()),
      mid_axis(math::simd_float4::z_axis()),
      weight(1.f),
      soften(1.f),
      twist_angle(0.f),
      start_joint(NULL),
      mid_joint(NULL),
      end_joint(NULL),
      start_joint_correction(NULL),
      mid_joint_correction(NULL) {}

bool IKTwoBoneJob::Validate() const {
  bool valid = true;
  valid &= start_joint && mid_joint && end_joint;
  valid &= start_joint_correction && mid_joint_correction;
  valid &= ozz::math::AreAllTrue1(ozz::math::IsNormalizedEst3(mid_axis));
  return valid;
}

namespace {

// Local data structure used to share constant data accross ik stages.
struct IKConstantSetup {
  IKConstantSetup(const IKTwoBoneJob& _job) {
    // Prepares constants
    one = simd_float4::one();
    mask_sign = simd_int4::mask_sign();
    m_one = Xor(one, mask_sign);

    // Computes inverse matrices required to change to start and mid spaces.
    inv_start_joint = Invert(*_job.start_joint);
    const Float4x4 inv_mid_joint = Invert(*_job.mid_joint);

    // Transform some positions to mid joint space (_ms)
    const SimdFloat4 start_ms =
        TransformPoint(inv_mid_joint, _job.start_joint->cols[3]);
    const SimdFloat4 end_ms =
        TransformPoint(inv_mid_joint, _job.end_joint->cols[3]);

    // Transform some positions to start joint space (_ss)
    const SimdFloat4 mid_ss =
        TransformPoint(inv_start_joint, _job.mid_joint->cols[3]);
    const SimdFloat4 end_ss =
        TransformPoint(inv_start_joint, _job.end_joint->cols[3]);

    // Computes bones vectors and length in mid and start spaces.
    // Start joint position will be treated as 0 because all joints are
    // expressed in start joint space.
    start_mid_ms = -start_ms;
    mid_end_ms = end_ms;
    start_mid_ss = mid_ss;
    const SimdFloat4 mid_end_ss = end_ss - mid_ss;
    const SimdFloat4 start_end_ss = end_ss;
    start_mid_ss_len2 = Length3Sqr(start_mid_ss);
    mid_end_ss_len2 = Length3Sqr(mid_end_ss);
    start_end_ss_len2 = Length3Sqr(start_end_ss);
  }

  // Constants
  SimdFloat4 one;
  SimdFloat4 m_one;
  SimdInt4 mask_sign;

  // Inverse matrices
  Float4x4 inv_start_joint;

  // Bones vectors and length in mid and start spaces (_ms and _ss).
  SimdFloat4 start_mid_ms;
  SimdFloat4 mid_end_ms;
  SimdFloat4 start_mid_ss;
  SimdFloat4 start_mid_ss_len2;
  SimdFloat4 mid_end_ss_len2;
  SimdFloat4 start_end_ss_len2;
};

// Smoothen handle position when it's further that a ratio of the joint chain
// length, and start to handle length isn't 0.
// Inspired from http://www.softimageblog.com/archives/108
bool SoftenHandle(const IKTwoBoneJob& _job, const IKConstantSetup& _setup,
                  SimdFloat4* _start_handle_ss,
                  SimdFloat4* _start_handle_ss_len2) {
  // Hanlde position in start joint space (_ss)
  const SimdFloat4 handle_ss =
      TransformPoint(_setup.inv_start_joint, _job.handle);

  const SimdFloat4& start_handle_original_ss = handle_ss;
  const SimdFloat4 start_handle_original_ss_len2 = Length3Sqr(handle_ss);

  const SimdFloat4 bones_len =
      Sqrt(SetY(_setup.start_mid_ss_len2, _setup.mid_end_ss_len2));
  const SimdFloat4 bones_chain_len = bones_len + SplatY(bones_len);
  const SimdFloat4 da =  // da.yzw needs to be 0
      bones_chain_len * simd_float4::LoadX(Clamp(_job.soften, 0.f, 1.f));
  const SimdFloat4 ds = bones_chain_len - da;

  // Sotftens handle position if it is further than a ratio (_soften) of the
  // whole bone chain length. Needs to check also ds and
  // start_handle_original_ss_len2 are != 0, because they're used as a
  // denominator. Note that da.yzw == 0
  const SimdFloat4 comperand = SetZ(SplatX(start_handle_original_ss_len2), ds);
  bool needs_softening = AreAllTrue3(CmpGt(comperand, da * da));
  if (needs_softening) {
    // Finds interpolation ratio (aka alpha).
    const SimdFloat4 start_handle_original_ss_inv_len =
        RSqrtEstX(start_handle_original_ss_len2);
    const SimdFloat4 start_handle_original_ss_len =  //  x^.5 = x^2 / (x^2)^.5
        start_handle_original_ss_len2 * start_handle_original_ss_inv_len;
    const SimdFloat4 alpha = (start_handle_original_ss_len - da) * RcpEstX(ds);
    // Approximate an exponential function with : 1-(3^4)/(alpha+3)^4
    // The derivative must be 1 for x = 0, and y must never exceeds 1.
    // Negative x aren't used.
    const SimdFloat4 three = simd_float4::Load1(3.f);
    const SimdFloat4 op = SetY(three, alpha + three);
    const SimdFloat4 op2 = op * op;
    const SimdFloat4 op4 = op2 * op2;
    const SimdFloat4 ratio = op4 * RcpEstX(SplatY(op4));

    // Recomputes start_handle_ss vector and length.
    const SimdFloat4 start_handle_ss_len = da + ds - ds * ratio;
    *_start_handle_ss_len2 = start_handle_ss_len * start_handle_ss_len;
    *_start_handle_ss =
        start_handle_original_ss *
        SplatX(start_handle_ss_len * start_handle_original_ss_inv_len);
  } else {
    *_start_handle_ss = start_handle_original_ss;
    *_start_handle_ss_len2 = start_handle_original_ss_len2;
  }

  // If handle position is softened, then it means that the real handle isn't
  // reached.
  return !needs_softening;
}

SimdQuaternion ComputeMidJoint(const IKTwoBoneJob& _job,
                               const IKConstantSetup& _setup,
                               _SimdFloat4 _start_handle_ss_len2) {
  // Computes expected angle at mid_ss joint, using law of cosine (generalized
  // Pythagorean).
  // c^2 = a^2 + b^2 - 2ab cosC
  // cosC = (a^2 + b^2 - c^2) / 2ab
  // Computes both corrected and initial mid joint angles
  // cosine within a single SimdFloat4 (corrected is x component, initial is y).
  const SimdFloat4 start_mid_end_sum_ss_len2 =
      _setup.start_mid_ss_len2 + _setup.mid_end_ss_len2;
  const SimdFloat4 start_mid_end_ss_half_rlen =
      SplatX(simd_float4::Load1(.5f) *
             RSqrtEstXNR(_setup.start_mid_ss_len2 * _setup.mid_end_ss_len2));
  // Cos value needs to be clamped, as it will exit expected range if
  // start_handle_ss_len2 is longer than the triangle can be (start_mid_ss +
  // mid_end_ss).
  const SimdFloat4 mid_cos_angles_unclamped =
      (SplatX(start_mid_end_sum_ss_len2) -
       SetY(_start_handle_ss_len2, _setup.start_end_ss_len2)) *
      start_mid_end_ss_half_rlen;
  const SimdFloat4 mid_cos_angles =
      Clamp(_setup.m_one, mid_cos_angles_unclamped, _setup.one);

  // Computes corrected angle
  const SimdFloat4 mid_corrected_angle = ACosX(mid_cos_angles);

  // Computes initial angle.
  // The sign of this angle needs to be decided. It's considered negative if
  // mid-to-end joint is bent backward (mid_axis direction dictates valid
  // bent direction).
  const SimdFloat4 bent_side_ref = Cross3(_setup.start_mid_ms, _job.mid_axis);
  const SimdInt4 bent_side_flip = SplatX(
      CmpLt(Dot3(bent_side_ref, _setup.mid_end_ms), simd_float4::zero()));
  const SimdFloat4 mid_initial_angle =
      Xor(ACosX(SplatY(mid_cos_angles)), And(bent_side_flip, _setup.mask_sign));

  // Finally deduces initial to corrected angle difference.
  const SimdFloat4 mid_angles_diff = mid_corrected_angle - mid_initial_angle;

  // Builds queternion.
  return SimdQuaternion::FromAxisAngle(_job.mid_axis, mid_angles_diff);
}

SimdQuaternion ComputeStartJoint(const IKTwoBoneJob& _job,
                                 const IKConstantSetup& _setup,
                                 const SimdQuaternion& _mid_rot_ms,
                                 _SimdFloat4 _start_handle_ss,
                                 _SimdFloat4 _start_handle_ss_len2) {
  // Pole vector in start joint space (_ss)
  const SimdFloat4 pole_ss =
      TransformVector(_setup.inv_start_joint, _job.pole_vector);

  // start_mid_ss with quaternion mid_rot_ms applied.
  const SimdFloat4 mid_end_ss_final = TransformVector(
      _setup.inv_start_joint,
      TransformVector(*_job.mid_joint,
                      TransformVector(_mid_rot_ms, _setup.mid_end_ms)));
  const SimdFloat4 start_end_ss_final = _setup.start_mid_ss + mid_end_ss_final;

  // Quaternion for rotating the effector onto the handle
  const SimdQuaternion end_to_handle_rot_ss =
      SimdQuaternion::FromVectors(start_end_ss_final, _start_handle_ss);

  // Calculates rotate_plane_rot quaternion which aligns joint chain plane to
  // the reference plane (pole vector). This can only be computed if start
  // handle axis is valid (not 0 length)
  // -------------------------------------------------
  SimdQuaternion start_rot_ss = end_to_handle_rot_ss;
  if (AreAllTrue1(CmpGt(_start_handle_ss_len2, simd_float4::zero()))) {
    // Computes each plane normal.
    const ozz::math::SimdFloat4 ref_plane_normal_ss =
        Cross3(_start_handle_ss, pole_ss);
    const ozz::math::SimdFloat4 ref_plane_normal_ss_len2 =
        ozz::math::Length3Sqr(ref_plane_normal_ss);
    // Computes joint chain plane normal, which is the same as mid joint axis
    // (same triangle).
    const ozz::math::SimdFloat4 mid_axis_ss =
        TransformVector(_setup.inv_start_joint,
                        TransformVector(*_job.mid_joint, _job.mid_axis));
    const ozz::math::SimdFloat4 joint_plane_normal_ss =
        TransformVector(end_to_handle_rot_ss, mid_axis_ss);
    const ozz::math::SimdFloat4 joint_plane_normal_ss_len2 =
        ozz::math::Length3Sqr(joint_plane_normal_ss);
    // Computes all reciprocal square roots at once.
    const SimdFloat4 rsqrts =
        RSqrtEstNR(SetZ(SetY(_start_handle_ss_len2, ref_plane_normal_ss_len2),
                        joint_plane_normal_ss_len2));

    // Computes angle cosine between the 2 normalized normals.
    const SimdFloat4 rotate_plane_cos_angle =
        ozz::math::Dot3(ref_plane_normal_ss * SplatY(rsqrts),
                        joint_plane_normal_ss * SplatZ(rsqrts));

    // Computes rotation axis, which is either start_handle_ss or
    // -start_handle_ss depending on rotation direction.
    const SimdFloat4 rotate_plane_axis_ss = _start_handle_ss * SplatX(rsqrts);
    const SimdFloat4 start_axis_flip =
        And(SplatX(Dot3(joint_plane_normal_ss, pole_ss)), _setup.mask_sign);
    const SimdFloat4 rotate_plane_axis_flipped_ss =
        Xor(rotate_plane_axis_ss, start_axis_flip);

    // Builds quaternion along rotation axis.
    const SimdQuaternion rotate_plane_ss = SimdQuaternion::FromAxisCosAngle(
        rotate_plane_axis_flipped_ss,
        Clamp(_setup.m_one, rotate_plane_cos_angle, _setup.one));

    if (_job.twist_angle != 0.f) {
      // If a twist angle is provided, rotation angle is rotated along
      // rotation plane axis.
      const SimdQuaternion twist_ss = SimdQuaternion::FromAxisAngle(
          rotate_plane_axis_ss, simd_float4::Load1(_job.twist_angle));
      start_rot_ss = twist_ss * rotate_plane_ss * end_to_handle_rot_ss;
    } else {
      start_rot_ss = rotate_plane_ss * end_to_handle_rot_ss;
    }
  }
  return start_rot_ss;
}

void WeightOutput(const IKTwoBoneJob& _job, const IKConstantSetup& _setup,
                  const SimdQuaternion& _start_rot,
                  const SimdQuaternion& _mid_rot) {
  const SimdFloat4 zero = simd_float4::zero();

  // Fix up quaternions so w is always positive, which is required for NLerp
  // (with identity quaternion) to lerp the shortest path.
  const SimdFloat4 _start_rot_fu =
      Xor(_start_rot.xyzw,
          And(_setup.mask_sign, CmpLt(SplatW(_start_rot.xyzw), zero)));
  const SimdFloat4 _mid_rot_fu = Xor(
      _mid_rot.xyzw, And(_setup.mask_sign, CmpLt(SplatW(_mid_rot.xyzw), zero)));

  if (_job.weight < 1.f) {
    // NLerp start and mid joint rotations.
    const SimdFloat4 identity = simd_float4::w_axis();
    const SimdFloat4 simd_weight =
        simd_float4::Load1(Clamp(0.f, _job.weight, 1.f));
    _job.start_joint_correction->xyzw =
        NormalizeEst4(Lerp(identity, _start_rot_fu, simd_weight));
    _job.mid_joint_correction->xyzw =
        NormalizeEst4(Lerp(identity, _mid_rot_fu, simd_weight));
  } else {
    // Quatenions don't need interpolation
    *_job.start_joint_correction = _start_rot;
    *_job.mid_joint_correction = _mid_rot;
  }
}
}  // namespace

bool IKTwoBoneJob::Run() const {
  if (!Validate()) {
    return false;
  }

  // Early out if weight is 0.
  if (weight <= 0.f) {
    *start_joint_correction = *mid_joint_correction =
        SimdQuaternion::identity();
    return true;
  }

  // Prepares constant ik data.
  const IKConstantSetup setup(*this);

  // Finds soften handle position.
  SimdFloat4 start_handle_ss;
  SimdFloat4 start_handle_ss_len2;
  SoftenHandle(*this, setup, &start_handle_ss, &start_handle_ss_len2);

  // Calculate mid_rot_local quaternion which solves for the mid_ss joint
  // rotation.
  const SimdQuaternion mid_rot_ms =
      ComputeMidJoint(*this, setup, start_handle_ss_len2);

  // Calculates end_to_handle_rot_ss quaternion which solves for effector
  // rotating onto the handle.
  const SimdQuaternion start_rot_ss = ComputeStartJoint(
      *this, setup, mid_rot_ms, start_handle_ss, start_handle_ss_len2);

  // Finally apply weight and output quaternions.
  WeightOutput(*this, setup, start_rot_ss, mid_rot_ms);

  return true;
}
}  // namespace animation
}  // namespace ozz