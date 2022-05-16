/* List and enable/disable service configurations
 *
 * Copyright (c) 2017-2022  Joachim Wiberg <troglobit@gmail.com>
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

#include "config.h"

#include <fcntl.h> /* Definition of AT_* constants */
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _LIBITE_LITE
# include <libite/lite.h>
#else
# include <lite/lite.h>
#endif

#include "client.h"
#include "initctl.h"


static int is_builtin(char *arg)
{
	svc_t *svc;

	svc = client_svc_find(arg);
	if (!svc)
		return 0;

	if (svc->file[0])
		return 0;

	return 1;
}

static int calc_width(char *arr[], size_t len)
{
	int width = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		int w = strlen(arr[i]);

		if (w > width)
			width = w;
	}

	return width;
}

static void do_list(const char *path)
{
	static int once = 0;
	int width, num, prev;
	glob_t gl;
	size_t i;

	if (chdir(path)) {
		char *dir, *ptr;

		if (!fexist(path))
			return;
		dir = strdupa(path);
		if (!dir)
			return;

		ptr = strrchr(dir, '/');
		if (ptr)
			*ptr++ = 0;
		else
			ptr = dir;

		if (heading)
			print_header("%s%s ", once ? "\n" : "", dir);
		puts(ptr);
		puts("");
		return;
	}

	if (glob("*.conf", 0, NULL, &gl))
		return;

	if (gl.gl_pathc <= 0)
		goto done;

	if (plain) {
		if (heading)
			print_header("%s%s ", once ? "\n" : "", path);
		once++;

		for (i = 0; i < gl.gl_pathc; i++) {
			if (!heading) {
				char buf[512];

				paste(buf, sizeof(buf), path, gl.gl_pathv[i]);
				puts(buf);
			} else
				puts(gl.gl_pathv[i]);
		}

		goto done;
	}

	if (heading)
		print_header("%s ", path);

	width = calc_width(gl.gl_pathv, gl.gl_pathc);
	if (width <= 0)
		goto done;

	num = (ttcols - 2) / width;
	if ((num - 1) * 2 + num * width > ttcols)
		num--;

	prev = 0;
	for (i = 0; i < gl.gl_pathc; i++) {
		if (i > 0 && !(i % num)) {
			puts("");
			prev = 0;
		}

		if (prev)
			printf("  ");
		printf("%-*s", width, gl.gl_pathv[i]);
		prev++;
	}
	puts("\n");

done:
	globfree(&gl);
}

int serv_list(char *arg)
{
	char path[256];

	if (arg && arg[0]) {
		paste(path, sizeof(path), FINIT_RCSD, arg);
		if (fisdir(path)) {
			do_list(path);
			return 0;
		}
		/* fall back to list all */
	}

	paste(path, sizeof(path), FINIT_RCSD, "available");
	if (fisdir(path))
		do_list(path);

	paste(path, sizeof(path), FINIT_RCSD, "enabled");
	if (fisdir(path))
		do_list(path);

	if (fisdir(FINIT_RCSD))
		do_list(FINIT_RCSD);

	if (fexist(FINIT_CONF))
		do_list(FINIT_CONF);

	return 0;
}

/*
 * Return path to configuration file for 'name'.  This may be any of the
 * following, provided sysconfdir is /etc:
 *
 *   - /etc/finit.d/available/$name.conf
 *   - /etc/finit.d/$name.conf
 *   - /etc/finit.conf
 *
 * The system *may* have a /etc/finit.d/available/ directory, or it may
 * just use a plain /etc/finit.d/ -- we do not set policy.
 *
 * If the resulting file doesn't exist, and creat is not set, *or*
 * the base directory doesn't exist, we return NULL.
.*/
static char *conf(char *path, size_t len, char *name, int creat)
{
	char corr[40];

	if (!name || !name[0] || !strcmp(name, "finit") || !strcmp(name, "finit.conf")) {
		strlcpy(path, FINIT_CONF, len);
		return path;
	}

	if (!strstr(name, ".conf")) {
		snprintf(corr, sizeof(corr), "%s.conf", name);
		name = corr;
	}

	if (!fisdir(FINIT_RCSD))
		return NULL;

	paste(path, len, FINIT_RCSD, "available/");
	if (!fisdir(path)) {
		if (creat && mkdir(path, 0755) && errno != EEXIST)
			return NULL;
		else
			paste(path, len, FINIT_RCSD, name);
	} else
		strlcat(path, name, len);

	/* fall back to static service unless edit/create */
	if (!creat && !fexist(path))
		paste(path, len, FINIT_RCSD, name);

	return path;
}

int serv_enable(char *arg)
{
	char corr[40];
	char path[256];
	int ena;

	if (!arg || !arg[0]) {
		WARNX("missing argument to enable, may be one of:");
		return serv_list("available");
	}

	if (!strstr(arg, ".conf")) {
		snprintf(corr, sizeof(corr), "%s.conf", arg);
		arg = corr;
	}

	if (chdir(FINIT_RCSD))
		ERR(72, "failed cd %s", FINIT_RCSD);

	if (icreate && mkdir("enabled", 0755) && EEXIST != errno)
		ERR(73, "failed creating %s/enabled directory", FINIT_RCSD);
	ena = !chdir("enabled");   /* System *may* have enabled/ dir. */

	snprintf(path, sizeof(path), "%savailable/%s", ena ? "../" : "", arg);
	if (!fexist(path))
		ERRX(72, "cannot find %s", conf(path, sizeof(path), arg, 0));

	if (fexist(arg))
		ERRX(1, "%s already enabled", arg);

	return symlink(path, arg) != 0;
}

