/** \file    Everything_ETP.c
 *  \ingroup EveryThing_ETP
 *  \brief
 *    Functions for remote queries using EveryThing's FTP protocol.
 *
 *   ref: https://www.voidtools.com/support/everything/etp/    <br>
 *   ref: https://www.voidtools.com/forum/viewtopic.php?t=1790 <br>
 *
 * To do a remote EveryThing search, connect to a remote ETP-server, login,
 * send the `EVERYTHING SEARCH` and parameters.
 *
 * The protocol goes something like this (mscgen tool diagram):
 *
 * \msc "ETP Protocol" width=25cm height=20cm
 *
 *   arcgradient="4", hscale="2";
 *
 *   a[label="Client"], b[label="Server"], c[label="Server error"], d[label="~/.authinfo lookup"], e[label="~/.netrc lookup"], f[label="~/envtool.cfg lookup"];
 *   a => d [label="Check ~/.authinfo",                            url="\ref state_authinfo_lookup"];
 *   d => e [label="Not found, check ~/.netrc",                    url="\ref state_netrc_lookup"];
 *   e => a [label="Host found",                                   url="\ref state_send_login"];
 *   e => f [label="Not found, check ~/envtool.cfg",               url="\ref state_envtool_cfg_lookup"];
 *   f => a [label="Host found",                                   url="\ref state_send_login"];
 *   f => a [label="Not found (try to login anuway)",              url="\ref state_send_login"];
 *   a => b [label="USER user",                                    url="\ref state_send_login"];
 *   a <= b [label="230 Welcome to Everything ETP/FTP",            url="\ref state_send_login"];
 *   a <= b [label="331 Password required for user",               url="\ref state_await_login"];
 *   a => b [label="PASS password",                                url="\ref state_send_pass"];
 *   c <= b [label="530 Login or password incorrect!",             url="\ref state_await_login"];
 *   a <= b [label="230 Logged on.",                               url="\ref state_await_login"];
 *   a => b [label="EVERYTHING REGEX 1",                           url="\ref state_send_query"];
 *   a => b [label="EVERYTHING SEARCH filespec",                   url="\ref state_send_query"];
 *   a => b [label="EVERYTHING QUERY",                             url="\ref state_send_query"];
 *   a <= b [label="RESULT_COUNT N",                               url="\ref state_RESULT_COUNT"];
 *   a <= b [label="PATH file 1",                                  url="\ref state_PATH"];
 *   ---    [label="for all files until N paths received",         url="\ref state_PATH"];
 *   a <= b [label="200 End",                                      url="\ref state_closing"];
 *
 * \endmsc
 *
 * Or in plain notation:
 * \code
 *    C -> USER anonymous (or <none>)
 *    C -> EVERYTHING SEARCH <spec>
 *    C -> EVERYTHING PATH_COLUMN 1
 *    C -> EVERYTHING SIZE_COLUMN 1
 *    C -> EVERYTHING DATE_MODIFIED_COLUMN 1
 *    C -> EVERYTHING QUERY
 *    ...
 *    S ->  200-Query results
 *    S ->  RESULT_COUNT 3
 *    ...
 *    S -> 200 End.
 * \endcode
 *
 * Which can be accomplished with a `test-etp.bat` file and the Windows ftp client:
 * \code
 *  :: the test-etp.bat file
 *  @echo off
 *  echo USER                                     > etp-commands
 *  echo QUOTE EVERYTHING SEARCH notepad.exe     >> etp-commands
 *  echo QUOTE EVERYTHING PATH_COLUMN 1          >> etp-commands
 *  echo QUOTE EVERYTHING SIZE_COLUMN 1          >> etp-commands
 *  echo QUOTE EVERYTHING DATE_MODIFIED_COLUMN 1 >> etp-commands
 *  echo QUOTE EVERYTHING QUERY                  >> etp-commands
 *  echo BYE                                     >> etp-commands
 * \endcode
 *
 * And when executed like: `c:\> test-etp.bat & ftp -s:etp-commands 10.0.0.37`, should give:
 *
 * \code
 *  Connected to 10.0.0.37.
 *  220 Welcome to Everything ETP/FTP
 *  530 Not logged on.
 *  User (10.0.0.37:(none)):
 *  230 Logged on.
 *  ftp> QUOTE EVERYTHING SEARCH notepad.exe
 *  200 Search set to (notepad.exe).
 *  ftp> QUOTE EVERYTHING PATH_COLUMN 1
 *  200 Path column set to (1).
 *  ftp> QUOTE EVERYTHING QUERY
 *  200-Query results
 *   RESULT_COUNT 3
 *   PATH C:\Windows
 *   SIZE 236032
 *   DATE_MODIFIED 131343347638616569
 *   FILE notepad.exe
 *   PATH C:\Windows\System32
 *   SIZE 236032
 *   DATE_MODIFIED 131343347658304156
 *   FILE notepad.exe
 *   PATH C:\Windows\WinSxS\x86_microsoft-windows-notepad_31bf3856ad364e35_10.0.15063.0_none_240fcb30f07103a5
 *   SIZE 236032
 *   DATE_MODIFIED 131343347658304156
 *   FILE notepad.exe
 *  200 End.
 *  ftp> BYE
 *  221 Goodbye.
 * \endcode
 *
 * This file is part of envtool.
 *
 * By Gisle Vanem <gvanem@yahoo.no> August 2017.
 */
#include <stdio.h>
#include <stdlib.h>

#if defined(__CYGWIN__) && !defined(__USE_W32_SOCKETS)
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <sys/ioctl.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <errno.h>

  #define CYGWIN_POSIX
#else

  /** Suppress warning for `inet_addr()` and `gethostbyname()`
   */
  #define _WINSOCK_DEPRECATED_NO_WARNINGS
  #include <winsock2.h>
  #include <windows.h>
#endif

#include "color.h"
#include "envtool.h"
#include "auth.h"
#include "report.h"
#include "Everything_ETP.h"

/**\def CONN_TIMEOUT
 * the `connect()` timeout for a non-blocking connection.
 */
