
class X
{
	int x;
public:
	X () : x(0) {}
	X (int _x) : x(_x) {}
	void inc () { x++; }
	int get () { return x; }
};


static X x1;

int main (void)
{
	X x2, x3(1);
	x1.inc ();
	x2.inc ();
	x3.inc ();
	return x1.get() + x2.get() + x3.get() ;
}

