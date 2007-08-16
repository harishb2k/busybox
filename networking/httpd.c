/* vi: set sw=4 ts=4: */
/*
 * httpd implementation for busybox
 *
 * Copyright (C) 2002,2003 Glenn Engel <glenne@engel.org>
 * Copyright (C) 2003-2006 Vladimir Oleynik <dzo@simtreas.ru>
 *
 * simplify patch stolen from libbb without using strdup
 *
 * Licensed under GPLv2 or later, see file LICENSE in this tarball for details.
 *
 *****************************************************************************
 *
 * Typical usage:
 *   for non root user
 * httpd -p 8080 -h $HOME/public_html
 *   or for daemon start from rc script with uid=0:
 * httpd -u www
 * This is equivalent if www user have uid=80 to
 * httpd -p 80 -u 80 -h /www -c /etc/httpd.conf -r "Web Server Authentication"
 *
 *
 * When a url contains "cgi-bin" it is assumed to be a cgi script.  The
 * server changes directory to the location of the script and executes it
 * after setting QUERY_STRING and other environment variables.
 *
 * Doc:
 * "CGI Environment Variables": http://hoohoo.ncsa.uiuc.edu/cgi/env.html
 *
 * The server can also be invoked as a url arg decoder and html text encoder
 * as follows:
 *  foo=`httpd -d $foo`           # decode "Hello%20World" as "Hello World"
 *  bar=`httpd -e "<Hello World>"`  # encode as "&#60Hello&#32World&#62"
 * Note that url encoding for arguments is not the same as html encoding for
 * presentation.  -d decodes a url-encoded argument while -e encodes in html
 * for page display.
 *
 * httpd.conf has the following format:
 *
 * A:172.20.         # Allow address from 172.20.0.0/16
 * A:10.0.0.0/25     # Allow any address from 10.0.0.0-10.0.0.127
 * A:10.0.0.0/255.255.255.128  # Allow any address that previous set
 * A:127.0.0.1       # Allow local loopback connections
 * D:*               # Deny from other IP connections
 * /cgi-bin:foo:bar  # Require user foo, pwd bar on urls starting with /cgi-bin/
 * /adm:admin:setup  # Require user admin, pwd setup on urls starting with /adm/
 * /adm:toor:PaSsWd  # or user toor, pwd PaSsWd on urls starting with /adm/
 * .au:audio/basic   # additional mime type for audio.au files
 * *.php:/path/php   # running cgi.php scripts through an interpreter
 *
 * A/D may be as a/d or allow/deny - first char case insensitive
 * Deny IP rules take precedence over allow rules.
 *
 *
 * The Deny/Allow IP logic:
 *
 *  - Default is to allow all.  No addresses are denied unless
 *         denied with a D: rule.
 *  - Order of Deny/Allow rules is significant
 *  - Deny rules take precedence over allow rules.
 *  - If a deny all rule (D:*) is used it acts as a catch-all for unmatched
 *       addresses.
 *  - Specification of Allow all (A:*) is a no-op
 *
 * Example:
 *   1. Allow only specified addresses
 *     A:172.20          # Allow any address that begins with 172.20.
 *     A:10.10.          # Allow any address that begins with 10.10.
 *     A:127.0.0.1       # Allow local loopback connections
 *     D:*               # Deny from other IP connections
 *
 *   2. Only deny specified addresses
 *     D:1.2.3.        # deny from 1.2.3.0 - 1.2.3.255
 *     D:2.3.4.        # deny from 2.3.4.0 - 2.3.4.255
 *     A:*             # (optional line added for clarity)
 *
 * If a sub directory contains a config file it is parsed and merged with
 * any existing settings as if it was appended to the original configuration.
 *
 * subdir paths are relative to the containing subdir and thus cannot
 * affect the parent rules.
 *
 * Note that since the sub dir is parsed in the forked thread servicing the
 * subdir http request, any merge is discarded when the process exits.  As a
 * result, the subdir settings only have a lifetime of a single request.
 *
 *
 * If -c is not set, an attempt will be made to open the default
 * root configuration file.  If -c is set and the file is not found, the
 * server exits with an error.
 *
*/

#include "libbb.h"
#if ENABLE_FEATURE_HTTPD_USE_SENDFILE
#include <sys/sendfile.h>
#endif

/* amount of buffering in a pipe */
#ifndef PIPE_BUF
# define PIPE_BUF 4096
#endif

static const char default_path_httpd_conf[] ALIGN1 = "/etc";
static const char httpd_conf[] ALIGN1 = "httpd.conf";

#define TIMEOUT 60

// Note: busybox xfuncs are not used because we want the server to keep running
//       if something bad happens due to a malformed user request.
//       As a result, all memory allocation after daemonize
//       is checked rigorously

//#define DEBUG 1
#define DEBUG 0

#define MAX_MEMORY_BUF 8192    /* IO buffer */

typedef struct Htaccess {
	char *after_colon;
	struct Htaccess *next;
	char before_colon[1];  /* really bigger, must be last */
} Htaccess;

typedef struct Htaccess_IP {
	unsigned ip;
	unsigned mask;
	int allow_deny;
	struct Htaccess_IP *next;
} Htaccess_IP;

struct globals {
	int server_socket;
	int accepted_socket;
	int verbose;
	volatile smallint alarm_signaled;
	smallint flg_deny_all;

	unsigned rmt_ip;
	unsigned tcp_port;       /* for set env REMOTE_PORT */
	const char *bind_addr_or_port;

	const char *g_query;
	const char *configFile;
	const char *home_httpd;

	const char *found_mime_type;
	const char *found_moved_temporarily;
	time_t last_mod;
	off_t ContentLength;          /* -1 - unknown */
	Htaccess_IP *ip_a_d;          /* config allow/deny lines */

	USE_FEATURE_HTTPD_BASIC_AUTH(const char *g_realm;)
	USE_FEATURE_HTTPD_BASIC_AUTH(char *remoteuser;)
	USE_FEATURE_HTTPD_CGI(char *referer;)
	USE_FEATURE_HTTPD_CGI(char *user_agent;)

	char *rmt_ip_str;        /* for set env REMOTE_ADDR */

#if ENABLE_FEATURE_HTTPD_BASIC_AUTH
	Htaccess *g_auth;        /* config user:password lines */
#endif
#if ENABLE_FEATURE_HTTPD_CONFIG_WITH_MIME_TYPES
	Htaccess *mime_a;             /* config mime types */
#endif
#if ENABLE_FEATURE_HTTPD_CONFIG_WITH_SCRIPT_INTERPR
	Htaccess *script_i;           /* config script interpreters */
#endif
	char iobuf[MAX_MEMORY_BUF];
};
#define G (*ptr_to_globals)
#define server_socket     (G.server_socket    )
#define accepted_socket   (G.accepted_socket  )
#define verbose           (G.verbose          )
#define alarm_signaled    (G.alarm_signaled   )
#define flg_deny_all      (G.flg_deny_all     )
#define rmt_ip            (G.rmt_ip           )
#define tcp_port          (G.tcp_port         )
#define bind_addr_or_port (G.bind_addr_or_port)
#define g_query           (G.g_query          )
#define configFile        (G.configFile       )
#define home_httpd        (G.home_httpd       )
#define found_mime_type   (G.found_mime_type  )
#define found_moved_temporarily (G.found_moved_temporarily)
#define ContentLength     (G.ContentLength    )
#define last_mod          (G.last_mod         )
#define ip_a_d            (G.ip_a_d           )
#define g_realm           (G.g_realm          )
#define remoteuser        (G.remoteuser       )
#define referer           (G.referer          )
#define user_agent        (G.user_agent       )
#define rmt_ip_str        (G.rmt_ip_str       )
#define g_auth            (G.g_auth           )
#define mime_a            (G.mime_a           )
#define script_i          (G.script_i         )
#define iobuf             (G.iobuf            )
#define INIT_G() do { \
	PTR_TO_GLOBALS = xzalloc(sizeof(G)); \
	USE_FEATURE_HTTPD_BASIC_AUTH(g_realm = "Web Server Authentication";) \
	bind_addr_or_port = "80"; \
	ContentLength = -1; \
} while (0)

static const char request_GET[] ALIGN1 = "GET";    /* size algorithmic optimize */

static const char *const suffixTable[] = {
/* Warning: shorter equivalent suffix in one line must be first */
	".htm.html", "text/html",
	".jpg.jpeg", "image/jpeg",
	".gif", "image/gif",
	".png", "image/png",
	".txt.h.c.cc.cpp", "text/plain",
	".css", "text/css",
	".wav", "audio/wav",
	".avi", "video/x-msvideo",
	".qt.mov", "video/quicktime",
	".mpe.mpeg", "video/mpeg",
	".mid.midi", "audio/midi",
	".mp3", "audio/mpeg",
#if 0                        /* unpopular */
	".au", "audio/basic",
	".pac", "application/x-ns-proxy-autoconfig",
	".vrml.wrl", "model/vrml",
#endif
	0, "application/octet-stream" /* default */
};

