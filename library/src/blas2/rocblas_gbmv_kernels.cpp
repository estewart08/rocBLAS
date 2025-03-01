/* ************************************************************************
 * Copyright (C) 2019-2024 Advanced Micro Devices, Inc. All rights reserved.
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

#include "check_numerics_matrix.hpp"
#include "check_numerics_vector.hpp"
#include "rocblas_gbmv.hpp"

/**
  *  Helper for the non-transpose case. Iterates through each diagonal
  *  and creates partial sums for each ty.
  */
template <int DIM_Y, typename T>
__device__ T rocblas_gbmvn_kernel_helper(rocblas_int ty,
                                         rocblas_int ind,
                                         rocblas_int m,
                                         rocblas_int n,
                                         rocblas_int kl,
                                         rocblas_int ku,
                                         const T*    A,
                                         int64_t     lda,
                                         const T*    x,
                                         int64_t     incx)
{
    T           res_A = 0.0;
    rocblas_int col;

    // Since the column is consistent, we can iterate up the diagonal
    // ty defines the column of banded & regular matrix
    for(col = ty; col < n; col += DIM_Y)
    {
        // We have to convert ind to banded matrix row
        rocblas_int row = ind + (ku - col);

        if(ind < m)
        {
            if(row <= kl + ku && row >= 0)
            {
                if(row <= ku && col >= (ku - row) && col < (ku - row + m))
                {
                    res_A += (A[row + col * size_t(lda)] * x[col * int64_t(incx)]);
                }
                if(row > ku && col < m - (row - ku))
                {
                    res_A += (A[row + col * size_t(lda)] * x[col * int64_t(incx)]);
                }
            }
        }
    }
    return res_A;
}

/**
  *  Helper for the (conjugate-)transpose case. Iterates through each diagonal
  *  and creates partial sums for each ty.
  *  The conjugate basically switches A from upper -> lower or lower -> upper
  *  triangular matrix. Since A is compressed, the indexing changes, and we
  *  basically just iterate down columns.
  */
template <int DIM_Y, typename T>
__device__ T rocblas_gbmvt_kernel_helper(bool        is_conj,
                                         rocblas_int ty,
                                         rocblas_int ind,
                                         rocblas_int m,
                                         rocblas_int n,
                                         rocblas_int kl,
                                         rocblas_int ku,
                                         const T*    A,
                                         int64_t     lda,
                                         const T*    x,
                                         int64_t     incx)
{
    T           res_A = 0.0;
    rocblas_int row;

    // for transpose case, ty defines the row
    for(row = ty; row < lda; row += DIM_Y)
    {
        // We have to convert ind to banded matrix row
        rocblas_int col = ind;

        if(col < n)
        {
            if(row <= kl + ku && row >= 0)
            {
                if(row <= ku && col >= (ku - row) && col < (ku - row + m))
                {
                    res_A += ((is_conj ? conj(A[row + col * size_t(lda)])
                                       : A[row + col * size_t(lda)])
                              * x[(row - ku + col) * int64_t(incx)]);
                }
                else if((row > ku && row <= kl + ku) && col < m - (row - ku))
                {
                    res_A += ((is_conj ? conj(A[row + col * size_t(lda)])
                                       : A[row + col * size_t(lda)])
                              * x[(row - ku + col) * int64_t(incx)]);
                }
            }
        }
    }
    return res_A;
}

/**
  *  A combined kernel to handle all gbmv cases (transpose, conjugate, normal).
  */
