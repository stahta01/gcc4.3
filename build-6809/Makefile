#!/usr/bin/make
# gcc6809 Makefile
#
# This makefile controls the configuration and building of gcc as a
# cross compiler for the Motorola 6809 processor.
#
# gcc trunk is assumed by default, unless you prefix the make command
# with 'GCC_VERSION=<version>', e.g. GCC_VERSION=4.1.0.  Note that
# only gcc 3.4 and higher are supported anymore.
#
# Targets
# ------------------------------------------------------------------
# everything                 Build and install from scratch
#
# distclean                  Revert back to pristine copy from source control
# build (default)            Build the source tree
# install                    Install built version
# info                       Print environment variables relevant to build
# tarball                    Build a binary tarball
# asm                        Build the assembler tools
#
# Command-Line Variables
# ------------------------------------------------------------------
#

# Figure out what host are on.  Darwin (OSX) does not support the -o
# option, so check without that first.
HOST_OS = $(shell uname)
ifeq ($(HOST_OS), Darwin)
   # Yes, we are on OSX.  Assume 'sudo' is available.
   SUDO ?= sudo
   AS_HOST = darwin
   HOST_CONFIG_OPTS=--with-gmp=/opt/local
   EXE =
else
   # It might be Cygwin or Linux.  Run with -o to find out.
   HOST_OS = $(shell uname -o)
   ifeq ($(HOST_OS), Cygwin)
      # We are on Cygwin.  There is no 'sudo'.
      SUDO :=
      AS_HOST = cygwin
      EXE = .exe
   else
      # We are probably on Linux.  Assume 'sudo' is available.
#      SUDO ?= sudo
      SUDO =
      AS_HOST = linux
      EXE =
   endif
endif

# Set default set of languages to build frontends for.
GCC_LANGUAGES ?= c,c++

# Set default gcc version to build.
GCC_VERSION ?= $(shell cat ../gcc/BASE-VER)

# Set default assembler version to build.
AS_VERSION ?= 5.1.1

# Set assembler/linker/library program names
AS_TARGET ?= as6809 aslink aslib
ifeq ($(HOST_OS), Cygwin)
AS_EXES = $(AS_TARGET:%=%.exe)
else
AS_EXES = $(AS_TARGET)
endif

# Set the prefix for installing the compiler
#prefix ?= /usr/local
prefix = /usr/src/gcc6809/build

# Set the prefix for installing the asxxxx tools.
# This need not be the same as $(prefix), since multiple target types
# can share the same programs.
as_prefix ?= $(prefix)

# Set install location for the asxxxx programs.
as_bindir := $(as_prefix)/bin

# Enable/disable support for gziped library (require zlib).
# When enabled gziped library are optional and transparent
#  That means:
#   - aslink can accept gziped and non-gziped libraries.
#   - ranlib can convert between gziped and non-gziped libraries,
#     default operation is: if the library was gziped then
#     the ranlib'ed library is gziped and if the library was
#     not gziped then the ranlib'ed library is not gziped.
#     ranlib can also force gzip or non-gzip to the ranlib'ed library.
gziplib ?= enable

# Set the target CPU
target_cpu := m6809

# Set the target machine type
# This can be overriden at the command-line.
target_vendor ?= unknown
#target_vendor = coco
#target_vendor = sim
#target_vendor = wpc

# Set the target OS
# This can be overriden at the command-line.
target_os ?= none
#target_os = freewpc
#target_os = nitros9
#target_os = os9

# Set the long canonical name for the target.  This is fed into
# "configure --target" and config.sub.
target = $(target_cpu)-$(target_vendor)-$(target_os)

# Set the prefix to be appended to the front of all compiler-generated
# binaries.  This is normally $(target).
program_prefix=$(target)-
vendor_suffix=$(target_vendor)

# Define a shorthand for referring to an executable in its full path.
run_binutil = $(prefix)/bin/$(program_prefix)$(1)$(program_suffix)

# For CCACHE_OPTIONS eventually
# CC="ccache gcc"

# For debugging/profiling the assembler programs
# EXTRA_ASXXXX_CFLAGS = -g -pg

