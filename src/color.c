/**\file    color.c
 * \ingroup Color
 *
 * Print to console using embedded colour-codes inside the string-format.
 *
 * \eg{.}
 *   \code{.c}
 *     C_printf ("~4Hello ~2world~0.\n");
 *   \endcode
 *   will print to stdout with \c Hello mapped to colour 4
 *   and \c world mapped to colour 2.
 *   See the \ref colour_map[] array below.
 *
 * By default, the colour indices maps to these foreground colour:
 * \li 0: the startup forground *and* background colour.
 * \li 1: bright cyan foreground.
 * \li 2: bright green foreground.
 * \li 3: bright yellow foreground.
 * \li 4: bright magenta foreground.
 * \li 5: bright red foreground.
 * \li 6: bright white foreground.
 * \li 7: dark cyan foreground.
 *
 * by G. Vanem <gvanem@yahoo.no> 2011.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <limits.h>
#include <ctype.h>
#include <io.h>
#include <windows.h>

/**
 * \note
 *   We do not depend on other local .h-files other than "color.h".
 *   Thus it's easy to use this module in other projects.
 */
#include "color.h"

#if defined(__CYGWIN__)
  #include <unistd.h>
  #define _fileno(f)         fileno (f)
  #define _write(f,buf,len)  write (f,buf,len)
#elif defined(_MSC_VER) && (_MSC_VER <= 1600)
  #define snprintf           _snprintf
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

static int c_trace = 0;
static const char *C_dump20 (const void *data_p, size_t size);
extern int         is_cygwin_tty (int fd);

#define TRACE(level, ...)  do {                             \
                            if (c_trace >= level) {         \
                               printf ("%s(%u): ",          \
                                       __FILE__, __LINE__); \
                               printf (__VA_ARGS__);        \
                            }                               \
                          } while (0)

#ifndef C_BUF_SIZE
#define C_BUF_SIZE 2048
#endif

#ifndef STDOUT_FILENO
#define STDOUT_FILENO  1
#endif

/**
 * The app using color.c must set to \b 1 prior to
 * calling the below \ref C_printf() or \ref C_puts() functions.
 * For CygWin, if this is \b !0, it will set \ref C_use_ansi_colours.
 */
int C_use_colours = 0;

/**
 * For CygWin or if we detect we're running under \b mintty.exe (or some other program
 * lacking WinCon support), this variable means we must use ANSI-sequences to set colours.
 *
 * If running under ConEmu (detecting a valid \c %ConEmuHWND% window and \c %ConEmuANSI=ON),
 * this variable is set to \b 1 to use a more rich set of ANSI-colours.
 */
int C_use_ansi_colours = 0;

/**
 * When this is set to \b 0, CygWin / ConEmu will also uses WinCon API to set colours.
 */
int C_no_ansi = 1;

/**
 * The program using color.c must set this to \b 1 if \c fwrite() shall
 * be used in \ref C_flush(). This can be needed to synchronize the output
 * with other calls (libraries?) that writes to stdout using \c fwrite().
 */
int C_use_fwrite = 0;

/**
 * A count of number of times \ref C_flush() was called with nothing
 * in the trace-buffer.
 */
unsigned C_redundant_flush = 0;

void (*C_write_hook) (const char *buf) = NULL;

static char   c_buf [C_BUF_SIZE];
static char  *c_head, *c_tail;
static int    c_raw = 0;
static int    c_binmode = 0;
static BOOL   c_always_set_bg = FALSE;
static BOOL   c_exited = FALSE;

/** The \c FILE to print to. This is set to \c stdout in \ref C_init().
 */
static FILE *c_out = NULL;

/** The width of the screen obtained from \c GetConsoleScreenBufferInfo()
 *  or \c %COLUMNS in \ref C_init().
 */
static size_t c_screen_width = UINT_MAX;

