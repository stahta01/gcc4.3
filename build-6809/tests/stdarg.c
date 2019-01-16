
#include <stdarg.h>

int sum (unsigned char count, ...)
{
	int s = 0;
	va_list ap;

	va_start (ap, count);
	switch (count)
	{
		case 3:
			s += va_arg (ap, int);
		case 2:
			s += va_arg (ap, int);
		case 1:
			s += va_arg (ap, int);
		default:
			break;
	}
	va_end (ap);
	return s;
}

int main (void)
{
	if (sum (1, 5) != 5) abort ();
	if (sum (2, 5, 10) != 15) abort ();
	if (sum (3, 5, 10, 15) != 30) abort ();

	if (sum (1, 5, 10, 15) != 5) abort ();
	(void)sum (2, 5);
	return 0;
}

