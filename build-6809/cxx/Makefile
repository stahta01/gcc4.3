#
# This makefile controls the configuration and building of libsupc++
#
# Targets
# ------------------------------------------------------------------
# build (default)            Build the source tree
# clean                      Remove object files and libraries
# distclean                  Revert back to pristine copy from source control
# install                    Install built version
# uninstall                  Uninstall built version
#
# Command-Line Variables
# ------------------------------------------------------------------
# prefix                     Mandatory
# target                     Mandatory
# program_prefix             Optional
# GCC_VERSION                Mandatory
# SRCDIR                     Mandatory
# SUDO                       Optional, sudo command
# hosted                     Optional, set to 'enable' or 'disable'
#

# If set to 'enable' a full hosted library is built,
# or 'disable' for a freestanding one.
hosted ?= enable
#hosted ?= disable

##########################################################################

ifeq ($(target),)
$(error null target)
endif
ifeq ($(prefix),)
$(error null prefix)
endif
ifeq ($(GCC_VERSION),)
$(error null GCC_VERSION)
endif
ifeq ($(SRCDIR),)
$(error null SRCDIR)
endif

CXX := $(prefix)/bin/$(program_prefix)g++
CC := $(prefix)/bin/$(program_prefix)gcc
AR := $(prefix)/bin/$(program_prefix)ar
AS := $(prefix)/bin/$(program_prefix)as
LD := $(prefix)/bin/$(program_prefix)ld
RANLIB := $(prefix)/bin/$(program_prefix)ranlib
INCPATH := $(prefix)/$(target)/include
LIBPATH := $(prefix)/$(target)/lib
BLDDIR := $(PWD)/$(target)
LIBNAME := libstdc++
CXXCONFIG := $(BLDDIR)/install/c++/$(GCC_VERSION)/bits/c++config.h
MULTILIB := $(shell [ -x $(CC) ] && $(CC) -print-multi-lib | sed -n 's/\(^[^;]*\).*$$/\1/p' || echo . )
LIBSUPC := $(shell for lib in $(MULTILIB); do echo $(BLDDIR)/$$lib/build/libsupc++/.libs/libsupc++.a ; done)
LIBSTDC := $(shell for lib in $(MULTILIB); do echo $(BLDDIR)/$$lib/build/$(LIBNAME).a ; done)

##########################################################################

.PHONY: build
build: $(LIBSTDC)

.PHONY: clean
clean:
	for lib in $(MULTILIB); do \
		cd $(BLDDIR)/$$lib/build/libsupc++ && $(MAKE) clean && \
		rm -f "$(BLDDIR)/$$lib/build/$(LIBNAME).a" ; \
	done

.PHONY: distclean
distclean:
	rm -rf "$(BLDDIR)"

.PHONY: install
install: build
	echo "Installing includes $(INCPATH)/c++ ..." && \
	$(SUDO) cp -r "$(BLDDIR)/install/c++/" "$(INCPATH)/c++/" && \
	for lib in $(MULTILIB); do \
		echo "Installing library $(LIBPATH)/$$lib/$(LIBNAME).a ..." && \
		$(SUDO) cp "$(BLDDIR)/$$lib/build/$(LIBNAME).a" "$(LIBPATH)/$$lib" && \
		$(SUDO) $(RANLIB) "$(LIBPATH)/$$lib/$(LIBNAME).a" ; \
	done

.PHONY: uninstall
uninstall:
	echo "Uninstalling includes $(INCPATH)/c++ ..." && \
	$(SUDO) rm -rf "$(INCPATH)/c++/" && \
	for lib in $(MULTILIB); do \
		echo "Uninstalling library $(LIBPATH)/$$lib/$(LIBNAME).a ..." && \
		$(SUDO) rm -f "$(LIBPATH)/$$lib/$(LIBNAME).a" && \
	done

##########################################################################

