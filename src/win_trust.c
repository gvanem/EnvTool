/*
 * Adapted from:
 *  https://msdn.microsoft.com/en-us/library/windows/desktop/aa382384(v=vs.85).aspx
 *
 * And extended with the help of:
 *  https://support.microsoft.com/en-us/kb/323809
 *  http://forum.sysinternals.com/howto-verify-the-digital-signature-of-a-file_topic19247.html
 */
#include <stdio.h>
#include <windows.h>
#include <wintrust.h>
#include <wincrypt.h>
#include <softpub.h>

#include "getopt_long.h"
#include "envtool.h"

#ifndef __IN
#define __IN
#endif

#ifndef __OUT
#define __OUT
#endif

#if defined(WIN_TRUST_TEST)
  #define PRINTF(args)      printf args
  #undef  ERROR
  #define ERROR(s)          printf ("%s() failed: %s\n", s, win_strerror(GetLastError()))
#else
  #define PRINTF(args)      ((void)0)
  #undef  ERROR
  #define ERROR(s)          last_err = GetLastError()
#endif

#define ASN_ENCODING  (X509_ASN_ENCODING | PKCS_7_ASN_ENCODING)
#define QUIT(rc)      do { res = (rc); goto quit; } while (0)

struct SPROG_PUBLISHERINFO {
       wchar_t *program_name;
       wchar_t *publisher_link;
       wchar_t *more_info_link;
     };

char *wintrust_subject;

static DWORD last_err;

static wchar_t *evil_char_to_wchar (const char *text);
static int      crypt_check_file (const char *fname);

#if defined(WIN_TRUST_TEST)

char  *program_name = "win_trust.exe";
struct prog_options opt;

const char *usage_fmt = "Usage: %s <-hcdr> PE-file\n"
                        "    -h: show this help.\n"
                        "    -c: call crypt_check_file().\n"
                        "    -d: set debug-level.\n"
                        "    -r: perform a Cert revocation check.\n";

static void usage (const char *fmt, ...)
{
  va_list args;

  va_start (args, fmt);
  vprintf (fmt, args);
  va_end (args);
  exit (-1);
}

int main (int argc, char **argv)
{
  const char *pe_file;
  DWORD       err;
  BOOL        check_details = FALSE;
  BOOL        revoke_check = FALSE;
  int         ch;

  while ((ch = getopt(argc, argv, "cdh?r")) != EOF)
        switch (ch)
        {
          case 'c': check_details = TRUE;
                    break;
          case 'd': opt.debug++;
                    break;
          case 'r': revoke_check = TRUE;
                    break;
          case '?':
          case 'h': usage (usage_fmt, argv[0]);
                    break;
        }

  if (!argv[optind])
     usage (usage_fmt, argv[0]);

  pe_file = argv[optind];

  err = wintrust_check (pe_file, check_details, revoke_check);

  switch (err)
  {
    case ERROR_SUCCESS:
         printf ("The file %s is signed and the signature was verified.\n", pe_file);
         break;

    case TRUST_E_NOSIGNATURE:
         err = GetLastError();
         if (err == TRUST_E_NOSIGNATURE ||
             err == TRUST_E_SUBJECT_FORM_UNKNOWN ||
             err == TRUST_E_PROVIDER_UNKNOWN)
              printf ("The file \"%s\" is not signed.\n", pe_file);
         else printf ("An unknown error occurred trying to verify the signature of "
                      "the \"%s\" file.\n", pe_file);
         break;

    case TRUST_E_EXPLICIT_DISTRUST:
         printf ("The signature is present, but specifically disallowed.\n");
         break;

    case TRUST_E_SUBJECT_NOT_TRUSTED:
         printf ("The signature is present, but not trusted.\n");
         break;

    case CRYPT_E_SECURITY_SETTINGS:
         printf ("CRYPT_E_SECURITY_SETTINGS - The hash "
                 "representing the subject or the publisher wasn't "
                 "explicitly trusted by the admin and admin policy "
                 "has disabled user trust. No signature, publisher "
                 "or timestamp errors.\n");
         break;

    default:
         printf ("Error is: 0x%lx.\n", err);
         break;
  }

#if 0
  if (err == ERROR_SUCCESS               ||
      err == TRUST_E_EXPLICIT_DISTRUST   ||
      err == TRUST_E_SUBJECT_NOT_TRUSTED ||
      err == CRYPT_E_SECURITY_SETTINGS)
  {
    CRYPT_PROVIDER_SGNR *signer   = NULL;
    CRYPT_PROVIDER_CERT *cert     = NULL;
    CRYPT_PROVIDER_DATA *provider = WTHelperProvDataFromStateData (data.hWVTStateData);

    if (provider)
    {
      /*
       * MS OIDs:
       *   https://msdn.microsoft.com/en-us/library/windows/desktop/aa377978(v=vs.85).aspx
       *
       *    1.3.6.1.xx
       *    1.3.6.1.5.5.7.3.3
       */
       DEBUGF (1, "SignerUsageOID: '%s'\n", provider->pszCTLSignerUsageOID);
       DEBUGF (1, "UsageOID:       '%s'\n", provider->pszUsageOID);

       signer = WTHelperGetProvSignerFromChain ((CRYPT_PROVIDER_DATA*)provider,
                                                0, FALSE, /* first signer*/
                                                0);       /* not a counter signer */
       DEBUGF (1, "signer:          %p\n", signer);
       if (signer)
       {
         /* grab the signer cert from CRYPT_PROV_SGNR
          */
         cert = WTHelperGetProvCertFromChain (signer, 0);          /* 0 = signer Cert */
         DEBUGF (1, "Signer Cert:     %p\n", cert);
         cert = WTHelperGetProvCertFromChain (signer, (DWORD)-1);  /* -1 = root Cert */
         DEBUGF (1, "Root Cert:       %p\n", cert);
       }
    }
    else
      ERROR ("WTHelperProvDataFromStateData");
  }
#endif

  return (err);
}
#endif  /* WIN_TRUST_TEST */

