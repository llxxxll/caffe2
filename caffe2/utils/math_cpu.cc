// Implementes the math functions for CPU.
// The implementation in this file allows us to route the underlying numerical
// computation library to different backends. Notably:
// (1) For all BLAS-related functions, one can explicitly request a BLAS backend
//     such as MKL, openblas or Atlas. To see the set of supported backends
//     currently provided, check //third_party/blas/.
// (2) If one chooses to link against MKL, we utilize MKL's vector math library
//     (VML) for a few functions such as Exp and Log.
// (3) Fallback implementations are provided in Eigen for cross-platform
//     support. Since Eigen is a header-only library and supports a number of
//     platforms, it allows one to quickly port Caffe2 to different platforms
//     where BLAS may not be present.

#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <atomic>
#include <random>

#ifdef CAFFE2_USE_MKL
#include <mkl.h>
#endif  // CAFFE2_USE_MKL

#include "caffe2/utils/math.h"
#include "caffe2/core/context.h"
#include "Eigen/Core"
#include "Eigen/Dense"

namespace caffe2 {
namespace math {

////////////////////////////////////////////////////////////////////////////////
// BLAS alternatives.
// Depending on whether we have specified an external BLAS library or not, we
// will delegate the Caffe math functions that are BLAS-related to either the
// CBLAS call or the Eigen implementation.
////////////////////////////////////////////////////////////////////////////////
#ifdef CAFFE2_USE_EIGEN_FOR_BLAS

// Caffe2 gemm provides a simpler interface to the gemm functions, with the
// limitation that the data has to be contiguous in memory.
//
// The gemm call implements the following operation:
//
//                  C = alpha * op(A) * op(B) + beta * C
//
// where op(A) has size M x K, op(B) has size K x N, and C has size M x N. Each
// of A, B, and C are matrices and alpha and beta are scalars. Note that the
// most common use case of gemm will involve setting alpha to 1 and beta to 0.
//
// op(A) and op(B) represent the transformations that are done to A and B before
// the matrix multiply; depending on the flags set, op(A) is equal to A or A^T
// (transpose) if the argument TransA or TransB is set to CblasNoTrans or
// CblasTrans, respectively, for each of A and B.
template <>
void Gemm<float, CPUContext>(
    const CBLAS_TRANSPOSE TransA, const CBLAS_TRANSPOSE TransB,
    const int M, const int N, const int K, const float alpha, const float* A,
    const float* B, const float beta, float* C, CPUContext* context) {
  auto C_mat = EigenMatrixMap<float>(C, N, M);
  if (beta == 0) {
    C_mat.setZero();
  } else {
    C_mat *= beta;
  }
  switch (TransA) {
  case CblasNoTrans: {
    switch (TransB) {
    case CblasNoTrans:
      C_mat.noalias() += alpha * (
          ConstEigenMatrixMap<float>(B, N, K) *
          ConstEigenMatrixMap<float>(A, K, M));
      return;
    case CblasTrans:
      C_mat.noalias() += alpha * (
          ConstEigenMatrixMap<float>(B, K, N).transpose() *
          ConstEigenMatrixMap<float>(A, K, M));
      return;
    default:
      LOG(FATAL) << "Unexpected CBLAS_TRANSPOSE for TransB";
    }
  }
  case CblasTrans: {
    switch (TransB) {
    case CblasNoTrans:
      C_mat.noalias() += alpha * (
          ConstEigenMatrixMap<float>(B, N, K) *
          ConstEigenMatrixMap<float>(A, M, K).transpose());
      return;
    case CblasTrans:
      C_mat.noalias() += alpha * (
          ConstEigenMatrixMap<float>(B, K, N).transpose() *
          ConstEigenMatrixMap<float>(A, M, K).transpose());
      return;
    default:
      LOG(FATAL) << "Unexpected CBLAS_TRANSPOSE for TransB";
    }
  }
  default:
    LOG(FATAL) << "Unexpected CBLAS_TRANSPOSE for TransA";
  }
}

template <>
void Gemv<float, CPUContext>(
    const CBLAS_TRANSPOSE TransA, const int M, const int N, const float alpha,
    const float* A, const float* x, const float beta, float* y,
    CPUContext* context) {
  EigenVectorMap<float> y_vec(y, TransA == CblasNoTrans ? M : N);
  if (beta == 0) {
    // In Caffe2 we often do a lazy initialization, which may contain NaNs in
    // the float values. As a result, if beta is 0, we explicitly do a setzero.
    y_vec.setZero();
  } else {
    y_vec *= beta;
  }
  switch (TransA) {
    case CblasNoTrans: {
      y_vec.noalias() += alpha * (
          ConstEigenMatrixMap<float>(A, N, M).transpose() *
          ConstEigenVectorMap<float>(x, N));
      return;
    }
    case CblasTrans: {
      y_vec.noalias() += alpha * (
          ConstEigenMatrixMap<float>(A, N, M) *
          ConstEigenVectorMap<float>(x, M));
      return;
    }
    default:
      LOG(FATAL) << "Gemv float found an unexpected CBLAS_TRANSPOSE input.";
  }
}

#define CAFFE2_SPECIALIZED_SCALE(T)                                         \
  namespace detail {                                                        \
  template <>                                                               \
  void ScaleDynamic<T, CPUContext>(                                         \
      const int n,                                                          \
      const T alpha,                                                        \
      const T* x,                                                           \
      T* y,                                                                 \
      CPUContext* context) {                                                \
    EigenVectorMap<T>(y, n) = ConstEigenVectorMap<T>(x, n) * alpha;         \
  }                                                                         \
  }                                                                         \
  template <>                                                               \
  void Scale<T, CPUContext>(                                                \
      const int n, const T* alpha, const T* x, T* y, CPUContext* context) { \
    EigenVectorMap<T>(y, n) = ConstEigenVectorMap<T>(x, n) * (*alpha);      \
  }
CAFFE2_SPECIALIZED_SCALE(float)
CAFFE2_SPECIALIZED_SCALE(double)
#undef CAFFE2_SPECIALIZED_SCALE

#define CAFFE2_SPECIALIZED_DOT(T)                                              \
template<>                                                                     \
void Dot<T, CPUContext>(                                                       \
    const int N, const T* a, const T* b, T* y,                                 \
    CPUContext* context) {                                                     \
  *y = ConstEigenVectorMap<T>(a, N).dot(ConstEigenVectorMap<T>(b, N));         \
}
CAFFE2_SPECIALIZED_DOT(float)
CAFFE2_SPECIALIZED_DOT(double)
#undef CAFFE2_SPECIALIZED_DOT

#define CAFFE2_SPECIALIZED_AXPY(T)                                          \
  namespace detail {                                                        \
  template <>                                                               \
  void AxpyDynamic<T, CPUContext>(                                          \
      const int N,                                                          \
      const T alpha,                                                        \
      const T* x,                                                           \
      T* Y,                                                                 \
      CPUContext* context) {                                                \
    EigenVectorMap<T>(Y, N) += ConstEigenVectorMap<T>(x, N) * alpha;        \
  }                                                                         \
  }                                                                         \
  template <>                                                               \
  void Axpy<T, CPUContext>(                                                 \
      const int N, const T* alpha, const T* x, T* Y, CPUContext* context) { \
    EigenVectorMap<T>(Y, N) += ConstEigenVectorMap<T>(x, N) * (*alpha);     \
  }
CAFFE2_SPECIALIZED_AXPY(float)
CAFFE2_SPECIALIZED_AXPY(double)
#undef CAFFE2_SPECIALIZED_AXPY

#define CAFFE2_SPECIALIZED_AXPBY(T)                                            \
template <>                                                                    \
void Axpby<T, CPUContext>(const int N, const T alpha, const T* x,              \
                          const T beta, T* y, CPUContext* context) {           \
  EigenVectorMap<T> y_vec(y, N);                                               \
  y_vec = y_vec * beta + ConstEigenVectorMap<T>(x, N) * alpha;                 \
}
CAFFE2_SPECIALIZED_AXPBY(float)
CAFFE2_SPECIALIZED_AXPBY(double)
#undef CAFFE2_SPECIALIZED_AXPBY

#else  // CAFFE2_USE_EIGEN_FOR_BLAS

template <>
void Gemm<float, CPUContext>(
    const CBLAS_TRANSPOSE TransA, const CBLAS_TRANSPOSE TransB,
    const int M, const int N, const int K, const float alpha, const float* A,
    const float* B, const float beta, float* C, CPUContext* context) {
  int lda = (TransA == CblasNoTrans) ? K : M;
  int ldb = (TransB == CblasNoTrans) ? N : K;
  cblas_sgemm(CblasRowMajor, TransA, TransB, M, N, K, alpha, A, lda, B, ldb,
              beta, C, N);
}

template <>
void Gemm<float, CPUContext>(
    const CBLAS_TRANSPOSE TransA, const CBLAS_TRANSPOSE TransB,
    const int M, const int N, const int K, const float alpha, const float* A,
    const int lda, const float* B, const float beta, const int ldb, float* C,
    const int ldc, CPUContext* context) {
  cblas_sgemm(CblasRowMajor, TransA, TransB, M, N, K, alpha, A, lda, B, ldb,
              beta, C, ldc);
}

template <>
void Gemv<float, CPUContext>(
    const CBLAS_TRANSPOSE TransA, const int M, const int N, const float alpha,
    const float* A, const float* x, const float beta, float* y,
    CPUContext* context) {
  cblas_sgemv(CblasRowMajor, TransA, M, N, alpha, A, N, x, 1, beta, y, 1);
}

#define CAFFE2_SPECIALIZED_SCALE(T, prefix)                                 \
  namespace detail {                                                       \
  template <>                                                               \
  void ScaleDynamic<T, CPUContext>(                                         \
      const int n,                                                          \
      const T alpha,                                                        \
      const T* x,                                                           \
      T* y,                                                                 \
      CPUContext* context) {                                                \
    if (y != x)                                                             \
      cblas_##prefix##copy(n, x, 1, y, 1);                                  \
    cblas_##prefix##scal(n, alpha, y, 1);                                   \
  }                                                                         \
  }                                                                         \
  template <>                                                               \
  void Scale<T, CPUContext>(                                                \
      const int n, const T* alpha, const T* x, T* y, CPUContext* context) { \
    if (y != x)                                                             \
      cblas_##prefix##copy(n, x, 1, y, 1);                                  \
    cblas_##prefix##scal(n, *alpha, y, 1);                                  \
  }
CAFFE2_SPECIALIZED_SCALE(float, s)
CAFFE2_SPECIALIZED_SCALE(double, d)
#undef CAFFE2_SPECIALIZED_SCALE

#define CAFFE2_SPECIALIZED_DOT(T, prefix)                                      \
template<>                                                                     \
void Dot<T, CPUContext>(                                                       \
    const int N, const T* a, const T* b, T* y,                                 \
    CPUContext* context) {                                                     \
  *y = cblas_##prefix##dot(N, a, 1, b, 1);                                     \
}
CAFFE2_SPECIALIZED_DOT(float, s)
CAFFE2_SPECIALIZED_DOT(double, d)
#undef CAFFE2_SPECIALIZED_DOT

#define CAFFE2_SPECIALIZED_AXPY(T, prefix)                                  \
  namespace detail {                                                        \
  template <>                                                               \
  void AxpyDynamic<T, CPUContext>(                                          \
      const int N,                                                          \
      const T alpha,                                                        \
      const T* x,                                                           \
      T* y,                                                                 \
      CPUContext* context) {                                                \
    cblas_##prefix##axpy(N, alpha, x, 1, y, 1);                             \
  }                                                                         \
  }                                                                         \
  template <>                                                               \
  void Axpy<T, CPUContext>(                                                 \
      const int N, const T* alpha, const T* x, T* y, CPUContext* context) { \
    cblas_##prefix##axpy(N, *alpha, x, 1, y, 1);                            \
  }
CAFFE2_SPECIALIZED_AXPY(float, s)
CAFFE2_SPECIALIZED_AXPY(double, d)
#undef CAFFE2_SPECIALIZED_AXPY

// cblas_[sd]axpby is not a standard blas function, and if MKL is not present,
// we will need to implement it.
#ifdef CAFFE2_USE_MKL
#define CAFFE2_SPECIALIZED_AXPBY(T, prefix)                                    \
template <>                                                                    \
void Axpby<T, CPUContext>(const int N, const T alpha, const T* x,              \
                          const T beta, T* y, CPUContext* context) {           \
  cblas_##prefix##axpby(N, alpha, x, 1, beta, y, 1);                           \
}
#else  // CAFFE2_USE_MKL
#define CAFFE2_SPECIALIZED_AXPBY(T, prefix)                                    \
template <>                                                                    \
void Axpby<T, CPUContext>(const int N, const T alpha, const T* x,              \
                          const T beta, T* y, CPUContext* context) {           \
  cblas_##prefix##scal(N, beta, y, 1);                                         \
  cblas_##prefix##axpy(N, alpha, x, 1, y, 1);                                  \
}
#endif  // CAFFE2_USE_MKL
CAFFE2_SPECIALIZED_AXPBY(float, s)
CAFFE2_SPECIALIZED_AXPBY(double, d)
#undef CAFFE2_SPECIALIZED_AXPBY

#endif  // CAFFE2_USE_EIGEN_FOR_BLAS


////////////////////////////////////////////////////////////////////////////////
// MKL VML alternatives.
// Depending on whether we are using MKL, we will delegate the Caffe math
// functions that are VML-related to either the VML call or the Eigen
// implementation. If you are setting the flags (such as AVX) right for your CPU
// architecture, usually Eigen will deliver a throughput as fast as the VML
// functions.
////////////////////////////////////////////////////////////////////////////////
#ifdef CAFFE2_USE_MKL

#define DELEGATE_SIMPLE_UNARY_FUNCTION(T, Funcname, OriginalFunc)              \
template <>                                                                    \
void Funcname<T, CPUContext>(                                                  \
    const int N, const T* x, T* y,                                             \
    CPUContext* context) {                                                     \
  OriginalFunc(N, x, y);                                                       \
}
DELEGATE_SIMPLE_UNARY_FUNCTION(float, Exp, vsExp)
DELEGATE_SIMPLE_UNARY_FUNCTION(double, Exp, vdExp)
DELEGATE_SIMPLE_UNARY_FUNCTION(float, Log, vsLn)
DELEGATE_SIMPLE_UNARY_FUNCTION(double, Log, vdLn)
DELEGATE_SIMPLE_UNARY_FUNCTION(float, Sqr, vsSqr)
DELEGATE_SIMPLE_UNARY_FUNCTION(double, Sqr, vdSqr)
#undef DELEGATE_SIMPLE_UNARY_FUNCTION

#define DELEGATE_POWX_FUNCTION(T, OriginalFunc)                                \
template <>                                                                    \
void Powx<T, CPUContext>(                                                      \
    const int N, const T* a, T b, T* y, CPUContext* context) {                 \
  OriginalFunc(N, a, b, y);                                                    \
}
DELEGATE_POWX_FUNCTION(float, vsPowx)
DELEGATE_POWX_FUNCTION(double, vdPowx)
#undef DELEGATE_POWX_FUNCTION

#define DELEGATE_SIMPLE_BINARY_FUNCTION(T, Funcname, OriginalFunc)             \
template <>                                                                    \
void Funcname<T, CPUContext>(                                                  \
    const int N, const T* a, const T* b, T* y,                                 \
    CPUContext* context) {                                                     \
  OriginalFunc(N, a, b, y);                                                    \
}
DELEGATE_SIMPLE_BINARY_FUNCTION(float,  Add, vsAdd)
DELEGATE_SIMPLE_BINARY_FUNCTION(double, Add, vdAdd)
DELEGATE_SIMPLE_BINARY_FUNCTION(float,  Sub, vsSub)
DELEGATE_SIMPLE_BINARY_FUNCTION(double, Sub, vdSub)
DELEGATE_SIMPLE_BINARY_FUNCTION(float,  Mul, vsMul)
DELEGATE_SIMPLE_BINARY_FUNCTION(double, Mul, vdMul)
DELEGATE_SIMPLE_BINARY_FUNCTION(float,  Div, vsDiv)
DELEGATE_SIMPLE_BINARY_FUNCTION(double, Div, vdDiv)
#undef DELEGATE_SIMPLE_BINARY_FUNCTION

#else  // CAFFE2_USE_MKL

#define DELEGATE_SIMPLE_UNARY_FUNCTION(T, Funcname, expr)                      \
template <>                                                                    \
void Funcname<T, CPUContext>(const int N, const T* x, T* y,                    \
                             CPUContext* context) {                            \
  EigenVectorMap<T>(y, N) = ConstEigenVectorMap<T>(x, N).array().expr();       \
}
DELEGATE_SIMPLE_UNARY_FUNCTION(float, Exp, exp)
DELEGATE_SIMPLE_UNARY_FUNCTION(double, Exp, exp)
DELEGATE_SIMPLE_UNARY_FUNCTION(float, Log, log)
DELEGATE_SIMPLE_UNARY_FUNCTION(double, Log, log)
DELEGATE_SIMPLE_UNARY_FUNCTION(float, Sqr, square)
DELEGATE_SIMPLE_UNARY_FUNCTION(double, Sqr, square)
#undef DELEGATE_SIMPLE_UNARY_FUNCTION

#define DELEGATE_POWX_FUNCTION(T)                                              \
template <>                                                                    \
void Powx<T, CPUContext>(                                                      \
    const int N, const T* a, T b, T* y, CPUContext* context) {                 \
  EigenVectorMap<T>(y, N) = ConstEigenVectorMap<T>(a, N).array().pow(b);       \
}
DELEGATE_POWX_FUNCTION(float)
DELEGATE_POWX_FUNCTION(double)
#undef DELEGATE_POWX_FUNCTION

#endif  // CAFFE2_USE_MKL


#define EIGEN_SIMPLE_BINARY_FUNCTION(T, Funcname, expr)                        \
template <>                                                                    \
void Funcname<T, CPUContext>(                                                  \
    const int N, const T* a, const T* b, T* y,                                 \
    CPUContext*) {                                                             \
  EigenVectorMap<T>(y, N) =                                                    \
      ConstEigenVectorMap<T>(a, N).array() expr                                \
      ConstEigenVectorMap<T>(b, N).array();                                    \
}

#ifdef CAFFE2_USE_MKL

#define DEFINE_SIMPLE_BINARY_FUNCTION(Funcname, expr)                          \
EIGEN_SIMPLE_BINARY_FUNCTION(int32_t, Funcname, expr)                          \
EIGEN_SIMPLE_BINARY_FUNCTION(int64_t, Funcname, expr)

#else

#define DEFINE_SIMPLE_BINARY_FUNCTION(Funcname, expr)                          \
EIGEN_SIMPLE_BINARY_FUNCTION(float, Funcname, expr)                            \
EIGEN_SIMPLE_BINARY_FUNCTION(double, Funcname, expr)                           \
EIGEN_SIMPLE_BINARY_FUNCTION(int32_t, Funcname, expr)                          \
EIGEN_SIMPLE_BINARY_FUNCTION(int64_t, Funcname, expr)

#endif

DEFINE_SIMPLE_BINARY_FUNCTION(Add, +)
DEFINE_SIMPLE_BINARY_FUNCTION(Sub, -)
DEFINE_SIMPLE_BINARY_FUNCTION(Mul, *)
DEFINE_SIMPLE_BINARY_FUNCTION(Div, /)

#undef EIGEN_SIMPLE_BINARY_FUNCTION
#undef DEFINE_FLOAT_BINARY_FUNCTION


////////////////////////////////////////////////////////////////////////////////
// Common math functions being used in Caffe that do not have a BLAS or MKL
// equivalent. For all these functions, we will simply implement them either via
// Eigen or via custom code.
////////////////////////////////////////////////////////////////////////////////

#define CAFFE2_SPECIALIZED_ROWWISEMAX(T)                                       \
template <> void RowwiseMax<T, CPUContext>(                                    \
    const int N, const int D, const T* x, T* y, CPUContext* context) {         \
  EigenVectorMap<T>(y, N) =                                                    \
      ConstEigenMatrixMap<T>(x, D, N).colwise().maxCoeff();                    \
}
CAFFE2_SPECIALIZED_ROWWISEMAX(float)

#define CAFFE2_SPECIALIZED_COLWISEMAX(T)                                       \
template <> void ColwiseMax<T, CPUContext>(                                    \
    const int N, const int D, const T* x, T* y, CPUContext* context) {         \
  EigenVectorMap<T>(y, D) =                                                    \
      ConstEigenMatrixMap<T>(x, D, N).rowwise().maxCoeff();                    \
}
CAFFE2_SPECIALIZED_COLWISEMAX(float)

// AddToRow and AddToCol adds the corresponding row/col vector b to the matrix a
// of shape M x N. The actual implementation uses eigen which is column major,
// so notice the row/column swap in the actual implementation.
#define DELEGATE_BROADCAST_BINARY_FUNCTION(T, Funcname, expr)               \
  template <>                                                               \
  void Funcname##ToRow<T, CPUContext>(                                      \
      const int M,                                                          \
      const int N,                                                          \
      const T* a,                                                           \
      const T* b,                                                           \
      T* y,                                                                 \
      CPUContext*) {                                                        \
    EigenArrayMap<T>(y, N, M) = ConstEigenArrayMap<T>(a, N, M).colwise()    \
                                    expr ConstEigenVectorArrayMap<T>(b, N); \
  }                                                                         \
  /* inplace versions */                                                    \
  template <>                                                               \
  void Funcname##ToRow<T, CPUContext>(                                      \
      const int M, const int N, const T* x, T* y, CPUContext* context) {    \
    EigenArrayMap<T>(y, N, M).colwise() expr## =                            \
        ConstEigenVectorArrayMap<T>(x, N);                                  \
  }                                                                         \
  template <>                                                               \
  void Funcname##ToCol<T, CPUContext>(                                      \
      const int M, const int N, const T* x, T* y, CPUContext* context) {    \
    EigenArrayMap<T>(y, N, M).rowwise() expr## =                            \
        ConstEigenVectorArrayMap<T>(x, M).transpose();                      \
  }

