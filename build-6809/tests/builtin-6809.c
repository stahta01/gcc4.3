void abort (void);

#define daatest(in,out) \
do { \
	bytevar = in; \
	__builtin_daa (bytevar); \
	if (bytevar != out) \
		abort (); \
} while (0)

void g (void)
{
	register unsigned char bytevar;
	// daatest (0x99, 0x99);

}

void h (void)
{
	static unsigned char bytevar;
	// daatest (0x99, 0x99);
}

int main (void)
{
	unsigned char bytevar;

	/* Generate swi, swi2, and swi3 instructions.  By default
	on the simulator these should just return without doing anything,
	so no harm and nothing to check */
	__builtin_swi ();
	__builtin_swi2 ();
	__builtin_swi3 ();

	/* Test that __builtin_daa does the right thing */
	daatest (0x99, 0x99);
	daatest (0, 0);
	daatest (0x1A, 0x20);
	// g ();
	// h ();

	/* These should compile OK but don't run due to lack of simulator support */
	// __builtin_cwai (0);
	// __builtin_sync ();

	/* Should generate a nop instruction.  Hard to test this... */
	__builtin_nop ();

	/* Should generate nothing at all */
	__builtin_blockage ();
	return 0;
}

