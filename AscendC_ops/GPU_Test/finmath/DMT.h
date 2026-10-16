#ifndef DFinMath_DMT_h
#define DFinMath_DMT_h

#pragma once

#include <vector>

namespace DFinMath
{
	//! Uniform random number generator
	/*! Mersenne Twister random number generator of period 2**19937-1

	For more details see http://www.math.keio.ac.jp/matumoto/emt.html

	\test the correctness of the returned values is tested by
	checking them against known good results.
	*/
	class DMT
	{
	private:
		static const int N = 624; // state size
		static const int M = 397; // shift size

	public:
		/*! if the given seed is 0, a random seed will be chosen based on clock() */
		explicit DMT(unsigned long seed = 1234);
		explicit DMT(const std::vector<unsigned long>& seeds);
		
		//! return a random number in the (0.0, 1.0)-interval
		double nextReal() const 
		{
			return (double(nextInt32()) + 0.5) / 4294967296.0;
		}
		
		//! return a random integer in the [0,0xffffffff]-interval
		unsigned long nextInt32() const 
		{
			if (mti == N)
				twist(); /* generate N words at a time */

			unsigned long y = mt[mti++];

			/* Tempering */
			y ^= (y >> 11);
			y ^= (y << 7) & 0x9d2c5680UL;
			y ^= (y << 15) & 0xefc60000UL;
			y ^= (y >> 18);
			return y;
		}

	private:
		void seedInitialization(unsigned long seed);
		void twist() const;
		mutable unsigned long mt[N];
		mutable int mti;
		static const unsigned long MATRIX_A, UPPER_MASK, LOWER_MASK;
	};

}

#endif