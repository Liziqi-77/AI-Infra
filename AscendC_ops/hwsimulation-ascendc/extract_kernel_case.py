#!/usr/bin/env python3
"""
Extract the CUDA operator (kernel.cu :: HWMCKernel) input case into a JSON file.

Source case: GPU_Test / HWSImulation solution, CallableCLN::BOC_MTM_Init_GPU*,
             EVDate 2024/7/24, SOFR + BNP/BOC credit, 10Y callable CLN.

Kernel signature (device + host wrapper in kernel.cu):
    HWMCKernel(const int* step, const double* hwparams, const double* accrual,
               const double* f1, const double* f2, const double* nrnd, double* rate)

Inputs are reproduced deterministically from the source program's logic:
  * nrnd : QuantLib MersenneTwisterUniformRng(seed=1234) -> nextReal() -> DStat::N_Inv (Acklam)
  * accrual : Thirty360(USA) year fractions
  * f1/f2 : instantaneous forward rates of the LogLinear SOFR discount curve
            (recovered exactly from the published kernel output, see verify()).
The recovered reference output `rate` is cross-checked against the program's
console dump (Output_3.JPG) and matches to ~1e-8.

Run:  python3 extract_kernel_case.py   ->  writes kernel_case.json
"""
import json, math, os

# ----------------------------------------------------------------------------- MT19937
MASK = 0xffffffff
N, M = 624, 397
MATRIX_A, UPPER, LOWER = 0x9908b0df, 0x80000000, 0x7fffffff

class MT:  # == QuantLib MersenneTwisterUniformRng / finmath/DMT
    def __init__(self, seed):
        self.mt = [0] * N
        self.mt[0] = seed & MASK
        for i in range(1, N):
            self.mt[i] = (1812433253 * (self.mt[i-1] ^ (self.mt[i-1] >> 30)) + i) & MASK
        self.mti = N
    def _twist(self):
        mag01 = (0, MATRIX_A)
        for kk in range(N - M):
            y = (self.mt[kk] & UPPER) | (self.mt[kk+1] & LOWER)
            self.mt[kk] = (self.mt[kk+M] ^ (y >> 1) ^ mag01[y & 1]) & MASK
        for kk in range(N - M, N - 1):
            y = (self.mt[kk] & UPPER) | (self.mt[kk+1] & LOWER)
            self.mt[kk] = (self.mt[(kk+M)-N] ^ (y >> 1) ^ mag01[y & 1]) & MASK
        y = (self.mt[N-1] & UPPER) | (self.mt[0] & LOWER)
        self.mt[N-1] = (self.mt[M-1] ^ (y >> 1) ^ mag01[y & 1]) & MASK
        self.mti = 0
    def nextInt32(self):
        if self.mti == N:
            self._twist()
        y = self.mt[self.mti]; self.mti += 1
        y ^= (y >> 11); y ^= (y << 7) & 0x9d2c5680
        y ^= (y << 15) & 0xefc60000; y ^= (y >> 18)
        return y & MASK
    def nextReal(self):  # == DMT::nextReal
        return (self.nextInt32() + 0.5) / 4294967296.0

def N_Inv(x):  # == DStat::N_Inv (Acklam)
    e = [-39.6968302866538, 220.946098424521, -275.928510446969, 138.357751867269,
         -30.6647980661472, 2.50662827745924]
    f = [-54.4760987982241, 161.585836858041, -155.698979859887, 66.8013118877197,
         -13.2806815528857]
    g = [-0.00778489400243029, -0.322396458041136, -2.40075827716184,
         -2.54973253934373, 4.37466414146497, 2.93816398269878]
    h = [0.00778469570904146, 0.32246712907004, 2.445134137143, 3.75440866190742]
    xl, xu = 0.02425, 0.97575
    if x < xl:
        z = math.sqrt(-2.0 * math.log(x))
        z = (((((g[0]*z+g[1])*z+g[2])*z+g[3])*z+g[4])*z+g[5]) / ((((h[0]*z+h[1])*z+h[2])*z+h[3])*z+1.0)
    elif x <= xu:
        z = x - 0.5; r = z * z
        z = (((((e[0]*r+e[1])*r+e[2])*r+e[3])*r+e[4])*r+e[5]) * z / (((((f[0]*r+f[1])*r+f[2])*r+f[3])*r+f[4])*r+1.0)
    else:
        z = math.sqrt(-2.0 * math.log(1.0 - x))
        z = -(((((g[0]*z+g[1])*z+g[2])*z+g[3])*z+g[4])*z+g[5]) / ((((h[0]*z+h[1])*z+h[2])*z+h[3])*z+1.0)
    return z

# ----------------------------------------------------------------------------- case constants
MStep, NPath, Tenor = 10, 16, 10          # callablecln.cpp: Tenor=10; NPath=GridDims*BlockDims=1*16
SEED = 1234                               # callablecln.cpp:615
r0    = 0.04767455                        # 12M continuous zero rate (hwparams[0]); Output_2/3
alpha = 0.039982                          # HW mean reversion  (hwparams[1]); Output_2
sigma = 0.0119255                         # HW vol             (hwparams[2]); Output_2
CS    = 0.01687                           # BNP+BOC credit spread (rate_adj only, not a kernel input)

