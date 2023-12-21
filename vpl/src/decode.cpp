#include <cstring>

#include <d3d11_allocator.h>
#include <sample_defs.h>
#include <sample_utils.h>

#include "callback.h"
#include "common.h"
#include "system.h"

#define LOG_MODULE "VPL DECODE"
#include "log.h"

#define CHECK_STATUS(X, MSG)                                                   \
  {                                                                            \
    mfxStatus __sts = (X);                                                     \
    if (__sts < MFX_ERR_NONE) {                                                \
      MSDK_PRINT_RET_MSG(__sts, MSG);                                          \
      LOG_ERROR(MSG + "failed, sts=" + std::to_string((int)__sts));            \
      return __sts;                                                            \
    }                                                                          \
  }

namespace {

class VplDecoder {
public:
  std::unique_ptr<NativeDevice> native_ = nullptr;
  MFXVideoSession session_;
  MFXVideoDECODE *mfxDEC_ = NULL;
  std::vector<mfxFrameSurface1> pmfxSurfaces_;
  mfxVideoParam mfxVideoParams_;
  bool initialized_ = false;
  D3D11FrameAllocator d3d11FrameAllocator_;
  mfxFrameAllocResponse mfxResponse_;

  void *device_;
  int64_t luid_;
  API api_;
  DataFormat codecID_;
  bool outputSharedHandle_;

  bool bt709_ = false;
  bool full_range_ = false;

  VplDecoder(void *device, int64_t luid, API api, DataFormat codecID,
             bool outputSharedHandle) {
    device_ = device;
    luid_ = luid;
    api_ = api;
    codecID_ = codecID;
    outputSharedHandle_ = outputSharedHandle;
    ZeroMemory(&mfxVideoParams_, sizeof(mfxVideoParams_));
    ZeroMemory(&mfxResponse_, sizeof(mfxResponse_));
  }

  mfxStatus init() {
    mfxStatus sts = MFX_ERR_NONE;
    native_ = std::make_unique<NativeDevice>();
    if (!native_->Init(luid_, (ID3D11Device *)device_, 4)) {
      LOG_ERROR("Failed to initialize native device");
      return MFX_ERR_DEVICE_FAILED;
    }
    sts = InitializeMFX();
    CHECK_STATUS(sts, "InitializeMFX");

    // Create Media SDK decoder
    mfxDEC_ = new MFXVideoDECODE(session_);
    if (!mfxDEC_) {
      LOG_ERROR("Failed to create MFXVideoDECODE");
      return MFX_ERR_NOT_INITIALIZED;
    }

    memset(&mfxVideoParams_, 0, sizeof(mfxVideoParams_));
    if (!convert_codec(codecID_, mfxVideoParams_.mfx.CodecId)) {
      LOG_ERROR("Unsupported codec");
      return MFX_ERR_UNSUPPORTED;
    }

    mfxVideoParams_.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
    // AsyncDepth: sSpecifies how many asynchronous operations an
    // application performs before the application explicitly synchronizes the
    // result. If zero, the value is not specified
    mfxVideoParams_.AsyncDepth = 1; // Not important.
    // DecodedOrder: For AVC and HEVC, used to instruct the decoder
    // to return output frames in the decoded order. Must be zero for all other
    // decoders.
    mfxVideoParams_.mfx.DecodedOrder = true; // Not important.

    // Validate video decode parameters (optional)
    sts = mfxDEC_->Query(&mfxVideoParams_, &mfxVideoParams_);
    CHECK_STATUS(sts, "Query");

    return MFX_ERR_NONE;
  }

