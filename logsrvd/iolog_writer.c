/*
 * Copyright (c) 2019 Todd C. Miller <Todd.Miller@courtesan.com>
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
 */

#include "config.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#ifdef HAVE_STDBOOL_H
# include <stdbool.h>
#else
# include "compat/stdbool.h"
#endif /* HAVE_STDBOOL_H */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "log_server.pb-c.h"
#include "sudo_compat.h"
#include "sudo_queue.h"
#include "sudo_debug.h"
#include "sudo_util.h"
#include "sudo_fatal.h"
#include "iolog_util.h"
#include "logsrvd.h"

/* I/O log file names relative to iolog_dir. */
static const char *iolog_names[] = {
    "stdin",	/* IOFD_STDIN */
    "stdout",	/* IOFD_STDOUT */
    "stderr",	/* IOFD_STDERR */
    "ttyin",	/* IOFD_TTYIN  */
    "ttyout",	/* IOFD_TTYOUT */
    "timing",	/* IOFD_TIMING */
    NULL	/* IOFD_MAX */
};

static inline bool
has_numval(InfoMessage *info)
{
    return info->value_case == INFO_MESSAGE__VALUE_NUMVAL;
}

static inline bool
has_strval(InfoMessage *info)
{
    return info->value_case == INFO_MESSAGE__VALUE_STRVAL;
}

static inline bool
has_strlistval(InfoMessage *info)
{
    return info->value_case == INFO_MESSAGE__VALUE_STRLISTVAL;
}

/*
 * Fill in I/O log details from an ExecMessage
 * Only makes a shallow copy of strings and string lists.
 */
static bool
iolog_details_fill(struct iolog_details *details, ExecMessage *msg)
{
    size_t idx;
    bool ret = true;
    debug_decl(iolog_details_fill, SUDO_DEBUG_UTIL)

    memset(details, 0, sizeof(*details));

    /* Start time. */
    details->start_time = msg->start_time->tv_sec;

    /* Default values */
    details->lines = 24;
    details->columns = 80;

    /* Pull out values by key from info array. */
    for (idx = 0; idx < msg->n_info_msgs; idx++) {
	InfoMessage *info = msg->info_msgs[idx];
	const char *key = info->key;
	switch (key[0]) {
	case 'c':
	    if (strcmp(key, "columns") == 0) {
		if (!has_numval(info)) {
		    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
			"columns specified but not a number");
		} else if (info->numval <= 0 || info->numval > INT_MAX) {
		    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
			"columns (%" PRId64 ") out of range", info->numval);
		} else {
		    details->columns = info->numval;
		}
		continue;
	    }
	    if (strcmp(key, "command") == 0) {
		if (has_strval(info)) {
		    details->command = info->strval;
		} else {
		    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
			"command specified but not a string");
		}
		continue;
	    }
	    if (strcmp(key, "cwd") == 0) {
		if (has_strval(info)) {
		    details->cwd = info->strval;
		} else {
		    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
			"cwd specified but not a string");
		}
		continue;
	    }
	    break;
	case 'l':
	    if (strcmp(key, "lines") == 0) {
		if (!has_numval(info)) {
		    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
			"lines specified but not a number");
		} else if (info->numval <= 0 || info->numval > INT_MAX) {
		    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
			"lines (%" PRId64 ") out of range", info->numval);
		} else {
		    details->lines = info->numval;
		}
		continue;
	    }
	    break;
	case 'r':
	    if (strcmp(key, "runargv") == 0) {
		if (has_strlistval(info)) {
		    details->argv = info->strlistval->strings;
		    details->argc = info->strlistval->n_strings;
		} else {
		    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
			"runargv specified but not a string list");
		}
		continue;
	    }
	    if (strcmp(key, "rungroup") == 0) {
		if (has_strval(info)) {
		    details->rungroup = info->strval;
		} else {
		    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
			"rungroup specified but not a string");
		}
		continue;
	    }
	    if (strcmp(key, "runuser") == 0) {
		if (has_strval(info)) {
		    details->runuser = info->strval;
		} else {
		    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
			"runuser specified but not a string");
		}
		continue;
	    }
	    break;
	case 's':
	    if (strcmp(key, "submithost") == 0) {
		if (has_strval(info)) {
		    details->submithost = info->strval;
		} else {
		    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
			"submithost specified but not a string");
		}
		continue;
	    }
	    if (strcmp(key, "submituser") == 0) {
		if (has_strval(info)) {
		    details->submituser = info->strval;
		} else {
		    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
			"submituser specified but not a string");
		}
		continue;
	    }
	    break;
	case 't':
	    if (strcmp(key, "ttyname") == 0) {
		if (has_strval(info)) {
		    details->ttyname = info->strval;
		} else {
		    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
			"ttyname specified but not a string");
		}
		continue;
	    }
	    break;
	}
    }

    /* Check for required settings */
    if (details->submituser == NULL) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "missing user in ExecMessage");
	ret = false;
    }
    if (details->submithost == NULL) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "missing host in ExecMessage");
	ret = false;
    }
    if (details->command == NULL) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "missing command in ExecMessage");
	ret = false;
    }

    debug_return_bool(ret);
}

