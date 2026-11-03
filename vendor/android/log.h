// Minimal <android/log.h> shim for non-Android (desktop) builds.
//
// The cleanroom library's logging TUs (legacy_decoder.cpp, jsc.cpp,
// varicode.cpp, engine.cpp) call the Android logging API directly:
//     __android_log_print(ANDROID_LOG_*, "JS8Decoder"/..., "...");
// On Android the real NDK provides <android/log.h>. On desktop builds that
// header is unavailable, so this shim supplies a functional fallback that writes
// level-prefixed messages to stderr. When compiled for Android (__ANDROID__)
// this file defers to the platform's real header via #include_next, so the
// NDK behaviour is preserved exactly.
#pragma once

#if defined(__ANDROID__)
  // Build for Android: use the real NDK logging header, not this shim.
  #include_next <android/log.h>
#else

#include <stdarg.h>
#include <stdio.h>

/* JS8 priority constants mirror the Android NDK ordering. */
enum {
  ANDROID_LOG_SILENT  = 0,
  ANDROID_LOG_ASSERT  = 1,
  ANDROID_LOG_ERROR   = 2,
  ANDROID_LOG_WARN    = 3,
  ANDROID_LOG_INFO    = 4,
  ANDROID_LOG_DEBUG   = 5,
  ANDROID_LOG_VERBOSE = 6,
};

/* desktop fallback: emit "<LEVEL> <tag>: <msg>" to stderr */
static inline void __android_log_print(int priority, const char* tag,
                                       const char* fmt, ...) {
  const char* level;
  switch (priority) {
    case ANDROID_LOG_ERROR:  level = "ERROR";  break;
    case ANDROID_LOG_WARN:   level = "WARN";   break;
    case ANDROID_LOG_INFO:   level = "INFO";   break;
    case ANDROID_LOG_DEBUG:  level = "DEBUG";  break;
    case ANDROID_LOG_VERBOSE:level = "VERBOSE";break;
    case ANDROID_LOG_ASSERT: level = "ASSERT"; break;
    default: level = "INFO"; break;
  }
  fprintf(stderr, "%s %s: ", level, tag ? tag : "JS8");
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

#endif  /* __ANDROID__ */
