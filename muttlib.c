/**
 * @file
 * Some miscellaneous functions
 *
 * @authors
 * Copyright (C) 1996-2000,2007,2010,2013 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 1999-2008 Thomas Roessler <roessler@does-not-exist.org>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include <stddef.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#ifdef ENABLE_NLS
#include <libintl.h>
#endif
#include <limits.h>
#include <pwd.h>
#include <regex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <wchar.h>
#include "mutt/mutt.h"
#include "mutt.h"
#include "address.h"
#include "alias.h"
#include "body.h"
#include "envelope.h"
#include "filter.h"
#include "format_flags.h"
#include "globals.h"
#include "header.h"
#include "mailbox.h"
#include "mime.h"
#include "mutt_charset.h"
#include "mutt_curses.h"
#include "mutt_regex.h"
#include "tags.h"

#include "mx.h"
#include "ncrypt/ncrypt.h"
#include "options.h"
#include "parameter.h"
#include "protos.h"
#include "url.h"
#ifdef USE_IMAP
#include "imap/imap.h"
#endif
#ifdef HAVE_SYS_SYSCALL_H
#include <sys/syscall.h>
#endif

static const char *xdg_env_vars[] = {
      [XDG_CONFIG_HOME] = "XDG_CONFIG_HOME",
      [XDG_CONFIG_DIRS] = "XDG_CONFIG_DIRS",
};

static const char *xdg_defaults[] = {
      [XDG_CONFIG_HOME] = "~/.config", [XDG_CONFIG_DIRS] = "/etc/xdg",
};

/**
 * mutt_adv_mktemp - Advanced mktemp(3)
 *
 * Modified by blong to accept a "suggestion" for file name.  If that file
 * exists, then construct one with unique name but keep any extension.  This
 * might fail, I guess.
 */
void mutt_adv_mktemp(char *s, size_t l)
{
  char prefix[_POSIX_PATH_MAX];
  char *suffix = NULL;
  struct stat sb;

  if (s[0] == '\0')
  {
    mutt_mktemp(s, l);
  }
  else
  {
    mutt_str_strfcpy(prefix, s, sizeof(prefix));
    mutt_file_sanitize_filename(prefix, 1);
    snprintf(s, l, "%s/%s", NONULL(Tmpdir), prefix);
    if (lstat(s, &sb) == -1 && errno == ENOENT)
      return;

    suffix = strrchr(prefix, '.');
    if (suffix)
    {
      *suffix = 0;
      suffix++;
    }
    mutt_mktemp_pfx_sfx(s, l, prefix, suffix);
  }
}

int mutt_remove_from_regex_list(struct RegexList **l, const char *str)
{
  struct RegexList *p = NULL, *last = NULL;
  int rc = -1;

  if (mutt_str_strcmp("*", str) == 0)
  {
    mutt_free_regex_list(l); /* ``unCMD *'' means delete all current entries */
    rc = 0;
  }
  else
  {
    p = *l;
    last = NULL;
    while (p)
    {
      if (mutt_str_strcasecmp(str, p->regex->pattern) == 0)
      {
        mutt_free_regex(&p->regex);
        if (last)
          last->next = p->next;
        else
          (*l) = p->next;
        FREE(&p);
        rc = 0;
      }
      else
      {
        last = p;
        p = p->next;
      }
    }
  }
  return rc;
}

/**
 * mutt_matches_ignore - Does the string match the ignore list
 *
 * checks Ignore and UnIgnore using mutt_list_match
 */
bool mutt_matches_ignore(const char *s)
{
  return mutt_list_match(s, &Ignore) && !mutt_list_match(s, &UnIgnore);
}

char *mutt_expand_path(char *s, size_t slen)
{
  return mutt_expand_path_regex(s, slen, 0);
}

char *mutt_expand_path_regex(char *s, size_t slen, int regex)
{
  char p[_POSIX_PATH_MAX] = "";
  char q[_POSIX_PATH_MAX] = "";
  char tmp[_POSIX_PATH_MAX];
  char *t = NULL;

  char *tail = "";

  bool recurse = false;

  do
  {
    recurse = false;

    switch (*s)
    {
      case '~':
      {
        if (*(s + 1) == '/' || *(s + 1) == 0)
        {
          mutt_str_strfcpy(p, NONULL(HomeDir), sizeof(p));
          tail = s + 1;
        }
        else
        {
          struct passwd *pw = NULL;
          if ((t = strchr(s + 1, '/')))
            *t = 0;

          if ((pw = getpwnam(s + 1)))
          {
            mutt_str_strfcpy(p, pw->pw_dir, sizeof(p));
            if (t)
            {
              *t = '/';
              tail = t;
            }
            else
              tail = "";
          }
          else
          {
            /* user not found! */
            if (t)
              *t = '/';
            *p = '\0';
            tail = s;
          }
        }
      }
      break;

      case '=':
      case '+':
      {
#ifdef USE_IMAP
        /* if folder = {host} or imap[s]://host/: don't append slash */
        if (mx_is_imap(NONULL(Folder)) &&
            (Folder[strlen(Folder) - 1] == '}' || Folder[strlen(Folder) - 1] == '/'))
        {
          mutt_str_strfcpy(p, NONULL(Folder), sizeof(p));
        }
        else
#endif
#ifdef USE_NOTMUCH
            if (mx_is_notmuch(NONULL(Folder)))
          mutt_str_strfcpy(p, NONULL(Folder), sizeof(p));
        else
#endif
            if (Folder && *Folder && Folder[strlen(Folder) - 1] == '/')
          mutt_str_strfcpy(p, NONULL(Folder), sizeof(p));
        else
          snprintf(p, sizeof(p), "%s/", NONULL(Folder));

        tail = s + 1;
      }
      break;

      /* elm compatibility, @ expands alias to user name */

      case '@':
      {
        struct Header *h = NULL;
        struct Address *alias = NULL;

        if ((alias = mutt_lookup_alias(s + 1)))
        {
          h = mutt_new_header();
          h->env = mutt_env_new();
          h->env->from = h->env->to = alias;
          mutt_default_save(p, sizeof(p), h);
          h->env->from = h->env->to = NULL;
          mutt_free_header(&h);
          /* Avoid infinite recursion if the resulting folder starts with '@' */
          if (*p != '@')
            recurse = true;

          tail = "";
        }
      }
      break;

      case '>':
      {
        mutt_str_strfcpy(p, NONULL(Mbox), sizeof(p));
        tail = s + 1;
      }
      break;

      case '<':
      {
        mutt_str_strfcpy(p, NONULL(Record), sizeof(p));
        tail = s + 1;
      }
      break;

      case '!':
      {
        if (*(s + 1) == '!')
        {
          mutt_str_strfcpy(p, NONULL(LastFolder), sizeof(p));
          tail = s + 2;
        }
        else
        {
          mutt_str_strfcpy(p, NONULL(SpoolFile), sizeof(p));
          tail = s + 1;
        }
      }
      break;

      case '-':
      {
        mutt_str_strfcpy(p, NONULL(LastFolder), sizeof(p));
        tail = s + 1;
      }
      break;

      case '^':
      {
        mutt_str_strfcpy(p, NONULL(CurrentFolder), sizeof(p));
        tail = s + 1;
      }
      break;

      default:
      {
        *p = '\0';
        tail = s;
      }
    }

    if (regex && *p && !recurse)
    {
      mutt_file_sanitize_regex(q, sizeof(q), p);
      snprintf(tmp, sizeof(tmp), "%s%s", q, tail);
    }
    else
      snprintf(tmp, sizeof(tmp), "%s%s", p, tail);

    mutt_str_strfcpy(s, tmp, slen);
  } while (recurse);

#ifdef USE_IMAP
  /* Rewrite IMAP path in canonical form - aids in string comparisons of
   * folders. May possibly fail, in which case s should be the same. */
  if (mx_is_imap(s))
    imap_expand_path(s, slen);
#endif

  return s;
}