/*
 * Create I/O log path
 * Sets iolog_dir and iolog_dir_fd in the closure
 * XXX - use iolog_dir and iolog_file code from sudoers/iolog.c
 */
static bool
create_iolog_dir(struct iolog_details *details, struct connection_closure *closure)
{
    char path[PATH_MAX];
    int len;
    debug_decl(create_iolog_dir, SUDO_DEBUG_UTIL)

    /* Create IOLOG_DIR/host/user/XXXXXX directory */
    if (mkdir(IOLOG_DIR, 0755) == -1 && errno != EEXIST) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "mkdir %s", path);
	goto bad;
    }
    len = snprintf(path, sizeof(path), "%s/%s", IOLOG_DIR,
	details->submithost);
    if (len < 0 || len >= ssizeof(path)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "failed to snprintf I/O log path");
	goto bad;
    }
    if (mkdir(path, 0755) == -1 && errno != EEXIST) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "mkdir %s", path);
	goto bad;
    }
    len = snprintf(path, sizeof(path), "%s/%s/%s", IOLOG_DIR,
	details->submithost, details->submituser);
    if (len < 0 || len >= ssizeof(path)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "failed to snprintf I/O log path");
	goto bad;
    }
    if (mkdir(path, 0755) == -1 && errno != EEXIST) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "mkdir %s", path);
	goto bad;
    }
    len = snprintf(path, sizeof(path), "%s/%s/%s/XXXXXX", IOLOG_DIR,
	details->submithost, details->submituser);
    if (len < 0 || len >= ssizeof(path)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "failed to snprintf I/O log path");
	goto bad;
    }
    if (mkdtemp(path) == NULL) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "mkdtemp %s", path);
	goto bad;
    }

    /* Make a copy of iolog_dir for error messages. */
    if ((closure->iolog_dir = strdup(path)) == NULL) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "strdup");
	goto bad;
    }

    /* We use iolog_dir_fd in calls to openat(2) */
    closure->iolog_dir_fd = open(closure->iolog_dir, O_RDONLY);
    if (closure->iolog_dir_fd == -1) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "%s", closure->iolog_dir);
	goto bad;
    }

    debug_return_bool(true);
bad:
    free(closure->iolog_dir);
    debug_return_bool(false);
}

/*
 * Write the sudo-style I/O log info file containing user and command info.
 */
