/* See LICENSE file for license details. */
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "arg.h"

char *argv0;

#include "config.h"

#undef MIN
#define MIN(x,y)  ((x) < (y) ? (x) : (y))

#define TIMESTAMP_LEN 30

enum req_field {
	REQ_RANGE,
	REQ_MOD,
	NUM_REQ_FIELDS,
};

static char *req_field_str[] = {
	[REQ_RANGE]   = "Range",
	[REQ_MOD]     = "If-Modified-Since",
};

enum req_method {
	M_GET,
	M_HEAD,
	NUM_REQ_METHODS,
};

static char *req_method_str[] = {
	[M_GET]  = "GET",
	[M_HEAD] = "HEAD",
};

struct request {
	enum req_method method;
	char target[PATH_MAX];
	char field[NUM_REQ_FIELDS][FIELD_MAX];
};

enum status {
	S_OK                    = 200,
	S_PARTIAL_CONTENT       = 206,
	S_MOVED_PERMANENTLY     = 301,
	S_NOT_MODIFIED          = 304,
	S_BAD_REQUEST           = 400,
	S_FORBIDDEN             = 403,
	S_NOT_FOUND             = 404,
	S_METHOD_NOT_ALLOWED    = 405,
	S_REQUEST_TIMEOUT       = 408,
	S_REQUEST_TOO_LARGE     = 431,
	S_INTERNAL_SERVER_ERROR = 500,
	S_VERSION_NOT_SUPPORTED = 505,
};

static char *status_str[] = {
	[S_OK]                    = "OK",
	[S_PARTIAL_CONTENT]       = "Partial Content",
	[S_MOVED_PERMANENTLY]     = "Moved Permanently",
	[S_NOT_MODIFIED]          = "Not Modified",
	[S_BAD_REQUEST]           = "Bad Request",
	[S_FORBIDDEN]             = "Forbidden",
	[S_NOT_FOUND]             = "Not Found",
	[S_METHOD_NOT_ALLOWED]    = "Method Not Allowed",
	[S_REQUEST_TIMEOUT]       = "Request Time-out",
	[S_REQUEST_TOO_LARGE]     = "Request Header Fields Too Large",
	[S_INTERNAL_SERVER_ERROR] = "Internal Server Error",
	[S_VERSION_NOT_SUPPORTED] = "HTTP Version not supported",
};

static char *
timestamp(time_t t, char buf[TIMESTAMP_LEN])
{
	if (!t)
		t = time(NULL);
	strftime(buf, TIMESTAMP_LEN, "%a, %d %b %Y %T GMT", gmtime(&t));

	return buf;
}

static size_t
decode(char src[PATH_MAX], char dest[PATH_MAX])
{
	size_t i;
	uint8_t n;
	char *s;

	for (s = src, i = 0; *s; s++, i++) {
		if (*s == '+') {
			dest[i] = ' ';
		} else if (*s == '%' && (sscanf(s + 1, "%2hhx", &n) == 1)) {
			dest[i] = n;
			s += 2;
		} else {
			dest[i] = *s;
		}
	}
	dest[i] = '\0';

	return i;
}

static size_t
encode(char src[PATH_MAX], char dest[PATH_MAX])
{
	size_t i;
	char *s;

	for (s = src, i = 0; *s; s++) {
		if (iscntrl(*s) || (unsigned char)*s > 127) {
			i += snprintf(dest + i, PATH_MAX - i, "%%%02X",
			              (unsigned char)*s);
		} else {
			dest[i] = *s;
			i++;
		}
	}

	return 0;
}

static enum status
sendstatus(int fd, enum status s)
{
	static char t[TIMESTAMP_LEN];

	if (dprintf(fd,
	            "HTTP/1.1 %d %s\r\n"
	            "Date: %s\r\n"
	            "Connection: close\r\n"
	            "%s"
	            "Content-Type: text/html\r\n"
	            "\r\n"
	            "<!DOCTYPE html>\n<html>\n\t<head>\n"
	            "\t\t<title>%d %s</title>\n\t</head>\n\t<body>\n"
	            "\t\t<h1>%d %s</h1>\n\t</body>\n</html>\n",
	            s, status_str[s], timestamp(0, t),
	            (s == S_METHOD_NOT_ALLOWED) ? "Allow: HEAD, GET\r\n" : "",
	            s, status_str[s], s, status_str[s]) < 0) {
		return S_REQUEST_TIMEOUT;
	}

	return s;
}

