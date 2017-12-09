/* 
   Implementation of the clm class (Levenberg-Marquardt least-squares-fit)
   author: J. de la Cruz Rodriguez (Stockholm University 2016)

   Documentation included in the header file clm.h.

   Dependencies: Eigen3 for SVD decomp.

   Modifications: 
           2016-04-08, JdlCR: Added Kahan summation to perform the matrix multiplication,
	                      it improves convergence significantly!
	   
	   2016-06-26, JdlCR: Changed to Eigen3 routines for SVD, 
	                      the implementation is much cleaner and faster than LaPack.

	   2016-12-13, JdlCR: added the possibility to add l-2 regulatization. The user 
	                      must provide a function that computes the regularization 
			      term for all parameter.

	   2017-01-11, JdlCR: added Kahan sumation but operating on each stokes parameter 
	                      first and then adding each contribution.

	   2017-03-25, JdlCR: allow to split the SVD decomposition per type of parameter
	                      Basically, the large impact of temperature can systematically
			      force the algorithm to filter the corrections to other parameters.
			      We proceed as described by Ruiz-Cobo & del Toro-Iniesta (1992).
			      Basically the singular values are projected into the sub-space
			      of each parameter and processed there.

	   2017-04-21, JdlCR: Implemented simple bracketing of lambda for best convergence.
	                      Also fixed regularization, according to Nik's comments.


	   2017-10-25, JdlCR: Added the possibility to work with perturbations to the model, 
	                      so the parameters are set to zero in every iteration. The 
			      corresponding fx must account for this. So the parameters are
			      added to the current version of, e.g., the model atmosphere.

	   2017-11-20, JdlCR: Fixed regularization, there was a small error on the right-hand 
	                      side term of the linear system that is solved to get the corrections
			      to the model. 
*/

#include <algorithm>
#include <vector>
#include <string>
#include <cstdio>
#include <iostream>
#include <cstring>
#include <cmath>
#include <eigen3/Eigen/Dense>
#include <eigen3/Eigen/SVD>
#include "clm.h"
#include "interpol.h"

using namespace std;
using namespace Eigen;

/* -------------------------------------------------------------------------------- */

/* --- Internal routines --- */


double **mat2d(int nx1, int nx2){
  double **p;
  p = new double* [nx1];
  p[0] = new double [nx1 * nx2]();
  for(int x1=1;x1<nx1;++x1) p[x1] = p[x1-1] + nx2;
  return p;
}

void del_mat(double **p){
  delete[] (p[0]);
  delete[] (p);
}

double **var2dim(double *data, int nx1, int nx2){
  double **p;
  p=new double* [nx1];
  p[0] = data;
  for(int x1=1;x1<nx1;++x1) p[x1]=p[x1-1] + nx2;
  return p;
}

/* --- Kahan summation for the elements of a vector--- */

inline double sumarr(double *arr, size_t n){

  double sum = 0.0L, c = 0.0L;
  
  for(size_t kk = 0; kk<n; kk++){
    double y = arr[kk] - c;
    double t = sum + y;
    c = (t - sum) - y;
    sum = t;
  }
    
  return (double)sum;
}

/* --- Kahan summation for the square of the elements of a vector --- */

inline double sumarr2(double *arr, int n){

  double sum = 0.0, c = 0.0;
  
  for(int kk = 0; kk<n; kk++){
    double y = arr[kk]*arr[kk] - c;
    double t = sum + y;
    c = (t - sum) - y;
    sum = t;
  }
    
  return (double)sum;
}

inline double sumarr2_4(double *arr, int n){

  int n4 = n/4;
  bool stokes = ((n4*4) == n) ? true : false;

  if(stokes){
    long double ichi[4] = {0.0L, 0.0L, 0.0L, 0.0L};
    for(int ss=0;ss<4;ss++){
      long double sum = 0.0, c = 0.0;
      for(int kk=0;kk<n4;kk++){
	long double y = arr[kk*4+ss]*arr[kk*4+ss] - c;
	long double t = sum + y;
	c = (t - sum) - y;
	sum = t;
      }
      ichi[ss] = sum;
    }
    return (double)(ichi[0] + ichi[1] + ichi[2] + ichi[3]);
  }else{
    long double sum = 0.0, c = 0.0;
    
    for(int kk = 0; kk<n; kk++){
      long double y = arr[kk]*arr[kk] - c;
      long double t = sum + y;
      c = (t - sum) - y;
      sum = t;
    }
    
    return (double)sum;
  }
}

