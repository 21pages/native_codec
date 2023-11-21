#include <public/common/AMFFactory.h>
#include <public/common/AMFSTL.h>
#include <public/common/Thread.h>
#include <public/common/TraceAdapter.h>
#include <public/include/components/VideoEncoderAV1.h>
#include <public/include/components/VideoEncoderHEVC.h>
#include <public/include/components/VideoEncoderVCE.h>
#include <public/include/core/Platform.h>
#include <stdio.h>

#include <cstring>
#include <iostream>
#include <math.h>

#include "callback.h"
#include "common.h"
#include "system.h"

#define LOG_MODULE "AMF ENCODE"
#include "log.h"

#define AMF_FACILITY L"AMFEncoder"
#define MILLISEC_TIME 10000

namespace {

#define AMF_CHECK_RETURN(res, msg)                                             \
  if (res != AMF_OK) {                                                         \
    LOG_ERROR(msg + ", result code: " + std::to_string(int(res)));             \
    return res;                                                                \
  }

/** Encoder output packet */
struct encoder_packet {
  uint8_t *data; /**< Packet data */
  size_t size;   /**< Packet size */

  int64_t pts; /**< Presentation timestamp */
  int64_t dts; /**< Decode timestamp */

  int32_t timebase_num; /**< Timebase numerator */
  int32_t timebase_den; /**< Timebase denominator */

  bool keyframe; /**< Is a keyframe */

  /* ---------------------------------------------------------------- */
  /* Internal video variables (will be parsed automatically) */

  /* DTS in microseconds */
  int64_t dts_usec;

  /* System DTS in microseconds */
  int64_t sys_dts_usec;
};

class AMFEncoder {

public:
  AMF_RESULT init_result_ = AMF_FAIL;
  DataFormat dataFormat_;
  amf::AMFComponentPtr AMFEncoder_ = NULL;
  amf::AMFContextPtr AMFContext_ = NULL;

private:
  // system
  void *handle_;
  // AMF Internals
  AMFFactoryHelper AMFFactory_;
  amf::AMF_MEMORY_TYPE AMFMemoryType_;
  amf::AMF_SURFACE_FORMAT AMFSurfaceFormat_ = amf::AMF_SURFACE_BGRA;
  std::pair<int32_t, int32_t> resolution_;
  amf_wstring codec_;
  // const
  AMF_COLOR_BIT_DEPTH_ENUM eDepth_ = AMF_COLOR_BIT_DEPTH_8;
  int query_timeout_ = 500;
  int32_t bitRateIn_;
  int32_t frameRate_;
  int32_t gop_;
  int32_t q_min_;
  int32_t q_max_;

  // Buffers
  std::vector<uint8_t> packetDataBuffer_;

public:
  AMFEncoder(void *handle, amf::AMF_MEMORY_TYPE memoryType, amf_wstring codec,
             DataFormat dataFormat, int32_t width, int32_t height,
             int32_t bitrate, int32_t framerate, int32_t gop, int32_t q_min,
             int32_t q_max) {
    handle_ = handle;
    dataFormat_ = dataFormat;
    AMFMemoryType_ = memoryType;
    resolution_ = std::make_pair(width, height);
    codec_ = codec;
    bitRateIn_ = bitrate;
    frameRate_ = framerate;
    gop_ = gop;
    q_min_ = q_min;
    q_max_ = q_max;
    init_result_ = initialize();
  }