#ifndef CONN_TIMEOUT
#define CONN_TIMEOUT 3000   /* 3 sec */
#endif

/**\def RECV_TIMEOUT
 * the `SO_RCVTIMEO` used in `setsockopt()`
 */
#ifndef RECV_TIMEOUT
#define RECV_TIMEOUT 2000   /* 2 sec */
#endif

/**\def MAX_RECV_BUF
 * size of receive buffer
 */
#ifndef MAX_RECV_BUF
#define MAX_RECV_BUF (16*1024)
#endif

#if defined(CYGWIN_POSIX)
  #define SOCKET             int
  #define INVALID_SOCKET     -1
  #undef  WSAETIMEDOUT
  #define WSAETIMEDOUT       ETIMEDOUT
  #define WSAGetLastError()  errno
  #define WSASetLastError(e) errno = e
  #define closesocket(s)     close(s)
#endif

DWORD ETP_total_rcv;
DWORD ETP_num_evry_dups;

/* Forward definition.
 */
struct state_CTX;

/**
 * \typedef ETP_state
 * All state-functions must match this function-type.
 */
typedef BOOL (*ETP_state) (struct state_CTX *ctx);

/** \struct IO_buf
 * Buffered I/O stream
 */
struct IO_buf {
       char   buffer [MAX_RECV_BUF];   /**< the input/trace buffer */
       char  *buffer_pos;              /**< current position in the buffer */
       size_t buffer_left;             /**< number of bytes left in the buffer: <br>
                                        * `buffer_left = buffer_end - buffer_pos`
                                        */
       int    buffer_read;             /**< used by `rbuf_read_char()` */
     };

/**
 * \struct state_CTX
 * The context used throughout the ETP transfer.
 * Keeping this together should make the ETP transfer
 * fully reentrant.
 */
struct state_CTX {
       ETP_state          state;            /**< Current state function */
       struct sockaddr_in sa;               /**< Used in state_resolve(), state_blocking_connect() and state_non_blocking_connect() */
       SOCKET             sock;             /**< Set in state_blocking_connect() and state_non_blocking_connect() */
       int                port;             /**< The server destination port to use */
       char              *raw_url;          /**< URL on raw form given in do_check_evry_ept() */
       char               hostname [200];   /**< Name of host to connect to */
       char               username [30];    /**< Name of user */
       char               password [30];    /**< And his password (if any) */
       BOOL               use_netrc;        /**< Use the `%APPDATA%/.netrc` file */
       BOOL               use_authinfo;     /**< Use the `%APPDATA%/.authinfo` file */
       DWORD              timeout;          /**< The socket timeout (= `RECV_TIMEOUT`) */
       int                retries;          /**< The retry counter; between 0 and `MAX_RETRIES` */
       int                ws_err;           /**< Last `WSAGetError()` */
       unsigned           results_expected; /**< The number of matches we're expecting */
       unsigned           results_got;      /**< The number of matches we got */
       unsigned           results_ignore;   /**< The number of matches we ignored */
       struct IO_buf      recv;             /**< The `IO_buf` for reception */
       struct IO_buf      trace;            /**< The `IO_buf` for tracing the protocol */

       /* These are set in state_PATH().
        */
       time_t             mtime;             /**< The `time_t` value of `path` */
       UINT64             fsize;             /**< The file-size value of `path` (`(INT64)-1` for folders) */
       char               path [_MAX_PATH];  /**< The name of file or folder */
     };

/**
 * Local functions; prototypes.
 */
static int         parse_host_spec      (struct state_CTX *ctx, const char *pattern, ...);
static void        connect_common_init  (struct state_CTX *ctx, const char *which_state);
static void        connect_common_final (struct state_CTX *ctx, int err);
static void        set_nonblock         (SOCKET sock, DWORD non_block);
static int         rbuf_read_char  (struct state_CTX *ctx, char *store);
static const char *ETP_tracef      (struct state_CTX *ctx, const char *fmt, ...);
static const char *ETP_state_name  (ETP_state f);

static BOOL state_init                 (struct state_CTX *ctx);
static BOOL state_exit                 (struct state_CTX *ctx);
static BOOL state_parse_url            (struct state_CTX *ctx);
static BOOL state_netrc_lookup         (struct state_CTX *ctx);
static BOOL state_authinfo_lookup      (struct state_CTX *ctx);
static BOOL state_send_login           (struct state_CTX *ctx);
static BOOL state_send_pass            (struct state_CTX *ctx);
static BOOL state_await_login          (struct state_CTX *ctx);
static BOOL state_send_query           (struct state_CTX *ctx);
static BOOL state_200                  (struct state_CTX *ctx);
static BOOL state_RESULT_COUNT         (struct state_CTX *ctx);
static BOOL state_PATH                 (struct state_CTX *ctx);
static BOOL state_closing              (struct state_CTX *ctx);
static BOOL state_resolve              (struct state_CTX *ctx);
static BOOL state_blocking_connect     (struct state_CTX *ctx);
static BOOL state_non_blocking_connect (struct state_CTX *ctx);

/**
 * Receive a response with timeout.
 * Stop when we get an `"\r\n"` terminated ASCII-line.
 *
 * * `opt.use_buffered_io == 1`:
 *    Use the buffered I/O setup in rbuf_read_char().
 *
 * * `opt.use_buffered_io == 0`:
 *   Call `recv()` for 1 character at a time! <br>
 *   This is to keep it simple; avoid the need for (a non-blocking socket and)
 *   `select()`. <br>
 *   But the `recv()` will hang if used against a non "line oriented"
 *   protocol. But only for max 2 seconds since `setsockopt()` with `SO_RCVTIMEO`
 *   was set to 2 sec.
 *
 *   Practice shows this is just a bit slower than the rbuf_read_char() method since
 *   Winsock2 provides good buffering with the above `setsockopt()` call. <br>
 *   The question is if this causes a higher number of kernel switches. And thus
 *   lower performance on slower CPUs.
 *
 * \param[in] ctx      the context we work with.
 * \param[in] out_buf  the location of the buffer to save too.
 * \param[in] out_len  the length of the buffer for saving.
 */
