// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "texture_handler.h"

#include <cassert>
#include <iostream>

#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <wmcodecdsp.h>
#include <combaseapi.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

template <class T> void SafeRelease(T **ppT)
{
  if (*ppT)
  {
    (*ppT)->Release();
    *ppT = NULL;
  }
}

const UINT32 VIDEO_FPS = 30;
const UINT64 VIDEO_FRAME_DURATION = 10 * 1000 * 1000 / VIDEO_FPS;

namespace camera_windows {

TextureHandler::~TextureHandler() {
  // Texture might still be processed while destructor is called.
  // Lock mutex for safe destruction
  const std::lock_guard<std::mutex> lock(buffer_mutex_);
  if (texture_registrar_ && texture_id_ > 0) {
    texture_registrar_->UnregisterTexture(texture_id_);
  }
  texture_id_ = -1;
  texture_ = nullptr;
  texture_registrar_ = nullptr;
}

int64_t TextureHandler::RegisterTexture() {
  if (!texture_registrar_) {
    return -1;
  }

  // Create flutter desktop pixelbuffer texture;
  texture_ =
      std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture(
          [this](size_t width,
                 size_t height) -> const FlutterDesktopPixelBuffer* {
            return this->ConvertPixelBufferForFlutter(width, height);
          }));

  texture_id_ = texture_registrar_->RegisterTexture(texture_.get());
  return texture_id_;
}

bool TextureHandler::UpdateBuffer(uint8_t* data, uint32_t data_length) {
  // Scoped lock guard.
  {
    const std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (!TextureRegistered()) {
      return false;
    }

    if (source_buffer_.size() != data_length) {
      // Update source buffer size.
      source_buffer_.resize(data_length);
    }
    std::copy(data, data + data_length, source_buffer_.data());
  }
  OnBufferUpdated();
  return true;
};

// Marks texture frame available after buffer is updated.
void TextureHandler::OnBufferUpdated() {
  if (TextureRegistered()) {
    texture_registrar_->MarkTextureFrameAvailable(texture_id_);
  }
}

const FlutterDesktopPixelBuffer* TextureHandler::ConvertPixelBufferForFlutter(
    size_t target_width, size_t target_height) {
  // TODO: optimize image processing size by adjusting capture size
  // dynamically to match target_width and target_height.
  // If target size changes, create new media type for preview and set new
  // target framesize to MF_MT_FRAME_SIZE attribute.
  // Size should be kept inside requested resolution preset.
  // Update output media type with IMFCaptureSink2::SetOutputMediaType method
  // call and implement IMFCaptureEngineOnSampleCallback2::OnSynchronizedEvent
  // to detect size changes.

  // Lock buffer mutex to protect texture processing
  std::unique_lock<std::mutex> buffer_lock(buffer_mutex_);
  if (!TextureRegistered()) {
    return nullptr;
  }

  const uint32_t bytes_per_pixel = 4;
  const uint32_t pixels_total = preview_frame_width_ * preview_frame_height_;
  const uint32_t data_size = pixels_total * bytes_per_pixel;
  if (data_size > 0 && source_buffer_.size() == data_size) {
    if (dest_buffer_.size() != data_size) {
      dest_buffer_.resize(data_size);
    }
    if (raw_buffer_.size() != data_size) {
      raw_buffer_.resize(data_size);
    }

    // Map buffers to structs for easier conversion.
    MFVideoFormatRGB32Pixel* src =
        reinterpret_cast<MFVideoFormatRGB32Pixel*>(source_buffer_.data());
    FlutterDesktopPixel* dst =
        reinterpret_cast<FlutterDesktopPixel*>(dest_buffer_.data());
    FlutterDesktopPixel* raw =
        reinterpret_cast<FlutterDesktopPixel*>(raw_buffer_.data());

    for (uint32_t y = 0; y < preview_frame_height_; y++) {
      for (uint32_t x = 0; x < preview_frame_width_; x++) {
        uint32_t sp = (y * preview_frame_width_) + x;
        if (mirror_preview_) {
          // Software mirror mode.
          // IMFCapturePreviewSink also has the SetMirrorState setting,
          // but if enabled, samples will not be processed.

          // Calculates mirrored pixel position.
          // Check the frame format
          uint32_t tp =
              (y * preview_frame_width_) + ((preview_frame_width_ - 1) - x);
          raw[tp].r = src[sp].r;
          raw[tp].g = src[sp].g;
          raw[tp].b = src[sp].b;
          raw[tp].a = 255;

          if (frame_foramt_.compare(FRAME_FORMAT_YUV) == 0) {
            dst[tp].r = src[sp].r;
            dst[tp].g = src[sp].g;
            dst[tp].b = src[sp].b;
            dst[tp].a = 255;
          } else if (frame_foramt_.compare(FRAME_FORMAT_RGB) == 0) {
            dst[tp].r = src[sp].b;
            dst[tp].g = src[sp].b;
            dst[tp].b = src[sp].b;
            dst[tp].a = 255;
          }
        } else {
          // Check the frame format
          raw[sp].r = src[sp].r;
          raw[sp].g = src[sp].g;
          raw[sp].b = src[sp].b;
          raw[sp].a = 255;
          if (frame_foramt_.compare(FRAME_FORMAT_YUV) == 0) {
            dst[sp].r = src[sp].r;
            dst[sp].g = src[sp].g;
            dst[sp].b = src[sp].b;
            dst[sp].a = 255;
          } else if (frame_foramt_.compare(FRAME_FORMAT_RGB) == 0) {
            dst[sp].r = src[sp].b;
            dst[sp].g = src[sp].b;
            dst[sp].b = src[sp].b;
            dst[sp].a = 255;
          }
        }
      }
    }

    if (!flutter_desktop_pixel_buffer_) {
      flutter_desktop_pixel_buffer_ =
          std::make_unique<FlutterDesktopPixelBuffer>();

      // Unlocks mutex after texture is processed.
      flutter_desktop_pixel_buffer_->release_callback =
          [](void* release_context) {
            auto mutex = reinterpret_cast<std::mutex*>(release_context);
            mutex->unlock();
          };
    }

    flutter_desktop_pixel_buffer_->buffer = dest_buffer_.data();
    flutter_desktop_pixel_buffer_->width = preview_frame_width_;
    flutter_desktop_pixel_buffer_->height = preview_frame_height_;
    if (capture_controller_listener_) {
      capture_controller_listener_->OnStreamedFrameAvailable(raw_buffer_.data(), preview_frame_width_ * preview_frame_height_ * 4);
    }
    if (isRecording_) {
      WriteFrame(pSinkWriter_, stream, dest_buffer_.data(), rtStart);
      rtStart += video_frame_duration_;
    }

    // Releases unique_lock and set mutex pointer for release context.
    flutter_desktop_pixel_buffer_->release_context = buffer_lock.release();

    return flutter_desktop_pixel_buffer_.get();
  }
  return nullptr;
}

