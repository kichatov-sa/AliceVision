// This file is part of the AliceVision project.
// Copyright (c) 2020 AliceVision contributors.
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.


#include <aliceVision/panorama/seams.hpp>

// Input and geometry
#include <aliceVision/sfmData/SfMData.hpp>
#include <aliceVision/sfmDataIO/sfmDataIO.hpp>

// Image
#include <aliceVision/image/all.hpp>
#include <aliceVision/mvsData/imageAlgo.hpp>

// System
#include <aliceVision/system/MemoryInfo.hpp>
#include <aliceVision/system/Logger.hpp>

// Reading command line options
#include <boost/program_options.hpp>
#include <aliceVision/system/cmdline.hpp>
#include <aliceVision/system/main.hpp>

// IO
#include <fstream>
#include <algorithm>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/filesystem.hpp>

// These constants define the current software version.
// They must be updated when the command line is changed.
#define ALICEVISION_SOFTWARE_VERSION_MAJOR 1
#define ALICEVISION_SOFTWARE_VERSION_MINOR 0

using namespace aliceVision;

namespace po = boost::program_options;
namespace bpt = boost::property_tree;
namespace fs = boost::filesystem;

bool computeWTALabels(image::Image<IndexT> & labels, const std::vector<std::shared_ptr<sfmData::View>> & views, const std::string & inputPath, std::pair<int, int> & panoramaSize)
{
    ALICEVISION_LOG_INFO("Estimating initial labels for panorama");

    WTASeams seams(panoramaSize.first, panoramaSize.second);

    for (const auto& viewIt : views)
    {
        IndexT viewId = viewIt->getViewId();

        // Load mask
        const std::string maskPath = (fs::path(inputPath) / (std::to_string(viewId) + "_mask.exr")).string();
        ALICEVISION_LOG_TRACE("Load mask with path " << maskPath);
        image::Image<unsigned char> mask;
        image::readImage(maskPath, mask, image::EImageColorSpace::NO_CONVERSION);

        // Get offset
        oiio::ParamValueList metadata = image::readImageMetadata(maskPath);
        const std::size_t offsetX = metadata.find("AliceVision:offsetX")->get_int();
        const std::size_t offsetY = metadata.find("AliceVision:offsetY")->get_int();

        // Load Weights
        const std::string weightsPath = (fs::path(inputPath) / (std::to_string(viewId) + "_weight.exr")).string();
        ALICEVISION_LOG_TRACE("Load weights with path " << weightsPath);
        image::Image<float> weights;
        image::readImage(weightsPath, weights, image::EImageColorSpace::NO_CONVERSION);

        if (!seams.append(mask, weights, viewId, offsetX, offsetY)) 
        {
            return false;
        }
    }

    labels = seams.getLabels();

    return true;
}

bool computeGCLabels(CachedImage<IndexT> & labels, image::TileCacheManager::shared_ptr& cacheManager, const std::vector<std::shared_ptr<sfmData::View>> & views, const std::string & inputPath, std::pair<int, int> & panoramaSize, int smallestViewScale) 
{   
    ALICEVISION_LOG_INFO("Estimating smart seams for panorama");

    int pyramidSize = 1 + std::max(0, smallestViewScale - 1);
    ALICEVISION_LOG_INFO("Graphcut pyramid size is " << pyramidSize);

    HierarchicalGraphcutSeams seams(cacheManager, panoramaSize.first, panoramaSize.second, pyramidSize);


    if (!seams.initialize()) 
    {
        return false;
    }

    if (!seams.setOriginalLabels(labels)) 
    {
        return false;
    }

    for (const auto& viewIt : views)
    {
        IndexT viewId = viewIt->getViewId();

        // Load mask
        const std::string maskPath = (fs::path(inputPath) / (std::to_string(viewId) + "_mask.exr")).string();
        ALICEVISION_LOG_TRACE("Load mask with path " << maskPath);
        image::Image<unsigned char> mask;
        image::readImage(maskPath, mask, image::EImageColorSpace::NO_CONVERSION);

        // Load Color
        const std::string colorsPath = (fs::path(inputPath) / (std::to_string(viewId) + ".exr")).string();
        ALICEVISION_LOG_TRACE("Load colors with path " << colorsPath);
        image::Image<image::RGBfColor> colors;
        image::readImage(colorsPath, colors, image::EImageColorSpace::NO_CONVERSION);

        // Get offset
        oiio::ParamValueList metadata = image::readImageMetadata(maskPath);
        const std::size_t offsetX = metadata.find("AliceVision:offsetX")->get_int();
        const std::size_t offsetY = metadata.find("AliceVision:offsetY")->get_int();


        // Append to graph cut
        if (!seams.append(colors, mask, viewId, offsetX, offsetY)) 
        {
            return false;
        }
    }

    if (!seams.process()) 
    {
        return false;
    }

    labels = seams.getLabels();

    return true;
}