#define DEFINE_BROADCAST_BINARY_FUNCTION(name, op)                       \
  DELEGATE_BROADCAST_BINARY_FUNCTION(int32_t, name, op)                  \
  DELEGATE_BROADCAST_BINARY_FUNCTION(int64_t, name, op)                  \
  DELEGATE_BROADCAST_BINARY_FUNCTION(float, name, op)                    \
  DELEGATE_BROADCAST_BINARY_FUNCTION(double, name, op)

DEFINE_BROADCAST_BINARY_FUNCTION(Add, +)
DEFINE_BROADCAST_BINARY_FUNCTION(Sub, -)
DEFINE_BROADCAST_BINARY_FUNCTION(Mul, *)
DEFINE_BROADCAST_BINARY_FUNCTION(Div, /)

#undef DEFINE_BROADCAST_BINARY_FUNCTION
#undef DELEGATE_BROADCAST_BINARY_FUNCTION

#define CAFFE2_SPECIALIZED_SET(T)                                              \
template <>                                                                    \
void Set<T, CPUContext>(const int N, const T alpha, T *Y,                      \
                           CPUContext* context) {                              \
  EigenVectorMap<T>(Y, N).setConstant(alpha);                                  \
}

CAFFE2_SPECIALIZED_SET(float);
CAFFE2_SPECIALIZED_SET(double);
CAFFE2_SPECIALIZED_SET(int);
CAFFE2_SPECIALIZED_SET(int64_t);
CAFFE2_SPECIALIZED_SET(bool);
CAFFE2_SPECIALIZED_SET(char);
#undef CAFFE2_SPECIALIZED_SET