/**
 * mutt_gecos_name - Lookup a user's real name in /etc/passwd
 *
 * Extract the real name from /etc/passwd's GECOS field.  When set, honor the
 * regular expression in GecosMask, otherwise assume that the GECOS field is a
 * comma-separated list.
 * Replace "&" by a capitalized version of the user's login name.
 */
char *mutt_gecos_name(char *dest, size_t destlen, struct passwd *pw)
{
  regmatch_t pat_match[1];
  size_t pwnl;
  char *p = NULL;

  if (!pw || !pw->pw_gecos)
    return NULL;

  memset(dest, 0, destlen);

  if (GecosMask.regex)
  {
    if (regexec(GecosMask.regex, pw->pw_gecos, 1, pat_match, 0) == 0)
      mutt_str_strfcpy(dest, pw->pw_gecos + pat_match[0].rm_so,
                       MIN(pat_match[0].rm_eo - pat_match[0].rm_so + 1, destlen));
  }
  else if ((p = strchr(pw->pw_gecos, ',')))
    mutt_str_strfcpy(dest, pw->pw_gecos, MIN(destlen, p - pw->pw_gecos + 1));
  else
    mutt_str_strfcpy(dest, pw->pw_gecos, destlen);

  pwnl = strlen(pw->pw_name);

  for (int idx = 0; dest[idx]; idx++)
  {
    if (dest[idx] == '&')
    {
      memmove(&dest[idx + pwnl], &dest[idx + 1],
              MAX((ssize_t)(destlen - idx - pwnl - 1), 0));
      memcpy(&dest[idx], pw->pw_name, MIN(destlen - idx - 1, pwnl));
      dest[idx] = toupper((unsigned char) dest[idx]);
    }
  }

  return dest;
}

/**
 * mutt_needs_mailcap - Does this type need a mailcap entry do display
 * @param m Attachment body to be displayed
 * @retval true  NeoMutt requires a mailcap entry to display
 * @retval false otherwise
 */
bool mutt_needs_mailcap(struct Body *m)
{
  switch (m->type)
  {
    case TYPETEXT:
      if (mutt_str_strcasecmp("plain", m->subtype) == 0)
        return false;
      break;
    case TYPEAPPLICATION:
      if ((WithCrypto & APPLICATION_PGP) && mutt_is_application_pgp(m))
        return false;
      if ((WithCrypto & APPLICATION_SMIME) && mutt_is_application_smime(m))
        return false;
      break;

    case TYPEMULTIPART:
    case TYPEMESSAGE:
      return false;
  }

  return true;
}

bool mutt_is_text_part(struct Body *b)
{
  int t = b->type;
  char *s = b->subtype;

  if ((WithCrypto & APPLICATION_PGP) && mutt_is_application_pgp(b))
    return false;

  if (t == TYPETEXT)
    return true;

  if (t == TYPEMESSAGE)
  {
    if (mutt_str_strcasecmp("delivery-status", s) == 0)
      return true;
  }

  if ((WithCrypto & APPLICATION_PGP) && t == TYPEAPPLICATION)
  {
    if (mutt_str_strcasecmp("pgp-keys", s) == 0)
      return true;
  }

  return false;
}

static FILE *frandom;

static void mutt_randbuf(void *out, size_t len)
{
  if (len > 1048576)
  {
    mutt_error(_("mutt_randbuf len=%zu"), len);
    exit(1);
  }
/* XXX switch to HAVE_GETRANDOM and getrandom() in about 2017 */
#if defined(SYS_getrandom) && defined(__linux__)
  long ret;
  do
  {
    ret = syscall(SYS_getrandom, out, len, 0, 0, 0, 0);
  } while ((ret == -1) && (errno == EINTR));
  if (ret == len)
    return;
/* let's try urandom in case we're on an old kernel, or the user has
   * configured selinux, seccomp or something to not allow getrandom */
#endif
  if (!frandom)
  {
    frandom = fopen("/dev/urandom", "rb");
    if (!frandom)
    {
      mutt_error(_("open /dev/urandom: %s"), strerror(errno));
      exit(1);
    }
    setbuf(frandom, NULL);
  }
  if (fread(out, 1, len, frandom) != len)
  {
    mutt_error(_("read /dev/urandom: %s"), strerror(errno));
    exit(1);
  }
}

static const unsigned char base32[] = "abcdefghijklmnopqrstuvwxyz234567";

void mutt_rand_base32(void *out, size_t len)
{
  uint8_t *p = out;

  mutt_randbuf(p, len);
  for (size_t pos = 0; pos < len; pos++)
    p[pos] = base32[p[pos] % 32];
}

uint32_t mutt_rand32(void)
{
  uint32_t ret;

  mutt_randbuf(&ret, sizeof(ret));
  return ret;
}

uint64_t mutt_rand64(void)
{
  uint64_t ret;

  mutt_randbuf(&ret, sizeof(ret));
  return ret;
}

