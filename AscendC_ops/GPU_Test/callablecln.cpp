#include "callablecln.hpp"

#include "cuda_runtime.h"
#include "device_launch_parameters.h"

//#include "kernel.cu"

#include <ql/cashflows/iborcoupon.hpp>
#include <ql/finmath/ChronoTime.hpp>
#include <ql/finmath/DStat.h>
#include <ql/finmath/DMT.h>
#include <ql/handle.hpp>
#include <ql/instruments/makeois.hpp>
#include <ql/instruments/bonds/fixedratebond.hpp>
#include <ql/indexes/ibor/sofr.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/math/matrix.hpp>
#include <ql/math/randomnumbers/mt19937uniformrng.hpp>
#include <ql/math/optimization/simplex.hpp>
#include <ql/math/optimization/levenbergmarquardt.hpp>
#include <ql/models/shortrate/onefactormodels/hullwhite.hpp>
#include <ql/models/shortrate/calibrationhelpers/swaptionhelper.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/pricingengines/swaption/jamshidianswaptionengine.hpp>
#include <ql/processes/hullwhiteprocess.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/credit/flathazardrate.hpp>
#include <ql/termstructures/credit/interpolatedhazardratecurve.hpp>
#include <ql/termstructures/yield/discountcurve.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/termstructures/yield/zerospreadedtermstructure.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/calendars/unitedstates.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/thirty360.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/time/schedule.hpp>
#include <ql/utilities/dataformatters.hpp>
#include <ql/utilities/utils.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/normal_distribution.hpp>
#include <boost/random/variate_generator.hpp>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream> // Required header

using namespace DFinMath; // use DFinMath classes
using namespace QuantLib;
using namespace std;


void CallHWMCKernel(int* d_step, double* d_hwparams, double* d_accrual, double* d_f1, double* d_f2, double* d_nrnd, double* d_rate);

namespace ccln_test
{

    struct SofrQuotes {
        Frequency freq;
        Month month; // start month
        Year year;
        Real price;
        RateAveraging::Type averagingMethod;
    };

    struct Datum {
        Integer settlementDays;
        Integer n;
        TimeUnit unit;
        Rate rate;
    };

    struct CalibrationData
    {
        Integer start;
        Integer length;
        Volatility volatility;
    };

    struct CommonVars 
    {
        // global data
        Date today, settlement;
        Swap::Type type;
        Real nominal;
        Calendar calendar;
        Natural settlementDays;
        Real init1YRate;

        Period fixedSofrPeriod, floatingSofrPeriod;
        DayCounter fixedSofrDayCount;
        BusinessDayConvention fixedSofrConvention, floatingSofrConvention;
        ext::shared_ptr<Sofr> sofrIndex = ext::make_shared<Sofr>();
        RelinkableHandle<YieldTermStructure> sofrTermStructure;

        Frequency fixedSwapFrequency;
        DayCounter fixedSwapDayCount;
        BusinessDayConvention fixedSwapConvention;
        ext::shared_ptr<IborIndex> swapIndex;
        RelinkableHandle<YieldTermStructure> swapTermStructure;

        // utilities
        ext::shared_ptr<OvernightIndexedSwap> makeSwap(Period length, Rate fixedRate,
            Spread spread, bool telescopicValueDates,
            Date effectiveDate = Null<Date>(), Integer paymentLag = 0,
            RateAveraging::Type averagingMethod = RateAveraging::Compound)
        {
            return MakeOIS(length, sofrIndex, fixedRate, 0 * Days)
                .withEffectiveDate(effectiveDate == Null<Date>() ? settlement : effectiveDate)
                .withOvernightLegSpread(spread)
                .withNominal(nominal)
                .withPaymentLag(paymentLag)
                .withDiscountingTermStructure(sofrTermStructure)
                .withTelescopicValueDates(telescopicValueDates)
                .withAveragingMethod(averagingMethod);
        }

        CommonVars() 
        {
            type = Swap::Payer;
            settlementDays = 0;
            nominal = 100.0;
            init1YRate = 0.05;

            fixedSofrConvention = ModifiedFollowing;
            floatingSofrConvention = ModifiedFollowing;
            fixedSofrPeriod = 1 * Years;
            floatingSofrPeriod = 1 * Years;
            fixedSofrDayCount = Actual360();
            sofrIndex = ext::make_shared<Sofr>(sofrTermStructure);

            fixedSwapConvention = ModifiedFollowing;
            fixedSwapFrequency = Annual;
            fixedSwapDayCount = Thirty360(Thirty360::BondBasis);
            //swapIndex = ext::shared_ptr<IborIndex>(new Euribor3M(swapTermStructure));
            calendar = sofrIndex->fixingCalendar(); 
            //UnitedStates(Market::SOFR);

            //today = Date(1, Aug, 2024);
            //today = calendar.adjust(Date::todaysDate());
            //Settings::instance().evaluationDate() = today;
            //settlement = calendar.advance(today, settlementDays * Days, Following);
            //sofrTermStructure.linkTo(flatRate(today, 0.05, Actual365Fixed()));
        }
    };

}