#define CAFFE2_INSTANTIATE_BINARY_OP(name, op, T)                          \
  template <>                                                              \
  void name<T, CPUContext>(                                                \
      const int n, const T* a, const T* b, bool* y, CPUContext* context) { \
    for (int i = 0; i < n; ++i) {                                          \
      y[i] = a[i] op b[i];                                                 \
    }                                                                      \
  }                                                                        \
  template <>                                                              \
  void name##ToRow<T, CPUContext>(                                         \
      const int m,                                                         \
      const int n,                                                         \
      const T* a,                                                          \
      const T* b,                                                          \
      bool* y,                                                             \
      CPUContext* context) {                                               \
    for (int i = 0; i < n * m; ++i) {                                      \
      y[i] = a[i] op b[i % n];                                             \
    }                                                                      \
  }

#define CAFFE2_DEFINE_BINARY_OP(name, op)         \
  CAFFE2_INSTANTIATE_BINARY_OP(name, op, float)   \
  CAFFE2_INSTANTIATE_BINARY_OP(name, op, double)  \
  CAFFE2_INSTANTIATE_BINARY_OP(name, op, int32_t) \
  CAFFE2_INSTANTIATE_BINARY_OP(name, op, int64_t)

CAFFE2_DEFINE_BINARY_OP(LT, <);
CAFFE2_DEFINE_BINARY_OP(LE, <=);
CAFFE2_DEFINE_BINARY_OP(GT, >);
CAFFE2_DEFINE_BINARY_OP(GE, >=);