std::wstring StringToWString(const std::string& str) {
  return std::wstring(str.begin(), str.end());
}

void FlipVertical(uint8_t* buffer, int width, int height, int bytesPerPixel) {
  int stride = width * bytesPerPixel;
  std::vector<uint8_t> tempRow(stride);

  for (int y = 0; y < height / 2; ++y) {
    uint8_t* topRow = buffer + y * stride;
    uint8_t* bottomRow = buffer + (height - y - 1) * stride;

    // Swap top and bottom rows
    memcpy(tempRow.data(), topRow, stride);
    memcpy(topRow, bottomRow, stride);
    memcpy(bottomRow, tempRow.data(), stride);
  }
}

void ConvertBGRToRGB(uint8_t* buffer, int width, int height, int bytesPerPixel) {
  for (int y = 0; y < height; ++y) {
    uint8_t* row = buffer + y * width * bytesPerPixel;
    for (int x = 0; x < width; ++x) {
      uint8_t* pixel = row + x * bytesPerPixel;
      // Swap B and R channels
      std::swap(pixel[0], pixel[2]);
    }
  }
}

HRESULT TextureHandler::InitializeSinkWriter(const std::string& path, IMFSinkWriter **ppWriter, DWORD *pStreamIndex) {
  *ppWriter = NULL;
  *pStreamIndex = NULL;

  IMFSinkWriter   *pSinkWriter = NULL;
  IMFMediaType    *pMediaTypeOut = NULL;   
  IMFMediaType    *pMediaTypeIn = NULL;   
  DWORD           streamIndex;  

  // Create output file
  std::wstring file_path = StringToWString(path);
  HRESULT hr = MFCreateSinkWriterFromURL(file_path.c_str(), NULL, NULL, &pSinkWriter);

  // Set the output media type
  if (SUCCEEDED(hr)) {
    hr = MFCreateMediaType(&pMediaTypeOut);
  }
  if (SUCCEEDED(hr)) {
    pMediaTypeOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pMediaTypeOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    pMediaTypeOut->SetUINT32(MF_MT_AVG_BITRATE, 5000000);
    pMediaTypeOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(pMediaTypeOut, MF_MT_FRAME_SIZE, preview_frame_width_, preview_frame_height_);
    MFSetAttributeRatio(pMediaTypeOut, MF_MT_FRAME_RATE, video_fps_, 1);
    MFSetAttributeRatio(pMediaTypeOut, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  }

  // Set the input media type
  if (SUCCEEDED(hr)){
    hr = MFCreateMediaType(&pMediaTypeIn);   
  }
  if (SUCCEEDED(hr)) {
    pMediaTypeIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pMediaTypeIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    pMediaTypeIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(pMediaTypeIn, MF_MT_FRAME_SIZE, preview_frame_width_, preview_frame_height_);
    MFSetAttributeRatio(pMediaTypeIn, MF_MT_FRAME_RATE, video_fps_, 1);
    MFSetAttributeRatio(pMediaTypeIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  }

  if (SUCCEEDED(hr)) {
    hr = pSinkWriter->AddStream(pMediaTypeOut, &streamIndex);
    if (SUCCEEDED(hr)) {
      hr = pSinkWriter->SetInputMediaType(streamIndex, pMediaTypeIn, NULL);
      
      // Tell the sink writer to start accepting data
      if (SUCCEEDED(hr)) {
        hr = pSinkWriter->BeginWriting();
      }
      
      // Return the pointer to the caller
      if (SUCCEEDED(hr)) {
        *ppWriter = pSinkWriter;
        (*ppWriter)->AddRef();
        *pStreamIndex = streamIndex;
      }
    }
  }

  SafeRelease(&pSinkWriter);
  SafeRelease(&pMediaTypeOut);
  SafeRelease(&pMediaTypeIn);
  return hr;
}

HRESULT TextureHandler::WriteFrame(IMFSinkWriter *pWriter, DWORD streamIndex, const uint8_t* buffer, const LONGLONG& currentSampleTime) {
  IMFSample *pSample = NULL;
  IMFMediaBuffer *pBuffer = NULL;
  const LONG cbWidth = 4 * preview_frame_width_;
  const DWORD cbBuffer = cbWidth * preview_frame_height_;
  BYTE *pData = NULL;

  // Create a new memory buffer
  HRESULT hr = MFCreateMemoryBuffer(cbBuffer, &pBuffer);

  // Lock the buffer and copy the video frame to the buffer.
  if (SUCCEEDED(hr)) {
    hr = pBuffer->Lock(&pData, NULL, NULL);
  }
  if (SUCCEEDED(hr)) {
    std::vector<uint8_t> flippedBuffer(buffer, buffer + cbBuffer);
    FlipVertical(flippedBuffer.data(), preview_frame_width_, preview_frame_height_, 4);
    ConvertBGRToRGB(flippedBuffer.data(), preview_frame_width_, preview_frame_height_, 4);
  
    hr = MFCopyImage(
      pData,                      // Destination buffer
      cbWidth,                    // Destination stride
      flippedBuffer.data(),
      cbWidth,                    // Source stride
      cbWidth,                    // Image width in bytes
      preview_frame_height_       // Image height in pixels
    );
  }
  if (pBuffer) {
    pBuffer->Unlock();
  }

  // Set the data length of the buffer
  if (SUCCEEDED(hr)) {
    hr = pBuffer->SetCurrentLength(cbBuffer);
  }

  // Create a media sample and add the buffer to the sample
  if (SUCCEEDED(hr)) {
    hr = MFCreateSample(&pSample);
  }
  if (SUCCEEDED(hr)) {
    hr = pSample->AddBuffer(pBuffer);
  }

  // Set the time stamp and the duration
  if (SUCCEEDED(hr)) {
    hr = pSample->SetSampleTime(currentSampleTime);
  }
  if (SUCCEEDED(hr)) {
    hr = pSample->SetSampleDuration(video_frame_duration_);
  }

  // Send the sample to the Sink Writer
  if (SUCCEEDED(hr)) {
    hr = pWriter->WriteSample(streamIndex, pSample);
  }

  SafeRelease(&pSample);
  SafeRelease(&pBuffer);
  return hr;
}

HRESULT TextureHandler::StartRecording(const std::string& path) {
  HRESULT hr = S_OK;

  hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  if (SUCCEEDED(hr)) {
    
    hr = MFStartup(MF_VERSION);
    if (SUCCEEDED(hr)) {
      
      hr = InitializeSinkWriter(path, &pSinkWriter_, &stream);
      if (SUCCEEDED(hr)) {
        isRecording_ = true;
      }
    }
  }

  return hr;
}


HRESULT TextureHandler::StopRecording() {
  HRESULT hr = S_OK;

  isRecording_ = false;
  rtStart = 0;

  hr = pSinkWriter_->Finalize();
  if (SUCCEEDED(hr)) {
    SafeRelease(&pSinkWriter_);
    MFShutdown();
  }
  CoUninitialize();

  return hr;
}

}  // namespace camera_windows
