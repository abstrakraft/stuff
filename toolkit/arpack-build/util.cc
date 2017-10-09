
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>

extern "C" int arscnd_(float *t) {
  struct timeval tv;
  gettimeofday(&tv, 0);
  *t = tv.tv_sec + 1e-6*tv.tv_usec;
  return 0;
}

// Called by runtime checking code generated by -fcheck.
extern "C" void _gfortran_runtime_error_at(const char *where,
                                           const char *msg, ...) {
  fflush(stdout);
  fflush(stderr);
  fprintf(stderr, "Fortran error: %s: ", where);
  va_list ap;
  va_start(ap, msg);
  vfprintf(stderr, msg, ap);
  fprintf(stderr, "\n");
  fflush(stderr);
  abort();
}

// From libgfortran. Needed for gcc-4.8 and earlier.
extern "C" int _gfortran_compare_string(int len1, const char *s1,
                                        int len2, const char *s2) {
  typedef unsigned char UCHARTYPE;
  const UCHARTYPE *s;
  int len;
  int res;

  res = memcmp(s1, s2, ((len1 < len2) ? len1 : len2));
  if (res != 0)
    return res;

  if (len1 == len2)
    return 0;

  if (len1 < len2) {
    len = len2 - len1;
    s = (UCHARTYPE *) &s2[len1];
    res = -1;
  } else {
    len = len1 - len2;
    s = (UCHARTYPE *) &s1[len2];
    res = 1;
  }

  while (len--) {
    if (*s != ' ') {
      if (*s > ' ')
        return res;
      else
        return -res;
    }
    s++;
  }

  return 0;
}