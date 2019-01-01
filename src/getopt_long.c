/** \file    getopt_long.c
 *  \ingroup Misc
 *  \brief
 *    Functions for parsing the program command-line.
 */

/*
 * Copyright (c) 2002 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F39502-99-1-0512.
 */
/*-
 * Copyright (c) 2000 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Dieter Baron and Thomas Klausner.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * AS IS AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <windows.h>
#include <shellapi.h>

#include "envtool.h"
#include "getopt_long.h"

#define PRINT_ERROR ((opterr) && (*options != ':'))

/**
 * \def FLAGS_PERMUTE   permute non-options to the end of `argv`.
 * \def FLAGS_ALLARGS   treat non-options as args to option "-1".
 * \def FLAGS_LONGONLY  operate as `getopt_long_only()`.
 */
#define FLAG_PERMUTE    0x01
#define FLAG_ALLARGS    0x02
#define FLAG_LONGONLY   0x04

/**
 * Return values
 *
 * \def BADCH
 *   If `getopt()` encounters an option character that was not in `optstring`, then `?` is returned.
 *
 * \def BADARG
 *   If `getopt()` encounters an option with a missing argument, then the return value depends on
 *   the first character in `optstring`: if it is `:`, then `:` is returned; otherwise `?` is returned.
 */
#define BADCH       (int)'?'
#define BADARG      ((*options == ':') ? (int)':' : (int)'?')
#define INORDER     (int)1

#define EMSG        ""

#define NO_PREFIX   (-1)
#define D_PREFIX    0
#define DD_PREFIX   1
#define W_PREFIX    2

char *optarg;
int   optind, opterr = 1, optopt;

static unsigned debugf_line;

#define DEBUGF_LEVEL 3

#undef  DEBUGF
#define DEBUGF(...)                                    \
        do {                                           \
          if (opt.debug >= DEBUGF_LEVEL) {             \
             debug_printf ("getopt_long.c(%u): ",      \
                           debugf_line ? debugf_line : \
                           __LINE__);                  \
             debug_printf (__VA_ARGS__);               \
             debugf_line = 0;                          \
          }                                            \
        } while (0)

/** Local globals.
 */
static const char *place = EMSG; /**< option letter processing */

static int nonopt_start = -1;    /**< first non option argument (for permute) */
static int nonopt_end   = -1;    /**< first option after non options (for permute) */
static int dash_prefix  = NO_PREFIX;

/** Error messages.
 */
static const char recargchar[]   = "option requires an argument -- %c";
static const char illoptchar[]   = "illegal option -- %c"; /* From P1003.2 */

static const char gnuoptchar[]   = "invalid option -- %c";
static const char recargstring[] = "option `%s%s' requires an argument";
static const char ambig[]        = "option `%s%.*s' is ambiguous";
static const char noarg[]        = "option `%s%.*s' doesn't allow an argument";
static const char illoptstring[] = "unrecognized option `%s%s'";

/**
 * Compute the greatest common divisor of a and b.
 */
static int gcd (int a, int b)
{
  int c;

  c = a % b;
  while (c != 0)
  {
    a = b;
    b = c;
    c = a % b;
  }
  return (b);
}

/**
 * Exchange the block from `nonopt_start` to `nonopt_end` with the block
 * from `nonopt_end` to `opt_end` (keeping the same order of arguments
 * in each block).
 */
static void permute_args (int panonopt_start, int panonopt_end,
                          int opt_end, char * const *nargv)
{
  int   cstart, cyclelen, i, j, ncycle, nnonopts, nopts, pos;
  char *swap;

  /* compute lengths of blocks and number and size of cycles
   */
  nnonopts = panonopt_end - panonopt_start;
  nopts    = opt_end - panonopt_end;
  ncycle   = gcd (nnonopts, nopts);
  cyclelen = (opt_end - panonopt_start) / ncycle;

  for (i = 0; i < ncycle; i++)
  {
    cstart = panonopt_end + i;
    pos = cstart;
    for (j = 0; j < cyclelen; j++)
    {
      if (pos >= panonopt_end)
           pos -= nnonopts;
      else pos += nopts;

      swap = nargv[pos];
      ((char**) nargv)[pos]   = nargv[cstart];
      ((char**)nargv)[cstart] = swap;
    }
  }
}

