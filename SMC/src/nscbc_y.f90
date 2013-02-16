
  subroutine outlet_ylo(lo,hi,ngq,ngc,dx,Q,con,fd,rhs,aux,dlo,dhi)
    integer, intent(in) :: lo(3), hi(3), ngq, ngc, dlo(3), dhi(3)
    double precision,intent(in   )::dx(3)
    double precision,intent(in   )::Q  (-ngq+lo(1):hi(1)+ngq,-ngq+lo(2):hi(2)+ngq,-ngq+lo(3):hi(3)+ngq,nprim)
    double precision,intent(in   )::con(-ngc+lo(1):hi(1)+ngc,-ngc+lo(2):hi(2)+ngc,-ngc+lo(3):hi(3)+ngc,ncons)
    double precision,intent(in   )::fd (lo(1):hi(1),lo(2):hi(2),lo(3):hi(3),ncons)
    double precision,intent(inout)::rhs(lo(1):hi(1),lo(2):hi(2),lo(3):hi(3),ncons)
    double precision,intent(in   )::aux(naux,lo(1):hi(1),lo(3):hi(3))

    integer :: i,j,k,n
    double precision :: dxinv(3)
    double precision :: rho, u, v, w, T, pres, Y(nspecies), h(nspecies), rhoE
    double precision :: dpdn, dudn, dvdn, dwdn, drhodn, dYdn(nspecies)
    double precision :: L(5+nspecies), Ltr(5+nspecies), lhs(ncons)
    double precision :: S_p, S_Y(nspecies), d_u, d_v, d_w, d_p
    double precision :: hcal, cpWT, gam1

    double precision, dimension(lo(1):hi(1),lo(3):hi(3)) :: &
         dpdx, dpdz, dudx, dvdx, dvdz, dwdz

    j = lo(2)

    do n=1,3
       dxinv(n) = 1.0d0 / dx(n)
    end do

    call comp_trans_deriv_y(j,lo,hi,ngq,Q,dxinv,dlo,dhi,dpdx,dpdz,dudx,dvdx,dvdz,dwdz)

    !$omp parallel do private(i,k,n,rho,u,v,w,T,pres,Y,h,rhoE) &
    !$omp private(dpdn, dudn,dvdn,dwdn, drhodn, dYdn, L, Ltr, lhs) &
    !$omp private(S_p, S_Y, d_u, d_v, d_w, d_p, hcal, cpWT, gam1)
    do k=lo(3),hi(3)
       do i=lo(1),hi(1)

          rho  = q  (i,j,k,qrho)
          u    = q  (i,j,k,qu)
          v    = q  (i,j,k,qv)
          w    = q  (i,j,k,qw)
          pres = q  (i,j,k,qpres)
          T    = q  (i,j,k,qtemp)
          Y    = q  (i,j,k,qy1:qy1+nspecies-1)
          h    = q  (i,j,k,qh1:qh1+nspecies-1)
          rhoE = con(i,j,k,iene)

          drhodn     = dxinv(2)*first_deriv_rb(q(i,j:j+3,k,qrho))
          dudn       = dxinv(2)*first_deriv_rb(q(i,j:j+3,k,qu))
          dvdn       = dxinv(2)*first_deriv_rb(q(i,j:j+3,k,qv))
          dwdn       = dxinv(2)*first_deriv_rb(q(i,j:j+3,k,qw))
          dpdn       = dxinv(2)*first_deriv_rb(q(i,j:j+3,k,qpres))
          do n=1,nspecies
             dYdn(n) = dxinv(2)*first_deriv_rb(q(i,j:j+3,k,qy1+n-1))
          end do

          ! Simple 1D LODI 
          L(1) = (v-aux(ics,i,k))*0.5d0*(dpdn-rho*aux(ics,i,k)*dvdn)
          L(2) = v*(drhodn-dpdn/aux(ics,i,k)**2)
          L(3) = v*dudn
          L(4) = v*dwdn
          L(5) = sigma*aux(ics,i,k)*(1.d0-Ma2_ylo)/(2.d0*Lydomain)*(pres-Pinfty)
          L(6:) = v*dYdn

          ! multi-D effects
          Ltr(5) = -0.5d0*(u*dpdx(i,k)+w*dpdz(i,k) &
               + aux(igamma,i,k)*pres*(dudx(i,k)+dwdz(i,k)) &
               + rho*aux(ics,i,k)*(u*dvdx(i,k)+w*dvdz(i,k)))
          L(5) = L(5) + min(0.99d0, (1.0d0-abs(v)/aux(ics,i,k))) * Ltr(5) 

          ! viscous and reaction effects
          d_u = fd(i,j,k,imx) / rho
          d_v = fd(i,j,k,imy) / rho
          d_w = fd(i,j,k,imz) / rho

          S_p = 0.d0
          d_p = fd(i,j,k,iene) - (d_u*con(i,j,k,imx)+d_v*con(i,j,k,imy)+d_w*con(i,j,k,imz))
          cpWT = aux(icp,i,k)*aux(iWbar,i,k)*T
          gam1 = aux(igamma,i,k) - 1.d0
          do n=1,nspecies
             hcal = h(n) - cpWT*inv_mwt(n)
             S_p    = S_p - hcal*aux(iwdot1+n-1,i,k)
             S_Y(n) = aux(iwdot1+n-1,i,k) / rho
             d_p    = d_p - hcal*fd(i,j,k,iry1+n-1)
          end do
          S_p = gam1 * S_p
          d_p = gam1 * d_p 

          ! S_Y seems to cause instabilities
          ! So set it to zero for now
          S_Y = 0.d0
          
          L(5) = L(5) + 0.5d0*(S_p + d_p + rho*aux(ics,i,k)*d_v)

          if (v > 0.d0) then
             L(2) = -S_p/aux(ics,i,k)**2
             L(3) = 0.d0
             L(4) = 0.d0
             L(5) = 0.5d0*S_p
             L(6:) = S_Y      
             
             L(5) = 0.5d0*(S_p + d_p + rho*aux(ics,i,k)*d_v) + Ltr(5) &
                  + outlet_eta*aux(igamma,i,k)*pres*(1.d0-Ma2_ylo)/(2.d0*Lydomain)*v
          end if
          
          call LtoLHS(2, L, lhs, aux(:,i,k), rho, u, v, w, T, Y, h, rhoE)
          
          rhs(i,j,k,:) = rhs(i,j,k,:) - lhs

       end do
    end do
    !$omp end parallel do

  end subroutine outlet_ylo


