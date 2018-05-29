/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------
   Contributing authors: Julien Tranchida (SNL)
                         Aidan Thompson (SNL)
   
   Please cite the related publication:
   Tranchida, J., Plimpton, S. J., Thibaudeau, P., & Thompson, A. P. (2018). 
   Massively parallel symplectic algorithm for coupled magnetic spin dynamics 
   and molecular dynamics. arXiv preprint arXiv:1801.10233.
------------------------------------------------------------------------- */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "atom.h"
#include "comm.h"
#include "error.h"
#include "fix.h"
#include "fix_nve_spin.h"
#include "force.h"
#include "pair_hybrid.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "neigh_request.h"
#include "math_const.h"
#include "memory.h"
#include "modify.h"
#include "pair_spin_exchange.h"
#include "update.h"

using namespace LAMMPS_NS;
using namespace MathConst;

/* ---------------------------------------------------------------------- */

PairSpinExchange::PairSpinExchange(LAMMPS *lmp) : PairSpin(lmp),
lockfixnvespin(NULL)
{
  single_enable = 0;
  no_virial_fdotr_compute = 1;
  lattice_flag = 0;
}

/* ---------------------------------------------------------------------- */

PairSpinExchange::~PairSpinExchange()
{
  if (allocated) {
    memory->destroy(setflag);
    memory->destroy(cut_spin_exchange);
    memory->destroy(J1_mag);
    memory->destroy(J1_mech);
    memory->destroy(J2);
    memory->destroy(J3);  
    memory->destroy(cutsq); // to be deleted
  }
}

/* ----------------------------------------------------------------------
   global settings
------------------------------------------------------------------------- */
  
void PairSpinExchange::settings(int narg, char **arg)
{
  if (narg < 1 || narg > 2)
    error->all(FLERR,"Incorrect number of args in pair_style pair/spin command");

  if (strcmp(update->unit_style,"metal") != 0)
    error->all(FLERR,"Spin simulations require metal unit style");
    
  cut_spin_exchange_global = force->numeric(FLERR,arg[0]);
    
  // reset cutoffs that have been explicitly set

  if (allocated) {
    int i,j;
    for (i = 1; i <= atom->ntypes; i++)
      for (j = i+1; j <= atom->ntypes; j++)
        if (setflag[i][j]) {
          cut_spin_exchange[i][j] = cut_spin_exchange_global;
        }
  }
   
}

/* ----------------------------------------------------------------------
   set coeffs for one or more type spin pairs (only one for now)
------------------------------------------------------------------------- */

void PairSpinExchange::coeff(int narg, char **arg)
{
  if (!allocated) allocate();
  
  // check if args correct

  if (strcmp(arg[2],"exchange") != 0)
    error->all(FLERR,"Incorrect args in pair_style command");
  if (narg != 7) 
    error->all(FLERR,"Incorrect args in pair_style command");

  int ilo,ihi,jlo,jhi;
  force->bounds(FLERR,arg[0],atom->ntypes,ilo,ihi);
  force->bounds(FLERR,arg[1],atom->ntypes,jlo,jhi);
 
  // get exchange arguments from input command

  const double rc = force->numeric(FLERR,arg[3]);
  const double j1 = force->numeric(FLERR,arg[4]);
  const double j2 = force->numeric(FLERR,arg[5]);  
  const double j3 = force->numeric(FLERR,arg[6]); 
  
  int count = 0;
  for (int i = ilo; i <= ihi; i++) {
    for (int j = MAX(jlo,i); j <= jhi; j++) {
      cut_spin_exchange[i][j] = rc;   
      J1_mag[i][j] = j1/hbar;
      J1_mech[i][j] = j1;
      J2[i][j] = j2;
      J3[i][j] = j3;
      setflag[i][j] = 1;
      count++;
    }
  }
  if (count == 0) 
    error->all(FLERR,"Incorrect args in pair_style command");  
}

/* ----------------------------------------------------------------------
   init specific to this pair style   
------------------------------------------------------------------------- */

