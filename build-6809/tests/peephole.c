
extern void f (unsigned char);
int main (void)
{
	volatile register unsigned char x asm ("d");
	x = (x+1) / 2;
	f (x);
}