DWORD wintrust_check (const char *pe_file, BOOL check_details, BOOL revoke_check)
{
  DWORD              rc;
  WINTRUST_DATA      data;
  WINTRUST_FILE_INFO file_info;
  GUID               action =
#if 0
                       WINTRUST_ACTION_GENERIC_VERIFY_V2;
#else
                       WINTRUST_ACTION_TRUSTPROVIDER_TEST;
#endif


  memset (&data, 0, sizeof(data));
  memset (&file_info, 0, sizeof(file_info));
  last_err = 0;

  file_info.cbStruct      = sizeof(file_info);
  file_info.pcwszFilePath = evil_char_to_wchar (pe_file);

  data.cbStruct            = sizeof(data);
  data.dwUIChoice          = WTD_UI_NONE;
  data.dwUnionChoice       = WTD_CHOICE_FILE;
  data.pFile               = &file_info;
  data.dwStateAction       = WTD_STATEACTION_VERIFY;
  data.dwUIContext         = WTD_UICONTEXT_EXECUTE;
  data.fdwRevocationChecks = revoke_check ? WTD_REVOKE_WHOLECHAIN : WTD_REVOKE_NONE;
  data.dwProvFlags         = revoke_check ? WTD_REVOCATION_CHECK_CHAIN : 0;

  rc = last_err = WinVerifyTrust (NULL, &action, &data);

  data.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust (NULL, &action, &data);
  FREE ((void*)file_info.pcwszFilePath);

  if (check_details)
  {
    PRINTF (("\nDetails for crypt_check_file (\"%s\").\n", pe_file));
    crypt_check_file (pe_file);
  }
  return (rc);
}

const char *wintrust_check_result (DWORD rc)
{
  static char buf [30];

  switch (rc)
  {
    case ERROR_SUCCESS:
         return ("Verified");
    case TRUST_E_NOSIGNATURE:
    case TRUST_E_SUBJECT_FORM_UNKNOWN:
    case TRUST_E_PROVIDER_UNKNOWN:
         return ("Not signed");
    case TRUST_E_EXPLICIT_DISTRUST:
         return ("Disallowed");
    case TRUST_E_SUBJECT_NOT_TRUSTED:
         return ("Not trusted");
    case CRYPT_E_SECURITY_SETTINGS:
         return ("Admin disabled");
    default:
         /* Cast to shut-up gcc in 64-bit mode.
          */
         snprintf (buf, sizeof(buf), "0x%08lx", (long unsigned int)rc);
         return (buf);
  }
}