/** The console-buffer information initialised by
 *  \c GetConsoleScreenBufferInfo() in \ref C_init().
 */
static CONSOLE_SCREEN_BUFFER_INFO console_info;

/** The critical section structure initialised by
 *  \c InitializeCriticalSection() in \ref C_init().
 */
static CRITICAL_SECTION crit;

static HANDLE console_hnd = INVALID_HANDLE_VALUE;

/**
 * Array of colour indices to WinCon colour values (foreground and background combined).
 *
 * This map is also configurable from the calling side via the C_init_colour_map()
 * function. The default colours are set in C_init().
 */
static WORD colour_map [8];

/**
 * Array of colour indices to ANSI-sequences (foreground and background combined).
 *
 * This map is set when the \ref colour_map[] is set.
 */
static char colour_map_ansi [DIM(colour_map)] [20];

static const char *wincon_to_ansi (WORD col);

/**
 * Returns 1 if detected running under the ConEmu program.
 */
int C_conemu_detected (void)
{
  const char *conemu_hwnd = getenv ("ConEmuHWND");
  const char *conemu_ansi = getenv ("ConEmuANSI");

  if (!conemu_hwnd || !conemu_ansi)
  {
    TRACE (1, "Not running under ConEmu.\n");
    return (FALSE);
  }
  if (!stricmp(conemu_ansi,"ON"))
  {
    TRACE (1, "Running under ConEmu with ANSI X3.64 support.\n");
    return (TRUE);
  }
  return (FALSE);
}

/**
 * Check \c %COLOUR_TRACE and return it's value.
 */
int C_trace_level (void)
{
  const char *env = getenv ("COLOUR_TRACE");

  if (env && *env >= '0' && *env <= '9')
     return (*env - '0');
  return (0);
}

/**
 * Customize the \ref colour_map[1..N].
 * Must be a list terminated by 0.
 *
 * \ref colour_map[0] can \b not be modified. It is reserved for the default
 * colour. I.e. the active colour in effect when program started.
 */
int C_init_colour_map (unsigned short col, ...)
{
  int     i;
  va_list args;
  va_start (args, col);

  colour_map[0] = console_info.wAttributes;
  TRACE (1, "i: 0, col: %u.\n", colour_map[0]);
  i = 1;

  while (col && i < DIM(colour_map))
  {
    colour_map [i] = col;
    TRACE (1, "i: %d, col: %u.\n", i, col);
    i++;

    /* Use an 'int' due to the clang-cl warning:
     *   second argument to 'va_arg' is of promotable type 'WORD' (aka 'unsigned short'); this va_arg
     *   has undefined behavior because arguments will be promoted to 'int' [-Wvarargs]
     */
    col = (WORD) va_arg (args, int);
  }

  /* Set the rest to default colours in case not all elements was filled.
   */
  while (i < DIM(colour_map))
  {
    col = console_info.wAttributes;
    TRACE (1, "i: %d, col: %u.\n", i, col);
    colour_map [i++] = col;
  }

  /**
   * Fill the ANSI-sequence array by looping over \ref colour_map_ansi[].
   * \note the size of both \ref colour_map_ansi[] and \ref colour_map[] \b are equal.
   */
  for (i = 0; i < DIM(colour_map_ansi); i++)
  {
    const char *p = wincon_to_ansi (colour_map[i]);

    TRACE (2, "colour_map_ansi[%u] -> %s\n", (unsigned)i, C_dump20(p,strlen(p)));
    strncpy (colour_map_ansi[i], p, sizeof(colour_map_ansi[i]));
  }
  return (1);
}

/**
 * Sets raw or normal output mode.
 * \param[in] raw  raw = 1: do not interpret \c '~n' to set colour.
 *                 raw = 0: do interpret \c '~n' as colour indices (\b default).
 * \return the previous mode.
 */
int C_setraw (int raw)
{
  int rc = c_raw;

  c_raw = raw;
  return (rc);
}

