//
// "Divu_Type" means S, where divergence U = S
// "Dsdt_Type" means pd S/pd t, where S is as above
// "RhoYchemProd_Type" means -omega_l/rho, i.e., the mass rate of decrease of species l due
//             to kinetics divided by rho
//
#include <winstd.H>
#include <unistd.h>

#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cfloat>
#include <fstream>
#include <vector>

#include <Geometry.H>
#include <Extrapolater.H>
#include <BoxDomain.H>
#include <ParmParse.H>
#include <ErrorList.H>
#include <HeatTransfer.H>
#include <HEATTRANSFER_F.H>
#include <ChemDriver_F.H>
#include <DIFFUSION_F.H>
#include <MultiGrid.H>
#include <ArrayLim.H>
#include <SPACE.H>
#include <Interpolater.H>
#include <ccse-mpi.H>
#include <Utility.H>

#if defined(BL_USE_NEWMECH) || defined(BL_USE_VELOCITY)
#include <DataServices.H>
#include <AmrData.H>
#endif

#include <PROB_F.H>
#include <NAVIERSTOKES_F.H>
#include <DERIVE_F.H>

#include <buildInfo.H>

static Box stripBox; // used for debugging

using std::cout;
using std::endl;
using std::cerr;

#define DEF_LIMITS(fab,fabdat,fablo,fabhi)   \
const int* fablo = (fab).loVect();           \
const int* fabhi = (fab).hiVect();           \
Real* fabdat = (fab).dataPtr();

#define DEF_CLIMITS(fab,fabdat,fablo,fabhi)  \
const int* fablo = (fab).loVect();           \
const int* fabhi = (fab).hiVect();           \
const Real* fabdat = (fab).dataPtr();

#define DEF_CLIMITSCOMP(fab,fabdat,fablo,fabhi,comp)  \
const int* fablo = (fab).loVect();                    \
const int* fabhi = (fab).hiVect();                    \
const Real* fabdat = (fab).dataPtr(comp);

#ifdef BL_USE_FLOAT
const Real Real_MIN = FLT_MIN;
const Real Real_MAX = FLT_MAX;
#else
const Real Real_MIN = DBL_MIN;
const Real Real_MAX = DBL_MAX;
#endif

const int  GEOM_GROW   = 1;
const int  PRESS_GROW  = 1;
const int  DIVU_GROW   = 1;
const int  DSDT_GROW   = 1;
const Real bogus_value =  1.e20;
const int  DQRAD_GROW  = 1;
const int  YDOT_GROW   = 1;
const int  HYPF_GROW   = 1;
const int  LinOp_grow  = 1;

static Real              typical_RhoH_value_default = -1.e10;
static const std::string typical_values_filename("typical_values.fab");

namespace
{
    bool initialized = false;
}
//
// Set all default values in Initialize()!!!
//
namespace
{
    std::set<std::string> ShowMF_Sets;
    std::string           ShowMF_Dir;
    bool                  ShowMF_Verbose;
    bool                  ShowMF_Check_Nans;
    FABio::Format         ShowMF_Fab_Format;
    bool                  do_not_use_funccount;
    bool                  do_active_control;
    bool                  do_active_control_temp;
    Real                  temp_control;
    Real                  crse_dt;
    int                   chem_box_chop_threshold;
}

Real HeatTransfer::p_amb_old;
Real HeatTransfer::p_amb_new;
Real HeatTransfer::dp0dt;
int  HeatTransfer::closed_chamber;
int  HeatTransfer::num_divu_iters;
int  HeatTransfer::init_once_done;
int  HeatTransfer::do_OT_radiation;
int  HeatTransfer::do_heat_sink;
int  HeatTransfer::RhoH;
int  HeatTransfer::do_diffuse_sync;
int  HeatTransfer::do_reflux_visc;
int  HeatTransfer::dpdt_option;
int  HeatTransfer::RhoYdot_Type;
int  HeatTransfer::FuncCount_Type;
int  HeatTransfer::divu_ceiling;
Real HeatTransfer::divu_dt_factor;
Real HeatTransfer::min_rho_divu_ceiling;
int  HeatTransfer::have_trac;
int  HeatTransfer::have_rhort;
int  HeatTransfer::Trac;
int  HeatTransfer::RhoRT;
int  HeatTransfer::first_spec;
int  HeatTransfer::last_spec;
int  HeatTransfer::nspecies;
int  HeatTransfer::floor_species;
int  HeatTransfer::do_set_rho_to_species_sum;
Real HeatTransfer::rgas;
Real HeatTransfer::prandtl;
Real HeatTransfer::schmidt;
Real HeatTransfer::constant_mu_val;
Real HeatTransfer::constant_rhoD_val;
Real HeatTransfer::constant_lambda_val;
int  HeatTransfer::unity_Le;
Real HeatTransfer::htt_tempmin;
Real HeatTransfer::htt_tempmax;
Real HeatTransfer::htt_hmixTYP;
int  HeatTransfer::zeroBndryVisc;
int  HeatTransfer::do_add_nonunityLe_corr_to_rhoh_adv_flux;
int  HeatTransfer::do_check_divudt;
int  HeatTransfer::hack_nochem;
int  HeatTransfer::hack_nospecdiff;
int  HeatTransfer::hack_noavgdivu;
int  HeatTransfer::use_tranlib;
Real HeatTransfer::trac_diff_coef;
Real HeatTransfer::P1atm_MKS;
bool HeatTransfer::plot_reactions;
bool HeatTransfer::plot_consumption;
bool HeatTransfer::plot_heat_release;
static bool plot_rhoydot;
Real HeatTransfer::new_T_threshold;
int  HeatTransfer::nGrowAdvForcing=1;
bool HeatTransfer::avg_down_chem;
int  HeatTransfer::reset_typical_vals_int=-1;
std::map<std::string,Real> HeatTransfer::typical_values_FileVals;

std::string                                HeatTransfer::turbFile;
ChemDriver*                                HeatTransfer::chemSolve;
std::map<std::string, Array<std::string> > HeatTransfer::auxDiag_names;

Array<Real> HeatTransfer::typical_values;

///////////////////////////////
// SDC Stuff

// these can be set in the inputs file
int HeatTransfer::sdc_iterMAX;
int HeatTransfer::num_mac_sync_iter;

static
std::string
to_upper (const std::string& s)
{
    std::string rtn = s;
    for (unsigned int i=0; i<rtn.length(); i++)
    {
        rtn[i] = toupper(rtn[i]);
    }
    return rtn;
}

static std::map<std::string,FABio::Format> ShowMF_Fab_Format_map;

