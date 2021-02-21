/* New client tool, replaces old /dev/initctl API and telinit tool
 *
 * Copyright (c) 2015-2021  Joachim Wiberg <troglobit@gmail.com>
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

#include <err.h>
#include <ftw.h>
#include <ctype.h>
#include <getopt.h>
#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <utmp.h>
#include <arpa/inet.h>
#include <lite/lite.h>

#include "client.h"
#include "cond.h"
#include "serv.h"
#include "service.h"
#include "util.h"

#define _PATH_COND _PATH_VARRUN "finit/cond/"

struct command {
	char  *cmd;
	int  (*cb)(char *arg);
};

int heading  = 1;
int numeric  = 0;
int verbose  = 0;
int runlevel = 0;
int iw, pw;

extern int reboot_main(int argc, char *argv[]);


/* figure ut width of IDENT and PID columns */
static void col_widths(void)
{
	char ident[MAX_IDENT_LEN];
	char pid[10];
	svc_t *svc;

	iw = 0;
	pw = 0;

	for (svc = client_svc_iterator(1); svc; svc = client_svc_iterator(0)) {
		int w, p;

		if (!numeric)
			svc_ident(svc, ident, sizeof(ident));
		else
			svc_jobid(svc, ident, sizeof(ident));

		w = (int)strlen(ident);
		if (w > iw)
			iw = w;

		p = snprintf(pid, sizeof(pid), "%d", svc->pid);
		if (p > pw)
			pw = p;
	}

	/* adjust for min col width */
	if (iw < 5)
		iw = 5;
	if (pw < 3)
		pw = 3;
}

static int runlevel_get(int *prevlevel)
{
	int result;
	struct init_request rq;

	memset(&rq, 0, sizeof(rq));
	rq.cmd = INIT_CMD_GET_RUNLEVEL;
	rq.magic = INIT_MAGIC;

	result = client_send(&rq, sizeof(rq));
	if (!result) {
		result = rq.runlevel;
		if (prevlevel)
			*prevlevel = rq.sleeptime;
	}

	return result;
}

static int toggle_debug(char *arg)
{
	struct init_request rq = {
		.magic = INIT_MAGIC,
		.cmd = INIT_CMD_DEBUG,
	};

	return client_send(&rq, sizeof(rq));
}

static int do_log(char *svc)
{
	char cmd[128];
	char *logfile = "/var/log/syslog";

	if (!svc || !svc[0])
		svc = "finit";

	if (!fexist(logfile))
		logfile = "/var/log/messages";

	snprintf(cmd, sizeof(cmd), "cat %s | grep %s | tail -10", logfile, svc);

	return system(cmd);
}

static int do_runlevel(char *arg)
{
	struct init_request rq = {
		.magic = INIT_MAGIC,
		.cmd = INIT_CMD_RUNLVL,
		.runlevel = (int)arg[0],
	};

	if (!rq.runlevel) {
		int prevlevel = 0;
		int runlevel;
		char prev;

		runlevel = runlevel_get(&prevlevel);
		if (255 == runlevel) {
			printf("unknown\n");
			return 0;
		}

		prev = prevlevel + '0';
		if (prev <= '0' || prev > '9')
			prev = 'N';

		printf("%c %d\n", prev , runlevel);
		return 0;
	}

	return client_send(&rq, sizeof(rq));
}

static int do_svc(int cmd, char *arg)
{
	struct init_request rq = {
		.magic = INIT_MAGIC,
		.cmd = cmd,
	};

	strlcpy(rq.data, arg, sizeof(rq.data));

	return client_send(&rq, sizeof(rq));
}

static int do_reload (char *arg) { return do_svc(INIT_CMD_RELOAD,      arg); }

/*
 * This is a wrapper for do_svc() that adds a simple sanity check of
 * the service(s) provided as argument.  If a service does not exist
 * we make sure to return an error code.
 */
static int do_startstop(int cmd, char *arg)
{
	struct init_request rq = {
		.magic = INIT_MAGIC,
		.cmd   = INIT_CMD_SVC_QUERY
	};

	strlcpy(rq.data, arg, sizeof(rq.data));
	if (client_send(&rq, sizeof(rq))) {
		fprintf(stderr, "No such job(s) or service(s): %s\n\n", rq.data);
		fprintf(stderr, "Usage: initctl %s <JOB|NAME>[:ID]\n",
			cmd == INIT_CMD_START_SVC ? "start" :
			(cmd == INIT_CMD_STOP_SVC ? "stop"  : "restart"));
		return 1;
	}

	return do_svc(cmd, arg);
}