inline double sumarr_4(double *arr, int n){

  int n4 = n/4;
  bool stokes = ((n4*4) == n) ? true : false;
  
  if(stokes){
    long double ichi[4] = {0.0L, 0.0L, 0.0L, 0.0L};
    for(int ss=0;ss<4;ss++){
      long double sum = 0.0L, c = 0.0L;
      for(int kk=0;kk<n4;kk++){
	long double y = arr[kk*4+ss] - c;
	long double t = sum + y;
	c = (t - sum) - y;
	sum = t;
      }
      ichi[ss] = sum;

    }
    return (double)(ichi[0] + ichi[1] + ichi[2] + ichi[3]);
  }else{
    long double sum = 0.0, c = 0.0;
    
    for(int kk = 0; kk<n; kk++){
      long double y = arr[kk] - c;
      long double t = sum + y;
      c = (t - sum) - y;
      sum = t;
    }
    
    return (double)sum;
  }
}

/* -------------------------------------------------------------------------------- */

void reg_t::zero(){
  if(to_reg && (npar > 0)){
    memset(reg, 0, nreg*sizeof(double));
    memset(dreg[0], 0, nreg*npar*sizeof(double));
  }
}

/* -------------------------------------------------------------------------------- */

void reg_t::set(int npar_in, int nreg_in, double scl_in)
{
  
  to_reg = false;
  npar = 0;
  nreg = 0;
  reg = NULL;
  dreg = NULL;
  scl = 0.0;
  
  if(npar_in >0){
    to_reg = true;
    npar = npar_in;
    nreg = nreg_in;
    scl = scl_in;
    
    reg = new double [nreg];
    dreg = mat2d(nreg, npar);

    zero();
  }
  
}

/* -------------------------------------------------------------------------------- */

double reg_t::getReg()
{
  if((nreg > 0) && (to_reg)){
    return sumarr2(reg, nreg);
  }else return 0.0;
}

/* -------------------------------------------------------------------------------- */

void reg_t::copyReg(double *reg_in)
{
  memcpy(reg, reg_in, nreg*sizeof(double));
}

/* -------------------------------------------------------------------------------- */

reg_t::reg_t(int npar_in, int nreg_in, double scl_in)
{
  set(npar_in, nreg_in, scl_in); 
}

/* -------------------------------------------------------------------------------- */

reg_t::reg_t(const reg_t &in)
{

  set(in.npar, in. nreg, in.scl);

  if(npar > 0){
    memcpy(reg, in.reg, nreg*sizeof(double));
    memcpy(dreg[0], in.dreg[0], nreg*npar*sizeof(double));
  }
  
}

/* -------------------------------------------------------------------------------- */

reg_t &reg_t::operator=(const reg_t &in)
{
  set(in.npar, in.nreg, in.scl);
  
  if(npar > 0){
    memcpy(reg, in.reg, nreg*sizeof(double));
    memcpy(dreg[0], in.dreg[0], nreg*npar*sizeof(double));
  }
  
  return *this; 
}

/* -------------------------------------------------------------------------------- */

void reg_t::del(){
  
  if(reg != NULL) delete [] reg;
  if(dreg != NULL) del_mat(dreg);
  to_reg = false;
  npar = 0;
  nreg = 0;
}


/* -------------------------------------------------------------------------------- */

reg_t::~reg_t(){
  del();
}

/* -------------------------------------------------------------------------------- */

clm::clm(int ind, int inpar){

  /* --- copy dimensions --- */
  
  nd = ind;
  npar = inpar;
  lwork = -1; // To be used on the first call by LaPack's SVD (not used anymore)
  tmp.resize(nd);
  ptype.resize(npar,0); // To define groups of parameters that are filtered together
  nvar = 1;
  
  /* --- resize fit control arrays --- */

  fcnt.resize(npar);
  diag.resize(npar);
  memset(&fcnt[0], 0, npar*sizeof(clmf));
  first = true;
  
  /* --- 
     Default values of the fit control, 
     can be changed externally 
     --- */
  
  xtol = 1.e-5;       // Controls the minimum relative change to Chi2
  chi2_thres = 1.0;   // Exit inversion if Chi2 is lower than this
  svd_thres = 1.e-13; // Cut-off "relative" thres. for small singular values
  lmax = 1.0e4;       // Maximum lambda value
  lmin = 1.0e-4;      // Minimum lambda value
  lfac = 10.0;         // Change lambda by this amount
  ilambda = 1.0;      // Initial damping parameter for the Hessian giag.
  maxreject = 7;      // Max failed evaluations of lambda.
  error = false;
  regularize = false; // Compute regularization terms in fx ?
  regul_scal = 1.0;   // scale factor for regularization terms
  regul_scal_in = 0.0;   // scale factor for regularization terms input
  proc = 0;           // print-out processor number
  reset_par = false;  // If true, use perturbation approach
}

