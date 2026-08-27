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

  const uint32_t pixels_total = preview_frame_width_ * preview_frame_height_;
  uint32_t src_data_size = pixels_total * 4;
  if (frame_format_.compare(FRAME_FORMAT_Y16) == 0 || frame_format_.compare(FRAME_FORMAT_YUV) == 0) {
    src_data_size = pixels_total * 2;
  } else if (frame_format_.compare(FRAME_FORMAT_NV12) == 0) {
    src_data_size = (pixels_total * 3) / 2;
  }
  const uint32_t dst_data_size = pixels_total * 4;
  const uint32_t raw_data_size = (frame_format_.compare(FRAME_FORMAT_Y16) == 0) ? src_data_size : dst_data_size;

  if (src_data_size > 0 && source_buffer_.size() == src_data_size) {
    if (dest_buffer_.size() != dst_data_size) {
      dest_buffer_.resize(dst_data_size);
    }
    if (raw_buffer_.size() != raw_data_size) {
      raw_buffer_.resize(raw_data_size);
    }

    FlutterDesktopPixel* dst = reinterpret_cast<FlutterDesktopPixel*>(dest_buffer_.data());

    auto clamp255 = [](int v) -> uint8_t {
      return (uint8_t)((v < 0) ? 0 : ((v > 255) ? 255 : v));
    };

    auto YUV2RGB = [&clamp255](int Y, int U, int V, uint8_t& r, uint8_t& g, uint8_t& b) {
      int c = Y;
      int d = U - 128;
      int e = V - 128;
      // Full range BT.601 to RGB
      r = clamp255(c + (int)(1.402f * e));
      g = clamp255(c - (int)(0.344136f * d) - (int)(0.714136f * e));
      b = clamp255(c + (int)(1.772f * d));
    };

    if (frame_format_.compare(FRAME_FORMAT_Y16) == 0) {
      // UVC-Y16
      uint16_t* src = reinterpret_cast<uint16_t*>(source_buffer_.data());
      uint16_t* raw_y16 = reinterpret_cast<uint16_t*>(raw_buffer_.data());

      // min-max normalization
      uint16_t min_val = 65535;
      uint16_t max_val = 0;
      for (uint32_t i = 0; i < pixels_total; i++) {
        if (src[i] < min_val) min_val = src[i];
        if (src[i] > max_val) max_val = src[i];
      }
      float range = (max_val > min_val) ? static_cast<float>(max_val - min_val) : 1.0f;

      for (uint32_t y = 0; y < preview_frame_height_; y++) {
        for (uint32_t x = 0; x < preview_frame_width_; x++) {
          // Software mirror mode.
          // IMFCapturePreviewSink also has the SetMirrorState setting,
          // but if enabled, samples will not be processed.

          // Calculates mirrored pixel position.
          uint32_t sp = (y * preview_frame_width_) + x;
          uint32_t tp = mirror_preview_ ? ((y * preview_frame_width_) + ((preview_frame_width_ - 1) - x)) : sp;

          // min-max normalization
          uint8_t gray = static_cast<uint8_t>(((src[sp] - min_val) / range) * 255.0f);

          // snapshot
          raw_y16[tp] = src[sp];

          // preview
          dst[tp].r = gray;
          dst[tp].g = gray;
          dst[tp].b = gray;
          dst[tp].a = 255;
        }
      }
    } else if (frame_format_.compare(FRAME_FORMAT_YUV) == 0) {
      // UVC-YUV (YUY2 / YUYV)
      uint8_t* src = reinterpret_cast<uint8_t*>(source_buffer_.data());
      FlutterDesktopPixel* raw = reinterpret_cast<FlutterDesktopPixel*>(raw_buffer_.data());

      for (uint32_t y = 0; y < preview_frame_height_; y++) {
        for (uint32_t x = 0; x < preview_frame_width_; x += 2) {
          uint32_t sp = (y * preview_frame_width_ + x) * 2; // byte offset
          
          uint8_t y0 = src[sp];
          uint8_t u  = src[sp + 1];
          uint8_t y1 = src[sp + 2];
          uint8_t v  = src[sp + 3];

          uint8_t r0, g0, b0, r1, g1, b1;
          YUV2RGB(y0, u, v, r0, g0, b0);
          YUV2RGB(y1, u, v, r1, g1, b1);
          
          uint32_t tp0 = mirror_preview_ ? (y * preview_frame_width_ + (preview_frame_width_ - 1 - x)) : (y * preview_frame_width_ + x);
          uint32_t tp1 = mirror_preview_ ? (y * preview_frame_width_ + (preview_frame_width_ - 1 - (x + 1))) : (y * preview_frame_width_ + (x + 1));

          dst[tp0].r = r0; dst[tp0].g = g0; dst[tp0].b = b0; dst[tp0].a = 255;
          dst[tp1].r = r1; dst[tp1].g = g1; dst[tp1].b = b1; dst[tp1].a = 255;
          raw[tp0] = dst[tp0];
          raw[tp1] = dst[tp1];
        }
      }
    } else if (frame_format_.compare(FRAME_FORMAT_NV12) == 0) {
      // UVC-NV12
      uint8_t* src_y = reinterpret_cast<uint8_t*>(source_buffer_.data());
      uint8_t* src_uv = src_y + pixels_total;
      FlutterDesktopPixel* raw = reinterpret_cast<FlutterDesktopPixel*>(raw_buffer_.data());

      for (uint32_t y = 0; y < preview_frame_height_; y++) {
        uint32_t uv_y = y / 2;
        for (uint32_t x = 0; x < preview_frame_width_; x++) {
          uint32_t y_idx = y * preview_frame_width_ + x;
          uint32_t uv_idx = uv_y * preview_frame_width_ + (x & ~1); // x floor to even
          
          uint8_t Y = src_y[y_idx];
          uint8_t U = src_uv[uv_idx];
          uint8_t V = src_uv[uv_idx + 1];

          uint8_t r, g, b;
          YUV2RGB(Y, U, V, r, g, b);

          uint32_t tp = mirror_preview_ ? (y * preview_frame_width_ + (preview_frame_width_ - 1 - x)) : y_idx;

          dst[tp].r = r; dst[tp].g = g; dst[tp].b = b; dst[tp].a = 255;
          raw[tp] = dst[tp];
        }
      }
    } else {
      // MFVideoFormat_RGB32
      MFVideoFormatRGB32Pixel* src = reinterpret_cast<MFVideoFormatRGB32Pixel*>(source_buffer_.data());
      FlutterDesktopPixel* raw = reinterpret_cast<FlutterDesktopPixel*>(raw_buffer_.data());
      for (uint32_t y = 0; y < preview_frame_height_; y++) {
        for (uint32_t x = 0; x < preview_frame_width_; x++) {
          // Software mirror mode.
          // IMFCapturePreviewSink also has the SetMirrorState setting,
          // but if enabled, samples will not be processed.

          // Calculates mirrored pixel position.
          // Check the frame format
          uint32_t sp = (y * preview_frame_width_) + x;
          uint32_t tp = mirror_preview_ ? ((y * preview_frame_width_) + ((preview_frame_width_ - 1) - x)) : sp;

          // snapshot
          raw[tp].r = src[sp].r;
          raw[tp].g = src[sp].g;
          raw[tp].b = src[sp].b;
          raw[tp].a = 255;

          // preview
          dst[tp].r = src[sp].b;
          dst[tp].g = src[sp].b;
          dst[tp].b = src[sp].b;
          dst[tp].a = 255;
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
      capture_controller_listener_->OnStreamedFrameAvailable(raw_buffer_.data(), raw_data_size);
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