static char *recv_line (struct state_CTX *ctx, char *out_buf, size_t out_len)
{
  int    rc, num;
  size_t i;
  char   ch, *start = out_buf;

  for (i = num = 0; i < out_len; i++)
  {
    if (opt.use_buffered_io)
         rc = rbuf_read_char (ctx, &ch);
    else rc = recv (ctx->sock, &ch, 1, 0);
    if (rc == 1)
    {
      num++;
      *out_buf++ = ch;
      if (ch == '\n')    /* Assumes `\r` already was received */
         break;
    }
    if (rc <= 0)
    {
      ctx->ws_err = WSAGetLastError();
      break;
    }
  }

  ETP_total_rcv += num;
  *out_buf = '\0';
  str_strip_nl (start);
  start = str_ltrim (start);

  ETP_tracef (ctx, "Rx: \"%s\", len: %d\n", start, num);

  if (opt.use_buffered_io && opt.debug >= 3)
     ETP_tracef (ctx, "recv.buffer_left: %u: recv.buffer_pos: %u, recv.buffer_read: %d, ws_err: %d\n",
                 ctx->recv.buffer_left, ctx->recv.buffer_pos - ctx->recv.buffer,
                 ctx->recv.buffer_read, ctx->ws_err);
  return (start);
}

/**
 * Send a single-line command to the server side.
 * Do not use a `"\r\n"` termination; it will be added here.
 *
 * \param[in] ctx  the context we work with.
 * \param[in] fmt  the var-arg format of what to send.
 */
static int send_cmd (struct state_CTX *ctx, const char *fmt, ...)
{
  int     len, rc;
  char    tx_buf [500];
  va_list args;

  va_start (args, fmt);

  len = vsnprintf (tx_buf, sizeof(tx_buf)-3, fmt, args);
  tx_buf[len] = '\r';
  tx_buf[len+1] = '\n';

  rc = send (ctx->sock, tx_buf, len+2, 0);
  ETP_tracef (ctx, "Tx: \"%.*s\\r\\n\", len: %d\n", len, tx_buf, rc);
  va_end (args);
  return (rc < 0 ? -1 : 0);
}

/**
 * Print the resulting match.
 *
 * \param[in] ctx    the context we work with.
 * \param[in] name   Either a file-name or a folder-name within a `ctx->path`
 * \param[in] is_dir If `FALSE`, `name` is treated as a file-name. <br>
 *                   if `TRUE`,  `name` is treated as a folder-name.
 */
static void report_file_ept (struct state_CTX *ctx, const char *name, BOOL is_dir)
{
  if (opt.dir_mode && !is_dir)
     ctx->results_ignore++;
  else
  {
    struct report r;
    static char prev_name [_MAX_PATH];
    char   full_name [_MAX_PATH+2];

    snprintf (full_name, sizeof(full_name), "%s%c%s", ctx->path, DIR_SEP, name);

    if (!opt.dir_mode && prev_name[0] && str_equal(prev_name, full_name))
       ETP_num_evry_dups++;
    else
    {
      r.file        = full_name;
      r.content     = NULL;      /* cannot read a remote file (yet) */
      r.mtime       = ctx->mtime;
      r.fsize       = ctx->fsize;
      r.is_dir      = is_dir;
      r.is_junction = FALSE;
      r.is_cwd      = FALSE;
      r.key         = HKEY_EVERYTHING_ETP;
      report_file (&r);
    }
    _strlcpy (prev_name, full_name, sizeof(prev_name));
  }
  ctx->mtime = 0;
  ctx->fsize = 0;
  ctx->results_got++;
}

/**
 * Print a warning for a failed login.
 * Optionally print what action it will take next.
 *
 * \param[in] ctx         the context we work with.
 * \param[in] is_authinfo TRUE if the warning applies to reading of
 *                        the `~/.authinfo` file.
 */
static void login_warning (struct state_CTX *ctx, BOOL is_authinfo)
{
  const char *next_action = "\n";
  char       *file;

  if (is_authinfo)
  {
    file = getenv_expand2 ("%APPDATA%\\.authinfo");
    if (!FILE_EXISTS(file))
         WARN ("%s: file not found.", file);
    else WARN ("%s: user/password/port not found for host \"%s\".", file, ctx->hostname);

    if (ctx->use_netrc)
       next_action = " Will try %%APPDATA%%\\.netrc next.\n";
  }
  else
  {
    file = getenv_expand2 ("%APPDATA%\\.netrc");
    if (!FILE_EXISTS(file))
         WARN ("%s: file not found.", file);
    else WARN ("%s: user/password not found for host \"%s\".", file, ctx->hostname);

    if (ctx->use_authinfo)
       next_action = " Will try %%APPDATA%%\\.authinfo next.\n";
  }
  WARN  (next_action);
  FREE (file);
}

/**
 * `PATH` state: gooble up the results.
 *
 * Expect `results_expected` results set in state_RESULT_COUNT(). <br>
 * When `"200 End"` is received, enter state_closing().
 *
 * \param[in] ctx  the context we work with.
 */
static BOOL state_PATH (struct state_CTX *ctx)
{
  FILETIME ft;
  char     buf [500];
  char    *rx = recv_line (ctx, buf, sizeof(buf));

  if (!strncmp(rx, "PATH ", 5))
  {
    _strlcpy (ctx->path, rx+5, sizeof(ctx->path));
    ETP_tracef (ctx, "path: %s", ctx->path);
    return (TRUE);
  }

  if (sscanf(rx, "SIZE %" U64_FMT, &ctx->fsize) == 1)
  {
    ETP_tracef (ctx, "size: %s", str_trim((char*)get_file_size_str(ctx->fsize)));
    return (TRUE);
  }

  if (sscanf(rx, "DATE_MODIFIED %" U64_FMT, (UINT64*)&ft) == 1)
  {
    ctx->mtime = FILETIME_to_time_t (&ft);
    ETP_tracef (ctx, "mtime: %.24s", ctime(&ctx->mtime));
    return (TRUE);
  }

  if (!strncmp(rx, "FILE ", 5))
  {
    ETP_tracef (ctx, "file: %s", rx+5);
    report_file_ept (ctx, rx + 5, FALSE);
    return (TRUE);
  }

  if (!strncmp(rx, "FOLDER ", 7))
  {
    ETP_tracef (ctx, "folder: %s", rx+7);
    report_file_ept (ctx, rx+7, TRUE);
    return (TRUE);
  }

#if 0
  if (*rx == '\0')
  {
    WARN ("Truncated Receive buffer.\n");
    ctx->state = state_closing;
    return (TRUE);
  }
#endif

  if (strncmp(rx,"200 End",7) != 0)
  {
    ETP_tracef (ctx, "results_got: %lu", ctx->results_got);
    WARN ("Unexpected response: \"%s\", err: %s\n", rx, ws2_strerror(ctx->ws_err));
  }

  ctx->state = state_closing;
  return (TRUE);
}

