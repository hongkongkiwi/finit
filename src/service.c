/* Finit service monitor, task starter and generic API for managing svc_t
 *
 * Copyright (c) 2008-2010  Claudio Matsuoka <cmatsuoka@gmail.com>
 * Copyright (c) 2008-2015  Joachim Nilsson <troglobit@gmail.com>
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

#include <ctype.h>		/* isblank() */
#include <sched.h>		/* sched_yield() */
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <net/if.h>
#include <lite/lite.h>

#include "conf.h"
#include "cond.h"
#include "finit.h"
#include "helpers.h"
#include "inetd.h"
#include "pid.h"
#include "private.h"
#include "sig.h"
#include "service.h"
#include "sm.h"
#include "tty.h"
#include "util.h"
#include "utmp-api.h"
#include "schedule.h"

#define RESPAWN_MAX    10	/* Prevent endless respawn of faulty services. */

static struct wq work = {
	.cb = service_worker,
};

static void svc_set_state(svc_t *svc, svc_state_t new);

/**
 * service_timeout_cb - libuev callback wrapper for service timeouts
 * @w:      Watcher
 * @arg:    Callback argument, from init
 * @events: Error, or ready to read/write (N/A for relative timers)
 *
 * Run callback registered when calling service_timeout_after().
 */
static void service_timeout_cb(uev_t *w, void *arg, int events)
{
	svc_t *svc = arg;

	/* Ignore any UEV_ERROR, we're a one-shot cb so just run it. */
	if (svc->timer_cb)
		svc->timer_cb(svc);
}

/**
 * service_timeout_after - Call a function after some time has elapsed
 * @svc:     Service to use as argument to the callback
 * @timeout: Timeout, in milliseconds
 * @cb:      Callback function
 *
 * After @timeout milliseconds has elapsed, call @cb() with @svc as the
 * argument.
 *
 * Returns:
 * POSIX OK(0) on success, non-zero on error.
 */
static int service_timeout_after(svc_t *svc, int timeout, void (*cb)(svc_t *svc))
{
	if (svc->timer_cb)
		return -EBUSY;

	svc->timer_cb = cb;
	return uev_timer_init(ctx, &svc->timer, service_timeout_cb, svc, timeout, 0);
}

/**
 * service_timeout_cancel - Cancel timeout associated with service
 * @svc: Service whose timeout to cancel
 *
 * If a timeout is associated with @svc, cancel it.
 *
 * Returns:
 * POSIX OK(0) on success, non-zero on error.
 */
static int service_timeout_cancel(svc_t *svc)
{
	int err;

	if (!svc->timer_cb)
		return 0;

	err = uev_timer_stop(&svc->timer);
	svc->timer_cb = NULL;

	return err;
}

static void redirect_null(void)
{
	FILE *fp;

	fp = fopen("/dev/null", "w");
	if (fp) {
		dup2(fileno(fp), STDOUT_FILENO);
		dup2(fileno(fp), STDERR_FILENO);
		fclose(fp);
	}
}

static int is_norespawn(void)
{
	return  sig_stopped()            ||
		fexist("/mnt/norespawn") ||
		fexist("/tmp/norespawn");
}

/**
 * service_start - Start service
 * @svc: Service to start
 *
 * Returns:
 * 0 if the service was successfully started. Non-zero otherwise.
 */
