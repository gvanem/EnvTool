/** \file    Everything_ETP.c
 *  \ingroup EveryThing_ETP
 *  \brief
 *    Functions for remote queries using EveryThing's ETP protocol.
 *
 *   ref: http://www.voidtools.com/support/everything/etp/     \n
 *   ref: https://www.voidtools.com/forum/viewtopic.php?t=1790 \n
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

  /** Suppress warning for 'inet_addr()' and 'gethostbyname()'
   */
  #define _WINSOCK_DEPRECATED_NO_WARNINGS
  #include <winsock2.h>
  #include <windows.h>
#endif

#include "color.h"
#include "envtool.h"
#include "auth.h"
#include "Everything_ETP.h"

#ifndef CONN_TIMEOUT
/**\def CONN_TIMEOUT the connect() timeout for a non-blocking connection. */
#define CONN_TIMEOUT 3000   /* 3 sec */
#endif

#ifndef RECV_TIMEOUT
/**\def RECV_TIMEOUT the SO_RCVTIMEO used in setsockopt() */
#define RECV_TIMEOUT 2000   /* 2 sec */
#endif

#ifndef MAX_RECV_BUF
/**\def MAX_RECV_BUF size of receiver buffer */
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
       char   buffer [MAX_RECV_BUF]; /* the input buffer */
       char  *buffer_pos;            /* current position in the buffer */
       size_t buffer_left;           /* number of bytes left in the buffer:
                                      * buffer_left = buffer_end - buffer_pos */
       int    buffer_read;           /* used by rbuf_read_char() */
     };

/**
 * \struct state_CTX
 * The context used throughout the ETP transfer.
 * Keeping this together should make the ETP transfer
 * fully reentrant.
 */
struct state_CTX {
       ETP_state          state;            /** Current state function */
       struct sockaddr_in sa;               /** Used in 'state_resolve()' and 'state_xx_connect()' */
       SOCKET             sock;             /** Set in 'state_xx_connect()' */
       int                port;             /** The server destination port to use */
       char              *raw_url;          /** URL on raw form given in do_check_evry_ept() */
       char               hostname [200];   /** Name of host to connect to */
       char               username [30];    /** Name of user */
       char               password [30];    /** And his password (if any) */
       BOOL               use_netrc;        /** Use the '%APPDATA/.netrc' file */
       BOOL               use_authinfo;     /** Use the '%APPDATA/.authinfo' file */
       DWORD              timeout;
       int                retries;
       int                ws_err;           /** Last WSAGetError() */
       unsigned           results_expected; /** The number of matches we're expecting */
       unsigned           results_got;      /** The number of matches we got */
       unsigned           results_ignore;   /** The number of matches we ignored */
       struct IO_buf      recv;             /** The IO_buf for reception */
       struct IO_buf      trace;            /** The IO_buf for tracing the protocol */

       /* These are set in state_PATH()
        */
       time_t             mtime;
       UINT64             fsize;
       char               path [_MAX_PATH];
     };

/**
 * Local functions; prototypes.
 */
static int         parse_host_spec      (struct state_CTX *ctx, const char *pattern, ...);
static void        connect_common_init  (struct state_CTX *ctx, const char *which_state);
static void        connect_common_final (struct state_CTX *ctx, int err);
static void        set_nonblock         (SOCKET sock, DWORD non_block);
static const char *ws2_strerror         (int err);

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

/*
 * Do a remote EveryThing search.
 * Connect to a remote ETP-server, login, send the SEARCH and parameters.
 * The protocol goes something like this (1st attempt of MSC in Doxygen)
 *
 * \msc "ETP Protocol" width=10cm height=20cm
 *   arcgradient = 8;
 *   a[label="Client"], b[label="Server"], c[label="Server error", URL="\ref state_closing"];
 *   a => b [label="USER xx"];
 *   a <= b [label="230.."];
 *   c <= b [label="!230.."];
 *   a => b [label="EVERYTHING SEARCH"];
 * \endmsc
 */

