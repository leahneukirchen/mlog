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
#include <sys/poll.h>
#include <sys/inotify.h>
int ifd;
#endif

int uflag;			/* remove duplicates */
int fflag;			/* -f follow / -ff tail+follow */
int sflag;			/* strip socklog prefix */

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

	int d = 0;
	int m = 0;
	int t = 0;
	int c = 0;
	int p = 0;
	int e = 0;

	while (s < z && *s != ' ') {
		switch (*s++) {
		case '-': m++; break;
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

	// 2024-01-10T17:57:34.40282
	if (!(d == 19 && m == 2 && t == 1 && c == 2 && e == 0))
		return;

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

	errno = 0;
	ssize_t r = getline(&(logs[i].line), &(logs[i].linealloc), logs[i].file);
	if (r >= 0) {		/* successful read */
		logs[i].linelen = r;
		if (sflag)
			strip(i);
		return 1;
	}

	int e = errno;
	free(logs[i].line);
	logs[i].line = 0;
	logs[i].linealloc = 0;

	if (e == 0 && fflag) { /* hit EOF */
		clearerr(logs[i].file);

		struct stat st;
		r = stat(logs[i].path, &st);
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
	fclose(logs[i].file);
	logs[i].file = 0;
	return 0;
}

int
main(int argc, char *argv[])
{
	int opt;

	while ((opt = getopt(argc, argv, "fsu")) != -1) {
                switch (opt) {
                case 'f': fflag++; break;
                case 's': sflag++; break;
                case 'u': uflag++; break;
                default:
		usage:
                        fputs("usage: mlog [-fu] FILES...\n", stderr);
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
	}

	while (1) {
		for (int i = 0; i < logcnt; i++)
			if (!logs[i].line)
				nextline(i);

		int minidx = -1;
		for (int i = 0; i < logcnt; i++) {
			if (logs[i].line) {
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
			if (!logs[i].line)
				continue;
			if (spacestrcmp(logs[minidx].line, logs[i].line) > 0)
				minidx = i;
		}

		fwrite(logs[minidx].line, logs[minidx].linelen, 1, stdout);
		if (logs[minidx].line[logs[minidx].linelen - 1] != '\n')
			fputc('\n', stdout);

		if (uflag) {
			for (int i = 0; i < logcnt; i++) {
				if (i != minidx &&
				    logs[i].line &&
				    strcmp(logs[i].line, logs[minidx].line) == 0)
					nextline(i);
			}
		}

		nextline(minidx);
	}
}