/**
 * Print a warning to `stderr`.
 */
static void warnx (const char *fmt, ...)
{
  extern char *program_name;
  va_list ap;

  va_start (ap, fmt);
  fprintf (stderr, "%s: ", program_name);
  vfprintf (stderr, fmt, ap);
  fprintf (stderr, "\n");
  va_end (ap);
}

/**
 * Parse long options in `argc` / `argv` argument vector.
 *
 * \retval -1     if `short_too` is set and the option does not match `long_options`.
 * \retval BADCH  if no match found.
 * \retval BADARG if option is missing required argument.
 * \retval 0      if option and possibly an argument was found.
 */
static int parse_long_options (char *const *nargv, const char *options,
                               const struct option *long_options,
                               int *idx, int short_too, int flags)
{
  const char *current_argv, *has_equal;
  const char *current_dash;
  size_t      current_argv_len;
  int         i, match, exact_match, second_partial_match;

  current_argv = place;
  switch (dash_prefix)
  {
    case D_PREFIX:
         current_dash = "-";
         break;
    case DD_PREFIX:
         current_dash = "--";
         break;
    case W_PREFIX:
         current_dash = "-W ";
         break;
    default:
         current_dash = "";
         break;
  }

  match = -1;
  exact_match = 0;
  second_partial_match = 0;

  optind++;

  has_equal = strchr (current_argv, '=');
  if (!has_equal)
      has_equal = strchr (current_argv, ':');

  if (has_equal)
  {
    /* argument found (--option=arg)
     */
    current_argv_len = has_equal - current_argv;
    has_equal++;
  }
  else
    current_argv_len = strlen (current_argv);

  for (i = 0; long_options[i].name; i++)
  {
    /* find matching long option
     */
    if (strncmp(current_argv, long_options[i].name, current_argv_len))
       continue;

    if (strlen(long_options[i].name) == current_argv_len)
    {
      /* exact match */
      match = i;
      exact_match = 1;
      break;
    }

    /* If this is a known short option, don't allow
     * a partial match of a single character.
     */
    if (short_too && current_argv_len == 1)
       continue;

    if (match == -1)        /* first partial match */
        match = i;
    else if ((flags & FLAG_LONGONLY) || long_options[i].has_arg != long_options[match].has_arg ||
             long_options[i].flag != long_options[match].flag ||
             long_options[i].val != long_options[match].val)
        second_partial_match = 1;
  }

  if (!exact_match && second_partial_match)
  {
    /* ambiguous abbreviation */
    if (PRINT_ERROR)
       warnx (ambig, current_dash, (int)current_argv_len, current_argv);
    optopt = 0;
    return (BADCH);
  }

  if (match != -1)       /* option found */
  {
    if (long_options[match].has_arg == no_argument && has_equal)
    {
      if (PRINT_ERROR)
         warnx (noarg, current_dash, (int)current_argv_len, current_argv);

      if (long_options[match].flag == NULL)
           optopt = long_options[match].val;
      else optopt = 0;
      return (BADCH);
    }

    if (long_options[match].has_arg == required_argument ||
        long_options[match].has_arg == optional_argument)
    {
      if (has_equal)
         optarg = (char*) has_equal;
      else if (long_options[match].has_arg == required_argument)
      {
        /* optional argument doesn't use next nargv
         */
        optarg = nargv[optind++];
      }
    }

    if ((long_options[match].has_arg == required_argument) && !optarg)
    {
      /* Missing argument; leading `:` indicates no error should be generated.
       */
      if (PRINT_ERROR)
         warnx (recargstring, current_dash, current_argv);

      if (long_options[match].flag == NULL)
           optopt = long_options[match].val;
      else optopt = 0;
      --optind;
      return (BADARG);
    }
  }
  else        /* unknown option */
  {
    if (short_too)
    {
      --optind;
      return (-1);
    }
    if (PRINT_ERROR)
       warnx (illoptstring, current_dash, current_argv);
    optopt = 0;
    return (BADCH);
  }

  if (idx)
     *idx = match;

  if (long_options[match].flag)
  {
    *long_options[match].flag = long_options[match].val;
    return (0);
  }
  return (long_options[match].val);
}