/**
 * `RESULT_COUNT` state.
 *
 * get the `"RESULT_COUNT N\r\n"` string.
 * And enter state_PATH().
 *
 * \param[in] ctx  the context we work with.
 */
static BOOL state_RESULT_COUNT (struct state_CTX *ctx)
{
  char  buf [200];
  char *rx = recv_line (ctx, buf, sizeof(buf));

  if (sscanf(rx,"RESULT_COUNT %u", &ctx->results_expected) == 1)
  {
    ctx->state = state_PATH;
    return (TRUE);
  }
  if (!strncmp(rx,"200 End",7))  /* Premature "200 End". No results? */
  {
    ctx->state = state_closing;
    return (TRUE);
  }
  WARN ("Unexpected response: \"%s\"\n", rx);
  ctx->state = state_closing;
  return (TRUE);
}

/**
 * State entered after commands was sent.
 *
 * Swallow received lines until `"200-xx\r\n"` is received.
 * Then enter state_RESULT_COUNT().
 *
 * \param[in] ctx  the context we work with.
 */
static BOOL state_200 (struct state_CTX *ctx)
{
  char   buf [200];
  char  *rx = recv_line (ctx, buf, sizeof(buf));
  size_t len = strlen (rx);

  if (!strncmp(rx,"200-",4))
     ctx->state = state_RESULT_COUNT;
  else if (len >= 1 && *rx != '2')
  {
    WARN ("This is not an ETP server; response was: \"%s\"\n", rx);
    ctx->state = state_closing;
  }
  return (TRUE);
}

/**
 * Close the socket and enter state_exit().
 *
 * \param[in] ctx  the context we work with.
 */
static BOOL state_closing (struct state_CTX *ctx)
{
  ETP_tracef (ctx, "closesocket(%d)", ctx->sock);

  closesocket (ctx->sock);

  if (ctx->results_expected > 0 && ctx->results_got < ctx->results_expected)
     WARN ("Expected %u results, but received only %u. Received %s bytes.\n",
           ctx->results_expected, ctx->results_got, dword_str(ETP_total_rcv));

  ctx->state = state_exit;
  return (TRUE);
}

/**
 * Send the search parameters and the `"QUERY x"` command.
 *
 * If the `send()` call fails, enter state_closing().
 * Otherwise, enter state_200().
 *
 * \todo Maybe sending all these commands in one `send()`
 *       is better?
 *
 * \param[in] ctx  the context we work with.
 */
static BOOL state_send_query (struct state_CTX *ctx)
{
  /* If a raw query, send `file_spec` query as-is.
   * But as for 'Everything_SetSearch()', a 'content:foo bar' string MUST
   * be quoted.
   */
  if (opt.evry_raw)
     send_cmd (ctx, "EVERYTHING SEARCH %s", evry_raw_query());
  else
  {
    /* Always send a "REGEX 1", but translate from a shell-pattern if
     * `opt.use_regex == 0`.
     */
    send_cmd (ctx, "EVERYTHING REGEX 1");
    if (opt.use_regex)
         send_cmd (ctx, "EVERYTHING SEARCH %s", opt.file_spec);
    else send_cmd (ctx, "EVERYTHING SEARCH ^%s$", translate_shell_pattern(opt.file_spec));
  }

  send_cmd (ctx, "EVERYTHING CASE %d", opt.case_sensitive);
  send_cmd (ctx, "EVERYTHING PATH_COLUMN 1");
  send_cmd (ctx, "EVERYTHING SIZE_COLUMN 1");
  send_cmd (ctx, "EVERYTHING DATE_MODIFIED_COLUMN 1");

  if (send_cmd(ctx, "EVERYTHING QUERY") < 0)
       ctx->state = state_closing;
  else ctx->state = state_200;
  return (TRUE);
}

/**
 * Send the USER name and optionally the `ctx->password`.
 *
 * `USER` can be empty if the remote `Everything.ini` has a
 * setting like:
 * \code
 *   etp_server_username=
 * \endcode
 *
 * I.e. it ignores passwords! But if `Everthing.ini` contains:
 * \code
 *   etp_server_username=foo
 *   etp_server_password=bar
 * \endcode
 *
 * `ctx->username` and `ctx->password` **must** match `foo/bar`.
 *
 * \param[in] ctx  the context we work with.
 */
static BOOL state_send_login (struct state_CTX *ctx)
{
  char buf[200], *rx;
  int  rc;

  if (ctx->username[0] && ctx->password[0])
  {
    rc = send_cmd (ctx, "USER %s", ctx->username);
    ctx->state = state_send_pass;
  }
  else
  {
    rc = send_cmd (ctx, "USER");
    ctx->state = state_await_login;
  }

  /* Ignore the "220 Welcome to Everything..." message.
   */
  buf[0] = '\0';
  rx = recv_line (ctx, buf, sizeof(buf));

  if (*rx == '\0' || rc < 0)   /* Empty response or Tx failed! */
  {
    _strlcpy (buf, "Failure in protococl.\n", sizeof(buf));
    WARN (buf);
    ETP_tracef (ctx, buf);
    ctx->state = state_closing;
  }
  return (TRUE);
}

