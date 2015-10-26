/*!\file color.c
 *
 * Print to console using embedded colour-codes inside the string-format.
 *
 * E.g. C_printf ("~4Hello ~2world~0.\n");
 *      will print to stdout with 'Hello' mapped to colour 4 (see below)
 *      and 'world' mapped to colour 2.
 *
 * by G. Vanem <gvanem@yahoo.no> 2011.
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <ctype.h>
#include <io.h>
#include <windows.h>

#include "color.h"

#ifdef __CYGWIN__
  #include <unistd.h>
  #define _fileno(f)         fileno (f)
  #define _write(f,buf,len)  write (f,buf,len)
#endif

#define loBYTE(w)     (BYTE)(w)
#define hiBYTE(w)     (BYTE)((WORD)(w) >> 8)
#define DIM(x)        (int) (sizeof(x) / sizeof((x)[0]))

#define FATAL(fmt, ...)  do {                                        \
                           fprintf (stderr, "\nFATAL: %s(%u): " fmt, \
                                    __FILE__, __LINE__,              \
                                    ## __VA_ARGS__);                 \
                           if (IsDebuggerPresent())                  \
                                abort();                             \
                           else ExitProcess (GetCurrentProcessId()); \
                         } while (0)


#define C_BUF_SIZE (2*1024)

#ifndef STDOUT_FILENO
#define STDOUT_FILENO  1
#endif

/* The program using color.c must set this to 1.
 */
int use_colours = 0;

unsigned c_redundant_flush = 0;

static char  c_buf [C_BUF_SIZE];
static char *c_head, *c_tail;
static FILE *c_out = NULL;
static int   c_raw = 0;
static int   c_binmode = 0;

static CONSOLE_SCREEN_BUFFER_INFO console_info;

static HANDLE console_hnd = INVALID_HANDLE_VALUE;

/*
 * \todo: make this configurable from the calling side.
 */
static WORD color_map [7];

static void init_color_map (void)
{
  int  i;
  WORD bg = console_info.wAttributes & ~7;

  for (i = 1; i < DIM(color_map); i++)
      color_map[i] = console_info.wAttributes;

  color_map[0] = 0;
  color_map[1] = (bg + 3) | FOREGROUND_INTENSITY;  /* bright cyan */
  color_map[2] = (bg + 2) | FOREGROUND_INTENSITY;  /* bright green */
  color_map[3] = (bg + 6) | FOREGROUND_INTENSITY;  /* bright yellow */
  color_map[4] = (bg + 5) | FOREGROUND_INTENSITY;  /* bright magenta */
  color_map[5] = (bg + 4) | FOREGROUND_INTENSITY;  /* bright red */
  color_map[6] = (bg + 7) | FOREGROUND_INTENSITY;  /* bright white */
}

int C_setraw (int raw)
{
  int rc = c_raw;

  c_raw = raw;
  return (rc);
}

int C_setbin (int bin)
{
  int rc = c_binmode;

  c_binmode = bin;
  return (rc);
}

static void C_exit (void)
{
  if (c_out)
     C_flush();
  c_head = c_tail = NULL;
  c_out = NULL;
}

static void C_init (void)
{
  if (!c_head || !c_out)
  {
    BOOL okay;

    console_hnd = GetStdHandle (STD_OUTPUT_HANDLE);
    okay = (console_hnd != INVALID_HANDLE_VALUE &&
            GetConsoleScreenBufferInfo(console_hnd, &console_info) &&
            GetFileType(console_hnd) == FILE_TYPE_CHAR);

    if (okay)
         init_color_map();
    else use_colours = 0;

    c_out  = stdout;
    c_head = c_buf;
    c_tail = c_head + C_BUF_SIZE - 1;
    atexit (C_exit);
  }
}

/*
 * Set console foreground and optionally background color.
 * FG is in the low 4 bits.
 * BG is in the upper 4 bits of the BYTE.
 * If 'col == 0', set default console colour.
 */
