module physbndry_reg_module

  use multifab_module
  use layout_module

  implicit none

  type :: physbndry_reg
     integer :: idim = -1
     integer :: iface = 0
     integer :: nc = -1
     logical :: stencil_flag
     logical, pointer :: localstatus(:)
     type(multifab) :: data
     type(layout) :: la
  end type physbndry_reg

  interface isValid
     module procedure get_localstatus
  end interface

  private

  public physbndry_reg_build, physbndry_reg_destroy, physbndry_reg, isValid

contains

  subroutine physbndry_reg_build(pbr, la0, nc_in, idim_in, iface_in, stencil_flag_in)
    use bl_error_module
    type(layout), intent(in) :: la0
    type(physbndry_reg), intent(out) :: pbr
    integer, intent(in) :: nc_in, idim_in, iface_in
    logical, intent(in) :: stencil_flag_in

    type(box) :: rbox, pd
    type(box), allocatable :: bxs(:)
    type(boxarray)         :: baa
    integer :: ibox, ngb, nlb
    integer ::  lo(la0%lap%dim),  hi(la0%lap%dim)
    integer :: plo(la0%lap%dim), phi(la0%lap%dim)
    logical :: pmask(la0%lap%dim)

    pbr%nc = nc_in
    pbr%idim = idim_in
    pbr%iface = iface_in
    pbr%stencil_flag = stencil_flag_in

    pd = get_pd(la0)
    pmask = get_pmask(la0)

    plo = lwb(pd)
    phi = upb(pd)

    ngb = nboxes(la0)
    allocate(bxs(ngb))

    do ibox=1,ngb
       rbox = get_box(la0,ibox)
       lo = lwb(rbox)
       hi = upb(rbox)

       if (pmask(idim_in)) then
          hi = lo
       else if (iface_in .eq. -1) then
          if (lo(idim_in) .ne. plo(idim_in)) then ! interior
             hi = lo
          else
             hi(idim_in) = lo(idim_in)
          end if
       else if (iface_in .eq. 1) then
          if (hi(idim_in) .ne. phi(idim_in)) then ! interior
             lo = hi
          else
             lo(idim_in) = hi(idim_in)
          end if
       else
          call bl_error("physbndry_reg_build: invalid iface")
       end if
       
       call build(bxs(ibox), lo, hi)
    end do

    call build(baa, bxs, sort=.false.)
    call build(pbr%la, baa, boxarray_bbox(baa), explicit_mapping=get_proc(la0))
    call build(pbr%data, pbr%la, nc=nc_in, ng=0, stencil=stencil_flag_in)
    call destroy(baa)

    deallocate(bxs)

    nlb = nfabs(pbr%data)
    allocate(pbr%localstatus(nlb))

    if (pmask(pbr%idim)) then
       pbr%localstatus = .false.
    else
       do ibox=1, nlb
          lo = lwb(get_box(pbr%data,ibox))
          hi = upb(get_box(pbr%data,ibox))
          if (any(lo.ne.hi)) then
             pbr%localstatus(ibox) = .true.
          else
             pbr%localstatus(ibox) = .false.
          end if
       end do
    end if

  end subroutine physbndry_reg_build


  subroutine physbndry_reg_destroy(pbr)
    type(physbndry_reg), intent(inout) :: pbr
    pbr%idim = -1
    pbr%iface = 0
    pbr%nc = -1
    call destroy(pbr%la)
    call destroy(pbr%data)
    deallocate(pbr%localstatus)
  end subroutine physbndry_reg_destroy

  function get_localstatus(pbr, ibox) result(r)
    integer, intent(in) :: ibox
    type(physbndry_reg), intent(in) :: pbr
    logical :: r
    if (pbr%nc <= 0) then
       r = .false.
    else
       r = pbr%localstatus(ibox)
    end if
  end function get_localstatus

end module physbndry_reg_module
