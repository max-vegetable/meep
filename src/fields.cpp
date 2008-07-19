/* Copyright (C) 2005-2008 Massachusetts Institute of Technology
%
%  This program is free software; you can redistribute it and/or modify
%  it under the terms of the GNU General Public License as published by
%  the Free Software Foundation; either version 2, or (at your option)
%  any later version.
%
%  This program is distributed in the hope that it will be useful,
%  but WITHOUT ANY WARRANTY; without even the implied warranty of
%  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
%  GNU General Public License for more details.
%
%  You should have received a copy of the GNU General Public License
%  along with this program; if not, write to the Free Software Foundation,
%  Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex>

#include "meep.hpp"
#include "meep_internals.hpp"

namespace meep {

fields::fields(structure *s, double m, bool store_pol_energy) :
  S(s->S), v(s->v), user_volume(s->user_volume), gv(s->gv), m(m)
{
  verbosity = 0;
  synchronized_magnetic_fields = false;
  outdir = new char[strlen(s->outdir) + 1]; strcpy(outdir, s->outdir);
  if (v.dim == Dcyl)
    S = S + r_to_minus_r_symmetry(m);
  phasein_time = 0;
  bands = NULL;
  for (int d=0;d<5;d++) k[d] = 0.0;
  is_real = 0;
  a = v.a;
  dt = s->dt;
  t = 0;
  sources = NULL;
  disable_sources = false;
  fluxes = NULL;
  // Time stuff:
  was_working_on = working_on = Other;
  for (int i=0;i<=Other;i++) times_spent[i] = 0.0;
  last_wall_time = last_step_output_wall_time = -1;
  am_now_working_on(Other);

  num_chunks = s->num_chunks;
  typedef fields_chunk *fields_chunk_ptr;
  chunks = new fields_chunk_ptr[num_chunks];
  for (int i=0;i<num_chunks;i++)
    chunks[i] = new fields_chunk(s->chunks[i], outdir, m, store_pol_energy);
  FOR_FIELD_TYPES(ft) {
    for (int ip=0;ip<3;ip++) {
      comm_sizes[ft][ip] = new int[num_chunks*num_chunks];
      for (int i=0;i<num_chunks*num_chunks;i++) comm_sizes[ft][ip][i] = 0;
    }
    typedef double *double_ptr;
    comm_blocks[ft] = new double_ptr[num_chunks*num_chunks];
    for (int i=0;i<num_chunks*num_chunks;i++)
      comm_blocks[ft][i] = 0;
  }
  for (int b=0;b<2;b++) FOR_DIRECTIONS(d)
    if (v.has_boundary((boundary_side)b, d)) boundaries[b][d] = Metallic;
    else boundaries[b][d] = None;
  chunk_connections_valid = false;
  
  // unit directions are periodic by default:
  FOR_DIRECTIONS(d)
    if (v.has_boundary(High, d) && v.has_boundary(Low, d) && d != R
	&& s->user_volume.num_direction(d) == 1)
      use_bloch(d, 0.0);
}

fields::fields(const fields &thef) :
  S(thef.S), v(thef.v), user_volume(thef.user_volume), gv(thef.gv)
{
  verbosity = 0;
  synchronized_magnetic_fields = thef.synchronized_magnetic_fields;
  outdir = new char[strlen(thef.outdir) + 1]; strcpy(outdir, thef.outdir);
  m = thef.m;
  phasein_time = thef.phasein_time;
  bands = NULL;
  for (int d=0;d<5;d++) k[d] = thef.k[d];
  is_real = thef.is_real;
  a = thef.a;
  dt = thef.dt;
  t = thef.t;
  sources = NULL;
  disable_sources = thef.disable_sources;
  fluxes = NULL;
  // Time stuff:
  was_working_on = working_on = Other;
  for (int i=0;i<=Other;i++) times_spent[i] = 0.0;
  last_wall_time = -1;
  am_now_working_on(Other);

  num_chunks = thef.num_chunks;
  typedef fields_chunk *fields_chunk_ptr;
  chunks = new fields_chunk_ptr[num_chunks];
  for (int i=0;i<num_chunks;i++)
    chunks[i] = new fields_chunk(*thef.chunks[i]);
  FOR_FIELD_TYPES(ft) {
    for (int ip=0;ip<3;ip++) {
      comm_sizes[ft][ip] = new int[num_chunks*num_chunks];
      for (int i=0;i<num_chunks*num_chunks;i++) comm_sizes[ft][ip][i] = 0;
    }
    typedef double *double_ptr;
    comm_blocks[ft] = new double_ptr[num_chunks*num_chunks];
    for (int i=0;i<num_chunks*num_chunks;i++)
      comm_blocks[ft][i] = 0;
  }
  for (int b=0;b<2;b++) FOR_DIRECTIONS(d)
    boundaries[b][d] = thef.boundaries[b][d];
  chunk_connections_valid = false;
}

fields::~fields() {
  for (int i=0;i<num_chunks;i++) delete chunks[i];
  delete[] chunks;
  FOR_FIELD_TYPES(ft) {
    for (int i=0;i<num_chunks*num_chunks;i++)
      delete[] comm_blocks[ft][i];
    delete[] comm_blocks[ft];
    for (int ip=0;ip<3;ip++)
      delete[] comm_sizes[ft][ip];
  }
  delete sources;
  delete fluxes;
  delete bands;
  delete[] outdir;
  if (!quiet) print_times();
}

void fields::verbose(int v) {
  verbosity = v;
  for (int i=0;i<num_chunks;i++) chunks[i]->verbose(v);
}

void fields::use_real_fields() {
  LOOP_OVER_DIRECTIONS(v.dim, d)
    if (boundaries[High][d] == Periodic && k[d] != 0.0)
      abort("Can't use real fields with bloch boundary conditions!\n");
  is_real = 1;
  for (int i=0;i<num_chunks;i++) chunks[i]->use_real_fields();
  chunk_connections_valid = false;
}

bool fields::have_component(component c) {
  for (int i=0;i<num_chunks;i++)
    if (chunks[i]->f[c][0])
      return true;
  return false;
}

fields_chunk::~fields_chunk() {
  if (s->refcount-- <= 1) delete s; // delete if not shared
  is_real = 0; // So that we can make sure to delete everything...
  // for mu=1 non-PML regions, H==B to save space/time - don't delete twice!
  DOCMP2 FOR_H_AND_B(hc,bc) if (f[hc][cmp] == f[bc][cmp]) f[bc][cmp] = NULL;
  DOCMP2 FOR_COMPONENTS(c) {
    delete[] f[c][cmp];
    delete[] f_backup[c][cmp];
    delete[] f_prev[c][cmp];
    delete[] f_minus_p[c][cmp];
  }
  delete[] f_rderiv_int;
  FOR_FIELD_TYPES(ft)
    for (int ip=0;ip<3;ip++)
      for (int io=0;io<2;io++)
	delete[] connections[ft][ip][io];
  FOR_FIELD_TYPES(ft) delete[] connection_phases[ft];
  while (dft_chunks) {
    dft_chunk *nxt = dft_chunks->next_in_chunk;
    delete dft_chunks;
    dft_chunks = nxt;
  }
  delete b_sources;
  delete d_sources;
  delete pol;
  delete olpol;
  FOR_FIELD_TYPES(ft) delete[] zeroes[ft];
}

fields_chunk::fields_chunk(structure_chunk *the_s, const char *od,
			   double m, bool store_pol_energy) : v(the_s->v), gv(the_s->gv), m(m), store_pol_energy(store_pol_energy) {
  s = the_s; s->refcount++;
  rshift = 0;
  verbosity = 0;
  outdir = od;
  new_s = NULL;
  bands = NULL;
  is_real = 0;
  a = s->a;
  Courant = s->Courant;
  dt = s->dt;
  dft_chunks = NULL;
  pol = polarization::set_up_polarizations(s, is_real, store_pol_energy);
  olpol = polarization::set_up_polarizations(s, is_real, store_pol_energy);
  b_sources = d_sources = NULL;
  FOR_COMPONENTS(c) DOCMP2 {
    f[c][cmp] = NULL;
    f_backup[c][cmp] = NULL;
    f_prev[c][cmp] = NULL;
    f_minus_p[c][cmp] = NULL;
  }
  f_rderiv_int = NULL;
  FOR_FIELD_TYPES(ft) {
    for (int ip=0;ip<3;ip++)
      num_connections[ft][ip][Incoming] 
	= num_connections[ft][ip][Outgoing] = 0;
    connection_phases[ft] = 0;
    for (int ip=0;ip<3;ip++) for (int io=0;io<2;io++)
      connections[ft][ip][io] = NULL;
    zeroes[ft] = NULL;
    num_zeroes[ft] = 0;
  }
  figure_out_step_plan();
}

fields_chunk::fields_chunk(const fields_chunk &thef)
  : v(thef.v), gv(thef.gv) {
  s = new structure_chunk(thef.s);
  rshift = thef.rshift;
  verbosity = thef.verbosity;
  outdir = thef.outdir;
  m = thef.m;
  store_pol_energy = thef.store_pol_energy;
  new_s = NULL;
  bands = NULL;
  is_real = thef.is_real;
  a = thef.a;
  Courant = thef.Courant;
  dt = thef.dt;
  dft_chunks = NULL;
  pol = polarization::set_up_polarizations(s, is_real, store_pol_energy);
  olpol = polarization::set_up_polarizations(s, is_real, store_pol_energy);
  b_sources = d_sources = NULL;
  FOR_COMPONENTS(c) DOCMP2 {
    f[c][cmp] = NULL;
    f_backup[c][cmp] = NULL;
    f_prev[c][cmp] = NULL;
  }
  FOR_COMPONENTS(c) DOCMP
    if (!is_magnetic(c) && thef.f[c][cmp]) {
      f[c][cmp] = new double[v.ntot()];
      memcpy(f[c][cmp], thef.f[c][cmp], sizeof(double) * v.ntot());
    }
  FOR_MAGNETIC_COMPONENTS(c) DOCMP {
    if (thef.f[c][cmp] == thef.f[c-Hx+Bx][cmp])
      f[c][cmp] = f[c-Hx+Bx][cmp];
    else if (thef.f[c][cmp]) {
      f[c][cmp] = new double[v.ntot()];
      memcpy(f[c][cmp], thef.f[c][cmp], sizeof(double) * v.ntot());
    }
  }
  FOR_FIELD_TYPES(ft) {
    for (int ip=0;ip<3;ip++)
      num_connections[ft][ip][Incoming] 
	= num_connections[ft][ip][Outgoing] = 0;
    connection_phases[ft] = 0;
    for (int ip=0;ip<3;ip++) for (int io=0;io<2;io++)
      connections[ft][ip][io] = NULL;
    zeroes[ft] = NULL;
    num_zeroes[ft] = 0;
  }
  FOR_COMPONENTS(c) DOCMP2 
    if (thef.f_minus_p[c][cmp]) {
      f_minus_p[c][cmp] = new double[v.ntot()];
      memcpy(f_minus_p[c][cmp], thef.f_minus_p[c][cmp], 
	     sizeof(double) * v.ntot());
    }
  f_rderiv_int = NULL;
  figure_out_step_plan();
}

static inline bool cross_negative(direction a, direction b) {
  if (a >= R) a = direction(a - 3);
  if (b >= R) b = direction(b - 3);
  return ((3+b-a)%3) == 2;
}

static inline direction cross(direction a, direction b) {
  if (a == b) abort("bug - cross expects different directions");
  bool dcyl = a >= R || b >= R;
  if (a >= R) a = direction(a - 3);
  if (b >= R) b = direction(b - 3);
  direction c = direction((3+2*a-b)%3);
  if (dcyl && c < Z) return direction(c + 3);
  return c;
}

/* Call this whenever we modify the structure_chunk (fields_chunk::s) to
   implement copy-on-write semantics.  See also structure::changing_chunks. */
