
unsigned int f (void) { return 0x4488; }

void g (void)
{
	static volatile unsigned int y = 0xF00F;
	y |= 0x0FF0;
}

int main (void)
{
	volatile unsigned int x = 0x4848;
	g ();
	x &= f(); /* result should be 0x4008 */
	if (x != 0x4008)
		abort ();
	return 0;
}