/* -------------------------------------------------------------------------------- */

clm::~clm()
{
  fcnt.clear();
  diag.clear();
}

/* -------------------------------------------------------------------------------- */

void clm::checkParameters(double *x)
{

  /* --- check that parameter values are within limits --- */

  for(int ii = 0; ii<npar; ii++){

    double range = fcnt[ii].limit[1] - fcnt[ii].limit[0];  
    double par = x[ii];
    
    if(fcnt[ii].cyclic){
      if(par < fcnt[ii].limit[0]) par += range;
      if(par > fcnt[ii].limit[1]) par -= range;
    }

    if(fcnt[ii].bouncing){
      if(par < fcnt[ii].limit[0]) par = -par;
      if(par > fcnt[ii].limit[1]) par = fcnt[ii].limit[1] - (par-fcnt[ii].limit[1]);
    }

    if(par <  fcnt[ii].limit[0]) par =  fcnt[ii].limit[0];
    if(par >  fcnt[ii].limit[1]) par =  fcnt[ii].limit[1];

    x[ii] = par;
  }
  
}

/* -------------------------------------------------------------------------------- */

double clm::checkLambda(double lamb)
{
  return max(min(lamb, lmax), lmin);
}

/* -------------------------------------------------------------------------------- */

void clm::checkMaxChange(double *dx, double *x)
{

  /* --- check that corrections to the parameter 
     are within reasonable limits --- */
  
  double maxcha = 0.0;
  for(int ii = 0; ii<npar; ii++){
    if(!fcnt[ii].capped) continue;
    
    if(dx[ii] >= 0) maxcha = fcnt[ii].maxchange[1];
    else maxcha = fcnt[ii].maxchange[0];

    if(fcnt[ii].relchange) maxcha *= fabs(x[ii]);
    

    if(fabs(dx[ii]) > fabs(maxcha)) dx[ii] *= fabs(maxcha / dx[ii]);

  }

}

/* -------------------------------------------------------------------------------- */

void clm::normalizeParameters(double *x)
{
  /* --- Normalize parameters with the given norm --- */

  for(int ii = 0; ii<npar; ii++) x[ii] /= fcnt[ii].scl; 
}

/* -------------------------------------------------------------------------------- */

void clm::scaleParameters(double *x)
{
  /* --- Scale parameters with the given norm --- */

  for(int ii = 0; ii<npar; ii++) x[ii] *= fcnt[ii].scl; 
}

/* -------------------------------------------------------------------------------- */

void clm::zero(double *res, double **rf)
{
  if(res) memset(&res[0], 0, nd*sizeof(double));
  if(rf)  memset(&rf[0][0], 0, nd*npar*sizeof(double));
}
  
/* -------------------------------------------------------------------------------- */

double clm::compute_chi2(double *res, double penalty)
{
  return (double)sumarr2_4(res,nd)/double(nd) + penalty;				 
}

/* -------------------------------------------------------------------------------- */

double clm::getChi2Pars(double *res, double **rf, double lambda,
			double *x, double *xnew, void *mydat, clm_func fx, reg_t &dregul)
{

  double newchi2 = 1.e13;
  
  /* --- Get new estimate of the model for the current lambda value --- */
  
  memset(xnew, 0, sizeof(double)*npar);
  compute_trial3(res, rf, lambda, x, xnew, dregul);


  /* --- Evaluate the new model, no response function is needed --- */

  double *new_res = new double [nd]();
  reg_t new_dregul = dregul;
  
  int status = fx(npar, nd, xnew, new_res, NULL, mydat, new_dregul, false);

  if(status){
    if(verb)
      fprintf(stderr, "clm::fitdata: [p:%4d] ERROR in the evaluation of FX, aborting inversion\n", proc);
    error = true;
  }else newchi2 = compute_chi2(new_res, new_dregul.getReg());

  delete [] new_res;
    
    
  /* --- copy individual penalties and return chi2 --- */
  
  if(dregul.to_reg)
    dregul.copyReg(new_dregul.reg);
  
  return (double)newchi2;
}

/* -------------------------------------------------------------------------------- */