static int do_start  (char *arg) { return do_startstop(INIT_CMD_START_SVC,   arg); }
static int do_stop   (char *arg) { return do_startstop(INIT_CMD_STOP_SVC,    arg); }
static int do_restart(char *arg) { return do_startstop(INIT_CMD_RESTART_SVC, arg); }

static int dump_one_cond(const char *fpath, const struct stat *sb, int tflag, struct FTW *ftwbuf)
{
	const char *cond, *asserted;
	char *nm = "init";
	pid_t pid = 1;

	if (tflag != FTW_F)
		return 0;

	if (!strcmp(fpath, _PATH_COND "reconf"))
		return 0;

	asserted = condstr(cond_get_path(fpath));
	cond = &fpath[strlen(_PATH_COND)];
	if (strncmp("pid/", cond, 4) == 0) {
		svc_t *svc;

		svc= client_svc_find_by_cond(cond);
		if (!svc) {
			nm  = "unknown";
			pid = 0;
		} else {
			nm  = svc_ident(svc, NULL, 0);
			pid = svc->pid;
		}
	}

	printf("%-*d  %-*s  %-6s  %s\n", pw, pid, iw, nm, asserted, cond);

	return 0;
}

static int do_cond_dump(char *arg)
{
	col_widths();
	if (heading) {
		char title[80];

		snprintf(title, sizeof(title), "%-*s  %-*s  %-6s  %s", pw, "PID",
			 iw, "IDENT", "STATUS", "CONDITION");
		printheader(NULL, title, 0);
	}

	if (nftw(_PATH_COND, dump_one_cond, 20, 0) == -1) {
		warnx("Failed parsing %s", _PATH_COND);
		return 1;
	}

	return 0;
}

static void show_cond_one(const char *_conds)
{
	static char conds[MAX_COND_LEN];
	char *cond;

	strlcpy(conds, _conds, sizeof(conds));

	putchar('<');

	for (cond = strtok(conds, ","); cond; cond = strtok(NULL, ",")) {
		if (cond != conds)
			putchar(',');

		switch (cond_get(cond)) {
		case COND_ON:
			printf("+%s", cond);
			break;

		case COND_FLUX:
			printf("\e[1m~%s\e[0m", cond);
			break;

		case COND_OFF:
			printf("\e[1m-%s\e[0m", cond);
			break;
		}
	}

	putchar('>');
}

static int do_cond_show(char *arg)
{
	char ident[MAX_IDENT_LEN];
	enum cond_state cond;
	svc_t *svc;

	col_widths();
	if (heading) {
		char title[80];

		snprintf(title, sizeof(title), "%-*s  %-*s  %-6s  %s", pw, "PID",
			 iw, "IDENT", "STATUS", "CONDITION (+ ON, ~ FLUX, - OFF)");
		printheader(NULL, title, 0);
	}

	for (svc = client_svc_iterator(1); svc; svc = client_svc_iterator(0)) {
		if (!svc->cond[0])
			continue;

		cond = cond_get_agg(svc->cond);

		if (!numeric)
			svc_ident(svc, ident, sizeof(ident));
		else
			svc_jobid(svc, ident, sizeof(ident));

		printf("%-*d  %-*s  ", pw, svc->pid, iw, ident);

		if (cond == COND_ON)
			printf("%-6.6s  ", condstr(cond));
		else
			printf("\e[1m%-6.6s\e[0m  ", condstr(cond));

		show_cond_one(svc->cond);
		puts("");
	}

	return 0;
}

static int do_cond(char *cmd)
{
	int c;
	char *arg;
	struct command command[] = {
		{ "show",    do_cond_show  },
		{ "dump",    do_cond_dump  },
		{ NULL, NULL }
	};

	arg = strpbrk(cmd, " \0");
	if (arg)
		*arg++ = 0;

	for (c = 0; command[c].cmd; c++) {
		if (string_match(command[c].cmd, cmd))
			return command[c].cb(arg);
	}

	return do_cond_show(NULL);
}