  AMF_RESULT encode(void *tex, EncodeCallback callback, void *obj) {
    amf::AMFSurfacePtr surface = NULL;
    amf::AMFComputeSyncPointPtr pSyncPoint = NULL;
    AMF_RESULT res;
    bool encoded = false;

    switch (AMFMemoryType_) {
    case amf::AMF_MEMORY_DX11:
      // https://github.com/GPUOpen-LibrariesAndSDKs/AMF/issues/280
      // AMF will not copy the surface during the CreateSurfaceFromDX11Native
      // call
      res = AMFContext_->CreateSurfaceFromDX11Native(tex, &surface, NULL);
      AMF_CHECK_RETURN(res, "CreateSurfaceFromDX11Native failed");
      {
        amf::AMFDataPtr data1;
        surface->Duplicate(surface->GetMemoryType(), &data1);
        surface = amf::AMFSurfacePtr(data1);
      }
      break;
    default:
      break;
    }
    res = AMFEncoder_->SubmitInput(surface);
    AMF_CHECK_RETURN(res, "SubmitInput failed");

    amf::AMFDataPtr data = NULL;
    do {
      data = NULL;
      res = AMFEncoder_->QueryOutput(&data);
      if (res == AMF_REPEAT) {
        amf_sleep(1);
      }
    } while (res == AMF_REPEAT);
    if (res == AMF_OK && data != NULL) {
      struct encoder_packet packet;
      PacketKeyframe(data, &packet);
      amf::AMFBufferPtr pBuffer = amf::AMFBufferPtr(data);
      packet.size = pBuffer->GetSize();
      if (packet.size > 0) {
        if (packetDataBuffer_.size() < packet.size) {
          size_t newBufferSize = (size_t)exp2(ceil(log2((double)packet.size)));
          packetDataBuffer_.resize(newBufferSize);
        }
        packet.data = packetDataBuffer_.data();
        std::memcpy(packet.data, pBuffer->GetNative(), packet.size);
        if (callback)
          callback(packet.data, packet.size, packet.keyframe, obj);
        encoded = true;
      }
      pBuffer = NULL;
    }
    data = NULL;
    pSyncPoint = NULL;
    surface = NULL;
    return encoded ? AMF_OK : AMF_FAIL;
  }

  AMF_RESULT destroy() {
    if (AMFEncoder_) {
      AMFEncoder_->Terminate();
      AMFEncoder_ = NULL;
    }
    if (AMFContext_) {
      AMFContext_->Terminate();
      AMFContext_ = NULL; // AMFContext_ is the last
    }
    AMFFactory_.Terminate();
    return AMF_OK;
  }

  AMF_RESULT test() {
    AMF_RESULT res = AMF_OK;
    amf::AMFSurfacePtr surface = nullptr;
    res = AMFContext_->AllocSurface(AMFMemoryType_, AMFSurfaceFormat_,
                                    resolution_.first, resolution_.second,
                                    &surface);
    AMF_CHECK_RETURN(res, "AllocSurface failed");
    if (surface->GetPlanesCount() < 1)
      return AMF_FAIL;
    void *native = surface->GetPlaneAt(0)->GetNative();
    if (!native)
      return AMF_FAIL;
    return encode(native, nullptr, nullptr);
  }

  AMF_RESULT set_qp(int32_t q_min, int32_t q_max) {
    bool q_valid = q_min >= 0 && q_min <= 51 && q_max >= 0 && q_max <= 51 &&
                   q_min <= q_max;
    if (!q_valid) {
      LOG_WARN("invalid qp range: [" + std::to_string(q_min) + ", " +
               std::to_string(q_max) + "]");
      return AMF_FAIL;
    }
    AMF_RESULT res = AMF_FAIL;
    switch (dataFormat_) {
    case H264:
      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_MIN_QP, q_min_);
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_MIN_QP failed, val = " +
                           std::to_string(q_min_));

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_MAX_QP, q_max_);
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_MAX_QP failed, val = " +
                           std::to_string(q_max_));
      break;
    case H265:
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_HEVC_GOP_SIZE failed");

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_MIN_QP_I, q_min_);
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_HEVC_MIN_QP_I failed");

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_MAX_QP_I, q_max_);
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_HEVC_MAX_QP_I failed");

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_MIN_QP_P, q_min_);
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_HEVC_MIN_QP_P failed");

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_MAX_QP_P, q_max_);
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_HEVC_MAX_QP_P failed");
      break;
    }

    return res;
  }

