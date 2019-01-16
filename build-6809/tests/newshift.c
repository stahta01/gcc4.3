#define HV 0x47C9
#define LV 0xC2

void broken (unsigned int x)
{
	while (x > 0)
		f (1 << x--);
}

#if 0

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;

volatile uint16_t m;
volatile uint8_t cm;

unsigned int lshrqi1 (uint8_t x) { return x >> 1; }
unsigned int lshrqi3 (uint8_t x) { return x >> 3; }
unsigned int lshrqi6 (uint8_t x) { return x >> 6; }
unsigned int lshrqi7 (uint8_t x) { return x >> 7; }
unsigned int lshrqi8 (uint8_t x) { return x >> 8; }
unsigned int lshrhi1 (uint16_t x) { return x >> 1; }
unsigned int lshrhi3 (uint16_t x) { return x >> 3; }
unsigned int lshrhi8 (uint16_t x) { return x >> 8; }
unsigned int lshrhi12 (uint16_t x) { return x >> 12; }
unsigned int lshrhi15 (uint16_t x) { return x >> 15; }
unsigned int lshrhiXXX (uint16_t x) { return x >> 23; }
unsigned int lshrhim1 () { return m >> 1; }
unsigned int lshrhiv1 (uint16_t n) { return n >> m; }
unsigned int lshrqiv1 (uint8_t n) { return n >> m; }
unsigned long long lshrsi1 (unsigned long long x) { return x >> 20; }

uint8_t ashlqi1 (uint8_t x) { return x << 1; }
uint8_t ashlqi3 (uint8_t x) { return x << 3; }
uint8_t  ashlqi6 (uint8_t x) { return x << 6; }
uint8_t  ashlqi7 (uint8_t x) { return x << 7; }
uint8_t  ashlqi8 (uint8_t x) { return x << 8; }
unsigned int ashlhi1 (uint16_t x) { return x << 1; }
unsigned int ashlhi3 (uint16_t x) { return x << 3; }
unsigned int ashlhi8 (uint16_t x) { return x << 8; }
unsigned int ashlhi12 (uint16_t x) { return x << 12; }
unsigned int ashlhi15 (uint16_t x) { return x << 15; }
unsigned int ashlhiXXX (uint16_t x) { return x << 23; }
unsigned int ashlhim1 () { return m << 1; }
unsigned int ashlhiv1 (uint16_t n) { return n << m; }
unsigned int ashlqiv1 (uint8_t n) { return n << m; }
unsigned long long ashlsi1 (unsigned long long x) { return x << 20; }

unsigned int by7 (unsigned char offset) { return m + offset * 7; }




int main (void)
{
	if (lshrhi1 (HV) != HV >> 1) return 1;
	if (lshrhi3 (HV) != HV >> 3) return 2;
	if (lshrhi8 (HV) != HV >> 8) return 3;
	if (lshrhi12 (HV) != HV >> 12) return 4;
	if (lshrhi15 (HV) != HV >> 15) return 5;

	if (lshrqi1 (LV) != LV >> 1) return 11;
	if (lshrqi3 (LV) != LV >> 3) return 12;
	if (lshrqi6 (LV) != LV >> 6) return 13;
	if (lshrqi7 (LV) != LV >> 7) return 14;
	if (lshrqi8 (LV) != LV >> 8) return 15;

	m = HV; if (lshrhim1 () != HV >> 1) return 21;
	
	m = 3; if (lshrhiv1 (HV) != HV >> 3) return 31;
	m = 3; if (lshrqiv1 (LV) != LV >> 3) return 31;

	lshrsi1 (0x12345678);
	return 0;
}

#endif