double clm::getChi2ParsLineSearch(double *res, double **rf, double &lambda,
				  double *x, double *xnew, void *mydat, clm_func fx, reg_t &dregul_in, double rchi2)
{

  /* ---
     JdlCR:
     Remember! we need to keep the original "reg" terms before calling getChi2Pars, 
     because they are used by the LM algorithm to estimate the new step. But 
     we need to new "reg" terms to compute the new Chi2 after taking the actual step. 

     Solution:
     Define best_chi2, where we store the new reg term of the best chi2, and make sure
     that we copy again the original reg terms before calling getChi2Pars into dregul.

     --- */
  
  
  /* --- Braket lambda/chi2 --- */

  vector<vector<double>> ixnew;
  vector<double> ilamb, ichi, tmp(npar, 0.0);
  reg_t dregul = dregul_in, best_dregul = dregul_in;
  ilamb.push_back(lambda);
  

  
  /* --- Init chi2 --- */
  
  ichi.push_back(getChi2Pars(res, rf, ilamb[0], x, xnew, mydat, fx, dregul));
  memcpy(&tmp[0],xnew, npar*sizeof(double)), ixnew.push_back(tmp);
  

  /* --- Can we take larger steps if Chi2 was ok? --- */
  
  if(ichi[0] < rchi2){ // Things improved compared to reference chi2, try to go further!
    int kk = 0, iter = 0;
    best_dregul.copyReg(dregul.reg);
    
    while((kk<1) || ((ichi[kk] < ichi[kk-1]) && (iter++ < 5) && (lambda > 1.e-5))){
      double ilfac = lfac;//(ilamb[kk] > 0.1)? lfac : sqrt(lfac);
      ilamb.push_back(ilamb[kk] / ilfac);
      dregul = dregul_in;
      ichi.push_back(getChi2Pars(res, rf, ilamb[kk+1], x, xnew, mydat, fx, dregul));
      if(ichi[kk+1] < ichi[kk]) best_dregul.copyReg(dregul.reg);
      
      memcpy(&tmp[0],xnew, npar*sizeof(double)), ixnew.push_back(tmp);
      kk += 1;
    }

    
    /* --- Check the index of the smallest chi2 --- */
    
    int idx = 0, nn=(int)ichi.size();
    if(nn > 1)
      for(kk = 1; kk<nn; kk++){
	if(ichi[kk]<ichi[kk-1]) idx = kk;
      }

    
    /* --- We could not braket the solution, too many iteration or too small lambda, 
       just return the best we have --- */
    
    if(idx == (nn-1)){
      memcpy(xnew, &ixnew[idx][0], npar*sizeof(double));
      lambda = ilamb[idx];

      dregul_in.copyReg(best_dregul.reg);
      
      return ichi[idx];
    }


    
    /* --- if the best chi2 is in the first element, try to bracket it by appending 
       chi2 with larger lambda values --- */
    
    kk = 0;
    while(idx == 0 && kk++ <= 5){
      double ilfac = lfac;//(ilamb[0]  >= 0.1)? lfac : sqrt(lfac);
      ilamb.insert(ilamb.begin(), ilamb[0] * ilfac);
      dregul = dregul_in;
      ichi.insert(ichi.begin(), getChi2Pars(res, rf, ilamb[0], x, xnew, mydat, fx, dregul));
      if(ichi[0] < ichi[1]) best_dregul.copyReg(dregul.reg);

      
      
      memcpy(&tmp[0],xnew, npar*sizeof(double)), ixnew.insert(ixnew.begin(), tmp);
      
      /* --- Check the index of the smallest chi2 after adding a new element --- */
      
      idx = 0, nn=(int)ichi.size();
      if(nn != 1)
	for(int ii = 1; ii<nn; ii++){
	  if(ichi[ii]<ichi[ii-1]) idx = ii;
	}
    } // while
    
    
    /* --- If we have braketed lambda with 3 values, use parabolic interpolation, 
       otherwise just take the best value we have (indexed by idx at this point) 
       If we improve he solution, just return that value. If not, try to re-bracket 
       the value of lambda with the new estimate. Only try twice, otherwise we are loosing
       time.
       --- */
    
    if(idx != 0){
      for(int ii=0; ii<3; ii++){
	vector<double> cc = parab_fit<double>(log(ilamb[idx-1]), log(ilamb[idx]),
					      log(ilamb[idx+1]), (ichi[idx-1]), (ichi[idx]),
					      (ichi[idx+1]));
	
	double glamb = exp(-0.5 * cc[1]/cc[2]);
	dregul = dregul_in;
	double gchi  = getChi2Pars(res, rf, glamb, x, &tmp[0], mydat, fx, dregul);
	
	if(gchi < ichi[idx]){
	  ichi.push_back(gchi);
	  best_dregul.copyReg(dregul.reg);
	  ilamb.push_back(glamb);
	  ixnew.push_back(tmp);
	  idx = (int)ichi.size()-1;
	  break;
	}else{ // Braket one more time with this new value
	  int dir = -1;
	  if(glamb < ilamb[idx]) dir = 1;
	  ichi[idx+dir] = gchi;
	  ilamb[idx+dir] = glamb;
	  ixnew[idx+dir] = tmp;
	}
      }
    }
    
    
    /* --- Copy best solution to output array --- */
    
    memcpy(xnew, &ixnew[idx][0], npar*sizeof(double));
    lambda = ilamb[idx];
    dregul_in.copyReg(best_dregul.reg);
    
    return ichi[idx];
    
  }else{ // Chi2 is worse than reference chi2

    /* --- Get out and increase lambda outside --- */
    
    return ichi[0];
  }

}