/**
 * Sets binary or cooked output mode.
 * \param[in] bin bin = 1: do not convert \c '\\n' into \c '\\r\\n'.<br>
 *                bin = 0: do convert \c '\\n' into \c '\\r\\n' (\b default).
 * \return the previous mode.
 */
int C_setbin (int bin)
{
  int rc = c_binmode;

  c_binmode = bin;
  return (rc);
}

/**
 * The global exit function.
 * Flushes the output buffer and deletes the critical section.
 */
void C_exit (void)
{
  C_reset();

  if (c_out)
     C_flush();
  c_head = c_tail = NULL;
  c_out = NULL;
  c_exited = TRUE;
  DeleteCriticalSection (&crit);
}

/**
 * Our local initialiser function. Called once to:
 *
 *  \li Set the trace-level from \c %COLOUR_TRACE%.
 *  \li Get the console-buffer information from Windows Console.
 *  \li If the console is not redirected:
 *     1. get the screen height and width.
 *     2. setup the \ref colour_map[] array and the
 *        \ref colour_map_ansi[] array. Even if ANSI output is \b not wanted.
 *  \li Set \ref c_out to default \c stdout and setup buffer head and tail.
 *  \li Initialise the critical-section structure \ref crit.
 */
static int C_init (void)
{
  if (c_exited)
  {
    fputs ("C_init() called after C_exit()!?\n", stderr);
    return (0);
  }

  if (!c_head || !c_out)
  {
    const char *env;
    BOOL        okay;

    c_trace = C_trace_level();

    console_hnd = GetStdHandle (STD_OUTPUT_HANDLE);
    okay = (console_hnd != INVALID_HANDLE_VALUE &&
            GetConsoleScreenBufferInfo(console_hnd, &console_info) &&
            GetFileType(console_hnd) == FILE_TYPE_CHAR);

#if defined(__CYGWIN__)
     if (!okay) /* Use ANSI-colours even if stdout is redirected */
     {
       console_info.srWindow.Right = 100;
       console_info.srWindow.Left  = 0;
       console_info.wAttributes    = 0x1F;   /* Bright white on blue background */
       c_always_set_bg = TRUE;
       okay = TRUE;
     }
#endif

    if (okay)
    {
      WORD bg = console_info.wAttributes & ~7;

      c_screen_width = console_info.srWindow.Right - console_info.srWindow.Left + 1;
      C_init_colour_map ((bg + 3) | FOREGROUND_INTENSITY,  /* "~1" -> bright cyan */
                         (bg + 2) | FOREGROUND_INTENSITY,  /* "~2" -> bright green */
                         (bg + 6) | FOREGROUND_INTENSITY,  /* "~3" -> bright yellow */
                         (bg + 5) | FOREGROUND_INTENSITY,  /* "~4" -> bright magenta */
                         (bg + 4) | FOREGROUND_INTENSITY,  /* "~5" -> bright red */
                         (bg + 7) | FOREGROUND_INTENSITY,  /* "~6" -> bright white */
                         (bg + 3),                         /* "~7" -> dark cyan */
                         0);
    }
    else
      C_use_colours = 0;

    if (C_no_ansi == 0 && C_use_colours)
    {
#if defined(__CYGWIN__)
      C_use_ansi_colours = 1;
#endif
      if (C_conemu_detected())
         C_use_ansi_colours = 1;
    }

    env = getenv ("COLUMNS");
    if (env && atoi(env) > 0)
       c_screen_width = atoi (env);

    c_out  = stdout;
    c_head = c_buf;
    c_tail = c_head + C_BUF_SIZE - 1;
    InitializeCriticalSection (&crit);
  }
  return (1);
}

/**
 * Set console foreground and optionally background color.
 * FG is in the low 4 bits.
 * BG is in the upper 4 bits of the BYTE.
 * If 'col == 0', set default console colour.
 */
