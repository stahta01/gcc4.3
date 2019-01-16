
void g_byte (unsigned char x) {}
void g_int (int x) {}
void g_long (long int x) {}

int main (void)
{
#ifdef __int8__
	g_byte (8);
#endif
#ifdef __int16__
	g_byte (16);
#endif
	g_byte (sizeof (int));
	g_byte (sizeof (long int));
	g_byte (sizeof (long long int));
	g_int (10);
	g_long (10);
	return 0;
}

