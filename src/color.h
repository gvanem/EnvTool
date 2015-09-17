#ifndef _COLOR_H
#define _COLOR_H

#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The app using color.c must set to 1 prior to
 * calling the below 'C_xx()' functions.
 */
extern int use_colours;

/* Count of unneeded C_flush calls.
 */
extern unsigned redundant_C_flush;

extern int C_printf  (const char *fmt, ...)
  #if defined(__GNUC__)
    __attribute__ ((format(printf,1,2)))
  #endif
   ;

extern int    C_vprintf  (const char *fmt, va_list args);
extern int    C_puts     (const char *str);
extern int    C_putsn    (const char *str, size_t len);
extern int    C_putc     (int ch);
extern int    C_putc_raw (int ch);
extern int    C_setraw   (int raw);
extern int    C_setbin   (int bin);
extern size_t C_flush    (void);

#ifdef __cplusplus
};
#endif

#endif
