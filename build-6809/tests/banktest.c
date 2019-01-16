
extern void a (void) __attribute__((bank ("3")));
extern void b (void) __attribute__((bank ("5")));

void f (void) __attribute__((bank ("5")));
void f (void)
{
	a ();
	b ();
}

void g (void) __attribute__((bank ("10")));
void g (void)
{
	a ();
	b ();
}

void h (void)
{
	f ();
	g ();
	a ();
}