static int
getrequest(int fd, struct request *r)
{
	size_t hlen, i, mlen;
	ssize_t off;
	char h[HEADER_MAX], *p, *q;

	/*
	 * receive header
	 */
	for (hlen = 0; ;) {
		if ((off = read(fd, h + hlen, sizeof(h) - hlen)) < 0) {
			return sendstatus(fd, S_REQUEST_TIMEOUT);
		} else if (off == 0) {
			break;
		}
		hlen += off;
		if (hlen >= 4 && !memcmp(h + hlen - 4, "\r\n\r\n", 4)) {
			break;
		}
		if (hlen == sizeof(h)) {
			return sendstatus(fd, S_REQUEST_TOO_LARGE);
		}
	}

	/* remove terminating empty line */
	if (hlen < 2) {
		return sendstatus(fd, S_BAD_REQUEST);
	}
	hlen -= 2;

	/* null-terminate the header */
	h[hlen] = '\0';

	/*
	 * parse request line
	 */

	/* METHOD */
	for (i = 0; i < NUM_REQ_METHODS; i++) {
		mlen = strlen(req_method_str[i]);
		if (!strncmp(req_method_str[i], h, mlen)) {
			r->method = i;
			break;
		}
	}
	if (i == NUM_REQ_METHODS) {
		return sendstatus(fd, S_METHOD_NOT_ALLOWED);
	}

	/* a single space must follow the method */
	if (h[mlen] != ' ') {
		return sendstatus(fd, S_BAD_REQUEST);
	}

	/* basis for next step */
	p = h + mlen + 1;

	/* TARGET */
	if (!(q = strchr(p, ' '))) {
		return sendstatus(fd, S_BAD_REQUEST);
	}
	*q = '\0';
	if (q - p + 1 > PATH_MAX) {
		return sendstatus(fd, S_REQUEST_TOO_LARGE);
	}
	memcpy(r->target, p, q - p + 1);
	decode(r->target, r->target);

	/* basis for next step */
	p = q + 1;

	/* HTTP-VERSION */
	if (strncmp(p, "HTTP/", sizeof("HTTP/") - 1)) {
		return sendstatus(fd, S_BAD_REQUEST);
	}
	p += sizeof("HTTP/") - 1;
	if (strncmp(p, "1.0", sizeof("1.0") - 1) &&
	    strncmp(p, "1.1", sizeof("1.1") - 1)) {
		return sendstatus(fd, S_VERSION_NOT_SUPPORTED);
	}
	p += sizeof("1.*") - 1;

	/* check terminator */
	if (strncmp(p, "\r\n", sizeof("\r\n") - 1)) {
		return sendstatus(fd, S_BAD_REQUEST);
	}

	/* basis for next step */
	p += sizeof("\r\n") - 1;

	/*
	 * parse request-fields
	 */

	/* empty all fields */
	for (i = 0; i < NUM_REQ_FIELDS; i++) {
		r->field[i][0] = '\0';
	}

	/* match field type */
	for (; *p != '\0';) {
		for (i = 0; i < NUM_REQ_FIELDS; i++) {
			if (!strncmp(p, req_field_str[i],
			             strlen(req_field_str[i]))) {
				break;
			}
		}
		if (i == NUM_REQ_FIELDS) {
			/* unmatched field, skip this line */
			if (!(q = strstr(p, "\r\n"))) {
				return sendstatus(fd, S_BAD_REQUEST);
			}
			p = q + (sizeof("\r\n") - 1);
			continue;
		}

		p += strlen(req_field_str[i]);

		/* a single colon must follow the field name */
		if (*p != ':') {
			return sendstatus(fd, S_BAD_REQUEST);
		}

		/* skip whitespace */
		for (++p; *p == ' '; p++)
			;

		/* extract field content */
		if (!(q = strstr(p, "\r\n"))) {
			return sendstatus(fd, S_BAD_REQUEST);
		}
		*q = '\0';
		if (q - p + 1 > FIELD_MAX) {
			return sendstatus(fd, S_REQUEST_TOO_LARGE);
		}
		memcpy(r->field[i], p, q - p + 1);

		/* go to next line */
		p = q + (sizeof("\r\n") - 1);
	}

	return 0;
}