private:
  AMF_RESULT initialize() {
    AMF_RESULT res;

    res = AMFFactory_.Init();
    if (res != AMF_OK) {
      std::cerr << "AMF init failed, error code = " << res << "\n";
      return res;
    }
    amf::AMFSetCustomTracer(AMFFactory_.GetTrace());
    amf::AMFTraceEnableWriter(AMF_TRACE_WRITER_CONSOLE, true);
    amf::AMFTraceSetWriterLevel(AMF_TRACE_WRITER_CONSOLE, AMF_TRACE_WARNING);

    // AMFContext_
    res = AMFFactory_.GetFactory()->CreateContext(&AMFContext_);
    AMF_CHECK_RETURN(res, "CreateContext failed");

    switch (AMFMemoryType_) {
    case amf::AMF_MEMORY_DX11:
      res = AMFContext_->InitDX11(handle_); // can be DX11 device
      AMF_CHECK_RETURN(res, "InitDX11 failed");
      break;
    case amf::AMF_MEMORY_VULKAN:
      res = amf::AMFContext1Ptr(AMFContext_)->InitVulkan(handle_);
      AMF_CHECK_RETURN(res, "InitVulkan failed");
      break;
    case amf::AMF_MEMORY_OPENCL:
      res = AMFContext_->InitOpenCL(NULL);
      AMF_CHECK_RETURN(res, "InitOpenCL failed");
      break;
    default:
      LOG_ERROR("unsupported amf memory type");
      return AMF_FAIL;
    }

    // component: encoder
    res = AMFFactory_.GetFactory()->CreateComponent(AMFContext_, codec_.c_str(),
                                                    &AMFEncoder_);
    AMF_CHECK_RETURN(res, "CreateComponent failed");

    res = SetParams(codec_);
    AMF_CHECK_RETURN(res, "Could not set params in encoder.");

    res = AMFEncoder_->Init(AMFSurfaceFormat_, resolution_.first,
                            resolution_.second);
    AMF_CHECK_RETURN(res, "encoder->Init() failed");

    return AMF_OK;
  }

  AMF_RESULT SetParams(const amf_wstring &codecStr) {
    AMF_RESULT res;
    if (codecStr == amf_wstring(AMFVideoEncoderVCE_AVC)) {
      // ------------- Encoder params usage---------------
      res = AMFEncoder_->SetProperty(
          AMF_VIDEO_ENCODER_USAGE,
          AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY_HIGH_QUALITY);
      AMF_CHECK_RETURN(res, "SetProperty AMF_VIDEO_ENCODER_USAGE failed");

      // ------------- Encoder params static---------------
      res = AMFEncoder_->SetProperty(
          AMF_VIDEO_ENCODER_FRAMESIZE,
          ::AMFConstructSize(resolution_.first, resolution_.second));
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_FRAMESIZE failed, (" +
                           std::to_string(resolution_.first) + "," +
                           std::to_string(resolution_.second) + ")");
      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_LOWLATENCY_MODE failed");
      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET,
                                     AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY);
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_QUALITY_PRESET failed");
      res =
          AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_COLOR_BIT_DEPTH, eDepth_);
      AMF_CHECK_RETURN(res,
                       "SetProperty(AMF_VIDEO_ENCODER_COLOR_BIT_DEPTH  failed");
      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                                     AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD");

      // ------------- Encoder params dynamic ---------------
      AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, 0);
      // do not check error for AMF_VIDEO_ENCODER_B_PIC_PATTERN
      // - can be not supported - check Capability Manager
      // sample
      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_QUERY_TIMEOUT,
                                     query_timeout_); // ms
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_QUERY_TIMEOUT failed");
      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE,
                                     bitRateIn_);
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_TARGET_BITRATE failed");
      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE,
                                     ::AMFConstructRate(frameRate_, 1));
      AMF_CHECK_RETURN(res, "SetProperty AMF_VIDEO_ENCODER_FRAMERATE failed");

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, gop_);
      AMF_CHECK_RETURN(res, "SetProperty AMF_VIDEO_ENCODER_IDR_PERIOD failed");

      set_qp(q_min_, q_max_);

    } else if (codecStr == amf_wstring(AMFVideoEncoder_HEVC)) {
      // ------------- Encoder params usage---------------
      res = AMFEncoder_->SetProperty(
          AMF_VIDEO_ENCODER_HEVC_USAGE,
          AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY_HIGH_QUALITY);
      AMF_CHECK_RETURN(res, "SetProperty AMF_VIDEO_ENCODER_HEVC_USAGE failed");

      // ------------- Encoder params static---------------
      res = AMFEncoder_->SetProperty(
          AMF_VIDEO_ENCODER_HEVC_FRAMESIZE,
          ::AMFConstructSize(resolution_.first, resolution_.second));
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_HEVC_FRAMESIZE failed");

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE,
                                     true);
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_LOWLATENCY_MODE failed");

      res = AMFEncoder_->SetProperty(
          AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
          AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY);
      AMF_CHECK_RETURN(
          res, "SetProperty AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET failed");

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH,
                                     eDepth_);
      AMF_CHECK_RETURN(
          res, "SetProperty AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH failed");

      res = AMFEncoder_->SetProperty(
          AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
          AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR);
      AMF_CHECK_RETURN(
          res, "SetProperty AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD failed");

      // ------------- Encoder params dynamic ---------------
      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT,
                                     query_timeout_); // ms
      AMF_CHECK_RETURN(
          res, "SetProperty(AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT failed",
          query_timeout_);

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE,
                                     bitRateIn_);
      AMF_CHECK_RETURN(
          res, "SetProperty AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE failed");

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE,
                                     ::AMFConstructRate(frameRate_, 1));
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_HEVC_FRAMERATE failed");

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE,
                                     gop_); // todo
      set_qp(q_min_, q_max_);

    } else {
      return AMF_FAIL;
    }
    return AMF_OK;
  }

  void PacketKeyframe(amf::AMFDataPtr &pData, struct encoder_packet *packet) {
    if (AMFVideoEncoderVCE_AVC == codec_) {
      uint64_t pktType;
      pData->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &pktType);
      packet->keyframe = AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR == pktType ||
                         AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I == pktType;
    } else if (AMFVideoEncoder_HEVC == codec_) {
      uint64_t pktType;
      pData->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &pktType);
      packet->keyframe =
          AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR == pktType ||
          AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_I == pktType;
    }
  }
};

