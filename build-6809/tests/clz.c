
volatile unsigned int x;
volatile unsigned long L;

int clz_wrapper (void)
{
	return __builtin_clz (x);
}

int clzl_wrapper (void)
{
	return __builtin_clzl (L);
}

int main (void)
{
	L = 0x00008000UL;
	if (clzl_wrapper () != 16)
		abort ();

	x = 0x8000U;
	if (clz_wrapper () != 0)
		abort ();

	x = 0x0001U;
	if (clz_wrapper () != 15)
		abort ();

	x = 0x5000U;
	if (clz_wrapper () != 1)
		abort ();
	return 0;
}