typedef enum {
	HTTP_OK = 200,
	HTTP_MOVED_TEMPORARILY = 302,
	HTTP_BAD_REQUEST = 400,       /* malformed syntax */
	HTTP_UNAUTHORIZED = 401, /* authentication needed, respond with auth hdr */
	HTTP_NOT_FOUND = 404,
	HTTP_FORBIDDEN = 403,
	HTTP_REQUEST_TIMEOUT = 408,
	HTTP_NOT_IMPLEMENTED = 501,   /* used for unrecognized requests */
	HTTP_INTERNAL_SERVER_ERROR = 500,
#if 0 /* future use */
	HTTP_CONTINUE = 100,
	HTTP_SWITCHING_PROTOCOLS = 101,
	HTTP_CREATED = 201,
	HTTP_ACCEPTED = 202,
	HTTP_NON_AUTHORITATIVE_INFO = 203,
	HTTP_NO_CONTENT = 204,
	HTTP_MULTIPLE_CHOICES = 300,
	HTTP_MOVED_PERMANENTLY = 301,
	HTTP_NOT_MODIFIED = 304,
	HTTP_PAYMENT_REQUIRED = 402,
	HTTP_BAD_GATEWAY = 502,
	HTTP_SERVICE_UNAVAILABLE = 503, /* overload, maintenance */
	HTTP_RESPONSE_SETSIZE = 0xffffffff
#endif
} HttpResponseNum;

typedef struct {
	HttpResponseNum type;
	const char *name;
	const char *info;
} HttpEnumString;

static const HttpEnumString httpResponseNames[] = {
	{ HTTP_OK, "OK", NULL },
	{ HTTP_MOVED_TEMPORARILY, "Found", "Directories must end with a slash." },
	{ HTTP_REQUEST_TIMEOUT, "Request Timeout",
		"No request appeared within a reasonable time period." },
	{ HTTP_NOT_IMPLEMENTED, "Not Implemented",
		"The requested method is not recognized by this server." },
#if ENABLE_FEATURE_HTTPD_BASIC_AUTH
	{ HTTP_UNAUTHORIZED, "Unauthorized", "" },
#endif
	{ HTTP_NOT_FOUND, "Not Found",
		"The requested URL was not found on this server." },
	{ HTTP_BAD_REQUEST, "Bad Request", "Unsupported method." },
	{ HTTP_FORBIDDEN, "Forbidden", "" },
	{ HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error",
		"Internal Server Error" },
#if 0                               /* not implemented */
	{ HTTP_CREATED, "Created" },
	{ HTTP_ACCEPTED, "Accepted" },
	{ HTTP_NO_CONTENT, "No Content" },
	{ HTTP_MULTIPLE_CHOICES, "Multiple Choices" },
	{ HTTP_MOVED_PERMANENTLY, "Moved Permanently" },
	{ HTTP_NOT_MODIFIED, "Not Modified" },
	{ HTTP_BAD_GATEWAY, "Bad Gateway", "" },
	{ HTTP_SERVICE_UNAVAILABLE, "Service Unavailable", "" },
#endif
};


static const char RFC1123FMT[] ALIGN1 = "%a, %d %b %Y %H:%M:%S GMT";


#define STRNCASECMP(a, str) strncasecmp((a), (str), sizeof(str)-1)


static int scan_ip(const char **ep, unsigned *ip, unsigned char endc)
{
	const char *p = *ep;
	int auto_mask = 8;
	int j;

	*ip = 0;
	for (j = 0; j < 4; j++) {
		unsigned octet;

		if ((*p < '0' || *p > '9') && (*p != '/' || j == 0) && *p != 0)
			return -auto_mask;
		octet = 0;
		while (*p >= '0' && *p <= '9') {
			octet *= 10;
			octet += *p - '0';
			if (octet > 255)
				return -auto_mask;
			p++;
		}
		if (*p == '.')
			p++;
		if (*p != '/' && *p != 0)
			auto_mask += 8;
		*ip = ((*ip) << 8) | octet;
	}
	if (*p != 0) {
		if (*p != endc)
			return -auto_mask;
		p++;
		if (*p == 0)
			return -auto_mask;
	}
	*ep = p;
	return auto_mask;
}

static int scan_ip_mask(const char *ipm, unsigned *ip, unsigned *mask)
{
	int i;
	unsigned msk;

	i = scan_ip(&ipm, ip, '/');
	if (i < 0)
		return i;
	if (*ipm) {
		const char *p = ipm;

		i = 0;
		while (*p) {
			if (*p < '0' || *p > '9') {
				if (*p == '.') {
					i = scan_ip(&ipm, mask, 0);
					return i != 32;
				}
				return -1;
			}
			i *= 10;
			i += *p - '0';
			p++;
		}
	}
	if (i > 32 || i < 0)
		return -1;
	msk = 0x80000000;
	*mask = 0;
	while (i > 0) {
		*mask |= msk;
		msk >>= 1;
		i--;
	}
	return 0;
}

#if ENABLE_FEATURE_HTTPD_BASIC_AUTH \
 || ENABLE_FEATURE_HTTPD_CONFIG_WITH_MIME_TYPES \
 || ENABLE_FEATURE_HTTPD_CONFIG_WITH_SCRIPT_INTERPR
static void free_config_lines(Htaccess **pprev)
{
	Htaccess *prev = *pprev;

	while (prev) {
		Htaccess *cur = prev;

		prev = cur->next;
		free(cur);
	}
	*pprev = NULL;
}
#endif

/* flag */
#define FIRST_PARSE          0
#define SUBDIR_PARSE         1
#define SIGNALED_PARSE       2
#define FIND_FROM_HTTPD_ROOT 3
/****************************************************************************
 *
 > $Function: parse_conf()
 *
 * $Description: parse configuration file into in-memory linked list.
 *
 * The first non-white character is examined to determine if the config line
 * is one of the following:
 *    .ext:mime/type   # new mime type not compiled into httpd
 *    [adAD]:from      # ip address allow/deny, * for wildcard
 *    /path:user:pass  # username/password
 *
 * Any previous IP rules are discarded.
 * If the flag argument is not SUBDIR_PARSE then all /path and mime rules
 * are also discarded.  That is, previous settings are retained if flag is
 * SUBDIR_PARSE.
 *
 * $Parameters:
 *      (const char *) path . . null for ip address checks, path for password
 *                              checks.
 *      (int) flag  . . . . . . the source of the parse request.
 *
 * $Return: (None)
 *
 ****************************************************************************/