$(LIBSTDC): $(LIBSUPC)
	for lib in $(MULTILIB); do \
		echo "Building $$lib/build/$(LIBNAME).a ..." && \
		cp $(BLDDIR)/$$lib/build/libsupc++/.libs/libsupc++.a \
			$(BLDDIR)/$$lib/build/$(LIBNAME).a ; \
	done

$(LIBSUPC): $(CXXCONFIG)
	for lib in $(MULTILIB); do \
		echo "Building $$lib/build/libsupc++ ..." && \
		echo "#include <stdlib.h>\n"\
		"char* __cxa_demangle(const char* __mangled_name, char* __output_buffer,\n"\
		"	size_t* __length, int* __status)\n"\
		"{\n"\
		"	*__status = -1;\n"\
		"	return NULL;\n"\
		"}" > $(BLDDIR)/$$lib/build/libsupc++/cp-demangle.c && \
		cd $(BLDDIR)/$$lib/build/libsupc++ && $(MAKE) ; \
	done

##########################################################################

$(BLDDIR)/build/config.status: $(CXX) | $(BLDDIR)/build
	@echo "Configuring libsupc++ ..." && rm -rf $(BLDDIR)/build/* && cd $(BLDDIR)/build && \
		CXX=$(CXX) CC=$(CC) AR=$(AR) AS=$(AS) LD=$(LD) RANLIB=$(RANLIB) \
		$(SRCDIR)/libstdc++-v3/configure --enable-sjlj-exceptions \
			--enable-multilib --$(hosted)-hosted-libstdcxx --disable-nls \
			--disable-shared --disable-libstdcxx-pch --disable-long-long \
			--with-newlib --prefix=$(BLDDIR)/install --host=$(target) \
			--with-gxx-include-dir=$(BLDDIR)/install/include/c++ \
			CXXFLAGS='-Os -fno-function-sections -fno-data-sections'

$(CXXCONFIG): $(BLDDIR)/build/config.status
	@echo "Building libsupc++ headers ..." && \
		cd $(BLDDIR)/build/include && $(MAKE) && $(MAKE) install-headers
	mkdir -p $(BLDDIR)/install/c++/$(GCC_VERSION) $(BLDDIR)/install/c++/$(GCC_VERSION)/bits $(BLDDIR)/install/c++/$(GCC_VERSION)/ext
	cd $(BLDDIR)/install/include/c++ && cp -r $(CXXINCLUDE) ../../c++/$(GCC_VERSION)
	cd $(BLDDIR)/install/include/c++/bits && cp -r $(CXXINCBITS) ../../../c++/$(GCC_VERSION)/bits
	cd $(BLDDIR)/install/include/c++/ext && cp -r $(CXXINCEXT) ../../../c++/$(GCC_VERSION)/ext
	cp $(BLDDIR)/install/include/c++/$(target)/bits/c++config.h $(CXXCONFIG)

$(BLDDIR)/build:
	mkdir -p $@

CXXINCLUDE = \
	cassert cctype cerrno cfloat ciso646 climits clocale cmath csetjmp \
	csignal cstdarg cstddef cstdio cstdlib cstring ctime cwchar cwctype \
	$(SRCDIR)/libstdc++-v3/libsupc++/cxxabi.h \
	$(SRCDIR)/libstdc++-v3/libsupc++/cxxabi-forced.h \
	$(SRCDIR)/libstdc++-v3/libsupc++/exception_defines.h \
	$(SRCDIR)/libstdc++-v3/libsupc++/exception \
	$(SRCDIR)/libstdc++-v3/libsupc++/new \
	$(SRCDIR)/libstdc++-v3/libsupc++/typeinfo

CXXINCBITS = \
	functexcept.h cpp_type_traits.h \
	../$(target)/bits/cxxabi_tweaks.h ../$(target)/bits/gthr.h \
	../$(target)/bits/gthr-single.h ../$(target)/bits/atomic_word.h \
	../$(target)/bits/os_defines.h ../$(target)/bits/cpu_defines.h

CXXINCEXT = \
	type_traits.h concurrence.h atomicity.h

##########################################################################

