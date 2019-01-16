int _start (void) { return 0; }
int __start (void) { return 0; }
int foo (void) { return 1; }
int (*fp) (void) __attribute__ ((section (".init_array"))) = foo;
int main (void) { return 0; }
