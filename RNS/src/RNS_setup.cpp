#include <winstd.H>

#include "LevelBld.H"

#include "RNS.H"
#include "RNS_F.H"
#include "Derive_F.H"
#include "ChemDriver.H"

using std::string;

static Box the_same_box (const Box& b) { return b; }
static Box grow_box_by_one (const Box& b) { return BoxLib::grow(b,1); }

typedef StateDescriptor::BndryFunc BndryFunc;

//
// Components are:
//  Interior, Inflow, Outflow,  Symmetry,     SlipWall,     NoSlipWall
//
static int scalar_bc[] =
{
    INT_DIR, EXT_DIR, FOEXTRAP, REFLECT_EVEN, REFLECT_EVEN, REFLECT_EVEN
};

static int norm_vel_bc[] =
{
    INT_DIR, EXT_DIR, FOEXTRAP, REFLECT_ODD,  REFLECT_ODD,  REFLECT_ODD
};

static int tang_vel_bc[] =
{
    INT_DIR, EXT_DIR, FOEXTRAP, REFLECT_EVEN, REFLECT_EVEN, REFLECT_ODD
};

static
void
set_scalar_bc (BCRec& bc, const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();
    for (int i = 0; i < BL_SPACEDIM; i++)
    {
        bc.setLo(i,scalar_bc[lo_bc[i]]);
        bc.setHi(i,scalar_bc[hi_bc[i]]);
    }
}

static
void
set_x_vel_bc(BCRec& bc, const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();
    bc.setLo(0,norm_vel_bc[lo_bc[0]]);
    bc.setHi(0,norm_vel_bc[hi_bc[0]]);
#if (BL_SPACEDIM >= 2)
    bc.setLo(1,tang_vel_bc[lo_bc[1]]);
    bc.setHi(1,tang_vel_bc[hi_bc[1]]);
#endif
#if (BL_SPACEDIM == 3)
    bc.setLo(2,tang_vel_bc[lo_bc[2]]);
    bc.setHi(2,tang_vel_bc[hi_bc[2]]);
#endif
}

#if (BL_SPACEDIM >= 2)
static
void
set_y_vel_bc(BCRec& bc, const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();
    bc.setLo(0,tang_vel_bc[lo_bc[0]]);
    bc.setHi(0,tang_vel_bc[hi_bc[0]]);
    bc.setLo(1,norm_vel_bc[lo_bc[1]]);
    bc.setHi(1,norm_vel_bc[hi_bc[1]]);
#if (BL_SPACEDIM == 3)
    bc.setLo(2,tang_vel_bc[lo_bc[2]]);
    bc.setHi(2,tang_vel_bc[hi_bc[2]]);
#endif
}
#endif

#if (BL_SPACEDIM == 3)
static
void
set_z_vel_bc(BCRec& bc, const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();
    bc.setLo(0,tang_vel_bc[lo_bc[0]]);
    bc.setHi(0,tang_vel_bc[hi_bc[0]]);
    bc.setLo(1,tang_vel_bc[lo_bc[1]]);
    bc.setHi(1,tang_vel_bc[hi_bc[1]]);
    bc.setLo(2,norm_vel_bc[lo_bc[2]]);
    bc.setHi(2,norm_vel_bc[hi_bc[2]]);
}
#endif