static int service_start(svc_t *svc)
{
	int i, result = 0, do_progress = 1;
	pid_t pid;
	sigset_t nmask, omask;

	if (!svc)
		return 1;

	/* Ignore if finit is SIGSTOP'ed */
	if (is_norespawn())
		return 1;

	/* Don't try and start service if it doesn't exist. */
	if (!whichp(svc->cmd) && !svc->inetd.cmd) {
		print(1, "Service %s does not exist!", svc->cmd);
		svc_missing(svc);
		return 1;
	}

#ifdef INETD_ENABLED
	if (svc_is_inetd(svc))
		return inetd_start(&svc->inetd);
#endif

	if (!svc->desc[0])
		do_progress = 0;

	if (do_progress) {
		if (svc_is_daemon(svc))
			print_desc("Starting ", svc->desc);
		else
			print_desc("", svc->desc);
	}

	/* Declare we're waiting for svc to create its pidfile */
	svc_starting(svc);

	/* Block SIGCHLD while forking.  */
	sigemptyset(&nmask);
	sigaddset(&nmask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &nmask, &omask);

	pid = fork();
	if (pid == 0) {
		int status;
		char *home = NULL;
#ifdef ENABLE_STATIC
		int uid = 0; /* XXX: Fix better warning that dropprivs is disabled. */
		int gid = 0;
#else
		int uid = getuser(svc->username, &home);
		int gid = getgroup(svc->group);
#endif
		char *args[MAX_NUM_SVC_ARGS];

		/* Set configured limits */
		for (int i = 0; i < RLIMIT_NLIMITS; i++) {
			if (setrlimit(i, &svc->rlimit[i]) == -1)
				logit(LOG_WARNING,
				      "%s: rlimit: Failed setting %s",
				      svc->cmd, rlim2str(i));
		}

		/* Set desired user+group */
		if (gid >= 0)
			setgid(gid);

		if (uid >= 0) {
			setuid(uid);

			/* Set default path for regular users */
			if (uid > 0)
				setenv("PATH", _PATH_DEFPATH, 1);
			if (home) {
				setenv("HOME", home, 1);
				chdir(home);
			}
		}

		/* Serve copy of args to process in case it modifies them. */
		for (i = 0; i < (MAX_NUM_SVC_ARGS - 1) && svc->args[i][0] != 0; i++)
			args[i] = svc->args[i];
		args[i] = NULL;

		/* Redirect inetd socket to stdin for connection */
#ifdef INETD_ENABLED
		if (svc_is_inetd_conn(svc)) {
			dup2(svc->stdin_fd, STDIN_FILENO);
			close(svc->stdin_fd);
			dup2(STDIN_FILENO, STDOUT_FILENO);
			dup2(STDIN_FILENO, STDERR_FILENO);
		} else
#endif

		if (svc->log.enabled) {
			int fd;

			if (svc->log.null) {
				redirect_null();
				goto logit_done;
			}
			if (svc->log.console) {
				goto logit_done;
			}

			/*
			 * Open PTY to connect to logger.  A pty isn't buffered
			 * like a pipe, and it eats newlines so they aren't logged
			 */
			fd = posix_openpt(O_RDWR);
			if (fd == -1) {
				svc->log.enabled = 0;
				goto logit_done;
			}
			if (grantpt(fd) == -1 || unlockpt(fd) == -1) {
				close(fd);
				svc->log.enabled = 0;
				goto logit_done;
			}

			/* SIGCHLD is still blocked for grantpt() and fork() */
			sigprocmask(SIG_BLOCK, &nmask, NULL);
			pid = fork();
			if (pid == 0) {
				int fds;
				char *tag  = basename(svc->cmd);
				char *prio = "daemon.info";

				fds = open(ptsname(fd), O_RDONLY);
				close(fd);
				if (fds == -1)
					_exit(0);
				dup2(fds, STDIN_FILENO);

				/* Reset signals */
				sig_unblock();

				if (svc->log.file[0] == '/') {
					char sz[20], num[3];

					snprintf(sz, sizeof(sz), "%d", logfile_size_max);
					snprintf(num, sizeof(num), "%d", logfile_count_max);

					execlp("logit", "logit", "-f", svc->log.file, "-n", sz, "-r", num, NULL);
					_exit(0);
				}

				if (svc->log.ident[0])
					tag = svc->log.ident;
				if (svc->log.prio[0])
					prio = svc->log.prio;

				execlp("logit", "logit", "-t", tag, "-p", prio, NULL);
				_exit(0);
			}

			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		} else if (log_is_debug()) {
			int fd;

			fd = open(CONSOLE, O_WRONLY | O_APPEND);
			if (-1 != fd) {
				dup2(fd, STDOUT_FILENO);
				dup2(fd, STDERR_FILENO);
				close(fd);
			}
		}
#ifdef REDIRECT_OUTPUT
		else
			redirect_null();
#endif

	logit_done:
		sig_unblock();

		if (svc->inetd.cmd)
			status = svc->inetd.cmd(svc->inetd.type);
		else if (svc_is_runtask(svc))
			status = exec_runtask(svc->cmd, args);
		else
			status = execv(svc->cmd, args);

#ifdef INETD_ENABLED
		if (svc_is_inetd_conn(svc)) {
			if (svc->inetd.type == SOCK_STREAM) {
				close(STDIN_FILENO);
				close(STDOUT_FILENO);
				close(STDERR_FILENO);
			}
		} else
#endif
		if (svc->log.enabled && !svc->log.null)
			waitpid(pid, NULL, 0);
		_exit(status);
	} else if (log_is_debug()) {
		char buf[CMD_SIZE] = "";

		for (i = 0; i < (MAX_NUM_SVC_ARGS - 1) && svc->args[i][0] != 0; i++) {
			char arg[MAX_ARG_LEN];

			snprintf(arg, sizeof(arg), "%s ", svc->args[i]);
			if (strlen(arg) < (sizeof(buf) - strlen(buf)))
				strcat(buf, arg);
		}
		_d("Starting %s: %s", svc->cmd, buf);
	}

	svc->pid = pid;
	svc->start_time = jiffies();

	switch (svc->type) {
	case SVC_TYPE_RUN:
		result = WEXITSTATUS(complete(svc->cmd, pid));
		svc->start_time = svc->pid = 0;
		svc->once++;
		svc_set_state(svc, SVC_STOPPING_STATE);
		break;

	case SVC_TYPE_SERVICE:
		pid_file_create(svc);
		break;

#ifdef INETD_ENABLED
	case SVC_TYPE_INETD_CONN:
		if (svc->inetd.type == SOCK_STREAM)
			close(svc->stdin_fd);
		break;
#endif

	default:
		break;
	}

	sigprocmask(SIG_SETMASK, &omask, NULL);
	if (do_progress)
		print_result(result);

	return result;
}

