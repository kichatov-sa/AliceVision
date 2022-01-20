// This file is part of the AliceVision project.
// Copyright (c) 2022 AliceVision contributors.
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <aliceVision/mvsUtils/MultiViewParams.hpp>
#include <aliceVision/mvsUtils/ImagesCache.hpp>
#include <aliceVision/depthMap/cuda/memory.hpp>
#include <aliceVision/depthMap/cuda/deviceCommon/DeviceCameraParams.hpp>

namespace aliceVision {
namespace depthMap {

/*
 * @class DeviceCamera
 * @brief Support class to maintain a camera frame in gpu memory and 
 *        also manage DeviceCameraParams in gpu contant memory.
 */
class DeviceCamera
{
public:

    /**
     * @brief DeviceCamera constructor.
     * @param[in] deviceCamId the unique gpu camera index should correspond to
     *            an available index in DeviceCameraParams constant memory
     */
    DeviceCamera(int deviceCamId);

    // destructor
    ~DeviceCamera();

    // this class handles unique data, no copy constructor
    DeviceCamera(DeviceCamera const&) = delete;

    // this class handles unique data, no copy operator
    void operator=(DeviceCamera const&) = delete;

    inline int getDeviceCamId() const { return _deviceCamId; }
    inline int getGlobalCamId() const { return _globalCamId; }
    inline int getOriginalWidth() const { return _originalWidth; }
    inline int getOriginalHeight() const { return _originalHeight; }
    inline int getWidth() const { return _width; }
    inline int getHeight() const { return _height; }
    inline int getDownscale() const { return _downscale; }
    inline int getTextureObject() const { return _textureObject; }
    inline int getDeviceMemoryConsumption() const { return _memBytes; }

    /**
     * @brief Update the DeviceCamera with a new host-side corresponding camera.
     * @param[in] globalCamId the camera index in the ImagesCache / MultiViewParams
     * @param[in] downscale the downscale to apply on gpu
     * @param[in,out] imageCache the image cache to get host-side data
     * @param[in] mp the multi-view parameters
     * @param[in] stream the CUDA stream for gpu execution
     */
    void fill(int globalCamId, 
              int downscale, 
              mvsUtils::ImagesCache<ImageRGBAf>& imageCache,
              const mvsUtils::MultiViewParams& mp, 
              cudaStream_t stream);
private:

    // private methods

    /**
     * @brief Update the DeviceCamera with a new host-side corresponding camera.
     * @param[in] mp the multi-view parameters
     * @param[in] stream the CUDA stream for gpu execution
     */
    void fillDeviceCameraParameters(const mvsUtils::MultiViewParams& mp, cudaStream_t stream);

    /**
     * @brief Update the DeviceCamera with a new host-side corresponding camera.
     * @param[in,out] imageCache the image cache to get host-side data
     * @param[in] stream the CUDA stream for gpu execution
     */
    void fillDeviceFrameFromImageCache(mvsUtils::ImagesCache<ImageRGBAf>& ic, cudaStream_t stream);

    // private members

    const int _deviceCamId; // the device camera index, identical to index in DeviceCache vector & index in constantCameraParametersArray_d
    int _globalCamId;       // the global camera index, host-sided image cache index
    int _originalWidth;     // the original image width (before downscale, in cpu memory)
    int _originalHeight;    // the original image height (before downscale, in cpu memory)
    int _width;             // the image width (after downscale, in gpu memory)
    int _height;            // the image height (after downscale, in gpu memory)
    int _downscale;         // the downscale factor (1 equal no downscale)
    int _memBytes;          // the device memory consumption

    DeviceCameraParams* _cameraParameters_h = nullptr; // host-side camera parameters
    std::unique_ptr<CudaDeviceMemoryPitched<CudaRGBA, 2>> _frame_dmp = nullptr;
    cudaTextureObject_t _textureObject;
};

/**
  * @brief Fill the host-side camera parameters from multi-view parameters.
  * @param[in,out] cameraParameters_h the host-side camera parameters
  * @param[in] globalCamId the camera index in the ImagesCache / MultiViewParams
  * @param[in] downscale the downscale to apply on gpu
  * @param[in] mp the multi-view parameters
  */
void fillHostCameraParameters(DeviceCameraParams& cameraParameters_h, int globalCamId, int downscale, const mvsUtils::MultiViewParams& mp);

} // namespace depthMap
} // namespace aliceVision