static int
compareent(const struct dirent **d1, const struct dirent **d2)
{
	int v;

	v = ((*d2)->d_type == DT_DIR ? 1 : -1) -
	    ((*d1)->d_type == DT_DIR ? 1 : -1);
	if (v)
		return v;
	return strcmp((*d1)->d_name, (*d2)->d_name);
}

static char *
filetype(int t)
{
	switch (t) {
	case DT_FIFO: return "|";
	case DT_DIR:  return "/";
	case DT_LNK:  return "@";
	case DT_SOCK: return "=";
	}
	return "";
}

static enum status
senddir(int fd, char *name, struct request *r)
{
	struct dirent **e;
	size_t i;
	int dirlen;
	static char t[TIMESTAMP_LEN];

	/* read directory */
	if ((dirlen = scandir(name, &e, NULL, compareent)) < 0) {
		return sendstatus(fd, S_FORBIDDEN);
	}

	/* send header as late as possible */
	if (dprintf(fd,
	            "HTTP/1.1 %d %s\r\n"
	            "Date: %s\r\n"
	            "Connection: close\r\n"
		    "Content-Type: text/html\r\n"
		    "\r\n",
	            S_OK, status_str[S_OK], timestamp(0, t)) < 0) {
		return S_REQUEST_TIMEOUT;
	}

	if (r->method == M_GET) {
		/* listing header */
		if (dprintf(fd,
		            "<!DOCTYPE html>\n<html>\n\t<head>"
		            "<title>Index of %s</title></head>\n"
		            "\t<body>\n\t\t<a href=\"..\">..</a>",
		            name) < 0) {
			return S_REQUEST_TIMEOUT;
		}

		/* listing */
		for (i = 0; i < dirlen; i++) {
			/* skip hidden files, "." and ".." */
			if (e[i]->d_name[0] == '.') {
				continue;
			}

			/* entry line */
			if (dprintf(fd, "<br />\n\t\t<a href=\"%s\">%s%s</a>",
			            e[i]->d_name, e[i]->d_name, filetype(e[i]->d_type)) < 0) {
				return S_REQUEST_TIMEOUT;
			}
		}

		/* listing footer */
		if (dprintf(fd, "\n\t</body>\n</html>\n") < 0) {
			return S_REQUEST_TIMEOUT;
		}
	}

	return S_OK;
}

static enum status
sendfile(int fd, char *name, struct request *r, struct stat *st, char *mime,
         off_t lower, off_t upper)
{
	FILE *fp;
	enum status s;
	ssize_t bread, bwritten;
	off_t remaining;
	int range;
	static char buf[BUFSIZ], *p, t1[TIMESTAMP_LEN], t2[TIMESTAMP_LEN];

	/* open file */
	if (!(fp = fopen(name, "r"))) {
		s = sendstatus(fd, S_FORBIDDEN);
		goto cleanup;
	}

	/* seek to lower bound */
	if (fseek(fp, lower, SEEK_SET)) {
		s = sendstatus(fd, S_INTERNAL_SERVER_ERROR);
		goto cleanup;
	}

	/* send header as late as possible */
	range = r->field[REQ_RANGE][0];
	s = range ? S_PARTIAL_CONTENT : S_OK;

	if (dprintf(fd,
	            "HTTP/1.1 %d %s\r\n"
	            "Date: %s\r\n"
	            "Connection: close\r\n"
		    "Last-Modified: %s\r\n"
		    "Content-Type: %s\r\n"
		    "Content-Length: %zu\r\n",
	            s, status_str[s], timestamp(0, t1),
		    timestamp(st->st_mtim.tv_sec, t2), mime, upper - lower) < 0) {
		s = S_REQUEST_TIMEOUT;
		goto cleanup;
	}
	if (range) {
		if (dprintf(fd, "Content-Range: bytes %zu-%zu/%zu\r\n",
		            lower, upper - 1, st->st_size) < 0) {
			s = S_REQUEST_TIMEOUT;
			goto cleanup;
		}
	}
	if (dprintf(fd, "\r\n") < 0) {
		s = S_REQUEST_TIMEOUT;
		goto cleanup;
	}

	if (r->method == M_GET) {
		/* write data until upper bound is hit */
		remaining = upper - lower + 1;

		while ((bread = fread(buf, 1, MIN(sizeof(buf), remaining), fp))) {
			if (bread < 0) {
				return S_INTERNAL_SERVER_ERROR;
			}
			remaining -= bread;
			p = buf;
			while (bread > 0) {
				bwritten = write(fd, p, bread);
				if (bwritten <= 0) {
					return S_REQUEST_TIMEOUT;
				}
				bread -= bwritten;
				p += bwritten;
			}
		}
	}
cleanup:
	if (fp)
		fclose(fp);

	return s;
}