CAFFE2_INSTANTIATE_BINARY_OP(Or, |, bool);
CAFFE2_INSTANTIATE_BINARY_OP(And, &, bool);
CAFFE2_INSTANTIATE_BINARY_OP(Xor, ^, bool);

template <>
void Not<bool, CPUContext>(
    const int n,
    const bool* x,
    bool* y,
    CPUContext* context) {
  for (int i = 0; i < n; ++i) {
    y[i] = !x[i];
  }
}

#undef CAFFE2_DEFINE_BINARY_OP
#undef CAFFE2_INSTANTIATE_BINARY_OP

template <>
void RandUniform<float, CPUContext>(
    const int n, const float a, const float b, float* r,
    CPUContext* context) {
  std::uniform_real_distribution<float> distribution(a, b);
  for (int i = 0; i < n; ++i) {
    r[i] = distribution(context->RandGenerator());
  }
}

template <>
void RandUniform<int, CPUContext>(
    const int n, const int a, const int b, int* r,
    CPUContext* context) {
  std::uniform_int_distribution<int> distribution(a, b);
  for (int i = 0; i < n; ++i) {
    r[i] = distribution(context->RandGenerator());
  }
}


template <>
void RandGaussian<float, CPUContext>(
    const int n, const float mean, const float std, float* r,
    CPUContext* context) {
  std::normal_distribution<float> distribution(mean, std);
  for (int i = 0; i < n; ++i) {
    r[i] = distribution(context->RandGenerator());
  }
}