/**
 * Parse `argc` / `argv` argument vector.
 * Called by user level routines.
 */
static int getopt_internal (int nargc, char * const *nargv,
                            const char *options,
                            const struct option *long_options,
                            int *idx, int flags)
{
  char *oli;                /* option letter list index */
  int   optchar, short_too;
  int   posixly_correct;    /* no static, can be changed on the fly */

  if (options == NULL)
     return (-1);

  /* Disable GNU extensions if POSIXLY_CORRECT is set or options
   * string begins with a `+`.
   */
  posixly_correct = (getenv("POSIXLY_CORRECT") != NULL);

  if (*options == '-')
     flags |= FLAG_ALLARGS;
  else
  if (posixly_correct || *options == '+')
     flags &= ~FLAG_PERMUTE;

  if (*options == '+' || *options == '-')
     options++;

  /* Some GNU programs (like cvs) set `optind` to 0 instead of
   * using `optreset`.  Work around this brain-damage.
   */
  if (optind == 0)
     optind = 1;

  optarg = NULL;

start:
  if (!*place)              /* update scanning pointer */
  {
    if (optind >= nargc)    /* end of argument vector */
    {
      place = EMSG;
      if (nonopt_end != -1)
      {
        /* do permutation, if we have to
         */
        permute_args (nonopt_start, nonopt_end, optind, nargv);
        optind -= nonopt_end - nonopt_start;
      }
      else if (nonopt_start != -1)
      {
        /* If we skipped non-options, set `optind` to the first of them.
         */
        optind = nonopt_start;
      }
      nonopt_start = nonopt_end = -1;
      return (-1);
    }

    if (*(place = nargv[optind]) != '-' || place[1] == '\0')
    {
      place = EMSG;       /* found non-option */
      if (flags & FLAG_ALLARGS)
      {
        /* GNU extension:
         * return non-option as argument to option 1
         */
        optarg = nargv[optind++];
        return (INORDER);
      }

      if (!(flags & FLAG_PERMUTE))
      {
        /* If no permutation wanted, stop parsing at first non-option.
         */
        return (-1);
      }

      /* do permutation
       */
      if (nonopt_start == -1)
          nonopt_start = optind;
      else if (nonopt_end != -1)
      {
        permute_args (nonopt_start, nonopt_end, optind, nargv);
        nonopt_start = optind - (nonopt_end - nonopt_start);
        nonopt_end = -1;
      }
      optind++;

      /* process next argument
       */
      goto start;
    }

    if (nonopt_start != -1 && nonopt_end == -1)
      nonopt_end = optind;

    /* If we have "-" do nothing, if "--" we are done.
     */
    if (place[1] != '\0' && *++place == '-' && place[1] == '\0')
    {
      optind++;
      place = EMSG;

      /* We found an option (--), so if we skipped non-options, we have to permute.
       */
      if (nonopt_end != -1)
      {
        permute_args (nonopt_start, nonopt_end, optind, nargv);
        optind -= nonopt_end - nonopt_start;
      }
      nonopt_start = nonopt_end = -1;
      return (-1);
    }
  }

  /* Check long options if:
   *  1) we were passed some.
   *  2) the arg is not just "-".
   *  3) either the arg starts with -- we are `getopt_long_only()`.
   */
  if (long_options && place != nargv[optind] && (*place == '-' || (flags & FLAG_LONGONLY)))
  {
    short_too = 0;
    dash_prefix = D_PREFIX;
    if (*place == '-')
    {
      place++;     /* --foo long option */
      dash_prefix = DD_PREFIX;
    }
    else if (*place != ':' && strchr(options, *place) != NULL)
      short_too = 1;      /* could be short option too */

    optchar = parse_long_options (nargv, options, long_options, idx, short_too, flags);
    if (optchar != -1)
    {
      place = EMSG;
      return (optchar);
    }
  }

  if ((optchar = (int)*place++) == (int)':' || (optchar == (int)'-' && *place != '\0') ||
      (oli = strchr(options, optchar)) == NULL)
  {
    /* If the user specified `-` and `-` isn't listed in
     * options, return -1 (non-option) as per POSIX.
     * Otherwise, it is an unknown option character (or `:`).
     */
    if (optchar == (int)'-' && *place == '\0')
       return (-1);

    if (!*place)
       ++optind;

    if (PRINT_ERROR)
       warnx (posixly_correct ? illoptchar : gnuoptchar, optchar);
    optopt = optchar;
    return (BADCH);
  }

  if (long_options && optchar == 'W' && oli[1] == ';')
  {
    /* -W long-option
     */
    if (*place)                   /* no space */
       ;                          /* NOTHING */
    else if (++optind >= nargc)   /* no arg */
    {
      place = EMSG;
      if (PRINT_ERROR)
         warnx (recargchar, optchar);
      optopt = optchar;
      return (BADARG);
    }
    else               /* white space */
      place = nargv [optind];

    dash_prefix = W_PREFIX;
    optchar = parse_long_options (nargv, options, long_options, idx, 0, flags);
    place = EMSG;
    return (optchar);
  }

  if (*++oli != ':')           /* doesn't take argument */
  {
    if (!*place)
        ++optind;
  }
  else                         /* takes (optional) argument */
  {
    optarg = NULL;
    if (*place)                /* no white space */
        optarg = (char*) place;
    else if (oli[1] != ':')    /* arg not optional */
    {
      if (++optind >= nargc)   /* no arg */
      {
        place = EMSG;
        if (PRINT_ERROR)
           warnx (recargchar, optchar);
        optopt = optchar;
        return (BADARG);
      }
      else
        optarg = nargv[optind];
    }
    place = EMSG;
    ++optind;
  }

  /* dump back option letter
   */
  return (optchar);
}

