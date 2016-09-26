#ifndef _COLOR_H
#define _COLOR_H

#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The app using color.c must set to 1 prior to
 * calling the below 'C_xx()' functions.
 * For CygWin, if this is > 1, it means to use ANSI-sequences to set colours.
 */
extern int C_use_colours;

/* For CygWin, this variable means we must use ANSI-sequences to set colours.
 */
extern int C_use_ansi_colours;

/*
 * Defined in newer <sal.h> for MSVC.
 */
#ifndef _Printf_format_string_
#define _Printf_format_string_
#endif

/* Count of unneeded 'C_flush()' calls. I.e. calls where length of buffer is 0.
 */
extern unsigned C_redundant_flush;

extern int C_printf (_Printf_format_string_ const char *fmt, ...)
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