static void parse_conf(const char *path, int flag)
{
	FILE *f;
#if ENABLE_FEATURE_HTTPD_BASIC_AUTH
	Htaccess *prev;
#endif
#if ENABLE_FEATURE_HTTPD_BASIC_AUTH \
 || ENABLE_FEATURE_HTTPD_CONFIG_WITH_MIME_TYPES \
 || ENABLE_FEATURE_HTTPD_CONFIG_WITH_SCRIPT_INTERPR
	Htaccess *cur;
#endif

	const char *cf = configFile;
	char buf[160];
	char *p0 = NULL;
	char *c, *p;

	/* free previous ip setup if present */
	Htaccess_IP *pip = ip_a_d;

	while (pip) {
		Htaccess_IP *cur_ipl = pip;

		pip = cur_ipl->next;
		free(cur_ipl);
	}
	ip_a_d = NULL;

	flg_deny_all = 0;

#if ENABLE_FEATURE_HTTPD_BASIC_AUTH \
 || ENABLE_FEATURE_HTTPD_CONFIG_WITH_MIME_TYPES \
 || ENABLE_FEATURE_HTTPD_CONFIG_WITH_SCRIPT_INTERPR
	/* retain previous auth and mime config only for subdir parse */
	if (flag != SUBDIR_PARSE) {
#if ENABLE_FEATURE_HTTPD_BASIC_AUTH
		free_config_lines(&g_auth);
#endif
#if ENABLE_FEATURE_HTTPD_CONFIG_WITH_MIME_TYPES
		free_config_lines(&mime_a);
#endif
#if ENABLE_FEATURE_HTTPD_CONFIG_WITH_SCRIPT_INTERPR
		free_config_lines(&script_i);
#endif
	}
#endif

	if (flag == SUBDIR_PARSE || cf == NULL) {
		cf = alloca(strlen(path) + sizeof(httpd_conf) + 2);
		if (cf == NULL) {
			if (flag == FIRST_PARSE)
			bb_error_msg_and_die(bb_msg_memory_exhausted);
			return;
		}
		sprintf((char *)cf, "%s/%s", path, httpd_conf);
	}

	while ((f = fopen(cf, "r")) == NULL) {
		if (flag == SUBDIR_PARSE || flag == FIND_FROM_HTTPD_ROOT) {
			/* config file not found, no changes to config */
			return;
		}
		if (configFile && flag == FIRST_PARSE) /* if -c option given */
			bb_perror_msg_and_die("%s", cf);
		flag = FIND_FROM_HTTPD_ROOT;
		cf = httpd_conf;
	}

#if ENABLE_FEATURE_HTTPD_BASIC_AUTH
	prev = g_auth;
#endif
	/* This could stand some work */
	while ((p0 = fgets(buf, sizeof(buf), f)) != NULL) {
		c = NULL;
		for (p = p0; *p0 != 0 && *p0 != '#'; p0++) {
			if (!isspace(*p0)) {
				*p++ = *p0;
				if (*p0 == ':' && c == NULL)
				c = p;
			}
		}
		*p = 0;

		/* test for empty or strange line */
		if (c == NULL || *c == 0)
			continue;
		p0 = buf;
		if (*p0 == 'd')
				*p0 = 'D';
		if (*c == '*') {
			if (*p0 == 'D') {
				/* memorize deny all */
				flg_deny_all = 1;
			}
			/* skip default other "word:*" config lines */
			continue;
		}

		if (*p0 == 'a')
			*p0 = 'A';
		else if (*p0 != 'D' && *p0 != 'A'
#if ENABLE_FEATURE_HTTPD_BASIC_AUTH
			 && *p0 != '/'
#endif
#if ENABLE_FEATURE_HTTPD_CONFIG_WITH_MIME_TYPES
			 && *p0 != '.'
#endif
#if ENABLE_FEATURE_HTTPD_CONFIG_WITH_SCRIPT_INTERPR
			 && *p0 != '*'
#endif
			)
			 continue;
		if (*p0 == 'A' || *p0 == 'D') {
			/* storing current config IP line */
			pip = xzalloc(sizeof(Htaccess_IP));
			if (pip) {
				if (scan_ip_mask(c, &(pip->ip), &(pip->mask))) {
					/* syntax IP{/mask} error detected, protect all */
					*p0 = 'D';
					pip->mask = 0;
				}
				pip->allow_deny = *p0;
				if (*p0 == 'D') {
					/* Deny:form_IP move top */
					pip->next = ip_a_d;
					ip_a_d = pip;
				} else {
					/* add to bottom A:form_IP config line */
					Htaccess_IP *prev_IP = ip_a_d;

					if (prev_IP == NULL) {
						ip_a_d = pip;
					} else {
						while (prev_IP->next)
							prev_IP = prev_IP->next;
						prev_IP->next = pip;
					}
				}
			}
			continue;
		}
#if ENABLE_FEATURE_HTTPD_BASIC_AUTH
		if (*p0 == '/') {
			/* make full path from httpd root / curent_path / config_line_path */
			cf = flag == SUBDIR_PARSE ? path : "";
			p0 = malloc(strlen(cf) + (c - buf) + 2 + strlen(c));
			if (p0 == NULL)
				continue;
			c[-1] = 0;
			sprintf(p0, "/%s%s", cf, buf);

			/* another call bb_simplify_path */
			cf = p = p0;

			do {
				if (*p == '/') {
					if (*cf == '/') {    /* skip duplicate (or initial) slash */
						continue;
					} else if (*cf == '.') {
						if (cf[1] == '/' || cf[1] == 0) { /* remove extra '.' */
							continue;
						} else if ((cf[1] == '.') && (cf[2] == '/' || cf[2] == 0)) {
							++cf;
							if (p > p0) {
								while (*--p != '/') /* omit previous dir */;
							}
							continue;
						}
					}
				}
				*++p = *cf;
			} while (*++cf);

			if ((p == p0) || (*p != '/')) {      /* not a trailing slash */
				++p;                             /* so keep last character */
			}
			*p = 0;
			sprintf(p0, "%s:%s", p0, c);
		}
#endif

#if ENABLE_FEATURE_HTTPD_BASIC_AUTH \
 || ENABLE_FEATURE_HTTPD_CONFIG_WITH_MIME_TYPES \
 || ENABLE_FEATURE_HTTPD_CONFIG_WITH_SCRIPT_INTERPR
		/* storing current config line */
		cur = xzalloc(sizeof(Htaccess) + strlen(p0));
		if (cur) {
			cf = strcpy(cur->before_colon, p0);
			c = strchr(cf, ':');
			*c++ = 0;
			cur->after_colon = c;
#if ENABLE_FEATURE_HTTPD_CONFIG_WITH_MIME_TYPES
			if (*cf == '.') {
				/* config .mime line move top for overwrite previous */
				cur->next = mime_a;
				mime_a = cur;
				continue;
			}
#endif
#if ENABLE_FEATURE_HTTPD_CONFIG_WITH_SCRIPT_INTERPR
			if (*cf == '*' && cf[1] == '.') {
				/* config script interpreter line move top for overwrite previous */
				cur->next = script_i;
				script_i = cur;
				continue;
			}
#endif
#if ENABLE_FEATURE_HTTPD_BASIC_AUTH
			free(p0);
			if (prev == NULL) {
				/* first line */
				g_auth = prev = cur;
			} else {
				/* sort path, if current lenght eq or bigger then move up */
				Htaccess *prev_hti = g_auth;
				size_t l = strlen(cf);
				Htaccess *hti;

				for (hti = prev_hti; hti; hti = hti->next) {
					if (l >= strlen(hti->before_colon)) {
						/* insert before hti */
						cur->next = hti;
						if (prev_hti != hti) {
							prev_hti->next = cur;
						} else {
							/* insert as top */
							g_auth = cur;
						}
						break;
					}
					if (prev_hti != hti)
						prev_hti = prev_hti->next;
				}
				if (!hti) {       /* not inserted, add to bottom */
					prev->next = cur;
					prev = cur;
				}
			}
#endif
		}
#endif
	 }
	 fclose(f);
}

#if ENABLE_FEATURE_HTTPD_ENCODE_URL_STR
/****************************************************************************
 *
 > $Function: encodeString()
 *
 * $Description: Given a string, html encode special characters.
 *   This is used for the -e command line option to provide an easy way
 *   for scripts to encode result data without confusing browsers.  The
 *   returned string pointer is memory allocated by malloc().
 *
 * $Parameters:
 *      (const char *) string . . The first string to encode.
 *
 * $Return: (char *) . . . .. . . A pointer to the encoded string.
 *
 * $Errors: Returns a null string ("") if memory is not available.
 *
 ****************************************************************************/
static char *encodeString(const char *string)
{
	/* take the simple route and encode everything */
	/* could possibly scan once to get length.     */
	int len = strlen(string);
	char *out = xmalloc(len * 6 + 1);
	char *p = out;
	char ch;

	while ((ch = *string++)) {
		// very simple check for what to encode
		if (isalnum(ch)) *p++ = ch;
		else p += sprintf(p, "&#%d;", (unsigned char) ch);
	}
	*p = '\0';
	return out;
}
#endif          /* FEATURE_HTTPD_ENCODE_URL_STR */

/****************************************************************************
 *
 > $Function: decodeString()
 *
 * $Description: Given a URL encoded string, convert it to plain ascii.
 *   Since decoding always makes strings smaller, the decode is done in-place.
 *   Thus, callers should strdup() the argument if they do not want the
 *   argument modified.  The return is the original pointer, allowing this
 *   function to be easily used as arguments to other functions.
 *
 * $Parameters:
 *      (char *) string . . . The first string to decode.
 *      (int)    option_d . . 1 if called for httpd -d
 *
 * $Return: (char *)  . . . . A pointer to the decoded string (same as input).
 *
 * $Errors: None
 *
 ****************************************************************************/
static char *decodeString(char *orig, int option_d)
{
	/* note that decoded string is always shorter than original */
	char *string = orig;
	char *ptr = string;
	char c;

	while ((c = *ptr++) != '\0') {
		unsigned value1, value2;

		if (option_d && c == '+') {
			*string++ = ' ';
			continue;
		}
		if (c != '%') {
			*string++ = c;
			continue;
		}
		if (sscanf(ptr, "%1X", &value1) != 1
		 || sscanf(ptr+1, "%1X", &value2) != 1
		) {
			if (!option_d)
				return NULL;
			*string++ = '%';
			continue;
		}
		value1 = value1 * 16 + value2;
		if (!option_d && (value1 == '/' || value1 == '\0')) {
			/* caller takes it as indication of invalid
			 * (dangerous wrt exploits) chars */
			return orig + 1;
		}
		*string++ = value1;
		ptr += 2;
	}
	*string = '\0';
	return orig;
}


#if ENABLE_FEATURE_HTTPD_CGI
/****************************************************************************
 * setenv helpers
 ****************************************************************************/
static void setenv1(const char *name, const char *value)
{
	if (!value)
		value = "";
	setenv(name, value, 1);
}
static void setenv_long(const char *name, long value)
{
	char buf[sizeof(value)*3 + 2];
	sprintf(buf, "%ld", value);
	setenv(name, buf, 1);
}
#endif

#if ENABLE_FEATURE_HTTPD_BASIC_AUTH
/*
 * Decode a base 64 data stream as per rfc1521.
 * Note that the rfc states that non base64 chars are to be ignored.
 * Since the decode always results in a shorter size than the input, it is
 * OK to pass the input arg as an output arg.
 * Parameter: a pointer to a base64 encoded string.
 * Decoded data is stored in-place.
 */