!   subroutine inlet_ylo(lo,hi,ngq,ngc,dx,Q,con,fd,rhs,aux,qin,dlo,dhi)
!     integer, intent(in) :: lo(3), hi(3), ngq, ngc, dlo(3), dhi(3)
!     double precision,intent(in   )::dx(3)
!     double precision,intent(in   )::Q  (-ngq+lo(1):hi(1)+ngq,-ngq+lo(2):hi(2)+ngq,-ngq+lo(3):hi(3)+ngq,nprim)
!     double precision,intent(in   )::con(-ngc+lo(1):hi(1)+ngc,-ngc+lo(2):hi(2)+ngc,-ngc+lo(3):hi(3)+ngc,ncons)
!     double precision,intent(in   )::fd (lo(1):hi(1),lo(2):hi(2),lo(3):hi(3),ncons)
!     double precision,intent(inout)::rhs(lo(1):hi(1),lo(2):hi(2),lo(3):hi(3),ncons)
!     double precision,intent(in   )::aux(naux,lo(1):hi(1),lo(3):hi(3))
!     double precision,intent(in   )::qin(nqin,lo(1):hi(1),lo(3):hi(3))

!     call bl_error("inlet_ylo not implemented")

!   end subroutine inlet_ylo


  subroutine outlet_yhi(lo,hi,ngq,ngc,dx,Q,con,fd,rhs,aux,dlo,dhi)
    integer, intent(in) :: lo(3), hi(3), ngq, ngc, dlo(3), dhi(3)
    double precision,intent(in   )::dx(3)
    double precision,intent(in   )::Q  (-ngq+lo(1):hi(1)+ngq,-ngq+lo(2):hi(2)+ngq,-ngq+lo(3):hi(3)+ngq,nprim)
    double precision,intent(in   )::con(-ngc+lo(1):hi(1)+ngc,-ngc+lo(2):hi(2)+ngc,-ngc+lo(3):hi(3)+ngc,ncons)
    double precision,intent(in   )::fd (lo(1):hi(1),lo(2):hi(2),lo(3):hi(3),ncons)
    double precision,intent(inout)::rhs(lo(1):hi(1),lo(2):hi(2),lo(3):hi(3),ncons)
    double precision,intent(in   )::aux(naux,lo(1):hi(1),lo(3):hi(3))

    integer :: i,j,k,n
    double precision :: dxinv(3)
    double precision :: rho, u, v, w, T, pres, Y(nspecies), h(nspecies), rhoE
    double precision :: dpdn, dudn, dvdn, dwdn, drhodn, dYdn(nspecies)
    double precision :: L(5+nspecies), Ltr(5+nspecies), lhs(ncons)
    double precision :: S_p, S_Y(nspecies), d_u, d_v, d_w, d_p
    double precision :: hcal, cpWT, gam1

    double precision, dimension(lo(1):hi(1),lo(3):hi(3)) :: &
         dpdx, dpdz, dudx, dvdx, dvdz, dwdz

    j = hi(2)

    do n=1,3
       dxinv(n) = 1.0d0 / dx(n)
    end do

    call comp_trans_deriv_y(j,lo,hi,ngq,Q,dxinv,dlo,dhi,dpdx,dpdz,dudx,dvdx,dvdz,dwdz)

    !$omp parallel do private(i,k,n,rho,u,v,w,T,pres,Y,h,rhoE) &
    !$omp private(dpdn, dudn, dvdn, dwdn, drhodn, dYdn, L, Ltr, lhs) &
    !$omp private(S_p, S_Y, d_u, d_v, d_w, d_p,  hcal, cpWT, gam1)
    do k=lo(3),hi(3)
       do i=lo(1),hi(1)

          rho  = q  (i,j,k,qrho)
          u    = q  (i,j,k,qu)
          v    = q  (i,j,k,qv)
          w    = q  (i,j,k,qw)
          pres = q  (i,j,k,qpres)
          T    = q  (i,j,k,qtemp)
          Y    = q  (i,j,k,qy1:qy1+nspecies-1)
          h    = q  (i,j,k,qh1:qh1+nspecies-1)
          rhoE = con(i,j,k,iene)
          
          drhodn     = dxinv(2)*first_deriv_lb(q(i,j-3:j,k,qrho))
          dudn       = dxinv(2)*first_deriv_lb(q(i,j-3:j,k,qu))
          dvdn       = dxinv(2)*first_deriv_lb(q(i,j-3:j,k,qv))
          dwdn       = dxinv(2)*first_deriv_lb(q(i,j-3:j,k,qw))
          dpdn       = dxinv(2)*first_deriv_lb(q(i,j-3:j,k,qpres))
          do n=1,nspecies
             dYdn(n) = dxinv(2)*first_deriv_lb(q(i,j-3:j,k,qy1+n-1))
          end do
          
          ! Simple 1D LODI 
          L(1) = sigma*aux(ics,i,k)*(1.d0-Ma2_yhi)/(2.d0*Lydomain)*(pres-Pinfty)
          L(2) = v*(drhodn-dpdn/aux(ics,i,k)**2)
          L(3) = v*dudn
          L(4) = v*dwdn
          L(5) = (v+aux(ics,i,k))*0.5d0*(dpdn+rho*aux(ics,i,k)*dvdn)
          L(6:) = v*dYdn
          
          ! multi-D effects
          Ltr(1) = -0.5d0*(u*dpdx(i,k)+w*dpdz(i,k) &
               + aux(igamma,i,k)*pres*(dudx(i,k)+dwdz(i,k)) &
               - rho*aux(ics,i,k)*(u*dvdx(i,k)+w*dvdz(i,k)))
          L(1) = L(1) + min(0.99d0, (1.0d0-abs(v)/aux(ics,i,k))) * Ltr(1)
          
          ! viscous and reaction effects
          d_u = fd(i,j,k,imx) / rho
          d_v = fd(i,j,k,imy) / rho
          d_w = fd(i,j,k,imz) / rho

          S_p = 0.d0
          d_p = fd(i,j,k,iene) - (d_u*con(i,j,k,imx)+d_v*con(i,j,k,imy)+d_w*con(i,j,k,imz))
          cpWT = aux(icp,i,k)*aux(iWbar,i,k)*T
          gam1 = aux(igamma,i,k) - 1.d0
          do n=1,nspecies
             hcal = h(n) - cpWT*inv_mwt(n)
             S_p    = S_p - hcal*aux(iwdot1+n-1,i,k)
             S_Y(n) = aux(iwdot1+n-1,i,k) / rho
             d_p    = d_p - hcal*fd(i,j,k,iry1+n-1)
          end do
          S_p = gam1 * S_p
          d_p = gam1 * d_p 

          ! S_Y seems to cause instabilities
          ! So set it to zero for now
          S_Y = 0.d0
          
          L(1) = L(1) + 0.5d0*(S_p + d_p - rho*aux(ics,i,k)*d_v)
          
          if (v < 0.d0) then
             L(1) = 0.5d0*S_p
             L(2) = -S_p/aux(ics,i,k)**2
             L(3) = 0.d0
             L(4) = 0.d0
             L(6:) = S_Y      
             
             L(1) = 0.5d0*(S_p + d_p - rho*aux(ics,i,k)*d_v) + Ltr(1) &
                  - outlet_eta*aux(igamma,i,k)*pres*(1.d0-Ma2_yhi)/(2.d0*Lydomain)*v
          end if
          
          call LtoLHS(2, L, lhs, aux(:,i,k), rho, u, v, w, T, Y, h, rhoE)
          
          rhs(i,j,k,:) = rhs(i,j,k,:) - lhs

       end do
    end do
    !$omp end parallel do

  end subroutine outlet_yhi