void PairSpinExchange::init_style()
{
  if (!atom->sp_flag)
    error->all(FLERR,"Pair spin requires atom/spin style");

  // need a full neighbor list

  int irequest = neighbor->request(this,instance_me);
  neighbor->requests[irequest]->half = 0;
  neighbor->requests[irequest]->full = 1;

  // checking if nve/spin is a listed fix

  int ifix = 0;
  while (ifix < modify->nfix) {
    if (strcmp(modify->fix[ifix]->style,"nve/spin") == 0) break;
    ifix++;
  }
  if (ifix == modify->nfix)
    error->all(FLERR,"pair/spin style requires nve/spin");

  // get the lattice_flag from nve/spin

  for (int i = 0; i < modify->nfix; i++) {
    if (strcmp(modify->fix[i]->style,"nve/spin") == 0) {
      lockfixnvespin = (FixNVESpin *) modify->fix[i];
      lattice_flag = lockfixnvespin->lattice_flag;
    }
  }

}

/* ----------------------------------------------------------------------
   init for one type pair i,j and corresponding j,i
------------------------------------------------------------------------- */

double PairSpinExchange::init_one(int i, int j)
{
   
   if (setflag[i][j] == 0) error->all(FLERR,"All pair coeffs are not set");

  return cut_spin_exchange_global;
}

/* ----------------------------------------------------------------------
   extract the larger cutoff
------------------------------------------------------------------------- */

void *PairSpinExchange::extract(const char *str, int &dim)
{
  dim = 0;
  if (strcmp(str,"cut") == 0) return (void *) &cut_spin_exchange_global;
  return NULL;
}

/* ---------------------------------------------------------------------- */

void PairSpinExchange::compute(int eflag, int vflag)
{
  int i,j,ii,jj,inum,jnum,itype,jtype;  
  double evdwl, ecoul;
  double xi[3], rij[3], eij[3];
  double spi[3], spj[3];
  double fi[3], fmi[3];
  double local_cut2;
  double rsq, inorm;
  int *ilist,*jlist,*numneigh,**firstneigh;  

  evdwl = ecoul = 0.0;
  if (eflag || vflag) ev_setup(eflag,vflag);
  else evflag = vflag_fdotr = 0;

  double **x = atom->x;
  double **f = atom->f;
  double **fm = atom->fm;
  double **sp = atom->sp;	
  int *type = atom->type;  
  int nlocal = atom->nlocal;  
  int newton_pair = force->newton_pair;
  
  inum = list->inum;
  ilist = list->ilist;
  numneigh = list->numneigh;
  firstneigh = list->firstneigh;

  // computation of the exchange interaction
  // loop over atoms and their neighbors

  for (ii = 0; ii < inum; ii++) {
    i = ilist[ii];
    itype = type[i];
    
    jlist = firstneigh[i];
    jnum = numneigh[i]; 
    xi[0] = x[i][0];
    xi[1] = x[i][1];
    xi[2] = x[i][2];
    spi[0] = sp[i][0]; 
    spi[1] = sp[i][1]; 
    spi[2] = sp[i][2];
  
    // loop on neighbors

    for (jj = 0; jj < jnum; jj++) {
      j = jlist[jj];
      j &= NEIGHMASK;
      jtype = type[j];

      spj[0] = sp[j][0]; 
      spj[1] = sp[j][1]; 
      spj[2] = sp[j][2]; 

      evdwl = 0.0;

      fi[0] = fi[1] = fi[2] = 0.0;
      fmi[0] = fmi[1] = fmi[2] = 0.0;
     
      rij[0] = x[j][0] - xi[0];
      rij[1] = x[j][1] - xi[1];
      rij[2] = x[j][2] - xi[2];
      rsq = rij[0]*rij[0] + rij[1]*rij[1] + rij[2]*rij[2];
      inorm = 1.0/sqrt(rsq);
      eij[0] = inorm*rij[0]; 
      eij[1] = inorm*rij[1]; 
      eij[2] = inorm*rij[2]; 

      local_cut2 = cut_spin_exchange[itype][jtype]*cut_spin_exchange[itype][jtype];

      // compute exchange interaction
      
      if (rsq <= local_cut2) {
	compute_exchange(i,j,rsq,fmi,spj);
        if (lattice_flag) {
	  compute_exchange_mech(i,j,rsq,eij,fi,spi,spj);
	}
      }

      f[i][0] += fi[0];	 
      f[i][1] += fi[1];	  	  
      f[i][2] += fi[2];
      fm[i][0] += fmi[0];	 
      fm[i][1] += fmi[1];	  	  
      fm[i][2] += fmi[2];

      if (newton_pair || j < nlocal) {
	f[j][0] -= fi[0];	 
        f[j][1] -= fi[1];	  	  
        f[j][2] -= fi[2];
      }

      if (eflag) {
	evdwl -= (spi[0]*fmi[0] + spi[1]*fmi[1] + spi[2]*fmi[2]);
	evdwl *= hbar;
      } else evdwl = 0.0;

      if (evflag) ev_tally_xyz(i,j,nlocal,newton_pair,
	  evdwl,ecoul,fi[0],fi[1],fi[2],rij[0],rij[1],rij[2]);
    }
  }  

  if (vflag_fdotr) virial_fdotr_compute();
  
}

