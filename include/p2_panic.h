#pragma once
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif
__attribute__((noreturn,format(printf,3,0)))
void p2_panic_v(const char* file,int line,const char* format,va_list* args);
#ifdef __cplusplus
}
#endif
