#include "slate.hh"
#include "test.hh"
#include "blas_flops.hh"

#include "scalapack_wrappers.hh"
#include "scalapack_support_routines.hh"
#include "print_matrix.hh"
#include "band_utils.hh"

#include "slate_mpi.hh"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#ifdef SLATE_WITH_MKL
extern "C" int MKL_Set_Num_Threads(int nt);
inline int slate_set_num_blas_threads(const int nt) { return MKL_Set_Num_Threads(nt); }
#else
inline int slate_set_num_blas_threads(const int nt) { return -1; }
#endif

#undef PIN_MATRICES

//------------------------------------------------------------------------------
template<typename scalar_t>
void test_tbsm_work(Params &params, bool run)
{
    using lld = long long;
    using real_t = blas::real_type<scalar_t>;
    using blas::real;
    using blas::imag;

    // get & mark input values
    blas::Side side = params.side.value();
    lapack::Uplo uplo = params.uplo.value();
    lapack::Op transA = params.transA.value();
    lapack::Op transB = params.transB.value();
    blas::Diag diag = params.diag.value();
    scalar_t alpha = params.alpha.value();
    int64_t m = params.dim.m();
    int64_t n = params.dim.n();
    int64_t kd = params.kd();
    int64_t nb = params.nb.value();
    int64_t p = params.p.value();
    int64_t q = params.q.value();
    int64_t lookahead = params.lookahead.value();
    lapack::Norm norm = params.norm.value();
    bool check = params.check.value()=='y';
    bool ref = params.ref.value()=='y';
    bool trace = params.trace.value()=='y';
    int verbose = params.verbose.value();
    slate::Target target = char2target(params.target.value());

    // mark non-standard output values
    params.time.value();
    //params.gflops.value();
    params.ref_time.value();
    //params.ref_gflops.value();

    if (! run) {
        printf("%% This does NOT test pivoting in tbsm. See gbtrs for that.\n");
        return;
    }

    // setup so trans(B) is m-by-n
    int64_t An  = (side == blas::Side::Left ? m : n);
    int64_t Am  = An;
    int64_t Bm  = (transB == blas::Op::NoTrans ? m : n);
    int64_t Bn  = (transB == blas::Op::NoTrans ? n : m);

    // local values
    const int izero=0, ione=1;

    // BLACS/MPI variables
    int ictxt, nprow, npcol, myrow, mycol, info;
    int descA_tst[9], descB_tst[9], descB_ref[9];
    int iam=0, nprocs=1;
    int iseed = 1;

    // initialize BLACS and ScaLAPACK
    Cblacs_pinfo(&iam, &nprocs);
    assert(p*q <= nprocs);
    Cblacs_get(-1, 0, &ictxt);
    Cblacs_gridinit(&ictxt, "Col", p, q);
    Cblacs_gridinfo(ictxt, &nprow, &npcol, &myrow, &mycol);
    int mpirank = mycol*nprow + myrow;

    // matrix A, figure out local size, allocate, create descriptor, initialize
    int64_t mlocA = scalapack_numroc(Am, nb, myrow, izero, nprow);
    int64_t nlocA = scalapack_numroc(An, nb, mycol, izero, npcol);
    scalapack_descinit(descA_tst, Am, An, nb, nb, izero, izero, ictxt, mlocA, &info);
    assert(info==0);
    int64_t lldA = (int64_t)descA_tst[8];
    std::vector<scalar_t> A_tst(lldA * nlocA);
    scalapack_pplrnt(&A_tst[0], Am, An, nb, nb, myrow, mycol, nprow, npcol, mlocA, iseed+1);
    zeroOutsideBand(&A_tst[0], Am, An, kd, kd, nb, nb, myrow, mycol, nprow, npcol, mlocA);

    // matrix B, figure out local size, allocate, create descriptor, initialize
    int64_t mlocB = scalapack_numroc(Bm, nb, myrow, izero, nprow);
    int64_t nlocB = scalapack_numroc(Bn, nb, mycol, izero, npcol);
    scalapack_descinit(descB_tst, Bm, Bn, nb, nb, izero, izero, ictxt, mlocB, &info);
    assert(info==0);
    int64_t lldB = (int64_t)descB_tst[8];
    std::vector<scalar_t> B_tst(lldB * nlocB);
    scalapack_pplrnt(&B_tst[0], Bm, Bn, nb, nb, myrow, mycol, nprow, npcol, mlocB, iseed+2);

    // if check is required, copy test data and create a descriptor for it
    std::vector< scalar_t > B_ref;
    if (check || ref) {
        scalapack_descinit(descB_ref, Bm, Bn, nb, nb, izero, izero, ictxt, mlocB, &info);
        assert(info==0);
        B_ref = B_tst;
    }

    // create SLATE matrices from the ScaLAPACK layouts
    auto Aband = BandFromScaLAPACK(
        Am, An, kd, kd, &A_tst[0], lldA, nb, nprow, npcol, MPI_COMM_WORLD);
    auto A = slate::TriangularBandMatrix<scalar_t>(uplo, diag, Aband);
    auto B = slate::Matrix<scalar_t>::fromScaLAPACK
        (Bm, Bn, &B_tst[0], lldB, nb, nprow, npcol, MPI_COMM_WORLD);
    slate::Pivots pivots;

    // Make A diagonally dominant to be reasonably well conditioned.
    // tbsm seems to pass with unit diagonal, even without diagonal dominance.
    for (int i = 0; i < A.mt(); ++i) {
        if (A.tileIsLocal(i, i)) {
            auto T = A(i, i);
            for (int ii = 0; ii < T.mb(); ++ii) {
                T.at(ii, ii) += Am;
            }
        }
    }

    if (transA == blas::Op::Trans)
        A = transpose(A);
    else if (transA == blas::Op::ConjTrans)
        A = conj_transpose(A);

    if (transB == blas::Op::Trans)
        B = transpose(B);
    else if (transB == blas::Op::ConjTrans)
        B = conj_transpose(B);

    if (verbose > 1) {
        // todo: print_matrix( A ) calls Matrix version;
        // need TriangularBandMatrix version.
        printf("alpha = %10.6f + %10.6fi;\n", real(alpha), imag(alpha));
        print_matrix("A_tst", mlocA, nlocA, &A_tst[0], lldA, p, q, MPI_COMM_WORLD);
        print_matrix("B_tst", mlocB, nlocB, &B_tst[0], lldB, p, q, MPI_COMM_WORLD);
        print_matrix("A", Aband);
        print_matrix("B", B);
    }

    if (trace) slate::trace::Trace::on();
    else slate::trace::Trace::off();

    {
        slate::trace::Block trace_block("MPI_Barrier");
        MPI_Barrier(MPI_COMM_WORLD);
    }
    double time = libtest::get_wtime();

//printf("%% tbsm\n");
    //----------------------------------------
    // call the routine
    slate::tbsm(side, alpha, A, pivots, B, {
        {slate::Option::Lookahead, lookahead},
        {slate::Option::Target, target}
    });

    {
        slate::trace::Block trace_block("MPI_Barrier");
        MPI_Barrier(MPI_COMM_WORLD);
    }
    double time_tst = libtest::get_wtime() - time;

    if (trace) slate::trace::Trace::finish();

    // compute and save timing/performance
    //double gflop = blas::Gflop<scalar_t>::gemm(m, n, k);
    params.time.value() = time_tst;
    //params.gflops.value() = gflop / time_tst;

    if (verbose > 1) {
        print_matrix("B2", B);
        print_matrix("B2_tst", mlocB, nlocB, &B_tst[0], lldB, p, q, MPI_COMM_WORLD);
    }

    if (check || ref) {
//printf("%% check & ref\n");
        // comparison with reference routine from ScaLAPACK

        // set MKL num threads appropriately for parallel BLAS
        int omp_num_threads;
        #pragma omp parallel
        { omp_num_threads = omp_get_num_threads(); }
        int saved_num_threads = slate_set_num_blas_threads(omp_num_threads);

        std::vector<real_t> worklantr(std::max({ mlocA, nlocA }));
        std::vector<real_t> worklange(std::max({ mlocB, nlocB }));

        // get norms of the original data
        real_t A_orig_norm = scalapack_plantr(norm2str(norm), uplo2str(uplo), diag2str(diag), Am, An, &A_tst[0], ione, ione, descA_tst, &worklantr[0]);
        real_t B_orig_norm = scalapack_plange(norm2str(norm), Bm, Bn, &B_tst[0], ione, ione, descB_tst, &worklange[0]);

        if (verbose > 1) {
            print_matrix("B_ref", mlocB, nlocB, &B_ref[0], lldB, p, q, MPI_COMM_WORLD);
        }

        //----------------------------------------
        // call the reference routine
        MPI_Barrier(MPI_COMM_WORLD);
        time = libtest::get_wtime();
        scalapack_ptrsm(side2str(side), uplo2str(uplo), op2str(transA), diag2str(diag),
                         m, n, alpha,
                         &A_tst[0], ione, ione, descA_tst,
                         &B_ref[0], ione, ione, descB_ref);
        MPI_Barrier(MPI_COMM_WORLD);
        double time_ref = libtest::get_wtime() - time;

        if (verbose > 1) {
            print_matrix("B2_ref", mlocB, nlocB, &B_ref[0], lldB, p, q, MPI_COMM_WORLD);
        }
        // local operation: error = B_ref - B_tst
        blas::axpy(B_ref.size(), -1.0, &B_tst[0], 1, &B_ref[0], 1);

        // norm(B_ref - B_tst)
        real_t B_diff_norm = scalapack_plange(norm2str(norm), Bm, Bn, &B_ref[0], ione, ione, descB_ref, &worklange[0]);

        if (verbose > 1) {
            print_matrix("B_diff", mlocB, nlocB, &B_ref[0], lldB, p, q, MPI_COMM_WORLD);
        }
        real_t error = B_diff_norm
                       / (sqrt(real_t(Am)+2) * std::abs(alpha) * A_orig_norm * B_orig_norm);

        params.ref_time.value() = time_ref;
        //params.ref_gflops.value() = gflop / time_ref;
        params.error.value() = error;

        slate_set_num_blas_threads(saved_num_threads);

        // Allow 3*eps; complex needs 2*sqrt(2) factor; see Higham, 2002, sec. 3.6.
        real_t eps = std::numeric_limits<real_t>::epsilon();
        params.okay.value() = (params.error.value() <= 3*eps);
    }
//printf("%% done\n");

#ifdef PIN_MATRICES
    cuerror = cudaHostUnregister(&A_tst[0]);
    cuerror = cudaHostUnregister(&B_tst[0]);
#endif

    //Cblacs_exit(1) is commented out because it does not handle re-entering ... some unknown problem
    //Cblacs_exit(1); // 1 means that you can run Cblacs again
}

// -----------------------------------------------------------------------------
void test_tbsm(Params &params, bool run)
{
    switch(params.datatype.value()) {
        case libtest::DataType::Integer:
            throw std::exception();
            break;

        case libtest::DataType::Single:
            test_tbsm_work<float> (params, run);
            break;

        case libtest::DataType::Double:
            test_tbsm_work<double> (params, run);
            break;

        case libtest::DataType::SingleComplex:
            test_tbsm_work<std::complex<float>> (params, run);
            break;

        case libtest::DataType::DoubleComplex:
            test_tbsm_work<std::complex<double>> (params, run);
            break;
    }
}