# Compute the GCC internal version number from the dotted version.
# E.g. 3.4.6 becomes 3004, 4.0.1 becomes 4000, etc.  The final digit
# is discarded.
ifeq ($(GCC_VERSION),trunk)
GCC_CURRENT_VERSION = 4.4.0
TARGET_GCC_VERSION = 9999
BUILD_DEBUG_CFLAGS = -g
else
GCC_CURRENT_VERSION = $(GCC_VERSION)
TARGET_GCC_VERSION = $(shell echo $(GCC_VERSION) | sed -e "s/\./00/" -e "s/\..*//")
endif

# Define various flags if gziplib is enabled.
ifeq ($(gziplib),enable)
	BINUTILS_CFLAGS = -DZLIBARCH -Wno-variadic-macros
	BINUTILS_LDFLAGS = -lz
endif

# Select the flags to be used for building the compiler itself.
# Normally, build optimized.
BUILD_DEBUG_CFLAGS ?= -O2 -fomit-frame-pointer
#BUILD_DEBUG_CFLAGS = -O0 -g  # uncomment to debug the compiler

BUILD_CFLAGS = -DTARGET_GCC_VERSION=$(TARGET_GCC_VERSION) $(BUILD_DEBUG_CFLAGS)

##########################################################################

SRCDIR := $(abspath $(PWD)/..)
BLDDIR := $(PWD)/$(target)
DEJADIR=/usr/share/dejagnu
TESTEXP := m6809-sim.exp

##########################################################################
##########################################################################

.PHONY : default
ifeq ($(HOST_OS), Cygwin)
# Running configure on Cygwin is awfully slow, so we don't
# do that by default.  You only need to run it once.
# make everything will still do it for you.
default :
	$(MAKE) build; $(MAKE) install
else
default :
	$(MAKE) config; $(MAKE) build; $(MAKE) install
endif

##########################################################################

coco :
	$(MAKE) everything target_vendor=coco prefix=/opt/coco

##########################################################################

sim :
	$(MAKE) everything target_vendor=sim

##########################################################################

dfboard :
	$(MAKE) sequential ACTIONS="everything cxx-install" prefix=/usr/src/gcc6809/build SUDO=

##########################################################################

.PHONY : build
build :
	-@echo "Building compiler..." && cd $(BLDDIR) && \
		find $(target) -name "*.dep" | xargs rm -f && \
		PATH="$(PATH):$(prefix)/bin" $(MAKE)

##########################################################################

.PHONY : install
install : post-install

.PHONY : post-install
post-install: gccinstall
	$(SUDO) install -m 644 $(SRCDIR)/gcc/config/m6809/crt0.S $(prefix)/lib/gcc/$(target)/$(GCC_VERSION)

##########################################################################

.PHONY : gccinstall
gccinstall :
	-@echo "Invoking GCC install..." && cd $(BLDDIR) && \
		$(SUDO) $(MAKE) install RANLIB_FOR_TARGET=$(call run_binutil,ranlib)

##########################################################################

.PHONY : config
config :	$(BLDDIR)/config.status

$(BLDDIR)/config.status : $(BLDDIR) info
	@$(MAKE) GCC_VERSION=$(GCC_VERSION) $(BLDDIR)
	@echo "Configuring compiler ..." && cd $(BLDDIR) && \
		$(CCACHE_OPTIONS) \
		CFLAGS="$(BUILD_CFLAGS)" $(SRCDIR)/configure \
		--enable-languages=$(GCC_LANGUAGES) \
		--target=$(target) --program-prefix=$(program_prefix) --enable-obsolete \
		--srcdir=$(SRCDIR) --disable-threads --disable-nls \
		--disable-libssp --enable-sjlj-exceptions \
		--prefix=$(prefix) \
		--with-as=$(call run_binutil,as) --with-ld=$(call run_binutil,ld) \
		$(HOST_CONFIG_OPTS)

$(BLDDIR) :
	mkdir -p $(BLDDIR)

##########################################################################