/* -------------------------------------------------------------------------------- */

void clm::scaleRF(double **rf)
{
  for(int pp = 0; pp<npar;pp++){
    double scl = fcnt[pp].scl;
    for(int ww=0; ww<nd; ww++)
      rf[pp][ww] *= scl;
  }

}

/* -------------------------------------------------------------------------------- */

double clm::fitdata(clm_func fx, double *x, void *mydat, int maxiter, reg_t &dregul)
{

  /* --- Init variables --- */

  regul_scal = regul_scal_in;

  if(reset_par) memset(x, 0, npar*sizeof(double));
  
  getParTypes();
  double chi2 = 1.e13, ochi2 = 1.e13, bestchi2 = 1.e13, olambda = 0.0, t0 = 0, t1 = 0;
  double orchi2=1.e13, rchi2=1.e13, reg = 0.0;
  int iter = 0, nretry = 0;
  bool exitme = false, toolittle = false, dcreased = false;
  string rej = "";
  memset(&diag[0],0,npar*sizeof(double));
  error = false;

  //reg_t dregul;
  //if(regularize) dregul.set(npar, regul_scal); // To store derivatives of regularization terms
  
  
  /* --- Init array for residues and response function --- */
  
  double **rf = mat2d(npar,nd);
  double *res = new double [nd]();
  double *bestpars = new double [nd]();
  double *xnew = new double [nd]();

  
  /* --- check parameters --- */
  if(!reset_par)
    checkParameters(x);


  /* --- adjust lambda parameter --- */

  double lambda = checkLambda(ilambda);
  t0 = getTime();

  /* --- Evaluate residues and RF, init chi2 --- */

  int status = fx(npar, nd, x, res, rf, mydat, dregul, false);
  if(status){
    if(verb)
      fprintf(stderr, "clm::fitdata: [p:%4d] ERROR in the evaluation of FX, aborting inversion\n", proc);
    ochi2 = 1.e13;
    error = true;
    iter = maxiter+1;
  }else{
    scaleRF(rf);
    memcpy(&bestpars[0], &x[0], npar*sizeof(double));
    //
    reg =  dregul.getReg();
    ochi2 = compute_chi2(res, reg);
    bestchi2 = ochi2;
    orchi2 = ochi2 - reg;
    

    if(verb)
      fprintf(stdout, "[p:%4d, Init] chi2=%f (%f), lambda=%e\n", proc,
	      orchi2, reg , lambda);
  }

  /* --- Main iterations --- */

  while(iter <= maxiter){

    
    /* --- Get new estimate of the model and Chi2 for a given lambda parameter.
       The new parameters are already checked for limits and too large corrections 
       inside getChi2Pars 
       --- */
    if(error) break;
    chi2 = getChi2ParsLineSearch(res, rf, lambda, x, xnew, mydat, fx, dregul, bestchi2);
    reg = dregul.getReg();
    rchi2 = chi2 - reg;
	
    if(chi2 != chi2) error = true;
    if(error) break;
    
   

    /* --- Check if we have improved or not --- */

    double reldchi = 2.0 * (chi2 - bestchi2) / (chi2 + bestchi2);
    olambda = lambda;
    if(lambda >= 1.e4) lambda = 1.e2;
    
    if((!dcreased && (chi2 < bestchi2)) || (dcreased && (rchi2 < orchi2))){
      
      /* --- Prep lambda for next iter. If we have been increasing lambda,
	 do not decrease it again until next iteration 
	 --- */
      
      //double ilfac = (lambda > 0.1)? lfac : sqrt(lfac);
      //if(nretry == 0 || lambda >= 1.0) lambda = checkLambda(lambda / ilfac);
      
      lambda *= lfac; // Helps to braket the solution
      if(lambda <= 1.e-3) lambda = 1.e-1;
      if(lambda >= 1.e5) lambda = 1e4;
      
      /* --- is the improvements below our threshold? --- */

      if(fabs(reldchi) < xtol){
	if(toolittle) exitme = true;
	else toolittle = true;
      }else toolittle = false;
   
      
      /* --- Store new best guessed model --- */
      
      bestchi2 = chi2;
      orchi2  = rchi2; //chi2 -  dregul.getReg();
      //
      memcpy(&bestpars[0], xnew, npar*sizeof(double));
      memcpy(&x[0],        xnew, npar*sizeof(double));
      nretry = 0;

      dcreased = false;
      rej = " ";

      
      /* --- Use perturbations approach --- */
      
      if(reset_par){
	status = fx(npar, nd, x, res, rf, mydat, dregul, true);
	memset(x, 0, npar*sizeof(double));
	memset(xnew, 0, npar*sizeof(double));

      }
      
    }else{
      
      /* --- Prep lambda for next trial --- */
      
      double ilfac = lfac*lfac;//(lambda >= 0.1)? lfac : sqrt(lfac);
      lambda = checkLambda(lambda * ilfac);
      nretry++;
      rej = " *";
      
      if(nretry < maxreject){
	if((nretry == (maxreject-3)) && 0){
	  regul_scal *= 0.75;
	  dcreased = true;
	}else dcreased = false;

	if(verb)
	  fprintf(stderr,"[p:%4d,i:%4d]  ->  chi2=%f (%f), increasing lambda [%e -> %e]\n",
		  proc,iter, chi2 - reg, reg, olambda, lambda);
	continue;
      }
	
    }

    /* --- printout --- */
    
    t1 = getTime();

    if(verb)
      fprintf(stderr,"[p:%4d,i:%4d] chi2=%14.5f (%f, %f), dchi2=%e, lambda=%e, elapsed=%5.3fs %s\n",
	      proc,iter, rchi2, reg ,regul_scal,chi2-ochi2, olambda, t1-t0,rej.c_str());
    
    ochi2 = chi2;

    
    /* --- Check if we are breaking the loop --- */
    
    if(chi2 < chi2_thres){
      if(verb)
	fprintf(stderr,"clm::fitdata: [p:%4d] Chi2 threshold reached [%f] -> chi2=%f\n",
		proc,chi2_thres, bestchi2);
      break;
    }
    
    if(nretry >= maxreject){
      if(verb) 
	fprintf(stderr,"clm::fitdata: [p:%4d] Too many failed attempts, finalizing inversion\n", proc);
      break;
    }

    if(exitme){
      if(verb) 
	fprintf(stderr,"clm::fitdata:  [p:%4d] relative change in chi2 is too low, inversion finished\n", proc);
      break;
    }
    

    /* --- prepare Jacobian for the next iteration --- */
    
    t0 = t1;
    zero(res, rf);
    dregul.zero();
    //
    status = fx(npar, nd, x, res, rf, mydat, dregul, false);
    //
    if(status){
      if(verb)
	fprintf(stderr, "clm::fitdata: [p:%4d] ERROR in the evaluation of FX, aborting inversion\n", proc);
      error = true;
      break;
    }
    
    scaleRF(rf);
    iter++;

    
    /* --- Scale down the regularization term if needed --- */

    const int offset_iter = 4;
    if( (fabs(reldchi) < 2.e-2) && 0){
      regul_scal *= 0.75;
      dcreased = true;
    }
  }
  
  /* --- Copy results to output array --- */

  memcpy(&x[0], &bestpars[0], npar*sizeof(double));
  
    
  /* --- clean-up --- */

  del_mat(rf);
  delete [] res;
  delete [] bestpars;
  delete [] xnew;
  //delete [] dregul;


  /* --- Since the best solution between different inversions of the same pixel may have
     reduced the regularization term by different amounts, it makes more sense to scale the 
     regularization with the initial scale factor (not the decreased one) --- */

  return (double)bestchi2;//orchi2 + dregul.getReg(); 
}