static int do_cmd(int cmd)
{
	struct init_request rq = {
		.magic = INIT_MAGIC,
		.cmd = cmd,
	};

	if (client_send(&rq, sizeof(rq))) {
		if (rq.cmd == INIT_CMD_NACK)
			puts(rq.data);

		return 1;
	}

	/* Wait here for systemd to shutdown/reboot */
	sleep(5);

	return 0;
}

int do_reboot  (char *arg) { return do_cmd(INIT_CMD_REBOOT);   }
int do_halt    (char *arg) { return do_cmd(INIT_CMD_HALT);     }
int do_poweroff(char *arg) { return do_cmd(INIT_CMD_POWEROFF); }
int do_suspend (char *arg) { return do_cmd(INIT_CMD_SUSPEND);  }

int utmp_show(char *file)
{
	time_t sec;
	struct utmp *ut;
	struct tm *sectm;

	int pid;
	char id[sizeof(ut->ut_id) + 1], user[sizeof(ut->ut_user) + 1], when[80];

	if (heading)
		printheader(NULL, file, 0);
	utmpname(file);

	setutent();
	while ((ut = getutent())) {
		char addr[64];

		memset(id, 0, sizeof(id));
		strlcpy(id, ut->ut_id, sizeof(id));

		memset(user, 0, sizeof(user));
		strlcpy(user, ut->ut_user, sizeof(user));

		sec = ut->ut_tv.tv_sec;
		sectm = localtime(&sec);
		strftime(when, sizeof(when), "%F %T", sectm);

		pid = ut->ut_pid;
		if (ut->ut_addr_v6[1] || ut->ut_addr_v6[2] || ut->ut_addr_v6[3])
			inet_ntop(AF_INET6, ut->ut_addr_v6, addr, sizeof(addr));
		else
			inet_ntop(AF_INET, &ut->ut_addr, addr, sizeof(addr));

		printf("[%d] [%05d] [%-4.4s] [%-8.8s] [%-12.12s] [%-20.20s] [%-15.15s] [%-19.19s]\n",
		       ut->ut_type, pid, id, user, ut->ut_line, ut->ut_host, addr, when);
	}
	endutent();

	return 0;
}

static int do_utmp(char *file)
{
	if (!has_utmp())
		return 1;

	if (fexist(file))
		return utmp_show(file);

	return  utmp_show(_PATH_WTMP) ||
		utmp_show(_PATH_UTMP);
}

static int show_version(char *arg)
{
	puts("v" PACKAGE_VERSION);
	return 0;
}

/**
 * runlevel_string - Convert a bit encoded runlevel to .conf syntax
 * @levels: Bit encoded runlevels
 *
 * Returns:
 * Pointer to string on the form [2345], may be up to 12 chars long,
 * plus escape sequences for highlighting current runlevel.
 */
char *runlevel_string(int runlevel, int levels)
{
	int i, pos = 1;
	static char lvl[21];

	memset(lvl, 0, sizeof(lvl));
	lvl[0] = '[';

	for (i = 0; i < 10; i++) {
		if (ISSET(levels, i)) {
			if (runlevel == i)
				pos = strlcat(lvl, "\e[1m", sizeof(lvl));

			if (i == 0)
				lvl[pos++] = 'S';
			else
				lvl[pos++] = '0' + i;

			if (runlevel == i)
				pos = strlcat(lvl, "\e[0m", sizeof(lvl));
		} else {
			lvl[pos++] = '-';
		}
	}

	lvl[pos++] = ']';
	lvl[pos]   = 0;

	return lvl;
}

