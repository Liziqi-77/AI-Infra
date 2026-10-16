#include "cuda_runtime.h"
#include "device_launch_parameters.h"
#include <math.h>

//hparams - r0 - a - sigma
__global__ void HWMCKernel(const int* step, const double* hwparams, const double* accrual,
    const double* f1, const double* f2, const double* nrnd, double* rate)
{
    int i = blockDim.x * blockIdx.x + threadIdx.x;

    double r0 = hwparams[0];
    double a = hwparams[1];
    double sig = hwparams[2];

    double t1, t2;
    double dt, a_dt, sig2;
    double a2, sig2_a2;
    double df_a_dt, sig_factor;
    double F1, F2;
    double Variance;
    double Expect, StdDev;

    int MStep = step[0];
    rate[i * MStep + 0] = r0;
    for (int j = 0; j < MStep - 1; j++)
    {
        dt = accrual[j + 1] - accrual[j];
        a_dt = a * dt;
        sig2 = sig * sig;
        a2 = a * a;
        sig2_a2 = sig2 / a2;
        df_a_dt = exp(-a_dt);
        sig_factor = sig / sqrt(2 * a) * sqrt(1 - exp(-2 * a * dt));

        t1 = accrual[j];
        t2 = accrual[j + 1];
        F1 = f1[j]; 
        F2 = f2[j]; 

        if (a < 0.0000000001)
        {
            // algebraic limit for small speed
            Variance = sig * sig * dt;
        }
        else
        {
            Variance = 0.5 * sig * sig / a * (1.0 - exp(-2.0 * a * dt));
        }
        StdDev = sqrt(Variance);

        double alpha_2 = (sig / a) * (1.0 - exp(-a * t2));
        alpha_2 = alpha_2 * 0.5 * alpha_2;
        alpha_2 = alpha_2 + F2;

        double alpha_1 = (sig / a) * (1.0 - exp(-a * t1));
        alpha_1 = alpha_1 * 0.5 * alpha_1;
        alpha_1 = alpha_1 + F1;

        Expect = rate[i * MStep + j] * exp(-a * dt) + alpha_2 - alpha_1 * exp(-a * dt);

        rate[i * MStep + j + 1] = Expect + StdDev * nrnd[i * (MStep - 1) + j];

        //rate[i * MStep + j + 1] = rate[i * MStep + j] * df_a_dt + F2 - df_a_dt * F1
        //    + sig2_a2 * exp(-a * t2) * (cosh(a * t2) - cosh(a * t1))
        //    + sig_factor * nrnd[i * (MStep - 1) + j];
    }
}


void CallHWMCKernel(int* d_step, double* d_hwparams, double* d_accrual, 
    double* d_f1, double* d_f2, double* d_nrnd, double* d_rate)
{
    HWMCKernel << <1, 16 >> > (d_step, d_hwparams, d_accrual, d_f1, d_f2, d_nrnd, d_rate);

    return;
}


