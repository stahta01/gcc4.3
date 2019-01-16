# gcc6809 User's Guide

This is the user's guide for the GCC port to the Motorola 6809 processor.

{Introduction
	gcc6809 was originally developed under gcc 3.1.1, then ported to
	gcc 3.3.6, and is now supported under gcc 3.4.  Support for
	earlier versions than 3.4 has been dropped.

	Work is underway to get gcc6809 working against the next version
	of GCC, version 4.2.0.

	This guide describes the current state of gcc6809.
}
{Installation
	This section describes the installation instructions for gcc6809.
	First, the compiler is distributed in source form, so you'll
	need to build it yourself.  Also, remember that gcc doesn't include
	an assembler, linker, or any user libraries.  These should already
	be installed before building gcc.  For the 6809, the supported
	assembler/linker is the asxxxx cross-assembler.  At present, no
	general-purpose C library is available, although there are efforts
	to make this happen underway.

	{Installing the Assembler/Linker
		Three programs are required from the astools package:
		* as6809, the assembler
		* aslink, the linker
		* aslib, the library manager

		(Under Cygwin, these would have a ".exe" extension.)

		It is up to you to build and install these programs.  They should
		be placed in /usr/local/m6809/bin, or the same place that the
		compiler is being installed.  A copy of astools 4.1.0 is
		included in the gcc6809 package; you can run "make asm" to
		build it.

		GCC has been tested and validated on versions as early as 1.5.2,
		and as recent as 4.1.0.  Versions 3.0.0 and higher are still
		supported now.  1.5.2 is broken and shouldn't be used.
	}
	{Unpacking the Sources
		If you received the gcc6809 package as a tarball, unpack it now.
		If you use Subversion, it's already expanded for you.  The
		following top-level files/directories exist:

		* Makefile - a wrapper around the GCC build system, hopefully
		to make it easier to use
		* src6809 - modified GCC core files, plus the new files needed
		for 6809 support
		* gcc-release - initially an empty directory, you need to install
		a core GCC release here.  The gcc6809 package does not contain
		all of GCC.  Find a file on one of the GNU mirrors and place it
		here; e.g. gcc-core-3.4.6.tar.bz2.
		* src - a complete GCC source tree.  The build process will
		explode the GCC core releases here, and patch it with the
		files in src6809.
		* build - where the actually build takes place.  All binaries
		and compiler generated temporaries can be found here.
		* doc - documentation files
	}
	{Building 'binutils'
		Since the astools programs use different command-line options from
		the GNU binutils, and most software assumes the GNU model,
		wrapper programs are provided in the gcc6809 package, inside
		the "binutils" directory.  These are just shell scripts that
		parse and re-package the command-line options before calling
		the astools.  Scripts provided for:

		* as, the GNU assembler
		* ld, the GNU linker
		* ar, the GNU archiver

		Before building GCC, these scripts need to be installed into
		/usr/local/m6809/bin also.  The provided Makefile in the gcc6809
		package can be used; just run "make binutils".

		For example, as6809 doesn't provide a command-line option
		to set the name of the output object file.  The as wrapper,
		after running the real assembler, will do this step for you.
	}
	{Building gcc
		The top-level Makefile can be used to configure, build, and
		install GCC in one step.  You can run "make everything" to get
		a clean, complete build and install in one step.

		When running on Linux, the build requires root privileges in
		order to install into /usr/local/m6809.  The makefile will
		attempt to run "sudo" before this step.  Cygwin doesn't suffer
		from this problem because all users can write to all
		directories.  If you don't have sudo installed, preceed your
		make command with "SUDO=".

		The main files installed by GCC are:
		* /usr/local/m6809/bin/gcc, which is the frontend driver to the
		compiler
		* /usr/local/libexec/gcc/m6809/version/cc1, which is the
		actual compiler
		* /usr/local/libexec/gcc/m6809/version/collect2, a frontend
		to the linker
		* /usr/local/lib/gcc/m6809/version/libgcc.a, a library of helper
		functions that is used for certain complex expressions that don't
		translate easily to 6809 instructions.

		The gcc6809 package also creates a copy of the gcc frontend driver
		named gcc-<version>.
		This makes it easy to keep several different versions of gcc
		at once.
	}
}
{Data Types
	Note: this section has changed since earlier releases.

	An "int" is 16 bits, or 2 bytes wide.  "short" and "char" can be used
	to refer to an 8-bit quantity.

	A "long" is 32 bits, or 4 bytes wide.

	Optionally, you can make integers 8-bits wide, by using the -mint8
	command-line option.  This also shortens the size of "long" to
	16-bits.  It does not affect "short" or "char".  It is strongly
	recommended that you don't do this unless you know what you are
	doing!

	All pointers are 16 bits wide.  Thus, a pointer is *not* 
	necessarily the same size as int.

	There is limited support for "far" functions, but there is no
	support for far pointers.
}
{Calling Conventions
	{Arguments
		GCC will normally try to pass arguments to a function in registers,
		to avoid copies to/from the stack.

		The lack of many registers presents a slight problem, because
		there are not enough registers for all arguments, and GCC also
		wants to keep local variables in registers.

		The default behavior is for the first 8-bit argument to be
		passed in the B (D) register, and for the first 16-bit argument
		to be passed in the X register.  All other arguments are pushed onto
		the stack.  This compromise allows functions with small argument
		lists to be fairly efficient.

		No form of promotion is used on function arguments, as none
		is necessary.

		The reason that X is used for 16-bits is that most 16-bit
		values tend to be pointers instead of simple data values.
		Using X right away allows access to all of the addressing modes
		on the 6809.  (Early versions of the compiler use 'D', and the
		generated code contains lots of "tfr d,x" instructions.)

		You can force all arguments to be pushed onto the stack using
		the -mnoreg-args option.
	}
	{Return Values
		An 8-bit return value is placed in B; a 16-bit return value is placed
		in X.
	}
	{Stack Layout
	}
	{Clobbering
		The B and X registers are assumed to be clobbered by any
		function calls, since return values and argument values may
		be placed there.

		The Y and U registers are assumed to be preserved across a
		function call.  Thus, if a function wants to use those registers,
		it must save/restore them internally.  GCC does this
		automatically.

		If you are writing assembly language macros inside C functions,
		you should understand this behavior.  Do not assume that
		a value in registers D or X has the same value after making
		any function call.
	}
}
{Registers
	The 6809 has a fairly small register set.  This makes it tough for GCC
	to do a good job, because its internal algorithms assume the availability
	of plenty of registers.  Some complex expressions may cause the compiler 
	to abort.  This doesn't happen nearly as often as it used to, 
	but occasionally it will.

	GCC can use the D and X registers for passing arguments into functions.
	D, or rather B, is used for the first 8-bit argument; X is used for the
	first 16-bit argument.  All other arguments are pushed onto the stack.
	
	GCC does not really use the 'A' accumulator.  GCC treats
	D as a generic, 16-bit register, but renames it to B when it has been
	assigned an 8-bit value.  Even when no 16-bit math is required, it
	still cannot use 'A' as a separate register from 'B'.  This is a
	limitation which, if removed, would bring some nice performance
	enhancements.  The 'A' register is used internally for some
	canned instruction sequences, but it cannot serve as a general-
	purpose 8-bit register.

	You can use the 'A' register in assembly macros.  This is useful
	for time-critical code in which using the register is faster than
	using temporaries on the stack.  Be careful that GCC does not
	use the 'D' register while doing this, though.

	The U and Y registers are used as general-purpose, 16-bit registers,
	and will be allocated for stack variables.  Note that 8-bit local
	variables cannot be assigned to these variables.  In some cases,
	you can get better performance for a local by extending its type
	(manaully) from 8-bit to 16-bit to force allocation to one of these
	registers.  U is preferred over Y since instructions using Y are all
	one-byte longer.

	The condition code register (CC) does not normally need to be read/
	written directly, but using a special syntax, you can create a
	global variable that shadows it:

		register U8 cc_reg asm ("cc");

	The only valid operations on CC are setting/clearing bits (corresponding
	to the "andcc" and "orcc" instructions), and assigning it (using
	the "tfr" instruction).

	The S register refers to the program stack and is used to reference
	local variables, function arguments, and compiler-generated temporaries.
}
{Interrupts
	Ordinary C functions can be used to implement interrupt handlers.
	These functions should be declared with the interrupt attribute
	to force emitting an "rti" instruction at function exit.

	Because IRQ saves and restore all registers automatically, there
	is no need for GCC to do this on an IRQ handler.  You should use
	the naked attribute on the IRQ function to optimize it.  However,
	do not do this on the FIRQ handler.
	
	The interrupt vector table can be declared as an ordinary structure
	of function pointers; use the section attribute to force them to
	be placed into a separate section, such as "vector".  This section
	should be mapped to address 0xFFF0 at link time.
}
{Command-Line Options
	This section describes extra command-line arguments to the compiler
	that are specific to the 6809.

	{-mint8
		Shorten "int" to just 8 bits.  See above for full details.

		This option is not recommended.
	}
	{-mlong_size
		TBD
	}
	{-mshort-branch, -mlong-branch
		Forces all branch instructions to be in the short (or long) format.
		By default, short branch instructions will be used when possible,
		and long instructions will be used otherwise.  Only use this option
		when the automatic branch calculation lengths are not sufficient.
	}
	{-mnodirect
		Inhibits placing variables in the direct section, even if they have
		been declared to be in the direct section.
	}
	{-mwpc
		Enables hardware acceleration on the WPC pinball platform.

		Currently, the only optimization being performed under WPC is to
		implement certain shift operations with the WPC hardware shift logic.
	}
	{-mfar-code-page=val
		Sets the code page for the module.  When using the farcall feature,
		this tells the compiler which page that this module will be linked
		into.  This is only used to optimize far calls to functions in
		the same code page, in which a farcall thunk is not required.
	}
	{-mcode-section=name
		Sets the name of the area/section to be used for code.
		This value can be overriden by declaring the function with a
		section attribute.
		The default value is ".text".
	}
	{-mdata-section=name
		Sets the name of the area/section to be used for initialized data.
		This value can be overriden by declaring the variable with a
		section attribute.
		The default value is ".data".
	}
	{-mbss-section=name
		Sets the name of the area/section to be used for uninitialized data.
		This value can be overriden by declaring the variable with a
		section attribute.
		The default value is ".bss".
	}
}
{Attributes
	Attributes are a GCC extension that allows declarations to be
	annotated with additional properties outside of the C language
	specification.  GCC 6809 defines some new attributes for features
	that are helpful on 6809 machines.

	To apply an attribute to a function declaration/definition, use 
	the following syntax:

		__attribute__((attr_name)) void f (...);

	If an attribute takes parameters, then use the following form:

		__attribute__((attr_name(arg))) void f (...);

	{section
		Declares that a function or variable resides in a different
		section.  This attribute takes a string argument, which is
		the name of the section.

		By default, declarations are placed into section names that
		correspond to the default sections that the assembler understands.
		Use of a section attribute causes an ".area" directive to be
		emitted prior to the declaration to place it in a new section.

		The section name "direct" is special.  This indicates that the
		section will be linked into the zero page area, which can be
		accessed using shorter, faster instructions.  It is up to you
		to make sure that the linker puts direct in the right place, and
		that the direct page register (DP) is loaded correctly for this
		to work.  Direct references are denoted in the assembly generated
		by using an asterisk in front of the object name.
	}
	{far
		Denotes a function declaration (i.e. prototype) that should be called
		using the far calling convention.

		Since the 6809 only provides a 64KB total address space, many
		6809 implementations use some form of bank switching to allow
		for more code than fits into the address space at once.  Each
		machine defines its own way of performing the bank switch.

		To declare a function that lives in a bank other than the default,
		use the following syntax:

		__attribute__((far(page_value))) void f(...);

		Note: this attribute only belongs on the declaration (in a header
		file) and not the
		definition (in a C file).  
		The generated code for the function itself is not
		any different; the differences are needed when other entities need
		to call this function.

		page_value is a string that represents an 8-bit value.  It can
		be a literal value, or a symbolic name that the assembler will
		understand.  Normally you want to place a simple number here.
		This value somehow reflects the value of the bank switching register,
		but GCC does not define its meaning.

		When a function declared as far is called, GCC emits different
		code than just a simple "jsr".  Instead it emits the following:

		%pre{
		jsr __far_call_handler
		.dw <function_name>
		.db <function_page>
		%pre}

		function_name is the ordinary name of the function.  function_page
		is the page value given in the function declaration's far
		attribute.

		The far call handler is machine-specific and is responsible for
		figuring out how to call the function.  It should save the
		current bank register(s), change them to the correct value based
		on the page value, call the function, and then restore them to
		their previous values.  It must also ensure that the registers on
		input to the actual subroutine are exactly what was in the registers
		prior to the far call handler being invoked.  Actually, only the
		B and X registers need be correct, as Y and U are not live on
		function entry; however, those must be saved/restored if they
		are used within the far call handler itself.

		Notice that the function name and page are located in memory
		immediately after the call to the handler.  The handler must
		update the return address to return beyond those 3 bytes.

		It is recommended that the far call handler be written in
		assembler, due to all of the strange requirements.

		Use the -mfar-code-page option to tell GCC what page the current
		module will be mapped to.  Far calls within the same code page
		can be optimized to direct calls.  This is optional, though.
	}
	{interrupt
		Specifies that a function is the entry point for an interrupt handler
		(on the 6809, this applies to both the IRQ and FIRQ vectors).

		This attribute causes the "rts" instruction to be replaced with
		an "rti" instruction.  It also prevents use of instructions
		such as "puls x,pc".
	}
	{naked
		Disable emitting prologue/epilogue code for the function.
		This causes no stack space to be allocated for local variables, and
		no registers to be saved/restored.  Use only when necessary.
	}
	{noreturn
		This is a standard GCC attribute.  The 6809 backend understands this
		attribute and will emit JMP instructions instead of JSR when calling
		functions that don't return.
	}
}
{Simulation
	A 6809 simulator has been ported.
}
{Known Limitations
	* More complex programs, especially those that do a lot of 16-bit
	math, will cause the compiler to abort.  There are some limitations
	in the code generator that need to be addressed.  You are less likely
	to see this when compiling with -mint8, since 16-bit math doesn't
	come up as often there.
	* Floating-point support is severely lacking and mostly untested,
	aside from the GCC builtin testcases.
	* The code generator assumes a frame pointer exists.  -fomit-frame-pointer
	will be ignored, but certain functions can't optimize a frame pointer
	away -- for example, functions that call alloca() -- and so these won't
	compile.
	* GCC 4.1 compiles cleanly but generates faulty code for unknown
	reasons.  It is likely that the backend isn't quite right, as much
	of the backend infrastructure changed during gcc 4.x development.
	* No debugger exists.  (-g is accepted but ignored)
}

