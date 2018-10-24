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
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
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

#ifndef _GETOPT_LONG_H_
#define _GETOPT_LONG_H_

/** \file    getopt_long.h
 *  \ingroup Misc
 *
 * GNU-like getopt_long() / getopt_long_only() with 4.4BSD optreset extension.
 * getopt() is declared here too for GNU programs.
 */
#define no_argument        0   /**< \def no_argument */
#define required_argument  1   /**< \def required_argument */
#define optional_argument  2   /**< \def required_argument */

/**\struct option
 */
struct option {
       const char *name; /**< name of long option */

       /**
        * one of `no_argument`, `required_argument` or `optional_argument`:<br>
        * whether option takes an argument.
        */
       int  has_arg;
       int *flag;    /**< if not NULL, set *flag to val when option found */
       int  val;     /**< if flag not NULL, value to set `*flag` to; else return value */
    };

int getopt_long (int, char * const *, const char *,
                 const struct option *, int *);

int getopt_long_only (int, char * const *, const char *,
                      const struct option *, int *);

int getopt (int nargc, char * const *nargv, const char *options);

/* getopt(3) external variables
 */
extern char *optarg;  /**< the argument to an option in `optsstring`. */
extern int   optind;  /**< the index of the next element to be processed in `argv`. */
extern int   opterr;  /**< if caller set this to zero, an error-message will never be printed. */
extern int   optopt;  /**< on errors, an unrecognised option character is stored in `optopt`. */

/**\typedef void (*set_option) (int o, const char *arg);
 * The function-type to set a short or long option.
 */
typedef void (*set_option) (int o, const char *arg);

/**\typedef struct command_line
 * The structure used by getopt_parse()
 */
typedef struct command_line {
        /*
         * Set on input by the caller.
         */
        const char          *env_opt;        /**< Take options from an environment variable? */
        const char          *short_opt;      /**< Short options for 'getopt_long()' */
        const struct option *long_opt;       /**< Long options for 'getopt_long()' */
        set_option           set_short_opt;  /**< Callback for setting a short option */
        set_option           set_long_opt;   /**< Callback for setting a long option */

        /*
         * Set on output by getopt_parse().
         */
        int      argc;       /**< Number of arguments from 'GetCommandLineW()' */
        char   **argv;       /**< All arguments from 'GetCommandLineW()' are stored here */
        int      argc0;      /**< The index of the first non-option in 'argv[]' */
        wchar_t *file_wbuf;  /**< Scratch-buffer for reading a response-file */
      } command_line;

void getopt_parse (struct command_line *cmd_line);
void getopt_free (struct command_line *cmd_line);

#endif  /* !_GETOPT_LONG_H_ */
