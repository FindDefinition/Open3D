// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "open3d/t/pipelines/registration/Registration.h"

#include "open3d/core/Tensor.h"
#include "open3d/core/nns/NearestNeighborSearch.h"
#include "open3d/t/geometry/PointCloud.h"
#include "open3d/utility/Console.h"
#include "open3d/utility/Helper.h"
#include "open3d/utility/Timer.h"

namespace open3d {
namespace t {
namespace pipelines {
namespace registration {

static RegistrationResult GetRegistrationResultAndCorrespondences(
        const geometry::PointCloud &source,
        const geometry::PointCloud &target,
        open3d::core::nns::NearestNeighborSearch &target_nns,
        double max_correspondence_distance,
        const core::Tensor &transformation) {
    core::Device device = source.GetDevice();
    core::Dtype dtype = core::Dtype::Float32;
    source.GetPoints().AssertDtype(dtype);
    target.GetPoints().AssertDtype(dtype);
    if (target.GetDevice() != device) {
        utility::LogError(
                "Target Pointcloud device {} != Source Pointcloud's device {}.",
                target.GetDevice().ToString(), device.ToString());
    }
    transformation.AssertShape({4, 4});
    transformation.AssertDtype(dtype);

    utility::Timer time_GetCorres, time_Search, time_GetResults;
    time_GetCorres.Start();

    core::Tensor transformation_device = transformation.To(device);

    RegistrationResult result(transformation_device);
    if (max_correspondence_distance <= 0.0) {
        return result;
    }

    time_Search.Start();

    bool check = target_nns.HybridIndex(max_correspondence_distance);
    if (!check) {
        utility::LogError(
                "[Tensor: EvaluateRegistration: "
                "GetRegistrationResultAndCorrespondences: "
                "NearestNeighborSearch::HybridSearch] "
                "Index is not set.");
    }

    core::Tensor distances;
    std::tie(result.correspondence_set.first, result.correspondence_set.second,
             distances) =
            target_nns.SqueezedHybridSearch(source.GetPoints(),
                                            max_correspondence_distance);

    time_Search.Stop();
    time_GetCorres.Stop();

    time_GetResults.Start();

    // Number of good correspondences (C).
    int num_correspondences = result.correspondence_set.first.GetShape()[0];

    // Reduction sum of "distances" for error.
    double squared_error =
            static_cast<double>(distances.Sum({0}).Item<float>());
    result.fitness_ = static_cast<double>(num_correspondences) /
                      static_cast<double>(source.GetPoints().GetShape()[0]);
    result.inlier_rmse_ =
            std::sqrt(squared_error / static_cast<double>(num_correspondences));
    result.transformation_ = transformation;

    time_GetResults.Stop();

    utility::LogInfo("       GetCorrespondences: {}",
                     time_GetCorres.GetDuration());
    utility::LogInfo("         Number of Correspondences: {}",
                     result.correspondence_set.first.GetShape()[0]);
    utility::LogInfo("         NNS Search: {}", time_Search.GetDuration());
    utility::LogInfo("       GetResults: {}", time_GetResults.GetDuration());

    return result;
}

RegistrationResult EvaluateRegistration(const geometry::PointCloud &source,
                                        const geometry::PointCloud &target,
                                        double max_correspondence_distance,
                                        const core::Tensor &transformation) {
    core::Device device = source.GetDevice();
    core::Dtype dtype = core::Dtype::Float32;
    source.GetPoints().AssertDtype(dtype);
    target.GetPoints().AssertDtype(dtype);
    if (target.GetDevice() != device) {
        utility::LogError(
                "Target Pointcloud device {} != Source Pointcloud's device {}.",
                target.GetDevice().ToString(), device.ToString());
    }
    transformation.AssertShape({4, 4});
    transformation.AssertDtype(dtype);
    core::Tensor transformation_device = transformation.To(device);

    open3d::core::nns::NearestNeighborSearch target_nns(target.GetPoints());

    geometry::PointCloud source_transformed = source.Clone();
    source_transformed.Transform(transformation_device);
    return GetRegistrationResultAndCorrespondences(
            source_transformed, target, target_nns, max_correspondence_distance,
            transformation_device);
}

RegistrationResult RegistrationICP(const geometry::PointCloud &source,
                                   const geometry::PointCloud &target,
                                   double max_correspondence_distance,
                                   const core::Tensor &init,
                                   const TransformationEstimation &estimation,
                                   const ICPConvergenceCriteria &criteria) {
    core::Device device = source.GetDevice();
    core::Dtype dtype = core::Dtype::Float32;
    source.GetPoints().AssertDtype(dtype);
    target.GetPoints().AssertDtype(dtype);
    if (target.GetDevice() != device) {
        utility::LogError(
                "Target Pointcloud device {} != Source Pointcloud's device {}.",
                target.GetDevice().ToString(), device.ToString());
    }
    init.AssertShape({4, 4});
    init.AssertDtype(dtype);
    core::Tensor transformation_device = init.To(device);

    open3d::core::nns::NearestNeighborSearch target_nns(target.GetPoints());
    geometry::PointCloud source_transformed = source.Clone();
    source_transformed.Transform(transformation_device);

    // TODO: Default constructor absent in RegistrationResult class.
    RegistrationResult result(transformation_device);

    utility::Timer time_getCorres;
    time_getCorres.Start();

    result = GetRegistrationResultAndCorrespondences(
            source_transformed, target, target_nns, max_correspondence_distance,
            transformation_device);
    CorrespondenceSet corres = result.correspondence_set;

    time_getCorres.Stop();
    // Correspondence Search computed in current iteration is used in next
    // iteration.
    double getCorresTimeNew = 0.0;
    double getCorresTimePrev = time_getCorres.GetDuration();

    for (int i = 0; i < criteria.max_iteration_; i++) {
        utility::LogDebug("ICP Iteration #{:d}: Fitness {:.4f}, RMSE {:.4f}", i,
                          result.fitness_, result.inlier_rmse_);

        utility::Timer time_registrationICP, time_getCorres,
                time_computeTransformation;
        utility::LogInfo("      GetRegistrationResultAndCorrespondences: {}",
                         getCorresTimePrev);
        time_registrationICP.Start();
        time_computeTransformation.Start();

        core::Tensor update = estimation.ComputeTransformation(
                source_transformed, target, corres);
        transformation_device = update.Matmul(transformation_device);

        time_computeTransformation.Stop();
        utility::LogInfo("     ComputeTransform: {}",
                         time_computeTransformation.GetDuration());

        source_transformed.Transform(update);
        double prev_fitness_ = result.fitness_;
        double prev_inliner_rmse_ = result.inlier_rmse_;

        time_getCorres.Start();
        result = GetRegistrationResultAndCorrespondences(
                source_transformed, target, target_nns,
                max_correspondence_distance, transformation_device);

        corres = result.correspondence_set;

        time_getCorres.Stop();
        getCorresTimeNew = time_getCorres.GetDuration();

        if (std::abs(prev_fitness_ - result.fitness_) <
                    criteria.relative_fitness_ &&
            std::abs(prev_inliner_rmse_ - result.inlier_rmse_) <
                    criteria.relative_rmse_) {
            break;
        }

        time_registrationICP.Stop();
        utility::LogInfo("   Registration Loop: {}",
                         time_registrationICP.GetDuration());
        getCorresTimePrev = getCorresTimeNew;
    }

    return result;
}

}  // namespace registration
}  // namespace pipelines
}  // namespace t
}  // namespace open3d
