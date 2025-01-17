# Root file system for test environment.
#
# Copyright (c) 2021  Jacques de Laval <jacques@de-laval.se>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
#     all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

srcdir         ?= ../
SKEL           ?= $(srcdir)/skel
DEST           ?= ../tenv-root

CACHE          ?= ~/.cache
ARCH           ?= x86_64

FINITBIN       ?= ./sbin/finit

BBVER          ?= 1.31.0
BBBIN           = busybox-$(ARCH)
BBHOME         ?= https://www.busybox.net/downloads/binaries
BBURL          ?= $(BBHOME)/$(BBVER)-defconfig-multiarch-musl/$(BBBIN)

_libs_src       = $(shell ldd $(FINITBIN) | grep -Eo '/[^ ]+')
libs            = $(foreach path,$(_libs_src),$(abspath $(DEST))$(path))

all: $(libs) $(DEST)/bin/$(BBBIN)
	@(cd $(DEST);					\
	for prg in `./bin/$(BBBIN) --list-full`; do 	\
		ln -sf /bin/$(BBBIN) $$prg;		\
	done)

$(DEST)/bin/$(BBBIN).md5:
	@mkdir -p $(DEST)
	@cp -a $(SKEL)/* $(DEST)/
	@chmod -R u+w $(DEST)/
	@find $(DEST) -name .empty -delete

$(DEST)/bin/$(BBBIN): $(DEST)/bin/$(BBBIN).md5
	@(cd $(dir $@);								\
	if ! md5sum --status -c $(BBBIN).md5 2>/dev/null; then			\
		if [ -d $(CACHE) ]; then					\
			echo "Cannot find $(BBBIN), checking $(CACHE) ...";	\
			cd $(CACHE);						\
			cp $(DEST)/bin/$(BBBIN).md5 .;				\
			if ! md5sum --status -c $(BBBIN).md5; then		\
				echo "No $(BBBIN) available downloading ...";	\
				wget $(BBURL);					\
			else							\
				echo "Found valid $(BBBIN) in cache!";		\
			fi;							\
			cp $(BBBIN) $@;						\
		else								\
			echo "Cannot find $(BBBIN), downloading ...";		\
			wget -O $@ $(BBURL);					\
		fi;								\
		cd $(dir $@);							\
		md5sum -c $(BBBIN).md5;						\
	fi)
	@chmod +x $@

$(libs): $(DEST)/bin/$(BBBIN).md5
	mkdir -p $(dir $@)
	cp $(patsubst $(abspath $(DEST))%,%,$@) $@