int do_disable(char *arg, int check)
{
	struct stat st;
	char corr[40];

	if (!arg || !arg[0]) {
		WARNX("missing argument to disable, may be one of:");
		return serv_list("enabled");
	}

	if (!strstr(arg, ".conf")) {
		snprintf(corr, sizeof(corr), "%s.conf", arg);
		arg = corr;
	}

	if (chdir(FINIT_RCSD))
		ERR(72, "failed cd %s", FINIT_RCSD);
	if (chdir("enabled"))	   /* System *may* have enabled/ dir. */
		dbg("Failed changing to %s/enabled/: %s", FINIT_RCSD, strerror(errno));

	if (check && stat(arg, &st))
		ERRX(6, "%s not (an) enabled (service).", arg);

	if (check && (st.st_mode & S_IFMT) == S_IFLNK)
		ERRX(1, "cannot disable %s, not a symlink.", arg);

	return remove(arg) != 0;
}

int serv_disable(char *arg)
{
	return do_disable(arg, 1);
}

int serv_touch(char *arg)
{
	char path[256];
	char *fn;

	if (!arg || !arg[0]) {
		WARNX("missing argument to touch, may be one of:");
		return serv_list("enabled");
	}

	fn = conf(path, sizeof(path), arg, 0);
	if (!fexist(fn)) {
		if (!strstr(arg, "finit.conf"))
			ERRX(72, "%s not available.", arg);
		if (is_builtin(arg))
			ERRX(4, "%s is a built-in service.", arg);

		strlcpy(path, FINIT_CONF, sizeof(path));
		fn = path;
	}

	/* libite:touch() follows symlinks */
	if (utimensat(AT_FDCWD, fn, NULL, AT_SYMLINK_NOFOLLOW))
		ERR(71, "failed marking %s for reload", fn);

	return 0;
}

int serv_show(char *arg)
{
	char path[256];
	char *fn;

	fn = conf(path, sizeof(path), arg, 0);
	if (!fexist(fn)) {
		if (is_builtin(arg))
			ERRX(4, "%s is a built-in service.", arg);

		WARNX("Cannot find %s", arg);
		return 1;
	}

	return systemf("cat %s", fn);
}

/*
 * Try to open an editor for the given file, if creat is given we
 * create a new file based on /lib/finit/sample.conf
 *
 * The order of editors that this command checks for is evaluated
 * as follows, in order:
 *
 *     sensible-editor :: debian based systems
 *     editor          :: debian + most other systems
 *     VISUAL          :: full-screen editor program
 *     EDITOR          :: line-mode editor program
 *     $(command ...)  :: fallback to mg or vi
 *
 * For details: https://jdebp.uk/FGA/unix-editors-and-pagers.html
 */
static int do_edit(char *arg, int creat)
{
	char *editor[] = {
		"sensible-editor",
		"editor",
		"${VISUAL:-${EDITOR:-$(command -v mg || command -v vi)}}"
	};
	char path[256];
	char *fn;

	fn = conf(path, sizeof(path), arg, creat);
	if (!fexist(fn)) {
		if (is_builtin(arg))
			ERRX(4, "%s is a built-in service.", arg);

		if (!creat) {
			WARNX("Cannot find %s, use -c flag, create command, or select one of:", arg);
			return serv_list(NULL);
		}

#ifdef SAMPLE_CONF
		/* Try copying file, it may have been removed by sysop */
		copyfile(SAMPLE_CONF, fn, 0, 0);
#endif
	} else if (creat)
		WARNX("the file %s already exists, falling back to edit.", fn);

	for (size_t i = 0; i < NELEMS(editor); i++) {
		if (systemf("%s %s 2>/dev/null", editor[i], path))
			continue;
		return 0;
	}

	return 1;
}

int serv_edit(char *arg)
{
	if (!arg || !arg[0]) {
		if (!yorn("Do you want to edit %s (y/N)? ", FINIT_CONF))
			return 0;
		arg = "";
	}

	return do_edit(arg, icreate);
}

int serv_creat(char *arg)
{
	char buf[256];
	char *fn;
	FILE *fp;

	if (!arg || !arg[0])
		ERRX(2, "missing argument to create");

	if (is_builtin(arg))
		ERRX(4, "%s is a built-in service.", arg);

	/* Input from a pipe or a proper TTY? */
	if (isatty(STDIN_FILENO))
		return do_edit(arg, 1);

	/* Open fn for writing from pipe */
	fn = conf(buf, sizeof(buf), arg, 1);
	if (!fn)
		ERR(73, "failed creating conf %s", arg);

	if (!icreate && fexist(fn)) {
		WARNX("%s already exists, skipping (use -c to override)", fn);
		fn = "/dev/null";
	}

	fp = fopen(fn, "w");
	if (!fp)
		ERR(73, "failed opening %s for writing", fn);

	while (fgets(buf, sizeof(buf), stdin))
		fputs(buf, fp);

	return fclose(fp);
}

int serv_delete(char *arg)
{
	char buf[256];
	char *fn;

	if (!arg || !arg[0]) {
		WARNX("missing argument to delete, may be one of:");
		return serv_list("available");
	}

	fn = conf(buf, sizeof(buf), arg, 0);
	if (!fn) {
		if (is_builtin(arg))
			ERRX(4, "%s is a built-in service.", arg);
		ERRX(72, FINIT_RCSD " missing on system.");
	}

	if (!fexist(fn))
		WARNX("cannot find %s", fn);

	if (iforce || yorn("Remove file and symlink(s) to %s (y/N)? ", fn)) {
		do_disable(arg, 0);
		if (remove(fn))
			ERR(1, "Failed removing %s", fn);
	}

	return 0;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
