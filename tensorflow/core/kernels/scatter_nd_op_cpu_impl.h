/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_CORE_KERNELS_SCATTER_ND_OP_CPU_IMPL_H_
#define TENSORFLOW_CORE_KERNELS_SCATTER_ND_OP_CPU_IMPL_H_

// Functor definitions for ScatterND ops, must be compilable by nvcc.

#define EIGEN_USE_THREADS

#include <atomic>

#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"

#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/kernels/bounds_check.h"
#include "tensorflow/core/kernels/fill_functor.h"
#include "tensorflow/core/kernels/scatter_nd_op.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/util/util.h"

namespace tensorflow {

typedef Eigen::ThreadPoolDevice CPUDevice;
#ifdef TENSORFLOW_USE_SYCL
typedef Eigen::SyclDevice SYCLDevice;
#endif  // TENSORFLOW_USE_SYCL

class OpKernelContext;

// Specialization of UpdateExecutor to CPU
namespace update_executor {

template <typename Input, typename Update, typename Output,
          scatter_nd_op::UpdateOp OP>
class UpdateExecutor {
 public:
  EIGEN_STRONG_INLINE static void Execute(Input value, Update update,
                                          Output output);
};

template <typename Input, typename Update, typename Output>
class UpdateExecutor<Input, Update, Output, scatter_nd_op::UpdateOp::ASSIGN> {
 public:
  EIGEN_STRONG_INLINE static void Execute(Input /* input */, Update update,
                                          Output output) {
    output = update;
  }
};

template <typename Input, typename Update, typename Output>
class UpdateExecutor<Input, Update, Output, scatter_nd_op::UpdateOp::ADD> {
 public:
  EIGEN_STRONG_INLINE static void Execute(Input /* input */, Update update,
                                          Output output) {
    output += update;
  }
};

template <typename Input, typename Update, typename Output>
class UpdateExecutor<Input, Update, Output, scatter_nd_op::UpdateOp::SUB> {
 public:
  EIGEN_STRONG_INLINE static void Execute(Input /* input */, Update update,
                                          Output output) {
    output -= update;
  }
};

}  // namespace update_executor

namespace functor {

// Implementation of update functor for CPU.
template <typename T, typename Index, scatter_nd_op::UpdateOp OP, int IXDIM>
struct ScatterNdFunctor<CPUDevice, T, Index, OP, IXDIM> {
  Index operator()(
      const CPUDevice& d, const Index slice_size,
      const Eigen::array<Eigen::DenseIndex, IXDIM> output_shape_prefix,
      typename TTypes<T, 2>::Tensor Tparams,
      typename TTypes<Index, 2>::ConstTensor Tindices,
      typename TTypes<T, 2>::ConstTensor Tupdates,
      typename TTypes<T, 2>::Tensor Toutput) {
    // error_loc is -1 if there's no out-of-bounds index,
    // otherwise it is the location of an OOB index in Tindices.
    Index error_loc = -1;

    const Eigen::DenseIndex batch_size = Tindices.dimension(0);

    Index batch_strides[IXDIM];
    for (int dim = IXDIM - 1; dim >= 0; --dim) {
      if (dim == IXDIM - 1) {
        batch_strides[dim] = 1;
      } else {
        batch_strides[dim] =
            batch_strides[dim + 1] * output_shape_prefix[dim + 1];
      }
    }

    for (Eigen::DenseIndex loc = 0; loc < batch_size; ++loc) {
      Index i = 0;
      bool out_of_bounds = false;
      for (int dim = 0; dim < IXDIM; ++dim) {
        const Index ix_d = internal::SubtleMustCopy(Tindices(loc, dim));
        out_of_bounds |= !FastBoundsCheck(ix_d, output_shape_prefix[dim]);
        i += ix_d * batch_strides[dim];
      }
      if (TF_PREDICT_FALSE(out_of_bounds)) {
        error_loc = loc;
        break;
      } else {
        auto input_chip = Toutput.template chip<0>(i);
        auto output_chip = input_chip.device(d);
        auto update_chip = Tupdates.template chip<0>(loc);
        update_executor::UpdateExecutor<
            decltype(input_chip), decltype(update_chip), decltype(output_chip),
            OP>::Execute(input_chip, update_chip, output_chip);
      }
    }

    return error_loc;
  }
};

#define REGISTER_SCATTER_ND_FULL(T, Index, op)                               \
  template Index                                                             \
  ScatterNdFunctor<CPUDevice, T, Index, op, CPU_PROVIDED_IXDIM>::operator()( \
      const CPUDevice& d, const Index slice_size,                            \
      const Eigen::array<Eigen::DenseIndex, CPU_PROVIDED_IXDIM>              \
          output_shape_prefix,                                               \
      typename TTypes<T, 2>::Tensor Tparams,                                 \
      typename TTypes<Index, 2>::ConstTensor Tindices,                       \
      typename TTypes<T, 2>::ConstTensor Tupdates,                           \
      typename TTypes<T, 2>::Tensor Toutput)

#define REGISTER_SCATTER_ND_INDEX(type, op)  \
  REGISTER_SCATTER_ND_FULL(type, int32, op); \
  REGISTER_SCATTER_ND_FULL(type, int64, op)

#define REGISTER_SCATTER_ND_UPDATE(type) \
  REGISTER_SCATTER_ND_INDEX(type, scatter_nd_op::UpdateOp::ASSIGN);

#define REGISTER_SCATTER_ND_MATH(type)                           \
  REGISTER_SCATTER_ND_INDEX(type, scatter_nd_op::UpdateOp::ADD); \
  REGISTER_SCATTER_ND_INDEX(type, scatter_nd_op::UpdateOp::SUB);

TF_CALL_ALL_TYPES(REGISTER_SCATTER_ND_UPDATE);
REGISTER_SCATTER_ND_INDEX(string, scatter_nd_op::UpdateOp::ADD);
TF_CALL_NUMBER_TYPES(REGISTER_SCATTER_ND_MATH)

#undef REGISTER_SCATTER_ND_MATH
#undef REGISTER_SCATTER_ND_UPDATE
#undef REGISTER_SCATTER_ND_INDEX
#undef REGISTER_SCATTER_ND_FULL

#ifdef TENSORFLOW_USE_SYCL
namespace {
template <typename PTR, typename T, scatter_nd_op::UpdateOp Op>
struct LeftUpdateSYCL {
  EIGEN_STRONG_INLINE EIGEN_DEVICE_FUNC void operator()(PTR out, const T& val);
};

template <typename PTR, typename T>
struct LeftUpdateSYCL<PTR, T, scatter_nd_op::UpdateOp::ASSIGN> {
  EIGEN_STRONG_INLINE EIGEN_DEVICE_FUNC void operator()(PTR out, const T& val) {
    *out = val;
  }
};

template <typename PTR, typename T>
struct LeftUpdateSYCL<PTR, T, scatter_nd_op::UpdateOp::ADD> {
  EIGEN_STRONG_INLINE EIGEN_DEVICE_FUNC void operator()(PTR out, const T& val) {
    *out += val;
  }
};

template <typename PTR, typename T>
struct LeftUpdateSYCL<PTR, T, scatter_nd_op::UpdateOp::SUB> {
  EIGEN_STRONG_INLINE EIGEN_DEVICE_FUNC void operator()(PTR out, const T& val) {
    *out -= val;
  }
};
}  // namespace

template <typename T, typename Index, scatter_nd_op::UpdateOp op, int IXDIM>
struct ScatterNdKernel {
  using write_accessor =
      cl::sycl::accessor<uint8_t, 1, cl::sycl::access::mode::write,
                         cl::sycl::access::target::global_buffer>;
  using read_accessor =
      cl::sycl::accessor<uint8_t, 1, cl::sycl::access::mode::read,
                         cl::sycl::access::target::global_buffer>;

