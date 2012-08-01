module advance_module

  use bl_error_module
  use multifab_module
  use probin_module
  use variables
  use time_module
  use transport_properties

  implicit none

  private

  ! for 8th-order first-order derivatives
  double precision, parameter :: ALP =  0.8d0
  double precision, parameter :: BET = -0.2d0
  double precision, parameter :: GAM =  4.d0/105.d0
  double precision, parameter :: DEL = -1.d0/280.d0

  !
  ! Some arithmetic constants.
  !
  double precision, parameter :: Zero          = 0.d0
  double precision, parameter :: One           = 1.d0
  double precision, parameter :: OneThird      = 1.d0/3.d0
  double precision, parameter :: TwoThirds     = 2.d0/3.d0
  double precision, parameter :: FourThirds    = 4.d0/3.d0
  double precision, parameter :: OneQuarter    = 1.d0/4.d0
  double precision, parameter :: ThreeQuarters = 3.d0/4.d0

  public advance

contains

  subroutine advance(U, dt, dx)

    type(multifab),    intent(inout) :: U
    double precision,  intent(inout) :: dt
    double precision,  intent(in   ) :: dx(U%dim) 

    if (advance_method == 2) then
       call bl_error("call advance_sdc")
    else
       call advance_rk3(U, dt, dx)
    end if

  end subroutine advance


  subroutine advance_rk3 (U,dt,dx)

    type(multifab),    intent(inout) :: U
    double precision,  intent(inout) :: dt
    double precision,  intent(in   ) :: dx(U%dim)

    integer          :: ng
    double precision :: courno, courno_proc
    type(layout)     :: la
    type(multifab)   :: Uprime, Unew

    ng = nghost(U)
    la = get_layout(U)

    call multifab_build(Uprime, la, ncons, 0)
    call multifab_build(Unew,   la, ncons, ng)

    ! RK Step 1
    courno_proc = 1.0d-50

    call dUdt(U, Uprime, dx, courno=courno_proc)

    if (fixed_dt > 0.d0) then
       dt = fixed_dt
       if ( parallel_IOProcessor() ) then
          print*, "Fixed dt = ", dt
       end if
    else

       call parallel_reduce(courno, courno_proc, MPI_MAX)

       dt = cflfac / courno

       if (stop_time > 0.d0) then
          if (time + dt > stop_time) then
             dt = stop_time - time
          end if
       end if
       
       if ( parallel_IOProcessor() ) then
          print*, "dt,courno", dt, courno
       end if
    end if

    call update_rk3(Zero,Unew,One,U,dt,Uprime)

    !
    ! Calculate U at time N+2/3
    !
    call dUdt(Unew,Uprime,dx)
    call update_rk3(OneQuarter,Unew,ThreeQuarters,U,OneQuarter*dt,Uprime)

    !
    ! Calculate U at time N+1
    !
    call dUdt(Unew,Uprime,dx)
    call update_rk3(OneThird,U,TwoThirds,Unew,TwoThirds*dt,Uprime)

    call destroy(Unew)
    call destroy(Uprime)

  end subroutine advance_rk3


  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
  !
  ! Compute U1 = a U1 + b U2 + c Uprime.
  !
  subroutine update_rk3 (a,U1,b,U2,c,Uprime)

    type(multifab),   intent(in   ) :: U2, Uprime
    type(multifab),   intent(inout) :: U1
    double precision, intent(in   ) :: a, b, c

    integer :: lo(U1%dim), hi(U1%dim), i, j, k, m, n, nc

    double precision, pointer, dimension(:,:,:,:) :: u1p, u2p, upp

    nc = ncomp(U1)

    do n=1,nboxes(U1)
       if ( remote(U1,n) ) cycle

       u1p => dataptr(U1,    n)
       u2p => dataptr(U2,    n)
       upp => dataptr(Uprime,n)

       lo = lwb(get_box(U1,n))
       hi = upb(get_box(U1,n))

       do m = 1, nc
          !$xxxxxOMP PARALLEL DO PRIVATE(i,j,k)
          do k = lo(3),hi(3)
             do j = lo(2),hi(2)
                do i = lo(1),hi(1)
                   u1p(i,j,k,m) = a * u1p(i,j,k,m) + b * u2p(i,j,k,m) + c * upp(i,j,k,m)
                end do
             end do
          end do
          !$xxxxxOMP END PARALLEL DO
       end do
    end do

  end subroutine update_rk3


  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
  !
  ! Compute dU/dt given U.
  !
  ! The Courant number (courno) is also computed if passed.
  !
  subroutine dUdt (U, Uprime, dx, courno)

    type(multifab),   intent(inout) :: U, Uprime
    double precision, intent(in   ) :: dx(U%dim)
    double precision, intent(inout), optional :: courno

    type(multifab) :: mu, xi ! viscosity
    type(multifab) :: lam ! partial thermal conductivity
    type(multifab) :: Ddiag ! diagonal components of D

    integer          :: lo(U%dim), hi(U%dim), i,j,k,m,n, ng, dim
    type(layout)     :: la
    type(multifab)   :: Q, Fhyp, Fdif

    double precision, pointer, dimension(:,:,:,:) :: up, fhp, fdp, qp, mup, xip, lamp, Ddp, upp

    dim = U%dim
    ng = nghost(U)
    la = get_layout(U)

    !
    ! Sync U prior to calculating D & F
    !
    call multifab_fill_boundary(U)

    call multifab_build(Q, la, nprim, ng)

    call multifab_build(Fhyp, la, ncons, 0)
    call multifab_build(Fdif, la, ncons, 0)

    call multifab_build(mu , la, 1, ng)
    call multifab_build(xi , la, 1, ng)
    call multifab_build(lam, la, 1, ng)
    call multifab_build(Ddiag, la, nspecies, ng)

    !
    ! Calculate primitive variables based on U
    !
    call ctoprim(U, Q, ng)

    if (present(courno)) then
       call compute_courno(Q, dx, courno)
    end if

    call get_transport_properties(Q, mu, xi, lam, Ddiag)

    !
    ! Hyperbolic terms
    !
    do n=1,nboxes(Fhyp)
       if ( remote(Fhyp,n) ) cycle

       up => dataptr(U,n)
       qp => dataptr(Q,n)
       fhp=> dataptr(Fhyp,n)

       lo = lwb(get_box(Fhyp,n))
       hi = upb(get_box(Fhyp,n))

       if (dim .ne. 3) then
          call bl_error("Only 3D hypterm is supported")
       else
          call hypterm_3d(lo,hi,ng,dx,up,qp,fhp)
       end if
    end do

    
    !
    ! Transport terms
    !
    do n=1,nboxes(Q)
       if ( remote(Q,n) ) cycle

       qp => dataptr(Q,n)
       fdp=> dataptr(Fdif,n)

       mup  => dataptr(mu   , n)
       xip  => dataptr(xi   , n)
       lamp => dataptr(lam  , n)
       Ddp  => dataptr(Ddiag, n)

       lo = lwb(get_box(Q,n))
       hi = upb(get_box(Q,n))

       if (dim .ne. 3) then
          call bl_error("Only 3D compact_diffterm is supported")
       else
          call compact_diffterm_3d(lo,hi,ng,dx,qp,fdp,mup,xip,lamp,Ddp)
       end if
    end do

    !
    ! Calculate U'
    !
    do n=1,nboxes(U)
       if ( remote(U,n) ) cycle
       
       fhp => dataptr(Fhyp,  n)
       fdp => dataptr(Fdif,  n)
       upp => dataptr(Uprime,n)

       lo = lwb(get_box(U,n))
       hi = upb(get_box(U,n))

       do m = 1, ncons
          !$xxxxxOMP PARALLEL DO PRIVATE(i,j,k)
          do k = lo(3),hi(3)
             do j = lo(2),hi(2)
                do i = lo(1),hi(1)
                   upp(i,j,k,m) = fhp(i,j,k,m) + fdp(i,j,k,m)
                end do
             end do
          end do
          !$xxxxxOMP END PARALLEL DO
       end do
    end do

    call destroy(Q)

    call destroy(Fhyp)
    call destroy(Fdif)

    call destroy(mu)
    call destroy(xi)
    call destroy(lam)
    call destroy(Ddiag)

  end subroutine dUdt


  subroutine hypterm_3d (lo,hi,ng,dx,cons,q,flux)

    integer,          intent(in ) :: lo(3),hi(3),ng
    double precision, intent(in ) :: dx(3)
    double precision, intent(in ) :: cons(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng,ncons)
    double precision, intent(in ) ::    q(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng,nprim)
    double precision, intent(out) :: flux(    lo(1):hi(1)   ,    lo(2):hi(2)   ,    lo(3):hi(3)   ,ncons)

    integer          :: i,j,k,n
    double precision :: unp1,unp2,unp3,unp4,unm1,unm2,unm3,unm4
    double precision :: dxinv(3)

    do i=1,3
       dxinv(i) = 1.0d0 / dx(i)
    end do

    do k=lo(3),hi(3)
       do j=lo(2),hi(2)
          do i=lo(1),hi(1)

             unp1 = q(i+1,j,k,qu)
             unp2 = q(i+2,j,k,qu)
             unp3 = q(i+3,j,k,qu)
             unp4 = q(i+4,j,k,qu)

             unm1 = q(i-1,j,k,qu)
             unm2 = q(i-2,j,k,qu)
             unm3 = q(i-3,j,k,qu)
             unm4 = q(i-4,j,k,qu)

             flux(i,j,k,irho)= - &
                   (ALP*(cons(i+1,j,k,imx)-cons(i-1,j,k,imx)) &
                  + BET*(cons(i+2,j,k,imx)-cons(i-2,j,k,imx)) &
                  + GAM*(cons(i+3,j,k,imx)-cons(i-3,j,k,imx)) &
                  + DEL*(cons(i+4,j,k,imx)-cons(i-4,j,k,imx)))*dxinv(1)

             flux(i,j,k,imx)= - &
                   (ALP*(cons(i+1,j,k,imx)*unp1-cons(i-1,j,k,imx)*unm1 &
                  + (q(i+1,j,k,qpres)-q(i-1,j,k,qpres)))               &
                  + BET*(cons(i+2,j,k,imx)*unp2-cons(i-2,j,k,imx)*unm2 &
                  + (q(i+2,j,k,qpres)-q(i-2,j,k,qpres)))               &
                  + GAM*(cons(i+3,j,k,imx)*unp3-cons(i-3,j,k,imx)*unm3 &
                  + (q(i+3,j,k,qpres)-q(i-3,j,k,qpres)))               &
                  + DEL*(cons(i+4,j,k,imx)*unp4-cons(i-4,j,k,imx)*unm4 &
                  + (q(i+4,j,k,qpres)-q(i-4,j,k,qpres))))*dxinv(1)

             flux(i,j,k,imy)= - &
                   (ALP*(cons(i+1,j,k,imy)*unp1-cons(i-1,j,k,imy)*unm1) &
                  + BET*(cons(i+2,j,k,imy)*unp2-cons(i-2,j,k,imy)*unm2) &
                  + GAM*(cons(i+3,j,k,imy)*unp3-cons(i-3,j,k,imy)*unm3) &
                  + DEL*(cons(i+4,j,k,imy)*unp4-cons(i-4,j,k,imy)*unm4))*dxinv(1)

             flux(i,j,k,imz)= - &
                   (ALP*(cons(i+1,j,k,imz)*unp1-cons(i-1,j,k,imz)*unm1) &
                  + BET*(cons(i+2,j,k,imz)*unp2-cons(i-2,j,k,imz)*unm2) &
                  + GAM*(cons(i+3,j,k,imz)*unp3-cons(i-3,j,k,imz)*unm3) &
                  + DEL*(cons(i+4,j,k,imz)*unp4-cons(i-4,j,k,imz)*unm4))*dxinv(1)

             flux(i,j,k,iene)= - &
                   (ALP*(cons(i+1,j,k,iene)*unp1-cons(i-1,j,k,iene)*unm1 &
                  + (q(i+1,j,k,qpres)*unp1-q(i-1,j,k,qpres)*unm1))       &
                  + BET*(cons(i+2,j,k,iene)*unp2-cons(i-2,j,k,iene)*unm2 &
                  + (q(i+2,j,k,qpres)*unp2-q(i-2,j,k,qpres)*unm2))       &
                  + GAM*(cons(i+3,j,k,iene)*unp3-cons(i-3,j,k,iene)*unm3 &
                  + (q(i+3,j,k,qpres)*unp3-q(i-3,j,k,qpres)*unm3))       &
                  + DEL*(cons(i+4,j,k,iene)*unp4-cons(i-4,j,k,iene)*unm4 &
                  + (q(i+4,j,k,qpres)*unp4-q(i-4,j,k,qpres)*unm4)))*dxinv(1) 

             do n = iry1, iry1+nspecies-1
                flux(i,j,k,n) = - &
                     ( ALP*(cons(i+1,j,k,n)*unp1-cons(i-1,j,k,n)*unm1) &
                     + BET*(cons(i+2,j,k,n)*unp2-cons(i-2,j,k,n)*unm2) &
                     + GAM*(cons(i+3,j,k,n)*unp3-cons(i-3,j,k,n)*unm3) &
                     + DEL*(cons(i+4,j,k,n)*unp4-cons(i-4,j,k,n)*unm4))*dxinv(1)
             end do

          enddo
       enddo
    enddo

    do k=lo(3),hi(3)
       do j=lo(2),hi(2)
          do i=lo(1),hi(1)

             unp1 = q(i,j+1,k,qv)
             unp2 = q(i,j+2,k,qv)
             unp3 = q(i,j+3,k,qv)
             unp4 = q(i,j+4,k,qv)

             unm1 = q(i,j-1,k,qv)
             unm2 = q(i,j-2,k,qv)
             unm3 = q(i,j-3,k,qv)
             unm4 = q(i,j-4,k,qv)

             flux(i,j,k,irho)=flux(i,j,k,irho) - &
                   (ALP*(cons(i,j+1,k,imy)-cons(i,j-1,k,imy)) &
                  + BET*(cons(i,j+2,k,imy)-cons(i,j-2,k,imy)) &
                  + GAM*(cons(i,j+3,k,imy)-cons(i,j-3,k,imy)) &
                  + DEL*(cons(i,j+4,k,imy)-cons(i,j-4,k,imy)))*dxinv(2)

             flux(i,j,k,imx)=flux(i,j,k,imx) - &
                   (ALP*(cons(i,j+1,k,imx)*unp1-cons(i,j-1,k,imx)*unm1) &
                  + BET*(cons(i,j+2,k,imx)*unp2-cons(i,j-2,k,imx)*unm2) &
                  + GAM*(cons(i,j+3,k,imx)*unp3-cons(i,j-3,k,imx)*unm3) &
                  + DEL*(cons(i,j+4,k,imx)*unp4-cons(i,j-4,k,imx)*unm4))*dxinv(2)

             flux(i,j,k,imy)=flux(i,j,k,imy) - &
                   (ALP*(cons(i,j+1,k,imy)*unp1-cons(i,j-1,k,imy)*unm1 &
                  + (q(i,j+1,k,qpres)-q(i,j-1,k,qpres)))               &
                  + BET*(cons(i,j+2,k,imy)*unp2-cons(i,j-2,k,imy)*unm2 &
                  + (q(i,j+2,k,qpres)-q(i,j-2,k,qpres)))               &
                  + GAM*(cons(i,j+3,k,imy)*unp3-cons(i,j-3,k,imy)*unm3 &
                  + (q(i,j+3,k,qpres)-q(i,j-3,k,qpres)))               &
                  + DEL*(cons(i,j+4,k,imy)*unp4-cons(i,j-4,k,imy)*unm4 &
                  + (q(i,j+4,k,qpres)-q(i,j-4,k,qpres))))*dxinv(2)

             flux(i,j,k,imz)=flux(i,j,k,imz) - &
                   (ALP*(cons(i,j+1,k,imz)*unp1-cons(i,j-1,k,imz)*unm1) &
                  + BET*(cons(i,j+2,k,imz)*unp2-cons(i,j-2,k,imz)*unm2) &
                  + GAM*(cons(i,j+3,k,imz)*unp3-cons(i,j-3,k,imz)*unm3) &
                  + DEL*(cons(i,j+4,k,imz)*unp4-cons(i,j-4,k,imz)*unm4))*dxinv(2)

             flux(i,j,k,iene)=flux(i,j,k,iene) - &
                   (ALP*(cons(i,j+1,k,iene)*unp1-cons(i,j-1,k,iene)*unm1 &
                  + (q(i,j+1,k,qpres)*unp1-q(i,j-1,k,qpres)*unm1))       &
                  + BET*(cons(i,j+2,k,iene)*unp2-cons(i,j-2,k,iene)*unm2 &
                  + (q(i,j+2,k,qpres)*unp2-q(i,j-2,k,qpres)*unm2))       &
                  + GAM*(cons(i,j+3,k,iene)*unp3-cons(i,j-3,k,iene)*unm3 &
                  + (q(i,j+3,k,qpres)*unp3-q(i,j-3,k,qpres)*unm3))       &
                  + DEL*(cons(i,j+4,k,iene)*unp4-cons(i,j-4,k,iene)*unm4 &
                  + (q(i,j+4,k,qpres)*unp4-q(i,j-4,k,qpres)*unm4)))*dxinv(2)

             do n = iry1, iry1+nspecies-1
                flux(i,j,k,n) = flux(i,j,k,n) - &
                     ( ALP*(cons(i,j+1,k,n)*unp1-cons(i,j-1,k,n)*unm1) &
                     + BET*(cons(i,j+2,k,n)*unp2-cons(i,j-2,k,n)*unm2) &
                     + GAM*(cons(i,j+3,k,n)*unp3-cons(i,j-3,k,n)*unm3) &
                     + DEL*(cons(i,j+4,k,n)*unp4-cons(i,j-4,k,n)*unm4))*dxinv(2)
             end do

          enddo
       enddo
    enddo

    do k=lo(3),hi(3)
       do j=lo(2),hi(2)
          do i=lo(1),hi(1)

             unp1 = q(i,j,k+1,qw)
             unp2 = q(i,j,k+2,qw)
             unp3 = q(i,j,k+3,qw)
             unp4 = q(i,j,k+4,qw)

             unm1 = q(i,j,k-1,qw)
             unm2 = q(i,j,k-2,qw)
             unm3 = q(i,j,k-3,qw)
             unm4 = q(i,j,k-4,qw)

             flux(i,j,k,irho)=flux(i,j,k,irho) - &
                   (ALP*(cons(i,j,k+1,imz)-cons(i,j,k-1,imz)) &
                  + BET*(cons(i,j,k+2,imz)-cons(i,j,k-2,imz)) &
                  + GAM*(cons(i,j,k+3,imz)-cons(i,j,k-3,imz)) &
                  + DEL*(cons(i,j,k+4,imz)-cons(i,j,k-4,imz)))*dxinv(3)

             flux(i,j,k,imx)=flux(i,j,k,imx) - &
                   (ALP*(cons(i,j,k+1,imx)*unp1-cons(i,j,k-1,imx)*unm1) &
                  + BET*(cons(i,j,k+2,imx)*unp2-cons(i,j,k-2,imx)*unm2) &
                  + GAM*(cons(i,j,k+3,imx)*unp3-cons(i,j,k-3,imx)*unm3) &
                  + DEL*(cons(i,j,k+4,imx)*unp4-cons(i,j,k-4,imx)*unm4))*dxinv(3)

             flux(i,j,k,imy)=flux(i,j,k,imy) - &
                   (ALP*(cons(i,j,k+1,imy)*unp1-cons(i,j,k-1,imy)*unm1) &
                  + BET*(cons(i,j,k+2,imy)*unp2-cons(i,j,k-2,imy)*unm2) &
                  + GAM*(cons(i,j,k+3,imy)*unp3-cons(i,j,k-3,imy)*unm3) &
                  + DEL*(cons(i,j,k+4,imy)*unp4-cons(i,j,k-4,imy)*unm4))*dxinv(3)

             flux(i,j,k,imz)=flux(i,j,k,imz) - &
                   (ALP*(cons(i,j,k+1,imz)*unp1-cons(i,j,k-1,imz)*unm1 &
                  + (q(i,j,k+1,qpres)-q(i,j,k-1,qpres)))               &
                  + BET*(cons(i,j,k+2,imz)*unp2-cons(i,j,k-2,imz)*unm2 &
                  + (q(i,j,k+2,qpres)-q(i,j,k-2,qpres)))               &
                  + GAM*(cons(i,j,k+3,imz)*unp3-cons(i,j,k-3,imz)*unm3 &
                  + (q(i,j,k+3,qpres)-q(i,j,k-3,qpres)))               &
                  + DEL*(cons(i,j,k+4,imz)*unp4-cons(i,j,k-4,imz)*unm4 &
                  + (q(i,j,k+4,qpres)-q(i,j,k-4,qpres))))*dxinv(3)

             flux(i,j,k,iene)=flux(i,j,k,iene) - &
                   (ALP*(cons(i,j,k+1,iene)*unp1-cons(i,j,k-1,iene)*unm1 &
                  + (q(i,j,k+1,qpres)*unp1-q(i,j,k-1,qpres)*unm1))       &
                  + BET*(cons(i,j,k+2,iene)*unp2-cons(i,j,k-2,iene)*unm2 &
                  + (q(i,j,k+2,qpres)*unp2-q(i,j,k-2,qpres)*unm2))       &
                  + GAM*(cons(i,j,k+3,iene)*unp3-cons(i,j,k-3,iene)*unm3 &
                  + (q(i,j,k+3,qpres)*unp3-q(i,j,k-3,qpres)*unm3))       &
                  + DEL*(cons(i,j,k+4,iene)*unp4-cons(i,j,k-4,iene)*unm4 &
                  + (q(i,j,k+4,qpres)*unp4-q(i,j,k-4,qpres)*unm4)))*dxinv(3)

             do n = iry1, iry1+nspecies-1
                flux(i,j,k,n) = flux(i,j,k,n) - &
                     ( ALP*(cons(i,j,k+1,n)*unp1-cons(i,j,k-1,n)*unm1) &
                     + BET*(cons(i,j,k+2,n)*unp2-cons(i,j,k-2,n)*unm2) &
                     + GAM*(cons(i,j,k+3,n)*unp3-cons(i,j,k-3,n)*unm3) &
                     + DEL*(cons(i,j,k+4,n)*unp4-cons(i,j,k-4,n)*unm4))*dxinv(3)
             end do

          enddo
       enddo
    enddo

  end subroutine hypterm_3d


  subroutine compact_diffterm_3d (lo,hi,ng,dx,q,flx,mu,xi,lam,Dd)

    integer,          intent(in ) :: lo(3),hi(3),ng
    double precision, intent(in ) :: dx(3)
    double precision, intent(in ) :: q  (-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng,nprim)
    double precision, intent(in ) :: mu (-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng)
    double precision, intent(in ) :: xi (-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng)
    double precision, intent(in ) :: lam(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng)
    double precision, intent(in ) :: Dd (-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng,nspecies)
    double precision, intent(out) :: flx(    lo(1):hi(1)   ,    lo(2):hi(2)   ,    lo(3):hi(3)   ,ncons)

    double precision, allocatable, dimension(:,:,:) :: ux,uy,uz,vx,vy,vz,wx,wy,wz
    double precision, allocatable, dimension(:,:,:) :: vsc1,vsc2
    double precision, allocatable, dimension(:,:,:,:) :: Hg, dcx, dcp

    double precision :: dxinv(3), dx2inv(3), divu
    double precision :: dmvxdy,dmwxdz,dmvywzdx
    double precision :: dmuydx,dmwydz,dmuxwzdy
    double precision :: dmuzdx,dmvzdy,dmuxvydz
    double precision :: tauxx,tauyy,tauzz 
    double precision :: Htot, Htmp(nspecies), Ytmp(nspecies)
    integer          :: i,j,k,n, qxn, qyn, qhn

    ! coefficients for 8th-order stencil of second-order derivative
    double precision, parameter :: m47 = 683.d0/10080.d0, m48 = -1.d0/224.d0
    double precision, parameter :: m11 = 5.d0/336.d0 + m48, &
         &                         m12 = -83.d0/3600.d0 - m47/5.d0 - 14.d0*m48/5.d0, &
         &                         m13 = 299.d0/50400.d0 + 2.d0*m47/5.d0 + 13.d0*m48/5.d0, &
         &                         m14 = 17.d0/12600.d0 - m47/5.d0 - 4.d0*m48/5.d0, &
         &                         m15 = 1.d0/1120.d0, &
         &                         m21 = -11.d0/560.d0 - 2.d0*m48, &
         &                         m22 = -31.d0/360.d0 + m47 + 3.d0*m48, &
         &                         m23 = 41.d0/200.d0 - 9.d0*m47/5.d0 + 4.d0*m48/5.d0, &
         &                         m24 = -5927.d0/50400.d0 + 4.d0*m47/5.d0 - 9.d0*m48/5.d0, &
         &                         m25 = 17.d0/600.d0 - m47/5.d0 - 4.d0*m48/5.d0, &
         &                         m26 = -503.d0/50400.d0 + m47/5.d0 + 4.d0*m48/5.d0, &
         &                         m31 = -1.d0/280.d0, &
         &                         m32 = 1097.d0/5040.d0 - 2.d0*m47 + 6.d0*m48, &
         &                         m33 = -1349.d0/10080.d0 + 3.d0*m47 - 12.d0*m48, &
         &                         m34 = -887.d0/5040.d0 - m47 + 6.d0*m48, &
         &                         m35 = 3613.d0/50400.d0 + 4.d0*m47/5.d0 - 9.d0*m48/5.d0, &
         &                         m36 = 467.d0/25200.d0 - 3.d0*m47/5.d0 + 18.d0*m48/5.d0, &
         &                         m37 = 139.d0/25200.d0 - m47/5.d0 - 9.d0*m48/5.d0, &
         &                         m41 = 17.d0/1680.d0 + 2.d0*m48, &
         &                         m42 = -319.d0/2520.d0 + 2.d0*m47 - 8.d0*m48, &
         &                         m43 = -919.d0/5040.d0 - 2.d0*m47 + 6.d0*m48, &
         &                         m44 = -445.d0/2016.d0, &
         &                         m45 = 583.d0/720.d0 - m47 + 6.d0*m48, &
         &                         m46 = -65.d0/224.d0 - 7.d0*m48

    allocate(ux(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng))
    allocate(uy(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng))
    allocate(uz(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng))
    allocate(vx(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng))
    allocate(vy(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng))
    allocate(vz(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng))
    allocate(wx(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng))
    allocate(wy(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng))
    allocate(wz(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng))

    allocate(vsc1(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng))
    allocate(vsc2(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng))

    allocate(Hg(lo(1):hi(1)+1,lo(2):hi(2)+1,lo(3):hi(3)+1,ncons))

    allocate(dcx(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng,nspecies))
    allocate(dcp(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng,nspecies))

    do i = 1,3
       dxinv(i) = 1.0d0 / dx(i)
       dx2inv(i) = dxinv(i)**2
    end do

    do k=lo(3),hi(3)
       do j=lo(2),hi(2)
          do i=lo(1),hi(1)
             flx(i,j,k,irho) = 0.0d0
          end do
       end do
    end do

    do k=lo(3)-ng,hi(3)+ng
       do j=lo(2)-ng,hi(2)+ng
          do i=lo(1)-ng,hi(1)+ng
             vsc1(i,j,k) = xi(i,j,k) + FourThirds*mu(i,j,k)
             vsc2(i,j,k) = xi(i,j,k) -  TwoThirds*mu(i,j,k)
          enddo
       enddo
    enddo

    do k=lo(3)-ng,hi(3)+ng
       do j=lo(2)-ng,hi(2)+ng
          do i=lo(1),hi(1)

             ux(i,j,k)= &
                   (ALP*(q(i+1,j,k,qu)-q(i-1,j,k,qu)) &
                  + BET*(q(i+2,j,k,qu)-q(i-2,j,k,qu)) &
                  + GAM*(q(i+3,j,k,qu)-q(i-3,j,k,qu)) &
                  + DEL*(q(i+4,j,k,qu)-q(i-4,j,k,qu)))*dxinv(1)

             vx(i,j,k)= &
                   (ALP*(q(i+1,j,k,qv)-q(i-1,j,k,qv)) &
                  + BET*(q(i+2,j,k,qv)-q(i-2,j,k,qv)) &
                  + GAM*(q(i+3,j,k,qv)-q(i-3,j,k,qv)) &
                  + DEL*(q(i+4,j,k,qv)-q(i-4,j,k,qv)))*dxinv(1)

             wx(i,j,k)= &
                   (ALP*(q(i+1,j,k,qw)-q(i-1,j,k,qw)) &
                  + BET*(q(i+2,j,k,qw)-q(i-2,j,k,qw)) &
                  + GAM*(q(i+3,j,k,qw)-q(i-3,j,k,qw)) &
                  + DEL*(q(i+4,j,k,qw)-q(i-4,j,k,qw)))*dxinv(1)
          enddo
       enddo
    enddo

    do k=lo(3)-ng,hi(3)+ng
       do j=lo(2),hi(2)   
          do i=lo(1)-ng,hi(1)+ng

             uy(i,j,k)= &
                   (ALP*(q(i,j+1,k,qu)-q(i,j-1,k,qu)) &
                  + BET*(q(i,j+2,k,qu)-q(i,j-2,k,qu)) &
                  + GAM*(q(i,j+3,k,qu)-q(i,j-3,k,qu)) &
                  + DEL*(q(i,j+4,k,qu)-q(i,j-4,k,qu)))*dxinv(2)

             vy(i,j,k)= &
                   (ALP*(q(i,j+1,k,qv)-q(i,j-1,k,qv)) &
                  + BET*(q(i,j+2,k,qv)-q(i,j-2,k,qv)) &
                  + GAM*(q(i,j+3,k,qv)-q(i,j-3,k,qv)) &
                  + DEL*(q(i,j+4,k,qv)-q(i,j-4,k,qv)))*dxinv(2)

             wy(i,j,k)= &
                   (ALP*(q(i,j+1,k,qw)-q(i,j-1,k,qw)) &
                  + BET*(q(i,j+2,k,qw)-q(i,j-2,k,qw)) &
                  + GAM*(q(i,j+3,k,qw)-q(i,j-3,k,qw)) &
                  + DEL*(q(i,j+4,k,qw)-q(i,j-4,k,qw)))*dxinv(2)
          enddo
       enddo
    enddo

    do k=lo(3),hi(3)
       do j=lo(2)-ng,hi(2)+ng
          do i=lo(1)-ng,hi(1)+ng

             uz(i,j,k)= &
                   (ALP*(q(i,j,k+1,qu)-q(i,j,k-1,qu)) &
                  + BET*(q(i,j,k+2,qu)-q(i,j,k-2,qu)) &
                  + GAM*(q(i,j,k+3,qu)-q(i,j,k-3,qu)) &
                  + DEL*(q(i,j,k+4,qu)-q(i,j,k-4,qu)))*dxinv(3)

             vz(i,j,k)= &
                   (ALP*(q(i,j,k+1,qv)-q(i,j,k-1,qv)) &
                  + BET*(q(i,j,k+2,qv)-q(i,j,k-2,qv)) &
                  + GAM*(q(i,j,k+3,qv)-q(i,j,k-3,qv)) &
                  + DEL*(q(i,j,k+4,qv)-q(i,j,k-4,qv)))*dxinv(3)

             wz(i,j,k)= &
                   (ALP*(q(i,j,k+1,qw)-q(i,j,k-1,qw)) &
                  + BET*(q(i,j,k+2,qw)-q(i,j,k-2,qw)) &
                  + GAM*(q(i,j,k+3,qw)-q(i,j,k-3,qw)) &
                  + DEL*(q(i,j,k+4,qw)-q(i,j,k-4,qw)))*dxinv(3)
          enddo
       enddo
    enddo

    do n=1,nspecies
       do k=lo(3)-ng,hi(3)+ng
          do j=lo(2)-ng,hi(2)+ng
             do i=lo(1)-ng,hi(1)+ng
                dcx(i,j,k,n) = q(i,j,k,qrho) * q(i,j,k,qy1+n-1) * Dd(i,j,k,n)
                dcp(i,j,k,n) = dcx(i,j,k,n)/q(i,j,k,qpres)*(q(i,j,k,qx1+n-1)-q(i,j,k,qy1+n-1))
             end do
          end do
       end do
    end do

    do k=lo(3),hi(3)
       do j=lo(2),hi(2)
          do i=lo(1),hi(1)

             ! d(mu*dv/dx)/dy
             dmvxdy = (ALP*(mu(i,j+1,k)*vx(i,j+1,k)-mu(i,j-1,k)*vx(i,j-1,k)) &
                  +    BET*(mu(i,j+2,k)*vx(i,j+2,k)-mu(i,j-2,k)*vx(i,j-2,k)) &
                  +    GAM*(mu(i,j+3,k)*vx(i,j+3,k)-mu(i,j-3,k)*vx(i,j-3,k)) &
                  +    DEL*(mu(i,j+4,k)*vx(i,j+4,k)-mu(i,j-4,k)*vx(i,j-4,k)))*dxinv(2) 

             ! d(mu*dw/dx)/dz
             dmwxdz = (ALP*(mu(i,j,k+1)*wx(i,j,k+1)-mu(i,j,k-1)*wx(i,j,k-1)) &
                  +    BET*(mu(i,j,k+2)*wx(i,j,k+2)-mu(i,j,k-2)*wx(i,j,k-2)) &
                  +    GAM*(mu(i,j,k+3)*wx(i,j,k+3)-mu(i,j,k-3)*wx(i,j,k-3)) &
                  +    DEL*(mu(i,j,k+4)*wx(i,j,k+4)-mu(i,j,k-4)*wx(i,j,k-4)))*dxinv(3) 

             ! d((xi-2/3*mu)*(vy+wz))/dx
             dmvywzdx = (ALP*(vsc2(i+1,j,k)*(vy(i+1,j,k)+wz(i+1,j,k))-vsc2(i-1,j,k)*(vy(i-1,j,k)+wz(i-1,j,k))) &
                  +      BET*(vsc2(i+2,j,k)*(vy(i+2,j,k)+wz(i+2,j,k))-vsc2(i-2,j,k)*(vy(i-2,j,k)+wz(i-2,j,k))) &
                  +      GAM*(vsc2(i+3,j,k)*(vy(i+3,j,k)+wz(i+3,j,k))-vsc2(i-3,j,k)*(vy(i-3,j,k)+wz(i-3,j,k))) &
                  +      DEL*(vsc2(i+4,j,k)*(vy(i+4,j,k)+wz(i+4,j,k))-vsc2(i-4,j,k)*(vy(i-4,j,k)+wz(i-4,j,k))) &
                  ) * dxinv(1)

             ! d(mu*du/dy)/dx
             dmuydx = (ALP*(mu(i+1,j,k)*uy(i+1,j,k)-mu(i-1,j,k)*uy(i-1,j,k)) &
                  +    BET*(mu(i+2,j,k)*uy(i+2,j,k)-mu(i-2,j,k)*uy(i-2,j,k)) &
                  +    GAM*(mu(i+3,j,k)*uy(i+3,j,k)-mu(i-3,j,k)*uy(i-3,j,k)) &
                  +    DEL*(mu(i+4,j,k)*uy(i+4,j,k)-mu(i-4,j,k)*uy(i-4,j,k)))*dxinv(1) 

             ! d(mu*dw/dy)/dz
             dmwydz = (ALP*(mu(i,j,k+1)*wy(i,j,k+1)-mu(i,j,k-1)*wy(i,j,k-1)) &
                  +    BET*(mu(i,j,k+2)*wy(i,j,k+2)-mu(i,j,k-2)*wy(i,j,k-2)) &
                  +    GAM*(mu(i,j,k+3)*wy(i,j,k+3)-mu(i,j,k-3)*wy(i,j,k-3)) &
                  +    DEL*(mu(i,j,k+4)*wy(i,j,k+4)-mu(i,j,k-4)*wy(i,j,k-4)))*dxinv(3) 

             ! d((xi-2/3*mu)*(ux+wz))/dy
             dmuxwzdy = (ALP*(vsc2(i,j+1,k)*(ux(i,j+1,k)+wz(i,j+1,k))-vsc2(i,j-1,k)*(ux(i,j-1,k)+wz(i,j-1,k))) &
                  +      BET*(vsc2(i,j+2,k)*(ux(i,j+2,k)+wz(i,j+2,k))-vsc2(i,j-2,k)*(ux(i,j-2,k)+wz(i,j-2,k))) &
                  +      GAM*(vsc2(i,j+3,k)*(ux(i,j+3,k)+wz(i,j+3,k))-vsc2(i,j-3,k)*(ux(i,j-3,k)+wz(i,j-3,k))) &
                  +      DEL*(vsc2(i,j+4,k)*(ux(i,j+4,k)+wz(i,j+4,k))-vsc2(i,j-4,k)*(ux(i,j-4,k)+wz(i,j-4,k))) &
                  ) * dxinv(2)

             ! d(mu*du/dz)/dx
             dmuzdx = (ALP*(mu(i+1,j,k)*uz(i+1,j,k)-mu(i-1,j,k)*uz(i-1,j,k)) &
                  +    BET*(mu(i+2,j,k)*uz(i+2,j,k)-mu(i-2,j,k)*uz(i-2,j,k)) &
                  +    GAM*(mu(i+3,j,k)*uz(i+3,j,k)-mu(i-3,j,k)*uz(i-3,j,k)) &
                  +    DEL*(mu(i+4,j,k)*uz(i+4,j,k)-mu(i-4,j,k)*uz(i-4,j,k)))*dxinv(1) 

             ! d(mu*dv/dz)/dy
             dmvzdy = (ALP*(mu(i,j+1,k)*vz(i,j+1,k)-mu(i,j-1,k)*vz(i,j-1,k)) &
                  +    BET*(mu(i,j+2,k)*vz(i,j+2,k)-mu(i,j-2,k)*vz(i,j-2,k)) &
                  +    GAM*(mu(i,j+3,k)*vz(i,j+3,k)-mu(i,j-3,k)*vz(i,j-3,k)) &
                  +    DEL*(mu(i,j+4,k)*vz(i,j+4,k)-mu(i,j-4,k)*vz(i,j-4,k)))*dxinv(2) 

             ! d((xi-2/3*mu)*(ux+vy))/dz
             dmuxvydz = (ALP*(vsc2(i,j,k+1)*(ux(i,j,k+1)+vy(i,j,k+1))-vsc2(i,j,k-1)*(ux(i,j,k-1)+vy(i,j,k-1))) &
                  +      BET*(vsc2(i,j,k+2)*(ux(i,j,k+2)+vy(i,j,k+2))-vsc2(i,j,k-2)*(ux(i,j,k-2)+vy(i,j,k-2))) &
                  +      GAM*(vsc2(i,j,k+3)*(ux(i,j,k+3)+vy(i,j,k+3))-vsc2(i,j,k-3)*(ux(i,j,k-3)+vy(i,j,k-3))) &
                  +      DEL*(vsc2(i,j,k+4)*(ux(i,j,k+4)+vy(i,j,k+4))-vsc2(i,j,k-4)*(ux(i,j,k-4)+vy(i,j,k-4))) &
                  ) * dxinv(3)

             flx(i,j,k,imx) = dmvxdy + dmwxdz + dmvywzdx
             flx(i,j,k,imy) = dmuydx + dmwydz + dmuxwzdy
             flx(i,j,k,imy) = dmuzdx + dmvzdy + dmuxvydz

             divu = (ux(i,j,k)+vy(i,j,k)+wz(i,j,k))*vsc2(i,j,k)
             tauxx = 2.d0*mu(i,j,k)*ux(i,j,k) + divu
             tauyy = 2.d0*mu(i,j,k)*ux(i,j,k) + divu
             tauzz = 2.d0*mu(i,j,k)*ux(i,j,k) + divu
             
             ! change in internal energy
             flx(i,j,k,iene) = tauxx*ux(i,j,k) + tauyy*vy(i,j,k) + tauzz*wz(i,j,k) &
                  + mu(i,j,k)*((uy(i,j,k)+vx(i,j,k))**2 &
                  &          + (wx(i,j,k)+uz(i,j,k))**2 &
                  &          + (vz(i,j,k)+wy(i,j,k))**2 )
          end do
       end do
    end do

    ! ------- BEGIN x-direction -------
    do k=lo(3),hi(3)
       do j=lo(2),hi(2)
          do i=lo(1),hi(1)+1
             Hg(i,j,k,imx) = m11*(vsc1(i-4,j,k)*q(i-4,j,k,qu)-vsc1(i+3,j,k)*q(i+3,j,k,qu)) &
                  +          m12*(vsc1(i-4,j,k)*q(i-3,j,k,qu)-vsc1(i+3,j,k)*q(i+2,j,k,qu)) &
                  +          m13*(vsc1(i-4,j,k)*q(i-2,j,k,qu)-vsc1(i+3,j,k)*q(i+1,j,k,qu)) &
                  +          m14*(vsc1(i-4,j,k)*q(i-1,j,k,qu)-vsc1(i+3,j,k)*q(i  ,j,k,qu)) &
                  +          m15*(vsc1(i-4,j,k)*q(i  ,j,k,qu)-vsc1(i+3,j,k)*q(i-1,j,k,qu)) &
                  &        + m21*(vsc1(i-3,j,k)*q(i-4,j,k,qu)-vsc1(i+2,j,k)*q(i+3,j,k,qu)) &
                  +          m22*(vsc1(i-3,j,k)*q(i-3,j,k,qu)-vsc1(i+2,j,k)*q(i+2,j,k,qu)) &
                  +          m23*(vsc1(i-3,j,k)*q(i-2,j,k,qu)-vsc1(i+2,j,k)*q(i+1,j,k,qu)) &
                  +          m24*(vsc1(i-3,j,k)*q(i-1,j,k,qu)-vsc1(i+2,j,k)*q(i  ,j,k,qu)) &
                  +          m25*(vsc1(i-3,j,k)*q(i  ,j,k,qu)-vsc1(i+2,j,k)*q(i-1,j,k,qu)) &
                  +          m26*(vsc1(i-3,j,k)*q(i+1,j,k,qu)-vsc1(i+2,j,k)*q(i-2,j,k,qu)) &
                  &        + m31*(vsc1(i-2,j,k)*q(i-4,j,k,qu)-vsc1(i+1,j,k)*q(i+3,j,k,qu)) &
                  +          m32*(vsc1(i-2,j,k)*q(i-3,j,k,qu)-vsc1(i+1,j,k)*q(i+2,j,k,qu)) &
                  +          m33*(vsc1(i-2,j,k)*q(i-2,j,k,qu)-vsc1(i+1,j,k)*q(i+1,j,k,qu)) &
                  +          m34*(vsc1(i-2,j,k)*q(i-1,j,k,qu)-vsc1(i+1,j,k)*q(i  ,j,k,qu)) &
                  +          m35*(vsc1(i-2,j,k)*q(i  ,j,k,qu)-vsc1(i+1,j,k)*q(i-1,j,k,qu)) &
                  +          m36*(vsc1(i-2,j,k)*q(i+1,j,k,qu)-vsc1(i+1,j,k)*q(i-2,j,k,qu)) &
                  +          m37*(vsc1(i-2,j,k)*q(i+2,j,k,qu)-vsc1(i+1,j,k)*q(i-3,j,k,qu)) &
                  &        + m41*(vsc1(i-1,j,k)*q(i-4,j,k,qu)-vsc1(i  ,j,k)*q(i+3,j,k,qu)) &
                  +          m42*(vsc1(i-1,j,k)*q(i-3,j,k,qu)-vsc1(i  ,j,k)*q(i+2,j,k,qu)) &
                  +          m43*(vsc1(i-1,j,k)*q(i-2,j,k,qu)-vsc1(i  ,j,k)*q(i+1,j,k,qu)) &
                  +          m44*(vsc1(i-1,j,k)*q(i-1,j,k,qu)-vsc1(i  ,j,k)*q(i  ,j,k,qu)) &
                  +          m45*(vsc1(i-1,j,k)*q(i  ,j,k,qu)-vsc1(i  ,j,k)*q(i-1,j,k,qu)) &
                  +          m46*(vsc1(i-1,j,k)*q(i+1,j,k,qu)-vsc1(i  ,j,k)*q(i-2,j,k,qu)) &
                  +          m47*(vsc1(i-1,j,k)*q(i+2,j,k,qu)-vsc1(i  ,j,k)*q(i-3,j,k,qu)) &
                  +          m48*(vsc1(i-1,j,k)*q(i+3,j,k,qu)-vsc1(i  ,j,k)*q(i-4,j,k,qu))
             
             Hg(i,j,k,imy) = m11*(mu(i-4,j,k)*q(i-4,j,k,qv)-mu(i+3,j,k)*q(i+3,j,k,qv)) &
                  +          m12*(mu(i-4,j,k)*q(i-3,j,k,qv)-mu(i+3,j,k)*q(i+2,j,k,qv)) &
                  +          m13*(mu(i-4,j,k)*q(i-2,j,k,qv)-mu(i+3,j,k)*q(i+1,j,k,qv)) &
                  +          m14*(mu(i-4,j,k)*q(i-1,j,k,qv)-mu(i+3,j,k)*q(i  ,j,k,qv)) &
                  +          m15*(mu(i-4,j,k)*q(i  ,j,k,qv)-mu(i+3,j,k)*q(i-1,j,k,qv)) &
                  &        + m21*(mu(i-3,j,k)*q(i-4,j,k,qv)-mu(i+2,j,k)*q(i+3,j,k,qv)) &
                  +          m22*(mu(i-3,j,k)*q(i-3,j,k,qv)-mu(i+2,j,k)*q(i+2,j,k,qv)) &
                  +          m23*(mu(i-3,j,k)*q(i-2,j,k,qv)-mu(i+2,j,k)*q(i+1,j,k,qv)) &
                  +          m24*(mu(i-3,j,k)*q(i-1,j,k,qv)-mu(i+2,j,k)*q(i  ,j,k,qv)) &
                  +          m25*(mu(i-3,j,k)*q(i  ,j,k,qv)-mu(i+2,j,k)*q(i-1,j,k,qv)) &
                  +          m26*(mu(i-3,j,k)*q(i+1,j,k,qv)-mu(i+2,j,k)*q(i-2,j,k,qv)) &
                  &        + m31*(mu(i-2,j,k)*q(i-4,j,k,qv)-mu(i+1,j,k)*q(i+3,j,k,qv)) &
                  +          m32*(mu(i-2,j,k)*q(i-3,j,k,qv)-mu(i+1,j,k)*q(i+2,j,k,qv)) &
                  +          m33*(mu(i-2,j,k)*q(i-2,j,k,qv)-mu(i+1,j,k)*q(i+1,j,k,qv)) &
                  +          m34*(mu(i-2,j,k)*q(i-1,j,k,qv)-mu(i+1,j,k)*q(i  ,j,k,qv)) &
                  +          m35*(mu(i-2,j,k)*q(i  ,j,k,qv)-mu(i+1,j,k)*q(i-1,j,k,qv)) &
                  +          m36*(mu(i-2,j,k)*q(i+1,j,k,qv)-mu(i+1,j,k)*q(i-2,j,k,qv)) &
                  +          m37*(mu(i-2,j,k)*q(i+2,j,k,qv)-mu(i+1,j,k)*q(i-3,j,k,qv)) &
                  &        + m41*(mu(i-1,j,k)*q(i-4,j,k,qv)-mu(i  ,j,k)*q(i+3,j,k,qv)) &
                  +          m42*(mu(i-1,j,k)*q(i-3,j,k,qv)-mu(i  ,j,k)*q(i+2,j,k,qv)) &
                  +          m43*(mu(i-1,j,k)*q(i-2,j,k,qv)-mu(i  ,j,k)*q(i+1,j,k,qv)) &
                  +          m44*(mu(i-1,j,k)*q(i-1,j,k,qv)-mu(i  ,j,k)*q(i  ,j,k,qv)) &
                  +          m45*(mu(i-1,j,k)*q(i  ,j,k,qv)-mu(i  ,j,k)*q(i-1,j,k,qv)) &
                  +          m46*(mu(i-1,j,k)*q(i+1,j,k,qv)-mu(i  ,j,k)*q(i-2,j,k,qv)) &
                  +          m47*(mu(i-1,j,k)*q(i+2,j,k,qv)-mu(i  ,j,k)*q(i-3,j,k,qv)) &
                  +          m48*(mu(i-1,j,k)*q(i+3,j,k,qv)-mu(i  ,j,k)*q(i-4,j,k,qv))

             Hg(i,j,k,imz) = m11*(mu(i-4,j,k)*q(i-4,j,k,qw)-mu(i+3,j,k)*q(i+3,j,k,qw)) &
                  +          m12*(mu(i-4,j,k)*q(i-3,j,k,qw)-mu(i+3,j,k)*q(i+2,j,k,qw)) &
                  +          m13*(mu(i-4,j,k)*q(i-2,j,k,qw)-mu(i+3,j,k)*q(i+1,j,k,qw)) &
                  +          m14*(mu(i-4,j,k)*q(i-1,j,k,qw)-mu(i+3,j,k)*q(i  ,j,k,qw)) &
                  +          m15*(mu(i-4,j,k)*q(i  ,j,k,qw)-mu(i+3,j,k)*q(i-1,j,k,qw)) &
                  &        + m21*(mu(i-3,j,k)*q(i-4,j,k,qw)-mu(i+2,j,k)*q(i+3,j,k,qw)) &
                  +          m22*(mu(i-3,j,k)*q(i-3,j,k,qw)-mu(i+2,j,k)*q(i+2,j,k,qw)) &
                  +          m23*(mu(i-3,j,k)*q(i-2,j,k,qw)-mu(i+2,j,k)*q(i+1,j,k,qw)) &
                  +          m24*(mu(i-3,j,k)*q(i-1,j,k,qw)-mu(i+2,j,k)*q(i  ,j,k,qw)) &
                  +          m25*(mu(i-3,j,k)*q(i  ,j,k,qw)-mu(i+2,j,k)*q(i-1,j,k,qw)) &
                  +          m26*(mu(i-3,j,k)*q(i+1,j,k,qw)-mu(i+2,j,k)*q(i-2,j,k,qw)) &
                  &        + m31*(mu(i-2,j,k)*q(i-4,j,k,qw)-mu(i+1,j,k)*q(i+3,j,k,qw)) &
                  +          m32*(mu(i-2,j,k)*q(i-3,j,k,qw)-mu(i+1,j,k)*q(i+2,j,k,qw)) &
                  +          m33*(mu(i-2,j,k)*q(i-2,j,k,qw)-mu(i+1,j,k)*q(i+1,j,k,qw)) &
                  +          m34*(mu(i-2,j,k)*q(i-1,j,k,qw)-mu(i+1,j,k)*q(i  ,j,k,qw)) &
                  +          m35*(mu(i-2,j,k)*q(i  ,j,k,qw)-mu(i+1,j,k)*q(i-1,j,k,qw)) &
                  +          m36*(mu(i-2,j,k)*q(i+1,j,k,qw)-mu(i+1,j,k)*q(i-2,j,k,qw)) &
                  +          m37*(mu(i-2,j,k)*q(i+2,j,k,qw)-mu(i+1,j,k)*q(i-3,j,k,qw)) &
                  &        + m41*(mu(i-1,j,k)*q(i-4,j,k,qw)-mu(i  ,j,k)*q(i+3,j,k,qw)) &
                  +          m42*(mu(i-1,j,k)*q(i-3,j,k,qw)-mu(i  ,j,k)*q(i+2,j,k,qw)) &
                  +          m43*(mu(i-1,j,k)*q(i-2,j,k,qw)-mu(i  ,j,k)*q(i+1,j,k,qw)) &
                  +          m44*(mu(i-1,j,k)*q(i-1,j,k,qw)-mu(i  ,j,k)*q(i  ,j,k,qw)) &
                  +          m45*(mu(i-1,j,k)*q(i  ,j,k,qw)-mu(i  ,j,k)*q(i-1,j,k,qw)) &
                  +          m46*(mu(i-1,j,k)*q(i+1,j,k,qw)-mu(i  ,j,k)*q(i-2,j,k,qw)) &
                  +          m47*(mu(i-1,j,k)*q(i+2,j,k,qw)-mu(i  ,j,k)*q(i-3,j,k,qw)) &
                  +          m48*(mu(i-1,j,k)*q(i+3,j,k,qw)-mu(i  ,j,k)*q(i-4,j,k,qw))

             Hg(i,j,k,iene) = m11*(lam(i-4,j,k)*q(i-4,j,k,qtemp)-lam(i+3,j,k)*q(i+3,j,k,qtemp)) &
                  +           m12*(lam(i-4,j,k)*q(i-3,j,k,qtemp)-lam(i+3,j,k)*q(i+2,j,k,qtemp)) &
                  +           m13*(lam(i-4,j,k)*q(i-2,j,k,qtemp)-lam(i+3,j,k)*q(i+1,j,k,qtemp)) &
                  +           m14*(lam(i-4,j,k)*q(i-1,j,k,qtemp)-lam(i+3,j,k)*q(i  ,j,k,qtemp)) &
                  +           m15*(lam(i-4,j,k)*q(i  ,j,k,qtemp)-lam(i+3,j,k)*q(i-1,j,k,qtemp)) &
                  &         + m21*(lam(i-3,j,k)*q(i-4,j,k,qtemp)-lam(i+2,j,k)*q(i+3,j,k,qtemp)) &
                  +           m22*(lam(i-3,j,k)*q(i-3,j,k,qtemp)-lam(i+2,j,k)*q(i+2,j,k,qtemp)) &
                  +           m23*(lam(i-3,j,k)*q(i-2,j,k,qtemp)-lam(i+2,j,k)*q(i+1,j,k,qtemp)) &
                  +           m24*(lam(i-3,j,k)*q(i-1,j,k,qtemp)-lam(i+2,j,k)*q(i  ,j,k,qtemp)) &
                  +           m25*(lam(i-3,j,k)*q(i  ,j,k,qtemp)-lam(i+2,j,k)*q(i-1,j,k,qtemp)) &
                  +           m26*(lam(i-3,j,k)*q(i+1,j,k,qtemp)-lam(i+2,j,k)*q(i-2,j,k,qtemp)) &
                  &         + m31*(lam(i-2,j,k)*q(i-4,j,k,qtemp)-lam(i+1,j,k)*q(i+3,j,k,qtemp)) &
                  +           m32*(lam(i-2,j,k)*q(i-3,j,k,qtemp)-lam(i+1,j,k)*q(i+2,j,k,qtemp)) &
                  +           m33*(lam(i-2,j,k)*q(i-2,j,k,qtemp)-lam(i+1,j,k)*q(i+1,j,k,qtemp)) &
                  +           m34*(lam(i-2,j,k)*q(i-1,j,k,qtemp)-lam(i+1,j,k)*q(i  ,j,k,qtemp)) &
                  +           m35*(lam(i-2,j,k)*q(i  ,j,k,qtemp)-lam(i+1,j,k)*q(i-1,j,k,qtemp)) &
                  +           m36*(lam(i-2,j,k)*q(i+1,j,k,qtemp)-lam(i+1,j,k)*q(i-2,j,k,qtemp)) &
                  +           m37*(lam(i-2,j,k)*q(i+2,j,k,qtemp)-lam(i+1,j,k)*q(i-3,j,k,qtemp)) &
                  &         + m41*(lam(i-1,j,k)*q(i-4,j,k,qtemp)-lam(i  ,j,k)*q(i+3,j,k,qtemp)) &
                  +           m42*(lam(i-1,j,k)*q(i-3,j,k,qtemp)-lam(i  ,j,k)*q(i+2,j,k,qtemp)) &
                  +           m43*(lam(i-1,j,k)*q(i-2,j,k,qtemp)-lam(i  ,j,k)*q(i+1,j,k,qtemp)) &
                  +           m44*(lam(i-1,j,k)*q(i-1,j,k,qtemp)-lam(i  ,j,k)*q(i  ,j,k,qtemp)) &
                  +           m45*(lam(i-1,j,k)*q(i  ,j,k,qtemp)-lam(i  ,j,k)*q(i-1,j,k,qtemp)) &
                  +           m46*(lam(i-1,j,k)*q(i+1,j,k,qtemp)-lam(i  ,j,k)*q(i-2,j,k,qtemp)) &
                  +           m47*(lam(i-1,j,k)*q(i+2,j,k,qtemp)-lam(i  ,j,k)*q(i-3,j,k,qtemp)) &
                  +           m48*(lam(i-1,j,k)*q(i+3,j,k,qtemp)-lam(i  ,j,k)*q(i-4,j,k,qtemp))

             Htot = 0.d0
             do n = 1, nspecies
                qxn = qx1+n-1
                qyn = qy1+n-1
                Htmp(n) = m11*(dcx(i-4,j,k,n)*q(i-4,j,k,qxn)-dcx(i+3,j,k,n)*q(i+3,j,k,qxn)) &
                  +       m12*(dcx(i-4,j,k,n)*q(i-3,j,k,qxn)-dcx(i+3,j,k,n)*q(i+2,j,k,qxn)) &
                  +       m13*(dcx(i-4,j,k,n)*q(i-2,j,k,qxn)-dcx(i+3,j,k,n)*q(i+1,j,k,qxn)) &
                  +       m14*(dcx(i-4,j,k,n)*q(i-1,j,k,qxn)-dcx(i+3,j,k,n)*q(i  ,j,k,qxn)) &
                  +       m15*(dcx(i-4,j,k,n)*q(i  ,j,k,qxn)-dcx(i+3,j,k,n)*q(i-1,j,k,qxn)) &
                  &     + m21*(dcx(i-3,j,k,n)*q(i-4,j,k,qxn)-dcx(i+2,j,k,n)*q(i+3,j,k,qxn)) &
                  +       m22*(dcx(i-3,j,k,n)*q(i-3,j,k,qxn)-dcx(i+2,j,k,n)*q(i+2,j,k,qxn)) &
                  +       m23*(dcx(i-3,j,k,n)*q(i-2,j,k,qxn)-dcx(i+2,j,k,n)*q(i+1,j,k,qxn)) &
                  +       m24*(dcx(i-3,j,k,n)*q(i-1,j,k,qxn)-dcx(i+2,j,k,n)*q(i  ,j,k,qxn)) &
                  +       m25*(dcx(i-3,j,k,n)*q(i  ,j,k,qxn)-dcx(i+2,j,k,n)*q(i-1,j,k,qxn)) &
                  +       m26*(dcx(i-3,j,k,n)*q(i+1,j,k,qxn)-dcx(i+2,j,k,n)*q(i-2,j,k,qxn)) &
                  &     + m31*(dcx(i-2,j,k,n)*q(i-4,j,k,qxn)-dcx(i+1,j,k,n)*q(i+3,j,k,qxn)) &
                  +       m32*(dcx(i-2,j,k,n)*q(i-3,j,k,qxn)-dcx(i+1,j,k,n)*q(i+2,j,k,qxn)) &
                  +       m33*(dcx(i-2,j,k,n)*q(i-2,j,k,qxn)-dcx(i+1,j,k,n)*q(i+1,j,k,qxn)) &
                  +       m34*(dcx(i-2,j,k,n)*q(i-1,j,k,qxn)-dcx(i+1,j,k,n)*q(i  ,j,k,qxn)) &
                  +       m35*(dcx(i-2,j,k,n)*q(i  ,j,k,qxn)-dcx(i+1,j,k,n)*q(i-1,j,k,qxn)) &
                  +       m36*(dcx(i-2,j,k,n)*q(i+1,j,k,qxn)-dcx(i+1,j,k,n)*q(i-2,j,k,qxn)) &
                  +       m37*(dcx(i-2,j,k,n)*q(i+2,j,k,qxn)-dcx(i+1,j,k,n)*q(i-3,j,k,qxn)) &
                  &     + m41*(dcx(i-1,j,k,n)*q(i-4,j,k,qxn)-dcx(i  ,j,k,n)*q(i+3,j,k,qxn)) &
                  +       m42*(dcx(i-1,j,k,n)*q(i-3,j,k,qxn)-dcx(i  ,j,k,n)*q(i+2,j,k,qxn)) &
                  +       m43*(dcx(i-1,j,k,n)*q(i-2,j,k,qxn)-dcx(i  ,j,k,n)*q(i+1,j,k,qxn)) &
                  +       m44*(dcx(i-1,j,k,n)*q(i-1,j,k,qxn)-dcx(i  ,j,k,n)*q(i  ,j,k,qxn)) &
                  +       m45*(dcx(i-1,j,k,n)*q(i  ,j,k,qxn)-dcx(i  ,j,k,n)*q(i-1,j,k,qxn)) &
                  +       m46*(dcx(i-1,j,k,n)*q(i+1,j,k,qxn)-dcx(i  ,j,k,n)*q(i-2,j,k,qxn)) &
                  +       m47*(dcx(i-1,j,k,n)*q(i+2,j,k,qxn)-dcx(i  ,j,k,n)*q(i-3,j,k,qxn)) &
                  +       m48*(dcx(i-1,j,k,n)*q(i+3,j,k,qxn)-dcx(i  ,j,k,n)*q(i-4,j,k,qxn))
                Htmp(n) = Htmp(n)  &
                  +       m11*(dcp(i-4,j,k,n)*q(i-4,j,k,qpres)-dcp(i+3,j,k,n)*q(i+3,j,k,qpres)) &
                  +       m12*(dcp(i-4,j,k,n)*q(i-3,j,k,qpres)-dcp(i+3,j,k,n)*q(i+2,j,k,qpres)) &
                  +       m13*(dcp(i-4,j,k,n)*q(i-2,j,k,qpres)-dcp(i+3,j,k,n)*q(i+1,j,k,qpres)) &
                  +       m14*(dcp(i-4,j,k,n)*q(i-1,j,k,qpres)-dcp(i+3,j,k,n)*q(i  ,j,k,qpres)) &
                  +       m15*(dcp(i-4,j,k,n)*q(i  ,j,k,qpres)-dcp(i+3,j,k,n)*q(i-1,j,k,qpres)) &
                  &     + m21*(dcp(i-3,j,k,n)*q(i-4,j,k,qpres)-dcp(i+2,j,k,n)*q(i+3,j,k,qpres)) &
                  +       m22*(dcp(i-3,j,k,n)*q(i-3,j,k,qpres)-dcp(i+2,j,k,n)*q(i+2,j,k,qpres)) &
                  +       m23*(dcp(i-3,j,k,n)*q(i-2,j,k,qpres)-dcp(i+2,j,k,n)*q(i+1,j,k,qpres)) &
                  +       m24*(dcp(i-3,j,k,n)*q(i-1,j,k,qpres)-dcp(i+2,j,k,n)*q(i  ,j,k,qpres)) &
                  +       m25*(dcp(i-3,j,k,n)*q(i  ,j,k,qpres)-dcp(i+2,j,k,n)*q(i-1,j,k,qpres)) &
                  +       m26*(dcp(i-3,j,k,n)*q(i+1,j,k,qpres)-dcp(i+2,j,k,n)*q(i-2,j,k,qpres)) &
                  &     + m31*(dcp(i-2,j,k,n)*q(i-4,j,k,qpres)-dcp(i+1,j,k,n)*q(i+3,j,k,qpres)) &
                  +       m32*(dcp(i-2,j,k,n)*q(i-3,j,k,qpres)-dcp(i+1,j,k,n)*q(i+2,j,k,qpres)) &
                  +       m33*(dcp(i-2,j,k,n)*q(i-2,j,k,qpres)-dcp(i+1,j,k,n)*q(i+1,j,k,qpres)) &
                  +       m34*(dcp(i-2,j,k,n)*q(i-1,j,k,qpres)-dcp(i+1,j,k,n)*q(i  ,j,k,qpres)) &
                  +       m35*(dcp(i-2,j,k,n)*q(i  ,j,k,qpres)-dcp(i+1,j,k,n)*q(i-1,j,k,qpres)) &
                  +       m36*(dcp(i-2,j,k,n)*q(i+1,j,k,qpres)-dcp(i+1,j,k,n)*q(i-2,j,k,qpres)) &
                  +       m37*(dcp(i-2,j,k,n)*q(i+2,j,k,qpres)-dcp(i+1,j,k,n)*q(i-3,j,k,qpres)) &
                  &     + m41*(dcp(i-1,j,k,n)*q(i-4,j,k,qpres)-dcp(i  ,j,k,n)*q(i+3,j,k,qpres)) &
                  +       m42*(dcp(i-1,j,k,n)*q(i-3,j,k,qpres)-dcp(i  ,j,k,n)*q(i+2,j,k,qpres)) &
                  +       m43*(dcp(i-1,j,k,n)*q(i-2,j,k,qpres)-dcp(i  ,j,k,n)*q(i+1,j,k,qpres)) &
                  +       m44*(dcp(i-1,j,k,n)*q(i-1,j,k,qpres)-dcp(i  ,j,k,n)*q(i  ,j,k,qpres)) &
                  +       m45*(dcp(i-1,j,k,n)*q(i  ,j,k,qpres)-dcp(i  ,j,k,n)*q(i-1,j,k,qpres)) &
                  +       m46*(dcp(i-1,j,k,n)*q(i+1,j,k,qpres)-dcp(i  ,j,k,n)*q(i-2,j,k,qpres)) &
                  +       m47*(dcp(i-1,j,k,n)*q(i+2,j,k,qpres)-dcp(i  ,j,k,n)*q(i-3,j,k,qpres)) &
                  +       m48*(dcp(i-1,j,k,n)*q(i+3,j,k,qpres)-dcp(i  ,j,k,n)*q(i-4,j,k,qpres))
                Htot = Htot + Htmp(n)
                Ytmp(n) = (q(i-1,j,k,qyn) + q(i,j,k,qyn)) / 2.d0
             end do

             do n = 1, nspecies
                Hg(i,j,k,iry1+n-1) = Htmp(n) - Ytmp(n)*Htot
             end do

             do n = 1, nspecies
                qxn = qx1+n-1
                qyn = qy1+n-1
                qhn = qh1+n-1
                Hg(i,j,k,iene) =  Hg(i,j,k,iene) &
                  + m11*(dcx(i-4,j,k,n)*q(i-4,j,k,qhn)*q(i-4,j,k,qxn)-dcx(i+3,j,k,n)*q(i+3,j,k,qhn)*q(i+3,j,k,qxn)) &
                  + m12*(dcx(i-4,j,k,n)*q(i-4,j,k,qhn)*q(i-3,j,k,qxn)-dcx(i+3,j,k,n)*q(i+3,j,k,qhn)*q(i+2,j,k,qxn)) &
                  + m13*(dcx(i-4,j,k,n)*q(i-4,j,k,qhn)*q(i-2,j,k,qxn)-dcx(i+3,j,k,n)*q(i+3,j,k,qhn)*q(i+1,j,k,qxn)) &
                  + m14*(dcx(i-4,j,k,n)*q(i-4,j,k,qhn)*q(i-1,j,k,qxn)-dcx(i+3,j,k,n)*q(i+3,j,k,qhn)*q(i  ,j,k,qxn)) &
                  + m15*(dcx(i-4,j,k,n)*q(i-4,j,k,qhn)*q(i  ,j,k,qxn)-dcx(i+3,j,k,n)*q(i+3,j,k,qhn)*q(i-1,j,k,qxn)) &
                  + m21*(dcx(i-3,j,k,n)*q(i-3,j,k,qhn)*q(i-4,j,k,qxn)-dcx(i+2,j,k,n)*q(i+2,j,k,qhn)*q(i+3,j,k,qxn)) &
                  + m22*(dcx(i-3,j,k,n)*q(i-3,j,k,qhn)*q(i-3,j,k,qxn)-dcx(i+2,j,k,n)*q(i+2,j,k,qhn)*q(i+2,j,k,qxn)) &
                  + m23*(dcx(i-3,j,k,n)*q(i-3,j,k,qhn)*q(i-2,j,k,qxn)-dcx(i+2,j,k,n)*q(i+2,j,k,qhn)*q(i+1,j,k,qxn)) &
                  + m24*(dcx(i-3,j,k,n)*q(i-3,j,k,qhn)*q(i-1,j,k,qxn)-dcx(i+2,j,k,n)*q(i+2,j,k,qhn)*q(i  ,j,k,qxn)) &
                  + m25*(dcx(i-3,j,k,n)*q(i-3,j,k,qhn)*q(i  ,j,k,qxn)-dcx(i+2,j,k,n)*q(i+2,j,k,qhn)*q(i-1,j,k,qxn)) &
                  + m26*(dcx(i-3,j,k,n)*q(i-3,j,k,qhn)*q(i+1,j,k,qxn)-dcx(i+2,j,k,n)*q(i+2,j,k,qhn)*q(i-2,j,k,qxn)) &
                  + m31*(dcx(i-2,j,k,n)*q(i-2,j,k,qhn)*q(i-4,j,k,qxn)-dcx(i+1,j,k,n)*q(i+1,j,k,qhn)*q(i+3,j,k,qxn)) &
                  + m32*(dcx(i-2,j,k,n)*q(i-2,j,k,qhn)*q(i-3,j,k,qxn)-dcx(i+1,j,k,n)*q(i+1,j,k,qhn)*q(i+2,j,k,qxn)) &
                  + m33*(dcx(i-2,j,k,n)*q(i-2,j,k,qhn)*q(i-2,j,k,qxn)-dcx(i+1,j,k,n)*q(i+1,j,k,qhn)*q(i+1,j,k,qxn)) &
                  + m34*(dcx(i-2,j,k,n)*q(i-2,j,k,qhn)*q(i-1,j,k,qxn)-dcx(i+1,j,k,n)*q(i+1,j,k,qhn)*q(i  ,j,k,qxn)) &
                  + m35*(dcx(i-2,j,k,n)*q(i-2,j,k,qhn)*q(i  ,j,k,qxn)-dcx(i+1,j,k,n)*q(i+1,j,k,qhn)*q(i-1,j,k,qxn)) &
                  + m36*(dcx(i-2,j,k,n)*q(i-2,j,k,qhn)*q(i+1,j,k,qxn)-dcx(i+1,j,k,n)*q(i+1,j,k,qhn)*q(i-2,j,k,qxn)) &
                  + m37*(dcx(i-2,j,k,n)*q(i-2,j,k,qhn)*q(i+2,j,k,qxn)-dcx(i+1,j,k,n)*q(i+1,j,k,qhn)*q(i-3,j,k,qxn)) &
                  + m41*(dcx(i-1,j,k,n)*q(i-1,j,k,qhn)*q(i-4,j,k,qxn)-dcx(i  ,j,k,n)*q(i  ,j,k,qhn)*q(i+3,j,k,qxn)) &
                  + m42*(dcx(i-1,j,k,n)*q(i-1,j,k,qhn)*q(i-3,j,k,qxn)-dcx(i  ,j,k,n)*q(i  ,j,k,qhn)*q(i+2,j,k,qxn)) &
                  + m43*(dcx(i-1,j,k,n)*q(i-1,j,k,qhn)*q(i-2,j,k,qxn)-dcx(i  ,j,k,n)*q(i  ,j,k,qhn)*q(i+1,j,k,qxn)) &
                  + m44*(dcx(i-1,j,k,n)*q(i-1,j,k,qhn)*q(i-1,j,k,qxn)-dcx(i  ,j,k,n)*q(i  ,j,k,qhn)*q(i  ,j,k,qxn)) &
                  + m45*(dcx(i-1,j,k,n)*q(i-1,j,k,qhn)*q(i  ,j,k,qxn)-dcx(i  ,j,k,n)*q(i  ,j,k,qhn)*q(i-1,j,k,qxn)) &
                  + m46*(dcx(i-1,j,k,n)*q(i-1,j,k,qhn)*q(i+1,j,k,qxn)-dcx(i  ,j,k,n)*q(i  ,j,k,qhn)*q(i-2,j,k,qxn)) &
                  + m47*(dcx(i-1,j,k,n)*q(i-1,j,k,qhn)*q(i+2,j,k,qxn)-dcx(i  ,j,k,n)*q(i  ,j,k,qhn)*q(i-3,j,k,qxn)) &
                  + m48*(dcx(i-1,j,k,n)*q(i-1,j,k,qhn)*q(i+3,j,k,qxn)-dcx(i  ,j,k,n)*q(i  ,j,k,qhn)*q(i-4,j,k,qxn))
                Hg(i,j,k,iene) =  Hg(i,j,k,iene) &
                  + m11*(dcp(i-4,j,k,n)*q(i-4,j,k,qhn)*q(i-4,j,k,qpres)-dcp(i+3,j,k,n)*q(i+3,j,k,qhn)*q(i+3,j,k,qpres)) &
                  + m12*(dcp(i-4,j,k,n)*q(i-4,j,k,qhn)*q(i-3,j,k,qpres)-dcp(i+3,j,k,n)*q(i+3,j,k,qhn)*q(i+2,j,k,qpres)) &
                  + m13*(dcp(i-4,j,k,n)*q(i-4,j,k,qhn)*q(i-2,j,k,qpres)-dcp(i+3,j,k,n)*q(i+3,j,k,qhn)*q(i+1,j,k,qpres)) &
                  + m14*(dcp(i-4,j,k,n)*q(i-4,j,k,qhn)*q(i-1,j,k,qpres)-dcp(i+3,j,k,n)*q(i+3,j,k,qhn)*q(i  ,j,k,qpres)) &
                  + m15*(dcp(i-4,j,k,n)*q(i-4,j,k,qhn)*q(i  ,j,k,qpres)-dcp(i+3,j,k,n)*q(i+3,j,k,qhn)*q(i-1,j,k,qpres)) &
                  + m21*(dcp(i-3,j,k,n)*q(i-3,j,k,qhn)*q(i-4,j,k,qpres)-dcp(i+2,j,k,n)*q(i+2,j,k,qhn)*q(i+3,j,k,qpres)) &
                  + m22*(dcp(i-3,j,k,n)*q(i-3,j,k,qhn)*q(i-3,j,k,qpres)-dcp(i+2,j,k,n)*q(i+2,j,k,qhn)*q(i+2,j,k,qpres)) &
                  + m23*(dcp(i-3,j,k,n)*q(i-3,j,k,qhn)*q(i-2,j,k,qpres)-dcp(i+2,j,k,n)*q(i+2,j,k,qhn)*q(i+1,j,k,qpres)) &
                  + m24*(dcp(i-3,j,k,n)*q(i-3,j,k,qhn)*q(i-1,j,k,qpres)-dcp(i+2,j,k,n)*q(i+2,j,k,qhn)*q(i  ,j,k,qpres)) &
                  + m25*(dcp(i-3,j,k,n)*q(i-3,j,k,qhn)*q(i  ,j,k,qpres)-dcp(i+2,j,k,n)*q(i+2,j,k,qhn)*q(i-1,j,k,qpres)) &
                  + m26*(dcp(i-3,j,k,n)*q(i-3,j,k,qhn)*q(i+1,j,k,qpres)-dcp(i+2,j,k,n)*q(i+2,j,k,qhn)*q(i-2,j,k,qpres)) &
                  + m31*(dcp(i-2,j,k,n)*q(i-2,j,k,qhn)*q(i-4,j,k,qpres)-dcp(i+1,j,k,n)*q(i+1,j,k,qhn)*q(i+3,j,k,qpres)) &
                  + m32*(dcp(i-2,j,k,n)*q(i-2,j,k,qhn)*q(i-3,j,k,qpres)-dcp(i+1,j,k,n)*q(i+1,j,k,qhn)*q(i+2,j,k,qpres)) &
                  + m33*(dcp(i-2,j,k,n)*q(i-2,j,k,qhn)*q(i-2,j,k,qpres)-dcp(i+1,j,k,n)*q(i+1,j,k,qhn)*q(i+1,j,k,qpres)) &
                  + m34*(dcp(i-2,j,k,n)*q(i-2,j,k,qhn)*q(i-1,j,k,qpres)-dcp(i+1,j,k,n)*q(i+1,j,k,qhn)*q(i  ,j,k,qpres)) &
                  + m35*(dcp(i-2,j,k,n)*q(i-2,j,k,qhn)*q(i  ,j,k,qpres)-dcp(i+1,j,k,n)*q(i+1,j,k,qhn)*q(i-1,j,k,qpres)) &
                  + m36*(dcp(i-2,j,k,n)*q(i-2,j,k,qhn)*q(i+1,j,k,qpres)-dcp(i+1,j,k,n)*q(i+1,j,k,qhn)*q(i-2,j,k,qpres)) &
                  + m37*(dcp(i-2,j,k,n)*q(i-2,j,k,qhn)*q(i+2,j,k,qpres)-dcp(i+1,j,k,n)*q(i+1,j,k,qhn)*q(i-3,j,k,qpres)) &
                  + m41*(dcp(i-1,j,k,n)*q(i-1,j,k,qhn)*q(i-4,j,k,qpres)-dcp(i  ,j,k,n)*q(i  ,j,k,qhn)*q(i+3,j,k,qpres)) &
                  + m42*(dcp(i-1,j,k,n)*q(i-1,j,k,qhn)*q(i-3,j,k,qpres)-dcp(i  ,j,k,n)*q(i  ,j,k,qhn)*q(i+2,j,k,qpres)) &
                  + m43*(dcp(i-1,j,k,n)*q(i-1,j,k,qhn)*q(i-2,j,k,qpres)-dcp(i  ,j,k,n)*q(i  ,j,k,qhn)*q(i+1,j,k,qpres)) &
                  + m44*(dcp(i-1,j,k,n)*q(i-1,j,k,qhn)*q(i-1,j,k,qpres)-dcp(i  ,j,k,n)*q(i  ,j,k,qhn)*q(i  ,j,k,qpres)) &
                  + m45*(dcp(i-1,j,k,n)*q(i-1,j,k,qhn)*q(i  ,j,k,qpres)-dcp(i  ,j,k,n)*q(i  ,j,k,qhn)*q(i-1,j,k,qpres)) &
                  + m46*(dcp(i-1,j,k,n)*q(i-1,j,k,qhn)*q(i+1,j,k,qpres)-dcp(i  ,j,k,n)*q(i  ,j,k,qhn)*q(i-2,j,k,qpres)) &
                  + m47*(dcp(i-1,j,k,n)*q(i-1,j,k,qhn)*q(i+2,j,k,qpres)-dcp(i  ,j,k,n)*q(i  ,j,k,qhn)*q(i-3,j,k,qpres)) &
                  + m48*(dcp(i-1,j,k,n)*q(i-1,j,k,qhn)*q(i+3,j,k,qpres)-dcp(i  ,j,k,n)*q(i  ,j,k,qhn)*q(i-4,j,k,qpres))
             end do

          end do
       end do
    end do

    ! add x-direction flux
    do k=lo(3),hi(3)
       do j=lo(2),hi(2)
          do i=lo(1),hi(1)
             do n=imx,ncons
                flx(i,j,k,n) = flx(i,j,k,n) + (Hg(i+1,j,k,n) - Hg(i,j,k,n)) * dx2inv(1)
             end do
          end do
       end do
    end do
    ! ------- END x-direction -------

    ! ------- BEGIN y-direction -------
    do k=lo(3),hi(3)
       do j=lo(2),hi(2)+1
          do i=lo(1),hi(1)
             Hg(i,j,k,imx) = m11*(mu(i,j-4,k)*q(i,j-4,k,qu)-mu(i,j+3,k)*q(i,j+3,k,qu)) &
                  +          m12*(mu(i,j-4,k)*q(i,j-3,k,qu)-mu(i,j+3,k)*q(i,j+2,k,qu)) &
                  +          m13*(mu(i,j-4,k)*q(i,j-2,k,qu)-mu(i,j+3,k)*q(i,j+1,k,qu)) &
                  +          m14*(mu(i,j-4,k)*q(i,j-1,k,qu)-mu(i,j+3,k)*q(i,j  ,k,qu)) &
                  +          m15*(mu(i,j-4,k)*q(i,j  ,k,qu)-mu(i,j+3,k)*q(i,j-1,k,qu)) &
                  &        + m21*(mu(i,j-3,k)*q(i,j-4,k,qu)-mu(i,j+2,k)*q(i,j+3,k,qu)) &
                  +          m22*(mu(i,j-3,k)*q(i,j-3,k,qu)-mu(i,j+2,k)*q(i,j+2,k,qu)) &
                  +          m23*(mu(i,j-3,k)*q(i,j-2,k,qu)-mu(i,j+2,k)*q(i,j+1,k,qu)) &
                  +          m24*(mu(i,j-3,k)*q(i,j-1,k,qu)-mu(i,j+2,k)*q(i,j  ,k,qu)) &
                  +          m25*(mu(i,j-3,k)*q(i,j  ,k,qu)-mu(i,j+2,k)*q(i,j-1,k,qu)) &
                  +          m26*(mu(i,j-3,k)*q(i,j+1,k,qu)-mu(i,j+2,k)*q(i,j-2,k,qu)) &
                  &        + m31*(mu(i,j-2,k)*q(i,j-4,k,qu)-mu(i,j+1,k)*q(i,j+3,k,qu)) &
                  +          m32*(mu(i,j-2,k)*q(i,j-3,k,qu)-mu(i,j+1,k)*q(i,j+2,k,qu)) &
                  +          m33*(mu(i,j-2,k)*q(i,j-2,k,qu)-mu(i,j+1,k)*q(i,j+1,k,qu)) &
                  +          m34*(mu(i,j-2,k)*q(i,j-1,k,qu)-mu(i,j+1,k)*q(i,j  ,k,qu)) &
                  +          m35*(mu(i,j-2,k)*q(i,j  ,k,qu)-mu(i,j+1,k)*q(i,j-1,k,qu)) &
                  +          m36*(mu(i,j-2,k)*q(i,j+1,k,qu)-mu(i,j+1,k)*q(i,j-2,k,qu)) &
                  +          m37*(mu(i,j-2,k)*q(i,j+2,k,qu)-mu(i,j+1,k)*q(i,j-3,k,qu)) &
                  &        + m41*(mu(i,j-1,k)*q(i,j-4,k,qu)-mu(i,j  ,k)*q(i,j+3,k,qu)) &
                  +          m42*(mu(i,j-1,k)*q(i,j-3,k,qu)-mu(i,j  ,k)*q(i,j+2,k,qu)) &
                  +          m43*(mu(i,j-1,k)*q(i,j-2,k,qu)-mu(i,j  ,k)*q(i,j+1,k,qu)) &
                  +          m44*(mu(i,j-1,k)*q(i,j-1,k,qu)-mu(i,j  ,k)*q(i,j  ,k,qu)) &
                  +          m45*(mu(i,j-1,k)*q(i,j  ,k,qu)-mu(i,j  ,k)*q(i,j-1,k,qu)) &
                  +          m46*(mu(i,j-1,k)*q(i,j+1,k,qu)-mu(i,j  ,k)*q(i,j-2,k,qu)) &
                  +          m47*(mu(i,j-1,k)*q(i,j+2,k,qu)-mu(i,j  ,k)*q(i,j-3,k,qu)) &
                  +          m48*(mu(i,j-1,k)*q(i,j+3,k,qu)-mu(i,j  ,k)*q(i,j-4,k,qu))

             Hg(i,j,k,imy) = m11*(vsc1(i,j-4,k)*q(i,j-4,k,qv)-vsc1(i,j+3,k)*q(i,j+3,k,qv)) &
                  +          m12*(vsc1(i,j-4,k)*q(i,j-3,k,qv)-vsc1(i,j+3,k)*q(i,j+2,k,qv)) &
                  +          m13*(vsc1(i,j-4,k)*q(i,j-2,k,qv)-vsc1(i,j+3,k)*q(i,j+1,k,qv)) &
                  +          m14*(vsc1(i,j-4,k)*q(i,j-1,k,qv)-vsc1(i,j+3,k)*q(i,j  ,k,qv)) &
                  +          m15*(vsc1(i,j-4,k)*q(i,j  ,k,qv)-vsc1(i,j+3,k)*q(i,j-1,k,qv)) &
                  &        + m21*(vsc1(i,j-3,k)*q(i,j-4,k,qv)-vsc1(i,j+2,k)*q(i,j+3,k,qv)) &
                  +          m22*(vsc1(i,j-3,k)*q(i,j-3,k,qv)-vsc1(i,j+2,k)*q(i,j+2,k,qv)) &
                  +          m23*(vsc1(i,j-3,k)*q(i,j-2,k,qv)-vsc1(i,j+2,k)*q(i,j+1,k,qv)) &
                  +          m24*(vsc1(i,j-3,k)*q(i,j-1,k,qv)-vsc1(i,j+2,k)*q(i,j  ,k,qv)) &
                  +          m25*(vsc1(i,j-3,k)*q(i,j  ,k,qv)-vsc1(i,j+2,k)*q(i,j-1,k,qv)) &
                  +          m26*(vsc1(i,j-3,k)*q(i,j+1,k,qv)-vsc1(i,j+2,k)*q(i,j-2,k,qv)) &
                  &        + m31*(vsc1(i,j-2,k)*q(i,j-4,k,qv)-vsc1(i,j+1,k)*q(i,j+3,k,qv)) &
                  +          m32*(vsc1(i,j-2,k)*q(i,j-3,k,qv)-vsc1(i,j+1,k)*q(i,j+2,k,qv)) &
                  +          m33*(vsc1(i,j-2,k)*q(i,j-2,k,qv)-vsc1(i,j+1,k)*q(i,j+1,k,qv)) &
                  +          m34*(vsc1(i,j-2,k)*q(i,j-1,k,qv)-vsc1(i,j+1,k)*q(i,j  ,k,qv)) &
                  +          m35*(vsc1(i,j-2,k)*q(i,j  ,k,qv)-vsc1(i,j+1,k)*q(i,j-1,k,qv)) &
                  +          m36*(vsc1(i,j-2,k)*q(i,j+1,k,qv)-vsc1(i,j+1,k)*q(i,j-2,k,qv)) &
                  +          m37*(vsc1(i,j-2,k)*q(i,j+2,k,qv)-vsc1(i,j+1,k)*q(i,j-3,k,qv)) &
                  &        + m41*(vsc1(i,j-1,k)*q(i,j-4,k,qv)-vsc1(i,j  ,k)*q(i,j+3,k,qv)) &
                  +          m42*(vsc1(i,j-1,k)*q(i,j-3,k,qv)-vsc1(i,j  ,k)*q(i,j+2,k,qv)) &
                  +          m43*(vsc1(i,j-1,k)*q(i,j-2,k,qv)-vsc1(i,j  ,k)*q(i,j+1,k,qv)) &
                  +          m44*(vsc1(i,j-1,k)*q(i,j-1,k,qv)-vsc1(i,j  ,k)*q(i,j  ,k,qv)) &
                  +          m45*(vsc1(i,j-1,k)*q(i,j  ,k,qv)-vsc1(i,j  ,k)*q(i,j-1,k,qv)) &
                  +          m46*(vsc1(i,j-1,k)*q(i,j+1,k,qv)-vsc1(i,j  ,k)*q(i,j-2,k,qv)) &
                  +          m47*(vsc1(i,j-1,k)*q(i,j+2,k,qv)-vsc1(i,j  ,k)*q(i,j-3,k,qv)) &
                  +          m48*(vsc1(i,j-1,k)*q(i,j+3,k,qv)-vsc1(i,j  ,k)*q(i,j-4,k,qv))

             Hg(i,j,k,imz) = m11*(mu(i,j-4,k)*q(i,j-4,k,qw)-mu(i,j+3,k)*q(i,j+3,k,qw)) &
                  +          m12*(mu(i,j-4,k)*q(i,j-3,k,qw)-mu(i,j+3,k)*q(i,j+2,k,qw)) &
                  +          m13*(mu(i,j-4,k)*q(i,j-2,k,qw)-mu(i,j+3,k)*q(i,j+1,k,qw)) &
                  +          m14*(mu(i,j-4,k)*q(i,j-1,k,qw)-mu(i,j+3,k)*q(i,j  ,k,qw)) &
                  +          m15*(mu(i,j-4,k)*q(i,j  ,k,qw)-mu(i,j+3,k)*q(i,j-1,k,qw)) &
                  &        + m21*(mu(i,j-3,k)*q(i,j-4,k,qw)-mu(i,j+2,k)*q(i,j+3,k,qw)) &
                  +          m22*(mu(i,j-3,k)*q(i,j-3,k,qw)-mu(i,j+2,k)*q(i,j+2,k,qw)) &
                  +          m23*(mu(i,j-3,k)*q(i,j-2,k,qw)-mu(i,j+2,k)*q(i,j+1,k,qw)) &
                  +          m24*(mu(i,j-3,k)*q(i,j-1,k,qw)-mu(i,j+2,k)*q(i,j  ,k,qw)) &
                  +          m25*(mu(i,j-3,k)*q(i,j  ,k,qw)-mu(i,j+2,k)*q(i,j-1,k,qw)) &
                  +          m26*(mu(i,j-3,k)*q(i,j+1,k,qw)-mu(i,j+2,k)*q(i,j-2,k,qw)) &
                  &        + m31*(mu(i,j-2,k)*q(i,j-4,k,qw)-mu(i,j+1,k)*q(i,j+3,k,qw)) &
                  +          m32*(mu(i,j-2,k)*q(i,j-3,k,qw)-mu(i,j+1,k)*q(i,j+2,k,qw)) &
                  +          m33*(mu(i,j-2,k)*q(i,j-2,k,qw)-mu(i,j+1,k)*q(i,j+1,k,qw)) &
                  +          m34*(mu(i,j-2,k)*q(i,j-1,k,qw)-mu(i,j+1,k)*q(i,j  ,k,qw)) &
                  +          m35*(mu(i,j-2,k)*q(i,j  ,k,qw)-mu(i,j+1,k)*q(i,j-1,k,qw)) &
                  +          m36*(mu(i,j-2,k)*q(i,j+1,k,qw)-mu(i,j+1,k)*q(i,j-2,k,qw)) &
                  +          m37*(mu(i,j-2,k)*q(i,j+2,k,qw)-mu(i,j+1,k)*q(i,j-3,k,qw)) &
                  &        + m41*(mu(i,j-1,k)*q(i,j-4,k,qw)-mu(i,j  ,k)*q(i,j+3,k,qw)) &
                  +          m42*(mu(i,j-1,k)*q(i,j-3,k,qw)-mu(i,j  ,k)*q(i,j+2,k,qw)) &
                  +          m43*(mu(i,j-1,k)*q(i,j-2,k,qw)-mu(i,j  ,k)*q(i,j+1,k,qw)) &
                  +          m44*(mu(i,j-1,k)*q(i,j-1,k,qw)-mu(i,j  ,k)*q(i,j  ,k,qw)) &
                  +          m45*(mu(i,j-1,k)*q(i,j  ,k,qw)-mu(i,j  ,k)*q(i,j-1,k,qw)) &
                  +          m46*(mu(i,j-1,k)*q(i,j+1,k,qw)-mu(i,j  ,k)*q(i,j-2,k,qw)) &
                  +          m47*(mu(i,j-1,k)*q(i,j+2,k,qw)-mu(i,j  ,k)*q(i,j-3,k,qw)) &
                  +          m48*(mu(i,j-1,k)*q(i,j+3,k,qw)-mu(i,j  ,k)*q(i,j-4,k,qw))

             Hg(i,j,k,iene) = m11*(lam(i,j-4,k)*q(i,j-4,k,qtemp)-lam(i,j+3,k)*q(i,j+3,k,qtemp)) &
                  +           m12*(lam(i,j-4,k)*q(i,j-3,k,qtemp)-lam(i,j+3,k)*q(i,j+2,k,qtemp)) &
                  +           m13*(lam(i,j-4,k)*q(i,j-2,k,qtemp)-lam(i,j+3,k)*q(i,j+1,k,qtemp)) &
                  +           m14*(lam(i,j-4,k)*q(i,j-1,k,qtemp)-lam(i,j+3,k)*q(i,j  ,k,qtemp)) &
                  +           m15*(lam(i,j-4,k)*q(i,j  ,k,qtemp)-lam(i,j+3,k)*q(i,j-1,k,qtemp)) &
                  &         + m21*(lam(i,j-3,k)*q(i,j-4,k,qtemp)-lam(i,j+2,k)*q(i,j+3,k,qtemp)) &
                  +           m22*(lam(i,j-3,k)*q(i,j-3,k,qtemp)-lam(i,j+2,k)*q(i,j+2,k,qtemp)) &
                  +           m23*(lam(i,j-3,k)*q(i,j-2,k,qtemp)-lam(i,j+2,k)*q(i,j+1,k,qtemp)) &
                  +           m24*(lam(i,j-3,k)*q(i,j-1,k,qtemp)-lam(i,j+2,k)*q(i,j  ,k,qtemp)) &
                  +           m25*(lam(i,j-3,k)*q(i,j  ,k,qtemp)-lam(i,j+2,k)*q(i,j-1,k,qtemp)) &
                  +           m26*(lam(i,j-3,k)*q(i,j+1,k,qtemp)-lam(i,j+2,k)*q(i,j-2,k,qtemp)) &
                  &         + m31*(lam(i,j-2,k)*q(i,j-4,k,qtemp)-lam(i,j+1,k)*q(i,j+3,k,qtemp)) &
                  +           m32*(lam(i,j-2,k)*q(i,j-3,k,qtemp)-lam(i,j+1,k)*q(i,j+2,k,qtemp)) &
                  +           m33*(lam(i,j-2,k)*q(i,j-2,k,qtemp)-lam(i,j+1,k)*q(i,j+1,k,qtemp)) &
                  +           m34*(lam(i,j-2,k)*q(i,j-1,k,qtemp)-lam(i,j+1,k)*q(i,j  ,k,qtemp)) &
                  +           m35*(lam(i,j-2,k)*q(i,j  ,k,qtemp)-lam(i,j+1,k)*q(i,j-1,k,qtemp)) &
                  +           m36*(lam(i,j-2,k)*q(i,j+1,k,qtemp)-lam(i,j+1,k)*q(i,j-2,k,qtemp)) &
                  +           m37*(lam(i,j-2,k)*q(i,j+2,k,qtemp)-lam(i,j+1,k)*q(i,j-3,k,qtemp)) &
                  &         + m41*(lam(i,j-1,k)*q(i,j-4,k,qtemp)-lam(i,j  ,k)*q(i,j+3,k,qtemp)) &
                  +           m42*(lam(i,j-1,k)*q(i,j-3,k,qtemp)-lam(i,j  ,k)*q(i,j+2,k,qtemp)) &
                  +           m43*(lam(i,j-1,k)*q(i,j-2,k,qtemp)-lam(i,j  ,k)*q(i,j+1,k,qtemp)) &
                  +           m44*(lam(i,j-1,k)*q(i,j-1,k,qtemp)-lam(i,j  ,k)*q(i,j  ,k,qtemp)) &
                  +           m45*(lam(i,j-1,k)*q(i,j  ,k,qtemp)-lam(i,j  ,k)*q(i,j-1,k,qtemp)) &
                  +           m46*(lam(i,j-1,k)*q(i,j+1,k,qtemp)-lam(i,j  ,k)*q(i,j-2,k,qtemp)) &
                  +           m47*(lam(i,j-1,k)*q(i,j+2,k,qtemp)-lam(i,j  ,k)*q(i,j-3,k,qtemp)) &
                  +           m48*(lam(i,j-1,k)*q(i,j+3,k,qtemp)-lam(i,j  ,k)*q(i,j-4,k,qtemp))

             Htot = 0.d0
             do n = 1, nspecies
                qxn = qx1+n-1
                qyn = qy1+n-1
                Htmp(n) = m11*(dcx(i,j-4,k,n)*q(i,j-4,k,qxn)-dcx(i,j+3,k,n)*q(i,j+3,k,qxn)) &
                  +       m12*(dcx(i,j-4,k,n)*q(i,j-3,k,qxn)-dcx(i,j+3,k,n)*q(i,j+2,k,qxn)) &
                  +       m13*(dcx(i,j-4,k,n)*q(i,j-2,k,qxn)-dcx(i,j+3,k,n)*q(i,j+1,k,qxn)) &
                  +       m14*(dcx(i,j-4,k,n)*q(i,j-1,k,qxn)-dcx(i,j+3,k,n)*q(i,j  ,k,qxn)) &
                  +       m15*(dcx(i,j-4,k,n)*q(i,j  ,k,qxn)-dcx(i,j+3,k,n)*q(i,j-1,k,qxn)) &
                  &     + m21*(dcx(i,j-3,k,n)*q(i,j-4,k,qxn)-dcx(i,j+2,k,n)*q(i,j+3,k,qxn)) &
                  +       m22*(dcx(i,j-3,k,n)*q(i,j-3,k,qxn)-dcx(i,j+2,k,n)*q(i,j+2,k,qxn)) &
                  +       m23*(dcx(i,j-3,k,n)*q(i,j-2,k,qxn)-dcx(i,j+2,k,n)*q(i,j+1,k,qxn)) &
                  +       m24*(dcx(i,j-3,k,n)*q(i,j-1,k,qxn)-dcx(i,j+2,k,n)*q(i,j  ,k,qxn)) &
                  +       m25*(dcx(i,j-3,k,n)*q(i,j  ,k,qxn)-dcx(i,j+2,k,n)*q(i,j-1,k,qxn)) &
                  +       m26*(dcx(i,j-3,k,n)*q(i,j+1,k,qxn)-dcx(i,j+2,k,n)*q(i,j-2,k,qxn)) &
                  &     + m31*(dcx(i,j-2,k,n)*q(i,j-4,k,qxn)-dcx(i,j+1,k,n)*q(i,j+3,k,qxn)) &
                  +       m32*(dcx(i,j-2,k,n)*q(i,j-3,k,qxn)-dcx(i,j+1,k,n)*q(i,j+2,k,qxn)) &
                  +       m33*(dcx(i,j-2,k,n)*q(i,j-2,k,qxn)-dcx(i,j+1,k,n)*q(i,j+1,k,qxn)) &
                  +       m34*(dcx(i,j-2,k,n)*q(i,j-1,k,qxn)-dcx(i,j+1,k,n)*q(i,j  ,k,qxn)) &
                  +       m35*(dcx(i,j-2,k,n)*q(i,j  ,k,qxn)-dcx(i,j+1,k,n)*q(i,j-1,k,qxn)) &
                  +       m36*(dcx(i,j-2,k,n)*q(i,j+1,k,qxn)-dcx(i,j+1,k,n)*q(i,j-2,k,qxn)) &
                  +       m37*(dcx(i,j-2,k,n)*q(i,j+2,k,qxn)-dcx(i,j+1,k,n)*q(i,j-3,k,qxn)) &
                  &     + m41*(dcx(i,j-1,k,n)*q(i,j-4,k,qxn)-dcx(i,j  ,k,n)*q(i,j+3,k,qxn)) &
                  +       m42*(dcx(i,j-1,k,n)*q(i,j-3,k,qxn)-dcx(i,j  ,k,n)*q(i,j+2,k,qxn)) &
                  +       m43*(dcx(i,j-1,k,n)*q(i,j-2,k,qxn)-dcx(i,j  ,k,n)*q(i,j+1,k,qxn)) &
                  +       m44*(dcx(i,j-1,k,n)*q(i,j-1,k,qxn)-dcx(i,j  ,k,n)*q(i,j  ,k,qxn)) &
                  +       m45*(dcx(i,j-1,k,n)*q(i,j  ,k,qxn)-dcx(i,j  ,k,n)*q(i,j-1,k,qxn)) &
                  +       m46*(dcx(i,j-1,k,n)*q(i,j+1,k,qxn)-dcx(i,j  ,k,n)*q(i,j-2,k,qxn)) &
                  +       m47*(dcx(i,j-1,k,n)*q(i,j+2,k,qxn)-dcx(i,j  ,k,n)*q(i,j-3,k,qxn)) &
                  +       m48*(dcx(i,j-1,k,n)*q(i,j+3,k,qxn)-dcx(i,j  ,k,n)*q(i,j-4,k,qxn))
                Htmp(n) = Htmp(n)  &
                  +       m11*(dcp(i,j-4,k,n)*q(i,j-4,k,qpres)-dcp(i,j+3,k,n)*q(i,j+3,k,qpres)) &
                  +       m12*(dcp(i,j-4,k,n)*q(i,j-3,k,qpres)-dcp(i,j+3,k,n)*q(i,j+2,k,qpres)) &
                  +       m13*(dcp(i,j-4,k,n)*q(i,j-2,k,qpres)-dcp(i,j+3,k,n)*q(i,j+1,k,qpres)) &
                  +       m14*(dcp(i,j-4,k,n)*q(i,j-1,k,qpres)-dcp(i,j+3,k,n)*q(i,j  ,k,qpres)) &
                  +       m15*(dcp(i,j-4,k,n)*q(i,j  ,k,qpres)-dcp(i,j+3,k,n)*q(i,j-1,k,qpres)) &
                  &     + m21*(dcp(i,j-3,k,n)*q(i,j-4,k,qpres)-dcp(i,j+2,k,n)*q(i,j+3,k,qpres)) &
                  +       m22*(dcp(i,j-3,k,n)*q(i,j-3,k,qpres)-dcp(i,j+2,k,n)*q(i,j+2,k,qpres)) &
                  +       m23*(dcp(i,j-3,k,n)*q(i,j-2,k,qpres)-dcp(i,j+2,k,n)*q(i,j+1,k,qpres)) &
                  +       m24*(dcp(i,j-3,k,n)*q(i,j-1,k,qpres)-dcp(i,j+2,k,n)*q(i,j  ,k,qpres)) &
                  +       m25*(dcp(i,j-3,k,n)*q(i,j  ,k,qpres)-dcp(i,j+2,k,n)*q(i,j-1,k,qpres)) &
                  +       m26*(dcp(i,j-3,k,n)*q(i,j+1,k,qpres)-dcp(i,j+2,k,n)*q(i,j-2,k,qpres)) &
                  &     + m31*(dcp(i,j-2,k,n)*q(i,j-4,k,qpres)-dcp(i,j+1,k,n)*q(i,j+3,k,qpres)) &
                  +       m32*(dcp(i,j-2,k,n)*q(i,j-3,k,qpres)-dcp(i,j+1,k,n)*q(i,j+2,k,qpres)) &
                  +       m33*(dcp(i,j-2,k,n)*q(i,j-2,k,qpres)-dcp(i,j+1,k,n)*q(i,j+1,k,qpres)) &
                  +       m34*(dcp(i,j-2,k,n)*q(i,j-1,k,qpres)-dcp(i,j+1,k,n)*q(i,j  ,k,qpres)) &
                  +       m35*(dcp(i,j-2,k,n)*q(i,j  ,k,qpres)-dcp(i,j+1,k,n)*q(i,j-1,k,qpres)) &
                  +       m36*(dcp(i,j-2,k,n)*q(i,j+1,k,qpres)-dcp(i,j+1,k,n)*q(i,j-2,k,qpres)) &
                  +       m37*(dcp(i,j-2,k,n)*q(i,j+2,k,qpres)-dcp(i,j+1,k,n)*q(i,j-3,k,qpres)) &
                  &     + m41*(dcp(i,j-1,k,n)*q(i,j-4,k,qpres)-dcp(i,j  ,k,n)*q(i,j+3,k,qpres)) &
                  +       m42*(dcp(i,j-1,k,n)*q(i,j-3,k,qpres)-dcp(i,j  ,k,n)*q(i,j+2,k,qpres)) &
                  +       m43*(dcp(i,j-1,k,n)*q(i,j-2,k,qpres)-dcp(i,j  ,k,n)*q(i,j+1,k,qpres)) &
                  +       m44*(dcp(i,j-1,k,n)*q(i,j-1,k,qpres)-dcp(i,j  ,k,n)*q(i,j  ,k,qpres)) &
                  +       m45*(dcp(i,j-1,k,n)*q(i,j  ,k,qpres)-dcp(i,j  ,k,n)*q(i,j-1,k,qpres)) &
                  +       m46*(dcp(i,j-1,k,n)*q(i,j+1,k,qpres)-dcp(i,j  ,k,n)*q(i,j-2,k,qpres)) &
                  +       m47*(dcp(i,j-1,k,n)*q(i,j+2,k,qpres)-dcp(i,j  ,k,n)*q(i,j-3,k,qpres)) &
                  +       m48*(dcp(i,j-1,k,n)*q(i,j+3,k,qpres)-dcp(i,j  ,k,n)*q(i,j-4,k,qpres))
                Htot = Htot + Htmp(n)
                Ytmp(n) = (q(i,j-1,k,qyn) + q(i,j,k,qyn)) / 2.d0
             end do

             do n = 1, nspecies
                Hg(i,j,k,iry1+n-1) = Htmp(n) - Ytmp(n)*Htot
             end do

             do n = 1, nspecies
                qxn = qx1+n-1
                qyn = qy1+n-1
                qhn = qh1+n-1
                Hg(i,j,k,iene) =  Hg(i,j,k,iene) &
                  + m11*(dcx(i,j-4,k,n)*q(i,j-4,k,qhn)*q(i,j-4,k,qxn)-dcx(i,j+3,k,n)*q(i,j+3,k,qhn)*q(i,j+3,k,qxn)) &
                  + m12*(dcx(i,j-4,k,n)*q(i,j-4,k,qhn)*q(i,j-3,k,qxn)-dcx(i,j+3,k,n)*q(i,j+3,k,qhn)*q(i,j+2,k,qxn)) &
                  + m13*(dcx(i,j-4,k,n)*q(i,j-4,k,qhn)*q(i,j-2,k,qxn)-dcx(i,j+3,k,n)*q(i,j+3,k,qhn)*q(i,j+1,k,qxn)) &
                  + m14*(dcx(i,j-4,k,n)*q(i,j-4,k,qhn)*q(i,j-1,k,qxn)-dcx(i,j+3,k,n)*q(i,j+3,k,qhn)*q(i,j  ,k,qxn)) &
                  + m15*(dcx(i,j-4,k,n)*q(i,j-4,k,qhn)*q(i,j  ,k,qxn)-dcx(i,j+3,k,n)*q(i,j+3,k,qhn)*q(i,j-1,k,qxn)) &
                  + m21*(dcx(i,j-3,k,n)*q(i,j-3,k,qhn)*q(i,j-4,k,qxn)-dcx(i,j+2,k,n)*q(i,j+2,k,qhn)*q(i,j+3,k,qxn)) &
                  + m22*(dcx(i,j-3,k,n)*q(i,j-3,k,qhn)*q(i,j-3,k,qxn)-dcx(i,j+2,k,n)*q(i,j+2,k,qhn)*q(i,j+2,k,qxn)) &
                  + m23*(dcx(i,j-3,k,n)*q(i,j-3,k,qhn)*q(i,j-2,k,qxn)-dcx(i,j+2,k,n)*q(i,j+2,k,qhn)*q(i,j+1,k,qxn)) &
                  + m24*(dcx(i,j-3,k,n)*q(i,j-3,k,qhn)*q(i,j-1,k,qxn)-dcx(i,j+2,k,n)*q(i,j+2,k,qhn)*q(i,j  ,k,qxn)) &
                  + m25*(dcx(i,j-3,k,n)*q(i,j-3,k,qhn)*q(i,j  ,k,qxn)-dcx(i,j+2,k,n)*q(i,j+2,k,qhn)*q(i,j-1,k,qxn)) &
                  + m26*(dcx(i,j-3,k,n)*q(i,j-3,k,qhn)*q(i,j+1,k,qxn)-dcx(i,j+2,k,n)*q(i,j+2,k,qhn)*q(i,j-2,k,qxn)) &
                  + m31*(dcx(i,j-2,k,n)*q(i,j-2,k,qhn)*q(i,j-4,k,qxn)-dcx(i,j+1,k,n)*q(i,j+1,k,qhn)*q(i,j+3,k,qxn)) &
                  + m32*(dcx(i,j-2,k,n)*q(i,j-2,k,qhn)*q(i,j-3,k,qxn)-dcx(i,j+1,k,n)*q(i,j+1,k,qhn)*q(i,j+2,k,qxn)) &
                  + m33*(dcx(i,j-2,k,n)*q(i,j-2,k,qhn)*q(i,j-2,k,qxn)-dcx(i,j+1,k,n)*q(i,j+1,k,qhn)*q(i,j+1,k,qxn)) &
                  + m34*(dcx(i,j-2,k,n)*q(i,j-2,k,qhn)*q(i,j-1,k,qxn)-dcx(i,j+1,k,n)*q(i,j+1,k,qhn)*q(i,j  ,k,qxn)) &
                  + m35*(dcx(i,j-2,k,n)*q(i,j-2,k,qhn)*q(i,j  ,k,qxn)-dcx(i,j+1,k,n)*q(i,j+1,k,qhn)*q(i,j-1,k,qxn)) &
                  + m36*(dcx(i,j-2,k,n)*q(i,j-2,k,qhn)*q(i,j+1,k,qxn)-dcx(i,j+1,k,n)*q(i,j+1,k,qhn)*q(i,j-2,k,qxn)) &
                  + m37*(dcx(i,j-2,k,n)*q(i,j-2,k,qhn)*q(i,j+2,k,qxn)-dcx(i,j+1,k,n)*q(i,j+1,k,qhn)*q(i,j-3,k,qxn)) &
                  + m41*(dcx(i,j-1,k,n)*q(i,j-1,k,qhn)*q(i,j-4,k,qxn)-dcx(i,j  ,k,n)*q(i,j  ,k,qhn)*q(i,j+3,k,qxn)) &
                  + m42*(dcx(i,j-1,k,n)*q(i,j-1,k,qhn)*q(i,j-3,k,qxn)-dcx(i,j  ,k,n)*q(i,j  ,k,qhn)*q(i,j+2,k,qxn)) &
                  + m43*(dcx(i,j-1,k,n)*q(i,j-1,k,qhn)*q(i,j-2,k,qxn)-dcx(i,j  ,k,n)*q(i,j  ,k,qhn)*q(i,j+1,k,qxn)) &
                  + m44*(dcx(i,j-1,k,n)*q(i,j-1,k,qhn)*q(i,j-1,k,qxn)-dcx(i,j  ,k,n)*q(i,j  ,k,qhn)*q(i,j  ,k,qxn)) &
                  + m45*(dcx(i,j-1,k,n)*q(i,j-1,k,qhn)*q(i,j  ,k,qxn)-dcx(i,j  ,k,n)*q(i,j  ,k,qhn)*q(i,j-1,k,qxn)) &
                  + m46*(dcx(i,j-1,k,n)*q(i,j-1,k,qhn)*q(i,j+1,k,qxn)-dcx(i,j  ,k,n)*q(i,j  ,k,qhn)*q(i,j-2,k,qxn)) &
                  + m47*(dcx(i,j-1,k,n)*q(i,j-1,k,qhn)*q(i,j+2,k,qxn)-dcx(i,j  ,k,n)*q(i,j  ,k,qhn)*q(i,j-3,k,qxn)) &
                  + m48*(dcx(i,j-1,k,n)*q(i,j-1,k,qhn)*q(i,j+3,k,qxn)-dcx(i,j  ,k,n)*q(i,j  ,k,qhn)*q(i,j-4,k,qxn))
                Hg(i,j,k,iene) =  Hg(i,j,k,iene) &
                  + m11*(dcp(i,j-4,k,n)*q(i,j-4,k,qhn)*q(i,j-4,k,qpres)-dcp(i,j+3,k,n)*q(i,j+3,k,qhn)*q(i,j+3,k,qpres)) &
                  + m12*(dcp(i,j-4,k,n)*q(i,j-4,k,qhn)*q(i,j-3,k,qpres)-dcp(i,j+3,k,n)*q(i,j+3,k,qhn)*q(i,j+2,k,qpres)) &
                  + m13*(dcp(i,j-4,k,n)*q(i,j-4,k,qhn)*q(i,j-2,k,qpres)-dcp(i,j+3,k,n)*q(i,j+3,k,qhn)*q(i,j+1,k,qpres)) &
                  + m14*(dcp(i,j-4,k,n)*q(i,j-4,k,qhn)*q(i,j-1,k,qpres)-dcp(i,j+3,k,n)*q(i,j+3,k,qhn)*q(i,j  ,k,qpres)) &
                  + m15*(dcp(i,j-4,k,n)*q(i,j-4,k,qhn)*q(i,j  ,k,qpres)-dcp(i,j+3,k,n)*q(i,j+3,k,qhn)*q(i,j-1,k,qpres)) &
                  + m21*(dcp(i,j-3,k,n)*q(i,j-3,k,qhn)*q(i,j-4,k,qpres)-dcp(i,j+2,k,n)*q(i,j+2,k,qhn)*q(i,j+3,k,qpres)) &
                  + m22*(dcp(i,j-3,k,n)*q(i,j-3,k,qhn)*q(i,j-3,k,qpres)-dcp(i,j+2,k,n)*q(i,j+2,k,qhn)*q(i,j+2,k,qpres)) &
                  + m23*(dcp(i,j-3,k,n)*q(i,j-3,k,qhn)*q(i,j-2,k,qpres)-dcp(i,j+2,k,n)*q(i,j+2,k,qhn)*q(i,j+1,k,qpres)) &
                  + m24*(dcp(i,j-3,k,n)*q(i,j-3,k,qhn)*q(i,j-1,k,qpres)-dcp(i,j+2,k,n)*q(i,j+2,k,qhn)*q(i,j  ,k,qpres)) &
                  + m25*(dcp(i,j-3,k,n)*q(i,j-3,k,qhn)*q(i,j  ,k,qpres)-dcp(i,j+2,k,n)*q(i,j+2,k,qhn)*q(i,j-1,k,qpres)) &
                  + m26*(dcp(i,j-3,k,n)*q(i,j-3,k,qhn)*q(i,j+1,k,qpres)-dcp(i,j+2,k,n)*q(i,j+2,k,qhn)*q(i,j-2,k,qpres)) &
                  + m31*(dcp(i,j-2,k,n)*q(i,j-2,k,qhn)*q(i,j-4,k,qpres)-dcp(i,j+1,k,n)*q(i,j+1,k,qhn)*q(i,j+3,k,qpres)) &
                  + m32*(dcp(i,j-2,k,n)*q(i,j-2,k,qhn)*q(i,j-3,k,qpres)-dcp(i,j+1,k,n)*q(i,j+1,k,qhn)*q(i,j+2,k,qpres)) &
                  + m33*(dcp(i,j-2,k,n)*q(i,j-2,k,qhn)*q(i,j-2,k,qpres)-dcp(i,j+1,k,n)*q(i,j+1,k,qhn)*q(i,j+1,k,qpres)) &
                  + m34*(dcp(i,j-2,k,n)*q(i,j-2,k,qhn)*q(i,j-1,k,qpres)-dcp(i,j+1,k,n)*q(i,j+1,k,qhn)*q(i,j  ,k,qpres)) &
                  + m35*(dcp(i,j-2,k,n)*q(i,j-2,k,qhn)*q(i,j  ,k,qpres)-dcp(i,j+1,k,n)*q(i,j+1,k,qhn)*q(i,j-1,k,qpres)) &
                  + m36*(dcp(i,j-2,k,n)*q(i,j-2,k,qhn)*q(i,j+1,k,qpres)-dcp(i,j+1,k,n)*q(i,j+1,k,qhn)*q(i,j-2,k,qpres)) &
                  + m37*(dcp(i,j-2,k,n)*q(i,j-2,k,qhn)*q(i,j+2,k,qpres)-dcp(i,j+1,k,n)*q(i,j+1,k,qhn)*q(i,j-3,k,qpres)) &
                  + m41*(dcp(i,j-1,k,n)*q(i,j-1,k,qhn)*q(i,j-4,k,qpres)-dcp(i,j  ,k,n)*q(i,j  ,k,qhn)*q(i,j+3,k,qpres)) &
                  + m42*(dcp(i,j-1,k,n)*q(i,j-1,k,qhn)*q(i,j-3,k,qpres)-dcp(i,j  ,k,n)*q(i,j  ,k,qhn)*q(i,j+2,k,qpres)) &
                  + m43*(dcp(i,j-1,k,n)*q(i,j-1,k,qhn)*q(i,j-2,k,qpres)-dcp(i,j  ,k,n)*q(i,j  ,k,qhn)*q(i,j+1,k,qpres)) &
                  + m44*(dcp(i,j-1,k,n)*q(i,j-1,k,qhn)*q(i,j-1,k,qpres)-dcp(i,j  ,k,n)*q(i,j  ,k,qhn)*q(i,j  ,k,qpres)) &
                  + m45*(dcp(i,j-1,k,n)*q(i,j-1,k,qhn)*q(i,j  ,k,qpres)-dcp(i,j  ,k,n)*q(i,j  ,k,qhn)*q(i,j-1,k,qpres)) &
                  + m46*(dcp(i,j-1,k,n)*q(i,j-1,k,qhn)*q(i,j+1,k,qpres)-dcp(i,j  ,k,n)*q(i,j  ,k,qhn)*q(i,j-2,k,qpres)) &
                  + m47*(dcp(i,j-1,k,n)*q(i,j-1,k,qhn)*q(i,j+2,k,qpres)-dcp(i,j  ,k,n)*q(i,j  ,k,qhn)*q(i,j-3,k,qpres)) &
                  + m48*(dcp(i,j-1,k,n)*q(i,j-1,k,qhn)*q(i,j+3,k,qpres)-dcp(i,j  ,k,n)*q(i,j  ,k,qhn)*q(i,j-4,k,qpres))
             end do

          end do
       end do
    end do

    ! add y-direction flux
    do k=lo(3),hi(3)
       do j=lo(2),hi(2)
          do i=lo(1),hi(1)
             do n=imx,ncons
                flx(i,j,k,n) = flx(i,j,k,n) + (Hg(i,j+1,k,n) - Hg(i,j,k,n)) * dx2inv(2)
             end do
          end do
       end do
    end do
    ! ------- END y-direction -------

    ! ------- BEGIN z-direction -------
    do k=lo(3),hi(3)+1
       do j=lo(2),hi(2)
          do i=lo(1),hi(1)
             Hg(i,j,k,imx) = m11*(mu(i,j,k-4)*q(i,j,k-4,qu)-mu(i,j,k+3)*q(i,j,k+3,qu)) &
                  +          m12*(mu(i,j,k-4)*q(i,j,k-3,qu)-mu(i,j,k+3)*q(i,j,k+2,qu)) &
                  +          m13*(mu(i,j,k-4)*q(i,j,k-2,qu)-mu(i,j,k+3)*q(i,j,k+1,qu)) &
                  +          m14*(mu(i,j,k-4)*q(i,j,k-1,qu)-mu(i,j,k+3)*q(i,j,k  ,qu)) &
                  +          m15*(mu(i,j,k-4)*q(i,j,k  ,qu)-mu(i,j,k+3)*q(i,j,k-1,qu)) &
                  &        + m21*(mu(i,j,k-3)*q(i,j,k-4,qu)-mu(i,j,k+2)*q(i,j,k+3,qu)) &
                  +          m22*(mu(i,j,k-3)*q(i,j,k-3,qu)-mu(i,j,k+2)*q(i,j,k+2,qu)) &
                  +          m23*(mu(i,j,k-3)*q(i,j,k-2,qu)-mu(i,j,k+2)*q(i,j,k+1,qu)) &
                  +          m24*(mu(i,j,k-3)*q(i,j,k-1,qu)-mu(i,j,k+2)*q(i,j,k  ,qu)) &
                  +          m25*(mu(i,j,k-3)*q(i,j,k  ,qu)-mu(i,j,k+2)*q(i,j,k-1,qu)) &
                  +          m26*(mu(i,j,k-3)*q(i,j,k+1,qu)-mu(i,j,k+2)*q(i,j,k-2,qu)) &
                  &        + m31*(mu(i,j,k-2)*q(i,j,k-4,qu)-mu(i,j,k+1)*q(i,j,k+3,qu)) &
                  +          m32*(mu(i,j,k-2)*q(i,j,k-3,qu)-mu(i,j,k+1)*q(i,j,k+2,qu)) &
                  +          m33*(mu(i,j,k-2)*q(i,j,k-2,qu)-mu(i,j,k+1)*q(i,j,k+1,qu)) &
                  +          m34*(mu(i,j,k-2)*q(i,j,k-1,qu)-mu(i,j,k+1)*q(i,j,k  ,qu)) &
                  +          m35*(mu(i,j,k-2)*q(i,j,k  ,qu)-mu(i,j,k+1)*q(i,j,k-1,qu)) &
                  +          m36*(mu(i,j,k-2)*q(i,j,k+1,qu)-mu(i,j,k+1)*q(i,j,k-2,qu)) &
                  +          m37*(mu(i,j,k-2)*q(i,j,k+2,qu)-mu(i,j,k+1)*q(i,j,k-3,qu)) &
                  &        + m41*(mu(i,j,k-1)*q(i,j,k-4,qu)-mu(i,j,k  )*q(i,j,k+3,qu)) &
                  +          m42*(mu(i,j,k-1)*q(i,j,k-3,qu)-mu(i,j,k  )*q(i,j,k+2,qu)) &
                  +          m43*(mu(i,j,k-1)*q(i,j,k-2,qu)-mu(i,j,k  )*q(i,j,k+1,qu)) &
                  +          m44*(mu(i,j,k-1)*q(i,j,k-1,qu)-mu(i,j,k  )*q(i,j,k  ,qu)) &
                  +          m45*(mu(i,j,k-1)*q(i,j,k  ,qu)-mu(i,j,k  )*q(i,j,k-1,qu)) &
                  +          m46*(mu(i,j,k-1)*q(i,j,k+1,qu)-mu(i,j,k  )*q(i,j,k-2,qu)) &
                  +          m47*(mu(i,j,k-1)*q(i,j,k+2,qu)-mu(i,j,k  )*q(i,j,k-3,qu)) &
                  +          m48*(mu(i,j,k-1)*q(i,j,k+3,qu)-mu(i,j,k  )*q(i,j,k-4,qu))

             Hg(i,j,k,imy) = m11*(mu(i,j,k-4)*q(i,j,k-4,qv)-mu(i,j,k+3)*q(i,j,k+3,qv)) &
                  +          m12*(mu(i,j,k-4)*q(i,j,k-3,qv)-mu(i,j,k+3)*q(i,j,k+2,qv)) &
                  +          m13*(mu(i,j,k-4)*q(i,j,k-2,qv)-mu(i,j,k+3)*q(i,j,k+1,qv)) &
                  +          m14*(mu(i,j,k-4)*q(i,j,k-1,qv)-mu(i,j,k+3)*q(i,j,k  ,qv)) &
                  +          m15*(mu(i,j,k-4)*q(i,j,k  ,qv)-mu(i,j,k+3)*q(i,j,k-1,qv)) &
                  &        + m21*(mu(i,j,k-3)*q(i,j,k-4,qv)-mu(i,j,k+2)*q(i,j,k+3,qv)) &
                  +          m22*(mu(i,j,k-3)*q(i,j,k-3,qv)-mu(i,j,k+2)*q(i,j,k+2,qv)) &
                  +          m23*(mu(i,j,k-3)*q(i,j,k-2,qv)-mu(i,j,k+2)*q(i,j,k+1,qv)) &
                  +          m24*(mu(i,j,k-3)*q(i,j,k-1,qv)-mu(i,j,k+2)*q(i,j,k  ,qv)) &
                  +          m25*(mu(i,j,k-3)*q(i,j,k  ,qv)-mu(i,j,k+2)*q(i,j,k-1,qv)) &
                  +          m26*(mu(i,j,k-3)*q(i,j,k+1,qv)-mu(i,j,k+2)*q(i,j,k-2,qv)) &
                  &        + m31*(mu(i,j,k-2)*q(i,j,k-4,qv)-mu(i,j,k+1)*q(i,j,k+3,qv)) &
                  +          m32*(mu(i,j,k-2)*q(i,j,k-3,qv)-mu(i,j,k+1)*q(i,j,k+2,qv)) &
                  +          m33*(mu(i,j,k-2)*q(i,j,k-2,qv)-mu(i,j,k+1)*q(i,j,k+1,qv)) &
                  +          m34*(mu(i,j,k-2)*q(i,j,k-1,qv)-mu(i,j,k+1)*q(i,j,k  ,qv)) &
                  +          m35*(mu(i,j,k-2)*q(i,j,k  ,qv)-mu(i,j,k+1)*q(i,j,k-1,qv)) &
                  +          m36*(mu(i,j,k-2)*q(i,j,k+1,qv)-mu(i,j,k+1)*q(i,j,k-2,qv)) &
                  +          m37*(mu(i,j,k-2)*q(i,j,k+2,qv)-mu(i,j,k+1)*q(i,j,k-3,qv)) &
                  &        + m41*(mu(i,j,k-1)*q(i,j,k-4,qv)-mu(i,j,k  )*q(i,j,k+3,qv)) &
                  +          m42*(mu(i,j,k-1)*q(i,j,k-3,qv)-mu(i,j,k  )*q(i,j,k+2,qv)) &
                  +          m43*(mu(i,j,k-1)*q(i,j,k-2,qv)-mu(i,j,k  )*q(i,j,k+1,qv)) &
                  +          m44*(mu(i,j,k-1)*q(i,j,k-1,qv)-mu(i,j,k  )*q(i,j,k  ,qv)) &
                  +          m45*(mu(i,j,k-1)*q(i,j,k  ,qv)-mu(i,j,k  )*q(i,j,k-1,qv)) &
                  +          m46*(mu(i,j,k-1)*q(i,j,k+1,qv)-mu(i,j,k  )*q(i,j,k-2,qv)) &
                  +          m47*(mu(i,j,k-1)*q(i,j,k+2,qv)-mu(i,j,k  )*q(i,j,k-3,qv)) &
                  +          m48*(mu(i,j,k-1)*q(i,j,k+3,qv)-mu(i,j,k  )*q(i,j,k-4,qv))

             Hg(i,j,k,imz) = m11*(vsc1(i,j,k-4)*q(i,j,k-4,qw)-vsc1(i,j,k+3)*q(i,j,k+3,qw)) &
                  +          m12*(vsc1(i,j,k-4)*q(i,j,k-3,qw)-vsc1(i,j,k+3)*q(i,j,k+2,qw)) &
                  +          m13*(vsc1(i,j,k-4)*q(i,j,k-2,qw)-vsc1(i,j,k+3)*q(i,j,k+1,qw)) &
                  +          m14*(vsc1(i,j,k-4)*q(i,j,k-1,qw)-vsc1(i,j,k+3)*q(i,j,k  ,qw)) &
                  +          m15*(vsc1(i,j,k-4)*q(i,j,k  ,qw)-vsc1(i,j,k+3)*q(i,j,k-1,qw)) &
                  &        + m21*(vsc1(i,j,k-3)*q(i,j,k-4,qw)-vsc1(i,j,k+2)*q(i,j,k+3,qw)) &
                  +          m22*(vsc1(i,j,k-3)*q(i,j,k-3,qw)-vsc1(i,j,k+2)*q(i,j,k+2,qw)) &
                  +          m23*(vsc1(i,j,k-3)*q(i,j,k-2,qw)-vsc1(i,j,k+2)*q(i,j,k+1,qw)) &
                  +          m24*(vsc1(i,j,k-3)*q(i,j,k-1,qw)-vsc1(i,j,k+2)*q(i,j,k  ,qw)) &
                  +          m25*(vsc1(i,j,k-3)*q(i,j,k  ,qw)-vsc1(i,j,k+2)*q(i,j,k-1,qw)) &
                  +          m26*(vsc1(i,j,k-3)*q(i,j,k+1,qw)-vsc1(i,j,k+2)*q(i,j,k-2,qw)) &
                  &        + m31*(vsc1(i,j,k-2)*q(i,j,k-4,qw)-vsc1(i,j,k+1)*q(i,j,k+3,qw)) &
                  +          m32*(vsc1(i,j,k-2)*q(i,j,k-3,qw)-vsc1(i,j,k+1)*q(i,j,k+2,qw)) &
                  +          m33*(vsc1(i,j,k-2)*q(i,j,k-2,qw)-vsc1(i,j,k+1)*q(i,j,k+1,qw)) &
                  +          m34*(vsc1(i,j,k-2)*q(i,j,k-1,qw)-vsc1(i,j,k+1)*q(i,j,k  ,qw)) &
                  +          m35*(vsc1(i,j,k-2)*q(i,j,k  ,qw)-vsc1(i,j,k+1)*q(i,j,k-1,qw)) &
                  +          m36*(vsc1(i,j,k-2)*q(i,j,k+1,qw)-vsc1(i,j,k+1)*q(i,j,k-2,qw)) &
                  +          m37*(vsc1(i,j,k-2)*q(i,j,k+2,qw)-vsc1(i,j,k+1)*q(i,j,k-3,qw)) &
                  &        + m41*(vsc1(i,j,k-1)*q(i,j,k-4,qw)-vsc1(i,j,k  )*q(i,j,k+3,qw)) &
                  +          m42*(vsc1(i,j,k-1)*q(i,j,k-3,qw)-vsc1(i,j,k  )*q(i,j,k+2,qw)) &
                  +          m43*(vsc1(i,j,k-1)*q(i,j,k-2,qw)-vsc1(i,j,k  )*q(i,j,k+1,qw)) &
                  +          m44*(vsc1(i,j,k-1)*q(i,j,k-1,qw)-vsc1(i,j,k  )*q(i,j,k  ,qw)) &
                  +          m45*(vsc1(i,j,k-1)*q(i,j,k  ,qw)-vsc1(i,j,k  )*q(i,j,k-1,qw)) &
                  +          m46*(vsc1(i,j,k-1)*q(i,j,k+1,qw)-vsc1(i,j,k  )*q(i,j,k-2,qw)) &
                  +          m47*(vsc1(i,j,k-1)*q(i,j,k+2,qw)-vsc1(i,j,k  )*q(i,j,k-3,qw)) &
                  +          m48*(vsc1(i,j,k-1)*q(i,j,k+3,qw)-vsc1(i,j,k  )*q(i,j,k-4,qw))

             Hg(i,j,k,iene) = m11*(lam(i,j,k-4)*q(i,j,k-4,qtemp)-lam(i,j,k+3)*q(i,j,k+3,qtemp)) &
                  +           m12*(lam(i,j,k-4)*q(i,j,k-3,qtemp)-lam(i,j,k+3)*q(i,j,k+2,qtemp)) &
                  +           m13*(lam(i,j,k-4)*q(i,j,k-2,qtemp)-lam(i,j,k+3)*q(i,j,k+1,qtemp)) &
                  +           m14*(lam(i,j,k-4)*q(i,j,k-1,qtemp)-lam(i,j,k+3)*q(i,j,k  ,qtemp)) &
                  +           m15*(lam(i,j,k-4)*q(i,j,k  ,qtemp)-lam(i,j,k+3)*q(i,j,k-1,qtemp)) &
                  &         + m21*(lam(i,j,k-3)*q(i,j,k-4,qtemp)-lam(i,j,k+2)*q(i,j,k+3,qtemp)) &
                  +           m22*(lam(i,j,k-3)*q(i,j,k-3,qtemp)-lam(i,j,k+2)*q(i,j,k+2,qtemp)) &
                  +           m23*(lam(i,j,k-3)*q(i,j,k-2,qtemp)-lam(i,j,k+2)*q(i,j,k+1,qtemp)) &
                  +           m24*(lam(i,j,k-3)*q(i,j,k-1,qtemp)-lam(i,j,k+2)*q(i,j,k  ,qtemp)) &
                  +           m25*(lam(i,j,k-3)*q(i,j,k  ,qtemp)-lam(i,j,k+2)*q(i,j,k-1,qtemp)) &
                  +           m26*(lam(i,j,k-3)*q(i,j,k+1,qtemp)-lam(i,j,k+2)*q(i,j,k-2,qtemp)) &
                  &         + m31*(lam(i,j,k-2)*q(i,j,k-4,qtemp)-lam(i,j,k+1)*q(i,j,k+3,qtemp)) &
                  +           m32*(lam(i,j,k-2)*q(i,j,k-3,qtemp)-lam(i,j,k+1)*q(i,j,k+2,qtemp)) &
                  +           m33*(lam(i,j,k-2)*q(i,j,k-2,qtemp)-lam(i,j,k+1)*q(i,j,k+1,qtemp)) &
                  +           m34*(lam(i,j,k-2)*q(i,j,k-1,qtemp)-lam(i,j,k+1)*q(i,j,k  ,qtemp)) &
                  +           m35*(lam(i,j,k-2)*q(i,j,k  ,qtemp)-lam(i,j,k+1)*q(i,j,k-1,qtemp)) &
                  +           m36*(lam(i,j,k-2)*q(i,j,k+1,qtemp)-lam(i,j,k+1)*q(i,j,k-2,qtemp)) &
                  +           m37*(lam(i,j,k-2)*q(i,j,k+2,qtemp)-lam(i,j,k+1)*q(i,j,k-3,qtemp)) &
                  &         + m41*(lam(i,j,k-1)*q(i,j,k-4,qtemp)-lam(i,j,k  )*q(i,j,k+3,qtemp)) &
                  +           m42*(lam(i,j,k-1)*q(i,j,k-3,qtemp)-lam(i,j,k  )*q(i,j,k+2,qtemp)) &
                  +           m43*(lam(i,j,k-1)*q(i,j,k-2,qtemp)-lam(i,j,k  )*q(i,j,k+1,qtemp)) &
                  +           m44*(lam(i,j,k-1)*q(i,j,k-1,qtemp)-lam(i,j,k  )*q(i,j,k  ,qtemp)) &
                  +           m45*(lam(i,j,k-1)*q(i,j,k  ,qtemp)-lam(i,j,k  )*q(i,j,k-1,qtemp)) &
                  +           m46*(lam(i,j,k-1)*q(i,j,k+1,qtemp)-lam(i,j,k  )*q(i,j,k-2,qtemp)) &
                  +           m47*(lam(i,j,k-1)*q(i,j,k+2,qtemp)-lam(i,j,k  )*q(i,j,k-3,qtemp)) &
                  +           m48*(lam(i,j,k-1)*q(i,j,k+3,qtemp)-lam(i,j,k  )*q(i,j,k-4,qtemp))