/* ----------------------------------------------------------------------
   update the pair interactions fmi acting on the spin ii
------------------------------------------------------------------------- */

void PairSpinExchange::compute_single_pair(int ii, double fmi[3]) 
{

  int *type = atom->type;
  double **x = atom->x;
  double **sp = atom->sp;
  double local_cut2;

  double xi[3], rij[3];
  double spj[3];

  int i,j,jnum,itype,jtype;
  int *ilist,*jlist,*numneigh,**firstneigh;

  double rsq;

  ilist = list->ilist;
  numneigh = list->numneigh;
  firstneigh = list->firstneigh;

  i = ilist[ii];
  itype = type[i];

  xi[0] = x[i][0];
  xi[1] = x[i][1];
  xi[2] = x[i][2];
 
  jlist = firstneigh[i];
  jnum = numneigh[i];

  for (int jj = 0; jj < jnum; jj++) {

    j = jlist[jj];
    j &= NEIGHMASK;
    jtype = type[j];
    local_cut2 = cut_spin_exchange[itype][jtype]*cut_spin_exchange[itype][jtype];

    spj[0] = sp[j][0];
    spj[1] = sp[j][1];
    spj[2] = sp[j][2];

    rij[0] = x[j][0] - xi[0];
    rij[1] = x[j][1] - xi[1];
    rij[2] = x[j][2] - xi[2];
    rsq = rij[0]*rij[0] + rij[1]*rij[1] + rij[2]*rij[2];

    if (rsq <= local_cut2) {
      compute_exchange(i,j,rsq,fmi,spj);
    }

  }

}

/* ----------------------------------------------------------------------
   compute exchange interaction between spins i and j
------------------------------------------------------------------------- */

void PairSpinExchange::compute_exchange(int i, int j, double rsq, double fmi[3], double spj[3])
{
  int *type = atom->type;  
  int itype, jtype;
  double Jex, ra;
  itype = type[i];
  jtype = type[j];

  ra = rsq/J3[itype][jtype]/J3[itype][jtype]; 
  Jex = 4.0*J1_mag[itype][jtype]*ra;
  Jex *= (1.0-J2[itype][jtype]*ra);
  Jex *= exp(-ra);

  fmi[0] += Jex*spj[0];
  fmi[1] += Jex*spj[1];
  fmi[2] += Jex*spj[2];
}

/* ----------------------------------------------------------------------
   compute the mechanical force due to the exchange interaction between atom i and atom j
------------------------------------------------------------------------- */

void PairSpinExchange::compute_exchange_mech(int i, int j, double rsq, double rij[3], double fi[3],  double spi[3], double spj[3])
{
  int *type = atom->type;  
  int itype, jtype;
  double Jex, Jex_mech, ra, rr, iJ3;
  itype = type[i];
  jtype = type[j];
    
  Jex = J1_mech[itype][jtype];
  iJ3 = 1.0/(J3[itype][jtype]*J3[itype][jtype]);

  ra = rsq*iJ3; 
  rr = sqrt(rsq)*iJ3;

  Jex_mech = 1.0-ra-J2[itype][jtype]*ra*(2.0-ra);
  Jex_mech *= 8.0*Jex*rr*exp(-ra);
  Jex_mech *= (spi[0]*spj[0]+spi[1]*spj[1]+spi[2]*spj[2]); 

  fi[0] -= Jex_mech*rij[0];
  fi[1] -= Jex_mech*rij[1];
  fi[2] -= Jex_mech*rij[2];
}