/**
 * Do a remote EveryThing search:
 *   Connect to a remote ETP-server, login, send the SEARCH and parameters.
 *
 * The protocol goes something like this:
 * \verbatim
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
 * \endverbatim
 *
 * Which can be accomplished with a .bat-file and a plain ftp client:
 *
 * \verbatim
 *  @echo off
 *  echo USER                                     > etp-commands
 *  echo QUOTE EVERYTHING SEARCH notepad.exe     >> etp-commands
 *  echo QUOTE EVERYTHING PATH_COLUMN 1          >> etp-commands
 *  echo QUOTE EVERYTHING SIZE_COLUMN 1          >> etp-commands
 *  echo QUOTE EVERYTHING DATE_MODIFIED_COLUMN 1 >> etp-commands
 *  echo QUOTE EVERYTHING QUERY                  >> etp-commands
 *  echo BYE                                     >> etp-commands
 *
 *  c:\> ftp -s:etp-commands 10.0.0.37
 *
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
 *
 * \endverbatim
 */

/**
 * Receive a response with timeout.
 * Stop when we get an '\r\n' terminated ASCII-line.
 *
 * opt.use_buffered_io == 1:
 *   Use the buffered I/O setup in 'rbuf_read_char()' and 'rbuf_fill_buf()'.
 *
 * opt.use_buffered_io == 0:
 *   Call 'recv()' for 1 character at a time!
 *   This is to keep it simple; avoid the need for (a non-blocking socket and)
 *   'select()'. But the 'recv()' will hang if used against a non "line oriented"
 *   protocol. But only for max 2 seconds since 'setsockopt()' with 'SO_RCVTIMEO'
 *   was set to 2 sec.
 *
 *   Practice shows this is just a bit slower than the 'rbuf_read_char()' method since
 *   Winsock2 provides good buffering with the above 'setsockopt()' call. The question
 *   is if this causes a higher number of kernel switches. And thus lower performance
 *   on slower CPUs.
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
      if (ch == '\n')    /* Assumes '\r' already was received */
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
  strip_nl (start);
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
 * Do not use a "\r\n" termination; it will be added here.
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
 * Print the resulting match:
 *  'name' is either a file-name within a 'ctx->path' (is_dir=FALSE).
 *  Or a folder-name within a 'ctx->path'             (is_dir=TRUE).
 */
static void report_file_ept (struct state_CTX *ctx, const char *name, BOOL is_dir)
{
  if (opt.dir_mode && !is_dir)
     ctx->results_ignore++;
  else
  {
    static char prev_name [_MAX_PATH];
    char   full_name [_MAX_PATH];

    snprintf (full_name, sizeof(full_name), "%s%c%s", ctx->path, DIR_SEP, name);

    if (!opt.dir_mode && prev_name[0] && !str_equal(prev_name, full_name))
         ETP_num_evry_dups++;
    else report_file (full_name, ctx->mtime, ctx->fsize, is_dir, FALSE, HKEY_EVERYTHING_ETP);
    _strlcpy (prev_name, full_name, sizeof(prev_name));
  }
  ctx->mtime = 0;
  ctx->fsize = 0;
  ctx->results_got++;
}

/**
 * Print a warning for a failed login or a non-existant.
 * And optionally what action to take next.
 */