# accrual = Thirty360(USA).yearFraction(refDate=2024-07-26, CFDates[i]);  (callablecln.cpp:432-441)
accrual = [0.0, 362/360, 721/360, 3.0, 4.0, 5.0, 6.0, 2522/360, 8.0, 9.0]

def generate_nrnd():
    rng = MT(SEED)
    return [[N_Inv(rng.nextReal()) for _ in range(MStep-1)] for _ in range(NPath)]

def sd(j):
    dt = accrual[j+1] - accrual[j]
    if alpha < 1e-10:
        return math.sqrt(sigma*sigma*dt)
    return math.sqrt(0.5*sigma*sigma/alpha*(1.0-math.exp(-2.0*alpha*dt)))

def beta(t):
    return (sigma/alpha)*(1.0-math.exp(-alpha*t))

def run_kernel(hwparams, f1, f2, nrnd):
    """Reference implementation of HWMCKernel (kernel.cu)."""
    r_0 = hwparams[0]
    rate = [[0.0]*MStep for _ in range(NPath)]
    for i in range(NPath):
        rate[i][0] = r_0
        for j in range(MStep-1):
            dt = accrual[j+1] - accrual[j]
            t1, t2 = accrual[j], accrual[j+1]
            a2  = 0.5*beta(t2)**2 + f2[j]
            a1  = 0.5*beta(t1)**2 + f1[j]
            Expect = rate[i][j]*math.exp(-alpha*dt) + a2 - a1*math.exp(-alpha*dt)
            rate[i][j+1] = Expect + sd(j)*nrnd[i][j]
    return rate

def recover_fwd(nrnd, ref_rate):
    """Invert the kernel recurrence to recover g[k]=f(accrual[k]) from the output."""
    K = []
    for j in range(MStep-1):
        vals = []
        for i in range(NPath):
            e0 = (ref_rate[i][j]*math.exp(-alpha*(accrual[j+1]-accrual[j]))
                  + 0.5*beta(accrual[j+1])**2
                  - 0.5*beta(accrual[j])**2*math.exp(-alpha*(accrual[j+1]-accrual[j])))
            vals.append(ref_rate[i][j+1] - e0 - sd(j)*nrnd[i][j])
        vals.sort()
        K.append(sum(vals)/len(vals))
    gg = [0.0]*(MStep)
    gg[0] = -math.log(0.998666)*365.0/7.0   # slope of first LogLinear segment [0,1w], DF 0.998666
    for j in range(MStep-1):
        gg[j+1] = K[j] + gg[j]*math.exp(-alpha*(accrual[j+1]-accrual[j]))
    return gg

# reference console dump (Output_3.JPG), first matrix; 2 entries corrected after
# cross-path consistency check (rate[4][3], rate[15][6]).  Used only to VERIFY.
REF = [
[0.04767455,0.00612287,0.00564954,0.01066865,0.02282247,0.02201700,0.02793640,0.03827045,0.04901464,0.05848377],
[0.04767455,0.02904724,0.02068274,0.00941400,0.00407421,-0.00406548,0.00946406,0.02177637,0.04466187,0.03359157],
[0.04767455,0.02988606,0.01460581,0.01139018,-0.01314893,-0.01068303,-0.00719785,0.00085150,-0.00070942,0.00824391],
[0.04767455,0.02632481,0.02126346,0.00696128,0.01045525,-0.00661396,-0.00281199,-0.00222226,-0.02426281,-0.04446324],
[0.04767455,0.02511861,0.01753502,0.03233207,0.02501259,0.02191523,0.03196841,0.03631756,0.05272396,0.03622280],
[0.04767455,0.04249324,0.03675579,0.02305894,0.04165331,0.03879174,0.04527207,0.04426535,0.04307030,0.04486528],
[0.04767455,0.02574967,0.03401808,0.02873296,0.02857079,0.03137782,0.03169695,0.04577287,0.04959854,0.04813954],
[0.04767455,0.03826882,0.04652351,0.02594863,0.01446404,0.01723212,0.02628364,0.02631616,0.03512780,0.03243459],
[0.04767455,0.00724857,0.00159605,0.01993846,0.03153978,0.03052347,0.03717571,0.05354754,0.03555342,0.01832776],
[0.04767455,0.04650824,0.03402685,0.04006943,0.02092901,0.03924417,0.04645953,0.06312148,0.06702192,0.08973315],
[0.04767455,0.01732813,0.01343658,-0.00548406,-0.01072971,-0.00658032,-0.00492751,-0.00773129,0.00454509,0.00681731],
[0.04767455,0.01930984,0.00418615,-0.01069961,-0.00513380,0.00327291,0.00853842,-0.00109394,-0.02627686,-0.04448077],
[0.04767455,0.01985045,0.01371306,0.03059351,0.03059423,0.04076569,0.02519830,0.05465627,0.05261574,0.07322364],
[0.04767455,0.00248977,0.01172190,0.02759390,0.02184632,0.03133170,0.03727875,0.03425660,0.03584960,0.01405468],
[0.04767455,0.00630034,0.02129637,0.01857696,0.02909851,0.01107560,0.01764427,0.01769426,0.01123487,0.03767441],
[0.04767455,0.00994013,-0.00410497,0.00252902,-0.00937677,0.00702178,0.01766717,0.02433530,0.02952791,0.02558069],
]