void fields_chunk::changing_structure() {
  if (s->refcount > 1) { // this chunk is shared, so make a copy
    s->refcount--;
    s = new structure_chunk(s);
  }
}

void fields_chunk::figure_out_step_plan() {
  FOR_COMPONENTS(cc)
    have_minus_deriv[cc] = have_plus_deriv[cc] = false;
  FOR_COMPONENTS(c1)
    if (f[c1][0]) {
      const direction dc1 = component_direction(c1);
      // Figure out which field components contribute.
      FOR_COMPONENTS(c2)
        if ((is_electric(c1) && is_magnetic(c2)) ||
            (is_D(c1) && is_magnetic(c2)) ||
            (is_magnetic(c1) && is_electric(c2)) ||
	    (is_B(c1) && is_electric(c2))) {
          const direction dc2 = component_direction(c2);
          if (dc1 != dc2 && v.has_field(c2) && v.has_field(c1) &&
              (has_direction(v.dim,cross(dc1,dc2)) ||
	       (v.dim == Dcyl && has_field_direction(v.dim,cross(dc1,dc2))))) {
            direction d_deriv = cross(dc1,dc2);
            if (cross_negative(dc2, dc1)) {
              minus_component[c1] = c2;
              have_minus_deriv[c1] = true;
              minus_deriv_direction[c1] = d_deriv;
            } else {
              plus_component[c1] = c2;
              have_plus_deriv[c1] = true;
              plus_deriv_direction[c1] = d_deriv;
            }
          }
        }
    }
  for (int i=0;i<3;i++) {
    num_each_direction[i] = v.yucky_num(i);
    stride_each_direction[i] = v.stride(v.yucky_direction(i));
  }
  FOR_DIRECTIONS(d) {
    num_any_direction[d] = 1;
    stride_any_direction[d] = 0;
    for (int i=0;i<3;i++)
      if (d == v.yucky_direction(i)) {
        num_any_direction[d] = v.yucky_num(i);
        stride_any_direction[d] = v.stride(v.yucky_direction(i));
      }
  }
}