  int decode(uint8_t *data, int len, DecodeCallback callback, void *obj) {
    mfxStatus sts = MFX_ERR_NONE;
    mfxSyncPoint syncp;
    mfxFrameSurface1 *pmfxOutSurface = NULL;
    int nIndex = 0;
    bool decoded = false;
    mfxBitstream mfxBS;

    setBitStream(&mfxBS, data, len);
    if (!initialized_) {
      sts = initializeDecode(&mfxBS, false);
      CHECK_STATUS(sts, "initializeDecode");
      initialized_ = true;
    }
    setBitStream(&mfxBS, data, len);

    int loop_counter = 0;
    do {
      if (loop_counter++ > 100) {
        std::cerr << "mfx decode loop two many times" << std::endl;
        break;
      }

      if (MFX_WRN_DEVICE_BUSY == sts)
        MSDK_SLEEP(1);
      if (MFX_ERR_MORE_SURFACE == sts || MFX_ERR_NONE == sts) {
        nIndex = GetFreeSurfaceIndex(
            pmfxSurfaces_.data(),
            pmfxSurfaces_.size()); // Find free frame surface
        if (nIndex >= pmfxSurfaces_.size()) {
          LOG_ERROR("GetFreeSurfaceIndex failed, nIndex=" +
                    std::to_string(nIndex));
          return -1;
        }
      }

      sts = mfxDEC_->DecodeFrameAsync(&mfxBS, &pmfxSurfaces_[nIndex],
                                      &pmfxOutSurface, &syncp);

      if (MFX_ERR_INCOMPATIBLE_VIDEO_PARAM == sts) {
        // https://github.com/FFmpeg/FFmpeg/blob/f84412d6f4e9c1f1d1a2491f9337d7e789c688ba/libavcodec/qsvdec.c#L736
        setBitStream(&mfxBS, data, len);
        LOG_INFO("Incompatible video parameters, resetting decoder");
        sts = initializeDecode(&mfxBS, true);
        CHECK_STATUS(sts, "initialize");
        continue;
      }

      // Ignore warnings if output is available,
      if (MFX_ERR_NONE < sts && syncp)
        sts = MFX_ERR_NONE;

      if (MFX_ERR_NONE == sts)
        sts = session_.SyncOperation(syncp, 1000);
      if (MFX_ERR_NONE == sts) {
        if (!pmfxOutSurface) {
          LOG_ERROR("pmfxOutSurface is null");
          return -1;
        }
        if (!convert(pmfxOutSurface)) {
          LOG_ERROR("Failed to convert");
          return -1;
        }
        void *output = nullptr;
        if (outputSharedHandle_) {
          HANDLE sharedHandle = native_->GetSharedHandle();
          if (!sharedHandle) {
            LOG_ERROR("Failed to GetSharedHandle");
            return -1;
          }
          output = sharedHandle;
        } else {
          output = native_->GetCurrentTexture();
        }

        if (MFX_ERR_NONE == sts) {
          if (callback)
            callback(output, obj);
          decoded = true;
        }
        break;
      }
    } while (MFX_ERR_NONE <= sts || MFX_ERR_MORE_SURFACE == sts);

    if (!decoded) {
      std::cerr << "decoded failed, sts=" << sts << std::endl;
    }

    return decoded ? 0 : -1;
  }

private:
  mfxStatus InitializeMFX() {
    mfxStatus sts = MFX_ERR_NONE;
    mfxIMPL impl = MFX_IMPL_HARDWARE_ANY | MFX_IMPL_VIA_D3D11;
    mfxVersion ver = {{0, 1}};
    D3D11AllocatorParams allocParams;

    sts = session_.Init(impl, &ver);
    CHECK_STATUS(sts, "session Init");

    sts = session_.SetHandle(MFX_HANDLE_D3D11_DEVICE, native_->device_.Get());
    CHECK_STATUS(sts, "SetHandle");

    allocParams.bUseSingleTexture = false; // important
    allocParams.pDevice = native_->device_.Get();
    allocParams.uncompressedResourceMiscFlags = 0;
    sts = d3d11FrameAllocator_.Init(&allocParams);
    CHECK_STATUS(sts, "init D3D11FrameAllocator");

    sts = session_.SetFrameAllocator(&d3d11FrameAllocator_);
    CHECK_STATUS(sts, "SetFrameAllocator");

    return MFX_ERR_NONE;
  }