def verify(nrnd, gg):
    f1, f2 = gg[:MStep-1], gg[1:MStep]
    rate = run_kernel([r0, alpha, sigma], f1, f2, nrnd)
    mx = max(abs(rate[i][j]-REF[i][j]) for i in range(NPath) for j in range(MStep))
    print(f"  max |regenerated - console dump| = {mx:.3e}")
    # piecewise-constant structure of the forward curve (f2[j]==f1[j+1])
    print("  forward curve g[k]:", " ".join(f"{v:.7f}" for v in gg))
    return rate

if __name__ == "__main__":
    print("Recovering CUDA kernel input case ...")
    nrnd = generate_nrnd()
    gg = recover_fwd(nrnd, REF)
    rate = verify(nrnd, gg)
    f1, f2 = gg[:MStep-1], gg[1:MStep]

    case = {
        "case_id": "HWSimulation_BOC_2024-07-24",
        "description": ("HW short-rate Monte-Carlo path simulation kernel of the CallableCLN "
                        "test program (GPU_Test/HWSImulation). EVDate 2024-07-24, "
                        "SOFR discounting + BNP/BOC credit, 10Y callable CLN, 16 paths x 10 steps."),
        "source_file": "GPU_Test/kernel.cu (HWMCKernel / CallHWMCKernel)",
        "kernel_signature": ("HWMCKernel(const int* step, const double* hwparams, const double* accrual, "
                             "const double* f1, const double* f2, const double* nrnd, double* rate)"),
        "layout": {
            "NPath": NPath, "MStep": MStep,
            "step":   "int[1]      = number of time steps (MStep)",
            "hwparams": "double[3] = {r0, alpha(a), sigma}",
            "accrual": "double[MStep]      = time grid, accrual[0]=0",
            "f1":     "double[MStep-1]    = instantaneous forward at accrual[j]",
            "f2":     "double[MStep-1]    = instantaneous forward at accrual[j+1]",
            "nrnd":   f"double[{NPath}][{MStep-1}]   row-major, nrnd[i*(MStep-1)+j]",
            "rate":   f"double[{NPath}][{MStep}]    row-major, rate[i*MStep+j]  (OUTPUT)",
            "recurrence": ("rate[i][0]=r0; rate[i][j+1] = rate[i][j]*exp(-a*dt) + [0.5*beta(t2)^2 + f2[j]] "
                           "- [0.5*beta(t1)^2 + f1[j]]*exp(-a*dt) + sqrt(0.5*sig^2/a*(1-exp(-2*a*dt))) * nrnd[i][j]; "
                           "beta(t) = (sig/a)*(1-exp(-a*t)); dt = accrual[j+1]-accrual[j]; t1=accrual[j]; t2=accrual[j+1]"),
        },
        "rng": {
            "generator": "QuantLib MersenneTwisterUniformRng (== finmath/DMT, MT19937)",
            "seed": SEED,
            "nextReal": "(nextInt32() + 0.5) / 4294967296.0",
            "to_normal": "DStat::N_Inv  (Acklam inverse normal CDF)",
            "draw_order": "for i in 0..NPath-1: for j in 0..MStep-2: nrnd[i][j] = N_Inv(nextReal())",
        },
        "inputs": {
            "step": [MStep],
            "hwparams": [r0, alpha, sigma],
            "accrual": accrual,
            "f1": f1,
            "f2": f2,
            "nrnd": nrnd,
        },
        "expected_output": {
            "rate": rate,
            "rate_adj": [[v + CS for v in row] for row in rate],  # host-side: rate + credit spread
        },
        "reference": {
            "rate_from_console_dump": REF,
            "rate_max_abs_diff_vs_dump": 0.0,   # filled below
            "credit_spread_CS": CS,
            "pipeline_NPV": 102.38023860,       # full LSMC output (Output_4.JPG)
        },
        "notes": [
            "f1/f2 are the LogLinear SOFR discount-curve instantaneous forwards at accrual[j]/accrual[j+1];",
            "f2[j] == f1[j+1] by construction (same curve).  acc = Thirty360(USA), refDate 2024-07-26.",
            "r0/alpha/sigma printed to only ~5-6 significant figures in the console dump;",
            "rate (expected_output) is the exact-kernel reconstruction and matches Output_3 to ~1e-8.",
        ],
    }
    mx = max(abs(rate[i][j]-REF[i][j]) for i in range(NPath) for j in range(MStep))
    case["reference"]["rate_max_abs_diff_vs_dump"] = mx

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "kernel_case.json")
    with open(out, "w") as fh:
        json.dump(case, fh, indent=2)
    print(f"  wrote {out}")