template<>
void Sum<float, CPUContext>(
    const int N, const float* x, float* y,
    CPUContext* context) {
  *y = ConstEigenVectorMap<float>(x, N).sum();
}

template<>
void Sum<double, CPUContext>(
    const int N, const double* x, double* y,
    CPUContext* context) {
  *y = ConstEigenVectorMap<double>(x, N).sum();
}

template <>
void Select<float, CPUContext>(
      const int N, const int D, const float* x, const int* idx, float* y,
      CPUContext* context) {
  for (int i = 0; i < N; ++i) {
    DCHECK_LT(idx[i], D);
    y[i] = x[i * D + idx[i]];
  }
}

// Function uses casting from int to unsigned to compare if value of
// parameter a is greater or equal to zero and lower than value of
// parameter b. The b parameter is of type signed and is always
// positive,
// therefore its value is always lower than 0x800... where casting
// negative value of a parameter converts it to value higher than
// 0x800...
// The casting allows to use one condition instead of two.
inline bool is_a_ge_zero_and_a_lt_b(int a, int b) {
  return static_cast<unsigned>(a) < static_cast<unsigned>(b);
}

template <>
void Im2col<float, CPUContext, StorageOrder::NCHW>(
    const float* data_im,
    const int channels,
    const int height,
    const int width,
    const int kernel_h,
    const int kernel_w,
    const int dilation_h,
    const int dilation_w,
    const int pad_t,
    const int pad_l,
    const int pad_b,
    const int pad_r,
    const int stride_h,
    const int stride_w,
    float* data_col,
    CPUContext* context) {
  const int output_h =
      (height + pad_b + pad_t - (dilation_h * (kernel_h - 1) + 1)) / stride_h +
      1;
  const int output_w =
      (width + pad_l + pad_r - (dilation_w * (kernel_w - 1) + 1)) / stride_w +
      1;

  // Fast path for zero padding and no dilation
  // From Torch, THNN_(unfolded_copy)
  if (dilation_h == 1 && dilation_w == 1 && pad_l == 0 && pad_r == 0 &&
      pad_t == 0 && pad_b == 0) {
    for (auto k = 0; k < channels * kernel_h * kernel_w; k++) {
      const auto nip = k / (kernel_h * kernel_w);
      const auto rest = k % (kernel_h * kernel_w);
      const auto kh = rest / kernel_w;
      const auto kw = rest % kernel_w;
      auto* dst = data_col + nip * (kernel_h * kernel_w * output_h * output_w) +
          kh * (kernel_w * output_h * output_w) + kw * (output_h * output_w);
      const auto* src = data_im + nip * (height * width);
      for (auto y = 0; y < output_h; y++) {
        const auto iy = y * stride_h + kh;
        const auto ix = kw;
        if (stride_w == 1) {
          memcpy(
              dst + (y * output_w),
              src + (iy * width + ix),
              sizeof(float) * output_w);
        } else {
          for (auto x = 0; x < output_w; x++) {
            memcpy(
                dst + (y * output_w + x),
                src + (iy * width + ix + x * stride_w),
                sizeof(float));
          }
        }
      }
    }
    return;
  }

  // Fast path for equal padding
  if (pad_l == pad_r && pad_t == pad_b) {
    // From Intel, https://github.com/BVLC/caffe/pull/3536
    const int pad_h = pad_t;
    const int pad_w = pad_l;
    const int channel_size = height * width;
    for (int channel = channels; channel--; data_im += channel_size) {
      for (int kernel_row = 0; kernel_row < kernel_h; kernel_row++) {
        for (int kernel_col = 0; kernel_col < kernel_w; kernel_col++) {
          int input_row = -pad_h + kernel_row * dilation_h;
          for (int output_rows = output_h; output_rows; output_rows--) {
            if (!is_a_ge_zero_and_a_lt_b(input_row, height)) {
              for (int output_cols = output_w; output_cols; output_cols--) {
                *(data_col++) = 0;
              }
            } else {
              int input_col = -pad_w + kernel_col * dilation_w;
              for (int output_col = output_w; output_col; output_col--) {
                if (is_a_ge_zero_and_a_lt_b(input_col, width)) {
                  *(data_col++) = data_im[input_row * width + input_col];
                } else {
                  *(data_col++) = 0;
                }
                input_col += stride_w;
              }
            }
            input_row += stride_h;
          }
        }
      }
    }
    return;
  }

  // Baseline
  const int dkernel_h = dilation_h * (kernel_h - 1) + 1;
  const int dkernel_w = dilation_w * (kernel_w - 1) + 1;

  int height_col = (height + pad_t + pad_b - dkernel_h) / stride_h + 1;
  int width_col = (width + pad_l + pad_r - dkernel_w) / stride_w + 1;

  int channels_col = channels * kernel_h * kernel_w;
  for (int c = 0; c < channels_col; ++c) {
    int w_offset = c % kernel_w;
    int h_offset = (c / kernel_w) % kernel_h;
    int c_im = c / kernel_h / kernel_w;
    for (int h = 0; h < height_col; ++h) {
      for (int w = 0; w < width_col; ++w) {
        int h_pad = h * stride_h - pad_t + h_offset * dilation_h;
        int w_pad = w * stride_w - pad_l + w_offset * dilation_w;
        if (h_pad >= 0 && h_pad < height && w_pad >= 0 && w_pad < width)
          data_col[(c * height_col + h) * width_col + w] =
              data_im[(c_im * height + h_pad) * width + w_pad];
        else
          data_col[(c * height_col + h) * width_col + w] = 0;
      }
    }
  }
}