void
HeatTransfer::Initialize ()
{
    if (initialized) return;

    NavierStokesBase::Initialize();
    //
    // Set all default values here!!!
    //
    ShowMF_Fab_Format_map["ASCII"] = FABio::FAB_ASCII;
    ShowMF_Fab_Format_map["IEEE"] = FABio::FAB_IEEE;
    ShowMF_Fab_Format_map["NATIVE"] = FABio::FAB_NATIVE;
    ShowMF_Fab_Format_map["8BIT"] = FABio::FAB_8BIT;
    ShowMF_Fab_Format_map["IEEE_32"] = FABio::FAB_IEEE_32;
    ShowMF_Verbose          = true;
    ShowMF_Check_Nans       = true;
    ShowMF_Fab_Format       = ShowMF_Fab_Format_map["ASCII"];
    do_not_use_funccount    = false;
    do_active_control       = false;
    do_active_control_temp  = false;
    temp_control            = -1;
    crse_dt                 = -1;
    chem_box_chop_threshold = -1;

    HeatTransfer::p_amb_old                 = -1.0;
    HeatTransfer::p_amb_new                 = -1.0;
    HeatTransfer::closed_chamber            = 0;
    HeatTransfer::num_divu_iters            = 1;
    HeatTransfer::init_once_done            = 0;
    HeatTransfer::do_OT_radiation           = 0;
    HeatTransfer::do_heat_sink              = 0;
    HeatTransfer::RhoH                      = -1;
    HeatTransfer::do_diffuse_sync           = 1;
    HeatTransfer::do_reflux_visc            = 1;
    HeatTransfer::dpdt_option               = 2;
    HeatTransfer::RhoYdot_Type                 = -1;
    HeatTransfer::FuncCount_Type            = -1;
    HeatTransfer::divu_ceiling              = 0;
    HeatTransfer::divu_dt_factor            = .5;
    HeatTransfer::min_rho_divu_ceiling      = -1.e20;
    HeatTransfer::have_trac                 = 0;
    HeatTransfer::have_rhort                = 0;
    HeatTransfer::Trac                      = -1;
    HeatTransfer::RhoRT                     = -1;
    HeatTransfer::first_spec                = -1;
    HeatTransfer::last_spec                 = -2;
    HeatTransfer::nspecies                  = 0;
    HeatTransfer::floor_species             = 1;
    HeatTransfer::do_set_rho_to_species_sum = 1;
    HeatTransfer::rgas                      = -1.0;
    HeatTransfer::prandtl                   = .7;
    HeatTransfer::schmidt                   = .7;
    HeatTransfer::constant_mu_val           = -1;
    HeatTransfer::constant_rhoD_val         = -1;
    HeatTransfer::constant_lambda_val       = -1;
    HeatTransfer::unity_Le                  = 1;
    HeatTransfer::htt_tempmin               = 298.0;
    HeatTransfer::htt_tempmax               = 40000.;
    HeatTransfer::htt_hmixTYP               = -1.;
    HeatTransfer::zeroBndryVisc             = 0;
    HeatTransfer::chemSolve                 = 0;
    HeatTransfer::do_check_divudt           = 1;
    HeatTransfer::hack_nochem               = 0;
    HeatTransfer::hack_nospecdiff           = 0;
    HeatTransfer::hack_noavgdivu            = 0;
    HeatTransfer::use_tranlib               = 0;
    HeatTransfer::trac_diff_coef            = 0.0;
    HeatTransfer::P1atm_MKS                 = -1.0;
    HeatTransfer::turbFile                  = "";
    HeatTransfer::plot_reactions            = false;
    HeatTransfer::plot_consumption          = true;
    HeatTransfer::plot_heat_release         = true;
    plot_rhoydot                            = false;
    HeatTransfer::new_T_threshold           = -1;  // On new AMR level, max change in lower bound for T, not used if <=0
    HeatTransfer::avg_down_chem             = false;
    HeatTransfer::reset_typical_vals_int    = -1;
    HeatTransfer::typical_values_FileVals.clear();

    HeatTransfer::do_add_nonunityLe_corr_to_rhoh_adv_flux = 1;

    HeatTransfer::sdc_iterMAX               = 1;
    HeatTransfer::num_mac_sync_iter         = 1;

    ParmParse pp("ns");

    pp.query("do_diffuse_sync",do_diffuse_sync);
    BL_ASSERT(do_diffuse_sync == 0 || do_diffuse_sync == 1);
    pp.query("do_reflux_visc",do_reflux_visc);
    BL_ASSERT(do_reflux_visc == 0 || do_reflux_visc == 1);
    pp.query("dpdt_option",dpdt_option);
    BL_ASSERT(dpdt_option >= 0 && dpdt_option <= 2);
    pp.query("do_active_control",do_active_control);
    pp.query("do_active_control_temp",do_active_control_temp);
    pp.query("temp_control",temp_control);

    if (do_active_control_temp && temp_control <= 0)
        BoxLib::Error("temp_control MUST be set with do_active_control_temp");

    verbose = pp.contains("v");

    pp.query("divu_ceiling",divu_ceiling);
    BL_ASSERT(divu_ceiling >= 0 && divu_ceiling <= 3);
    pp.query("divu_dt_factor",divu_dt_factor);
    BL_ASSERT(divu_dt_factor>0 && divu_dt_factor <= 1.0);
    pp.query("min_rho_divu_ceiling",min_rho_divu_ceiling);
    if (divu_ceiling) BL_ASSERT(min_rho_divu_ceiling >= 0.0);

    pp.query("htt_tempmin",htt_tempmin);
    pp.query("htt_tempmax",htt_tempmax);

    pp.query("floor_species",floor_species);
    BL_ASSERT(floor_species == 0 || floor_species == 1);

    pp.query("do_set_rho_to_species_sum",do_set_rho_to_species_sum);

    pp.query("num_divu_iters",num_divu_iters);

    pp.query("do_not_use_funccount",do_not_use_funccount);

    pp.query("schmidt",schmidt);
    pp.query("prandtl",prandtl);
    pp.query("unity_Le",unity_Le);
    unity_Le = unity_Le ? 1 : 0;
    if (unity_Le)
    {
	schmidt = prandtl;
        if (verbose && ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::read_params: Le=1, setting Sc = Pr" << '\n';
    }

    pp.query("sdc_iterMAX",sdc_iterMAX);
    pp.query("num_mac_sync_iter",num_mac_sync_iter);

    pp.query("constant_mu_val",constant_mu_val);
    pp.query("constant_rhoD_val",constant_rhoD_val);
    pp.query("constant_lambda_val",constant_lambda_val);
    if (constant_mu_val != -1)
    {
        if (verbose && ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::read_params: using constant_mu_val = " 
                      << constant_mu_val << '\n';
    }
    if (constant_rhoD_val != -1)
    {
        if (verbose && ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::read_params: using constant_rhoD_val = " 
                      << constant_rhoD_val << '\n';
    }
    if (constant_lambda_val != -1)
    {
        if (verbose && ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::read_params: using constant_lambda_val = " 
                      << constant_lambda_val << '\n';
    }

    pp.query("do_add_nonunityLe_corr_to_rhoh_adv_flux", do_add_nonunityLe_corr_to_rhoh_adv_flux);
    pp.query("hack_nochem",hack_nochem);
    pp.query("hack_nospecdiff",hack_nospecdiff);
    pp.query("hack_noavgdivu",hack_noavgdivu);
    pp.query("do_check_divudt",do_check_divudt);
    pp.query("avg_down_chem",avg_down_chem);
    pp.query("reset_typical_vals_int",reset_typical_vals_int);

    pp.query("do_OT_radiation",do_OT_radiation);
    do_OT_radiation = (do_OT_radiation ? 1 : 0);
    pp.query("do_heat_sink",do_heat_sink);
    do_heat_sink = (do_heat_sink ? 1 : 0);

    pp.query("use_tranlib",use_tranlib);
    if (use_tranlib == 1) {
      chemSolve->SetTransport(ChemDriver::CD_TRANLIB);
      if (verbose && ParallelDescriptor::IOProcessor())
        std::cout << "HeatTransfer::read_params: Using Tranlib transport " << '\n';
    }
    else {
      chemSolve->SetTransport(ChemDriver::CD_EG);
      if (verbose && ParallelDescriptor::IOProcessor())
        std::cout << "HeatTransfer::read_params: Using EGLib transport " << '\n';
    }
    chemSolve = new ChemDriver();

    pp.query("turbFile",turbFile);

    pp.query("zeroBndryVisc",zeroBndryVisc);
    //
    // Set variability/visc for velocities.
    //
    if (variable_vel_visc != 1)
      BoxLib::Error("HeatTransfer::read_params() -- must use variable viscosity");
    //
    // Set variability/visc for tracer
    //
    if (variable_scal_diff != 1)
      BoxLib::Error("HeatTransfer::read_params() -- must use variable scalar diffusivity");
    //
    // Read in scalar value and use it as tracer.
    //
    pp.query("scal_diff_coefs",trac_diff_coef);

    for (int i = 0; i < visc_coef.size(); i++)
        visc_coef[i] = bogus_value;

    // Useful for debugging
    ParmParse pproot;
    if (int nsv=pproot.countval("ShowMF_Sets"))
    {
        Array<std::string> ShowMF_set_names(nsv);
        pproot.getarr("ShowMF_Sets",ShowMF_set_names);
        for (int i=0; i<nsv; ++i) {
            ShowMF_Sets.insert(ShowMF_set_names[i]);
        }
        ShowMF_Dir="."; pproot.query("ShowMF_Dir",ShowMF_Dir);
        pproot.query("ShowMF_Verbose",ShowMF_Verbose);
        pproot.query("ShowMF_Check_Nans",ShowMF_Check_Nans);
        std::string format="NATIVE"; pproot.query("ShowMF_Fab_Format",format);
        if (ShowMF_Fab_Format_map.count(to_upper(format)) == 0) {
            BoxLib::Abort("Unknown FABio format label");
        }
        ShowMF_Fab_Format = ShowMF_Fab_Format_map[format];

        if (ShowMF_Verbose>0 && ShowMF_set_names.size()>0 && ParallelDescriptor::IOProcessor()) {
            cout << "   ******************************  Debug: ShowMF_Sets: ";
            for (int i=0; i<ShowMF_set_names.size(); ++i) {
                cout << ShowMF_set_names[i] << " ";
            }
            cout << '\n';
        }

    }

#ifdef PARTICLES
    read_particle_params();
#endif

    if (verbose && ParallelDescriptor::IOProcessor())
    {
        std::cout << "\nDumping ParmParse table:\n \n";
        ParmParse::dumpTable(std::cout);
        std::cout << "\n... done dumping ParmParse table.\n" << '\n';
    }

    BoxLib::ExecOnFinalize(HeatTransfer::Finalize);

    initialized = true;
}

void
HeatTransfer::Finalize ()
{
    initialized = false;
}

static
void
FabMinMax (FArrayBox& fab,
           const Box& box,
           Real       fmin,
           Real       fmax,
           int        sComp,
           int        nComp)
{
    BL_ASSERT(fab.box().contains(box));
    BL_ASSERT(sComp + nComp <= fab.nComp());

    const int* lo     = box.loVect();
    const int* hi     = box.hiVect();
    Real*      fabdat = fab.dataPtr(sComp);
    
    FORT_FABMINMAX(lo, hi,
                   fabdat, ARLIM(fab.loVect()), ARLIM(fab.hiVect()),
                   &fmin, &fmax, &nComp);
}

static
Box
getStrip(const Geometry& geom)
{
    const Box& box = geom.Domain();
    IntVect be = box.bigEnd();
    IntVect se = box.smallEnd();
    se[0] = (int) 0.5*(se[0]+be[0]);
    be[0] = se[0];
    return Box(se,be);
}

void
showMFsub(const std::string&   mySet,
          const MultiFab&      mf,
          const Box&           box,
          const std::string&   name,
          int                  lev = -1,
          int                  iter = -1) // Default value = no append 2nd integer
{
    if (ShowMF_Sets.count(mySet)>0)
    {
        const FABio::Format saved_format = FArrayBox::getFormat();
        FArrayBox::setFormat(ShowMF_Fab_Format);
        std::string DebugDir(ShowMF_Dir);
        if (ParallelDescriptor::IOProcessor())
            if (!BoxLib::UtilCreateDirectory(DebugDir, 0755))
                BoxLib::CreateDirectoryFailed(DebugDir);
        ParallelDescriptor::Barrier();

        std::string junkname = name;
        if (lev>=0) {
            junkname = BoxLib::Concatenate(junkname+"_",lev,1);
        }
        if (iter>=0) {
            junkname = BoxLib::Concatenate(junkname+"_",iter,1);
        }
        junkname = DebugDir + "/" + junkname;

        if (ShowMF_Verbose>0 && ParallelDescriptor::IOProcessor()) {
            cout << "   ******************************  Debug: writing " << junkname << '\n';
        }

        FArrayBox sub(box,mf.nComp());

        mf.copyTo(sub,0,0,mf.nComp(),mf.nGrow());

        if (ShowMF_Check_Nans)
        {
            BL_ASSERT(!sub.contains_nan(box,0,mf.nComp()));
        }
        std::ofstream os;
        os.open(junkname.c_str());
        sub.writeOn(os);
        os.close();
        FArrayBox::setFormat(saved_format);
    }
}

void
showMF(const std::string&   mySet,
       const MultiFab&      mf,
       const std::string&   name,
       int                  lev = -1,
       int                  iter = -1, // Default value = no append 2nd integer
       int                  step = -1) // Default value = no append 3nd integer
{
    if (ShowMF_Sets.count(mySet)>0)
    {
        const FABio::Format saved_format = FArrayBox::getFormat();
        FArrayBox::setFormat(ShowMF_Fab_Format);
        std::string DebugDir(ShowMF_Dir);
        if (ParallelDescriptor::IOProcessor())
            if (!BoxLib::UtilCreateDirectory(DebugDir, 0755))
                BoxLib::CreateDirectoryFailed(DebugDir);
        ParallelDescriptor::Barrier();

        std::string junkname = name;
        if (lev>=0) {
            junkname = BoxLib::Concatenate(junkname+"_",lev,1);
        }
        if (iter>=0) {
            junkname = BoxLib::Concatenate(junkname+"_",iter,1);
        }
        if (step>=0) {
            junkname = BoxLib::Concatenate(junkname+"_",step,5);
        }
        junkname = DebugDir + "/" + junkname;

        if (ShowMF_Verbose>0 && ParallelDescriptor::IOProcessor()) {
            cout << "   ******************************  Debug: writing " << junkname << '\n';
        }

#if 0
        if (ShowMF_Check_Nans)
        {
            for (MFIter mfi(mf); mfi.isValid(); ++mfi)
            {
	      //                BL_ASSERT(!mf[mfi].contains_nan(mfi.validbox(),0,mf.nComp()));
            }
        }
#endif
        VisMF::Write(mf,junkname);
        FArrayBox::setFormat(saved_format);
    }
}

HeatTransfer::FPLoc 
HeatTransfer::fpi_phys_loc (int p_bc)
{
    //
    // Location of data that FillPatchIterator returns at physical boundaries
    //
    if (p_bc == EXT_DIR || p_bc == HOEXTRAP || p_bc == FOEXTRAP)
    {
        return HT_Edge;
    }
    return HT_Center;
}
    
void
HeatTransfer::center_to_edge_fancy (const FArrayBox& cfab,
                                    FArrayBox&       efab,
                                    const Box&       ccBox,
                                    int              sComp,
                                    int              dComp,
                                    int              nComp,
                                    const Box&       domain,
                                    const FPLoc&     bc_lo,
                                    const FPLoc&     bc_hi)
{
    const Box&      ebox = efab.box();
    const IndexType ixt  = ebox.ixType();

    BL_ASSERT(!(ixt.cellCentered()) && !(ixt.nodeCentered()));

    int dir = -1;
    for (int d = 0; d < BL_SPACEDIM; d++)
        if (ixt.test(d))
            dir = d;

    BL_ASSERT(BoxLib::grow(ccBox,-BoxLib::BASISV(dir)).contains(BoxLib::enclosedCells(ebox)));
    BL_ASSERT(sComp+nComp <= cfab.nComp() && dComp+nComp <= efab.nComp());
    //
    // Exclude unnecessary cc->ec calcs
    //
    Box ccVBox = ccBox;
    if (bc_lo!=HT_Center)
        ccVBox.setSmall(dir,std::max(domain.smallEnd(dir),ccVBox.smallEnd(dir)));
    if (bc_hi!=HT_Center)
        ccVBox.setBig(dir,std::min(domain.bigEnd(dir),ccVBox.bigEnd(dir)));
    //
    // Shift cell-centered data to edges
    //
    const int isharm = def_harm_avg_cen2edge?1:0;
    FORT_CEN2EDG(ccVBox.loVect(),ccVBox.hiVect(),
                 ARLIM(cfab.loVect()),ARLIM(cfab.hiVect()),cfab.dataPtr(sComp),
                 ARLIM(efab.loVect()),ARLIM(efab.hiVect()),efab.dataPtr(dComp),
                 &nComp, &dir, &isharm);
    //
    // Fix boundary...i.e. fill-patched data in cfab REALLY lives on edges
    //
    if ( !(domain.contains(ccBox)) )
    {
        if (bc_lo==HT_Edge)
        {
            BoxList gCells = BoxLib::boxDiff(ccBox,domain);
            if (gCells.ok())
            {
                const int inc = +1;
                FArrayBox ovlpFab;
                for (BoxList::const_iterator bli = gCells.begin(), end = gCells.end();
                     bli != end;
                     ++bli)
                {
                    if (bc_lo == HT_Edge)
                    {
                        ovlpFab.resize(*bli,nComp);
                        ovlpFab.copy(cfab,sComp,0,nComp);
                        ovlpFab.shiftHalf(dir,inc);
                        efab.copy(ovlpFab,0,dComp,nComp);
                    }
                }
            }
        }
        if (bc_hi==HT_Edge)
        {
            BoxList gCells = BoxLib::boxDiff(ccBox,domain);
            if (gCells.ok())
            {
                const int inc = -1;
                FArrayBox ovlpFab;
                for (BoxList::const_iterator bli = gCells.begin(), end = gCells.end();
                     bli != end;
                     ++bli)
                {
                    if (bc_hi == HT_Edge)
                    {
                        ovlpFab.resize(*bli,nComp);
                        ovlpFab.copy(cfab,sComp,0,nComp);
                        ovlpFab.shiftHalf(dir,inc);
                        efab.copy(ovlpFab,0,dComp,nComp);
                    }
                }
            }
        }
    }
}    

void
HeatTransfer::variableCleanUp ()
{
    NavierStokesBase::variableCleanUp();

    delete chemSolve;
    chemSolve = 0;

    ShowMF_Sets.clear();
    auxDiag_names.clear();
    typical_values.clear();
}

HeatTransfer::HeatTransfer ()
{
    if (!init_once_done)
        init_once();

    if (!do_temp)
        BoxLib::Abort("do_temp MUST be true");

    if (!have_divu)
        BoxLib::Abort("have_divu MUST be true");

    if (!have_dsdt)
        BoxLib::Abort("have_dsdt MUST be true");

    // p_amb_old and p_amb_new contain the old-time and new-time
    // pressure at level 0.  For closed chamber problems they change over time.
    // set p_amb_old and new if they haven't been set yet
    // to the value in htdata.H set in PROB_F.F
    // only the coarse level advance and the level 0-1 mac_sync 
    // can modify these later
    if (p_amb_old == -1.0)
    {
      FORT_GETPAMB(&p_amb_old);
    }
    if (p_amb_new == -1.0)
    {
      FORT_GETPAMB(&p_amb_new);
    }

    FORT_GETCLOSEDCHAMBER(&closed_chamber);

    updateFluxReg = false;

    EdgeState              = 0;
    EdgeFlux               = 0;
    SpecDiffusionFluxn     = 0;
    SpecDiffusionFluxnp1   = 0;
#ifdef USE_WBAR
    SpecDiffusionFluxWbar  = 0;
#endif
}

HeatTransfer::HeatTransfer (Amr&            papa,
                            int             lev,
                            const Geometry& level_geom,
                            const BoxArray& bl,
                            Real            time)
    :
    NavierStokesBase(papa,lev,level_geom,bl,time)
{
    if (!init_once_done)
        init_once();

    if (!do_temp)
        BoxLib::Abort("do_temp MUST be true");

    if (!have_divu)
        BoxLib::Abort("have_divu MUST be true");

    if (!have_dsdt)
        BoxLib::Abort("have_dsdt MUST be true");

    // p_amb_old and p_amb_new contain the old-time and new-time
    // pressure at level 0.  For closed chamber problems they change over time.
    // set p_amb_old and new if they haven't been set yet
    // to the value in htdata.H set in PROB_F.F
    // only the coarse level advance and the level 0-1 mac_sync 
    // can modify these later
    if (p_amb_old == -1.0)
    {
      FORT_GETPAMB(&p_amb_old);
    }
    if (p_amb_new == -1.0)
    {
      FORT_GETPAMB(&p_amb_new);
    }

    FORT_GETCLOSEDCHAMBER(&closed_chamber);

    updateFluxReg = false;

    define_data();
}

HeatTransfer::~HeatTransfer ()
{
    ;
}

void
HeatTransfer::define_data ()
{
    const int nGrow       = 0;
    const int nEdgeStates = desc_lst[State_Type].nComp();

    EdgeState = (raii_fbs.push_back(new FluxBoxes(this, nEdgeStates, nGrow)))->get();
    EdgeFlux  = (raii_fbs.push_back(new FluxBoxes(this, nEdgeStates, nGrow)))->get();
    
    if (nspecies>0 && !unity_Le)
    {
	SpecDiffusionFluxn   = (raii_fbs.push_back(
				    new FluxBoxes(this, nspecies+3, nGrow)))->get();
	SpecDiffusionFluxnp1 = (raii_fbs.push_back(
				    new FluxBoxes(this, nspecies+3, nGrow)))->get();

#ifdef USE_WBAR
	SpecDiffusionFluxWbar = (raii_fbs.push_back(
				     new FluxBoxes(this, nspecies, nGrow)))->get();
#endif
        sumSpecFluxDotGradHn.define(grids,1,0,Fab_allocate);
        sumSpecFluxDotGradHnp1.define(grids,1,0,Fab_allocate);
    }

    for (std::map<std::string,Array<std::string> >::iterator it = auxDiag_names.begin(), end = auxDiag_names.end();
         it != end; ++it)
    {
        auxDiag[it->first] = raii_mfs.push_back(new MultiFab(grids,it->second.size(),0));
        auxDiag[it->first]->setVal(0);
    }

    // HACK for debugging
    if (level==0)
        stripBox = getStrip(geom);

#ifdef USE_WBAR
    // this will hold the transport coefficients for Wbar
    diffWbar_cc.define(grids,nspecies,1);
#endif
}

void
HeatTransfer::init_once ()
{
    //
    // Computes the static variables unique to HeatTransfer.
    // Check that (some) things are set up correctly.
    //
    int dummy_State_Type;

    int have_temp = isStateVariable("temp", dummy_State_Type, Temp);

    have_temp = have_temp && State_Type == dummy_State_Type;
    have_temp = have_temp && isStateVariable("rhoh", dummy_State_Type, RhoH);
    have_temp = have_temp && State_Type == dummy_State_Type;

    have_trac = isStateVariable("tracer", dummy_State_Type, Trac);
    have_trac = have_trac && State_Type == dummy_State_Type;

    have_rhort = isStateVariable("RhoRT", dummy_State_Type, RhoRT);
    have_rhort = have_rhort && State_Type == dummy_State_Type;

    if (!have_temp)
        BoxLib::Abort("HeatTransfer::init_once(): RhoH & Temp must both be the state");
    
    if (!have_rhort && verbose && ParallelDescriptor::IOProcessor())
        BoxLib::Warning("HeatTransfer::init_once(): RhoRT being stored in the Tracer slot");
    
    if (Temp < RhoH)
        BoxLib::Abort("HeatTransfer::init_once(): must have RhoH < Temp");
    //
    // Temperature must be non-conservative, rho*h must be conservative.
    //
    if (advectionType[Temp] == Conservative)
        BoxLib::Abort("HeatTransfer::init_once(): Temp must be non-conservative");

    if (advectionType[RhoH] != Conservative)
        BoxLib::Abort("HeatTransfer::init_once(): RhoH must be conservative");
    //
    // Species checks.
    //
    BL_ASSERT(Temp > RhoH && RhoH > Density);
    //
    // Here we want to count relative to Density instead of relative
    // to RhoH, so we can put RhoH after the species instead of before.  
    // This logic should work in both cases.
    //
    first_spec =  Density + 1;
    last_spec  = first_spec + getChemSolve().numSpecies() - 1;
    
    for (int i = first_spec; i <= last_spec; i++)
        if (advectionType[i] != Conservative)
            BoxLib::Error("HeatTransfer::init_once: species must be conservative");
    
    int diffuse_spec = is_diffusive[first_spec];
    for (int i = first_spec+1; i <= last_spec; i++)
        if (is_diffusive[i] != diffuse_spec)
            BoxLib::Error("HeatTransfer::init_once: Le != 1; diffuse");
    //
    // Load integer pointers into Fortran common, reqd for proper ICs.
    //
    const int density = (int)Density;

    FORT_SET_SCAL_NUMB(&density, &Temp, &Trac, &RhoH, &first_spec, &last_spec);
    //
    // Load constants into Fortran common to compute viscosities, etc.
    //
    const int var_visc = (constant_mu_val     == -1 ? 1 : 0);
    const int var_cond = (constant_lambda_val == -1 ? 1 : 0);
    const int var_diff = (constant_rhoD_val   == -1 ? 1 : 0);
    
    FORT_SET_HT_VISC_COMMON(&var_visc, &constant_mu_val,
                            &var_cond, &constant_lambda_val,
                            &var_diff, &constant_rhoD_val,
                            &prandtl,  &schmidt, &unity_Le);

    // initialize default typical values
    FORT_INIT_TYPVALS_COMMON();
    //
    // make space for typical values
    //
    typical_values.resize(NUM_STATE,-1); // -ve means don't use for anything
    typical_values[RhoH] = typical_RhoH_value_default;

    const Array<std::string>& speciesNames = getChemSolve().speciesNames();
    ParmParse pp("ht");
    for (int i=0; i<nspecies; ++i) {
      const std::string ppStr = std::string("typValY_") + speciesNames[i];
      if (pp.countval(ppStr.c_str())>0) {
        pp.get(ppStr.c_str(),typical_values_FileVals[speciesNames[i]]);
      }
    }
    std::string otherKeys[4] = {"Temp", "RhoH", "Vel", "Trac"};
    for (int i=0; i<4; ++i) {
      const std::string ppStr(std::string("typVal_")+otherKeys[i]);
      if (pp.countval(ppStr.c_str())>0) {
        pp.get(ppStr.c_str(),typical_values_FileVals[otherKeys[i]]);
      }
    }
    //
    // Get universal gas constant from Fortran.
    //
    rgas = getChemSolve().getRuniversal();
    P1atm_MKS = getChemSolve().getP1atm_MKS();

    if (rgas <= 0.0)
    {
        std::cerr << "HeatTransfer::init_once(): bad rgas: " << rgas << '\n';
        BoxLib::Abort();
    }
    if (P1atm_MKS <= 0.0)
    {
        std::cerr << "HeatTransfer::init_once(): bad P1atm_MKS: " << P1atm_MKS << '\n';
        BoxLib::Abort();
    }
    //
    // Chemistry.
    //
    int ydot_good = RhoYdot_Type >= 0 && RhoYdot_Type <desc_lst.size()
        && RhoYdot_Type != Divu_Type
        && RhoYdot_Type != Dsdt_Type
        && RhoYdot_Type != State_Type;
    //
    // This is the minimum number of boxes per MPI proc that I want
    // when chopping up chemistry work.
    //
    pp.query("chem_box_chop_threshold",chem_box_chop_threshold);

    if (chem_box_chop_threshold <= 0)
    {
#ifdef BL_USE_OMP
        chem_box_chop_threshold = 8;
#else
        chem_box_chop_threshold = 4;
#endif
    }
    
    if (!ydot_good)
        BoxLib::Error("HeatTransfer::init_once(): need RhoYdot_Type if do_chemistry");

    if (desc_lst[RhoYdot_Type].nComp() < nspecies)
        BoxLib::Error("HeatTransfer::init_once(): RhoYdot_Type needs nspecies components");
    //
    // Enforce Le = 1, unless !unity_Le
    //
    if (unity_Le && (schmidt != prandtl) )
    {
        if (verbose && ParallelDescriptor::IOProcessor())
            std::cout << "**************** WARNING ***************\n"
                      << "HeatTransfer::init_once() : currently must have"
                      << "equal Schmidt and Prandtl numbers unless !unity_Le.\n"
                      << "Setting Schmidt = Prandtl\n"
                      << "**************** WARNING ***************\n";
    
        schmidt = prandtl;
    }
    //
    // We are done.
    //
    num_state_type = desc_lst.size();

    if (verbose && ParallelDescriptor::IOProcessor())
        std::cout << "HeatTransfer::init_once(): num_state_type = " << num_state_type << '\n';

    pp.query("plot_reactions",plot_reactions);
    if (plot_reactions)
    {
        auxDiag_names["REACTIONS"].resize(getChemSolve().numReactions());
        for (int i = 0; i < auxDiag_names["REACTIONS"].size(); ++i)
            auxDiag_names["REACTIONS"][i] = BoxLib::Concatenate("R",i+1);
        if (ParallelDescriptor::IOProcessor())
            std::cout << "***** Make sure to increase amr.regrid_int !!!!!" << '\n';
    }

    pp.query("plot_consumption",plot_consumption);
    pp.query("plot_auxDiags",plot_consumption); // This is for backward comptibility - FIXME
    if (plot_consumption && consumptionName.size()>0)
    {
        auxDiag_names["CONSUMPTION"].resize(consumptionName.size());
        for (int j=0; j<consumptionName.size(); ++j)
        {
            auxDiag_names["CONSUMPTION"][j] = consumptionName[j] + "_ConsumptionRate";
        }
    }

    pp.query("plot_heat_release",plot_heat_release);
    if (plot_heat_release)
    {
        auxDiag_names["HEATRELEASE"].resize(1);
        auxDiag_names["HEATRELEASE"][0] = "HeatRelease";
    }
    
    pp.query("new_T_threshold",new_T_threshold);

#ifdef BL_COMM_PROFILING
    auxDiag_names["COMMPROF"].resize(3);
    auxDiag_names["COMMPROF"][0] = "mpiRank";
    auxDiag_names["COMMPROF"][1] = "proximityRank";
    auxDiag_names["COMMPROF"][2] = "proximityOrder";
#endif

    init_once_done = 1;
}

void
HeatTransfer::restart (Amr&          papa,
                       std::istream& is,
                       bool          bReadSpecial)
{

    NavierStokesBase::restart(papa,is,bReadSpecial);

    define_data();

    bool running_sdc_from_strang_chk = false;

    if (running_sdc_from_strang_chk)
    {
      MultiFab& rYdot_old = get_old_data(RhoYdot_Type);
      MultiFab& rYdot_new = get_new_data(RhoYdot_Type);
      MultiFab& S_old = get_old_data(State_Type);
      MultiFab& S_new = get_new_data(State_Type);


#ifdef _OPENMP
#pragma omp parallel
#endif
      for (MFIter mfi(rYdot_old,true); mfi.isValid(); ++mfi)
      {
	  FArrayBox& ry1 = rYdot_old[mfi];
	  FArrayBox& ry2 = rYdot_new[mfi];
	  const FArrayBox& S1 = S_old[mfi];
	  const FArrayBox& S2 = S_new[mfi];
	  for (int i=0; i<nspecies; ++i)
	  {
	      ry1.mult(S1,Density,i,1);
	      ry2.mult(S2,Density,i,1);
	  }
      }
    }

    // Deal with typical values
    set_typical_values(true);


    // get pamb
    if (level == 0 && ParallelDescriptor::IOProcessor())
    {
      std::ifstream PambFile;
      std::string FullPathPambFile = parent->theRestartFile();
      FullPathPambFile += "/Pamb";
      PambFile.open(FullPathPambFile.c_str(), std::ios::in);

      PambFile >> p_amb_old;
      p_amb_new = p_amb_old;
      PambFile.close();

    }
    ParallelDescriptor::Bcast(&p_amb_old, 1, ParallelDescriptor::IOProcessorNumber());
    ParallelDescriptor::Bcast(&p_amb_new, 1, ParallelDescriptor::IOProcessorNumber());

}

void
HeatTransfer::set_typical_values(bool restart)
{
    if (level==0)
    {
        const int nComp = typical_values.size();

        if (restart)
        {
            BL_ASSERT(nComp==NUM_STATE);

            for (int i=0; i<nComp; ++i)
                typical_values[i] = -1;
            
            if (ParallelDescriptor::IOProcessor())
            {
                const std::string tvfile = parent->theRestartFile() + "/" + typical_values_filename;
                std::ifstream tvis;
                tvis.open(tvfile.c_str(),std::ios::in|std::ios::binary);
                
                if (tvis.good())
                {
                    FArrayBox tvfab;
                    tvfab.readFrom(tvis);
                    if (tvfab.nComp() != typical_values.size())
                        BoxLib::Abort("Typical values file has wrong number of components");
                    for (int i=0; i<typical_values.size(); ++i)
                        typical_values[i] = tvfab.dataPtr()[i];
                }
            }

	    ParallelDescriptor::ReduceRealMax(typical_values.dataPtr(),nComp); //FIXME: better way?
        }
	else
        {
            //
            // Check fortan common values, override values set above if fortran values > 0.
            //
            Array<Real> tvTmp(nComp,0);
            FORT_GETTYPICALVALS(tvTmp.dataPtr(), &nComp);
            ParallelDescriptor::ReduceRealMax(tvTmp.dataPtr(),nComp);
        
            for (int i=0; i<nComp; ++i)
            {
                if (tvTmp[i] > 0)
                    typical_values[i] = tvTmp[i];
            }
	}
        //
        // If typVals specified in inputs, these take precedence componentwise.
        //
        for (std::map<std::string,Real>::const_iterator it=typical_values_FileVals.begin(), 
               End=typical_values_FileVals.end();
             it!=End;
             ++it)
        {
            const int idx = getChemSolve().index(it->first);
            if (idx>=0)
            {
                typical_values[first_spec+idx] = it->second;
            }
            else
            {
                if (it->first == "Temp")
                    typical_values[Temp] = it->second;
                else if (it->first == "RhoH")
                    typical_values[RhoH] = it->second;
                else if (it->first == "Trac")
                    typical_values[Trac] = it->second;
                else if (it->first == "Vel")
                {
                    for (int d=0; d<BL_SPACEDIM; ++d)
                        typical_values[d] = it->second;
                }
            }
        }

        FORT_SETTYPICALVALS(typical_values.dataPtr(), &nComp);

        if (ParallelDescriptor::IOProcessor())
        {
            cout << "Typical vals: " << '\n';
            cout << "\tVelocity: ";
            for (int i=0; i<BL_SPACEDIM; ++i)
            {
                cout << typical_values[i] << ' ';
            }
            cout << '\n';
            cout << "\tDensity: " << typical_values[Density] << '\n';
            cout << "\tTemp:    " << typical_values[Temp]    << '\n';
            cout << "\tRhoH:    " << typical_values[RhoH]    << '\n';
            const Array<std::string>& names = getChemSolve().speciesNames();
            for (int i=0; i<nspecies; ++i)
            {
                cout << "\tY_" << names[i] << ": " << typical_values[first_spec+i] << '\n';
            }
        }
    }
}

void
HeatTransfer::reset_typical_values (const MultiFab& S)
{
    //
    // NOTE: Assumes that this level has valid data everywhere.
    //
    const int nComp = typical_values.size();

    BL_ASSERT(nComp == S.nComp());

    for (int i=0; i<nComp; ++i)
    {
        const Real thisMax = S.max(i);
        const Real thisMin = S.min(i);
        const Real newVal = std::abs(thisMax - thisMin);
        if (newVal > 0)
        {
            if ( (i>=first_spec && i<=last_spec) )
                typical_values[i] = newVal / typical_values[Density];
            else
                typical_values[i] = newVal;
        }
    }
    //
    // If typVals specified in inputs, these take precedence componentwise.
    //
    if (parent->levelSteps(0) == 0)
    {
        for (std::map<std::string,Real>::const_iterator it=typical_values_FileVals.begin(), 
                 End=typical_values_FileVals.end();
             it!=End;
             ++it)
        {
            const int idx = getChemSolve().index(it->first);
            if (idx>=0)
            {
                typical_values[first_spec+idx] = it->second;
            }
            else
            {
                if (it->first == "Temp")
                    typical_values[Temp] = it->second;
                else if (it->first == "RhoH")
                    typical_values[RhoH] = it->second;
                else if (it->first == "Trac")
                    typical_values[Trac] = it->second;
                else if (it->first == "Vel")
                {
                    for (int d=0; d<BL_SPACEDIM; ++d)
                        typical_values[d] = it->second;
                }
            }
        }
    }

    FORT_SETTYPICALVALS(typical_values.dataPtr(), &nComp);

    if (ParallelDescriptor::IOProcessor())
    {
        cout << "New typical vals: " << '\n';
        cout << "\tVelocity: ";
        for (int i=0; i<BL_SPACEDIM; ++i) {
            cout << typical_values[i] << ' ';
        }
        cout << '\n';
        cout << "\tDensity:  " << typical_values[Density] << '\n';
        cout << "\tTemp:     " << typical_values[Temp]    << '\n';
        cout << "\tRhoH:     " << typical_values[RhoH]    << '\n';
        const Array<std::string>& names = getChemSolve().speciesNames();
        for (int i=0; i<nspecies; ++i)
        {
            cout << "\tY_" << names[i] << ": " << typical_values[first_spec+i] << '\n';
        }
    }
}

Real
HeatTransfer::estTimeStep ()
{

    Real estdt = NavierStokesBase::estTimeStep();

    if (fixed_dt > 0.0 || !divu_ceiling)
        //
        // The n-s function did the right thing in this case.
        //
        return estdt;

    const Real strt_time = ParallelDescriptor::second();

    Real dt, ns_estdt = estdt, divu_dt = 1.0e20;

    const int   n_grow   = 1;
    const Real  cur_time = state[State_Type].curTime();
    const Real* dx       = geom.CellSize();
    MultiFab*   dsdt     = getDsdt(0,cur_time);
    MultiFab*   divu     = getDivCond(0,cur_time);

    for (FillPatchIterator U_fpi(*this,*divu,n_grow,cur_time,State_Type,Xvel,BL_SPACEDIM);
         U_fpi.isValid();
         ++U_fpi)
    {
        const int        i   = U_fpi.index();
        FArrayBox&       U   = U_fpi();
        const FArrayBox& Rho = rho_ctime[U_fpi];
	const Box&     grdbx = grids[i];
        const int*       lo  = grdbx.loVect();
        const int*       hi  = grdbx.hiVect();

        DEF_CLIMITS((*divu)[U_fpi],sdat,slo,shi);
        DEF_CLIMITS(Rho,rhodat,rholo,rhohi);
        DEF_CLIMITS(U,vel,ulo,uhi);

        DEF_CLIMITS(volume[i],vol,v_lo,v_hi);

        DEF_CLIMITS(area[0][i],areax,ax_lo,ax_hi);
        DEF_CLIMITS(area[1][i],areay,ay_lo,ay_hi);
#if (BL_SPACEDIM==3) 
        DEF_CLIMITS(area[2][i],areaz,az_lo,az_hi)
#endif
        FORT_EST_DIVU_DT(divu_ceiling,&divu_dt_factor,
                         dx,sdat,ARLIM(slo),ARLIM(shi),
                         (*dsdt)[U_fpi].dataPtr(),
                         rhodat,ARLIM(rholo),ARLIM(rhohi),
                         vel,ARLIM(ulo),ARLIM(uhi),
                         vol,ARLIM(v_lo),ARLIM(v_hi),
                         areax,ARLIM(ax_lo),ARLIM(ax_hi),
                         areay,ARLIM(ay_lo),ARLIM(ay_hi),
#if (BL_SPACEDIM==3) 
                         areaz,ARLIM(az_lo),ARLIM(az_hi),
#endif 
                         lo,hi,&dt,&min_rho_divu_ceiling);

        divu_dt = std::min(divu_dt,dt);
    }

    delete divu;
    delete dsdt;

    ParallelDescriptor::ReduceRealMin(divu_dt);

    if (verbose && ParallelDescriptor::IOProcessor())
    {
        std::cout << "HeatTransfer::estTimeStep(): estdt, divu_dt = " 
                  << estdt << ", " << divu_dt << '\n';
    }

    estdt = std::min(estdt, divu_dt);

    if (estdt < ns_estdt && verbose && ParallelDescriptor::IOProcessor())
        std::cout << "HeatTransfer::estTimeStep(): timestep reduced from " 
                  << ns_estdt << " to " << estdt << '\n';

    if (verbose > 1)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::estTimeStep(): lev: " << level << ", time: " << run_time << '\n';
    }

    return estdt;
}

void
HeatTransfer::checkTimeStep (Real dt)
{
    if (fixed_dt > 0.0 || !divu_ceiling) 
        return;

    const int   n_grow    = 1;
    const Real  cur_time  = state[State_Type].curTime();
    const Real* dx        = geom.CellSize();
    MultiFab*   dsdt      = getDsdt(0,cur_time);
    MultiFab*   divu      = getDivCond(0,cur_time);

    for (FillPatchIterator U_fpi(*this,*divu,n_grow,cur_time,State_Type,Xvel,BL_SPACEDIM);
         U_fpi.isValid();
         ++U_fpi)
    {
        const int        i   = U_fpi.index();
        FArrayBox&       U   = U_fpi();
        const FArrayBox& Rho = rho_ctime[U_fpi];
	const Box&    grdbx  = grids[i];
        const int*       lo  = grdbx.loVect();
        const int*       hi  = grdbx.hiVect();

        DEF_LIMITS((*divu)[U_fpi],sdat,slo,shi);
        DEF_CLIMITS(Rho,rhodat,rholo,rhohi);
        DEF_CLIMITS(U,vel,ulo,uhi);

        DEF_CLIMITS(volume[i],vol,v_lo,v_hi);
        DEF_CLIMITS(area[0][i],areax,ax_lo,ax_hi);
        DEF_CLIMITS(area[1][i],areay,ay_lo,ay_hi);

#if (BL_SPACEDIM==3) 
        DEF_CLIMITS(area[2][i],areaz,az_lo,az_hi);
#endif
        FORT_CHECK_DIVU_DT(divu_ceiling,&divu_dt_factor,
                           dx,sdat,ARLIM(slo),ARLIM(shi),
			   (*dsdt)[U_fpi].dataPtr(),
                           rhodat,ARLIM(rholo),ARLIM(rhohi),
                           vel,ARLIM(ulo),ARLIM(uhi),
                           vol,ARLIM(v_lo),ARLIM(v_hi),
                           areax,ARLIM(ax_lo),ARLIM(ax_hi),
                           areay,ARLIM(ay_lo),ARLIM(ay_hi),
#if (BL_SPACEDIM==3) 
                           areaz,ARLIM(az_lo),ARLIM(az_hi),
#endif 
                           lo,hi,&dt,&min_rho_divu_ceiling);
    }

    delete dsdt;
    delete divu;
}

void
HeatTransfer::setTimeLevel (Real time,
                            Real dt_old,
                            Real dt_new)
{
    NavierStokesBase::setTimeLevel(time, dt_old, dt_new);    

    state[RhoYdot_Type].setTimeLevel(time,dt_old,dt_new);

    state[FuncCount_Type].setTimeLevel(time,dt_old,dt_new);
}

//
// This (minus the NEWMECH stuff) is copied from NavierStokes.cpp
//

void
HeatTransfer::initData ()
{
    //
    // Initialize the state and the pressure.
    //
    int         ns       = NUM_STATE - BL_SPACEDIM;
    const Real* dx       = geom.CellSize();
    MultiFab&   S_new    = get_new_data(State_Type);
    MultiFab&   P_new    = get_new_data(Press_Type);
    const Real  cur_time = state[State_Type].curTime();

#ifdef BL_USE_NEWMECH
    //
    // This code has a few drawbacks.  It assumes that the physical
    // domain size of the current problem is the same as that of the
    // one that generated the pltfile.  It also assumes that the pltfile
    // has at least as many levels as does the current problem.  If
    // either of these are false this code is likely to core dump.
    //
    ParmParse pp("ht");

    std::string pltfile;
    pp.query("pltfile", pltfile);
    if (pltfile.empty())
        BoxLib::Abort("You must specify `pltfile'");
    if (verbose && ParallelDescriptor::IOProcessor())
        std::cout << "initData: reading data from: " << pltfile << '\n';

    DataServices::SetBatchMode();
    FileType fileType(NEWPLT);
    DataServices dataServices(pltfile, fileType);

    if (!dataServices.AmrDataOk())
        //
        // This calls ParallelDescriptor::EndParallel() and exit()
        //
        DataServices::Dispatch(DataServices::ExitRequest, NULL);
    
    AmrData&                  amrData     = dataServices.AmrDataRef();
    const int                 nspecies    = getChemSolve().numSpecies();
    const Array<std::string>& names       = getChemSolve().speciesNames();   
    Array<std::string>        plotnames   = amrData.PlotVarNames();

    int idT = -1, idX = -1;
    for (int i = 0; i < plotnames.size(); ++i)
    {
        if (plotnames[i] == "temp")       idT = i;
        if (plotnames[i] == "x_velocity") idX = i;
    }
    //
    // In the plotfile the mass fractions directly follow the velocities.
    //
    int idSpec = idX + BL_SPACEDIM;

    for (int i = 0; i < BL_SPACEDIM; i++)
    {
        amrData.FillVar(S_new, level, plotnames[idX+i], Xvel+i);
        amrData.FlushGrids(idX+i);
    }
    amrData.FillVar(S_new, level, plotnames[idT], Temp);
    amrData.FlushGrids(idT);

    for (int i = 0; i < nspecies; i++)
    {
        amrData.FillVar(S_new, level, plotnames[idSpec+i], first_spec+i);
        amrData.FlushGrids(idSpec+i);
    }

    if (verbose && ParallelDescriptor::IOProcessor())
        std::cout << "initData: finished init from pltfile" << '\n';
#endif

    for (MFIter snewmfi(S_new); snewmfi.isValid(); ++snewmfi)
    {
        BL_ASSERT(grids[snewmfi.index()] == snewmfi.validbox());

        P_new[snewmfi].setVal(0);

        const int  i       = snewmfi.index();
        RealBox    gridloc = RealBox(grids[i],geom.CellSize(),geom.ProbLo());
	const Box& vbx     = snewmfi.validbox();
        const int* lo      = vbx.loVect();
        const int* hi      = vbx.hiVect();
        const int* s_lo    = S_new[snewmfi].loVect();
        const int* s_hi    = S_new[snewmfi].hiVect();
        const int* p_lo    = P_new[snewmfi].loVect();
        const int* p_hi    = P_new[snewmfi].hiVect();

#ifdef BL_USE_NEWMECH
        FORT_INITDATANEWMECH (&level,&cur_time,lo,hi,&ns,
                              S_new[snewmfi].dataPtr(Xvel),
                              S_new[snewmfi].dataPtr(BL_SPACEDIM),
                              ARLIM(s_lo), ARLIM(s_hi),
                              P_new[snewmfi].dataPtr(),
                              ARLIM(p_lo), ARLIM(p_hi),
                              dx,gridloc.lo(),gridloc.hi() );
#else
        FORT_INITDATA (&level,&cur_time,lo,hi,&ns,
                       S_new[snewmfi].dataPtr(Xvel),
                       S_new[snewmfi].dataPtr(BL_SPACEDIM),
                       ARLIM(s_lo), ARLIM(s_hi),
                       P_new[snewmfi].dataPtr(),
                       ARLIM(p_lo), ARLIM(p_hi),
                       dx,gridloc.lo(),gridloc.hi() );
#endif
    }

    showMFsub("1D",S_new,stripBox,"1D_S",level);

#ifdef BL_USE_VELOCITY
    //
    // We want to add the velocity from the supplied plotfile
    // to what we already put into the velocity field via FORT_INITDATA.
    //
    // This code has a few drawbacks.  It assumes that the physical
    // domain size of the current problem is the same as that of the
    // one that generated the pltfile.  It also assumes that the pltfile
    // has at least as many levels (with the same refinement ratios) as does
    // the current problem.  If either of these are false this code is
    // likely to core dump.
    //
    ParmParse pp("ht");

    std::string velocity_plotfile;
    pp.query("velocity_plotfile", velocity_plotfile);

    if (!velocity_plotfile.empty())
    {
        if (verbose && ParallelDescriptor::IOProcessor())
            std::cout << "initData: reading data from: " << velocity_plotfile << '\n';

        DataServices::SetBatchMode();
        Amrvis::FileType fileType(Amrvis::NEWPLT);
        DataServices dataServices(velocity_plotfile, fileType);

        if (!dataServices.AmrDataOk())
            //
            // This calls ParallelDescriptor::EndParallel() and exit()
            //
            DataServices::Dispatch(DataServices::ExitRequest, NULL);

        AmrData&                  amrData   = dataServices.AmrDataRef();
        //const int                 nspecies  = getChemSolve().numSpecies();
        //const Array<std::string>& names     = getChemSolve().speciesNames();   
        Array<std::string>        plotnames = amrData.PlotVarNames();

        if (amrData.FinestLevel() < level)
            BoxLib::Abort("initData: not enough levels in plotfile");

        if (amrData.ProbDomain()[level] != Domain())
            BoxLib::Abort("initData: problem domains do not match");
    
        int idX = -1;
        for (int i = 0; i < plotnames.size(); ++i)
            if (plotnames[i] == "x_velocity") idX = i;

        if (idX == -1)
            BoxLib::Abort("Could not find velocity fields in supplied velocity_plotfile");

        MultiFab tmp(S_new.boxArray(), 1, 0);
        for (int i = 0; i < BL_SPACEDIM; i++)
        {
            amrData.FillVar(tmp, level, plotnames[idX+i], 0);
#ifdef _OPENMP
#pragma omp parallel
#endif
            for (MFIter mfi(tmp,true); mfi.isValid(); ++mfi) {
                S_new[mfi].plus(tmp[mfi], mfi.tilebox(), 0, Xvel+i, 1);
	    }
            amrData.FlushGrids(idX+i);
        }

        if (verbose && ParallelDescriptor::IOProcessor())
            std::cout << "initData: finished init from velocity_plotfile" << '\n';
    }
#endif /*BL_USE_VELOCITY*/

    make_rho_prev_time();
    make_rho_curr_time();
    //
    // Initialize other types.
    //
    initDataOtherTypes();
    //
    // Initialize divU and dSdt.
    //
    if (have_divu)
    {
        const Real dt       = 1.0;
        const Real dtin     = -1.0; // Dummy value denotes initialization.
        const Real cur_time = state[Divu_Type].curTime();
        MultiFab&  Divu_new = get_new_data(Divu_Type);

        state[State_Type].setTimeLevel(cur_time,dt,dt);

        calcDiffusivity(cur_time);
#ifdef USE_WBAR
	calcDiffusivity_Wbar(cur_time);
#endif

        calc_divu(cur_time,dtin,Divu_new);

        if (have_dsdt)
            get_new_data(Dsdt_Type).setVal(0);
    }

    if (state[Press_Type].descriptor()->timeType() == StateDescriptor::Point) 
    {
        get_new_data(Dpdt_Type).setVal(0);
    }

    is_first_step_after_regrid = false;
    old_intersect_new          = grids;

#ifdef PARTICLES
    NavierStokesBase::initParticleData();
#endif
}

void
HeatTransfer::initDataOtherTypes ()
{
    // to be consistent with Strang code, compute enthalpy with the EOS
    const Real cur_time  = state[State_Type].curTime();
    {
        MultiFab rhoh(grids,1,0);
	compute_rhohmix(cur_time,rhoh);
        get_new_data(State_Type).copy(rhoh,0,RhoH,1);
    }
    //
    // Assume that by now, S_new has "good" data
    //
    MultiFab& R = get_new_data(RhoYdot_Type);

    // It is our current belief that at the beginning of a simulation we want omegadot=0
    // This is how the Strang code does it
    //    compute_instantaneous_reaction_rates(R,get_new_data(State_Type),nGrow);
    get_new_data(RhoYdot_Type).setVal(0);

    showMFsub("1D",R,stripBox,"1D_dd_idot_R",level);

    get_new_data(FuncCount_Type).setVal(1);

    setThermoPress(state[State_Type].curTime());
}

void
HeatTransfer::compute_instantaneous_reaction_rates (MultiFab&       R,
                                                    const MultiFab& S,
						    Real            time,
                                                    int             nGrow,
                                                    HowToFillGrow   how)
{
    if (hack_nochem) 
    {
	R.setVal(0);
        R.setBndry(0,0,nspecies);
	return;
    }

    const Real strt_time = ParallelDescriptor::second();

    const TimeLevel whichTime = which_time(State_Type, time);

    BL_ASSERT((nGrow==0)  ||  (how == HT_ZERO_GROW_CELLS) || (how == HT_EXTRAP_GROW_CELLS));

    if ((nGrow>0) && (how == HT_ZERO_GROW_CELLS))
    {
        R.setBndry(0,0,nspecies);
    }
    
    const int sCompRhoH    = RhoH;
    const int sCompRhoY    = first_spec;
    const int sCompT       = Temp;
    const int sCompRhoYdot = 0;
    
    ChemDriver& chem_solve = getChemSolve();

    for (MFIter mfi(S); mfi.isValid(); ++mfi)
    {
        const FArrayBox& rhoY = S[mfi];
        const FArrayBox& rhoH = S[mfi];
        const FArrayBox& T    = S[mfi];
        const Box& box = mfi.validbox();
        FArrayBox& rhoYdot = R[mfi];

        chem_solve.reactionRateRhoY(rhoYdot,rhoY,rhoH,T,box,sCompRhoY,sCompRhoH,sCompT,sCompRhoYdot);
    }
    
    if ((nGrow>0) && (how == HT_EXTRAP_GROW_CELLS))
    {
        R.FillBoundary(0,nspecies, geom.periodicity());
	BL_ASSERT(R.nGrow() == 1);
	Extrapolater::FirstOrderExtrap(R, geom, 0, nspecies);
    }

    if (verbose > 1)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::compute_instantaneous_reaction_rates(): lev: " << level 
		      << ", time: " << run_time << '\n';
    }
} 

void
HeatTransfer::init (AmrLevel& old)
{
    NavierStokesBase::init(old);

    HeatTransfer* oldht    = (HeatTransfer*) &old;
    const Real    cur_time = oldht->state[State_Type].curTime();
    //
    // Get best ydot data.
    //
    MultiFab& Ydot = get_new_data(RhoYdot_Type);

    for (FillPatchIterator fpi(*oldht,Ydot,Ydot.nGrow(),cur_time,RhoYdot_Type,0,nspecies);
         fpi.isValid();
         ++fpi)
    {
      Ydot[fpi].copy(fpi());
    }

    RhoH_to_Temp(get_new_data(State_Type));

    MultiFab& FuncCount = get_new_data(FuncCount_Type);

    for (FillPatchIterator fpi(*oldht,FuncCount,FuncCount.nGrow(),cur_time,FuncCount_Type,0,1);
         fpi.isValid();
         ++fpi)
    {
      FuncCount[fpi].copy(fpi());
    }
}

//
// Inits the data on a new level that did not exist before regridding.
//

void
HeatTransfer::init ()
{
    NavierStokesBase::init();
 
    HeatTransfer& coarser  = getLevel(level-1);
    const Real    cur_time = coarser.state[State_Type].curTime();
    //
    // Get best ydot data.
    //
    FillCoarsePatch(get_new_data(RhoYdot_Type),0,cur_time,RhoYdot_Type,0,nspecies);

    RhoH_to_Temp(get_new_data(State_Type));
    get_new_data(State_Type).setBndry(1.e30);
    showMF("sdc",get_new_data(State_Type),"sdc_new_state",level);

    if (new_T_threshold>0)
    {
        MultiFab& crse = coarser.get_new_data(State_Type);
        MultiFab& fine = get_new_data(State_Type);

        RhoH_to_Temp(crse,0); // Make sure T is current
        Real min_T_crse = crse.min(Temp);
        Real min_T_fine = min_T_crse * std::min(1.0, new_T_threshold);

        const int* ratio = crse_ratio.getVect();
        int Tcomp = (int)Temp;
        int Rcomp = (int)Density;
        int n_tmp = std::max( (int)Density, std::max( (int)Temp, std::max( last_spec, RhoH) ) );
        Array<Real> tmp(n_tmp);
        int num_cells_hacked = 0;
        for (MFIter mfi(fine); mfi.isValid(); ++mfi)
        {
            FArrayBox& fab = fine[mfi];
            const Box& box = mfi.validbox();
            num_cells_hacked += 
                FORT_CONSERVATIVE_T_FLOOR(box.loVect(), box.hiVect(),
                                          fab.dataPtr(), ARLIM(fab.loVect()), ARLIM(fab.hiVect()),
                                          &min_T_fine, &Tcomp, &Rcomp, &first_spec, &last_spec, &RhoH,
                                          ratio, tmp.dataPtr(), &n_tmp);
        }

        ParallelDescriptor::ReduceIntSum(num_cells_hacked);

        if (num_cells_hacked > 0) {

            Real old_min = fine.min(Temp);
            RhoH_to_Temp(get_new_data(State_Type));
            Real new_min = fine.min(Temp);

            if (verbose && ParallelDescriptor::IOProcessor()) {
                std::cout << "...level data adjusted to reduce new extrema (" << num_cells_hacked
                          << " cells affected), new min = " << new_min << " (old min = " << old_min << ")" << '\n';
            }
        }
    }
    showMF("sdc",get_new_data(State_Type),"sdc_new_state_floored",level);

    FillCoarsePatch(get_new_data(FuncCount_Type),0,cur_time,FuncCount_Type,0,1);
}

void
HeatTransfer::post_timestep (int crse_iteration)
{
    NavierStokesBase::post_timestep(crse_iteration);

    if (plot_reactions && level == 0)
    {
        const int Ndiag = auxDiag["REACTIONS"]->nComp();
        //
        // Multiply by the inverse of the coarse timestep.
        //
        const Real factor = 1.0 / crse_dt;

        for (int i = parent->finestLevel(); i >= 0; --i)
            getLevel(i).auxDiag["REACTIONS"]->mult(factor);

        for (int i = parent->finestLevel(); i > 0; --i)
        {
            HeatTransfer& clev = getLevel(i-1);
            HeatTransfer& flev = getLevel(i);

            MultiFab& Ydot_crse = *(clev.auxDiag["REACTIONS"]);
            MultiFab& Ydot_fine = *(flev.auxDiag["REACTIONS"]);

	    BoxLib::average_down(Ydot_fine, Ydot_crse, flev.geom, clev.geom,
				 0, Ndiag, parent->refRatio(i-1));
        }
    }
}

void
HeatTransfer::post_restart ()
{
    NavierStokesBase::post_restart();

    make_rho_prev_time();
    make_rho_curr_time();

    Real dummy  = 0;
    int MyProc  = ParallelDescriptor::MyProc();
    int step    = parent->levelSteps(0);
    int restart = 1;

    if (do_active_control)
    {
        int usetemp = 0;
        FORT_ACTIVECONTROL(&dummy,&dummy,&crse_dt,&MyProc,&step,&restart,&usetemp);
    }
    else if (do_active_control_temp)
    {
        int usetemp = 1;
        FORT_ACTIVECONTROL(&dummy,&dummy,&crse_dt,&MyProc,&step,&restart,&usetemp);
    }
}

void
HeatTransfer::postCoarseTimeStep (Real cumtime)
{
    //
    // postCoarseTimeStep() is only called by level 0.
    //
    BL_ASSERT(level == 0);
    AmrLevel::postCoarseTimeStep(cumtime);
}

void
HeatTransfer::post_regrid (int lbase,
                           int new_finest)
{
    NavierStokesBase::post_regrid(lbase, new_finest);
    //
    // FIXME: This may be necessary regardless, unless the interpolation
    //        to fine from coarse data preserves rho=sum(rho.Y)
    //
    if (do_set_rho_to_species_sum)
    {
        const int nGrow = 0;
        if (parent->levelSteps(0)>0 && level>lbase)
            set_rho_to_species_sum(get_new_data(State_Type),0,nGrow,0);
    }
}

void
HeatTransfer::checkPoint (const std::string& dir,
                          std::ostream&      os,
                          VisMF::How         how,
                          bool               dump_old)
{
    NavierStokesBase::checkPoint(dir,os,how,dump_old);

    // typical values
    if (level == 0 && ParallelDescriptor::IOProcessor())
    {
      const std::string tvfile = dir + "/" + typical_values_filename;
      std::ofstream tvos;
      tvos.open(tvfile.c_str(),std::ios::out|std::ios::trunc|std::ios::binary);
      if (!tvos.good())
	BoxLib::FileOpenFailed(tvfile);
      Box tvbox(IntVect(),(NUM_STATE-1)*BoxLib::BASISV(0));
      int nComp = typical_values.size();
      FArrayBox tvfab(tvbox,nComp);
      for (int i=0; i<nComp; ++i) {
	tvfab.dataPtr()[i] = typical_values[i];
      }
      tvfab.writeOn(tvos);
    }

    // ambient pressure
    if (level == 0 && ParallelDescriptor::IOProcessor())
    {
      std::ofstream PambFile;
      std::string FullPathPambFile = dir;
      FullPathPambFile += "/Pamb";
      PambFile.open(FullPathPambFile.c_str(), std::ios::out);

      PambFile << std::setprecision(17) << p_amb_new;
      PambFile.close();
    }

}

void
HeatTransfer::post_init (Real stop_time)
{
    if (level > 0)
        //
        // Nothing to sync up at level > 0.
        //
        return;

    const Real strt_time    = ParallelDescriptor::second();
    const Real cur_time     = state[State_Type].curTime();
    const int  finest_level = parent->finestLevel();
    Real        dt_init     = 0.0;
    Real Sbar;

    Array<Real> dt_save(finest_level+1);
    Array<int>  nc_save(finest_level+1);
    Real        dt_init2 = 0.0;
    Array<Real> dt_save2(finest_level+1);
    Array<int>  nc_save2(finest_level+1);
    //
    // Load typical values for each state component
    //
    set_typical_values(false);

    // ensure system is solvable by creating deltaS = S - Sbar
    if (closed_chamber == 1)
    {

      // ensure divu is average down so computing deltaS = S - Sbar works for multilevel
      for (int k = finest_level-1; k >= 0; k--)
      {
	HeatTransfer&   fine_lev = getLevel(k+1);
	HeatTransfer&   crse_lev = getLevel(k);

	MultiFab& Divu_crse = crse_lev.get_new_data(Divu_Type);
	MultiFab& Divu_fine = fine_lev.get_new_data(Divu_Type);

	BoxLib::average_down(Divu_fine, Divu_crse, fine_lev.geom, crse_lev.geom,
			     0, 1, crse_lev.fine_ratio);
      }

      // compute number of cells
      Real num_cells = grids.numPts();

      // compute Sbar and subtract from S
      for (int lev = 0; lev <= finest_level; lev++)
      {
	// pointer to S
	MultiFab& divu_lev = getLevel(lev).get_new_data(Divu_Type);
	if (lev == 0)
	{
	  Sbar = divu_lev.sum() / num_cells;
	}
	divu_lev.plus(-Sbar,0,1);
      }
    }

    //
    // Ensure state is consistent, i.e. velocity field satisfies initial
    // estimate of constraint, coarse levels are fine level averages, pressure
    // is zero.
    //
    post_init_state();

    if (closed_chamber == 1)
    {
      // restore S
      for (int lev = 0; lev <= finest_level; lev++)
      {
	MultiFab& divu_lev = getLevel(lev).get_new_data(Divu_Type);
	divu_lev.plus(Sbar,0,1);
      }
    }

    //
    // Estimate the initial timestepping.
    //
    post_init_estDT(dt_init,nc_save,dt_save,stop_time);
    //
    // Better estimate needs dt to estimate divu
    //
    const bool do_iter        = do_init_proj && projector;
    const int  init_divu_iter = do_iter ? num_divu_iters : 0;

    if (verbose && ParallelDescriptor::IOProcessor())
        std::cout << "doing num_divu_iters = " << num_divu_iters << '\n';

    for (int iter = 0; iter < init_divu_iter; ++iter)
    {
        //
        // Update species destruction rates in each level but not state.
        //
        if (nspecies > 0)
        {
            for (int k = 0; k <= finest_level; k++)
            {
                MultiFab& S_new = getLevel(k).get_new_data(State_Type);
                //
                // Don't update S_new in this strang_chem() call ...
                //
                MultiFab S_tmp(S_new.boxArray(),S_new.nComp(),0);

		MultiFab Forcing_tmp(S_new.boxArray(),nspecies+1,0);
		Forcing_tmp.setVal(0);

		getLevel(k).advance_chemistry(S_new,S_tmp,dt_save[k]/2.0,Forcing_tmp,0);
            }
        }
        //
        // Recompute the velocity to obey constraint with chemistry and
        // divqrad and then average that down.
        //
        if (nspecies > 0)
        {
            for (int k = 0; k <= finest_level; k++)
            {
                MultiFab&  Divu_new = getLevel(k).get_new_data(Divu_Type);
                getLevel(k).calc_divu(cur_time,dt_save[k],Divu_new);
            }
            if (!hack_noavgdivu)
            {
                for (int k = finest_level-1; k >= 0; k--)
                {
                    HeatTransfer&   fine_lev = getLevel(k+1);
                    HeatTransfer&   crse_lev = getLevel(k);

                    MultiFab& Divu_crse = crse_lev.get_new_data(Divu_Type);
                    MultiFab& Divu_fine = fine_lev.get_new_data(Divu_Type);

		    BoxLib::average_down(Divu_fine, Divu_crse, fine_lev.geom, crse_lev.geom,
					 0, 1, crse_lev.fine_ratio);
                }
            }
            //
            // Recompute the initial velocity field based on this new constraint
            //
            const Real divu_time = state[Divu_Type].curTime();

            int havedivu = 1;

	    // ensure system is solvable by creating deltaS = S - Sbar
	    if (closed_chamber == 1)
	    {
	      // compute number of cells
	      Real num_cells = grids.numPts();

	      // compute Sbar and subtract from S
	      for (int lev = 0; lev <= finest_level; lev++)
	      {
		// pointer to S
		MultiFab& divu_lev = getLevel(lev).get_new_data(Divu_Type);
		if (lev == 0)
		{
		  Sbar = divu_lev.sum() / num_cells;
		}
		divu_lev.plus(-Sbar,0,1);
	      }
	    }

            projector->initialVelocityProject(0,divu_time,havedivu);

	    if (closed_chamber == 1)
	    {
	      // restore S
	      for (int lev = 0; lev <= finest_level; lev++)
	      {
		MultiFab& divu_lev = getLevel(lev).get_new_data(Divu_Type);
		divu_lev.plus(Sbar,0,1);
	      }
	    }

            //
            // Average down the new velocity
            //
            for (int k = finest_level-1; k >= 0; k--)
            {
	      HeatTransfer&   fine_lev = getLevel(k+1);
	      HeatTransfer&   crse_lev = getLevel(k);
	      MultiFab&       S_fine  = getLevel(k+1).get_new_data(State_Type);
	      MultiFab&       S_crse  = getLevel(k).get_new_data(State_Type);

	      BoxLib::average_down(S_fine, S_crse, fine_lev.geom, crse_lev.geom,
				   Xvel, BL_SPACEDIM, getLevel(k).fine_ratio);
            }
        }
        //
        // Estimate the initial timestepping again, using new velocity
        // (necessary?) Need to pass space to save dt, nc, but these are
        // hacked, just pass something.
        //
	// reset Ncycle to nref...
        //
	parent->setNCycle(nc_save);
        post_init_estDT(dt_init2, nc_save2, dt_save2, stop_time);
	//
	// Compute dt_init,dt_save as the minimum of the values computed
	// in the calls to post_init_estDT
	// Then setTimeLevel and dt_level to these values.
	//
	dt_init = std::min(dt_init,dt_init2);
	Array<Real> dt_level(finest_level+1,dt_init);

	parent->setDtLevel(dt_level);
	for (int k = 0; k <= finest_level; k++)
        {
	    dt_save[k] = std::min(dt_save[k],dt_save2[k]);
	    getLevel(k).setTimeLevel(cur_time,dt_init,dt_init);
        }
    } // end divu_iters

    //
    // Initialize the pressure by iterating the initial timestep.
    //
    post_init_press(dt_init, nc_save, dt_save);
    //
    // Compute the initial estimate of conservation.
    //
    if (sum_interval > 0)
        sum_integrated_quantities();

    if (verbose)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::post_init(): lev: " << level << ", time: " << run_time << '\n';
    }
}

void
HeatTransfer::sum_integrated_quantities ()
{
    const int finest_level = parent->finestLevel();
    const Real time        = state[State_Type].curTime();

    Real mass = 0.0;
    for (int lev = 0; lev <= finest_level; lev++)
	mass += getLevel(lev).volWgtSum("density",time);

    if (verbose && ParallelDescriptor::IOProcessor())
        std::cout << "TIME= " << time << " MASS= " << mass;

    if (getChemSolve().index(fuelName) >= 0)
    {
        int MyProc  = ParallelDescriptor::MyProc();
        int step    = parent->levelSteps(0);
        int restart = 0;

        if (do_active_control)
        {
            Real fuelmass = 0.0;
            std::string fuel = "rho.Y(" + fuelName + ")";
            for (int lev = 0; lev <= finest_level; lev++)
                fuelmass += getLevel(lev).volWgtSum(fuel,time);

            if (verbose && ParallelDescriptor::IOProcessor())
                std::cout << " FUELMASS= " << fuelmass;

            int usetemp = 0;

            FORT_ACTIVECONTROL(&fuelmass,&time,&crse_dt,&MyProc,&step,&restart,&usetemp);
        }
        else if (do_active_control_temp)
        {
            const int   DM     = BL_SPACEDIM-1;
            const Real* dx     = geom.CellSize();
            const Real* problo = geom.ProbLo();
            Real        hival  = geom.ProbHi(DM);
            const Real  time   = state[State_Type].curTime();
            MultiFab&   mf     = get_new_data(State_Type);

            for (FillPatchIterator Tfpi(*this,mf,1,time,State_Type,Temp,1);
                 Tfpi.isValid();
                 ++Tfpi)
            {
                const FArrayBox& fab  = Tfpi();
                const Box&       vbox = Tfpi.validbox();

                for (IntVect iv = vbox.smallEnd(); iv <= vbox.bigEnd(); vbox.next(iv))
                {
                    const Real T_hi = fab(iv);

                    if (T_hi > temp_control)
                    {
                        const Real hi = problo[DM] + (iv[DM] + .5) * dx[DM];

                        if (hi < hival)
                        {
                            hival = hi;

                            IntVect lo_iv = iv;

                            lo_iv[DM] -= 1;

                            const Real T_lo = fab(lo_iv);

                            if (T_lo < temp_control)
                            {
                                const Real lo    = problo[DM] + (lo_iv[DM] + .5) * dx[DM];
                                const Real slope = (T_hi - T_lo) / (hi - lo);

                                hival = (temp_control - T_lo) / slope + lo;
                            }
                        }
                    }
                }
            }

            ParallelDescriptor::ReduceRealMin(hival);

            if (verbose && ParallelDescriptor::IOProcessor())
                std::cout << " HIVAL= " << hival;

            int usetemp = 1;

            FORT_ACTIVECONTROL(&hival,&time,&crse_dt,&MyProc,&step,&restart,&usetemp);
        }
	else
	{
	  Real fuelmass = 0.0;
	  std::string fuel = "rho.Y(" + fuelName + ")";
	  for (int lev = 0; lev <= finest_level; lev++)
	    fuelmass += getLevel(lev).volWgtSum(fuel,time);
	  
	  if (verbose && ParallelDescriptor::IOProcessor())
	    std::cout << " FUELMASS= " << fuelmass;
	}
    }

    if (getChemSolve().index(productName) >= 0)
    {
        int MyProc  = ParallelDescriptor::MyProc();
        int step    = parent->levelSteps(0);
        int restart = 0;

	  Real productmass = 0.0;
	  std::string product = "rho.Y(" + productName + ")";
	  for (int lev = 0; lev <= finest_level; lev++)
	    productmass += getLevel(lev).volWgtSum(product,time);
	  
	  if (verbose && ParallelDescriptor::IOProcessor())
	    std::cout << " PRODUCTMASS= " << productmass;
    }

    if (verbose && ParallelDescriptor::IOProcessor())
        std::cout << '\n';

    Real rho_h    = 0.0;
    Real rho_temp = 0.0;

    for (int lev = 0; lev <= finest_level; lev++)
    {
        rho_temp += getLevel(lev).volWgtSum("rho_temp",time);
        rho_h     = getLevel(lev).volWgtSum("rhoh",time);
        if (verbose && ParallelDescriptor::IOProcessor())
            std::cout << "TIME= " << time << " LEV= " << lev << " RHOH= " << rho_h << '\n';
    }
    if (verbose && ParallelDescriptor::IOProcessor())
        std::cout << "TIME= " << time << " RHO*TEMP= " << rho_temp << '\n';

    if (verbose) {

        int old_prec = std::cout.precision(15);
        int nc=BL_SPACEDIM+1;
        Array<Real> vmin(nc), vmax(nc);
        Array<int> comps(nc);
        for (int n=0; n<BL_SPACEDIM; ++n) {
            comps[n] = Xvel+n;
        }
        comps[nc-1] = Temp;
        
        for (int lev = 0; lev <= finest_level; lev++)
        {
            MultiFab& newstate = getLevel(lev).get_data(State_Type,time);
            for (int n=0; n<=BL_SPACEDIM; ++n) {
                Real thismin = newstate.min(comps[n],0);
                Real thismax = newstate.max(comps[n],0);

                if (lev==0) {
                    vmin[n] = thismin;
                    vmax[n] = thismax;
                } else {
                    vmin[n] = std::min(thismin,vmin[n]);
                    vmax[n] = std::max(thismax,vmax[n]);
                }
            }
        }

        if (ParallelDescriptor::IOProcessor())
        {
            std::cout << "TIME= " << time 
		      << " min,max temp = " << vmin[nc-1] << ", " << vmax[nc-1] << '\n';
            for (int n=0; n<BL_SPACEDIM; ++n) {
                std::string str = (n==0 ? "xvel" : (n==1 ? "yvel" : "zvel") );
                std::cout << "TIME= " << time 
			  << " min,max "<< str << "  = " << vmin[Xvel+n] 
			  << ", " << vmax[Xvel+n] << '\n';
            }
        }

        if (nspecies > 0)
        {
            Real min_sum, max_sum;
            for (int lev = 0; lev <= finest_level; lev++) {
                MultiFab* mf = getLevel(lev).derive("rhominsumrhoY",time,0);
                Real this_min = mf->min(0);
                Real this_max = mf->max(0);
                if (lev==0) {
                    min_sum = this_min;
                    max_sum = this_max;
                } else {
                    min_sum = std::min(this_min,min_sum);
                    max_sum = std::max(this_max,max_sum);
                }
                delete mf;
            }
            
            if (ParallelDescriptor::IOProcessor()) {
                std::cout << "TIME= " << time 
			  << " min,max rho-sum rho Y_l = "
                          << min_sum << ", " << max_sum << '\n';
            }
            
            for (int lev = 0; lev <= finest_level; lev++)
            {
                MultiFab* mf = getLevel(lev).derive("sumRhoYdot",time,0);
                Real this_min = mf->min(0);
                Real this_max = mf->max(0);
                if (lev==0) {
                    min_sum = this_min;
                    max_sum = this_max;
                } else {
                    min_sum = std::min(this_min,min_sum);
                    max_sum = std::max(this_max,max_sum);
                }
                delete mf;
            }
            
            if (ParallelDescriptor::IOProcessor()) {
                std::cout << "TIME= " << time 
			  << " min,max sum RhoYdot = "
                          << min_sum << ", " << max_sum << '\n';
            }
        }       

        std::cout << std::setprecision(old_prec);
    }
}
    
void
HeatTransfer::post_init_press (Real&        dt_init,
			       Array<int>&  nc_save,
			       Array<Real>& dt_save)
{
    const int  nState          = desc_lst[State_Type].nComp();
    const int  nGrow           = 0;
    const Real cur_time        = state[State_Type].curTime();
    const int  finest_level    = parent->finestLevel();
    NavierStokesBase::initial_iter = true;
    Real Sbar_old, Sbar_new;
    //
    // Make space to save a copy of the initial State_Type state data
    //
    PArray<MultiFab> saved_state(finest_level+1,PArrayManage);

    if (init_iter > 0)
        for (int k = 0; k <= finest_level; k++)
            saved_state.set(k,new MultiFab(getLevel(k).grids,nState,nGrow));

    //
    // Iterate over the advance function.
    //
    for (int iter = 0; iter < init_iter; iter++)
    {
	//
	// Squirrel away copy of pre-advance State_Type state
	//
        for (int k = 0; k <= finest_level; k++)
	    MultiFab::Copy(saved_state[k],
                           getLevel(k).get_new_data(State_Type),
			   0,
                           0,
                           nState,
                           nGrow);

        for (int k = 0; k <= finest_level; k++ )
        {
            getLevel(k).advance(cur_time,dt_init,1,1);
        }
        //
        // This constructs a guess at P, also sets p_old == p_new.
        //
        MultiFab** sig = new MultiFab*[finest_level+1];

        for (int k = 0; k <= finest_level; k++)
        {
            sig[k] = &(getLevel(k).get_rho_half_time());
        }

	// ensure system is solvable by creating deltaS = S - Sbar
	if (closed_chamber == 1)
	{

	  // compute number of cells
	  Real num_cells = grids.numPts();

	  // ensure divu_new is average down so computing deltaS = S - Sbar works for multilevel
	  for (int k = finest_level-1; k >= 0; k--)
	  {
	    HeatTransfer&   fine_lev = getLevel(k+1);
	    HeatTransfer&   crse_lev = getLevel(k);
	    
	    MultiFab& Divu_crse = crse_lev.get_new_data(Divu_Type);
	    MultiFab& Divu_fine = fine_lev.get_new_data(Divu_Type);

	    BoxLib::average_down(Divu_fine, Divu_crse, fine_lev.geom, crse_lev.geom,
				 0, 1, crse_lev.fine_ratio);
	  }

	  // compute Sbar and subtract from S
	  for (int lev = 0; lev <= finest_level; lev++)
	  {
	    // pointer to S
	    MultiFab& divu_lev = getLevel(lev).get_new_data(Divu_Type);
	    if (lev == 0)
	    {
	      Sbar_new = divu_lev.sum() / num_cells;
	    }
	    divu_lev.plus(-Sbar_new,0,1);
	  }
	  
	  // ensure divu_old is average down so computing deltaS = S - Sbar works for multilevel
	  for (int k = finest_level-1; k >= 0; k--)
	  {
	    HeatTransfer&   fine_lev = getLevel(k+1);
	    HeatTransfer&   crse_lev = getLevel(k);
		  
	    MultiFab& Divu_crse = crse_lev.get_old_data(Divu_Type);
	    MultiFab& Divu_fine = fine_lev.get_old_data(Divu_Type);

	    BoxLib::average_down(Divu_fine, Divu_crse, fine_lev.geom, crse_lev.geom,
				 0, 1, crse_lev.fine_ratio);
	  }

	  // compute Sbar and subtract from S
	  for (int lev = 0; lev <= finest_level; lev++)
	  {
	    // pointer to S
	    MultiFab& divu_lev = getLevel(lev).get_old_data(Divu_Type);
	    if (lev == 0)
	    {
	      Sbar_old = divu_lev.sum() / num_cells;
	    }
	    divu_lev.plus(-Sbar_old,0,1);
	  }
	}

        if (projector)
	  {
            int havedivu = 1;
	    projector->initialSyncProject(0,sig,parent->dtLevel(0),cur_time,
					  havedivu);
	  }
	
	if (closed_chamber == 1)
	{
	  // restore S_new
	  for (int lev = 0; lev <= finest_level; lev++)
	  {
	    MultiFab& divu_lev = getLevel(lev).get_new_data(Divu_Type);
	    divu_lev.plus(Sbar_new,0,1);
	  }

	  // restore S_old
	  for (int lev = 0; lev <= finest_level; lev++)
	  {
	    MultiFab& divu_lev = getLevel(lev).get_old_data(Divu_Type);
	    divu_lev.plus(Sbar_old,0,1);
	  }

	}

        delete [] sig;

        for (int k = finest_level-1; k>= 0; k--)
        {
            getLevel(k).avgDown();
        }
        for (int k = 0; k <= finest_level; k++)
        {
            //
            // Reset state variables to initial time, but
            // do not pressure variable, only pressure time.
            //
            getLevel(k).resetState(cur_time, dt_init, dt_init);
        }
	//
	// For State_Type state, restore state we saved above
	//
        for (int k = 0; k <= finest_level; k++)
	    MultiFab::Copy(getLevel(k).get_new_data(State_Type),
                           saved_state[k],
			   0,
                           0,
                           nState,
                           nGrow);

        NavierStokesBase::initial_iter = false;
    }

    if (init_iter <= 0)
        NavierStokesBase::initial_iter = false; // Just being compulsive -- rbp.

    NavierStokesBase::initial_step = false;
    //
    // Re-instate timestep.
    //
    for (int k = 0; k <= finest_level; k++)
    {
        getLevel(k).setTimeLevel(cur_time,dt_save[k],dt_save[k]);
        if (getLevel(k).state[Press_Type].descriptor()->timeType() ==
            StateDescriptor::Point)
          getLevel(k).state[Press_Type].setNewTimeLevel(cur_time+.5*dt_init);
    }
    parent->setDtLevel(dt_save);
    parent->setNCycle(nc_save);
}

//
// Reset the time levels to time (time) and timestep dt.
// This is done at the start of the timestep in the pressure
// iteration section.
//

void
HeatTransfer::resetState (Real time,
                          Real dt_old,
                          Real dt_new)
{
    NavierStokesBase::resetState(time,dt_old,dt_new);

    state[RhoYdot_Type].reset();
    state[RhoYdot_Type].setTimeLevel(time,dt_old,dt_new);

    state[FuncCount_Type].reset();
    state[FuncCount_Type].setTimeLevel(time,dt_old,dt_new);
}

void
HeatTransfer::avgDown ()
{
    if (level == parent->finestLevel()) return;

    const Real      strt_time = ParallelDescriptor::second();
    HeatTransfer&   fine_lev  = getLevel(level+1);
    MultiFab&       S_crse    = get_new_data(State_Type);
    MultiFab&       S_fine    = fine_lev.get_new_data(State_Type);

    BoxLib::average_down(S_fine, S_crse, fine_lev.geom, geom,
			 0, S_crse.nComp(), fine_ratio);
    //
    // Fill rho_ctime at the current and finer levels with the correct data.
    //
    for (int lev = level; lev <= parent->finestLevel(); lev++)
    {
        getLevel(lev).make_rho_curr_time();
    }
    //
    // Reset the temperature
    //
    RhoH_to_Temp(S_crse);
    //
    // Now average down pressure over time n-(n+1) interval.
    //
    MultiFab&       P_crse      = get_new_data(Press_Type);
    MultiFab&       P_fine_init = fine_lev.get_new_data(Press_Type);
    MultiFab&       P_fine_avg  = fine_lev.p_avg;
    MultiFab&       P_fine      = initial_step ? P_fine_init : P_fine_avg;
    const BoxArray& P_fgrids    = fine_lev.state[Press_Type].boxArray();

    BoxArray crse_P_fine_BA = P_fgrids;

    crse_P_fine_BA.coarsen(fine_ratio);

    MultiFab crse_P_fine(crse_P_fine_BA,1,0);

    for (MFIter mfi(P_fine); mfi.isValid(); ++mfi)
    {
        const int i = mfi.index();

        injectDown(crse_P_fine_BA[i],crse_P_fine[mfi],P_fine[mfi],fine_ratio);
    }

    P_crse.copy(crse_P_fine, parent->Geom(level).periodicity());  // Parallel copy

    crse_P_fine.clear();
    //
    // Next average down divu and dSdT at new time.
    //
    if (hack_noavgdivu) 
    {
        //
        // Now that state averaged down, recompute divu (don't avgDown,
        // since that will give a very different value, and screw up mac)
        //
        StateData& divuSD = get_state_data(Divu_Type);// should be const...
        const Real time   = divuSD.curTime();
        const Real dt     = time - divuSD.prevTime();
        calc_divu(time,dt,divuSD.newData());
    }
    else
    {
        MultiFab& Divu_crse = get_new_data(Divu_Type);
        MultiFab& Divu_fine = fine_lev.get_new_data(Divu_Type);
            
	BoxLib::average_down(Divu_fine, Divu_crse, fine_lev.geom, geom,
			     0, Divu_crse.nComp(), fine_ratio);
    }

    if (hack_noavgdivu)
    {
        //
        //  Now that have good divu, recompute dsdt (don't avgDown,
        //   since that will give a very different value, and screw up mac)
        //
        StateData& dsdtSD = get_state_data(Dsdt_Type);// should be const...
        MultiFab&  dsdt   = dsdtSD.newData();

        if (get_state_data(Divu_Type).hasOldData())
        {
            const Real time = dsdtSD.curTime();
            const Real dt   = time - dsdtSD.prevTime();
            calc_dsdt(time,dt,dsdt);
        }
        else
        {
            dsdt.setVal(0.0);
        }
    }
    else
    {
        MultiFab& Dsdt_crse = get_new_data(Dsdt_Type);
        MultiFab& Dsdt_fine = fine_lev.get_new_data(Dsdt_Type);
            
	BoxLib::average_down(Dsdt_fine, Dsdt_crse, fine_lev.geom, geom,
			     0, Dsdt_crse.nComp(), fine_ratio);
    }

    if (verbose > 1)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::avgDown(): lev: " << level << ", time: " << run_time << '\n';
    }
}