/**
 * service_kill - Forcefully terminate a service
 * @param svc  Service to kill
 *
 * Called when a service refuses to terminate gracefully.
 */
static void service_kill(svc_t *svc)
{
	service_timeout_cancel(svc);

	if (svc->pid <= 1) {
		/* Avoid killing ourselves or all processes ... */
		_d("%s: Aborting SIGKILL, already terminated.", svc->cmd);
		return;
	}

	_d("%s: Sending SIGKILL to pid:%d", pid_get_name(svc->pid, NULL, 0), svc->pid);
	if (runlevel != 1)
		print_desc("Killing ", svc->desc);

	kill(svc->pid, SIGKILL);

	/* Let SIGKILLs stand out, show result as [WARN] */
	if (runlevel != 1)
		print(2, NULL);
}

/**
 * service_stop - Stop service
 * @svc: Service to stop
 *
 * Returns:
 * 0 if the service was successfully stopped. Non-zero otherwise.
 */
static int service_stop(svc_t *svc)
{
	int res = 0;

	if (!svc)
		return 1;

	if (svc->state <= SVC_STOPPING_STATE)
		return 0;

#ifdef INETD_ENABLED
	if (svc_is_inetd(svc)) {
		int do_progress = runlevel != 1 && !svc_is_busy(svc);

		if (do_progress)
			print_desc("Stopping ", svc->desc);

		inetd_stop(&svc->inetd);

		if (do_progress)
			print_result(0);

		svc_set_state(svc, SVC_STOPPING_STATE);
		return 0;
	} else
#endif
	service_timeout_cancel(svc);

	if (svc->pid <= 1)
		return 1;

	_d("Sending SIGTERM to pid:%d name:%s", svc->pid, pid_get_name(svc->pid, NULL, 0));
	svc_set_state(svc, SVC_STOPPING_STATE);

	if (runlevel != 1)
		print_desc("Stopping ", svc->desc);

	res = kill(svc->pid, SIGTERM);

	if (runlevel != 1)
		print_result(res);

	return res;
}

/**
 * service_restart - Restart a service by sending %SIGHUP
 * @svc: Service to reload
 *
 * This function does some basic checks of the runtime state of Finit
 * and a sanity check of the @svc before sending %SIGHUP.
 *
 * Returns:
 * POSIX OK(0) or non-zero on error.
 */
static int service_restart(svc_t *svc)
{
	int do_progress = 1;
	int rc;

	/* Ignore if finit is SIGSTOP'ed */
	if (is_norespawn())
		return 1;

	if (!svc || !svc->sighup)
		return 1;

	if (svc->pid <= 1) {
		_d("Bad PID %d for %s, SIGHUP", svc->pid, svc->cmd);
		svc->start_time = svc->pid = 0;
		return 1;
	}

	/* Skip progress if desc disabled or bootstrap task */
	if (!svc->desc[0] || svc_in_runlevel(svc, 0))
		do_progress = 0;

	if (do_progress)
		print_desc("Restarting ", svc->desc);

	_d("Sending SIGHUP to PID %d", svc->pid);
	rc = kill(svc->pid, SIGHUP);

	/* Declare we're waiting for svc to re-assert/touch its pidfile */
	svc_starting(svc);

	/* Service does not maintain a PID file on its own */
	if (svc_has_pidfile(svc)) {
		sched_yield();
		touch(pid_file(svc));
	}

	if (do_progress)
		print_result(rc);

	return rc;
}

/**
 * service_reload_dynamic - Called on SIGHUP, 'init q' or 'initctl reload'
 *
 * This function is called when Finit has recieved SIGHUP to reload
 * .conf files in /etc/finit.d.  It is responsible for starting,
 * stopping and reloading (forwarding SIGHUP) to processes affected.
 */
void service_reload_dynamic(void)
{
	sm_set_reload(&sm);
	sm_step(&sm);
}

/**
 * service_runlevel - Change to a new runlevel
 * @newlevel: New runlevel to activate
 *
 * Stops all services not in @newlevel and starts, or lets continue to run,
 * those in @newlevel.  Also updates @prevlevel and active @runlevel.
 */