void mutt_mktemp_full(char *s, size_t slen, const char *prefix,
                      const char *suffix, const char *src, int line)
{
  size_t n = snprintf(s, slen, "%s/%s-%s-%d-%d-%" PRIu64 "%s%s", NONULL(Tmpdir),
                      NONULL(prefix), NONULL(ShortHostname), (int) getuid(),
                      (int) getpid(), mutt_rand64(), suffix ? "." : "", NONULL(suffix));
  if (n >= slen)
    mutt_debug(1, "%s:%d: ERROR: insufficient buffer space to hold temporary "
                  "filename! slen=%zu but need %zu\n",
               src, line, slen, n);
  mutt_debug(3, "%s:%d: mutt_mktemp returns \"%s\".\n", src, line, s);
  if (unlink(s) && errno != ENOENT)
    mutt_debug(1, "%s:%d: ERROR: unlink(\"%s\"): %s (errno %d)\n", src, line, s,
               strerror(errno), errno);
}

/**
 * mutt_pretty_mailbox - Shorten a mailbox path using '~' or '='
 *
 * Collapse the pathname using ~ or = when possible
 */
void mutt_pretty_mailbox(char *s, size_t buflen)
{
  char *p = s, *q = s;
  size_t len;
  enum UrlScheme scheme;
  char tmp[PATH_MAX];

  scheme = url_check_scheme(s);

#ifdef USE_IMAP
  if (scheme == U_IMAP || scheme == U_IMAPS)
  {
    imap_pretty_mailbox(s);
    return;
  }
#endif

#ifdef USE_NOTMUCH
  if (scheme == U_NOTMUCH)
    return;
#endif

  /* if s is an url, only collapse path component */
  if (scheme != U_UNKNOWN)
  {
    p = strchr(s, ':') + 1;
    if (strncmp(p, "//", 2) == 0)
      q = strchr(p + 2, '/');
    if (!q)
      q = strchr(p, '\0');
    p = q;
  }

  /* cleanup path */
  if (strstr(p, "//") || strstr(p, "/./"))
  {
    /* first attempt to collapse the pathname, this is more
     * lightweight than realpath() and doesn't resolve links
     */
    while (*p)
    {
      if (*p == '/' && p[1] == '/')
      {
        *q++ = '/';
        p += 2;
      }
      else if (p[0] == '/' && p[1] == '.' && p[2] == '/')
      {
        *q++ = '/';
        p += 3;
      }
      else
        *q++ = *p++;
    }
    *q = 0;
  }
  else if (strstr(p, "..") && (scheme == U_UNKNOWN || scheme == U_FILE) && realpath(p, tmp))
    mutt_str_strfcpy(p, tmp, buflen - (p - s));

  len = mutt_str_strlen(Folder);
  if ((mutt_str_strncmp(s, Folder, len) == 0) && s[len] == '/')
  {
    *s++ = '=';
    memmove(s, s + len, mutt_str_strlen(s + len) + 1);
  }
  else if ((mutt_str_strncmp(s, HomeDir, (len = mutt_str_strlen(HomeDir))) == 0) &&
           s[len] == '/')
  {
    *s++ = '~';
    memmove(s, s + len - 1, mutt_str_strlen(s + len - 1) + 1);
  }
}

void mutt_pretty_size(char *s, size_t len, LOFF_T n)
{
  if (n == 0)
    mutt_str_strfcpy(s, "0K", len);
  else if (n < 10189) /* 0.1K - 9.9K */
    snprintf(s, len, "%3.1fK", (n < 103) ? 0.1 : n / 1024.0);
  else if (n < 1023949) /* 10K - 999K */
  {
    /* 51 is magic which causes 10189/10240 to be rounded up to 10 */
    snprintf(s, len, OFF_T_FMT "K", (n + 51) / 1024);
  }
  else if (n < 10433332) /* 1.0M - 9.9M */
    snprintf(s, len, "%3.1fM", n / 1048576.0);
  else /* 10M+ */
  {
    /* (10433332 + 52428) / 1048576 = 10 */
    snprintf(s, len, OFF_T_FMT "M", (n + 52428) / 1048576);
  }
}

void mutt_expand_file_fmt(char *dest, size_t destlen, const char *fmt, const char *src)
{
  char tmp[LONG_STRING];

  mutt_file_quote_filename(tmp, sizeof(tmp), src);
  mutt_expand_fmt(dest, destlen, fmt, tmp);
}

void mutt_expand_fmt(char *dest, size_t destlen, const char *fmt, const char *src)
{
  const char *p = NULL;
  char *d = NULL;
  size_t slen;
  bool found = false;

  slen = mutt_str_strlen(src);
  destlen--;

  for (p = fmt, d = dest; destlen && *p; p++)
  {
    if (*p == '%')
    {
      switch (p[1])
      {
        case '%':
          *d++ = *p++;
          destlen--;
          break;
        case 's':
          found = true;
          mutt_str_strfcpy(d, src, destlen + 1);
          d += destlen > slen ? slen : destlen;
          destlen -= destlen > slen ? slen : destlen;
          p++;
          break;
        default:
          *d++ = *p;
          destlen--;
          break;
      }
    }
    else
    {
      *d++ = *p;
      destlen--;
    }
  }

  *d = '\0';

  if (!found && destlen > 0)
  {
    mutt_str_strcat(dest, destlen, " ");
    mutt_str_strcat(dest, destlen, src);
  }
}

/**
 * mutt_check_overwrite - Ask the user if overwriting is necessary
 * @retval  0 on success
 * @retval -1 on abort
 * @retval  1 on error
 */
int mutt_check_overwrite(const char *attname, const char *path, char *fname,
                         size_t flen, int *append, char **directory)
{
  int rc = 0;
  char tmp[_POSIX_PATH_MAX];
  struct stat st;

  mutt_str_strfcpy(fname, path, flen);
  if (access(fname, F_OK) != 0)
    return 0;
  if (stat(fname, &st) != 0)
    return -1;
  if (S_ISDIR(st.st_mode))
  {
    if (directory)
    {
      switch (mutt_multi_choice
              /* L10N:
                 Means "The path you specified as the destination file is a directory."
                 See the msgid "Save to file: " (alias.c, recvattach.c) */
              (_("File is a directory, save under it? [(y)es, (n)o, (a)ll]"), _("yna")))
      {
        case 3: /* all */
          mutt_str_replace(directory, fname);
          break;
        case 1: /* yes */
          FREE(directory);
          break;
        case -1: /* abort */
          FREE(directory);
          return -1;
        case 2: /* no */
          FREE(directory);
          return 1;
      }
    }
    /* L10N:
       Means "The path you specified as the destination file is a directory."
       See the msgid "Save to file: " (alias.c, recvattach.c) */
    else if ((rc = mutt_yesorno(_("File is a directory, save under it?"), MUTT_YES)) != MUTT_YES)
      return (rc == MUTT_NO) ? 1 : -1;

    mutt_str_strfcpy(tmp, mutt_file_basename(NONULL(attname)), sizeof(tmp));
    if (mutt_get_field(_("File under directory: "), tmp, sizeof(tmp),
                       MUTT_FILE | MUTT_CLEAR) != 0 ||
        !tmp[0])
      return -1;
    mutt_file_concat_path(fname, path, tmp, flen);
  }

  if (*append == 0 && access(fname, F_OK) == 0)
  {
    switch (mutt_multi_choice(
        _("File exists, (o)verwrite, (a)ppend, or (c)ancel?"), _("oac")))
    {
      case -1: /* abort */
        return -1;
      case 3: /* cancel */
        return 1;

      case 2: /* append */
        *append = MUTT_SAVE_APPEND;
        break;
      case 1: /* overwrite */
        *append = MUTT_SAVE_OVERWRITE;
        break;
    }
  }
  return 0;
}

