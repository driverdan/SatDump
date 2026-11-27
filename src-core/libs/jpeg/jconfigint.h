/*
 * jconfigint.h for SatDump.
 *
 * Upstream's CMake generates this file from src/jconfigint.h.in after probing
 * the compiler. SatDump builds the library from its own CMake, so the probed
 * values are derived from predefined compiler macros here instead.
 */

/* Rename every global symbol, see satdump_jpeg_prefix.h */
#include "satdump_jpeg_prefix.h"

/* libjpeg-turbo build number */
#define BUILD  "satdump"

/* How to hide global symbols. */
#if !defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
#define HIDDEN  __attribute__((visibility("hidden")))
#else
#define HIDDEN
#endif

/* Compiler's inline keyword */
#undef inline

/* How to obtain function inlining. */
#if defined(_MSC_VER)
#define INLINE  __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define INLINE  __inline__ __attribute__((always_inline))
#else
#define INLINE  __inline
#endif

/* How to obtain thread-local storage */
#if defined(_MSC_VER)
#define THREAD_LOCAL  __declspec(thread)
#else
#define THREAD_LOCAL  __thread
#endif

/* Define to the full name of this package. */
#define PACKAGE_NAME  "libjpeg-turbo"

/* Version number of package */
#define VERSION  "3.2.1"

/* The size of `size_t', as computed by sizeof. */
#if defined(__SIZEOF_SIZE_T__)
#define SIZEOF_SIZE_T  __SIZEOF_SIZE_T__
#elif defined(_WIN64)
#define SIZEOF_SIZE_T  8
#elif defined(_WIN32)
#define SIZEOF_SIZE_T  4
#else
#error "Cannot determine the size of size_t"
#endif

/* Define if your compiler has __builtin_ctzl() and sizeof(unsigned long) == sizeof(size_t). */
#if (defined(__GNUC__) || defined(__clang__)) && defined(__SIZEOF_LONG__) && \
    __SIZEOF_LONG__ == SIZEOF_SIZE_T
#define HAVE_BUILTIN_CTZL
#endif

/* Define to 1 if you have the <intrin.h> header file. */
#if defined(_MSC_VER)
#define HAVE_INTRIN_H
#endif

#if defined(_MSC_VER) && defined(HAVE_INTRIN_H)
#if (SIZEOF_SIZE_T == 8)
#define HAVE_BITSCANFORWARD64
#elif (SIZEOF_SIZE_T == 4)
#define HAVE_BITSCANFORWARD
#endif
#endif

#if defined(__has_attribute)
#if __has_attribute(fallthrough)
#define FALLTHROUGH  __attribute__((fallthrough));
#else
#define FALLTHROUGH
#endif
#else
#define FALLTHROUGH
#endif

/*
 * Define BITS_IN_JSAMPLE as either
 *   8   for 8-bit sample values (the usual setting)
 *   12  for 12-bit sample values
 * Only 8 and 12 are legal data precisions for lossy JPEG according to the
 * JPEG standard, and the IJG code does not support anything else!
 */

#ifndef BITS_IN_JSAMPLE
#define BITS_IN_JSAMPLE  8      /* use 8 or 12 */
#endif

#undef C_ARITH_CODING_SUPPORTED
#undef D_ARITH_CODING_SUPPORTED
#undef WITH_SIMD

#if BITS_IN_JSAMPLE == 8

/* Support arithmetic encoding */
#define C_ARITH_CODING_SUPPORTED 1

/* Support arithmetic decoding */
#define D_ARITH_CODING_SUPPORTED 1

/* Use accelerated SIMD routines. */
/* #undef WITH_SIMD */

#define SIMD_ARCHITECTURE  NONE

/* #undef WITH_SIMDE */

#endif

/* #undef WITH_PROFILE */