static int
normabspath(char *path)
{
	size_t len;
	int last = 0;
	char *p, *q;

	/* require and skip first slash */
	if (path[0] != '/') {
		return 1;
	}
	p = path + 1;

	/* get length of path */
	len = strlen(p);

	for (; !last; ) {
		/* bound path component within (p,q) */
		if (!(q = strchr(p, '/'))) {
			q = strchr(p, '\0');
			last = 1;
		}

		if (p == q || (q - p == 1 && p[0] == '.')) {
			/* "/" or "./" */
			goto squash;
		} else if (q - p == 2 && p[0] == '.' && p[1] == '.') {
			/* "../" */
			if (p != path + 1) {
				/* place p right after the previous / */
				for (p -= 2; p > path && *p != '/'; p--);
				p++;
			}
			goto squash;
		} else {
			/* move on */
			p = q + 1;
			continue;
		}
squash:
		/* squash (p,q) into void */
		if (last) {
			*p = '\0';
			len = p - path;
		} else {
			memcpy(p, q + 1, len - ((q + 1) - path) + 2);
			len -= (q + 1) - p;
		}
	}

	return 0;
}

static enum status
sendresponse(int fd, struct request *r)
{
	struct stat st;
	struct tm tm;
	size_t len, i;
	off_t lower, upper;
	static char realtarget[PATH_MAX], tmptarget[PATH_MAX], t[TIMESTAMP_LEN];
	char *p, *q, *mime;

	/* normalize target */
	memcpy(realtarget, r->target, sizeof(realtarget));
	if (normabspath(realtarget)) {
		return sendstatus(fd, S_BAD_REQUEST);
	}

	/* reject hidden target */
	if (realtarget[0] == '.' || strstr(realtarget, "/.")) {
		return sendstatus(fd, S_FORBIDDEN);
	}

	/* stat the target */
	if (stat(realtarget, &st) < 0) {
		return sendstatus(fd, (errno == EACCES) ? S_FORBIDDEN : S_NOT_FOUND);
	}

	if (S_ISDIR(st.st_mode)) {
		/* add / to target if not present */
		len = strlen(realtarget);
		if (len == PATH_MAX - 2) {
			return sendstatus(fd, S_REQUEST_TOO_LARGE);
		}
		if (len && realtarget[len - 1] != '/') {
			realtarget[len] = '/';
			realtarget[len + 1] = '\0';
		}
	}

	/* redirect if targets differ */
	if (strcmp(r->target, realtarget)) {
		/* encode realtarget */
		encode(realtarget, tmptarget);

		if (dprintf(fd,
		            "HTTP/1.1 %d %s\r\n"
		            "Date: %s\r\n"
		            "Connection: close\r\n"
		            "Location: %s\r\n"
		            "\r\n",
		            S_MOVED_PERMANENTLY,
		            status_str[S_MOVED_PERMANENTLY], timestamp(0, t),
		            tmptarget) < 0) {
			return S_REQUEST_TIMEOUT;
		}

		return S_MOVED_PERMANENTLY;
	}

	if (S_ISDIR(st.st_mode)) {
		/* append docindex to target */
		if (snprintf(realtarget, sizeof(realtarget), "%s%s",
		             r->target, docindex) >= sizeof(realtarget)) {
			return sendstatus(fd, S_REQUEST_TOO_LARGE);
		}

		/* stat the docindex, which must be a regular file */
		if (stat(realtarget, &st) < 0 || !S_ISREG(st.st_mode)) {
			if (listdirs) {
				/* remove index suffix and serve dir */
				realtarget[strlen(realtarget) -
				           strlen(docindex)] = '\0';
				return senddir(fd, realtarget, r);
			} else {
				/* reject */
				if (!S_ISREG(st.st_mode) || errno == EACCES) {
					return sendstatus(fd, S_FORBIDDEN);
				} else {
					return sendstatus(fd, S_NOT_FOUND);
				}
			}
		}
	}

	/* modified since */
	if (r->field[REQ_MOD][0]) {
		/* parse field */
		if (!strptime(r->field[REQ_MOD], "%a, %d %b %Y %T GMT", &tm)) {
			return sendstatus(fd, S_BAD_REQUEST);
		}

		/* compare with last modification date of the file */
		if (difftime(st.st_mtim.tv_sec, mktime(&tm)) <= 0) {
			if (dprintf(fd,
			            "HTTP/1.1 %d %s\r\n"
			            "Date: %s\r\n"
			            "Connection: close\r\n"
				    "\r\n",
			            S_NOT_MODIFIED, status_str[S_NOT_MODIFIED],
			            timestamp(0, t)) < 0) {
				return S_REQUEST_TIMEOUT;
			}
		}
	}

	/* range */
	lower = 0;
	upper = st.st_size;

	if (r->field[REQ_RANGE][0]) {
		/* parse field */
		p = r->field[REQ_RANGE];

		if (strncmp(p, "bytes=", sizeof("bytes=") - 1)) {
			return sendstatus(fd, S_BAD_REQUEST);
		}
		p += sizeof("bytes=") - 1;

		if (!(q = strchr(p, '-'))) {
			return sendstatus(fd, S_BAD_REQUEST);
		}
		*(q++) = '\0';
		if (p[0]) {
			lower = atoi(p);
		}
		if (q[0]) {
			upper = atoi(q);
		}

		/* sanitize range */
		if (lower < 0 || upper < 0 || lower > upper) {
			return sendstatus(fd, S_BAD_REQUEST);
		}
		upper = MIN(st.st_size, upper);
	}

	/* mime */
	mime = "application/octet-stream";
	if ((p = strrchr(realtarget, '.'))) {
		for (i = 0; i < sizeof(mimes) / sizeof(*mimes); i++) {
			if (!strcmp(mimes[i].ext, p + 1)) {
				mime = mimes[i].type;
				break;
			}
		}
	}

	return sendfile(fd, realtarget, r, &st, mime, lower, upper);
}