!xxxxxx
             Htot = 0.d0
             do n = 1, nspecies
                qxn = qx1+n-1
                qyn = qy1+n-1
                Htmp(n) = m11*(dcx(i,j,k-4,n)*q(i,j,k-4,qxn)-dcx(i,j,k+3,n)*q(i,j,k+3,qxn)) &
                  +       m12*(dcx(i,j,k-4,n)*q(i,j,k-3,qxn)-dcx(i,j,k+3,n)*q(i,j,k+2,qxn)) &
                  +       m13*(dcx(i,j,k-4,n)*q(i,j,k-2,qxn)-dcx(i,j,k+3,n)*q(i,j,k+1,qxn)) &
                  +       m14*(dcx(i,j,k-4,n)*q(i,j,k-1,qxn)-dcx(i,j,k+3,n)*q(i,j,k  ,qxn)) &
                  +       m15*(dcx(i,j,k-4,n)*q(i,j,k  ,qxn)-dcx(i,j,k+3,n)*q(i,j,k-1,qxn)) &
                  &     + m21*(dcx(i,j,k-3,n)*q(i,j,k-4,qxn)-dcx(i,j,k+2,n)*q(i,j,k+3,qxn)) &
                  +       m22*(dcx(i,j,k-3,n)*q(i,j,k-3,qxn)-dcx(i,j,k+2,n)*q(i,j,k+2,qxn)) &
                  +       m23*(dcx(i,j,k-3,n)*q(i,j,k-2,qxn)-dcx(i,j,k+2,n)*q(i,j,k+1,qxn)) &
                  +       m24*(dcx(i,j,k-3,n)*q(i,j,k-1,qxn)-dcx(i,j,k+2,n)*q(i,j,k  ,qxn)) &
                  +       m25*(dcx(i,j,k-3,n)*q(i,j,k  ,qxn)-dcx(i,j,k+2,n)*q(i,j,k-1,qxn)) &
                  +       m26*(dcx(i,j,k-3,n)*q(i,j,k+1,qxn)-dcx(i,j,k+2,n)*q(i,j,k-2,qxn)) &
                  &     + m31*(dcx(i,j,k-2,n)*q(i,j,k-4,qxn)-dcx(i,j,k+1,n)*q(i,j,k+3,qxn)) &
                  +       m32*(dcx(i,j,k-2,n)*q(i,j,k-3,qxn)-dcx(i,j,k+1,n)*q(i,j,k+2,qxn)) &
                  +       m33*(dcx(i,j,k-2,n)*q(i,j,k-2,qxn)-dcx(i,j,k+1,n)*q(i,j,k+1,qxn)) &
                  +       m34*(dcx(i,j,k-2,n)*q(i,j,k-1,qxn)-dcx(i,j,k+1,n)*q(i,j,k  ,qxn)) &
                  +       m35*(dcx(i,j,k-2,n)*q(i,j,k  ,qxn)-dcx(i,j,k+1,n)*q(i,j,k-1,qxn)) &
                  +       m36*(dcx(i,j,k-2,n)*q(i,j,k+1,qxn)-dcx(i,j,k+1,n)*q(i,j,k-2,qxn)) &
                  +       m37*(dcx(i,j,k-2,n)*q(i,j,k+2,qxn)-dcx(i,j,k+1,n)*q(i,j,k-3,qxn)) &
                  &     + m41*(dcx(i,j,k-1,n)*q(i,j,k-4,qxn)-dcx(i,j,k  ,n)*q(i,j,k+3,qxn)) &
                  +       m42*(dcx(i,j,k-1,n)*q(i,j,k-3,qxn)-dcx(i,j,k  ,n)*q(i,j,k+2,qxn)) &
                  +       m43*(dcx(i,j,k-1,n)*q(i,j,k-2,qxn)-dcx(i,j,k  ,n)*q(i,j,k+1,qxn)) &
                  +       m44*(dcx(i,j,k-1,n)*q(i,j,k-1,qxn)-dcx(i,j,k  ,n)*q(i,j,k  ,qxn)) &
                  +       m45*(dcx(i,j,k-1,n)*q(i,j,k  ,qxn)-dcx(i,j,k  ,n)*q(i,j,k-1,qxn)) &
                  +       m46*(dcx(i,j,k-1,n)*q(i,j,k+1,qxn)-dcx(i,j,k  ,n)*q(i,j,k-2,qxn)) &
                  +       m47*(dcx(i,j,k-1,n)*q(i,j,k+2,qxn)-dcx(i,j,k  ,n)*q(i,j,k-3,qxn)) &
                  +       m48*(dcx(i,j,k-1,n)*q(i,j,k+3,qxn)-dcx(i,j,k  ,n)*q(i,j,k-4,qxn))
                Htmp(n) = Htmp(n)  &                   
                  +       m11*(dcp(i,j,k-4,n)*q(i,j,k-4,qpres)-dcp(i,j,k+3,n)*q(i,j,k+3,qpres)) &
                  +       m12*(dcp(i,j,k-4,n)*q(i,j,k-3,qpres)-dcp(i,j,k+3,n)*q(i,j,k+2,qpres)) &
                  +       m13*(dcp(i,j,k-4,n)*q(i,j,k-2,qpres)-dcp(i,j,k+3,n)*q(i,j,k+1,qpres)) &
                  +       m14*(dcp(i,j,k-4,n)*q(i,j,k-1,qpres)-dcp(i,j,k+3,n)*q(i,j,k  ,qpres)) &
                  +       m15*(dcp(i,j,k-4,n)*q(i,j,k  ,qpres)-dcp(i,j,k+3,n)*q(i,j,k-1,qpres)) &
                  &     + m21*(dcp(i,j,k-3,n)*q(i,j,k-4,qpres)-dcp(i,j,k+2,n)*q(i,j,k+3,qpres)) &
                  +       m22*(dcp(i,j,k-3,n)*q(i,j,k-3,qpres)-dcp(i,j,k+2,n)*q(i,j,k+2,qpres)) &
                  +       m23*(dcp(i,j,k-3,n)*q(i,j,k-2,qpres)-dcp(i,j,k+2,n)*q(i,j,k+1,qpres)) &
                  +       m24*(dcp(i,j,k-3,n)*q(i,j,k-1,qpres)-dcp(i,j,k+2,n)*q(i,j,k  ,qpres)) &
                  +       m25*(dcp(i,j,k-3,n)*q(i,j,k  ,qpres)-dcp(i,j,k+2,n)*q(i,j,k-1,qpres)) &
                  +       m26*(dcp(i,j,k-3,n)*q(i,j,k+1,qpres)-dcp(i,j,k+2,n)*q(i,j,k-2,qpres)) &
                  &     + m31*(dcp(i,j,k-2,n)*q(i,j,k-4,qpres)-dcp(i,j,k+1,n)*q(i,j,k+3,qpres)) &
                  +       m32*(dcp(i,j,k-2,n)*q(i,j,k-3,qpres)-dcp(i,j,k+1,n)*q(i,j,k+2,qpres)) &
                  +       m33*(dcp(i,j,k-2,n)*q(i,j,k-2,qpres)-dcp(i,j,k+1,n)*q(i,j,k+1,qpres)) &
                  +       m34*(dcp(i,j,k-2,n)*q(i,j,k-1,qpres)-dcp(i,j,k+1,n)*q(i,j,k  ,qpres)) &
                  +       m35*(dcp(i,j,k-2,n)*q(i,j,k  ,qpres)-dcp(i,j,k+1,n)*q(i,j,k-1,qpres)) &
                  +       m36*(dcp(i,j,k-2,n)*q(i,j,k+1,qpres)-dcp(i,j,k+1,n)*q(i,j,k-2,qpres)) &
                  +       m37*(dcp(i,j,k-2,n)*q(i,j,k+2,qpres)-dcp(i,j,k+1,n)*q(i,j,k-3,qpres)) &
                  &     + m41*(dcp(i,j,k-1,n)*q(i,j,k-4,qpres)-dcp(i,j,k  ,n)*q(i,j,k+3,qpres)) &
                  +       m42*(dcp(i,j,k-1,n)*q(i,j,k-3,qpres)-dcp(i,j,k  ,n)*q(i,j,k+2,qpres)) &
                  +       m43*(dcp(i,j,k-1,n)*q(i,j,k-2,qpres)-dcp(i,j,k  ,n)*q(i,j,k+1,qpres)) &
                  +       m44*(dcp(i,j,k-1,n)*q(i,j,k-1,qpres)-dcp(i,j,k  ,n)*q(i,j,k  ,qpres)) &
                  +       m45*(dcp(i,j,k-1,n)*q(i,j,k  ,qpres)-dcp(i,j,k  ,n)*q(i,j,k-1,qpres)) &
                  +       m46*(dcp(i,j,k-1,n)*q(i,j,k+1,qpres)-dcp(i,j,k  ,n)*q(i,j,k-2,qpres)) &
                  +       m47*(dcp(i,j,k-1,n)*q(i,j,k+2,qpres)-dcp(i,j,k  ,n)*q(i,j,k-3,qpres)) &
                  +       m48*(dcp(i,j,k-1,n)*q(i,j,k+3,qpres)-dcp(i,j,k  ,n)*q(i,j,k-4,qpres))
                Htot = Htot + Htmp(n)
                Ytmp(n) = (q(i,j,k-1,qyn) + q(i,j,k,qyn)) / 2.d0
             end do

             do n = 1, nspecies
                Hg(i,j,k,iry1+n-1) = Htmp(n) - Ytmp(n)*Htot
             end do

             do n = 1, nspecies
                qxn = qx1+n-1
                qyn = qy1+n-1
                qhn = qh1+n-1
                Hg(i,j,k,iene) =  Hg(i,j,k,iene) &
                  + m11*(dcx(i,j,k-4,n)*q(i,j,k-4,qhn)*q(i,j,k-4,qxn)-dcx(i,j,k+3,n)*q(i,j,k+3,qhn)*q(i,j,k+3,qxn)) &
                  + m12*(dcx(i,j,k-4,n)*q(i,j,k-4,qhn)*q(i,j,k-3,qxn)-dcx(i,j,k+3,n)*q(i,j,k+3,qhn)*q(i,j,k+2,qxn)) &
                  + m13*(dcx(i,j,k-4,n)*q(i,j,k-4,qhn)*q(i,j,k-2,qxn)-dcx(i,j,k+3,n)*q(i,j,k+3,qhn)*q(i,j,k+1,qxn)) &
                  + m14*(dcx(i,j,k-4,n)*q(i,j,k-4,qhn)*q(i,j,k-1,qxn)-dcx(i,j,k+3,n)*q(i,j,k+3,qhn)*q(i,j,k  ,qxn)) &
                  + m15*(dcx(i,j,k-4,n)*q(i,j,k-4,qhn)*q(i,j,k  ,qxn)-dcx(i,j,k+3,n)*q(i,j,k+3,qhn)*q(i,j,k-1,qxn)) &
                  + m21*(dcx(i,j,k-3,n)*q(i,j,k-3,qhn)*q(i,j,k-4,qxn)-dcx(i,j,k+2,n)*q(i,j,k+2,qhn)*q(i,j,k+3,qxn)) &
                  + m22*(dcx(i,j,k-3,n)*q(i,j,k-3,qhn)*q(i,j,k-3,qxn)-dcx(i,j,k+2,n)*q(i,j,k+2,qhn)*q(i,j,k+2,qxn)) &
                  + m23*(dcx(i,j,k-3,n)*q(i,j,k-3,qhn)*q(i,j,k-2,qxn)-dcx(i,j,k+2,n)*q(i,j,k+2,qhn)*q(i,j,k+1,qxn)) &
                  + m24*(dcx(i,j,k-3,n)*q(i,j,k-3,qhn)*q(i,j,k-1,qxn)-dcx(i,j,k+2,n)*q(i,j,k+2,qhn)*q(i,j,k  ,qxn)) &
                  + m25*(dcx(i,j,k-3,n)*q(i,j,k-3,qhn)*q(i,j,k  ,qxn)-dcx(i,j,k+2,n)*q(i,j,k+2,qhn)*q(i,j,k-1,qxn)) &
                  + m26*(dcx(i,j,k-3,n)*q(i,j,k-3,qhn)*q(i,j,k+1,qxn)-dcx(i,j,k+2,n)*q(i,j,k+2,qhn)*q(i,j,k-2,qxn)) &
                  + m31*(dcx(i,j,k-2,n)*q(i,j,k-2,qhn)*q(i,j,k-4,qxn)-dcx(i,j,k+1,n)*q(i,j,k+1,qhn)*q(i,j,k+3,qxn)) &
                  + m32*(dcx(i,j,k-2,n)*q(i,j,k-2,qhn)*q(i,j,k-3,qxn)-dcx(i,j,k+1,n)*q(i,j,k+1,qhn)*q(i,j,k+2,qxn)) &
                  + m33*(dcx(i,j,k-2,n)*q(i,j,k-2,qhn)*q(i,j,k-2,qxn)-dcx(i,j,k+1,n)*q(i,j,k+1,qhn)*q(i,j,k+1,qxn)) &
                  + m34*(dcx(i,j,k-2,n)*q(i,j,k-2,qhn)*q(i,j,k-1,qxn)-dcx(i,j,k+1,n)*q(i,j,k+1,qhn)*q(i,j,k  ,qxn)) &
                  + m35*(dcx(i,j,k-2,n)*q(i,j,k-2,qhn)*q(i,j,k  ,qxn)-dcx(i,j,k+1,n)*q(i,j,k+1,qhn)*q(i,j,k-1,qxn)) &
                  + m36*(dcx(i,j,k-2,n)*q(i,j,k-2,qhn)*q(i,j,k+1,qxn)-dcx(i,j,k+1,n)*q(i,j,k+1,qhn)*q(i,j,k-2,qxn)) &
                  + m37*(dcx(i,j,k-2,n)*q(i,j,k-2,qhn)*q(i,j,k+2,qxn)-dcx(i,j,k+1,n)*q(i,j,k+1,qhn)*q(i,j,k-3,qxn)) &
                  + m41*(dcx(i,j,k-1,n)*q(i,j,k-1,qhn)*q(i,j,k-4,qxn)-dcx(i,j,k  ,n)*q(i,j,k  ,qhn)*q(i,j,k+3,qxn)) &
                  + m42*(dcx(i,j,k-1,n)*q(i,j,k-1,qhn)*q(i,j,k-3,qxn)-dcx(i,j,k  ,n)*q(i,j,k  ,qhn)*q(i,j,k+2,qxn)) &
                  + m43*(dcx(i,j,k-1,n)*q(i,j,k-1,qhn)*q(i,j,k-2,qxn)-dcx(i,j,k  ,n)*q(i,j,k  ,qhn)*q(i,j,k+1,qxn)) &
                  + m44*(dcx(i,j,k-1,n)*q(i,j,k-1,qhn)*q(i,j,k-1,qxn)-dcx(i,j,k  ,n)*q(i,j,k  ,qhn)*q(i,j,k  ,qxn)) &
                  + m45*(dcx(i,j,k-1,n)*q(i,j,k-1,qhn)*q(i,j,k  ,qxn)-dcx(i,j,k  ,n)*q(i,j,k  ,qhn)*q(i,j,k-1,qxn)) &
                  + m46*(dcx(i,j,k-1,n)*q(i,j,k-1,qhn)*q(i,j,k+1,qxn)-dcx(i,j,k  ,n)*q(i,j,k  ,qhn)*q(i,j,k-2,qxn)) &
                  + m47*(dcx(i,j,k-1,n)*q(i,j,k-1,qhn)*q(i,j,k+2,qxn)-dcx(i,j,k  ,n)*q(i,j,k  ,qhn)*q(i,j,k-3,qxn)) &
                  + m48*(dcx(i,j,k-1,n)*q(i,j,k-1,qhn)*q(i,j,k+3,qxn)-dcx(i,j,k  ,n)*q(i,j,k  ,qhn)*q(i,j,k-4,qxn))
                Hg(i,j,k,iene) =  Hg(i,j,k,iene) &
                  + m11*(dcp(i,j,k-4,n)*q(i,j,k-4,qhn)*q(i,j,k-4,qpres)-dcp(i,j,k+3,n)*q(i,j,k+3,qhn)*q(i,j,k+3,qpres)) &
                  + m12*(dcp(i,j,k-4,n)*q(i,j,k-4,qhn)*q(i,j,k-3,qpres)-dcp(i,j,k+3,n)*q(i,j,k+3,qhn)*q(i,j,k+2,qpres)) &
                  + m13*(dcp(i,j,k-4,n)*q(i,j,k-4,qhn)*q(i,j,k-2,qpres)-dcp(i,j,k+3,n)*q(i,j,k+3,qhn)*q(i,j,k+1,qpres)) &
                  + m14*(dcp(i,j,k-4,n)*q(i,j,k-4,qhn)*q(i,j,k-1,qpres)-dcp(i,j,k+3,n)*q(i,j,k+3,qhn)*q(i,j,k  ,qpres)) &
                  + m15*(dcp(i,j,k-4,n)*q(i,j,k-4,qhn)*q(i,j,k  ,qpres)-dcp(i,j,k+3,n)*q(i,j,k+3,qhn)*q(i,j,k-1,qpres)) &
                  + m21*(dcp(i,j,k-3,n)*q(i,j,k-3,qhn)*q(i,j,k-4,qpres)-dcp(i,j,k+2,n)*q(i,j,k+2,qhn)*q(i,j,k+3,qpres)) &
                  + m22*(dcp(i,j,k-3,n)*q(i,j,k-3,qhn)*q(i,j,k-3,qpres)-dcp(i,j,k+2,n)*q(i,j,k+2,qhn)*q(i,j,k+2,qpres)) &
                  + m23*(dcp(i,j,k-3,n)*q(i,j,k-3,qhn)*q(i,j,k-2,qpres)-dcp(i,j,k+2,n)*q(i,j,k+2,qhn)*q(i,j,k+1,qpres)) &
                  + m24*(dcp(i,j,k-3,n)*q(i,j,k-3,qhn)*q(i,j,k-1,qpres)-dcp(i,j,k+2,n)*q(i,j,k+2,qhn)*q(i,j,k  ,qpres)) &
                  + m25*(dcp(i,j,k-3,n)*q(i,j,k-3,qhn)*q(i,j,k  ,qpres)-dcp(i,j,k+2,n)*q(i,j,k+2,qhn)*q(i,j,k-1,qpres)) &
                  + m26*(dcp(i,j,k-3,n)*q(i,j,k-3,qhn)*q(i,j,k+1,qpres)-dcp(i,j,k+2,n)*q(i,j,k+2,qhn)*q(i,j,k-2,qpres)) &
                  + m31*(dcp(i,j,k-2,n)*q(i,j,k-2,qhn)*q(i,j,k-4,qpres)-dcp(i,j,k+1,n)*q(i,j,k+1,qhn)*q(i,j,k+3,qpres)) &
                  + m32*(dcp(i,j,k-2,n)*q(i,j,k-2,qhn)*q(i,j,k-3,qpres)-dcp(i,j,k+1,n)*q(i,j,k+1,qhn)*q(i,j,k+2,qpres)) &
                  + m33*(dcp(i,j,k-2,n)*q(i,j,k-2,qhn)*q(i,j,k-2,qpres)-dcp(i,j,k+1,n)*q(i,j,k+1,qhn)*q(i,j,k+1,qpres)) &
                  + m34*(dcp(i,j,k-2,n)*q(i,j,k-2,qhn)*q(i,j,k-1,qpres)-dcp(i,j,k+1,n)*q(i,j,k+1,qhn)*q(i,j,k  ,qpres)) &
                  + m35*(dcp(i,j,k-2,n)*q(i,j,k-2,qhn)*q(i,j,k  ,qpres)-dcp(i,j,k+1,n)*q(i,j,k+1,qhn)*q(i,j,k-1,qpres)) &
                  + m36*(dcp(i,j,k-2,n)*q(i,j,k-2,qhn)*q(i,j,k+1,qpres)-dcp(i,j,k+1,n)*q(i,j,k+1,qhn)*q(i,j,k-2,qpres)) &
                  + m37*(dcp(i,j,k-2,n)*q(i,j,k-2,qhn)*q(i,j,k+2,qpres)-dcp(i,j,k+1,n)*q(i,j,k+1,qhn)*q(i,j,k-3,qpres)) &
                  + m41*(dcp(i,j,k-1,n)*q(i,j,k-1,qhn)*q(i,j,k-4,qpres)-dcp(i,j,k  ,n)*q(i,j,k  ,qhn)*q(i,j,k+3,qpres)) &
                  + m42*(dcp(i,j,k-1,n)*q(i,j,k-1,qhn)*q(i,j,k-3,qpres)-dcp(i,j,k  ,n)*q(i,j,k  ,qhn)*q(i,j,k+2,qpres)) &
                  + m43*(dcp(i,j,k-1,n)*q(i,j,k-1,qhn)*q(i,j,k-2,qpres)-dcp(i,j,k  ,n)*q(i,j,k  ,qhn)*q(i,j,k+1,qpres)) &
                  + m44*(dcp(i,j,k-1,n)*q(i,j,k-1,qhn)*q(i,j,k-1,qpres)-dcp(i,j,k  ,n)*q(i,j,k  ,qhn)*q(i,j,k  ,qpres)) &
                  + m45*(dcp(i,j,k-1,n)*q(i,j,k-1,qhn)*q(i,j,k  ,qpres)-dcp(i,j,k  ,n)*q(i,j,k  ,qhn)*q(i,j,k-1,qpres)) &
                  + m46*(dcp(i,j,k-1,n)*q(i,j,k-1,qhn)*q(i,j,k+1,qpres)-dcp(i,j,k  ,n)*q(i,j,k  ,qhn)*q(i,j,k-2,qpres)) &
                  + m47*(dcp(i,j,k-1,n)*q(i,j,k-1,qhn)*q(i,j,k+2,qpres)-dcp(i,j,k  ,n)*q(i,j,k  ,qhn)*q(i,j,k-3,qpres)) &
                  + m48*(dcp(i,j,k-1,n)*q(i,j,k-1,qhn)*q(i,j,k+3,qpres)-dcp(i,j,k  ,n)*q(i,j,k  ,qhn)*q(i,j,k-4,qpres))
             end do

          end do
       end do
    end do

    ! add z-direction flux
    do k=lo(3),hi(3)
       do j=lo(2),hi(2)
          do i=lo(1),hi(1)
             do n=imx,ncons
                flx(i,j,k,n) = flx(i,j,k,n) + (Hg(i,j,k+1,n) - Hg(i,j,k,n)) * dx2inv(3)
             end do
          end do
       end do
    end do
    ! ------- END z-direction -------

    ! add kinetic energy
    do k=lo(3),hi(3)
       do j=lo(2),hi(2)
          do i=lo(1),hi(1)
             flx(i,j,k,iene) = flx(i,j,k,iene) &
                  + flx(i,j,k,imx)*q(i,j,k,qu) &
                  + flx(i,j,k,imy)*q(i,j,k,qv) &
                  + flx(i,j,k,imz)*q(i,j,k,qw)
          end do
       end do
    end do

    deallocate(ux,uy,uz,vx,vy,vz,wx,wy,wz,vsc1,vsc2,Hg,dcx,dcp)

  end subroutine compact_diffterm_3d


  subroutine compute_courno(Q, dx, courno)
    type(multifab), intent(in) :: Q
    double precision, intent(in) :: dx(Q%dim)
    double precision, intent(inout) :: courno

    integer :: n, ng, dim, lo(Q%dim), hi(Q%dim)
    double precision, pointer :: qp(:,:,:,:)

    dim = Q%dim
    ng = nghost(Q)

    do n=1,nboxes(Q)
       if (remote(Q,n)) cycle

       qp => dataptr(Q,n)

       lo = lwb(get_box(Q,n))
       hi = upb(get_box(Q,n))

       if (dim .ne. 3) then
          call bl_error("Only 3D compute_courno is supported")
       else
          call comp_courno_3d(lo,hi,ng,dx,qp,courno)
       end if
    end do
  end subroutine compute_courno

  subroutine comp_courno_3d(lo,hi,ng,dx,Q,courno)
    integer, intent(in) :: lo(3), hi(3), ng
    double precision, intent(in) :: dx(3)
    double precision, intent(in) :: q(-ng+lo(1):hi(1)+ng,-ng+lo(2):hi(2)+ng,-ng+lo(3):hi(3)+ng,nprim)
    double precision, intent(inout) :: courno

    integer :: i,j,k, iwrk
    double precision :: dxinv(3), c, courx, coury, courz, rwrk, Ru, Ruc, Pa, Cv, Cp
    double precision :: Tt, X(nspecies), gamma

    do i=1,3
       dxinv(i) = 1.0d0 / dx(i)
    end do

    call ckrp(iwrk, rwrk, Ru, Ruc, Pa)

    do k=lo(3),hi(3)
       do j=lo(2),hi(2)
          do i=lo(1),hi(1)

             Tt = q(i,j,k,qtemp)
             X  = q(i,j,k,qx1:qx1+nspecies-1)
             call ckcvbl(Tt, X, iwrk, rwrk, Cv)
             Cp = Cv + Ru
             gamma = Cp / Cv
             c = sqrt(gamma*q(i,j,k,qpres)/q(i,j,k,qrho))

             courx = (c+abs(q(i,j,k,qu))) * dxinv(1)
             coury = (c+abs(q(i,j,k,qv))) * dxinv(2)
             courz = (c+abs(q(i,j,k,qw))) * dxinv(3)

             courno = max(courx,coury,courz,courno)
          end do
       end do
    end do

  end subroutine comp_courno_3d

end module advance_module