void
RNS::variableSetUp ()
{
    BL_ASSERT(desc_lst.size() == 0);

    // initialize the start time for our CPU-time tracker
    startCPUTime = ParallelDescriptor::second();

    // Get options, set phys_bc
    read_params();

    if (chemSolve == 0) 
    {
	int max_points;
	if ( use_vode || chem_solver != GAUSS_BURNING ) 
	{
	    max_points = 1;
	}
	else 
	{
#if (BL_SPACEDIM == 1)
	    max_points = 2;
#elif (BL_SPACEDIM == 2)
	    max_points = 4;
#else
	    max_points = 8;
#endif
	}
	chemSolve = new ChemDriver(use_vode, max_points);
    }

    //
    // Set number of state variables and pointers to components
    //
    int cnt = 0;
    Density = cnt++;
    Xmom = cnt++;
#if (BL_SPACEDIM >= 2)
    Ymom = cnt++;
#endif
#if (BL_SPACEDIM == 3)
    Zmom = cnt++;
#endif
    Eden = cnt++;
    Temp = cnt++;
    
    NumSpec = chemSolve->numSpecies();
    FirstSpec = cnt++;
    cnt += NumSpec - 2;
    LastSpec = cnt++;
    
    NUM_STATE = cnt;

    fuelID      = chemSolve->index(     fuelName);
    oxidizerID  = chemSolve->index( oxidizerName);
    productID   = chemSolve->index(  productName);
    flameTracID = chemSolve->index(flameTracName);

    if (fuelID < 0)
    {
	plot_fuelConsumption = 0;
    }

    if (ChemDriver::isNull())
    {
	plot_X = plot_omegadot = plot_dYdt = plot_heatRelease = plot_fuelConsumption = 0;
    }

    int nriemann_F, nchemsolver_F;
    // Define NUM_GROW from the f90 module.
    BL_FORT_PROC_CALL(GET_METHOD_PARAMS, get_method_params)(&NUM_GROW, &nriemann_F, &nchemsolver_F);
    if (nriemann_F != NUM_RIEMANN_TYPE)
    {
	BoxLib::Abort("Something is wrong with RiemannType");
    }
    if (nchemsolver_F != NUM_CHEMSOLVER_TYPE)
    {
	BoxLib::Abort("Something is wrong with ChemSolverType");
    }

    int dm = BL_SPACEDIM;    
    int riemann = RNS::Riemann;
    int chem_solver_i = RNS::chem_solver;

    BL_FORT_PROC_CALL(SET_METHOD_PARAMS, set_method_params)
	(dm, Density, Xmom, Eden, Temp, FirstSpec, NUM_STATE, NumSpec, 
	 small_dens, small_temp, small_pres, gamma, gravity_dir, gravity, Treference,
	 riemann, difmag, HLL_factor, &blocksize[0], 
	 do_weno, do_mdcd_weno, weno_p, weno_eps, weno_gauss_phi,
	 use_vode, new_J_cell, chem_solver_i, chem_do_weno);
    
    int coord_type = Geometry::Coord();
    const Real* prob_lo   = Geometry::ProbLo();
    const Real* prob_hi   = Geometry::ProbHi();
    
    BL_FORT_PROC_CALL(SET_PROBLEM_PARAMS, set_problem_params)
	(dm,phys_bc.lo(),phys_bc.hi(),prob_lo,prob_hi,Outflow,Symmetry,SlipWall,NoSlipWall,coord_type);
    
    Interpolater* interp;
    if (do_quartic_interp)
    {
	interp = &quartic_interp;
    }
    else
    {
	interp = &cell_cons_interp;
    }
    
    // Note that the default is state_data_extrap = false, store_in_checkpoint = true
    // We only need to put these explicitly if we want to do something different,
    // like not store the state data in a checkpoint directory
    bool state_data_extrap = false;
    bool store_in_checkpoint;
    
    store_in_checkpoint = true;
    desc_lst.addDescriptor(State_Type,IndexType::TheCellType(),
			   StateDescriptor::Point,NUM_GROW,NUM_STATE,
			   interp,state_data_extrap,store_in_checkpoint);
    
    Array<BCRec>       bcs(NUM_STATE);
    Array<std::string> name(NUM_STATE);
    
    BCRec bc;
    cnt = 0;
    set_scalar_bc(bc,phys_bc); bcs[cnt] = bc; name[cnt] = "density";
    cnt++; set_x_vel_bc(bc,phys_bc);  bcs[cnt] = bc; name[cnt] = "xmom";
#if (BL_SPACEDIM >= 2)
    cnt++; set_y_vel_bc(bc,phys_bc);  bcs[cnt] = bc; name[cnt] = "ymom";
#endif
#if (BL_SPACEDIM == 3)
    cnt++; set_z_vel_bc(bc,phys_bc);  bcs[cnt] = bc; name[cnt] = "zmom";
#endif
    cnt++; set_scalar_bc(bc,phys_bc); bcs[cnt] = bc; name[cnt] = "rho_E";
    cnt++; set_scalar_bc(bc,phys_bc); bcs[cnt] = bc; name[cnt] = "Temp";
    
    // Get the species names from the chemdriver.
    const Array<std::string>& spec_names = chemSolve->speciesNames();
    for (int i=0; i<NumSpec; i++)
    {
	cnt++; set_scalar_bc(bc,phys_bc); bcs[cnt] = bc;
	name[cnt] = "rho.Y(" + spec_names[i] + ")";
    }
	
    desc_lst.setComponent(State_Type,
			  Density,
			  name,
			  bcs,
			  BndryFunc(BL_FORT_PROC_CALL(RNS_REGFILL,rns_regfill),
				    BL_FORT_PROC_CALL(RNS_GRPFILL,rns_grpfill)));

    desc_lst.resetComponentBCs(State_Type,
			       Density,
			       bcs[Density],
			       BndryFunc(BL_FORT_PROC_CALL(RNS_DENFILL,rns_denfill),
					 BL_FORT_PROC_CALL(RNS_GRPFILL,rns_grpfill)));
    desc_lst.resetComponentBCs(State_Type,
			       Xmom,
			       bcs[Xmom],
			       BndryFunc(BL_FORT_PROC_CALL(RNS_MXFILL,rns_mxfill),
					 BL_FORT_PROC_CALL(RNS_GRPFILL,rns_grpfill)));
#if (BL_SPACEDIM >= 2)
    desc_lst.resetComponentBCs(State_Type,
			       Ymom,
			       bcs[Ymom],
			       BndryFunc(BL_FORT_PROC_CALL(RNS_MYFILL,rns_myfill),
					 BL_FORT_PROC_CALL(RNS_GRPFILL,rns_grpfill)));
#endif
#if (BL_SPACEDIM == 3)
    desc_lst.resetComponentBCs(State_Type,
			       Zmom,
			       bcs[Zmom],
			       BndryFunc(BL_FORT_PROC_CALL(RNS_MZFILL,rns_mzfill),
					 BL_FORT_PROC_CALL(RNS_GRPFILL,rns_grpfill)));
#endif

    desc_lst.resetComponentBCs(State_Type,
			       Temp,
			       bcs[Temp],
			       BndryFunc(BL_FORT_PROC_CALL(RNS_TEMPFILL,rns_tempfill),
					 BL_FORT_PROC_CALL(RNS_GRPFILL,rns_grpfill)));
			           
    //
    // DEFINE DERIVED QUANTITIES
    //
    // Pressure
    //
    derive_lst.add("pressure",IndexType::TheCellType(),1,
		   BL_FORT_PROC_CALL(RNS_DERPRES,rns_derpres),the_same_box);
    derive_lst.addComponent("pressure",desc_lst,State_Type,Density,NUM_STATE);
  
    //
    // Sound speed (c)
    //
    derive_lst.add("soundspeed",IndexType::TheCellType(),1,
		   BL_FORT_PROC_CALL(RNS_DERSOUNDSPEED,rns_dersoundspeed),the_same_box);
    derive_lst.addComponent("soundspeed",desc_lst,State_Type,Density,NUM_STATE);
    
    //
    // Mach number(M)
    //
    derive_lst.add("MachNumber",IndexType::TheCellType(),1,
		   BL_FORT_PROC_CALL(RNS_DERMACHNUMBER,rns_dermachnumber),the_same_box);
    derive_lst.addComponent("MachNumber",desc_lst,State_Type,Density,NUM_STATE);

#if (BL_SPACEDIM > 1)
    //
    // Vorticity
    //
    derive_lst.add("magvort",IndexType::TheCellType(),1,
		   BL_FORT_PROC_CALL(RNS_DERMAGVORT,rns_dermagvort),grow_box_by_one);
    derive_lst.addComponent("magvort",desc_lst,State_Type,Density,1);
    derive_lst.addComponent("magvort",desc_lst,State_Type,Xmom,BL_SPACEDIM);
#endif
  
    //
    // Div(u)
    //
    derive_lst.add("divu",IndexType::TheCellType(),1,
		   BL_FORT_PROC_CALL(RNS_DERDIVU,rns_derdivu),grow_box_by_one);
    derive_lst.addComponent("divu",desc_lst,State_Type,Density,1);
    derive_lst.addComponent("divu",desc_lst,State_Type,Xmom,BL_SPACEDIM);
    
    //
    // Species mass fractions.
    //
    for (int i = 0; i < NumSpec; i++)
    {
	const std::string name = "Y("+spec_names[i]+")";
	derive_lst.add(name,IndexType::TheCellType(),1,
		       BL_FORT_PROC_CALL(RNS_DERSPEC,rns_derspec),the_same_box);
	derive_lst.addComponent(name,desc_lst,State_Type,Density,1);
	derive_lst.addComponent(name,desc_lst,State_Type,FirstSpec + i,1);
    }

    //
    // Species mole fractions
    //
    Array<std::string> var_names_molefrac(NumSpec);
    for (int i = 0; i < NumSpec; i++)
        var_names_molefrac[i] = "X("+spec_names[i]+")";
    derive_lst.add("molefrac",IndexType::TheCellType(),NumSpec,var_names_molefrac,
		   BL_FORT_PROC_CALL(RNS_DERMOLEFRAC,rns_dermolefrac),the_same_box);
    derive_lst.addComponent("molefrac",desc_lst,State_Type,Density,1);
    derive_lst.addComponent("molefrac",desc_lst,State_Type,FirstSpec,NumSpec);
    
    //
    // Velocities
    //
    derive_lst.add("x_velocity",IndexType::TheCellType(),1,
		   BL_FORT_PROC_CALL(RNS_DERVEL,rns_dervel),the_same_box);
    derive_lst.addComponent("x_velocity",desc_lst,State_Type,Density,1);
    derive_lst.addComponent("x_velocity",desc_lst,State_Type,Xmom,1);
  
#if (BL_SPACEDIM >= 2)
    derive_lst.add("y_velocity",IndexType::TheCellType(),1,
		   BL_FORT_PROC_CALL(RNS_DERVEL,rns_dervel),the_same_box);
    derive_lst.addComponent("y_velocity",desc_lst,State_Type,Density,1);
    derive_lst.addComponent("y_velocity",desc_lst,State_Type,Ymom,1);
#endif
    
#if (BL_SPACEDIM == 3)
    derive_lst.add("z_velocity",IndexType::TheCellType(),1,
		   BL_FORT_PROC_CALL(RNS_DERVEL,rns_dervel),the_same_box);
    derive_lst.addComponent("z_velocity",desc_lst,State_Type,Density,1);
    derive_lst.addComponent("z_velocity",desc_lst,State_Type,Zmom,1);
#endif
    
    //
    // DEFINE ERROR ESTIMATION QUANTITIES
    //
    ErrorSetUp();
}