static void
serve(int insock)
{
	struct request r;
	struct sockaddr_storage in_sa;
	struct timeval tv;
	pid_t p;
	socklen_t in_sa_len;
	time_t t;
	enum status status;
	int infd;
	char inip4[INET_ADDRSTRLEN], inip6[INET6_ADDRSTRLEN], tstmp[25];

	while (1) {
		/* accept incoming connections */
		in_sa_len = sizeof(in_sa);
		if ((infd = accept(insock, (struct sockaddr *)&in_sa,
		                   &in_sa_len)) < 0) {
			fprintf(stderr, "%s: accept: %s\n", argv0,
			        strerror(errno));
			continue;
		}

		/* fork and handle */
		switch ((p = fork())) {
		case -1:
			fprintf(stderr, "%s: fork: %s\n", argv0,
			        strerror(errno));
			break;
		case 0:
			close(insock);

			/* set connection timeout */
			tv.tv_sec = 30;
			tv.tv_usec = 0;
			if (setsockopt(infd, SOL_SOCKET, SO_RCVTIMEO, &tv,
			               sizeof(tv)) < 0 ||
			    setsockopt(infd, SOL_SOCKET, SO_SNDTIMEO, &tv,
			               sizeof(tv)) < 0) {
				fprintf(stderr, "%s: setsockopt: %s\n",
				        argv0, strerror(errno));
				return;
			}

			/* handle request */
			if (!(status = getrequest(infd, &r))) {
				status = sendresponse(infd, &r);
			}

			/* write output to log */
			t = time(NULL);
			strftime(tstmp, sizeof(tstmp), "%Y-%m-%dT%H:%M:%S",
			         gmtime(&t));

			if (in_sa.ss_family == AF_INET) {
				inet_ntop(AF_INET,
				          &(((struct sockaddr_in *)&in_sa)->sin_addr),
				          inip4, sizeof(inip4));
				printf("%s\t%s\t%d\t%s\n", tstmp, inip4,
				       status, r.target);
			} else {
				inet_ntop(AF_INET6,
				          &(((struct sockaddr_in6*)&in_sa)->sin6_addr),
				          inip6, sizeof(inip6));
				printf("%s\t%s\t%d\t%s\n", tstmp, inip6,
				       status, r.target);
			}

			/* clean up and finish */
			shutdown(infd, SHUT_RD);
			shutdown(infd, SHUT_WR);
			close(infd);
			_exit(0);
		default:
			/* close the connection in the parent */
			close(infd);
		}
	}
}

void
die(const char *errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);

	exit(1);
}

