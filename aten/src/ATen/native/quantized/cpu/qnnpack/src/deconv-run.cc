#include <conv_utils.h>
#include <pytorch_qnnpack.h>
#include <qnnpack_func.h>
#include <qnnpack/indirection.h>
#include <qnnpack/log.h>
#include <qnnpack/math.h>
#include <qnnpack/params.h>

#include <cstring>
#include <memory>

namespace qnnpack {
struct q8conv_context {
  size_t bs;
  size_t ks;
  size_t kc;
  size_t kc_stride;
  size_t m;
  size_t m_stride;
  size_t n;
  size_t n_stride;
  const uint8_t** indirect_a;
  const void* packed_w;
  uint8_t* c;
  size_t c_stride;
  union pytorch_qnnp_conv_quantization_params quantization_params;
  const pytorch_q8conv_ukernel_function ukernel;
};

static void compute_q8conv(
    const struct q8conv_context context[1],
    size_t group_index,
    size_t image_index,
    size_t mr_block_start,
    size_t nr_block_start,
    size_t group_range /* always 1 */,
    size_t image_range /* always 1 */,
    size_t mr_block_size,
    size_t nr_block_size) {
  const size_t bs = context->bs;
  const size_t ks = context->ks;
  const size_t kc = context->kc;
  const size_t kc_stride = context->kc_stride;
  const size_t m = context->m;
  const size_t m_stride = context->m_stride;
  const size_t n = context->n;
  const size_t n_stride = context->n_stride;
  const uint8_t** indirect_a = context->indirect_a;
  const void* packed_w = context->packed_w;
  uint8_t* c = context->c;
  const size_t c_stride = context->c_stride;

  const size_t output_channel_index = group_index * n + nr_block_start;
  context->ukernel(
      mr_block_size,
      nr_block_size,
      kc,
      ks,
      indirect_a +
          (mr_block_start + (image_index + group_index * bs) * m_stride) * ks,
      (const void*)((uintptr_t)packed_w + (nr_block_start + group_index * n_stride) * (kc_stride * sizeof(uint8_t) + sizeof(int32_t))),
      c + (mr_block_start + image_index * m) * c_stride + group_index * n +
          nr_block_start,
      c_stride,
      output_channel_index,
      &context->quantization_params);
};

struct QnnpackDeleter {
  void operator()(pytorch_qnnp_operator_t op) {
    pytorch_qnnp_delete_operator(op);
  }
};

enum pytorch_qnnp_status qnnpackDeConv(
    const conv_param_t& deconv_p,
    void* packed_weights,
    const size_t batch_size,
    const size_t input_height,
    const size_t input_width,
    const uint8_t input_zero_point,
    const uint8_t* input,
    const uint8_t* kernel_zero_points,
    const float* requantization_scales,
    const uint8_t output_zero_point,
    const uint8_t output_min,
    const uint8_t output_max,
    uint8_t* output,
    pthreadpool_t threadpool) {

  if (batch_size == 0) {
    // Doesn't matter what's going on, if no batches, return
    return pytorch_qnnp_status_success;
  }
  // Check all invalid parameters
  const size_t kernel_width = deconv_p.kernel_dims[0];
  const size_t kernel_height = deconv_p.kernel_dims[1];

  const size_t stride_width = deconv_p.stride_dims[0];
  const size_t stride_height = deconv_p.stride_dims[1];

  const size_t dilation_width = deconv_p.dilation[0];
  const size_t dilation_height = deconv_p.dilation[1];

  // Support vars
  const size_t group_input_channels = deconv_p.group_input_channels;
  const size_t group_output_channels = deconv_p.group_output_channels;
  const uint32_t mr = pytorch_qnnp_params.q8conv.mr;
  const uint32_t nr = pytorch_qnnp_params.q8conv.nr;
  const uint32_t kr = pytorch_qnnp_params.q8conv.kr;
  const size_t k_stride = (group_input_channels + (kr - 1)) & -kr;
  const size_t n_stride = (group_output_channels + (nr - 1)) & -nr;

  // Create the kernel
  pytorch_qnnp_operator_t deconvolution =
      static_cast<pytorch_qnnp_operator_t>(
          calloc(1, sizeof(struct pytorch_qnnp_operator)));
  if (deconvolution == nullptr) {
    pytorch_qnnp_log_error(
        "failed to allocate %zu bytes for qnnp_operator structure",
        sizeof(struct pytorch_qnnp_operator));
    pytorch_qnnp_delete_operator(deconvolution);
    return pytorch_qnnp_status_out_of_memory;
  }
  std::unique_ptr<pytorch_qnnp_operator, QnnpackDeleter> qnnpack_uniq_ptr(
      deconvolution);

  // Populate the kernel
  size_t zero_size = sizeof(uint8_t) * k_stride;
  size_t zero_offset = 0;
  if (group_input_channels < 8) {
    zero_size += 8;
    zero_offset = 8;
  }
  void* zero_buffer = malloc(zero_size);
  if (zero_buffer == NULL) {
    pytorch_qnnp_log_error(
        "failed to allocate %zu bytes for zero padding", zero_size);
    pytorch_qnnp_delete_operator(deconvolution);
    return pytorch_qnnp_status_out_of_memory;
  }
  memset(zero_buffer, input_zero_point, zero_size);

  deconvolution->zero_buffer = zero_buffer;
  deconvolution->zero_pointer = (void*) ((uintptr_t) zero_buffer + zero_offset);

  deconvolution->input_padding_top = deconv_p.padding[0];
  deconvolution->input_padding_left = deconv_p.padding[1];
  deconvolution->input_padding_bottom = deconv_p.padding[2];
  deconvolution->input_padding_right = deconv_p.padding[3];
  deconvolution->adjustment_width = deconv_p.adjustment_dims[0];
  deconvolution->adjustment_height = deconv_p.adjustment_dims[1];

  deconvolution->kernel_width = kernel_width;
  deconvolution->kernel_height = kernel_height;
  deconvolution->stride_width = stride_width;
  deconvolution->stride_height = stride_height;
  deconvolution->dilation_width = dilation_width;
  deconvolution->dilation_height = dilation_height;
  deconvolution->groups = deconv_p.groups;
  deconvolution->group_input_channels = group_input_channels;
  deconvolution->group_output_channels = group_output_channels;

  // deconvolution->kernel_zero_point = deconv_p.kernel_zero_points;
  // const float kernel_scale = deconv_p.kernel_scale;
  // const float deconvolution_scale = input_scale * kernel_scale / output_scale;
  deconvolution->conv_quantization_params =
      pytorch_qnnp_compute_conv_quantization_params(
          input_zero_point,
          kernel_zero_points,
          requantization_scales,
          output_zero_point,
          output_min,
          output_max);

  deconvolution->ukernel_type = pytorch_qnnp_ukernel_type_conv;
  deconvolution->format = pytorch_qnnp_format_quint8;

  // Setup the kernel
  const std::array<size_t, 2> output_dims =
      deconv_p.compute_output_dims({input_width, input_height});
  const size_t output_width = output_dims[0];
  const size_t output_height = output_dims[1];
  const size_t kernel_size = kernel_height * kernel_width;
  const size_t output_size = output_height * output_width;
  const size_t groups = deconvolution->groups;
  const size_t output_tile_size = pytorch_qnnp_params.q8conv.mr;
  const size_t tiled_output_size = round_up(output_size, output_tile_size);
  const size_t indirection_buffer_size =
      sizeof(void*) * batch_size * groups * tiled_output_size * kernel_size;

  deconvolution->batch_size = batch_size;
  deconvolution->input_height = input_height;
  deconvolution->input_width = input_width;
  deconvolution->input = input;
  deconvolution->input_pixel_stride = deconv_p.input_channels;
  deconvolution->output_height = output_height;
  deconvolution->output_width = output_width;
  deconvolution->output = output;
  deconvolution->output_pixel_stride = deconv_p.output_channels;

  const void** indirection_buffer = (const void**)realloc(
      deconvolution->indirection_buffer, indirection_buffer_size);
  if (indirection_buffer == NULL) {
    pytorch_qnnp_log_error(
        "failed to allocate %zu bytes for indirection buffer",
        indirection_buffer_size);
    pytorch_qnnp_delete_operator(deconvolution);
    return pytorch_qnnp_status_out_of_memory;
  }
  deconvolution->indirection_buffer = indirection_buffer;

  pytorch_qnnp_indirection_init_deconv2d(
      deconvolution, output_tile_size, tiled_output_size);

  // Run the kernel
  const size_t m_stride = round_up(output_size, mr);
  struct q8conv_context q8conv_context = {
      .bs = deconvolution->batch_size,
      .ks = kernel_size,
      .kc = group_input_channels,
      .kc_stride = k_stride * kernel_size,
      .m = output_size,
      .m_stride = m_stride,
      .n = group_output_channels,
      .n_stride = n_stride,
      .indirect_a = (const uint8_t**)deconvolution->indirection_buffer,
      .packed_w = packed_weights,
      .c = output,
      .c_stride = deconvolution->output_pixel_stride,
      .quantization_params = deconvolution->conv_quantization_params,
      .ukernel = pytorch_qnnp_params.q8conv.conv,
  };

  pthreadpool_compute_4d_tiled(
      threadpool,
      (pthreadpool_function_4d_tiled_t)compute_q8conv,
      &q8conv_context,
      deconvolution->groups,
      batch_size,
      output_size,
      group_output_channels,
      1,
      1,
      mr,
      nr);
  return pytorch_qnnp_status_success;
}
}  // namespace qnnpack