static void decodeBase64(char *Data)
{
	const unsigned char *in = (const unsigned char *)Data;
	// The decoded size will be at most 3/4 the size of the encoded
	unsigned ch = 0;
	int i = 0;

	while (*in) {
		int t = *in++;

		if (t >= '0' && t <= '9')
			t = t - '0' + 52;
		else if (t >= 'A' && t <= 'Z')
			t = t - 'A';
		else if (t >= 'a' && t <= 'z')
			t = t - 'a' + 26;
		else if (t == '+')
			t = 62;
		else if (t == '/')
			t = 63;
		else if (t == '=')
			t = 0;
		else
			continue;

		ch = (ch << 6) | t;
		i++;
		if (i == 4) {
			*Data++ = (char) (ch >> 16);
			*Data++ = (char) (ch >> 8);
			*Data++ = (char) ch;
			i = 0;
		}
	}
	*Data = '\0';
}
#endif


/****************************************************************************
 *
 > $Function: openServer()
 *
 * $Description: create a listen server socket on the designated port.
 *
 * $Return: (int)  . . . A connection socket. -1 for errors.
 *
 * $Errors: None
 *
 ****************************************************************************/
#if BB_MMU
static int openServer(void)
{
	int n = bb_strtou(bind_addr_or_port, NULL, 10);
	if (!errno && n && n <= 0xffff)
		n = create_and_bind_stream_or_die(NULL, n);
	else
		n = create_and_bind_stream_or_die(bind_addr_or_port, 80);
	xlisten(n, 9);
	return n;
}
#endif

/*
 * Create and send HTTP response headers.
 * The arguments are combined and sent as one write operation.  Note that
 * IE will puke big-time if the headers are not sent in one packet and the
 * second packet is delayed for any reason.
 * responseNum - the result code to send.
 * Return result of write().
 */
static int sendHeaders(HttpResponseNum responseNum)
{
	const char *responseString = "";
	const char *infoString = NULL;
	const char *mime_type;
	unsigned i;
	time_t timer = time(0);
	char tmp_str[80];
	int len;

	for (i = 0; i < ARRAY_SIZE(httpResponseNames); i++) {
		if (httpResponseNames[i].type == responseNum) {
			responseString = httpResponseNames[i].name;
			infoString = httpResponseNames[i].info;
			break;
		}
	}
	/* error message is HTML */
	mime_type = responseNum == HTTP_OK ?
				found_mime_type : "text/html";

	if (verbose)
		write(2, iobuf, sprintf(iobuf, "%s response:%u\n", rmt_ip_str, responseNum));

	/* emit the current date */
	strftime(tmp_str, sizeof(tmp_str), RFC1123FMT, gmtime(&timer));
	len = sprintf(iobuf,
			"HTTP/1.0 %d %s\r\nContent-type: %s\r\n"
			"Date: %s\r\nConnection: close\r\n",
			responseNum, responseString, mime_type, tmp_str);

#if ENABLE_FEATURE_HTTPD_BASIC_AUTH
	if (responseNum == HTTP_UNAUTHORIZED) {
		len += sprintf(iobuf + len,
				"WWW-Authenticate: Basic realm=\"%s\"\r\n",
				g_realm);
	}
#endif
	if (responseNum == HTTP_MOVED_TEMPORARILY) {
		len += sprintf(iobuf + len, "Location: %s/%s%s\r\n",
				found_moved_temporarily,
				(g_query ? "?" : ""),
				(g_query ? g_query : ""));
	}

	if (ContentLength != -1) {    /* file */
		strftime(tmp_str, sizeof(tmp_str), RFC1123FMT, gmtime(&last_mod));
		len += sprintf(iobuf + len, "Last-Modified: %s\r\n%s %"OFF_FMT"d\r\n",
			tmp_str, "Content-length:", ContentLength);
	}
	strcat(iobuf, "\r\n");
	len += 2;
	if (infoString) {
		len += sprintf(iobuf + len,
				"<HTML><HEAD><TITLE>%d %s</TITLE></HEAD>\n"
				"<BODY><H1>%d %s</H1>\n%s\n</BODY></HTML>\n",
				responseNum, responseString,
				responseNum, responseString, infoString);
	}
	if (DEBUG)
		fprintf(stderr, "headers: '%s'\n", iobuf);
	i = accepted_socket;
	if (i == 0)
		i++; /* write to fd #1 in inetd mode */
	return full_write(i, iobuf, len);
}

/*
 * Read from the socket until '\n' or EOF. '\r' chars are removed.
 * '\n' is replaced with NUL.
 * Return number of characters read or 0 if nothing is read
 * ('\r' and '\n' are not counted).
 * Data is returned in iobuf.
 */
static int get_line(void)
{
	int count = 0;

	/* We must not read extra chars. Reading byte-by-byte... */
	while (read(accepted_socket, iobuf + count, 1) == 1) {
		if (iobuf[count] == '\r')
			continue;
		if (iobuf[count] == '\n') {
			iobuf[count] = '\0';
			return count;
		}
		if (count < (MAX_MEMORY_BUF - 1))      /* check overflow */
			count++;
	}
	return count;
}

#if ENABLE_FEATURE_HTTPD_CGI
/****************************************************************************
 *
 > $Function: sendCgi()
 *
 * $Description: Execute a CGI script and send it's stdout back
 *
 *   Environment variables are set up and the script is invoked with pipes
 *   for stdin/stdout.  If a post is being done the script is fed the POST
 *   data in addition to setting the QUERY_STRING variable (for GETs or POSTs).
 *
 * $Parameters:
 *      (const char *) url . . . . . . The requested URL (with leading /).
 *      (int bodyLen)  . . . . . . . . Length of the post body.
 *      (const char *cookie) . . . . . For set HTTP_COOKIE.
 *      (const char *content_type) . . For set CONTENT_TYPE.
 *
 * $Return: (char *)  . . . . A pointer to the decoded string (same as input).
 *
 * $Errors: None
 *
 ****************************************************************************/