static int
getipsock(void)
{
	struct addrinfo hints, *ai, *p;
	int ret, insock = 0, yes;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((ret = getaddrinfo(host, port, &hints, &ai))) {
		die("%s: getaddrinfo: %s\n", argv0, gai_strerror(ret));
	}

	for (yes = 1, p = ai; p; p = p->ai_next) {
		if ((insock = socket(p->ai_family, p->ai_socktype,
		                     p->ai_protocol)) < 0) {
			continue;
		}
		if (setsockopt(insock, SOL_SOCKET, SO_REUSEADDR, &yes,
		               sizeof(int)) < 0) {
			die("%s: setsockopt: %s\n", argv0, strerror(errno));
		}
		if (bind(insock, p->ai_addr, p->ai_addrlen) < 0) {
			close(insock);
			continue;
		}
		break;
	}
	freeaddrinfo(ai);
	if (!p) {
		die("%s: failed to bind\n", argv0);
	}

	if (listen(insock, SOMAXCONN) < 0) {
		die("%s: listen: %s\n", argv0, strerror(errno));
	}

	return insock;
}

static int
getusock(char *udsname)
{
	struct sockaddr_un addr;
	int insock;

	if ((insock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		die("%s: socket: %s\n", argv0, strerror(errno));
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, udsname, sizeof(addr.sun_path) - 1);

	unlink(udsname);

	if (bind(insock, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
		die("%s: bind: %s\n", argv0, strerror(errno));
	}

	if (listen(insock, SOMAXCONN) < 0) {
		die("%s: listen: %s\n", argv0, strerror(errno));
	}

	return insock;
}

static void
usage(void)
{
	die("usage: %s [-v] [[[-h host] [-p port]] | [-U udsocket]] "
	    "[-d dir] [-u user] [-g group]\n", argv0);
}

int
main(int argc, char *argv[])
{
	struct passwd *pwd = NULL;
	struct group *grp = NULL;
	struct rlimit rlim;
	int insock;
	char *udsname = NULL;

	ARGBEGIN {
	case 'd':
		servedir = EARGF(usage());
		break;
	case 'g':
		group = EARGF(usage());
		break;
	case 'h':
		host = EARGF(usage());
		break;
	case 'p':
		port = EARGF(usage());
		break;
	case 'u':
		user = EARGF(usage());
		break;
	case 'U':
		udsname = EARGF(usage());
		break;
	case 'v':
		fputs("quark-"VERSION"\n", stderr);
		return 0;
	default:
		usage();
	} ARGEND

	if (argc)
		usage();

	/* reap children automatically */
	if (signal(SIGCHLD, SIG_IGN) == SIG_ERR) {
		fprintf(stderr, "%s: signal: Failed to set SIG_IGN on"
		        "SIGCHLD\n", argv0);
		return 1;
	}

	/* raise the process limit */
	rlim.rlim_cur = rlim.rlim_max = maxnprocs;
	if (setrlimit(RLIMIT_NPROC, &rlim) < 0) {
		fprintf(stderr, "%s: setrlimit RLIMIT_NPROC: %s\n", argv0,
		        strerror(errno));
		return 1;
	}

	/* validate user and group */
	errno = 0;
	if (user && !(pwd = getpwnam(user))) {
		die("%s: invalid user %s\n", argv0, user);
	}
	errno = 0;
	if (group && !(grp = getgrnam(group))) {
		die("%s: invalid group %s\n", argv0, group);
	}

	/* bind socket */
	insock = udsname ? getusock(udsname) : getipsock();

	/* chroot */
	if (chdir(servedir) < 0) {
		die("%s: chdir %s: %s\n", argv0, servedir, strerror(errno));
	}
	if (chroot(".") < 0) {
		die("%s: chroot .: %s\n", argv0, strerror(errno));
	}

	/* drop root */
	if (grp && setgroups(1, &(grp->gr_gid)) < 0) {
		die("%s: setgroups: %s\n", argv0, strerror(errno));
	}
	if (grp && setgid(grp->gr_gid) < 0) {
		die("%s: setgid: %s\n", argv0, strerror(errno));
	}
	if (pwd && setuid(pwd->pw_uid) < 0) {
		die("%s: setuid: %s\n", argv0, strerror(errno));
	}
	if (getuid() == 0) {
		die("%s: won't run as root user\n", argv0);
	}
	if (getgid() == 0) {
		die("%s: won't run as root group\n", argv0);
	}

	serve(insock);
	close(insock);

	return 0;
}
