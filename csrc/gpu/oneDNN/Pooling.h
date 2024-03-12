#pragma once

#include <ATen/ATen.h>
#include <ATen/record_function.h>

#include <oneDNN/Runtime.h>
#include <operators/comm/Scalar.h>
#include <runtime/Utils.h>
#include <tensor/Tensor.h>
#include <utils/LRUCache.h>
#include "Reorder.h"
#include "Utils.h"

#include <oneapi/dnnl/dnnl.hpp>

using namespace xpu::dpcpp;
using namespace at::AtenIpexTypeXPU;
using namespace at::AtenIpexTypeQuantizedXPU;

namespace xpu {
namespace oneDNN {

static inline bool is_valid_pooling(
    std::vector<int64_t> src,
    std::vector<int64_t> dst,
    std::vector<int64_t> ker,
    std::vector<int64_t> str,
    std::vector<int64_t> pad) {
  for (int i = 0; i < 2; i++) {
    // oneDNN only support: input size is divisible by the output size
    if (src[i] % dst[i] != 0)
      return false;

    if ((src[i] - ker[i] + pad[i] + pad[i]) / str[i] + 1 != dst[i]) {
      return false;
    }
  }
  return true;
}

using alg = dnnl::algorithm;

template <alg alg_kind>
static at::Tensor pooling(
    at::Tensor& dst,
    const at::Tensor& src,
    int64_t nbatch,
    int64_t nInputPlane,
    int64_t srcDepth,
    int64_t srcHeight,
    int64_t srcWidth,
    int64_t dstDepth,
    int64_t dstHeight,
    int64_t dstWidth,
    std::vector<int64_t>& stride_vec,
    std::vector<int64_t>& kernel_vec,
    std::vector<int64_t>& dilation_vec,
    std::vector<int64_t>& padding_l_vec,
    std::vector<int64_t>& padding_r_vec) {
  at::Device curDevice = at::Device(at::kXPU, current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);
  auto strm = GpuStreamManager::Instance().get_stream();

  prop_kind prop_kind = dnnl::prop_kind::forward_training;
  auto data_t = get_onednn_dtype(src);
  if (data_t == memory::data_type::f16 || data_t == memory::data_type::s8 ||
      data_t == memory::data_type::u8 || data_t == memory::data_type::s32) {
    prop_kind = dnnl::prop_kind::forward_inference;
  }

  memory::format_tag format;

  memory::dims src_tz;
  memory::dims dst_tz;

  auto ndim = src.ndimension();
  // FIXME:
  // XPU path only supports pool2d (ndim equals 3 or 4)
  // and pool3d (ndim equals 4 or 5)
  // need to suuport the pool1d operator (ndim equals 2 or 3)
  TORCH_CHECK(
      ndim == 3 || ndim == 4 || ndim == 5,
      "oneDNN only supports pooling with 3-dim, 4-dim or 5-dim.");

  if ((ndim == 4 && srcDepth == 0) || ndim == 3) {
    /*
      This path is used for AvgPool2d/AdaptiveAvgPool2d
      Takeing AvgPool2d for example, PyTorch support two cases of AvgPool2d:
      1. 3D: Input (C, H, W),  Output (C, H0, W0), Kernel (kH, kW)
      For this case, the nbatch (n) value set as 1,
      and does not support channel last format. For a 3-dim tensor,
      the PyTorch suggest_memory_format can only be Contiguous or
      ChannelsLast1D (nwc), the ChannelsLast1D (nwc) does not match the
      sementics of Input (C, H, W) case. Then the suggest_memory_format can
      only be Contiguous for this Pool2d with 3-dim input, and the corresponding
      oneDNN format is nchw.
      2. 4D: Input (N, C, H, W),  Output (N, C, H0, W0), Kernel (kH, kW)
      This case supports Contiguous and ChannelsLast2D memory_format.
      For Contiguous situation, the oneDNN format is nchw.
      For ChannelsLast2D situation, the oneDNN format is nhwc.
    */
    format = is_smf_channels_last(src) ? memory::format_tag::nhwc
                                       : memory::format_tag::nchw;
    src_tz = {nbatch, nInputPlane, srcHeight, srcWidth};
    dst_tz = {nbatch, nInputPlane, dstHeight, dstWidth};
  } else if (ndim == 5 || (ndim == 4 && srcDepth != 0)) {
    /*
      This path is used for AvgPool3d/AdaptiveAvgPool3d.
      Takeing MaxPool3d for example, PyTorch support two cases of MaxPool3d:
      1. 4D: Input (C, D, H, W),  Output (C, D0, H0, W0), Kernel (kD, kH, kW)
      For this case, the nbatch (n) value set as 1, and srcDepth != 0
      The srcDepth != 0 is used to distinguish from Pooling2D (N, C, H, W).
      This case does not support channel last format. For a 4-dim tensor,
      the PyTorch suggest_memory_format can only be Contiguous or
      ChannelsLast (nhwc), the ChannelsLast (nhwc) does not match the
      sementics of Input (C, D, H, W) case. Then the suggest_memory_format
      can only be Contiguous for this Pool2d with 3-dim input, and the
      corresponding oneDNN format is nchw.
      2. 5D: Input (N, C, D, H, W),  Output (N, C, D0, H0, W0), Kernel (kD, kH,
      kW) This case supports Contiguous and ChannelsLast2D memory_format. For
      Contiguous situation, the oneDNN format is ncdhw. For ChannelsLast2D
      situation, the oneDNN format is ndhwc.
    */
    format = is_smf_channels_last(src) ? memory::format_tag::ndhwc
                                       : memory::format_tag::ncdhw;
    src_tz = {nbatch, nInputPlane, srcDepth, srcHeight, srcWidth};
    dst_tz = {nbatch, nInputPlane, dstDepth, dstHeight, dstWidth};
  }

  auto src_ctx = at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(src);
  // propagate blocked format from src to dst
  bool is_onednn_layout_suggested = !src_ctx.is_plain();
  auto src_usr_md = is_onednn_layout_suggested
      ? src_ctx.meta()
      : memory::desc({src_tz}, data_t, format);
  auto dst_usr_md = memory::desc({dst_tz}, data_t, format);
  auto dst_md = is_onednn_layout_suggested
      ? memory::desc({dst_tz}, data_t, memory::format_tag::any)
      : dst_usr_md;

  auto pooling_fwd_pd = pooling_forward::primitive_desc(
      engine,
      prop_kind,
      alg_kind,
      src_usr_md,
      dst_md,
      stride_vec,
      kernel_vec,
      dilation_vec,
      padding_l_vec,
      padding_r_vec);

  memory src_m, dst_m;
  if (!is_onednn_layout_suggested) {
    src_m = dpcpp_onednn_memory(src_usr_md, engine, src.data_ptr());
    dst_m = dpcpp_onednn_memory(dst_usr_md, engine, dst.data_ptr());
  } else {
    src_m = dpcpp_onednn_memory(src_usr_md, engine, src.data_ptr());

    auto expected_dst_md = pooling_fwd_pd.dst_desc();
    if (expected_dst_md != dst_usr_md) {
      // reallocate memory due to padding needed by oneDNN in some blk fmt
      if (src.is_quantized()) {
        auto quantizer = dpcpp_make_per_tensor_affine_quantizer(
            src.q_scale(),
            src.q_zero_point(),
            at::typeMetaToScalarType(src.options().dtype()));
        dst = empty_opaque_qtensor(expected_dst_md, c10::nullopt, quantizer);
      } else {
        dst = empty_opaque_tensor(expected_dst_md, src.options(), c10::nullopt);
      }
      dst_m = dpcpp_onednn_memory(expected_dst_md, engine, dst.data_ptr());
    } else {
      dst_m = dpcpp_onednn_memory(dst_usr_md, engine, dst.data_ptr());
    }
  }

  auto pooling_fwd = pooling_forward(pooling_fwd_pd);

  DPCPP_ONEDNN_EXEC(
      pooling_fwd, strm, {{DNNL_ARG_SRC, src_m}, {DNNL_ARG_DST, dst_m}});
  return dst;
} // namespace oneDNN

template <algorithm alg_kind>
static std::tuple<at::Tensor, at::Tensor> pooling(
    at::Tensor& dst,
    at::Tensor& idx,
    const at::Tensor& src,
    int64_t nbatch,
    int64_t nInputPlane,
    int64_t srcDepth,
    int64_t srcHeight,
    int64_t srcWidth,
    int64_t dstDepth,
    int64_t dstHeight,
    int64_t dstWidth,
    std::vector<int64_t>& stride_vec,
    std::vector<int64_t>& kernel_vec,
    std::vector<int64_t>& dilation_vec,
    std::vector<int64_t>& padding_l_vec,
    std::vector<int64_t>& padding_r_vec) {
  at::Device curDevice = at::Device(at::kXPU, current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);
  auto strm = GpuStreamManager::Instance().get_stream();

  auto prop_kind = dnnl::prop_kind::forward_training;
  auto data_t = get_onednn_dtype(src);
  if (data_t == memory::data_type::f16 || data_t == memory::data_type::s8 ||
      data_t == memory::data_type::u8 || data_t == memory::data_type::s32) {
    prop_kind = dnnl::prop_kind::forward_inference;
  }

  memory::format_tag format;
  memory::dims src_tz;
  memory::dims dst_tz;

  auto ndim = src.ndimension();
  // FIXME:
  // XPU path only supports pool2d (ndim equals 3 or 4)
  // and pool3d (ndim equals 4 or 5)
  // need to suuport the pool1d operator (ndim equals 2 or 3)
  TORCH_CHECK(
      ndim == 3 || ndim == 4 || ndim == 5,
      "oneDNN only supports pooling with 3-dim, 4-dim or 5-dim.");

  if ((ndim == 4 && srcDepth == 0) || ndim == 3) {
    /*
      This path is used for MaxPool2d/AdaptiveMaxPool2d
      Takeing MaxPool2d for example, PyTorch support two cases of MaxPool2d:
      1. 3D: Input (C, H, W),  Output (C, H0, W0), Kernel (kH, kW)
      For this case, the nbatch (n) value set as 1,
      and does not support channel last format. For a 3-dim tensor,
      the PyTorch suggest_memory_format can only be Contiguous or
      ChannelsLast1D (nwc), the ChannelsLast1D (nwc) does not match the
      sementics of Input (C, H, W) case. Then the suggest_memory_format can
      only be Contiguous for this Pool2d with 3-dim input, and the corresponding
      oneDNN format is nchw.
      2. 4D: Input (N, C, H, W),  Output (N, C, H0, W0), Kernel (kH, kW)
      This case supports Contiguous and ChannelsLast2D memory_format.
      For Contiguous situation, the oneDNN format is nchw.
      For ChannelsLast2D situation, the oneDNN format is nhwc.
    */
    format = is_smf_channels_last(src) ? memory::format_tag::nhwc
                                       : memory::format_tag::nchw;
    src_tz = {nbatch, nInputPlane, srcHeight, srcWidth};
    dst_tz = {nbatch, nInputPlane, dstHeight, dstWidth};
  } else if (ndim == 5 || (ndim == 4 && srcDepth != 0)) {
    /*
      This path is used for MaxPool3d/AdaptiveMaxPool3d.
      Takeing MaxPool3d for example, PyTorch support two cases of MaxPool3d:
      1. 4D: Input (C, D, H, W),  Output (C, D0, H0, W0), Kernel (kD, kH, kW)
      For this case, the nbatch (n) value set as 1, and srcDepth != 0
      The srcDepth != 0 is used to distinguish from Pooling2D (N, C, H, W).
      This case does not support channel last format. For a 4-dim tensor,
      the PyTorch suggest_memory_format can only be Contiguous or
      ChannelsLast (nhwc), the ChannelsLast (nhwc) does not match the
      sementics of Input (C, D, H, W) case. Then the suggest_memory_format
      can only be Contiguous for this Pool2d with 3-dim input, and the
      corresponding oneDNN format is nchw.
      2. 5D: Input (N, C, D, H, W),  Output (N, C, D0, H0, W0), Kernel (kD, kH,
      kW) This case supports Contiguous and ChannelsLast2D memory_format. For
      Contiguous situation, the oneDNN format is ncdhw. For ChannelsLast2D
      situation, the oneDNN format is ndhwc.
    */
    format = is_smf_channels_last(src) ? memory::format_tag::ndhwc
                                       : memory::format_tag::ncdhw;
    src_tz = {nbatch, nInputPlane, srcDepth, srcHeight, srcWidth};
    dst_tz = {nbatch, nInputPlane, dstDepth, dstHeight, dstWidth};
  }

  auto src_ctx = at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(src);
  // propagate blocked format from src to dst
  bool is_onednn_layout_suggested = !src_ctx.is_plain();
  auto src_usr_md = is_onednn_layout_suggested
      ? src_ctx.meta()
      : memory::desc(src_tz, data_t, format);
  auto idx_usr_md = memory::desc(dst_tz, data_t, format);
  auto dst_usr_md = memory::desc(dst_tz, data_t, format);
  auto dst_md = is_onednn_layout_suggested
      ? memory::desc(dst_tz, data_t, memory::format_tag::any)
      : dst_usr_md;

  auto pooling_fwd_pd = pooling_forward::primitive_desc(
      engine,
      prop_kind,
      alg_kind,
      src_usr_md,
      dst_md,
      stride_vec,
      kernel_vec,
      dilation_vec,
      padding_l_vec,
      padding_r_vec);

  auto expected_dst_md = pooling_fwd_pd.dst_desc();

  memory src_usr_m, dst_usr_m;
  if (!is_onednn_layout_suggested) {
    src_usr_m = dpcpp_onednn_memory(src_usr_md, engine, src.data_ptr());
    dst_usr_m = dpcpp_onednn_memory(dst_usr_md, engine, dst.data_ptr());
  } else {
    src_usr_m = dpcpp_onednn_memory(src_usr_md, engine, src.data_ptr());

    if (expected_dst_md != dst_usr_md) {
      // reallocate memory due to padding needed by oneDNN in some blk fmt
      if (src.is_quantized()) {
        auto quantizer = dpcpp_make_per_tensor_affine_quantizer(
            src.q_scale(),
            src.q_zero_point(),
            at::typeMetaToScalarType(src.options().dtype()));
        dst = empty_opaque_qtensor(expected_dst_md, c10::nullopt, quantizer);
      } else {
        dst = empty_opaque_tensor(expected_dst_md, src.options(), c10::nullopt);
      }
      dst_usr_m = dpcpp_onednn_memory(expected_dst_md, engine, dst.data_ptr());
    } else {
      dst_usr_m = dpcpp_onednn_memory(dst_usr_md, engine, dst.data_ptr());
    }
  }

  auto src_m = src_usr_m;
  auto dst_m = dst_usr_m;

  if (prop_kind == dnnl::prop_kind::forward_training) {
    at::Tensor idx_;
    memory idx_m;
    if (!is_onednn_layout_suggested) {
      idx_ = at::empty_like(idx, at::TensorOptions().dtype(at::kInt));
      idx_m = dpcpp_onednn_memory(idx_usr_md, engine, idx_.data_ptr());
    } else {
      auto expected_idx_md = pooling_fwd_pd.workspace_desc();
      idx_ = empty_opaque_tensor(
          expected_idx_md,
          at::TensorOptions(at::kXPU).dtype(at::kInt),
          c10::nullopt);
      idx_m = dpcpp_onednn_memory(expected_idx_md, engine, idx_.data_ptr());
    }

    auto pooling_fwd = pooling_forward(pooling_fwd_pd);

    DPCPP_ONEDNN_EXEC(
        pooling_fwd,
        strm,
        {{DNNL_ARG_SRC, src_m},
         {DNNL_ARG_DST, dst_m},
         {DNNL_ARG_WORKSPACE, idx_m}});

    if (!is_onednn_layout_suggested) {
      idx.copy_(idx_);
    } else {
      // reorder if materialized
      auto idx_internal_ctx = DPCPPTensorContext::release_tensor_ctx(idx_);
      DPCPPTensorContext::set_tensor_ctx(idx, std::move(idx_internal_ctx));
    }
  } else {
    idx = at::empty({dst_tz}, at::TensorOptions(at::kXPU).dtype(at::kInt));
    auto pooling_fwd = pooling_forward(pooling_fwd_pd);
    DPCPP_ONEDNN_EXEC(
        pooling_fwd, strm, {{DNNL_ARG_SRC, src_m}, {DNNL_ARG_DST, dst_m}});
  }

  return {dst, idx};
} // namespace xpu

template <alg alg_kind>
static at::Tensor pooling_backward(
    at::Tensor& diff_src,
    const at::Tensor& diff_dst,
    const at::Tensor& src,
    int64_t nbatch,
    int64_t nInputPlane,
    int64_t diff_src_depth,
    int64_t diff_src_height,
    int64_t diff_src_width,
    int64_t diff_dst_depth,
    int64_t diff_dst_height,
    int64_t diff_dst_width,
    std::vector<int64_t>& stride_vec,
    std::vector<int64_t>& kernel_vec,
    std::vector<int64_t>& dilation_vec,
    std::vector<int64_t>& padding_l_vec,
    std::vector<int64_t>& padding_r_vec) {
  at::Device curDevice = at::Device(at::kXPU, current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);
  auto strm = GpuStreamManager::Instance().get_stream();
  prop_kind prop_kind = dnnl::prop_kind::forward_training;

  auto data_t = get_onednn_dtype(diff_dst);
  TORCH_CHECK(
      data_t == memory::data_type::f32 || data_t == memory::data_type::bf16,
      "oneDNN only supports pooling backward with fp32 and bf16 datatype");

  memory::format_tag format;

  memory::dims diff_src_tz;
  memory::dims diff_dst_tz;
  memory::dims kernel;
  memory::dims stride;
  memory::dims dilation;
  memory::dims padding;

  auto ndim = src.ndimension();
  // FIXME:
  // XPU path only supports pool2d (ndim equals 3 or 4)
  // and pool3d (ndim equals 4 or 5)
  // need to suuport the pool1d operator (ndim equals 2 or 3)
  TORCH_CHECK(
      ndim == 3 || ndim == 4 || ndim == 5,
      "oneDNN only supports pooling with 3-dim, 4-dim or 5-dim.");

  if ((ndim == 4 && diff_src_depth == 0) || ndim == 3) {
    /*
      This path is used for AvgPool2d/AdaptiveAvgPool2d
      Takeing AvgPool2d for example, PyTorch support two cases of AvgPool2d:
      1. 3D: Input (C, H, W),  Output (C, H0, W0), Kernel (kH, kW)
         For this case, the nbatch (n) value set as 1,
         and does not support channel last format. For a 3-dim tensor,
         the PyTorch suggest_memory_format can only be Contiguous or
         ChannelsLast1D (nwc), the ChannelsLast1D (nwc) does not match the
         sementics of Input (C, H, W) case. Then the suggest_memory_format can
      only be Contiguous for this Pool2d with 3-dim input, and the corresponding
         oneDNN format is nchw.
      2. 4D: Input (N, C, H, W),  Output (N, C, H0, W0), Kernel (kH, kW)
         This case supports Contiguous and ChannelsLast2D memory_format.
         For Contiguous situation, the oneDNN format is nchw.
         For ChannelsLast2D situation, the oneDNN format is nhwc.
    */
    format = is_smf_channels_last(src) ? memory::format_tag::nhwc
                                       : memory::format_tag::nchw;
    diff_src_tz = {nbatch, nInputPlane, diff_src_height, diff_src_width};
    diff_dst_tz = {nbatch, nInputPlane, diff_dst_height, diff_dst_width};
  } else if (ndim == 5 || (ndim == 4 && diff_src_depth != 0)) {
    /*
      This path is used for AvgPool3d/AdaptiveAvgPool3d.
      Takeing MaxPool3d for example, PyTorch support two cases of MaxPool3d:
      1. 4D: Input (C, D, H, W),  Output (C, D0, H0, W0), Kernel (kD, kH, kW)
         For this case, the nbatch (n) value set as 1, and srcDepth != 0
         The srcDepth != 0 is used to distinguish from Pooling2D (N, C, H, W).
         This case does not support channel last format. For a 4-dim tensor,
         the PyTorch suggest_memory_format can only be Contiguous or
         ChannelsLast (nhwc), the ChannelsLast (nhwc) does not match the
         sementics of Input (C, D, H, W) case. Then the suggest_memory_format
      can only be Contiguous for this Pool2d with 3-dim input, and the
      corresponding oneDNN format is nchw.
      2. 5D: Input (N, C, D, H, W),  Output (N, C, D0, H0, W0), Kernel (kD, kH,
      kW) This case supports Contiguous and ChannelsLast2D memory_format. For
      Contiguous situation, the oneDNN format is ncdhw. For ChannelsLast2D
      situation, the oneDNN format is ndhwc.
    */
    format = is_smf_channels_last(src) ? memory::format_tag::ndhwc
                                       : memory::format_tag::ncdhw;
    diff_src_tz = {
        nbatch, nInputPlane, diff_src_depth, diff_src_height, diff_src_width};
    diff_dst_tz = {
        nbatch, nInputPlane, diff_dst_depth, diff_dst_height, diff_dst_width};
  }

  auto diff_src_usr_md = memory::desc({diff_src_tz}, data_t, format);
  // src should have the same size of diff_src_tz
  auto src_ctx = DPCPPTensorContext::get_tensor_ctx(src);
  auto src_usr_md = src_ctx.is_plain() ? diff_src_usr_md : src_ctx.meta();

  auto diff_dst_ctx =
      at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(diff_dst);
  bool is_onednn_layout_suggested = !diff_dst_ctx.is_plain();
  auto diff_dst_usr_md = is_onednn_layout_suggested
      ? diff_dst_ctx.meta()
      : memory::desc({diff_dst_tz}, data_t, format);
  auto diff_src_md = is_onednn_layout_suggested
      ? memory::desc({diff_src_tz}, data_t, memory::format_tag::any)
      : diff_src_usr_md;

  auto pooling_fwd_pd = pooling_forward::primitive_desc(
      engine,
      prop_kind,
      alg_kind,
      src_usr_md,
      diff_dst_usr_md,
      stride_vec,
      kernel_vec,
      dilation_vec,
      padding_l_vec,
      padding_r_vec);

  auto pooling_bwd_pd = dnnl::pooling_backward::primitive_desc(
      engine,
      alg_kind,
      diff_src_md,
      diff_dst_usr_md,
      stride_vec,
      kernel_vec,
      dilation_vec,
      padding_l_vec,
      padding_r_vec,
      pooling_fwd_pd);

  auto pooling_bwd = dnnl::pooling_backward(pooling_bwd_pd);

  memory diff_src_m, diff_dst_m;
  if (!is_onednn_layout_suggested) {
    diff_dst_m =
        dpcpp_onednn_memory(diff_dst_usr_md, engine, diff_dst.data_ptr());

    diff_src_m =
        dpcpp_onednn_memory(diff_src_usr_md, engine, diff_src.data_ptr());
  } else {
    diff_dst_m =
        dpcpp_onednn_memory(diff_dst_usr_md, engine, diff_dst.data_ptr());

    auto expected_diff_src_md = pooling_bwd_pd.diff_src_desc();
    if (expected_diff_src_md != diff_src_usr_md) {
      diff_src = empty_opaque_tensor(
          expected_diff_src_md, diff_dst.options(), c10::nullopt);
      diff_src_m = dpcpp_onednn_memory(
          expected_diff_src_md, engine, diff_src.data_ptr());
    } else {
      diff_src_m =
          dpcpp_onednn_memory(diff_src_usr_md, engine, diff_src.data_ptr());
    }
  }

  DPCPP_ONEDNN_EXEC(
      pooling_bwd,
      strm,
      {{DNNL_ARG_DIFF_DST, diff_dst_m}, {DNNL_ARG_DIFF_SRC, diff_src_m}});

  return diff_src;
}

template <alg alg_kind>
static at::Tensor pooling_backward(
    at::Tensor& diff_src,
    const at::Tensor& diff_dst,
    const at::Tensor& src,
    const at::Tensor& idx,
    int64_t nbatch,
    int64_t nInputPlane,
    int64_t diff_src_depth,
    int64_t diff_src_height,
    int64_t diff_src_width,
    int64_t diff_dst_depth,
    int64_t diff_dst_height,
    int64_t diff_dst_width,
    std::vector<int64_t>& stride_vec,
    std::vector<int64_t>& kernel_vec,
    std::vector<int64_t>& dilation_vec,
    std::vector<int64_t>& padding_l_vec,
    std::vector<int64_t>& padding_r_vec) {
  at::Device curDevice = at::Device(at::kXPU, current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);
  auto strm = GpuStreamManager::Instance().get_stream();

  auto prop_kind = dnnl::prop_kind::forward_training;
  auto data_t = get_onednn_dtype(diff_dst);
  TORCH_CHECK(
      data_t == memory::data_type::f32 || data_t == memory::data_type::bf16,
      "oneDNN only supports pooling backward with fp32 and bf16 datatype");

  memory::format_tag format;
  memory::dims diff_src_tz;
  memory::dims diff_dst_tz;

  auto ndim = src.ndimension();
  // FIXME:
  // XPU path only supports pool2d (ndim equals 3 or 4)
  // and pool3d (ndim equals 4 or 5)
  // need to suuport the pool1d operator (ndim equals 2 or 3)
  TORCH_CHECK(
      ndim == 3 || ndim == 4 || ndim == 5,
      "oneDNN only supports pooling with 3-dim, 4-dim or 5-dim.");

  if ((ndim == 4 && diff_src_depth == 0) || ndim == 3) {
    /*
      This path is used for MaxPool2d/AdaptiveMaxPool2d
      Takeing MaxPool2d for example, PyTorch support two cases of MaxPool2d:
      1. 3D: Input (C, H, W),  Output (C, H0, W0), Kernel (kH, kW)
      For this case, the nbatch (n) value set as 1,
      and does not support channel last format. For a 3-dim tensor,
      the PyTorch suggest_memory_format can only be Contiguous or
      ChannelsLast1D (nwc), the ChannelsLast1D (nwc) does not match the
      sementics of Input (C, H, W) case. Then the suggest_memory_format can
      only be Contiguous for this Pool2d with 3-dim input, and the corresponding
      oneDNN format is nchw.
      2. 4D: Input (N, C, H, W),  Output (N, C, H0, W0), Kernel (kH, kW)
      This case supports Contiguous and ChannelsLast2D memory_format.
      For Contiguous situation, the oneDNN format is nchw.
      For ChannelsLast2D situation, the oneDNN format is nhwc.
    */
    format = is_smf_channels_last(src) ? memory::format_tag::nhwc
                                       : memory::format_tag::nchw;
    diff_src_tz = {nbatch, nInputPlane, diff_src_height, diff_src_width};
    diff_dst_tz = {nbatch, nInputPlane, diff_dst_height, diff_dst_width};
  } else if (ndim == 5 || (ndim == 4 && diff_src_depth != 0)) {
    /*
      This path is used for MaxPool3d/AdaptiveMaxPool3d.
      Takeing MaxPool3d for example, PyTorch support two cases of MaxPool3d:
      1. 4D: Input (C, D, H, W),  Output (C, D0, H0, W0), Kernel (kD, kH, kW)
      For this case, the nbatch (n) value set as 1, and srcDepth != 0
      The srcDepth != 0 is used to distinguish from Pooling2D (N, C, H, W).
      This case does not support channel last format. For a 4-dim tensor,
      the PyTorch suggest_memory_format can only be Contiguous or
      ChannelsLast (nhwc), the ChannelsLast (nhwc) does not match the
      sementics of Input (C, D, H, W) case. Then the suggest_memory_format
      can only be Contiguous for this Pool2d with 3-dim input, and the
      corresponding oneDNN format is nchw.
      2. 5D: Input (N, C, D, H, W),  Output (N, C, D0, H0, W0), Kernel (kD, kH,
      kW) This case supports Contiguous and ChannelsLast2D memory_format. For
      Contiguous situation, the oneDNN format is ncdhw. For ChannelsLast2D
      situation, the oneDNN format is ndhwc.
    */
    format = is_smf_channels_last(src) ? memory::format_tag::ndhwc
                                       : memory::format_tag::ncdhw;
    diff_src_tz = {
        nbatch, nInputPlane, diff_src_depth, diff_src_height, diff_src_width};
    diff_dst_tz = {
        nbatch, nInputPlane, diff_dst_depth, diff_dst_height, diff_dst_width};
  }

  auto diff_src_usr_md = memory::desc({diff_src_tz}, data_t, format);
  // src should have the same size of diff_src_tz
  auto src_ctx = DPCPPTensorContext::get_tensor_ctx(src);
  auto src_usr_md = src_ctx.is_plain() ? diff_src_usr_md : src_ctx.meta();

  auto diff_dst_ctx =
      at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(diff_dst);
  // propagate blocked format fom diff_dst to diff_src
  bool is_onednn_layout_suggested = !diff_dst_ctx.is_plain();
  auto diff_dst_usr_md = is_onednn_layout_suggested
      ? diff_dst_ctx.meta()
      : memory::desc({diff_dst_tz}, data_t, format);
  auto diff_src_md = is_onednn_layout_suggested
      ? memory::desc({diff_src_tz}, data_t, memory::format_tag::any)
      : diff_src_usr_md;

  auto pooling_fwd_pd = pooling_forward::primitive_desc(
      engine,
      prop_kind,
      alg_kind,
      src_usr_md,
      diff_dst_usr_md,
      stride_vec,
      kernel_vec,
      dilation_vec,
      padding_l_vec,
      padding_r_vec);

  auto pooling_bwd_pd = dnnl::pooling_backward::primitive_desc(
      engine,
      alg_kind,
      diff_src_md,
      diff_dst_usr_md,
      stride_vec,
      kernel_vec,
      dilation_vec,
      padding_l_vec,
      padding_r_vec,
      pooling_fwd_pd);

  auto expected_diff_src_md = pooling_bwd_pd.diff_src_desc();
  memory diff_src_usr_m, diff_dst_usr_m, idx_usr_m;
  if (!is_onednn_layout_suggested) {
    diff_dst_usr_m =
        dpcpp_onednn_memory(diff_dst_usr_md, engine, diff_dst.data_ptr());
    diff_src_usr_m =
        dpcpp_onednn_memory(diff_src_usr_md, engine, diff_src.data_ptr());
  } else {
    diff_dst_usr_m =
        dpcpp_onednn_memory(diff_dst_usr_md, engine, diff_dst.data_ptr());

    if (expected_diff_src_md != diff_src_usr_md) {
      diff_src = empty_opaque_tensor(
          expected_diff_src_md, diff_dst.options(), c10::nullopt);
      diff_src_usr_m = dpcpp_onednn_memory(
          expected_diff_src_md, engine, diff_src.data_ptr());
    } else {
      diff_src_usr_m =
          dpcpp_onednn_memory(diff_src_usr_md, engine, diff_src.data_ptr());
    }
  }

  auto diff_dst_m = diff_dst_usr_m;
  auto diff_src_m = diff_src_usr_m;

  auto expexted_idx_md = pooling_bwd_pd.workspace_desc();
  auto idx_ctx = at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(idx);
  at::Tensor idx_usr = idx;
  if (idx_ctx.is_plain()) {
    idx_usr = idx.to(kInt);

    idx_usr_m = dpcpp_onednn_memory(
        {diff_dst_tz,
         (memory::data_type)expexted_idx_md.get_data_type(),
         format},
        engine,
        idx_usr.data_ptr());
  } else {
    idx_usr_m = dpcpp_onednn_memory(idx_ctx.meta(), engine, idx.data_ptr());
  }

  at::Tensor idx_opt;
  auto idx_m = idx_usr_m;
  if (is_onednn_layout_suggested && idx_usr_m.get_desc() != expexted_idx_md) {
    idx_opt = empty_opaque_tensor(
        expexted_idx_md,
        at::TensorOptions(at::kXPU).dtype(at::kInt),
        c10::nullopt);
    idx_m = dpcpp_onednn_memory(expexted_idx_md, engine, idx_opt.data_ptr());

    MSG("issue reorder");

    xpu::oneDNN::reorder(idx_usr, idx_opt);
  }

  auto pooling_bwd = dnnl::pooling_backward(pooling_bwd_pd);
  DPCPP_ONEDNN_EXEC(
      pooling_bwd,
      strm,
      {{DNNL_ARG_DIFF_DST, diff_dst_m},
       {DNNL_ARG_DIFF_SRC, diff_src_m},
       {DNNL_ARG_WORKSPACE, idx_m}});

  return diff_src;
}

} // namespace oneDNN
} // namespace xpu