/* -------------------------------------------------------------------------------- */

void clm::compute_trial3(double *res, double **rf, double lambda,
			 double *x, double *xnew, reg_t &dregul)
{

  /* --- 
     Remember that dregul[npar] is the penalty term squared! 
     dregul[npar] = gamma * gamma;

     --- */
  
  
  /* --- Init Eigen-3 arrays --- */
  
  MatrixXd A(npar, npar); A.Zero(npar, npar);
  VectorXd B(npar); B.Zero(npar);
  Map<VectorXd> RES(xnew, npar);
  int npen = dregul.nreg;
  
  /* --- other arrays and constants --- */

  double **LL = mat2d(npar,npar), *tmp1 = new double [npen]();
  double ww[npar], wt[npar], wi[npar][2];
  
  /* --- 
     compute the Hessian matrix and the right-hand side of eq.: 
     A*(dx) = B
     where A = J.T # J
           B = J.T # res
	   and "dx" is the correction to the current model

     If we have regulatization, then the system is:

           A = J.T # J     +    L.T # L
	   B = J   # res   -    L.T # gamma 

     where gamma is the vector of individual penalties
     and L is a diagonal matrix with the derivatives of gamma.

     dregul is a vector where [0:npar-1] contains the derivatives of the
     gamma^2 functions, [npar:2*npar-1] contains the individual penalty 
     functions (not squared), and [2*npar] is the penalty term.
     --- */

  
  if(dregul.to_reg && true){
    /* --- 
       Compensate linear system with regularization terms:
       
       -> The Hessian matrix is modified in the diagonal addition of
       the derivative of the penalty function squared.
       
       -> The right hand side is modifed with the dot product of
       the L matrix with the individual penalties array (not squared).
       --- */

    for(int yy = 0; yy<npar; yy++){

      for(int xx = 0; xx<npar; xx++){
	
	for(int jj=0; jj<npen; jj++)
	  tmp1[jj] = dregul.dreg[jj][xx] * dregul.dreg[jj][yy]; // L.t # L = L # L.t
	
	LL[yy][xx] = sumarr(tmp1, npen);

      }//xx
      for(int jj=0;jj<npen; jj++)
	tmp1[jj] =  dregul.dreg[jj][yy] * dregul.reg[jj]; // L.t # gamm
      
      B[yy]  = - sumarr(tmp1, npen);    
    }//yy
  }

  
  // Totally splitting the function in vectors and not using dreg as a matrix
  
  if(dregul.to_reg && false ){
    for(int jj = 0; jj<npen; jj++)
      for(int yy = 0; yy<npar; yy++){
	
	for(int xx = 0; xx<npar; xx++) LL[yy][xx] += dregul.dreg[jj][xx] * dregul.dreg[jj][yy];
     
	B[yy] -=  dregul.dreg[jj][yy] * dregul.reg[jj];     
      }//yy
  }
  

  
  //cerr<<endl;
  
  for(int yy = 0; yy<npar; yy++){

    
    /* --- Compute the Hessian matrix --- */

    for(int xx = 0; xx<=yy; xx++){
      for(int ww = 0; ww<nd; ww++) tmp[ww] = rf[yy][ww] * rf[xx][ww];
      
      A(yy,xx) = A(xx,yy) = sumarr_4(&tmp[0], nd); // Remember that A is symmetric!

    } // xx




    if(dregul.to_reg){
      for(int xx=0;xx<npar;xx++)
	A(yy,xx) += LL[yy][xx];
    } 
    
    
    
    /* --- There are claims that it works better to store the 
       largest diagonal terms in this cycle and multiply lambda 
       by this value than the current estimate.
       --- */
    
    //diag[yy] = max(A(yy,yy), diag[yy]*0.6);
    //double idia = diag[yy];


    
    /* --- Damp the diagonal of A --- */
    
    A(yy,yy) += lambda * A(yy,yy);


    
    /* --- Compute J * Residue --- */
    
    for(int ww = 0; ww<nd; ww++) tmp[ww] = rf[yy][ww] * res[ww];
    B[yy] += sumarr_4(&tmp[0], nd);

    
  } // yy

  del_mat(LL);
  delete [] tmp1;
  
  /* --- 
     Solve linear system with SVD decomposition and singular value thresholding.
     The SVD is computed with Eigen3 instead of LaPack.
     --- */
  
  JacobiSVD<MatrixXd,ColPivHouseholderQRPreconditioner> svd(A, ComputeThinU | ComputeThinV);
  

  
  if(nvar > 1){
    MatrixXd U = svd.matrixU(), V = svd.matrixV();
    VectorXd W = svd.singularValues();

    /* --- 
       Two steps, the second is recomputed with the filtered singular values 
       --- */
    
    for(int tt=0;tt<2;tt++){ 
      memset( ww,0,npar*sizeof(double));

      for(int nn=0;nn<nvar;nn++){
	memset( wt,0,npar*sizeof(double));

	
	/* --- Get submatrix --- */
	
	for(int j=0;j<npar;j++)
	  for(int i=0;i<ntype[nn];i++){
	    int ii = pidx[nn][i];
	    wt[j] += V(j,ii)*V(j,ii)*W[j];
	  }

	
	/* --- Get the maximum --- */
	
	double wmax = 0.0;
	for(int j=0;j<npar;j++)
	  if(wt[j] > wmax) wmax = wt[j];

	
	/* --- Remove singular values for each type of variable --- */

	wmax *= svd_thres;
	for(int j=0;j<npar;j++){
	  if(wt[j] < wmax) wt[j] = 0.0;
	  else ww[j] += wt[j];
	} // j
      }// nn

      for(int j=0;j<npar;j++){
	W[j] = ww[j];
	wi[j][tt] = W[j];	
      }
    } // tt

    
    /* --- Now recover filtered singular values from submatrix --- */
    
    for(int j=0;j<npar;j++){
      if(fabs(wi[j][1]) > 1.e-10) W[j] = wi[j][0]*wi[j][0] / wi[j][1];
      else                        W[j] = 0.0;
    }
    

    /* --- Now solve the linear system --- */
    
    memcpy(xnew, &B[0], npar*sizeof(double));
    backSub(npar, U, W, V,  xnew);
    
    
  }else{
    svd.setThreshold(svd_thres);
    RES = svd.solve(B);
  }
  //double relative_error = (A*RES - B).norm() / B.norm();
  //fprintf(stderr,"The relative error is: %e\n", relative_error);
  
  /* --- 
     New estimate of the parameters, xnew = x + dx.
     Check for maximum change, add to current pars and normalize.
     --- */
  
  scaleParameters(xnew);
  checkMaxChange(xnew, x);
  
  for(int ii = 0; ii<npar; ii++) xnew[ii] += x[ii];


  
  /* --- Check that new parameters are within limits --- */
  if(!reset_par)
    checkParameters(xnew);
  

 
}

