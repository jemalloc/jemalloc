#include <mutex>
#include <new>

// Full import of jemalloc_internal.h contains non-C++ keywords
// Instead define just enough to get the cpp stubs to compile

#define	JEMALLOC_NO_DEMANGLE
#ifdef JEMALLOC_JET
#  define JEMALLOC_N(n) jet_##n
#  include "jemalloc/internal/public_namespace.h"
#  define JEMALLOC_NO_RENAME
#  include "jemalloc/jemalloc.h"
#  undef JEMALLOC_NO_RENAME
#else
#  define JEMALLOC_N(n) je_##n
#  include "jemalloc/jemalloc.h"
#endif
#include "jemalloc/internal/private_namespace.h"
//#include "jemalloc/jemalloc.h"
#include "jemalloc/internal/jemalloc_internal_macros.h"
#include  "jemalloc/internal/jemalloc_internal_defs.h"
#define JEMALLOC_H_TYPES
#include "jemalloc/internal/util.h"

// All operators in this file are exported.

// Possibly alias hidden versions of malloc and sdallocx to avoid
// an extra plt thunk?
//
// extern __typeof (sdallocx) sdallocx_int
//  __attribute ((alias ("sdallocx"),
//		visibility ("hidden")));
//
// .. but needs to work with jemalloc namespaces

void* operator new(std::size_t size);
void* operator new[](std::size_t size);
void* operator new(std::size_t size, const std::nothrow_t&) noexcept;
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept;
void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;
void operator delete(void* ptr, const std::nothrow_t&) noexcept;
void operator delete[](void* ptr, const std::nothrow_t&) noexcept;

#if __cpp_sized_deallocation >= 201309
/* C++14's sized-delete operators */
void operator delete(void* ptr, std::size_t size) noexcept;
void operator delete[](void* ptr, std::size_t size) noexcept;
#endif


template <bool IsNoExcept>
JEMALLOC_INLINE
void* newImpl(std::size_t size) noexcept(IsNoExcept) {
  void* ptr = je_malloc(size);
  if (likely(ptr != nullptr)) {
    return ptr;
  }

  while (ptr == nullptr) {
    std::new_handler handler;
    // GCC-4.8 and clang 4.0 do not have std::get_new_handler.
    {
      static std::mutex mtx;
      std::lock_guard<std::mutex> lock(mtx);

      handler = std::set_new_handler(nullptr);
      std::set_new_handler(handler);
    }
    if (handler == nullptr) {
      break;
    }

    try {
      handler();
    } catch (const std::bad_alloc&) {
      break;
    }

    ptr = je_malloc(size);
  }

  if (ptr == nullptr && !IsNoExcept) {
    std::__throw_bad_alloc();
  }
  return ptr;
}

void* operator new(std::size_t size) {
  return newImpl<false>(size);
}

void* operator new[](std::size_t size) {
  return newImpl<false>(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return newImpl<true>(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return newImpl<true>(size);
}

void operator delete(void* ptr) noexcept { free(ptr); }

void operator delete[](void* ptr) noexcept { free(ptr); }

void operator delete(void* ptr, const std::nothrow_t&) noexcept { free(ptr); }

void operator delete[](void* ptr, const std::nothrow_t&) noexcept { free(ptr); }

#if __cpp_sized_deallocation >= 201309

void operator delete(void* ptr, std::size_t size) noexcept {
  je_sdallocx(ptr, size, /*flags=*/0);
}

void operator delete[](void* ptr, std::size_t size) noexcept {
  je_sdallocx(ptr, size, /*flags=*/0);
}

#endif  // __cpp_sized_deallocation
