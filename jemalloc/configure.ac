dnl Process this file with autoconf to produce a configure script.
AC_INIT([Makefile.in])

dnl ============================================================================
dnl Custom macro definitions.

dnl JE_CFLAGS_APPEND(cflag)
AC_DEFUN([JE_CFLAGS_APPEND],
[
AC_MSG_CHECKING([whether compiler supports $1])
TCFLAGS="${CFLAGS}"
if test "x${CFLAGS}" = "x" ; then
  CFLAGS="$1"
else
  CFLAGS="${CFLAGS} $1"
fi
AC_RUN_IFELSE([AC_LANG_PROGRAM(
[[
]], [[
    return 0;
]])],
              AC_MSG_RESULT([yes]),
              AC_MSG_RESULT([no])
              [CFLAGS="${TCFLAGS}"]
)
])

dnl JE_COMPILABLE(label, hcode, mcode, rvar)
AC_DEFUN([JE_COMPILABLE],
[
AC_MSG_CHECKING([whether $1 is compilable])
AC_RUN_IFELSE([AC_LANG_PROGRAM(
[$2], [$3])],
              AC_MSG_RESULT([yes])
              [$4="yes"],
              AC_MSG_RESULT([no])
              [$4="no"]
)
])

dnl ============================================================================

srcroot=$srcdir
if test "x${srcroot}" = "x." ; then
  srcroot=""
else
  srcroot="${srcroot}/"
fi
AC_SUBST([srcroot])
abs_srcroot="`cd \"${srcdir}\"; pwd`/"
AC_SUBST([abs_srcroot])

objroot=""
AC_SUBST([objroot])
abs_objroot="`pwd`/"
AC_SUBST([abs_objroot])

dnl Munge install path variables.
if test "x$prefix" = "xNONE" ; then
  prefix="/usr/local"
fi
if test "x$exec_prefix" = "xNONE" ; then
  exec_prefix=$prefix
fi
PREFIX=$prefix
AC_SUBST([PREFIX])
BINDIR=`eval echo $bindir`
BINDIR=`eval echo $BINDIR`
AC_SUBST([BINDIR])
INCLUDEDIR=`eval echo $includedir`
INCLUDEDIR=`eval echo $INCLUDEDIR`
AC_SUBST([INCLUDEDIR])
LIBDIR=`eval echo $libdir`
LIBDIR=`eval echo $LIBDIR`
AC_SUBST([LIBDIR])
DATADIR=`eval echo $datadir`
DATADIR=`eval echo $DATADIR`
AC_SUBST([DATADIR])
MANDIR=`eval echo $mandir`
MANDIR=`eval echo $MANDIR`
AC_SUBST([MANDIR])

dnl If CFLAGS isn't defined, set CFLAGS to something reasonable.  Otherwise,
dnl just prevent autoconf from molesting CFLAGS.
CFLAGS=$CFLAGS
AC_PROG_CC
if test "x$CFLAGS" = "x" ; then
  no_CFLAGS="yes"
  JE_CFLAGS_APPEND([-std=gnu99])
  JE_CFLAGS_APPEND([-Wall])
  JE_CFLAGS_APPEND([-pipe])
  JE_CFLAGS_APPEND([-g3])
  JE_CFLAGS_APPEND([-march=native])
fi
dnl Append EXTRA_CFLAGS to CFLAGS, if defined.
if test "x$EXTRA_CFLAGS" != "x" ; then
  JE_CFLAGS_APPEND([$EXTRA_CFLAGS])
fi
AC_PROG_CPP

AC_CHECK_SIZEOF([void *])
if test "x${ac_cv_sizeof_void_p}" = "x8" ; then
  LG_SIZEOF_PTR=3
elif test "x${ac_cv_sizeof_void_p}" = "x4" ; then
  LG_SIZEOF_PTR=2
else
  AC_MSG_ERROR([Unsupported pointer size: ${ac_cv_sizeof_void_p}])
fi
AC_DEFINE_UNQUOTED([LG_SIZEOF_PTR], [$LG_SIZEOF_PTR])

AC_CHECK_SIZEOF([int])
if test "x${ac_cv_sizeof_int}" = "x8" ; then
  LG_SIZEOF_INT=3
elif test "x${ac_cv_sizeof_int}" = "x4" ; then
  LG_SIZEOF_INT=2
