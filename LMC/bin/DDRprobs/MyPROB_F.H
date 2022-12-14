#undef BL_LANG_CC
#ifndef BL_LANG_FORT
#define BL_LANG_FORT
#endif

#define SDIM BL_SPACEDIM

#include <REAL.H>
#include <CONSTANTS.H>
#include <BC_TYPES.H>
#include <PROB_AMR_F.H>
#include <PROB_F.H>
#include <ArrayLim.H>
#include <ChemDriver_F.H>
#include <HEATTRANSFER_F.H>
#if defined(BL_DO_FLCT)
#include <infl_frc.H>
#endif

#define BL_PROB_JET_DIFFUSION 'diffusion_jet'
#define BL_PROB_PREMIXED_FIXED_INFLOW 'premixed_fixed-inflow'
#define BL_PROB_PREMIXED_CONTROLLED_INFLOW 'premixed_controlled-inflow'
#define BL_PROB_PREMIXED_FREE 'premixed_free'
#define BL_PROB_UNDEFINED 'undefined_problem'


#define BL_XLO 0
#define BL_YLO 1
#define BL_XHI 2
#define BL_YHI 3
#define BL_ZLO 4
#define BL_ZHI 5
#define BL_INTERIOR 6

#define BL_FUELPIPE 1
#define BL_OUTFLOW  2
#define BL_OXIDIZER 3
#define BL_AIR      4
#define BL_PIPEEND  5
#define BL_VOLUME   6
