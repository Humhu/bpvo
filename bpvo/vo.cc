/*
   This file is part of bpvo.

   bpvo is free software: you can redistribute it and/or modify
   it under the terms of the Lesser GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   bpvo is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   Lesser GNU General Public License for more details.

   You should have received a copy of the Lesser GNU General Public License
   along with bpvo.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * Contributor: halismai@cs.cmu.edu
 */

#include "bpvo/vo.h"
#include "bpvo/vo_frame.h"
#include "bpvo/vo_pose_estimator.h"
#include "bpvo/trajectory.h"
#include "bpvo/point_cloud.h"

namespace bpvo {

class VisualOdometry::Impl
{
 public:
  inline Impl(const Matrix33&, float, ImageSize, const AlgorithmParameters& p);

  inline Result addFrame(const cv::Mat&, const cv::Mat&, const Matrix44&);

  inline const Trajectory& trajectory() const { return _trajectory; }

  inline bool checkResult( const std::vector<OptimizerStatistics>& stats );
  inline int numPointsAtLevel(int) const;
  inline const PointVector& pointsAtLevel(int) const;

 private:

  AlgorithmParameters _params;
  ImageSize _image_size;
  UniquePointer<VisualOdometryPoseEstimator> _vo_pose;
  UniquePointer<VisualOdometryFrame> _ref_frame;
  UniquePointer<VisualOdometryFrame> _cur_frame;
  UniquePointer<VisualOdometryFrame> _prev_frame;
  Matrix44 _T_kf;
  Trajectory _trajectory;

  KeyFramingReason shouldKeyFrame(const Matrix44&) const;