void service_runlevel(int newlevel)
{
	if (!rescue && runlevel <= 1 && newlevel > 1)
		networking(1);

	sm_set_runlevel(&sm, newlevel);
	sm_step(&sm);

	if (!rescue && runlevel <= 1)
		networking(0);
}

/*
 * log:/path/to/logfile,priority:facility.level,tag:ident
 */
static void parse_log(svc_t *svc, char *arg)
{
	char *tok;

	tok = strtok(arg, ":, ");
	while (tok) {
		if (!strcmp(tok, "log"))
			svc->log.enabled = 1;
		else if (!strcmp(tok, "null") || !strcmp(tok, "/dev/null"))
			svc->log.null = 1;
		else if (!strcmp(tok, "console") || !strcmp(tok, "/dev/console"))
			svc->log.console = 1;
		else if (tok[0] == '/')
			strlcpy(svc->log.file, tok, sizeof(svc->log.file));
		else if (!strcmp(tok, "priority") || !strcmp(tok, "prio"))
			strlcpy(svc->log.prio, strtok(NULL, ","), sizeof(svc->log.prio));
		else if (!strcmp(tok, "tag") || !strcmp(tok, "identity") || !strcmp(tok, "ident"))
			strlcpy(svc->log.ident, strtok(NULL, ","), sizeof(svc->log.ident));

		tok = strtok(NULL, ":=, ");
	}
}

/*
 * name:<name>
 */
static void parse_name(svc_t *svc, char *arg)
{
	char *name = NULL;

	if (arg && !strncasecmp(arg, "name:", 5)) {
		name = arg + 5;
	} else {
		name = strrchr(svc->cmd, '/');
		name = name ? name + 1 : svc->cmd;
	}

	strlcpy(svc->name, name, sizeof(svc->name));
}

/**
 * parse_cmdline_args - Update the command line args in the svc struct
 *
 * strtok internal pointer must be positioned at first command line arg
 * when this function is called.
 *
 * Side effect: strtok internal pointer will be modified.
 */
static void parse_cmdline_args(svc_t *svc, char *cmd)
{
	int i;
	char *arg;

	strlcpy(svc->args[0], cmd, sizeof(svc->args[0]));

	/* Copy supplied args. Stop at MAX_NUM_SVC_ARGS-1 to allow the args
	 * array to be zero-terminated. */
	for (i=1; (arg = strtok(NULL, " ")) && i < (MAX_NUM_SVC_ARGS-1); i++)
		strlcpy(svc->args[i], arg, sizeof(svc->args[0]));

	/* clear remaining args in case they were set earlier.
	 * This also zero-terminates the args array.*/
	for (; i < MAX_NUM_SVC_ARGS; i++)
		svc->args[i][0] = 0;
}


/**
 * service_register - Register service, task or run commands
 * @type:   %SVC_TYPE_SERVICE(0), %SVC_TYPE_TASK(1), %SVC_TYPE_RUN(2)
 * @cfg:    Configuration, complete command, with -- for description text
 * @rlimit: Limits for this service/task/run/inetd, may be global limits
 * @file:   The file name service was loaded from
 *
 * This function is used to register commands to be run on different
 * system runlevels with optional username.  The @type argument details
 * if it's service to bo monitored/respawned (daemon), a one-shot task
 * or a command that must run in sequence and not in parallell, like
 * service and task commands do.
 *
 * The @line can optionally start with a username, denoted by an @
 * character. Like this:
 *
 *     service @username [!0-6,S] <!COND> /path/to/daemon arg -- Description
 *     task @username [!0-6,S] /path/to/task arg              -- Description
 *     run  @username [!0-6,S] /path/to/cmd arg               -- Description
 *     inetd tcp/ssh nowait [2345] @root:root /sbin/sshd -i   -- Description
 *
 * If the username is left out the command is started as root.  The []
 * brackets denote the allowed runlevels, if left out the default for a
 * service is set to [2-5].  Allowed runlevels mimic that of SysV init
 * with the addition of the 'S' runlevel, which is only run once at
 * startup.  It can be seen as the system bootstrap.  If a task or run
 * command is listed in more than the [S] runlevel they will be called
 * when changing runlevel.
 *
 * Services (daemons, not inetd services) also support an optional <!EV>
 * argument.  This is for services that, e.g., require a system gateway
 * or interface to be up before they are started.  Or restarted, or even
 * SIGHUP'ed, when the gateway changes or interfaces come and go.  The
 * special case when a service is declared with <!> means it does not
 * support SIGHUP but must be STOP/START'ed at system reconfiguration.
 *
 * Service conditions can be: svc/<PATH> for PID files, net/<IFNAME>/up
 * and net/<IFNAME>/exists.  The condition handling is further described
 * docs/conditions.md.
 *
 * For multiple instances of the same command, e.g. multiple DHCP
 * clients, the user must enter an ID, using the :ID syntax.
 *
 *     service :1 /sbin/udhcpc -i eth1
 *     service :2 /sbin/udhcpc -i eth2
 *
 * Without the :ID syntax Finit will overwrite the first service line
 * with the contents of the second.  The :ID must be [1,MAXINT].
 *
 * Returns:
 * POSIX OK(0) on success, or non-zero errno exit status on failure.
 */
