module hypterm_module

  implicit none

  private

  public :: hypterm

contains

  subroutine hypterm(lo,hi,domlo,domhi,U,Ulo,Uhi,fx,fxlo,fxhi,fy,fylo,fyhi,dx)

    use meth_params_module, only : NVAR, difmag

    integer, intent(in) :: lo(2), hi(2), domlo(2), domhi(2), Ulo(2), Uhi(2), fxlo(2), fxhi(2), fylo(2), fyhi(2)
    double precision, intent(in   ) :: dx(2)
    double precision, intent(in   ) ::  U( Ulo(1): Uhi(1), Ulo(2): Uhi(2),NVAR)
    double precision, intent(inout) :: fx(fxlo(1):fxhi(1),fxlo(2):fxhi(2),NVAR)
    double precision, intent(inout) :: fy(fylo(1):fyhi(1),fylo(2):fyhi(2),NVAR)

    call hypterm_x(lo,hi,domlo,domhi,U,Ulo,Uhi,fx,fxlo,fxhi,dx)
    call hypterm_y(lo,hi,domlo,domhi,U,Ulo,Uhi,fy,fylo,fyhi,dx)

    if (difmag .gt. 0.0d0) then
       call add_artifical_viscocity(lo,hi,U,Ulo,Uhi,fx,fxlo,fxhi,fy,fylo,fyhi,dx)
    end if

  end subroutine hypterm


  subroutine hypterm_x(lo,hi,domlo,domhi,U,Ulo,Uhi,fx,fxlo,fxhi,dx)

    use meth_params_module, only : NVAR, URHO, UMX, UMY, UTEMP, UFS, UEDEN, NSPEC, NCHARV, CFS, do_mdcd
    use eigen_module, only : get_eigen_matrices
    use renorm_module, only : floor_species
    use eos_module, only : eos_get_eref
    use mdcd_module, only : mdcd
    use weno_module, only : weno4_gauss, weno5_face
    use riemann_module, only : riemann
    use RNS_boundary_module, only : get_hyper_bc_flag

    integer, intent(in) :: lo(2), hi(2), domlo(2), domhi(2), Ulo(2), Uhi(2), fxlo(2), fxhi(2)
    double precision, intent(in) :: dx(2)
    double precision, intent(in   ) ::  U( Ulo(1): Uhi(1), Ulo(2): Uhi(2),NVAR)
    double precision, intent(inout) :: fx(fxlo(1):fxhi(1),fxlo(2):fxhi(2),NVAR)

    integer :: i, j, n, ii, jj, ivar, m, g, bc_flag(2)
    double precision, allocatable :: Y(:,:), RoeW(:), v(:,:), flux(:,:)
    double precision, dimension(:,:,:), allocatable, target :: UG1,UG2
    double precision, dimension(:,:)  , allocatable, target :: UL,UR
    double precision, dimension(:,:,:), pointer :: UG
    double precision :: rhoInv, rho0, Y0(nspec), T0, v0(3), fac, eref
    double precision :: egv1(NCHARV,NCHARV), egv2(NCHARV,NCHARV), Uch(NCHARV), charv(-3:2,NCHARV)
    double precision :: cvl(NCHARV), cvr(NCHARV)

    allocate(Y   (lo(1)-3:hi(1)+3,nspec))
    allocate(RoeW(lo(1)-3:hi(1)+3))
    allocate(v   (lo(1)-3:hi(1)+3,2))

    allocate(flux(lo(1):hi(1)+1,NVAR))

    ! x-average, y-Gauss
    allocate(UG1(lo(1)-3:hi(1)+3,lo(2):hi(2),NVAR))
    allocate(UG2(lo(1)-3:hi(1)+3,lo(2):hi(2),NVAR))

    ! x-face, y-Gauss
    allocate(UL(lo(1):hi(1)+1,NVAR))
    allocate(UR(lo(1):hi(1)+1,NVAR))

    UG1 = 0.d0
    UG2 = 0.d0

    do j = lo(2), hi(2)
       do i = lo(1)-3, hi(1)+3
          ! given U(i,j-2:j+2,:), compute UG1 and UG2
          rho0 = U(i,j,URHO)
          rhoInv = 1.d0/rho0
          do n=1,nspec
             Y0(n) = U(i,j,UFS+n-1)*rhoInv
          end do
          v0(1) = U(i,j,UMY) * rhoInv
          v0(2) = U(i,j,UMX) * rhoInv
          v0(3) = 0.d0
          T0 = U(i,j,UTEMP)

          ! egv1: left matrix;  egv2: right matrix
          call get_eigen_matrices(rho0, Y0, T0, v0, egv1, egv2)

          do jj=-2,2
             Uch(1) = U(i,j+jj,UMY)
             Uch(2) = U(i,j+jj,UMX)
             Uch(3) = 0.d0
             Uch(4) = U(i,j+jj,UEDEN)
             rhoinV = 1.d0/U(i,j+jj,URHO)
             do n=1,nspec
                Uch(CFS+n-1) = U(i,j+jj,UFS+n-1)
                Y0(n) = Uch(CFS+n-1)*rhoInv
             end do
             eref = eos_get_eref(Y0)
             Uch(4) = Uch(4) - U(i,j+jj,URHO)*eref

             do n=1,NCHARV
                charv(jj,n) = dot_product(egv1(:,n),Uch)
             end do
          end do
          
          do ivar=1,NCHARV
             call weno4_gauss(charv(-2:2,ivar), cvl(ivar), cvr(ivar))
          end do

          egv1 = transpose(egv2)  ! egv1 now holds transposed right matrix

          do n=1,NCHARV
             UG1(i,j,UMY  ) = UG1(i,j,UMY  ) + cvl(n)*egv2(1,n)
             UG2(i,j,UMY  ) = UG2(i,j,UMY  ) + cvr(n)*egv2(1,n)
             UG1(i,j,UMX  ) = UG1(i,j,UMX  ) + cvl(n)*egv2(2,n)
             UG2(i,j,UMX  ) = UG2(i,j,UMX  ) + cvr(n)*egv2(2,n)
             UG1(i,j,UEDEN) = UG1(i,j,UEDEN) + cvl(n)*egv2(4,n)
             UG2(i,j,UEDEN) = UG2(i,j,UEDEN) + cvr(n)*egv2(4,n)
          end do
          
          do m=1,nspec
             UG1(i,j,UFS+m-1) = UG1(i,j,UFS+m-1) + dot_product(cvl, egv1(:,CFS+m-1))
             UG2(i,j,UFS+m-1) = UG2(i,j,UFS+m-1) + dot_product(cvr, egv1(:,CFS+m-1))
             UG1(i,j,URHO   ) = UG1(i,j,URHO   ) + UG1(i,j,UFS+m-1)
             UG2(i,j,URHO   ) = UG2(i,j,URHO   ) + UG2(i,j,UFS+m-1)
          end do
          
          rhoInv = 1.d0/UG1(i,j,URHO)
          do n=1,nspec
             Y0(n) = UG1(i,j,UFS+n-1) * rhoInv
          end do
          call floor_species(nspec, Y0)
          eref = eos_get_eref(Y0)
          UG1(i,j,UEDEN) = UG1(i,j,UEDEN) + UG1(i,j,URHO) * eref
          do n=1,nspec
             UG1(i,j,UFS+n-1) = UG1(i,j,URHO)*Y0(n)
          end do
          
          rhoInv = 1.d0/UG2(i,j,URHO)
          do n=1,nspec
             Y0(n) = UG2(i,j,UFS+n-1) * rhoInv
          end do
          call floor_species(nspec, Y0)
          eref = eos_get_eref(Y0)
          UG2(i,j,UEDEN) = UG2(i,j,UEDEN) + UG2(i,j,URHO) * eref
          do n=1,nspec
             UG2(i,j,UFS+n-1) = UG2(i,j,URHO)*Y0(n)
          end do
          
          UG1(i,j,UTEMP) = U(i,j,UTEMP)
          UG2(i,j,UTEMP) = U(i,j,UTEMP)
       end do
    end do

    do g = 1, 2
       if (g.eq.1) then
          UG => UG1
       else
          UG => UG2
       end if

       do j = lo(2), hi(2)
          ! given UG(i-3:i+2,j,:), compute UL(i,:) and UR(i,:)
          UL = 0.d0
          UR = 0.d0

          do i = lo(1)-3, hi(1)+3
             rhoInv = 1.d0/UG(i,j,URHO)
             RoeW(i) = sqrt(UG(i,j,URHO))
             do n=1,nspec
                Y(i,n) = UG(i,j,UFS+n-1)*rhoInv
             end do
             v(i,1) = UG(i,j,UMX)*rhoInv
             v(i,2) = UG(i,j,UMY)*rhoInv
          end do
          
          do i = lo(1), hi(1)+1
             rho0 = RoeW(i-1)*RoeW(i)
             fac = 1.d0/(RoeW(i-1)+RoeW(i))
             Y0 = (Y(i-1,:)*RoeW(i-1)+Y(i,:)*RoeW(i))*fac
             T0 = (UG(i-1,j,UTEMP)*RoeW(i-1)+UG(i,j,UTEMP)*RoeW(i))*fac
             v0(1) = (v(i-1,1)*RoeW(i-1)+v(i,1)*RoeW(i))*fac
             v0(2) = (v(i-1,2)*RoeW(i-1)+v(i,2)*RoeW(i))*fac
             v0(3) = 0.d0
             
             call floor_species(nspec,Y0)
             
             ! egv1: left matrix;  egv2: right matrix
             call get_eigen_matrices(rho0, Y0, T0, v0, egv1, egv2)

             do ii=-3,2
                Uch(1) = UG(i+ii,j,UMX)
                Uch(2) = UG(i+ii,j,UMY)
                Uch(3) = 0.d0  ! because of 2d
                Uch(4) = UG(i+ii,j,UEDEN)
                
                eref = eos_get_eref(Y(i+ii,:))
                Uch(4) = Uch(4) - UG(i+ii,j,URHO) * eref
                
                do n=1,nspec
                   Uch(CFS+n-1) = UG(i+ii,j,UFS+n-1)
                end do
                
                do n=1,NCHARV
                   charv(ii,n) = dot_product(egv1(:,n),Uch)
                end do
             end do

             if (do_mdcd) then
                do ivar=1,NCHARV
                   call mdcd(charv(:,ivar), cvl(ivar), cvr(ivar))
                end do
             else
                do ivar=1,NCHARV
                   call weno5_face(charv(:,ivar), cvl(ivar), cvr(ivar))
                end do
             end if
             
             egv1 = transpose(egv2)  ! egv1 now holds transposed right matrix
             
             do n=1,NCHARV
                UL(i,UMX  ) = UL(i,UMX  ) + cvl(n)*egv2(1,n)
                UR(i,UMX  ) = UR(i,UMX  ) + cvr(n)*egv2(1,n)
                UL(i,UMY  ) = UL(i,UMY  ) + cvl(n)*egv2(2,n)
                UR(i,UMY  ) = UR(i,UMY  ) + cvr(n)*egv2(2,n)
                UL(i,UEDEN) = UL(i,UEDEN) + cvl(n)*egv2(4,n)
                UR(i,UEDEN) = UR(i,UEDEN) + cvr(n)*egv2(4,n)
             end do
             
             do m=1,nspec
                UL(i,UFS+m-1) = UL(i,UFS+m-1) + dot_product(cvl, egv1(:,CFS+m-1))
                UR(i,UFS+m-1) = UR(i,UFS+m-1) + dot_product(cvr, egv1(:,CFS+m-1))
                UL(i,URHO   ) = UL(i,URHO) + UL(i,UFS+m-1)
                UR(i,URHO   ) = UR(i,URHO) + UR(i,UFS+m-1)
             end do
             
             rhoInv = 1.d0/UL(i,URHO)
             do n=1,nspec
                Y0(n) = UL(i,UFS+n-1) * rhoInv
             end do
             call floor_species(nspec, Y0)
             eref = eos_get_eref(Y0)
             UL(i,UEDEN) = UL(i,UEDEN) + UL(i,URHO) * eref
             do n=1,nspec
                UL(i,UFS+n-1) = UL(i,URHO)*Y0(n)
             end do
             
             rhoInv = 1.d0/UR(i,URHO)
             do n=1,nspec
                Y0(n) = UR(i,UFS+n-1) * rhoInv
             end do
             call floor_species(nspec, Y0)
             eref = eos_get_eref(Y0)
             UR(i,UEDEN) = UR(i,UEDEN) + UR(i,URHO) * eref
             do n=1,nspec
                UR(i,UFS+n-1) = UR(i,URHO)*Y0(n)
             end do
             
             UL(i,UTEMP) = T0
             UR(i,UTEMP) = T0
          end do

          call get_hyper_bc_flag(1,(/lo(1),j/),(/hi(1)+1,j/),domlo,domhi,dx,bc_flag)

          call riemann(lo(1),hi(1),UL,UR,lo(1),hi(1)+1,flux,lo(1),hi(1)+1, &
               dir=1, bc_flag=bc_flag)
          do n=1,NVAR
             do i=lo(1),hi(1)+1
                fx(i,j,n) = fx(i,j,n) + 0.5d0*flux(i,n)
             end do
          end do
       end do
          
       Nullify(UG)
    end do

    deallocate(Y, RoeW, v, flux)
    deallocate(UG1,UG2,UL,UR)
    
  end subroutine hypterm_x
    

  subroutine hypterm_y(lo,hi,domlo,domhi,U,Ulo,Uhi,fy,fylo,fyhi,dx)

    use meth_params_module, only : NVAR, URHO, UMX, UMY, UTEMP, UFS, UEDEN, NSPEC, NCHARV, CFS, do_mdcd
    use eigen_module, only : get_eigen_matrices
    use renorm_module, only : floor_species
    use eos_module, only : eos_get_eref
    use mdcd_module, only : mdcd
    use weno_module, only : weno4_gauss, weno5_face
    use riemann_module, only : riemann
    use RNS_boundary_module, only : get_hyper_bc_flag

    integer, intent(in) :: lo(2), hi(2), domlo(2), domhi(2), Ulo(2), Uhi(2), fylo(2), fyhi(2)
    double precision, intent(in) :: dx(2)
    double precision, intent(in   ) ::  U( Ulo(1): Uhi(1), Ulo(2): Uhi(2),NVAR)
    double precision, intent(inout) :: fy(fylo(1):fyhi(1),fylo(2):fyhi(2),NVAR)

    integer :: i, j, n, ii, jj, ivar, m, g, bc_flag(2)
    double precision :: RoeWl, RoeWr, rhoInvl, rhoInvr
    double precision, allocatable :: flux(:,:)
    double precision, dimension(:,:,:), allocatable, target :: UG1,UG2
    double precision, dimension(:,:),   allocatable, target :: UL,UR
    double precision, dimension(:,:,:), pointer :: UG
    double precision :: rhoInv, rho0, Y0(nspec), T0, v0(3), fac, eref
    double precision :: egv1(NCHARV,NCHARV), egv2(NCHARV,NCHARV), Uch(NCHARV), charv(-3:2,NCHARV)
    double precision :: cvl(NCHARV), cvr(NCHARV)
    
    allocate(flux(lo(2):hi(2)+1,NVAR))

    allocate(UG1(lo(1):hi(1),lo(2)-3:hi(2)+3,NVAR))
    allocate(UG2(lo(1):hi(1),lo(2)-3:hi(2)+3,NVAR))
    allocate(UL(lo(2):hi(2)+1,NVAR))
    allocate(UR(lo(2):hi(2)+1,NVAR))

    UG1 = 0.d0
    UG2 = 0.d0
    
    do j = lo(2)-3, hi(2)+3
       do i = lo(1), hi(1)
          ! given U(i-2:i+2,j,:), compute UG1(i,j,:) and UG2(i,j,:)
          rho0 = U(i,j,URHO)
          rhoInv = 1.d0/rho0
          do n=1,nspec
             Y0(n) = U(i,j,UFS+n-1)*rhoInv
          end do
          v0(1) = U(i,j,UMX)*rhoInv
          v0(2) = U(i,j,UMY)*rhoInv
          v0(3) = 0.d0
          T0 = U(i,j,UTEMP)

          ! egv1: left matrix;  egv2: right matrix
          call get_eigen_matrices(rho0, Y0, T0, v0, egv1, egv2)

          do ii=-2,2
             Uch(1) = U(i+ii,j,UMX)
             Uch(2) = U(i+ii,j,UMY)
             Uch(3) = 0.d0   ! becuase of 2d 
             Uch(4) = U(i+ii,j,UEDEN)
             rhoInv = 1.d0/U(i+ii,j,URHO)
             do n=1,nspec
                Uch(CFS+n-1) = U(i+ii,j,UFS+n-1)
                Y0(n) = Uch(CFS+n-1)*rhoInv
             end do
             eref = eos_get_eref(Y0)
             Uch(4) = Uch(4) - U(i+ii,j,URHO)*eref
             
             do n=1,NCHARV
                charv(ii,n) = dot_product(egv1(:,n),Uch)
             enddo
          enddo

          do ivar=1,NCHARV
             call weno4_gauss(charv(-2:2,ivar), cvl(ivar), cvr(ivar))
          enddo

          egv1 = transpose(egv2)  ! egv1 now holds transposed right matrix

          do n=1,NCHARV
             UG1(i,j,UMX  ) = UG1(i,j,UMX  ) + cvl(n)*egv2(1,n)
             UG2(i,j,UMX  ) = UG2(i,j,UMX  ) + cvr(n)*egv2(1,n)
             UG1(i,j,UMY  ) = UG1(i,j,UMY  ) + cvl(n)*egv2(2,n)
             UG2(i,j,UMY  ) = UG2(i,j,UMY  ) + cvr(n)*egv2(2,n)
             UG1(i,j,UEDEN) = UG1(i,j,UEDEN) + cvl(n)*egv2(4,n)
             UG2(i,j,UEDEN) = UG2(i,j,UEDEN) + cvr(n)*egv2(4,n)
          end do
          
          do m=1,nspec
             UG1(i,j,UFS+m-1) = UG1(i,j,UFS+m-1) + dot_product(cvl, egv1(:,CFS+m-1))
             UG2(i,j,UFS+m-1) = UG2(i,j,UFS+m-1) + dot_product(cvr, egv1(:,CFS+m-1))
             UG1(i,j,URHO   ) = UG1(i,j,URHO   ) + UG1(i,j,UFS+m-1)
             UG2(i,j,URHO   ) = UG2(i,j,URHO   ) + UG2(i,j,UFS+m-1)
          end do
          
          rhoInv = 1.d0/UG1(i,j,URHO)
          do n=1,nspec
             Y0(n) = UG1(i,j,UFS+n-1) * rhoInv
          end do
          call floor_species(nspec, Y0)
          eref = eos_get_eref(Y0)
          UG1(i,j,UEDEN) = UG1(i,j,UEDEN) + UG1(i,j,URHO) * eref
          do n=1,nspec
             UG1(i,j,UFS+n-1) = UG1(i,j,URHO)*Y0(n)
          end do
          
          rhoInv = 1.d0/UG2(i,j,URHO)
          do n=1,nspec
             Y0(n) = UG2(i,j,UFS+n-1) * rhoInv
          end do
          call floor_species(nspec, Y0)
          eref = eos_get_eref(Y0)
          UG2(i,j,UEDEN) = UG2(i,j,UEDEN) + UG2(i,j,URHO) * eref
          do n=1,nspec
             UG2(i,j,UFS+n-1) = UG2(i,j,URHO)*Y0(n)
          end do
          
          UG1(i,j,UTEMP) = U(i,j,UTEMP)
          UG2(i,j,UTEMP) = U(i,j,UTEMP)
       end do
    end do

    do g = 1, 2
       if (g .eq. 1) then
          UG => UG1
       else
          UG => UG2
       end if

       do i = lo(1), hi(1)
          UL = 0.d0
          UR = 0.d0
          do j = lo(2),hi(2)+1
             ! given UG(i,j-3:j+2,:), compute UL(j,:) and UR(j,:)
             rhoInvl = 1.d0/UG(i,j-1,URHO)
             rhoInvr = 1.d0/UG(i,j  ,URHO)
             RoeWl = sqrt(UG(i,j-1,URHO))
             RoeWr = sqrt(UG(i,j  ,URHO))
             rho0 = RoeWl*Roewr
             fac = 1.d0/(RoeWl+RoeWr)
             do n=1,nspec
                Y0(n) = (UG(i,j-1,UFS+n-1)*rhoInvl*RoeWl+UG(i,j,UFS+n-1)*rhoInvr*RoeWr)*fac
             end do
             T0 = (UG(i,j-1,UTEMP)*RoeWl+UG(i,j,UTEMP)*RoeWr)*fac
             v0(1) = (UG(i,j-1,UMY)*rhoInvl*RoeWl+UG(i,j,UMY)*rhoInvr*RoeWr)*fac
             v0(2) = (UG(i,j-1,UMX)*rhoInvl*RoeWl+UG(i,j,UMX)*rhoInvr*RoeWr)*fac
             v0(3) = 0.d0

             call floor_species(nspec,Y0)

             ! egv1: left matrix;  egv2: right matrix
             call get_eigen_matrices(rho0, Y0, T0, v0, egv1, egv2)

             do jj=-3,2
                Uch(1) = UG(i,j+jj,UMY)
                Uch(2) = UG(i,j+jj,UMX)
                Uch(3) = 0d0
                Uch(4) = UG(i,j+jj,UEDEN)
                rhoInv = 1.d0/UG(i,j+jj,URHO)
                do n=1,nspec
                   Uch(CFS+n-1) = UG(i,j+jj,UFS+n-1)
                   Y0(n) = Uch(CFS+n-1)*rhoInv
                end do
                eref = eos_get_eref(Y0)
                Uch(4) = Uch(4) - UG(i,j+jj,URHO)*eref
                do n=1,NCHARV
                   charv(jj,n) = dot_product(egv1(:,n),Uch)
                end do
             end do
             
             if (do_mdcd) then
                do ivar=1,NCHARV
                   call mdcd(charv(:,ivar), cvl(ivar), cvr(ivar))
                end do
             else
                do ivar=1,NCHARV
                   call weno5_face(charv(:,ivar), cvl(ivar), cvr(ivar))
                end do
             end if
             
             egv1 = transpose(egv2)  ! egv1 now holds transposed right matrix

             do n=1,NCHARV
                UL(j,UMY  ) = UL(j,UMY  ) + cvl(n)*egv2(1,n)
                UR(j,UMY  ) = UR(j,UMY  ) + cvr(n)*egv2(1,n)
                UL(j,UMX  ) = UL(j,UMX  ) + cvl(n)*egv2(2,n)
                UR(j,UMX  ) = UR(j,UMX  ) + cvr(n)*egv2(2,n)
                UL(j,UEDEN) = UL(j,UEDEN) + cvl(n)*egv2(4,n)
                UR(j,UEDEN) = UR(j,UEDEN) + cvr(n)*egv2(4,n)
             end do
             
             do m=1,nspec
                UL(j,UFS+m-1) = UL(j,UFS+m-1) + dot_product(cvl, egv1(:,CFS+m-1))
                UR(j,UFS+m-1) = UR(j,UFS+m-1) + dot_product(cvr, egv1(:,CFS+m-1))
                UL(j,URHO   ) = UL(j,URHO) + UL(j,UFS+m-1)
                UR(j,URHO   ) = UR(j,URHO) + UR(j,UFS+m-1)
             end do
             
             rhoInv = 1.d0/UL(j,URHO)
             do n=1,nspec
                Y0(n) = UL(j,UFS+n-1) * rhoInv
             end do
             call floor_species(nspec, Y0)
             eref = eos_get_eref(Y0)
             UL(j,UEDEN) = UL(j,UEDEN) + UL(j,URHO) * eref
             do n=1,nspec
                UL(j,UFS+n-1) = UL(j,URHO)*Y0(n)
             end do
             
             rhoInv = 1.d0/UR(j,URHO)
             do n=1,nspec
                Y0(n) = UR(j,UFS+n-1) * rhoInv
             end do
             call floor_species(nspec, Y0)
             eref = eos_get_eref(Y0)
             UR(j,UEDEN) = UR(j,UEDEN) + UR(j,URHO) * eref
             do n=1,nspec
                UR(j,UFS+n-1) = UR(j,URHO)*Y0(n)
             end do
             
             UL(j,UTEMP) = T0
             UR(j,UTEMP) = T0
          end do

          call get_hyper_bc_flag(2,(/i,lo(2)/),(/i,hi(2)+1/),domlo,domhi,dx,bc_flag)

          call riemann(lo(2),hi(2),UL,UR,lo(2),hi(2)+1,flux,lo(2),hi(2)+1, &
               dir=2, bc_flag=bc_flag)
          do n=1,NVAR
             do j=lo(2),hi(2)+1
                fy(i,j,n) = fy(i,j,n) + 0.5d0*flux(j,n)
             end do
          end do
       end do
       
       Nullify(UG)
    end do

    deallocate(flux)
    deallocate(UG1,UG2,UL,UR)
    
  end subroutine hypterm_y

  
  subroutine add_artifical_viscocity(lo,hi,U,Ulo,Uhi,fx,fxlo,fxhi,fy,fylo,fyhi,dx)

    use meth_params_module, only : NVAR, URHO, UMX, UMY, UTEMP, UEDEN, UFS, NSPEC, difmag
    use eos_module, only : eos_get_T, eos_get_c

    integer, intent(in) :: lo(2), hi(2), Ulo(2), Uhi(2), fxlo(2), fxhi(2), fylo(2), fyhi(2)
    double precision, intent(in   ) :: dx(2)
    double precision, intent(in   ) ::  U( Ulo(1): Uhi(1), Ulo(2): Uhi(2),NVAR)
    double precision, intent(inout) :: fx(fxlo(1):fxhi(1),fxlo(2):fxhi(2),NVAR)
    double precision, intent(inout) :: fy(fylo(1):fyhi(1),fylo(2):fyhi(2),NVAR)
    
    double precision, allocatable :: vx(:,:), vy(:,:), cs(:,:)
    double precision :: rhoInv, hdvdx, hdvdy, hdivv, dxinv(2), e, T, Y(nspec)
    double precision :: nu, fac, cmin
    integer :: i, j, n

    dxinv = 1.d0/dx
    
    allocate(vx(lo(1)-1:hi(1)+1,lo(2)-1:hi(2)+1))
    allocate(vy(lo(1)-1:hi(1)+1,lo(2)-1:hi(2)+1))
    allocate(cs(lo(1)-1:hi(1)+1,lo(2)-1:hi(2)+1))
    
    do    j=lo(2)-1,hi(2)+1
       do i=lo(1)-1,hi(1)+1
          rhoInv = 1.d0/U(i,j,URHO)
          vx(i,j) = U(i,j,UMX)*rhoInv
          vy(i,j) = U(i,j,UMY)*rhoInv
          e  = U(i,j,UEDEN)*rhoInv - 0.5d0*(vx(i,j)**2+vy(i,j)**2)
          Y = U(i,j,UFS:UFS+NSPEC-1)*rhoInv
          T = U(i,j,UTEMP)
          call eos_get_T(T, e, Y)
          call eos_get_c(cs(i,j), U(i,j,URHO), T, Y)
       end do
    end do

    ! x-direction

    fac = 0.25d0*dx(1)*dxinv(2)
    
    do j=lo(2),hi(2)
       do i=lo(1),hi(1)+1
          hdvdx = vx(i,j)-vx(i-1,j)
          hdvdy = fac*(-vy(i-1,j-1)-vy(i,j-1)+vy(i-1,j+1)+vy(i,j+1))
          hdivv = hdvdx + hdvdy
          if (hdivv .lt. 0.d0) then
             cmin = min(cs(i-1,j),cs(i,j))
             nu = difmag * hdivv * min(hdivv*hdivv/(cmin*cmin*difmag), 1.d0)
             do n=1,NVAR
                if (n.ne.UTEMP) then
                   fx(i,j,n) = fx(i,j,n) - nu*(U(i-1,j,n)-U(i,j,n))
                end if
             end do
          end if
       end do
    end do

    ! y-direction

    fac = 0.25d0*dxinv(1)*dx(2)
    
    do j=lo(2),hi(2)+1
       do i=lo(1),hi(1)
          hdvdx = fac*(-vx(i-1,j-1)-vx(i-1,j)+vx(i+1,j-1)+vx(i+1,j))
          hdvdy = vy(i,j)-vy(i,j-1)
          hdivv = hdvdx + hdvdy
          if (hdivv .lt. 0.d0) then
             cmin = min(cs(i,j-1),cs(i,j))
             nu = difmag * hdivv * min(hdivv*hdivv/(cmin*cmin*difmag), 1.d0)
             do n=1,NVAR
                if (n.ne.UTEMP) then
                   fy(i,j,n) = fy(i,j,n) - nu*(U(i,j-1,n)-U(i,j,n))
                end if
             end do
          end if
       end do
    end do

    deallocate(vx,vy,cs)

  end subroutine add_artifical_viscocity

end module hypterm_module

