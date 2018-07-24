#!/bin/sh

objroot=$1

cat <<EOF
#ifndef JEMALLOC_H_
#define JEMALLOC_H_

#ifdef _AIX

/* Force symbol mangling on AIX */
#define JEMALLOC_MANGLE	1

/*
 * Define global variable _malloc_user_defined_name so AIX runtime
 * linker load lubjemalloc_wrapper archive as custom memory subsystem
 */
#ifdef __cplusplus
#define JEMALLOC_LOAD_WRAPPER \
	extern "C" const char *_malloc_user_defined_name; \
	const char *_malloc_user_defined_name="libjemalloc_wrapper.a";
#else
#define JEMALLOC_LOAD_WRAPPER const char *_malloc_user_defined_name = "libjemalloc_wrapper.a";
#endif

#else

#define JEMALLOC_LOAD_WRAPPER

#endif

#ifdef __cplusplus
extern "C" {
#endif

EOF

for hdr in jemalloc_defs.h jemalloc_rename.h jemalloc_macros.h \
           jemalloc_protos.h jemalloc_typedefs.h jemalloc_mangle.h ; do
  cat "${objroot}include/jemalloc/${hdr}" \
      | grep -v 'Generated from .* by configure\.' \
      | sed -e 's/ $//g'
  echo
done

cat <<EOF

#ifdef __cplusplus
}
#endif
#endif /* JEMALLOC_H_ */
EOF
