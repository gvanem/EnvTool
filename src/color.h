#ifndef _COLOR_H
#define _COLOR_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int C_printf  (const char *fmt, ...)
  #if defined(__GNUC__)
    __attribute__ ((format(printf,1,2)))
  #endif
   ;

extern int C_vprintf  (const char *fmt, va_list args);
extern int C_puts     (const char *str);
extern int C_putc     (int ch);
extern int C_putc_raw (int ch);
extern int C_setmode  (int raw);

#ifdef __cplusplus
};
#endif

#endif
