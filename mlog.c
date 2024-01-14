/* mlog - merge log files by timestamp */

#ifdef __linux__
#define USE_INOTIFY
#endif

#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef USE_INOTIFY
#include <poll.h>
#include <sys/inotify.h>
int ifd;
#endif

int uflag;			/* remove duplicates */
int fflag;			/* -f follow / -ff tail+follow */
int sflag;			/* strip socklog prefix */
int nflag;			/* number of lines at end to print */

struct logfile {
	char *path;
	FILE *file;
	char *line;
	size_t linelen;
	size_t linealloc;
	dev_t st_dev;
	ino_t st_ino;
#ifdef USE_INOTIFY
	int wd;
#endif
};

struct logfile *logs;
int logcnt;

int spacestrcmp(const char *a, const char *b)
{
        for (; *a==*b && *a && *a != ' '; a++, b++);
        return *(unsigned char *)a - *(unsigned char *)b;
}

void
reopen(int i)
{
	logs[i].file = fopen(logs[i].path, "r");
	if (!logs[i].file)
		return;

	struct stat st;
	fstat(fileno(logs[i].file), &st);

	if (S_ISDIR(st.st_mode)) {
		fclose(logs[i].file);
		logs[i].file = 0;
		errno = EISDIR;
		return;
	}

	logs[i].st_dev = st.st_dev;
	logs[i].st_ino = st.st_ino;

#ifdef USE_INOTIFY
	if (logs[i].wd)
		inotify_rm_watch(ifd, logs[i].wd);

	// note that IN_DELETE_SELF doesn't work as we keep the file open
	logs[i].wd = inotify_add_watch(ifd, logs[i].path, IN_MODIFY | IN_ATTRIB | IN_MOVE_SELF /* ? | IN_CLOSE_WRITE */);
#endif
}

void
wait_inotify()
{
#ifdef USE_INOTIFY
	struct pollfd fds[1];
	fds[0].fd = ifd;
	fds[0].events = POLLIN;

	int timeout = -1;
	for (int i = 0; i < logcnt; i++)
		if (!logs[i].file)
			timeout = 3000; /* missing file, need to wait if it appears */

	int r = poll(fds, 1, timeout);
	if (r == 1) {
		char ibuf[255];
		while (read(ifd, ibuf, sizeof ibuf) == sizeof ibuf)
			;
	}
	if (r != 0)
		usleep(25000);	/* 25ms, debounce quick subsequent writes */
#else
	sleep(3);
#endif
}

void
strip(int i)
{
	char *s = logs[i].line;
	char *z = s + logs[i].linelen;

	if ('0' <= s[0] && s[0] <= '9') { /* ISO */
		int d = 0, m= 0, t = 0, c = 0, p = 0, e = 0;

		// 2024-01-10T17:57:34.40282
		// 2024-01-10_17:57:34.40282
		while (s < z && *s != ' ') {
			switch (*s++) {
			case '-': m++; break;
			case '_':
			case 'T': t++; break;
			case ':': c++; break;
			case '.': p++; break;
			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9': d++; break;
			default: e++;
			}
		}
		if (!(e == 0 && d == 19 && m == 2 && t == 1 && c == 2))
			return;
	} else if (s[0] == '@') { /* hex TAI */
		// @4000000065a07a8e011726e4
		size_t h = 0;
		while (++s < z && *s != ' ')
			if ((unsigned)*s-'0' < 10 || ((unsigned)*s|32)-'a' < 6)
				h++;
		if (h != 24)
			return;
	} else {
		return;
	}

	char *b = s;

	// skip "daemon.notice:"
	while (s < z && *++s != ' ')
		;
	if (s[-1] != ':')
		return;

	// skip " Jan 10 19:17:56"
	if (s + 16 < z && s[10] == ':' && s[13] == ':')
		s += 16;

	if (s >= z)
		return;

	size_t shrink = s - b;
	size_t rest = logs[i].linelen - shrink - (b - logs[i].line);
	memmove(b, s, rest);
	logs[i].linelen -= shrink;
}

static int
ready(int i)
{
	if (!logs[i].line)
		return 0;
	if (logs[i].linelen == 0)
		return 0;
	if (logs[i].line[logs[i].linelen - 1] == '\n')
		return 1;
	return 0;
}

