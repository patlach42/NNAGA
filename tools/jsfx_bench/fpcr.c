#include <stdio.h>
#include <stdint.h>
#include <time.h>

static inline uint64_t rd(void){ uint64_t v; __asm__ __volatile__("mrs %0, fpcr":"=r"(v)); return v; }
static inline void wr(uint64_t v){ __asm__ __volatile__("msr fpcr, %0"::"r"(v)); }
static inline uint64_t ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000000000ull+t.tv_nsec; }

/* stand-in for the JIT body: a handful of FP ops, like a tiny @sample */
static double body(double x){
  for(int i=0;i<8;i++) x = x*1.0000001 + 0.5;
  return x;
}

#define N 2000000
int main(void){
  volatile double acc=0; uint64_t t0,t1;

  /* A: plain call, no FPCR traffic  */
  t0=ns(); for(long i=0;i<N;i++) acc+=body(i); t1=ns();
  double a=(double)(t1-t0)/N;

  /* B: read FPCR each call (FZ already set -> EEL's "else" branch) */
  wr(rd()|(1u<<24));
  t0=ns(); for(long i=0;i<N;i++){ volatile uint64_t f=rd(); (void)f; acc+=body(i);} t1=ns();
  double b=(double)(t1-t0)/N;

  /* C: read + set + restore each call (EEL's slow branch, FZ clear) */
  wr(rd()&~(uint64_t)(1u<<24));
  t0=ns(); for(long i=0;i<N;i++){ uint64_t f=rd(); wr(f|(1u<<24)); acc+=body(i); wr(f);} t1=ns();
  double c=(double)(t1-t0)/N;

  printf("body only          : %7.2f ns/call\n",a);
  printf("+ mrs fpcr         : %7.2f ns/call  (+%.2f)\n",b,b-a);
  printf("+ mrs/msr/msr      : %7.2f ns/call  (+%.2f)\n",c,c-a);
  printf("acc=%f\n",(double)acc);
  return 0;
}
