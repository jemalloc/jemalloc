#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/prof_sys.h"

#if defined (__linux__) && defined(JE_HAVE_GETTID)

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h> // strtoul
#include <string.h>
#include <unistd.h>

static int prof_mapping_containing_addr(
    uintptr_t addr,
    const char* maps_path,
    uintptr_t* mm_start,
    uintptr_t* mm_end) {
  int ret = ENOENT; // not found
  *mm_start = *mm_end = 0;

  // Each line of /proc/<pid>/maps is:
  // <start>-<end> <perms> <offset> <dev> <inode> <pathname>
  //
  // The fields we care about are always within the first 34 characters so
  // as long as `buf` contains the start of a mapping line it can always be
  // parsed.
  static const int kMappingFieldsWidth = 34;

  int fd = -1;
  char buf[4096];
  ssize_t remaining = 0; // actual number of bytes read to buf
  char* line = NULL;

  while (1) {
    if (fd < 0) {
      // case 0: initial open of maps file
      fd = malloc_open(maps_path, O_RDONLY);
      if (fd < 0) {
        return errno;
      }

      remaining = malloc_read_fd(fd, buf, sizeof(buf));
      if (remaining <= 0) {
        break;
      }
      line = buf;
    } else if (line == NULL) {
      // case 1: no newline found in buf
      remaining = malloc_read_fd(fd, buf, sizeof(buf));
      if (remaining <= 0) {
        break;
      }
      line = memchr(buf, '\n', remaining);
      if (line != NULL) {
        line++; // advance to character after newline
        remaining -= (line - buf);
      }
    } else if (line != NULL && remaining < kMappingFieldsWidth) {
      // case 2: found newline but insufficient characters remaining in buf

      // fd currently points to the character immediately after the last
      // character in buf. Seek fd to the character after the newline.
      if (malloc_lseek(fd, -remaining, SEEK_CUR) == -1) {
        ret = errno;
        break;
      }

      remaining = malloc_read_fd(fd, buf, sizeof(buf));
      if (remaining <= 0) {
        break;
      }
      line = buf;
    } else {
      // case 3: found newline and sufficient characters to parse

      // parse <start>-<end>
      char* tmp = line;
      uintptr_t start_addr = strtoul(tmp, &tmp, 16);
      if (addr >= start_addr) {
        tmp++; // advance to character after '-'
        uintptr_t end_addr = strtoul(tmp, &tmp, 16);
        if (addr < end_addr) {
          *mm_start = start_addr;
          *mm_end = end_addr;
          ret = 0;
          break;
        }
      }

      // Advance to character after next newline in the current buf.
      char* prev_line = line;
      line = memchr(line, '\n', remaining);
      if (line != NULL) {
        line++; // advance to character after newline
        remaining -= (line - prev_line);
      }
    }
  }

  malloc_close(fd);
  return ret;
}

static uintptr_t prof_main_thread_stack_start(const char* stat_path) {
  uintptr_t stack_start = 0;

  int fd = malloc_open(stat_path, O_RDONLY);
  if (fd < 0) {
    return 0;
  }

  char buf[512];
  ssize_t n = malloc_read_fd(fd, buf, sizeof(buf) - 1);
  if (n >= 0) {
    buf[n] = '\0';
    if (sscanf(
            buf,
            "%*d (%*[^)]) %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %*u %*u %*d %*u %*u %*u %"FMTuPTR,
            &stack_start) != 1) {
    }
  }
  malloc_close(fd);
  return stack_start;
}

uintptr_t prof_thread_stack_start(uintptr_t stack_end) {
  pid_t pid = getpid();
  pid_t tid = gettid();
  if (pid == tid) {
    char stat_path[32]; // "/proc/<pid>/stat"
    malloc_snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
    return prof_main_thread_stack_start(stat_path);
  } else {
    // NOTE: Prior to kernel 4.5 an entry for every thread stack was included in
    // /proc/<pid>/maps as [STACK:<tid>]. Starting with kernel 4.5 only the main
    // thread stack remains as the [stack] mapping. For other thread stacks the
    // mapping is still visible in /proc/<pid>/task/<tid>/maps (though not
    // labeled as [STACK:tid]).
    // https://lists.ubuntu.com/archives/kernel-team/2016-March/074681.html
    char maps_path[64]; // "/proc/<pid>/task/<tid>/maps"
    malloc_snprintf(maps_path, sizeof(maps_path), "/proc/%d/task/%d/maps", pid, tid);

    uintptr_t mm_start, mm_end;
    if (prof_mapping_containing_addr(
            stack_end, maps_path, &mm_start, &mm_end) != 0) {
      return 0;
    }
    return mm_end;
  }
}

#else

uintptr_t prof_thread_stack_start(UNUSED uintptr_t stack_end) {
  return 0;
}

#endif // __linux__