  ScatterNdKernel(
      const read_accessor indices, const read_accessor updates,
      write_accessor out,
      const Eigen::array<Eigen::DenseIndex, IXDIM> output_shape_prefix,
      const Eigen::array<int64, IXDIM> batch_strides, const int64 num_indices,
      const Index slice_size)
      : indices_(indices),
        updates_(updates),
        out_(out),
        output_shape_prefix_(output_shape_prefix),
        batch_strides_(batch_strides),
        num_indices_(num_indices),
        slice_size_(slice_size) {}

  void operator()(cl::sycl::item<1> id) {
    const T* updates = ConvertToActualTypeSycl(T, updates_);
    const Index* indices = ConvertToActualTypeSycl(Index, indices_);
    T* out = ConvertToActualTypeSycl(T, out_);

    auto update = LeftUpdateSYCL<decltype(out), T, op>();

    for (Index index = 0; index < num_indices_; index++) {
      Index i = 0;
      bool out_of_bounds = false;
      for (int dim = 0; dim < IXDIM; ++dim) {
        int offset = (IXDIM * index + dim);
        const Index ix_d = indices[offset];
        out_of_bounds |= !FastBoundsCheck(ix_d, output_shape_prefix_[dim]);
        i += ix_d * batch_strides_[dim] * slice_size_;
      }
      if (!out_of_bounds) {
        for (int si = 0; si < slice_size_; si++) {
          update(out + i + si, updates[index * slice_size_ + si]);
        }
      }
    }
  }