  bool convert_codec(DataFormat dataFormat, mfxU32 &CodecId) {
    switch (dataFormat) {
    case H264:
      CodecId = MFX_CODEC_AVC;
      return true;
    case H265:
      CodecId = MFX_CODEC_HEVC;
      return true;
    }
    return false;
  }

  mfxStatus initializeDecode(mfxBitstream *mfxBS, bool reinit) {
    mfxStatus sts = MFX_ERR_NONE;
    mfxFrameAllocRequest Request;
    memset(&Request, 0, sizeof(Request));
    mfxU16 numSurfaces;
    mfxU16 width, height;
    mfxU8 bitsPerPixel = 12; // NV12
    mfxU32 surfaceSize;
    mfxU8 *surfaceBuffers;

    // mfxExtVideoSignalInfo got MFX_ERR_INVALID_VIDEO_PARAM
    // mfxExtVideoSignalInfo video_signal_info = {0};

    // https://spec.oneapi.io/versions/1.1-rev-1/elements/oneVPL/source/API_ref/VPL_func_vid_decode.html#mfxvideodecode-decodeheader
    sts = mfxDEC_->DecodeHeader(mfxBS, &mfxVideoParams_);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    sts = mfxDEC_->QueryIOSurf(&mfxVideoParams_, &Request);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    numSurfaces = Request.NumFrameSuggested;

    // Request.Type |= WILL_READ; // This line is only required for Windows
    // DirectX11 to ensure that surfaces can be retrieved by the application

    // Allocate surfaces for decoder
    if (reinit) {
      sts = d3d11FrameAllocator_.FreeFrames(&mfxResponse_);
      MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
    }
    sts = d3d11FrameAllocator_.AllocFrames(&Request, &mfxResponse_);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Allocate surface headers (mfxFrameSurface1) for decoder
    pmfxSurfaces_.resize(numSurfaces);
    for (int i = 0; i < numSurfaces; i++) {
      memset(&pmfxSurfaces_[i], 0, sizeof(mfxFrameSurface1));
      pmfxSurfaces_[i].Info = mfxVideoParams_.mfx.FrameInfo;
      pmfxSurfaces_[i].Data.MemId =
          mfxResponse_
              .mids[i]; // MID (memory id) represents one video NV12 surface
    }

    // Initialize the Media SDK decoder
    if (reinit) {
      // https://github.com/FFmpeg/FFmpeg/blob/f84412d6f4e9c1f1d1a2491f9337d7e789c688ba/libavcodec/qsvdec.c#L181
      sts = mfxDEC_->Close();
      MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
    }
    sts = mfxDEC_->Init(&mfxVideoParams_);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
    return MFX_ERR_NONE;
  }

  void setBitStream(mfxBitstream *mfxBS, uint8_t *data, int len) {
    memset(mfxBS, 0, sizeof(mfxBitstream));
    mfxBS->Data = data;
    mfxBS->DataLength = len;
    mfxBS->MaxLength = len;
    mfxBS->DataFlag = MFX_BITSTREAM_COMPLETE_FRAME;
  }