static void C_set (WORD col)
{
  BYTE   fg, bg;
  WORD   attr;
  static WORD last_attr = (WORD)-1;

  if (col == 0)     /* restore to default colour */
  {
    attr = console_info.wAttributes;
    fg   = loBYTE (attr);
    bg   = hiBYTE (attr);
  }
  else
  {
    attr = col;
    fg   = loBYTE (attr);
    bg   = hiBYTE (attr);

    if (bg == (BYTE)-1)
    {
      attr = console_info.wAttributes & ~7;
      attr &= ~8;  /* Since 'wAttributes' could have been hi-intensity at startup. */
    }
    else
      attr = bg << 4;

    attr |= fg;
  }

  if (attr != last_attr)
     SetConsoleTextAttribute (console_hnd, attr);
  last_attr = attr;
}

/*
 * Write out the trace-buffer.
 */
size_t C_flush (void)
{
  size_t len = (unsigned int) (c_head - c_buf);

  if (len == 0)
  {
    c_redundant_flush++;
    return (0);
  }

  assert (c_out);
  len = _write (_fileno(c_out), c_buf, (unsigned int)len);

  c_head = c_buf;   /* restart buffer */
  return (len);
}

int C_printf (const char *fmt, ...)
{
  int     len;
  va_list args;

  va_start (args, fmt);
  len = C_vprintf (fmt, args);
  va_end (args);
  return (len);
}

int C_vprintf (const char *fmt, va_list args)
{
  int len1, len2;

  if (c_raw)
  {
    C_init();
    C_flush();
    len2 = vfprintf (c_out, fmt, args);
    fflush (c_out);
  }
  else
  {
    char buf [2*C_BUF_SIZE];

    buf [sizeof(buf)-1] = '\0';
    len2 = vsnprintf (buf, sizeof(buf)-1, fmt, args);
    len1 = C_puts (buf);
    if (len2 < len1)
      FATAL ("len1: %d, len2: %d. c_buf: '%.*s',\nbuf: '%s'\n",
             len1, len2, (int)(c_head - c_buf), c_buf, buf);
  }
  return (len1);
}

int C_putc (int ch)
{
  static BOOL get_color = FALSE;
  int    i, rc = 0;

  C_init();

  assert (c_head);
  assert (c_tail);
  assert (c_head >= c_buf);
  assert (c_head <= c_tail);

  if (get_color && !c_raw)
  {
    WORD color;

    get_color = FALSE;
    if (ch == '~')
       goto put_it;

    i = ch - '0';
    if (i >= 0 && i < DIM(color_map))
      color = color_map [i];
    else
      FATAL ("Illegal color index %d ('%c'/0x%02X) in c_buf: '%.*s'\n",
             i, ch, ch, (int)(c_head - c_buf), c_buf);

    C_flush();
    if (use_colours)
       C_set (color);
    return (1);
  }

  if (ch == '~' && !c_raw)
  {
    get_color = TRUE;   /* change state; get colour index in next char */
    return (0);
  }

  if (ch == '\n' && c_binmode)
  {
    if ((c_head == c_buf) ||
        (c_head > c_buf && c_head[-1] != '\r'))
    {
      *c_head++ = '\r';
      rc++;
    }
  }

put_it:
  *c_head++ = ch;
  rc++;

  if (ch == '\n' || c_head >= c_tail)
     C_flush();
  return (rc);
}

int C_putc_raw (int ch)
{
  int rc, raw = c_raw;

  c_raw = 1;
  rc = C_putc (ch);
  c_raw = raw;
  return (rc);
}

int C_puts (const char *str)
{
  int ch, rc = 0;

  for (rc = 0; (ch = *str) != '\0'; str++)
      rc += C_putc (ch);
  return (rc);
}

int C_putsn (const char *str, size_t len)
{
  int    rc = 0;
  size_t i;

  for (i = 0; i < len; i++)
      rc += C_putc (*str++);
  return (rc);
}


