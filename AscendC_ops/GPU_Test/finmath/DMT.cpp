#include <ql/finmath/DMT.h>

namespace DFinMath
{
	// constant vector a
	const unsigned long DMT::MATRIX_A = 0x9908b0dfUL;
	// most significant w-r bits
	const unsigned long DMT::UPPER_MASK = 0x80000000UL;
	// least significant r bits
	const unsigned long DMT::LOWER_MASK = 0x7fffffffUL;

	DMT::DMT(unsigned long seed)
	{
		seedInitialization(seed);
	}

	void DMT::seedInitialization(unsigned long seed)
	{
		/* initializes mt with a seed */
		unsigned long s = seed;
		mt[0] = s & 0xffffffffUL;
		for (mti = 1; mti<N; mti++) 
		{
			mt[mti] =
				(1812433253UL * (mt[mti - 1] ^ (mt[mti - 1] >> 30)) + mti);
			/* See Knuth TAOCP Vol2. 3rd Ed. P.106 for multiplier. */
			/* In the previous versions, MSBs of the seed affect   */
			/* only MSBs of the array mt[].                        */
			/* 2002/01/09 modified by Makoto Matsumoto             */
			mt[mti] &= 0xffffffffUL;
			/* for >32 bit machines */
		}
	}

	DMT::DMT(const std::vector<unsigned long>& seeds)
	{
		seedInitialization(19650218UL);
		int i = 1, j = 0, k = (N>seeds.size() ? N : seeds.size());
		for (; k != 0U; k--) 
		{
			mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525UL))
				+ seeds[j] + j; /* non linear */
			mt[i] &= 0xffffffffUL; /* for WORDSIZE > 32 machines */
			i++; j++;
			if (i >= N) { mt[0] = mt[N - 1]; i = 1; }
			if (j >= seeds.size()) j = 0;
		}
		for (k = N - 1; k != 0U; k--) 
		{
			mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941UL))
				- i; /* non linear */
			mt[i] &= 0xffffffffUL; /* for WORDSIZE > 32 machines */
			i++;
			if (i >= N) { mt[0] = mt[N - 1]; i = 1; }
		}

		mt[0] = UPPER_MASK; /*MSB is 1; assuring non-zero initial array*/
	}

	void DMT::twist() const
	{
		static const unsigned long mag01[2] = { 0x0UL, MATRIX_A };
		/* mag01[x] = x * MATRIX_A  for x=0,1 */
		int kk;
		unsigned long y;

		for (kk = 0; kk<N - M; kk++) {
			y = (mt[kk] & UPPER_MASK) | (mt[kk + 1] & LOWER_MASK);
			mt[kk] = mt[kk + M] ^ (y >> 1) ^ mag01[y & 0x1UL];
		}
		for (; kk<N - 1; kk++) {
			y = (mt[kk] & UPPER_MASK) | (mt[kk + 1] & LOWER_MASK);
			mt[kk] = mt[(kk + M) - N] ^ (y >> 1) ^ mag01[y & 0x1UL];
		}
		y = (mt[N - 1] & UPPER_MASK) | (mt[0] & LOWER_MASK);
		mt[N - 1] = mt[M - 1] ^ (y >> 1) ^ mag01[y & 0x1UL];

		mti = 0;
	}

}
