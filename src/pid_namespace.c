#include "jemalloc/internal/pid_namespace_externs.h"

#ifdef JEMALLOC_PID_NAMESPACE
#include <sys/stat.h>

static const char* PID_NAMESPACE_PATH = "/proc/self/ns/pid";
static const char* PID_NAMESPACE_SEP = "pid:[";

ssize_t pid_namespace() {
  char buf[PATH_MAX + 1];
  ssize_t linklen = readlink(PID_NAMESPACE_PATH, buf, PATH_MAX);
  if (linklen == -1) {
    return 0;
  }
  // Trim the trailing "]"
  buf[linklen-1] = '\0';
  char* index = strtok(buf, PID_NAMESPACE_SEP);
  return atol(index);
}
#endif