int service_register(int type, char *cfg, struct rlimit rlimit[], char *file)
{
	char id_str[MAX_ID_LEN];
#ifdef INETD_ENABLED
	int forking = 0;
#endif
	int levels = 0;
	int manual = 0;
	char *line;
	char *id = NULL;
	char *username = NULL, *log = NULL, *pid = NULL;
	char *service = NULL, *proto = NULL, *ifaces = NULL;
	char *cmd, *desc, *runlevels = NULL, *cond = NULL;
	char *name = NULL;
	svc_t *svc;
	plugin_t *plugin = NULL;

	if (!cfg) {
		_e("Invalid input argument");
		return errno = EINVAL;
	}

	line = strdup(cfg);
	if (!line)
		return 1;

	desc = strstr(line, "-- ");
	if (desc) {
		*desc = 0;
		desc += 3;

		while (*desc && isblank(*desc))
			desc++;
	} else {
		int pos;

		/* Find "--\n" to denote empty/no description */
		pos = (int)strlen(line) - 2;
		if (pos > 0 && !strcmp(&line[pos], "--")) {
			line[pos] = 0;
			desc = &line[pos];
		}
	}

	cmd = strtok(line, " ");
	if (!cmd) {
	incomplete:
		_e("Incomplete service '%s', cannot register", cfg);
		free(line);
		return errno = ENOENT;
	}

	while (cmd) {
		if (cmd[0] == '@')	/* @username[:group] */
			username = &cmd[1];
		else if (cmd[0] == '[')	/* [runlevels] */
			runlevels = &cmd[0];
		else if (cmd[0] == '<')	/* <[!][cond][,cond..]> */
			cond = &cmd[1];
		else if (cmd[0] == ':')	/* :ID */
			id = &cmd[1];
#ifdef INETD_ENABLED
		else if (!strncasecmp(cmd, "nowait", 6))
			forking = 1;
		else if (!strncasecmp(cmd, "wait", 4))
			forking = 0;
#endif
		else if (!strncasecmp(cmd, "log", 3))
			log = cmd;
		else if (!strncasecmp(cmd, "pid", 3))
			pid = cmd;
		else if (!strncasecmp(cmd, "name:", 5))
			name = cmd;
		else if (!strncasecmp(cmd, "manual:yes", 10))
			manual = 1;
		else if (cmd[0] != '/' && strchr(cmd, '/'))
			service = cmd;   /* inetd service/proto */
		else
			break;

		/* Check if valid command follows... */
		cmd = strtok(NULL, " ");
		if (!cmd)
			goto incomplete;
	}

	levels = conf_parse_runlevels(runlevels);
	if (runlevel > 0 && !ISOTHER(levels, 0)) {
		_d("Skipping %s, bootstrap is completed.", cmd);
		free(line);
		return 0;
	}

	/* Example: inetd ssh/tcp@eth0,eth1 or 222/tcp@eth2 */
	if (service) {
		ifaces = strchr(service, '@');
		if (ifaces)
			*ifaces++ = 0;

		proto = strchr(service, '/');
		if (!proto)
			goto incomplete;
		*proto++ = 0;
	}

#ifdef INETD_ENABLED
	/* Find plugin that provides a callback for this inetd service */
	if (type == SVC_TYPE_INETD) {
		if (!strncasecmp(cmd, "internal", 8)) {
			char *ptr, *ps = service;

			/* internal.service */
			ptr = strchr(cmd, '.');
			if (ptr) {
				*ptr++ = 0;
				ps = ptr;
			}

			plugin = plugin_find(ps);
			if (!plugin || !plugin->inetd.cmd) {
				_w("Inetd service %s has no internal plugin, skipping ...", service);
				free(line);
				return errno = ENOENT;
			}
		}

		/* Check if known inetd, update command line, then add ifnames for filtering only. */
		svc = inetd_find_svc(cmd, service, proto);
		if (svc) {
			parse_cmdline_args(svc, cmd);
			goto inetd_setup;
		}
		if (!id) {
			int n = svc_next_id_int(cmd);

			if (n) {
				snprintf(id_str, sizeof(id_str), "%d", n);
				id = id_str;
			}
		}
	}
recreate:
#endif

	if (!id)
		id = "1";

	svc = svc_find(cmd, id);
	if (!svc) {
		_d("Creating new svc for %s id #%s type %d", cmd, id, type);
		svc = svc_new(cmd, id, type);
		if (!svc) {
			_e("Out of memory, cannot register service %s", cmd);
			free(line);
			return errno = ENOMEM;
		}

		if (type == SVC_TYPE_SERVICE && manual) {
			svc_stop(svc);
		}
	}
#ifdef INETD_ENABLED
	else {
		if (svc_is_inetd(svc) && type != SVC_TYPE_INETD) {
			_d("Service was previously inetd, deregistering ...");
			inetd_del(&svc->inetd);
			svc_del(svc);
			goto recreate;
		}
	}
#endif

	/* Always clear svc PID file, for now.  See TODO */
	svc->pidfile[0] = 0;
	/* Decode any optional pid:/optional/path/to/file.pid */
	if (pid && svc_is_daemon(svc) && pid_file_parse(svc, pid))
		_e("Invalid 'pid' argument to service: %s", pid);

	if (username) {
		char *ptr = strchr(username, ':');

		if (ptr) {
			*ptr++ = 0;
			strlcpy(svc->group, ptr, sizeof(svc->group));
		}
		strlcpy(svc->username, username, sizeof(svc->username));
	}

	if (plugin) {
		/* Internal plugin provides this service */
		svc->inetd.cmd = plugin->inetd.cmd;
		svc->inetd.builtin = 1;
	} else
		parse_cmdline_args(svc, cmd);

	svc->runlevels = levels;
	_d("Service %s runlevel 0x%2x", svc->cmd, svc->runlevels);

	conf_parse_cond(svc, cond);

	parse_name(svc, name);

	if (log)
		parse_log(svc, log);
	if (desc)
		strlcpy(svc->desc, desc, sizeof(svc->desc));

#ifdef INETD_ENABLED
	if (svc_is_inetd(svc)) {
		char *iface, *name = service;

		if (svc->inetd.cmd && plugin)
			name = plugin->name;

		if (inetd_new(&svc->inetd, name, service, proto, forking, svc)) {
			_e("Failed registering new inetd service %s/%s", service, proto);
			free(line);
			return svc_del(svc);
		}

	inetd_setup:
		inetd_flush(&svc->inetd);

		if (!ifaces) {
			_d("No specific iface listed for %s, allowing ANY", service);
			inetd_allow(&svc->inetd, NULL);
		} else {
			for (iface = strtok(ifaces, ","); iface; iface = strtok(NULL, ",")) {
				if (iface[0] == '!')
					inetd_deny(&svc->inetd, &iface[1]);
				else
					inetd_allow(&svc->inetd, iface);
			}
		}
	}
#endif
	/* Set configured limits */
	memcpy(svc->rlimit, rlimit, sizeof(svc->rlimit));

	/* New, recently modified or unchanged ... used on reload. */
	if (file && conf_changed(file))
		svc_mark_dirty(svc);
	else
		svc_mark_clean(svc);

	if (!file)
		svc->protect = 1;

	/* Free duped line, from above */
	free(line);
	return 0;
}