else
  AC_MSG_ERROR([Unsupported int size: ${ac_cv_sizeof_int}])
fi
AC_DEFINE_UNQUOTED([LG_SIZEOF_INT], [$LG_SIZEOF_INT])

AC_CANONICAL_HOST
dnl CPU-specific settings.
CPU_SPINWAIT=""
case "${host_cpu}" in
  i[[345]]86)
	;;
  i686)
	JE_COMPILABLE([__asm__], [], [[__asm__ volatile("pause"); return 0;]],
	              [asm])
	if test "x${asm}" = "xyes" ; then
	    CPU_SPINWAIT='__asm__ volatile("pause")'
	fi
	;;
  x86_64)
	JE_COMPILABLE([__asm__ syntax], [],
	              [[__asm__ volatile("pause"); return 0;]], [asm])
	if test "x${asm}" = "xyes" ; then
	    CPU_SPINWAIT='__asm__ volatile("pause")'
	fi
	;;
  *)
	;;
esac
AC_DEFINE_UNQUOTED([CPU_SPINWAIT], [$CPU_SPINWAIT])

JE_COMPILABLE([__attribute__ syntax],
              [static __attribute__((unused)) void foo(void){}],
              [],
              [attribute])
if test "x${attribute}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_HAVE_ATTR], [ ])
  if test "x$GCC" = "xyes" ; then
    JE_CFLAGS_APPEND([-fvisibility=internal])
  fi
fi

dnl Platform-specific settings.  abi and RPATH can probably be determined
dnl programmatically, but doing so is error-prone, which makes it generally
dnl not worth the trouble.
dnl 
dnl Define cpp macros in CPPFLAGS, rather than doing AC_DEFINE(macro), since the
dnl definitions need to be seen before any headers are included, which is a pain
dnl to make happen otherwise.
case "${host}" in
  *-*-darwin*)
	CFLAGS="$CFLAGS -fno-common -no-cpp-precomp"
	abi="macho"
	RPATH=""
	;;
  *-*-freebsd*)
	CFLAGS="$CFLAGS"
	abi="elf"
	RPATH="-Wl,-rpath,"
	;;
  *-*-linux*)
	CFLAGS="$CFLAGS"
	CPPFLAGS="$CPPFLAGS -D_GNU_SOURCE"
	abi="elf"
	RPATH="-Wl,-rpath,"
	;;
  *-*-netbsd*)
	AC_MSG_CHECKING([ABI])
        AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
[[#ifdef __ELF__
/* ELF */
#else
#error aout
#endif
]])],
                          [CFLAGS="$CFLAGS"; abi="elf"],
                          [abi="aout"])
	AC_MSG_RESULT([$abi])
	RPATH="-Wl,-rpath,"
	;;
  *-*-solaris2*)
	CFLAGS="$CFLAGS"
	abi="elf"
	RPATH="-Wl,-R,"
	dnl Solaris needs this for sigwait().
	CPPFLAGS="$CPPFLAGS -D_POSIX_PTHREAD_SEMANTICS"
	LIBS="$LIBS -lposix4 -lsocket -lnsl"
	;;
  *)
	AC_MSG_RESULT([Unsupported operating system: ${host}])
	abi="elf"
	RPATH="-Wl,-rpath,"
	;;
esac
AC_SUBST([abi])
AC_SUBST([RPATH])

dnl Support optional additions to rpath.
AC_ARG_WITH([rpath],
  [AS_HELP_STRING([--with-rpath=<rpath>], [Colon-separated rpath (ELF systems only)])],
if test "x$with_rpath" = "xno" ; then
  RPATH_EXTRA=
else
  RPATH_EXTRA="`echo $with_rpath | tr \":\" \" \"`"
fi,
  RPATH_EXTRA=
)
AC_SUBST([RPATH_EXTRA])

dnl Disable rules that do automatic regeneration of configure output by default.
AC_ARG_ENABLE([autogen],
  [AS_HELP_STRING([--enable-autogen], [Automatically regenerate configure output])],
if test "x$enable_autogen" = "xno" ; then
  enable_autogen="0"
else
  enable_autogen="1"
fi
,
enable_autogen="0"
)
AC_SUBST([enable_autogen])

AC_PROG_INSTALL
AC_PROG_RANLIB
AC_PATH_PROG([AR], [ar], , [$PATH])
AC_PATH_PROG([LD], [ld], , [$PATH])
AC_PATH_PROG([AUTOCONF], [autoconf], , [$PATH])