/**
 * We sent the `USER` / `PASS` commands.
 *
 * Await the `"230 Logged on"` message and enter state_send_query(). <br>
 * If server replies with a `"530"` message (`"Login or password incorrect"`),
 * enter state_closing().
 *
 * \param[in] ctx  the context we work with.
 */
static BOOL state_await_login (struct state_CTX *ctx)
{
  char  buf [200];
  char *rx = recv_line (ctx, buf, sizeof(buf));

  /* "230": Server accepted our login.
   */
  if (!strncmp(rx,"230",3))
  {
    ctx->state = state_send_query;
    return (TRUE);
  }

  /** Any `"5xx"` message or a timeout is fatal here; enter state_closing().
   */
  snprintf (buf, sizeof(buf), "Failed to login; USER %s.\n", ctx->username);
  WARN (buf);
  ETP_tracef (ctx, buf);
  ctx->state = state_closing;
  return (TRUE);
}

/**
 * We're prepared to send a password.
 *
 * But if server replies with `"230 Logged on"`, we know it ignores
 * passwords (dangerous!). So simply proceed to state_send_query() state.
 *
 * \param[in] ctx  the context we work with.
 */
static BOOL state_send_pass (struct state_CTX *ctx)
{
  char  buf [200];
  char *rx = recv_line (ctx, buf, sizeof(buf));

  if (!strcmp(rx,"230 Logged on."))
       ctx->state = state_send_query;   /* ETP server ignores passwords */
  else if (send_cmd(ctx, "PASS %s", ctx->password) < 0)
       ctx->state = state_closing;      /* Transmit failed; close */
  else ctx->state = state_await_login;  /* "PASS" sent okay, await loging confirmation */
  return (TRUE);
}

/**
 * Check if `ctx->hostname` is simply an IPv4-address.
 *
 * If TRUE,
 *   enter state_blocking_connect() or state_non_blocking_connect().<br>
 * otherwise,
 *   call `gethostbyname()` to get the IPv4-address. <br>
 *   Then enter state_blocking_connect() or state_non_blocking_connect().
 *
 * \param[in] ctx  the context we work with.
 *
 * \note Using `gethostbyname()` could block for a long period.
 */
static BOOL state_resolve (struct state_CTX *ctx)
{
  struct hostent *he;

  ETP_tracef (ctx, "ctx->hostname: '%s'\n", ctx->hostname);
  ETP_tracef (ctx, "ctx->username: '%s'\n", ctx->username);
  ETP_tracef (ctx, "ctx->password: '%s'\n", ctx->password);
  ETP_tracef (ctx, "ctx->port:      %u\n",  ctx->port);

  /* An 'inet_addr ("")' will on Winsock return our own IP-address!
   * Avoid that.
   */
  if (!ctx->hostname[0])
  {
    WARN ("Empty hostname!\n");
    goto fail;
  }

  /* If 'ctx->hostname' is not simply an IPv4-address, it
   * must be resolved to an IPv4-address first.
   */
  ctx->sa.sin_addr.s_addr = inet_addr (ctx->hostname);
  if (ctx->sa.sin_addr.s_addr == INADDR_NONE)
  {
    if (!opt.quiet)
        C_printf ("Resolving %s...", ctx->hostname);
    C_flush();
    he = gethostbyname (ctx->hostname);

    if (!he)
    {
      WARN (" Unknown host.\n");
      goto fail;
    }
    ctx->sa.sin_addr.s_addr = *(u_long*) he->h_addr_list[0];
    C_putc ('\r');
  }

  if (opt.use_nonblock_io)
  {
    connect_common_init (ctx, "state_non_blocking_connect");
    set_nonblock (ctx->sock, 1);
    connect (ctx->sock, (const struct sockaddr*)&ctx->sa, sizeof(ctx->sa));
    ctx->state = state_non_blocking_connect;
  }
  else
    ctx->state = state_blocking_connect;
  return (TRUE);

fail:
  ctx->state = state_closing;
  return (TRUE);
}

/**\def SELECT_TIME_USEC
 *      `select()` timeout for each try (in usec)
 */
#define SELECT_TIME_USEC (500*1000)

/**\def MAX_RETRIES
 *      Max retries is the result of `CONN_TIMEOUT` (in msec)
 */
#define MAX_RETRIES (1000 * CONN_TIMEOUT / SELECT_TIME_USEC)

/**
 * When the IPv4-address is known, perform the non-blocking `connect()`.
 *
 * The `ctx->sock` is already in non-block state.
 * Loop here until `WSAWOULDBLOCK` is no longer a result in `select()`.
 * If successful, enter state_send_login().
 *
 * \param[in] ctx  the context we work with.
 */
static BOOL state_non_blocking_connect (struct state_CTX *ctx)
{
  struct timeval tv;
  fd_set wr_fds, ex_fds;
  int    rc;

  ETP_tracef (ctx, "In %s(), retries: %d.\n", __FUNCTION__, ctx->retries);

  if (ctx->retries++ >= MAX_RETRIES)
  {
    connect_common_final (ctx, WSAETIMEDOUT);
    return (TRUE);
  }
  if (halt_flag > 0)  /* SIGINT caught */
  {
    connect_common_final (ctx, WSAECONNREFUSED); /* Any better error-code? */
    return (TRUE);
  }

  FD_ZERO (&wr_fds);
  FD_ZERO (&ex_fds);
  FD_SET (ctx->sock, &wr_fds);
  FD_SET (ctx->sock, &ex_fds);
  tv.tv_sec  = 0;
  tv.tv_usec = SELECT_TIME_USEC;

  rc = (int) select ((int)(ctx->sock+1), NULL, &wr_fds, &ex_fds, &tv);
  if (rc >= 1)
  {
    if (FD_ISSET(ctx->sock,&wr_fds))
    {
      /* socket writable; connected
       */
      connect_common_final (ctx, 0);
      set_nonblock (ctx->sock, 0);
    }
    else if (FD_ISSET(ctx->sock,&ex_fds))
    {
      /* socket exception; not connected
       */
      int opt_val = 0;
      int opt_len = sizeof(opt_val);

      getsockopt (ctx->sock, SOL_SOCKET, SO_ERROR, (char*)&opt_val, &opt_len);
      connect_common_final (ctx, opt_val);   /* Probably WSAECONNREFUSED */
      set_nonblock (ctx->sock, 0);
    }
  }
  return (TRUE);
}