static void login_warning (struct state_CTX *ctx, BOOL is_authinfo)
{
  const char *next_action = "\n";
  char       *file;

  if (is_authinfo)
  {
    file = getenv_expand ("%APPDATA%\\.authinfo");
    if (!FILE_EXISTS(file))
         WARN ("%s: file not found.", file);
    else WARN ("%s: user/password/port not found for host \"%s\".", file, ctx->hostname);

    if (ctx->use_netrc)
       next_action = " Will try %%APPDATA%%\\.netrc next.\n";
  }
  else
  {
    file = getenv_expand ("%APPDATA%\\.netrc");
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
 * PATH state: gooble up the results.
 * Expect 'results_expected' results set in 'RESULT_COUNT_state()'.
 * When "200 End" is received, enter 'state_closing'.
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
    ETP_tracef (ctx, "file: %s", rx + 5);
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

  if (strncmp(rx,"200 End",7))
  {
    ETP_tracef (ctx, "results_got: %lu", ctx->results_got);
    WARN ("Unexpected response: \"%s\", err: %s\n", rx, ws2_strerror(ctx->ws_err));
  }

  ctx->state = state_closing;
  return (TRUE);
}

/**
 * RESULT_COUNT state: get the "RESULT_COUNT n\r\n" string.
 * Goto 'state_PATH()'.
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
 * State entered after commands was sent:
 *   Swallow received lines until "200-xx\r\n" is received.
 *   Then enter 'state_RESULT_COUNT()'.
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
 * Close the socket and goto 'state_exit'.
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
 * Send the search parameters and the QUERY command.
 * If the 'send()' call fail, enter 'state_closing'.
 * Otherwise, enter 'state_200'.
 *
 * \todo: Maybe sending all these commands in one 'send()'
 *        is better?
 */
static BOOL state_send_query (struct state_CTX *ctx)
{
  /* If raw query, send 'file_spec' query as-is below
   */
  if (!opt.evry_raw)
  {
    /* Always send a "REGEX 1", but translate from a shell-pattern if 'opt.use_regex == 0'.
     */
    send_cmd (ctx, "EVERYTHING REGEX 1");
    if (!opt.use_regex)
       send_cmd (ctx, "EVERYTHING SEARCH ^%s$", translate_shell_pattern(opt.file_spec));
  }

  send_cmd (ctx, "EVERYTHING SEARCH %s", opt.file_spec);
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
 * Send the USER name and optionally the 'ctx->password'.
 * "USER" can be empty if the remote Everything.ini has a
 * setting like:
 *   etp_server_username=
 *
 * If the .ini-file contains:
 *   etp_server_username=foo
 *   etp_server_password=bar
 * 'ctx->username' and 'ctx->password' MUST match 'foo/bar'.
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
 * We sent the USER/PASS commands.
 * Await the "230 Logged on" message and enter 'state_send_query'.
 * If server replies with a "530" message ("Login or password incorrect"),
 * enter 'state_closing'.
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

  /* Any "5xx" message or a timeout is fatal here; enter 'state_closing'.
   */
  snprintf (buf, sizeof(buf), "Failed to login; USER %s.\n", ctx->username);
  WARN (buf);
  ETP_tracef (ctx, buf);
  ctx->state = state_closing;
  return (TRUE);
}

/**
 * We're prepared to send a password. But if server replies with
 * "230 Logged on", we know it ignores passwords (dangerous!).
 * So proceed to 'state_send_query' state.
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
 * Check if 'ctx->hostname' is simply an IPv4-address.
 * If TRUE
 *   Enter 'state_blocking_connect' or 'state_non_blocking_connect'.
 * else
 *   Call 'gethostbyname()' to get the IPv4-address.
 *   Then enter 'state_blocking_connect' or 'state_non_blocking_connect'.
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

/**
 * When the IPv4-address is known, perform the non-blocking 'connect()'.
 * The 'ctx->sock' is already in non-block state.
 * Loop here until 'WSAWOULDBLOCK' is no longer a result in 'select()'.
 * If successful, enter 'state_send_login'.
 *
 * \def SELECT_TIME_USEC
 *      select() timeout for each try (in usec)
 */
#define SELECT_TIME_USEC (500*1000)

/** \def MAX_RETRIES
 *       Max retries is the result of CONN_TIMEOUT (in msec)
 */
#define MAX_RETRIES (1000 * CONN_TIMEOUT / SELECT_TIME_USEC)

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

  FD_ZERO (&wr_fds);
  FD_ZERO (&ex_fds);
  FD_SET (ctx->sock, &wr_fds);
  FD_SET (ctx->sock, &ex_fds);
  tv.tv_sec  = 0;
  tv.tv_usec = SELECT_TIME_USEC;

  rc = select (ctx->sock+1, NULL, &wr_fds, &ex_fds, &tv);
  if (rc >= 1)
  {
    /* socket writable; connected
     */
    if (FD_ISSET(ctx->sock,&wr_fds))
    {
      connect_common_final (ctx, 0);
      set_nonblock (ctx->sock, 0);
    }
    /* socket exception; not connected
     */
    else if (FD_ISSET(ctx->sock,&ex_fds))
    {
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
 * When the IPv4-address is known, perform the blocking 'connect()'.
 * If successful, enter 'state_send_login'.
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
 * Lookup the USER/PASSWORD for 'ctx->hostname' in the '~/.netrc' file.
 * Enter 'state_send_login' even if USER/PASSWORD are not found.
 */
static BOOL state_netrc_lookup (struct state_CTX *ctx)
{
  const char *user  = NULL;
  const char *passw = NULL;
  BOOL  okay = (netrc_init() && netrc_lookup(ctx->hostname, &user, &passw));

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

  netrc_exit();

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
 * Lookup the USER/PASSWORD for 'ctx->hostname' in the '~/.authinfo' file.
 * Enter 'state_send_login' even if USER/PASSWORD are not found.
 */
static BOOL state_authinfo_lookup (struct state_CTX *ctx)
{
  const char *user  = NULL;
  const char *passw = NULL;
  int         port = 0;
  BOOL        okay = (authinfo_init() && authinfo_lookup(ctx->hostname, &user, &passw, &port));

  if (!okay)
     login_warning (ctx, TRUE);
  else
  {
    if (user)
       _strlcpy (ctx->username, user, sizeof(ctx->username));
    if (passw)
       _strlcpy (ctx->password, passw, sizeof(ctx->password));
    if (port)
       ctx->port = port;
  }

  ETP_tracef (ctx, "Got USER: %s, PASS: %s and PORT: %u in '%%APPDATA%%\\.authinfo' for '%s'\n",
              user ? user : "<None>", passw ? passw : "<none>", ctx->port, ctx->hostname);

  authinfo_exit();

  /* Do not attempt "~/.authinfo" login ever again.
   */
  ctx->use_authinfo = FALSE;

  /* If "~/.authinfo" login failed, maybe try "~/.netrc" next?
   */
  if (!okay && ctx->use_netrc)
       ctx->state = state_netrc_lookup;
  else ctx->state = state_resolve;

  return (TRUE);
}

/**
 * Check if 'ctx->raw_url' matches one of these formats:
 *   "user:passwd@host_or_IP-address<:port>".    Both user-name and password (+ port).
 *   "user@host_or_IP-address<:port>".           Only user-name (+ port).
 *   "host_or_IP-address<:port>".                Only host/IP-address (+ port).
 */
static BOOL state_parse_url (struct state_CTX *ctx)
{
  int n;

  ETP_tracef (ctx, "Cracking the host-spec: '%s'.\n", ctx->raw_url);

  /* Assume we must use '~/.netrc' or '~/.authifo'.
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

  if (ctx->use_authinfo)    /* Try parsing a '~/.authinfo' first */
       ctx->state = state_authinfo_lookup;
  else if (ctx->use_netrc)
       ctx->state = state_netrc_lookup;
  else ctx->state = state_resolve;

  return (TRUE);
}

/**
 * The first state-machine function we enter.
 * Initialise Winsock and create the socket.
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
 * Run the state-machine until a state-function returns FALSE.
 */
static void run_state_machine (struct state_CTX *ctx)
{
  while (1)
  {
    ETP_state old_state = ctx->state;
    BOOL      rc = (*ctx->state) (ctx);

    if (opt.debug >= 2)
    {
      C_printf ("~2%s~0 -> ~2%s\n~6", ETP_state_name(old_state), ETP_state_name(ctx->state));

      /* Set raw mode in case 'ctx->trace.buffer' contains a "~".
       */
      C_setraw (1);
      C_puts (ETP_tracef(ctx, NULL));
      C_setraw (0);
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
 * Common stuff done before both a blocking and non-blocking 'connect()'.
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
 * Common stuff done after both 'state_blocking_connect()' and
 * 'state_non_blocking_connect()'.
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

#if defined(CYGWIN_POSIX)
/**
 * If we use POSIX sockets in Cygwin, the 'err' is really 'errno'.
 * And the error-string for 'err' is simply from 'strerror()'.
 */
static const char *ws2_strerror (int err)
{
  return strerror (err);
}
#else

/**
 * Return error-string for 'err' for Winsock error-codes.
 * These strings are stored by "kernel32.dll" and not
 * "ws2_32.dll".
 */
static const char *ws2_strerror (int err)
{
  static char buf [500];

  if (err == 0)
     return ("No error");

  init_misc();

  if (kernel32_hnd != INVALID_HANDLE_VALUE &&
     FormatMessageA (FORMAT_MESSAGE_FROM_HMODULE,
                     kernel32_hnd, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     buf, sizeof(buf), NULL))
     return strip_nl (buf);

  snprintf (buf, sizeof(buf), "%d?", err);
  return (buf);
}
#endif

/**
 * Save a piece of trace-information into the context.
 * Retrieve it when 'fmt == NULL'.
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
 * Called from envtool.c:
 *   if the 'opt.evry_host' smartlist is not empty, this function gets called
 *   for each ETP-host in the smartlist.
 *
 * \todo:
 *   The 'netrc_init()' + 'netrc_exit()' is currently done for each host.
 *   Should do something better.
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
  ctx.port              = 21;
  ctx.trace.buffer[0]   = '?';
  ctx.trace.buffer[1]   = '\0';
  ctx.trace.buffer_pos  = ctx.trace.buffer;
  ctx.trace.buffer_left = sizeof(ctx.trace.buffer);
  ctx.recv.buffer_pos   = ctx.recv.buffer;
  ctx.recv.buffer_left  = 0;    /* Gets set in 'rbuf_read_char()' */

  run_state_machine (&ctx);
  return (ctx.results_got - ctx.results_ignore);
}

/**
 * _MSC_VER <= 1700 (Visual Studio 2012 or older) is lacking 'vsscanf()'.
 * Create our own using 'sscanf()'. Scraped from:
 *   https://stackoverflow.com/questions/2457331/replacement-for-vsscanf-on-msvc
 *
 * If using the Windows-Kit ('_VCRUNTIME_H' is defined) it should have a
 * working 'vsscanf()'. I'm not sure if using MSC_VER <= 1700 with the
 * Windows-Kit is possible. But if using Clang-cl, test this function with
 * it.
 */
#if (defined(_MSC_VER) && (_MSC_VER <= 1800) && !defined(_VCRUNTIME_H)) || defined(__clang__)
  static int _vsscanf2 (const char *buf, const char *fmt, va_list args)
  {
    void *a[5];  /* 5 args is enough here */
    int   i;

    for (i = 0; i < DIM(a); i++)
       a[i] = va_arg (args, void*);
    return sscanf (buf, fmt, a[0], a[1], a[2], a[3], a[4]);
  }
  #define vsscanf(buf, fmt, args) _vsscanf2 (buf, fmt, args)
#endif

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
 * Read at most 'sizeof(ctx->recv.buffer)' bytes from 'ctx->sock',
 * storing them to 'ctx->recv.buffer'. This uses select() to timeout
 * the stale connections
 *
 * Note: Winsock ignores the first argument in 'select()'.
 *       All needed information is really in the 'fd_set's.
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
    rc = select (ctx->sock+1, &rd_fds, NULL, &ex_fds, &tv);
    if (rc <= 0)
       return (rc);
  }
  return recv (ctx->sock, ctx->recv.buffer, sizeof(ctx->recv.buffer), 0);
}

/**
 * Read a character from 'ctx->recv.buffer'.
 *
 * If there is something in the buffer, the character is returned from
 * 'ctx->recv.buffer_pos'.
 *
 * Otherwise, refill the buffer using 'rbuf_read_sock()'.
 */
static int rbuf_read_char (struct state_CTX *ctx, char *store)
{
  if (ctx->recv.buffer_left == 0)
  {
   /* If nothing left in 'ctx->recv.buffer', refill the buffer
    * using the above 'rbuf_read_sock()'.
    */
    int num;

    ctx->recv.buffer_pos = ctx->recv.buffer;
    num = rbuf_read_sock (ctx);
    if (num <= 0)
       return (num);
    ctx->recv.buffer_read = num;
    ctx->recv.buffer_left = num;
  }
  *store = *ctx->recv.buffer_pos++;
  ctx->recv.buffer_left--;
  return (1);
}
