
int main (void)
{
	int foo = 20;
	
	int bar (int *p)
	{
		return (*p) * 2;	
	}

	int (*ptr) (void) = bar;
	return 40 - (*bar) (&foo);
}

