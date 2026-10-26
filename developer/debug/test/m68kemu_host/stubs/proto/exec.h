#pragma once
#ifdef __cplusplus
#include <cstdlib>
#include <cstring>
#else
#include <stdlib.h>
#include <string.h>
#endif
#define AllocMem(size, flags) calloc(1, size)
#define FreeMem(ptr, size) free(ptr)
#define AllocVec(size, flags) calloc(1, size)
#define FreeVec(ptr) free(ptr)
#define CopyMem(src, dst, n) memcpy(dst, src, n)