static BOOL PrintCertificateInfo (const CERT_CONTEXT *cert_context)
{
  char  *name = NULL;
  DWORD  data, n;
  BOOL   res = FALSE;

  PRINTF (("Serial Number: "));
  data = cert_context->pCertInfo->SerialNumber.cbData;

#if 1
  for (n = 0; n < data; n++)
      PRINTF (("%02x ", cert_context->pCertInfo->SerialNumber.pbData[data-n+1]));
  PRINTF (("\n"));
#else
  hex_dump (&cert_context->pCertInfo->SerialNumber.pbData[data+1], data);
#endif

  /* Get Issuer name size
  */
  data = CertGetNameString (cert_context,
                            CERT_NAME_SIMPLE_DISPLAY_TYPE, CERT_NAME_ISSUER_FLAG,
                            NULL, NULL, 0);
  if (!data)
  {
    ERROR ("CertGetNameString");
    QUIT (FALSE);
  }

  name = alloca (data);

  /* Get 'Issuer name'
   */
  if (!CertGetNameString (cert_context, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                          CERT_NAME_ISSUER_FLAG, NULL, name, data))
  {
    ERROR ("CertGetNameString");
    QUIT (FALSE);
  }

  PRINTF (("Issuer Name:   %s\n", name));

  /* Get 'Subject Name'
   */
  data = CertGetNameString (cert_context, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
                            NULL, NULL, 0);
  if (!data)
  {
    ERROR ("CertGetNameString");
    QUIT (FALSE);
  }

  name = alloca (data);
  if (!CertGetNameString(cert_context, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
                         NULL, name, data))
  {
    ERROR ("CertGetNameString");
    QUIT (FALSE);
  }

  PRINTF (("Subject Name:  %s\n", name));

#if !defined(WIN_TRUST_TEST)
  FREE (wintrust_subject);
  wintrust_subject = STRDUP (name);
#endif

  res = TRUE;

quit:
  return res;
}

static BOOL GetProgAndPublisherInfo (const CMSG_SIGNER_INFO     *signer_info __IN,
                                     struct SPROG_PUBLISHERINFO *info        __OUT)
{
  SPC_SP_OPUS_INFO *opus_info;
  DWORD             data, n;
  BOOL              res = FALSE;

  /* Loop through authenticated attributes and find SPC_SP_OPUS_INFO_OBJID OID.
   */
  for (n = 0; n < signer_info->AuthAttrs.cAttr; n++)
  {
    if (lstrcmpA(SPC_SP_OPUS_INFO_OBJID, signer_info->AuthAttrs.rgAttr[n].pszObjId))
       continue;

    /* Get Size of SPC_SP_OPUS_INFO structure
     */
    res = CryptDecodeObject (ASN_ENCODING,
                             SPC_SP_OPUS_INFO_OBJID,
                             signer_info->AuthAttrs.rgAttr[n].rgValue[0].pbData,
                             signer_info->AuthAttrs.rgAttr[n].rgValue[0].cbData,
                             0, NULL, &data);
    if (!res)
    {
      ERROR ("CryptDecodeObject");
      QUIT (FALSE);
    }

    opus_info = alloca (data);

    /* Decode and get SPC_SP_OPUS_INFO structure
     */
    res = CryptDecodeObject (ASN_ENCODING,
                             SPC_SP_OPUS_INFO_OBJID,
                             signer_info->AuthAttrs.rgAttr[n].rgValue[0].pbData,
                             signer_info->AuthAttrs.rgAttr[n].rgValue[0].cbData,
                             0, opus_info, &data);
    if (!res)
    {
      ERROR ("CryptDecodeObject");
      QUIT (FALSE);
    }

    /* Fill in Program Name if present */
    if (opus_info->pwszProgramName)
       info->program_name = WCSDUP (opus_info->pwszProgramName);

    /* Fill in Publisher Information if present */
    if (opus_info->pPublisherInfo)
    {
      switch (opus_info->pPublisherInfo->dwLinkChoice)
      {
        case SPC_URL_LINK_CHOICE:
             info->publisher_link = WCSDUP (opus_info->pPublisherInfo->pwszUrl);
             break;
        case SPC_FILE_LINK_CHOICE:
             info->publisher_link = WCSDUP (opus_info->pPublisherInfo->pwszFile);
             break;
        default:
             break;
      }
    }

    /* Fill in More Info if present */
    if (opus_info->pMoreInfo)
    {
      switch (opus_info->pMoreInfo->dwLinkChoice)
      {
        case SPC_URL_LINK_CHOICE:
             info->more_info_link = WCSDUP (opus_info->pMoreInfo->pwszUrl);
             break;
        case SPC_FILE_LINK_CHOICE:
             info->more_info_link = WCSDUP (opus_info->pMoreInfo->pwszFile);
             break;
        default:
             break;
      }
    }
    QUIT (TRUE);
  }

quit:
  return (res);
}

