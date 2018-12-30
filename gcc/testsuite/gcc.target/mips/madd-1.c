/* { dg-do compile } */
/* { dg-mips-options "-O2 -march=vr4130 -mgp32" } */
/* { dg-final { scan-assembler-times "\tmacc\t\\\$1," 3 } } */

NOMIPS16 long long
f1 (int x, int y, long long z)
{
  return (long long) x * y + z;
}

NOMIPS16 long long
f2 (int x, int y, long long z)
{
  return z + (long long) y * x;
}

NOMIPS16 long long
f3 (int x, int y, long long z)
{
  long long t = (long long) x * y;
  int temp = 5;
  if (temp == 5)
    z += t;
  return z;
}
