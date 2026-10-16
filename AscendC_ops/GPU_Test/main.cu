#include "callablecln.hpp"

#include "cuda_runtime.h"
#include "device_launch_parameters.h"

#include <iostream>

using namespace std;

int main()
{
    std::cout << "\n====================================== Test Begin =======================================" << std::endl;

    CallableCLN::BOC_MTM_Init_GPU_1();

    std::cout << "\n======================================= Test End ========================================" << std::endl;

    return 0;
}
