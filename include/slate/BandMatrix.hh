//------------------------------------------------------------------------------
// Copyright (c) 2017, University of Tennessee
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the University of Tennessee nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL UNIVERSITY OF TENNESSEE BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//------------------------------------------------------------------------------
// This research was supported by the Exascale Computing Project (17-SC-20-SC),
// a collaborative effort of two U.S. Department of Energy organizations (Office
// of Science and the National Nuclear Security Administration) responsible for
// the planning and preparation of a capable exascale ecosystem, including
// software, applications, hardware, advanced system engineering and early
// testbed platforms, in support of the nation's exascale computing imperative.
//------------------------------------------------------------------------------
// For assistance with SLATE, email <slate-user@icl.utk.edu>.
// You can also join the "SLATE User" Google group by going to
// https://groups.google.com/a/icl.utk.edu/forum/#!forum/slate-user,
// signing in with your Google credentials, and then clicking "Join group".
//------------------------------------------------------------------------------

#ifndef SLATE_BAND_MATRIX_HH
#define SLATE_BAND_MATRIX_HH

#include "slate/Matrix.hh"
#include "slate/Tile.hh"
#include "slate/types.hh"

#include "lapack.hh"

#include <algorithm>
#include <utility>

#include "slate/internal/cuda.hh"
#include "slate/internal/cublas.hh"
#include "slate/internal/mpi.hh"
#include "slate/internal/openmp.hh"

namespace slate {

//==============================================================================
/// General banded, non-symmetric, m-by-n, distributed, tiled matrices.
template <typename scalar_t>
class BandMatrix: public Matrix<scalar_t> {
public:
    // constructors
    BandMatrix();

    BandMatrix(int64_t m, int64_t n, int64_t kl, int64_t ku,
               int64_t nb, int p, int q, MPI_Comm mpi_comm);

    BandMatrix(int64_t kl, int64_t ku, Matrix<scalar_t>& orig);

public:
    template <typename T>
    friend void swap(BandMatrix<T>& A, BandMatrix<T>& B);

    int64_t lowerBandwidth() const;
    void    lowerBandwidth(int64_t kl);

    int64_t upperBandwidth() const;
    void    upperBandwidth(int64_t ku);

    // todo: specialize for band
    // int64_t getMaxHostTiles();
    // int64_t getMaxDeviceTiles(int device);
    // void allocateBatchArrays();
    // void reserveHostWorkspace();
    // void reserveDeviceWorkspace();
    // void gather(scalar_t* A, int64_t lda);

    void tileUpdateAllOrigin();

protected:
    int64_t kl_, ku_;
};

//------------------------------------------------------------------------------
/// Default constructor creates an empty band matrix with bandwidth = 0.
template <typename scalar_t>
BandMatrix<scalar_t>::BandMatrix():
    Matrix<scalar_t>(),
    kl_(0),
    ku_(0)
{}

//------------------------------------------------------------------------------
/// Constructor creates an m-by-n band matrix, with no tiles allocated,
/// with fixed nb-by-nb tile size and 2D block cyclic distribution.
/// Tiles can be added with tileInsert().
///
/// @param[in] m
///     Number of rows of the matrix. m >= 0.
///
/// @param[in] n
///     Number of columns of the matrix. n >= 0.
///
/// @param[in] kl
///     Number of subdiagonals within band. kl >= 0.
///
/// @param[in] ku
///     Number of superdiagonals within band. ku >= 0.
///
/// @param[in] nb
///     Block size in 2D block-cyclic distribution.
///
/// @param[in] p
///     Number of block rows in 2D block-cyclic distribution. p > 0.
///
/// @param[in] q
///     Number of block columns of 2D block-cyclic distribution. q > 0.
///
/// @param[in] mpi_comm
///     MPI communicator to distribute matrix across.
///     p*q == MPI_Comm_size(mpi_comm).
///
template <typename scalar_t>
BandMatrix<scalar_t>::BandMatrix(
    int64_t m, int64_t n, int64_t kl, int64_t ku, int64_t nb,
    int p, int q, MPI_Comm mpi_comm)
    : Matrix<scalar_t>(m, n, nb, p, q, mpi_comm),
      kl_(kl),
      ku_(ku)
{}

//------------------------------------------------------------------------------
/// Conversion from general Matrix.
/// Creates a shallow copy view of the original matrix,
///
/// @param[in] orig
///     Original matrix.
///
/// @param[in] kl
///     Lower bandwidth.
///
/// @param[in] ku
///     Upper bandwidth.
///
template <typename scalar_t>
BandMatrix<scalar_t>::BandMatrix(
    int64_t kl, int64_t ku, Matrix<scalar_t>& orig)
    : Matrix<scalar_t>(orig, 0, orig.mt()-1, 0, orig.nt()-1),
      kl_(kl),
      ku_(ku)
{}

//------------------------------------------------------------------------------
/// Swap contents of band matrices A and B.
template <typename scalar_t>
void swap(BandMatrix<scalar_t>& A, BandMatrix<scalar_t>& B)
{
    using std::swap;
    swap(static_cast< Matrix<scalar_t>& >(A),
         static_cast< Matrix<scalar_t>& >(B));
    swap(A.kl_, B.kl_);
    swap(A.ku_, B.ku_);
}

//------------------------------------------------------------------------------
/// @return number of subdiagonals within band.
template <typename scalar_t>
int64_t BandMatrix<scalar_t>::lowerBandwidth() const
{
    return (this->op() == Op::NoTrans ? kl_ : ku_);
}

//------------------------------------------------------------------------------
/// Sets number of subdiagonals within band.
template <typename scalar_t>
void BandMatrix<scalar_t>::lowerBandwidth(int64_t kl)
{
    if (this->op() == Op::NoTrans)
        this->kl_ = kl;
    else
        this->ku_ = kl;
}

//------------------------------------------------------------------------------
/// @return number of superdiagonals within band.
template <typename scalar_t>
int64_t BandMatrix<scalar_t>::upperBandwidth() const
{
    return (this->op() == Op::NoTrans ? ku_ : kl_);
}

//------------------------------------------------------------------------------
/// Sets number of superdiagonals within band.
template <typename scalar_t>
void BandMatrix<scalar_t>::upperBandwidth(int64_t ku)
{
    if (this->op() == Op::NoTrans)
        this->ku_ = ku;
    else
        this->kl_ = ku;
}

//------------------------------------------------------------------------------
/// Move all tiles back to their origin.
//
// Assuming fixed size, square tiles for simplicity
// todo: generalize
template <typename scalar_t>
void BandMatrix<scalar_t>::tileUpdateAllOrigin()
{
    int64_t mt = this->mt();
    int64_t nt = this->nt();
    int64_t klt = ceildiv( this->kl_, this->tileNb(0) );
    int64_t kut = ceildiv( this->ku_, this->tileNb(0) );
    for (int64_t j = 0; j < nt; ++j) {
        int64_t istart = blas::max( 0, j-kut );
        int64_t iend   = blas::min( j+klt+1, mt );
        for (int64_t i = istart; i < iend; ++i) {
            if (this->tileIsLocal(i, j)) {
                this->tileUpdateOrigin(i, j);
            }
        }
    }
}

} // namespace slate

#endif // SLATE_BAND_MATRIX_HH