static int show_status(char *arg)
{
	char ident[MAX_IDENT_LEN];
	svc_t *svc;

	runlevel = runlevel_get(NULL);

	if (arg && arg[0]) {
		long now = jiffies();
		char buf[42] = "N/A";

		svc = client_svc_find(arg);
		if (!svc)
			return 1;

		printf("Job         : %d\n", svc->job);
		printf("Service     : %s\n", svc->cmd);
		printf("Description : %s\n", svc->desc);
		printf("Identity    : %s\n", svc_ident(svc, ident, sizeof(ident)));
		printf("PID         : %d\n", svc->pid);
		printf("Uptime      : %s\n", svc->pid ? uptime(now - svc->start_time, buf, sizeof(buf)) : buf);
		printf("Runlevels   : %s\n", runlevel_string(runlevel, svc->runlevels));
		printf("Status      : %s\n", svc_status(svc));
		printf("\n");

		return do_log(svc->cmd);
	}

	col_widths();
	if (heading) {
		char title[80];

		snprintf(title, sizeof(title), "%-*s  %-*s  %-8s %-12s ",
			 pw, "PID", iw, "IDENT", "STATUS", "RUNLEVELS");
		if (!verbose)
			strlcat(title, "DESCRIPTION", sizeof(title)); 
		else
			strlcat(title, "COMMAND", sizeof(title)); 

		printheader(NULL, title, 0);
	}

	for (svc = client_svc_iterator(1); svc; svc = client_svc_iterator(0)) {
		char *lvls;
		int i;

		printf("%-*d  ", pw, svc->pid);

		if (!numeric)
			svc_ident(svc, ident, sizeof(ident));
		else
			svc_jobid(svc, ident, sizeof(ident));

		printf("%-*s  %-8s ", iw, ident, svc_status(svc));

		lvls = runlevel_string(runlevel, svc->runlevels);
		if (strchr(lvls, '\e'))
			printf("%-20.20s ", lvls);
		else
			printf("%-12.12s ", lvls);

		if (!verbose) {
			printf("%s\n", svc->desc);
		} else {
			char args[CMD_SIZE] = "";

			for (i = 1; i < MAX_NUM_SVC_ARGS; i++) {
				if (strlen(svc->args[i]) == 0)
					continue;
				if (args[0])
					strlcat(args, " ", sizeof(args));
				strlcat(args, svc->args[i], sizeof(args));
			}

			printf("%s %s\n", svc->cmd, args);
		}
	}

	return 0;
}

static int dump_cgroup(const char *fpath, const struct stat *sb, int tflag, struct FTW *ftwbuf)
{
	FILE *fp;
	char *group;
	char path[256];
	char buf[256];
	int user = 0;
	int num = 0;

	if (tflag == FTW_F)
		return 0;

	snprintf(path, sizeof(path), "%s/cgroup.procs", fpath);
	fp = fopen(path, "r");
	if (!fp)
		return 0;

	if (strstr(fpath, "finit/user"))
		user = 1;

	group = strrchr(fpath, '/');
	if (!group)
		goto err;
	group++;

	while (fgets(buf, sizeof(buf), fp)) {
		FILE *cfp;
		int pid;

		if (num == 0)
			printf("%c  :- %s/", user ? ' ' : '|', group);
		num++;

		pid = atoi(chomp(buf));
		snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
		cfp = fopen(path, "r");
		if (!cfp)
			continue;
		fgets(buf, sizeof(buf), cfp);
		fclose(cfp);

		printf("\n%c      :- %d %s", user ? ' ' : '|', pid, buf);
	}

err:
	fclose(fp);
	if (num)
		printf("\r%c\n", user ? ' ' : '|');

	return 0;
}

static int show_cgroup(char *arg)
{
	puts("finit/");
	puts("|- init/");
	nftw("/sys/fs/cgroup/finit/init", dump_cgroup, 20, 0);
	puts("|- system/");
	nftw("/sys/fs/cgroup/finit/system", dump_cgroup, 20, 0);
	puts("`- user/");
	nftw("/sys/fs/cgroup/finit/user", dump_cgroup, 20, 0);

	return 0;
}

static int transform(char *nm)
{
	char *names[] = {
		"reboot", "shutdown", "poweroff", "halt", "suspend",
		NULL
	};
	size_t i;

	for (i = 0; names[i]; i++) {
		if (!strcmp(nm, names[i]))
			return 1;
	}

	return 0;
}