 private:
  const read_accessor indices_;
  const read_accessor updates_;
  write_accessor out_;
  const Eigen::array<Eigen::DenseIndex, IXDIM> output_shape_prefix_;
  const Eigen::array<int64, IXDIM> batch_strides_;
  const int64 num_indices_;
  const Index slice_size_;
};

// Implementation of update functor for SYCL.
template <typename T, typename Index, scatter_nd_op::UpdateOp OP, int IXDIM>
struct ScatterNdFunctor<SYCLDevice, T, Index, OP, IXDIM> {
  Index operator()(
      const SYCLDevice& d, const Index slice_size,
      const Eigen::array<Eigen::DenseIndex, IXDIM> output_shape_prefix,
      typename TTypes<T, 2>::Tensor Tparams,
      typename TTypes<Index, 2>::ConstTensor Tindices,
      typename TTypes<T, 2>::ConstTensor Tupdates,
      typename TTypes<T, 2>::Tensor Toutput) {
    const Eigen::DenseIndex batch_size = Tindices.dimension(0);

    // Index batch_strides[IXDIM];
    Eigen::array<int64, IXDIM> batch_strides;
    for (int dim = IXDIM - 1; dim >= 0; --dim) {
      if (dim == IXDIM - 1) {
        batch_strides[dim] = 1;
      } else {
        batch_strides[dim] =
            batch_strides[dim + 1] * output_shape_prefix[dim + 1];
      }
    }

    const int num_threads = Toutput.size();

    auto indices_buffer = d.get_sycl_buffer(Tindices.data());
    auto updates_buffer = d.get_sycl_buffer(Tupdates.data());
    auto output_buffer = d.get_sycl_buffer(Toutput.data());

    d.sycl_queue().submit([&](cl::sycl::handler& cgh) {
      auto indices_access =
          indices_buffer.template get_access<cl::sycl::access::mode::read>(cgh);
      auto updates_access =
          updates_buffer.template get_access<cl::sycl::access::mode::read>(cgh);

      auto output_access =
          output_buffer.template get_access<cl::sycl::access::mode::write>(cgh);

      ScatterNdKernel<T, Index, OP, IXDIM> kernel(
          indices_access, updates_access, output_access, output_shape_prefix,
          batch_strides, batch_size, slice_size);

      cgh.parallel_for(cl::sycl::range<1>(num_threads), kernel);
    });

    return -1;
  }
};

#define REGISTER_SCATTER_ND_FULL_SYCL(T, Index, op)                           \
  template Index                                                              \
  ScatterNdFunctor<SYCLDevice, T, Index, op, CPU_PROVIDED_IXDIM>::operator()( \
      const SYCLDevice& d, const Index slice_size,                            \
      const Eigen::array<Eigen::DenseIndex, CPU_PROVIDED_IXDIM>               \
          output_shape_prefix,                                                \
      typename TTypes<T, 2>::Tensor Tparams,                                  \
      typename TTypes<Index, 2>::ConstTensor Tindices,                        \
      typename TTypes<T, 2>::ConstTensor Tupdates,                            \
      typename TTypes<T, 2>::Tensor Toutput)

#define REGISTER_SCATTER_ND_INDEX_SYCL(type, op)  \
  REGISTER_SCATTER_ND_FULL_SYCL(type, int32, op); \
  REGISTER_SCATTER_ND_FULL_SYCL(type, int64, op)

#define REGISTER_SCATTER_ND_UPDATE_SYCL(type) \
  REGISTER_SCATTER_ND_INDEX_SYCL(type, scatter_nd_op::UpdateOp::ASSIGN);

#define REGISTER_SCATTER_ND_MATH_SYCL(type)                           \
  REGISTER_SCATTER_ND_INDEX_SYCL(type, scatter_nd_op::UpdateOp::ADD); \
  REGISTER_SCATTER_ND_INDEX_SYCL(type, scatter_nd_op::UpdateOp::SUB);

TF_CALL_SYCL_NUMBER_TYPES(REGISTER_SCATTER_ND_UPDATE_SYCL)
TF_CALL_SYCL_NUMBER_TYPES(REGISTER_SCATTER_ND_MATH_SYCL)

#undef REGISTER_SCATTER_ND_MATH_SYCL
#undef REGISTER_SCATTER_ND_UPDATE_SYCL
#undef REGISTER_SCATTER_ND_INDEX_SYCL
#undef REGISTER_SCATTER_ND_FULL_SYCL

#endif  // TENSORFLOW_USE_SYCL

}  // namespace functor

}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_KERNELS_SCATTER_ND_OP_CPU_IMPL_H_