static bool
iolog_details_write(struct iolog_details *details, struct connection_closure *closure)
{
    int fd, i;
    FILE *fp;
    int error;
    debug_decl(iolog_details_write, SUDO_DEBUG_UTIL)

    fd = openat(closure->iolog_dir_fd, "log", O_CREAT|O_EXCL|O_WRONLY, 0600);
    if (fd == -1 || (fp = fdopen(fd, "w")) == NULL) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to open %s", closure->iolog_dir);
	if (fd != -1)
	    close(fd);
	debug_return_bool(false);
    }

    fprintf(fp, "%lld:%s:%s:%s:%s:%d:%d\n%s\n",
	(long long)details->start_time, details->submituser,
	details->runuser ? details->runuser : RUNAS_DEFAULT,
	details->rungroup ? details->rungroup : "",
	details->ttyname ? details->ttyname : "unknown",
	details->lines, details->columns,
	details->cwd ? details->cwd : "unknown");
    fputs(details->command, fp);
    for (i = 1; i < details->argc; i++) {
	fputc(' ', fp);
	fputs(details->argv[i], fp);
    }
    fputc('\n', fp);
    fflush(fp);
    if ((error = ferror(fp))) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to write to I/O log file %s", closure->iolog_dir);
    }
    fclose(fp);

    debug_return_bool(!error);
}

static bool
iolog_open(int iofd, struct connection_closure *closure)
{
    debug_decl(iolog_open, SUDO_DEBUG_UTIL)

    if (iofd < 0 || iofd >= IOFD_MAX) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "invalid iofd %d", iofd);
	debug_return_bool(false);
    }

    closure->io_fds[iofd] = openat(closure->iolog_dir_fd,
	iolog_names[iofd], O_CREAT|O_EXCL|O_WRONLY, 0600);
    debug_return_bool(closure->io_fds[iofd] != -1);
}

void
iolog_close(struct connection_closure *closure)
{
    int i;
    debug_decl(iolog_close, SUDO_DEBUG_UTIL)

    for (i = 0; i < IOFD_MAX; i++) {
	if (closure->io_fds[i] == -1)
	    continue;
	close(closure->io_fds[i]);
    }
    if (closure->iolog_dir_fd != -1)
	close(closure->iolog_dir_fd);

    debug_return;
}

bool
iolog_init(ExecMessage *msg, struct connection_closure *closure)
{
    struct iolog_details details;
    int i;
    debug_decl(iolog_init, SUDO_DEBUG_UTIL)

    /* Init io_fds in closure. */
    for (i = 0; i < IOFD_MAX; i++)
        closure->io_fds[i] = -1;

    /* Fill in iolog_details */
    if (!iolog_details_fill(&details, msg))
	debug_return_bool(false);

    /* Create I/O log dir */
    if (!create_iolog_dir(&details, closure))
	debug_return_bool(false);

    /* Write sudo I/O log info file */
    if (!iolog_details_write(&details, closure))
	debug_return_bool(false);

    /* Create timing, stdout, stderr and ttyout files for sudoreplay. */
    if (!iolog_open(IOFD_TIMING, closure) ||
	!iolog_open(IOFD_STDOUT, closure) ||
	!iolog_open(IOFD_STDERR, closure) ||
	!iolog_open(IOFD_TTYOUT, closure))
	debug_return_bool(false);

    /* Ready to log I/O buffers. */
    debug_return_bool(true);
}

/*
 * Read the next record from the timing file.
 * Return 0 on success, 1 on EOF and -1 on error.
 */
static int
read_timing_record(FILE *fp, struct timing_closure *timing)
{
    char line[LINE_MAX];
    debug_decl(read_timing_record, SUDO_DEBUG_UTIL)

    /* Read next record from timing file. */
    if (fgets(line, sizeof(line), fp) == NULL) {
	/* EOF or error reading timing file, we are done. */
	if (feof(fp))
	    debug_return_int(1);	/* EOF */
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "error reading timing file");
	debug_return_int(-1);
    }

    /* Parse timing file record. */
    line[strcspn(line, "\n")] = '\0';
    if (!parse_timing(line, timing)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "invalid timing file line: %s", line);
	debug_return_int(-1);
    }

    debug_return_int(0);
}

