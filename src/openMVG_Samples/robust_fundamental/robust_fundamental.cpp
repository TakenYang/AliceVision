
// Copyright (c) 2012, 2013 Pierre MOULON.

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "openMVG/image/image.hpp"
#include "openMVG/features/features.hpp"
#include "openMVG/matching/regions_matcher.hpp"
#include "openMVG/multiview/solver_fundamental_kernel.hpp"
#include "openMVG/multiview/conditioning.hpp"
#include "openMVG/robust_estimation/robust_estimator_ACRansac.hpp"
#include "openMVG/robust_estimation/robust_estimator_ACRansacKernelAdaptator.hpp"

#include "nonFree/sift/SIFT_describer.hpp"
#include "third_party/cmdLine/cmdLine.h"

#include "third_party/stlplus3/filesystemSimplified/file_system.hpp"
#include "third_party/vectorGraphics/svgDrawer.hpp"

#include <string>
#include <iostream>

using namespace openMVG;
using namespace openMVG::image;
using namespace openMVG::matching;
using namespace openMVG::robust;
using namespace svg;
using namespace std;


int main(int argc, char **argv) 
{

  const std::string sInputDir = stlplus::folder_up(string(THIS_SOURCE_DIR))
    + "/imageData/SceauxCastle/";
  Image<RGBColor> image;
  std::string jpg_filenameL = sInputDir + "100_7101.jpg";
  std::string jpg_filenameR = sInputDir + "100_7102.jpg";
  
  std::string sFeaturePreset = "";
  
  CmdLine cmd;
  cmd.add( make_option('l', jpg_filenameL, "left") );
  cmd.add( make_option('r', jpg_filenameR, "right") );
  cmd.add( make_option('p', sFeaturePreset, "describerPreset") );
  
  try {
      if (argc == 1) throw std::string("Invalid command line parameter.");
      cmd.process(argc, argv);
  } catch(const std::string& s) {
      std::cerr << "Usage: " << argv[0] << '\n'
      << "\n[Optional]\n"
      << "[-l|--left] the left image (default 100_7101.jpg)"
      << "[-r|--right] the right image (default 100_7102.jpg)"
      << "[-p|--describerPreset]\n"
      << "  (used to control the Image_describer configuration):\n"
      << "   LOW,\n"
      << "   MEDIUM,\n"
      << "   NORMAL (default),\n"
      << "   HIGH,\n"
      << "   ULTRA: !!Can take long time!!\n"
      << std::endl;

      std::cerr << s << std::endl;
      return EXIT_FAILURE;
  }

  Image<unsigned char> imageL, imageR;
  ReadImage(jpg_filenameL.c_str(), &imageL);
  ReadImage(jpg_filenameR.c_str(), &imageR);

  //--
  // Detect regions thanks to an image_describer
  //--
  using namespace openMVG::features;
  std::unique_ptr<Image_describer> image_describer(new SIFT_Image_describer);
  if (!sFeaturePreset.empty())
  {
    if (!image_describer->Set_configuration_preset(sFeaturePreset))
    {
      std::cerr << "Preset configuration failed." << std::endl;
      return EXIT_FAILURE;
    }
  }
 
  std::map<IndexT, std::unique_ptr<features::Regions> > regions_perImage;
  image_describer->Describe(imageL, regions_perImage[0]);
  image_describer->Describe(imageR, regions_perImage[1]);

  const SIFT_Regions* regionsL = dynamic_cast<SIFT_Regions*>(regions_perImage.at(0).get());
  const SIFT_Regions* regionsR = dynamic_cast<SIFT_Regions*>(regions_perImage.at(1).get());

  const PointFeatures
    featsL = regions_perImage.at(0)->GetRegionsPositions(),
    featsR = regions_perImage.at(1)->GetRegionsPositions();

  // Show both images side by side
  {
    Image<unsigned char> concat;
    ConcatH(imageL, imageR, concat);
    string out_filename = "01_concat.jpg";
    WriteImage(out_filename.c_str(), concat);
  }

  //- Draw features on the two image (side by side)
  {
    Image<unsigned char> concat;
    ConcatH(imageL, imageR, concat);

    //-- Draw features :
    for (size_t i=0; i < featsL.size(); ++i )  {
      const SIOPointFeature point = regionsL->Features()[i];
      DrawCircle(point.x(), point.y(), point.scale(), 255, &concat);
    }
    for (size_t i=0; i < featsR.size(); ++i )  {
      const SIOPointFeature point = regionsR->Features()[i];
      DrawCircle(point.x()+imageL.Width(), point.y(), point.scale(), 255, &concat);
    }
    string out_filename = "02_features.jpg";
    WriteImage(out_filename.c_str(), concat);
  }

  std::vector<IndMatch> vec_PutativeMatches;
  //-- Perform matching -> find Nearest neighbor, filtered with Distance ratio
  {
    // Find corresponding points
    matching::DistanceRatioMatch(
      0.8, matching::BRUTE_FORCE_L2,
      *regions_perImage.at(0).get(),
      *regions_perImage.at(1).get(),
      vec_PutativeMatches);

    // Draw correspondences after Nearest Neighbor ratio filter
    svgDrawer svgStream( imageL.Width() + imageR.Width(), max(imageL.Height(), imageR.Height()));
    svgStream.drawImage(jpg_filenameL, imageL.Width(), imageL.Height());
    svgStream.drawImage(jpg_filenameR, imageR.Width(), imageR.Height(), imageL.Width());
    for (size_t i = 0; i < vec_PutativeMatches.size(); ++i) 
    {
      //Get back linked feature, draw a circle and link them by a line
      const SIOPointFeature L = regionsL->Features()[vec_PutativeMatches[i]._i];
      const SIOPointFeature R = regionsR->Features()[vec_PutativeMatches[i]._j];
      svgStream.drawLine(L.x(), L.y(), R.x()+imageL.Width(), R.y(), svgStyle().stroke("green", 2.0));
      svgStream.drawCircle(L.x(), L.y(), L.scale(), svgStyle().stroke("yellow", 2.0));
      svgStream.drawCircle(R.x()+imageL.Width(), R.y(), R.scale(),svgStyle().stroke("yellow", 2.0));
    }
    const string out_filename = "03_siftMatches.svg";
    ofstream svgFile( out_filename.c_str() );
    svgFile << svgStream.closeSvgFile().str();
    svgFile.close();
  }

  // Fundamental geometry filtering of putative matches
  {
    //A. get back interest point and send it to the robust estimation framework
    Mat xL(2, vec_PutativeMatches.size());
    Mat xR(2, vec_PutativeMatches.size());

    for (size_t k = 0; k < vec_PutativeMatches.size(); ++k)
    {
      const PointFeature & imaL = featsL[vec_PutativeMatches[k]._i];
      const PointFeature & imaR = featsR[vec_PutativeMatches[k]._j];
      xL.col(k) = imaL.coords().cast<double>();
      xR.col(k) = imaR.coords().cast<double>();
    }

    //-- Fundamental robust estimation
    std::vector<size_t> vec_inliers;
    typedef ACKernelAdaptor<
      openMVG::fundamental::kernel::SevenPointSolver,
      openMVG::fundamental::kernel::SymmetricEpipolarDistanceError,
      UnnormalizerT,
      Mat3>
      KernelType;

    KernelType kernel(
      xL, imageL.Width(), imageL.Height(),
      xR, imageR.Width(), imageR.Height(),
      true); // configure as point to line error model.

    Mat3 F;
    const std::pair<double,double> ACRansacOut = ACRANSAC(kernel, vec_inliers, 1024, &F,
      Square(4.0), // Upper bound of authorized threshold
      true);
    const double & thresholdF = ACRansacOut.first;

    // Check the fundamental support some point to be considered as valid
    if (vec_inliers.size() > KernelType::MINIMUM_SAMPLES *2.5) 
    {
      std::cout << "\nFound a fundamental under the confidence threshold of: "
        << thresholdF << " pixels\n\twith: " << vec_inliers.size() << " inliers"
        << " from: " << vec_PutativeMatches.size()
        << " putatives correspondences"
        << std::endl;

      //Show fundamental validated point and compute residuals
      std::vector<double> vec_residuals(vec_inliers.size(), 0.0);
      svgDrawer svgStream( imageL.Width() + imageR.Width(), max(imageL.Height(), imageR.Height()));
      svgStream.drawImage(jpg_filenameL, imageL.Width(), imageL.Height());
      svgStream.drawImage(jpg_filenameR, imageR.Width(), imageR.Height(), imageL.Width());
      for ( size_t i = 0; i < vec_inliers.size(); ++i)  
      {
        const SIOPointFeature & LL = regionsL->Features()[vec_PutativeMatches[vec_inliers[i]]._i];
        const SIOPointFeature & RR = regionsR->Features()[vec_PutativeMatches[vec_inliers[i]]._j];
        const Vec2f L = LL.coords();
        const Vec2f R = RR.coords();
        svgStream.drawLine(L.x(), L.y(), R.x()+imageL.Width(), R.y(), svgStyle().stroke("green", 2.0));
        svgStream.drawCircle(L.x(), L.y(), LL.scale(), svgStyle().stroke("yellow", 2.0));
        svgStream.drawCircle(R.x()+imageL.Width(), R.y(), RR.scale(),svgStyle().stroke("yellow", 2.0));
        // residual computation
        vec_residuals[i] = std::sqrt(KernelType::ErrorT::Error(F,
                                       LL.coords().cast<double>(),
                                       RR.coords().cast<double>()));
      }
      const string out_filename = "04_ACRansacFundamental.svg";
      ofstream svgFile( out_filename.c_str() );
      svgFile << svgStream.closeSvgFile().str();
      svgFile.close();

      // Display some statistics of reprojection errors
      float dMin, dMax, dMean, dMedian;
      minMaxMeanMedian<float>(vec_residuals.begin(), vec_residuals.end(),
                            dMin, dMax, dMean, dMedian);

      std::cout << std::endl
        << "Fundamental matrix estimation, residuals statistics:" << "\n"
        << "\t-- Residual min:\t" << dMin << std::endl
        << "\t-- Residual median:\t" << dMedian << std::endl
        << "\t-- Residual max:\t "  << dMax << std::endl
        << "\t-- Residual mean:\t " << dMean << std::endl;
    }
    else  
    {
      std::cout << "ACRANSAC was unable to estimate a rigid fundamental"
        << std::endl;
    }
  }
  return EXIT_SUCCESS;
}