BOOL GetDateOfTimeStamp (CMSG_SIGNER_INFO *signer_info, SYSTEMTIME *st)
{
  FILETIME lft, ft;
  DWORD    data, n;
  BOOL     res = FALSE;

  /* Loop through authenticated attributes and find szOID_RSA_signingTime OID.
   */
  for (n = 0; n < signer_info->AuthAttrs.cAttr; n++)
  {
    if (lstrcmpA(szOID_RSA_signingTime, signer_info->AuthAttrs.rgAttr[n].pszObjId))
       continue;

    /* Decode and get FILETIME structure */
    data = sizeof(ft);
    res = CryptDecodeObject (ASN_ENCODING,
                             szOID_RSA_signingTime,
                             signer_info->AuthAttrs.rgAttr[n].rgValue[0].pbData,
                             signer_info->AuthAttrs.rgAttr[n].rgValue[0].cbData,
                             0, (void*)&ft, &data);
    if (!res)
    {
      ERROR ("CryptDecodeObject");
      break;
    }

    /* Convert to local time */
    FileTimeToLocalFileTime (&ft, &lft);
    FileTimeToSystemTime (&lft, st);
    break;
  }
  return (res);
}

BOOL GetTimeStampSignerInfo (CMSG_SIGNER_INFO  *signer_info         __IN,
                             CMSG_SIGNER_INFO **counter_signer_info  __OUT)
{
  BOOL   res = FALSE;
  DWORD  size, n;
  void  *data;

  *counter_signer_info = NULL;

  /* Loop through unathenticated attributes for szOID_RSA_counterSign OID
   */
  for (n = 0; n < signer_info->UnauthAttrs.cAttr; n++)
  {
    if (lstrcmpA(signer_info->UnauthAttrs.rgAttr[n].pszObjId, szOID_RSA_counterSign))
       continue;

    /* Get size of CMSG_SIGNER_INFO structure
     */
    res = CryptDecodeObject (ASN_ENCODING,
                             PKCS7_SIGNER_INFO,
                             signer_info->UnauthAttrs.rgAttr[n].rgValue[0].pbData,
                             signer_info->UnauthAttrs.rgAttr[n].rgValue[0].cbData,
                             0, NULL, &size);
    if (!res)
    {
      ERROR ("CryptDecodeObject");
      QUIT (FALSE);
    }

    *counter_signer_info = CALLOC (size,1);
    if (*counter_signer_info == NULL)
    {
      PRINTF (("Unable to allocate memory for timestamp info.\n"));
      QUIT (FALSE);
    }

    /* Decode and get CMSG_SIGNER_INFO structure for timestamp certificate.
     */
    data = *counter_signer_info;
    res = CryptDecodeObject (ASN_ENCODING,
                             PKCS7_SIGNER_INFO,
                             signer_info->UnauthAttrs.rgAttr[n].rgValue[0].pbData,
                             signer_info->UnauthAttrs.rgAttr[n].rgValue[0].cbData,
                             0, data, &size);
    if (!res)
    {
      ERROR ("CryptDecodeObject");
      QUIT (FALSE);
    }
    res = TRUE;
    break;
  }

quit:
  return (res);
}