void
HeatTransfer::differential_diffusion_update (MultiFab& Force,
                                             int       FComp,
                                             MultiFab& Dnew,
                                             int       DComp,
                                             MultiFab& DDnew)
{
  BL_PROFILE_REGION_START("R::HT::differential_diffusion_update()");
  BL_PROFILE("HT::differential_diffusion_update()");
  //
  // Recompute the D[Nspec+1] = Div(Fi.Hi) using
  // Fi.Hi based on solution here.
  //
  BL_ASSERT(Force.boxArray() == grids);
  BL_ASSERT(FComp+Force.nComp()>=nspecies+1);
  BL_ASSERT(Dnew.boxArray() == grids);
  BL_ASSERT(DDnew.boxArray() == grids);
  BL_ASSERT(DComp+Dnew.nComp()>=nspecies+1);

  const Real strt_time = ParallelDescriptor::second();

  if (hack_nospecdiff)
  {
    BoxLib::Error("differential_diffusion_update: hack_nospecdiff not implemented");
  }
  MultiFab& S_old = get_old_data(State_Type);
  MultiFab& S_new = get_new_data(State_Type);
  Real prev_time = state[State_Type].prevTime();
  Real curr_time = state[State_Type].curTime();
  Real dt = curr_time - prev_time;
  BL_ASSERT(dt>0);
  const int nGrow = 0;

  MultiFab::Copy(S_new,S_old,first_spec,first_spec,nspecies+1,0);
  int rho_flag = 2;
  MultiFab rho_half;
  Diffusion::SolveMode solve_mode = Diffusion::ONEPASS;
  MultiFab **betanp1, **betan = 0; // Will not need betan since time-explicit pieces computed above
  FluxBoxes fb_betap1(this, nspecies+2, nGrow);
  betanp1 = fb_betap1.get();
  getDiffusivity(betanp1, curr_time, first_spec, 0, nspecies+1); // species (rhoD) and RhoH (lambda/cp)
  getDiffusivity(betanp1, curr_time, Temp, nspecies+1, 1); // temperature (lambda)
  //
  // Diffuse RhoY (and RhoH, unless theta_enthalpy>0 -- if so, will modify the source term for
  //   RhoH with the results of the RhoY diffusion)
  //
  MultiFab* alpha = 0; // Never need alpha for RhoY, RhoH
  int alphaComp = 0;
  int nComp = nspecies+1;
  for (int sigma = 0; sigma < nComp; ++sigma)
  {
    int betaComp = sigma;
    const int state_ind = first_spec + sigma;
    bool add_old_time_divFlux = false; // indicate that the rhs contains the time-explicit diff terms already
    diffusion->diffuse_scalar(dt,state_ind,1.0,rho_half,rho_flag,
			      SpecDiffusionFluxn,SpecDiffusionFluxnp1,sigma,&Force,sigma,alpha,
			      alphaComp,betan,betanp1,betaComp,solve_mode,add_old_time_divFlux);
  }

#ifdef USE_WBAR
  // add lagged grad Wbar fluxes (SpecDiffusionFluxWbar) to time-advanced 
  // species diffusion fluxes (SpecDiffusionFluxnp1)
  for (int d=0; d<BL_SPACEDIM; ++d)
  {
    for (MFIter mfi(*SpecDiffusionFluxWbar[d]); mfi.isValid(); ++mfi)
    {
      const Box& ebox = (*SpecDiffusionFluxWbar[d])[mfi].box();
      (*SpecDiffusionFluxnp1[d])[mfi].plus((*SpecDiffusionFluxWbar[d])[mfi],ebox,0,0,nspecies);
    }
  }
#endif

  //
  // Modify/update new-time fluxes to ensure sum of species fluxes = 0
  //
  adjust_spec_diffusion_fluxes(curr_time);
  //
  // AJN FLUXREG
  // We have just performed the correction diffusion solve for Y_m and h
  // If updateFluxReg=T, we update VISCOUS flux registers:
  //   ADD Gamma_{m,AD}^(k+1),
  //   ADD lambda^(k)/cp^(k) grad h_AD^(k+1).
  //
  if (do_reflux && updateFluxReg)
  {
    for (int d = 0; d < BL_SPACEDIM; d++)
    {
      if (level > 0)
      {
	getViscFluxReg().FineAdd(*SpecDiffusionFluxnp1[d],d,
				 0,first_spec,nspecies+1,dt);
      }
      if (level < parent->finestLevel())
      {
	getLevel(level+1).getViscFluxReg().CrseInit((*SpecDiffusionFluxnp1[d]),d,
						    0,first_spec,nspecies+1,-dt,
						    FluxRegister::ADD);
      }
    }
  }

  flux_divergence(Dnew,DComp,SpecDiffusionFluxnp1,0,nComp,-1);
    
  if (verbose)
  {
    const int IOProc   = ParallelDescriptor::IOProcessorNumber();
    Real      run_time = ParallelDescriptor::second() - strt_time;

    ParallelDescriptor::ReduceRealMax(run_time,IOProc);

    if (ParallelDescriptor::IOProcessor())
      std::cout << "HeatTransfer::differential_diffusion_update(): lev: " << level 
		<< ", time: " << run_time << '\n';
  }

  BL_PROFILE_REGION_STOP("R::HT::differential_diffusion_update()");
}

void
HeatTransfer::adjust_spec_diffusion_fluxes (Real time)
{
    //
    // In this function we explicitly adjust the species diffusion fluxes so that their sum
    // is zero.  The fluxes are class member data, either SpecDiffusionFluxn or 
    // SpecDiffusionFluxnp1, depending on time
    //
    const Real      strt_time = ParallelDescriptor::second();
    const TimeLevel whichTime = which_time(State_Type,time);

    BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);    

    MultiFab* const * flux = (whichTime == AmrOldTime) ? SpecDiffusionFluxn : SpecDiffusionFluxnp1;

    MultiFab& S = get_data(State_Type,time);
    //
    // Fill grow cells in the state for RhoY, Temp
    // For Dirichlet physical boundary grow cells, this data will live on the cell face, otherwise
    // it will live at cell centers.
    //
    const int nGrow = 1;
    BL_ASSERT(S.nGrow()>=nGrow);
    for (FillPatchIterator Tfpi(*this,S,nGrow,time,State_Type,Temp,1),
                           Yfpi(*this,S,nGrow,time,State_Type,first_spec,nspecies);
         Yfpi.isValid() && Tfpi.isValid();
         ++Yfpi, ++Tfpi)
    {
        const Box& vbox = Yfpi.validbox();
        FArrayBox& fab = S[Yfpi];
        BoxList gcells = BoxLib::boxDiff(Box(vbox).grow(nGrow),vbox);
        for (BoxList::const_iterator it = gcells.begin(), end = gcells.end(); it != end; ++it)
        {
            const Box& gbox = *it;
            fab.copy(Tfpi(),gbox,0,gbox,Temp,1);
            fab.copy(Yfpi(),gbox,0,gbox,first_spec,nspecies);
        }
    }
    //
    // Get boundary info for Y (assume all Ys have the same boundary type.
    //
    const BCRec& Ybc = get_desc_lst()[State_Type].getBC(first_spec);
    // 
    // The following REPAIR_FLUX routine modifies the fluxes of all the species
    // to ensure that they sum to zero.  It requires the RhoY on valid + 1 grow (and, at least
    // as of this writing actually used the values of RhoY on edges that it gets by arithmetic
    // averaging.
    //
    const Box& domain = geom.Domain();
    for (MFIter mfi(S); mfi.isValid(); ++mfi)
    {
        const Box& box   = mfi.validbox();
        FArrayBox& Y = S[mfi];
        int sCompY=first_spec;

        for (int d =0; d < BL_SPACEDIM; ++d)
        {
            FArrayBox& f = (*flux[d])[mfi];
            FORT_REPAIR_FLUX(box.loVect(), box.hiVect(), domain.loVect(), domain.hiVect(),
                             f.dataPtr(),      ARLIM(f.loVect()),ARLIM(f.hiVect()),
                             Y.dataPtr(sCompY),ARLIM(Y.loVect()),ARLIM(Y.hiVect()),
                             &d, Ybc.vect());
        }
    }

    if (verbose > 1)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::adjust_spec_diffusion_fluxes(): lev: " << level << ", time: " << run_time << '\n';
    }
}

void
HeatTransfer::compute_enthalpy_fluxes (Real                   time,
				       const MultiFab* const* beta)
{
    BL_ASSERT(beta && beta[0]->nComp() == nspecies+2);

    const Real      strt_time = ParallelDescriptor::second();
    const TimeLevel whichTime = which_time(State_Type,time);

    BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);    

    MultiFab* const * flux = (whichTime == AmrOldTime) ? SpecDiffusionFluxn : SpecDiffusionFluxnp1;

    MultiFab& S = get_data(State_Type,time);

    MultiFab& sumSpecFluxDotGradH = (whichTime == AmrOldTime) ? sumSpecFluxDotGradHn : sumSpecFluxDotGradHnp1;
    //
    //  Compute species enthalpy on the edges, and increment the heat flux with the 
    //  flux of enthalpy due to species diffusion.  While here, we also compute the Fi.Grad(Hi) term
    //  required for the temperature equation.  Both the fluxes and the Fi.Grad(Hi) terms are stored
    //  in the class data.
    //
    const Box&   domain = geom.Domain();
    const BCRec& Tbc    = get_desc_lst()[State_Type].getBC(Temp);
    //
    // Fill ghost cells for rhoY and Temp.
    //
    for (FillPatchIterator rYfpi(*this,S,1,time,State_Type,first_spec,nspecies),
                            Tfpi(*this,S,1,time,State_Type,Temp,1);
         rYfpi.isValid() && Tfpi.isValid();
         ++rYfpi,++Tfpi)
    {
        S[rYfpi].copy(rYfpi(),0,first_spec,nspecies);
        S[rYfpi].copy(Tfpi(),0,Temp,1);
    }

    for (MFIter mfi(S); mfi.isValid(); ++mfi)
    {
        const Box& box = mfi.validbox();
            
        int              FComp    = 0;
        int              TComp    = Temp;
        int              RhoYComp = first_spec;
        int              dComp    = 0;
        FArrayBox&       fh       = sumSpecFluxDotGradH[mfi];            
        FArrayBox&       T        = S[mfi];
        FArrayBox&       RhoY     = S[mfi];
        const FArrayBox& rDx      = (*beta[0])[mfi];
        FArrayBox&       fix      = (*flux[0])[mfi];
        const FArrayBox& rDy      = (*beta[1])[mfi];
        FArrayBox&       fiy      = (*flux[1])[mfi];
        const Real*      dx       = geom.CellSize();
            
#if BL_SPACEDIM == 3        
        const FArrayBox& rDz      = (*beta[2])[mfi];
        FArrayBox&       fiz      = (*flux[2])[mfi];
#endif

        FORT_ENTH_DIFF_TERMS(box.loVect(), box.hiVect(), domain.loVect(), domain.hiVect(), dx,
                             T.dataPtr(TComp), ARLIM(T.loVect()),  ARLIM(T.hiVect()),
                             RhoY.dataPtr(RhoYComp), ARLIM(RhoY.loVect()),  ARLIM(RhoY.hiVect()),
                                 
                             rDx.dataPtr(dComp),ARLIM(rDx.loVect()),ARLIM(rDx.hiVect()),
                             fix.dataPtr(FComp),ARLIM(fix.loVect()),ARLIM(fix.hiVect()),
                             area[0][mfi].dataPtr(), ARLIM(area[0][mfi].loVect()),ARLIM(area[0][mfi].hiVect()),
                                 
                             rDy.dataPtr(dComp),ARLIM(rDy.loVect()),ARLIM(rDy.hiVect()),
                             fiy.dataPtr(FComp),ARLIM(fiy.loVect()),ARLIM(fiy.hiVect()),
                             area[1][mfi].dataPtr(), ARLIM(area[1][mfi].loVect()),ARLIM(area[1][mfi].hiVect()),
#if BL_SPACEDIM == 3
                             rDz.dataPtr(dComp),ARLIM(rDz.loVect()),ARLIM(rDz.hiVect()),
                             fiz.dataPtr(FComp),ARLIM(fiz.loVect()),ARLIM(fiz.hiVect()),
                             area[2][mfi].dataPtr(), ARLIM(area[2][mfi].loVect()),ARLIM(area[2][mfi].hiVect()),
#endif
                             fh.dataPtr(),     ARLIM(fh.loVect()), ARLIM(fh.hiVect()),
                             Tbc.vect() );
    }

    if (verbose > 1)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::compute_enthalpy_fluxes(): lev: " << level << ", time: " << run_time << '\n';
    }
}
    
void
HeatTransfer::velocity_diffusion_update (Real dt)
{

    //
    // Do implicit c-n solve for velocity
    // compute the viscous forcing
    // do following except at initial iteration--rbp, per jbb
    //
    if (is_diffusive[Xvel])
    {
        const Real strt_time = ParallelDescriptor::second();

        int rho_flag;
        if (do_mom_diff == 0)
        {
           rho_flag = 1;
        }
        else
        {
           rho_flag = 3;
        }

        MultiFab *delta_rhs = 0;
	FluxBoxes fb_betan, fb_betanp1;

        diffuse_velocity_setup(dt, delta_rhs, fb_betan, fb_betanp1);

        int rhsComp  = 0;
        int betaComp = 0;
        diffusion->diffuse_velocity(dt,be_cn_theta,get_rho_half_time(),rho_flag,
                                    delta_rhs,rhsComp,fb_betan.get(),fb_betanp1.get(),betaComp);

	delete delta_rhs;

        if (verbose > 1)
        {
            const int IOProc   = ParallelDescriptor::IOProcessorNumber();
            Real      run_time = ParallelDescriptor::second() - strt_time;

            ParallelDescriptor::ReduceRealMax(run_time,IOProc);

            if (ParallelDescriptor::IOProcessor())
                std::cout << "HeatTransfer::velocity_diffusion_update(): lev: " << level << ", time: " << run_time << '\n';
        }
    }
}
 