template <int DIM_X, int DIM_Y, typename T>
__device__ void rocblas_gbmvx_kernel_calc(rocblas_operation transA,
                                          rocblas_int       m,
                                          rocblas_int       n,
                                          rocblas_int       kl,
                                          rocblas_int       ku,
                                          T                 alpha,
                                          const T*          A,
                                          int64_t           lda,
                                          const T*          x,
                                          int64_t           incx,
                                          T                 beta,
                                          T*                y,
                                          int64_t           incy)
{
    rocblas_int thread_id = threadIdx.x + threadIdx.y * DIM_X;

    // threads are all configurated locally
    // Create "tilted" blocks. With the compaction, each diagonal,
    // (from top right to bottom left) is like a row in a normal
    // matrix, so the blocks are "tilted" to the right.
    rocblas_int tx = threadIdx.x;
    rocblas_int ty = threadIdx.y;

    rocblas_int ind = blockIdx.x * DIM_X + tx;

    __shared__ T sdata[DIM_X * DIM_Y];

    T res_A;

    if(alpha)
    {
        // Indexing is different for transpose/non-transpose case. To keep it clean
        // it's separated in two helper functions. They could potentially be combined
        // if more elegant logic is used.
        if(transA == rocblas_operation_none)
        {
            res_A = rocblas_gbmvn_kernel_helper<DIM_Y>(ty, ind, m, n, kl, ku, A, lda, x, incx);
        }
        else
        {
            bool is_conj = transA == rocblas_operation_conjugate_transpose;
            res_A        = rocblas_gbmvt_kernel_helper<DIM_Y>(
                is_conj, ty, ind, m, n, kl, ku, A, lda, x, incx);
        }
        // Store partial sums for the diagonal
        sdata[tx + ty * DIM_X] = res_A;
        __syncthreads();
    }

    thread_id           = threadIdx.x + threadIdx.y * blockDim.x;
    ind                 = blockIdx.x * DIM_X + thread_id;
    rocblas_int max_ind = transA == rocblas_operation_none ? m : n;
    if(thread_id < DIM_X && ind < max_ind)
    {
        // Add the partial sums of each diagonal and store
        if(alpha)
            for(rocblas_int i = 1; i < DIM_Y; i++)
                sdata[thread_id] += sdata[thread_id + DIM_X * i];

        // Update y.
        if(beta != 0)
            y[ind * int64_t(incy)] = alpha
                                         ? alpha * sdata[thread_id] + beta * y[ind * int64_t(incy)]
                                         : beta * y[ind * int64_t(incy)];
        else
            y[ind * int64_t(incy)] = alpha ? alpha * sdata[thread_id] : 0;
    }
}

/**
  *  Loads pointers (in case of future batched versions) and launches
  *  the actual calculation kernel.
  *
  *  Summary of banded matrices:
  *  Banded matrices consist of the centre diagonal, along with 'kl' sub-diagonals and 'ku' super-diagonals.
  *
  *  These matrices are then compacted into a banded storage format. The main diagonal resides on the (ku+1)'th row,
  *  the the first super-diagonal on the RHS of the ku'th row, the first sub-diagonal on the LHS of the (ku+2)'th row, etc.
  *
  *  Ex: (m = 5, n = 5; ku = 1, kl = 2)
  *
  *  1 2 0 0 0              0 2 2 2 2
  *  3 1 2 0 0              1 1 1 1 1    <- main diag on (ku+1)'th row = (1+1)'th row = 2nd row
  *  4 3 1 2 0     ---->    3 3 3 3 0
  *  0 4 3 1 2              4 4 4 0 0
  *  0 0 4 3 1              0 0 0 0 0
  *
  *  Note: This definition uses 1-indexing as seen above.
  *
  *  The empty parts of these sparse matrices are not to be touched. As can be seen, the column
  *  of each element is preserved in the compaction, and the diagonals are "pushed" upwards and
  *  reside on the same row as the other elements of the same diagonal.
  */
template <int DIM_X, int DIM_Y, typename U, typename V, typename W>
ROCBLAS_KERNEL(DIM_X* DIM_Y)
rocblas_gbmvx_kernel(rocblas_operation transA,
                     rocblas_int       m,
                     rocblas_int       n,
                     rocblas_int       kl,
                     rocblas_int       ku,
                     U                 alphaa,
                     V                 Aa,
                     rocblas_stride    shifta,
                     int64_t           lda,
                     rocblas_stride    strideA,
                     V                 xa,
                     rocblas_stride    shiftx,
                     int64_t           incx,
                     rocblas_stride    stridex,
                     U                 betaa,
                     W                 ya,
                     rocblas_stride    shifty,
                     int64_t           incy,
                     rocblas_stride    stridey)
{
    rocblas_int num_threads = blockDim.x * blockDim.y * blockDim.z;
    if(DIM_X * DIM_Y != num_threads)
        return; // need to launch exactly the same number of threads as template parameters indicate

    auto alpha = load_scalar(alphaa, blockIdx.y, 0);
    auto beta  = load_scalar(betaa, blockIdx.y, 0);

    if(!alpha && beta == 1)
        return;

    const auto* A = cond_load_ptr_batch(alpha, Aa, blockIdx.y, shifta, strideA);
    const auto* x = cond_load_ptr_batch(alpha, xa, blockIdx.y, shiftx, stridex);

    auto* y = load_ptr_batch(ya, blockIdx.y, shifty, stridey);

    rocblas_gbmvx_kernel_calc<DIM_X, DIM_Y>(
        transA, m, n, kl, ku, alpha, A, lda, x, incx, beta, y, incy);
}

/**
  *  Here, U is either a `const T* const*` or a `const T*`
  *  V is either a `T*` or a `T* const*`
  */