bool convert_codec(DataFormat lhs, amf_wstring &rhs) {
  switch (lhs) {
  case H264:
    rhs = AMFVideoEncoderVCE_AVC;
    break;
  case H265:
    rhs = AMFVideoEncoder_HEVC;
    break;
  default:
    LOG_ERROR("unsupported codec: " + std::to_string((int)lhs));
    return false;
  }
  return true;
}

} // namespace
#include "common.cpp"

extern "C" {

void *amf_new_encoder(void *handle, int64_t luid, API api,
                      DataFormat dataFormat, int32_t width, int32_t height,
                      int32_t kbs, int32_t framerate, int32_t gop,
                      int32_t q_min, int32_t q_max) {
  try {
    if (width % 2 != 0 || height % 2 != 0) {
      return NULL;
    }
    amf_wstring codecStr;
    if (!convert_codec(dataFormat, codecStr)) {
      return NULL;
    }
    amf::AMF_MEMORY_TYPE memoryType;
    if (!convert_api(api, memoryType)) {
      return NULL;
    }
    AMFEncoder *enc =
        new AMFEncoder(handle, memoryType, codecStr, dataFormat, width, height,
                       kbs * 1000, framerate, gop, q_min, q_max);
    if (enc && enc->init_result_ != AMF_OK) {
      enc->destroy();
      delete enc;
      enc = NULL;
    }
    return enc;
  } catch (const std::exception &e) {
    LOG_ERROR("new failed: " + e.what());
  }
  return NULL;
}

int amf_encode(void *encoder, void *tex, EncodeCallback callback, void *obj) {
  try {
    AMFEncoder *enc = (AMFEncoder *)encoder;
    return -enc->encode(tex, callback, obj);
  } catch (const std::exception &e) {
    LOG_ERROR("encode failed: " + e.what());
  }
  return -1;
}

int amf_destroy_encoder(void *encoder) {
  try {
    AMFEncoder *enc = (AMFEncoder *)encoder;
    return enc->destroy();
  } catch (const std::exception &e) {
    LOG_ERROR("destroy failed: " + e.what());
  }
  return -1;
}

int amf_driver_support() {
  try {
    AMFFactoryHelper factory;
    AMF_RESULT res = factory.Init();
    if (res == AMF_OK) {
      factory.Terminate();
      return 0;
    }
  } catch (const std::exception &e) {
  }
  return -1;
}

int amf_test_encode(void *outDescs, int32_t maxDescNum, int32_t *outDescNum,
                    API api, DataFormat dataFormat, int32_t width,
                    int32_t height, int32_t kbs, int32_t framerate, int32_t gop,
                    int32_t q_min, int32_t q_max) {
  try {
    AdapterDesc *descs = (AdapterDesc *)outDescs;
    Adapters adapters;
    if (!adapters.Init(ADAPTER_VENDOR_AMD))
      return -1;
    int count = 0;
    for (auto &adapter : adapters.adapters_) {
      AMFEncoder *e = (AMFEncoder *)amf_new_encoder(
          (void *)adapter.get()->device_.Get(), LUID(adapter.get()->desc1_),
          api, dataFormat, width, height, kbs, framerate, gop, q_min, q_max);
      if (!e)
        continue;
      if (e->test() == AMF_OK) {
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
    LOG_ERROR("test " + std::to_string(kbs) + "failed: " + e.what());
  }
  return -1;
}

int amf_set_bitrate(void *encoder, int32_t kbs) {
  try {
    AMFEncoder *enc = (AMFEncoder *)encoder;
    AMF_RESULT res = AMF_FAIL;
    switch (enc->dataFormat_) {
    case H264:
      res = enc->AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE,
                                          kbs * 1000);
      break;
    case H265:
      res = enc->AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE,
                                          kbs * 1000);
      break;
    }
    return res == AMF_OK ? 0 : -1;
  } catch (const std::exception &e) {
    LOG_ERROR("set bitrate to " + std::to_string(kbs) +
              "k failed: " + e.what());
  }
  return -1;
}

int amf_set_framerate(void *encoder, int32_t framerate) {
  try {
    AMFEncoder *enc = (AMFEncoder *)encoder;
    AMF_RESULT res = AMF_FAIL;
    AMFRate rate = ::AMFConstructRate(framerate, 1);
    switch (enc->dataFormat_) {
    case H264:
      res = enc->AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, rate);
      break;
    case H265:
      res =
          enc->AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, rate);
      break;
    }
    return res == AMF_OK ? 0 : -1;
  } catch (const std::exception &e) {
    LOG_ERROR("set framerate to " + std::to_string(framerate) +
              " failed: " + e.what());
  }
  return -1;
}

int amf_set_qp(void *encoder, int32_t q_min, int32_t q_max) {
  try {
    AMFEncoder *enc = (AMFEncoder *)encoder;
    AMF_RESULT res = enc->set_qp(q_min, q_max);
    return res == AMF_OK ? 0 : -1;
  } catch (const std::exception &e) {
    LOG_ERROR("set qp to [" + std::to_string(q_min) + ", " +
              std::to_string(q_max) + "] failed: " + e.what());
  }
  return -1;
}

} // extern "C"