static int crypt_check_file (const char *fname)
{
  wchar_t             file_name [_MAX_PATH];
  HCERTSTORE          h_store      = NULL;
  HCRYPTMSG           h_msg        = NULL;
  const CERT_CONTEXT *cert_context = NULL;
  DWORD               encoding, content_type, format_type;
  DWORD               signer_info_size;
  CMSG_SIGNER_INFO   *signer_info = NULL;
  CMSG_SIGNER_INFO   *counter_signer_info = NULL;
  CERT_INFO           cert_info;
  SYSTEMTIME          st;
  int                 res;

  struct SPROG_PUBLISHERINFO publisher_info;

  memset (&publisher_info, 0, sizeof(publisher_info));

  if (mbstowcs(file_name, fname, DIM(file_name)) == -1)
  {
    PRINTF (("Unable to convert to unicode.\n"));
    QUIT (-1);
  }

  /* Get message handle and store handle from the signed file
   */
  res = CryptQueryObject (CERT_QUERY_OBJECT_FILE,
                          file_name,
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY,
                          0, &encoding, &content_type, &format_type,
                          &h_store, &h_msg, NULL);
  if (!res)
  {
    ERROR ("CryptQueryObject");
    QUIT (-1);
  }

  /* Get signer information size
   */
  res = CryptMsgGetParam (h_msg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &signer_info_size);
  if (!res)
  {
    ERROR ("CryptMsgGetParam");
    QUIT (-1);
  }

  signer_info = alloca (signer_info_size);

  /* Get signer information
   */
  res = CryptMsgGetParam (h_msg, CMSG_SIGNER_INFO_PARAM, 0,
                          (void*)signer_info, &signer_info_size);
  if (!res)
  {
    ERROR ("CryptMsgGetParam");
    QUIT (-1);
  }

  /* Get program name and publisher information from
   * signer info structure
   */
  if (GetProgAndPublisherInfo(signer_info, &publisher_info))
  {
    if (publisher_info.program_name)
       PRINTF (("Program Name:   %" WIDESTR_FMT "\n", publisher_info.program_name));

    if (publisher_info.publisher_link)
       PRINTF (("Publisher Link: %" WIDESTR_FMT "\n", publisher_info.publisher_link));

    if (publisher_info.more_info_link)
       PRINTF (("MoreInfo Link:  %" WIDESTR_FMT "\n", publisher_info.more_info_link));
  }

  PRINTF (("\n"));

  /* Search for the signer certificate in the temporary certificate store
   */
  cert_info.Issuer       = signer_info->Issuer;
  cert_info.SerialNumber = signer_info->SerialNumber;

  cert_context = CertFindCertificateInStore (h_store, ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT,
                                             (void*)&cert_info, NULL);
  if (!cert_context)
  {
    ERROR ("CertFindCertificateInStore");
    QUIT (-1);
  }

  /* Print Signer certificate information
   */
  PRINTF (("Signer Certificate:\n\n"));
  PrintCertificateInfo (cert_context);
  PRINTF (("\n"));

  /* Get the timestamp certificate signerinfo structure
   */
  if (GetTimeStampSignerInfo(signer_info, &counter_signer_info))
  {
    /* Search for Timestamp certificate in the temporary certificate store
     */
    cert_info.Issuer       = counter_signer_info->Issuer;
    cert_info.SerialNumber = counter_signer_info->SerialNumber;

    cert_context = CertFindCertificateInStore (h_store, ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT,
                                              (void*)&cert_info, NULL);
    if (!cert_context)
    {
      ERROR ("CertFindCertificateInStore");
      QUIT (-1);
    }

    PRINTF (("TimeStamp Certificate:\n\n"));
    PrintCertificateInfo (cert_context);

    PRINTF (("\nTimeStamp: "));
    if (GetDateOfTimeStamp (counter_signer_info, &st))
         PRINTF (("%02d/%02d/%04d %02d:%02d\n",
                  st.wMonth, st.wDay, st.wYear, st.wHour, st.wMinute));
    else PRINTF (("<None>\n"));
  }

quit:
  FREE (publisher_info.program_name);
  FREE (publisher_info.publisher_link);
  FREE (publisher_info.more_info_link);
  FREE (counter_signer_info);

  if (cert_context)
     CertFreeCertificateContext (cert_context);

  if (h_store)
     CertCloseStore (h_store, 0);

  if (h_msg)
     CryptMsgClose (h_msg);

  return (res);
}

static wchar_t *evil_char_to_wchar (const char *text)
{
  wchar_t *wtext;
  int      wsize;

  if (!text)
     return (NULL);

  wsize = MultiByteToWideChar (CP_ACP, 0, text, strlen(text) + 1, NULL, 0);
  if (wsize == 0 || wsize > (ULONG_MAX/sizeof(wchar_t)))
  {
    if (wsize == 0)
       ERROR ("MultiByteToWideChar");
    return (NULL);
  }

  wtext = MALLOC (wsize * sizeof(wchar_t));
  if (wtext && !MultiByteToWideChar (CP_ACP, 0, text, strlen(text)+1, wtext, wsize))
  {
    ERROR ("MultiByteToWideChar");
    return (NULL);
  }
  return (wtext);
}

