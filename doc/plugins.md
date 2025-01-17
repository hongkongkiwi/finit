Hooks & Plugins
===============

* [Plugins](#plugins)
* [Hooks](#hooks)
  * [Bootstrap Hooks](#bootstrap-hooks)
  * [Runtime Hooks](#runtime-hooks)
  * [Shutdown Hooks](#shutdown-hooks)

Finit can be extended to add general functionality in the form of I/O
monitors, or hook plugins.

The following sections detail existing plugins and hook points.  For
more information, see the plugins listed below.


Plugins
-------

For your convenience a set of *optional* plugins are available:

* *alsa-utils.so*: Restore and save ALSA sound settings on
  startup/shutdown.  _Optional plugin._

* *bootmisc.so*: Setup necessary files and system directories for, e.g.,
  UTMP (tracks logins at boot).  This plugin is central to get a working
  system and runs at `HOOK_BASEFS_UP`.  The `/var`, `/run`, and `/dev`
  file systems must be writable for this plugin to work.

  Note: On an embedded system both `/var` and `/run` can be `tmpfs` RAM
  disks and `/dev` is usually a `devtmpfs`.  This must be defined in the
  `/etc/fstab` file and in the Linux kernel config.

* *dbus.so*: Setup and start system message bus, D-Bus, at boot.
  _Optional plugin._

* *hook-scripts.so*: Trigger the execution of scripts from plugin hook
  points (see [Hooks](#hooks)).  Scripts are located in
  `/libexec/finit/hook` by default, this can be build-time customized
  by using the `--with-hook-scripts-path=PATH` argument to
  `configure`.  _Optional plugin._

* *hotplug.so*: Setup and start either udev or mdev hotplug daemon, if
  available.  Enabled by default.

* *rtc.so*: Restore and save system clock from/to RTC on boot/halt.
  Enabled by default.

* *modules-load.so*: Scans `/etc/modules-load.d/*.conf` for modules to
  load using `modprobe`.  Each file can contain multiple lines with the
  name of the module to load.  Any line starting with the standard UNIX
  comment character, `#`, is skipped.
  
  Modules are loaded when entering runlevel `[2345]` using the `task`
  stanza.  Each module gets a unique `name:modprobe.foo`, and
  optional`:ID`.  The `:ID` is a globally incremented index, which can
  be disabled per file (anywhere) using the following config line:

        set noindex

  **Note:** unlike the traditional .conf `module` directive, which load
  any listed module immediately, this plugin creates standard `taks`
  directives which load the module(s) in the background.  As long as
  the `modprobe` program is found in the path, these tasks will always
  return `[ OK ]` at boot.  Check the actual status using `initctl`.

* *netlink.so*: Listens to Linux kernel Netlink events for gateway and
  interfaces.  These events are then sent to the Finit service monitor
  for services that may want to be SIGHUP'ed on new default route or
  interfaces going up/down.  Enabled by default.

* *resolvconf.so*: Setup necessary files for `resolvconf` at startup.
  _Optional plugin._

* *tty.so*: Watches `/dev`, using inotify, for new device nodes (TTY's)
  to start/stop getty consoles on them on demand.  Useful when plugging
  in a usb2serial converter to login to your embedded device.  Enabled
  by default.

* *urandom.so*: Setup random seed at startup.  Enabled by default.

* *x11-common.so*: Setup necessary files for X-Window.  _Optional plugin._

Usually you want to hook into the boot process once, simple hook plugins
like `bootmisc.so` are great for that purpose.  They are called at each
hook point in the boot process, useful to insert some pre-bootstrap
mechanisms, like generating configuration files, restoring HW device
state, etc.  Available hook points are:


Hooks
-----

### Bootstrap Hooks

* `HOOK_BANNER`: The very first point at which a plugin can run.
  Intended to be used as a banner replacement.  Essentially this runs
  just before entering runlevel S.  Assume nothing is available, so
  be prepared to use absolute paths, etc.

* `HOOK_ROOTFS_UP`: When `finit.conf` has been read and `/` has is
  mounted — very early

* `HOOK_BASEFS_UP`: All of `/etc/fstab` is mounted, swap is available
  and default init signals are setup

* `HOOK_NETWORK_UP`: System bootstrap, runlevel S, has completed and
  networking is up (`lo` is up and the `network` script has run)

* `HOOK_SVC_UP`: All services in the active runlevel has been launched

* `HOOK_SYSTEM_UP`: All services *and* everything in `/etc/finit.d`
  has been launched

### Runtime Hooks

* `HOOK_SVC_RECONF`: Called when the user has changed something in the
  `/etc/finit.d` directory and issued `SIGHUP`.  The hook is called when
  all modified/removed services have been stopped.  When the hook has
  completed, Finit continues to start all modified and new services.

* `HOOK_RUNLEVEL_CHANGE`: Called when the user has issued a runlevel
  change.  The hook is called when services not matching the new
  runlevel have been been stopped.  When the hook has completed, Finit
  continues to start all services in the new runlevel.

### Shutdown Hooks

* `HOOK_SHUTDOWN`: Called at shutdown/reboot, right before all
  services are sent `SIGTERM`

Plugins like `tty.so` extend finit by acting on events, they are called
I/O plugins and are called from the finit main loop when `poll()`
detects an event.  See the source code for `plugins/*.c` for more help
and ideas.