static int usage(int rc)
{
	fprintf(stderr,
		"Usage: %s [OPTIONS] [COMMAND]\n"
		"\n"
		"Options:\n"
		"  -b, --batch               Batch mode, no screen size probing\n"
		"  -n, --numeric             Show JOB:ID instead of NAME:ID\n"
		"  -t, --no-heading          Skip table headings\n"
		"  -v, --verbose             Verbose output\n"
		"  -h, --help                This help text\n"
		"\n"
		"Commands:\n"
		"  debug                     Toggle Finit (daemon) debug\n"
		"  help                      This help text\n"
		"  version                   Show Finit version\n"
		"\n"
		"  ls|list                   List all .conf in /etc/finit.d/\n"
		"  enable   <CONF>           Enable   .conf in /etc/finit.d/available/\n"
		"  disable  <CONF>           Disable  .conf in /etc/finit.d/[enabled/]\n"
		"  touch    <CONF>           Mark     .conf in /etc/finit.d/ for reload\n"
		"  reload                    Reload  *.conf in /etc/finit.d/ (activates changes)\n"
//		"  reload   <JOB|NAME>[:ID]  Reload (SIGHUP) service by job# or name\n"
		"\n"
		"  cond     show             Show condition status\n"
		"  cond     dump             Dump all conditions and their status\n"
		"\n"
		"  log      [NAME]           Show ten last Finit, or NAME, messages from syslog\n"
		"  start    <JOB|NAME>[:ID]  Start service by job# or name, with optional ID\n"
		"  stop     <JOB|NAME>[:ID]  Stop/Pause a running service by job# or name\n"
		"  restart  <JOB|NAME>[:ID]  Restart (stop/start) service by job# or name\n"
		"  status   <JOB|NAME>[:ID]  Show service status, by job# or name\n"
		"  status | show             Show status of services, default command\n"
		"\n"
		"  ps                        List processes based on cgroups\n"
		"\n"
		"  runlevel [0-9]            Show or set runlevel: 0 halt, 6 reboot\n"
		"  reboot                    Reboot system\n"
		"  halt                      Halt system\n"
		"  poweroff                  Halt and power off system\n"
		"  suspend                   Suspend system\n", prognm);

	if (has_utmp())
		fprintf(stderr,
			"\n"
			"  utmp     show             Raw dump of UTMP/WTMP db\n"
			"\n");
	else
		fprintf(stderr, "\n");

	return rc;
}

static int do_help(char *arg)
{
	return usage(0);
}

int main(int argc, char *argv[])
{
	int interactive = 1, c;
	char *cmd, arg[120];
	struct command command[] = {
		{ "debug",    toggle_debug },
		{ "help",     do_help      },
		{ "version",  show_version },

		{ "list",     serv_list    },
		{ "ls",       serv_list    },
		{ "enable",   serv_enable  },
		{ "disable",  serv_disable },
		{ "touch",    serv_touch   },
		{ "reload",   do_reload    },

		{ "cond",     do_cond      },

		{ "log",      do_log       },
		{ "start",    do_start     },
		{ "stop",     do_stop      },
		{ "restart",  do_restart   },
		{ "status",   show_status  },
		{ "show",     show_status  }, /* Convenience alias */

		{ "ps",       show_cgroup  },

		{ "runlevel", do_runlevel  },
		{ "reboot",   do_reboot    },
		{ "halt",     do_halt      },
		{ "poweroff", do_poweroff  },
		{ "suspend",  do_suspend   },

		{ "utmp",     do_utmp      },
		{ NULL, NULL }
	};
	struct option long_options[] = {
		{ "batch",      0, NULL, 'b' },
		{ "help",       0, NULL, 'h' },
		{ "numeric",    0, NULL, 'n' },
		{ "no-heading", 0, NULL, 't' },
		{ "verbose",    0, NULL, 'v' },
		{ NULL, 0, NULL, 0 }
	};

	if (transform(progname(argv[0])))
		return reboot_main(argc, argv);

	while ((c = getopt_long(argc, argv, "bh?ntv", long_options, NULL)) != EOF) {
		switch(c) {
		case 'b':
			interactive = 0;
			break;

		case 'h':
		case '?':
			return usage(0);

		case 'n':
			numeric = 1;
			break;

		case 't':
			heading = 0;
			break;

		case 'v':
			verbose = 1;
			break;
		}
	}

	if (interactive)
		screen_init();

	if (optind >= argc)
		return show_status(NULL);

	if (!has_utmp()) {
		for (c = 0; command[c].cmd; c++) {
			if (!string_compare(command[c].cmd, "utmp"))
				continue;

			command[c].cb = NULL;
			break;
		}
	}

	memset(arg, 0, sizeof(arg));
	cmd = argv[optind++];
	while (optind < argc) {
		strlcat(arg, argv[optind++], sizeof(arg));
		if (optind < argc)
			strlcat(arg, " ", sizeof(arg));
	}

	for (c = 0; command[c].cmd; c++) {
		if (!string_match(command[c].cmd, cmd))
			continue;
		if (!command[c].cb)
			continue;

		return command[c].cb(arg);
	}

	return usage(1);
}

void logit(int prio, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (prio <= LOG_ERR)
		verrx(1, fmt, ap);
	else
		vwarnx(fmt, ap);
	va_end(ap);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