void
HeatTransfer::diffuse_velocity_setup (Real        dt,
                                      MultiFab*&  delta_rhs,
                                      FluxBoxes&  fb_betan,
                                      FluxBoxes&  fb_betanp1)
{
    //
    // Do setup for implicit c-n solve for velocity.
    //
    BL_ASSERT(delta_rhs==0);
    const Real time = state[State_Type].prevTime();
    //
    // Assume always variable viscosity.
    //
    MultiFab** betan = fb_betan.define(this);
    MultiFab** betanp1 = fb_betanp1.define(this);
    
    getViscosity(betan, time);
    getViscosity(betanp1, time+dt);
    //
    // Do the following only if it is a non-trivial operation.
    //
    if (S_in_vel_diffusion)
    {
        //
        // Include div mu S*I terms in rhs
        //  (i.e. make nonzero delta_rhs to add into RHS):
        //
        // The scalar and tensor solvers incorporate the relevant pieces of
        //  of Div(tau), provided the flow is divergence-free.  Howeever, if
        //  Div(U) =/= 0, there is an additional piece not accounted for,
        //  which is of the form A.Div(U).  For constant viscosity, Div(tau)_i
        //  = Lapacian(U_i) + mu/3 d[Div(U)]/dx_i.  For mu not constant,
        //  Div(tau)_i = d[ mu(du_i/dx_j + du_j/dx_i) ]/dx_i - 2mu/3 d[Div(U)]/dx_i
        //
        // As a convenience, we treat this additional term as a "source" in
        // the diffusive solve, computing Div(U) in the "normal" way we
        // always do--via a call to calc_divu.  This routine computes delta_rhs
        // if necessary, and stores it as an auxilliary rhs to the viscous solves.
        // This is a little strange, but probably not bad.
        //
        delta_rhs = new MultiFab(grids,BL_SPACEDIM,0);
        delta_rhs->setVal(0);
      
        MultiFab divmusi(grids,BL_SPACEDIM,0);
        //
	// Assume always variable viscosity.
        //
	diffusion->compute_divmusi(time,betan,divmusi);
	divmusi.mult((-2./3.)*(1.0-be_cn_theta),0,BL_SPACEDIM,0);
        (*delta_rhs).plus(divmusi,0,BL_SPACEDIM,0);
        //
	// Assume always variable viscosity.
        //
	diffusion->compute_divmusi(time+dt,betanp1,divmusi);
	divmusi.mult((-2./3.)*be_cn_theta,0,BL_SPACEDIM,0);
        (*delta_rhs).plus(divmusi,0,BL_SPACEDIM,0);
    }
}

void
HeatTransfer::getViscTerms (MultiFab& visc_terms,
                            int       src_comp, 
                            int       num_comp,
                            Real      time)
{
    const Real strt_time = ParallelDescriptor::second();
    //
    // Load "viscous" terms, starting from component = 0.
    //
    // JFG: for species, this procedure returns the *negative* of the divergence of 
    // of the diffusive fluxes.  specifically, in the mixture averaged case, the
    // diffusive flux vector for species k is
    //
    //       j_k = - rho D_k,mix grad Y_k
    //
    // so the divergence of the flux, div dot j_k, has a negative in it.  instead 
    // this procedure returns - div dot j_k to remove the negative.
    //
    // note the fluxes used in the code are extensive, that is, scaled by the areas
    // of the cell edges.  the calculation of the divergence is the sum of un-divided
    // differences of the extensive fluxes, all divided by volume.  so the effect is 
    // to give the true divided difference approximation to the divergence of the
    // intensive flux.
    //
    MultiFab** vel_visc  = 0;        // Potentially reused, raise scope
    FluxBoxes fb;
    const int  nGrow     = visc_terms.nGrow();
    //
    // Get Div(tau) from the tensor operator, if velocity and have non-const viscosity
    //
    visc_terms.setBndry(1.e30);
    if (src_comp < BL_SPACEDIM)
    {
        if (src_comp != Xvel || num_comp < BL_SPACEDIM)
            BoxLib::Error("tensor v -> getViscTerms needs all v-components at once");

        vel_visc = fb.define(this);
        getViscosity(vel_visc, time);

        showMF("velVT",*viscn_cc,"velVT_viscn_cc",level);
        for (int dir=0; dir<BL_SPACEDIM; ++dir) {
            showMF("velVT",*(vel_visc[dir]),BoxLib::Concatenate("velVT_viscn_",dir,1),level);
        }

        int viscComp = 0;
        diffusion->getTensorViscTerms(visc_terms,time,vel_visc,viscComp);
        showMF("velVT",visc_terms,"velVT_visc_terms_1",level);
    }
    else
    {
      BoxLib::Abort("Should only call getViscTerms for velocity");
    }
    //
    // Add Div(u) term if desired, if this is velocity, and if Div(u) is nonzero
    // If const-visc, term is mu.Div(u)/3, else it's -Div(mu.Div(u).I)*2/3
    //
    if (src_comp < BL_SPACEDIM && S_in_vel_diffusion)
    {
        if (num_comp < BL_SPACEDIM)
            BoxLib::Error("getViscTerms() need all v-components at once");
    
        MultiFab divmusi(grids,BL_SPACEDIM,0);
        showMF("velVT",get_old_data(Divu_Type),"velVT_divu",level);
        //
	// Assume always using variable viscosity.
        //
	diffusion->compute_divmusi(time,vel_visc,divmusi); // pre-computed visc above
	divmusi.mult((-2./3.),0,BL_SPACEDIM,0);
        showMF("velVT",divmusi,"velVT_divmusi",level);
        visc_terms.plus(divmusi,Xvel,BL_SPACEDIM,0);
        showMF("velVT",visc_terms,"velVT_visc_terms_3",level);
    }
    //
    // Ensure consistent grow cells
    //
    if (nGrow > 0)
    {
        visc_terms.FillBoundary(0,num_comp, geom.periodicity());
	BL_ASSERT(visc_terms.nGrow() == 1);
	Extrapolater::FirstOrderExtrap(visc_terms, geom, 0, num_comp);
    }

    if (verbose > 1)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::getViscTerms(): lev: " << level << ", time: " << run_time << '\n';
    }
}

void dumpProfileFab(const FArrayBox& fab,
                    const std::string file)
{
    const Box& box = fab.box();
    int imid = (int)(0.5*(box.smallEnd()[0] + box.bigEnd()[0]));
    IntVect iv1 = box.smallEnd(); iv1[0] = imid;
    IntVect iv2 = box.bigEnd(); iv2[0] = imid;
    iv1[1] = 115; iv2[1]=125;
    Box pb(iv1,iv2);
    std::cout << "dumping over " << pb << '\n';

    std::ofstream osf(file.c_str());
    for (IntVect iv = pb.smallEnd(); iv <= pb.bigEnd(); pb.next(iv))
    {
        osf << iv[1] << " " << (iv[1]+0.5)*3.5/256 << " ";
        for (int n=0; n<fab.nComp(); ++n) 
            osf << fab(iv,n) << " ";
        osf << '\n';
    }
    BoxLib::Abort();
}

void dumpProfile(const MultiFab& mf,
                 const std::string file)
{
    const FArrayBox& fab = mf[0];
    dumpProfileFab(fab,file);
}

Real MFnorm(const MultiFab& mf,
            int             sComp,
            int             nComp)
{
    const int p = 0; // max norm
    Real normtot = 0.0;
    for (MFIter mfi(mf); mfi.isValid(); ++mfi)
        normtot = std::max(normtot,mf[mfi].norm(mfi.validbox(),p,sComp,nComp));
    ParallelDescriptor::ReduceRealMax(normtot);
    return normtot;
}

void
HeatTransfer::compute_differential_diffusion_fluxes (const Real& time,
                                                     const Real& dt)
{
    BL_PROFILE("HT:::compute_differential_diffusion_fluxes()");
    // explicit computation of species and enthalpy (heat) diffusion fluxes
    // save fluxes in class data
    const Real      strt_time = ParallelDescriptor::second();
    const TimeLevel whichTime = which_time(State_Type,time);

    BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);    

    MultiFab* const * flux = (whichTime == AmrOldTime) ? SpecDiffusionFluxn : SpecDiffusionFluxnp1;

    if (hack_nospecdiff)
    {
      BoxLib::Error("compute_differential_diffusion_fluxes: hack_nospecdiff not implemented");
    }

    const int  rho_flag  = 2;
    MultiFab   rho_half;      // Never need alpha for RhoY
    MultiFab*  alpha     = 0;
    int        alphaComp = 0;
    const Real a         = 0;
    const Real b         = 1;
    MultiFab&  S         = get_data(State_Type,time);

    // allocate edge-beta for species, RhoH, and Temp
    FluxBoxes fb(this, nspecies+2, 0);
    MultiFab** beta = fb.get();

    // average transport coefficients for species, RhoH, and Temp to edges
    getDiffusivity(beta, time, first_spec, 0, nspecies+1);
    getDiffusivity(beta, time, Temp, nspecies+1, 1);

    showMF("dd",S,"dd_preFP",level);
    //
    // Fill grow cells in the state for (Rho,RhoY,RhoH,T).  For Dirichlet physical
    // boundary grow cells, this data will live on the cell face, otherwise
    // it will live at cell centers.
    //
    const int nGrow = 1;

    BL_ASSERT(S.nGrow()>=nGrow);

    for (FillPatchIterator Yfpi(*this,S,nGrow,time,State_Type,Density,nspecies+2),
	                   Tfpi(*this,S,nGrow,time,State_Type,Temp,1);
         Yfpi.isValid() && Tfpi.isValid();
         ++Yfpi, ++Tfpi)
    {
      const Box& vbox   = Yfpi.validbox();
      FArrayBox& fab    = S[Tfpi];
      BoxList    gcells = BoxLib::boxDiff(Box(vbox).grow(nGrow),vbox);
      for (BoxList::const_iterator it = gcells.begin(), end = gcells.end(); it != end; ++it)
      {
	const Box& gbox = *it;
	fab.copy(Yfpi(),gbox,0,gbox,Density,nspecies+2);
	fab.copy(Tfpi(),gbox,0,gbox,Temp,1);
      }
    }
    showMF("dd",S,"dd_rsT_fp",level);
    //
    // Create and fill (Rho,RhoY,RhoH,T) at coarser level
    //    
    MultiFab S_crse;
    MultiFab Phi_crse;
    if (level > 0) 
    {
        HeatTransfer* coarser = (HeatTransfer*) &(parent->getLevel(level-1));

        Phi_crse.define(coarser->grids,        1, 2, Fab_allocate);
        S_crse.define  (coarser->grids,S.nComp(), 0, Fab_allocate);

        for (FillPatchIterator S_fpi(*coarser,S_crse,0,time,State_Type,0,S.nComp());
             S_fpi.isValid(); ++S_fpi)
	{
            S_crse[S_fpi].copy(S_fpi(),0,0,S.nComp());
	}
    }

    MultiFab Phi(grids,1,nGrow);
    const int phiComp = 0;

    for (int sigma = 0; sigma < nspecies+1; ++sigma)
    {
        const int state_ind = first_spec + sigma;
        ViscBndry visc_bndry;

        for (MFIter mfi(S); mfi.isValid(); ++mfi)
        {
            const Box& box = Phi[mfi].box();
            Phi[mfi].copy(S[mfi],box,state_ind,box,0,1);
            Phi[mfi].divide(S[mfi],box,Density,0,1);
        }

        if (level > 0) 
        {
            MultiFab::Copy(Phi_crse,S_crse,state_ind,0,1,0);
            for (MFIter mfi(S_crse); mfi.isValid(); ++mfi)
            {
		Phi_crse[mfi].divide(S_crse[mfi],Phi_crse[mfi].box(),Density,0,1);
            }
	    Phi_crse.FillBoundary(getLevel(level-1).geom.periodicity());
        }

	diffusion->getBndryDataGivenS(visc_bndry,Phi,Phi_crse,state_ind,0,1);

        const bool     do_applyBC           = true;
        const bool     bndry_already_filled = true;            
        const int      betaComp             = sigma;
        Real*          rhsscale             = 0;
        ABecLaplacian* visc_op = diffusion->getViscOp(state_ind,a,b,time,visc_bndry,
                                                      rho_half,rho_flag,rhsscale,
                                                      beta,betaComp,alpha,alphaComp,bndry_already_filled);
        visc_op->maxOrder(diffusion->maxOrder());
        visc_op->compFlux(D_DECL(*flux[0],*flux[1],*flux[2]),Phi,do_applyBC,LinOp::Inhomogeneous_BC,phiComp,sigma);

        delete visc_op;
    }

    Phi.clear();

    //
    // Remove scaling left in fluxes from solve.
    //
    for (int d=0; d < BL_SPACEDIM; ++d)
        flux[d]->mult(b/geom.CellSize()[d],0,nspecies+1);

#ifdef USE_WBAR
    compute_Wbar_fluxes(time,0);

    // add grad Wbar fluxes (SpecDiffusionFluxWbar) to 
    // species diffusion fluxes (flux)
    for (int d=0; d<BL_SPACEDIM; ++d)
    {
      for (MFIter mfi(*SpecDiffusionFluxWbar[d]); mfi.isValid(); ++mfi)
      {
	const Box& ebox = (*SpecDiffusionFluxWbar[d])[mfi].box();
	(*flux[d])[mfi].plus((*SpecDiffusionFluxWbar[d])[mfi],ebox,0,0,nspecies);
      }
    }
#endif

    //
    // Modify update/fluxes to preserve flux sum = 0 (conservatively correct Gamma_m)
    adjust_spec_diffusion_fluxes(time);
    //
    // AJN FLUXREG
    // We have just explicitly computed "D" for Y_m and h given an input state.
    // Update VISCOUS flux registers as follows.
    // If this is the predictor and sdc_iterMAX>1:
    //   COPY (1/2)*Gamma_m^n and (1/2)*lambda^n/cp^n grad h^n to flux registers
    // If this is the predictor AND sdc_iterMAX=1:
    //   DO NOTHING - we never call compute_differential_diffusion_terms again to
    //   subtract (1/2)*Gamma_m^{(k)} and (1/2)*lambda/cp grad h^{(k)}, which in this
    //   case is equal to the t^n version
    // If updateFluxReg=T (we are in the final corrector):
    //   SUBTRACT (1/2)*Gamma_m^(k) and (1/2)*lambda^(k)/cp^(k) grad h^(k)
    //
    if ( do_reflux && ( (is_predictor && sdc_iterMAX>1) || updateFluxReg ) )
    {
        for (int d = 0; d < BL_SPACEDIM; d++)
        {
            if (level > 0)
            {
                if (is_predictor && sdc_iterMAX>1)
                {
                    getViscFluxReg().FineAdd(*SpecDiffusionFluxn[d],d,0,
                                             first_spec,nspecies+1,0.5*dt);
                }
                if (updateFluxReg)
                {
                    getViscFluxReg().FineAdd(*SpecDiffusionFluxnp1[d],d,0,
                                             first_spec,nspecies+1,-0.5*dt);
                }
            }

            if (level < parent->finestLevel())
            {
                if (is_predictor && sdc_iterMAX>1)
                {
                    getLevel(level+1).getViscFluxReg().CrseInit((*SpecDiffusionFluxn[d]),d,0,
                                                                first_spec,nspecies+1,-0.5*dt,
                                                                FluxRegister::COPY);
                }
                if (updateFluxReg)
                {
                    getLevel(level+1).getViscFluxReg().CrseInit((*SpecDiffusionFluxnp1[d]),d,0,
                                                                first_spec,nspecies+1,0.5*dt,
                                                                FluxRegister::ADD);
                }
            }
        }
    }
    // build heat flux and temperature source terms
    // compute lambda grad T (for temperature and divu)
    // compute sum_m (Gamma_m + lambda/cp grad Y) (for enthalpy)
    // compute sum_m Gamma_m dot grad h_m (for divu)
    //
    compute_enthalpy_fluxes(time,beta);

    //
    // AJN FLUXREG
    // We have just computed "DD" given an input state.
    // Update ADVECTIVE flux registers as follows.
    // If we are in the predictor and sdc_iterMAX>1:
    //   ADD (1/2)*h_m^n(Gamma_m^n-lambda^n/cp^n grad Y_m^n)
    // If we are in the predictor and sdc_iterMAX=1:
    //   ADD 1.0*h_m^n(Gamma_m^n-lambda^n/cp^n grad Y_m^n)
    // If updateFluxReg=T (we are in the final corrector):
    //   ADD (1/2)*h_m^(k)(Gamma_m^(k)-lambda^(k)/cp^(k) grad Y_m^(k))
    //
    if ( do_reflux && ( is_predictor || updateFluxReg ) )
    {
        for (int d = 0; d < BL_SPACEDIM; d++)
        {
            if (level > 0)
            {
                if (is_predictor)
                {
                    const Real fac = (sdc_iterMAX==1) ? dt : 0.5*dt;
                    advflux_reg->FineAdd(*SpecDiffusionFluxn[d],d,
                                         nspecies+1,RhoH,1,fac);
                }
                if (updateFluxReg)
                {
                    advflux_reg->FineAdd(*SpecDiffusionFluxnp1[d],d,
                                         nspecies+1,RhoH,1,0.5*dt);
                }
            }

            if (level < parent->finestLevel())
            {
                if (is_predictor)
                {
                    const Real fac = (sdc_iterMAX==1) ? dt : 0.5*dt;
                    getAdvFluxReg(level+1).CrseInit((*SpecDiffusionFluxn[d]),d,nspecies+1,RhoH,
                                                    1,-fac,FluxRegister::ADD);
                }
                if (updateFluxReg)
                {
                    getAdvFluxReg(level+1).CrseInit((*SpecDiffusionFluxnp1[d]),d,nspecies+1,RhoH,
                                                    1,-0.5*dt,FluxRegister::ADD);
                }
            }
        }
    }

    if (verbose > 1)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::compute_differential_diffusion_fluxes(): lev: " << level 
		      << ", time: " << run_time << '\n';
    }
}

void
HeatTransfer::scalar_advection_update (Real dt,
                                       int  first_scalar,
                                       int  last_scalar)
{
  //
  // Careful: If here, the sign of aofs is flipped (wrt the usual NS treatment).
  //
  MultiFab&       S_new = get_new_data(State_Type);
  const MultiFab& S_old = get_old_data(State_Type);
  
  for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
    {
      const Box& box   = mfi.validbox();
      const int nc = last_scalar - first_scalar + 1; 
      FArrayBox& snew = S_new[mfi];
      
      snew.copy( (*aofs)[mfi],box,first_scalar,box,first_scalar,nc);
      snew.mult(dt,box,first_scalar,nc);
      snew.plus(S_old[mfi],box,first_scalar,first_scalar,nc);            
    }
}

void
HeatTransfer::flux_divergence (MultiFab&        fdiv,
                               int              fdivComp,
                               const MultiFab* const* f,
                               int              fluxComp,
                               int              nComp,
                               Real             scale) const
{
    BL_ASSERT(fdiv.nComp() >= fdivComp+nComp);

    for (MFIter mfi(fdiv); mfi.isValid(); ++mfi)
    {
	const Box& box = mfi.validbox();
        FArrayBox& fab = fdiv[mfi];

	FORT_FLUXDIV(box.loVect(), box.hiVect(),
                     fab.dataPtr(fdivComp), ARLIM(fab.loVect()), ARLIM(fab.hiVect()),
                     (*f[0])[mfi].dataPtr(fluxComp), ARLIM((*f[0])[mfi].loVect()),   ARLIM((*f[0])[mfi].hiVect()),
                     (*f[1])[mfi].dataPtr(fluxComp), ARLIM((*f[1])[mfi].loVect()),   ARLIM((*f[1])[mfi].hiVect()),
#if BL_SPACEDIM == 3
                     (*f[2])[mfi].dataPtr(fluxComp), ARLIM((*f[2])[mfi].loVect()),   ARLIM((*f[2])[mfi].hiVect()),
#endif
                     volume[mfi].dataPtr(),          ARLIM(volume[mfi].loVect()),    ARLIM(volume[mfi].hiVect()),
                     &nComp, &scale);
    }	
}

void
HeatTransfer::compute_differential_diffusion_terms (MultiFab& D,
                                                    MultiFab& DD,
#ifdef USE_WBAR
						    MultiFab& DWbar,
#endif
                                                    Real      time,
                                                    Real      dt)
{
    BL_PROFILE("HT:::compute_differential_diffusion_terms()");
    // 
    // Sets vt for species, RhoH and Temp together
    // Uses state at time to explicitly compute fluxes, and resets internal
    //  data for fluxes, etc
    //
    BL_ASSERT(D.boxArray() == grids);
    BL_ASSERT(D.nComp() >= nspecies+2); // room for spec+RhoH+Temp

    if (hack_nospecdiff)
    {
      BoxLib::Error("compute_differential_diffusion_terms: hack_nospecdiff not implemented");
    }

    const TimeLevel whichTime = which_time(State_Type,time);
    BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);    
    MultiFab& sumSpecFluxDotGradH = (whichTime == AmrOldTime) ? sumSpecFluxDotGradHn : sumSpecFluxDotGradHnp1;
    MultiFab* const * flux = (whichTime == AmrOldTime) ? SpecDiffusionFluxn : SpecDiffusionFluxnp1;
#ifdef USE_WBAR
    MultiFab* const * fluxWbar = SpecDiffusionFluxWbar;
#endif
    //
    // Compute/adjust species fluxes/heat flux/conduction, save in class data
    compute_differential_diffusion_fluxes(time,dt);


    // Then compute:
    // -Div(Fi),
    // -Div((lambda/cp).Grad(h) + Fi.(Lei-1)) + heating,
    // -Div(lambda.Grad(T)) + heating + sum(Fi.Grad(Hi)) 
    //

    // compute div Gamma_m for species AND 
    // div lambda/cp grad h for enthalpy
    flux_divergence(D,0,flux,0,nspecies+1,-1);

    // compute div sum_m h_m (Gamma_m + lambda/cp grad Y_m), a.k.a. the "diffdiff" terms
    flux_divergence(DD,0,flux,nspecies+1,1,-1);

    // compute div lambda grad T for temperature
    flux_divergence(D,nspecies+1,flux,nspecies+2,1,-1);

#ifdef USE_WBAR
    // compute div beta_for_Wbar grad Wbar
    flux_divergence(DWbar,0,fluxWbar,0,nspecies,-1);
#endif

    // add sum_m Gamma_m dot grad h_m to D for temperature
    MultiFab::Add(D,sumSpecFluxDotGradH,0,nspecies+1,1,0);

    if (D.nGrow() > 0 && DD.nGrow() > 0)
    {
	const int nc = nspecies+2;

	D.FillBoundary(0,nc, geom.periodicity());
	DD.FillBoundary(0,1, geom.periodicity());

	BL_ASSERT(D.nGrow() == 1);
	BL_ASSERT(DD.nGrow() == 1);
	
	Extrapolater::FirstOrderExtrap(D, geom, 0, nc);
	Extrapolater::FirstOrderExtrap(DD, geom, 0, 1);
    }
}

void
HeatTransfer::set_rho_to_species_sum (MultiFab& S,
                                      int       strtcomp, 
                                      int       nghost_in,
                                      int       minzero)
{
    set_rho_to_species_sum(S, strtcomp, S, strtcomp, nghost_in, minzero);
}

//
// This function
//       sets the density component in S_out to the sum of 
//       the species components in S_in
// if minzero = 1, the species components are first "max-ed" w/ zero
//  thus changing S_in values    
//
// s_in_strt is the state component corresponding to the
// 0-th component of S_in. It is otherwise assumed that 
// that the components of S_in "align" with those in S_out.
//
void
HeatTransfer::set_rho_to_species_sum (MultiFab& S_in, 
                                      int       s_in_strt,
                                      MultiFab& S_out,
                                      int       s_out_strt,  
                                      int       nghost_in,
                                      int       minzero)

{
    const BoxArray& sgrids = S_in.boxArray();

    BL_ASSERT(sgrids == S_out.boxArray());

    const int s_first_spec = first_spec - s_in_strt;
    const int s_last_spec  = last_spec  - s_in_strt;
    const int s_num_spec   = last_spec - first_spec + 1;
    const int s_density    = Density-s_out_strt;
    const int nghost       = std::min(S_in.nGrow(), std::min(S_out.nGrow(), nghost_in));

    if (minzero)
    {
        for (MFIter mfi(S_in); mfi.isValid(); ++mfi)
        {
            const int i   = mfi.index();
            Box       box = BoxLib::grow(sgrids[i], nghost);
            FabMinMax(S_in[mfi], box, 0.0, Real_MAX, s_first_spec, s_num_spec);
        }
    }

    BL_ASSERT(s_density >= 0);

    S_out.setVal(0, s_density, 1, nghost);

    for (MFIter mfi(S_in); mfi.isValid(); ++mfi)
    {
        const int i   = mfi.index();
        Box       box = BoxLib::grow(sgrids[i],nghost);

        for (int spec = s_first_spec; spec <= s_last_spec; spec++)
        {
            S_out[mfi].plus(S_in[mfi],box,spec,s_density,1);
        }
    }

    make_rho_curr_time();
}

void
HeatTransfer::temperature_stats (MultiFab& S)
{
    if (verbose)
    {
        //
        // Calculate some minimums and maximums.
        //
        Real tdhmin[3] = { 1.0e30, 1.0e30, 1.0e30};
        Real tdhmax[3] = {-1.0e30,-1.0e30,-1.0e30};

        for (MFIter S_mfi(S); S_mfi.isValid(); ++S_mfi)
        {
            const Box& bx = S_mfi.validbox();

            tdhmin[0] = std::min(tdhmin[0],S[S_mfi].min(bx,Temp));
            tdhmin[1] = std::min(tdhmin[1],S[S_mfi].min(bx,Density));
            tdhmin[2] = std::min(tdhmin[2],S[S_mfi].min(bx,RhoH));

            tdhmax[0] = std::max(tdhmax[0],S[S_mfi].max(bx,Temp));
            tdhmax[1] = std::max(tdhmax[1],S[S_mfi].max(bx,Density));
            tdhmax[2] = std::max(tdhmax[2],S[S_mfi].max(bx,RhoH));
        }

        const int IOProc = ParallelDescriptor::IOProcessorNumber();

        ParallelDescriptor::ReduceRealMin(tdhmin,3,IOProc);
        ParallelDescriptor::ReduceRealMax(tdhmax,3,IOProc);

        FArrayBox   Y, tmp;
        bool        aNegY = false;
        Array<Real> minY(nspecies,1.e30);

        for (MFIter S_mfi(S); S_mfi.isValid(); ++S_mfi)
        {
            Y.resize(S_mfi.validbox(),1);

            tmp.resize(S_mfi.validbox(),1);
            tmp.copy(S[S_mfi],Density,0,1);
            tmp.invert(1);

            for (int i = 0; i < nspecies; ++i)
            {
                Y.copy(S[S_mfi],first_spec+i,0,1);
                Y.mult(tmp,0,0,1);
                minY[i] = std::min(minY[i],Y.min(0));
            }
        }

        ParallelDescriptor::ReduceRealMin(minY.dataPtr(),nspecies,IOProc);

        for (int i = 0; i < nspecies; ++i)
        {
            if (minY[i] < 0) aNegY = true;
        }

        if (ParallelDescriptor::IOProcessor())
        {
            std::cout << "  Min,max temp = " << tdhmin[0] << ", " << tdhmax[0] << '\n';
            std::cout << "  Min,max rho  = " << tdhmin[1] << ", " << tdhmax[1] << '\n';
            std::cout << "  Min,max rhoh = " << tdhmin[2] << ", " << tdhmax[2] << '\n';
            if (aNegY)
            {
                const Array<std::string>& names = HeatTransfer::getChemSolve().speciesNames();
                std::cout << "  Species w/min < 0: ";
                for (int i = 0; i < nspecies; ++i)
                    if (minY[i] < 0)
                        std::cout << "Y(" << names[i] << ") [" << minY[i] << "]  ";
                std::cout << '\n';
            }
        }
    }
}

void
HeatTransfer::compute_rhoRT (const MultiFab& S,
                             MultiFab&       p, 
                             int             pComp,
                             const MultiFab* temp)
{
    BL_ASSERT(pComp<p.nComp());

    const Real strt_time = ParallelDescriptor::second();

    const BoxArray& sgrids = S.boxArray();
    int nCompY = last_spec - first_spec + 1;

    FArrayBox state, tmp;

    for (MFIter mfi(S); mfi.isValid(); ++mfi)
    {
        const int  i      = mfi.index();
	const Box& box    = sgrids[i];
	const int  sCompR = 0;
	const int  sCompT = 1;
	const int  sCompY = 2;
	
        state.resize(box,nCompY+2);
	BL_ASSERT(S[mfi].box().contains(box));
	state.copy(S[mfi],box,Density,box,sCompR,1);

	if (temp)
	{
	    BL_ASSERT(temp->boxArray()[i].contains(box));
	    state.copy((*temp)[mfi],box,0,box,sCompT,1);
	}
        else
        {
	    state.copy(S[mfi],box,Temp,box,sCompT,1);
	}
	state.copy(S[mfi],box,first_spec,box,sCompY,nCompY);

        tmp.resize(box,1);
        tmp.copy(state,sCompR,0,1);
        tmp.invert(1);

        for (int k = 0; k < nCompY; k++)
            state.mult(tmp,0,sCompY+k,1);

	getChemSolve().getPGivenRTY(p[mfi],state,state,state,box,sCompR,sCompT,sCompY,pComp);
    }

    if (verbose > 1)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::compute_rhoRT(): lev: " << level << ", time: " << run_time << '\n';
    }
}
			   
//
// Setup for the advance function.
//

#ifndef NDEBUG
#if defined(BL_OSF1)
#if defined(BL_USE_DOUBLE)
const Real BL_BOGUS      = DBL_QNAN;
#else
const Real BL_BOGUS      = FLT_QNAN;
#endif
#else
const Real BL_BOGUS      = 1.e30;
#endif
#endif

void
HeatTransfer::set_htt_hmixTYP ()
{
    const int finest_level = parent->finestLevel();

    // set typical value for hmix, needed for TfromHY solves if not provided explicitly
    if (typical_values[RhoH]==typical_RhoH_value_default)
    {
        htt_hmixTYP = 0;
        std::vector< std::pair<int,Box> > isects;
        for (int k = 0; k <= finest_level; k++)
        {
            AmrLevel&       ht = getLevel(k);
            const MultiFab& S  = ht.get_new_data(State_Type);
            const BoxArray& ba = ht.boxArray();
            MultiFab hmix(ba,1,0);
            MultiFab::Copy(hmix,S,RhoH,0,1,0);
            MultiFab::Divide(hmix,S,Density,0,1,0);
            if (k != finest_level) 
            {
                AmrLevel& htf = getLevel(k+1);
                BoxArray  baf = htf.boxArray();
                baf.coarsen(parent->refRatio(k));
                for (MFIter mfi(hmix); mfi.isValid(); ++mfi)
                {
                    baf.intersections(ba[mfi.index()],isects);
                    for (int i = 0; i < isects.size(); i++)
                        hmix[mfi].setVal(0,isects[i].second,0,1);
                }
            }
            htt_hmixTYP = std::max(htt_hmixTYP,hmix.norm0(0));
        }
        ParallelDescriptor::ReduceRealMax(htt_hmixTYP);
        if (verbose && ParallelDescriptor::IOProcessor())
            std::cout << "setting htt_hmixTYP(via domain scan) = " << htt_hmixTYP << '\n';
    }
    else
    {
        htt_hmixTYP = typical_values[RhoH];
        if (verbose && ParallelDescriptor::IOProcessor())
            std::cout << "setting htt_hmixTYP(from user input) = " << htt_hmixTYP << '\n';
    }
}

void
HeatTransfer::advance_setup (Real time,
                             Real dt,
                             int  iteration,
                             int  ncycle)
{
    NavierStokesBase::advance_setup(time, dt, iteration, ncycle);

    for (int k = 0; k < num_state_type; k++)
    {
        MultiFab& nstate = get_new_data(k);
        MultiFab& ostate = get_old_data(k);

	MultiFab::Copy(nstate,ostate,0,0,nstate.nComp(),nstate.nGrow());
    }
    if (level == 0)
        set_htt_hmixTYP();

    make_rho_curr_time();

#ifdef USE_WBAR
    calcDiffusivity_Wbar(time);
#endif

    if (plot_reactions && level == 0)
    {
        for (int i = parent->finestLevel(); i >= 0; --i)
            getLevel(i).auxDiag["REACTIONS"]->setVal(0);
    }
}

void
HeatTransfer::setThermoPress(Real time)
{
    const TimeLevel whichTime = which_time(State_Type,time);
    
    BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);
    
    MultiFab& S = (whichTime == AmrOldTime) ? get_old_data(State_Type) : get_new_data(State_Type);
    
    const int pComp = (have_rhort ? RhoRT : Trac);

    compute_rhoRT (S,S,pComp);
}

Real
HeatTransfer::predict_velocity (Real  dt,
                                Real& comp_cfl)
{
    if (verbose && ParallelDescriptor::IOProcessor())
        std::cout << "... predict edge velocities\n";
    //
    // Get simulation parameters.
    //
    const int   nComp          = BL_SPACEDIM;
    const Real* dx             = geom.CellSize();
    const Real  prev_time      = state[State_Type].prevTime();
    const Real  prev_pres_time = state[Press_Type].prevTime();
    const Real strt_time       = ParallelDescriptor::second();
    //
    // Compute viscous terms at level n.
    // Ensure reasonable values in 1 grow cell.  Here, do extrap for
    // c-f/phys boundary, since we have no interpolator fn, also,
    // preserve extrap for corners at periodic/non-periodic intersections.
    //
    MultiFab visc_terms(grids,nComp,1);

    if (be_cn_theta != 1.0)
    {
	getViscTerms(visc_terms,Xvel,nComp,prev_time);
    }
    else
    {
	visc_terms.setVal(0);
    }
    //
    // Set up the timestep estimation.
    //
    Real cflgrid,u_max[3];
    Real cflmax = 1.0e-10;
    comp_cfl    = (level == 0) ? cflmax : comp_cfl;

    FArrayBox tforces;

    Array<int> bndry[BL_SPACEDIM];

    MultiFab Gp(grids,BL_SPACEDIM,1);

    getGradP(Gp, prev_pres_time);
    
    FArrayBox null_fab;

    showMF("mac",Gp,"pv_Gp",level);
    showMF("mac",rho_ptime,"pv_rho_old",level);
    showMF("mac",visc_terms,"pv_visc_terms",level);

#ifndef NDEBUG
    for (int dir=0; dir<BL_SPACEDIM; ++dir) {
        u_mac[dir].setVal(0);
    }
    MultiFab Force(grids,BL_SPACEDIM,Godunov::hypgrow());
    Force.setVal(0);
#endif

    for (FillPatchIterator U_fpi(*this,visc_terms,Godunov::hypgrow(),prev_time,State_Type,Xvel,BL_SPACEDIM)
#ifdef MOREGENGETFORCE
	     , S_fpi(*this,visc_terms,1,prev_time,State_Type,Density,NUM_SCALARS);
	 S_fpi.isValid() && U_fpi.isValid();
	 ++S_fpi, ++U_fpi
#else
             ; U_fpi.isValid();
	 ++U_fpi
#endif
	)
    {
        const int i = U_fpi.index();

#ifdef GENGETFORCE
        getForce(tforces,i,1,Xvel,BL_SPACEDIM,prev_time,rho_ptime[U_fpi]);
#elif MOREGENGETFORCE
	if (ParallelDescriptor::IOProcessor() && getForceVerbose)
	    std::cout << "---" << '\n' << "A - Predict velocity:" << '\n' << " Calling getForce..." << '\n';
        getForce(tforces,i,1,Xvel,BL_SPACEDIM,prev_time,U_fpi(),S_fpi(),0);
#else
	getForce(tforces,i,1,Xvel,BL_SPACEDIM,rho_ptime[U_fpi]);
#endif		 
        //
        // Test velocities, rho and cfl.
        //
        cflgrid  = godunov->test_u_rho(U_fpi(),rho_ptime[U_fpi],grids[i],dx,dt,u_max);
        cflmax   = std::max(cflgrid,cflmax);
        comp_cfl = std::max(cflgrid,comp_cfl);
        //
        // Compute the total forcing.
        //
        godunov->Sum_tf_gp_visc(tforces,0,visc_terms[U_fpi],0,Gp[U_fpi],0,rho_ptime[U_fpi],0);

#ifndef NDEBUG
        Force[U_fpi].copy(tforces,0,0,BL_SPACEDIM);
#endif

        D_TERM(bndry[0] = getBCArray(State_Type,i,0,1);,
               bndry[1] = getBCArray(State_Type,i,1,1);,
               bndry[2] = getBCArray(State_Type,i,2,1);)

        godunov->Setup(grids[i], dx, dt, 1,
                       D_DECL(null_fab,null_fab,null_fab),
                       D_DECL(bndry[0].dataPtr(),bndry[1].dataPtr(),bndry[2].dataPtr()),
                       D_DECL(U_fpi(),U_fpi(),U_fpi()), D_DECL(0,1,2),
                       tforces,0);

        godunov->ComputeUmac(grids[i], dx, dt, 
                             u_mac[0][U_fpi], bndry[0].dataPtr(),
                             u_mac[1][U_fpi], bndry[1].dataPtr(),
#if (BL_SPACEDIM == 3)
                             u_mac[2][U_fpi], bndry[2].dataPtr(),
#endif
                             U_fpi(), tforces);
    }

    showMF("mac",u_mac[0],"pv_umac0",level);
    showMF("mac",u_mac[1],"pv_umac1",level);
#if BL_SPACEDIM==3
    showMF("mac",u_mac[2],"pv_umac2",level);
#endif
#ifndef NDEBUG
    showMF("mac",Force,"pv_force",level);
#endif

    Real tempdt = std::min(change_max,cfl/cflmax);

    ParallelDescriptor::ReduceRealMin(tempdt);

    if (verbose > 1)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::predict_velocity(): lev: " << level << ", time: " << run_time << '\n';
    }

    return dt*tempdt;
}

