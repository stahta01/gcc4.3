
int main (void)
{
	double x = 0;
	double y = 0;

	if (x != y)
		abort ();
	y++;
	if (x > y)
		abort ();
	x += 2;
	if (x < y)
		abort ();

	x = 10.0;
	x++;
	if ((x < 10.5) || (x > 11.5))
		abort ();
	return 0;
}