void mutt_save_path(char *d, size_t dsize, struct Address *a)
{
  if (a && a->mailbox)
  {
    mutt_str_strfcpy(d, a->mailbox, dsize);
    if (!option(OPT_SAVE_ADDRESS))
    {
      char *p = NULL;

      if ((p = strpbrk(d, "%@")))
        *p = 0;
    }
    mutt_str_strlower(d);
  }
  else
    *d = 0;
}

void mutt_safe_path(char *s, size_t l, struct Address *a)
{
  mutt_save_path(s, l, a);
  for (char *p = s; *p; p++)
    if ((*p == '/') || ISSPACE(*p) || !IsPrint((unsigned char) *p))
      *p = '_';
}

/**
 * mutt_apply_replace - Apply replacements to a buffer
 *
 * Note this function uses a fixed size buffer of LONG_STRING and so
 * should only be used for visual modifications, such as disp_subj.
 */
char *mutt_apply_replace(char *dbuf, size_t dlen, char *sbuf, struct ReplaceList *rlist)
{
  struct ReplaceList *l = NULL;
  static regmatch_t *pmatch = NULL;
  static int nmatch = 0;
  static char twinbuf[2][LONG_STRING];
  int switcher = 0;
  char *p = NULL;
  int n;
  size_t cpysize, tlen;
  char *src = NULL, *dst = NULL;

  if (dbuf && dlen)
    dbuf[0] = '\0';

  if (sbuf == NULL || *sbuf == '\0' || (dbuf && !dlen))
    return dbuf;

  twinbuf[0][0] = '\0';
  twinbuf[1][0] = '\0';
  src = twinbuf[switcher];
  dst = src;

  mutt_str_strfcpy(src, sbuf, LONG_STRING);

  for (l = rlist; l; l = l->next)
  {
    /* If this pattern needs more matches, expand pmatch. */
    if (l->nmatch > nmatch)
    {
      mutt_mem_realloc(&pmatch, l->nmatch * sizeof(regmatch_t));
      nmatch = l->nmatch;
    }

    if (regexec(l->regex->regex, src, l->nmatch, pmatch, 0) == 0)
    {
      tlen = 0;
      switcher ^= 1;
      dst = twinbuf[switcher];

      mutt_debug(5, "%s matches %s\n", src, l->regex->pattern);

      /* Copy into other twinbuf with substitutions */
      if (l->template)
      {
        for (p = l->template; *p && (tlen < LONG_STRING - 1);)
        {
          if (*p == '%')
          {
            p++;
            if (*p == 'L')
            {
              p++;
              cpysize = MIN(pmatch[0].rm_so, LONG_STRING - tlen - 1);
              strncpy(&dst[tlen], src, cpysize);
              tlen += cpysize;
            }
            else if (*p == 'R')
            {
              p++;
              cpysize = MIN(strlen(src) - pmatch[0].rm_eo, LONG_STRING - tlen - 1);
              strncpy(&dst[tlen], &src[pmatch[0].rm_eo], cpysize);
              tlen += cpysize;
            }
            else
            {
              n = strtoul(p, &p, 10);             /* get subst number */
              while (isdigit((unsigned char) *p)) /* skip subst token */
                p++;
              for (int i = pmatch[n].rm_so;
                   (i < pmatch[n].rm_eo) && (tlen < LONG_STRING - 1); i++)
                dst[tlen++] = src[i];
            }
          }
          else
            dst[tlen++] = *p++;
        }
      }
      dst[tlen] = '\0';
      mutt_debug(5, "subst %s\n", dst);
    }
    src = dst;
  }

  if (dbuf)
    mutt_str_strfcpy(dbuf, dst, dlen);
  else
    dbuf = mutt_str_strdup(dst);
  return dbuf;
}

/**
 * mutt_expando_format - Expand expandos (%x) in a string
 * @param[out] buf      Buffer in which to save string
 * @param[in]  buflen   Buffer length
 * @param[in]  col      Starting column
 * @param[in]  cols     Number of screen columns
 * @param[in]  src      Printf-like format string
 * @param[in]  callback Callback for processing
 * @param[in]  data     Callback data
 * @param[in]  flags    Callback flags
 */