bool
iolog_restart(RestartMessage *msg, struct connection_closure *closure)
{
    struct timespec target;
    struct timing_closure timing;
    FILE *fp = NULL;
    int i, fd;
    off_t length;
    debug_decl(iolog_init, SUDO_DEBUG_UTIL)

    target.tv_sec = msg->resume_point->tv_sec;
    target.tv_nsec = msg->resume_point->tv_nsec;

    /* We use iolog_dir_fd in calls to openat(2) */
    if ((closure->iolog_dir = strdup(msg->log_id)) == NULL) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "strdup");
	goto bad;
    }
    closure->iolog_dir_fd = open(closure->iolog_dir, O_RDONLY);
    if (closure->iolog_dir_fd == -1) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "%s", closure->iolog_dir);
	goto bad;
    }

    /* Open existing I/O log files. */
    for (i = 0; i < IOFD_MAX; i++) {
	closure->io_fds[i] = openat(closure->iolog_dir_fd, iolog_names[i],
	    O_RDWR, 0600);
    }

    /* Dup IOFD_TIMING to a stream for easier processing. */
    if (closure->io_fds[IOFD_TIMING] == -1)
	goto bad;
    if ((fd = dup(closure->io_fds[IOFD_TIMING])) == -1) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "dup");
	goto bad;
    }
    if ((fp = fdopen(fd, "r")) == NULL) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "fdopen");
	goto bad;
    }

    /* Parse timing file until we reach the target point. */
    /* XXX - split up */
    for (;;) {
	if (read_timing_record(fp, &timing) != 0)
	    goto bad;
	sudo_timespecadd(&timing.delay, &closure->elapsed_time,
	    &closure->elapsed_time);
	if (timing.event < IOFD_TIMING) {
	    if (closure->io_fds[timing.event] == -1) {
		/* Missing log file. */
		sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		    "iofd %d referenced but not open", timing.event);
		goto bad;
	    }
	    length = lseek(closure->io_fds[timing.event], timing.u.nbytes,
		SEEK_CUR);
	    if (length == -1) {
		sudo_debug_printf(
		    SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
		    "lseek(%d, %lld, SEEK_CUR", closure->io_fds[timing.event],
		    (long long)timing.u.nbytes);
		goto bad;
	    }
	    if (ftruncate(closure->io_fds[timing.event], length) == -1) {
		sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
		    "ftruncate(%d, %lld)", closure->io_fds[IOFD_TIMING], length);
		goto bad;
	    }
	}
	if (sudo_timespeccmp(&closure->elapsed_time, &target, >=)) {
	    if (sudo_timespeccmp(&closure->elapsed_time, &target, ==))
		break;

	    /* Mismatch between resume point and stored log. */
	    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		"resume point mismatch, target [%lld, %ld], have [%lld, %ld]",
		(long long)target.tv_sec, target.tv_nsec,
		(long long)closure->elapsed_time.tv_sec,
		closure->elapsed_time.tv_nsec);
	    goto bad;
	}
    }
    /* Update timing file position after determining resume point. */
#ifdef HAVE_FSEEKO
    length = ftello(fp);
#else
    length = ftell(fp);
#endif
    fclose(fp);
    if (lseek(closure->io_fds[IOFD_TIMING], length, SEEK_SET) == -1) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "lseek(%d, %lld, SEEK_SET", closure->io_fds[IOFD_TIMING], length);
	goto bad;
    }
    if (ftruncate(closure->io_fds[IOFD_TIMING], length) == -1) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "ftruncate(%d, %lld)", closure->io_fds[IOFD_TIMING], length);
	goto bad;
    }

    /* Ready to log I/O buffers. */
    debug_return_bool(true);
bad:
    if (fp != NULL)
	fclose(fp);
    debug_return_bool(false);
}

