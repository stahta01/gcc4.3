
typedef unsigned long T;

T x, y, z, r;


void f (void)
{
#if 0
	z = x / y;
	r = x % y;
#else

	short y0 = __builtin_clzl (y);
	z = 0;
	r = x;
	while (r >= y)
	{
		short guess = __builtin_clzl (r) - y0;
		if (guess > 0)
		{
			z += guess;
			r -= y * (1 << guess);
		}
		else
		{
			z++;
			r -= y;
		}
	}
#endif
}

int main (void)
{
	for (x=1; x <= 1000; x++)
		for (y=1; y < x; y++)
		{
			f ();
			printf ("%lX / %lX = %lX r %lX\n", x, y, z, r);
			if (z * y + r != x)
				abort ();
		}
	return 0;
}