static int sendCgi(const char *url,
		const char *request, int bodyLen, const char *cookie,
		const char *content_type)
{
	struct { int rd; int wr; } fromCgi;  /* CGI -> httpd pipe */
	struct { int rd; int wr; } toCgi;    /* httpd -> CGI pipe */
	char *fullpath;
	char *argp[] = { NULL, NULL };
	int pid = 0;
	int inFd;
	int outFd;
	int buf_count;
	int status;
	size_t post_read_size, post_read_idx;

	if (pipe(&fromCgi.rd) != 0)
		return 0;
	if (pipe(&toCgi.rd) != 0)
		return 0;

/*
 * Note: We can use vfork() here in the no-mmu case, although
 * the child modifies the parent's variables, due to:
 * 1) The parent does not use the child-modified variables.
 * 2) The allocated memory (in the child) is freed when the process
 *    exits. This happens instantly after the child finishes,
 *    since httpd is run from inetd (and it can't run standalone
 *    in uClinux).
 */

// FIXME: setenv leaks memory! (old values of env vars are leaked)
// Thus we have a bug on NOMMU.
// Need to use this instead:
// [malloc +] putenv + (in child: exec) + (in parent: unsetenv [+ free])

#if !BB_MMU
	fullpath = NULL;
	pid = vfork();
#else
	pid = fork();
#endif
	if (pid < 0)
		return 0;

	if (!pid) {
		/* child process */
		char *script;
		char *purl;

		if (accepted_socket > 1)
			close(accepted_socket);
		if (server_socket > 1)
			close(server_socket);

		xmove_fd(toCgi.rd, 0);  /* replace stdin with the pipe */
		xmove_fd(fromCgi.wr, 1);  /* replace stdout with the pipe */
		close(fromCgi.rd);
		close(toCgi.wr);
		/* Huh? User seeing stderr can be a security problem.
		 * If CGI really wants that, it can always dup2(1,2). */
		/* dup2(1, 2); */

		/*
		 * Find PATH_INFO.
		 */
		xfunc_error_retval = 242;
		purl = xstrdup(url);
		script = purl;
		while ((script = strchr(script + 1, '/')) != NULL) {
			/* have script.cgi/PATH_INFO or dirs/script.cgi[/PATH_INFO] */
			struct stat sb;

			*script = '\0';
			if (is_directory(purl + 1, 1, &sb) == 0) {
				/* not directory, found script.cgi/PATH_INFO */
				*script = '/';
				break;
			}
			*script = '/';          /* is directory, find next '/' */
		}
		setenv1("PATH_INFO", script);   /* set /PATH_INFO or "" */
		setenv1("REQUEST_METHOD", request);
		if (g_query) {
			char *uri = alloca(strlen(purl) + 2 + strlen(g_query));
			if (uri)
				sprintf(uri, "%s?%s", purl, g_query);
			setenv1("REQUEST_URI", uri);
		} else {
			setenv1("REQUEST_URI", purl);
		}
		if (script != NULL)
			*script = '\0';         /* cut off /PATH_INFO */

		/* SCRIPT_FILENAME required by PHP in CGI mode */
		fullpath = concat_path_file(home_httpd, purl);
		setenv1("SCRIPT_FILENAME", fullpath);
		/* set SCRIPT_NAME as full path: /cgi-bin/dirs/script.cgi */
		setenv1("SCRIPT_NAME", purl);
		/* http://hoohoo.ncsa.uiuc.edu/cgi/env.html:
		 * QUERY_STRING: The information which follows the ? in the URL
		 * which referenced this script. This is the query information.
		 * It should not be decoded in any fashion. This variable
		 * should always be set when there is query information,
		 * regardless of command line decoding. */
		/* (Older versions of bbox seem to do some decoding) */
		setenv1("QUERY_STRING", g_query);
		putenv((char*)"SERVER_SOFTWARE=busybox httpd/"BB_VER);
		putenv((char*)"SERVER_PROTOCOL=HTTP/1.0");
		putenv((char*)"GATEWAY_INTERFACE=CGI/1.1");
		/* Having _separate_ variables for IP and port defeats
		 * the purpose of having socket abstraction. Which "port"
		 * are you using on Unix domain socket?
		 * IOW - REMOTE_PEER="1.2.3.4:56" makes much more sense.
		 * Oh well... */
		{
			char *p = rmt_ip_str ? : (char*)"";
			char *cp = strrchr(p, ':');
			if (ENABLE_FEATURE_IPV6 && cp && strchr(cp, ']'))
				cp = NULL;
			if (cp) *cp = '\0'; /* delete :PORT */
			setenv1("REMOTE_ADDR", p);
			if (cp) *cp = ':';
		}
		setenv1("HTTP_USER_AGENT", user_agent);
#if ENABLE_FEATURE_HTTPD_SET_REMOTE_PORT_TO_ENV
		setenv_long("REMOTE_PORT", tcp_port);
#endif
		if (bodyLen)
			setenv_long("CONTENT_LENGTH", bodyLen);
		if (cookie)
			setenv1("HTTP_COOKIE", cookie);
		if (content_type)
			setenv1("CONTENT_TYPE", content_type);
#if ENABLE_FEATURE_HTTPD_BASIC_AUTH
		if (remoteuser) {
			setenv1("REMOTE_USER", remoteuser);
			putenv((char*)"AUTH_TYPE=Basic");
		}
#endif
		if (referer)
			setenv1("HTTP_REFERER", referer);

		/* set execve argp[0] without path */
		argp[0] = (char*)bb_basename(purl);
		/* but script argp[0] must have absolute path and chdiring to this */
		script = strrchr(fullpath, '/');
		if (!script)
			goto error_execing_cgi;
		*script = '\0';
		if (chdir(fullpath) == 0) {
			// Now run the program.  If it fails,
			// use _exit() so no destructors
			// get called and make a mess.
#if ENABLE_FEATURE_HTTPD_CONFIG_WITH_SCRIPT_INTERPR
			char *interpr = NULL;
			char *suffix = strrchr(purl, '.');

			if (suffix) {
				Htaccess *cur;
				for (cur = script_i; cur; cur = cur->next) {
					if (strcmp(cur->before_colon + 1, suffix) == 0) {
						interpr = cur->after_colon;
						break;
					}
				}
			}
#endif
			*script = '/';
#if ENABLE_FEATURE_HTTPD_CONFIG_WITH_SCRIPT_INTERPR
			if (interpr)
				execv(interpr, argp);
			else
#endif
				execv(fullpath, argp);
		}
 error_execing_cgi:
		/* send to stdout (even if we are not from inetd) */
		accepted_socket = 1;
		sendHeaders(HTTP_NOT_FOUND);
		_exit(242);
	} /* end child */

	/* parent process */
#if !BB_MMU
	free(fullpath);
#endif

	buf_count = 0;
	post_read_size = 0;
	post_read_idx = 0; /* for gcc */
	inFd = fromCgi.rd;
	outFd = toCgi.wr;
	close(fromCgi.wr);
	close(toCgi.rd);
	signal(SIGPIPE, SIG_IGN);

	while (1) {
		fd_set readSet;
		fd_set writeSet;
		char wbuf[128];
		int nfound;
		int count;

		FD_ZERO(&readSet);
		FD_ZERO(&writeSet);
		FD_SET(inFd, &readSet);
		if (bodyLen > 0 || post_read_size > 0) {
			FD_SET(outFd, &writeSet);
			nfound = outFd > inFd ? outFd : inFd;
			if (post_read_size == 0) {
				FD_SET(accepted_socket, &readSet);
				if (nfound < accepted_socket)
					nfound = accepted_socket;
			}
			/* Now wait on the set of sockets! */
			nfound = select(nfound + 1, &readSet, &writeSet, NULL, NULL);
		} else {
			if (!bodyLen) {
				close(outFd); /* no more POST data to CGI */
				bodyLen = -1;
			}
			nfound = select(inFd + 1, &readSet, NULL, NULL, NULL);
		}

		if (nfound <= 0) {
			if (waitpid(pid, &status, WNOHANG) <= 0) {
				/* Weird. CGI didn't exit and no fd's
				 * are ready, yet select returned?! */
				continue;
			}
			close(inFd);
			if (DEBUG && WIFEXITED(status))
				bb_error_msg("piped has exited with status=%d", WEXITSTATUS(status));
			if (DEBUG && WIFSIGNALED(status))
				bb_error_msg("piped has exited with signal=%d", WTERMSIG(status));
			break;
		}

		if (post_read_size > 0 && FD_ISSET(outFd, &writeSet)) {
			/* Have data from peer and can write to CGI */
		// huh? why full_write? what if we will block?
		// (imagine that CGI does not read its stdin...)
			count = full_write(outFd, wbuf + post_read_idx, post_read_size);
			if (count > 0) {
				post_read_idx += count;
				post_read_size -= count;
			} else {
				post_read_size = bodyLen = 0; /* broken pipe to CGI */
			}
		} else if (bodyLen > 0 && post_read_size == 0
		 && FD_ISSET(accepted_socket, &readSet)
		) {
			/* We expect data, prev data portion is eaten by CGI
			 * and there *is* data to read from the peer
			 * (POSTDATA?) */
			count = bodyLen > (int)sizeof(wbuf) ? (int)sizeof(wbuf) : bodyLen;
			count = safe_read(accepted_socket, wbuf, count);
			if (count > 0) {
				post_read_size = count;
				post_read_idx = 0;
				bodyLen -= count;
			} else {
				bodyLen = 0; /* closed */
			}
		}

#define PIPESIZE PIPE_BUF
#if PIPESIZE >= MAX_MEMORY_BUF
# error "PIPESIZE >= MAX_MEMORY_BUF"
#endif
		if (FD_ISSET(inFd, &readSet)) {
			/* There is something to read from CGI */
			int s = accepted_socket;
			char *rbuf = iobuf;

			/* Are we still buffering CGI output? */
			if (buf_count >= 0) {
				/* According to http://hoohoo.ncsa.uiuc.edu/cgi/out.html,
				 * CGI scripts MUST send their own header terminated by
				 * empty line, then data. That's why we have only one
				 * <cr><lf> pair here. We will output "200 OK" line
				 * if needed, but CGI still has to provide blank line
				 * between header and body */
				static const char HTTP_200[] ALIGN1 = "HTTP/1.0 200 OK\r\n";

				/* Must use safe_read, not full_read, because
				 * CGI may output a few first bytes and then wait
				 * for POSTDATA without closing stdout.
				 * With full_read we may wait here forever. */
				count = safe_read(inFd, rbuf + buf_count, PIPESIZE - 8);
				if (count <= 0) {
					/* eof (or error) and there was no "HTTP",
					 * so write it, then write received data */
					if (buf_count) {
						full_write(s, HTTP_200, sizeof(HTTP_200)-1);
						full_write(s, rbuf, buf_count);
					}
					break; /* closed */
				}
				buf_count += count;
				count = 0;
				/* "Status" header format is: "Status: 302 Redirected\r\n" */
				if (buf_count >= 8 && memcmp(rbuf, "Status: ", 8) == 0) {
					/* send "HTTP/1.0 " */
					if (full_write(s, HTTP_200, 9) != 9)
						break;
					rbuf += 8; /* skip "Status: " */
					count = buf_count - 8;
					buf_count = -1; /* buffering off */
				} else if (buf_count >= 4) {
					/* Did CGI add "HTTP"? */
					if (memcmp(rbuf, HTTP_200, 4) != 0) {
						/* there is no "HTTP", do it ourself */
						if (full_write(s, HTTP_200, sizeof(HTTP_200)-1) != sizeof(HTTP_200)-1)
							break;
					}
					/* Commented out:
					if (!strstr(rbuf, "ontent-")) {
						full_write(s, "Content-type: text/plain\r\n\r\n", 28);
					}
					 * Counter-example of valid CGI without Content-type:
					 * echo -en "HTTP/1.0 302 Found\r\n"
					 * echo -en "Location: http://www.busybox.net\r\n"
					 * echo -en "\r\n"
					 */
					count = buf_count;
					buf_count = -1; /* buffering off */
				}
			} else {
				count = safe_read(inFd, rbuf, PIPESIZE);
				if (count <= 0)
					break;  /* eof (or error) */
			}
			if (full_write(s, rbuf, count) != count)
				break;
			if (DEBUG)
				fprintf(stderr, "cgi read %d bytes: '%.*s'\n", count, count, rbuf);
		} /* if (FD_ISSET(inFd)) */
	} /* while (1) */
	return 0;
}
#endif          /* FEATURE_HTTPD_CGI */