.PHONY : cxx-build cxx-install cxx-uninstall cxx-clean cxx-distclean
cxx-build cxx-install cxx-uninstall cxx-clean cxx-distclean :
	@cd cxx && $(MAKE) $(subst cxx-,,$@) target=$(target) prefix=$(prefix) program_prefix=$(program_prefix) GCC_VERSION=$(GCC_VERSION) SRCDIR=$(SRCDIR) SUDO=$(SUDO)

##########################################################################

.PHONY : everything
everything :
	$(MAKE) sequential ACTIONS="all_binutils all_gcc"

.PHONY : all_binutils
all_binutils:
	$(MAKE) sequential ACTIONS="sudo-force asm-clean asm asm-install binutils-clean binutils"

.PHONY : all_gcc
all_gcc :
	$(MAKE) sequential ACTIONS="sudo-force distclean config build install"

.PHONY : sequential
sequential:
	for t in $(ACTIONS); do $(MAKE) $$t; done

.PHONY : sudo-force
sudo-force :
	$(SUDO) whoami

##########################################################################

.PHONY : distclean
distclean : asm-clean binutils-clean cxx-distclean
	@echo "Cleaning everything else ..." && rm -rf $(BLDDIR)/*

##########################################################################

.PHONY : clean
clean :
	@echo "Cleaning in $(BLDDIR) ..." && cd $(BLDDIR) && $(MAKE) clean

##########################################################################

.PHONY : test
test: $(DEJADIR)/baseboards/$(TESTEXP)
	@+echo "Running GCC testsuite in $(BLDDIR) ..." && cd $(BLDDIR) && $(MAKE) -k check-gcc RUNTESTFLAGS="$(EXPFLAGS) SIM=/usr/local/bin/m6809-run --target_board=m6809-sim"

.PHONY : smoketest
smoketest: $(DEJADIR)/baseboards/$(TESTEXP)
	@+echo "Running GCC smoketest in $(BLDDIR) ..." && cd $(BLDDIR) && $(MAKE) -k check-gcc RUNTESTFLAGS="$(EXPFLAGS) SIM=/usr/local/bin/m6809-run --target_board=m6809-sim compile.exp execute.exp"

.PHONY : simtest
simtest: $(DEJADIR)/baseboards/$(TESTEXP)
	@+echo "Running GCC simulator test in $(BLDDIR) ..." && cd $(BLDDIR) && $(MAKE) -k check-gcc RUNTESTFLAGS="$(EXPFLAGS) SIM=/usr/local/bin/m6809-run --target_board=m6809-sim execute.exp"

.PHONY : test-6809
test-6809:
	@echo "Running GCC testsuite in $(BLDDIR) ..." && cd $(BLDDIR) && make -k check-gcc RUNTESTFLAGS="SIM=/usr/local/bin/m6809-run --target_board=m6809-sim gcc.target/m6809/m6809.exp"

$(DEJADIR)/baseboards/$(TESTEXP) : testsuite/$(TESTEXP) $(DEJADIR)
	sudo cp -p $< $@

##########################################################################

.PHONY : info
info :
	@echo HOST_OS is $(HOST_OS)
	@echo SUDO is $(SUDO)
	@echo GCC_VERSION is $(GCC_VERSION)
	@echo TARGET_GCC_VERSION is $(TARGET_GCC_VERSION)
	@echo TARGET_CFLAGS is $(TARGET_CFLAGS)
	@echo AS_EXES is $(AS_EXES)
	@echo SRCDIR is $(SRCDIR)
	@echo BLDDIR is $(BLDDIR)
	@echo as_bindir is $(as_bindir)
	@echo prefix is $(prefix)

##########################################################################

.PHONY : binutils
binutils:
	@echo "Installing GNU wrappers for bintools..."
	$(SUDO) mkdir -p $(prefix)/bin
	cd ../binutils && $(MAKE) \
		BINUTILS_CFLAGS="$(BINUTILS_CFLAGS)" \
		BINUTILS_LDFLAGS="$(BINUTILS_LDFLAGS)" \
		&& $(SUDO) cp -f ranlib $(call run_binutil,ranlib)
	for prog in ar as ld; do \
		$(SUDO) sh -c "sed -e 's!@AS_PREFIX@!${as_prefix}!' < ../binutils/$$prog > $(call run_binutil,$$prog)"; \
		$(SUDO) chmod a+x $(call run_binutil,$$prog); done

.PHONY : binutils-clean
binutils-clean:
	@echo "Cleaning in binutils ..." && \
	cd ../binutils && $(MAKE) clean

##########################################################################

ASBUILD = ../as-$(AS_VERSION)/asxmak/$(AS_HOST)/build

.PHONY : asm
asm:
	@echo "Building assembler, linker, and library tool..." && \
		mkdir -p ../as-$(AS_VERSION)/asxmak/$(AS_HOST)/exe && \
		cd $(ASBUILD) && \
		$(MAKE) $(AS_TARGET) \
			EXTRA_ASXXXX_CFLAGS="$(BINUTILS_CFLAGS) -I$(SRCDIR)/binutils $(EXTRA_ASXXXX_CFLAGS)" \
			EXTRA_ASXXXX_LDFLAGS="$(BINUTILS_LDFLAGS)"

##########################################################################

.PHONY : asm-clean
asm-clean:
	@echo "Cleaning assembler, linker, and library tool..." && \
		cd $(ASBUILD) && $(MAKE) clean && rm -f $(AS_TARGET)

##########################################################################

.PHONY : asm-install
asm-install:
	@echo "Installing assembler files..." && \
		$(SUDO) mkdir -p $(as_bindir) && \
		cd $(ASBUILD)/../exe && $(SUDO) cp -p $(AS_EXES) $(as_bindir)

##########################################################################

unconfig:
	rm -f $(BLDDIR)/Makefile $(BLDDIR)/config.status

##########################################################################

BINPKG := gcc6809-$(GCC_VERSION)-$(AS_HOST)$(vendor_suffix)-$(shell date +'%Y%m%d')-bin.tar.gz

tarball:
	rm -f $(BINPKG); tar zcvf $(BINPKG) \
		$(prefix)/m6809/bin/gcc-$(GCC_VERSION) \
		$(prefix)/m6809/include \
		$(prefix)/m6809/lib \
		$(prefix)/$(target) \
		$(prefix)/bin/*09 \
		$(prefix)/bin/${program_prefix}ar \
		$(prefix)/bin/${program_prefix}as \
		$(prefix)/bin/${program_prefix}cpp \
		$(prefix)/bin/${program_prefix}gcc \
		$(prefix)/bin/${program_prefix}gcc-$(GCC_VERSION) \
		$(prefix)/bin/${program_prefix}gccbug \
		$(prefix)/bin/${program_prefix}gcov \
		$(prefix)/bin/${program_prefix}ld \
		$(prefix)/bin/${program_prefix}ranlib \
		$(prefix)/bin/m6809-run \
		$(prefix)/lib/gcc/m6809/$(GCC_VERSION) \
		$(prefix)/lib/gcc/$(target) \
		$(prefix)/libexec/gcc/m6809/$(GCC_VERSION)
		$(prefix)/libexec/gcc/$(target)

SRCPKG := gcc6809-$(shell date +'%Y%m%d').tar.gz

.PHONY: source-tarball
source-tarball:
	cd .. \
	&& rm -f $(SRCPKG) \
	&& tar zcvf $(SRCPKG) --exclude=".git*" --exclude="libada" --exclude="libgfortran" --exclude="libjava" --exclude="libobjc" --exclude="libstdc++-v3" --exclude="gnattools" --exclude="*.exe" --exclude="gcc/ada" --exclude="gcc/fortan" --exclude="gcc/objc" --exclude="gcc/java" --exclude="gcc/testsuite" --exclude='*.[oa]' *

.PHONY : source-patch
source-patch:
	cd .. && git diff @{upstream} | build-6809/patch_filter '\.git.*' '\.exe\$$' 'patches' \
		'gcc.sum' 'gcc-.*.sum' 'm6809-run.log' '^build-6809\/web\/' 'as6809\.conf\.example' \
		'djgpp' 'build-6809\/doc\/' 'autobuild-gcc6809' > gcc6809-$(shell basename `git symbolic-ref HEAD`)-N.patch