template <>
void Im2col<float, CPUContext, StorageOrder::NHWC>(
    const float* data_im,
    const int channels,
    const int height,
    const int width,
    const int kernel_h,
    const int kernel_w,
    const int dilation_h,
    const int dilation_w,
    const int pad_t,
    const int pad_l,
    const int pad_b,
    const int pad_r,
    const int stride_h,
    const int stride_w,
    float* data_col,
    CPUContext* context) {
  const int dkernel_h = dilation_h * (kernel_h - 1) + 1;
  const int dkernel_w = dilation_w * (kernel_w - 1) + 1;

  int height_col = (height + pad_t + pad_b - dkernel_h) / stride_h + 1;
  int width_col = (width + pad_l + pad_r - dkernel_w) / stride_w + 1;

  int h_pad = -pad_t;
  for (int h = 0; h < height_col; ++h) {
    int w_pad = -pad_l;
    for (int w = 0; w < width_col; ++w) {
      for (int ih = h_pad; ih < h_pad + dkernel_h; ih += dilation_h) {
        for (int iw = w_pad; iw < w_pad + dkernel_w; iw += dilation_w) {
          if (ih >= 0 && ih < height && iw >= 0 && iw < width) {
            memcpy(data_col, data_im + (ih * width + iw) * channels,
                   sizeof(float) * channels);
          } else {
            // This should be simply padded with zero.
            memset(data_col, 0, sizeof(float) * channels);
          }
          data_col += channels;
        }
      }
      w_pad += stride_w;
    }
    h_pad += stride_h;
  }
}