void
HeatTransfer::set_reasonable_grow_cells_for_R (Real time)
{
    //
    // Ensure reasonable grow cells for R.
    //
    MultiFab& React = get_data(RhoYdot_Type, time);
    React.FillBoundary(0,nspecies, geom.periodicity());
    BL_ASSERT(React.nGrow() == 1);
    Extrapolater::FirstOrderExtrap(React, geom, 0, nspecies);
}

Real
HeatTransfer::advance (Real time,
		       Real dt,
		       int  iteration,
		       int  ncycle)
{

    BL_PROFILE_VAR("HT::advance::mac", HTMAC);
    if (closed_chamber == 1 && level == 0)
    {
      // set new-time ambient pressure to be a copy of old-time ambient pressure
      p_amb_new = p_amb_old;
    }

    BL_PROFILE_VAR_STOP(HTMAC);

    BL_PROFILE_REGION_START("R::HT::advance()[src_sdc]");
    BL_PROFILE("HT::advance()[src_sdc]");
    is_predictor = true;
    updateFluxReg = false;

    if (level == 0)
    {
      crse_dt = dt;
      int thisLevelStep = parent->levelSteps(0);
      FORT_SET_COMMON(&time,&thisLevelStep);
    }

    if (verbose && ParallelDescriptor::IOProcessor())
    {
      std::cout << "HeatTransfer::advance(): at start of time step\n"
		<< "SDC Advancing level " << level
		<< " : starting time = " << time
		<< " with dt = "         << dt << '\n';
    }

    // swaps old and new states for all state types
    // then copies each of the old state types into the new state types
    // computes old transport coefficients and copies them into new
    BL_PROFILE_VAR("HT::advance::setup", HTSETUP);
    advance_setup(time,dt,iteration,ncycle);
    BL_PROFILE_VAR_STOP(HTSETUP);

    if (level==0 && reset_typical_vals_int>0)
    {
      int L0_steps = parent->levelSteps(0);
      if (L0_steps>0 && L0_steps%reset_typical_vals_int==0)
      {
        reset_typical_values(get_old_data(State_Type));
      }
    }

    if (do_check_divudt)
    {
      checkTimeStep(dt);
    }
    
    MultiFab& S_new = get_new_data(State_Type);
    MultiFab& S_old = get_old_data(State_Type);

    const Real prev_time = state[State_Type].prevTime();
    const Real cur_time  = state[State_Type].curTime();


    Real dt_test = 0.0, dummy = 0.0;    

    BL_PROFILE_VAR("HT::advance::diffusion", HTDIFF);
    if (floor_species == 1)
    {
      for (MFIter mfi(S_old); mfi.isValid(); ++mfi)
      {
	const Box& box = mfi.validbox();            
	const FArrayBox& species = S_old[mfi];
	FORT_FLOORSPEC(box.loVect(), box.hiVect(), species.dataPtr(first_spec),
		       ARLIM(species.loVect()), ARLIM(species.hiVect()));
      }
    }
    BL_PROFILE_VAR_STOP(HTDIFF);

    // Build a copy of old-time rho with grow cells for use in the diffusion solves
    BL_PROFILE_VAR_START(HTDIFF);
    make_rho_prev_time();
    BL_PROFILE_VAR_STOP(HTDIFF);

    // compute old-time thermodynamic pressure
    BL_PROFILE_VAR_START(HTMAC);
    setThermoPress(prev_time);  
    BL_PROFILE_VAR_STOP(HTMAC);

    MultiFab Dn(grids,nspecies+2,nGrowAdvForcing);
    MultiFab DDn(grids,1,nGrowAdvForcing);
#ifdef USE_WBAR
    MultiFab DWbar(grids,nspecies,nGrowAdvForcing);
#endif

    // Compute Dn and DDn (based on state at tn)
    //  (Note that coeffs at tn and tnp1 were intialized in _setup)
    if (verbose && ParallelDescriptor::IOProcessor())
      std::cout << "Computing Dn, DDn, and DWbar \n";

    BL_PROFILE_VAR_START(HTDIFF);
#ifdef USE_WBAR
    compute_differential_diffusion_terms(Dn,DDn,DWbar,prev_time,dt);
#else
    compute_differential_diffusion_terms(Dn,DDn,prev_time,dt);
#endif
    BL_PROFILE_VAR_STOP(HTDIFF);

    showMF("sdc",Dn,"sdc_Dn",level,parent->levelSteps(level));
    showMF("sdc",DDn,"sdc_DDn",level,parent->levelSteps(level));
#ifdef USE_WBAR
    showMF("sdc",DWbar,"sdc_DWbar",level,parent->levelSteps(level));
#endif

    /*
      You could compute instantaneous I_R here but for now it's using either the
      previous step or divu_iter's version of I_R.  Either way, we have to make 
      sure that the nGrowAdvForcing grow cells have something reasonable in them
    */
    BL_PROFILE_VAR("HT::advance::reactions", HTREAC);
    set_reasonable_grow_cells_for_R(cur_time);
    BL_PROFILE_VAR_STOP(HTREAC);

    // copy old state into new state for Dn and DDn.
    // Note: this was already done for scalars, transport coefficients,
    // and divu in advance_setup
    MultiFab Dnp1(grids,nspecies+2,nGrowAdvForcing);
    MultiFab DDnp1(grids,1,nGrowAdvForcing);
    MultiFab chi(grids,1,nGrowAdvForcing);
    MultiFab chi_increment(grids,1,nGrowAdvForcing);
    MultiFab mac_divu(grids,1,nGrowAdvForcing);

    // used for closed chamber algorithm
    MultiFab theta_old(grids,1,nGrowAdvForcing);
    MultiFab theta_nph(grids,1,nGrowAdvForcing);
    Real Sbar, thetabar;
    Real Sbar_old, Sbar_new;

    BL_PROFILE_VAR_START(HTDIFF);
    MultiFab::Copy(Dnp1,Dn,0,0,nspecies+2,nGrowAdvForcing);
    MultiFab::Copy(DDnp1,DDn,0,0,1,nGrowAdvForcing);
    BL_PROFILE_VAR_STOP(HTDIFF);

    BL_PROFILE_VAR_START(HTMAC);
    chi.setVal(0,nGrowAdvForcing);
    BL_PROFILE_VAR_STOP(HTMAC);

    is_predictor = false;

    BL_PROFILE_VAR_NS("HT::advance::velocity_adv", HTVEL);
    for (int sdc_iter=1; sdc_iter<=sdc_iterMAX; ++sdc_iter)
    {

      if (sdc_iter == sdc_iterMAX)
      {
	updateFluxReg = true;
      }

      if (sdc_iter > 1)
      {
	// compute new-time transport coefficients
        BL_PROFILE_VAR_START(HTDIFF);
	calcDiffusivity(cur_time);
#ifdef USE_WBAR
	calcDiffusivity_Wbar(cur_time);
#endif
        BL_PROFILE_VAR_STOP(HTDIFF);

	// compute Dnp1 and DDnp1
	// iteratively lagged
	if (verbose && ParallelDescriptor::IOProcessor()) {
            std::cout << "Computing Dnp1 and DDnp1 (SDC iteration " << sdc_iter << ")\n";
        }
        BL_PROFILE_VAR_START(HTDIFF);
#ifdef USE_WBAR
	compute_differential_diffusion_terms(Dnp1,DDnp1,DWbar,cur_time,dt);
#else
	compute_differential_diffusion_terms(Dnp1,DDnp1,cur_time,dt);
#endif
        BL_PROFILE_VAR_STOP(HTDIFF);

	// compute new-time DivU with instantaneous reaction rates
        BL_PROFILE_VAR_START(HTMAC);
	calc_divu(cur_time, dt, get_new_data(Divu_Type));
        BL_PROFILE_VAR_STOP(HTMAC);
      }

      // compute U^{ADV,*}
      // you could avoid having to redo this computation by saving this state
      BL_PROFILE_VAR_START(HTVEL);
      dt_test = predict_velocity(dt,dummy);  
      BL_PROFILE_VAR_STOP(HTVEL);

      // create S^{n+1/2} by averaging old and new
      BL_PROFILE_VAR_START(HTMAC);
      MultiFab Forcing(grids,nspecies+1,nGrowAdvForcing);
      Forcing.setBndry(1.e30);
      create_mac_rhs(mac_divu,nGrowAdvForcing,time+0.5*dt,dt);
      BL_PROFILE_VAR_STOP(HTMAC);
      showMF("sdc",Forcing,"sdc_mac_rhs1",level,sdc_iter,parent->levelSteps(level));
      
      // compute new-time thermodynamic pressure
      BL_PROFILE_VAR_START(HTMAC);
      setThermoPress(cur_time);

      // compute chi_increment
      chi_increment.setVal(0.0,nGrowAdvForcing);
      calc_dpdt(cur_time,dt,chi_increment,u_mac);
      BL_PROFILE_VAR_STOP(HTMAC);

      showMF("sdc",chi_increment,"sdc_chi_increment",level,sdc_iter,parent->levelSteps(level));

      // add chi_increment to chi
      BL_PROFILE_VAR_START(HTMAC);
      MultiFab::Add(chi,chi_increment,0,0,1,nGrowAdvForcing);
      BL_PROFILE_VAR_STOP(HTMAC);
      showMF("sdc",chi,"sdc_chi",level,sdc_iter,parent->levelSteps(level));

      // add chi to time-centered mac_divu
      BL_PROFILE_VAR_START(HTMAC);
      MultiFab::Add(mac_divu,chi,0,0,1,nGrowAdvForcing);
      BL_PROFILE_VAR_STOP(HTMAC);

      if (closed_chamber == 1 && level == 0)
      {	

        BL_PROFILE_VAR_START(HTMAC);

	// compute old, new, and time-centered theta = 1 / (gamma P)
	for (MFIter mfi(S_old); mfi.isValid(); ++mfi)
	{
	  const Box& box = mfi.validbox();            
	  FArrayBox& thetafab = theta_old[mfi];
	  const FArrayBox& rhoY = S_old[mfi];
	  const FArrayBox& T = S_old[mfi];
	  FORT_CALCGAMMAPINV(box.loVect(), box.hiVect(),
			     thetafab.dataPtr(),       ARLIM(thetafab.loVect()), ARLIM(thetafab.hiVect()),
			     rhoY.dataPtr(first_spec), ARLIM(rhoY.loVect()),     ARLIM(rhoY.hiVect()),
			     T.dataPtr(Temp),          ARLIM(T.loVect()),        ARLIM(T.hiVect()),
			     &p_amb_old);
	}

	for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
	{
	  const Box& box = mfi.validbox();            
	  FArrayBox& thetafab = theta_nph[mfi];
	  const FArrayBox& rhoY = S_new[mfi];
	  const FArrayBox& T = S_new[mfi];
	  FORT_CALCGAMMAPINV(box.loVect(), box.hiVect(),
			     thetafab.dataPtr(),       ARLIM(thetafab.loVect()), ARLIM(thetafab.hiVect()),
			     rhoY.dataPtr(first_spec), ARLIM(rhoY.loVect()),     ARLIM(rhoY.hiVect()),
			     T.dataPtr(Temp),          ARLIM(T.loVect()),        ARLIM(T.hiVect()),
			     &p_amb_new);
	}

	for (MFIter mfi(theta_nph); mfi.isValid(); ++mfi)
	{
	  FArrayBox& th_nph = theta_nph[mfi];
	  const FArrayBox& th_old = theta_old[mfi];
	  const Box& box = mfi.validbox();
	  th_nph.plus(th_old,box,box,0,0,1);
	  th_nph.mult(0.5);
	}

	// compute number of cells
	Real num_cells = grids.numPts();

	// compute the average of mac_divu theta
	Sbar = mac_divu.sum() / num_cells;
	thetabar = theta_nph.sum() / num_cells;

	// subtract mean from mac_divu and theta_nph
	mac_divu.plus(-Sbar,0,1);
	theta_nph.plus(-thetabar,0,1);

	p_amb_new = p_amb_old + dt*(Sbar/thetabar);
	dp0dt = Sbar/thetabar;

	// update mac rhs by adding delta_theta * (Sbar / thetabar)
	for (MFIter mfi(mac_divu); mfi.isValid(); ++mfi)
	{
	  FArrayBox& m_du = mac_divu[mfi];
	  FArrayBox& th_nph = theta_nph[mfi];
	  const Box& box = mfi.validbox();
	  th_nph.mult(Sbar/thetabar);
	  m_du.minus(th_nph,box,box,0,0,1);
	}
        BL_PROFILE_VAR_STOP(HTMAC);

	if (ParallelDescriptor::IOProcessor())
	{
	  std::cout << "level 0: prev_time, cur_time, p_amb_old, p_amb_new = " 
		    << prev_time << " " << cur_time << " " 
		    << p_amb_old << " " << p_amb_new << std::endl;
	}

      }

      // MAC-project... and overwrite U^{ADV,*}
      showMF("sdc",Forcing,"sdc_Forcing_for_mac",level,sdc_iter,parent->levelSteps(level));
      BL_PROFILE_VAR_START(HTMAC);
      mac_project(time,dt,S_old,&mac_divu,1,nGrowAdvForcing,updateFluxReg);

      if (closed_chamber == 1 && level == 0)
      {
	// add Sbar back to mac_divu
	mac_divu.plus(Sbar,0,1);
      }
      BL_PROFILE_VAR_STOP(HTMAC);

      //
      // Compute A (advection terms) with F = Dn + R
      //
      showMF("sdc",get_new_data(RhoYdot_Type),"sdc_R_for_A_forcing",level,sdc_iter,parent->levelSteps(level));
      BL_PROFILE_VAR("HT::advance::advection", HTADV);
      for (MFIter mfi(Forcing); mfi.isValid(); ++mfi)
      {
	FArrayBox& f = Forcing[mfi];
	const FArrayBox& dn = Dn[mfi];
	const FArrayBox& ddn = DDn[mfi];
	const FArrayBox& r = get_new_data(RhoYdot_Type)[mfi];
	const Box& gbox = BoxLib::grow(mfi.validbox(),nGrowAdvForcing);
	f.copy(dn,gbox,0,gbox,0,nspecies+1); // add Dn to RhoY and RhoH
	f.plus(ddn,gbox,gbox,0,nspecies,1); // add DDn to RhoH forcing
	if (closed_chamber == 1)
	{
	  f.plus(dp0dt,nspecies,1); // add dp0/dt to enthalpy forcing
	}
	f.plus(r,gbox,gbox,0,0,nspecies); // add R to RhoY, no contribution for RhoH
      }
      BL_PROFILE_VAR_STOP(HTADV);

      showMF("sdc",Forcing,"sdc_Forcing_for_A",level,sdc_iter,parent->levelSteps(level));
      BL_PROFILE_VAR_START(HTADV);
      Forcing.FillBoundary(0,nspecies+1,geom.periodicity());
      BL_PROFILE_VAR_STOP(HTADV);

      if (verbose && ParallelDescriptor::IOProcessor())
	std::cout << "A (SDC iter " << sdc_iter << ")\n";

      // compute A
      BL_PROFILE_VAR_START(HTADV);
      aofs->setVal(1.e30,aofs->nGrow());
      compute_scalar_advection_fluxes_and_divergence(Forcing,mac_divu,dt);
      BL_PROFILE_VAR_STOP(HTADV);
      showMF("sdc",*aofs,"sdc_A_pred",level,sdc_iter,parent->levelSteps(level));

      // update rho, rho*Y, and rho*h
      BL_PROFILE_VAR_START(HTADV);
      scalar_advection_update(dt, Density, RhoH);

      // update pointer to new-time density
      make_rho_curr_time();
      BL_PROFILE_VAR_STOP(HTADV);

      // 
      // Compute Dhat, diffuse with F 
      //                 = A + R + 0.5(Dn + Dnp1) - Dnp1 + Dhat + 0.5(DDn + DDnp1)
      //                 = A + R + 0.5(Dn - Dnp1) + Dhat + 0.5(DDn + DDnp1)
      // NOTE: Here we use 0.5*DDnp1 from the previous iteration
      // 
      if (verbose && ParallelDescriptor::IOProcessor()) {
	std::cout << "Dhat (SDC corrector " << sdc_iter << ")\n";
      }

      showMF("sdc",*aofs,"sdc_A_before_Dhat",level,sdc_iter,parent->levelSteps(level));
      showMF("sdc",get_new_data(RhoYdot_Type),"sdc_R_before_Dhat",level,sdc_iter,parent->levelSteps(level));
      showMF("sdc",Dn,"sdc_Dn_before_Dhat",level,sdc_iter,parent->levelSteps(level));
      showMF("sdc",DDn,"sdc_DDn_before_Dhat",level,sdc_iter,parent->levelSteps(level));
      showMF("sdc",Dnp1,"sdc_Dnp1_before_Dhat",level,sdc_iter,parent->levelSteps(level));
      showMF("sdc",DDnp1,"sdc_DDnp1_before_Dhat",level,sdc_iter,parent->levelSteps(level));

      BL_PROFILE_VAR_START(HTDIFF);
      for (MFIter mfi(Forcing); mfi.isValid(); ++mfi) 
      {
	const Box& box = mfi.validbox();
	FArrayBox& f = Forcing[mfi];
	const FArrayBox& a = (*aofs)[mfi];
	const FArrayBox& r = get_new_data(RhoYdot_Type)[mfi];
	const FArrayBox& dn = Dn[mfi];
	const FArrayBox& ddn = DDn[mfi];
	const FArrayBox& dnp1 = Dnp1[mfi];
	const FArrayBox& ddnp1 = DDnp1[mfi];

	f.copy(dn,box,0,box,0,nspecies+1); // copy Dn into RhoY and RhoH
	f.minus(dnp1,box,box,0,0,nspecies+1); // subtract Dnp1 from RhoY and RhoH
	f.plus(ddn  ,box,box,0,nspecies,1); // add DDn to RhoH, no contribution for RhoY
	f.plus(ddnp1,box,box,0,nspecies,1); // add DDnp1 to RhoH, no contribution for RhoY
	f.mult(0.5);
#ifdef USE_WBAR
	const FArrayBox& dwbar = DWbar[mfi];
	f.plus(dwbar,box,box,0,0,nspecies); // add DWbar to RhoY
#endif
	if (closed_chamber == 1)
	{
	  f.plus(dp0dt,nspecies,1); // add dp0/dt to enthalpy forcing
	}
	f.plus(a,box,box,first_spec,0,nspecies+1); // add A into RhoY and RhoH
	f.plus(r,box,box,0,0,nspecies); // no reactions for RhoH
      }
      BL_PROFILE_VAR_STOP(HTDIFF);

      MultiFab Dhat(grids,nspecies+2,nGrowAdvForcing);

      // advection-diffusion solve
      showMF("sdc",Forcing,"sdc_Forcing_before_Dhat",level,sdc_iter,parent->levelSteps(level));
      BL_PROFILE_VAR_START(HTDIFF);
      differential_diffusion_update(Forcing,0,Dhat,0,DDnp1);
      BL_PROFILE_VAR_STOP(HTDIFF);

      showMF("sdc",Dn,"sdc_Dn_before_R",level,sdc_iter,parent->levelSteps(level));
      showMF("sdc",Dnp1,"sdc_Dnp1_before_R",level,sdc_iter,parent->levelSteps(level));
      showMF("sdc",DDn,"sdc_DDn_before_R",level,sdc_iter,parent->levelSteps(level));
      showMF("sdc",DDnp1,"sdc_DDnp1_before_R",level,sdc_iter,parent->levelSteps(level));
      showMF("sdc",Dhat,"sdc_Dhat_before_R",level,sdc_iter,parent->levelSteps(level));
      showMF("sdc",*aofs,"sdc_A_before_R",level,sdc_iter,parent->levelSteps(level));

      // 
      // Compute R (F = A + 0.5(Dn - Dnp1 + DDn + DDnp1) + Dhat )
      // 
      BL_PROFILE_VAR_START(HTREAC);
      for (MFIter mfi(Forcing); mfi.isValid(); ++mfi) 
        {
	  const Box& box = mfi.validbox();
	  FArrayBox& f = Forcing[mfi];
	  const FArrayBox& a = (*aofs)[mfi];
	  const FArrayBox& dn = Dn[mfi];
	  const FArrayBox& dnp1 = Dnp1[mfi];
	  const FArrayBox& dhat = Dhat[mfi];
	  const FArrayBox& ddn = DDn[mfi];
	  const FArrayBox& ddnp1 = DDnp1[mfi];
	  f.copy(dn,box,0,box,0,nspecies+1);    // copy Dn into RhoY and RhoH
	  f.minus(dnp1,box,box,0,0,nspecies+1); // subtract Dnp1 from RhoY and RhoH
	  f.plus(ddn  ,box,box,0,nspecies,1);   // add DDn to RhoH, no contribution for RhoY
	  f.plus(ddnp1,box,box,0,nspecies,1);   // add DDnp1 to RhoH, no contribution for RhoY
	  f.mult(0.5);
	  if (closed_chamber == 1)
	  {
	    f.plus(dp0dt,nspecies,1); // add dp0/dt to enthalpy forcing
	  }
	  f.plus(dhat,box,box,0,0,nspecies+1);       // add Dhat to RhoY and RHoH
	  f.plus(a,box,box,first_spec,0,nspecies+1); // add A to RhoY and RhoH
        }
      BL_PROFILE_VAR_STOP(HTREAC);
      
      Dhat.clear();

      if (verbose && ParallelDescriptor::IOProcessor()) {
	std::cout << "R (SDC corrector " << sdc_iter << ")\n";
      }

      showMF("sdc",S_old,"sdc_Sold_before_R",level,sdc_iter,parent->levelSteps(level));
      showMF("sdc",Forcing,"sdc_Forcing_before_R",level,sdc_iter,parent->levelSteps(level));
      BL_PROFILE_VAR_START(HTREAC);
      advance_chemistry(S_old,S_new,dt,Forcing,0);
      BL_PROFILE_VAR_STOP(HTREAC);
      showMF("sdc",S_new,"sdc_Snew_after_R",level,sdc_iter,parent->levelSteps(level));

      BL_PROFILE_VAR_START(HTDIFF);
      if (floor_species == 1)
      {
	for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
	{
	  const Box& box = mfi.validbox();            
	  const FArrayBox& species = S_new[mfi];
	  FORT_FLOORSPEC(box.loVect(), box.hiVect(),
			 species.dataPtr(first_spec), ARLIM(species.loVect()),  ARLIM(species.hiVect()));
	}
      }
      BL_PROFILE_VAR_STOP(HTDIFF);

      if (verbose && ParallelDescriptor::IOProcessor())
	std::cout << "DONE WITH R (SDC corrector " << sdc_iter << ")\n";

      BL_PROFILE_VAR_START(HTMAC);
      setThermoPress(cur_time);
      BL_PROFILE_VAR_STOP(HTMAC);
    }

    Dn.clear();
    DDn.clear();
    Dnp1.clear();
    DDnp1.clear();
    chi_increment.clear();

    if (verbose && ParallelDescriptor::IOProcessor())
      std::cout << " SDC iterations complete \n";

    if (plot_consumption)
    {
      for (int j=0; j<consumptionName.size(); ++j)
      {
        int consumptionComp = getChemSolve().index(consumptionName[j]);
        MultiFab::Copy((*auxDiag["CONSUMPTION"]),get_new_data(RhoYdot_Type),consumptionComp,j,1,0);
        auxDiag["CONSUMPTION"]->mult(-1,j,1); // Convert production to consumption
      }
    }

    if (plot_heat_release)
    {
      FArrayBox enthi, T;
      const MultiFab& R = get_new_data(RhoYdot_Type);
      for (MFIter mfi(R); mfi.isValid(); ++mfi)
      {
        const Box& box = mfi.validbox();
        T.resize(mfi.validbox(),1);
        T.setVal(298.15);
        
        enthi.resize(mfi.validbox(),R.nComp());
        getChemSolve().getHGivenT(enthi,T,box,0,0);
        enthi.mult(R[mfi],0,0,R.nComp());
        
        // Form heat release
        (*auxDiag["HEATRELEASE"])[mfi].setVal(0);
        for (int j=0; j<R.nComp(); ++j)
	{
          (*auxDiag["HEATRELEASE"])[mfi].minus(enthi,j,0,1);
        }
      }
    }

#ifdef BL_COMM_PROFILING
    for (MFIter mfi(*auxDiag["COMMPROF"]); mfi.isValid(); ++mfi)
    {
      int rank(ParallelDescriptor::MyProc());
      (*auxDiag["COMMPROF"])[mfi].setVal(rank, 0);
      (*auxDiag["COMMPROF"])[mfi].setVal(DistributionMapping::ProximityMap(rank),   1);
      (*auxDiag["COMMPROF"])[mfi].setVal(DistributionMapping::ProximityOrder(rank), 2);
    }
#endif

    BL_PROFILE_VAR_START(HTDIFF);
    calcDiffusivity(cur_time);
#ifdef USE_WBAR
    calcDiffusivity_Wbar(cur_time);
#endif

    calcViscosity(cur_time,dt,iteration,ncycle);
    BL_PROFILE_VAR_STOP(HTDIFF);
    //
    // Set the dependent value of RhoRT to be the thermodynamic pressure.  By keeping this in
    // the state, we can use the average down stuff to be sure that RhoRT_avg is avg(RhoRT),
    // not ave(Rho)avg(R)avg(T), which seems to give the p-relax stuff in the mac Rhs troubles.
    //
    BL_PROFILE_VAR_START(HTMAC);
    setThermoPress(cur_time);
    BL_PROFILE_VAR_STOP(HTMAC);

    BL_PROFILE_VAR("HT::advance::project", HTPROJ);
    calc_divu(time+dt, dt, get_new_data(Divu_Type));
    BL_PROFILE_VAR_STOP(HTPROJ);
    showMF("sdc",get_new_data(Divu_Type),"sdc_Divu",level,parent->levelSteps(level));

    BL_PROFILE_VAR_START(HTPROJ);
    if (!NavierStokesBase::initial_step && level != parent->finestLevel())
    {
        //
        // Set new divu to old div + dt*dsdt_old where covered by fine.
        //
        BoxArray crsndgrids = getLevel(level+1).grids;

        crsndgrids.coarsen(fine_ratio);
            
        MultiFab& divu_new = get_new_data(Divu_Type);
        MultiFab& divu_old = get_old_data(Divu_Type);
        MultiFab& dsdt_old = get_old_data(Dsdt_Type);

        std::vector< std::pair<int,Box> > isects;
            
        for (MFIter mfi(divu_new); mfi.isValid();++mfi)
        {
            isects = crsndgrids.intersections(mfi.validbox());

            for (int i = 0, N = isects.size(); i < N; i++)
            {
                const Box& ovlp = isects[i].second;
		divu_new[mfi].copy(dsdt_old[mfi],ovlp,0,ovlp,0,1);
                divu_new[mfi].mult(dt,ovlp,0,1);
                divu_new[mfi].plus(divu_old[mfi],ovlp,0,0,1);
            }
        }
    }
    BL_PROFILE_VAR_STOP(HTPROJ);
    showMF("sdc",get_new_data(Divu_Type),"sdc_DivUnew",level,parent->levelSteps(level));
        
    BL_PROFILE_VAR_START(HTPROJ);
    calc_dsdt(time, dt, get_new_data(Dsdt_Type));
    BL_PROFILE_VAR_STOP(HTPROJ);
    showMF("sdc",get_new_data(Dsdt_Type),"sdc_Dsdtnew",level,parent->levelSteps(level));

    BL_PROFILE_VAR_START(HTPROJ);
    if (NavierStokesBase::initial_step)
        MultiFab::Copy(get_old_data(Dsdt_Type),get_new_data(Dsdt_Type),0,0,1,0);
    BL_PROFILE_VAR_STOP(HTPROJ);
    //
    // Add the advective and other terms to get velocity (or momentum) at t^{n+1}.
    //
    BL_PROFILE_VAR_START(HTVEL);
    if (do_mom_diff == 0) {
      velocity_advection(dt);
    }
    velocity_update(dt);
    BL_PROFILE_VAR_STOP(HTVEL);

    showMF("sdc",get_new_data(State_Type),"sdc_Snew_preProj",level,parent->levelSteps(level));

    // compute chi correction
    // place to take dpdt stuff out of nodal project
    //    calc_dpdt(cur_time,dt,chi_increment,u_mac);
    //    MultiFab::Add(get_new_data(Divu_Type),chi_increment,0,0,1,0);

    // subtract mean from divu
    BL_PROFILE_VAR_START(HTPROJ);
    if (closed_chamber == 1 && level == 0)
    {
      MultiFab& divu_old = get_old_data(Divu_Type);
      MultiFab& divu_new = get_new_data(Divu_Type);

      // compute number of cells
      Real num_cells = grids.numPts();

      // compute average of S at old and new times
      Sbar_old = divu_old.sum() / num_cells;
      Sbar_new = divu_new.sum() / num_cells;

      // subtract mean from divu
      divu_old.plus(-Sbar_old,0,1);
      divu_new.plus(-Sbar_new,0,1);
    }

    //
    // Increment rho average.
    //
    if (!initial_step)
    {
      if (level > 0)
      {
	Real alpha = 1.0/Real(ncycle);
	if (iteration == ncycle)
	  alpha = 0.5/Real(ncycle);
	incrRhoAvg(alpha);
      }

      //
      // Do a level project to update the pressure and velocity fields.
      //

      level_projector(dt,time,iteration);

      // restore Sbar
      if (closed_chamber == 1 && level == 0)
      {
	MultiFab& divu_old = get_old_data(Divu_Type);
	MultiFab& divu_new = get_new_data(Divu_Type);

	divu_old.plus(Sbar_old,0,1);
	divu_new.plus(Sbar_new,0,1);
      }

      if (level > 0 && iteration == 1) p_avg.setVal(0);
    }
    BL_PROFILE_VAR_STOP(HTPROJ);

    showMF("sdc",get_new_data(State_Type),"sdc_Snew_postProj",level,parent->levelSteps(level));

#ifdef PARTICLES
    if (theNSPC() != 0)
    {
        theNSPC()->AdvectWithUmac(u_mac, level, dt);
    }
#endif

    BL_PROFILE_VAR("HT::advance::cleanup", HTCLEANUP);
    advance_cleanup(iteration,ncycle);
    BL_PROFILE_VAR_STOP(HTCLEANUP);
    //
    // Update estimate for allowable time step.
    //
    if (fixed_dt > 0)
    {
      dt_test = estTimeStep();
    }
    else
    {
      dt_test = std::min(dt_test, estTimeStep());
    }

    if (verbose && ParallelDescriptor::IOProcessor())
        std::cout << "HeatTransfer::advance(): at end of time step\n";

    temperature_stats(S_new);

    BL_PROFILE_VAR_START(HTMAC);

    // during initialization, reset time 0 ambient pressure
    if (closed_chamber == 1 && level == 0 && !initial_step)
    {
      p_amb_old = p_amb_new;
    }
    BL_PROFILE_VAR_STOP(HTMAC);

    BL_PROFILE_REGION_STOP("R::HT::advance()[src_sdc]");

    return dt_test;
}

void
HeatTransfer::create_mac_rhs (MultiFab& mac_rhs, int nGrow, Real time, Real dt)
{
    BL_ASSERT(mac_rhs.nGrow()>=nGrow);
    BL_ASSERT(mac_rhs.boxArray()==grids);

    int sCompDivU = 0;
    int nCompDivU = 1;
    for (FillPatchIterator Divu_fpi(*this,mac_rhs,nGrow,time,Divu_Type,sCompDivU,nCompDivU);
         Divu_fpi.isValid();
         ++Divu_fpi) {
      mac_rhs[Divu_fpi].copy(Divu_fpi(),0,sCompDivU,nCompDivU);
    }
}

DistributionMapping
HeatTransfer::getFuncCountDM (const BoxArray& bxba, int ngrow)
{
    //
    // Sometimes "mf" is the valid region of the State.
    // Sometimes it's the region covered by AuxBoundaryData.
    // When ngrow>0 were doing AuxBoundaryData with nGrow()==ngrow.
    //
    DistributionMapping rr;
    rr.RoundRobinProcessorMap(bxba.size(),ParallelDescriptor::NProcs());

    MultiFab fctmpnew(bxba, 1, 0, rr);
    fctmpnew.setVal(1);

    const MultiFab& FC = get_new_data(FuncCount_Type);
    fctmpnew.copy(FC,0,0,1,std::min(ngrow,FC.nGrow()),0);

    int count = 0;
    Array<long> vwrk(bxba.size());
    for (MFIter mfi(fctmpnew); mfi.isValid(); ++mfi)
        vwrk[count++] = static_cast<long>(fctmpnew[mfi].sum(0));

    fctmpnew.clear();

#if BL_USE_MPI
    const int IOProc = ParallelDescriptor::IOProcessorNumber();

    Array<int> nmtags(ParallelDescriptor::NProcs(),0);
    Array<int> offset(ParallelDescriptor::NProcs(),0);

    const Array<int>& procmap = rr.ProcessorMap();

    for (int i = 0; i < vwrk.size(); i++)
        nmtags[procmap[i]]++;

    BL_ASSERT(nmtags[ParallelDescriptor::MyProc()] == count);

    for (int i = 1; i < offset.size(); i++)
        offset[i] = offset[i-1] + nmtags[i-1];

    Array<long> vwrktmp;

    if (ParallelDescriptor::IOProcessor()) vwrktmp = vwrk;

    MPI_Gatherv(vwrk.dataPtr(),
                count,
                ParallelDescriptor::Mpi_typemap<long>::type(),
                ParallelDescriptor::IOProcessor() ? vwrktmp.dataPtr() : 0,
                nmtags.dataPtr(),
                offset.dataPtr(),
                ParallelDescriptor::Mpi_typemap<long>::type(),
                IOProc,
                ParallelDescriptor::Communicator());

    if (ParallelDescriptor::IOProcessor())
    {
        //
        // We must now assemble vwrk in the proper order.
        //
        std::vector< std::vector<int> > table(ParallelDescriptor::NProcs());

        for (int i = 0; i < vwrk.size(); i++)
            table[procmap[i]].push_back(i);

        int idx = 0;
        for (int i = 0; i < table.size(); i++)
        {
            std::vector<int>& tbl = table[i];
            for (int j = 0; j < tbl.size(); j++)
                vwrk[tbl[j]] = vwrktmp[idx++];
        }
    }

    vwrktmp.clear();
    //
    // Send the properly-ordered vwrk to all processors.
    //
    ParallelDescriptor::Bcast(vwrk.dataPtr(), vwrk.size(), IOProc);
#endif

    DistributionMapping res;

    res.KnapSackProcessorMap(vwrk,ParallelDescriptor::NProcs());

    return res;
}