/* ----------------------------------------------------------------------
   allocate all arrays
------------------------------------------------------------------------- */

void PairSpinExchange::allocate()
{
  allocated = 1;
  int n = atom->ntypes;

  memory->create(setflag,n+1,n+1,"pair:setflag");
  for (int i = 1; i <= n; i++)
    for (int j = i; j <= n; j++)
      setflag[i][j] = 0;
      
  memory->create(cut_spin_exchange,n+1,n+1,"pair/spin/exchange:cut_spin_exchange");
  memory->create(J1_mag,n+1,n+1,"pair/spin/exchange:J1_mag");
  memory->create(J1_mech,n+1,n+1,"pair/spin/exchange:J1_mech");
  memory->create(J2,n+1,n+1,"pair/spin/exchange:J2");  
  memory->create(J3,n+1,n+1,"pair/spin/exchange:J3");
  memory->create(cutsq,n+1,n+1,"pair:cutsq"); 
}



/* ----------------------------------------------------------------------
   proc 0 writes to restart file
------------------------------------------------------------------------- */

void PairSpinExchange::write_restart(FILE *fp)
{
  write_restart_settings(fp);
  
  int i,j;
  for (i = 1; i <= atom->ntypes; i++) {
    for (j = i; j <= atom->ntypes; j++) {
      fwrite(&setflag[i][j],sizeof(int),1,fp);
      if (setflag[i][j]) {
        fwrite(&J1_mag[i][j],sizeof(double),1,fp);
        fwrite(&J1_mech[i][j],sizeof(double),1,fp);
        fwrite(&J2[i][j],sizeof(double),1,fp);
        fwrite(&J3[i][j],sizeof(double),1,fp);
        fwrite(&cut_spin_exchange[i][j],sizeof(double),1,fp);
      }
    }
  }
}

/* ----------------------------------------------------------------------
   proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void PairSpinExchange::read_restart(FILE *fp)
{
  read_restart_settings(fp);

  allocate();

  int i,j;
  int me = comm->me;
  for (i = 1; i <= atom->ntypes; i++) {
    for (j = i; j <= atom->ntypes; j++) {
      if (me == 0) fread(&setflag[i][j],sizeof(int),1,fp);
      MPI_Bcast(&setflag[i][j],1,MPI_INT,0,world);
      if (setflag[i][j]) {
        if (me == 0) {
          fread(&J1_mag[i][j],sizeof(double),1,fp);
          fread(&J1_mech[i][j],sizeof(double),1,fp);
          fread(&J2[i][j],sizeof(double),1,fp);
          fread(&J2[i][j],sizeof(double),1,fp);
          fread(&cut_spin_exchange[i][j],sizeof(double),1,fp);
        }
        MPI_Bcast(&J1_mag[i][j],1,MPI_DOUBLE,0,world);
        MPI_Bcast(&J1_mech[i][j],1,MPI_DOUBLE,0,world);
        MPI_Bcast(&J2[i][j],1,MPI_DOUBLE,0,world);
        MPI_Bcast(&J3[i][j],1,MPI_DOUBLE,0,world);
        MPI_Bcast(&cut_spin_exchange[i][j],1,MPI_DOUBLE,0,world);
      }
    }
  }
}

 
/* ----------------------------------------------------------------------
   proc 0 writes to restart file
------------------------------------------------------------------------- */

void PairSpinExchange::write_restart_settings(FILE *fp)
{
  fwrite(&cut_spin_exchange_global,sizeof(double),1,fp);
  fwrite(&offset_flag,sizeof(int),1,fp);
  fwrite(&mix_flag,sizeof(int),1,fp);
}

/* ----------------------------------------------------------------------
   proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void PairSpinExchange::read_restart_settings(FILE *fp)
{
  if (comm->me == 0) {
    fread(&cut_spin_exchange_global,sizeof(double),1,fp);
    fread(&offset_flag,sizeof(int),1,fp);
    fread(&mix_flag,sizeof(int),1,fp);
  }
  MPI_Bcast(&cut_spin_exchange_global,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&offset_flag,1,MPI_INT,0,world);
  MPI_Bcast(&mix_flag,1,MPI_INT,0,world); 
}

