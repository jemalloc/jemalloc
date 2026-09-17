#ifndef JEMALLOC_INTERNAL_OS_WINDOWS_FMT_H
#define JEMALLOC_INTERNAL_OS_WINDOWS_FMT_H

#ifdef _WIN64
#  define FMT64_PREFIX "ll"
#  define FMTPTR_PREFIX "ll"
#else
#  define FMT64_PREFIX "ll"
#  define FMTPTR_PREFIX ""
#endif
#define FMTd32 "d"
#define FMTu32 "u"
#define FMTx32 "x"
#define FMTd64 FMT64_PREFIX "d"
#define FMTu64 FMT64_PREFIX "u"
#define FMTx64 FMT64_PREFIX "x"
#define FMTdPTR FMTPTR_PREFIX "d"
#define FMTuPTR FMTPTR_PREFIX "u"
#define FMTxPTR FMTPTR_PREFIX "x"

#endif /* JEMALLOC_INTERNAL_OS_WINDOWS_FMT_H */