void
HeatTransfer::advance_chemistry (MultiFab&       mf_old,
                                 MultiFab&       mf_new,
                                 Real            dt,
                                 const MultiFab& Force,
                                 int             nCompF,
                                 bool            use_stiff_solver)
{
    BL_PROFILE("HT:::advance_chemistry()");

    const Real strt_time = ParallelDescriptor::second();

    const bool do_avg_down_chem = avg_down_chem
        && level < parent->finestLevel()
        && getLevel(level+1).state[RhoYdot_Type].hasOldData();

    if (hack_nochem)
    {
        FArrayBox tmp;

        for (MFIter mfi(mf_old); mfi.isValid(); ++mfi)
        {
            const Box& box = mfi.validbox();
            tmp.resize(box,nspecies+1);
            const FArrayBox& f = Force[mfi];
            tmp.copy(f,box,0,box,0,nspecies+1);
            tmp.mult(dt);
            FArrayBox& Sold = mf_old[mfi];
            FArrayBox& Snew = mf_new[mfi];
            Snew.copy(Sold,box,first_spec,box,first_spec,nspecies+1);
            Snew.plus(tmp,box,box,0,first_spec,nspecies+1);
            Snew.copy(Sold,box,Temp,box,Temp,1);
        }
    }
    else
    {
        BoxArray cf_grids;

        if (do_avg_down_chem)
        {
            cf_grids = getLevel(level+1).boxArray(); cf_grids.coarsen(fine_ratio);
        }

        MultiFab&  React_new = get_new_data(RhoYdot_Type);
        const int  ngrow     = React_new.nGrow();
        //
        // Chop the grids to level out the chemistry work.
        // We want enough grids so that KNAPSACK works well,
        // but not too many to make unweildy BoxArrays.
        //
        const int Threshold = chem_box_chop_threshold * ParallelDescriptor::NProcs();
        BoxArray  ba        = mf_new.boxArray();
        bool      done      = (ba.size() >= Threshold);

        for (int cnt = 1; !done; cnt *= 2)
        {
            const int ChunkSize = parent->maxGridSize(level)/cnt;

            if (ChunkSize < 16)
                //
                // Don't let grids get too small.
                //
                break;

            IntVect chunk(D_DECL(ChunkSize,ChunkSize,ChunkSize));

            for (int j = BL_SPACEDIM-1; j >=0 && ba.size() < Threshold; j--)
            {
                chunk[j] /= 2;
                ba.maxSize(chunk);
                if (ba.size() >= Threshold) done = true;
            }
        }

        DistributionMapping dm = getFuncCountDM(ba,ngrow);

        MultiFab diagTemp;
        MultiFab STemp(ba, nspecies+3, 0, dm);
        MultiFab fcnCntTemp(ba, 1, 0, dm);
        MultiFab FTemp(ba, Force.nComp(), 0, dm);

        const bool do_diag = plot_reactions && BoxLib::intersect(ba,auxDiag["REACTIONS"]->boxArray()).size() != 0;

        if (do_diag)
        {
            diagTemp.define(ba, auxDiag["REACTIONS"]->nComp(), 0, dm, Fab_allocate);
            diagTemp.copy(*auxDiag["REACTIONS"]); // Parallel copy
        }

        if (verbose && ParallelDescriptor::IOProcessor())
            std::cout << "*** advance_chemistry: FABs in tmp MF: " << STemp.size() << '\n';

        STemp.copy(mf_old,first_spec,0,nspecies+3); // Parallel copy.
        FTemp.copy(Force);                          // Parallel copy.

        for (MFIter Smfi(STemp); Smfi.isValid(); ++Smfi)
        {
            const FArrayBox& rYo      = STemp[Smfi];
            const FArrayBox& rHo      = STemp[Smfi];
            const FArrayBox& To       = STemp[Smfi];
            FArrayBox&       rYn      = STemp[Smfi];
            FArrayBox&       rHn      = STemp[Smfi];
            FArrayBox&       Tn       = STemp[Smfi];
            const Box&       bx       = Smfi.validbox();
            FArrayBox&       fc       = fcnCntTemp[Smfi];
            const FArrayBox& frc      = FTemp[Smfi];
            FArrayBox*       chemDiag = (do_diag ? &(diagTemp[Smfi]) : 0);

            BoxArray ba = do_avg_down_chem ? BoxLib::complementIn(bx,cf_grids) : BoxArray(bx);

            for (int i = 0; i < ba.size(); ++i)
            {
                const int s_spec = 0, s_rhoh = nspecies, s_temp = nspecies+2;

                bool ok = getChemSolve().solveTransient_sdc(rYn,rHn,Tn,rYo,rHo,To,frc,fc,ba[i],
							    s_spec,s_rhoh,s_temp,dt,chemDiag,
							    use_stiff_solver);
            }
        }

        FTemp.clear();

        mf_new.copy(STemp,0,first_spec,nspecies+3); // Parallel copy.

        STemp.clear();
        //
        // Set React_new (I_R).
        //
        MultiFab::Copy(React_new, mf_old, first_spec, 0, nspecies, 0);

        MultiFab::Subtract(React_new, mf_new, first_spec, 0, nspecies, 0);

        React_new.mult(-1/dt);

        MultiFab::Subtract(React_new, Force, 0, 0, nspecies, 0);

        if (do_diag)
        {
            auxDiag["REACTIONS"]->copy(diagTemp); // Parallel copy
            diagTemp.clear();
        }

        MultiFab& FC = get_new_data(FuncCount_Type);
	FC.copy(fcnCntTemp,0,0,1,0,std::min(ngrow,FC.nGrow()));

        fcnCntTemp.clear();
        //
        // Approximate covered crse chemistry (I_R) with averaged down fine I_R from previous time step.
        //
        if (do_avg_down_chem)
        {
            MultiFab& fine_React = getLevel(level+1).get_old_data(RhoYdot_Type);
	    BoxLib::average_down(fine_React, React_new, getLevel(level+1).geom, geom,
				 0, nspecies, fine_ratio);
        }
        //
        // Ensure consistent grow cells.
        //
        if (ngrow > 0)
        {
	    BL_ASSERT(React_new.nGrow() == 1);
	    React_new.FillBoundary(0,nspecies, geom.periodicity());
	    Extrapolater::FirstOrderExtrap(React_new, geom, 0, nspecies);
        }
    }

    if (verbose)
    {
        const int IOProc = ParallelDescriptor::IOProcessorNumber();

        Real mx = ParallelDescriptor::second() - strt_time, mn = mx;

        ParallelDescriptor::ReduceRealMin(mn,IOProc);
        ParallelDescriptor::ReduceRealMax(mx,IOProc);

        if (ParallelDescriptor::IOProcessor())
           std::cout << "HeatTransfer::advance_chemistry(): lev: " << level << ", time: ["
                      << mn
                      << " ... "
                      << mx
                      << "]\n";
    }
}

void
HeatTransfer::compute_scalar_advection_fluxes_and_divergence (const MultiFab& Force,
                                                              const MultiFab& DivU,
                                                              Real            dt)
{
  BL_PROFILE("HT::comp_sc_adv_fluxes_and_div()");
  //
  // Compute -Div(advective fluxes)  [ which is -aofs in NS, BTW ... careful...
  //
  if (verbose && ParallelDescriptor::IOProcessor())
    std::cout << "... computing advection terms\n";

  const Real  strt_time = ParallelDescriptor::second();
  const Real* dx        = geom.CellSize();
  const Real  prev_time = state[State_Type].prevTime();
  //
  // Gather info necesary to build transverse velocities
  // (stored internal to Godunov)
  //
  MultiFab Gp, VelViscTerms;
  const int use_forces_in_trans = godunov->useForcesInTrans();
  if (use_forces_in_trans || (do_mom_diff == 1))
  {
    VelViscTerms.define(grids,BL_SPACEDIM,nGrowAdvForcing,Fab_allocate);
    getViscTerms(VelViscTerms,Xvel,BL_SPACEDIM,prev_time);
    showMF("dd",VelViscTerms,"dd_VelViscTerms_in_aofs",level);

    Gp.define(grids,BL_SPACEDIM,nGrowAdvForcing,Fab_allocate);
    getGradP(Gp, state[Press_Type].prevTime());
    showMF("dd",Gp,"dd_Gp_in_aofs",level);
  }

  FArrayBox tvelforces;

  const int nState = desc_lst[State_Type].nComp();

  MultiFab dummy(grids,1,0,Fab_noallocate);
  for (FillPatchIterator S_fpi(*this,dummy,Godunov::hypgrow(),prev_time,State_Type,0,nState);
       S_fpi.isValid();
       ++S_fpi)
  {
    const Box& box = S_fpi.validbox();
    const int i = S_fpi.index();
    const FArrayBox& S = S_fpi();
    const FArrayBox& divu = DivU[S_fpi];
    const FArrayBox& force = Force[S_fpi];

    if (use_forces_in_trans || (do_mom_diff == 1))
    {
      NavierStokesBase::getForce(tvelforces,i,nGrowAdvForcing,Xvel,BL_SPACEDIM,
#ifdef GENGETFORCE
				 prev_time,
#endif		 
				 S,Density);

      godunov->Sum_tf_gp_visc(tvelforces,0,VelViscTerms[S_fpi],0,Gp[S_fpi],0,S,Density);
    }
    else
    {
      tvelforces.resize(BoxLib::grow(grids[i],nGrowAdvForcing),BL_SPACEDIM);
      tvelforces.setVal(0);
    }
    //
    // Set up the workspace internal to Godunov
    //
    Array<int> u_bc[BL_SPACEDIM];
    D_TERM(u_bc[0] = getBCArray(State_Type,i,0,1);,
           u_bc[1] = getBCArray(State_Type,i,1,1);,
           u_bc[2] = getBCArray(State_Type,i,2,1);)
      godunov->BuildWorkSpace(box,dx,dt);
    godunov->ComputeTransverVelocities(box,dx,dt,
                                       D_DECL(u_bc[0].dataPtr(),u_bc[1].dataPtr(),u_bc[2].dataPtr()),
                                       D_DECL(S,S,S), D_DECL(0,1,2), tvelforces, 0);

    (*aofs)[i].setVal(0,aofs->boxArray()[i],Density,NUM_SCALARS);
    for (int d=0; d<BL_SPACEDIM; ++d)
    {
      (*EdgeState[d])[i].setVal(0,(*EdgeState[d])[i].box(),Density,NUM_SCALARS);
      (*EdgeFlux[d])[i].setVal(0,(*EdgeFlux[d])[i].box(),Density,NUM_SCALARS);
    } 

    for (int comp = 0 ; comp < NUM_SCALARS ; comp++)
    {
      int state_ind = Density + comp;
      bool advect_this_comp = ( (state_ind >= first_spec) && (state_ind <= last_spec) )
        || state_ind == RhoH;

      if (advect_this_comp) {

        if (state_ind != Density) {
          Array<int> bc = getBCArray(State_Type,i,state_ind,1);
	  int iconserv = advectionType[state_ind] == Conservative ? 1 : 0;

          int fComp = Density + comp - first_spec;
          BL_ASSERT(fComp>=0 && fComp<=force.nComp());
          godunov->edge_states_fpu(box, dx, dt,
                                   D_DECL(u_mac[0][i],u_mac[1][i],u_mac[2][i]), D_DECL(0,0,0),
                                   D_DECL((*EdgeState[0])[i],(*EdgeState[1])[i],(*EdgeState[2])[i]),
                                   D_DECL(state_ind,state_ind,state_ind),
                                   S,state_ind,force,fComp,divu,0,state_ind,bc.dataPtr(),iconserv);
          for (int d=0; d<BL_SPACEDIM; ++d)
            (*EdgeFlux[d])[i].copy((*EdgeState[d])[i],state_ind,state_ind,1);
          int avcomp = 0;
          int ucomp = 0;
          // Compute Div(flux.Area), return Area-scaled (extensive) fluxes
          godunov->ComputeAofs(grids[i],
                               D_DECL(area[0][i],area[1][i],area[2][i]),D_DECL(avcomp,avcomp,avcomp),
                               D_DECL(u_mac[0][i],u_mac[1][i],u_mac[2][i]),D_DECL(ucomp,ucomp,ucomp),
                               D_DECL((*EdgeFlux[0])[i],(*EdgeFlux[1])[i],(*EdgeFlux[2])[i]),
                               D_DECL(state_ind,state_ind,state_ind), volume[i], avcomp,
                               (*aofs)[i], state_ind, iconserv);

          // Accumulate rho flux divergence, rho on edges, and rho flux on edges
          if (state_ind >= first_spec && state_ind <= last_spec) {
            (*aofs)[i].plus((*aofs)[i],state_ind,Density,1);
            for (int d=0; d<BL_SPACEDIM; ++d) {
              (*EdgeState[d])[i].plus((*EdgeState[d])[i],state_ind,Density,1);
              (*EdgeFlux[d])[i].plus((*EdgeFlux[d])[i],state_ind,Density,1);
            }
          }
        }
      }
    }
  }

  tvelforces.clear();

  if (use_forces_in_trans || (do_mom_diff == 1))
  {
      Gp.clear();
      VelViscTerms.clear();
  }

  showMF("sdc",*EdgeState[0],"sdc_ESTATE_x",level,parent->levelSteps(level));
  showMF("sdc",*EdgeState[1],"sdc_ESTATE_y",level,parent->levelSteps(level));
#if BL_SPACEDIM==3
  showMF("sdc",*EdgeState[2],"sdc_ESTATE_z",level,parent->levelSteps(level));
#endif

  // NOTE: Change sense of aofs here so that d/dt ~ aofs...be sure we use our own update function!
  aofs->mult(-1,Density,NUM_SCALARS);

  // AJN FLUXREG
  // We have just computed the advective fluxes.  If updateFluxReg=T,
  // ADD advective fluxes to ADVECTIVE flux register
  //
  //   FIXME: Since the fluxes and states are class data, perhaps these flux register calls should be
  //          managed in the advance function directly rather than hidden/encoded inside here??
  if (do_reflux && updateFluxReg)
  {
    if (level > 0)
    {
      for (int d = 0; d < BL_SPACEDIM; d++)
      {
        advflux_reg->FineAdd((*EdgeFlux[d]),d,Density,Density,NUM_SCALARS,dt);
      }
    }
    if (level < parent->finestLevel())
    {
      for (int d = 0; d < BL_SPACEDIM; d++)
      {
        getAdvFluxReg(level+1).CrseInit((*EdgeFlux[d]),d,Density,Density,NUM_SCALARS,-dt,FluxRegister::ADD);
      }
    }
  }

  if (verbose > 1)
  {
      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
      Real      run_time = ParallelDescriptor::second() - strt_time;

      ParallelDescriptor::ReduceRealMax(run_time,IOProc);

      if (ParallelDescriptor::IOProcessor())
          std::cout << "HeatTransfer::compute_scalar_advection_fluxes_and_divergence(): lev: " << level << ", time: " << run_time << '\n';
  }
}

//
// An enum to clean up mac_sync...questionable usefullness
//
enum SYNC_SCHEME {ReAdvect, UseEdgeState, Other};

void
HeatTransfer::mac_sync ()
{

    BL_PROFILE("HT::mac_sync()");
    if (verbose && ParallelDescriptor::IOProcessor())
        std::cout << "... mac_sync\n";

    const Real strt_time = ParallelDescriptor::second();

    int        sigma;
    const int  finest_level   = parent->finestLevel();
    const int  ngrids         = grids.size();
    const Real prev_time      = state[State_Type].prevTime();
    const Real cur_time       = state[State_Type].curTime();
    const Real prev_pres_time = state[Press_Type].prevTime();
    const Real dt             = parent->dtLevel(level);
    MultiFab&  rho_half       = get_rho_half_time();
    //
    // DeltaSsync Will hold q^{n+1,p} * (delta rho)^sync for conserved quantities
    // as defined before Eq (18) in DayBell:2000.  Note that in the paper, 
    // Eq (18) is missing Y_m^{n+1,p} * (delta rho)^sync in the RHS
    // and Eq (19) is missing the h^{n+1,p} * (delta rho)^sync in the RHS.
    //
    MultiFab*  DeltaSsync = 0;

    MultiFab& S_new = get_new_data(State_Type);

    ////////////////////////
    // save states that we need to reset with each mac sync iteration
    ////////////////////////
    const int numscal = NUM_STATE - BL_SPACEDIM;

    MultiFab chi_sync(grids,1,0);
    chi_sync.setVal(0);

    PArray<MultiFab> S_new_sav(finest_level+1,PArrayManage);

    for (int lev=level; lev<=finest_level; lev++)
    {
      const MultiFab& S_new_lev = getLevel(lev).get_new_data(State_Type);
      S_new_sav.set(lev,new MultiFab(S_new_lev.boxArray(),NUM_STATE,1,S_new_lev.DistributionMap()));
      MultiFab::Copy(S_new_sav[lev],S_new_lev,0,0,NUM_STATE,1);
    }

    PArray<MultiFab> Ssync_sav(finest_level,PArrayManage);
    PArray<MultiFab> Vsync_sav(finest_level,PArrayManage);

    for (int lev=level; lev<=finest_level-1; lev++)
    {
      const MultiFab& Ssync_lev = getLevel(lev).Ssync;
      Ssync_sav.set(lev,new MultiFab(Ssync_lev.boxArray(),numscal,1,Ssync_lev.DistributionMap()));
      MultiFab::Copy(Ssync_sav[lev],Ssync_lev,0,0,numscal,1);

      const MultiFab& Vsync_lev = getLevel(lev).Vsync;
      Vsync_sav.set(lev,new MultiFab(Vsync_lev.boxArray(),BL_SPACEDIM,1,Vsync_lev.DistributionMap()));
      MultiFab::Copy(Vsync_sav[lev],Vsync_lev,0,0,BL_SPACEDIM,1);
    }

    ////////////////////////
    // begin mac_sync_iter loop here
    ////////////////////////
    for (int mac_sync_iter=0; mac_sync_iter < num_mac_sync_iter; mac_sync_iter++)
    {
      bool last_mac_sync_iter;
      if (mac_sync_iter == num_mac_sync_iter-1)
      {
	last_mac_sync_iter=true;
      }
      else
      {
	last_mac_sync_iter=false;
      }

      MultiFab chi_sync_increment(grids,1,0);

      for (int lev=level; lev<=finest_level; lev++)
      {
	MultiFab& S_new_lev = getLevel(lev).get_new_data(State_Type);
	MultiFab::Copy(S_new_lev,S_new_sav[lev],0,0,NUM_STATE,1);
      }

      for (int lev=level; lev<=finest_level-1; lev++)
      {
	MultiFab& Ssync_lev = getLevel(lev).Ssync;
	MultiFab::Copy(Ssync_lev,Ssync_sav[lev],0,0,numscal,1);

	MultiFab& Vsync_lev = getLevel(lev).Vsync;
	MultiFab::Copy(Vsync_lev,Vsync_sav[lev],0,0,BL_SPACEDIM,1);
      }

      make_rho_curr_time();
      get_rho_half_time();

      //
      // Compute the corrective pressure, mac_sync_phi, used to 
      // compute U^{ADV,corr} in mac_sync_compute
      //
      bool subtract_avg = false;
      if (closed_chamber && level == 0)
      {
	subtract_avg = true;
      }
      Real offset = 0.0;

      BL_PROFILE_VAR("HT::mac_sync::ucorr", HTUCORR);
      mac_projector->mac_sync_solve(level,dt,rho_half,fine_ratio,
				    &chi_sync,subtract_avg,offset);
      BL_PROFILE_VAR_STOP(HTUCORR);

      if (!do_reflux) return;

      Array<SYNC_SCHEME> sync_scheme(NUM_STATE,ReAdvect);

      if (do_mom_diff == 1)
        for (int i=0; i<BL_SPACEDIM; ++i)
	  sync_scheme[i] = UseEdgeState;

      for (int i=BL_SPACEDIM; i<NUM_STATE; ++i)
        sync_scheme[i] = UseEdgeState;
        
      Array<int> incr_sync(NUM_STATE,0);
      for (int i=0; i<sync_scheme.size(); ++i)
        if (sync_scheme[i] == ReAdvect)
	  incr_sync[i] = 1;
      //
      // After solving for mac_sync_phi in mac_sync_solve(), we
      // can now do the sync advect step in mac_sync_compute().
      // This consists of two steps
      //
      // 1. compute U^{ADV,corr} as the gradient of mac_sync_phi
      // 2. add -D^MAC ( U^{ADV,corr} * rho * q)^{n+1/2} ) to flux registers,
      //    which already contain the delta F adv/diff flux mismatches
      //

      //
      // velocities
      //
      BL_PROFILE_VAR("HT::mac_sync::Vsync", HTVSYNC);
      if (do_mom_diff == 0) 
	{
	  mac_projector->mac_sync_compute(level,u_mac,Vsync,Ssync,rho_half,
					  (level > 0) ? &getAdvFluxReg(level) : 0,
					  advectionType,prev_time,
					  prev_pres_time,dt,NUM_STATE,
					  be_cn_theta,
					  modify_reflux_normal_vel,
					  do_mom_diff,
					  incr_sync, 
					  last_mac_sync_iter);
	}
      else
      {
        for (int comp=0; comp<BL_SPACEDIM; ++comp)
	{
	  if (sync_scheme[comp]==UseEdgeState)
          {
	    mac_projector->mac_sync_compute(level,Vsync,comp,
					    comp,EdgeState, comp,rho_half,
					    (level > 0 ? &getAdvFluxReg(level):0),
					    advectionType,modify_reflux_normal_vel,dt,
					    last_mac_sync_iter);
	  }
        }
      }
      BL_PROFILE_VAR_STOP(HTVSYNC);

      showMF("sdcSync",Ssync,"sdc_Ssync_no_Ucorr",level,parent->levelSteps(level));
      showMF("sdcSync",*EdgeState[0],"sdc_ESTATE_x_no_Ucorr",level,parent->levelSteps(level));
      showMF("sdcSync",*EdgeState[1],"sdc_ESTATE_y_no_Ucorr",level,parent->levelSteps(level));
      //
      // Scalars.
      //
      BL_PROFILE_VAR("HT::mac_sync::Ssync", HTSSYNC);
      for (int comp=BL_SPACEDIM; comp<NUM_STATE; ++comp)
      {
        if (sync_scheme[comp]==UseEdgeState)
        {
	  int s_ind = comp - BL_SPACEDIM;
	  //
	  // Ssync contains the adv/diff coarse-fine flux mismatch divergence
	  // This routine does a sync advect step for a single scalar component,
	  // i.e., subtracts the D(Ucorr rho q) term from Ssync
	  // The half-time edge states are passed in.
	  // This routine is useful when the edge states are computed
	  // in a physics-class-specific manner. (For example, as they are
	  // in the calculation of div rho U h = div U sum_l (rho Y)_l h_l(T)).
	  // Note: the density component now contains (delta rho)^sync since there
	  // is no diffusion for this term
	  //
	  mac_projector->mac_sync_compute(level,Ssync,comp,s_ind,
                                          EdgeState,comp,rho_half,
					  (level > 0 ? &getAdvFluxReg(level):0),
					  advectionType,modify_reflux_normal_vel,dt,
					  last_mac_sync_iter);
        }
      }
      BL_PROFILE_VAR_STOP(HTSSYNC);
        
      showMF("sdcSync",Ssync,"sdc_Ssync_after_Ucorr",level,parent->levelSteps(level));
      showMF("sdcSync",*EdgeState[0],"sdc_ESTATE_x_no_Ucorr",level,parent->levelSteps(level));
      showMF("sdcSync",*EdgeState[1],"sdc_ESTATE_y_no_Ucorr",level,parent->levelSteps(level));

      BL_PROFILE_VAR_START(HTSSYNC);
      Ssync.mult(dt,Ssync.nGrow());

      sync_setup(DeltaSsync);
      //
      // For all conservative variables Q (other than density)
      // set DeltaSsync = q^{n+1,p} * (delta rho)^sync,
      // then subtract DeltaSsync from Ssync
      // (these are the terms that were accidentally omitted in (18) and (19)
      //
      FArrayBox delta_ssync;

      for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
      {
        const int  i   = mfi.index();
        const Box& grd = grids[i];

        int iconserved = -1;

        for (int istate = BL_SPACEDIM; istate < NUM_STATE; istate++)
	{
	  if (istate != Density && advectionType[istate] == Conservative)
          {
	    iconserved++;
	    delta_ssync.resize(grd,1);
	    delta_ssync.copy(S_new[mfi],grd,istate,grd,0,1); // delta_ssync = (rho*q)^{n+1,p}
	    delta_ssync.divide(S_new[mfi],grd,Density,0,1); // delta_ssync = q^{n+1,p}
	    FArrayBox& s_sync = Ssync[mfi]; // Ssync = RHS of Eq (18), (19) without the q^{n+1,p} * (delta rho)^sync terms
	    // note that the density component contains (delta rho)^sync
	    delta_ssync.mult(s_sync,grd,Density-BL_SPACEDIM,0,1); // delta_ssync = q^{n+1,p} * (delta rho)^sync
	    (*DeltaSsync)[mfi].copy(delta_ssync,grd,0,grd,iconserved,1); // DeltaSsync = q^{n+1,p} * (delta rho)^sync
	    s_sync.minus(delta_ssync,grd,0,istate-BL_SPACEDIM,1); // Ssync = Ssync - q^{n+1,p} * (delta rho)^sync
	  }
        }
      }

      delta_ssync.clear();
      //
      // Now, increment density.
      //
      for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
      {
        const int i = mfi.index();
        S_new[mfi].plus(Ssync[mfi],grids[i],Density-BL_SPACEDIM,Density,1);
      }

      make_rho_curr_time();
      BL_PROFILE_VAR_STOP(HTSSYNC);
      //
      // Set do_diffuse_sync to 0 for debugging reasons only.
      //
      BL_PROFILE_VAR_START(HTVSYNC);
      if (do_mom_diff == 1)
      {
        for (MFIter Vsyncmfi(Vsync); Vsyncmfi.isValid(); ++Vsyncmfi)
        {
	  const int  i    = Vsyncmfi.index();
	  const Box& vbox = rho_ctime.box(i);

	  D_TERM(Vsync[Vsyncmfi].divide(rho_ctime[Vsyncmfi],vbox,0,Xvel,1);,
		 Vsync[Vsyncmfi].divide(rho_ctime[Vsyncmfi],vbox,0,Yvel,1);,
		 Vsync[Vsyncmfi].divide(rho_ctime[Vsyncmfi],vbox,0,Zvel,1););
        }
      }
      BL_PROFILE_VAR_STOP(HTVSYNC);
      
      BL_PROFILE_VAR_START(HTSSYNC);
      if (do_diffuse_sync)
      {
	FluxBoxes fb_beta(this);
        MultiFab** beta = fb_beta.get();
        if (is_diffusive[Xvel])
        {
	  int rho_flag = (do_mom_diff == 0) ? 1 : 3;
	  getViscosity(beta, cur_time);
	  diffusion->diffuse_Vsync(Vsync,dt,be_cn_theta,rho_half,rho_flag,beta,0,
				   last_mac_sync_iter);
        }
	    
        if (!unity_Le 
	    && nspecies>0 
            && do_add_nonunityLe_corr_to_rhoh_adv_flux) 
	{
	  //
	  // Diffuse the species syncs such that sum(SpecDiffSyncFluxes) = 0
	  // After exiting, SpecDiffusionFluxnp1 should contain rhoD grad (delta Y)^sync
	  // Also, Ssync for species should contain rho^{n+1} * (delta Y)^sync
	  //
	  differential_spec_diffuse_sync(dt,false,last_mac_sync_iter);

#ifdef USE_WBAR
	  for (int dir=0; dir<BL_SPACEDIM; ++dir)
	  {
	    (*SpecDiffusionFluxWbar)[dir].setVal(0.);
	  }

	  // compute beta grad Wbar terms using the n+1,p state
	  // we want this to have a negative sign since we will add
	  // the beta grad Wbar terms using the updated state later to create the delta
	  // store in SpecDiffusionFluxWbar
	  compute_Wbar_fluxes(cur_time,-1);

	  // create an updated (but stil temporary) new state species
	  // by adding Ssync plus DeltaSsync to the new state
	  for (MFIter mfi(Ssync); mfi.isValid(); ++mfi)
	  {
	    const int i = mfi.index();
		
	    int iconserved = -1;
		
	    for (int istate = BL_SPACEDIM; istate < NUM_STATE; istate++)
	    {
	      if (istate != Density && advectionType[istate] == Conservative)
	      {
		iconserved++;
	
		// only actually update the species
		// this is the Y_m^{n+1,p} * delta rho^sync piece
		if (istate >= first_spec && istate <= last_spec)
		{
		  Ssync[mfi].plus((*DeltaSsync)[mfi],grids[i],iconserved,istate-BL_SPACEDIM,1);
		}
	      }
	    }
	  }
	  for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
	  {
	    const int i = mfi.index();
		
	    for (int sigma = 0; sigma < numscal; sigma++)
	    {
	      // only actually update the species
	      // this is the rho^{n+1} * delta Y^sync piece
	      if (BL_SPACEDIM+sigma >= first_spec && BL_SPACEDIM+sigma <= last_spec)
	      {
		S_new[mfi].plus(Ssync[mfi],grids[i],sigma,BL_SPACEDIM+sigma,1);
	      }
	    }
	  }

	  // compute beta grad Wbar terms using the temporary new state
	  // add these to SpecDiffusionFluxWbar so it contains the delta
	  compute_Wbar_fluxes(cur_time,1);

	  // subtract Ssync and DeltaSsync from the new state to restore the n+1,p state
	  for (MFIter mfi(Ssync); mfi.isValid(); ++mfi)
	  {
	    const int i = mfi.index();
		
	    int iconserved = -1;
		
	    for (int istate = BL_SPACEDIM; istate < NUM_STATE; istate++)
	    {
	      if (istate != Density && advectionType[istate] == Conservative)
	      {
		iconserved++;
	
		// only actually update the species
		// this is the Y_m^{n+1,p} * delta rho^sync piece
		if (istate >= first_spec && istate <= last_spec)
		{
		  Ssync[mfi].minus((*DeltaSsync)[mfi],grids[i],iconserved,istate-BL_SPACEDIM,1);
		}
	      }
	    }
	  }
	  for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
	  {
	    const int i = mfi.index();
		
	    for (int sigma = 0; sigma < numscal; sigma++)
	    {
	      // only actually update the species
	      // this is the rho^{n+1} * delta Y^sync piece
	      if (BL_SPACEDIM+sigma >= first_spec && BL_SPACEDIM+sigma <= last_spec)
	      {
		S_new[mfi].minus(Ssync[mfi],grids[i],sigma,BL_SPACEDIM+sigma,1);
	      }
	    }
	  }

	  // take divergence of beta grad delta Wbar and multiply divergence by dt/2
	  MultiFab DWbar(grids,nspecies,nGrowAdvForcing);
	  MultiFab* const * fluxWbar = SpecDiffusionFluxWbar;
	  flux_divergence(DWbar,0,fluxWbar,0,nspecies,-1);
	  DWbar.mult(dt/2.0);

	  // reset Ssync to be the same RHS as above, but with the (dt/2) div beta grad delta Wbar term
	  // use the code above, but add on the grad delta Wbar term
	  for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
	  {
	    const int  i   = mfi.index();
	    const Box& grd = grids[i];
	    const FArrayBox& DWbarFab = DWbar[mfi];
	    int iconserved = -1;
		
	    for (int istate = BL_SPACEDIM; istate < NUM_STATE; istate++)
	    {
	      if (istate != Density && advectionType[istate] == Conservative)
	      {
		iconserved++;
		if (istate >= first_spec && istate <= last_spec)
		{
		  delta_ssync.resize(grd,1);
		  delta_ssync.copy(S_new[mfi],grd,istate,grd,0,1); // delta_ssync = (rho*q)^{n+1,p}
		  delta_ssync.divide(S_new[mfi],grd,Density,0,1); // delta_ssync = q^{n+1,p}
		  FArrayBox& s_sync = Ssync[mfi]; // Ssync = RHS of Eq (18), (19) without the q^{n+1,p} * (delta rho)^sync terms
		  delta_ssync.mult(s_sync,grd,Density-BL_SPACEDIM,0,1); // delta_ssync = q^{n+1,p} * (delta rho)^sync
		  (*DeltaSsync)[mfi].copy(delta_ssync,grd,0,grd,iconserved,1); // DeltaSsync = q^{n+1,p} * (delta rho)^sync
		  s_sync.minus(delta_ssync,grd,0,istate-BL_SPACEDIM,1); // Ssync = Ssync - q^{n+1,p} * (delta rho)^sync
		  s_sync.plus(DWbarFab,grd,istate-first_spec,istate-BL_SPACEDIM,1); // add grad delta Wbar terms
		}
	      }
	    }
	  }
	    
	  delta_ssync.clear();

	  // call differential_spec_diffuse_sync again, but this time the conservative
	  // correction needs to be the sum of the delta Y_m and SpecDiffusionFluxWbar terms
	  // FIXME
	  differential_spec_diffuse_sync(dt, true);

#endif

	  const Real cur_time  = state[State_Type].curTime();
	  const Real a = 1.0;     // Passed around, but not used
	  Real rhsscale;          //  -ditto-
	  const int rho_flag = 2; // FIXME: Messy assumption
	  MultiFab *alpha=0;      //  -ditto-
	  FluxBoxes fb_SC  (this, 1, 0);
	  FluxBoxes fb_NULN(this, nspecies, 0);
	  FluxBoxes fb_visc(this, 1, 0);
	  MultiFab **fluxSC    =   fb_SC.get();
	  MultiFab **fluxNULN  = fb_NULN.get();
	  MultiFab **rhoh_visc = fb_visc.get();
	  
	  const int nGrow    = 1; // Size to grow fil-patched fab for T below
	  const int dataComp = 0; // coeffs loaded into 0-comp for all species
                  
	  getDiffusivity(rhoh_visc, cur_time, RhoH, 0, 1); // RhoH (lambda/cp)
	  
	  MultiFab Soln(grids,1,1);

	  // compute lambda/cp grad (delta Y_m^sync)
	  for (int comp = 0; comp < nspecies; ++comp)
          {
	    const Real b     = be_cn_theta;
	    const int  sigma = first_spec + comp;
	    //
	    //  start by getting lambda/cp.Grad(delta Y^sync)
	    //   (note: neg of usual diff flux)
	    //
	    ViscBndry      visc_bndry;
	    ABecLaplacian* visc_op;

	    visc_op = diffusion->getViscOp(sigma,a,b,cur_time,
					   visc_bndry,rho_half,
					   rho_flag,&rhsscale,rhoh_visc,dataComp,
					   alpha,dataComp);

	    visc_op->maxOrder(diffusion->maxOrder());

	    // copy rho^{n+1} * (delta Y)^sync into Soln
	    MultiFab::Copy(Soln,Ssync,sigma-BL_SPACEDIM,0,1,0);

	    // divide Soln by rho
	    for (MFIter Smfi(Soln); Smfi.isValid(); ++Smfi)
	    {
	      Soln[Smfi].divide(S_new[Smfi],Smfi.validbox(),Density,0,1);
            }

	    // compute lambda/cp.Grad(delta Y^sync) and weight
	    visc_op->compFlux(D_DECL(*fluxSC[0],*fluxSC[1],*fluxSC[2]),Soln);

	    for (int d = 0; d < BL_SPACEDIM; ++d)
	      fluxSC[d]->mult(-b/geom.CellSize()[d]);
	    //
	    // Here, get fluxNULN = (lambda/cp - rho.D)Grad(delta Ysync)
	    //                    = lambda/cp.Grad(delta Ysync) + SpecSyncDiffFlux
	    //
	    for (int d = 0; d < BL_SPACEDIM; ++d)
            {
	      MFIter SDF_mfi(*SpecDiffusionFluxnp1[d]);
	      
	      for ( ; SDF_mfi.isValid(); ++SDF_mfi)
	      {
		FArrayBox& fluxSC_fab   = (*fluxSC[d])[SDF_mfi];
		FArrayBox& fluxNULN_fab = (*fluxNULN[d])[SDF_mfi];
		FArrayBox& SDF_fab = (*SpecDiffusionFluxnp1[d])[SDF_mfi];
		const Box& ebox    = SDF_mfi.validbox();
		// copy in (delta Gamma)^sync
		fluxNULN_fab.copy(SDF_fab,ebox,comp,ebox,comp,1);
		// add in (lambda/cp) grad (delta Y^sync)
		fluxNULN_fab.plus(fluxSC_fab,ebox,0,comp,1);
	      }
	    }
	    delete visc_op;
	  }

	  Soln.clear();
	  
	  fb_SC.clear();
	  fb_visc.clear();
	  //
	  // Multiply fluxi by h_i (let FLXDIV routine below sum up the fluxes)
	  //
	  FArrayBox eTemp, h;

	  for (FillPatchIterator Tnew_fpi(*this,S_new,nGrow,cur_time,State_Type,Temp,1);
	       Tnew_fpi.isValid();
	       ++Tnew_fpi)
          {
	    const Box& box = Tnew_fpi.validbox();

	    for (int d = 0; d < BL_SPACEDIM; ++d)
	    {
	      const Box& ebox = BoxLib::surroundingNodes(box,d);
	      eTemp.resize(ebox,1);
	      FPLoc bc_lo = fpi_phys_loc(get_desc_lst()[State_Type].getBC(Temp).lo(d));
	      FPLoc bc_hi = fpi_phys_loc(get_desc_lst()[State_Type].getBC(Temp).hi(d));
              
	      center_to_edge_fancy(Tnew_fpi(),eTemp,BoxLib::grow(box,BoxLib::BASISV(d)),
				   0,0,1,geom.Domain(),bc_lo,bc_hi);
	      
	      h.resize(ebox,nspecies);
	      getChemSolve().getHGivenT(h,eTemp,ebox,0,0);
	      
	      // multiply fluxNULN by h_m
	      (*fluxNULN[d])[Tnew_fpi].mult(h,ebox,0,0,nspecies);
	    }
	  }

	  h.clear();
	  eTemp.clear();
	  //
	  // Add the NULN fluxes to the RHS of the (delta h)^sync diffusion solve
	  // afterwards, the entire RHS should be ready.
	  //
	  for (MFIter Ssync_mfi(Ssync); Ssync_mfi.isValid(); ++Ssync_mfi)
          {
	    FArrayBox& syncn = Ssync[Ssync_mfi];
	    const FArrayBox& synco = Ssync[Ssync_mfi];
	    const Box& box = Ssync_mfi.validbox();

	    //
	    // Multiply by dt*dt, one to make it extensive, and one because
	    // Ssync multiplied above by dt, need same units here.
	    //
	    const Real mult = dt*dt;
	    const int sigmaRhoH = RhoH - BL_SPACEDIM; // RhoH comp in Ssync
		    
	    FORT_INCRWEXTFLXDIV(box.loVect(), box.hiVect(),
				(*fluxNULN[0])[Ssync_mfi].dataPtr(),
				ARLIM((*fluxNULN[0])[Ssync_mfi].loVect()),
				ARLIM((*fluxNULN[0])[Ssync_mfi].hiVect()),
				(*fluxNULN[1])[Ssync_mfi].dataPtr(),
				ARLIM((*fluxNULN[1])[Ssync_mfi].loVect()),
				ARLIM((*fluxNULN[1])[Ssync_mfi].hiVect()),
#if BL_SPACEDIM == 3
				(*fluxNULN[2])[Ssync_mfi].dataPtr(),
				ARLIM((*fluxNULN[2])[Ssync_mfi].loVect()),
				ARLIM((*fluxNULN[2])[Ssync_mfi].hiVect()),
#endif
				synco.dataPtr(sigmaRhoH),
				ARLIM(synco.loVect()),
				ARLIM(synco.hiVect()),
				syncn.dataPtr(sigmaRhoH),
				ARLIM(syncn.loVect()),
				ARLIM(syncn.hiVect()),
				volume[Ssync_mfi].dataPtr(),
				ARLIM(volume[Ssync_mfi].loVect()),
				ARLIM(volume[Ssync_mfi].hiVect()),
				&nspecies, &mult);
	  }
        }

	FluxBoxes fb_flux(this);
        MultiFab **flux = fb_flux.get();
	
        showMF("sdcSync",Ssync,"sdc_Sync_preDiff",level,parent->levelSteps(level));

        for (sigma = 0; sigma < numscal; sigma++)
        {
	  int rho_flag = 0;
	  int do_viscsyncflux = do_reflux;
	  const int state_ind = BL_SPACEDIM + sigma;
	  //
	  // To diffuse, or not?
	  // (1) Density, no
	  // (2) RhoH...if diffusive
	  // (3) Trac...if diffusive
	  // (4) Spec:
	  //    (a) if Le==1, and spec diffusive
	  //    (b) if Le!=1, do differential diffusion instead, done above
	  // (5) Temp, no (set instead by RhoH to Temp)
	  //
	  const bool is_spec = state_ind<=last_spec && state_ind>=first_spec;
	  int do_it
	    =  state_ind!=Density 
	    && state_ind!=Temp
	    && is_diffusive[state_ind]
	    && !(is_spec && !unity_Le);
	  
	  if (do_it && (is_spec || state_ind==RhoH))
	    rho_flag = 2;

	  if (do_it)
          {
	    MultiFab* alpha = 0;
	    getDiffusivity(beta, cur_time, state_ind, 0, 1);
                    
	    // on entry, Ssync = RHS for (delta h)^sync diffusive solve
	    // on exit, Ssync = rho^{n+1} * (delta h)^sync
	    // on exit, flux = coeff * grad phi
	    diffusion->diffuse_Ssync(Ssync,sigma,dt,be_cn_theta,rho_half,
				     rho_flag,flux,0,beta,0,alpha,0);
	    if (do_viscsyncflux && level > 0 && last_mac_sync_iter)
	    {
	      for (int d=0; d<BL_SPACEDIM; ++d)
		getViscFluxReg().FineAdd(*flux[d],d,0,state_ind,1,dt);
              }
	  }
        }
      }
      BL_PROFILE_VAR_STOP(HTSSYNC);
      showMF("sdcSync",Ssync,"sdc_Sync_postDiff",level,parent->levelSteps(level));

      //
      // For all conservative variables Q (other than density)
      // increment sync by (sync_for_rho)*q_presync.
      // Before this loop, Ssync holds rho^{n+1} (delta phi)^sync
      // DeltaSsync holds (delta rho)^sync phi^p
      //
      BL_PROFILE_VAR_START(HTSSYNC);
      for (MFIter mfi(Ssync); mfi.isValid(); ++mfi)
      {
	const int i = mfi.index();
	  
	int iconserved = -1;
	  
	for (int istate = BL_SPACEDIM; istate < NUM_STATE; istate++)
        {
	  if (istate != Density && advectionType[istate] == Conservative)
          {
	    iconserved++;
	    Ssync[mfi].plus((*DeltaSsync)[mfi],grids[i],iconserved,istate-BL_SPACEDIM,1);
	  }
        }
      }
      sync_cleanup(DeltaSsync);
      BL_PROFILE_VAR_STOP(HTSSYNC);
      showMF("sdcSync",Ssync,"sdc_Sync_post_add_QdRho",level,parent->levelSteps(level));
      //
      // Increment the state (for all but rho, since that was done above)
      //
      BL_PROFILE_VAR_START(HTSSYNC);
      for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
      {
        const int i = mfi.index();

        for (int sigma = 0; sigma < numscal; sigma++)
        {
	  if (!(BL_SPACEDIM+sigma == Density))
	  {
	    S_new[mfi].plus(Ssync[mfi],grids[i],sigma,BL_SPACEDIM+sigma,1);
	  }
        }
      }

      //
      // Recompute temperature and rho R T after the mac_sync.
      //
      RhoH_to_Temp(S_new);
      setThermoPress(cur_time);
      BL_PROFILE_VAR_STOP(HTSSYNC);
      
      showMF("sdcSync",S_new,"sdc_Snew_postSync",level,parent->levelSteps(level));
      //
      // Get boundary conditions.
      //
      BL_PROFILE_VAR_START(HTSSYNC);
      Real mult = 1.0;
      Array<int*>         sync_bc(grids.size());
      Array< Array<int> > sync_bc_array(grids.size());
      for (int i = 0; i < ngrids; i++)
      {
        sync_bc_array[i] = getBCArray(State_Type,i,Density,numscal);
        sync_bc[i]       = sync_bc_array[i].dataPtr();
      }
      //
      // Interpolate the sync correction to the finer levels.
      //
      IntVect ratio = IntVect::TheUnitVector();
      
      for (int lev = level+1; lev <= finest_level; lev++)
      {
        ratio                   *= parent->refRatio(lev-1);
        HeatTransfer& fine_level = getLevel(lev);
        MultiFab& S_new_lev      = fine_level.get_new_data(State_Type);
        //
        // New way of interpolating syncs to make sure mass is conserved
        // and to ensure freestream preservation for species & temperature.
        //
        const BoxArray& fine_grids = S_new_lev.boxArray();
        const int nghost           = S_new_lev.nGrow();
        MultiFab increment(fine_grids, numscal, nghost);
        increment.setVal(0,nghost);
        //
        // Note: we use the lincc_interp (which_interp==3) for density,
        // rho*h and rho*Y, cell_cons_interp for everything else. Doing
        // so is needed for freestream preservation of Y and T.  The setting
        // which_interp=5 calls the lincc_interp, but then follows with a
        // "protection" step to be sure that all the components but the
        // first and the last have their sync adjusted to try to preserve
        // positivity after the sync is applied.  The density sync is then
        // adjusted to be the sum of the species syncs.
        //
        // HACK note: Presently, the species mass syncs are redistributed 
        //            without consequence to the enthalpy sync.  Clearly
        //            the species carry enthalphy, so the enthalph sync should
        //            be adjusted as well.  Note yet sure how to do this correctly.
        //            Punt for now...
        //
        const SyncInterpType which_interp = CellConsProt_T;

        const int nComp = 2+nspecies;

        SyncInterp(Ssync, level, increment, lev, ratio, 
                   Density-BL_SPACEDIM, Density-BL_SPACEDIM, nComp, 1, mult, 
                   sync_bc.dataPtr(), which_interp, Density);

        if (have_trac)
	  SyncInterp(Ssync, level, increment, lev, ratio, 
		     Trac-BL_SPACEDIM, Trac-BL_SPACEDIM, 1, 1, mult, 
		     sync_bc.dataPtr());
	
        if (have_rhort)
	  SyncInterp(Ssync, level, increment, lev, ratio, 
		     RhoRT-BL_SPACEDIM, RhoRT-BL_SPACEDIM, 1, 1, mult, 
		     sync_bc.dataPtr());
	
        SyncInterp(Ssync, level, increment, lev, ratio, 
                   Temp-BL_SPACEDIM, Temp-BL_SPACEDIM, 1, 1, mult, 
                   sync_bc.dataPtr());
        if (do_set_rho_to_species_sum)
        {
	  increment.setVal(0,Density-BL_SPACEDIM,1,0);

	  for (int istate = first_spec; istate <= last_spec; istate++)
          { 
	    for (MFIter mfi(increment); mfi.isValid(); ++mfi)
            {
	      int i = mfi.index();
	      increment[mfi].plus(increment[mfi],fine_grids[i],
				  istate-BL_SPACEDIM,Density-BL_SPACEDIM,1);
	    }
	  }
        }
 
        for (MFIter mfi(increment); mfi.isValid(); ++mfi)
        {
	  int i = mfi.index();
	  S_new_lev[mfi].plus(increment[mfi],fine_grids[i],0,Density,numscal);
        }

	if (last_mac_sync_iter)
	{
	  fine_level.make_rho_curr_time();
	  fine_level.incrRhoAvg(increment,Density-BL_SPACEDIM,1.0);
	}
        //
        // Recompute temperature and rho R T after interpolation of the mac_sync correction
        //   of the individual quantities rho, Y, T.
        //
        RhoH_to_Temp(S_new_lev);
        fine_level.setThermoPress(cur_time);
      }

      //
      // Average down Trac = rho R T after interpolation of the mac_sync correction
      //   of the individual quantities rho, Y, T.
      //
      for (int lev = finest_level-1; lev >= level; lev--)
      {
        HeatTransfer& fine_level = getLevel(lev+1);
        HeatTransfer& crse_level = getLevel(lev);

        MultiFab& S_crse_loc = crse_level.get_new_data(State_Type);
        MultiFab& S_fine_loc = fine_level.get_new_data(State_Type);

        const int pComp = (have_rhort ? RhoRT : Trac);
	BoxLib::average_down(S_fine_loc, S_crse_loc, fine_level.geom, crse_level.geom,
			     pComp, 1, crse_level.fine_ratio);
      }
      BL_PROFILE_VAR_STOP(HTSSYNC);
      showMF("sdcSync",S_new,"sdc_Snew_postInterpAvgSync",level,parent->levelSteps(level));

      chi_sync_increment.setVal(0,0);
      calc_dpdt(cur_time,dt,chi_sync_increment,u_mac);

      MultiFab::Subtract(chi_sync,chi_sync_increment,0,0,1,0);

    }  // end loop over mac_sync_iters

    if (verbose)
    {
      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
      Real      run_time = ParallelDescriptor::second() - strt_time;
      
      ParallelDescriptor::ReduceRealMax(run_time,IOProc);
      
      if (ParallelDescriptor::IOProcessor())
	std::cout << "HeatTransfer::mac_sync(): lev: " << level << ", time: " << run_time << '\n';
    }

}

