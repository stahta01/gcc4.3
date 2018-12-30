/* { dg-do compile } */
/* { dg-options "-O2" } */
/* { dg-options "-O2 -msse2" { target { i?86-*-* x86_64-*-* } } } */

typedef unsigned __attribute__ ((__mode__ (__pointer__))) uintptr_t;

#undef __vector
#define __vector __attribute__ ((__vector_size__ (16)))

typedef __vector signed char qword;
typedef __vector uintptr_t VU;

extern short g[192 + 16];

void f (qword);

void f1 (unsigned ctr)
{
  VU pin;

  pin = (VU){(uintptr_t) &g[16]};
  do
    {
      f ((qword) pin);
      ctr--;
    }
  while (ctr);
}
