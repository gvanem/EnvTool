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

#if defined(__CYGWIN__)
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

#if defined(NDEBUG)
  #define TRACE(level, ...)  ((void)0)
#else
  #define TRACE(level, ...)  do {                            \
                              if (trace >= level) {          \
                                printf ("%s(%u): ",          \
                                        __FILE__, __LINE__); \
                                printf (__VA_ARGS__);        \
                              }                              \
                            } while (0)
#endif

#ifndef C_BUF_SIZE
#define C_BUF_SIZE (2*1024)
#endif

#ifndef STDOUT_FILENO
#define STDOUT_FILENO  1
#endif

/* The program using color.c must set this to 1.
 */
int use_colours = 0;

/* The program using color.c must set this to 1 if fwrite() shall
 * be used in 'C_flush()'. This can be needed to syncronise the output
 * with other calls (libraries?) that writes to stdout using fwrite().
 */
int use_fwrite = 0;

/* For CygWin or if we detect we're running under mintty.exe (or some other program lacking
 * WinCon support), this variable means we must use ANSI-sequences to set colours.
 */
int use_ansi_colours = 0;

unsigned c_redundant_flush = 0;

static char  c_buf [C_BUF_SIZE];
static char *c_head, *c_tail;
static FILE *c_out = NULL;
static int   c_raw = 0;
static int   c_binmode = 0;

static CONSOLE_SCREEN_BUFFER_INFO console_info;

static HANDLE console_hnd = INVALID_HANDLE_VALUE;

#if !defined(NDEBUG)
  static int trace = 0;
#endif

/*
 * \todo: make this configurable from the calling side.
 */
static WORD colour_map [7];

static char colour_map_ansi [DIM(colour_map)] [20];
static void init_colour_map_ansi (void);

static void init_colour_map (void)
{
  int  i;
  WORD bg = console_info.wAttributes & ~7;

  for (i = 0; i < DIM(colour_map); i++)
      colour_map[i] = console_info.wAttributes;

  colour_map[0] = 0;
  colour_map[1] = (bg + 3) | FOREGROUND_INTENSITY;  /* bright cyan */
  colour_map[2] = (bg + 2) | FOREGROUND_INTENSITY;  /* bright green */
  colour_map[3] = (bg + 6) | FOREGROUND_INTENSITY;  /* bright yellow */
  colour_map[4] = (bg + 5) | FOREGROUND_INTENSITY;  /* bright magenta */
  colour_map[5] = (bg + 4) | FOREGROUND_INTENSITY;  /* bright red */
  colour_map[6] = (bg + 7) | FOREGROUND_INTENSITY;  /* bright white */
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

#if !defined(NDEBUG)
    const char *env = getenv ("COLOUR_TRACE");

    if (env)
      trace = *env - '0';
#endif

    console_hnd = GetStdHandle (STD_OUTPUT_HANDLE);
    okay = (console_hnd != INVALID_HANDLE_VALUE &&
            GetConsoleScreenBufferInfo(console_hnd, &console_info) &&
            GetFileType(console_hnd) == FILE_TYPE_CHAR);

    if (okay)
    {
      init_colour_map();

#if defined(__CYGWIN__)
      use_ansi_colours = 1;
#endif

      if (use_ansi_colours)
         init_colour_map_ansi();
    }
    else
      use_colours = 0;

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
 * Figure out if the parent process is mintty.exe which does not
 * support WinCon colors. Finding the name of parent can be used as
 * described here:
 *  http://www.codeproject.com/Articles/9893/Get-Parent-Process-PID
 *  http://www.scheibli.com/projects/getpids/index.html
 * or
 *  https://github.com/git-for-windows/git/blob/27c08ea187462e56ffb514c8c997df419f004ce5/compat/winansi.c#L530-L569
 *  f:\gv\VC_project\Winsock-tracer\Escape-From-DLL-Hell\Common\Process.cpp
 */
const char *get_parent_process_name (void)
{
#if !defined(NDEBUG)
  if (trace > 0)
     return ("mintty.exe");
#endif
  return (NULL);
}

static const char *wincon_to_ansi (WORD col)
{
  static char ret[20];  /* max: "\x1B[30;1;40;1m" == 12 */
  static BYTE wincon_to_SGR[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
  BYTE   fg, bg, SGR;
  size_t left = sizeof(ret);
  BOOL   bold;
  char  *p = ret;

  if (col == 0)
     return ("\x1b[0m");

  fg  = col & 7;
  SGR = wincon_to_SGR [fg & ~FOREGROUND_INTENSITY];
  bold = (col & FOREGROUND_INTENSITY);

  if (bold)
       p += snprintf (p, left, "\x1B[%d;1m", 30 + SGR);
  else p += snprintf (p, left, "\x1B[%dm", 30 + SGR);
  left -= (ret - p);

  bold = (col & BACKGROUND_INTENSITY);
  bg   = (col & ~BACKGROUND_INTENSITY) >> 4;
  if (bg && bg != (console_info.wAttributes >> 4))
  {
    TRACE (2, "col: %u, bg: 0x%02X, console_info.wAttributes: 0x%04X\n",
           col, bg, console_info.wAttributes);
    SGR = wincon_to_SGR [bg];
    if (bold)
         snprintf (p-1, left+1, ";%d;1m", 40 + SGR);
    else snprintf (p-1, left+1, ";%dm", 40 + SGR);
  }
  return (ret);
}

static void init_colour_map_ansi (void)
{
  size_t i;

  for (i = 0; i < DIM(colour_map_ansi); i++)
  {
    const char *p = wincon_to_ansi (colour_map[i]);
    extern const char *dump20 (const void *data_p, unsigned size);

    TRACE (2, "colour_map_ansi[%u] -> %s\n", (unsigned)i, dump20(p,strlen(p)));
    strncpy (colour_map_ansi[i], p, sizeof(colour_map_ansi[i]));
  }
}

static void C_set_ansi (WORD col)
{
  int i, raw_save = c_raw;

  c_raw = 1;
  for (i = 0; i < DIM(colour_map); i++)
      if (col == colour_map[i])
      {
        C_puts (colour_map_ansi[i]);
        break;
      }
  c_raw = raw_save;
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
  if (use_fwrite)
       len = fwrite (c_buf, 1, len, c_out);
  else len = _write (_fileno(c_out), c_buf, (unsigned int)len);

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
    len1 = vfprintf (c_out, fmt, args);
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

  if (!c_raw)
  {
    if (get_color)
    {
      WORD color;

      get_color = FALSE;
      if (ch == '~')
         goto put_it;

      i = ch - '0';
      if (i >= 0 && i < DIM(colour_map))
        color = colour_map [i];
      else
        FATAL ("Illegal color index %d ('%c'/0x%02X) in c_buf: '%.*s'\n",
               i, ch, ch, (int)(c_head - c_buf), c_buf);

      C_flush();

      if (use_ansi_colours)
         C_set_ansi (color);
      else if (use_colours)
         C_set (color);
      return (1);
    }

    if (ch == '~')
    {
      get_color = TRUE;   /* change state; get colour index in next char */
      return (0);
    }
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