  UniquePointer<PointCloud> getPointCloudFromRefFrame() const;
}; // VisualOdometry::Impl


VisualOdometry::VisualOdometry(const Matrix33& K, float baseline,
                               ImageSize image_size, const AlgorithmParameters& params)
    : _impl(new Impl(K, baseline, image_size, params)) {}

VisualOdometry::~VisualOdometry() { delete _impl; }

Result VisualOdometry::addFrame(const cv::Mat& image, const cv::Mat& disparity,
                                const Matrix44& guess)
{
  THROW_ERROR_IF( image.empty() || disparity.empty(),
                 "nullptr image/disparity" );

  return _impl->addFrame(image, disparity, guess);
}

int VisualOdometry::numPointsAtLevel(int level) const
{
  return _impl->numPointsAtLevel(level);
}

const Trajectory& VisualOdometry::trajectory() const
{
  return _impl->trajectory();
}

auto VisualOdometry::pointsAtLevel(int level) const -> const PointVector&
{
  return _impl->pointsAtLevel(level);
}


//
// implementation
//

VisualOdometry::Impl::
Impl(const Matrix33& K, float b, ImageSize s, const AlgorithmParameters& p)
  : _params(p)
  , _image_size(s)
  , _vo_pose(make_unique<VisualOdometryPoseEstimator>(p))
  , _T_kf(Matrix44::Identity())
{
  if(_params.numPyramidLevels <= 0) {
    _params.numPyramidLevels = 1 + std::round(
        std::log2(std::min(s.rows, s.cols) / (double) p.minImageDimensionForPyramid));
    Info("auto pyramid level set to %d\n", _params.numPyramidLevels);
  }

  _ref_frame = make_unique<VisualOdometryFrame>(K, b, _params);
  _cur_frame = make_unique<VisualOdometryFrame>(K, b, _params);
  _prev_frame = make_unique<VisualOdometryFrame>(K, b, _params);
}

static inline Result FirstFrameResult(int n_levels)
{
  Result r;
  r.success = false;
  r.displacement.setIdentity();
  r.covariance.setIdentity();
  r.optimizerStatistics.resize(n_levels);
  r.isKeyFrame = true;
  r.keyFramingReason = KeyFramingReason::kFirstFrame;
  r.pointCloud = nullptr;

  return r;
}

inline bool VisualOdometry::Impl::
checkResult(const std::vector<OptimizerStatistics>& stats)
{
  std::stringstream ss;
  for(int i = stats.size() - 1; i >= _params.maxTestLevel; --i)
    {
      ss << i << ": " << stats[i].finalError << "(" << stats[i].numPixels << "), ";
    }

  const OptimizerStatistics& finStats = stats[_params.maxTestLevel];
  if( finStats.finalError / finStats.numPixels > _params.maxSolutionError ) 
  {
     Info("Error exceeded: %s\n", ss.str().c_str());
     return false; 
  }

  for(int i = stats.size() - 1; i >= _params.maxTestLevel; --i)
  {
    if( stats[i].status == kSolverError ) { return false; }
  }
  return true;
}

inline Result VisualOdometry::Impl::
addFrame(const cv::Mat& I, const cv::Mat& D, const Matrix44& guess)
{
  _cur_frame->setData(I, D);

  if(!_ref_frame->hasTemplate())
  {
    std::swap(_ref_frame, _cur_frame);
    _ref_frame->setTemplate();
    _trajectory.push_back( _T_kf );
    return FirstFrameResult(_ref_frame->numLevels());
  }

  Matrix44 T_est;
  Matrix44 T_guess = _T_kf * guess;

  Result ret;
  ret.optimizerStatistics = _vo_pose->estimatePose(_ref_frame.get(), _cur_frame.get(),
                                                   T_guess, T_est);
  ret.success = checkResult( ret.optimizerStatistics );
  if( !ret.success )
  { 
    Info("Initial pose estimation failed\n");
    ret.keyFramingReason = kEstimationFailed;
  }
  else
  {
    ret.keyFramingReason = shouldKeyFrame(T_est);
  }

  ret.isKeyFrame = KeyFramingReason::kNoKeyFraming != ret.keyFramingReason;
  
  if(!ret.isKeyFrame)
  {
    // store _cur_frame in _prev_frame as a keyframe candidate for the future
    std::swap(_prev_frame, _cur_frame);
    // If no keyframing required, return displacement by subtracting accumulated displacements
    ret.displacement = T_est * _T_kf.inverse();
    _T_kf = T_est;
  } 
  else
  {
    Info("Keyframing: %s\n", ToString(ret.keyFramingReason).c_str());
    // If keyframing required, reset accumulated displacements
    _T_kf.setIdentity();

    // store the point cloud
    ret.pointCloud = getPointCloudFromRefFrame();

    // If no previous frame, we've keyframed twice in a row unsuccessfully
    // Can't return anything useful
    if(_prev_frame->empty())
    {
      std::swap(_cur_frame, _ref_frame);
      _ref_frame->setTemplate();
      Info("Could not obtain intermediate frame!\n");
      ret.success = false;
    }
    // Else use previous frame with current frame
    else
    {
      std::swap(_prev_frame, _ref_frame);
      _prev_frame->clear();
      _ref_frame->setTemplate();

      T_guess = guess;
      ret.optimizerStatistics = _vo_pose->estimatePose(_ref_frame.get(), _cur_frame.get(),
                                                       T_guess, T_est);
      ret.displacement = T_est;
      _T_kf = T_est;

      ret.success = checkResult( ret.optimizerStatistics );  
      if( !ret.success )
      {
        Info("Keyframe pose re-estimation failed\n" );
        ret.keyFramingReason = kEstimationFailed;
      }
      else
      {
        ret.keyFramingReason = shouldKeyFrame(T_est);
      }

      if( ret.keyFramingReason != kNoKeyFraming )
      {
        Info("Backup keyframe failed keyframe requirements!\n");
        ret.success = false;
      }
    }
  }

  // TODO
  // _trajectory.push_back(ret.pose);

  // if(ret.pointCloud)
  //   ret.pointCloud->pose() = _trajectory.back();

  return ret;
}

inline KeyFramingReason VisualOdometry::Impl::
shouldKeyFrame(const Matrix44& pose) const
{
  auto t_norm = pose.block<3,1>(0,3).squaredNorm();
  if(t_norm > math::sq(_params.minTranslationMagToKeyFrame))
  {
    dprintf("keyFramingReason::kLargeTranslation\n");
    return KeyFramingReason::kLargeTranslation;
  }

  auto r_norm = math::RotationMatrixToEulerAngles(pose).squaredNorm();
  if(r_norm > math::sq(_params.minRotationMagToKeyFrame))
  {
    dprintf("kLargeRotation\n");
    return KeyFramingReason::kLargeRotation;
  }

  auto frac_good = _vo_pose->getFractionOfGoodPoints(_params.goodPointThreshold);
  if(frac_good < _params.maxFractionOfGoodPointsToKeyFrame)
  {
    dprintf("kSmallFracOfGoodPoints\n");
    return KeyFramingReason::kSmallFracOfGoodPoints;
  }

  return KeyFramingReason::kNoKeyFraming;
}

inline int VisualOdometry::Impl::
numPointsAtLevel(int level) const
{
  if(level < 0)
    level = _params.maxTestLevel;

  int ret = 0;
  if(_ref_frame)
    ret = _ref_frame->getTemplateDataAtLevel(level)->numPoints();

  return ret;
}

inline auto VisualOdometry::Impl::
pointsAtLevel(int level) const -> const PointVector&
{
  THROW_ERROR_IF( _ref_frame == nullptr, "no reference frame has been set");
  if(level < 0)
    level = _params.maxTestLevel;
  return _ref_frame->getTemplateDataAtLevel(level)->points();
}

template <class Warp> static inline
typename PointWithInfo::Color
GetColor(const cv::Mat& image, const Warp& warp, const Point& p)
{
  const auto uv = warp.getImagePoint(p);
  const auto c = uv[1] >= 0 && uv[1] < image.rows &&
                 uv[0] >= 0 && uv[0] < image.cols ?
                 image.at<uint8_t>(uv[1], uv[0]) : 0;

  return PointWithInfo::Color(c, c, c, 255);
}

inline UniquePointer<PointCloud> VisualOdometry::Impl::
getPointCloudFromRefFrame() const
{
  const auto& points = pointsAtLevel(_params.maxTestLevel);
  const auto& weights = _vo_pose->getWeights();

  const auto n = points.size();
  // TODO we should test if the weights/num_channels == n!
  // THROW_ERROR_IF( n > weights.size(),
  //                Format("size mismatch [%zu != %zu]", points.size(), weights.size()).c_str());
  if( n != weights.size() ) { return nullptr; }

  auto ret = make_unique<PointCloud>(n);

  const auto& image = *_ref_frame->imagePointer();
  const auto& warp = _ref_frame->getTemplateDataAtLevel(_params.maxTestLevel)->warp();
  for(size_t i = 0; i < n; ++i) {
    auto color = GetColor(image, warp, points[i]);
    ret->operator[](i) = PointWithInfo(points[i], color, weights[i]);
  }

  return ret;
}

}; // bpvo


