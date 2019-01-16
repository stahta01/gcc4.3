
#include <stdlib.h>
#include <setjmp.h>

unsigned char events;


int main (void)
{
	jmp_buf J;

	asm ("ldu\t#0xaaaa");
	asm ("ldx\t#0xbbbb");
	asm ("ldy\t#0xcccc");

	events = 0;
	if (setjmp (J) == 0)
	{
		/* normal data path */
		events |= 0x1;

		/* exception occurred -- unwind */
		longjmp (J, 1);

		/* fallthru - should not happen */
		events |= 0x4;
	}
	else
	{
		/* handle exception */
		events |= 0x2;
	}

	if (events != 0x3)
		abort ();
	return 0;
}