  bool convert(mfxFrameSurface1 *pmfxOutSurface) {
    mfxStatus sts = MFX_ERR_NONE;
    mfxHDLPair pair = {NULL};
    sts = d3d11FrameAllocator_.GetFrameHDL(pmfxOutSurface->Data.MemId,
                                           (mfxHDL *)&pair);
    if (MFX_ERR_NONE != sts) {
      LOG_ERROR("Failed to GetFrameHDL");
      return false;
    }
    ID3D11Texture2D *texture = (ID3D11Texture2D *)pair.first;
    D3D11_TEXTURE2D_DESC desc2D;
    texture->GetDesc(&desc2D);
    if (!native_->EnsureTexture(desc2D.Width, desc2D.Height)) {
      LOG_ERROR("Failed to EnsureTexture");
      return false;
    }
    native_->next(); // comment out to remove picture shaking
    native_->BeginQuery();

    // nv12 -> bgra
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc;
    ZeroMemory(&contentDesc, sizeof(contentDesc));
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputFrameRate.Numerator = 60;
    contentDesc.InputFrameRate.Denominator = 1;
    contentDesc.InputWidth = pmfxOutSurface->Info.CropW;
    contentDesc.InputHeight = pmfxOutSurface->Info.CropH;
    contentDesc.OutputWidth = pmfxOutSurface->Info.CropW;
    contentDesc.OutputHeight = pmfxOutSurface->Info.CropH;
    contentDesc.OutputFrameRate.Numerator = 60;
    contentDesc.OutputFrameRate.Denominator = 1;
    DXGI_COLOR_SPACE_TYPE colorSpace_out =
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    DXGI_COLOR_SPACE_TYPE colorSpace_in;
    if (bt709_) {
      if (full_range_) {
        colorSpace_in = DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709;
      } else {
        colorSpace_in = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
      }
    } else {
      if (full_range_) {
        colorSpace_in = DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601;
      } else {
        colorSpace_in = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
      }
    }
    if (!native_->Process(texture, native_->GetCurrentTexture(), contentDesc,
                          colorSpace_in, colorSpace_out)) {
      LOG_ERROR("Failed to process");
      native_->EndQuery();
      return false;
    }

    native_->context_->Flush();
    native_->EndQuery();
    if (!native_->Query()) {
      LOG_ERROR("Failed to query");
      return false;
    }
    return true;
  }
};

} // namespace

extern "C" {

int vpl_destroy_decoder(void *decoder) {
  VplDecoder *p = (VplDecoder *)decoder;
  if (p) {
    if (p->mfxDEC_) {
      p->mfxDEC_->Close();
      delete p->mfxDEC_;
    }
  }
  return 0;
}

void *vpl_new_decoder(void *device, int64_t luid, API api, DataFormat codecID,
                      bool outputSharedHandle) {
  VplDecoder *p = NULL;
  try {
    p = new VplDecoder(device, luid, api, codecID, outputSharedHandle);
    if (p) {
      if (p->init() == MFX_ERR_NONE) {
        return p;
      }
    }
  } catch (const std::exception &e) {
    LOG_ERROR("new failed: " + e.what());
  }

  if (p) {
    vpl_destroy_decoder(p);
    delete p;
    p = NULL;
  }
  return NULL;
}

int vpl_decode(void *decoder, uint8_t *data, int len, DecodeCallback callback,
               void *obj) {
  try {
    VplDecoder *p = (VplDecoder *)decoder;
    return p->decode(data, len, callback, obj);
  } catch (const std::exception &e) {
    LOG_ERROR("decode failed: " + e.what());
  }
  return -1;
}

int vpl_test_decode(AdapterDesc *outDescs, int32_t maxDescNum,
                    int32_t *outDescNum, API api, DataFormat dataFormat,
                    bool outputSharedHandle, uint8_t *data, int32_t length) {
  try {
    AdapterDesc *descs = (AdapterDesc *)outDescs;
    Adapters adapters;
    if (!adapters.Init(ADAPTER_VENDOR_INTEL))
      return -1;
    int count = 0;
    for (auto &adapter : adapters.adapters_) {
      VplDecoder *p =
          (VplDecoder *)vpl_new_decoder(nullptr, LUID(adapter.get()->desc1_),
                                        api, dataFormat, outputSharedHandle);
      if (!p)
        continue;
      if (vpl_decode(p, data, length, nullptr, nullptr) == 0) {
        AdapterDesc *desc = descs + count;
        desc->luid = LUID(adapter.get()->desc1_);
        count += 1;
        if (count >= maxDescNum)
          break;
      }
    }
    *outDescNum = count;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
  }
  return -1;
}
} // extern "C"