!   subroutine inlet_yhi(lo,hi,ngq,ngc,dx,Q,con,fd,rhs,aux,qin,dlo,dhi)
!     integer, intent(in) :: lo(3), hi(3), ngq, ngc, dlo(3), dhi(3)
!     double precision,intent(in   )::dx(3)
!     double precision,intent(in   )::Q  (-ngq+lo(1):hi(1)+ngq,-ngq+lo(2):hi(2)+ngq,-ngq+lo(3):hi(3)+ngq,nprim)
!     double precision,intent(in   )::con(-ngc+lo(1):hi(1)+ngc,-ngc+lo(2):hi(2)+ngc,-ngc+lo(3):hi(3)+ngc,ncons)
!     double precision,intent(in   )::fd (lo(1):hi(1),lo(2):hi(2),lo(3):hi(3),ncons)
!     double precision,intent(inout)::rhs(lo(1):hi(1),lo(2):hi(2),lo(3):hi(3),ncons)
!     double precision,intent(in   )::aux(naux,lo(1):hi(1),lo(3):hi(3))
!     double precision,intent(in   )::qin(nqin,lo(1):hi(1),lo(3):hi(3))

!     call bl_error("inlet_yhi not implemented")

!   end subroutine inlet_yhi


  subroutine comp_trans_deriv_y(j,lo,hi,ngq,Q,dxinv,dlo,dhi,dpdx,dpdz,dudx,dvdx,dvdz,dwdz)
    integer, intent(in) :: j, lo(3), hi(3), ngq,dlo(3),dhi(3)
    double precision, intent(in) :: dxinv(3)
    double precision, intent(in) :: Q(-ngq+lo(1):hi(1)+ngq,-ngq+lo(2):hi(2)+ngq,-ngq+lo(3):hi(3)+ngq,nprim)
    double precision, intent(out):: dpdx(lo(1):hi(1),lo(3):hi(3))
    double precision, intent(out):: dpdz(lo(1):hi(1),lo(3):hi(3))
    double precision, intent(out):: dudx(lo(1):hi(1),lo(3):hi(3))
    double precision, intent(out):: dvdx(lo(1):hi(1),lo(3):hi(3))
    double precision, intent(out):: dvdz(lo(1):hi(1),lo(3):hi(3))
    double precision, intent(out):: dwdz(lo(1):hi(1),lo(3):hi(3))

    integer :: i, k, slo(3), shi(3)

    slo = dlo + stencil_ng
    shi = dhi - stencil_ng

    !$omp parallel private(i,k)

    !d()/dx
    !$omp do
    do k=lo(3),hi(3)

       do i=slo(1),shi(1)
          dudx(i,k) = dxinv(1)*first_deriv_8(q(i-4:i+4,j,k,qu))
          dvdx(i,k) = dxinv(1)*first_deriv_8(q(i-4:i+4,j,k,qv))
          dpdx(i,k) = dxinv(1)*first_deriv_8(q(i-4:i+4,j,k,qpres))
       end do

       ! lo-x boundary
       if (dlo(1) .eq. lo(1)) then
          i = lo(1)
          ! use completely right-biased stencil
          dudx(i,k) = dxinv(1)*first_deriv_rb(q(i:i+3,j,k,qu))
          dvdx(i,k) = dxinv(1)*first_deriv_rb(q(i:i+3,j,k,qv))
          dpdx(i,k) = dxinv(1)*first_deriv_rb(q(i:i+3,j,k,qpres))

          i = lo(1)+1
          ! use 3rd-order slightly right-biased stencil
          dudx(i,k) = dxinv(1)*first_deriv_r3(q(i-1:i+2,j,k,qu))
          dvdx(i,k) = dxinv(1)*first_deriv_r3(q(i-1:i+2,j,k,qv))
          dpdx(i,k) = dxinv(1)*first_deriv_r3(q(i-1:i+2,j,k,qpres))

          i = lo(1)+2
          ! use 4th-order stencil
          dudx(i,k) = dxinv(1)*first_deriv_4(q(i-2:i+2,j,k,qu))
          dvdx(i,k) = dxinv(1)*first_deriv_4(q(i-2:i+2,j,k,qv))
          dpdx(i,k) = dxinv(1)*first_deriv_4(q(i-2:i+2,j,k,qpres))

          i = lo(1)+3
          ! use 6th-order stencil
          dudx(i,k) = dxinv(1)*first_deriv_6(q(i-3:i+3,j,k,qu))
          dvdx(i,k) = dxinv(1)*first_deriv_6(q(i-3:i+3,j,k,qv))
          dpdx(i,k) = dxinv(1)*first_deriv_6(q(i-3:i+3,j,k,qpres))
       end if

       ! hi-x boundary
       if (dhi(1) .eq. hi(1)) then
          i = hi(1)-3
          ! use 6th-order stencil
          dudx(i,k) = dxinv(1)*first_deriv_6(q(i-3:i+3,j,k,qu))
          dvdx(i,k) = dxinv(1)*first_deriv_6(q(i-3:i+3,j,k,qv))
          dpdx(i,k) = dxinv(1)*first_deriv_6(q(i-3:i+3,j,k,qpres))

          i = hi(1)-2
          ! use 4th-order stencil
          dudx(i,k) = dxinv(1)*first_deriv_4(q(i-2:i+2,j,k,qu))
          dvdx(i,k) = dxinv(1)*first_deriv_4(q(i-2:i+2,j,k,qv))
          dpdx(i,k) = dxinv(1)*first_deriv_4(q(i-2:i+2,j,k,qpres))

          i = hi(1)-1
          ! use 3rd-order slightly left-biased stencil
          dudx(i,k) = dxinv(1)*first_deriv_l3(q(i-2:i+1,j,k,qu))
          dvdx(i,k) = dxinv(1)*first_deriv_l3(q(i-2:i+1,j,k,qv))
          dpdx(i,k) = dxinv(1)*first_deriv_l3(q(i-2:i+1,j,k,qpres))

          i = hi(1)
          ! use completely left-biased stencil
          dudx(i,k) = dxinv(1)*first_deriv_lb(q(i-3:i,j,k,qu))
          dvdx(i,k) = dxinv(1)*first_deriv_lb(q(i-3:i,j,k,qv))
          dpdx(i,k) = dxinv(1)*first_deriv_lb(q(i-3:i,j,k,qpres))
       end if

    end do
    !$omp end do nowait

    ! d()/dz
    !$omp do
    do k=slo(3),shi(3)
       do i=lo(1),hi(1)
          dvdz(i,k) = dxinv(3)*first_deriv_8(q(i,j,k-4:k+4,qv))
          dwdz(i,k) = dxinv(3)*first_deriv_8(q(i,j,k-4:k+4,qw))
          dpdz(i,k) = dxinv(3)*first_deriv_8(q(i,j,k-4:k+4,qpres))
       end do
    end do
    !$omp end do nowait

    ! lo-z boundary
    if (dlo(3) .eq. lo(3)) then
       k = lo(3)
       ! use completely right-biased stencil
       !$omp do
       do i=lo(1),hi(1)
          dvdz(i,k) = dxinv(3)*first_deriv_rb(q(i,j,k:k+3,qv))
          dwdz(i,k) = dxinv(3)*first_deriv_rb(q(i,j,k:k+3,qw))
          dpdz(i,k) = dxinv(3)*first_deriv_rb(q(i,j,k:k+3,qpres))
       end do
       !$omp end do nowait

       k = lo(3)+1
       ! use 3rd-order slightly right-biased stencil
       !$omp do
       do i=lo(1),hi(1)
          dvdz(i,k) = dxinv(3)*first_deriv_r3(q(i,j,k-1:k+2,qv))
          dwdz(i,k) = dxinv(3)*first_deriv_r3(q(i,j,k-1:k+2,qw))
          dpdz(i,k) = dxinv(3)*first_deriv_r3(q(i,j,k-1:k+2,qpres))
       end do
       !$omp end do nowait

       k = lo(3)+2
       ! use 4th-order stencil
       !$omp do
       do i=lo(1),hi(1)
          dvdz(i,k) = dxinv(3)*first_deriv_4(q(i,j,k-2:k+2,qv))
          dwdz(i,k) = dxinv(3)*first_deriv_4(q(i,j,k-2:k+2,qw))
          dpdz(i,k) = dxinv(3)*first_deriv_4(q(i,j,k-2:k+2,qpres))
       end do
       !$omp end do nowait

       k = lo(3)+3
       ! use 6th-order stencil
       !$omp do
       do i=lo(1),hi(1)
          dvdz(i,k) = dxinv(3)*first_deriv_4(q(i,j,k-3:k+3,qv))
          dwdz(i,k) = dxinv(3)*first_deriv_4(q(i,j,k-3:k+3,qw))
          dpdz(i,k) = dxinv(3)*first_deriv_4(q(i,j,k-3:k+3,qpres))
       end do
       !$omp end do nowait
    end if

    ! hi-z boundary
    if (dhi(3) .eq. hi(3)) then
       k = hi(3)-3
       ! use 6th-order stencil
       !$omp do
       do i=lo(1),hi(1)
          dvdz(i,k) = dxinv(3)*first_deriv_4(q(i,j,k-3:k+3,qv))
          dwdz(i,k) = dxinv(3)*first_deriv_4(q(i,j,k-3:k+3,qw))
          dpdz(i,k) = dxinv(3)*first_deriv_4(q(i,j,k-3:k+3,qpres))
       end do
       !$omp end do nowait

       k = hi(3)-2
       ! use 4th-order stencil
       !$omp do
       do i=lo(1),hi(1)
          dvdz(i,k) = dxinv(3)*first_deriv_4(q(i,j,k-2:k+2,qv))
          dwdz(i,k) = dxinv(3)*first_deriv_4(q(i,j,k-2:k+2,qw))
          dpdz(i,k) = dxinv(3)*first_deriv_4(q(i,j,k-2:k+2,qpres))
       end do
       !$omp end do nowait

       k = hi(3)-1
       ! use 3rd-order slightly left-biased stencil
       !$omp do
       do i=lo(1),hi(1)
          dvdz(i,k) = dxinv(3)*first_deriv_l3(q(i,j,k-2:k+1,qv))
          dwdz(i,k) = dxinv(3)*first_deriv_l3(q(i,j,k-2:k+1,qw))
          dpdz(i,k) = dxinv(3)*first_deriv_l3(q(i,j,k-2:k+1,qpres))
       end do
       !$omp end do nowait

       k = hi(3)
       ! use completely left-biased stencil
       !$omp do
       do i=lo(1),hi(1)
          dvdz(i,k) = dxinv(3)*first_deriv_lb(q(i,j,k-3:k,qv))
          dwdz(i,k) = dxinv(3)*first_deriv_lb(q(i,j,k-3:k,qw))
          dpdz(i,k) = dxinv(3)*first_deriv_lb(q(i,j,k-3:k,qpres))
       end do
       !$omp end do nowait
    end if

    !$omp end parallel
  end subroutine comp_trans_deriv_y
