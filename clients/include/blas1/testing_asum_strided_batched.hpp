/* ************************************************************************
 * Copyright (C) 2018-2024 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell cop-
 * ies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IM-
 * PLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNE-
 * CTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * ************************************************************************ */

#pragma once

#include "bytes.hpp"
#include "cblas_interface.hpp"
#include "flops.hpp"
#include "near.hpp"
#include "rocblas.hpp"
#include "rocblas_init.hpp"
#include "rocblas_math.hpp"
#include "rocblas_random.hpp"
#include "rocblas_test.hpp"
#include "rocblas_vector.hpp"
#include "unit.hpp"
#include "utility.hpp"

template <typename T>
void testing_asum_strided_batched_bad_arg(const Arguments& arg)
{
    auto rocblas_asum_strided_batched_fn    = arg.api == FORTRAN
                                                  ? rocblas_asum_strided_batched<T, true>
                                                  : rocblas_asum_strided_batched<T, false>;
    auto rocblas_asum_strided_batched_fn_64 = arg.api == FORTRAN_64
                                                  ? rocblas_asum_strided_batched_64<T, true>
                                                  : rocblas_asum_strided_batched_64<T, false>;

    for(auto pointer_mode : {rocblas_pointer_mode_host, rocblas_pointer_mode_device})
    {
        rocblas_local_handle handle{arg};
        CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, pointer_mode));

        int64_t        N           = 100;
        int64_t        incx        = 1;
        rocblas_stride stridex     = N;
        int64_t        batch_count = 2;
        real_t<T>      h_rocblas_result[1];

        // Allocate device memory
        device_strided_batch_vector<T> dx(N, incx, stridex, batch_count);

        // Check device memory allocation
        CHECK_DEVICE_ALLOCATION(dx.memcheck());

        DAPI_EXPECT(rocblas_status_invalid_handle,
                    rocblas_asum_strided_batched_fn,
                    (nullptr, N, dx, incx, stridex, batch_count, h_rocblas_result));

        DAPI_EXPECT(rocblas_status_invalid_pointer,
                    rocblas_asum_strided_batched_fn,
                    (handle, N, nullptr, incx, stridex, batch_count, h_rocblas_result));
        DAPI_EXPECT(rocblas_status_invalid_pointer,
                    rocblas_asum_strided_batched_fn,
                    (handle, N, dx, incx, stridex, batch_count, nullptr));
    }
};