static bool is_tm(component c) {
  switch (c) {
  case Hx: case Hy: case Bx: case By: case Ez: case Dz: return true;
  default: return false;
  }
  return false;
}

static bool is_like(ndim d, component c1, component c2) {
  if (d != D2) return true;
  return !(is_tm(c1) ^ is_tm(c2));
}

void fields_chunk::alloc_f(component the_c) {
  FOR_COMPONENTS(c)
    if (is_mine() && v.has_field(c) && is_like(v.dim, the_c, c)
	&& !is_magnetic(c))
      DOCMP {
        if (!f[c][cmp]) {
          f[c][cmp] = new double[v.ntot()];
          for (int i=0;i<v.ntot();i++) f[c][cmp][i] = 0.0;
	}
    }
  /* initially, we just set H == B ... later on, we lazily allocate
     H fields if needed (if mu != 1 or in PML) in update_h_from_b */
  FOR_H_AND_B(hc,bc) DOCMP 
    if (!f[hc][cmp] && f[bc][cmp]) f[hc][cmp] = f[bc][cmp];
  figure_out_step_plan();
}

void fields_chunk::remove_sources() {
  delete b_sources;
  delete d_sources;
  d_sources = b_sources = NULL;
}

void fields::remove_sources() {
  delete sources;
  sources = NULL;
  for (int i=0;i<num_chunks;i++) 
    chunks[i]->remove_sources();
}