/****************************************************************************
 *
 > $Function: sendFile()
 *
 * $Description: Send a file response to a HTTP request
 *
 * $Parameter:
 *      (const char *) url . . The URL requested.
 *
 * $Return: (int)  . . . . . . Always 0.
 *
 ****************************************************************************/
static int sendFile(const char *url)
{
	char *suffix;
	int f;
	int fd;
	const char *const *table;
	const char *try_suffix;
	ssize_t count;
#if ENABLE_FEATURE_HTTPD_USE_SENDFILE
	off_t offset = 0;
#endif

	suffix = strrchr(url, '.');

	for (table = suffixTable; *table; table += 2)
		if (suffix != NULL && (try_suffix = strstr(*table, suffix)) != 0) {
			try_suffix += strlen(suffix);
			if (*try_suffix == 0 || *try_suffix == '.')
				break;
		}
	/* also, if not found, set default as "application/octet-stream";  */
	found_mime_type = table[1];
#if ENABLE_FEATURE_HTTPD_CONFIG_WITH_MIME_TYPES
	if (suffix) {
		Htaccess * cur;
		for (cur = mime_a; cur; cur = cur->next) {
			if (strcmp(cur->before_colon, suffix) == 0) {
				found_mime_type = cur->after_colon;
				break;
			}
		}
	}
#endif  /* FEATURE_HTTPD_CONFIG_WITH_MIME_TYPES */

	if (DEBUG)
		fprintf(stderr, "sending file '%s' content-type: %s\n",
			url, found_mime_type);

	f = open(url, O_RDONLY);
	if (f < 0) {
		if (DEBUG)
			bb_perror_msg("cannot open '%s'", url);
		sendHeaders(HTTP_NOT_FOUND);
		return 0;
	}

	sendHeaders(HTTP_OK);
	fd = accepted_socket;
	if (fd == 0)
		fd++; /* write to fd #1 in inetd mode */
#if ENABLE_FEATURE_HTTPD_USE_SENDFILE
	do {
		/* byte count is rounded down to 64k */
		count = sendfile(fd, f, &offset, MAXINT(ssize_t) - 0xffff);
		if (count < 0) {
			if (offset == 0)
				goto fallback;
			bb_perror_msg("sendfile '%s'", url);
		}
	} while (count > 0);
	close(f);
	return 0;

 fallback:
#endif
	while ((count = full_read(f, iobuf, MAX_MEMORY_BUF)) > 0) {
		if (full_write(fd, iobuf, count) != count)
			break;
	}
	close(f);
	return 0;
}

static int checkPermIP(void)
{
	Htaccess_IP * cur;

	/* This could stand some work */
	for (cur = ip_a_d; cur; cur = cur->next) {
#if DEBUG
		fprintf(stderr,
			"checkPermIP: '%s' ? '%u.%u.%u.%u/%u.%u.%u.%u'\n",
			rmt_ip_str,
			(unsigned char)(cur->ip >> 24),
			(unsigned char)(cur->ip >> 16),
			(unsigned char)(cur->ip >> 8),
			(unsigned char)(cur->ip),
			(unsigned char)(cur->mask >> 24),
			(unsigned char)(cur->mask >> 16),
			(unsigned char)(cur->mask >> 8),
			(unsigned char)(cur->mask)
		);
#endif
		if ((rmt_ip & cur->mask) == cur->ip)
			return cur->allow_deny == 'A';   /* Allow/Deny */
	}

	/* if unconfigured, return 1 - access from all */
	return !flg_deny_all;
}

/****************************************************************************
 *
 > $Function: checkPerm()
 *
 * $Description: Check the permission file for access password protected.
 *
 *   If config file isn't present, everything is allowed.
 *   Entries are of the form you can see example from header source
 *
 * $Parameters:
 *      (const char *) path  . . . . The file path.
 *      (const char *) request . . . User information to validate.
 *
 * $Return: (int)  . . . . . . . . . 1 if request OK, 0 otherwise.
 *
 ****************************************************************************/

#if ENABLE_FEATURE_HTTPD_BASIC_AUTH
static int checkPerm(const char *path, const char *request)
{
	Htaccess * cur;
	const char *p;
	const char *p0;

	const char *prev = NULL;

	/* This could stand some work */
	for (cur = g_auth; cur; cur = cur->next) {
		size_t l;

		p0 = cur->before_colon;
		if (prev != NULL && strcmp(prev, p0) != 0)
			continue;       /* find next identical */
		p = cur->after_colon;
		if (DEBUG)
			fprintf(stderr, "checkPerm: '%s' ? '%s'\n", p0, request);

		l = strlen(p0);
		if (strncmp(p0, path, l) == 0
		 && (l == 1 || path[l] == '/' || path[l] == '\0')
		) {
			char *u;
			/* path match found.  Check request */
			/* for check next /path:user:password */
			prev = p0;
			u = strchr(request, ':');
			if (u == NULL) {
				/* bad request, ':' required */
				break;
			}

			if (ENABLE_FEATURE_HTTPD_AUTH_MD5) {
				char *cipher;
				char *pp;

				if (strncmp(p, request, u-request) != 0) {
					/* user uncompared */
					continue;
				}
				pp = strchr(p, ':');
				if (pp && pp[1] == '$' && pp[2] == '1' &&
						pp[3] == '$' && pp[4]) {
					pp++;
					cipher = pw_encrypt(u+1, pp);
					if (strcmp(cipher, pp) == 0)
						goto set_remoteuser_var;   /* Ok */
					/* unauthorized */
					continue;
				}
			}

			if (strcmp(p, request) == 0) {
set_remoteuser_var:
				remoteuser = strdup(request);
				if (remoteuser)
					remoteuser[(u - request)] = 0;
				return 1;   /* Ok */
			}
			/* unauthorized */
		}
	}   /* for */

	return prev == NULL;
}

#endif  /* FEATURE_HTTPD_BASIC_AUTH */

/*
 * Handle timeouts
 */
static void handle_sigalrm(int sig)
{
	sendHeaders(HTTP_REQUEST_TIMEOUT);
	alarm_signaled = 1;
}

/*
 * Handle an incoming http request and exit.
 */
