/* Finit daemon log functions
 *
 * Copyright (c) 2008-2022  Joachim Wiberg <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config.h"		/* Generated by configure script */

#include <stdio.h>
#include <stdarg.h>
#ifdef _LIBITE_LITE
# include <libite/lite.h>
#else
# include <lite/lite.h>
#endif

#include "finit.h"
#include "helpers.h"
#include "log.h"
#include "util.h"

static int up       = 0;
static int loglevel = LOG_INFO;

void log_init(void)
{
	ttinit();

	if (debug)
		loglevel = LOG_DEBUG;
	else
		loglevel = LOG_INFO;
}

/* If we enabled terse mode at boot, restore to previous setting at shutdown */
void log_exit(void)
{
	/*
	 * Unless in debug mode at shutdown, Reinitialize screen,
	 * terminal may have been resized at runtime
	 */
	if (!debug)
		ttinit();

	enable_progress(1);
}

static int log_open(void)
{
	int opts;

	if (up)
		return 1;
	if (access("/dev/log", W_OK))
		return 0;

	opts = LOG_CONS | LOG_PID;
	if (debug)
		opts |= LOG_PERROR;

	openlog("finit", opts, LOG_DAEMON);
	setlogmask(LOG_UPTO(loglevel));

	return up = 1;
}

static void log_close(void)
{
	closelog();
	up = 0;
}

/* Toggle debug mode */
void log_debug(void)
{
	debug = !debug;

	log_close();
	log_init();
	log_open();

	logit(LOG_NOTICE, "Debug mode %s", debug ? "enabled" : "disabled");
}

/*
 * Log to /dev/kmsg until syslogd has started, then openlog()
 * and continue logging as a regular daemon.
 */
void logit(int prio, const char *fmt, ...)
{
	va_list ap;
	FILE *fp;

	va_start(ap, fmt);

	if (up || log_open()) {
		vsyslog(prio, fmt, ap);
		goto done;
	}

	if (LOG_PRI(prio) > loglevel)
		goto done;

	if (in_container() || !(fp = fopen("/dev/kmsg", "w"))) {
		vfprintf(stderr, fmt, ap);
		fputs("\n", stderr);
		goto done;
	}

	fprintf(fp, "<%d>finit[1]:", LOG_DAEMON | prio);
	vfprintf(fp, fmt, ap);
	fclose(fp);

	if (debug) {
		va_end(ap);
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		fputs("\n", stderr);
	}

done:
	va_end(ap);
}

/*
 * Log to file, intended for debug only.
 */
void flog(char *file, const char *fmt, ...)
{
        char fn[80];
        va_list ap;
        FILE *fp;

        snprintf(fn, sizeof(fn), "/tmp/%s.log", file);
        fp = fopen(fn, "a");
        if (!fp)
                return;

        va_start(ap, fmt);
        vfprintf(fp, fmt, ap);
        va_end(ap);

        fclose(fp);
}


/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