int
nextline(int i)
{
	if (!logs[i].file) {
		if (fflag)
			reopen(i);
		else
			return 0;
		if (!logs[i].file)
			return 0;
	}

	char *line = 0;
	size_t linealloc = 0;
	ssize_t linelen = 0;
	int append = 0;

	if (!logs[i].line) {
		;
	} else if (logs[i].linelen == 0 || ready(i)) {
		line = logs[i].line;
		linealloc = logs[i].linealloc;
	} else {
		append = 1;
	}

	errno = 0;
	clearerr(logs[i].file);
	linelen = getline(&line, &linealloc, logs[i].file);
	if (linelen > 0) {	/* successful read */
		if (append) {
			if (logs[i].linealloc < logs[i].linelen + linelen) {
				logs[i].linealloc += linelen;
				logs[i].line = realloc(logs[i].line, logs[i].linealloc);
			}
			memcpy(logs[i].line + logs[i].linelen,
			    line, linelen);
			logs[i].linelen += linelen;
			free(line);
		} else {
			logs[i].line = line;
			logs[i].linealloc = linealloc;
			logs[i].linelen = linelen;
		}

		if (sflag && ready(i))
			strip(i);
		return 1;
	}

	int e = errno;
	if (e == 0 && fflag) { /* hit EOF */
		if (!append)
			logs[i].linelen = 0;

		struct stat st;
		int r = stat(logs[i].path, &st);
		if (r < 0 ||
		    st.st_dev != logs[i].st_dev ||
		    st.st_ino != logs[i].st_ino ||
		    ftell(logs[i].file) > st.st_size) {
			fprintf(stderr, "stat failed, file vanished?\n");
			fclose(logs[i].file);
			logs[i].file = 0;
			if (r == 0 && fflag)
				return nextline(i);
			return 0;
		}

		return 1;
	}

	if (e != 0)
		fprintf(stderr, "error reading '%s': %s\n",
		    logs[i].path, strerror(e));

	free(line);
	logs[i].line = 0;
	logs[i].linealloc = 0;

	fclose(logs[i].file);
	logs[i].file = 0;
	return 0;
}

void
tail_line(FILE *file, int n)
{
	size_t page = 4096;

	if (fseek(file, -page, SEEK_END) < 0) {
		if (errno == EINVAL)
			rewind(file);
		else
			return; /* can't seek */
	}

	int l = -1;		/* line ends with newline */
	char buf[page];

	while (1) {
		clearerr(file);
		long off = ftell(file);
		ssize_t in = fread(buf, 1, page, file);

		size_t i;
		for (i = in; l < n && i > 0; i--) {
			if (buf[i] == '\n')
				l++;
		}

		if (l == n) {
			fseek(file, off + i + 2, SEEK_SET);
			break;
		} else if (off == 0) {	/* file too short */
			rewind(file);
			break;
		}

		/* seek back more and count again */
		if (fseek(file, -2*page, SEEK_CUR) < 0) {
			if (errno == EINVAL) {
				rewind(file);
				break;
			} else {
				return;
			}
		}
	}

	clearerr(file);
}

int
main(int argc, char *argv[])
{
	int opt;

	while ((opt = getopt(argc, argv, "fn:su")) != -1) {
                switch (opt) {
                case 'f': fflag++; break;
                case 'n': nflag = atoi(optarg); break;
                case 's': sflag++; break;
                case 'u': uflag++; break;
                default:
		usage:
                        fputs("usage: mlog [-fsu] [-n LINES] FILES...\n", stderr);
                        exit(2);
                }
        }

	logcnt = argc - optind;
	if (logcnt == 0)
		goto usage;
	logs = calloc(logcnt, sizeof logs[0]);
	if (!logs)
		exit(111);

#ifdef USE_INOTIFY
        ifd = inotify_init();
        if (ifd < 0)
                exit(111);
#endif

	for (int i = 0; i < logcnt; i++) {
		logs[i].path = argv[optind+i];
		reopen(i);
		if (!logs[i].file)
			fprintf(stderr, "mlog: can't open log '%s': %s\n",
			    logs[i].path,
			    strerror(errno));
		if (fflag > 1 && logs[i].file)
			fseek(logs[i].file, 0L, SEEK_END);
		else if (nflag > 0 && logs[i].file)
			tail_line(logs[i].file, nflag);
	}

	while (1) {
		for (int i = 0; i < logcnt; i++)
			if (!ready(i))
				nextline(i);

		int minidx = -1;
		for (int i = 0; i < logcnt; i++) {
			if (ready(i)) {
				minidx = i;
				break;
			}
		}
		if (minidx < 0) {
			if (fflag) {
				wait_inotify();
				continue;
			} else {
				break;
			}
		}
		for (int i = minidx + 1; i < logcnt; i++) {
			if (!ready(i))
				continue;
			if (spacestrcmp(logs[minidx].line, logs[i].line) > 0)
				minidx = i;
		}

		fwrite(logs[minidx].line, logs[minidx].linelen, 1, stdout);

		if (uflag) {
			for (int i = 0; i < logcnt; i++) {
				if (i != minidx &&
				    ready(i) &&
				    strcmp(logs[i].line, logs[minidx].line) == 0)
					nextline(i);
			}
		}

		nextline(minidx);
	}

	for (int i = 0; i < logcnt; i++)
		free(logs[i].line);
	free(logs);
}