/**
 * Parse `argc` / `argv` argument vector.
 */
int getopt (int nargc, char * const *nargv, const char *options)
{
  /** We don't pass `FLAG_PERMUTE` to `getopt_internal()` since
   *  the BSD getopt(3) (unlike GNU) has never done this.
   */
  return getopt_internal (nargc, nargv, options, NULL, NULL, 0);
}

/**
 * Parse `argc` / `argv` argument vector.
 */
int getopt_long (int nargc, char * const *nargv, const char *options,
                 const struct option *long_options, int *idx)
{
  return getopt_internal (nargc, nargv, options, long_options, idx,
                          FLAG_PERMUTE);
}

/**
 * Parse `argc` / `argv` argument vector.
 */
int getopt_long_only (int nargc, char * const *nargv, const char *options,
                      const struct option *long_options, int *idx)
{
  return getopt_internal (nargc, nargv, options, long_options, idx,
                          FLAG_PERMUTE|FLAG_LONGONLY);
}

/*
 * If needs 'getopt()' or 'getopt_long()' only,
 * do not compile the below code.
 */
#if !defined(NO_GETOPT_PARSE)

/**
 * Dummy function used when caller did not set his own `set_option` functions
 * to set either a short or long option.
 */
static void dummy_set_opt (int o, const char *arg)
{
  ARGSUSED (o);
  ARGSUSED (arg);
}

/**
 * Read a `file` and return it as one `wchar_t` string.
 * The `file` is known to exist.
 *
 *  + Open it in read-mode / binary.
 *  + Determine it's length.
 *  + Allocate space for it.
 *  + Read into `c->file_wbuf` one by one character.
 *
 * One unescaped file-line is one element. Escaped lines (like `\"foo\"`)
 * will be taken care of by `CommandLineToArgvW()`.
 *
 * A file like:
 * \code
 *   element-1 element-2 element-3 \"element-3a\" "element-3b"
 *   element-4
 *   "element-4a element-4b"
 *   "element \"number\" 5"
 * \endcode
 *
 * shall be turned into a vector (`wfileV[]` below) with these wide-char elements:
 * \code
 *   element-1              at wfileV [0]
 *   element-2
 *   element-3
 *   "element-3a"
 *   element-3b
 *   element-4
 *   element-4a element-4b
 *   element "number" 5     at wfileV [7]
 * \endcode
 */