template <typename T>
void testing_asum_strided_batched(const Arguments& arg)
{
    auto rocblas_asum_strided_batched_fn    = arg.api == FORTRAN
                                                  ? rocblas_asum_strided_batched<T, true>
                                                  : rocblas_asum_strided_batched<T, false>;
    auto rocblas_asum_strided_batched_fn_64 = arg.api == FORTRAN_64
                                                  ? rocblas_asum_strided_batched_64<T, true>
                                                  : rocblas_asum_strided_batched_64<T, false>;

    int64_t        N           = arg.N;
    int64_t        incx        = arg.incx;
    rocblas_stride stridex     = arg.stride_x;
    int64_t        batch_count = arg.batch_count;

    double rocblas_error_1;
    double rocblas_error_2;

    rocblas_local_handle handle{arg};

    // check to prevent undefined memory allocation error
    if(N <= 0 || incx <= 0 || batch_count <= 0)
    {
        host_vector<real_t<T>> hr_1(std::max(int64_t(1), std::abs(batch_count)));
        host_vector<real_t<T>> hr_2(std::max(int64_t(1), std::abs(batch_count)));
        host_vector<real_t<T>> result_0(std::max(int64_t(1), std::abs(batch_count)));
        CHECK_HIP_ERROR(hr_1.memcheck());
        CHECK_HIP_ERROR(hr_2.memcheck());
        CHECK_HIP_ERROR(result_0.memcheck());

        device_vector<real_t<T>> dr(std::max(int64_t(1), std::abs(batch_count)));
        CHECK_DEVICE_ALLOCATION(dr.memcheck());

        CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_host));
        DAPI_CHECK(rocblas_asum_strided_batched_fn,
                   (handle, N, nullptr, incx, stridex, batch_count, hr_1));

        CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_device));
        DAPI_CHECK(rocblas_asum_strided_batched_fn,
                   (handle, N, nullptr, incx, stridex, batch_count, dr));

        CHECK_HIP_ERROR(hr_2.transfer_from(dr));

        if(batch_count > 0)
        {
            unit_check_general<real_t<T>, real_t<T>>(1, batch_count, 1, result_0, hr_1);
            unit_check_general<real_t<T>, real_t<T>>(1, batch_count, 1, result_0, hr_2);
        }

        return;
    }

    // Naming: `h` is in CPU (host) memory(eg hx), `d` is in GPU (device) memory (eg dx).
    // Allocate host memory
    host_strided_batch_vector<T> hx(N, incx, stridex, batch_count);
    host_vector<real_t<T>>       hr_1(batch_count);
    host_vector<real_t<T>>       hr_2(batch_count);
    host_vector<real_t<T>>       hr_gold(batch_count);

    // Check host memory allocation
    CHECK_HIP_ERROR(hx.memcheck());

    // Allocate device memory
    device_strided_batch_vector<T> dx(N, incx, stridex, batch_count);
    device_vector<real_t<T>>       dr(batch_count);

    // Check device memory allocation
    CHECK_DEVICE_ALLOCATION(dx.memcheck());
    CHECK_DEVICE_ALLOCATION(dr.memcheck());

    // Initialize memory on host.
    rocblas_init_vector(hx, arg, rocblas_client_alpha_sets_nan, true);

    // copy data from CPU to device, does not work for incx != 1
    CHECK_HIP_ERROR(dx.transfer_from(hx));

    double cpu_time_used;

    if(arg.unit_check || arg.norm_check)
    {
        if(arg.pointer_mode_host)
        {
            CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_host));
            DAPI_CHECK(rocblas_asum_strided_batched_fn,
                       (handle, N, dx, incx, stridex, batch_count, hr_1));
        }

        if(arg.pointer_mode_device)
        {
            CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_device));
            handle.pre_test(arg);
            DAPI_CHECK(rocblas_asum_strided_batched_fn,
                       (handle, N, dx, incx, stridex, batch_count, dr));
            handle.post_test(arg);

            if(arg.repeatability_check)
            {
                host_vector<real_t<T>> hr_copy(batch_count);
                CHECK_HIP_ERROR(hr_2.transfer_from(dr));
                for(int i = 0; i < arg.iters; i++)
                {
                    DAPI_CHECK(rocblas_asum_strided_batched_fn,
                               (handle, N, dx, incx, stridex, batch_count, dr));
                    CHECK_HIP_ERROR(hr_copy.transfer_from(dr));
                    unit_check_general<real_t<T>, real_t<T>>(batch_count, 1, 1, hr_2, hr_copy);
                }
                return;
            }
        }

        // CPU BLAS
        cpu_time_used = get_time_us_no_sync();
        for(size_t b = 0; b < batch_count; b++)
        {
            ref_asum<T>(N, hx[b], incx, hr_gold + b);
        }
        cpu_time_used = get_time_us_no_sync() - cpu_time_used;

        real_t<T> abs_error = std::numeric_limits<real_t<T>>::epsilon() * hr_gold[0];
        real_t<T> tolerance = 20.0;
        abs_error *= tolerance;

        // Near check for asum ILP64 bit
        bool near_check = arg.initialization == rocblas_initialization::hpl;

        if(arg.pointer_mode_host)
        {
            if(arg.unit_check)
            {
                if(near_check)
                    near_check_general<real_t<T>, real_t<T>>(
                        batch_count, 1, 1, hr_gold, hr_1, abs_error);
                else
                    unit_check_general<real_t<T>, real_t<T>>(batch_count, 1, 1, hr_gold, hr_1);
            }

            if(arg.norm_check)
            {
                rocblas_error_1 = std::abs((hr_gold[0] - hr_1[0]) / hr_gold[0]);
            }
        }

        if(arg.pointer_mode_device)
        {
            CHECK_HIP_ERROR(hr_2.transfer_from(dr));

            if(arg.unit_check)
            {
                if(near_check)
                    near_check_general<real_t<T>, real_t<T>>(
                        batch_count, 1, 1, hr_gold, hr_2, abs_error);
                else
                    unit_check_general<real_t<T>, real_t<T>>(batch_count, 1, 1, hr_gold, hr_2);
            }

            if(arg.norm_check)
            {
                rocblas_error_2 = std::abs((hr_gold[0] - hr_2[0]) / hr_gold[0]);
            }
        }
    }

    if(arg.timing)
    {
        double gpu_time_used;
        int    number_cold_calls = arg.cold_iters;
        int    total_calls       = number_cold_calls + arg.iters;

        CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_device));
        hipStream_t stream;
        CHECK_ROCBLAS_ERROR(rocblas_get_stream(handle, &stream));

        for(int iter = 0; iter < total_calls; iter++)
        {
            if(iter == number_cold_calls)
                gpu_time_used = get_time_us_sync(stream);

            DAPI_DISPATCH(rocblas_asum_strided_batched_fn,
                          (handle, N, dx, incx, stridex, batch_count, dr));
        }

        gpu_time_used = get_time_us_sync(stream) - gpu_time_used;

        ArgumentModel<e_N, e_incx, e_stride_x, e_batch_count>{}.log_args<T>(rocblas_cout,
                                                                            arg,
                                                                            gpu_time_used,
                                                                            asum_gflop_count<T>(N),
                                                                            asum_gbyte_count<T>(N),
                                                                            cpu_time_used,
                                                                            rocblas_error_1,
                                                                            rocblas_error_2);
    }
}
