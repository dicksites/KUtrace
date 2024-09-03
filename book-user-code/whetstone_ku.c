/*
 * C Converted Whetstone Double Precision Benchmark
 *		Version 1.2	22 March 1998
 *
 *	(c) Copyright 1998 Painter Engineering, Inc.
 *		All Rights Reserved.
 *
 *		Permission is granted to use, duplicate, and
 *		publish this text and program as long as it
 *		includes this entire comment block and limited
 *		rights reference.
 *
 * Converted by Rich Painter, Painter Engineering, Inc. based on the
 * www.netlib.org benchmark/whetstoned version obtained 16 March 1998.
 *
 * A novel approach was used here to keep the look and feel of the
 * FORTRAN version.  Altering the FORTRAN-based array indices,
 * starting at element 1, to start at element 0 for C, would require
 * numerous changes, including decrementing the variable indices by 1.
 * Instead, the array E1[] was declared 1 element larger in C.  This
 * allows the FORTRAN index range to function without any literal or
 * variable indices changes.  The array element E1[0] is simply never
 * used and does not alter the benchmark results.
 *
 * The major FORTRAN comment blocks were retained to minimize
 * differences between versions.  Modules N5 and N12, like in the
 * FORTRAN version, have been eliminated here.
 *
 * An optional command-line argument has been provided [-c] to
 * offer continuous repetition of the entire benchmark.
 * An optional argument for setting an alternate LOOP count is also
 * provided.  Define PRINTOUT to cause the POUT() function to print
 * outputs at various stages.  Final timing measurements should be
 * made with the PRINTOUT undefined.
 *
 * Questions and comments may be directed to the author at
 *			r.painter@ieee.org
 */

/* 
 * dsites 2020.06.02 compile with g++ -O2 whetstone_ku.c kutrace_lib.cc -lm -o whetstone_ku
 *                   KUtrace labels for sections added
 *                   make all loop results live (else modules 6 7 8 empty)
 */

/*
C**********************************************************************
C     Benchmark #2 -- Double  Precision Whetstone (A001)
C
C     o	This is a REAL*8 version of
C	the Whetstone benchmark program.
C
C     o	DO-loop semantics are ANSI-66 compatible.
C
C     o	Final measurements are to be made with all
C	WRITE statements and FORMAT sttements removed.
C
C**********************************************************************   
*/

/* standard C library headers required */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>		// gettimeofday


/* the following is optional depending on the timing function used */
#include <time.h>

/* dsites 2020.06.02 */
#include "kutrace_lib.h"

/* map the FORTRAN math functions, etc. to the C versions */
#define DSIN	sin
#define DCOS	cos
#define DATAN	atan
#define DLOG	log
#define DEXP	exp
#define DSQRT	sqrt
#define IF		if

/* function prototypes */
void POUT(long N, long J, long K, double X1, double X2, double X3, double X4);
void PA(double E[]);
void P0(void);
void P3(double X, double Y, double *Z);
#define USAGE	"usage: whetdc [-c] [loops]\n"

/*
	COMMON T,T1,T2,E1(4),J,K,L
*/
double T,T1,T2,E1[5];
double VT2;	/* dsites added volatile to make module 8 live */
int J,K,L;

inline uint64_t GetUsec() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000000lu) + tv.tv_usec;
}

uint64_t startusec, elapsedusec;