/**
 * When the IPv4-address is known, perform the blocking `connect()`.
 * If successful, enter state_send_login().
 *
 * \param[in] ctx  the context we work with.
 */
static BOOL state_blocking_connect (struct state_CTX *ctx)
{
  int rc;

  connect_common_init (ctx, "state_blocking_connect");
  rc = connect (ctx->sock, (const struct sockaddr*)&ctx->sa, sizeof(ctx->sa));
  connect_common_final (ctx, rc < 0 ? WSAGetLastError() : 0);
  return (TRUE);
}

/**
 * Lookup the `USER` / `PASSWORD` for `ctx->hostname` in the `~/.netrc` file.
 *
 * Enter state_send_login() even if `USER` / `PASSWORD` are not found.
 *
 * \param[in] ctx  the context we work with.
 */
static BOOL state_netrc_lookup (struct state_CTX *ctx)
{
  const char *user  = NULL;
  const char *passw = NULL;
  BOOL        okay = netrc_lookup (ctx->hostname, &user, &passw);

  if (!okay)
     login_warning (ctx, FALSE);
  else
  {
    if (user)
       _strlcpy (ctx->username, user, sizeof(ctx->username));
    if (passw)
       _strlcpy (ctx->password, passw, sizeof(ctx->password));
  }

  ETP_tracef (ctx, "Got USER: %s and PASS: %s in '%%APPDATA%%\\.netrc' for '%s'\n",
              user ? user : "<None>", passw ? passw : "<none>", ctx->hostname);

  /* Do not attempt "~/.netrc" login ever again.
   */
  ctx->use_netrc = FALSE;

  /* If "~/.netrc" login failed, maybe try "~/.authinfo" next?
   */
  if (!okay && ctx->use_authinfo)
       ctx->state = state_authinfo_lookup;
  else ctx->state = state_resolve;
  return (TRUE);
}

/**
 * Lookup the `USER`, `PASSWORD` and `port` for `ctx->hostname` in the `~/.authinfo` file.
 *
 * Enter state_send_login() even if `USER` / `PASSWORD` are not found.
 *
 * \param[in] ctx  the context we work with.
 */
static BOOL state_authinfo_lookup (struct state_CTX *ctx)
{
  const char *user  = NULL;
  const char *passw = NULL;
  int         port = 0;
  BOOL        okay = authinfo_lookup (ctx->hostname, &user, &passw, &port);

  if (!okay)
     login_warning (ctx, TRUE);
  else
  {
    if (user)
       _strlcpy (ctx->username, user, sizeof(ctx->username));
    if (passw)
       _strlcpy (ctx->password, passw, sizeof(ctx->password));
    if (port && !ctx->port)
       ctx->port = port;
  }

  ETP_tracef (ctx, "Got USER: %s, PASS: %s and PORT: %u in '%%APPDATA%%\\.authinfo' for '%s'\n",
              user ? user : "<None>", passw ? passw : "<none>", ctx->port, ctx->hostname);

  /* Do not attempt "~/.authinfo" login ever again.
   */
  ctx->use_authinfo = FALSE;

  /* If "~/.authinfo" lookup failed, maybe try "~/.netrc" next?
   */
  if (!okay && ctx->use_netrc)
       ctx->state = state_netrc_lookup;
  else ctx->state = state_resolve;

  return (TRUE);
}

/**
 * Lookup the `USER`, `PASSWORD` and `port` for `ctx->hostname` in the `~/envtool.cfg` file.
 *
 * Enter state_send_login() even if `USER` / `PASSWORD` are not found.
 *
 * \param[in] ctx  the context we work with.
 *
 * \note Currently not used. So make it non-static to suppress a warning
 *       from clang-cl and gcc.
 */
BOOL state_envtool_cfg_lookup (struct state_CTX *ctx)
{
  ARGSUSED (ctx);
  return (FALSE);
}

/**
 * Check if `ctx->raw_url` matches one of these formats:
 *
 *  \li `user:passwd@host_or_IP-address<:port>`    Both user-name and password (+ port).
 *  \li `user@host_or_IP-address<:port>`           Only user-name (+ port).
 *  \li `host_or_IP-address<:port>`                Only host/IP-address (+ port).
 *
 * \param[in] ctx  the context we work with.
 *
 * \todo Fix the `~/.netrc` vs `~/.authinfo` preference selection:
 *       if a non-default entry is found in `~/.netrc`, do not try to use a default
 *       entry from `~/.authinfo`. Or vice versa. Hence we *must* parse both files
 *       first before we send any `USER` or `PASSWORD` commands.
 */
static BOOL state_parse_url (struct state_CTX *ctx)
{
  int n;

  ETP_tracef (ctx, "Cracking the host-spec: '%s'.\n", ctx->raw_url);

  /* Assume we must use `~/.netrc` or `~/.authifo`.
   */
  ctx->use_netrc = ctx->use_authinfo = TRUE;

  /* Check simple case of "host_or_IP-address<:port>" first.
   */
  n = parse_host_spec (ctx, "%200[^:]:%d", ctx->hostname, &ctx->port);

  if ((n == 1 || n == 2) && !strchr(ctx->raw_url,'@'))
     ctx->use_netrc = ctx->use_authinfo = TRUE;
  else
  {
    /* Check for "user:passwd@host_or_IP-address<:port>".
     */
    n = parse_host_spec (ctx, "%30[^:@]:%30[^:@]@%200[^:]:%d", ctx->username, ctx->password, ctx->hostname, &ctx->port);
    if (n == 3 || n == 4)
       ctx->use_netrc = ctx->use_authinfo = FALSE;
    else
    {
      /* Check for "user@host_or_IP-address<:port>".
       */
      n = parse_host_spec (ctx, "%30[^:@]@%200[^:@]:%d", ctx->username, ctx->hostname, &ctx->port);
      if (n == 2 || n == 3)
         ctx->use_netrc = ctx->use_authinfo = FALSE;
    }
  }

  if (ctx->use_authinfo)
  {
    /** If using a `~/.authinfo` file, enter state_authinfo_lookup().
     */
    ctx->state = state_authinfo_lookup;
  }
  else if (ctx->use_netrc)
  {
    /** If using a `~/.netrc` file, enter state_netrc_lookup().
     */
    ctx->state = state_netrc_lookup;
  }
  else
  {
    /** If a username, password (and possibly a port) was specified in the URL.<br>
     *  Thus go directly to state_resolve().
     */
    ctx->state = state_resolve;
  }
  return (TRUE);
}