/*
 * This function is called when cleaning up lingering (stopped) services
 * after a .conf reload, as well as when an inetd connection terminates.
 *
 * We need to ensure we properly stop the service before removing it,
 * including stopping any pending restart or SIGKILL timers before we
 * proceed to free() the svc itself.
 *
 * Remember to not try to stop inetd connections, they only get here
 * when already stopped, if we do we end up in a recursive loop.
 */
void service_unregister(svc_t *svc)
{
	if (!svc)
		return;

	/*
	 * Only try stopping @svc if it's *not* an inetd connection.
	 * Prevents infinite recursion when called from service_step()
	 */
	switch (svc->type) {
#ifdef INETD_ENABLED
	case SVC_TYPE_INETD:
		inetd_del(&svc->inetd);
		break;

	case SVC_TYPE_INETD_CONN:
		/* inetd connection, if UDP unblock parent */
		if (svc_is_busy(svc->inetd.svc)) {
			svc_unblock(svc->inetd.svc);
			service_step(svc->inetd.svc);
		}
		break;
#endif

	default:
		service_stop(svc);
		break;
	}

	svc_del(svc);
}

void service_monitor(pid_t lost)
{
	svc_t *svc;

	if (fexist(SYNC_SHUTDOWN) || lost <= 1)
		return;

	if (tty_respawn(lost))
		return;

	svc = svc_find_by_pid(lost);
	if (!svc) {
		_d("collected unknown PID %d", lost);
		return;
	}

	_d("collected %s(%d)", svc->cmd, lost);

	/* Try removing PID file (in case service does not clean up after itself) */
	if (svc_is_daemon(svc)) {
		char *fn;

		fn = pid_file(svc);
		if (remove(fn) && errno != ENOENT)
			logit(LOG_CRIT, "Failed removing service %s pidfile %s", basename(svc->cmd), fn);
	}

	/* No longer running, update books. */
	svc->start_time = svc->pid = 0;

	if (!service_step(svc)) {
		/* Clean out any bootstrap tasks, they've had their time in the sun. */
		if (svc_clean_bootstrap(svc))
			_d("collected bootstrap task %s(%d), removing.", svc->cmd, lost);
	}

	sm_step(&sm);
}