static bool
iolog_write(int iofd, void *buf, size_t len, struct connection_closure *closure)
{
    debug_decl(iolog_write, SUDO_DEBUG_UTIL)
    size_t nread;

    if (iofd < 0 || iofd >= IOFD_MAX) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "invalid iofd %d", iofd);
	debug_return_bool(false);
    }
    nread = write(closure->io_fds[iofd], buf, len);
    debug_return_bool(nread == len);
}

/*
 * Add given delta to elapsed time.
 * We cannot use timespecadd here since delta is not struct timespec.
 */
static void
update_elapsed_time(TimeSpec *delta, struct timespec *elapsed)
{
    debug_decl(update_elapsed_time, SUDO_DEBUG_UTIL)

    /* Cannot use timespecadd since msg doesn't use struct timespec. */
    elapsed->tv_sec += delta->tv_sec;
    elapsed->tv_nsec += delta->tv_nsec;
    while (elapsed->tv_nsec >= 1000000000) {
	elapsed->tv_sec++;
	elapsed->tv_nsec -= 1000000000;
    }

    debug_return;
}

int
store_iobuf(int iofd, IoBuffer *msg, struct connection_closure *closure)
{
    char tbuf[1024];
    int len;
    debug_decl(store_iobuf, SUDO_DEBUG_UTIL)

    /* Open log file as needed. */
    if (closure->io_fds[iofd] == -1) {
	if (!iolog_open(iofd, closure))
	    debug_return_int(-1);
    }

    /* Format timing data. */
    /* FIXME - assumes IOFD_* matches IO_EVENT_* */
    len = snprintf(tbuf, sizeof(tbuf), "%d %lld.%09d %zu\n",
	iofd, (long long)msg->delay->tv_sec, (int)msg->delay->tv_nsec,
	msg->data.len);
    if (len < 0 || len >= ssizeof(tbuf)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to format timing buffer");
	debug_return_int(-1);
    }

    /* Write to specified I/O log file. */
    if (!iolog_write(iofd, msg->data.data, msg->data.len, closure))
	debug_return_int(-1);

    /* Write timing data. */
    if (!iolog_write(IOFD_TIMING, tbuf, len, closure))
	debug_return_int(-1);

    update_elapsed_time(msg->delay, &closure->elapsed_time);

    debug_return_int(0);
}

int
store_suspend(CommandSuspend *msg, struct connection_closure *closure)
{
    char tbuf[1024];
    int len;
    debug_decl(store_suspend, SUDO_DEBUG_UTIL)

    /* Format timing data including suspend signal. */
    len = snprintf(tbuf, sizeof(tbuf), "%d %lld.%09d %s\n", IO_EVENT_SUSPEND,
	(long long)msg->delay->tv_sec, (int)msg->delay->tv_nsec,
	msg->signal);
    if (len < 0 || len >= ssizeof(tbuf)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to format timing buffer");
	debug_return_int(-1);
    }

    /* Write timing data. */
    if (!iolog_write(IOFD_TIMING, tbuf, len, closure))
	debug_return_int(-1);

    update_elapsed_time(msg->delay, &closure->elapsed_time);

    debug_return_int(0);
}

int
store_winsize(ChangeWindowSize *msg, struct connection_closure *closure)
{
    char tbuf[1024];
    int len;
    debug_decl(store_winsize, SUDO_DEBUG_UTIL)

    /* Format timing data including new window size. */
    len = snprintf(tbuf, sizeof(tbuf), "%d %lld.%09d %d %d\n", IO_EVENT_WINSIZE,
	(long long)msg->delay->tv_sec, (int)msg->delay->tv_nsec,
	msg->rows, msg->cols);
    if (len < 0 || len >= ssizeof(tbuf)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to format timing buffer");
	debug_return_int(-1);
    }

    /* Write timing data. */
    if (!iolog_write(IOFD_TIMING, tbuf, len, closure))
	debug_return_int(-1);

    update_elapsed_time(msg->delay, &closure->elapsed_time);

    debug_return_int(0);
}