// This file is part of the AliceVision project.
// Copyright (c) 2017 AliceVision contributors.
// Copyright (c) 2012 openMVG contributors.
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "aliceVision/image/all.hpp"
#include "aliceVision/feature/feature.hpp"
#include "aliceVision/feature/sift/ImageDescriber_SIFT.hpp"
#include "aliceVision/matching/RegionsMatcher.hpp"
#include "aliceVision/multiview/relativePose/HomographyKernel.hpp"
#include "aliceVision/robustEstimation/conditioning.hpp"
#include "aliceVision/robustEstimation/ACRansac.hpp"
#include "aliceVision/multiview/RelativePoseKernel.hpp"
#include "aliceVision/matching/guidedMatching.hpp"

#include "dependencies/vectorGraphics/svgDrawer.hpp"

#include <string>
#include <iostream>

// These constants define the current software version.
// They must be updated when the command line is changed.
#define ALICEVISION_SOFTWARE_VERSION_MAJOR 1
#define ALICEVISION_SOFTWARE_VERSION_MINOR 0

using namespace aliceVision;
using namespace aliceVision::image;
using namespace aliceVision::matching;
using namespace aliceVision::robustEstimation;
using namespace svg;

int main() {
  std::mt19937 randomNumberGenerator;
  Image<RGBColor> image;
  const std::string jpg_filenameL = std::string("../") + std::string(THIS_SOURCE_DIR) + "/imageData/StanfordMobileVisualSearch/Ace_0.png";
  const std::string jpg_filenameR = std::string("../") + std::string(THIS_SOURCE_DIR) + "/imageData/StanfordMobileVisualSearch/Ace_1.png";

  Image<unsigned char> imageL, imageR;
  readImage(jpg_filenameL, imageL, image::EImageColorSpace::NO_CONVERSION);
  readImage(jpg_filenameR, imageR, image::EImageColorSpace::NO_CONVERSION);

  //--
  // Detect regions thanks to an image_describer
  //--
  using namespace aliceVision::feature;
  SiftParams siftParams;
  siftParams._firstOctave = -1;
  std::unique_ptr<ImageDescriber> image_describer(new ImageDescriber_SIFT(siftParams));
  std::map<IndexT, std::unique_ptr<feature::Regions> > regions_perImage;
  image_describer->describe(imageL, regions_perImage[0]);
  image_describer->describe(imageR, regions_perImage[1]);

  const SIFT_Regions* regionsL = dynamic_cast<SIFT_Regions*>(regions_perImage.at(0).get());
  const SIFT_Regions* regionsR = dynamic_cast<SIFT_Regions*>(regions_perImage.at(1).get());

  const PointFeatures
    featsL = regions_perImage.at(0)->GetRegionsPositions(),
    featsR = regions_perImage.at(1)->GetRegionsPositions();

  // Show both images side by side
  {
    Image<unsigned char> concat;
    ConcatH(imageL, imageR, concat);
    std::string out_filename = "01_concat.jpg";
    writeImage(out_filename, concat, image::EImageColorSpace::NO_CONVERSION);
  }

  //- Draw features on the two image (side by side)
  {
    Image<unsigned char> concat;
    ConcatH(imageL, imageR, concat);

    //-- Draw features :
    for (size_t i=0; i < featsL.size(); ++i )  {
      const PointFeature point = regionsL->Features()[i];
      DrawCircle(point.x(), point.y(), point.scale(), 255, &concat);
    }
    for (size_t i=0; i < featsR.size(); ++i )  {
      const PointFeature point = regionsR->Features()[i];
      DrawCircle(point.x()+imageL.Width(), point.y(), point.scale(), 255, &concat);
    }
    std::string out_filename = "02_features.jpg";
    writeImage(out_filename, concat, image::EImageColorSpace::NO_CONVERSION);
  }

  std::vector<IndMatch> vec_PutativeMatches;
  //-- Perform matching -> find Nearest neighbor, filtered with Distance ratio
  {
    // Find corresponding points
    matching::DistanceRatioMatch(
      randomNumberGenerator,
      0.8, matching::BRUTE_FORCE_L2,
      *regions_perImage.at(0).get(),
      *regions_perImage.at(1).get(),
      vec_PutativeMatches);

    // Draw correspondences after Nearest Neighbor ratio filter
    svgDrawer svgStream(imageL.Width() + imageR.Width(), std::max(imageL.Height(), imageR.Height()));
    svgStream.drawImage(jpg_filenameL, imageL.Width(), imageL.Height());
    svgStream.drawImage(jpg_filenameR, imageR.Width(), imageR.Height(), imageL.Width());
    for (size_t i = 0; i < vec_PutativeMatches.size(); ++i) {
      //Get back linked feature, draw a circle and link them by a line
      const PointFeature L = regionsL->Features()[vec_PutativeMatches[i]._i];
      const PointFeature R = regionsR->Features()[vec_PutativeMatches[i]._j];
      svgStream.drawLine(L.x(), L.y(), R.x()+imageL.Width(), R.y(), svgStyle().stroke("green", 2.0));
      svgStream.drawCircle(L.x(), L.y(), L.scale(), svgStyle().stroke("yellow", 2.0));
      svgStream.drawCircle(R.x()+imageL.Width(), R.y(), R.scale(),svgStyle().stroke("yellow", 2.0));
    }
    const std::string out_filename = "03_siftMatches.svg";
    std::ofstream svgFile( out_filename.c_str() );
    svgFile << svgStream.closeSvgFile().str();
    svgFile.close();
  }

  // Homography geometry filtering of putative matches
  {
    //A. get back interest point and send it to the robust estimation framework
    Mat xL(2, vec_PutativeMatches.size());
    Mat xR(2, vec_PutativeMatches.size());

    for (size_t k = 0; k < vec_PutativeMatches.size(); ++k)  {
      const PointFeature & imaL = featsL[vec_PutativeMatches[k]._i];
      const PointFeature & imaR = featsR[vec_PutativeMatches[k]._j];
      xL.col(k) = imaL.coords().cast<double>();
      xR.col(k) = imaR.coords().cast<double>();
    }

    //-- Homography robust estimation
    std::vector<size_t> vec_inliers;
    typedef multiview::RelativePoseKernel<
      multiview::relativePose::Homography4PSolver,
      multiview::relativePose::HomographyAsymmetricError,
      multiview::UnnormalizerI,
      robustEstimation::Mat3Model>
      KernelType;

    KernelType kernel(
      xL, imageL.Width(), imageL.Height(),
      xR, imageR.Width(), imageR.Height(),
      false); // configure as point to point error model.

    robustEstimation::Mat3Model H;
    const std::pair<double,double> ACRansacOut = ACRANSAC(kernel, randomNumberGenerator, 
    vec_inliers, 1024, &H,
      std::numeric_limits<double>::infinity());
    const double & thresholdH = ACRansacOut.first;

    // Check the homography support some point to be considered as valid
    if (vec_inliers.size() > kernel.getMinimumNbRequiredSamples() *2.5) {

      std::cout << "\nFound a homography under the confidence threshold of: "
        << thresholdH << " pixels\n\twith: " << vec_inliers.size() << " inliers"
        << " from: " << vec_PutativeMatches.size()
         << " putatives correspondences"
        << std::endl;

      //Show homography validated point and compute residuals
      std::vector<double> vec_residuals(vec_inliers.size(), 0.0);
      svgDrawer svgStream(imageL.Width() + imageR.Width(), std::max(imageL.Height(), imageR.Height()));
      svgStream.drawImage(jpg_filenameL, imageL.Width(), imageL.Height());
      svgStream.drawImage(jpg_filenameR, imageR.Width(), imageR.Height(), imageL.Width());
      for ( size_t i = 0; i < vec_inliers.size(); ++i)  {
        const PointFeature & LL = regionsL->Features()[vec_PutativeMatches[vec_inliers[i]]._i];
        const PointFeature & RR = regionsR->Features()[vec_PutativeMatches[vec_inliers[i]]._j];
        const Vec2f L = LL.coords();
        const Vec2f R = RR.coords();
        svgStream.drawLine(L.x(), L.y(), R.x()+imageL.Width(), R.y(), svgStyle().stroke("green", 2.0));
        svgStream.drawCircle(L.x(), L.y(), LL.scale(), svgStyle().stroke("yellow", 2.0));
        svgStream.drawCircle(R.x()+imageL.Width(), R.y(), RR.scale(),svgStyle().stroke("yellow", 2.0));
        // residual computation
        vec_residuals[i] = std::sqrt(KernelType::ErrorT().error(H,  LL.coords().cast<double>(), RR.coords().cast<double>()));
      }
      std::string out_filename = "04_ACRansacHomography.svg";
      std::ofstream svgFile(out_filename.c_str());
      svgFile << svgStream.closeSvgFile().str();
      svgFile.close();

      // Display some statistics of reprojection errors
      BoxStats<float> stats(vec_residuals.begin(), vec_residuals.end());

      std::cout << std::endl
        << "Homography matrix estimation, residuals statistics:" << "\n"
        << "\t-- Residual min:\t" << stats.min << std::endl
        << "\t-- Residual median:\t" << stats.median << std::endl
        << "\t-- Residual max:\t "  << stats.max << std::endl
        << "\t-- Residual mean:\t " << stats.mean << std::endl
        << "\t-- Residual first quartile:\t "  << stats.firstQuartile << std::endl
        << "\t-- Residual third quartile:\t "  << stats.thirdQuartile << std::endl;

      // --
      // Perform GUIDED MATCHING
      // --
      // Use the computed model to check valid correspondences
      // a. by considering only the geometric error,
      // b. by considering geometric error and descriptor distance ratio.
      std::vector< IndMatches > vec_corresponding_indexes(2);

      Mat xL, xR;
      PointsToMat(featsL, xL);
      PointsToMat(featsR, xR);

      //a. by considering only the geometric error

      matching::guidedMatching<robustEstimation::Mat3Model, multiview::relativePose::HomographyAsymmetricError>(
        H, xL, xR, Square(thresholdH), vec_corresponding_indexes[0]);
      std::cout << "\nGuided homography matching (geometric error) found "
        << vec_corresponding_indexes[0].size() << " correspondences."
        << std::endl;

      // b. by considering geometric error and descriptor distance ratio
      matching::guidedMatching<robustEstimation::Mat3Model, multiview::relativePose::HomographyAsymmetricError>(
        H,
        NULL, *regions_perImage.at(0), // Null since no Intrinsic is defined
        NULL, *regions_perImage.at(1), // Null since no Intrinsic is defined
        Square(thresholdH), Square(0.8),
        vec_corresponding_indexes[1]);

      std::cout << "\nGuided homography matching "
        << "(geometric + descriptor distance ratio) found "
        << vec_corresponding_indexes[1].size() << " correspondences."
        << std::endl;

      for (size_t idx = 0; idx < 2; ++idx)
      {
        const std::vector<IndMatch> & vec_corresponding_index = vec_corresponding_indexes[idx];
        //Show homography validated correspondences
        svgDrawer svgStream(imageL.Width() + imageR.Width(), std::max(imageL.Height(), imageR.Height()));
        svgStream.drawImage(jpg_filenameL, imageL.Width(), imageL.Height());
        svgStream.drawImage(jpg_filenameR, imageR.Width(), imageR.Height(), imageL.Width());
        for ( size_t i = 0; i < vec_corresponding_index.size(); ++i)  {

          const PointFeature & LL = regionsL->Features()[vec_corresponding_index[i]._i];
          const PointFeature & RR = regionsR->Features()[vec_corresponding_index[i]._j];
          const Vec2f L = LL.coords();
          const Vec2f R = RR.coords();
          svgStream.drawLine(L.x(), L.y(), R.x()+imageL.Width(), R.y(), svgStyle().stroke("green", 2.0));
          svgStream.drawCircle(L.x(), L.y(), LL.scale(), svgStyle().stroke("yellow", 2.0));
          svgStream.drawCircle(R.x()+imageL.Width(), R.y(), RR.scale(),svgStyle().stroke("yellow", 2.0));
        }
        const std::string out_filename =
          (idx == 0) ? "04_ACRansacHomography_guided_geom.svg"
            : "04_ACRansacHomography_guided_geom_distratio.svg";
        std::ofstream svgFile(out_filename.c_str());
        svgFile << svgStream.closeSvgFile().str();
        svgFile.close();
      }
    }
    else  {
      std::cout << "ACRANSAC was unable to estimate a rigid homography"
        << std::endl;
    }
  }
  return EXIT_SUCCESS;
}