static void service_retry(svc_t *svc)
{
	int timeout;
	char *restart_cnt = (char *)&svc->restart_cnt;

	service_timeout_cancel(svc);

	if (svc->state != SVC_HALTED_STATE ||
	    svc->block != SVC_BLOCK_RESTARTING) {
		_d("%s not crashing anymore", svc->cmd);
		*restart_cnt = 0;
		return;
	}

	if (*restart_cnt >= RESPAWN_MAX) {
		logit(LOG_ERR, "%s keeps crashing, not restarting", svc->cmd);
		svc_crashing(svc);
		*restart_cnt = 0;
		service_step(svc);
		return;
	}

	(*restart_cnt)++;

	_d("%s crashed, trying to start it again, attempt %d", svc->cmd, *restart_cnt);
	svc_unblock(svc);
	service_step(svc);

	/* Wait 2s for the first 5 respawns, then back off to 5s */
	timeout = ((*restart_cnt) <= (RESPAWN_MAX / 2)) ? 2000 : 5000;
	service_timeout_after(svc, timeout, service_retry);
}

static void svc_set_state(svc_t *svc, svc_state_t new)
{
	svc_state_t *state = (svc_state_t *)&svc->state;

	*state = new;

	/* if PID isn't collected within SVC_TERM_TIMEOUT msec, kill it! */
	if ((*state == SVC_STOPPING_STATE) && !svc_is_inetd(svc)) {
		service_timeout_cancel(svc);
		service_timeout_after(svc, SVC_TERM_TIMEOUT, service_kill);
	}
}

/*
 * Transition inetd/task/run/service
 *
 * Returns: non-zero if the @svc is no longer valid (removed)
 */