/**
 * The first state-machine function we enter.
 *
 * If `CYGWIN_POSIX` is defined, simply create the TCP socket. <br>
 * Otherwise initialise Winsock and create the TCP socket.
 *
 * Enter `state_parse_url()` state.
 *
 * \param[in] ctx  the context we work with.
 */
static BOOL state_init (struct state_CTX *ctx)
{
#if !defined(CYGWIN_POSIX)
  WSADATA wsadata;

  ETP_tracef (ctx, "WSAStartup().\n");

  if (WSAStartup(MAKEWORD(1,1), &wsadata))
  {
    ctx->ws_err = WSAGetLastError();
    WARN ("Failed to start Winsock: %s.\n", ws2_strerror(ctx->ws_err));
    ctx->state = state_exit;
    return (TRUE);
  }
#endif

  ctx->sock = socket (AF_INET, SOCK_STREAM, 0);
  if (ctx->sock == INVALID_SOCKET)
  {
    char buf [500];

    ctx->ws_err = WSAGetLastError();
    snprintf (buf, sizeof(buf), "Failed to create socket: %s.\n", ws2_strerror(ctx->ws_err));
    WARN (buf);
    ETP_tracef (ctx, buf);
    ctx->state = state_exit;
    return (TRUE);
  }

#if defined(CYGWIN_POSIX)
  ETP_tracef (ctx, "state_init() okay.\n");
#endif

  ctx->state = state_parse_url;
  return (TRUE);
}

/**
 * The last state-machine function we enter.
 *
 * \param[in] ctx   the context we work with.
 * \retval    FALSE forces run_state_machine() to quit it's loop
 */
static BOOL state_exit (struct state_CTX *ctx)
{
#if !defined(CYGWIN_POSIX)
  ETP_tracef (ctx, "WSACleanup()");
  WSACleanup();
#endif

  return (FALSE);
}

/**
 * Run the state-machine until a state-function returns FALSE. <br>
 * Or the SIGINT handler detects user pressing `^C` (i.e `halt_flag` becomes non-zero). <br>
 * The state-function is always set to `ctx->state`.
 *
 * \param[in] ctx  the context we work with.
 */
static void run_state_machine (struct state_CTX *ctx)
{
  while (1)
  {
    ETP_state old_state = ctx->state;
    BOOL      rc = (*ctx->state) (ctx);

    if (opt.debug >= 2)
    {
      int save;

      C_printf ("~2%s~0 -> ~2%s\n~6", ETP_state_name(old_state), ETP_state_name(ctx->state));

      /* Set raw mode in case 'ctx->trace.buffer' contains a "~".
       */
      save = C_setraw (1);
      C_puts (ETP_tracef(ctx, NULL));
      C_setraw (save);
      C_puts ("~0\n");
    }
    if (!rc)
       break;

    if (halt_flag > 0)  /* SIGINT caught */
    {
      C_puts ("~0");
      break;
    }
  }
}

/**
 * Common stuff done before both a blocking and non-blocking `connect()`.
 *
 * \param[in] ctx          the context we work with.
 * \param[in] which_state  the name of the state we're about to enter after this call
 *                         (for ETP_tracef() only).
 */
static void connect_common_init (struct state_CTX *ctx, const char *which_state)
{
  int  rx_size;

  ETP_tracef (ctx, "In %s(). use_netrc: %d, use_authinfo: %d, opt.use_nonblock_io: %d\n",
              which_state, ctx->use_netrc, ctx->use_authinfo, opt.use_nonblock_io);

  if (opt.use_buffered_io)
       rx_size = sizeof(ctx->recv.buffer);
  else rx_size = MAX_RECV_BUF;  /* 16 kByte. Should currently be the same for both cases. */

  ctx->sa.sin_family = AF_INET;
  ctx->sa.sin_port   = htons ((WORD)ctx->port);
  setsockopt (ctx->sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ctx->timeout, sizeof(ctx->timeout));
  setsockopt (ctx->sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rx_size, sizeof(rx_size));

  if (!opt.quiet)
     C_printf ("Connecting to %s/%u...", inet_ntoa(ctx->sa.sin_addr), ctx->port);

  C_flush();
}

/**
 * Common stuff done after both state_blocking_connect() and
 * state_non_blocking_connect().
 *
 * \param[in] ctx  the context we work with.
 * \param[in] err  If `!0`, this is the error-code from the last Winsock called prior to entering here.<br>
 *                 If `0`, the last Winsock called prior to entering here, succeeded (we got connected).
 */
static void connect_common_final (struct state_CTX *ctx, int err)
{
  if (err)  /* The connection failed */
  {
    char buf [500];

    ctx->ws_err = err;
    snprintf (buf, sizeof(buf), "Failed to connect: %s.\n", ws2_strerror(ctx->ws_err));
    WARN (buf);
    ETP_tracef (ctx, buf);
    ctx->state = state_closing;
  }
  else
  {
    if (!opt.quiet)
       C_putc ('\n');
    ctx->state = state_send_login;
  }
}

/**
 * Set socket blocking state.
 */
static void set_nonblock (SOCKET sock, DWORD non_block)
{
#ifdef CYGWIN_POSIX
  int flag = non_block ? 1 : 0;
  ioctl (sock, FIONBIO, &flag);
#else
  ioctlsocket (sock, FIONBIO, &non_block);
#endif
}

