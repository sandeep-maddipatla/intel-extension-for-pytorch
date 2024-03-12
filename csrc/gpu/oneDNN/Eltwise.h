#pragma once
#include <ATen/ATen.h>
#include <ATen/Config.h>
#include <ATen/record_function.h>

#include <oneDNN/Reorder.h>
#include <oneDNN/Runtime.h>
#include <oneDNN/Utils.h>
#include <runtime/Utils.h>
#include <tensor/Tensor.h>
#include <utils/LRUCache.h>

using namespace dnnl;
using dnnl::algorithm;
using namespace xpu::dpcpp;

namespace xpu {
namespace oneDNN {

template <algorithm alg_kind>
static inline void eltwise(
    at::Tensor& dst,
    const at::Tensor& src,
    float alpha = 0,
    float beta = 0) {
  Device curDevice = Device(kXPU, current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);

  MSG("begin");
  std::vector<int64_t> dims;
  for (size_t i = 0; i < src.dim(); i++) {
    dims.push_back(src.size(i));
  }

  memory::dims src_tz = dims;
  auto data_t = get_onednn_dtype(src);
  auto format_data = get_dnnl_default_format(
      src.dim(),
      src.dim() == 4 ? (!src.is_contiguous() &&
                        src.is_contiguous(at::MemoryFormat::ChannelsLast))
                     : (!src.is_contiguous() &&
                        src.is_contiguous(at::MemoryFormat::ChannelsLast3d)));
  auto src_md = memory::desc({src_tz}, data_t, format_data);

  auto src_ctx = at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(src);

  memory src_memory;
  if (src_ctx.is_plain() || src.is_contiguous(at::MemoryFormat::ChannelsLast) ||
      src.is_contiguous(at::MemoryFormat::ChannelsLast3d)) {
    src_memory = dpcpp_onednn_memory(src_md, engine, src.data_ptr());
  } else {
    src_md = src_ctx.is_plain() ? src_md : src_ctx.meta();
    src_memory = dpcpp_onednn_memory(src_md, engine, src.data_ptr());
  }

  primitive_attr attr;
#ifdef USE_SCRATCHPAD_MODE
  attr.set_scratchpad_mode(dnnl::scratchpad_mode::user);
#endif
  auto eltwise_forward_pd = eltwise_forward::primitive_desc(
      engine, prop_kind::forward, alg_kind, src_md, src_md, alpha, beta, attr);

  memory dst_memory;
  if (src_ctx.is_plain()) {
    if (!dst.defined()) {
      dst = src.is_contiguous(at::MemoryFormat::ChannelsLast)
          ? at::empty_like(src, at::MemoryFormat::ChannelsLast)
          : at::empty_like(src);
    }
    dst_memory = dpcpp_onednn_memory(
        eltwise_forward_pd.dst_desc(), engine, dst.data_ptr());
  } else {
    if (dst.defined()) {
      Tensor dst_ = dst;
      auto dst_ctx =
          at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(dst);
      auto dst_md = dst_ctx.meta();
      auto expected_dst_md = eltwise_forward_pd.dst_desc();

      if (dst_md != expected_dst_md) {
        dst_ = at::AtenIpexTypeXPU::empty_opaque_tensor(
            expected_dst_md, src.options(), c10::nullopt);
      }
      if (!dst.is_same(dst_)) {
        dst_ctx = DPCPPTensorContext::release_tensor_ctx(dst_);
        DPCPPTensorContext::set_tensor_ctx(dst, std::move(dst_ctx));
      }

      dst_memory = dpcpp_onednn_memory(expected_dst_md, engine, dst.data_ptr());
    } else {
      auto plain_dst_md = memory::desc({src_tz}, data_t, format_data);
      auto expected_dst_md = eltwise_forward_pd.dst_desc();
      auto dst_opt = src.is_contiguous(at::MemoryFormat::ChannelsLast)
          ? src.options().memory_format(at::MemoryFormat::ChannelsLast)
          : src.options();

      if (plain_dst_md != expected_dst_md) {
        dst = at::AtenIpexTypeXPU::empty_opaque_tensor(
            expected_dst_md, src.options(), c10::nullopt);
        dst_memory =
            dpcpp_onednn_memory(expected_dst_md, engine, dst.data_ptr());
      } else {
        dst = at::empty_like(src);
        dst_memory = dpcpp_onednn_memory(plain_dst_md, engine, dst.data_ptr());
      }
    }
  }

  auto strm = GpuStreamManager::Instance().get_stream();

  auto eltwise_fwd = dnnl::eltwise_forward(eltwise_forward_pd);

#ifdef USE_SCRATCHPAD_MODE
  size_t scratchpad_size = eltwise_forward_pd.scratchpad_desc().get_size();
  Tensor scratchpad_tensor = at::AtenIpexTypeXPU::empty(
      {scratchpad_size}, src.options().dtype(at::kByte), c10::nullopt);
  auto scratchpad_memory = dpcpp_onednn_memory(
      eltwise_forward_pd.scratchpad_desc(),
      engine,
      scratchpad_tensor.data_ptr());
  DPCPP_ONEDNN_EXEC(
      eltwise_fwd,
      strm,
      {{DNNL_ARG_SRC, src_memory},
       {DNNL_ARG_DST, dst_memory},
       {DNNL_ARG_SCRATCHPAD, scratchpad_memory}});
#else
  DPCPP_ONEDNN_EXEC(
      eltwise_fwd,
      strm,
      {{DNNL_ARG_SRC, src_memory}, {DNNL_ARG_DST, dst_memory}});
#endif
}

template <algorithm alg_kind>
static inline void eltwise_backward(
    at::Tensor& diff_src,
    const at::Tensor& src_dst,
    const at::Tensor& diff_dst_,
    float alpha = 0,
    float beta = 0) {
  Device curDevice = Device(kXPU, current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);
  auto strm = GpuStreamManager::Instance().get_stream();

  auto data_t = get_onednn_dtype(src_dst);
  std::vector<int64_t> src_dst_dims;
  for (size_t i = 0; i < src_dst.dim(); i++) {
    src_dst_dims.push_back(src_dst.size(i));
  }

  memory::dims src_dst_tz = src_dst_dims;
  auto format_data = get_dnnl_default_format(
      src_dst.dim(),
      src_dst.dim() == 4
          ? (!src_dst.is_contiguous() &&
             src_dst.is_contiguous(at::MemoryFormat::ChannelsLast))
          : (!src_dst.is_contiguous() &&
             src_dst.is_contiguous(at::MemoryFormat::ChannelsLast3d)));
  auto src_dst_md = memory::desc({src_dst_tz}, data_t, format_data);
  auto diff_dst_md = memory::desc({src_dst_tz}, data_t, format_data);
  Tensor diff_dst;
  if (src_dst.is_contiguous(MemoryFormat::ChannelsLast)) {
    diff_dst = diff_dst_.contiguous(MemoryFormat::ChannelsLast);
  } else if (src_dst.is_contiguous(MemoryFormat::ChannelsLast3d)) {
    diff_dst = diff_dst_.contiguous(MemoryFormat::ChannelsLast3d);
  } else {
    diff_dst = diff_dst_;
  }

  memory src_dst_usr_memory, diff_dst_usr_memory;
  if (!Settings::I().is_onednn_layout_enabled() ||
      src_dst.is_contiguous(at::MemoryFormat::ChannelsLast) ||
      src_dst.is_contiguous(at::MemoryFormat::ChannelsLast3d)) {
    src_dst_usr_memory =
        dpcpp_onednn_memory(src_dst_md, engine, src_dst.data_ptr());
    diff_dst_usr_memory =
        dpcpp_onednn_memory(diff_dst_md, engine, diff_dst.data_ptr());
  } else {
    auto src_dst_ctx =
        at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(src_dst);
    src_dst_md = src_dst_ctx.is_plain() ? src_dst_md : src_dst_ctx.meta();
    src_dst_usr_memory =
        dpcpp_onednn_memory(src_dst_md, engine, src_dst.data_ptr());

    auto diff_dst_ctx =
        at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(diff_dst);
    diff_dst_md = diff_dst_ctx.is_plain() ? diff_dst_md : diff_dst_ctx.meta();
    diff_dst_usr_memory =
        dpcpp_onednn_memory(diff_dst_md, engine, diff_dst.data_ptr());
  }
  auto src_dst_memory = src_dst_usr_memory;
  auto diff_dst_memory = diff_dst_usr_memory;

  primitive_attr attr;
#ifdef USE_SCRATCHPAD_MODE
  attr.set_scratchpad_mode(dnnl::scratchpad_mode::user);
#endif

  auto eltwise_forward_pd = eltwise_forward::primitive_desc(
      engine,
      prop_kind::forward_training,
      alg_kind,
      src_dst_md,
      src_dst_md,
      alpha,
      beta,
      attr);

  Tensor diff_dst__;
  auto expected_dst_md = eltwise_forward_pd.dst_desc();
  if (Settings::I().is_onednn_layout_enabled()) {
    auto src_dst_ctx =
        at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(src_dst);
    auto diff_dst_ctx =
        at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(diff_dst);
    if ((diff_dst_ctx.is_plain()) && (!src_dst_ctx.is_plain())) {
      diff_dst__ =
          empty_opaque_tensor(expected_dst_md, src_dst.options(), c10::nullopt);
      diff_dst_memory =
          dpcpp_onednn_memory(expected_dst_md, engine, diff_dst__.data_ptr());
      diff_dst_md = expected_dst_md;
      MSG("issue reorder");
      xpu::oneDNN::reorder(diff_dst, diff_dst__);
    }
  }

  auto eltwise_backward_pd = eltwise_backward::primitive_desc(
      engine,
      alg_kind,
      diff_dst_md,
      src_dst_md,
      src_dst_md,
      alpha,
      beta,
      eltwise_forward_pd,
      attr);

  memory diff_src_memory;
  if (!Settings::I().is_onednn_layout_enabled()) {
    if (!diff_src.defined()) {
      if (src_dst.is_contiguous(MemoryFormat::ChannelsLast)) {
        diff_src = at::empty_like(src_dst, MemoryFormat::ChannelsLast);
      } else {
        diff_src = at::empty_like(src_dst);
      }
    }
    auto diff_src_md = memory::desc({src_dst_tz, data_t, format_data});
    diff_src_memory =
        dpcpp_onednn_memory(diff_src_md, engine, diff_src.data_ptr());
  } else {
    auto plain_diff_src_md = memory::desc({src_dst_tz}, data_t, format_data);
    auto expected_diff_src_md = eltwise_backward_pd.diff_src_desc();
    if (plain_diff_src_md != expected_diff_src_md) {
      diff_src = at::AtenIpexTypeXPU::empty_opaque_tensor(
          expected_diff_src_md, src_dst.options(), c10::nullopt);
      diff_src_memory = dpcpp_onednn_memory(
          expected_diff_src_md, engine, diff_src.data_ptr());
    } else {
      diff_src = at::empty_like(src_dst);
      diff_src_memory =
          dpcpp_onednn_memory(plain_diff_src_md, engine, diff_src.data_ptr());
    }
  }

  auto eltwise_bwd = dnnl::eltwise_backward(eltwise_backward_pd);

#ifdef USE_SCRATCHPAD_MODE
  size_t scratchpad_size = eltwise_backward_pd.scratchpad_desc().get_size();
  Tensor scratchpad_tensor = at::AtenIpexTypeXPU::empty(
      {scratchpad_size}, src_dst.options().dtype(at::kByte), c10::nullopt);
  auto scratchpad_memory = dpcpp_onednn_memory(
      eltwise_backward_pd.scratchpad_desc(),
      engine,
      scratchpad_tensor.data_ptr());
#endif

  if (alg_kind == algorithm::eltwise_logistic_use_dst_for_bwd) {
    DPCPP_ONEDNN_EXEC(
        eltwise_bwd,
        strm,
        {
            {DNNL_ARG_DST, src_dst_memory},
            {DNNL_ARG_DIFF_DST, diff_dst_memory},
            {DNNL_ARG_DIFF_SRC, diff_src_memory},
#ifdef USE_SCRATCHPAD_MODE
            {DNNL_ARG_SCRATCHPAD, scratchpad_memory},
#endif
        });
  } else {
    DPCPP_ONEDNN_EXEC(
        eltwise_bwd,
        strm,
        {
            {DNNL_ARG_SRC, src_dst_memory},
            {DNNL_ARG_DIFF_DST, diff_dst_memory},
            {DNNL_ARG_DIFF_SRC, diff_src_memory},
#ifdef USE_SCRATCHPAD_MODE
            {DNNL_ARG_SCRATCHPAD, scratchpad_memory},
#endif
        });
  }
}

} // namespace oneDNN
} // namespace xpu