int
main(int argc, char *argv[])
{
	bool makelive = true;	/* dsites */

        /* used in the FORTRAN version */
	long I;
	long N1, N2, N3, N4, N6, N7, N8, N9, N10, N11;
	double X1,X2,X3,X4,X,Y,Z;
	long LOOP;
	int II, JJ;

	/* added for this version */
	long loopstart;
	long startsec, finisec;
	float KIPS;
	int continuous;

	loopstart = 1000;		/* see the note about LOOP below */
	continuous = 0;

	II = 1;		/* start at the first arg (temp use of II here) */
	while (II < argc) {
		if (strncmp(argv[II], "-c", 2) == 0 || argv[II][0] == 'c') {
			continuous = 1;
		} else if (atol(argv[II]) > 0) {
			loopstart = atol(argv[II]);
		} else {
			fprintf(stderr, USAGE);
			return(1);
		}
		II++;
	}

LCONT:
/*
C
C	Start benchmark timing at this point.
C
*/
	startusec = GetUsec();
	startsec = time(0);
	makelive = (startsec == 0);	/* dsites compiler doesn't know this is always false */

/*
C
C	The actual benchmark starts here.
C
*/
	T  = .499975;
	T1 = 0.50025;
	T2 = 2.0;
	VT2 = 2.0;
/*
C
C	With loopcount LOOP=10, one million Whetstone instructions
C	will be executed in EACH MAJOR LOOP..A MAJOR LOOP IS EXECUTED
C	'II' TIMES TO INCREASE WALL-CLOCK TIMING ACCURACY.
C
	LOOP = 1000;
*/
	LOOP = loopstart;
	II   = 1;

	JJ = 1;

IILOOP:
	N1  = 0;
	N2  = 12 * LOOP;
	N3  = 14 * LOOP;
	N4  = 345 * LOOP;
	N6  = 210 * LOOP;
	N7  = 32 * LOOP;
	N8  = 899 * LOOP;
	N9  = 616 * LOOP;
	N10 = 0;
	N11 = 93 * LOOP;
/*
C
C	Module 1: Simple identifiers
C
*/
	// kutrace::mark_a("mod 1"); // omitted

	X1  =  1.0;
	X2  = -1.0;
	X3  = -1.0;
	X4  = -1.0;

	for (I = 1; I <= N1; I++) {
	    X1 = (X1 + X2 + X3 - X4) * T;
	    X2 = (X1 + X2 - X3 + X4) * T;
	    X3 = (X1 - X2 + X3 + X4) * T;
	    X4 = (-X1+ X2 + X3 + X4) * T;
	}
	if (makelive) POUT(N1,N1,N1,X1,X2,X3,X4);	/* dsites */

#ifdef PRINTOUT
	IF (JJ==II)POUT(N1,N1,N1,X1,X2,X3,X4);
#endif

/*
C
C	Module 2: Array elements
C
*/
	kutrace::mark_a("mod 2");

	E1[1] =  1.0;
	E1[2] = -1.0;
	E1[3] = -1.0;
	E1[4] = -1.0;

	for (I = 1; I <= N2; I++) {
	    E1[1] = ( E1[1] + E1[2] + E1[3] - E1[4]) * T;
	    E1[2] = ( E1[1] + E1[2] - E1[3] + E1[4]) * T;
	    E1[3] = ( E1[1] - E1[2] + E1[3] + E1[4]) * T;
	    E1[4] = (-E1[1] + E1[2] + E1[3] + E1[4]) * T;
	}
	if (makelive) POUT(N2,N3,N2,E1[1],E1[2],E1[3],E1[4]);	/* dsites */

#ifdef PRINTOUT
	IF (JJ==II)POUT(N2,N3,N2,E1[1],E1[2],E1[3],E1[4]);
#endif

/*
C
C	Module 3: Array as parameter
C
*/
	kutrace::mark_a("mod 3");

	for (I = 1; I <= N3; I++)
		PA(E1);
	if (makelive) POUT(N3,N2,N2,E1[1],E1[2],E1[3],E1[4]);	/* dsites */

#ifdef PRINTOUT
	IF (JJ==II)POUT(N3,N2,N2,E1[1],E1[2],E1[3],E1[4]);
#endif

/*
C
C	Module 4: Conditional jumps
C
*/
	kutrace::mark_a("mod 4");

	J = 1;
	for (I = 1; I <= N4; I++) {
		if (J == 1)
			J = 2;
		else
			J = 3;

		if (J > 2)
			J = 0;
		else
			J = 1;

		if (J < 1)
			J = 1;
		else
			J = 0;
	}
	if (makelive) POUT(N4,J,J,X1,X2,X3,X4);	/* dsites */

#ifdef PRINTOUT
	IF (JJ==II)POUT(N4,J,J,X1,X2,X3,X4);
#endif

/*
C
C	Module 5: Omitted
C 	Module 6: Integer arithmetic
C
*/
	kutrace::mark_a("mod 6");


	J = 1;
	K = 2;
	L = 3;

	for (I = 1; I <= N6; I++) {
	    J = J * (K-J) * (L-K);
	    K = L * K - (L-J) * K;
	    L = (L-K) * (K+J);
	    E1[L-1] = J + K + L;
	    E1[K-1] = J * K * L;
	}
	if (makelive) POUT(N6,J,K,E1[1],E1[2],E1[3],E1[4]);	/* dsites */

#ifdef PRINTOUT
	IF (JJ==II)POUT(N6,J,K,E1[1],E1[2],E1[3],E1[4]);
#endif

/*
C
C	Module 7: Trigonometric functions
C
*/
	kutrace::mark_a("mod 7");

	X = 0.5;
	Y = 0.5;

	for (I = 1; I <= N7; I++) {
		X = T * DATAN(T2*DSIN(X)*DCOS(X)/(DCOS(X+Y)+DCOS(X-Y)-1.0));
		Y = T * DATAN(T2*DSIN(Y)*DCOS(Y)/(DCOS(X+Y)+DCOS(X-Y)-1.0));
	}
	if (makelive) POUT(N7,J,K,X,X,Y,Y);	/* dsites */

#ifdef PRINTOUT
	IF (JJ==II)POUT(N7,J,K,X,X,Y,Y);
#endif

/*
C
C	Module 8: Procedure calls
C
*/
	kutrace::mark_a("mod 8");

	X = 1.0;
	Y = 1.0;
	Z = 1.0;

	for (I = 1; I <= N8; I++)
		P3(X,Y,&Z);
	if (makelive) POUT(N8,J,K,X,Y,Z,Z);	/* dsites */

#ifdef PRINTOUT
	IF (JJ==II)POUT(N8,J,K,X,Y,Z,Z);
#endif

/*
C
C	Module 9: Array references
C
*/
	kutrace::mark_a("mod 9");

	J = 1;
	K = 2;
	L = 3;
	E1[1] = 1.0;
	E1[2] = 2.0;
	E1[3] = 3.0;

	for (I = 1; I <= N9; I++)
		P0();
	if (makelive) POUT(N9,J,K,E1[1],E1[2],E1[3],E1[4]);	/* dsites */

#ifdef PRINTOUT
	IF (JJ==II)POUT(N9,J,K,E1[1],E1[2],E1[3],E1[4]);
#endif

/*
C
C	Module 10: Integer arithmetic
C
*/
	// kutrace::mark_a("mod 10"); // omitted

	J = 2;
	K = 3;

	for (I = 1; I <= N10; I++) {
	    J = J + K;
	    K = J + K;
	    J = K - J;
	    K = K - J - J;
	}
	if (makelive) POUT(N10,J,K,X1,X2,X3,X4);	/* dsites */

#ifdef PRINTOUT
	IF (JJ==II)POUT(N10,J,K,X1,X2,X3,X4);
#endif

/*
C
C	Module 11: Standard functions
C
*/
	kutrace::mark_a("mod 11");

	X = 0.75;

	for (I = 1; I <= N11; I++)
		X = DSQRT(DEXP(DLOG(X)/T1));
	if (makelive) POUT(N11,J,K,X,X,X,X);	/* dsites */

#ifdef PRINTOUT
	IF (JJ==II)POUT(N11,J,K,X,X,X,X);
#endif

/*
C
C      THIS IS THE END OF THE MAJOR LOOP.
C
*/
	if (++JJ <= II)
		goto IILOOP;

/*
C
C      Stop benchmark timing at this point.
C
*/
	elapsedusec = GetUsec() - startusec;
	finisec = time(0);

/*
C----------------------------------------------------------------
C      Performance in Whetstone KIP's per second is given by
C
C	(100*LOOP*II)/TIME
C
C      where TIME is in seconds.
C--------------------------------------------------------------------
*/
	printf("\n");
	if (finisec-startsec <= 0) {
		printf("Insufficient duration- Increase the LOOP count\n");
		return(1);
	}

/*
	printf("Loops: %ld, Iterations: %d, Duration: %ld sec.\n",
			LOOP, II, finisec-startsec);

	KIPS = (100.0*LOOP*II)/(float)(finisec-startsec);
	if (KIPS >= 1000.0)
		printf("C Converted Double Precision Whetstones: %.1f MIPS\n", KIPS/1000.0);
	else
		printf("C Converted Double Precision Whetstones: %.1f KIPS\n", KIPS);
*/

	printf("Loops: %ld, Iterations: %d, Duration: %.3f sec.\n",
			LOOP, II, elapsedusec / 1000000.0);
	printf("C Converted Double Precision Whetstones: %.0f MIPS\n", (100000.0*LOOP*II) / elapsedusec);

	if (continuous)
		goto LCONT;

	return(0);
}

void
PA(double E[])
{
	J = 0;

L10:
	E[1] = ( E[1] + E[2] + E[3] - E[4]) * T;
	E[2] = ( E[1] + E[2] - E[3] + E[4]) * T;
	E[3] = ( E[1] - E[2] + E[3] + E[4]) * T;
	E[4] = (-E[1] + E[2] + E[3] + E[4]) / T2;
	J += 1;

	if (J < 6)
		goto L10;
}

void
P0(void)
{
	E1[J] = E1[K];
	E1[K] = E1[L];
	E1[L] = E1[J];
}

void __attribute__ ((noinline))	/* dsites */
P3(double X, double Y, double *Z)
{
	double X1, Y1;

	X1 = X;
	Y1 = Y;
	X1 = T * (X1 + Y1);
	Y1 = T * (X1 + Y1);
	*Z  = (X1 + Y1) / VT2;
}

// #ifdef PRINTOUT
#if 1
void
POUT(long N, long J, long K, double X1, double X2, double X3, double X4)
{
	printf("%7ld %7ld %7ld %12.4e %12.4e %12.4e %12.4e\n",
						N, J, K, X1, X2, X3, X4);
}
#endif