static void read_file_as_wchar_t (struct command_line *c, const char *file)
{
  FILE *f = fopen (file, "rb");
  long  i, flen = filelength (fileno(f));
  int   ch;
  BOOL  escpaped = FALSE;

  DEBUGF ("filelength: %lu.\n", flen);
  c->file_wbuf = MALLOC (2*(flen+1));
  for (i = 0; i < flen; i++)
  {
    ch = fgetc (f);
    if (ch == EOF)
       break;

    if (ch == '\\')
       escpaped ^= 1;
    if (!escpaped && (ch == '\r' || ch == '\n'))
       ch = ' ';
    c->file_wbuf[i] = (wchar_t) ch;
  }
  c->file_wbuf[i] = L'\0';
  fclose (f);
}

static void dump_argv (const struct command_line *c, unsigned line)
{
  const char *p;
  int   i;

  debugf_line = line;
  DEBUGF ("c->argc: %d\n", c->argc);
  for (i = 0; c->argv && i <= c->argc; i++)
  {
    p = c->argv[i];
    debugf_line = line;
    DEBUGF ("c->argv[%2d]: %-40.40s (0x%p)\n",
            i, (p && IsBadReadPtr(p,40)) ? "<bogus>" : p, p);
  }
}

/**
 * Parse the short and long options from these sources in order:
 *  + the `c->env_opt` variable.
 *  + the command-line given by `GetCommandLineW()`.
 *  + any elements found in a `"@response_file"`.
 *
 * Match elements in all sources against the `c->short_opt` and `c->long_opt`
 * and call corresponding `c->set_short_opt` and `c->set_long_opt` functions.
 *
 * A command-line like this should be legal:
 *  \code
 *    program --arg1 @response-file-1 --arg2 @response-file-2 --arg3
 *  \endcode
 *
 * \param[in] c  The structure defining how the command-line is to be set and parsed.
 *
 * \note the reason for converting the command-line into a `wchar_t` array, is that
 *       there is no `CommandLineToArgvA()` function. Only `CommandLineToArgvW()`.
 */