static void C_set (WORD col)
{
  static WORD last_attr = (WORD)-1;
  WORD   attr;

  assert (!C_use_ansi_colours);

  if (col == 0)     /* restore to default colour */
     attr = console_info.wAttributes;

  else
  {
    BYTE fg, bg;

    attr = col;
    fg   = loBYTE (attr);
    bg   = hiBYTE (attr);

    if (bg == (BYTE)-1)
    {
      attr = console_info.wAttributes & ~7;
      attr &= ~8;  /* Since 'wAttributes' could have been hi-intensity at startup. */
    }
    else
      attr = (WORD) (bg << 4);

    attr |= fg;
  }

  if (attr != last_attr)
     SetConsoleTextAttribute (console_hnd, attr);
  last_attr = attr;
}

/**
 * Reset console foreground and background to what is was initially.
 */
void C_reset (void)
{
  if (C_use_ansi_colours)
  {
     int raw = c_raw;

     c_raw = 1;
     C_puts (colour_map_ansi[0]);
     c_raw = raw;
  }
  else if (console_hnd != INVALID_HANDLE_VALUE)
     SetConsoleTextAttribute (console_hnd, console_info.wAttributes);
}

/**
 * Figure out if the parent process is \c mintty.exe which does not
 * support WinCon colors. Finding the name of parent can be used as
 * described here:
 *  http://www.codeproject.com/Articles/9893/Get-Parent-Process-PID \n
 *  http://www.scheibli.com/projects/getpids/index.html             \n
 * or \n
 *  https://github.com/git-for-windows/git/blob/27c08ea187462e56ffb514c8c997df419f004ce5/compat/winansi.c#L530-L569
 */
const char *get_parent_process_name (void)
{
  if (c_trace > 0 || is_cygwin_tty(STDOUT_FILENO))
     return ("mintty.exe");  /* Just for testing */
  return (NULL);
}

/**
 * Create an ANSI-sequence array.
 *
 * \param[in] col the Windows colour to map to a corresponding ANSI sequence.
 * \return    the ANSI sequence.
 */
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
  if (c_always_set_bg || (bg && bg != (console_info.wAttributes >> 4)))
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

/**
 * Set console colour using an ANSI sequence.
 * The corresponding WinCon colour set in \ref colour_map[] is used as a lookup-value.
 */
void C_set_ansi (unsigned short col)
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

/**
 * Change colour using ANSI or WinCon API.
 * Does nothing if console is redirected.
 */
void C_set_colour (unsigned short col)
{
  if (C_use_ansi_colours)
     C_set_ansi (col);
  else if (C_use_colours)
     C_set (col);
}

/**
 * Write out the trace-buffer.
 */
size_t C_flush (void)
{
  size_t len1 = (unsigned int) (c_head - c_buf);
  size_t len2;

  if (!c_out || len1 == 0)
  {
    C_redundant_flush++;
    return (0);
  }

  EnterCriticalSection (&crit);
  if (C_use_fwrite)
       len2 = fwrite (c_buf, 1, len1, c_out);
  else len2 = _write (_fileno(c_out), c_buf, (unsigned int)len1);

  if (C_write_hook)
  {
    c_buf [len1] = '\0';
    (*C_write_hook) (c_buf);
  }

  c_head = c_buf;   /* restart buffer */
  LeaveCriticalSection (&crit);
  return (len2);
}

/**
 * An printf() style console print function.
 */
int C_printf (const char *fmt, ...)
{
  int     len;
  va_list args;

  va_start (args, fmt);
  len = C_vprintf (fmt, args);
  va_end (args);
  return (len);
}

/**
 * A var-arg style console print function.
 */