/**
 * Save or retrieve a piece of trace-information into the context.
 *
 * \param[in] ctx  the context we work with.
 * \param[in] fmt  If `!NULL`, a var-arg format and parameters to trace. <br>
 *                 If `NULL`, return the pointer to any previous trace-information.
 */
static const char *ETP_tracef (struct state_CTX *ctx, const char *fmt, ...)
{
  va_list args;
  int     i, len;

  if (opt.debug <= 1)
     return (ctx->trace.buffer);

  if (!fmt)
  {
    ctx->trace.buffer_pos  = ctx->trace.buffer;
    ctx->trace.buffer_left = sizeof(ctx->trace.buffer);
    return (ctx->trace.buffer);
  }

  for (i = 0; i < 6; i++)
  {
    *ctx->trace.buffer_pos++ = ' ';
    ctx->trace.buffer_left--;
  }

  va_start (args, fmt);
  len = vsnprintf (ctx->trace.buffer_pos, ctx->trace.buffer_left, fmt, args);
  ctx->trace.buffer_left -= len;
  ctx->trace.buffer_pos  += len;

  va_end (args);

  /* Caller should not use 'ctx->trace.buffer' unless 'fmt == NULL'.
   * Hence return NULL.
   */
  return (NULL);
}

/**
 * Return the name of a state-function. Handy for tracing.
 */
static const char *ETP_state_name (ETP_state f)
{
  #define IF_VALUE(_f) if (f == _f) return (#_f)

  IF_VALUE (state_init);
  IF_VALUE (state_parse_url);
  IF_VALUE (state_netrc_lookup);
  IF_VALUE (state_authinfo_lookup);
  IF_VALUE (state_resolve);
  IF_VALUE (state_blocking_connect);
  IF_VALUE (state_non_blocking_connect);
  IF_VALUE (state_send_login);
  IF_VALUE (state_send_pass);
  IF_VALUE (state_await_login);
  IF_VALUE (state_200);
  IF_VALUE (state_send_query);
  IF_VALUE (state_RESULT_COUNT);
  IF_VALUE (state_PATH);
  IF_VALUE (state_closing);
  IF_VALUE (state_exit);
  return ("?");
}

/**
 * Called from `envtool.c`:
 *   if the `opt.evry_host` smartlist is not empty, this function gets called
 *   for each ETP-host in the smartlist.
 */
int do_check_evry_ept (const char *host)
{
  struct state_CTX ctx;
  char   host_buf [100];

  memset (&ctx, 0, sizeof(ctx));
  ctx.state             = state_init;
  ctx.sock              = INVALID_SOCKET;
  ctx.timeout           = RECV_TIMEOUT;
  ctx.raw_url           = _strlcpy (host_buf, host, sizeof(host_buf));
  ctx.port              = 0;
  ctx.trace.buffer[0]   = '?';
  ctx.trace.buffer[1]   = '\0';
  ctx.trace.buffer_pos  = ctx.trace.buffer;
  ctx.trace.buffer_left = sizeof(ctx.trace.buffer);
  ctx.recv.buffer_pos   = ctx.recv.buffer;
  ctx.recv.buffer_left  = 0;    /* Gets set in `rbuf_read_char()' */

  run_state_machine (&ctx);
  return (ctx.results_got - ctx.results_ignore);
}

static int parse_host_spec (struct state_CTX *ctx, const char *pattern, ...)
{
  int     n;
  va_list args;

  va_start (args, pattern);

  ctx->username[0] = ctx->password[0] = ctx->hostname[0] = '\0';

  n = vsscanf (ctx->raw_url, pattern, args);
  va_end (args);

  ETP_tracef (ctx,
              "pattern: '%s'\n"
              "      n: %d, username: '%s', password: '%s', hostname: '%s', port: %d\n",
              pattern, n, ctx->username, ctx->password, ctx->hostname, ctx->port);
  return (n);
}

/**
 * Read at most `sizeof(ctx->recv.buffer)` bytes from `ctx->sock`,
 * storing them to `ctx->recv.buffer`. This uses `select()` to timeout
 * a stale connection.
 *
 * \param[in] ctx  the context we work with.
 *
 * \note Winsock ignores the first argument in `select()`.
 *       All needed information is really in the `fd_set`s. <br>
 *       But we use it for Cygwin (which tries hard to be POSIX compatible).
 */
static int rbuf_read_sock (struct state_CTX *ctx)
{
  if (ctx->timeout)
  {
    struct timeval tv;
    fd_set rd_fds, ex_fds;
    int    rc;

    FD_ZERO (&rd_fds);
    FD_ZERO (&ex_fds);
    FD_SET (ctx->sock, &rd_fds);
    FD_SET (ctx->sock, &ex_fds);
    tv.tv_sec  = ctx->timeout / 1000;
    tv.tv_usec = ctx->timeout % 1000;
    rc = (int) select ((int)(ctx->sock+1), &rd_fds, NULL, &ex_fds, &tv);
    if (rc <= 0)
       return (rc);
  }
  return recv (ctx->sock, ctx->recv.buffer, sizeof(ctx->recv.buffer), 0);
}

/**
 * Read a character from `ctx->recv.buffer`.
 *
 * \param[in] ctx    the context we work with.
 * \param[in] store  the location to store the buffered or first received character to.
 */
static int rbuf_read_char (struct state_CTX *ctx, char *store)
{
  if (ctx->recv.buffer_left == 0)
  {
    /** If nothing left in 'ctx->recv.buffer', refill the buffer
     *  calling `rbuf_read_sock()`.
     */
    int num;

    ctx->recv.buffer_pos = ctx->recv.buffer;
    num = rbuf_read_sock (ctx);
    if (num <= 0)
       return (num);
    ctx->recv.buffer_read = num;
    ctx->recv.buffer_left = num;
  }

  /**
   * Otherwise, the character is returned from `ctx->recv.buffer_pos`.
   */
  *store = *ctx->recv.buffer_pos++;
  ctx->recv.buffer_left--;
  return (1);
}