void fields_chunk::remove_polarizabilities() {
  delete pol;
  delete olpol;
  pol = olpol = NULL;
  changing_structure();
  s->remove_polarizabilities();
}

void fields::remove_polarizabilities() {
  for (int i=0;i<num_chunks;i++) 
    chunks[i]->remove_polarizabilities();
}

void fields::remove_fluxes() {
  delete fluxes;
  fluxes = NULL;
}

void fields_chunk::zero_fields() {
  FOR_COMPONENTS(c) DOCMP {
    if (f[c][cmp]) for (int i=0;i<v.ntot();i++) f[c][cmp][i] = 0.0;
    if (f_backup[c][cmp]) for (int i=0;i<v.ntot();i++) f_backup[c][cmp][i] = 0.0;
    if (f_prev[c][cmp]) for (int i=0;i<v.ntot();i++) f_prev[c][cmp][i] = 0.0;
  }
  if (is_mine() && pol) pol->zero_fields();
  if (is_mine() && olpol) olpol->zero_fields();
}

void fields::zero_fields() {
  for (int i=0;i<num_chunks;i++)
    chunks[i]->zero_fields();
  synchronized_magnetic_fields = false;
}

void fields::reset() {
  remove_sources();
  remove_fluxes();
  zero_fields();
  t = 0;
}

