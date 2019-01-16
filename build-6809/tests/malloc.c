
#include <stdio.h>
#include <stdlib.h>

#define writes(s) 	write (1, s, strlen (s))

void g (void)
{
	char buf[10];

	writes ("This is a test.\n");
	sprintf (buf, "%p\n", buf);
	writes (buf);
}

void f (char *p)
{
}

int main (void)
{
	char *p;

	g ();
#if 0
	p = malloc (20);
	f (p);
	p = malloc (30);
	f (p);
#endif
	return 0;
}