dnl Do not prefix public APIs by default.
AC_ARG_WITH([jemalloc_prefix],
  [AS_HELP_STRING([--with-jemalloc-prefix=<prefix>], [Prefix to prepend to all public APIs])],
  [JEMALLOC_PREFIX="$with_jemalloc_prefix"],
  [JEMALLOC_PREFIX=]
)
if test "x$JEMALLOC_PREFIX" != "x" ; then
  AC_DEFINE([JEMALLOC_PREFIX], [ ])
  jemalloc_prefix="$JEMALLOC_PREFIX"
  AC_SUBST([jemalloc_prefix])
  AC_DEFINE_UNQUOTED([JEMALLOC_P(string_that_no_one_should_want_to_use_as_a_jemalloc_API_prefix)], [${JEMALLOC_PREFIX}##string_that_no_one_should_want_to_use_as_a_jemalloc_API_prefix])
fi

dnl Do not add suffix to installed files by default.
AC_ARG_WITH([install_suffix],
  [AS_HELP_STRING([--with-install-suffix=<suffix>], [Suffix to append to all installed files])],
  [INSTALL_SUFFIX="$with_install_suffix"],
  [INSTALL_SUFFIX=]
)
install_suffix="$INSTALL_SUFFIX"
AC_SUBST([install_suffix])

cfgoutputs_in="Makefile doc/jemalloc.3.in"
cfgoutputs_in="${cfgoutputs_in} include/jemalloc/jemalloc.h.in"
cfgoutputs_in="${cfgoutputs_in} include/jemalloc/internal/jemalloc_internal.h.in"

cfgoutputs_out="Makefile doc/jemalloc${install_suffix}.3"
cfgoutputs_out="${cfgoutputs_out} include/jemalloc/jemalloc${install_suffix}.h"
cfgoutputs_out="${cfgoutputs_out} include/jemalloc/internal/jemalloc_internal.h"

cfgoutputs_tup="Makefile doc/jemalloc${install_suffix}.3:doc/jemalloc.3.in"
cfgoutputs_tup="${cfgoutputs_tup} include/jemalloc/jemalloc${install_suffix}.h:include/jemalloc/jemalloc.h.in"
cfgoutputs_tup="${cfgoutputs_tup} include/jemalloc/internal/jemalloc_internal.h"

cfghdrs_in="include/jemalloc/jemalloc_defs.h.in"

cfghdrs_out="include/jemalloc/jemalloc_defs${install_suffix}.h"

cfghdrs_tup="include/jemalloc/jemalloc_defs${install_suffix}.h:include/jemalloc/jemalloc_defs.h.in"

dnl Do not compile with debugging by default.
AC_ARG_ENABLE([debug],
  [AS_HELP_STRING([--enable-debug], [Build debugging code])],
[if test "x$enable_debug" = "xno" ; then
  enable_debug="0"
else
  enable_debug="1"
fi
],
[enable_debug="0"]
)
if test "x$enable_debug" = "x1" ; then
  AC_DEFINE([JEMALLOC_DEBUG], [ ])
fi
AC_SUBST([enable_debug])

dnl Only optimize if not debugging.
if test "x$enable_debug" = "x0" -a "x$no_CFLAGS" = "xyes" ; then
  dnl Make sure that an optimization flag was not specified in EXTRA_CFLAGS.
  optimize="no"
  echo "$EXTRA_CFLAGS" | grep "\-O" >/dev/null || optimize="yes"
  if test "x${optimize}" = "xyes" ; then
    if test "x$GCC" = "xyes" ; then
      JE_CFLAGS_APPEND([-O3])
      JE_CFLAGS_APPEND([-funroll-loops])
      JE_CFLAGS_APPEND([-fomit-frame-pointer])
    else
      JE_CFLAGS_APPEND([-O])
    fi
  fi
fi

dnl Do not enable statistics calculation by default.
AC_ARG_ENABLE([stats],
  [AS_HELP_STRING([--enable-stats], [Enable statistics calculation/reporting])],
[if test "x$enable_stats" = "xno" ; then
  enable_stats="0"
else
  enable_stats="1"
fi
],
[enable_stats="0"]
)
if test "x$enable_stats" = "x1" ; then
  AC_DEFINE([JEMALLOC_STATS], [ ])
fi
AC_SUBST([enable_stats])
if test "x$enable_stats" = "x0" ; then
  roff_stats=".\\\" "
else
  roff_stats=""
fi
AC_SUBST([roff_stats])

dnl Do not enable profiling by default.
AC_ARG_ENABLE([prof],
  [AS_HELP_STRING([--enable-prof], [Enable allocation profiling])],
[if test "x$enable_prof" = "xno" ; then
  enable_prof="0"
else
  enable_prof="1"
fi
],
[enable_prof="0"]
)
AC_ARG_ENABLE([prof-libgcc],
  [AS_HELP_STRING([--disable-prof-libgcc],
  [Do not use libgcc for backtracing])],
[if test "x$enable_prof_libgcc" = "xno" ; then
  enable_prof_libgcc="0"
else
  enable_prof_libgcc="1"
fi
],
[enable_prof_libgcc="1"]
)
AC_ARG_ENABLE([prof-libunwind],
  [AS_HELP_STRING([--enable-prof-libunwind], [Use libunwind for backtracing])],
[if test "x$enable_prof_libunwind" = "xno" ; then
  enable_prof_libunwind="0"
else
  enable_prof_libunwind="1"
fi
],
[enable_prof_libunwind="0"]
)
AC_ARG_WITH([static_libunwind],
  [AS_HELP_STRING([--with-static-libunwind=<libunwind.a>],
  [Path to static libunwind library; use rather than dynamically linking])],
if test "x$with_static_libunwind" = "xno" ; then
  LUNWIND="-lunwind"
else
  if test ! -f "$with_static_libunwind" ; then
    AC_MSG_ERROR([Static libunwind not found: $with_static_libunwind])
  fi
  LUNWIND="$with_static_libunwind"
fi,
  LUNWIND="-lunwind"
)
dnl Finish prof-related definitions below, once TLS configuration is done.

dnl Enable tiny allocations by default.
AC_ARG_ENABLE([tiny],
  [AS_HELP_STRING([--disable-tiny], [Disable tiny (sub-quantum) allocations])],
[if test "x$enable_tiny" = "xno" ; then
  enable_tiny="0"
else
  enable_tiny="1"
fi
],
[enable_tiny="1"]
)
if test "x$enable_tiny" = "x1" ; then
  AC_DEFINE([JEMALLOC_TINY], [ ])
fi
AC_SUBST([enable_tiny])
if test "x$enable_tiny" = "x0" ; then
  roff_tiny=".\\\" "
  roff_no_tiny=""
else
  roff_tiny=""
  roff_no_tiny=".\\\" "
fi
AC_SUBST([roff_tiny])
AC_SUBST([roff_no_tiny])

dnl Enable thread-specific caching by default.
AC_ARG_ENABLE([tcache],
  [AS_HELP_STRING([--disable-tcache], [Disable per thread caches])],
[if test "x$enable_tcache" = "xno" ; then
  enable_tcache="0"
else
  enable_tcache="1"
fi
],
[enable_tcache="1"]
)
dnl Finish tcache-related definitions below, once TLS configuration is done.

dnl Do not enable mmap()ped swap files by default.
AC_ARG_ENABLE([swap],
  [AS_HELP_STRING([--enable-swap], [Enable mmap()ped swap files])],
[if test "x$enable_swap" = "xno" ; then
  enable_swap="0"
else
  enable_swap="1"
fi
],
[enable_swap="0"]
)
if test "x$enable_swap" = "x1" ; then
  AC_DEFINE([JEMALLOC_SWAP], [ ])
fi
AC_SUBST([enable_swap])
if test "x$enable_swap" = "x0" ; then
  roff_swap=".\\\" "
else
  roff_swap=""
fi
AC_SUBST([roff_swap])

dnl Do not enable allocation from DSS by default.
AC_ARG_ENABLE([dss],
  [AS_HELP_STRING([--enable-dss], [Enable allocation from DSS])],
[if test "x$enable_dss" = "xno" ; then
  enable_dss="0"
else
  enable_dss="1"
fi
],
[enable_dss="0"]
)
if test "x$enable_dss" = "x1" ; then
  AC_DEFINE([JEMALLOC_DSS], [ ])
fi
AC_SUBST([enable_dss])
if test "x$enable_dss" = "x0" ; then
  roff_dss=".\\\" "
else
  roff_dss=""
fi
AC_SUBST([roff_dss])

dnl Do not support the junk/zero filling option by default.
AC_ARG_ENABLE([fill],
  [AS_HELP_STRING([--enable-fill], [Support junk/zero filling option])],
[if test "x$enable_fill" = "xno" ; then
  enable_fill="0"
else
  enable_fill="1"
fi
],
[enable_fill="0"]
)
if test "x$enable_fill" = "x1" ; then
  AC_DEFINE([JEMALLOC_FILL], [ ])
fi
AC_SUBST([enable_fill])
if test "x$enable_fill" = "x0" ; then
  roff_fill=".\\\" "
else
  roff_fill=""
fi
AC_SUBST([roff_fill])

dnl Do not support the xmalloc option by default.
AC_ARG_ENABLE([xmalloc],
  [AS_HELP_STRING([--enable-xmalloc], [Support xmalloc option])],
[if test "x$enable_xmalloc" = "xno" ; then
  enable_xmalloc="0"
else
  enable_xmalloc="1"
fi
],
[enable_xmalloc="0"]
)
if test "x$enable_xmalloc" = "x1" ; then
  AC_DEFINE([JEMALLOC_XMALLOC], [ ])
fi
AC_SUBST([enable_xmalloc])
if test "x$enable_xmalloc" = "x0" ; then
  roff_xmalloc=".\\\" "
else
  roff_xmalloc=""
fi
AC_SUBST([roff_xmalloc])

dnl Do not support the SYSV option by default.
AC_ARG_ENABLE([sysv],
  [AS_HELP_STRING([--enable-sysv], [Support SYSV semantics option])],
[if test "x$enable_sysv" = "xno" ; then
  enable_sysv="0"
else
  enable_sysv="1"
fi
],
[enable_sysv="0"]
)
if test "x$enable_sysv" = "x1" ; then
  AC_DEFINE([JEMALLOC_SYSV], [ ])
fi
AC_SUBST([enable_sysv])
if test "x$enable_sysv" = "x0" ; then
  roff_sysv=".\\\" "
else
  roff_sysv=""
fi
AC_SUBST([roff_sysv])

dnl Do not determine page shift at run time by default.
AC_ARG_ENABLE([dynamic_page_shift],
  [AS_HELP_STRING([--enable-dynamic-page-shift],
  [Determine page size at run time (don't trust configure result)])],
[if test "x$enable_dynamic_page_shift" = "xno" ; then
  enable_dynamic_page_shift="0"
else
  enable_dynamic_page_shift="1"
fi
],
[enable_dynamic_page_shift="0"]
)
if test "x$enable_dynamic_page_shift" = "x1" ; then
  AC_DEFINE([DYNAMIC_PAGE_SHIFT], [ ])
fi
AC_SUBST([enable_dynamic_page_shift])

AC_MSG_CHECKING([STATIC_PAGE_SHIFT])
AC_RUN_IFELSE([AC_LANG_PROGRAM(
[[#include <stdio.h>
#include <unistd.h>
#include <strings.h>
]], [[
    long result;
    FILE *f;

    result = sysconf(_SC_PAGESIZE);
    if (result == -1) {
	return 1;
    }
    f = fopen("conftest.out", "w");
    if (f == NULL) {
	return 1;
    }
    fprintf(f, "%u\n", ffs((int)result) - 1);
    close(f);

    return 0;
]])],
              [STATIC_PAGE_SHIFT=`cat conftest.out`]
              AC_MSG_RESULT([$STATIC_PAGE_SHIFT])
              AC_DEFINE_UNQUOTED([STATIC_PAGE_SHIFT], [$STATIC_PAGE_SHIFT]),
              AC_MSG_RESULT([error]))

dnl ============================================================================
dnl jemalloc configuration.
dnl 

dnl Set VERSION if source directory has an embedded git repository.
if test -d "${srcroot}../.git" ; then
  git describe --long > ${srcroot}VERSION
fi
jemalloc_version=`cat ${srcroot}VERSION`
jemalloc_version_major=`echo ${jemalloc_version} | tr ".g-" " " | awk '{print [$]1}'`
jemalloc_version_minor=`echo ${jemalloc_version} | tr ".g-" " " | awk '{print [$]2}'`
jemalloc_version_bugfix=`echo ${jemalloc_version} | tr ".g-" " " | awk '{print [$]3}'`
jemalloc_version_nrev=`echo ${jemalloc_version} | tr ".g-" " " | awk '{print [$]4}'`
jemalloc_version_gid=`echo ${jemalloc_version} | tr ".g-" " " | awk '{print [$]5}'`
AC_SUBST([jemalloc_version])
AC_SUBST([jemalloc_version_major])
AC_SUBST([jemalloc_version_minor])
AC_SUBST([jemalloc_version_bugfix])
AC_SUBST([jemalloc_version_nrev])
AC_SUBST([jemalloc_version_gid])

dnl ============================================================================
dnl Configure pthreads.

AC_CHECK_HEADERS([pthread.h], , [AC_MSG_ERROR([pthread.h is missing])])
AC_CHECK_LIB([pthread], [pthread_create], [LIBS="$LIBS -lpthread"],
             [AC_MSG_ERROR([libpthread is missing])])

CPPFLAGS="$CPPFLAGS -D_REENTRANT"

dnl Enable lazy locking by default.
AC_ARG_ENABLE([lazy_lock],
  [AS_HELP_STRING([--disable-lazy-lock],
  [Disable lazy locking (always lock, even when single-threaded)])],
[if test "x$enable_lazy_lock" = "xno" ; then
  enable_lazy_lock="0"
else
  enable_lazy_lock="1"
fi
],
[enable_lazy_lock="1"]
)
if test "x$enable_lazy_lock" = "x1" ; then
  AC_CHECK_HEADERS([dlfcn.h], , [AC_MSG_ERROR([dlfcn.h is missing])])
  AC_CHECK_LIB([dl], [dlopen], [LIBS="$LIBS -ldl"],
               [AC_MSG_ERROR([libdl is missing])])
  AC_DEFINE([JEMALLOC_LAZY_LOCK], [ ])
fi
AC_SUBST([enable_lazy_lock])

AC_ARG_ENABLE([tls],
  [AS_HELP_STRING([--disable-tls], [Disable thread-local storage (__thread keyword)])],
if test "x$enable_tls" = "xno" ; then
  enable_tls="0"
else
  enable_tls="1"
fi
,
enable_tls="1"
)
if test "x${enable_tls}" = "x1" ; then
AC_MSG_CHECKING([for TLS])
AC_RUN_IFELSE([AC_LANG_PROGRAM(
[[
    __thread int x;
]], [[
    x = 42;

    return 0;
]])],
              AC_MSG_RESULT([yes]),
              AC_MSG_RESULT([no])
              enable_tls="0")
fi
if test "x${enable_tls}" = "x0" ; then
  AC_DEFINE_UNQUOTED([NO_TLS], [ ])
fi

dnl Finish tcache-related definitions, now that TLS configuration is done.
if test "x$enable_tls" = "x0" ; then
  enable_tcache="0"
fi
if test "x$enable_tcache" = "x1" ; then
  AC_DEFINE([JEMALLOC_TCACHE], [ ])
fi
AC_SUBST([enable_tcache])
if test "x$enable_tcache" = "x0" ; then
  roff_tcache=".\\\" "
  roff_no_tcache=""
else
  roff_tcache=""
  roff_no_tcache=".\\\" "
fi
AC_SUBST([roff_tcache])
AC_SUBST([roff_no_tcache])

dnl Finish prof-related definitions, now that TLS configuration is done.
if test "x$enable_tls" = "x0" ; then
  enable_prof="0"
fi
if test "x$enable_prof" = "x1" ; then
  LIBS="$LIBS -lm"
  AC_DEFINE([JEMALLOC_PROF], [ ])
  if test "x$enable_prof_libunwind" = "x1" ; then
    AC_CHECK_HEADERS([libunwind.h], , [enable_prof_libunwind="0"])
    AC_CHECK_LIB([unwind], [backtrace], [LIBS="$LIBS $LUNWIND"],
                 [enable_prof_libunwind="0"])
    if test "x${enable_prof_libunwind}" = "x1" ; then
      AC_DEFINE([JEMALLOC_PROF_LIBUNWIND], [ ])
    fi
  fi
fi
AC_SUBST([enable_prof])
if test "x$enable_prof" = "x0" ; then
  roff_prof=".\\\" "
  roff_no_prof=""
else
  roff_prof=""
  roff_no_prof=".\\\" "
fi
AC_SUBST([roff_prof])
AC_SUBST([roff_no_prof])

dnl If libunwind isn't enabled, try to use libgcc rather than gcc intrinsics
dnl for backtracing.
if test "x$enable_prof" = "x1" -a "x$enable_prof_libunwind" = "x0" \
 -a "x$GCC" = "xyes" -a "x$enable_prof_libgcc" = "x1" ; then
  enable_prof_libgcc="1"
  AC_CHECK_HEADERS([unwind.h], , [enable_prof_libgcc="0"])
  AC_CHECK_LIB([gcc], [_Unwind_Backtrace], [LIBS="$LIBS -lgcc"], [enable_prof_libgcc="0"])
  if test "x${enable_prof_libgcc}" = "x1" ; then
    AC_DEFINE([JEMALLOC_PROF_LIBGCC], [ ])
  fi
fi

dnl ============================================================================
dnl Check for typedefs, structures, and compiler characteristics.
AC_HEADER_STDBOOL

dnl Process .in files.
AC_SUBST([cfghdrs_in])
AC_SUBST([cfghdrs_out])
AC_CONFIG_HEADERS([$cfghdrs_tup cfghdrs.stamp])

dnl ============================================================================
dnl Generate outputs.
AC_CONFIG_FILES([$cfgoutputs_tup cfgoutputs.stamp])
AC_SUBST([cfgoutputs_in])
AC_SUBST([cfgoutputs_out])
AC_OUTPUT

dnl ============================================================================
dnl Print out the results of configuration.
AC_MSG_RESULT([===============================================================================])
AC_MSG_RESULT([jemalloc version   : $jemalloc_version])
AC_MSG_RESULT([])
AC_MSG_RESULT([CC                 : ${CC}])
AC_MSG_RESULT([CPPFLAGS           : ${CPPFLAGS}])
AC_MSG_RESULT([CFLAGS             : ${CFLAGS}])
AC_MSG_RESULT([LDFLAGS            : ${LDFLAGS}])
AC_MSG_RESULT([LIBS               : ${LIBS}])
AC_MSG_RESULT([RPATH_EXTRA        : ${RPATH_EXTRA}])
AC_MSG_RESULT([])
AC_MSG_RESULT([PREFIX             : ${PREFIX}])
AC_MSG_RESULT([BINDIR             : ${BINDIR}])
AC_MSG_RESULT([INCLUDEDIR         : ${INCLUDEDIR}])
AC_MSG_RESULT([LIBDIR             : ${LIBDIR}])
AC_MSG_RESULT([DATADIR            : ${DATADIR}])
AC_MSG_RESULT([MANDIR             : ${MANDIR}])
AC_MSG_RESULT([])
AC_MSG_RESULT([srcroot            : ${srcroot}])
AC_MSG_RESULT([abs_srcroot        : ${abs_srcroot}])
AC_MSG_RESULT([objroot            : ${objroot}])
AC_MSG_RESULT([abs_objroot        : ${abs_objroot}])
AC_MSG_RESULT([])
AC_MSG_RESULT([JEMALLOC_PREFIX    : ${JEMALLOC_PREFIX}])
AC_MSG_RESULT([install_suffix     : ${install_suffix}])
AC_MSG_RESULT([autogen            : ${enable_autogen}])
AC_MSG_RESULT([debug              : ${enable_debug}])
AC_MSG_RESULT([stats              : ${enable_stats}])
AC_MSG_RESULT([prof               : ${enable_prof}])
AC_MSG_RESULT([prof-libunwind     : ${enable_prof_libunwind}])
AC_MSG_RESULT([tiny               : ${enable_tiny}])
AC_MSG_RESULT([tcache             : ${enable_tcache}])
AC_MSG_RESULT([fill               : ${enable_fill}])
AC_MSG_RESULT([xmalloc            : ${enable_xmalloc}])
AC_MSG_RESULT([sysv               : ${enable_sysv}])
AC_MSG_RESULT([swap               : ${enable_swap}])
AC_MSG_RESULT([dss                : ${enable_dss}])
AC_MSG_RESULT([dynamic_page_shift : ${enable_dynamic_page_shift}])
AC_MSG_RESULT([lazy_lock          : ${enable_lazy_lock}])
AC_MSG_RESULT([===============================================================================])
