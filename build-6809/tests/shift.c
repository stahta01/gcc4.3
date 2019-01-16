
extern int f (unsigned int *);
int main (void)
{
	unsigned int x;
	f (&x);
	x <<= 10;
	f (&x);
	x >>= 10;
	return 0;
}

