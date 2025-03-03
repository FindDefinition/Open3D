// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018-2021 www.open3d.org
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

#include "open3d/t/geometry/PointCloud.h"

#include <string>
#include <unordered_map>

#include "open3d/core/CUDAUtils.h"
#include "open3d/core/hashmap/HashMap.h"
#include "open3d/t/geometry/LineSet.h"
#include "open3d/t/geometry/TriangleMesh.h"
#include "pybind/docstring.h"
#include "pybind/t/geometry/geometry.h"

namespace open3d {
namespace t {
namespace geometry {

// Image functions have similar arguments, thus the arg docstrings may be shared
static const std::unordered_map<std::string, std::string>
        map_shared_argument_docstrings = {
                {"rgbd_image",
                 "The input RGBD image should have a uint16_t depth image and  "
                 "RGB image with any DType and the same size."},
                {"depth", "The input depth image should be a uint16_t image."},
                {"intrinsics", "Intrinsic parameters of the camera."},
                {"extrinsics", "Extrinsic parameters of the camera."},
                {"depth_scale", "The depth is scaled by 1 / depth_scale."},
                {"depth_max", "Truncated at depth_max distance."},
                {"stride",
                 "Sampling factor to support coarse point cloud extraction. "
                 "Unless normals are requested, there is no low pass "
                 "filtering, so aliasing is possible for stride>1."},
                {"with_normals",
                 "Also compute normals for the point cloud. If True, the point "
                 "cloud will only contain points with valid normals. If "
                 "normals are requested, the depth map is first filtered to "
                 "ensure smooth normals."},
                {"max_nn",
                 "Neighbor search max neighbors parameter [default = 30]."},
                {"radius",
                 "neighbors search radius parameter to use HybridSearch. "
                 "[Recommended ~1.4x voxel size]."}};

void pybind_pointcloud(py::module& m) {
    py::class_<PointCloud, PyGeometry<PointCloud>, std::shared_ptr<PointCloud>,
               Geometry, DrawableGeometry>
            pointcloud(m, "PointCloud",
                       R"(
A point cloud contains a list of 3D points. The point cloud class stores the
attribute data in key-value maps, where the key is a string representing the
attribute name and the value is a Tensor containing the attribute data.

The attributes of the point cloud have different levels::

    import open3d as o3d

    device = o3d.core.Device("CPU:0")
    dtype = o3d.core.float32

    # Create an empty point cloud
    # Use pcd.point to access the points' attributes
    pcd = o3d.t.geometry.PointCloud(device)

    # Default attribute: "positions".
    # This attribute is created by default and is required by all point clouds.
    # The shape must be (N, 3). The device of "positions" determines the device
    # of the point cloud.
    pcd.point.positions = o3d.core.Tensor([[0, 0, 0],
                                              [1, 1, 1],
                                              [2, 2, 2]], dtype, device)

    # Common attributes: "normals", "colors".
    # Common attributes are used in built-in point cloud operations. The
    # spellings must be correct. For example, if "normal" is used instead of
    # "normals", some internal operations that expects "normals" will not work.
    # "normals" and "colors" must have shape (N, 3) and must be on the same
    # device as the point cloud.
    pcd.point.normals = o3d.core.Tensor([[0, 0, 1],
                                            [0, 1, 0],
                                            [1, 0, 0]], dtype, device)
    pcd.point.colors = o3d.core.Tensor([[0.0, 0.0, 0.0],
                                            [0.1, 0.1, 0.1],
                                            [0.2, 0.2, 0.2]], dtype, device)

    # User-defined attributes.
    # You can also attach custom attributes. The value tensor must be on the
    # same device as the point cloud. The are no restrictions on the shape and
    # dtype, e.g.,
    pcd.point.intensities = o3d.core.Tensor([0.3, 0.1, 0.4], dtype, device)
    pcd.point.labels = o3d.core.Tensor([3, 1, 4], o3d.core.int32, device)
)");

    // Constructors.
    pointcloud
            .def(py::init<const core::Device&>(),
                 "Construct an empty pointcloud on the provided ``device`` "
                 "(default: 'CPU:0').",
                 "device"_a = core::Device("CPU:0"))
            .def(py::init<const core::Tensor&>(), "positions"_a)
            .def(py::init<const std::unordered_map<std::string,
                                                   core::Tensor>&>(),
                 "map_keys_to_tensors"_a)
            .def("__repr__", &PointCloud::ToString);

    // Pickle support.
    pointcloud.def(py::pickle(
            [](const PointCloud& pcd) {
                // __getstate__
                // Convert point attributes to tensor map to CPU.
                auto map_keys_to_tensors = pcd.GetPointAttr();

                return py::make_tuple(pcd.GetDevice(), pcd.GetPointAttr());
            },
            [](py::tuple t) {
                // __setstate__
                if (t.size() != 2) {
                    utility::LogError(
                            "Cannot unpickle PointCloud! Expecting a tuple of "
                            "size 2.");
                }

                const core::Device device = t[0].cast<core::Device>();
                PointCloud pcd(device);
                if (!device.IsAvailable()) {
                    utility::LogWarning(
                            "Device ({}) is not available. PointCloud will be "
                            "created on CPU.",
                            device.ToString());
                    pcd.To(core::Device("CPU:0"));
                }

                const TensorMap map_keys_to_tensors = t[1].cast<TensorMap>();
                for (auto& kv : map_keys_to_tensors) {
                    pcd.SetPointAttr(kv.first, kv.second);
                }

                return pcd;
            }));

    // def_property_readonly is sufficient, since the returned TensorMap can
    // be editable in Python. We don't want the TensorMap to be replaced
    // by another TensorMap in Python.
    pointcloud.def_property_readonly(
            "point", py::overload_cast<>(&PointCloud::GetPointAttr, py::const_),
            "Point's attributes: positions, colors, normals, etc.");

    // Device transfers.
    pointcloud.def("to", &PointCloud::To,
                   "Transfer the point cloud to a specified device.",
                   "device"_a, "copy"_a = false);
    pointcloud.def("clone", &PointCloud::Clone,
                   "Returns a copy of the point cloud on the same device.");

    pointcloud.def(
            "cpu",
            [](const PointCloud& pointcloud) {
                return pointcloud.To(core::Device("CPU:0"));
            },
            "Transfer the point cloud to CPU. If the point cloud is "
            "already on CPU, no copy will be performed.");
    pointcloud.def(
            "cuda",
            [](const PointCloud& pointcloud, int device_id) {
                return pointcloud.To(core::Device("CUDA", device_id));
            },
            "Transfer the point cloud to a CUDA device. If the point cloud is "
            "already on the specified CUDA device, no copy will be performed.",
            "device_id"_a = 0);

    // Pointcloud specific functions.
    pointcloud.def("get_min_bound", &PointCloud::GetMinBound,
                   "Returns the min bound for point coordinates.");
    pointcloud.def("get_max_bound", &PointCloud::GetMaxBound,
                   "Returns the max bound for point coordinates.");
    pointcloud.def("get_center", &PointCloud::GetCenter,
                   "Returns the center for point coordinates.");

    pointcloud.def("append",
                   [](const PointCloud& self, const PointCloud& other) {
                       return self.Append(other);
                   });
    pointcloud.def("__add__",
                   [](const PointCloud& self, const PointCloud& other) {
                       return self.Append(other);
                   });

    pointcloud.def("transform", &PointCloud::Transform, "transformation"_a,
                   "Transforms the points and normals (if exist).");
    pointcloud.def("translate", &PointCloud::Translate, "translation"_a,
                   "relative"_a = true, "Translates points.");
    pointcloud.def("scale", &PointCloud::Scale, "scale"_a, "center"_a,
                   "Scale points.");
    pointcloud.def("rotate", &PointCloud::Rotate, "R"_a, "center"_a,
                   "Rotate points and normals (if exist).");

    pointcloud.def("select_by_mask", &PointCloud::SelectByMask,
                   "boolean_mask"_a, "invert"_a = false,
                   "Select points from input pointcloud, based on boolean mask "
                   "indices into output point cloud.");
    pointcloud.def("select_by_index", &PointCloud::SelectByIndex, "indices"_a,
                   "invert"_a = false, "remove_duplicates"_a = false,
                   "Select points from input pointcloud, based on indices into "
                   "output point cloud.");
    pointcloud.def(
            "voxel_down_sample",
            [](const PointCloud& pointcloud, const double voxel_size) {
                return pointcloud.VoxelDownSample(
                        voxel_size, core::HashBackendType::Default);
            },
            "Downsamples a point cloud with a specified voxel size.",
            "voxel_size"_a);
    pointcloud.def("uniform_down_sample", &PointCloud::UniformDownSample,
                   "Downsamples a point cloud by selecting every kth index "
                   "point and its attributes.",
                   "every_k_points"_a);
    pointcloud.def("random_down_sample", &PointCloud::RandomDownSample,
                   "Downsample a pointcloud by selecting random index point "
                   "and its attributes.",
                   "sampling_ratio"_a);
    pointcloud.def("farthest_point_down_sample",
                   &PointCloud::FarthestPointDownSample,
                   "Downsample a pointcloud into output pointcloud with a set "
                   "of points has farthest distance.The sampling is performed "
                   "by selecting the farthest point from previous selected "
                   "points iteratively",
                   "num_samples"_a);
    pointcloud.def("remove_radius_outliers", &PointCloud::RemoveRadiusOutliers,
                   "nb_points"_a, "search_radius"_a,
                   "Remove points that have less than nb_points neighbors in a "
                   "sphere of a given search radius.");
    pointcloud.def("remove_duplicated_points",
                   &PointCloud::RemoveDuplicatedPoints,
                   "Remove duplicated points and there associated attributes.");
    pointcloud.def(
            "remove_non_finite_points", &PointCloud::RemoveNonFinitePoints,
            "remove_nan"_a = true, "remove_infinite"_a = true,
            "Remove all points from the point cloud that have a nan entry, or "
            "infinite value. It also removes the corresponding attributes.");
    pointcloud.def("paint_uniform_color", &PointCloud::PaintUniformColor,
                   "color"_a, "Assigns uniform color to the point cloud.");

    pointcloud.def(
            "estimate_normals", &PointCloud::EstimateNormals,
            py::call_guard<py::gil_scoped_release>(), py::arg("max_nn") = 30,
            py::arg("radius") = py::none(),
            "Function to estimate point normals. If the point cloud normals "
            "exist, the estimated normals are oriented with respect to the "
            "same. It uses KNN search (Not recommended to use on GPU) if only "
            "max_nn parameter is provided, Radius search (Not recommended to "
            "use on GPU) if only radius is provided and Hybrid Search "
            "(Recommended) if radius parameter is also provided.");
    pointcloud.def(
            "estimate_color_gradients", &PointCloud::EstimateColorGradients,
            py::call_guard<py::gil_scoped_release>(), py::arg("max_nn") = 30,
            py::arg("radius") = py::none(),
            "Function to estimate point color gradients. It uses KNN search "
            "(Not recommended to use on GPU) if only max_nn parameter is "
            "provided, Radius search (Not recommended to use on GPU) if only "
            "radius is provided and Hybrid Search (Recommended) if radius "
            "parameter is also provided.");

    // creation (static)
    pointcloud.def_static(
            "create_from_depth_image", &PointCloud::CreateFromDepthImage,
            py::call_guard<py::gil_scoped_release>(), "depth"_a, "intrinsics"_a,
            "extrinsics"_a =
                    core::Tensor::Eye(4, core::Float32, core::Device("CPU:0")),
            "depth_scale"_a = 1000.0f, "depth_max"_a = 3.0f, "stride"_a = 1,
            "with_normals"_a = false,
            "Factory function to create a pointcloud (with only 'points') from "
            "a depth image and a camera model.\n\n Given depth value d at (u, "
            "v) image coordinate, the corresponding 3d point is:\n z = d / "
            "depth_scale\n\n x = (u - cx) * z / fx\n\n y = (v - cy) * z / fy");
    pointcloud.def_static(
            "create_from_rgbd_image", &PointCloud::CreateFromRGBDImage,
            py::call_guard<py::gil_scoped_release>(), "rgbd_image"_a,
            "intrinsics"_a,
            "extrinsics"_a =
                    core::Tensor::Eye(4, core::Float32, core::Device("CPU:0")),
            "depth_scale"_a = 1000.0f, "depth_max"_a = 3.0f, "stride"_a = 1,
            "with_normals"_a = false,
            "Factory function to create a pointcloud (with properties "
            "{'points', 'colors'}) from an RGBD image and a camera model.\n\n"
            "Given depth value d at (u, v) image coordinate, the corresponding "
            "3d point is:\n\n z = d / depth_scale\n\n x = (u - cx) * z / "
            "fx\n\n y "
            "= (v - cy) * z / fy");
    pointcloud.def_static(
            "from_legacy", &PointCloud::FromLegacy, "pcd_legacy"_a,
            "dtype"_a = core::Float32, "device"_a = core::Device("CPU:0"),
            "Create a PointCloud from a legacy Open3D PointCloud.");

    // processing
    pointcloud.def("project_to_depth_image", &PointCloud::ProjectToDepthImage,
                   "width"_a, "height"_a, "intrinsics"_a,
                   "extrinsics"_a = core::Tensor::Eye(4, core::Float32,
                                                      core::Device("CPU:0")),
                   "depth_scale"_a = 1000.0, "depth_max"_a = 3.0,
                   "Project a point cloud to a depth image.");
    pointcloud.def("project_to_rgbd_image", &PointCloud::ProjectToRGBDImage,
                   "width"_a, "height"_a, "intrinsics"_a,
                   "extrinsics"_a = core::Tensor::Eye(4, core::Float32,
                                                      core::Device("CPU:0")),
                   "depth_scale"_a = 1000.0, "depth_max"_a = 3.0,
                   "Project a colored point cloud to a RGBD image.");
    pointcloud.def(
            "hidden_point_removal", &PointCloud::HiddenPointRemoval,
            "camera_location"_a, "radius"_a,
            R"(Removes hidden points from a point cloud and returns a mesh of
the remaining points. Based on Katz et al. 'Direct Visibility of Point Sets',
2007. Additional information about the choice of radius for noisy point clouds
can be found in Mehra et. al. 'Visibility of Noisy Point Cloud Data', 2010.
This is a wrapper for a CPU implementation and a copy of the point cloud data
and resulting visible triangle mesh and indiecs will be made.

Args:
    camera_location. All points not visible from that location will be removed.
    radius. The radius of the spherical projection.

Return:
    Tuple of visible triangle mesh and indices of visible points on the same
    device as the point cloud.

Example:
    We use armadillo mesh to compute the visible points from given camera::

        # Convert mesh to a point cloud and estimate dimensions.
        armadillo_data = o3d.data.ArmadilloMesh()
        pcd = o3d.io.read_triangle_mesh(
        armadillo_data.path).sample_points_poisson_disk(5000)

        diameter = np.linalg.norm(
                np.asarray(pcd.get_max_bound()) - np.asarray(pcd.get_min_bound()))

        # Define parameters used for hidden_point_removal.
        camera = o3d.core.Tensor([0, 0, diameter], o3d.core.float32)
        radius = diameter * 100

        # Get all points that are visible from given view point.
        pcd = o3d.t.geometry.PointCloud.from_legacy(pcd)
        _, pt_map = pcd.hidden_point_removal(camera, radius)
        pcd = pcd.select_by_index(pt_map)
        o3d.visualization.draw([pcd], point_size=5))");
    pointcloud.def(
            "cluster_dbscan", &PointCloud::ClusterDBSCAN, "eps"_a,
            "min_points"_a, "print_progress"_a = false,
            R"(Cluster PointCloud using the DBSCAN algorithm  Ester et al.,'A
Density-Based Algorithm for Discovering Clusters in Large Spatial Databases
with Noise', 1996. This is a wrapper for a CPU implementation and a copy of the
point cloud data and resulting labels will be made.

Args:
    eps. Density parameter that is used to find neighbouring points.
    min_points. Minimum number of points to form a cluster.
    print_progress (default False). If 'True' the progress is visualized in the console.

Return:
    A Tensor list of point labels on the same device as the point cloud, -1
    indicates noise according to the algorithm.

Example:
    We use Redwood dataset for demonstration::

        import matplotlib.pyplot as plt

        sample_ply_data = o3d.data.PLYPointCloud()
        pcd = o3d.t.io.read_point_cloud(sample_ply_data.path)
        labels = pcd.cluster_dbscan(eps=0.02, min_points=10, print_progress=True)

        max_label = labels.max().item()
        colors = plt.get_cmap("tab20")(
                labels.numpy() / (max_label if max_label > 0 else 1))
        colors = o3d.core.Tensor(colors[:, :3], o3d.core.float32)
        colors[labels < 0] = 0
        pcd.point.colors = colors
        o3d.visualization.draw([pcd]))");
    pointcloud.def(
            "segment_plane", &PointCloud::SegmentPlane,
            "distance_threshold"_a = 0.01, "ransac_n"_a = 3,
            "num_iterations"_a = 100, "probability"_a = 0.99999999,
            R"(Segments a plane in the point cloud using the RANSAC algorithm.
This is a wrapper for a CPU implementation and a copy of the point cloud data and
resulting plane model and inlier indiecs will be made.

Args:
    distance_threshold (default 0.01). Max distance a point can be from the plane
    model, and still be considered an inlier.
    ransac_n (default 3). Number of initial points to be considered inliers in each iteration.
    num_iterations (default 100). Maximum number of iterations.
    probability (default 0.99999999). Expected probability of finding the optimal plane.

Return:
    Tuple of the plane model ax + by + cz + d = 0 and the indices of
    the plane inliers on the same device as the point cloud.

Example:
    We use Redwood dataset to compute its plane model and inliers::

        sample_pcd_data = o3d.data.PCDPointCloud()
        pcd = o3d.t.io.read_point_cloud(sample_pcd_data.path)
        plane_model, inliers = pcd.segment_plane(distance_threshold=0.01,
                                                 ransac_n=3,
                                                 num_iterations=1000)
        inlier_cloud = pcd.select_by_index(inliers)
        inlier_cloud.paint_uniform_color([1.0, 0, 0])
        outlier_cloud = pcd.select_by_index(inliers, invert=True)
        o3d.visualization.draw([inlier_cloud, outlier_cloud]))");
    pointcloud.def(
            "compute_convex_hull", &PointCloud::ComputeConvexHull,
            "joggle_inputs"_a = false,
            R"doc(Compute the convex hull of a triangle mesh using qhull. This runs on the CPU.

Args:
    joggle_inputs (default False). Handle precision problems by
    randomly perturbing the input data. Set to True if perturbing the input
    iis acceptable but you need convex simplicial output. If False,
    neighboring facets may be merged in case of precision problems. See
    `QHull docs <http://www.qhull.org/html/qh-impre.htm#joggle>`__ for more
    details.

Return:
    TriangleMesh representing the convexh hull. This contains an
    extra vertex property "point_indices" that contains the index of the
    corresponding vertex in the original mesh.

Example:
    We will load the Eagle dataset, compute and display it's convex hull::

        eagle = o3d.data.EaglePointCloud()
        pcd = o3d.t.io.read_point_cloud(eagle.path)
        hull = pcd.compute_convex_hull()
        o3d.visualization.draw([{'name': 'eagle', 'geometry': pcd}, {'name': 'convex hull', 'geometry': hull}])
    )doc");
    pointcloud.def("compute_boundary_points",
                   &PointCloud::ComputeBoundaryPoints, "radius"_a,
                   "max_nn"_a = 30, "angle_threshold"_a = 90.0,
                   R"doc(Compute the boundary points of a point cloud.
The implementation is inspired by the PCL implementation. Reference:
https://pointclouds.org/documentation/classpcl_1_1_boundary_estimation.html

Args:
    radius. Neighbor search radius parameter.
    max_nn (default 30). Maximum number of neighbors to search.
    angle_threshold (default 90.0). Angle threshold to decide if a point is on the boundary.

Return:
    Tensor of boundary points and its boolean mask tensor.

Example:
    We will load the DemoCropPointCloud dataset, compute its boundary points::

        ply_point_cloud = o3d.data.DemoCropPointCloud()
        pcd = o3d.t.io.read_point_cloud(ply_point_cloud.point_cloud_path)
        boundaries, mask = pcd.compute_boundary_points(radius, max_nn)
        boundaries.paint_uniform_color([1.0, 0.0, 0.0])
        o3d.visualization.draw([pcd, boundaries])
    )doc");

    // conversion
    pointcloud.def("to_legacy", &PointCloud::ToLegacy,
                   "Convert to a legacy Open3D PointCloud.");
    pointcloud.def(
            "get_axis_aligned_bounding_box",
            &PointCloud::GetAxisAlignedBoundingBox,
            "Create an axis-aligned bounding box from attribute 'positions'.");
    pointcloud.def("crop", &PointCloud::Crop,
                   "Function to crop pointcloud into output pointcloud.",
                   "aabb"_a, "invert"_a = false);

    docstring::ClassMethodDocInject(m, "PointCloud", "estimate_normals",
                                    map_shared_argument_docstrings);
    docstring::ClassMethodDocInject(m, "PointCloud", "create_from_depth_image",
                                    map_shared_argument_docstrings);
    docstring::ClassMethodDocInject(m, "PointCloud", "create_from_rgbd_image",
                                    map_shared_argument_docstrings);
    docstring::ClassMethodDocInject(
            m, "PointCloud", "select_by_mask",
            {{"boolean_mask",
              "Boolean indexing tensor of shape {n,} containing true value for "
              "the indices that is to be selected.."},
             {"invert", "Set to `True` to invert the selection of indices."}});
    docstring::ClassMethodDocInject(
            m, "PointCloud", "select_by_index",
            {{"indices",
              "Int64 indexing tensor of shape {n,} containing index value that "
              "is to be selected."},
             {"invert",
              "Set to `True` to invert the selection of indices, and also "
              "ignore the duplicated indices."},
             {"remove_duplicates",
              "Set to `True` to remove the duplicated indices."}});
    docstring::ClassMethodDocInject(
            m, "PointCloud", "voxel_down_sample",
            {{"voxel_size", "Voxel size. A positive number."}});
    docstring::ClassMethodDocInject(
            m, "PointCloud", "uniform_down_sample",
            {{"every_k_points",
              "Sample rate, the selected point indices are [0, k, 2k, …]."}});
    docstring::ClassMethodDocInject(
            m, "PointCloud", "random_down_sample",
            {{"sampling_ratio",
              "Sampling ratio, the ratio of sample to total number of points "
              "in the pointcloud."}});
    docstring::ClassMethodDocInject(
            m, "PointCloud", "farthest_point_down_sample",
            {{"num_samples", "Number of points to be sampled."}});
    docstring::ClassMethodDocInject(
            m, "PointCloud", "remove_radius_outliers",
            {{"nb_points",
              "Number of neighbor points required within the radius."},
             {"search_radius", "Radius of the sphere."}});
    docstring::ClassMethodDocInject(
            m, "PointCloud", "paint_uniform_color",
            {{"color",
              "Color of the pointcloud. Floating color values are clipped "
              "between 0.0 and 1.0."}});
    docstring::ClassMethodDocInject(
            m, "PointCloud", "crop",
            {{"aabb", "AxisAlignedBoundingBox to crop points."},
             {"invert",
              "Crop the points outside of the bounding box or inside of the "
              "bounding box."}});

    pointcloud.def("extrude_rotation", &PointCloud::ExtrudeRotation, "angle"_a,
                   "axis"_a, "resolution"_a = 16, "translation"_a = 0.0,
                   "capping"_a = true,
                   R"(Sweeps the point set rotationally about an axis.

Args:
    angle (float): The rotation angle in degree.
    
    axis (open3d.core.Tensor): The rotation axis.
    
    resolution (int): The resolution defines the number of intermediate sweeps
        about the rotation axis.

    translation (float): The translation along the rotation axis. 

Returns:
    A line set with the result of the sweep operation.


Example:

    This code generates a number of helices from a point cloud::

        import open3d as o3d
        import numpy as np
        pcd = o3d.t.geometry.PointCloud(np.random.rand(10,3))
        helices = pcd.extrude_rotation(3*360, [0,1,0], resolution=3*16, translation=2)
        o3d.visualization.draw([{'name': 'helices', 'geometry': helices}])

)");

    pointcloud.def("extrude_linear", &PointCloud::ExtrudeLinear, "vector"_a,
                   "scale"_a = 1.0, "capping"_a = true,
                   R"(Sweeps the point cloud along a direction vector.

Args:
    
    vector (open3d.core.Tensor): The direction vector.
    
    scale (float): Scalar factor which essentially scales the direction vector.

Returns:
    A line set with the result of the sweep operation.


Example:

    This code generates a set of straight lines from a point cloud::
        import open3d as o3d
        import numpy as np
        pcd = o3d.t.geometry.PointCloud(np.random.rand(10,3))
        lines = pcd.extrude_linear([0,1,0])
        o3d.visualization.draw([{'name': 'lines', 'geometry': lines}])


)");
}

}  // namespace geometry
}  // namespace t
}  // namespace open3d