void fields_chunk::use_real_fields() {
  is_real = 1;
  // for mu=1 non-PML regions, H==B to save space/time - don't delete twice!
  FOR_H_AND_B(hc,bc) if (f[hc][1] == f[bc][1]) f[bc][1] = NULL;
  FOR_COMPONENTS(c) if (f[c][1]) {
    delete[] f[c][1];
    f[c][1] = 0;
  }
  if (is_mine() && pol) pol->use_real_fields();
  if (is_mine() && olpol) olpol->use_real_fields();
}

int fields::phase_in_material(const structure *snew, double time) {
  if (snew->num_chunks != num_chunks)
    abort("Can only phase in similar sets of chunks: %d vs %d\n", 
	  snew->num_chunks, num_chunks);
  for (int i=0;i<num_chunks;i++)
    if (chunks[i]->is_mine())
      chunks[i]->phase_in_material(snew->chunks[i]);
  phasein_time = (int) (time/dt);
  return phasein_time;
}

void fields_chunk::phase_in_material(const structure_chunk *snew) {
  new_s = snew;
}

int fields::is_phasing() {
  return phasein_time > 0;
}

// This is used for phasing the *radial origin* of a cylindrical structure
void fields::set_rshift(double rshift) {
  if (v.dim != Dcyl) abort("set_rshift is only for cylindrical coords");
  if (gv.in_direction_min(R) <= 0 && gv.in_direction_max(R) >= 0)
    abort("set_rshift is invalid if volume contains r=0");
  for (int i = 0; i < num_chunks; ++i)
    chunks[i]->rshift = rshift;
}

bool fields::equal_layout(const fields &f) const {
  if (a != f.a || 
      num_chunks != f.num_chunks ||
      gv != f.gv ||
      S != f.S)
    return false;
  for (int d=0;d<5;d++)
    if (k[d] != f.k[d])
      return false;
  for (int i = 0; i < num_chunks; ++i)
    if (chunks[i]->a != f.chunks[i]->a ||
	chunks[i]->gv != f.chunks[i]->gv)
      return false;
  return true;
}

// total computational volume, including regions redundant by symmetry
geometric_volume fields::total_volume(void) const {
  geometric_volume gv0 = v.interior();
  geometric_volume gv = gv0;
  for (int n = 1; n < S.multiplicity(); ++n)
    gv = gv | S.transform(gv0, n);
  if (gv.dim == Dcyl && gv.in_direction_min(R) < 0)
    gv.set_direction_min(R, 0);
  return gv;
}

/* One-pixel periodic dimensions are used almost exclusively to
   emulate lower-dimensional computations, so if the user passes an
   empty size in that direction, they probably really intended to
   specify that whole dimension.  This function detects that case. */
bool fields::nosize_direction(direction d) const {
  return (v.has_boundary(Low, d) && v.has_boundary(High, d) &&
	  boundaries[Low][d] == Periodic && boundaries[High][d] == Periodic
	  && v.num_direction(d) == 1);
}

} // namespace meep
