// Copyright (c) 2017-2022, University of Tennessee. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// This program is free software: you can redistribute it and/or modify it under
// the terms of the BSD 3-Clause license. See the accompanying LICENSE file.

#include "slate/Exception.hh"
#include "slate/internal/device.hh"

#include "device_util.hh"

#include <cstdio>

namespace slate {
namespace device {

//------------------------------------------------------------------------------
/// Kernel implementing row and column scaling.
/// Each thread block deals with one tile.
/// Each thread deals with one row.
/// Launched by gescale_row_col().
///
/// @param[in] m
///     Number of rows of each tile. m >= 1.
///
/// @param[in] n
///     Number of columns of each tile. n >= 1.
///
/// @param[in] Rarray
///     Vector of length m containing row scaling factors.
///
/// @param[in] Carray
///     Vector of length n containing column scaling factors.
///
/// @param[in,out] Aarray
///     Array of tiles of dimension gridDim.x,
///     where each Aarray[k] is an m-by-n matrix stored in an lda-by-n array.
///
/// @param[in] lda
///     Leading dimension of each tile in Aarray. lda >= m.
///
template <typename scalar_t, typename scalar_t2>
void gescale_row_col_batch_kernel(
    int64_t m, int64_t n,
    scalar_t2 const* const* Rarray,
    scalar_t2 const* const* Carray,
    scalar_t** Aarray, int64_t lda,
    int64_t batch_count, blas::Queue& queue)
{
    #pragma omp target is_device_ptr(Rarray, Carray, Aarray) device(queue.device())
    #pragma omp teams distribute
    for (int64_t k = 0; k < batch_count; ++k) {
        scalar_t2 const* R = Rarray[ k ];
        scalar_t2 const* C = Carray[ k ];
        scalar_t* tileA    = Aarray[ k ];
        #pragma omp parallel for schedule(static, 1)
        // thread per row
        for (int64_t i = 0; i < m; ++i) {
            scalar_t* rowA = &tileA[ i ];
            scalar_t2 ri = R[ i ];
            for (int64_t j = 0; j < n; ++j)
                rowA[ j*lda ] = rowA[ j*lda ] * (ri * C[ j ]);
        }
    }
}

//------------------------------------------------------------------------------
/// Kernel implementing column scaling.
/// Each thread block deals with one tile.
/// Each thread deals with one row.
/// Launched by gescale_row_col().
///
/// @param[in] m
///     Number of rows of each tile. m >= 1.
///
/// @param[in] n
///     Number of columns of each tile. n >= 1.
///
/// @param[in] Carray
///     Vector of length n containing column scaling factors.
///
/// @param[in,out] Aarray
///     Array of tiles of dimension gridDim.x,
///     where each Aarray[k] is an m-by-n matrix stored in an lda-by-n array.
///
/// @param[in] lda
///     Leading dimension of each tile in Aarray. lda >= m.
///
/// @param[in] batch_count
///     Size of Aarray. batch_count >= 0.
///
/// @param[in] queue
///     BLAS++ queue to execute in.
///
template <typename scalar_t, typename scalar_t2>
void gescale_col_batch_kernel(
    int64_t m, int64_t n,
    scalar_t2 const* const* Carray,
    scalar_t** Aarray, int64_t lda,
    int64_t batch_count, blas::Queue& queue)
{
    #pragma omp target is_device_ptr(Carray, Aarray) device(queue.device())
    #pragma omp teams distribute
    for (int64_t k = 0; k < batch_count; ++k) {
        scalar_t2 const* C = Carray[ k ];
        scalar_t* tileA    = Aarray[ k ];
        #pragma omp parallel for schedule(static, 1)
        // thread per row
        for (int64_t i = 0; i < m; ++i) {
            scalar_t* rowA = &tileA[ i ];
            for (int64_t j = 0; j < n; ++j)
                rowA[ j*lda ] = rowA[ j*lda ] * C[ j ];
        }
    }
}

//------------------------------------------------------------------------------
/// Kernel implementing row scaling.
/// Each thread block deals with one tile.
/// Each thread deals with one row.
/// Launched by gescale_row_col().
///
/// @param[in] m
///     Number of rows of each tile. m >= 1.
///
/// @param[in] n
///     Number of columns of each tile. n >= 1.
///
/// @param[in] Rarray
///     Vector of length m containing row scaling factors.
///
/// @param[in,out] Aarray
///     Array of tiles of dimension gridDim.x,
///     where each Aarray[k] is an m-by-n matrix stored in an lda-by-n array.
///
/// @param[in] lda
///     Leading dimension of each tile in Aarray. lda >= m.
///
/// @param[in] batch_count
///     Size of Aarray. batch_count >= 0.
///
/// @param[in] queue
///     BLAS++ queue to execute in.
///
template <typename scalar_t, typename scalar_t2>
void gescale_row_batch_kernel(
    int64_t m, int64_t n,
    scalar_t2 const* const* Rarray,
    scalar_t** Aarray, int64_t lda,
    int64_t batch_count, blas::Queue& queue)
{
    #pragma omp target is_device_ptr(Rarray, Aarray) device(queue.device())
    #pragma omp teams distribute
    for (int64_t k = 0; k < batch_count; ++k) {
        scalar_t2 const* R = Rarray[ k ];
        scalar_t* tileA    = Aarray[ k ];
        // distribute rows (i) to threads
        #pragma omp parallel for schedule(static, 1)
        for (int64_t i = 0; i < m; ++i) {
            scalar_t* rowA = &tileA[ i ];
            scalar_t2 ri = R[ i ];
            for (int64_t j = 0; j < n; ++j)
                rowA[ j*lda ] = rowA[ j*lda ] * ri;
        }
    }
}

//------------------------------------------------------------------------------
/// Batched routine for row and column scaling.
///
/// @param[in] equed
///     Form of scaling to do.
///     - Equed::Row:  sets $ A = diag(R) A         $
///     - Equed::Col:  sets $ A =         A diag(C) $
///     - Equed::Both: sets $ A = diag(R) A diag(C) $
///     for each R in Rarray, C in Carray, and A in Aarray.
///
/// @param[in] m
///     Number of rows of each tile. m >= 0.
///
/// @param[in] n
///     Number of columns of each tile. n >= 0.
///
/// @param[in] Rarray
///     Vector of length m containing row scaling factors.
///
/// @param[in] Carray
///     Vector of length n containing column scaling factors.
///
/// @param[in,out] Aarray
///     Array in GPU memory of dimension batch_count, containing pointers to tiles,
///     where each Aarray[k] is an m-by-n matrix stored in an lda-by-n array in GPU memory.
///
/// @param[in] lda
///     Leading dimension of each tile in A. lda >= m.
///
/// @param[in] batch_count
///     Size of Aarray. batch_count >= 0.
///
/// @param[in] queue
///     BLAS++ queue to execute in.
///
template <typename scalar_t, typename scalar_t2>
void gescale_row_col_batch(
    Equed equed, int64_t m, int64_t n,
    scalar_t2 const* const* Rarray,
    scalar_t2 const* const* Carray,
    scalar_t** Aarray, int64_t lda,
    int64_t batch_count, blas::Queue& queue)
{
    // quick return
    if (batch_count == 0)
        return;

    // Use omp target offload
    if (equed == Equed::Row) {
        gescale_row_batch_kernel(
            m, n, Rarray, Aarray, lda, batch_count, queue );
    }
    else if (equed == Equed::Col) {
        gescale_col_batch_kernel(
            m, n, Carray, Aarray, lda, batch_count, queue );
    }
    else if (equed == Equed::Both) {
        gescale_row_col_batch_kernel(
            m, n, Rarray, Carray, Aarray, lda, batch_count, queue );
    }
}

//------------------------------------------------------------------------------
// Explicit instantiations.
template
void gescale_row_col_batch(
    Equed equed, int64_t m, int64_t n,
    float const* const* Rarray,
    float const* const* Carray,
    float** Aarray, int64_t lda,
    int64_t batch_count, blas::Queue& queue);

template
void gescale_row_col_batch(
    Equed equed, int64_t m, int64_t n,
    double const* const* Rarray,
    double const* const* Carray,
    double** Aarray, int64_t lda,
    int64_t batch_count, blas::Queue& queue);

// real R, C
template
void gescale_row_col_batch(
    Equed equed, int64_t m, int64_t n,
    float const* const* Rarray,
    float const* const* Carray,
    std::complex<float>** Aarray, int64_t lda,
    int64_t batch_count, blas::Queue& queue);

template
void gescale_row_col_batch(
    Equed equed, int64_t m, int64_t n,
    double const* const* Rarray,
    double const* const* Carray,
    std::complex<double>** Aarray, int64_t lda,
    int64_t batch_count, blas::Queue& queue);

// complex R, C
template
void gescale_row_col_batch(
    Equed equed, int64_t m, int64_t n,
    std::complex<float> const* const* Rarray,
    std::complex<float> const* const* Carray,
    std::complex<float>** Aarray, int64_t lda,
    int64_t batch_count, blas::Queue& queue);

template
void gescale_row_col_batch(
    Equed equed, int64_t m, int64_t n,
    std::complex<double> const* const* Rarray,
    std::complex<double> const* const* Carray,
    std::complex<double>** Aarray, int64_t lda,
    int64_t batch_count, blas::Queue& queue);

} // namespace device
} // namespace slate
