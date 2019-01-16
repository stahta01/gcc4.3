
signed char z1 = 0x15;
signed char z2 = 0x35;
void z (void)
{
	if (z1 * z2 != 0x15 * 0x35)
		abort ();
}

void test_mul (int a, int b, int p)
{
	if (a*b != p) abort ();
}

void test_umul (unsigned int a, unsigned int b, unsigned int p)
{
	if (a*b != p) abort ();
}

void test_divmod (int a, int b, int q, int r)
{
	if (a/b != q) abort ();
	if (a%b != r) abort ();
}

void test_udivmod (unsigned int a, unsigned int b, 
	unsigned int q, unsigned int r)
{
	if (a/b != q) abort ();
	if (a%b != r) abort ();
}

void test_add_long (long a, long b, long c)
{
	if (a+b != c) abort ();
}

void test_divmod_long (long a, long b, long q, long r)
{
	if (a/b != q) abort ();
	if (a%b != r) abort ();
}

long negsi2 (long a)
{
	return -a;
}

void test_negsi2 (long a)
{
	if (negsi2 (negsi2 (a)) != a)
		abort ();
}


#define TM(a,b)	test_mul(a,b,a*b); test_umul(a,b,a*b)
#define TD(a,b)	test_divmod(a,b,a/b,a%b); test_udivmod(a,b,a/b,a%b)

int main (void)
{
	int a, b;

#if 0
	if (sizeof (int) != 2)
		abort ();
	if (sizeof (long) != 4)
		abort ();
#endif

	for (a=100; a < 1000; a += 100)
	{
		for (b=1; b < a; b++)
		{
			TD(a,b);
		}
	}

	test_add_long (500L, 1500L, 2000L);
	//test_divmod_long (250, 50, 5, 0);
	// test_negsi2 (0x12345678);
	//test_divmod_long (500000, 60000, 8, 20000);
	exit (0);
}

