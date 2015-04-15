#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <ctype.h>
#include <io.h>
#include <windows.h>

#include "color.h"

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


#define TRACE_BUF_SIZE (2*1024)

#ifndef STDOUT_FILENO
#define STDOUT_FILENO  1
#endif

extern int use_colours;  /* The app using color.c must set this. */

static char  trace_buf [TRACE_BUF_SIZE];
static char *trace_ptr, *trace_end;
static int   trace_hnd = -1;

static BOOL  tilde_escape = TRUE;
static BOOL  stdout_redirected = FALSE;
static BOOL  c_raw = FALSE;
static int   c_binmode = 0;

static CONSOLE_SCREEN_BUFFER_INFO console_info;

static HANDLE console_hnd = INVALID_HANDLE_VALUE;

static WORD color_map [6];

static void init_color_map (void)
{
  WORD bg = console_info.wAttributes & ~7;
  int  i;

  for (i = 1; i < DIM(color_map); i++)
      color_map[i] = console_info.wAttributes;

  color_map[0] = 0;
  color_map[1] = (bg + 3) | FOREGROUND_INTENSITY;  /* bright cyan */
  color_map[2] = (bg + 2) | FOREGROUND_INTENSITY;  /* bright green */
  color_map[3] = (bg + 6) | FOREGROUND_INTENSITY;  /* bright yellow */
  color_map[4] = (bg + 5);                         /* magenta */
  color_map[5] = (bg + 4) | FOREGROUND_INTENSITY;  /* bright red */
}

int C_setmode (int raw)
{
  int rc = c_raw;
  c_raw = raw;
  return (rc);
}

static void C_exit (void)
{
  trace_ptr = trace_end = NULL;
  trace_hnd = -1;
}

static void C_init (void)
{
  if (!trace_ptr || trace_hnd == -1)
  {
    BOOL okay;

    console_hnd = GetStdHandle (STD_OUTPUT_HANDLE);
    okay = (console_hnd != INVALID_HANDLE_VALUE &&
            GetConsoleScreenBufferInfo(console_hnd, &console_info));

    if (!okay || GetFileType(console_hnd) != FILE_TYPE_CHAR)
    {
      stdout_redirected = TRUE;
      use_colours = FALSE;
    }
    else
      init_color_map();

    trace_hnd = STDOUT_FILENO;
    trace_ptr = trace_buf;
    trace_end = trace_ptr + TRACE_BUF_SIZE - 1;
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
         attr = console_info.wAttributes & ~7;
    else attr = bg << 4;

    attr |= fg;
  }

  if (attr != last_attr)
     SetConsoleTextAttribute (console_hnd, attr);
  last_attr = attr;
}

/*
 * Write out the trace-buffer.
 */
static unsigned int C_flush (void)
{
  unsigned int len = (unsigned int) (trace_ptr - trace_buf);

  assert (trace_hnd >= 1);
  len = _write (trace_hnd, trace_buf, len);

  trace_ptr = trace_buf;   /* restart buffer */
  return (len);
}

int C_printf (const char *fmt, ...)
{
  char    buf [2*TRACE_BUF_SIZE];
  int     len1, len2;
  va_list args;

  va_start (args, fmt);

  buf [sizeof(buf)-1] = '\0';
  len2 = vsnprintf (buf, sizeof(buf)-1, fmt, args);
  len1 = C_puts (buf);

  if (len1 < len2)
    FATAL ("len1: %d, len2: %d. trace_buf: '%.*s',\nbuf: '%s'\n",
           len1, len2, trace_ptr - trace_buf, trace_buf, buf);

  va_end (args);
  return (len2);
}

int C_vprintf (const char *fmt, va_list args)
{
  char buf [2*TRACE_BUF_SIZE];
  int  len1, len2;

  buf [sizeof(buf)-1] = '\0';
  len2 = vsnprintf (buf, sizeof(buf)-1, fmt, args);
  len1 = C_puts (buf);
  if (len1 < len2)
    FATAL ("len1: %d, len2: %d. trace_buf: '%.*s',\nbuf: '%s'\n",
           len1, len2, trace_ptr - trace_buf, trace_buf, buf);

  return (len2);
}

int C_putc (int ch)
{
  static BOOL get_color = FALSE;
  int    i, rc = 0;

  C_init();

  assert (trace_ptr);
  assert (trace_end);
  assert (trace_ptr >= trace_buf);
  assert (trace_ptr <= trace_end);

  if (tilde_escape && get_color && !c_raw)
  {
    WORD color;

    get_color = FALSE;
    if (ch == '~')
       goto put_it;

    i = ch - '0';
    if (i >= 0 && i < DIM(color_map))
      color = color_map [i];
    else
      FATAL ("Illegal color index %d ('%c'/0x%02X) in trace_buf: '%.*s'\n",
             i, ch, ch, trace_ptr - trace_buf, trace_buf);

    C_flush();
    if (!stdout_redirected && use_colours)
       C_set (color);
    return (1);
  }

  if (tilde_escape && ch == '~' && !c_raw)
  {
    get_color = TRUE;
    return (1);
  }

  if (ch == '\n' && c_binmode)
  {
    if ((trace_ptr == trace_buf) ||
        (trace_ptr > trace_buf && trace_ptr[-1] != '\r'))
    {
      *trace_ptr++ = '\r';
      rc++;
    }
  }

put_it:
  *trace_ptr++ = ch;
  rc++;

  if (ch == '\n' || trace_ptr >= trace_end)
     C_flush();
  return (rc);
}

int C_putc_raw (int ch)
{
  BOOL save = tilde_escape;
  int  rc;

  tilde_escape = FALSE;
  rc = C_putc (ch);
  tilde_escape = save;
  return (rc);
}

int C_puts (const char *str)
{
  int ch, rc = 0;

  for (rc = 0; (ch = *str) != '\0'; str++)
      rc += C_putc (ch);
  return (rc);
}