void getopt_parse (struct command_line *c)
{
  int         i, j, k, l;
  int         wargC  = 0;       /* wide count of cmd-line elements */
  int         wenvC  = 0;       /* wide count of `c->env_opt elements` */
  int         wfileC = 0;       /* wide count of response-file elements */
  wchar_t   **wargV  = NULL;    /* wide array of cmd-line elements */
  wchar_t   **wenvV  = NULL;    /* wide array of `c->env_opt` elements */
  wchar_t   **wfileV = NULL;    /* wide array of response-file elements */
  const char *env, *file;
  wchar_t    *cmd = GetCommandLineW();
  wchar_t     wenv_buf [1000];
  UINT        acp  = GetConsoleCP();
  size_t      wlen;

  set_option set_short_opt = c->set_short_opt;
  set_option set_long_opt  = c->set_long_opt;

  c->file_wbuf = NULL;

  if (!set_short_opt)
     set_short_opt = dummy_set_opt;

  if (!set_long_opt)
     set_long_opt = dummy_set_opt;

  if (c->env_opt)
  {
    env = getenv (c->env_opt);
    if (env)
    {
      wenv_buf[0] = L'\0';
      MultiByteToWideChar (CP_ACP, 0, env, -1, wenv_buf, DIM(wenv_buf));
      wenvV = CommandLineToArgvW (wenv_buf, &wenvC);
    }
  }

  if (wcsstr(cmd,L" -ddd"))    /* because getopt_long hasn't been called yet */
     opt.debug = DEBUGF_LEVEL;

  wargV = CommandLineToArgvW (cmd, &wargC);
  c->argc  = wargC + wenvC;
  c->argv  = CALLOC (sizeof(char*), c->argc + 1);
  c->argc0 = 0;

  DEBUGF ("c->argc: %d\n", c->argc);

  for (i = j = k = l = 0; i < c->argc; )
  {
    wchar_t *warg = NULL;
    char    *aarg;

    if (i > 0)
    {
      if (wenvV && k < wenvC)         /* pick one arg from the `c->env_opt` */
         warg = wenvV [k++];

      else if (wfileV && l < wfileC)  /* pick one arg from the response-file */
         warg = wfileV[l++];
    }

    if (!warg && wargV)
    {
      /* pick one arg from command-line */
      warg = wargV [j++];
    }

    DEBUGF ("i: %2d, j: %2d, k: %2d, l: %2d, c->argc: %2d, warg: '%" WIDESTR_FMT "'.\n",
            i, j, k, l, c->argc, warg);

    /* We reached the end of all sources
     */
    if (warg == NULL)
    {
      c->argv [i] = NULL;
      break;
    }

    wlen = WideCharToMultiByte (acp, 0, warg, (int)wcslen(warg), 0, 0, 0, 0);
    aarg = MALLOC (2 * (wlen+1));
    WideCharToMultiByte (acp, 0, warg, (int)wcslen(warg), aarg, (int)wlen, 0, 0);
    aarg [wlen] = '\0';

    /* The cmd-line contains a response file. If it doesn't exist, simply add
     * `@file` to `c->argv[i]`.
     * We do not support a `@file` inside a response-file; it will be passed
     * on as-is.
     */
    if (!c->file_wbuf && aarg[0] == '@' && FILE_EXISTS(aarg+1))
    {
      file = aarg + 1;
      read_file_as_wchar_t (c, file);
      wfileV = CommandLineToArgvW (c->file_wbuf, &wfileC);

      DEBUGF ("file: %s, wfileV: 0x%p, wfileC: %d\n", file, wfileV, wfileC);

      if (wfileV && wfileC > 0)  /* Insert `wfileV[]` on next loop(s)  */
      {
        c->argc--;    /* since `@file` argument shall be dropped from `c->argv[]` */
        c->argv = REALLOC (c->argv, sizeof(char*) * (c->argc + wfileC + 1));
        c->argv [c->argc + wfileC] = NULL;
        FREE (aarg);
        dump_argv (c, __LINE__);
        c->argc += wfileC;
        continue;
      }
    }

    c->argv [i++] = aarg;

    /* Do we need this anymore?
     */
    if (wfileV && l == wfileC)
    {
      FREE (c->file_wbuf);
      LocalFree (wfileV);
      wfileV = NULL;
    }
  }

  if (wenvV)
     LocalFree (wenvV);

  if (wargV)
     LocalFree (wargV);

  dump_argv (c, __LINE__);

  /**
   * Use a `"+"` first in `getopt_long()` options. This will disable the
   * GNU extensions that allow non-options to come before options.
   * E.g. a command-line like:
   *      \code
   *        envtool --path foo* -d
   *      \endcode
   *
   *      is equivalent to:
   *      \code
   *        envtool --path -d foo*
   *      \endcode
   *
   * We do not want that since whatever comes after `"foo*"` should be
   * pointed to by `c->argv[c->argc0]`.
   * See `py_execfile()` for an example.
   */

  opt.debug = 0;  /* let `getopt_long()` set this again */

  while (1)
  {
    int index = 0;

    i = getopt_long (c->argc, c->argv, c->short_opt, c->long_opt, &index);
    if (i == 0)
       (*set_long_opt) (index, optarg);
    else if (i > 0)
       (*set_short_opt) (i, optarg);
    else if (i == -1)
       break;
  }

  DEBUGF ("c->argc: %d, optind: %d\n", c->argc, optind);

  if (c->argc > optind)
     c->argc0 = optind;
}

/**
 * Free the data allocated in `getopt_parse()`.
 */
void getopt_free (struct command_line *c)
{
  char *p;
  int   i;

  dump_argv (c, __LINE__);

  for (i = 0; i < c->argc; i++)
  {
    p = c->argv[i];
    FREE (p);
    c->argv[i] = NULL;
  }
  FREE (c->argv);
  FREE (c->file_wbuf);
}
#endif /* NO_GETOPT_PARSE */