size_t getGraphcutOptimalScale(int width, int height)
{
    /*
    Look for the smallest scale such that the image is not smaller than the
    convolution window size.
    minsize / 2^x = 5
    minsize / 5 = 2^x
    x = log2(minsize/5)
    */

    size_t minsize = std::min(width, height);
    size_t gaussianFilterRadius = 2;

    int gaussianFilterSize = 1 + 2 * gaussianFilterRadius;
    
    size_t optimal_scale = size_t(floor(std::log2(double(minsize) / gaussianFilterSize)));
    
    return (optimal_scale - 1/*Security*/);
}

int aliceVision_main(int argc, char** argv)
{
    std::string sfmDataFilepath;
    std::string warpingFolder;
    std::string outputLabels;
    std::string temporaryCachePath;

    bool useGraphCut = true;
    image::EStorageDataType storageDataType = image::EStorageDataType::Float;

    system::EVerboseLevel verboseLevel = system::Logger::getDefaultVerboseLevel();

    // Program description
    po::options_description allParams(
        "Perform panorama stiching of cameras around a nodal point for 360° panorama creation. \n"
        "AliceVision PanoramaCompositing");

    // Description of mandatory parameters
    po::options_description requiredParams("Required parameters");
    requiredParams.add_options()
        ("input,i", po::value<std::string>(&sfmDataFilepath)->required(), "Input sfmData.")
        ("warpingFolder,w", po::value<std::string>(&warpingFolder)->required(), "Folder with warped images.")
        ("output,o", po::value<std::string>(&outputLabels)->required(), "Path of the output labels.")
        ("cacheFolder,f", po::value<std::string>(&temporaryCachePath)->required(), "Path of the temporary cache.");
    allParams.add(requiredParams);

    // Description of optional parameters
    po::options_description optionalParams("Optional parameters");
    optionalParams.add_options()
    ("useGraphCut,g", po::value<bool>(&useGraphCut)->default_value(useGraphCut), "Do we use graphcut for ghost removal ?");
    allParams.add(optionalParams);

    // Setup log level given command line
    po::options_description logParams("Log parameters");
    logParams.add_options()("verboseLevel,v",
                            po::value<system::EVerboseLevel>(&verboseLevel)->default_value(verboseLevel),
                            "verbosity level (fatal, error, warning, info, debug, trace).");
    allParams.add(logParams);

    // Effectively parse command line given parse options
    po::variables_map vm;
    try
    {
        po::store(po::parse_command_line(argc, argv, allParams), vm);

        if(vm.count("help") || (argc == 1))
        {
            ALICEVISION_COUT(allParams);
            return EXIT_SUCCESS;
        }
        po::notify(vm);
    }
    catch(boost::program_options::required_option& e)
    {
        ALICEVISION_CERR("ERROR: " << e.what());
        ALICEVISION_COUT("Usage:\n\n" << allParams);
        return EXIT_FAILURE;
    }
    catch(boost::program_options::error& e)
    {
        ALICEVISION_CERR("ERROR: " << e.what());
        ALICEVISION_COUT("Usage:\n\n" << allParams);
        return EXIT_FAILURE;
    }

    ALICEVISION_COUT("Program called with the following parameters:");
    ALICEVISION_COUT(vm);

    // Set verbose level given command line
    system::Logger::get()->setLogLevel(verboseLevel);

    // load input scene
    sfmData::SfMData sfmData;
    if(!sfmDataIO::Load(sfmData, sfmDataFilepath,
                        sfmDataIO::ESfMData(sfmDataIO::VIEWS | sfmDataIO::EXTRINSICS | sfmDataIO::INTRINSICS)))
    {
        ALICEVISION_LOG_ERROR("The input file '" + sfmDataFilepath + "' cannot be read");
        return EXIT_FAILURE;
    }

    int tileSize;
    std::pair<int, int> panoramaSize;
    {
        const IndexT viewId = *sfmData.getValidViews().begin();
        const std::string viewFilepath = (fs::path(warpingFolder) / (std::to_string(viewId) + ".exr")).string();
        ALICEVISION_LOG_TRACE("Read panorama size from file: " << viewFilepath);

        oiio::ParamValueList metadata = image::readImageMetadata(viewFilepath);
        panoramaSize.first = metadata.find("AliceVision:panoramaWidth")->get_int();
        panoramaSize.second = metadata.find("AliceVision:panoramaHeight")->get_int();
        tileSize = metadata.find("AliceVision:tileSize")->get_int();

        if(panoramaSize.first == 0 || panoramaSize.second == 0)
        {
            ALICEVISION_LOG_ERROR("The output panorama size is empty.");
            return EXIT_FAILURE;
        }

        if(tileSize == 0)
        {
            ALICEVISION_LOG_ERROR("no information on tileSize");
            return EXIT_FAILURE;
        }

        ALICEVISION_LOG_INFO("Output labels size set to " << panoramaSize.first << "x" << panoramaSize.second);
    }

    if(!temporaryCachePath.empty() && !fs::exists(temporaryCachePath))
    {
        fs::create_directory(temporaryCachePath);
    }

    // Create a cache manager
    image::TileCacheManager::shared_ptr cacheManager = image::TileCacheManager::create(temporaryCachePath, 256, 256, 65536);
    if(!cacheManager)
    {
        ALICEVISION_LOG_ERROR("Error creating the cache manager");
        return EXIT_FAILURE;
    }

    // Configure the cache manager memory
    system::MemoryInfo memInfo = system::getMemoryInfo();
    const double convertionGb = std::pow(2,30);
    ALICEVISION_LOG_INFO("Available RAM is " << std::setw(5) << memInfo.availableRam / convertionGb << "GB (" << memInfo.availableRam << " octets).");
    cacheManager->setMaxMemory(1024ll * 1024ll * 1024ll * 6ll);


    //Get a list of views ordered by their image scale
    size_t smallestScale = 0;
    std::vector<std::shared_ptr<sfmData::View>> viewOrderedByScale;
    {
        std::map<size_t, std::vector<std::shared_ptr<sfmData::View>>> mapViewsScale;
        for(const auto & it : sfmData.getViews()) 
        {
            auto view = it.second;
            IndexT viewId = view->getViewId();

            if(!sfmData.isPoseAndIntrinsicDefined(view.get()))
            {
                // skip unreconstructed views
                continue;
            }

            // Load mask
            const std::string maskPath = (fs::path(warpingFolder) / (std::to_string(viewId) + "_mask.exr")).string();
            image::Image<unsigned char> mask;
            image::readImage(maskPath, mask, image::EImageColorSpace::NO_CONVERSION);

            //Estimate scale
            size_t scale = getGraphcutOptimalScale(mask.Width(), mask.Height());
            mapViewsScale[scale].push_back(it.second);
        }
        
        if (mapViewsScale.size() == 0) 
        {
            ALICEVISION_LOG_ERROR("No valid view");
            return EXIT_FAILURE;
        }

        smallestScale = mapViewsScale.begin()->first;
        for (auto scaledList : mapViewsScale)
        {
            for (auto item : scaledList.second) 
            {   
                viewOrderedByScale.push_back(item);
            }
        }
    }

    ALICEVISION_LOG_INFO(viewOrderedByScale.size() << " views to process");

    image::Image<IndexT> labels;
    if (!computeWTALabels(labels, viewOrderedByScale, warpingFolder, panoramaSize)) 
    {
        ALICEVISION_LOG_ERROR("Error computing initial labels");
        return EXIT_FAILURE;
    }

    /*if (useGraphCut)
    {
        if (!computeGCLabels(labels, cacheManager, viewOrderedByScale, warpingFolder, panoramaSize, smallestScale)) 
        {
            ALICEVISION_LOG_ERROR("Error computing graph cut labels");
            return EXIT_FAILURE;
        }
    }

    if (!labels.writeImage(outputLabels)) 
    {
        ALICEVISION_LOG_ERROR("Error writing labels to disk");
        return EXIT_FAILURE;
    }*/

    return EXIT_SUCCESS;
}