#ifdef USE_WBAR
void
HeatTransfer::compute_Wbar_fluxes(Real time,
				  Real inc)
{
    BL_PROFILE("HT::compute_Wbar_fluxes()");

    // allocate edge-beta for Wbar
    FluxBoxes fb_betaWbar(this, nspecies, 0);
    MultiFab** betaWbar =fb_betaWbar.get();

    // average transport coefficients for Wbar to edges
    getDiffusivity_Wbar(betaWbar,time);

    int nGrowOp = 1;

    FArrayBox tmp;
    MultiFab rho_and_species(grids,nspecies+1,nGrowOp);

    for (FillPatchIterator fpi(*this,rho_and_species,nGrowOp,time,State_Type,Density,nspecies+1);
         fpi.isValid();
	 ++fpi)
    {
      FArrayBox& rho_and_spec = rho_and_species[fpi];
      rho_and_spec.copy(fpi(),0,0,nspecies+1);

      tmp.resize(rho_and_spec.box(),1);
      tmp.copy(rho_and_spec,0,0,1);
      tmp.invert(1);

      for (int comp = 0; comp < nspecies; ++comp) 
	rho_and_spec.mult(tmp,0,comp+1,1);
    }

    // add in grad wbar term
    MultiFab Wbar;

    Wbar.define(grids,1,nGrowOp,Fab_allocate);

    for (MFIter mfi(rho_and_species); mfi.isValid(); ++mfi)
    {
      const Box& gbox = BoxLib::grow(mfi.validbox(),nGrowOp);
      getChemSolve().getMwmixGivenY(Wbar[mfi],rho_and_species[mfi],gbox,1,0);
    }

    //
    // Here, we'll use the same LinOp as Y for filling grow cells.
    //
    const Real* dx    = geom.CellSize();
    const BCRec& bc = get_desc_lst()[State_Type].getBC(first_spec);
    ViscBndry*     bndry   = new ViscBndry(grids,1,geom);
    ABecLaplacian* visc_op = new ABecLaplacian(bndry,dx);

    visc_op->maxOrder(diffusion->maxOrder());

    MultiFab rho_and_species_crse;
    const int nGrowCrse = InterpBndryData::IBD_max_order_DEF - 1;

    if (level == 0)
    {
      bndry->setBndryValues(Wbar,0,0,1,bc);
    }
    else
    {
      HeatTransfer& coarser = *(HeatTransfer*) &(parent->getLevel(level-1));

      rho_and_species_crse.define(coarser.grids,nspecies+1,nGrowCrse,Fab_allocate);

      for (FillPatchIterator fpi(coarser,rho_and_species_crse,nGrowCrse,time,State_Type,Density,nspecies+1);
	   fpi.isValid();
	   ++fpi)
      {
	FArrayBox& rho_and_spec = rho_and_species_crse[fpi];
	rho_and_spec.copy(fpi(),0,0,nspecies+1);
	tmp.resize(rho_and_spec.box(),1);
	tmp.copy(rho_and_spec,0,0,1);
	tmp.invert(1);
	
	for (int comp = 0; comp < nspecies; ++comp) 
	  rho_and_spec.mult(tmp,0,comp+1,1);
      }

      BoxArray cgrids = grids;
      cgrids.coarsen(crse_ratio);
      BndryRegister crse_br(cgrids,0,1,nGrowCrse,1);
      crse_br.setVal(1.e200);
      MultiFab Wbar_crse(rho_and_species_crse.boxArray(),1,nGrowCrse);
      for (MFIter mfi(rho_and_species_crse); mfi.isValid(); ++mfi)
      {
	const Box& box = rho_and_species_crse[mfi].box();
	getChemSolve().getMwmixGivenY(Wbar_crse[mfi],rho_and_species_crse[mfi],box,1,0);
      }	  
      crse_br.copyFrom(Wbar_crse,nGrowCrse,0,0,1);
      bndry->setBndryValues(crse_br,0,Wbar,0,0,1,crse_ratio,bc);
    }

    visc_op->setScalars(0,1);
    visc_op->bCoefficients(1);
    visc_op->applyBC(Wbar,0,1,0,LinOp::Inhomogeneous_BC);

    delete visc_op;
    
    for (MFIter mfi(Wbar); mfi.isValid(); ++mfi)
    {
      const Box&       vbox = mfi.validbox();
      const FArrayBox& wbar = Wbar[mfi];
      const Real       mult = -1.0;

      for (int d=0; d<BL_SPACEDIM; ++d) 
      {
	const FArrayBox& rhoDe = (*betaWbar[d])[mfi];
	FArrayBox&       fluxfab  = (*SpecDiffusionFluxWbar[d])[mfi];
		  
	for (int ispec=0; ispec<nspecies; ++ispec)
	{
	  FORT_GRADWBAR(vbox.loVect(), vbox.hiVect(),
			wbar.dataPtr(), ARLIM(wbar.loVect()),ARLIM(wbar.hiVect()),
			rhoDe.dataPtr(ispec), ARLIM(rhoDe.loVect()), ARLIM(rhoDe.hiVect()),
			fluxfab.dataPtr(ispec), ARLIM(fluxfab.loVect()), ARLIM(fluxfab.hiVect()),
			area[d][mfi].dataPtr(), ARLIM(area[d][mfi].loVect()), ARLIM(area[d][mfi].hiVect()),
			&dx[d], &d, &mult, &inc);
	}
      }
    }
}
#endif

void
HeatTransfer::differential_spec_diffuse_sync (Real dt,
                                              bool Wbar_corrector,
					      bool last_mac_sync_iter)
{
   BL_PROFILE_REGION_START("R::HT::differential_spec_diffuse_sync()");
   BL_PROFILE("HT::differential_spec_diffuse_sync()");
  
  // Diffuse the species syncs such that sum(SpecDiffSyncFluxes) = 0
  // After exiting, SpecDiffusionFluxnp1 should contain rhoD grad (delta Y)^sync
  // Also, Ssync for species should contain rho^{n+1} * (delta Y)^sync

    if (hack_nospecdiff)
    {
      BoxLib::Error("differential_spec_diffuse_sync: hack_nospecdiff not implemented");
    }

    const Real strt_time = ParallelDescriptor::second();

    if (verbose && ParallelDescriptor::IOProcessor())
        std::cout << "Doing differential sync diffusion ..." << '\n';
    //
    // Do implicit c-n solve for each scalar...but dont reflux.
    // Save the fluxes, coeffs and source term, we need 'em for later
    // Actually, since Ssync multiplied by dt in mac_sync before
    // a call to this routine (I hope...), Ssync is in units of s,
    // not the "usual" ds/dt...lets convert it (divide by dt) so
    // we can use a generic flux adjustment function
    //
    const Real cur_time = state[State_Type].curTime();
    FluxBoxes fb_betanp1(this, nspecies, 0);
    MultiFab **betanp1 = fb_betanp1.get();
    getDiffusivity(betanp1, cur_time, first_spec, 0, nspecies); // species

    MultiFab Rhs(grids,nspecies,0);
    const int spec_Ssync_sComp = first_spec - BL_SPACEDIM;

    //
    // Rhs and Ssync contain the RHS of DayBell:2000 Eq (18)
    // with the additional -Y_m^{n+1,p} * (delta rho)^sync term
    // Copy this into Rhs; we will need this later since we overwrite SSync
    // in the solves.
    //
    MultiFab::Copy(Rhs,Ssync,spec_Ssync_sComp,0,nspecies,0);
    //
    // Some standard settings
    //
    const Array<int> rho_flag(nspecies,2);
    const MultiFab* alpha = 0;

    FluxBoxes fb_fluxSC(this);
    MultiFab** fluxSC = fb_fluxSC.get();

    const MultiFab& RhoHalftime = get_rho_half_time();

    for (int sigma = 0; sigma < nspecies; ++sigma)
    {
        //
        // Here, we use Ssync as a source in units of s, as expected by diffuse_Ssync
        // (i.e., ds/dt ~ d(Ssync)/dt, vs. ds/dt ~ Rhs in diffuse_scalar).  This was
        // apparently done to mimic diffuse_Vsync, which does the same, because the
        // diffused result is an acceleration, not a velocity, req'd by the projection.
        //
	const int ssync_ind = first_spec + sigma - Density;
                    
	// on entry, Ssync = RHS for (delta Ytilde)^sync diffusive solve
	// on exit, Ssync = rho^{n+1} * (delta Ytilde)^sync
	// on exit, fluxSC = rhoD grad (delta Ytilde)^sync
	diffusion->diffuse_Ssync(Ssync,ssync_ind,dt,be_cn_theta,
				 RhoHalftime,rho_flag[sigma],fluxSC,0,
			  	  betanp1,sigma,alpha,0);
	//
	// Pull fluxes into flux array
	// this is the rhoD grad (delta Ytilde)^sync terms in DayBell:2000 Eq (18)
	//
	for (int d=0; d<BL_SPACEDIM; ++d)
	{
	  MultiFab::Copy(*SpecDiffusionFluxnp1[d],*fluxSC[d],0,sigma,1,0);
	}
    }
    fb_betanp1.clear();
    fb_fluxSC.clear();
    //
    // Modify update/fluxes to preserve flux sum = 0
    // (Be sure to pass the "normal" looking Rhs to this generic function)
    //

#ifdef USE_WBAR

    // if in the Wbar corrector, add the grad delta Wbar fluxes
    if (Wbar_corrector)
    {
      for (int d=0; d<BL_SPACEDIM; ++d)
	{
	  for (MFIter mfi(*SpecDiffusionFluxWbar[d]); mfi.isValid(); ++mfi)
	    {
	      const Box& ebox = (*SpecDiffusionFluxWbar[d])[mfi].box();
	      (*SpecDiffusionFluxnp1[d])[mfi].plus((*SpecDiffusionFluxWbar[d])[mfi],ebox,0,0,nspecies);
	    }
	}
    }

#endif

    // need to correct SpecDiffusionFluxnp1 to contain rhoD grad (delta Y)^sync
    adjust_spec_diffusion_fluxes(cur_time);
    //
    // Need to correct Ssync to contain rho^{n+1} * (delta Y)^sync.
    // Do this by setting
    // Ssync = "RHS from diffusion solve" + (dt/2)*div(delta Gamma)
    //
    // Recompute update with adjusted diffusion fluxes
    //
    FArrayBox update, efab[BL_SPACEDIM];

    for (MFIter mfi(Ssync); mfi.isValid(); ++mfi)
    {
	int        iGrid = mfi.index();
	const Box& box   = mfi.validbox();

	// copy corrected (delta gamma) on edges into efab
        for (int d=0; d<BL_SPACEDIM; ++d)
        {
            const Box& ebox = BoxLib::surroundingNodes(box,d);
            efab[d].resize(ebox,nspecies);
            
            efab[d].copy((*SpecDiffusionFluxnp1[d])[mfi],ebox,0,ebox,0,nspecies);
            efab[d].mult(be_cn_theta);
        }

        update.resize(box,nspecies);
	update.setVal(0);

	// is this right? - I'm not sure what the scaling on SpecDiffusionFluxnp1 is
        Real scale = -dt;

	// take divergence of (delta gamma) and put it in update
	// update = scale * div(flux) / vol
	// we want update to contain (dt/2) div (delta gamma)
	FORT_FLUXDIV(box.loVect(), box.hiVect(),
                     update.dataPtr(),  
		     ARLIM(update.loVect()),  ARLIM(update.hiVect()),
                     efab[0].dataPtr(), ARLIM(efab[0].loVect()), ARLIM(efab[0].hiVect()),
                     efab[1].dataPtr(), ARLIM(efab[1].loVect()), ARLIM(efab[1].hiVect()),
#if BL_SPACEDIM == 3
                     efab[2].dataPtr(), ARLIM(efab[2].loVect()), ARLIM(efab[2].hiVect()),
#endif
                     volume[mfi].dataPtr(),  ARLIM(volume[mfi].loVect()),  ARLIM(volume[mfi].hiVect()),
                     &nspecies,&scale);

	// add RHS from diffusion solve
	update.plus(Rhs[iGrid],box,0,0,nspecies);

	// Ssync = "RHS from diffusion solve" + (dt/2) * div (delta gamma)
	Ssync[mfi].copy(update,box,0,box,first_spec-BL_SPACEDIM,nspecies);
    }

    Rhs.clear();
    //
    // Do refluxing AFTER flux adjustment
    //
    if (do_reflux && level > 0 && last_mac_sync_iter)
    {
      for (int d=0; d<BL_SPACEDIM; ++d)
      {
	  getViscFluxReg().FineAdd(*SpecDiffusionFluxnp1[d],d,0,first_spec,nspecies,dt);
      }
    }

    if (verbose)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::differential_spec_diffuse_sync(): lev: " << level << ", time: " << run_time << '\n';
    }

   BL_PROFILE_REGION_STOP("R::HT::differential_spec_diffuse_sync()");
}

void
HeatTransfer::reflux ()
{
    BL_PROFILE("HT::reflux()");
    // no need to reflux if this is the finest level
    if (level == parent->finestLevel()) return;

    const Real strt_time = ParallelDescriptor::second();

    BL_ASSERT(do_reflux);

    //
    // First do refluxing step.
    //
    FluxRegister& fr_adv  = getAdvFluxReg(level+1);
    FluxRegister& fr_visc = getViscFluxReg(level+1);
    const Real    dt_crse = parent->dtLevel(level);
    const Real    scale   = 1.0/dt_crse;
    //
    // It is important, for do_mom_diff == 0, to do the viscous
    //   refluxing first, since this will be divided by rho_half
    //   before the advective refluxing is added.  In the case of
    //   do_mom_diff == 1, both components of the refluxing will
    //   be divided by rho^(n+1) in NavierStokesBase::level_sync.
    //
    // take divergence of diffusive flux registers into cell-centered RHS
    fr_visc.Reflux(Vsync,volume,scale,0,0,BL_SPACEDIM,geom);
    if (do_reflux_visc)
        fr_visc.Reflux(Ssync,volume,scale,BL_SPACEDIM,0,NUM_STATE-BL_SPACEDIM,geom);

    showMF("sdcSync",Ssync,"sdc_Ssync_after_viscReflux",level);

    const MultiFab& RhoHalftime = get_rho_half_time();

    if (do_mom_diff == 0) 
    {
        for (MFIter mfi(Vsync); mfi.isValid(); ++mfi)
        {
            const int i = mfi.index();

            D_TERM(Vsync[mfi].divide(RhoHalftime[mfi],grids[i],0,Xvel,1);,
                   Vsync[mfi].divide(RhoHalftime[mfi],grids[i],0,Yvel,1);,
                   Vsync[mfi].divide(RhoHalftime[mfi],grids[i],0,Zvel,1););
        }
    }

    FArrayBox tmp;

    // for any variables that used non-conservative advective differencing,
    // divide the sync by rhohalf
    for (MFIter mfi(Ssync); mfi.isValid(); ++mfi)
    {
        const int i = mfi.index();

        tmp.resize(grids[i],1);
        tmp.copy(RhoHalftime[mfi],0,0,1);
        tmp.invert(1);

        for (int istate = BL_SPACEDIM; istate < NUM_STATE; istate++)
        {
            if (advectionType[istate] == NonConservative)
            {
                const int sigma = istate -  BL_SPACEDIM;

                Ssync[mfi].mult(tmp,0,sigma,1);
            }
        }
    }

    tmp.clear();

    // take divergence of advective flux registers into cell-centered RHS
    fr_adv.Reflux(Vsync,volume,scale,0,0,BL_SPACEDIM,geom);
    fr_adv.Reflux(Ssync,volume,scale,BL_SPACEDIM,0,NUM_STATE-BL_SPACEDIM,geom);
    showMF("sdcSync",Ssync,"sdc_Ssync_after_advReflux",level);

    BoxArray baf = getLevel(level+1).boxArray();

    baf.coarsen(fine_ratio);
    //
    // This is necessary in order to zero out the contribution to any
    // coarse grid cells which underlie fine grid cells.
    //
    std::vector< std::pair<int,Box> > isects;

    for (MFIter mfi(Vsync); mfi.isValid(); ++mfi)
    {
        baf.intersections(grids[mfi.index()],isects);

        for (int i = 0, N = isects.size(); i < N; i++)
        {
            Vsync[mfi].setVal(0,isects[i].second,0,BL_SPACEDIM);
            Ssync[mfi].setVal(0,isects[i].second,0,NUM_STATE-BL_SPACEDIM);
        }
    }

    showMF("sdcSync",Ssync,"sdc_Ssync_after_zero",level);

    if (verbose > 1)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::Reflux(): lev: " << level << ", time: " << run_time << '\n';
    }
}

void
HeatTransfer::calcViscosity (const Real time,
			     const Real dt,
			     const int  iteration,
			     const int  ncycle)
{
    const TimeLevel whichTime = which_time(State_Type, time);

    BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);

    compute_vel_visc(time, whichTime == AmrOldTime ? viscn_cc : viscnp1_cc);
}

void
HeatTransfer::calcDiffusivity (const Real time)
{
    BL_PROFILE("HT::calcDiffusivity()");

    const TimeLevel whichTime = which_time(State_Type, time);

    BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);

    const int  nGrow           = 1;
    const int  offset          = BL_SPACEDIM + 1; // No diffusion coeff for vels or rho
    MultiFab&  diff            = (whichTime == AmrOldTime) ? (*diffn_cc) : (*diffnp1_cc);
    FArrayBox tmp, bcen;

    // for open chambers, ambient pressure is constant in time
    Real p_amb = p_amb_old;

    if (closed_chamber == 1)
    {
      // for closed chambers, use piecewise-linear interpolation
      // we need level 0 prev and cur_time for closed chamber algorithm
      AmrLevel& amr_lev = parent->getLevel(0);
      StateData& state_data = amr_lev.get_state_data(0);
      const Real lev_0_prevtime = state_data.prevTime();
      const Real lev_0_curtime = state_data.curTime();
      const Real prevtime = state[State_Type].prevTime();
      const Real curtime = state[State_Type].curTime();
      if (whichTime == AmrNewTime)
      {
    	  // get new-time ambient pressure
	  p_amb = (lev_0_curtime-curtime )/(lev_0_curtime-lev_0_prevtime) * p_amb_old +
                  (curtime-lev_0_prevtime)/(lev_0_curtime-lev_0_prevtime) * p_amb_new;
      }
      else
      {
    	  // get old-time ambient pressure
	  p_amb = (lev_0_curtime-prevtime )/(lev_0_curtime-lev_0_prevtime) * p_amb_old +
                  (prevtime-lev_0_prevtime)/(lev_0_curtime-lev_0_prevtime) * p_amb_new;
      }
    }

    for (FillPatchIterator Rho_and_spec_fpi(*this,diff,nGrow,time,State_Type,Density,nspecies+1),
                                   Temp_fpi(*this,diff,nGrow,time,State_Type,Temp,1);
         Rho_and_spec_fpi.isValid() && Temp_fpi.isValid();
         ++Rho_and_spec_fpi, ++Temp_fpi)
    {
        FArrayBox& Tfab = Temp_fpi();
        FArrayBox& RYfab = Rho_and_spec_fpi();
	const Box& gbox = RYfab.box();

	const int  vflag   = false;
	// rhoD + lambda + mu
        const int nc_bcen = nspecies+2; 
        int       dotemp  = 1;
        bcen.resize(gbox,nc_bcen);
        
        FORT_SPECTEMPVISC(gbox.loVect(),gbox.hiVect(),
                          ARLIM(Tfab.loVect()),ARLIM(Tfab.hiVect()),Tfab.dataPtr(),
                          ARLIM(RYfab.loVect()),ARLIM(RYfab.hiVect()),RYfab.dataPtr(1),
                          ARLIM(bcen.loVect()),ARLIM(bcen.hiVect()),bcen.dataPtr(),
                          &nc_bcen, &P1atm_MKS, &dotemp, &vflag, &p_amb);
        
        FArrayBox& Dfab = diff[Rho_and_spec_fpi];

	// beta for Y's
        Dfab.copy(bcen,0,first_spec-offset,nspecies);
	// lambda in Temp slot
        Dfab.copy(bcen,nspecies,Temp-offset,1);

        //
        // Convert from tmp=RhoY_l to Y_l
        //
        tmp.resize(gbox,1);
        tmp.copy(RYfab,0,0,1);
        tmp.invert(1);
	for (int n = 1; n < nspecies+1; n++)
	{
	  RYfab.mult(tmp,0,n,1);
	}

        for (int icomp = RhoH; icomp <= NUM_STATE; icomp++)
        {
            if (icomp == RhoH)
            {
	      // lambda/cp in RhoH slot
	      const int sCompT = 0, sCompY = 1, sCompCp = RhoH-offset;
	      getChemSolve().getCpmixGivenTY(Dfab,Tfab,RYfab,gbox,sCompT,sCompY,sCompCp);
	      Dfab.invert(1,sCompCp,1);
	      Dfab.mult(Dfab,Temp-offset,RhoH-offset,1);
            }
            else if (icomp == Trac || icomp == RhoRT)
            {
	      // fill Trac and RhoRT slot so trac_diff_coef (typically zero)
	      Dfab.setVal(trac_diff_coef, gbox, icomp-offset, 1);
            }
        }
    }
    showMFsub("1D",diff,stripBox,"1D_calcD_visc",level);
}