void mutt_expando_format(char *buf, size_t buflen, size_t col, int cols, const char *src,
                         format_t *callback, unsigned long data, enum FormatFlag flags)
{
  char prefix[SHORT_STRING], tmp[LONG_STRING], *cp = NULL, *wptr = buf, ch;
  char if_str[SHORT_STRING], else_str[SHORT_STRING];
  size_t wlen, count, len, wid;
  pid_t pid;
  FILE *filter = NULL;
  int n;
  char *recycler = NULL;

  char src2[STRING];
  mutt_str_strfcpy(src2, src, mutt_str_strlen(src) + 1);
  src = src2;

  prefix[0] = '\0';
  buflen--; /* save room for the terminal \0 */
  wlen = ((flags & MUTT_FORMAT_ARROWCURSOR) && option(OPT_ARROW_CURSOR)) ? 3 : 0;
  col += wlen;

  if ((flags & MUTT_FORMAT_NOFILTER) == 0)
  {
    int off = -1;

    /* Do not consider filters if no pipe at end */
    n = mutt_str_strlen(src);
    if (n > 1 && src[n - 1] == '|')
    {
      /* Scan backwards for backslashes */
      off = n;
      while (off > 0 && src[off - 2] == '\\')
        off--;
    }

    /* If number of backslashes is even, the pipe is real. */
    /* n-off is the number of backslashes. */
    if (off > 0 && ((n - off) % 2) == 0)
    {
      struct Buffer word = {0};
      struct Buffer command = {0};
      struct Buffer srcbuf = {0};
      char srccopy[LONG_STRING];
      int i = 0;

      mutt_debug(3, "fmtpipe = %s\n", src);

      strncpy(srccopy, src, n);
      srccopy[n - 1] = '\0';

      /* prepare BUFFERs */
      mutt_buffer_from(&srcbuf, srccopy);
      mutt_buffer_rewind(&srcbuf);

      /* Iterate expansions across successive arguments */
      do
      {
        /* Extract the command name and copy to command line */
        mutt_debug(3, "fmtpipe +++: %s\n", srcbuf.dptr);
        if (word.data)
          *word.data = '\0';
        mutt_extract_token(&word, &srcbuf, 0);
        mutt_debug(3, "fmtpipe %2d: %s\n", i++, word.data);
        mutt_buffer_addch(&command, '\'');
        mutt_expando_format(buf, sizeof(buf), 0, cols, word.data, callback,
                            data, flags | MUTT_FORMAT_NOFILTER);
        for (char *p = tmp; p && *p; p++)
        {
          if (*p == '\'')
            /* shell quoting doesn't permit escaping a single quote within
             * single-quoted material.  double-quoting instead will lead
             * shell variable expansions, so break out of the single-quoted
             * span, insert a double-quoted single quote, and resume. */
            mutt_buffer_addstr(&command, "'\"'\"'");
          else
            mutt_buffer_addch(&command, *p);
        }
        mutt_buffer_addch(&command, '\'');
        mutt_buffer_addch(&command, ' ');
      } while (MoreArgs(&srcbuf));

      mutt_debug(3, "fmtpipe > %s\n", command.data);

      col -= wlen; /* reset to passed in value */
      wptr = buf;  /* reset write ptr */
      wlen = ((flags & MUTT_FORMAT_ARROWCURSOR) && option(OPT_ARROW_CURSOR)) ? 3 : 0;
      pid = mutt_create_filter(command.data, NULL, &filter, NULL);
      if (pid != -1)
      {
        int rc;

        n = fread(buf, 1, buflen /* already decremented */, filter);
        mutt_file_fclose(&filter);
        rc = mutt_wait_filter(pid);
        if (rc != 0)
          mutt_debug(1, "format pipe command exited code %d\n", rc);
        if (n > 0)
        {
          buf[n] = 0;
          while ((n > 0) && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = '\0';
          mutt_debug(3, "fmtpipe < %s\n", buf);

          /* If the result ends with '%', this indicates that the filter
           * generated %-tokens that neomutt can expand.  Eliminate the '%'
           * marker and recycle the string through mutt_expando_format().
           * To literally end with "%", use "%%". */
          if ((n > 0) && buf[n - 1] == '%')
          {
            n--;
            buf[n] = '\0'; /* remove '%' */
            if ((n > 0) && buf[n - 1] != '%')
            {
              recycler = mutt_str_strdup(buf);
              if (recycler)
              {
                /* buflen is decremented at the start of this function
                 * to save space for the terminal nul char.  We can add
                 * it back for the recursive call since the expansion of
                 * format pipes does not try to append a nul itself.
                 */
                mutt_expando_format(buf, buflen + 1, col, cols, recycler,
                                    callback, data, flags);
                FREE(&recycler);
              }
            }
          }
        }
        else
        {
          /* read error */
          mutt_debug(1, "error reading from fmtpipe: %s (errno=%d)\n",
                     strerror(errno), errno);
          *wptr = 0;
        }
      }
      else
      {
        /* Filter failed; erase write buffer */
        *wptr = '\0';
      }

      mutt_buffer_reinit(&srcbuf);
      mutt_buffer_reinit(&word);
      mutt_buffer_reinit(&command);
      return;
    }
  }

  while (*src && wlen < buflen)
  {
    if (*src == '%')
    {
      if (*++src == '%')
      {
        *wptr++ = '%';
        wlen++;
        col++;
        src++;
        continue;
      }

      if (*src == '?')
      {
        /* change original %? to new %< notation */
        /* %?x?y&z? to %<x?y&z> where y and z are nestable */
        char *p = (char *) src;
        *p = '<';
        for (; *p && *p != '?'; p++)
          ;
        /* nothing */
        if (*p == '?')
        {
          p++;
        }
        for (; *p && *p != '?'; p++)
          ;
        /* nothing */
        if (*p == '?')
        {
          *p = '>';
        }
      }

      if (*src == '<')
      {
        flags |= MUTT_FORMAT_OPTIONAL;
        ch = *(++src); /* save the character to switch on */
        src++;
        cp = prefix;
        count = 0;
        while ((count < sizeof(prefix)) && (*src != '?'))
        {
          *cp++ = *src++;
          count++;
        }
        *cp = 0;
      }
      else
      {
        flags &= ~MUTT_FORMAT_OPTIONAL;

        /* eat the format string */
        cp = prefix;
        count = 0;
        while (count < sizeof(prefix) && (isdigit((unsigned char) *src) ||
                                          *src == '.' || *src == '-' || *src == '='))
        {
          *cp++ = *src++;
          count++;
        }
        *cp = 0;

        if (!*src)
          break; /* bad format */

        ch = *src++; /* save the character to switch on */
      }

      if (flags & MUTT_FORMAT_OPTIONAL)
      {
        int lrbalance;

        if (*src != '?')
          break; /* bad format */
        src++;

        /* eat the `if' part of the string */
        cp = if_str;
        count = 0;
        lrbalance = 1;
        while ((lrbalance > 0) && (count < sizeof(if_str)) && *src)
        {
          if ((src[0] == '%') && (src[1] == '>'))
          {
            /* This is a padding expando; copy two chars and carry on */
            *cp++ = *src++;
            *cp++ = *src++;
            count += 2;
            continue;
          }

          if (*src == '\\')
          {
            src++;
            *cp++ = *src++;
          }
          else if ((src[0] == '%') && (src[1] == '<'))
          {
            lrbalance++;
          }
          else if (src[0] == '>')
          {
            lrbalance--;
          }
          if (lrbalance == 0)
            break;
          if ((lrbalance == 1) && (src[0] == '&'))
            break;
          *cp++ = *src++;
          count++;
        }
        *cp = 0;

        /* eat the `else' part of the string (optional) */
        if (*src == '&')
          src++; /* skip the & */
        cp = else_str;
        count = 0;
        while ((lrbalance > 0) && (count < sizeof(else_str)) && *src)
        {
          if ((src[0] == '%') && (src[1] == '>'))
          {
            /* This is a padding expando; copy two chars and carry on */
            *cp++ = *src++;
            *cp++ = *src++;
            count += 2;
            continue;
          }

          if (*src == '\\')
          {
            src++;
            *cp++ = *src++;
          }
          else if ((src[0] == '%') && (src[1] == '<'))
          {
            lrbalance++;
          }
          else if (src[0] == '>')
          {
            lrbalance--;
          }
          if (lrbalance == 0)
            break;
          if ((lrbalance == 1) && (src[0] == '&'))
            break;
          *cp++ = *src++;
          count++;
        }
        *cp = 0;

        if (!*src)
          break; /* bad format */

        src++; /* move past the trailing `>' (formerly '?') */
      }

      /* handle generic cases first */
      if (ch == '>' || ch == '*')
      {
        /* %>X: right justify to EOL, left takes precedence
         * %*X: right justify to EOL, right takes precedence */
        int soft = ch == '*';
        int pl, pw;
        pl = mutt_mb_charlen(src, &pw);
        if (pl <= 0)
          pl = pw = 1;

        /* see if there's room to add content, else ignore */
        if ((col < cols && wlen < buflen) || soft)
        {
          int pad;

          /* get contents after padding */
          mutt_expando_format(tmp, sizeof(tmp), 0, cols, src + pl, callback, data, flags);
          len = mutt_str_strlen(tmp);
          wid = mutt_strwidth(tmp);

          pad = (cols - col - wid) / pw;
          if (pad >= 0)
          {
            /* try to consume as many columns as we can, if we don't have
             * memory for that, use as much memory as possible */
            if (wlen + (pad * pl) + len > buflen)
              pad = (buflen > wlen + len) ? ((buflen - wlen - len) / pl) : 0;
            else
            {
              /* Add pre-spacing to make multi-column pad characters and
               * the contents after padding line up */
              while ((col + (pad * pw) + wid < cols) && (wlen + (pad * pl) + len < buflen))
              {
                *wptr++ = ' ';
                wlen++;
                col++;
              }
            }
            while (pad-- > 0)
            {
              memcpy(wptr, src, pl);
              wptr += pl;
              wlen += pl;
              col += pw;
            }
          }
          else if (soft && pad < 0)
          {
            int offset =
                ((flags & MUTT_FORMAT_ARROWCURSOR) && option(OPT_ARROW_CURSOR)) ? 3 : 0;
            int avail_cols = (cols > offset) ? (cols - offset) : 0;
            /* \0-terminate buf for length computation in mutt_wstr_trunc() */
            *wptr = 0;
            /* make sure right part is at most as wide as display */
            len = mutt_wstr_trunc(tmp, buflen, avail_cols, &wid);
            /* truncate left so that right part fits completely in */
            wlen = mutt_wstr_trunc(buf, buflen - len, avail_cols - wid, &col);
            wptr = buf + wlen;
            /* Multi-column characters may be truncated in the middle.
             * Add spacing so the right hand side lines up. */
            while ((col + wid < avail_cols) && (wlen + len < buflen))
            {
              *wptr++ = ' ';
              wlen++;
              col++;
            }
          }
          if (len + wlen > buflen)
            len = mutt_wstr_trunc(tmp, buflen - wlen, cols - col, NULL);
          memcpy(wptr, tmp, len);
          wptr += len;
          wlen += len;
          col += wid;
          src += pl;
        }
        break; /* skip rest of input */
      }
      else if (ch == '|')
      {
        /* pad to EOL */
        int pl, pw, c;
        pl = mutt_mb_charlen(src, &pw);
        if (pl <= 0)
          pl = pw = 1;

        /* see if there's room to add content, else ignore */
        if (col < cols && wlen < buflen)
        {
          c = (cols - col) / pw;
          if (c > 0 && wlen + (c * pl) > buflen)
            c = ((signed) (buflen - wlen)) / pl;
          while (c > 0)
          {
            memcpy(wptr, src, pl);
            wptr += pl;
            wlen += pl;
            col += pw;
            c--;
          }
          src += pl;
        }
        break; /* skip rest of input */
      }
      else
      {
        bool tolower = false;
        bool nodots = false;

        while (ch == '_' || ch == ':')
        {
          if (ch == '_')
            tolower = true;
          else if (ch == ':')
            nodots = true;

          ch = *src++;
        }

        /* use callback function to handle this case */
        src = callback(tmp, sizeof(tmp), col, cols, ch, src, prefix, if_str,
                       else_str, data, flags);

        if (tolower)
          mutt_str_strlower(tmp);
        if (nodots)
        {
          char *p = tmp;
          for (; *p; p++)
            if (*p == '.')
              *p = '_';
        }

        if ((len = mutt_str_strlen(tmp)) + wlen > buflen)
          len = mutt_wstr_trunc(tmp, buflen - wlen, cols - col, NULL);

        memcpy(wptr, tmp, len);
        wptr += len;
        wlen += len;
        col += mutt_strwidth(tmp);
      }
    }
    else if (*src == '\\')
    {
      if (!*++src)
        break;
      switch (*src)
      {
        case 'n':
          *wptr = '\n';
          break;
        case 't':
          *wptr = '\t';
          break;
        case 'r':
          *wptr = '\r';
          break;
        case 'f':
          *wptr = '\f';
          break;
        case 'v':
          *wptr = '\v';
          break;
        default:
          *wptr = *src;
          break;
      }
      src++;
      wptr++;
      wlen++;
      col++;
    }
    else
    {
      int bytes, width;
      /* in case of error, simply copy byte */
      bytes = mutt_mb_charlen(src, &width);
      if (bytes < 0)
      {
        bytes = 1;
        width = 1;
      }
      if ((bytes > 0) && ((wlen + bytes) < buflen))
      {
        memcpy(wptr, src, bytes);
        wptr += bytes;
        src += bytes;
        wlen += bytes;
        col += width;
      }
      else
      {
        src += buflen - wlen;
        wlen = buflen;
      }
    }
  }
  *wptr = 0;
}

/**
 * mutt_open_read - Run a command to read from
 *
 * This function allows the user to specify a command to read stdout from in
 * place of a normal file.  If the last character in the string is a pipe (|),
 * then we assume it is a command to run instead of a normal file.
 */
FILE *mutt_open_read(const char *path, pid_t *thepid)
{
  FILE *f = NULL;
  struct stat s;

  int len = mutt_str_strlen(path);

  if (path[len - 1] == '|')
  {
    /* read from a pipe */

    char *p = mutt_str_strdup(path);

    p[len - 1] = 0;
    mutt_endwin(NULL);
    *thepid = mutt_create_filter(p, NULL, &f, NULL);
    FREE(&p);
  }
  else
  {
    if (stat(path, &s) < 0)
      return NULL;
    if (S_ISDIR(s.st_mode))
    {
      errno = EINVAL;
      return NULL;
    }
    f = fopen(path, "r");
    *thepid = -1;
  }
  return f;
}

/**
 * mutt_save_confirm - Ask the user to save
 * @retval  0 if OK to proceed
 * @retval -1 to abort
 * @retval  1 to retry
 */
int mutt_save_confirm(const char *s, struct stat *st)
{
  char tmp[_POSIX_PATH_MAX];
  int ret = 0;
  int rc;
  int magic = 0;

  magic = mx_get_magic(s);

#ifdef USE_POP
  if (magic == MUTT_POP)
  {
    mutt_error(_("Can't save message to POP mailbox."));
    return 1;
  }
#endif

  if (magic > 0 && !mx_access(s, W_OK))
  {
    if (option(OPT_CONFIRMAPPEND))
    {
      snprintf(tmp, sizeof(tmp), _("Append messages to %s?"), s);
      rc = mutt_yesorno(tmp, MUTT_YES);
      if (rc == MUTT_NO)
        ret = 1;
      else if (rc == MUTT_ABORT)
        ret = -1;
    }
  }

#ifdef USE_NNTP
  if (magic == MUTT_NNTP)
  {
    mutt_error(_("Can't save message to news server."));
    return 0;
  }
#endif

  if (stat(s, st) != -1)
  {
    if (magic == -1)
    {
      mutt_error(_("%s is not a mailbox!"), s);
      return 1;
    }
  }
  else if (magic != MUTT_IMAP)
  {
    st->st_mtime = 0;
    st->st_atime = 0;

    /* pathname does not exist */
    if (errno == ENOENT)
    {
      if (option(OPT_CONFIRMCREATE))
      {
        snprintf(tmp, sizeof(tmp), _("Create %s?"), s);
        rc = mutt_yesorno(tmp, MUTT_YES);
        if (rc == MUTT_NO)
          ret = 1;
        else if (rc == MUTT_ABORT)
          ret = -1;
      }

      /* user confirmed with MUTT_YES or set OPT_CONFIRMCREATE */
      if (ret == 0)
      {
        /* create dir recursively */
        if (mutt_file_mkdir(mutt_file_dirname(s), S_IRWXU) == -1)
        {
          /* report failure & abort */
          mutt_perror(s);
          return 1;
        }
      }
    }
    else
    {
      mutt_perror(s);
      return 1;
    }
  }

  mutt_window_clearline(MuttMessageWindow, 0);
  return ret;
}

void mutt_sleep(short s)
{
  if (SleepTime > s)
    sleep(SleepTime);
  else if (s)
    sleep(s);
}

const char *mutt_make_version(void)
{
  static char vstring[STRING];
  snprintf(vstring, sizeof(vstring), "NeoMutt %s%s", PACKAGE_VERSION, GitVer);
  return vstring;
}

struct Regex *mutt_compile_regex(const char *s, int flags)
{
  struct Regex *pp = mutt_mem_calloc(1, sizeof(struct Regex));
  pp->pattern = mutt_str_strdup(s);
  pp->regex = mutt_mem_calloc(1, sizeof(regex_t));
  if (REGCOMP(pp->regex, NONULL(s), flags) != 0)
    mutt_free_regex(&pp);

  return pp;
}

void mutt_free_regex(struct Regex **pp)
{
  FREE(&(*pp)->pattern);
  regfree((*pp)->regex);
  FREE(&(*pp)->regex);
  FREE(pp);
}

void mutt_free_regex_list(struct RegexList **list)
{
  struct RegexList *p = NULL;

  if (!list)
    return;
  while (*list)
  {
    p = *list;
    *list = (*list)->next;
    mutt_free_regex(&p->regex);
    FREE(&p);
  }
}

void mutt_free_replace_list(struct ReplaceList **list)
{
  struct ReplaceList *p = NULL;

  if (!list)
    return;
  while (*list)
  {
    p = *list;
    *list = (*list)->next;
    mutt_free_regex(&p->regex);
    FREE(&p->template);
    FREE(&p);
  }
}

bool mutt_match_regex_list(const char *s, struct RegexList *l)
{
  if (!s)
    return false;

  for (; l; l = l->next)
  {
    if (regexec(l->regex->regex, s, (size_t) 0, (regmatch_t *) 0, (int) 0) == 0)
    {
      mutt_debug(5, "%s matches %s\n", s, l->regex->pattern);
      return true;
    }
  }

  return false;
}

/**
 * mutt_match_spam_list - Does a string match a spam pattern
 * @param s        String to check
 * @param l        List of spam patterns
 * @param text     Buffer to save match
 * @param textsize Buffer length
 * @retval true if \a s matches a pattern in \a l
 * @retval false otherwise
 *
 * Match a string against the patterns defined by the 'spam' command and output
 * the expanded format into `text` when there is a match.  If textsize<=0, the
 * match is performed but the format is not expanded and no assumptions are made
 * about the value of `text` so it may be NULL.
 */
bool mutt_match_spam_list(const char *s, struct ReplaceList *l, char *text, int textsize)
{
  static regmatch_t *pmatch = NULL;
  static int nmatch = 0;
  int tlen = 0;
  char *p = NULL;

  if (!s)
    return false;

  for (; l; l = l->next)
  {
    /* If this pattern needs more matches, expand pmatch. */
    if (l->nmatch > nmatch)
    {
      mutt_mem_realloc(&pmatch, l->nmatch * sizeof(regmatch_t));
      nmatch = l->nmatch;
    }

    /* Does this pattern match? */
    if (regexec(l->regex->regex, s, (size_t) l->nmatch, (regmatch_t *) pmatch, (int) 0) == 0)
    {
      mutt_debug(5, "%s matches %s\n", s, l->regex->pattern);
      mutt_debug(5, "%d subs\n", (int) l->regex->regex->re_nsub);

      /* Copy template into text, with substitutions. */
      for (p = l->template; *p && tlen < textsize - 1;)
      {
        /* backreference to pattern match substring, eg. %1, %2, etc) */
        if (*p == '%')
        {
          char *e = NULL; /* used as pointer to end of integer backreference in strtol() call */
          int n;

          p++; /* skip over % char */
          n = strtol(p, &e, 10);
          /* Ensure that the integer conversion succeeded (e!=p) and bounds check.  The upper bound check
           * should not strictly be necessary since add_to_spam_list() finds the largest value, and
           * the static array above is always large enough based on that value. */
          if (e != p && n >= 0 && n <= l->nmatch && pmatch[n].rm_so != -1)
          {
            /* copy as much of the substring match as will fit in the output buffer, saving space for
             * the terminating nul char */
            int idx;
            for (idx = pmatch[n].rm_so;
                 (idx < pmatch[n].rm_eo) && (tlen < textsize - 1); ++idx)
              text[tlen++] = s[idx];
          }
          p = e; /* skip over the parsed integer */
        }
        else
        {
          text[tlen++] = *p++;
        }
      }
      /* tlen should always be less than textsize except when textsize<=0
       * because the bounds checks in the above code leave room for the
       * terminal nul char.   This should avoid returning an unterminated
       * string to the caller.  When textsize<=0 we make no assumption about
       * the validity of the text pointer. */
      if (tlen < textsize)
      {
        text[tlen] = '\0';
        mutt_debug(5, "\"%s\"\n", text);
      }
      return true;
    }
  }

  return false;
}

void mutt_encode_path(char *dest, size_t dlen, const char *src)
{
  char *p = mutt_str_strdup(src);
  int rc = mutt_convert_string(&p, Charset, "utf-8", 0);
  /* `src' may be NULL, such as when called from the pop3 driver. */
  mutt_str_strfcpy(dest, (rc == 0) ? NONULL(p) : NONULL(src), dlen);
  FREE(&p);
}

/**
 * mutt_set_xdg_path - Find an XDG path or its fallback
 * @param type    Type of XDG variable, e.g. #XDG_CONFIG_HOME
 * @param buf     Buffer to save path
 * @param bufsize Buffer length
 * @retval 1 if an entry was found that actually exists on disk and 0 otherwise
 *
 * Process an XDG environment variable or its fallback.
 */
int mutt_set_xdg_path(enum XdgType type, char *buf, size_t bufsize)
{
  char *xdg_env = getenv(xdg_env_vars[type]);
  char *xdg = (xdg_env && *xdg_env) ? mutt_str_strdup(xdg_env) :
                                      mutt_str_strdup(xdg_defaults[type]);
  char *x = xdg; /* strsep() changes xdg, so free x instead later */
  char *token = NULL;
  int rc = 0;

  while ((token = strsep(&xdg, ":")))
  {
    if (snprintf(buf, bufsize, "%s/%s/neomuttrc", token, PACKAGE) < 0)
      continue;
    mutt_expand_path(buf, bufsize);
    if (access(buf, F_OK) == 0)
    {
      rc = 1;
      break;
    }

    if (snprintf(buf, bufsize, "%s/%s/Muttrc", token, PACKAGE) < 0)
      continue;
    mutt_expand_path(buf, bufsize);
    if (access(buf, F_OK) == 0)
    {
      rc = 1;
      break;
    }
  }

  FREE(&x);
  return rc;
}

void mutt_get_parent_path(char *output, char *path, size_t olen)
{
#ifdef USE_IMAP
  if (mx_is_imap(path))
    imap_get_parent_path(output, path, olen);
  else
#endif
#ifdef USE_NOTMUCH
      if (mx_is_notmuch(path))
    mutt_str_strfcpy(output, NONULL(Folder), olen);
  else
#endif
  {
    mutt_str_strfcpy(output, path, olen);
    int n = mutt_str_strlen(output);

    /* Remove everything until the next slash */
    for (n--; ((n >= 0) && (output[n] != '/')); n--)
      ;

    if (n > 0)
      output[n] = '\0';
    else
    {
      output[0] = '/';
      output[1] = '\0';
    }
  }
}

char debugfilename[_POSIX_PATH_MAX];
FILE *debugfile = NULL;
int debuglevel;
char *debugfile_cmdline = NULL;
int debuglevel_cmdline;

int mutt_debug_real(const char *function, const char *file, int line, int level, ...)
{
  va_list ap;
  time_t now = time(NULL);
  static char buf[23] = "";
  static time_t last = 0;
  int ret = 0;

  if ((debuglevel < level) || !debugfile)
    return 0;

  if (now > last)
  {
    ret += strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    last = now;
  }
  ret += fprintf(debugfile, "[%s] %s() ", buf, function);

  va_start(ap, level);
  const char *fmt = va_arg(ap, const char *);
  ret += vfprintf(debugfile, fmt, ap);
  va_end(ap);
  return ret;
}

/**
 * mutt_inbox_cmp - do two folders share the same path and one is an inbox
 * @param a First path
 * @param b Second path
 * @retval -1 if a is INBOX of b
 * @retval 0 if none is INBOX
 * @retval 1 if b is INBOX for a
 *
 * This function compares two folder paths. It first looks for the position of
 * the last common '/' character. If a valid position is found and it's not the
 * last character in any of the two paths, the remaining parts of the paths are
 * compared (case insensitively) with the string "INBOX". If one of the two
 * paths matches, it's reported as being less than the other and the function
 * returns -1 (a < b) or 1 (a > b). If no paths match the requirements, the two
 * paths are considered equivalent and this function returns 0.
 *
 * Examples:
 * * mutt_inbox_cmp("/foo/bar",      "/foo/baz") --> 0
 * * mutt_inbox_cmp("/foo/bar/",     "/foo/bar/inbox") --> 0
 * * mutt_inbox_cmp("/foo/bar/sent", "/foo/bar/inbox") --> 1
 * * mutt_inbox_cmp("=INBOX",        "=Drafts") --> -1
 */
int mutt_inbox_cmp(const char *a, const char *b)
{
  /* fast-track in case the paths have been mutt_pretty_mailbox'ified */
  if (a[0] == '=' && b[0] == '=')
    return (mutt_str_strcasecmp(a + 1, "inbox") == 0) ?
               -1 :
               (mutt_str_strcasecmp(b + 1, "inbox") == 0) ? 1 : 0;

  const char *a_end = strrchr(a, '/');
  const char *b_end = strrchr(b, '/');

  /* If one path contains a '/', but not the other */
  if (!a_end ^ !b_end)
    return 0;

  /* If neither path contains a '/' */
  if (!a_end)
    return 0;

  /* Compare the subpaths */
  size_t a_len = a_end - a;
  size_t b_len = b_end - b;
  size_t min = MIN(a_len, b_len);
  int same = (a[min] == '/') && (b[min] == '/') && (a[min + 1] != '\0') &&
             (b[min + 1] != '\0') && (mutt_str_strncasecmp(a, b, min) == 0);

  if (!same)
    return 0;

  if (mutt_str_strcasecmp(&a[min + 1], "inbox") == 0)
    return -1;

  if (mutt_str_strcasecmp(&b[min + 1], "inbox") == 0)
    return 1;

  return 0;
}
