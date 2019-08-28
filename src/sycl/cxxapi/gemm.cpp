/*******************************************************************************
* Copyright 2019 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <memory>
#include <CL/sycl.hpp>

#include "dnnl.hpp"

#include "common/dnnl_traits.hpp"
#include "common/gemm_utils.hpp"
#include "common/primitive.hpp"
#include "common/primitive_desc.hpp"
#include "ocl/jit_gen9_gemm.hpp"
#include "sycl/sycl_engine.hpp"
#include "sycl/sycl_memory_storage.hpp"
#include "sycl/sycl_stream.hpp"

using namespace dnnl;
using namespace dnnl::impl;
using namespace dnnl::impl::sycl;

namespace {

template <memory_api_kind_t api_kind>
struct create_memory_t {};

template <>
struct create_memory_t<memory_api_kind_t::buffer> {
    template <typename T>
    static std::unique_ptr<memory_t> call(engine_t *eng,
            dnnl_memory_desc_t *mem_desc, dim_t offset, const void *handle) {
        auto *buf = static_cast<cl::sycl::buffer<T, 1> *>(
                const_cast<void *>(handle));
        auto buf_u8 = buf->template reinterpret<uint8_t>(
                cl::sycl::range<1>(buf->get_size()));
        std::unique_ptr<memory_t> mem(new memory_t(eng, mem_desc,
                memory_flags_t::use_backend_ptr, DNNL_MEMORY_NONE));
        mem->set_data_handle(&buf_u8);
        mem->memory_storage()->set_offset(offset * sizeof(T));
        return mem;
    }
};

template <>
struct create_memory_t<memory_api_kind_t::usm> {
    template <typename T>
    static std::unique_ptr<memory_t> call(engine_t *eng,
            dnnl_memory_desc_t *mem_desc, dim_t offset, const void *handle) {
        std::unique_ptr<memory_t> mem(new memory_t(eng, mem_desc,
                memory_flags_t::use_backend_ptr, const_cast<void *>(handle)));
        mem->memory_storage()->set_offset(offset * sizeof(T));
        return mem;
    }
};

template <memory_api_kind_t memory_api_kind, data_type_t data_type, typename T>
void gemm_generic(cl::sycl::queue &queue, const char *transa,
        const char *transb, dim_t m, dim_t n, dim_t k, float alpha,
        const void *a, dim_t offset_a, dim_t lda, const void *b, dim_t offset_b,
        dim_t ldb, float beta, void *c, dim_t offset_c, dim_t ldc) {

#ifndef DNNL_SYCL_INTEL
    if (memory_api_kind == memory_api_kind_t::usm)
        error::wrap_c_api(
                status::runtime_error, "USM interface is not supported.");
#endif

    using data_t = typename prec_traits<data_type>::type;
    static_assert(sizeof(data_t) == sizeof(T), "not expected");

    status_t status;

    // Check inputs
    status = check_gemm_input(
            *transa, *transb, m, n, k, lda, ldb, ldc, alpha, beta);
    error::wrap_c_api(status, "invalid arguments");

    // Create engine
    cl::sycl::device dev = queue.get_device();
    cl::sycl::context ctx = queue.get_context();
    engine_kind_t eng_kind;
    if (dev.is_cpu() || dev.is_host()) {
        eng_kind = engine_kind::cpu;
        error::wrap_c_api(
                status::unimplemented, "SYCL CPU GEMM not implemented");
    } else {
        assert(dev.is_gpu());
        eng_kind = engine_kind::gpu;
    }

    std::unique_ptr<sycl_engine_base_t> engine;
    engine_t *engine_ptr;
    status = get_engine_factory(eng_kind)->engine_create(&engine_ptr, dev, ctx);
    error::wrap_c_api(status, "invalid queue");

    engine.reset(utils::downcast<sycl_engine_base_t *>(engine_ptr));

    // Create stream
    std::unique_ptr<stream_t> s;
    stream_t *s_ptr;
    status = engine->create_stream(&s_ptr, queue);
    error::wrap_c_api(status, "invalid queue");
    s.reset(s_ptr);

    // Create primitive descriptor
    using pd_type = typename ocl::jit_gen9_gemm_t<data_type>::pd_t;

    gemm_desc_t op_desc;
    op_desc.primitive_kind = dnnl_gemm;
    op_desc.transa = (*transa == 'n' || *transa == 'N') ? transpose::notrans
                                                        : transpose::trans;
    op_desc.transb = (*transb == 'n' || *transb == 'N') ? transpose::notrans
                                                        : transpose::trans;
    op_desc.m = m;
    op_desc.n = n;
    op_desc.k = k;
    op_desc.lda = lda;
    op_desc.ldb = ldb;
    op_desc.ldc = ldc;
    op_desc.alpha = alpha;
    op_desc.beta = beta;
    op_desc.a_type = data_type;
    op_desc.b_type = data_type;
    op_desc.c_type = data_type;

    dnnl_memory_desc_t a_desc, b_desc, c_desc;

    status = create_gemm_memory_desc(&a_desc, &op_desc, 0, data_type);
    assert(status == status::success);
    status = create_gemm_memory_desc(&b_desc, &op_desc, 1, data_type);
    assert(status == status::success);
    status = create_gemm_memory_desc(&c_desc, &op_desc, 2, data_type);
    assert(status == status::success);

    std::unique_ptr<primitive_desc_t> pd;
    primitive_attr_t attr;
    primitive_desc_t *pd_ptr;
    status = primitive_desc_t::create<pd_type>(&pd_ptr,
            reinterpret_cast<const op_desc_t *>(&op_desc), &attr, engine.get(),
            nullptr);
    error::wrap_c_api(status, "invalid arguments");
    pd.reset(pd_ptr);

    // Create memory objects
    auto a_mem = create_memory_t<memory_api_kind>::template call<T>(
            engine.get(), &a_desc, offset_a, a);
    auto b_mem = create_memory_t<memory_api_kind>::template call<T>(
            engine.get(), &b_desc, offset_b, b);
    auto c_mem = create_memory_t<memory_api_kind>::template call<T>(
            engine.get(), &c_desc, offset_c, c);

    // Create primitive
    primitive_t *gemm_prim;
    status = pd->create_primitive(&gemm_prim);
    error::wrap_c_api(status, "could not create a primitive");

    exec_args_t args = {
            {DNNL_ARG_SRC_0, {a_mem.get(), true}},
            {DNNL_ARG_SRC_1, {b_mem.get(), true}},
            {DNNL_ARG_DST, {c_mem.get(), false}},
    };

    exec_ctx_t exec_ctx(s.get(), std::move(args));
    status = gemm_prim->execute(exec_ctx);
    gemm_prim->release();
    error::wrap_c_api(status, "could not execute a primitive");

    error::wrap_c_api(s->wait(), "could not wait a stream");
}

} // namespace

namespace dnnl {

// Buffer interfaces
void DNNL_API gemm(cl::sycl::queue &queue, char transa, char transb, dim_t m,
        dim_t n, dim_t k, float alpha, cl::sycl::buffer<float, 1> &a,
        dim_t offset_a, dim_t lda, cl::sycl::buffer<float, 1> &b,
        dim_t offset_b, dim_t ldb, float beta, cl::sycl::buffer<float, 1> &c,
        dim_t offset_c, dim_t ldc) {
    return gemm_generic<memory_api_kind_t::buffer, data_type::f32, float>(queue,
            &transb, &transa, n, m, k, alpha, &b, offset_b, ldb, &a, offset_a,
            lda, beta, &c, offset_c, ldc);
}

void DNNL_API gemm(cl::sycl::queue &queue, char transa, char transb, dim_t m,
        dim_t n, dim_t k, float alpha, cl::sycl::buffer<cl::sycl::half, 1> &a,
        dim_t offset_a, dim_t lda, cl::sycl::buffer<cl::sycl::half, 1> &b,
        dim_t offset_b, dim_t ldb, float beta,
        cl::sycl::buffer<cl::sycl::half, 1> &c, dim_t offset_c, dim_t ldc) {
    return gemm_generic<memory_api_kind_t::buffer, data_type::f16,
            cl::sycl::half>(queue, &transb, &transa, n, m, k, alpha, &b,
            offset_b, ldb, &a, offset_a, lda, beta, &c, offset_c, ldc);
}

// USM interfaces
void DNNL_API gemm(cl::sycl::queue &queue, char transa, char transb, dim_t m,
        dim_t n, dim_t k, float alpha, const float *a, dim_t lda,
        const float *b, dim_t ldb, float beta, float *c, dim_t ldc) {
    return gemm_generic<memory_api_kind_t::usm, data_type::f32, float>(queue,
            &transb, &transa, n, m, k, alpha, b, 0, ldb, a, 0, lda, beta, c, 0,
            ldc);
}

void DNNL_API gemm(cl::sycl::queue &queue, char transa, char transb, dim_t m,
        dim_t n, dim_t k, float alpha, const cl::sycl::half *a, dim_t lda,
        const cl::sycl::half *b, dim_t ldb, float beta, cl::sycl::half *c,
        dim_t ldc) {
    return gemm_generic<memory_api_kind_t::usm, data_type::f16, cl::sycl::half>(
            queue, &transb, &transa, n, m, k, alpha, b, 0, ldb, a, 0, lda, beta,
            c, 0, ldc);
}

} // namespace dnnl