#ifdef USE_WBAR
void
HeatTransfer::calcDiffusivity_Wbar (const Real time)
{
    BL_PROFILE("HT::calcDiffusivity_Wbar()");
  // diffn_cc or diffnp1_cc contains cell-centered transport coefficients from Y's
  //

    const TimeLevel whichTime = which_time(State_Type, time);

    BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);

    MultiFab& diff       = (whichTime == AmrOldTime) ? (*diffn_cc) : (*diffnp1_cc);
    const int nGrow      = diff.nGrow();

    BL_ASSERT(diffWbar_cc.nGrow() >= nGrow);

    for (FillPatchIterator Rho_and_spec_fpi(*this,diff,nGrow,time,State_Type,Density,nspecies+1);
         Rho_and_spec_fpi.isValid();
         ++Rho_and_spec_fpi)
    {
        const FArrayBox& RD = diff[Rho_and_spec_fpi];
        const FArrayBox& RYfab = Rho_and_spec_fpi();
	FArrayBox& Dfab_Wbar = diffWbar_cc[Rho_and_spec_fpi];
	const Box& gbox = RYfab.box();
        
        FORT_BETA_WBAR(gbox.loVect(),gbox.hiVect(),
		       RD.dataPtr(),ARLIM(RD.loVect()),ARLIM(RD.hiVect()),
		       Dfab_Wbar.dataPtr(),ARLIM(Dfab_Wbar.loVect()),ARLIM(Dfab_Wbar.hiVect()),
		       RYfab.dataPtr(1),ARLIM(RYfab.loVect()),ARLIM(RYfab.hiVect()));
    }
}
#endif

void
HeatTransfer::getViscosity (MultiFab*  beta[BL_SPACEDIM],
                            const Real time)
{
    BL_PROFILE("HT::getViscosity()");
    const TimeLevel whichTime = which_time(State_Type, time);

    BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);

    MultiFab* visc = (whichTime == AmrOldTime) ? viscn_cc : viscnp1_cc;

    for (MFIter viscMfi(*visc); viscMfi.isValid(); ++viscMfi)
    {
        const int i = viscMfi.index();

        for (int dir = 0; dir < BL_SPACEDIM; dir++)
        {
            FPLoc bc_lo = fpi_phys_loc(get_desc_lst()[State_Type].getBC(Density).lo(dir));
            FPLoc bc_hi = fpi_phys_loc(get_desc_lst()[State_Type].getBC(Density).hi(dir));

            center_to_edge_fancy((*visc)[viscMfi],(*beta[dir])[viscMfi],
                                 BoxLib::grow(grids[i],BoxLib::BASISV(dir)), 0, 0, 1,
                                 geom.Domain(), bc_lo, bc_hi);
        }
    }
}

void
HeatTransfer::getDiffusivity (MultiFab*  beta[BL_SPACEDIM],
                              const Real time,
                              const int  state_comp,
                              const int  dst_comp,
                              const int  ncomp)
{
    BL_PROFILE("HT::getDiffusivity()");
    BL_ASSERT(state_comp > Density);

    const TimeLevel whichTime = which_time(State_Type, time);

    BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);

    MultiFab* diff      = (whichTime == AmrOldTime) ? diffn_cc : diffnp1_cc;
    const int offset    = BL_SPACEDIM + 1; // No diffusion coeff for vels or rho
    int       diff_comp = state_comp - offset;

    for (MFIter diffMfi(*diff); diffMfi.isValid(); ++diffMfi)
    {
        const int i = diffMfi.index();

        for (int dir = 0; dir < BL_SPACEDIM; dir++)
        {
            FPLoc bc_lo = fpi_phys_loc(get_desc_lst()[State_Type].getBC(state_comp).lo(dir));
            FPLoc bc_hi = fpi_phys_loc(get_desc_lst()[State_Type].getBC(state_comp).hi(dir));

            center_to_edge_fancy((*diff)[diffMfi],(*beta[dir])[diffMfi],
                                 BoxLib::grow(grids[i],BoxLib::BASISV(dir)), diff_comp, 
                                 dst_comp, ncomp, geom.Domain(), bc_lo, bc_hi);
        }
    }

    if (zeroBndryVisc > 0)
        zeroBoundaryVisc(beta,time,state_comp,dst_comp,ncomp);
}

#ifdef USE_WBAR
void
HeatTransfer::getDiffusivity_Wbar (MultiFab*  beta[BL_SPACEDIM],
				   const Real time)	   
{
    BL_PROFILE("HT::getDiffusivity_Wbar()");
    MultiFab& diff = diffWbar_cc;

    for (MFIter diffMfi(diff); diffMfi.isValid(); ++diffMfi)
    {
        const int i = diffMfi.index();

        for (int dir = 0; dir < BL_SPACEDIM; dir++)
        {
            FPLoc bc_lo = fpi_phys_loc(get_desc_lst()[State_Type].getBC(first_spec).lo(dir));
            FPLoc bc_hi = fpi_phys_loc(get_desc_lst()[State_Type].getBC(first_spec).hi(dir));

            center_to_edge_fancy(diff[diffMfi],(*beta[dir])[diffMfi],
                                 BoxLib::grow(grids[i],BoxLib::BASISV(dir)), 0,
                                 0, nspecies, geom.Domain(), bc_lo, bc_hi);
        }
    }

    if (zeroBndryVisc > 0)
      zeroBoundaryVisc(beta,time,BL_SPACEDIM+1,0,nspecies);
}
#endif

void
HeatTransfer::zeroBoundaryVisc (MultiFab*  beta[BL_SPACEDIM],
                                const Real time,
                                const int  state_comp,
                                const int  dst_comp,
                                const int  ncomp) const
{
    BL_ASSERT(state_comp > Density);

    const int isrz = (int) geom.IsRZ();
    for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
        Box edom = BoxLib::surroundingNodes(geom.Domain(),dir);
        
        for (MFIter mfi(*(beta[dir])); mfi.isValid(); ++mfi)
        {
            FArrayBox& beta_fab = (*(beta[dir]))[mfi];
            const Box& ebox     = BoxLib::surroundingNodes(mfi.validbox(),dir);
            FORT_ZEROVISC(beta_fab.dataPtr(dst_comp),
                          ARLIM(beta_fab.loVect()), ARLIM(beta_fab.hiVect()),
                          ebox.loVect(),  ebox.hiVect(),
                          edom.loVect(),  edom.hiVect(),
                          geom.CellSize(), geom.ProbLo(), phys_bc.vect(),
                          &dir, &isrz, &state_comp, &ncomp);
        }
    }
}

void
HeatTransfer::compute_vel_visc (Real      time,
                                MultiFab* beta)
{
    const int nGrow = beta->nGrow();

    BL_ASSERT(nGrow == 1);
    BL_ASSERT(first_spec == Density+1);

    FArrayBox tmp;

    MultiFab dummy(grids,1,0,Fab_noallocate);

    for (FillPatchIterator Temp_fpi(*this,dummy,nGrow,time,State_Type,Temp,1),
                   Rho_and_spec_fpi(*this,dummy,nGrow,time,State_Type,Density,nspecies+1);
         Rho_and_spec_fpi.isValid() && Temp_fpi.isValid();
         ++Rho_and_spec_fpi, ++Temp_fpi)
    {
        const Box& box          = Rho_and_spec_fpi().box();
        FArrayBox& temp         = Temp_fpi();
        FArrayBox& rho_and_spec = Rho_and_spec_fpi();
        //
        // Convert from RhoY_l to Y_l
        //
        tmp.resize(box,1);
        tmp.copy(rho_and_spec,0,0,1);
        tmp.invert(1);

        for (int n = 1; n < nspecies+1; ++n)
            rho_and_spec.mult(tmp,0,n,1);

        FORT_VELVISC(box.loVect(),box.hiVect(),
                     ARLIM(temp.loVect()),ARLIM(temp.hiVect()),temp.dataPtr(),
                     ARLIM(rho_and_spec.loVect()),ARLIM(rho_and_spec.hiVect()),
                     rho_and_spec.dataPtr(1),
                     ARLIM(tmp.loVect()),ARLIM(tmp.hiVect()),tmp.dataPtr());

        (*beta)[Temp_fpi].copy(tmp,0,0,1);
    }
}

void
HeatTransfer::calc_divu (Real      time,
                         Real      dt,
                         MultiFab& divu)
{
    BL_PROFILE("HT::calc_divu()");
    const int nGrow = 0;

    int       vtCompY=0, vtCompT=0;
    MultiFab  mcViscTerms;

#ifdef USE_WBAR
    MultiFab DWbar_temp(grids,nspecies,nGrowAdvForcing);
#endif

    vtCompT = nspecies + 1;
    vtCompY = 0;
    mcViscTerms.define(grids,nspecies+2,nGrow,Fab_allocate);

    // we don't want to update flux registers due to fluxes in divu computation
    bool do_reflux_hold = do_reflux;
    do_reflux = false;
    
    // DD is computed and stored in divu, but we don't need it and overwrite
    // divu in CALCDIVU.
#ifdef USE_WBAR
    compute_differential_diffusion_terms(mcViscTerms,divu,DWbar_temp,time,dt);
#else
    compute_differential_diffusion_terms(mcViscTerms,divu,time,dt);
#endif

    do_reflux = do_reflux_hold;

    //
    // if we are in the initial projection (time=0, dt=-1), set RhoYdot=0
    // if we are in a divu_iter (time=0, dt>0), use I_R
    // if we are in an init_iter or regular time step (time=dt, dt>0), use instananeous
    MultiFab   RhoYdotTmp;
    MultiFab&  S       = get_data(State_Type,time);
    const bool use_IR  = (time == 0 && dt > 0);

    MultiFab&  RhoYdot = (use_IR) ? get_new_data(RhoYdot_Type) : RhoYdotTmp;

    if (!use_IR)
    {
        if (time == 0)
        {
            // initial projection, set omegadot to zero
            RhoYdot.define(grids,nspecies,0,Fab_allocate);
            RhoYdot.setVal(0);
        }
        else if (dt > 0)
        {
            // init_iter or regular time step, use instantaneous omegadot
            RhoYdot.define(grids,nspecies,0,Fab_allocate);
            compute_instantaneous_reaction_rates(RhoYdot,S,time,nGrow);
        }
        else
        {
            BoxLib::Abort("bad divu_logic - shouldn't be here");
        }
    }

    for (MFIter mfi(S); mfi.isValid(); ++mfi)
    {
        const Box& box = mfi.validbox();            
        FArrayBox& du = divu[mfi];
        const FArrayBox& vtY = mcViscTerms[mfi];
        const FArrayBox& vtT = mcViscTerms[mfi];
        const FArrayBox& rhoY = S[mfi];
        const FArrayBox& rYdot = RhoYdot[mfi];
        const FArrayBox& T = S[mfi];
        FORT_CALCDIVU(box.loVect(), box.hiVect(),
                      du.dataPtr(),             ARLIM(du.loVect()),    ARLIM(du.hiVect()),
                      rYdot.dataPtr(),          ARLIM(rYdot.loVect()), ARLIM(rYdot.hiVect()),
                      vtY.dataPtr(vtCompY),     ARLIM(vtY.loVect()),   ARLIM(vtY.hiVect()),
                      vtT.dataPtr(vtCompT),     ARLIM(vtT.loVect()),   ARLIM(vtT.hiVect()),
                      rhoY.dataPtr(first_spec), ARLIM(rhoY.loVect()),  ARLIM(rhoY.hiVect()),
                      T.dataPtr(Temp),          ARLIM(T.loVect()),     ARLIM(T.hiVect()));
    }
}

//
// Compute the Eulerian Dp/Dt for use in pressure relaxation.
//
void
HeatTransfer::calc_dpdt (Real      time,
                         Real      dt,
                         MultiFab& dpdt,
                         MultiFab* u_mac)
{
  BL_PROFILE("HT::calc_dpdt()");

  Real dpdt_factor;
  FORT_GETDPDT(&dpdt_factor);

  // for open chambers, ambient pressure is constant in time
  Real p_amb = p_amb_old;

  if (closed_chamber == 1)
  {
    // for closed chambers, use piecewise-linear interpolation
    // we need level 0 prev and cur_time for closed chamber algorithm
    AmrLevel& amr_lev = parent->getLevel(0);
    StateData& state_data = amr_lev.get_state_data(0);
    const Real lev_0_prevtime = state_data.prevTime();
    const Real lev_0_curtime = state_data.curTime();

    // use new-time ambient pressure
    p_amb = (lev_0_curtime-time )/(lev_0_curtime-lev_0_prevtime) * p_amb_old +
            (time-lev_0_prevtime)/(lev_0_curtime-lev_0_prevtime) * p_amb_new;
  }
  
  if (dt <= 0.0 || dpdt_factor <= 0)
  {
    dpdt.setVal(0);
    return;
  }

  const int pComp = (have_rhort ? RhoRT : Trac);
  int nGrow = dpdt.nGrow();
  MultiFab Peos(grids,1,nGrow);
  for (FillPatchIterator S_fpi(*this,Peos,nGrow,time,State_Type,pComp,1);
       S_fpi.isValid();
       ++S_fpi)
  {
      Peos[S_fpi].copy(S_fpi());
  }

  for (MFIter mfi(dpdt); mfi.isValid(); ++mfi)
  {
    const Box& vbox = mfi.validbox();

    dpdt[mfi].copy(Peos[mfi],vbox,0,vbox,0,1);
    dpdt[mfi].plus(-p_amb,vbox);
    dpdt[mfi].mult(1.0/dt,vbox);
    dpdt[mfi].divide(Peos[mfi],vbox,0,0,1);
    dpdt[mfi].mult(dpdt_factor,vbox);
  }

  Peos.clear();

  if (nGrow > 0) {
      dpdt.FillBoundary(0,1, geom.periodicity());
      BL_ASSERT(dpdt.nGrow() == 1);
      Extrapolater::FirstOrderExtrap(dpdt, geom, 0, 1);
  }
}

//
// Function to use if Divu_Type and Dsdt_Type are in the state.
//

void
HeatTransfer::calc_dsdt (Real      time,
                         Real      dt,
                         MultiFab& dsdt)
{
    dsdt.setVal(0);
}


void
HeatTransfer::RhoH_to_Temp (MultiFab& S,
                            int       nGrow,
                            int       dominmax)
{
    BL_PROFILE("HT::RhoH_to_Temp()");
    const Real strt_time = ParallelDescriptor::second();
    //
    // If this hasn't been set yet, we cannnot do it correct here (scan multilevel),
    // just wing it.
    //
    const Real htt_hmixTYP_SAVE = htt_hmixTYP; 
    if (htt_hmixTYP <= 0)
    {
        if (typical_values[RhoH]==typical_RhoH_value_default)
	{
            htt_hmixTYP = S.norm0(RhoH);
	}
        else
	{
            htt_hmixTYP = typical_values[RhoH];
	}        
        if (verbose && ParallelDescriptor::IOProcessor())
            std::cout << "setting htt_hmixTYP = " << htt_hmixTYP << '\n';
    }

    int max_iters = 0;
    for (MFIter mfi(S); mfi.isValid(); ++mfi)
    {
        const Box& box = BoxLib::grow(mfi.validbox(),nGrow);
        max_iters = std::max(max_iters, RhoH_to_Temp(S[mfi],box,dominmax));
    }

    if (verbose > 1)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceIntMax(max_iters,IOProc);
        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (verbose && ParallelDescriptor::IOProcessor())
            std::cout << "HeatTransfer::RhoH_to_Temp: max_iters = " << max_iters << ", time: " << run_time << '\n';
    }
    //
    // Reset it back.
    //
    htt_hmixTYP = htt_hmixTYP_SAVE;
}


static
int RhoH_to_Temp_DoIt(FArrayBox&       Tfab,
                    const FArrayBox& Hfab,
                    const FArrayBox& Yfab,
                    const Box&       box,
                    int              sCompH,
                    int              sCompY,
                    int              dCompT,
    	            const Real&      htt_hmixTYP,
                    ChemDriver&      cd)
{
    const Real eps = cd.getHtoTerrMAX();
    Real errMAX = eps*htt_hmixTYP;

    int iters = cd.getTGivenHY(Tfab,Hfab,Yfab,box,sCompH,sCompY,dCompT,errMAX);

    if (iters < 0)
        BoxLib::Error("HeatTransfer::RhoH_to_Temp(fab): error in H->T");

    return iters;
}


int
HeatTransfer::RhoH_to_Temp (FArrayBox& S,
                            const Box& box,
                            int        dominmax)
{
    BL_ASSERT(S.box().contains(box));

    // temporary storage for 1/rho, H, and Y
    FArrayBox rhoInv, H, Y;
    rhoInv.resize(box,1);
    H.resize(box,1);
    Y.resize(box,nspecies);

    // copy in rho, rhoH, and rhoY
    rhoInv.copy(S,box,Density,   box,0,1);
    H.copy     (S,box,RhoH,      box,0,1);
    Y.copy     (S,box,first_spec,box,0,nspecies);

    // invert rho, then multiply 1/rho by rhoH and rhoY to get H and Y
    rhoInv.invert(1,box,0,1);
    H.mult(rhoInv,box,0,0,1);
    for (int i=0; i<nspecies; i++)
    {
      Y.mult(rhoInv,box,0,i,1);
    }    

    // we index into Temperature component in S.  H and Y begin with the 0th component
    int iters = RhoH_to_Temp_DoIt(S,H,Y,box,0,0,Temp,htt_hmixTYP,getChemSolve());

    if (dominmax)
        FabMinMax(S, box, htt_tempmin, htt_tempmax, Temp, 1);    

    return iters;
}

void
HeatTransfer::setPlotVariables ()
{
    AmrLevel::setPlotVariables();

    const Array<std::string>& names = getChemSolve().speciesNames();

    ParmParse pp("ht");

    bool plot_rhoY,plot_massFrac,plot_moleFrac,plot_conc;
    plot_rhoY=plot_massFrac=plot_moleFrac=plot_conc = false;

    if (pp.query("plot_massfrac",plot_massFrac))
    {
        if (plot_massFrac)
        {
            for (int i = 0; i < names.size(); i++)
            {
                const std::string name = "Y("+names[i]+")";
                parent->addDerivePlotVar(name);
            }
        }
        else
        {
            for (int i = 0; i < names.size(); i++)
            {
                const std::string name = "Y("+names[i]+")";
                parent->deleteDerivePlotVar(name);
            }
        }
    }

    if (pp.query("plot_molefrac",plot_moleFrac))
    {
        if (plot_moleFrac)
            parent->addDerivePlotVar("molefrac");
        else
            parent->deleteDerivePlotVar("molefrac");
    }

    if (pp.query("plot_concentration",plot_conc))
    {
        if (plot_conc)
            parent->addDerivePlotVar("concentration");
        else
            parent->deleteDerivePlotVar("concentration");
    }

    if (pp.query("plot_rhoY",plot_rhoY))
    {
        if (plot_rhoY)
        {
            for (int i = 0; i < names.size(); i++)
            {
                const std::string name = "rho.Y("+names[i]+")";
                parent->addStatePlotVar(name);
            }
        }
        else
        {
            for (int i = 0; i < names.size(); i++)
            {
                const std::string name = "rho.Y("+names[i]+")";
                parent->deleteStatePlotVar(name);
            }
        }
    }

    if (verbose && ParallelDescriptor::IOProcessor())
    {
        std::cout << "\nState Plot Vars: ";

        std::list<std::string>::const_iterator li = parent->statePlotVars().begin(), end = parent->statePlotVars().end();

        for ( ; li != end; ++li)
            std::cout << *li << ' ';
        std::cout << '\n';

        std::cout << "\nDerive Plot Vars: ";

        li  = parent->derivePlotVars().begin();
        end = parent->derivePlotVars().end();

        for ( ; li != end; ++li)
            std::cout << *li << ' ';
        std::cout << '\n';
    }
}

void
HeatTransfer::writePlotFile (const std::string& dir,
                             std::ostream&  os,
                             VisMF::How     how)
{
    if ( ! Amr::Plot_Files_Output() ) return;

    ParmParse pp("ht");

    pp.query("plot_rhoydot",plot_rhoydot);
    //
    // Note that this is really the same as its NavierStokes counterpart,
    // but in order to add diagnostic MultiFabs into the plotfile, code had
    // to be interspersed within this function.
    //
    int i, n;
    //
    // The list of indices of State to write to plotfile.
    // first component of pair is state_type,
    // second component of pair is component # within the state_type
    //
    std::vector<std::pair<int,int> > plot_var_map;
    for (int typ = 0; typ < desc_lst.size(); typ++)
    {
        for (int comp = 0; comp < desc_lst[typ].nComp();comp++)
        {
            const std::string& name = desc_lst[typ].name(comp);

            if (parent->isStatePlotVar(name) && desc_lst[typ].getType() == IndexType::TheCellType())
            {
                //
                // When running SDC we get things of this form in the State.
                // I want a simple way not to write'm out to plotfiles.
                //
                if (name.find("I_R[") != std::string::npos)
                {
                    if (plot_rhoydot)
                        plot_var_map.push_back(std::pair<int,int>(typ,comp));
                }
                else
                {
                    plot_var_map.push_back(std::pair<int,int>(typ,comp));
                }
            }
        }
    }

    int num_derive = 0;
    std::list<std::string> derive_names;
    const std::list<DeriveRec>& dlist = derive_lst.dlist();

    for (std::list<DeriveRec>::const_iterator it = dlist.begin(), end = dlist.end();
         it != end;
         ++it)
    {
        if (parent->isDerivePlotVar(it->name()))
	{
            derive_names.push_back(it->name());
            num_derive += it->numDerive();
	}
    }

    int num_auxDiag = 0;
    for (std::map<std::string,MultiFab*>::const_iterator it = auxDiag.begin(), end = auxDiag.end();
         it != end;
         ++it)
    {
        num_auxDiag += it->second->nComp();
    }

    int n_data_items = plot_var_map.size() + num_derive + num_auxDiag;
    Real cur_time = state[State_Type].curTime();

    if (level == 0 && ParallelDescriptor::IOProcessor())
    {
        //
        // The first thing we write out is the plotfile type.
        //
        os << thePlotFileType() << '\n';

        if (n_data_items == 0)
            BoxLib::Error("Must specify at least one valid data item to plot");

        os << n_data_items << '\n';

	//
	// Names of variables -- first state, then derived
	//
	for (i =0; i < plot_var_map.size(); i++)
        {
	    int typ  = plot_var_map[i].first;
	    int comp = plot_var_map[i].second;
	    os << desc_lst[typ].name(comp) << '\n';
        }

	for (std::list<std::string>::const_iterator it = derive_names.begin(), end = derive_names.end();
             it != end;
             ++it)
        {
	    const DeriveRec* rec = derive_lst.get(*it);
	    for (i = 0; i < rec->numDerive(); i++)
                os << rec->variableName(i) << '\n';
        }
        //
        // Hack in additional diagnostics.
        //
        for (std::map<std::string,Array<std::string> >::const_iterator it = auxDiag_names.begin(), end = auxDiag_names.end();
             it != end;
             ++it)
        {
            for (i=0; i<it->second.size(); ++i)
                os << it->second[i] << '\n';
        }

        os << BL_SPACEDIM << '\n';
        os << parent->cumTime() << '\n';
        int f_lev = parent->finestLevel();
        os << f_lev << '\n';
        for (i = 0; i < BL_SPACEDIM; i++)
            os << Geometry::ProbLo(i) << ' ';
        os << '\n';
        for (i = 0; i < BL_SPACEDIM; i++)
            os << Geometry::ProbHi(i) << ' ';
        os << '\n';
        for (i = 0; i < f_lev; i++)
            os << parent->refRatio(i)[0] << ' ';
        os << '\n';
        for (i = 0; i <= f_lev; i++)
            os << parent->Geom(i).Domain() << ' ';
        os << '\n';
        for (i = 0; i <= f_lev; i++)
            os << parent->levelSteps(i) << ' ';
        os << '\n';
        for (i = 0; i <= f_lev; i++)
        {
            for (int k = 0; k < BL_SPACEDIM; k++)
                os << parent->Geom(i).CellSize()[k] << ' ';
            os << '\n';
        }
        os << (int) Geometry::Coord() << '\n';
        os << "0\n"; // Write bndry data.


        // job_info file with details about the run
	std::ofstream jobInfoFile;
	std::string FullPathJobInfoFile = dir;
	std::string PrettyLine = "===============================================================================\n";

	FullPathJobInfoFile += "/job_info";
	jobInfoFile.open(FullPathJobInfoFile.c_str(), std::ios::out);

	// job information
	jobInfoFile << PrettyLine;
	jobInfoFile << " Job Information\n";
	jobInfoFile << PrettyLine;
	
	jobInfoFile << "number of MPI processes: " << ParallelDescriptor::NProcs() << "\n";
#ifdef _OPENMP
	jobInfoFile << "number of threads:       " << omp_get_max_threads() << "\n";
#endif
	jobInfoFile << "\n\n";

        // plotfile information
	jobInfoFile << PrettyLine;
	jobInfoFile << " Plotfile Information\n";
	jobInfoFile << PrettyLine;

	time_t now = time(0);

	// Convert now to tm struct for local timezone
	tm* localtm = localtime(&now);
	jobInfoFile   << "output data / time: " << asctime(localtm);

	char currentDir[FILENAME_MAX];
	if (getcwd(currentDir, FILENAME_MAX)) {
	  jobInfoFile << "output dir:         " << currentDir << "\n";
	}

	jobInfoFile << "\n\n";


        // build information
	jobInfoFile << PrettyLine;
	jobInfoFile << " Build Information\n";
	jobInfoFile << PrettyLine;

	jobInfoFile << "build date:    " << buildInfoGetBuildDate() << "\n";
	jobInfoFile << "build machine: " << buildInfoGetBuildMachine() << "\n";
	jobInfoFile << "build dir:     " << buildInfoGetBuildDir() << "\n";
	jobInfoFile << "BoxLib dir:    " << buildInfoGetBoxlibDir() << "\n";

	jobInfoFile << "\n";

	jobInfoFile << "COMP:          " << buildInfoGetComp() << "\n";
	jobInfoFile << "COMP version:  " << buildInfoGetCompVersion() << "\n";
	jobInfoFile << "FCOMP:         " << buildInfoGetFcomp() << "\n";
	jobInfoFile << "FCOMP version: " << buildInfoGetFcompVersion() << "\n";

	jobInfoFile << "\n";

	for (int n = 1; n <= buildInfoGetNumModules(); n++) {
	    jobInfoFile << buildInfoGetModuleName(n) << ": " << buildInfoGetModuleVal(n) << "\n";
	}

	jobInfoFile << "\n";

	const char* githash1 = buildInfoGetGitHash(1);
	const char* githash2 = buildInfoGetGitHash(2);
	const char* githash3 = buildInfoGetGitHash(3);
	if (strlen(githash1) > 0) {
	  jobInfoFile << "LMC    git hash: " << githash1 << "\n";
	}
	if (strlen(githash2) > 0) {
	  jobInfoFile << "BoxLib git hash: " << githash2 << "\n";
	}
	if (strlen(githash3) > 0) {
	  jobInfoFile << "IAMR   git hash: " << githash3 << "\n";
	}

	jobInfoFile << "\n\n";


	// runtime parameters
	jobInfoFile << PrettyLine;
	jobInfoFile << " Inputs File Parameters\n";
	jobInfoFile << PrettyLine;
	
	ParmParse::dumpTable(jobInfoFile, true);

	jobInfoFile.close();	

    }
    // Build the directory to hold the MultiFab at this level.
    // The name is relative to the directory containing the Header file.
    //
    static const std::string BaseName = "/Cell";

    std::string Level = BoxLib::Concatenate("Level_", level, 1);
    //
    // Now for the full pathname of that directory.
    //
    std::string FullPath = dir;
    if (!FullPath.empty() && FullPath[FullPath.length()-1] != '/')
        FullPath += '/';
    FullPath += Level;
    //
    // Only the I/O processor makes the directory if it doesn't already exist.
    //
    if (ParallelDescriptor::IOProcessor())
        if (!BoxLib::UtilCreateDirectory(FullPath, 0755))
            BoxLib::CreateDirectoryFailed(FullPath);
    //
    // Force other processors to wait till directory is built.
    //
    ParallelDescriptor::Barrier();

    if (ParallelDescriptor::IOProcessor())
    {
        os << level << ' ' << grids.size() << ' ' << cur_time << '\n';
        os << parent->levelSteps(level) << '\n';

        for (i = 0; i < grids.size(); ++i)
        {
            RealBox gridloc = RealBox(grids[i],geom.CellSize(),geom.ProbLo());
            for (n = 0; n < BL_SPACEDIM; n++)
                os << gridloc.lo(n) << ' ' << gridloc.hi(n) << '\n';
        }
        //
        // The full relative pathname of the MultiFabs at this level.
        // The name is relative to the Header file containing this name.
        // It's the name that gets written into the Header.
        //
        if (n_data_items > 0)
        {
            std::string PathNameInHeader = Level;
            PathNameInHeader += BaseName;
            os << PathNameInHeader << '\n';
        }
    }
    //
    // We combine all of the multifabs -- state, derived, etc -- into one
    // multifab -- plotMF.
    // NOTE: we are assuming that each state variable has one component,
    // but a derived variable is allowed to have multiple components.
    int       cnt   = 0;
    int       ncomp = 1;
    const int nGrow = 0;
    MultiFab  plotMF(grids,n_data_items,nGrow);
    MultiFab* this_dat = 0;
    //
    // Cull data from state variables -- use no ghost cells.
    //
    for (i = 0; i < plot_var_map.size(); i++)
    {
	int typ  = plot_var_map[i].first;
	int comp = plot_var_map[i].second;
	this_dat = &state[typ].newData();
	MultiFab::Copy(plotMF,*this_dat,comp,cnt,ncomp,nGrow);
	cnt+= ncomp;
    }
    //
    // Cull data from derived variables.
    // 
    Real plot_time;

    if (derive_names.size() > 0)
    {
	for (std::list<std::string>::const_iterator it = derive_names.begin(), end = derive_names.end();
             it != end;
             ++it) 
	{
            if (*it == "avg_pressure" ||
                *it == "gradpx"       ||
                *it == "gradpy"       ||
                *it == "gradpz") 
            {
                if (state[Press_Type].descriptor()->timeType() == 
                    StateDescriptor::Interval) 
                {
                    plot_time = cur_time;
                }
                else
                {
                    int f_lev = parent->finestLevel();
                    plot_time = getLevel(f_lev).state[Press_Type].curTime();
                }
            }
            else
            {
                plot_time = cur_time;
            } 
	    const DeriveRec* rec = derive_lst.get(*it);
	    ncomp = rec->numDerive();
	    MultiFab* derive_dat = derive(*it,plot_time,nGrow);
	    MultiFab::Copy(plotMF,*derive_dat,0,cnt,ncomp,nGrow);
	    delete derive_dat;
	    cnt += ncomp;
	}
    }
    //
    // Cull data from diagnostic multifabs.
    //
    for (std::map<std::string,MultiFab*>::const_iterator it = auxDiag.begin(), end = auxDiag.end();
         it != end;
         ++it)
    {
        int nComp = it->second->nComp();
        MultiFab::Copy(plotMF,*it->second,0,cnt,nComp,nGrow);
        cnt += nComp;
    }
    //
    // Use the Full pathname when naming the MultiFab.
    //
    std::string TheFullPath = FullPath;
    TheFullPath += BaseName;
    VisMF::Write(plotMF,TheFullPath,how);
}

MultiFab*
HeatTransfer::derive (const std::string& name,
                      Real               time,
                      int                ngrow)
{        
  BL_ASSERT(ngrow >= 0);
  
  MultiFab* mf = 0;
  const DeriveRec* rec = derive_lst.get(name);
  if (rec)
  {
    BoxArray dstBA(grids);
    mf = new MultiFab(dstBA, rec->numDerive(), ngrow);
    int dcomp = 0;
    derive(name,time,*mf,dcomp);
  }
  else
  {
    mf = AmrLevel::derive(name,time,ngrow);
  }

  if (mf==0) {
    std::string msg("HeatTransfer::derive(): unknown variable: ");
    msg += name;
    BoxLib::Error(msg.c_str());
  }
  return mf;
}
 
void
HeatTransfer::derive (const std::string& name,
                      Real               time,
                      MultiFab&          mf,
                      int                dcomp)
{
    if (name == "mean_progress_curvature")
    {
        // 
        // Smooth the temperature, then use smoothed T to compute mean progress curvature
        //

        // Assert because we do not know how to un-convert the destination
        //   and also, implicitly assume the convert that was used to build mf BA is trivial
        BL_ASSERT(mf.boxArray()[0].ixType()==IndexType::TheCellType());

        const DeriveRec* rec = derive_lst.get(name);

        Box srcB = mf.boxArray()[0];
        Box dstB = rec->boxMap()(srcB);

        // Find nGrowSRC
        int nGrowSRC = 0;
        for ( ; !srcB.contains(dstB); ++nGrowSRC)
        {
            srcB.grow(1);
        }
        BL_ASSERT(nGrowSRC);  // Need grow cells for this to work!

        MultiFab tmf(mf.boxArray(),1,nGrowSRC);
        
        for (FillPatchIterator fpi(*this,tmf,nGrowSRC,time,State_Type,Temp,1);
             fpi.isValid();
             ++fpi)
        {
            tmf[fpi].copy(fpi());
        }

        int num_smooth_pre = 3;
        
        for (int i=0; i<num_smooth_pre; ++i)
        {
            // Fix up fine-fine and periodic
            tmf.FillBoundary(0,1,geom.periodicity());
                        
            for (MFIter mfi(tmf); mfi.isValid(); ++mfi)
            {
                // 
                // Use result MultiFab for temporary container to hold smooth T field
                // 
                const Box& box = mfi.validbox();
                FORT_SMOOTH(box.loVect(),box.hiVect(),
                            tmf[mfi].dataPtr(),
                            ARLIM(tmf[mfi].loVect()),ARLIM(tmf[mfi].hiVect()),
                            mf[mfi].dataPtr(dcomp),
                            ARLIM(mf[mfi].loVect()),ARLIM(mf[mfi].hiVect()));

                // Set result back into slot for smoothed T, leave grow cells
                tmf[mfi].copy(mf[mfi],box,dcomp,box,0,1);
            }
        }

        tmf.FillBoundary(0,1,geom.periodicity());

        const Real* dx = geom.CellSize();
        
        FArrayBox nWork;
        for (MFIter mfi(mf); mfi.isValid(); ++mfi)
        {
            const FArrayBox& Tg = tmf[mfi];
            FArrayBox& MC = mf[mfi];
            const Box& box = mfi.validbox();
            const Box& nodebox = BoxLib::surroundingNodes(box);
            nWork.resize(nodebox,BL_SPACEDIM);
            
            FORT_MCURVE(box.loVect(),box.hiVect(),
                        Tg.dataPtr(),ARLIM(Tg.loVect()),ARLIM(Tg.hiVect()),
                        MC.dataPtr(dcomp),ARLIM(MC.loVect()),ARLIM(MC.hiVect()),
                        nWork.dataPtr(),ARLIM(nWork.loVect()),ARLIM(nWork.hiVect()),
                        dx);
        }
    } 
    else
    {
#ifdef PARTICLES
        ParticleDerive(name,time,mf,dcomp);
#else
        AmrLevel::derive(name,time,mf,dcomp);
#endif
    }
}
