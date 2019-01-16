# Generic makefile for compiling 6809 programs composed of one source file

# Dump the compiler version.
# CC_DEBUG_OPTS += -v

# Dump the assembler version.
# CC_DEBUG_OPTS += -Wa,-a,-v

# Debug the assembler output
CC_DEBUG_OPTS += -save-temps

ifeq ($(list),y)
CC_DEBUG_OPTS += -Wa,-a
endif
ifeq ($(rtl),y)
CC_DEBUG_OPTS += -da
endif
ifeq ($(annotate),y)
CC_DEBUG_OPTS += -save-temps -Wa,-a,-v -dP
endif
ifeq ($(exp),y)
CC_DEBUG_OPTS += -mexperiment
endif
ifeq ($(sr),y)
CC_DEBUG_OPTS += -msoft-reg-count=16
endif
ifeq ($(dret),y)
CC_DEBUG_OPTS += -mdret
endif

ifdef OPT
OPT_CFLAGS=$(OPT)
else
OPT_CFLAGS=-O2
endif

ifdef int8
CC_DEBUG_OPTS += -mint8
endif

ifndef TARGET
TARGET=m6809-unknown-none
endif

ifdef VER
CC=/usr/local/bin/$(TARGET)-gcc-$(VER) $(CC_DEBUG_OPTS)
else
CC=/usr/local/bin/$(TARGET)-gcc $(CC_DEBUG_OPTS)
endif

RUN=m6809-run

# Uncomment if no libc is available.
#LIBS = minimal_libc.o

# PROGS is a list of the programs in the current directory that
# can be compiled.  It is calculated by scanning the dir for
# sources.
PROGS := $(shell ls -1 *.c | sed -e "s/\..*$$//")
ASPROGS := $(shell ls -1 *.S | sed -e "s/\..*$$//")

# RUNS is a list of make-targets for running all of the programs.
RUNS = $(PROGS:%=run-%) $(ASPROGS:%=run-%)

# CLEANS is likewise a list of make-targets to clean them all.
CLEANS = $(PROGS:%=clean-%) $(ASPROGS:%=clean-%)

CFLAGS = $(OPT_CFLAGS) $(PROG_CFLAGS) $(CC_DEBUG_OPTS)
CPPFLAGS = $(OPT_CFLAGS) $(PROG_CPPFLAGS) $(CC_DEBUG_OPTS)
ASFLAGS = -P

all : $(RUNS)

.PHONY : $(RUNS)
$(RUNS) : run-% : %
	@echo "Running $<..." && $(RUN) $<

# How to rebuild a program from scratch.
.PHONY : $(PROGS)
$(PROGS) : % : %.c $(LIBS)
	@echo "Building $@..."
	rm -f $@.o $@ \
		&& $(CC) -o $@.o $(CFLAGS) -c $< \
		&& $(CC) -o $@ -Wl,--map $@.o $(LIBS)

.PHONY : $(ASPROGS)
$(ASPROGS) : % : %.S $(LIBS)
	@echo "Building $@..."
	rm -f $@.o $@ \
		&& $(CC) -o $@.o $(CFLAGS) -c $< \
		&& $(CC) -o $@ -Wl,--map $@.o $(LIBS)

minimal_libc.o : minimal_libc.s
	$(CC) -o $@ -c -x assembler-with-cpp $<

.PHONY : $(CLEANS)
$(CLEANS) : clean-% : %
	rm -f $< $<.o $<.i $<.lst $<.map

$(PROGS:%=%.o) : Makefile

newshift : PROG_CFLAGS += -mint16 -mexperiment -da

shiftall: PROG_CFLAGS += -da -O0

clean: 
	rm -f $(PROGS) $(PROGS:%=%.o) $(PROGS:%=%.i) $(PROGS:%=%.lst) $(PROGS:%=%.map)

info:
	@echo "PROGS = $(PROGS)"