int service_step(svc_t *svc)
{
	cond_state_t cond;
	svc_state_t old_state;
	svc_cmd_t enabled;
	char *restart_cnt = (char *)&svc->restart_cnt;
	int changed = 0;
	int err;

restart:
	old_state = svc->state;
	enabled = svc_enabled(svc);

	_d("%20s(%4d): %8s %3sabled/%-7s cond:%-4s", svc->cmd, svc->pid,
	   svc_status(svc), enabled ? "en" : "dis", svc_dirtystr(svc),
	   condstr(cond_get_agg(svc->cond)));

	switch (svc->state) {
	case SVC_HALTED_STATE:
		if (enabled)
			svc_set_state(svc, SVC_READY_STATE);
		break;

	case SVC_DONE_STATE:
#ifdef INETD_ENABLED
		if (svc_is_inetd_conn(svc)) {
			service_unregister(svc);
			return -1;
		}
#endif
		if (svc_is_changed(svc))
			svc_set_state(svc, SVC_HALTED_STATE);
		break;

	case SVC_STOPPING_STATE:
		if (!svc->pid) {
			/* PID was collected normally, no need to kill it */
			service_timeout_cancel(svc);

			switch (svc->type) {
			case SVC_TYPE_SERVICE:
			case SVC_TYPE_INETD:
				svc_set_state(svc, SVC_HALTED_STATE);
				break;

			case SVC_TYPE_INETD_CONN:
			case SVC_TYPE_TASK:
			case SVC_TYPE_RUN:
				svc_set_state(svc, SVC_DONE_STATE);
				break;

			default:
				_e("unknown service type %d", svc->type);
				break;
			}
		}
		break;

	case SVC_READY_STATE:
		if (!enabled) {
			svc_set_state(svc, SVC_HALTED_STATE);
		} else if (cond_get_agg(svc->cond) == COND_ON) {
			/* wait until all processes have been stopped before continuing... */
			if (sm_is_in_teardown(&sm))
				break;

			err = service_start(svc);
			if (err) {
				(*restart_cnt)++;
				if (!svc_is_inetd_conn(svc))
					break;
			}

			/* Everything went fine, clean and set state */
			svc_mark_clean(svc);
			svc_set_state(svc, SVC_RUNNING_STATE);
		}
		break;

	case SVC_RUNNING_STATE:
		if (!enabled) {
			service_stop(svc);
			break;
		}

		if (!svc->pid) {
			if (svc_is_daemon(svc)) {
				svc_restarting(svc);
				svc_set_state(svc, SVC_HALTED_STATE);

				/*
				 * Restart directly after the first crash,
				 * then retry after 2 sec
				 */
				_d("delayed restart of %s", svc->cmd);
				service_timeout_after(svc, 1, service_retry);
				break;
			}

			/* Collected inetd connection, drive it to stopping */
			if (svc_is_inetd_conn(svc)) {
				svc_set_state(svc, SVC_STOPPING_STATE);
				break;
			}

			if (svc_is_runtask(svc)) {
				svc_set_state(svc, SVC_STOPPING_STATE);
				svc->once++;
				break;
			}
		}

		cond = cond_get_agg(svc->cond);
		switch (cond) {
		case COND_OFF:
			service_stop(svc);
			break;

		case COND_FLUX:
			kill(svc->pid, SIGSTOP);
			svc_set_state(svc, SVC_WAITING_STATE);
			break;

		case COND_ON:
			if (svc_is_changed(svc)) {
				if (svc->sighup) {
					/* wait until all processes has been stopped before continuing... */
					if (sm_is_in_teardown(&sm))
						break;
					service_restart(svc);
				} else {
#ifdef INETD_ENABLED
					if (svc_is_inetd(svc))
						inetd_stop_children(&svc->inetd, 1);
					else
#endif
						service_stop(svc);
				}
				svc_mark_clean(svc);
			}
			break;
		}
		break;

	case SVC_WAITING_STATE:
		if (!enabled) {
			kill(svc->pid, SIGCONT);
			service_stop(svc);
			break;
		}

		if (!svc->pid) {
			(*restart_cnt)++;
			svc_set_state(svc, SVC_READY_STATE);
			break;
		}

		cond = cond_get_agg(svc->cond);
		switch (cond) {
		case COND_ON:
			kill(svc->pid, SIGCONT);
			svc_set_state(svc, SVC_RUNNING_STATE);
			/* Reassert condition if we go from waiting and no change */
			if (!svc_is_changed(svc)) {
				char cond[MAX_COND_LEN];

				mkcond(cond, sizeof(cond), svc->cmd);
				cond_set_path(cond_path(cond), COND_ON);
			}
			break;

		case COND_OFF:
			kill(svc->pid, SIGCONT);
			service_stop(svc);
			break;

		case COND_FLUX:
			break;
		}
		break;
	}

	if (svc->state != old_state) {
		_d("%20s(%4d): -> %8s", svc->cmd, svc->pid, svc_status(svc));
		changed++;
		goto restart;
	}

	/*
	 * When a run/task/service changes state, e.g. transitioning from
	 * waiting to running, other services may need to change state too.
	 */
	if (changed)
		schedule_work(&work);

	return 0;
}

void service_step_all(int types)
{
	svc_foreach_type(types, service_step);
}

void service_worker(void *unused)
{
	service_step_all(SVC_TYPE_SERVICE | SVC_TYPE_RUNTASK | SVC_TYPE_INETD);
}

/**
 * svc_clean_runtask - Clear once flag of runtasks
 *
 * XXX: runtasks should be stopped before calling this
 */
void service_runtask_clean(void)
{
	svc_t *svc, *iter = NULL;

	for (svc = svc_iterator(&iter, 1); svc; svc = svc_iterator(&iter, 0)) {
		if (!svc_is_runtask(svc))
			continue;

		svc->once = 0;
		if (svc->state == SVC_DONE_STATE)
			svc_set_state(svc, SVC_HALTED_STATE);
	}
}

/**
 * service_completed - Have run/task completed in current runlevel
 *
 * This function checks if all run/task have run once in the current
 * runlevel.  E.g., at bootstrap we must wait for these scripts or
 * programs to complete their run before switching to the configured
 * runlevel.
 *
 * All tasks with %HOOK_SVC_UP, %HOOK_SYSTEM_UP set in their condition
 * mask are skipped.  These tasks cannot run until finalize()
 *
 * Returns:
 * %TRUE(1) or %FALSE(0)
 */
int service_completed(void)
{
	svc_t *svc, *iter = NULL;

	for (svc = svc_iterator(&iter, 1); svc; svc = svc_iterator(&iter, 0)) {
		if (!svc_is_runtask(svc))
			continue;

		if (!svc_enabled(svc))
			continue;

		if (strstr(svc->cond, plugin_hook_str(HOOK_SVC_UP)) ||
		    strstr(svc->cond, plugin_hook_str(HOOK_SYSTEM_UP))) {
			_d("Skipping %s(%s), post-strap hook", svc->desc, svc->cmd);
			continue;
		}

		if (!svc->once) {
			_d("%s has not yet completed ...", svc->cmd);
			return 0;
		}
		_d("%s has completed ...", svc->cmd);
	}

	return 1;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