template <typename T, typename U, typename V>
rocblas_status rocblas_internal_gbmv_launcher(rocblas_handle    handle,
                                              rocblas_operation transA,
                                              rocblas_int       m,
                                              rocblas_int       n,
                                              rocblas_int       kl,
                                              rocblas_int       ku,
                                              const T*          alpha,
                                              U                 A,
                                              rocblas_stride    offseta,
                                              int64_t           lda,
                                              rocblas_stride    strideA,
                                              U                 x,
                                              rocblas_stride    offsetx,
                                              int64_t           incx,
                                              rocblas_stride    stridex,
                                              const T*          beta,
                                              V                 y,
                                              rocblas_stride    offsety,
                                              int64_t           incy,
                                              rocblas_stride    stridey,
                                              rocblas_int       batch_count)
{
    // quick return
    if(!m || !n || !batch_count)
        return rocblas_status_success;

    // in case of negative inc shift pointer to end of data for negative indexing tid*inc
    auto shiftx
        = incx < 0 ? offsetx - ptrdiff_t(incx) * (transA == rocblas_operation_none ? n - 1 : m - 1)
                   : offsetx;
    auto shifty
        = incy < 0 ? offsety - ptrdiff_t(incy) * (transA == rocblas_operation_none ? m - 1 : n - 1)
                   : offsety;

    // (gemv) GBMVX_DIM_Y must be at least 4, 8 * 8 is very slow only 40Gflop/s
    rocblas_int          block_dim   = transA == rocblas_operation_none ? m : n;
    static constexpr int GBMVX_DIM_X = 64;
    static constexpr int GBMVX_DIM_Y = 16;
    rocblas_int          blocks      = (block_dim - 1) / (GBMVX_DIM_X) + 1;
    dim3                 gbmvx_grid(blocks, batch_count);
    dim3                 gbmvx_threads(GBMVX_DIM_X, GBMVX_DIM_Y);

    // Launch a modified gemv kernel. The logic is similar to gemv just with modified
    // indices for the banded matrices.
    if(handle->pointer_mode == rocblas_pointer_mode_device)
    {
        ROCBLAS_LAUNCH_KERNEL((rocblas_gbmvx_kernel<GBMVX_DIM_X, GBMVX_DIM_Y>),
                              gbmvx_grid,
                              gbmvx_threads,
                              0,
                              handle->get_stream(),
                              transA,
                              m,
                              n,
                              kl,
                              ku,
                              alpha,
                              A,
                              offseta,
                              lda,
                              strideA,
                              x,
                              shiftx,
                              incx,
                              stridex,
                              beta,
                              y,
                              shifty,
                              incy,
                              stridey);
    }
    else
    {
        if(!*alpha && *beta == 1)
            return rocblas_status_success;

        ROCBLAS_LAUNCH_KERNEL((rocblas_gbmvx_kernel<GBMVX_DIM_X, GBMVX_DIM_Y>),
                              gbmvx_grid,
                              gbmvx_threads,
                              0,
                              handle->get_stream(),
                              transA,
                              m,
                              n,
                              kl,
                              ku,
                              *alpha,
                              A,
                              offseta,
                              lda,
                              strideA,
                              x,
                              shiftx,
                              incx,
                              stridex,
                              *beta,
                              y,
                              shifty,
                              incy,
                              stridey);
    }

    return rocblas_status_success;
}

//TODO :-Add rocblas_check_numerics_gb_matrix_template for checking Matrix `A` which is a General Band matrix
template <typename T, typename U>
rocblas_status rocblas_gbmv_check_numerics(const char*       function_name,
                                           rocblas_handle    handle,
                                           rocblas_operation trans_a,
                                           int64_t           m,
                                           int64_t           n,
                                           T                 A,
                                           rocblas_stride    offset_a,
                                           int64_t           lda,
                                           rocblas_stride    stride_a,
                                           T                 x,
                                           rocblas_stride    offset_x,
                                           int64_t           inc_x,
                                           rocblas_stride    stride_x,
                                           U                 y,
                                           rocblas_stride    offset_y,
                                           int64_t           inc_y,
                                           rocblas_stride    stride_y,
                                           int64_t           batch_count,
                                           const int         check_numerics,
                                           bool              is_input)
{
    if(is_input)
    {
        //TODO :-Add rocblas_check_numerics_gb_matrix_template for checking Matrix `A` which is a General Band matrix

        //Checking trans_a to transpose a vector 'x'
        int64_t        n_x = trans_a == rocblas_operation_none ? n : m;
        rocblas_status check_numerics_status
            = rocblas_internal_check_numerics_vector_template(function_name,
                                                              handle,
                                                              n_x,
                                                              x,
                                                              offset_x,
                                                              inc_x,
                                                              stride_x,
                                                              batch_count,
                                                              check_numerics,
                                                              is_input);
        return check_numerics_status;
    }
    else
    {
        //Checking trans_a to transpose a vector 'y'
        int64_t        n_y = trans_a == rocblas_operation_none ? m : n;
        rocblas_status check_numerics_status
            = rocblas_internal_check_numerics_vector_template(function_name,
                                                              handle,
                                                              n_y,
                                                              y,
                                                              offset_y,
                                                              inc_y,
                                                              stride_y,
                                                              batch_count,
                                                              check_numerics,
                                                              is_input);
        return check_numerics_status;
    }
}