template <>
void Col2im<float, CPUContext, StorageOrder::NCHW>(
    const float* data_col,
    const int channels,
    const int height,
    const int width,
    const int kernel_h,
    const int kernel_w,
    const int dilation_h,
    const int dilation_w,
    const int pad_t,
    const int pad_l,
    const int pad_b,
    const int pad_r,
    const int stride_h,
    const int stride_w,
    float* data_im,
    CPUContext* context) {
  const int output_h =
      (height + pad_b + pad_t - (dilation_h * (kernel_h - 1) + 1)) / stride_h +
      1;
  const int output_w =
      (width + pad_l + pad_r - (dilation_w * (kernel_w - 1) + 1)) / stride_w +
      1;

  Set<float, CPUContext>(height * width * channels, 0, data_im, context);

  // Fast path for zero padding and no dilation
  // From Torch, modified THNN_(unfolded_acc)
  if (dilation_h == 1 && dilation_w == 1 && pad_l == 0 && pad_r == 0 &&
      pad_t == 0 && pad_b == 0) {
    for (auto k = 0; k < channels * kernel_h * kernel_w; k++) {
      const auto nip = k / (kernel_h * kernel_w);
      const auto rest = k % (kernel_h * kernel_w);
      const auto kh = rest / kernel_w;
      const auto kw = rest % kernel_w;
      const auto* dst = data_col +
          nip * (kernel_h * kernel_w * output_h * output_w) +
          kh * (kernel_w * output_h * output_w) + kw * (output_h * output_w);
      auto* src = data_im + nip * (height * width);
      for (auto y = 0; y < output_h; y++) {
        const auto iy = y * stride_h + kh;
        const auto ix = kw;
        if (stride_w == 1) {
          auto offsrc = src + (iy * width + ix);
          const auto offdst = dst + (y * output_w);
          for (auto i = 0; i < output_w; ++i) {
            offsrc[i] += offdst[i];
          }
        } else {
          for (auto x = 0; x < output_w; x++) {
            auto offsrc = src + (iy * width + ix + x * stride_w);
            const auto offdst = dst + (y * output_w + x);
            *offsrc += *offdst;
          }
        }
      }
    }
    return;
  }

  // Fast path for equal padding
  if (pad_l == pad_r && pad_t == pad_b) {
    // From Intel, https://github.com/BVLC/caffe/pull/3536
    const int pad_h = pad_t;
    const int pad_w = pad_l;
    const int channel_size = height * width;
    for (int channel = channels; channel--; data_im += channel_size) {
      for (int kernel_row = 0; kernel_row < kernel_h; kernel_row++) {
        for (int kernel_col = 0; kernel_col < kernel_w; kernel_col++) {
          int input_row = -pad_h + kernel_row * dilation_h;
          for (int output_rows = output_h; output_rows; output_rows--) {
            if (!is_a_ge_zero_and_a_lt_b(input_row, height)) {
              data_col += output_w;
            } else {
              int input_col = -pad_w + kernel_col * dilation_w;
              for (int output_col = output_w; output_col; output_col--) {
                if (is_a_ge_zero_and_a_lt_b(input_col, width)) {
                  data_im[input_row * width + input_col] += *data_col;
                }
                data_col++;
                input_col += stride_w;
              }
            }
            input_row += stride_h;
          }
        }
      }
    }
    return;
  }

  // Fallback
  const int dkernel_h = dilation_h * (kernel_h - 1) + 1;
  const int dkernel_w = dilation_w * (kernel_w - 1) + 1;

  int height_col = (height + pad_t + pad_b - dkernel_h) / stride_h + 1;
  int width_col = (width + pad_l + pad_r - dkernel_w) / stride_w + 1;
  int channels_col = channels * kernel_h * kernel_w;
  for (int c = 0; c < channels_col; ++c) {
    int w_offset = c % kernel_w;
    int h_offset = (c / kernel_w) % kernel_h;
    int c_im = c / kernel_h / kernel_w;
    for (int h = 0; h < height_col; ++h) {
      for (int w = 0; w < width_col; ++w) {
        int h_pad = h * stride_h - pad_t + h_offset * dilation_h;
        int w_pad = w * stride_w - pad_l + w_offset * dilation_w;
        if (h_pad >= 0 && h_pad < height && w_pad >= 0 && w_pad < width) {
          data_im[(c_im * height + h_pad) * width + w_pad] +=
              data_col[(c * height_col + h) * width_col + w];
        }
      }
    }
  }
}

