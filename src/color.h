/**\file    color.h
 * \ingroup Color
 */
#ifndef _COLOR_H
#define _COLOR_H

#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief
 * These are the default colour that `C_init_colour_map()` is initialised with.
 * Can be used like:
 * \code
 *   C_printf ("  " C_BR_RED "<not found>" C_DEFAULT);
 * \endcode
 *
 * But not after `C_init_colour_map()` has been called to alter these
 * colours with something else.
 */
#define C_BR_CYAN     "~1"   /**< bright cyan */
#define C_BR_GREEN    "~2"   /**< bright green */
#define C_BR_YELLOW   "~3"   /**< bright yellow */
#define C_BR_MEGENTA  "~4"   /**< bright magenta */
#define C_BR_RED      "~5"   /**< bright red */
#define C_BR_WHITE    "~6"   /**< bright white */
#define C_DK_CYAN     "~7"   /**< dark cyan */
#define C_BG_RED      "~8"   /**< white on red background */
#define C_BG_BLACK    "~9"   /**< white on black background (not yet) */
#define C_DEFAULT     "~0"   /**< restore default colour */

extern int C_use_colours;
extern int C_use_ansi_colours;
extern int C_no_ansi;
extern int C_use_fwrite;

/*
 * Defined in newer <sal.h> for MSVC.
 */
#ifndef _Printf_format_string_
#define _Printf_format_string_
#endif

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
extern void   C_reset    (void);
extern void   C_init     (void);
extern void   C_exit     (void);
extern void   C_set_colour      (unsigned short col);
extern void   C_set_ansi        (unsigned short col);
extern void   C_puts_long_line  (const char *start, size_t indent);
extern int    C_init_colour_map (unsigned short col1, ...);
extern size_t C_screen_width    (void);
extern int    C_trace_level     (void);
extern int    C_conemu_detected (void);
extern int    C_VT_detected     (int cmd_only);

#ifdef __cplusplus
}
#endif

#endif /* _COLOR_H */