// Instantiations below will need to be manually updated to match any change in
// template parameters in the files *gbmv*.cpp

#ifdef INST_GBMV_LAUNCHER
#error INST_GBMV_LAUNCHER  already defined
#endif

#define INST_GBMV_LAUNCHER(T_, U_, V_)                                                            \
    template rocblas_status rocblas_internal_gbmv_launcher<T_, U_, V_>(rocblas_handle    handle,  \
                                                                       rocblas_operation transA,  \
                                                                       rocblas_int       m,       \
                                                                       rocblas_int       n,       \
                                                                       rocblas_int       kl,      \
                                                                       rocblas_int       ku,      \
                                                                       const T_*         alpha,   \
                                                                       U_                A,       \
                                                                       rocblas_stride    offseta, \
                                                                       int64_t           lda,     \
                                                                       rocblas_stride    strideA, \
                                                                       U_                x,       \
                                                                       rocblas_stride    offsetx, \
                                                                       int64_t           incx,    \
                                                                       rocblas_stride    stridex, \
                                                                       const T_*         beta,    \
                                                                       V_                y,       \
                                                                       rocblas_stride    offsety, \
                                                                       int64_t           incy,    \
                                                                       rocblas_stride    stridey, \
                                                                       rocblas_int batch_count);

INST_GBMV_LAUNCHER(double, double const* const*, double* const*)
INST_GBMV_LAUNCHER(rocblas_float_complex,
                   rocblas_float_complex const* const*,
                   rocblas_float_complex* const*)
INST_GBMV_LAUNCHER(rocblas_double_complex,
                   rocblas_double_complex const* const*,
                   rocblas_double_complex* const*)
INST_GBMV_LAUNCHER(float, float const*, float*)
INST_GBMV_LAUNCHER(double, double const*, double*)
INST_GBMV_LAUNCHER(rocblas_float_complex, rocblas_float_complex const*, rocblas_float_complex*)
INST_GBMV_LAUNCHER(rocblas_double_complex, rocblas_double_complex const*, rocblas_double_complex*)
INST_GBMV_LAUNCHER(float, float const* const*, float* const*)

#undef INST_GBMV_LAUNCHER

#ifdef INST_GBMV_NUMERICS
#error INST_GBMV_NUMERICS  already defined
#endif

#define INST_GBMV_NUMERICS(T_, U_)                                                                \
    template rocblas_status rocblas_gbmv_check_numerics<T_, U_>(const char*       function_name,  \
                                                                rocblas_handle    handle,         \
                                                                rocblas_operation trans_a,        \
                                                                int64_t           m,              \
                                                                int64_t           n,              \
                                                                T_                A,              \
                                                                rocblas_stride    offset_a,       \
                                                                int64_t           lda,            \
                                                                rocblas_stride    stride_a,       \
                                                                T_                x,              \
                                                                rocblas_stride    offset_x,       \
                                                                int64_t           inc_x,          \
                                                                rocblas_stride    stride_x,       \
                                                                U_                y,              \
                                                                rocblas_stride    offset_y,       \
                                                                int64_t           inc_y,          \
                                                                rocblas_stride    stride_y,       \
                                                                int64_t           batch_count,    \
                                                                const int         check_numerics, \
                                                                bool              is_input);

INST_GBMV_NUMERICS(float const*, float*)
INST_GBMV_NUMERICS(double const*, double*)
INST_GBMV_NUMERICS(rocblas_float_complex const*, rocblas_float_complex*)
INST_GBMV_NUMERICS(rocblas_double_complex const*, rocblas_double_complex*)
INST_GBMV_NUMERICS(float const* const*, float* const*)
INST_GBMV_NUMERICS(double const* const*, double* const*)
INST_GBMV_NUMERICS(rocblas_float_complex const* const*, rocblas_float_complex* const*)
INST_GBMV_NUMERICS(rocblas_double_complex const* const*, rocblas_double_complex* const*)

#undef INST_GBMV_NUMERICS