static void handle_incoming_and_exit(void) ATTRIBUTE_NORETURN;
static void handle_incoming_and_exit(void)
{
	char *url;
	char *purl;
	int blank = -1;
	char *test;
	struct stat sb;
	int ip_allowed;
#if ENABLE_FEATURE_HTTPD_CGI
	const char *prequest = request_GET;
	unsigned long length = 0;
	char *cookie = 0;
	char *content_type = 0;
#endif
	struct sigaction sa;

#if ENABLE_FEATURE_HTTPD_BASIC_AUTH
	int credentials = -1;  /* if not required this is Ok */
#endif

	sa.sa_handler = handle_sigalrm;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0; /* no SA_RESTART */
	sigaction(SIGALRM, &sa, NULL);

	/* It's not a real loop (it ends with while(0)).
	 * Break from this "loop" jumps to exit(0) */
	do {
		int count;

		alarm(TIMEOUT);
		if (!get_line())
			break;  /* EOF or error or empty line */

		purl = strpbrk(iobuf, " \t");
		if (purl == NULL) {
 BAD_REQUEST:
			sendHeaders(HTTP_BAD_REQUEST);
			break;
		}
		*purl = '\0';
#if ENABLE_FEATURE_HTTPD_CGI
		if (strcasecmp(iobuf, prequest) != 0) {
			prequest = "POST";
			if (strcasecmp(iobuf, prequest) != 0) {
				sendHeaders(HTTP_NOT_IMPLEMENTED);
				break;
			}
		}
#else
		if (strcasecmp(iobuf, request_GET) != 0) {
			sendHeaders(HTTP_NOT_IMPLEMENTED);
			break;
		}
#endif
		*purl = ' ';
		count = sscanf(purl, " %[^ ] HTTP/%d.%*d", iobuf, &blank);

		if (count < 1 || iobuf[0] != '/') {
			/* Garbled request/URL */
			goto BAD_REQUEST;
		}
		url = alloca(strlen(iobuf) + sizeof("/index.html"));
		if (url == NULL) {
			sendHeaders(HTTP_INTERNAL_SERVER_ERROR);
			break;
		}
		strcpy(url, iobuf);
		/* extract url args if present */
		test = strchr(url, '?');
		g_query = NULL;
		if (test) {
			*test++ = '\0';
			g_query = test;
		}

		test = decodeString(url, 0);
		if (test == NULL)
			goto BAD_REQUEST;
		if (test == url+1) {
			/* '/' or NUL is encoded */
			sendHeaders(HTTP_NOT_FOUND);
			break;
		}

		/* algorithm stolen from libbb bb_simplify_path(),
		 * but don't strdup and reducing trailing slash and protect out root */
		purl = test = url;
		do {
			if (*purl == '/') {
				/* skip duplicate (or initial) slash */
				if (*test == '/') {
					continue;
				}
				if (*test == '.') {
					/* skip extra '.' */
					if (test[1] == '/' || !test[1]) {
						continue;
					}
					/* '..': be careful */
					if (test[1] == '.' && (test[2] == '/' || !test[2])) {
						++test;
						if (purl == url) {
							/* protect root */
							goto BAD_REQUEST;
						}
						while (*--purl != '/') /* omit previous dir */;
							continue;
					}
				}
			}
			*++purl = *test;
		} while (*++test);
		*++purl = '\0';       /* so keep last character */
		test = purl;          /* end ptr */

		/* If URL is directory, adding '/' */
		if (test[-1] != '/') {
			if (is_directory(url + 1, 1, &sb)) {
				found_moved_temporarily = url;
			}
		}
		if (verbose > 1) {
			/* Hopefully does it with single write[v] */
			/* (uclibc does, glibc: ?) */
			fdprintf(2, "%s url:%s\n", rmt_ip_str, url);
		}

		test = url;
		ip_allowed = checkPermIP();
		while (ip_allowed && (test = strchr(test + 1, '/')) != NULL) {
			/* have path1/path2 */
			*test = '\0';
			if (is_directory(url + 1, 1, &sb)) {
				/* may be having subdir config */
				parse_conf(url + 1, SUBDIR_PARSE);
				ip_allowed = checkPermIP();
			}
			*test = '/';
		}
		if (blank >= 0) {
			/* read until blank line for HTTP version specified, else parse immediate */
			while (1) {
				alarm(TIMEOUT);
				if (!get_line())
					break; /* EOF or error or empty line */

				if (DEBUG)
					fprintf(stderr, "header: '%s'\n", iobuf);

#if ENABLE_FEATURE_HTTPD_CGI
				/* try and do our best to parse more lines */
				if ((STRNCASECMP(iobuf, "Content-length:") == 0)) {
					/* extra read only for POST */
					if (prequest != request_GET) {
						test = iobuf + sizeof("Content-length:") - 1;
						if (!test[0])
							goto bail_out;
						errno = 0;
						/* not using strtoul: it ignores leading minus! */
						length = strtol(test, &test, 10);
						/* length is "ulong", but we need to pass it to int later */
						/* so we check for negative or too large values in one go: */
						/* (long -> ulong conv caused negatives to be seen as > INT_MAX) */
						if (test[0] || errno || length > INT_MAX)
							goto bail_out;
					}
				} else if (STRNCASECMP(iobuf, "Cookie:") == 0) {
					cookie = strdup(skip_whitespace(iobuf + sizeof("Cookie:")-1));
				} else if (STRNCASECMP(iobuf, "Content-Type:") == 0) {
					content_type = strdup(skip_whitespace(iobuf + sizeof("Content-Type:")-1));
				} else if (STRNCASECMP(iobuf, "Referer:") == 0) {
					referer = strdup(skip_whitespace(iobuf + sizeof("Referer:")-1));
				} else if (STRNCASECMP(iobuf, "User-Agent:") == 0) {
					user_agent = strdup(skip_whitespace(iobuf + sizeof("User-Agent:")-1));
				}
#endif

#if ENABLE_FEATURE_HTTPD_BASIC_AUTH
				if (STRNCASECMP(iobuf, "Authorization:") == 0) {
					/* We only allow Basic credentials.
					 * It shows up as "Authorization: Basic <userid:password>" where
					 * the userid:password is base64 encoded.
					 */
					test = skip_whitespace(iobuf + sizeof("Authorization:")-1);
					if (STRNCASECMP(test, "Basic") != 0)
						continue;
					test += sizeof("Basic")-1;
					/* decodeBase64() skips whitespace itself */
					decodeBase64(test);
					credentials = checkPerm(url, test);
				}
#endif          /* FEATURE_HTTPD_BASIC_AUTH */

			} /* while extra header reading */
		}
		alarm(0);
		if (alarm_signaled)
			break;

		if (strcmp(bb_basename(url), httpd_conf) == 0 || ip_allowed == 0) {
			/* protect listing [/path]/httpd_conf or IP deny */
#if ENABLE_FEATURE_HTTPD_CGI
 FORBIDDEN:		/* protect listing /cgi-bin */
#endif
			sendHeaders(HTTP_FORBIDDEN);
			break;
		}

#if ENABLE_FEATURE_HTTPD_BASIC_AUTH
		if (credentials <= 0 && checkPerm(url, ":") == 0) {
			sendHeaders(HTTP_UNAUTHORIZED);
			break;
		}
#endif

		if (found_moved_temporarily) {
			sendHeaders(HTTP_MOVED_TEMPORARILY);
			/* clear unforked memory flag */
			found_moved_temporarily = NULL;
			break;
		}

		test = url + 1;      /* skip first '/' */

#if ENABLE_FEATURE_HTTPD_CGI
		if (strncmp(test, "cgi-bin", 7) == 0) {
			if (test[7] == '/' && test[8] == '\0')
				goto FORBIDDEN;     /* protect listing cgi-bin/ */
			sendCgi(url, prequest, length, cookie, content_type);
			break;
		}
#if ENABLE_FEATURE_HTTPD_CONFIG_WITH_SCRIPT_INTERPR
		{
			char *suffix = strrchr(test, '.');
			if (suffix) {
				Htaccess *cur;
				for (cur = script_i; cur; cur = cur->next) {
					if (strcmp(cur->before_colon + 1, suffix) == 0) {
						sendCgi(url, prequest, length, cookie, content_type);
						goto bail_out;
					}
				}
			}
		}
#endif
		if (prequest != request_GET) {
			sendHeaders(HTTP_NOT_IMPLEMENTED);
			break;
		}
#endif  /* FEATURE_HTTPD_CGI */
		if (purl[-1] == '/')
			strcpy(purl, "index.html");
		if (stat(test, &sb) == 0) {
			/* It's a dir URL and there is index.html */
			ContentLength = sb.st_size;
			last_mod = sb.st_mtime;
		}
#if ENABLE_FEATURE_HTTPD_CGI
		else if (purl[-1] == '/') {
			/* It's a dir URL and there is no index.html
			 * Try cgi-bin/index.cgi */
			if (access("/cgi-bin/index.cgi"+1, X_OK) == 0) {
				purl[0] = '\0';
				g_query = url;
				sendCgi("/cgi-bin/index.cgi", prequest, length, cookie, content_type);
				break;
			}
		}
#endif  /* FEATURE_HTTPD_CGI */
		sendFile(test);
		ContentLength = -1;
	} while (0);

#if ENABLE_FEATURE_HTTPD_CGI
 bail_out:
#endif

	exit(0);

#if 0 /* Is this needed? Why? */
	if (DEBUG)
		fprintf(stderr, "closing socket\n");
#if ENABLE_FEATURE_HTTPD_CGI
	free(cookie);
	free(content_type);
	free(referer);
	referer = NULL;
#if ENABLE_FEATURE_HTTPD_BASIC_AUTH
	free(remoteuser);
	remoteuser = NULL;
#endif
#endif
	/* Properly wait for remote to closed */
	int retval;
	shutdown(accepted_socket, SHUT_WR);
	do {
		fd_set s_fd;
		struct timeval tv;
		FD_ZERO(&s_fd);
		FD_SET(accepted_socket, &s_fd);
		tv.tv_sec = 2;
		tv.tv_usec = 0;
		retval = select(accepted_socket + 1, &s_fd, NULL, NULL, &tv);
	} while (retval > 0 && read(accepted_socket, iobuf, sizeof(iobuf) > 0));
	shutdown(accepted_socket, SHUT_RD);
	close(accepted_socket);
	exit(0);
#endif
}

#if BB_MMU
/*
 * The main http server function.
 * Given an open socket, listen for new connections and farm out
 * the processing as a forked process.
 * Never returns.
 */
