 &fortin
  probtype = 3
  flct_file = ""
  flct_file = "../TurbBin_plt0620"
  forceInflow = .FALSE.
  forceInflow = .TRUE.
  zstandoff = -0.03
  zBL = .008
  vheight = 0.0
  numInflPlanesStore = 64
  numInflPlanesStore = 32
  numInflPlanesStore = 16

  Vco_l  = .2
  Vco_r  = .2
  tVco_l = 0.0
  tVco_r = 1.0

  rhot = .025
  Vin = 1.5
  Vin = 5.0
  Vin = 3.0
  Vin = 3.9

  turb_scale=1.067
  turb_scale=1.0
  turb_scale=1.5

  flametracval = 1.e-9
  max_temp_lev = 0
  temperr = 100
  max_vort_lev = 0
  refine_stick = 1
  max_stick_lev = 1
  refine_stick_x = 0.003
  refine_stick_z = 0.003
  refine_nozzle = 1
  max_nozzle_lev = 1
  refine_nozzle_x = 0.03
  refine_nozzle_z = 0.04
  refine_nozzle_z = 0.07
  tempgrad = 300.0

  swK = 0.0
  dBL = .0001
  dBL = .17
  anisotsc = 1.3
  anisotsc = 1.0
  stBL = .002
  stBL = .004
  wallTh = 0.00375
  Ro = .025
  Rf = .025
  stTh = 0.001
  stTh = 0.003
  stTh = 0.002
  rhot = .006
 /
 &heattransin
  pamb = 101325.
  dpdt_factor = 1.0
  dpdt_factor = .3
 /