/* -------------------------------------------------------------------------------- */

void clm::backSub(int n, MatrixXd &u, VectorXd &w, MatrixXd &v,  double *b)
{

  double *tmp = new double [n];
  
  for(int j = 0; j<n; j++){
    if(w[j] != 0.0){
      double s = 0.0;
      for(int i=0;i<n;i++) s += u(i,j)*b[i];
      s/= w[j], tmp[j] = s;
    } else tmp[j] = 0.0;
    
  }// j
  
  for(int j = 0; j<n;j++){
    double s = 0.0;
    for(int jj=0;jj<n;jj++) s += v(j,jj)*tmp[jj];
    b[j] = s;
  }
    
  delete [] tmp;
}

/* -------------------------------------------------------------------------------- */

void clm::getParTypes()
{
  nvar = 0;
  unsigned npp[npar];
  memset(npp,0,sizeof(unsigned)*npar);

  
  /* --- get number of different parameters --- */
  
  for(int ii=0;ii<npar;ii++) npp[ptype[ii]]++;
  for(int ii=0;ii<npar;ii++) if(npp[ii] > 0) nvar++;

  
  /* --- store the indexes of each type of var --- */

  pidx.resize(nvar);
  ntype.resize(nvar);
  
  int k = 0;

  for(int ii=0;ii<npar;ii++){
    if(npp[ii] > 0){
      ntype[k] = npp[ii];
      pidx[k].resize(npp[ii],0);
      int z=0;
      for(int jj=0;jj<npar;jj++) if(ptype[jj] == ii) pidx[k][z++] = jj;
      k++;
    }
  }

  
  /* --- Printout only for debugging ? --- */

  if(0){
    fprintf(stderr,"\nclm::getParTypes: nvar = %d, SVD singular values will be filtered per variable\n", nvar);
    for(int ii=0;ii<nvar;ii++){
      fprintf(stderr,"    [%3d] -> [ ", ii);
      for(int jj=0;jj<ntype[ii];jj++) fprintf(stderr, "%2d ", pidx[ii][jj]);
      cerr<<"]"<<endl;				 
    }
    first = false;
  }
  
  
}