int C_vprintf (const char *fmt, va_list args)
{
  int len1, len2;

  if (!C_init())
     return (0);

  if (c_raw)
  {
    C_flush();
    len1 = vfprintf (c_out, fmt, args);
    fflush (c_out);
  }
  else
  {
    char buf [2*C_BUF_SIZE];

    EnterCriticalSection (&crit);

    /* Terminate first. Because if '_MSC_VER < 1900',
     * should the returned buffer be exactly big enough for the result,
     * 'vsnprintf() do not add a trailing NUL.
     */
    buf [sizeof(buf)-1] = '\0';
    len2 = vsnprintf (buf, sizeof(buf)-1, fmt, args);
    len1 = C_puts (buf);
    LeaveCriticalSection (&crit);
    if (len2 < len1)
       FATAL ("len1: %d, len2: %d. c_buf: '%.*s',\nbuf: '%s'\n",
              len1, len2, (int)(c_head - c_buf), c_buf, buf);
  }
  return (len1);
}

/**
 * Put a single character to output buffer (at \c c_head).
 * Interpret a "~n" sequence as output buffer gets filled.
 */
int C_putc (int ch)
{
  static BOOL get_color = FALSE;
  int    i, rc = 0;

  if (!C_init())
     return (0);

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

      if (C_write_hook)
      {
        char buf[3] = { '~', '\0', '\0' };

        buf[1] = (char) ch;
        (*C_write_hook) (buf);
      }
      C_set_colour (color);
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
  *c_head++ = (char) ch;
  rc++;

  if (ch == '\n' || c_head >= c_tail)
     C_flush();
  return (rc);
}

/**
 * Put a single character to output buffer (at \c c_head).
 * No interpretation of "~n" sequences.
 */
int C_putc_raw (int ch)
{
  int rc, raw = c_raw;

  c_raw = 1;
  rc = C_putc (ch);
  c_raw = raw;
  return (rc);
}

/**
 * Put a 0-terminated string to output buffer.
 */
int C_puts (const char *str)
{
  int ch, rc = 0;

  for (rc = 0; (ch = *str) != '\0'; str++)
      rc += C_putc (ch);
  return (rc);
}

/**
 * Put a string (or buffer) of maximum \c len bytes to output buffer.
 */
int C_putsn (const char *str, size_t len)
{
  int    rc = 0;
  size_t i;

  for (i = 0; i < len; i++)
      rc += C_putc (*str++);
  return (rc);
}

/**
 * Print a long string to screen.
 * Try to wrap nicely according to the screen-width.
 * Multiple spaces ("  ") are collapsed into one space.
 */
void C_puts_long_line (const char *start, size_t indent)
{
  size_t      width = (c_screen_width == 0) ? UINT_MAX : c_screen_width;
  size_t      left  = width - indent;
  const char *c = start;

  while (*c)
  {
    if (*c == ' ')
    {
      /* Break a long line at a space.
       */
      const char *p = strchr (c+1, ' ');

      if (!p)
          p = strchr (c+1, '\0');
      if (left < 2 || (left <= (size_t)(p - c)))
      {
        C_printf ("\n%*c", (int)indent, ' ');
        left  = width - indent;
        start = ++c;
        continue;
      }
      if (isspace((int)c[1]))  /* Drop excessive blanks */
      {
        c++;
        continue;
      }
    }
    C_putc (*c++);
    left--;
  }
  C_putc ('\n');
}

/**
 * Return the \ref c_screen_width.
 */
size_t C_screen_width (void)
{
  return (c_screen_width);
}

/**
 * Dump max 20 bytes of data as hex-printables.
 */
static const char *C_dump20 (const void *data, size_t size)
{
  static char ret [25];
  size_t ofs;
  int    ch;

  for (ofs = 0; ofs < sizeof(ret)-4 && ofs < size; ofs++)
  {
    ch = ((const BYTE*)data) [ofs];
    if (ch < ' ')            /* non-printable */
         ret [ofs] = '.';
    else ret [ofs] = (char) ch;
    ret [ofs+1] = '\0';
  }
  if (ofs < size)
     strcat (ret, "...");
  return (ret);
}
