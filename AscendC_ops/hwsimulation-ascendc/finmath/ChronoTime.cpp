#include <ql/finmath/ChronoTime.hpp>

#include <time.h>
#include <stdio.h>
#include <chrono>

using namespace std;
using namespace std::chrono;

std::chrono::nanoseconds now()
{
	system_clock::duration d = system_clock::now().time_since_epoch();
	nanoseconds nan = duration_cast<nanoseconds>(d);

	return nan;
}

long long Chrono_Time()
{
	nanoseconds beg1 = now();

	return beg1.count();
}

ChronoTime::ChronoTime()
{
	stamptime = Chrono_Time();
}