template <>
void Col2im<float, CPUContext, StorageOrder::NHWC>(
    const float* data_col,
    const int channels,
    const int height,
    const int width,
    const int kernel_h,
    const int kernel_w,
    const int dilation_h,
    const int dilation_w,
    const int pad_t,
    const int pad_l,
    const int pad_b,
    const int pad_r,
    const int stride_h,
    const int stride_w,
    float* data_im,
    CPUContext* context) {
  const int dkernel_h = dilation_h * (kernel_h - 1) + 1;
  const int dkernel_w = dilation_w * (kernel_w - 1) + 1;

  Set<float, CPUContext>(height * width * channels, 0, data_im, context);
  int height_col = (height + pad_t + pad_b - dkernel_h) / stride_h + 1;
  int width_col = (width + pad_l + pad_r - dkernel_w) / stride_w + 1;
  int h_pad = -pad_t;
  for (int h = 0; h < height_col; ++h) {
    int w_pad = -pad_l;
    for (int w = 0; w < width_col; ++w) {
      for (int ih = h_pad; ih < h_pad + dkernel_h; ih += dilation_h) {
        for (int iw = w_pad; iw < w_pad + dkernel_w; iw += dilation_w) {
          if (ih >= 0 && ih < height && iw >= 0 && iw < width) {
            auto* data_im_patch = data_im + (ih * width + iw) * channels;
            Add<float, CPUContext>(
                  channels, data_im_patch, data_col, data_im_patch, context);
          }
          data_col += channels;
        }
      }
      w_pad += stride_w;
    }
    h_pad += stride_h;
  }
}

template <>
void CopyMatrix<CPUContext>(
    const size_t itemsize, const int M, const int N, const void* A,
    const int lda, void* B, const int ldb, CPUContext* context) {
  for (int i = 0; i < M; ++i) {
    memcpy(static_cast<char*>(B) + ldb * i * itemsize,
           static_cast<const char*>(A) + lda * i * itemsize,
           itemsize * N);
  }
}

uint32_t randomNumberSeed() {
  // Copied from folly::randomNumberSeed (at 418ad4)
  static std::atomic<uint32_t> seedInput(0);
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  const uint32_t kPrime0 = 51551;
  const uint32_t kPrime1 = 61631;
  const uint32_t kPrime2 = 64997;
  const uint32_t kPrime3 = 111857;
  return kPrime0 * (seedInput++) + kPrime1 * static_cast<uint32_t>(getpid()) +
      kPrime2 * static_cast<uint32_t>(tv.tv_sec) +
      kPrime3 * static_cast<uint32_t>(tv.tv_usec);
}

}  // namespace math
}  // namespace caffe2
