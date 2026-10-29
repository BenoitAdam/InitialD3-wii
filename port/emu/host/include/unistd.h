#pragma once
#include <stdint.h>
typedef uint32_t useconds_t;
#ifdef __cplusplus
extern "C" {
#endif
int usleep(useconds_t usec);
#ifdef __cplusplus
}
#endif