// 16 path, Random
void CallableCLN::BOC_MTM_Init_GPU_1()
{
    std::cout << "\nCalibrate HW Model EVDate: 2024/7/24 from Swaptions Vol." << std::endl;

    using namespace ccln_test;

    // ================================================================
    // ------------------- Part I Zeros Data --------------------------    
    // 2024/7/24, 15
    Datum zerosData[] = {
        { 2,  1, Weeks,  0.0535192 },
        { 2,  1, Months, 0.0534510 },
        { 2,  3, Months, 0.0526580 },
        { 2,  6, Months, 0.0511680 },

        { 2,  1,  Years, 0.0478620 },
        { 2,  2,  Years, 0.0428125 },
        { 2,  3,  Years, 0.0404440 },
        { 2,  5,  Years, 0.0386505 },
        { 2,  7,  Years, 0.0382700 },
        { 2, 10,  Years, 0.0383450 },

        { 2, 15,  Years, 0.0388155 },
        { 2, 20,  Years, 0.0387270 },
        { 2, 30,  Years, 0.0371900 },
        { 2, 40,  Years, 0.0352250 },
        { 2, 50,  Years, 0.0331210 }
    };

    // 2024/7/24, 16
    Datum dfsData[] = {
        { 2,  0, Days,   1.0 },
        { 2,  1, Weeks,  0.998666 },
        { 2,  1, Months, 0.995123 },
        { 2,  3, Months, 0.986144 },
        { 2,  6, Months, 0.974090 },

        { 2,  1,  Years, 0.953195 },
        { 2,  2,  Years, 0.918348 },
        { 2,  3,  Years, 0.886603 },
        { 2,  5,  Years, 0.825624 },
        { 2,  7,  Years, 0.766551 },
        { 2, 10,  Years, 0.683292 },

        { 2, 15,  Years, 0.559829 },
        { 2, 20,  Years, 0.462565 },
        { 2, 30,  Years, 0.337203 },
        { 2, 40,  Years, 0.266119 },
        { 2, 50,  Years, 0.227807 }
    };

    CommonVars vars;
    vars.today = Date(24, July, 2024); // Date(24, July, 2024);
    Date evDate = vars.today;
    vars.settlementDays = 2;

    Calendar Cal = vars.calendar;
    Date settlement = Cal.advance(evDate, vars.settlementDays * Days, Following);
    Date refDate = settlement;

    Settings::instance().evaluationDate() = evDate;

    std::cout << "Evaluation Date: " << evDate << std::endl;
    std::cout << "Settlement Date: " << settlement << std::endl;
    std::cout << "Reference Date: " << refDate << std::endl;

    ext::shared_ptr<Sofr> sofrIndex = ext::make_shared<Sofr>();
    Actual365Fixed DC365;
    Actual360 DC360;

    bool telescopicValueDates = false;
    RateAveraging::Type averagingMethod = RateAveraging::Type::Compound;
    double tolerance = 1.0e-8;

    std::vector<Date> vDFDate;
    for (int i = 0; i < 16; i++)
    {
        vDFDate.push_back(Cal.advance(settlement, dfsData[i].n * dfsData[i].unit));
    }

    std::vector<Real> vDF;
    for (int i = 0; i < 16; i++)
    {
        vDF.push_back(dfsData[i].rate);
    }

    // original
    Handle<YieldTermStructure> h_sofrDFTS_org(ext::shared_ptr<YieldTermStructure>(
        new InterpolatedDiscountCurve<LogLinear>(vDFDate, vDF, DC365, Cal)));

    // BNP => 68.9 bp, BOC => 99.8 bp, Total => 168.7 bp
    Real CS = 0.01687;
    //ext::shared_ptr<Quote> me(new SimpleQuote(CS));
    //Handle<Quote> mh(me);
    //Handle<YieldTermStructure> h_sofrDFTS(ext::shared_ptr<YieldTermStructure>(new ZeroSpreadedTermStructure(h_sofrDFTS_org, mh)));

    Date d3m = Cal.advance(settlement, Period(3, Months));
    Date d6m = Cal.advance(settlement, Period(6, Months));
    Date d9m = Cal.advance(settlement, Period(9, Months));
    Date d12m = Cal.advance(settlement, Period(12, Months));

    Compounding Comp = Compounding::Continuous;
    DayCounter DC = DC365;

    double depo3M = h_sofrDFTS_org->zeroRate(d3m, DC, Comp, Annual).rate();
    double depo6M = h_sofrDFTS_org->zeroRate(d6m, DC, Comp, Annual).rate();
    double depo9M = h_sofrDFTS_org->zeroRate(d9m, DC, Comp, Annual).rate();
    double depo12M = h_sofrDFTS_org->zeroRate(d12m, DC, Comp, Annual).rate();

    vars.init1YRate = h_sofrDFTS_org->zeroRate(d12m, DC365, Continuous, Annual).rate();

    std::cout << "\n==================================" << std::endl;
    std::cout << "Part I Sofr Curve Output" << std::endl;
    std::cout << "-------------------------" << std::endl;
    std::cout << " 3M Date: " << d3m << std::endl;
    std::cout << " 6M Date: " << d6m << std::endl;
    std::cout << " 9M Date: " << d9m << std::endl;
    std::cout << "12M Date: " << d12m << std::endl;

    std::cout << "-------------------------" << std::endl;
    std::cout << " 3M Zero Rate: " << depo3M << std::endl;
    std::cout << " 6M Zero Rate: " << depo6M << std::endl;
    std::cout << " 9M Zero Rate: " << depo9M << std::endl;
    std::cout << "12M Zero Rate: " << depo12M << std::endl;

    double Df3M = h_sofrDFTS_org->discount(d3m);
    double Df6M = h_sofrDFTS_org->discount(d6m);
    double Df9M = h_sofrDFTS_org->discount(d9m);
    double Df12M = h_sofrDFTS_org->discount(d12m);

    std::cout << "-------------------------" << std::endl;
    std::cout << " 3M DF: " << Df3M << std::endl;
    std::cout << " 6M DF: " << Df6M << std::endl;
    std::cout << " 9M DF: " << Df9M << std::endl;
    std::cout << "12M DF: " << Df12M << std::endl;

    std::cout << "-------------------------" << std::endl;
    std::cout << "12M DF: " << Df12M << std::endl;
    std::cout << "12M Cont. Zero Rate: " << vars.init1YRate << std::endl;
    std::cout << "=================================" << std::endl;


    // =====================================================================
    // ----------------- Part II Swaption Vol Data 2024/7/24 -------------------
    std::cout << "\nPart II Calibration Output" << std::endl;

    CalibrationData data[] =
    {

        {  1, 10, 0.2785 }, //{  1, 30, 0.2659 },  //{ 1, 30, 0.2697 },
        {  3,  7, 0.2802 }, //{  5, 25, 0.2525 },  //{ 5, 25, 0.2503 },
        {  5,  5, 0.2700 }, //{ 10, 20, 0.2310 },  //{ 10, 20, 0.2302 },
        {  7,  3, 0.2605 }, //{ 15, 15, 0.2272 },  //{ 15, 15, 0.2242 },
        { 10,  1, 0.2433 }  //{ 20, 10, 0.2270 },  //{ 20, 10, 0.2296 },
    };

    bool usingAtParCoupons = IborCoupon::Settings::instance().usingAtParCoupons();

    //Date settlement(vars.settlement);
    //Handle<YieldTermStructure> termStructure(sofrTS);

    ext::shared_ptr<HullWhite> model(new HullWhite(h_sofrDFTS_org));
    ext::shared_ptr<Sofr> sofr3(new Sofr(h_sofrDFTS_org));
    ext::shared_ptr<PricingEngine> engine(new JamshidianSwaptionEngine(model));

    std::cout << "  Step_1, Setup Term Structure." << std::endl;

    std::vector<ext::shared_ptr<CalibrationHelper> > swaptions;
    for (auto& i : data)
    {
        ext::shared_ptr<Quote> vol(new SimpleQuote(i.volatility));
        ext::shared_ptr<BlackCalibrationHelper> helper(new SwaptionHelper(
            Period(i.start, Years), Period(i.length, Years), Handle<Quote>(vol),
            sofr3, Period(1, Years), Actual360(), Actual360(), h_sofrDFTS_org));

        helper->setPricingEngine(engine);
        swaptions.push_back(helper);
    }

    std::cout << "  Step_2, Setup Swaptions Data." << std::endl;
    //--Set up the optimization problem
    //--    Real simplexLambda = 0.1;
    //--    Simplex optimizationMethod(simplexLambda);

    LevenbergMarquardt optimizationMethod(1.0e-8, 1.0e-8, 1.0e-8);
    EndCriteria endCriteria(10000, 100, 1e-6, 1e-8, 1e-8);

    //--Optimize
    ChronoTime begTime;
    model->calibrate(swaptions, optimizationMethod, endCriteria);
    ChronoTime endTime;

    EndCriteria::Type ecType = model->endCriteria();

    std::cout << "  Step_3, Calibration Done." << std::endl;

    tolerance = 1.0e-5;
    Array xMinCalculated = model->params();
    Real yMinCalculated = model->value(xMinCalculated, swaptions); // 0.0

    std::cout << "  Step_4, Get Parameters." << std::endl;

    double alpha = xMinCalculated[0];  //0.0430427; //xMinCalculated[0]; // 0.0502329; 
    double sigma = xMinCalculated[1];  //0.00994729; //xMinCalculated[1]; // 0.0138362; 

    //--Array xMinExpected(2);
    //--xMinExpected[0] = cachedA;
    //--xMinExpected[1] = cachedSigma;    
    //--Real yMinExpected = model->value(xMinExpected, swaptions);

    std::cout << std::endl;
    std::cout << "------------------------------------" << std::endl;
    std::cout << "Calibration results:\n"
        << "                  a = " << alpha << ", \n"
        << "                sig = " << sigma << ", \n"
        << "          f(a, sig) = " << yMinCalculated << ", \n"
        << "       end criteria = " << ecType << std::endl;
    std::cout << "------------------------------------" << std::endl;
    std::cout << "12M Cont. Zero Rate = " << vars.init1YRate << std::endl;
    std::cout << "------------------------------------" << std::endl;


    // ================================================================
    // ------------------- Part III Credit Curve --------------------------    
    // 2024/7/24, 8
    Datum cdsData[] = {
        { 2,  0,  Months, 0.0 },
        { 2,  6,  Months, 0.00214 },
        { 2,  1,  Years,  0.00271 },
        { 2,  2,  Years,  0.00343 },
        { 2,  3,  Years,  0.00437 },
        { 2,  5,  Years,  0.00652 },
        { 2,  7,  Years,  0.00823 },
        { 2, 10,  Years,  0.00998 }
    };

    std::vector<Date> vCDSDate;
    for (int i = 0; i < 8; i++)
    {
        vCDSDate.push_back(Cal.advance(settlement, cdsData[i].n * cdsData[i].unit));
    }

    std::vector<Real> vCDS;
    for (int i = 0; i < 8; i++)
    {
        vCDS.push_back(cdsData[i].rate);
    }

    RelinkableHandle<DefaultProbabilityTermStructure> h_HazardTS;

    h_HazardTS.linkTo(ext::shared_ptr<DefaultProbabilityTermStructure>(
        new InterpolatedHazardRateCurve<BackwardFlat>(vCDSDate, vCDS, Thirty360(Thirty360::BondBasis))));

    //Thirty360 dc(Thirty360::USA);

    //-----------------------------------------------------------------------------------------------------

    int Tenor = 10;
    int FreezePeriod = 0; // no freeze period

    int MStep = Tenor;
    int NPath = 16; // 10240;

    Date startDate(26, Jul, 2024);
    std::cout << "Note Start Date: " << startDate << std::endl;

    vector<double> accrual(0);
    vector<Date> CFDates(0);
    for (int i = 0; i <= Tenor; i++)
    {
        //CFDates.push_back(Date(startDate.dayOfMonth(), startDate.month(), startDate.year() + i));
        CFDates.push_back(Cal.advance(startDate, i* Years));
    }

    Date aDate(vars.today);  // MStep = 30
    //Date EvaluationDate(aDate);

    Thirty360 dc(Thirty360::USA);
    accrual.push_back(0.0);
    for (int i = 1; i <= Tenor; i++)
    {
        double diffyear = dc.yearFraction(refDate, CFDates[i]);
        if (diffyear > 0)
        {
            accrual.push_back(diffyear);
        }
    }
    MStep = accrual.size() - 1;

    /*
    Real Q1Y = 1.0 - h_HazardTS->defaultProbability(evDate, Cal.advance(evDate, 1 * Years), true);
    Real Q2Y = 1.0 - h_HazardTS->defaultProbability(evDate, Cal.advance(evDate, 2 * Years), true);
    Real Q3Y = 1.0 - h_HazardTS->defaultProbability(evDate, Cal.advance(evDate, 3 * Years), true);
    Real Q4Y = 1.0 - h_HazardTS->defaultProbability(evDate, Cal.advance(evDate, 4 * Years), true);
    Real Q5Y = 1.0 - h_HazardTS->defaultProbability(evDate, Cal.advance(evDate, 5 * Years), true);
    Real Q6Y = 1.0 - h_HazardTS->defaultProbability(evDate, Cal.advance(evDate, 6 * Years), true);
    Real Q7Y = 1.0 - h_HazardTS->defaultProbability(evDate, Cal.advance(evDate, 7 * Years), true);
    Real Q8Y = 1.0 - h_HazardTS->defaultProbability(evDate, Cal.advance(evDate, 8 * Years), true);
    Real Q9Y = 1.0 - h_HazardTS->defaultProbability(evDate, Cal.advance(evDate, 9 * Years), true);
    Real Q10Y = 1.0 - h_HazardTS->defaultProbability(evDate, Cal.advance(evDate, 10 * Years), true);
    */

    /*
    std::vector<Real> vQ;
    vQ.push_back(1.0);
    vQ.push_back(Q1Y);
    vQ.push_back(Q2Y);
    vQ.push_back(Q3Y);
    vQ.push_back(Q4Y);
    vQ.push_back(Q5Y);
    vQ.push_back(Q6Y);
    vQ.push_back(Q7Y);
    vQ.push_back(Q8Y);
    vQ.push_back(Q9Y);
    vQ.push_back(Q10Y);
    */

    Real Q1Y = 1.0 - h_HazardTS->defaultProbability(refDate, CFDates[1], true);
    Real Q2Y = 1.0 - h_HazardTS->defaultProbability(refDate, CFDates[2], true);
    Real Q3Y = 1.0 - h_HazardTS->defaultProbability(refDate, CFDates[3], true);
    Real Q4Y = 1.0 - h_HazardTS->defaultProbability(refDate, CFDates[4], true);
    Real Q5Y = 1.0 - h_HazardTS->defaultProbability(refDate, CFDates[5], true);
    Real Q6Y = 1.0 - h_HazardTS->defaultProbability(refDate, CFDates[6], true);
    Real Q7Y = 1.0 - h_HazardTS->defaultProbability(refDate, CFDates[7], true);
    Real Q8Y = 1.0 - h_HazardTS->defaultProbability(refDate, CFDates[8], true);
    Real Q9Y = 1.0 - h_HazardTS->defaultProbability(refDate, CFDates[9], true);
    Real Q10Y = 1.0 - h_HazardTS->defaultProbability(refDate, CFDates[10], true);


    std::vector<Real> v_Q;
    v_Q.push_back(1.0);
    for (int i = 1; i <= Tenor; i++)
    {
        v_Q.push_back(1.0 - h_HazardTS->defaultProbability(refDate, CFDates[i], true));
    }

    for (int i = 0; i <= Tenor; i++)
    {
        //std::cout << i << " Year Survival Rate: " << v_Q[i] << std::endl;
    }

    std::cout << "\n==================================" << std::endl;
    std::cout << "Part III Credit Curve Output" << std::endl;
    std::cout << "-------------------------" << std::endl;
    std::cout << "1Y Survival Rate: " << Q1Y << std::endl;
    std::cout << "2Y Survival Rate: " << Q2Y << std::endl;
    std::cout << "3Y Survival Rate: " << Q3Y << std::endl;
    std::cout << "4Y Survival Rate: " << Q4Y << std::endl;
    std::cout << "5Y Survival Rate: " << Q5Y << std::endl;
    std::cout << "6Y Survival Rate: " << Q6Y << std::endl;
    std::cout << "7Y Survival Rate: " << Q7Y << std::endl;
    std::cout << "8Y Survival Rate: " << Q8Y << std::endl;
    std::cout << "9Y Survival Rate: " << Q9Y << std::endl;
    std::cout << "10Y Survival Rate: " << Q10Y << std::endl;


    // =====================================================================
    // ----------------- Part IV HW Simulation on 2024/7/24 ----------------

    // -------------------------- Process CF Dates -------------------------

    std::vector<double> ICV(Tenor + 1); //Initial Call Value, // contract input
    std::vector<double> CV(0);

    ICV[0] = -100.0;
    for (int i = 1; i <= Tenor; i++)
    {
        ICV[i] = 0.0;
    }

    //for (int i = FreezePeriod; i <= Tenor; i++)
    {
        ICV[1] = 107.79;
        ICV[2] = 116.37;
        ICV[3] = 124.95;
        ICV[4] = 133.53;
        ICV[5] = 142.11;
        ICV[6] = 150.69;
        ICV[7] = 159.27;
        ICV[8] = 167.85;
        ICV[9] = 176.43;
        ICV[10] = 185.01;
    }

    for (int j = Tenor - MStep; j <= Tenor; j++)
    {
        CV.push_back(ICV[j]);
    }

    if (accrual.size() < 2)
    {
        std::cout << "Issue Expire!" << std::endl;
        QL_FAIL("---Issue Expire!---");
    }

    //std::cout << "1.0" << std::endl;

    Date maturity = Date(26, July, 2034);
    //int BondTenor = 10;
    Period CpPeriod(1, Years);

    Calendar nuCal = NullCalendar();
    Date firstCouponDate = nuCal.advance(startDate, CpPeriod); // CFDates[1]; 
    Date lastCouponDate = nuCal.advance(startDate, 10 * CpPeriod); // CFDates[10];
    Frequency CouponFreq = CpPeriod.frequency();

    //create cash flows schedule    
    Schedule Sch = Schedule(firstCouponDate, lastCouponDate, CpPeriod, nuCal, Unadjusted, Unadjusted, DateGeneration::Forward, false);

    std::vector<Date> couponDates = Sch.dates(); // 10

    //double spread_dv01 = 0.01687;
    //Handle<Quote> h_spread(ext::make_shared<SimpleQuote>(spread_dv01));
    //Handle<YieldTermStructure> h_sofrDFTS_Shift(ext::make_shared<ZeroSpreadedTermStructure>(h_sofrDFTS_org, h_spread, Compounded, Annual, DC365));


    // -------------------- Process Simulation by GPU ----------------------
    int h_step[1];
    h_step[0] = MStep;
    double h_hwparams[3];
    double* h_accrual = new double[Tenor]; //[30];
    double* h_F1 = new double[Tenor - 1]; //[29];
    double* h_F2 = new double[Tenor - 1]; //[29];
    double* h_NRnd = new double[NPath * (MStep - 1)]; // [464] ;
    double* h_rate = new double[NPath * MStep]; //[480]

    h_hwparams[0] = vars.init1YRate;
    h_hwparams[1] = alpha;
    h_hwparams[2] = sigma;
    for (int i = 0; i < Tenor; i++)
    {
        h_accrual[i] = accrual[i];
    }

    for (int i = 0; i < 1; i++)
    {
        //rate[i][0] = r0;
        double t1, t2;
        double dt, a_dt, sig2;
        double a2, sig2_a2;
        double df_a_dt, sig_factor;
        double F1, F2;
        for (int j = 0; j < MStep - 1; j++)
        {
            t1 = accrual[j];
            t2 = accrual[j + 1];
            F1 = h_sofrDFTS_org->forwardRate(t1, t1, Compounding::Continuous, Frequency::NoFrequency).rate();
            F2 = h_sofrDFTS_org->forwardRate(t2, t2, Compounding::Continuous, Frequency::NoFrequency).rate();
            h_F1[j] = F1;
            h_F2[j] = F2;
        }
    }

    /*
    for (int i = 0; i < (NPath * (MStep - 1)); i++)
    {
        h_NRnd[i] = 0.0;
    }
    */

    MersenneTwisterUniformRng Rnd(1234);
    DStat aStat;
    for (int i = 0; i < NPath; i++)
    {
        for (int j = 0; j < MStep - 1; j++)
        {
            double NRnd = aStat.N_Inv(Rnd.nextReal());
            h_NRnd[i * (MStep - 1) + j] = NRnd;
        }
    }


    std::cout << "======================= Accrual ============================" << std::endl;
    for (int i = 0; i < Tenor; i++)
    {
        std::cout << h_accrual[i] << std::endl;
    }

    /*
    std::cout << "\n======================= F1 ============================" << std::endl;
    for (int i = 0; i < (Tenor - 1); i++)
    {
        std::cout << h_F1[i] << std::endl;
    }

    std::cout << "\n======================= F2 ============================" << std::endl;
    for (int i = 0; i < (Tenor - 1); i++)
    {
        std::cout << h_F2[i] << std::endl;
    }

    std::cout << "\n======================= NRnd ============================" << std::endl;
    for (int i = 0; i < (NPath * (MStep - 1)); i++)
    {
        std::cout << h_NRnd[i] << std::endl;
    }

    std::cout << "\n======================= Rate ============================" << std::endl;
    for (int i = 0; i < (NPath * MStep); i++)
    {
        h_rate[i] = 0.0;
        std::cout << h_rate[i] << std::endl;
    }
    */

    //CUdevice   device;
    //CUcontext  context;
    //CUmodule   module;
    //CUfunction function;

    //char* module_file = (char*)"kernel.cu.ptx";
    //char* kernel_name = (char*)"HWMCKernel";
    //char* kernel_name = (char*)"HWMCKernel_MT";

    //CUresult err = cuInit(0);
    //cuDeviceGet(&device, 0);
    //err = cuCtxCreate(&context, 0, device);
    //err = cuModuleLoad(&module, module_file);
    //err = cuModuleGetFunction(&function, module, kernel_name);


    // Choose which GPU to run on, change this on a multi-GPU system.
    int* d_step;
    double* d_hwparams;
    double* d_accrual;

    double* d_f1;
    double* d_f2;
    double* d_nrnd;
    double* d_rate;

    cudaMalloc((void **)&d_step, sizeof(int) * 1);
    cudaMalloc((void**)&d_hwparams, sizeof(double) * 3);
    cudaMalloc((void**)&d_accrual, sizeof(double) * Tenor); //30
    cudaMalloc((void**)&d_f1, sizeof(double) * (Tenor - 1)); //29
    cudaMalloc((void**)&d_f2, sizeof(double) * (Tenor - 1)); //29
    cudaMalloc((void**)&d_nrnd, sizeof(double) * NPath * (MStep - 1)); //464
    cudaMalloc((void**)&d_rate, sizeof(double) * NPath * MStep); //480

    cudaMemcpy(d_step, h_step, sizeof(int) * 1, cudaMemcpyHostToDevice);
    cudaMemcpy(d_hwparams, h_hwparams, sizeof(double) * 3, cudaMemcpyHostToDevice);
    cudaMemcpy(d_accrual, h_accrual, sizeof(double) * Tenor, cudaMemcpyHostToDevice);
    cudaMemcpy(d_f1, h_F1, sizeof(double) * (Tenor - 1), cudaMemcpyHostToDevice);
    cudaMemcpy(d_f2, h_F2, sizeof(double) * (Tenor - 1), cudaMemcpyHostToDevice);
    cudaMemcpy(d_nrnd, h_NRnd, sizeof(double) * NPath * (MStep - 1), cudaMemcpyHostToDevice);

    //void* args[7] = { &d_step, &d_hwparams, &d_accrual, &d_f1, &d_f2, &d_nrnd, &d_rate };
    //void* args[6] = { &d_step, &d_hwparams, &d_accrual, &d_f1, &d_f2, &d_rate };

    int GridDims = 1; //128 //64;
    int BlockDims = 16;

    NPath = GridDims * BlockDims;

    //cuLaunchKernel(function,
    //    GridDims, 1, 1,    // 1 x 1 x 1 blocks, 64 X 256 = 16384
    //    BlockDims, 1, 1,   // N x 1 x 1 threads
    //    0, 0,
    //    args, 0);

    CallHWMCKernel(d_step, d_hwparams, d_accrual, d_f1, d_f2, d_nrnd, d_rate);

    //HWMCKernel <<<GridDims, BlockDims>>> (d_step, d_hwparams, d_accrual, d_f1, d_f2, d_nrnd, d_rate);

    cudaMemcpy(h_rate, d_rate, sizeof(double) * NPath * MStep, cudaMemcpyDeviceToHost);

    /*
    std::cout << "\n======================= Rate ============================" << std::endl;
    for (int i = 0; i < (NPath * MStep); i++)
    {
        std::cout << i << " : " << h_rate[i] << std::endl;
    }
    */

    Real r0 = vars.init1YRate;

    std::cout << "\n==================================" << std::endl;
    std::cout << "Part IV GPU Simulation Output" << std::endl;
    std::cout << "---------------------------" << std::endl;
    std::cout << "HW Simulation results:\n"
        << "  Start Date: " << startDate << ", 1Y Later: " << d12m << ", \n"
        << "  Initial Short Rate(12M):  " << r0 << ", \n"
        << "  Alpha: " << alpha << ", \n"
        << "  Sigma: " << sigma << ", \n"
        << "  Credit Spread: " << CS << std::endl;

    Matrix rate(NPath, MStep);
    Matrix rate_adj(NPath, MStep);
    for (int i = 0; i < NPath; i++)
    {
        for (int j = 0; j < MStep; j++)
        {
            rate[i][j] = h_rate[i * MStep + j];
            rate_adj[i][j] = h_rate[i * MStep + j] + CS;
        }
    }


    std::cout << "\n----------------------------------------------------------------------" << std::endl;
    for (int i = 0; i < NPath; i++)
    {
        std::cout << "Path: " << setw(2) << i << ":";
        for (int j = 0; j < MStep; j++)
        {
            std::cout << std::setprecision(8) << setw(12) << fixed << rate[i][j];
        }
        std::cout << std::endl;
    }

    std::cout << "\n----------------------------------------------------------------------" << std::endl;
    for (int i = 0; i < NPath; i++)
    {
        std::cout << "Path: " << setw(2) << i << ":";
        for (int j = 0; j < MStep; j++)
        {
            std::cout << std::setprecision(8) << setw(12) << fixed << rate_adj[i][j];
        }
        std::cout << std::endl;
    }


    delete[] h_accrual;
    delete[] h_F1;
    delete[] h_F2;
    delete[] h_NRnd;
    delete[] h_rate;

    cudaFree(d_hwparams);
    cudaFree(d_accrual);
    cudaFree(d_f1);
    cudaFree(d_f2);
    cudaFree(d_nrnd);
    cudaFree(d_rate);


    // =====================================================================
    // ---------------------- Part V Bond Cash Flows -----------------------


    // =====================================================================
    // --------------------------- Part V LSMC -----------------------------


    Matrix MRate(NPath, MStep, 0.0); // 16 * 11, including InitRate, 0, 1, ..., 10
    Matrix MRate_adj(NPath, MStep, 0.0); // Add Credit Spread

    for (int i = 0; i < NPath; i++)
    {
        for (int j = 0; j < MStep; j++)
        {
            MRate[i][j] = rate[i][j];
            MRate_adj[i][j] = rate_adj[i][j];
        }
    }


    // =====================================================================
    // ---------------------- Part V Bond Cash Flows --------------------------

    double NPA = 100.0;
    double couponRate = 0.0; //no coupon, 0.0858% implied in Callable price
    double recoveryRate = 0.4;
    DayCounter cpDC = Actual365Fixed(); // Thirty360(Thirty360::ISDA);  //

    std::vector<Real> NoteCpAmt(couponDates.size() + 1); // 11, from evDate
    std::vector<Date> NoteCpDate(couponDates.size() + 1); // 11

    int BondTenor = Tenor;
    NoteCpDate[0] = startDate;
    for (int i = 1; i <= BondTenor; i++)
    {
        NoteCpDate[i] = couponDates[i - 1];
    }

    NoteCpAmt[0] = 0.0;
    for (int i = 1; i <= BondTenor; i++)
    {
        NoteCpAmt[i] = NPA * couponRate * cpDC.yearFraction(NoteCpDate[i - 1], NoteCpDate[i]);
        //NoteCpAmt[i] = 0.0;
    }

    /*
    std::cout << "\n==================================" << std::endl;
    std::cout << "Part V Bond Cash Flow" << std::endl;
    std::cout << "-------------------------------------------------------" << std::endl;
    for (int i = 0; i <= BondTenor; i++)
    {
        std::cout << "Time: " << i << ", Date: " << NoteCpDate[i] << ", Amount: " << NoteCpAmt[i] << "\n";
    }
    std::cout << std::endl;

    std::cout << "----------------------------------" << std::endl;
    for (int i = 0; i <= Tenor; i++)
    {
        std::cout << i << " : CV[i] = " << CV[i] << std::endl;
    }
    */

    // =====================================================================
    // ---------------------- Part V LSMC ------------------------
    std::cout << "\n==================================" << std::endl;
    std::cout << "Part IV LSMC Output" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    /*
    std::vector<double> CV(BondTenor + 1);
    CV[0] = -100.0;
    CV[1] = 107.79;
    CV[2] = 116.39;
    CV[3] = 124.95;
    CV[4] = 133.53;
    CV[5] = 142.11;
    CV[6] = 150.69;
    CV[7] = 159.27;
    CV[8] = 167.85;
    CV[9] = 176.85;
    CV[10] = 185.01;
    */

    //for (int i = 0; i < 11; i++)
    //{
    //    CV[i] = NPA;
    //}


    Matrix DF(NPath, MStep + 1);
    Matrix disc(NPath, MStep + 1);
    Matrix HoldCF(NPath, MStep + 1);
    Matrix ExerValue(NPath, MStep + 1);

    Matrix CF(NPath, MStep + 1);  // Note CF

    /*
    std::vector<std::vector<double>> MCF;
    for (int i = 0; i < NPath; i++)
    {
        std::vector<double> VCF(MStep + 1, 0.0);
        MCF.push_back(VCF);
    }
    */

    Matrix Value(NPath, MStep + 1, 0.0); // Call Value

    int j = 0;
    int dim = 0;
    int Counter = 0;
    int OBS = 0;
    std::vector<int> RegPath;
    //Matrix MY;
    //Matrix MX;

    // ============================= Set DF ======================================
    for (int i = 0; i < NPath; i++)
    {
        DF[i][0] = 1.0;
        for (int k = 1; k <= MStep; k++)
        {
            double dt = accrual[k] - accrual[k - 1];
            // **** Continuous Compounding
            DF[i][k] = DF[i][k - 1] / exp(MRate_adj[i][k - 1] * dt);
        }
    }

    /*
    for (int k = 1; k <= MStep; k++)
    {
        double dt = accrual[k] - accrual[k - 1];
        std::cout << "accrual[k-1]: " << accrual[k-1] << ", accrual[k]: " << accrual[k] << std::endl;
        std::cout << "    k: " << k << ", dt: " << dt << ", DF: " << DF[0][k] << std::endl;
    }
    */

    for (int i = 0; i < NPath; i++)
    {
        for (int k = 0; k <= MStep; k++)
        {
            disc[i][k] = DF[i][k];
        }
    }

    /*
    std::cout << "------- Discount Function --------" << std::endl;
    for (int j = 0; j <= MStep; j++)
    {
        std::cout << "j = " << j << ", " << disc[0][j] << std::endl;
    }
    */

    // *************************************************************************
    // *************************************************************************
    // Node = 10, 10 Years
    // Terminal
    j = MStep; // 10
    // Time : Tenor don't need to consider
    // Terminal Condition
    {
        for (int i = 0; i < NPath; i++)
        {
            HoldCF[i][j] = CV[j];
            CF[i][j] = CV[j]; // Note Cash Flow

            ExerValue[i][j] = max(HoldCF[i][j] - CV[j], 0.0);
            Value[i][j] = 0.0; // Call Value
        }
    }

    std::cout << "\n========== MC Step : " << j << "==========" << std::endl;
    /*
    std::cout << " ------------ CF ------------ " << std::endl;
    for (int i = 0; i < NPath; i++)
    {
        std::cout << i << " : " << CF[i][j] << std::endl;
    }

    std::cout << " --------- Value ------------ " << std::endl;
    for (int i = 0; i < NPath; i++)
    {
        std::cout << i << " : " << Value[i][j] << std::endl;
    }
    */


    // =========================================================================
    // Node = 9, 9 Years, j = 9
    // Terminal Condition, time: Tenor-1    

    double CpValue = 0.0;
    double RdValue = 0.0;
    double AllValue = 0.0;

    j = j - 1;
    if (j == 0)
        goto stop;

    std::cout << "\n=========== Step 9 =============" << std::endl;
    {
        RegPath = std::vector<int>(0);
        for (int i = 0; i < NPath; i++)
        {
            double X1 = CF[i][j + 1];
            double X2 = NoteCpAmt[j + 1];
            double X3 = (disc[i][j + 1] / disc[i][j]);

            double X4 = Q10Y / Q9Y;
            double X5 = (Q9Y - Q10Y) / Q9Y;
            double X6 = NPA;

            CpValue = (Q10Y / Q9Y) * (CF[i][j + 1]) * (disc[i][j + 1] / disc[i][j]);
            //+ (Q10Y / Q9Y) * couponRate * NPA * (disc[i][j + 1] / disc[i][j]);
            RdValue = (Q9Y - Q10Y) / Q9Y * NPA * recoveryRate * (disc[i][j + 1] / disc[i][j]);
            AllValue = CpValue + RdValue;

            //std::cout << "All Value: " << AllValue << std::endl;
            HoldCF[i][j] = AllValue;
            //HoldCF[i][j] = CF[i][j + 1] * disc[i][j + 1] / disc[i][j];

            if (HoldCF[i][j] > CV[j]) // Exercise better
            {
                RegPath.push_back(i);
                CF[i][j] = CV[j];
                //double x = CF[i][j];
                ExerValue[i][j] = HoldCF[i][j] - CV[j];
            }
            else
            {
                CF[i][j] = HoldCF[i][j];
                //double x = CF[i][j];
                ExerValue[i][j] = 0.0;
            }
            //CF[i, j] = Math.Min(HoldCF[i, j], CV[j]);
            //ExerValue[i, j] = Math.Max(HoldCF[i, j] - CV[j], 0.0);
            Value[i][j] = ExerValue[i][j];
        }
        OBS = RegPath.size();
    }

    std::cout << "\n========== MC Step : " << j << "==========" << std::endl;
    /*
    std::cout << " ----------- HoldCF --------- " << std::endl;
    for (int i = 0; i < NPath; i++)
    {
        std::cout << i << " : " << HoldCF[i][j] << std::endl;
    }

    std::cout << " ------------ CV ------------ " << std::endl;
    std::cout << j << " : " << CV[j] << std::endl;

    std::cout << " -------- ExerValue --------- " << std::endl;
    for (int i = 0; i < NPath; i++)
    {
        std::cout << i << " : " << ExerValue[i][j] << std::endl;
    }

    std::cout << " ------------ CF ------------ " << std::endl;
    for (int i = 0; i < NPath; i++)
    {
        std::cout << i << " : " << CF[i][j] << std::endl;
    }

    std::cout << " --------- Value ------------ " << std::endl;
    for (int i = 0; i < NPath; i++)
    {
        std::cout << i << " : " << Value[i][j] << std::endl;
    }
    */
    j = j - 1;
    if (j == 0)
        goto stop;


    // =========================================================================
    // Node = 8, 8 Years, j = 8
    // Terminal Condition, time: Tenor-2

    std::cout << "\n=========== Step 8 =============" << std::endl;
    dim = 4;
    Counter = BondTenor - 2;
    {
        RegPath = std::vector<int>(0);
        for (int i = 0; i < NPath; i++)
        {
            //double x = CF[i][j + 1];
            CpValue = (Q9Y / Q8Y) * (CF[i][j + 1]) * (disc[i][j + 1] / disc[i][j]);
            //+ (Q9Y / Q8Y) * couponRate * NPA * (disc[i][j + 1] / disc[i][j])
            //+ (Q10Y / Q8Y) * couponRate * NPA * (disc[i][j + 2] / disc[i][j])
            //+ (Q10Y / Q8Y) * NPA * (disc[i][j + 2] / disc[i][j]);

            //double y = CF[i][j + 1];
            RdValue = (Q8Y - Q9Y) / Q8Y * NPA * recoveryRate * (disc[i][j + 1] / disc[i][j]);
            //+ (Q9Y - Q10Y) / Q8Y * NPA * recoveryRate * (disc[i][j + 2] / disc[i][j]);

            AllValue = CpValue + RdValue;
            HoldCF[i][j] = AllValue;

            //HoldCF[i][j] = CF[i][j + 1] * disc[i][j + 1] / disc[i][j];            
            //std::cout << "All Value: " << AllValue << std::endl;
            if (HoldCF[i][j] > CV[j]) // Exercise better
            {
                RegPath.push_back(i);
                ExerValue[i][j] = HoldCF[i][j] - CV[j];
            }
            else
            {
                ExerValue[i][j] = 0.0;
            }
            //ExerValue[i, j] = Math.Max(HoldCF[i, j] - CV[j], 0.0);            
            // ***************************
            CF[i][j] = HoldCF[i][j];
        }

        OBS = RegPath.size();
        //if (OBS > MinPath)
        {
            Matrix MY = Matrix(RegPath.size(), 1);
            Matrix MX = Matrix(RegPath.size(), dim);

            for (int i = 0; i < OBS; i++)
            {
                int obs = RegPath[i];
                MY[i][0] = Value[obs][j + 1] * disc[obs][j + 1] / disc[obs][j];

                double adf = disc[obs][j + 1] / disc[obs][j];
                double ar = MRate_adj[obs][j];

                MX[i][0] = 1.0;
                MX[i][1] = adf;
                MX[i][2] = adf * adf;
                MX[i][3] = adf * adf * adf;
            }

            Matrix MXT = transpose(MX);
            Matrix MA = MXT * MX;
            Matrix MAI(inverse(MA));
            Matrix MC = MXT * MY;
            Matrix MB = MAI * MC;

            for (int i = 0; i < NPath; i++)
            {
                CF[i][j] = HoldCF[i][j];
                Value[i][j] = Value[i][j + 1] * disc[i][j + 1] / disc[i][j];
            }

            for (int i = 0; i < OBS; i++)
            {
                int obs = RegPath[i];

                double adf = disc[obs][j + 1] / disc[obs][j];
                double ar = MRate_adj[obs][j];
                double Expect = MB[0][0] * 1 + MB[1][0] * adf + MB[2][0] * adf * adf
                    + MB[3][0] * adf * adf * adf;

                if (ExerValue[obs][j] > Expect)
                {
                    CF[obs][j] = CV[j];
                    Value[obs][j] = ExerValue[obs][j];
                }
            }
        }

        Counter = Counter - 1;

        std::cout << "\n========== MC Step : " << j << "==========" << std::endl;
        /*
        std::cout << " ----------- HoldCF --------- " << std::endl;
        for (int i = 0; i < NPath; i++)
        {
            std::cout << i << " : " << HoldCF[i][j] << std::endl;
        }

        std::cout << " ------------ CV ------------ " << std::endl;
        std::cout << j << " : " << CV[j] << std::endl;

        std::cout << " -------- ExerValue --------- " << std::endl;
        for (int i = 0; i < NPath; i++)
        {
            std::cout << i << " : " << ExerValue[i][j] << std::endl;
        }

        std::cout << " ------------ CF ------------ " << std::endl;
        for (int i = 0; i < NPath; i++)
        {
            std::cout << i << " : " << CF[i][j] << std::endl;
        }

        std::cout << " --------- Value ------------ " << std::endl;
        for (int i = 0; i < NPath; i++)
        {
            std::cout << i << " : " << Value[i][j] << std::endl;
        }
        */
    }

    j = j - 1;
    if (j == 0)
        goto stop;

    // =========================================================================
    // Node = 7, 7 Years, j = 7
    // Terminal Condition, time: Tenor-3
    {
        std::cout << "\n=========== Step 7 =============" << std::endl;
        RegPath = std::vector<int>(0);
        for (int i = 0; i < NPath; i++)
        {

            CpValue = (Q8Y / Q7Y) * (CF[i][j + 1]) * (disc[i][j + 1] / disc[i][j]);
            //+ (Q8Y / Q7Y) * couponRate * NPA * (disc[i][j + 1] / disc[i][j])
            //+ (Q9Y / Q7Y) * couponRate * NPA * (disc[i][j + 2] / disc[i][j])
            //+ (Q10Y / Q7Y) * couponRate * NPA * (disc[i][j + 3] / disc[i][j])
            //+ (Q10Y / Q7Y) * NPA * (disc[i][j + 3] / disc[i][j]);

            RdValue = (Q7Y - Q8Y) / Q7Y * NPA * recoveryRate * (disc[i][j + 1] / disc[i][j]);
            //+ (Q8Y - Q9Y) / Q7Y * NPA * recoveryRate * (disc[i][j + 2] / disc[i][j])
            //+ (Q9Y - Q10Y) / Q7Y * NPA * recoveryRate * (disc[i][j + 3] / disc[i][j]);
            AllValue = CpValue + RdValue;
            HoldCF[i][j] = AllValue;

            //HoldCF[i][j] = CF[i][j + 1] * disc[i][j + 1] / disc[i][j];
            //std::cout << "All Value: " << AllValue << std::endl;

            if (HoldCF[i][j] > CV[j]) // Exercise better
            {
                RegPath.push_back(i);
                ExerValue[i][j] = HoldCF[i][j] - CV[j];
            }
            else
            {
                ExerValue[i][j] = 0.0;
            }
            //ExerValue[i, j] = Math.Max(HoldCF[i, j] - CV[j], 0.0);            

            // ***************************
            CF[i][j] = HoldCF[i][j];
        }

        OBS = RegPath.size();
        //if (OBS > MinPath)
        {
            Matrix MY = Matrix(RegPath.size(), 1);
            Matrix MX = Matrix(RegPath.size(), dim);

            for (int i = 0; i < OBS; i++)
            {
                int obs = RegPath[i];
                MY[i][0] = Value[obs][j + 1] * disc[obs][j + 1] / disc[obs][j];

                double adf = disc[obs][j + 1] / disc[obs][j];
                double ar = MRate_adj[obs][j];

                MX[i][0] = 1.0;
                MX[i][1] = adf;
                MX[i][2] = adf * adf;
                MX[i][3] = adf * adf * adf;
            }

            Matrix MXT = transpose(MX);
            Matrix MA = MXT * MX;
            Matrix MAI(inverse(MA));
            Matrix MC = MXT * MY;
            Matrix MB = MAI * MC;

            for (int i = 0; i < NPath; i++)
            {
                CF[i][j] = HoldCF[i][j];
                Value[i][j] = Value[i][j + 1] * disc[i][j + 1] / disc[i][j];
            }

            for (int i = 0; i < OBS; i++)
            {
                int obs = RegPath[i];

                double adf = disc[obs][j + 1] / disc[obs][j];
                double ar = MRate_adj[obs][j];
                double Expect = MB[0][0] * 1 + MB[1][0] * adf + MB[2][0] * adf * adf
                    + MB[3][0] * adf * adf * adf;

                if (ExerValue[obs][j] > Expect)
                {
                    CF[obs][j] = CV[j];
                    Value[obs][j] = ExerValue[obs][j];
                }
            }
        }

        Counter = Counter - 1;

        std::cout << "\n========== MC Step : " << j << "==========" << std::endl;
        /*
        std::cout << " ----------- HoldCF --------- " << std::endl;
        for (int i = 0; i < NPath; i++)
        {
            std::cout << i << " : " << HoldCF[i][j] << std::endl;
        }

        std::cout << " ------------ CV ------------ " << std::endl;
        std::cout << j << " : " << CV[j] << std::endl;

        std::cout << " -------- ExerValue --------- " << std::endl;
        for (int i = 0; i < NPath; i++)
        {
            std::cout << i << " : " << ExerValue[i][j] << std::endl;
        }

        std::cout << " ------------ CF ------------ " << std::endl;
        for (int i = 0; i < NPath; i++)
        {
            std::cout << i << " : " << CF[i][j] << std::endl;
        }

        std::cout << " --------- Value ------------ " << std::endl;
        for (int i = 0; i < NPath; i++)
        {
            std::cout << i << " : " << Value[i][j] << std::endl;
        }
        */
    }

    j = j - 1;
    if (j == 0)
        goto stop;

    // =========================================================================
    // Node = 6, 6 Years, j = 6
    // Terminal Condition, time: Tenor-4
    {
        std::cout << "\n=========== Step 6 =============" << std::endl;
        RegPath = std::vector<int>(0);
        for (int i = 0; i < NPath; i++)
        {
            CpValue = (Q7Y / Q6Y) * (CF[i][j + 1]) * (disc[i][j + 1] / disc[i][j]);
            // //(Q7Y / Q6Y) * couponRate * NPA * (disc[i][j + 1] / disc[i][j])
            //+ (Q8Y / Q6Y) * couponRate * NPA * (disc[i][j + 2] / disc[i][j])
            //+ (Q9Y / Q6Y) * couponRate * NPA * (disc[i][j + 3] / disc[i][j])
            //+ (Q10Y / Q6Y) * couponRate * NPA * (disc[i][j + 4] / disc[i][j])
            //+ (Q10Y / Q6Y) * NPA * (disc[i][j + 4] / disc[i][j]);
            RdValue = (Q6Y - Q7Y) / Q6Y * NPA * recoveryRate * (disc[i][j + 1] / disc[i][j]);
            //+ (Q7Y - Q8Y) / Q6Y * NPA * recoveryRate * (disc[i][j + 2] / disc[i][j])
            //+ (Q8Y - Q9Y) / Q6Y * NPA * recoveryRate * (disc[i][j + 3] / disc[i][j])
            //+ (Q9Y - Q10Y) / Q6Y * NPA * recoveryRate * (disc[i][j + 4] / disc[i][j]);
            AllValue = CpValue + RdValue;
            HoldCF[i][j] = AllValue;

            //HoldCF[i][j] = CF[i][j + 1] * disc[i][j + 1] / disc[i][j];
            //std::cout << "All Value: " << AllValue << std::endl;

            if (HoldCF[i][j] > CV[j]) // Exercise better
            {
                RegPath.push_back(i);
                ExerValue[i][j] = HoldCF[i][j] - CV[j];
            }
            else
            {
                ExerValue[i][j] = 0.0;
            }
            //ExerValue[i, j] = Math.Max(HoldCF[i, j] - CV[j], 0.0);            

            // ***************************
            CF[i][j] = HoldCF[i][j];
        }

        OBS = RegPath.size();
        //if (OBS > MinPath)
        {
            Matrix MY = Matrix(RegPath.size(), 1);
            Matrix MX = Matrix(RegPath.size(), dim);

            for (int i = 0; i < OBS; i++)
            {
                int obs = RegPath[i];
                MY[i][0] = Value[obs][j + 1] * disc[obs][j + 1] / disc[obs][j];

                double adf = disc[obs][j + 1] / disc[obs][j];
                double ar = MRate_adj[obs][j];

                MX[i][0] = 1.0;
                MX[i][1] = adf;
                MX[i][2] = adf * adf;
                MX[i][3] = adf * adf * adf;
            }

            Matrix MXT = transpose(MX);
            Matrix MA = MXT * MX;
            Matrix MAI(inverse(MA));
            Matrix MC = MXT * MY;
            Matrix MB = MAI * MC;

            for (int i = 0; i < NPath; i++)
            {
                CF[i][j] = HoldCF[i][j];
                Value[i][j] = Value[i][j + 1] * disc[i][j + 1] / disc[i][j];
            }

            for (int i = 0; i < OBS; i++)
            {
                int obs = RegPath[i];

                double adf = disc[obs][j + 1] / disc[obs][j];
                double ar = MRate_adj[obs][j];
                double Expect = MB[0][0] * 1 + MB[1][0] * adf + MB[2][0] * adf * adf
                    + MB[3][0] * adf * adf * adf;

                if (ExerValue[obs][j] > Expect)
                {
                    CF[obs][j] = CV[j];
                    Value[obs][j] = ExerValue[obs][j];
                }
            }
        }

        Counter = Counter - 1;
    }

    std::cout << "\n========== MC Step : " << j << "==========" << std::endl;
    /*
    std::cout << " ----------- HoldCF --------- " << std::endl;
    for (int i = 0; i < NPath; i++)
    {
        std::cout << i << " : " << HoldCF[i][j] << std::endl;
    }

    std::cout << " ------------ CV ------------ " << std::endl;
    std::cout << j << " : " << CV[j] << std::endl;

    std::cout << " -------- ExerValue --------- " << std::endl;
    for (int i = 0; i < NPath; i++)
    {
        std::cout << i << " : " << ExerValue[i][j] << std::endl;
    }

    std::cout << " ------------ CF ------------ " << std::endl;
    for (int i = 0; i < NPath; i++)
    {
        std::cout << i << " : " << CF[i][j] << std::endl;
    }

    std::cout << " --------- Value ------------ " << std::endl;
    for (int i = 0; i < NPath; i++)
    {
        std::cout << i << " : " << Value[i][j] << std::endl;
    }
    */

    j = j - 1;
    if (j == 0)
        goto stop;

    // =========================================================================
    // Node = 5, 5 Years, j = 5
    // Terminal Condition, time: Tenor-5
    {
        std::cout << "\n=========== Step 5 =============" << std::endl;
        RegPath = std::vector<int>(0);
        for (int i = 0; i < NPath; i++)
        {

            CpValue = (Q6Y / Q5Y) * (CF[i][j + 1]) * (disc[i][j + 1] / disc[i][j]);
            //+ (Q6Y / Q5Y) * couponRate * NPA * (disc[i][j + 1] / disc[i][j])
            //+ (Q7Y / Q5Y) * couponRate * NPA * (disc[i][j + 2] / disc[i][j])
            //+ (Q8Y / Q5Y) * couponRate * NPA * (disc[i][j + 3] / disc[i][j])
            //+ (Q9Y / Q5Y) * couponRate * NPA * (disc[i][j + 4] / disc[i][j])
            //+ (Q10Y / Q5Y) * couponRate * NPA * (disc[i][j + 5] / disc[i][j])
            //+ (Q10Y / Q5Y) * NPA * (disc[i][j + 5] / disc[i][j]);
            RdValue = (Q5Y - Q6Y) / Q5Y * NPA * recoveryRate * (disc[i][j + 1] / disc[i][j]);
            //+ (Q6Y - Q7Y) / Q5Y * NPA * recoveryRate * (disc[i][j + 2] / disc[i][j])
            //+ (Q7Y - Q8Y) / Q5Y * NPA * recoveryRate * (disc[i][j + 3] / disc[i][j])
            //+ (Q8Y - Q9Y) / Q5Y * NPA * recoveryRate * (disc[i][j + 4] / disc[i][j])
            //+ (Q9Y - Q10Y) / Q5Y * NPA * recoveryRate * (disc[i][j + 5] / disc[i][j]);
            AllValue = CpValue + RdValue;
            HoldCF[i][j] = AllValue;

            //HoldCF[i][j] = CF[i][j + 1] * disc[i][j + 1] / disc[i][j];
            //std::cout << "All Value: " << AllValue << std::endl;

            if (HoldCF[i][j] > CV[j]) // Exercise better
            {
                RegPath.push_back(i);
                ExerValue[i][j] = HoldCF[i][j] - CV[j];
            }
            else
            {
                ExerValue[i][j] = 0.0;
            }
            //ExerValue[i, j] = Math.Max(HoldCF[i, j] - CV[j], 0.0);            

            // ***************************
            CF[i][j] = HoldCF[i][j];
        }

        OBS = RegPath.size();
        //if (OBS > MinPath)
        {
            Matrix MY = Matrix(RegPath.size(), 1);
            Matrix MX = Matrix(RegPath.size(), dim);

            for (int i = 0; i < OBS; i++)
            {
                int obs = RegPath[i];
                MY[i][0] = Value[obs][j + 1] * disc[obs][j + 1] / disc[obs][j];

                double adf = disc[obs][j + 1] / disc[obs][j];
                double ar = MRate_adj[obs][j];

                MX[i][0] = 1.0;
                MX[i][1] = adf;
                MX[i][2] = adf * adf;
                MX[i][3] = adf * adf * adf;
            }

            Matrix MXT = transpose(MX);
            Matrix MA = MXT * MX;
            Matrix MAI(inverse(MA));
            Matrix MC = MXT * MY;
            Matrix MB = MAI * MC;

            for (int i = 0; i < NPath; i++)
            {
                CF[i][j] = HoldCF[i][j];
                Value[i][j] = Value[i][j + 1] * disc[i][j + 1] / disc[i][j];
            }

            for (int i = 0; i < OBS; i++)
            {
                int obs = RegPath[i];

                double adf = disc[obs][j + 1] / disc[obs][j];
                double ar = MRate_adj[obs][j];
                double Expect = MB[0][0] * 1 + MB[1][0] * adf + MB[2][0] * adf * adf
                    + MB[3][0] * adf * adf * adf;

                if (ExerValue[obs][j] > Expect)
                {
                    CF[obs][j] = CV[j];
                    Value[obs][j] = ExerValue[obs][j];
                }
            }
        }

        Counter = Counter - 1;
    }

    j = j - 1;
    if (j == 0)
        goto stop;

    // =========================================================================
    // Node = 4, 4 Years, j = 4
    // Terminal Condition, time: Tenor-4
    {
        std::cout << "\n=========== Step 4 =============" << std::endl;
        RegPath = std::vector<int>(0);
        for (int i = 0; i < NPath; i++)
        {
            CpValue = (Q5Y / Q4Y) * (CF[i][j + 1]) * (disc[i][j + 1] / disc[i][j]);
            //(Q5Y / Q4Y) * couponRate * NPA * (disc[i][j + 1] / disc[i][j])
            //+ (Q6Y / Q4Y) * couponRate * NPA * (disc[i][j + 2] / disc[i][j])
            //+ (Q7Y / Q4Y) * couponRate * NPA * (disc[i][j + 3] / disc[i][j])
            //+ (Q8Y / Q4Y) * couponRate * NPA * (disc[i][j + 4] / disc[i][j])
            //+ (Q9Y / Q4Y) * couponRate * NPA * (disc[i][j + 5] / disc[i][j])
            //+ (Q10Y / Q4Y) * couponRate * NPA * (disc[i][j + 6] / disc[i][j])
            //+ (Q10Y / Q4Y) * NPA * (disc[i][j + 6] / disc[i][j]);

            RdValue = (Q4Y - Q5Y) / Q4Y * NPA * recoveryRate * (disc[i][j + 1] / disc[i][j]);
            //+ (Q5Y - Q6Y) / Q4Y * NPA * recoveryRate * (disc[i][j + 2] / disc[i][j])
            //+ (Q6Y - Q7Y) / Q4Y * NPA * recoveryRate * (disc[i][j + 3] / disc[i][j])
            //+ (Q7Y - Q8Y) / Q4Y * NPA * recoveryRate * (disc[i][j + 4] / disc[i][j])
            //+ (Q8Y - Q9Y) / Q4Y * NPA * recoveryRate * (disc[i][j + 5] / disc[i][j])
            //+ (Q9Y - Q10Y) / Q4Y * NPA * recoveryRate * (disc[i][j + 6] / disc[i][j]);
            AllValue = CpValue + RdValue;
            HoldCF[i][j] = AllValue;

            //HoldCF[i][j] = CF[i][j + 1] * disc[i][j + 1] / disc[i][j];
            //HoldCF[i][j] = (CF[i][j + 1] + NoteCpAmt[j + 1]) * disc[i][j + 1] / disc[i][j];

            //std::cout << "All Value: " << AllValue << std::endl;

            if (HoldCF[i][j] > CV[j]) // Exercise better
            {
                RegPath.push_back(i);
                ExerValue[i][j] = HoldCF[i][j] - CV[j];
            }
            else
            {
                ExerValue[i][j] = 0.0;
            }
            //ExerValue[i, j] = Math.Max(HoldCF[i, j] - CV[j], 0.0);            

            // ***************************
            CF[i][j] = HoldCF[i][j];
        }

        OBS = RegPath.size();
        //if (OBS > MinPath)
        {
            Matrix MY = Matrix(RegPath.size(), 1);
            Matrix MX = Matrix(RegPath.size(), dim);

            for (int i = 0; i < OBS; i++)
            {
                int obs = RegPath[i];
                MY[i][0] = Value[obs][j + 1] * disc[obs][j + 1] / disc[obs][j];

                double adf = disc[obs][j + 1] / disc[obs][j];
                double ar = MRate_adj[obs][j];

                MX[i][0] = 1.0;
                MX[i][1] = adf;
                MX[i][2] = adf * adf;
                MX[i][3] = adf * adf * adf;
            }

            Matrix MXT = transpose(MX);
            Matrix MA = MXT * MX;
            Matrix MAI(inverse(MA));
            Matrix MC = MXT * MY;
            Matrix MB = MAI * MC;

            for (int i = 0; i < NPath; i++)
            {
                CF[i][j] = HoldCF[i][j];
                Value[i][j] = Value[i][j + 1] * disc[i][j + 1] / disc[i][j];
            }

            for (int i = 0; i < OBS; i++)
            {
                int obs = RegPath[i];

                double adf = disc[obs][j + 1] / disc[obs][j];
                double ar = MRate_adj[obs][j];
                double Expect = MB[0][0] * 1 + MB[1][0] * adf + MB[2][0] * adf * adf
                    + MB[3][0] * adf * adf * adf;

                if (ExerValue[obs][j] > Expect)
                {
                    CF[obs][j] = CV[j];
                    Value[obs][j] = ExerValue[obs][j];
                }
            }
        }

        Counter = Counter - 1;
    }

    j = j - 1;
    if (j == 0)
        goto stop;


    // =========================================================================
    // Node = 3, 3 Years, j = 3
    // Terminal Condition, time: Tenor-3
    {
        std::cout << "\n=========== Step 3 =============" << std::endl;
        RegPath = std::vector<int>(0);
        for (int i = 0; i < NPath; i++)
        {
            CpValue = (Q4Y / Q3Y) * (CF[i][j + 1]) * (disc[i][j + 1] / disc[i][j]);
            // (Q4Y / Q3Y) * couponRate * NPA * (disc[i][j + 1] / disc[i][j])
            //+ (Q5Y / Q3Y) * couponRate * NPA * (disc[i][j + 2] / disc[i][j])
            //+ (Q6Y / Q3Y) * couponRate * NPA * (disc[i][j + 3] / disc[i][j])
            //+ (Q7Y / Q3Y) * couponRate * NPA * (disc[i][j + 4] / disc[i][j])
            //+ (Q8Y / Q3Y) * couponRate * NPA * (disc[i][j + 5] / disc[i][j])
            //+ (Q9Y / Q3Y) * couponRate * NPA * (disc[i][j + 6] / disc[i][j])
            //+ (Q10Y / Q3Y) * couponRate * NPA * (disc[i][j + 7] / disc[i][j])
            //+ (Q10Y / Q3Y) * NPA * (disc[i][j + 7] / disc[i][j]);

            RdValue = (Q3Y - Q4Y) / Q3Y * NPA * recoveryRate * (disc[i][j + 1] / disc[i][j]);
            //+ (Q4Y - Q5Y) / Q3Y * NPA * recoveryRate * (disc[i][j + 2] / disc[i][j])
            //+ (Q5Y - Q6Y) / Q3Y * NPA * recoveryRate * (disc[i][j + 3] / disc[i][j])
            //+ (Q6Y - Q7Y) / Q3Y * NPA * recoveryRate * (disc[i][j + 4] / disc[i][j])
            //+ (Q7Y - Q8Y) / Q3Y * NPA * recoveryRate * (disc[i][j + 5] / disc[i][j])
            //+ (Q8Y - Q9Y) / Q3Y * NPA * recoveryRate * (disc[i][j + 6] / disc[i][j])
            //+ (Q9Y - Q10Y) / Q3Y * NPA * recoveryRate * (disc[i][j + 7] / disc[i][j]);
            AllValue = CpValue + RdValue;
            HoldCF[i][j] = AllValue;

            //HoldCF[i][j] = CF[i][j + 1] * disc[i][j + 1] / disc[i][j];
            //HoldCF[i][j] = (CF[i][j + 1] + NoteCpAmt[j + 1]) * disc[i][j + 1] / disc[i][j];

            //std::cout << "All Value: " << AllValue << std::endl;

            if (HoldCF[i][j] > CV[j]) // Exercise better
            {
                RegPath.push_back(i);
                ExerValue[i][j] = HoldCF[i][j] + NoteCpAmt[j] - CV[j];
            }
            else
            {
                ExerValue[i][j] = 0.0;
            }
            //ExerValue[i, j] = Math.Max(HoldCF[i, j] - CV[j], 0.0);            

            // ***************************
            CF[i][j] = HoldCF[i][j];
        }

        OBS = RegPath.size();
        //if (OBS > MinPath)
        {
            Matrix MY = Matrix(RegPath.size(), 1);
            Matrix MX = Matrix(RegPath.size(), dim);

            for (int i = 0; i < OBS; i++)
            {
                int obs = RegPath[i];
                MY[i][0] = Value[obs][j + 1] * disc[obs][j + 1] / disc[obs][j];

                double adf = disc[obs][j + 1] / disc[obs][j];
                double ar = MRate_adj[obs][j];

                MX[i][0] = 1.0;
                MX[i][1] = adf;
                MX[i][2] = adf * adf;
                MX[i][3] = adf * adf * adf;
            }

            Matrix MXT = transpose(MX);
            Matrix MA = MXT * MX;
            Matrix MAI(inverse(MA));
            Matrix MC = MXT * MY;
            Matrix MB = MAI * MC;

            for (int i = 0; i < NPath; i++)
            {
                CF[i][j] = HoldCF[i][j];
                Value[i][j] = Value[i][j + 1] * disc[i][j + 1] / disc[i][j];
            }

            for (int i = 0; i < OBS; i++)
            {
                int obs = RegPath[i];

                double adf = disc[obs][j + 1] / disc[obs][j];
                double ar = MRate_adj[obs][j];
                double Expect = MB[0][0] * 1 + MB[1][0] * adf + MB[2][0] * adf * adf
                    + MB[3][0] * adf * adf * adf;

                if (ExerValue[obs][j] > Expect)
                {
                    CF[obs][j] = CV[j];
                    Value[obs][j] = ExerValue[obs][j];
                }
            }
        }

        Counter = Counter - 1;
    }


    j = j - 1;
    if (j == 0)
        goto stop;


    // =========================================================================
    // Node = 2, 2 Years, j = 2
    // Terminal Condition, time: Tenor-2
    {
        std::cout << "\n=========== Step 2 =============" << std::endl;
        RegPath = std::vector<int>(0);
        for (int i = 0; i < NPath; i++)
        {

            CpValue = (Q3Y / Q2Y) * (CF[i][j + 1]) * (disc[i][j + 1] / disc[i][j]);
            //(Q3Y / Q2Y) * couponRate * NPA * (disc[i][j + 1] / disc[i][j])
            //+ (Q4Y / Q2Y) * couponRate * NPA * (disc[i][j + 2] / disc[i][j])
            //+ (Q5Y / Q2Y) * couponRate * NPA * (disc[i][j + 3] / disc[i][j])
            //+ (Q6Y / Q2Y) * couponRate * NPA * (disc[i][j + 4] / disc[i][j])
            //+ (Q7Y / Q2Y) * couponRate * NPA * (disc[i][j + 5] / disc[i][j])
            //+ (Q8Y / Q2Y) * couponRate * NPA * (disc[i][j + 6] / disc[i][j])
            //+ (Q9Y / Q2Y) * couponRate * NPA * (disc[i][j + 7] / disc[i][j])
            //+ (Q10Y / Q2Y) * couponRate * NPA * (disc[i][j + 8] / disc[i][j])
            //+ (Q10Y / Q2Y) * NPA * (disc[i][j + 8] / disc[i][j]);

            RdValue = (Q2Y - Q3Y) / Q2Y * NPA * recoveryRate * (disc[i][j + 1] / disc[i][j]);
            //+ (Q3Y - Q4Y) / Q2Y * NPA * recoveryRate * (disc[i][j + 2] / disc[i][j])
            //+ (Q4Y - Q5Y) / Q2Y * NPA * recoveryRate * (disc[i][j + 3] / disc[i][j])
            //+ (Q5Y - Q6Y) / Q2Y * NPA * recoveryRate * (disc[i][j + 4] / disc[i][j])
            //+ (Q6Y - Q7Y) / Q2Y * NPA * recoveryRate * (disc[i][j + 5] / disc[i][j])
            //+ (Q7Y - Q8Y) / Q2Y * NPA * recoveryRate * (disc[i][j + 6] / disc[i][j])
            //+ (Q8Y - Q9Y) / Q2Y * NPA * recoveryRate * (disc[i][j + 7] / disc[i][j])
            //+ (Q9Y - Q10Y) / Q2Y * NPA * recoveryRate * (disc[i][j + 8] / disc[i][j]);

            AllValue = CpValue + RdValue;
            HoldCF[i][j] = AllValue;

            //CF[i][j] = HoldCF[i][j];
            //HoldCF[i][j] = CF[i][j + 1] * disc[i][j + 1] / disc[i][j];           
            //std::cout << "All Value: " << AllValue << std::endl;
            if (HoldCF[i][j] > CV[j]) // Exercise better
            {
                RegPath.push_back(i);
                ExerValue[i][j] = HoldCF[i][j] - CV[j];
            }
            else
            {
                ExerValue[i][j] = 0.0;
            }
            //ExerValue[i, j] = Math.Max(HoldCF[i, j] - CV[j], 0.0);            

            // ***************************
            CF[i][j] = HoldCF[i][j];
        }

        OBS = RegPath.size();
        //if (OBS > MinPath)
        {
            Matrix MY = Matrix(RegPath.size(), 1);
            Matrix MX = Matrix(RegPath.size(), dim);

            for (int i = 0; i < OBS; i++)
            {
                int obs = RegPath[i];
                MY[i][0] = Value[obs][j + 1] * disc[obs][j + 1] / disc[obs][j];

                double adf = disc[obs][j + 1] / disc[obs][j];
                double ar = MRate_adj[obs][j];

                MX[i][0] = 1.0;
                MX[i][1] = adf;
                MX[i][2] = adf * adf;
                MX[i][3] = adf * adf * adf;
            }

            Matrix MXT = transpose(MX);
            Matrix MA = MXT * MX;
            Matrix MAI(inverse(MA));
            Matrix MC = MXT * MY;
            Matrix MB = MAI * MC;

            for (int i = 0; i < NPath; i++)
            {
                CF[i][j] = HoldCF[i][j];
                Value[i][j] = Value[i][j + 1] * disc[i][j + 1] / disc[i][j];
            }

            for (int i = 0; i < OBS; i++)
            {
                int obs = RegPath[i];

                double adf = disc[obs][j + 1] / disc[obs][j];
                double ar = MRate_adj[obs][j];
                double Expect = MB[0][0] * 1 + MB[1][0] * adf + MB[2][0] * adf * adf
                    + MB[3][0] * adf * adf * adf;

                if (ExerValue[obs][j] > Expect)
                {
                    CF[obs][j] = CV[j];
                    Value[obs][j] = ExerValue[obs][j];
                }
            }
        }

        Counter = Counter - 1;
    }

    j = j - 1;
    if (j == 0)
        goto stop;


    // =========================================================================
    // Node = 1, 1 Years, j = 1
    // Terminal Condition, time: Tenor-1
    {
        std::cout << "\n=========== Step 1 =============" << std::endl;
        RegPath = std::vector<int>(0);
        for (int i = 0; i < NPath; i++)
        {

            CpValue = (Q2Y / Q1Y) * (CF[i][j + 1]) * (disc[i][j + 1] / disc[i][j]);
            //(Q2Y / Q1Y) * couponRate * NPA * (disc[i][j + 1] / disc[i][j])
            //+ (Q3Y / Q1Y) * couponRate * NPA * (disc[i][j + 2] / disc[i][j])
            //+ (Q4Y / Q1Y) * couponRate * NPA * (disc[i][j + 3] / disc[i][j])
            //+ (Q5Y / Q1Y) * couponRate * NPA * (disc[i][j + 4] / disc[i][j])
            //+ (Q6Y / Q1Y) * couponRate * NPA * (disc[i][j + 5] / disc[i][j])
            //+ (Q7Y / Q1Y) * couponRate * NPA * (disc[i][j + 6] / disc[i][j])
            //+ (Q8Y / Q1Y) * couponRate * NPA * (disc[i][j + 7] / disc[i][j])
            //+ (Q9Y / Q1Y) * couponRate * NPA * (disc[i][j + 8] / disc[i][j])
            //+ (Q10Y / Q1Y) * couponRate * NPA * (disc[i][j + 9] / disc[i][j])
            //+ (Q10Y / Q1Y) * NPA * (disc[i][j + 9] / disc[i][j]);

            RdValue = (Q1Y - Q2Y) / Q1Y * NPA * recoveryRate * (disc[i][j + 1] / disc[i][j]);
            //+ (Q2Y - Q3Y) / Q1Y * NPA * recoveryRate * (disc[i][j + 2] / disc[i][j])
            //+ (Q3Y - Q4Y) / Q1Y * NPA * recoveryRate * (disc[i][j + 3] / disc[i][j])
            //+ (Q4Y - Q5Y) / Q1Y * NPA * recoveryRate * (disc[i][j + 4] / disc[i][j])
            //+ (Q5Y - Q6Y) / Q1Y * NPA * recoveryRate * (disc[i][j + 5] / disc[i][j])
            //+ (Q6Y - Q7Y) / Q1Y * NPA * recoveryRate * (disc[i][j + 6] / disc[i][j])
            //+ (Q7Y - Q8Y) / Q1Y * NPA * recoveryRate * (disc[i][j + 7] / disc[i][j])
            //+ (Q8Y - Q9Y) / Q1Y * NPA * recoveryRate * (disc[i][j + 8] / disc[i][j])
            //+ (Q9Y - Q10Y) / Q1Y * NPA * recoveryRate * (disc[i][j + 9] / disc[i][j]);
            AllValue = CpValue + RdValue;
            HoldCF[i][j] = AllValue;

            //HoldCF[i][j] = CF[i][j + 1] * disc[i][j + 1] / disc[i][j];
            //std::cout << "All Value: " << AllValue << std::endl;

            if (HoldCF[i][j] > CV[j]) // Exercise better
            {
                RegPath.push_back(i);
                ExerValue[i][j] = HoldCF[i][j] - CV[j];
            }
            else
            {
                ExerValue[i][j] = 0.0;
            }
            //ExerValue[i, j] = Math.Max(HoldCF[i, j] - CV[j], 0.0);            

            // ***************************
            CF[i][j] = HoldCF[i][j];
        }

        OBS = RegPath.size();
        //if (OBS > MinPath)
        {
            Matrix MY = Matrix(RegPath.size(), 1);
            Matrix MX = Matrix(RegPath.size(), dim);

            for (int i = 0; i < OBS; i++)
            {
                int obs = RegPath[i];
                MY[i][0] = Value[obs][j + 1] * disc[obs][j + 1] / disc[obs][j];

                double adf = disc[obs][j + 1] / disc[obs][j];
                double ar = MRate_adj[obs][j];

                MX[i][0] = 1.0;
                MX[i][1] = adf;
                MX[i][2] = adf * adf;
                MX[i][3] = adf * adf * adf;
            }

            Matrix MXT = transpose(MX);
            Matrix MA = MXT * MX;
            Matrix MAI(inverse(MA));
            Matrix MC = MXT * MY;
            Matrix MB = MAI * MC;

            for (int i = 0; i < NPath; i++)
            {
                CF[i][j] = HoldCF[i][j];
                Value[i][j] = Value[i][j + 1] * disc[i][j + 1] / disc[i][j];
            }

            for (int i = 0; i < OBS; i++)
            {
                int obs = RegPath[i];

                double adf = disc[obs][j + 1] / disc[obs][j];
                double ar = MRate_adj[obs][j];
                double Expect = MB[0][0] * 1 + MB[1][0] * adf + MB[2][0] * adf * adf
                    + MB[3][0] * adf * adf * adf;

                if (ExerValue[obs][j] > (Expect))
                {
                    CF[obs][j] = CV[j];
                    Value[obs][j] = ExerValue[obs][j];
                }
            }
        }

        Counter = Counter - 1;
    }

    j = j - 1; // j = 0

    // *************************************************************************
    // *************************************************************************
    // T0

stop:
    std::cout << "\n--------- MC All Steps Finish ---------" << std::endl;

    double total_CF = 0.0;

    double total_Value = 0.0;

    double avg_CF = 0.0;

    double avg_Value = 0.0;

    for (int i = 0; i < NPath; i++)
    {
        double x1 = (disc[i][j + 1] / disc[i][j]);
        double x2 = CF[i][j + 1];
        double x3 = NoteCpAmt[j + 1];
        double x4 = CF[i][j + 1];


        CpValue = (Q1Y * (CF[i][j + 1]) * (disc[i][j + 1] / disc[i][j]));
        //(Q1Y * (CF[i][j + 1]) * (disc[i][j + 1] / disc[i][j]));
        //Q1Y * couponRate * NPA * (disc[i][j + 1] / disc[i][j])            
        RdValue = (1.0 - Q1Y) * NPA * recoveryRate * (disc[i][j + 1] / disc[i][j]);
        AllValue = CpValue + RdValue;

        //AllValue = CF[i][j + 1] * disc[i][j + 1] / disc[i][j];

        total_CF = total_CF + AllValue;

        total_Value = total_Value + Value[i][1] * disc[i][1];
    }

    /*
    for (int i = 0; i < NPath; i++)
    {
        std::cout << "Path: " << i << std::endl;
        for (int j = 0; j < MStep; j++)
        {
            std::cout << "Step: " << j << ", Value: " << Value[i][j] << std::endl;
        }
    }
    */

    avg_CF = total_CF / NPath;
    avg_Value = total_Value / NPath;

    std::cout << "\n================== GPU Simulation Output =================" << std::endl;
    std::cout << "Num of Path: " << NPath << std::endl;
    std::cout << "        NPA: " << setprecision(2) << NPA << std::endl;
    std::cout << "Coupon Rate: " << setprecision(4) << couponRate << std::endl;
    std::cout << "  NPV: " << setprecision(8) << avg_CF << std::endl;
    std::cout << "  BNP+BOC CDS Spread(bp): " << CS * 10000.0 << std::endl;

    return;

}