static void mini_httpd(int server) ATTRIBUTE_NORETURN;
static void mini_httpd(int server)
{
	fd_set readfd, portfd;

	FD_ZERO(&portfd);
	FD_SET(server, &portfd);

	/* copy the ports we are watching to the readfd set */
	while (1) {
		int s;
		union {
			struct sockaddr sa;
			struct sockaddr_in sin;
			USE_FEATURE_IPV6(struct sockaddr_in6 sin6;)
		} fromAddr;
		socklen_t fromAddrLen = sizeof(fromAddr);

		/* Now wait INDEFINITELY on the set of sockets! */
		readfd = portfd;
		if (select(server + 1, &readfd, 0, 0, 0) <= 0)
			continue;
		if (!FD_ISSET(server, &readfd))
			continue;
		s = accept(server, &fromAddr.sa, &fromAddrLen);
		if (s < 0)
			continue;
		accepted_socket = s;
		rmt_ip = 0;
		tcp_port = 0;
		if (verbose || ENABLE_FEATURE_HTTPD_CGI || DEBUG) {
			free(rmt_ip_str);
			rmt_ip_str = xmalloc_sockaddr2dotted(&fromAddr.sa, fromAddrLen);
			if (DEBUG)
				bb_error_msg("connection from '%s'", rmt_ip_str);
		}
		if (fromAddr.sa.sa_family == AF_INET) {
			rmt_ip = ntohl(fromAddr.sin.sin_addr.s_addr);
			tcp_port = ntohs(fromAddr.sin.sin_port);
		}
#if ENABLE_FEATURE_IPV6
		if (fromAddr.sa.sa_family == AF_INET6) {
			//rmt_ip = ntohl(fromAddr.sin.sin_addr.s_addr);
			tcp_port = ntohs(fromAddr.sin6.sin6_port);
		}
#endif

		/* set the KEEPALIVE option to cull dead connections */
		setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &const_int_1, sizeof(const_int_1));

		if (fork() == 0) {
			/* child */
#if ENABLE_FEATURE_HTTPD_RELOAD_CONFIG_SIGHUP
			/* Do not reload config on HUP */
			signal(SIGHUP, SIG_IGN);
#endif
			handle_incoming_and_exit();
		}
		close(s);
	} /* while (1) */
	/* return 0; - never reached */
}
#endif

/* from inetd */
static void mini_httpd_inetd(void) ATTRIBUTE_NORETURN;
static void mini_httpd_inetd(void)
{
	union {
		struct sockaddr sa;
		struct sockaddr_in sin;
		USE_FEATURE_IPV6(struct sockaddr_in6 sin6;)
	} fromAddr;
	socklen_t fromAddrLen = sizeof(fromAddr);

	getpeername(0, &fromAddr.sa, &fromAddrLen);
	rmt_ip = 0;
	tcp_port = 0;
	if (verbose || ENABLE_FEATURE_HTTPD_CGI || DEBUG) {
		free(rmt_ip_str);
		rmt_ip_str = xmalloc_sockaddr2dotted(&fromAddr.sa, fromAddrLen);
	}
	if (fromAddr.sa.sa_family == AF_INET) {
		rmt_ip = ntohl(fromAddr.sin.sin_addr.s_addr);
		tcp_port = ntohs(fromAddr.sin.sin_port);
	}
#if ENABLE_FEATURE_IPV6
	if (fromAddr.sa.sa_family == AF_INET6) {
		//rmt_ip = ntohl(fromAddr.sin.sin_addr.s_addr);
		tcp_port = ntohs(fromAddr.sin6.sin6_port);
	}
#endif
	handle_incoming_and_exit();
}

#if ENABLE_FEATURE_HTTPD_RELOAD_CONFIG_SIGHUP
static void sighup_handler(int sig)
{
	/* set and reset */
	struct sigaction sa;

	parse_conf(default_path_httpd_conf, sig == SIGHUP ? SIGNALED_PARSE : FIRST_PARSE);
	sa.sa_handler = sighup_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGHUP, &sa, NULL);
}
#endif

enum {
	c_opt_config_file = 0,
	d_opt_decode_url,
	h_opt_home_httpd,
	USE_FEATURE_HTTPD_ENCODE_URL_STR(e_opt_encode_url,)
	USE_FEATURE_HTTPD_BASIC_AUTH(    r_opt_realm     ,)
	USE_FEATURE_HTTPD_AUTH_MD5(      m_opt_md5       ,)
	USE_FEATURE_HTTPD_SETUID(        u_opt_setuid    ,)
	p_opt_port      ,
	p_opt_inetd     ,
	p_opt_foreground,
	p_opt_verbose   ,
	OPT_CONFIG_FILE = 1 << c_opt_config_file,
	OPT_DECODE_URL  = 1 << d_opt_decode_url,
	OPT_HOME_HTTPD  = 1 << h_opt_home_httpd,
	OPT_ENCODE_URL  = USE_FEATURE_HTTPD_ENCODE_URL_STR((1 << e_opt_encode_url)) + 0,
	OPT_REALM       = USE_FEATURE_HTTPD_BASIC_AUTH(    (1 << r_opt_realm     )) + 0,
	OPT_MD5         = USE_FEATURE_HTTPD_AUTH_MD5(      (1 << m_opt_md5       )) + 0,
	OPT_SETUID      = USE_FEATURE_HTTPD_SETUID(        (1 << u_opt_setuid    )) + 0,
	OPT_PORT        = 1 << p_opt_port,
	OPT_INETD       = 1 << p_opt_inetd,
	OPT_FOREGROUND  = 1 << p_opt_foreground,
	OPT_VERBOSE     = 1 << p_opt_verbose,
};


int httpd_main(int argc, char **argv);
int httpd_main(int argc, char **argv)
{
	unsigned opt;
	char *url_for_decode;
	USE_FEATURE_HTTPD_ENCODE_URL_STR(const char *url_for_encode;)
	USE_FEATURE_HTTPD_SETUID(const char *s_ugid = NULL;)
	USE_FEATURE_HTTPD_SETUID(struct bb_uidgid_t ugid;)
	USE_FEATURE_HTTPD_AUTH_MD5(const char *pass;)

#if ENABLE_LOCALE_SUPPORT
	/* Undo busybox.c: we want to speak English in http (dates etc) */
	setlocale(LC_TIME, "C");
#endif

	INIT_G();
	home_httpd = xrealloc_getcwd_or_warn(NULL);
	opt_complementary = "vv"; /* counter */
	/* We do not "absolutize" path given by -h (home) opt.
	 * If user gives relative path in -h, $SCRIPT_FILENAME can end up
	 * relative too. */
	opt = getopt32(argc, argv, "c:d:h:"
			USE_FEATURE_HTTPD_ENCODE_URL_STR("e:")
			USE_FEATURE_HTTPD_BASIC_AUTH("r:")
			USE_FEATURE_HTTPD_AUTH_MD5("m:")
			USE_FEATURE_HTTPD_SETUID("u:")
			"p:ifv",
			&configFile, &url_for_decode, &home_httpd
			USE_FEATURE_HTTPD_ENCODE_URL_STR(, &url_for_encode)
			USE_FEATURE_HTTPD_BASIC_AUTH(, &g_realm)
			USE_FEATURE_HTTPD_AUTH_MD5(, &pass)
			USE_FEATURE_HTTPD_SETUID(, &s_ugid)
			, &bind_addr_or_port
			, &verbose
		);
	if (opt & OPT_DECODE_URL) {
		printf("%s", decodeString(url_for_decode, 1));
		return 0;
	}
#if ENABLE_FEATURE_HTTPD_ENCODE_URL_STR
	if (opt & OPT_ENCODE_URL) {
		printf("%s", encodeString(url_for_encode));
		return 0;
	}
#endif
#if ENABLE_FEATURE_HTTPD_AUTH_MD5
	if (opt & OPT_MD5) {
		puts(pw_encrypt(pass, "$1$"));
		return 0;
	}
#endif
#if ENABLE_FEATURE_HTTPD_SETUID
	if (opt & OPT_SETUID) {
		if (!get_uidgid(&ugid, s_ugid, 1))
			bb_error_msg_and_die("unrecognized user[:group] "
						"name '%s'", s_ugid);
	}
#endif

	xchdir(home_httpd);
	if (!(opt & OPT_INETD)) {
#if BB_MMU
		signal(SIGCHLD, SIG_IGN);
		server_socket = openServer();
#if ENABLE_FEATURE_HTTPD_SETUID
		/* drop privileges */
		if (opt & OPT_SETUID) {
			if (ugid.gid != (gid_t)-1) {
				if (setgroups(1, &ugid.gid) == -1)
					bb_perror_msg_and_die("setgroups");
				xsetgid(ugid.gid);
			}
			xsetuid(ugid.uid);
		}
#endif
#else	/* BB_MMU */
		bb_error_msg_and_die("-i is required");
#endif
	}

#if ENABLE_FEATURE_HTTPD_CGI
	{
		char *p = getenv("PATH");
		/* env strings themself are not freed, no need to strdup(p): */
		clearenv();
		if (p)
			putenv(p - 5);
		if (!(opt & OPT_INETD))
			setenv_long("SERVER_PORT", tcp_port);
	}
#endif

#if ENABLE_FEATURE_HTTPD_RELOAD_CONFIG_SIGHUP
	sighup_handler(0);
#else
	parse_conf(default_path_httpd_conf, FIRST_PARSE);
#endif

#if BB_MMU
	if (opt & OPT_INETD)
		mini_httpd_inetd();
	if (!(opt & OPT_FOREGROUND))
		bb_daemonize(0); /* don't change current directory */
	mini_httpd(server_socket); /* never returns */
#else
	mini_httpd_inetd(); /* never returns */
	/* return 0; */
#endif
}
