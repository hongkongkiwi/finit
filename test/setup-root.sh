#!/bin/sh

set -eu

# shellcheck disable=SC2154
make -C "$top_builddir" DESTDIR="$TENV_ROOT" install

# shellcheck disable=SC2154
FINITBIN="$(pwd)/$top_builddir/src/finit" DEST="$TENV_ROOT" make -f "$srcdir/tenv/root.mk"

# Drop plugins we don't need in test, only causes confusing FAIL in logs.
for plugin in tty.so urandom.so rtc.so modprobe.so hotplug.so; do
	find "$TENV_ROOT" -name $plugin -delete
done

echo "$TENV_ROOT:"
ls -l "$TENV_ROOT"
echo "$TENV_ROOT:/bin"
ls -l "$TENV_ROOT"/bin
echo "$TENV_ROOT:/sbin"
ls -l "$TENV_ROOT"/sbin
echo "$TENV_ROOT:/var"
ls -l "$TENV_ROOT"/var/

