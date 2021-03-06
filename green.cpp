#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "green.h"
#include <complex>

#define MAXLINE 256
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))

/*
#ifdef OMP
#include "omp.h"
#endif */
/*******************************************************************************
 * The class of Green is designed to evaluate the LDOS via the Green's Function
 * method. The meaning of input/output parameters are as follows:
 *
 *   natom   (input, value)  total number of atoms in system
 *   sysdim  (input, value)  dimension of the system; usually 3
 *   nit     (input, value)  maximum iterations during Lanczos diagonalization
 *   wmin    (input, value)  minimum value for the angular frequency
 *   wmax    (input, value)  maximum value for the angular frequency
 *   nw      (input, value)  total number of points in LDOS
 *   epson   (input, value)  epson that govens the width of delta-function
 *   Hessian (input, pointer of pointer) mass-weighted force constant matrix, of
 *                           dimension [natom*sysdim][natm*sysdim]; it is actually
 *                           the dynamical matrix at gamma point
 *   iatom   (input, value)  index of the atom to evaluate local phonon DOS
 *******************************************************************************
 * References:
 *  1. Z. Tang and N. R. Aluru, Phys. Rev. B 74, 235441 (2006).
 *  2. C. Hudon, R. Meyer, and L.J. Lewis, Phys. Rev. B 76, 045409 (2007).
 *  3. L.T. Kong and L.J. Lewis, Phys. Rev. B 77, 165422 (2008).
 *
 * NOTE: The real-space Green's function method is not expected to work accurately
 *       for small systems, say, system less than 500 atoms.
 *******************************************************************************/

/*------------------------------------------------------------------------------
 * Constructor is used as the main driver
 *----------------------------------------------------------------------------*/
Green::Green(const int ntm, const int sdim, const int niter, const double min, const double max,
             const int ndos, const double eps, double **Hessian, const int itm)
{
  const int NMAX = 300;
  const double tpi = 8.*atan(1.);
  natom = ntm; sysdim = sdim; nit = niter; epson = eps;
  wmin = min*tpi; wmax = max*tpi; nw = ndos + (ndos+1)%2;
  H = Hessian; iatom = itm;

  memory = new Memory;
  if (natom < 1 || iatom < 1 || iatom > natom){
    printf("\nError: Wrong number of total atoms or wrong index of interested atom!\n");
    return;
  }
  ndim = natom * sysdim;
  if (natom < NMAX) nit = ndim;

  if (nit < 1)   {printf("\nError: Wrong input of maximum iterations!\n"); return;}
  if (nit > ndim){printf("\nError: # Lanczos iterations is not expected to exceed the degree of freedom!\n"); return;}
  if (nw  < 1)   {printf("\nError: Wrong input of points in LDOS!\n"); return;}

  // initialize variables and allocate local memories
  dw    = (wmax - wmin)/double(nw-1);
  memory->create(alpha,sysdim,nit,  "Green_Green:alpha");
  memory->create(beta,sysdim,nit+1,"Green_Green:beta");
  memory->create(ldos,sysdim,nw, "Green_Green:ldos");

/*
#ifdef OMP
  npmax = omp_get_max_threads();
#endif */
  // use Lanczos algorithm to diagonalize the Hessian
  Lanczos();
  // Get the inverser of the treated hessian by continued fractional method
  if (natom < NMAX) recursion();
  else Recursion();

  // normalize the LDOS computed
  Normalize();

  // write the result
  writeLDOS();

return;
}

/*------------------------------------------------------------------------------
 * Deconstructor is used to free memory
 *----------------------------------------------------------------------------*/
Green::~Green()
{
  H = NULL;
  memory->destroy(alpha);
  memory->destroy(beta);
  memory->destroy(ldos);

  delete memory;

return;
}
      
/*------------------------------------------------------------------------------
 * Private method to diagonalize a matrix by the Lanczos algorithm
 *----------------------------------------------------------------------------*/
void Green::Lanczos()
{
  int ipos = (iatom-1)*sysdim;

  // Loop over dimension
  //#pragma omp parallel for default(shared) schedule(dynamic,1) num_threads(MIN(sysdim,npmax))
  for (int idim=0; idim<sysdim; idim++){

    double v0[ndim], v1[ndim], w0[ndim];
    double *vp = &v0[0], *v  = &v1[0], *w  = &w0[0];

    beta[idim][0] = 0.;
    for (int i=0; i<ndim; i++){vp[i] = v[i] = 0.;}
    v[ipos+idim] = 1.;

    // Loop on fraction levels
    for (int i=0; i<nit; i++){
      double sum_a = 0.;
      for (int j=0; j<ndim; j++){
        double sumHv = 0.;
        for (int k=0; k<ndim; k++) sumHv += H[j][k]*v[k];
        w[j] = sumHv - beta[idim][i]*vp[j];
        sum_a += w[j]*v[j];
      }
      alpha[idim][i] = sum_a;

      for (int k=0; k<ndim; k++) w[k] -= alpha[idim][i]*v[k];

      double gamma = 0.;
      for (int k=0; k<ndim; k++) gamma += w[k]*v[k];
      for (int k=0; k<ndim; k++) w[k] -= gamma*v[k];

      double sum_b = 0.;
      for (int k=0; k<ndim; k++) sum_b += w[k]*w[k];
      beta[idim][i+1] = sqrt(sum_b);

      double *ptr = vp; vp = v; v = ptr; ptr = NULL;
      double tmp = 1./beta[idim][i+1];    
      for (int k=0; k<ndim; k++) v[k] = w[k]*tmp;
    }
  }

return;
}

/*------------------------------------------------------------------------------
 * Private method to compute the LDOS via the recusive method for system with
 * many atoms
 *----------------------------------------------------------------------------*/
void Green::Recursion()
{
  // local variables
  double *alpha_inf, *beta_inf, *xmin, *xmax;
  memory->create(alpha_inf, sysdim, "Recursion:alpha_inf");
  memory->create(beta_inf,  sysdim, "Recursion:beta_inf");
  memory->create(xmin, sysdim, "Recursion:xmin");
  memory->create(xmax, sysdim, "Recursion:xmax");

  int nave = nit/4;
  //#pragma omp parallel for default(shared) schedule(guided) num_threads(MIN(sysdim,npmax))
  for (int idim=0; idim<sysdim; idim++){
    alpha_inf[idim] = beta_inf[idim] = 0.;

    for (int i= nit-nave; i<nit; i++){
      alpha_inf[idim] += alpha[idim][i];
      beta_inf[idim] += beta[idim][i+1];
    }

    alpha_inf[idim] /= double(nave);
    beta_inf[idim]  /= double(nave);

    xmin[idim] = alpha_inf[idim] - 2.*beta_inf[idim];
    xmax[idim] = alpha_inf[idim] + 2.*beta_inf[idim];
  }

  // #pragma omp parallel for default(shared) schedule(guided) num_threads(npmax)
  for (int i=0; i<nw; i++){
    double w = wmin + double(i)*dw;
    double a = w*w;
    std::complex<double> Z = std::complex<double>(w*w, epson);

    for (int idim=0; idim<sysdim; idim++){
      double two_b = 2.*beta_inf[idim]*beta_inf[idim];
      double rtwob = 1./two_b;

      std::complex<double> z_m_a = Z - alpha_inf[idim]*alpha_inf[idim];
      std::complex<double> r_x, rec_x, rec_x_inv;
      double ax, bx;

      if ( a < xmin[idim] ){
        r_x = sqrt(-2.*two_b + z_m_a);
        ax = std::real(r_x) * rtwob;
        bx = std::imag(r_x) * rtwob;
      } else if (a > xmax[idim]) {
        r_x = sqrt(-2.*two_b + z_m_a);
        ax = -std::real(r_x) * rtwob;
        bx = -std::imag(r_x) * rtwob;
      } else {
        r_x = sqrt(2.*two_b - z_m_a);
        ax =  std::imag(r_x) * rtwob;
        bx = -std::real(r_x) * rtwob;
      }

      double sr = (a - alpha_inf[idim])*rtwob + ax;
      double si = epson * rtwob + bx;
      rec_x = std::complex<double> (sr, si);

      for (int j=0; j<nit; j++){
        rec_x_inv = Z - alpha[idim][nit-j-1] - beta[idim][nit-j]*beta[idim][nit-j]*rec_x;
        rec_x = 1./rec_x_inv;
      }
      ldos[idim][i] = std::imag(rec_x)*w;
    }
  }
  memory->destroy(alpha_inf);
  memory->destroy(beta_inf);
  memory->destroy(xmin);
  memory->destroy(xmax);

return;
}

/*------------------------------------------------------------------------------
 * Private method to compute the LDOS via the recusive method for system with
 * a few atoms (less than NMAX)
 *----------------------------------------------------------------------------*/
void Green::recursion()
{
  // #pragma omp parallel for default(shared) schedule(guided) num_threads(npmax)
  for (int i=0; i<nw; i++){
    double w = wmin + double(i)*dw;
    std::complex<double> Z = std::complex<double>(w*w, epson);

    for (int idim=0; idim<sysdim; idim++){
      std::complex<double> rec_x = std::complex<double>(0.,0.);

      for (int j=0; j<nit; j++){
        std::complex<double> rec_x_inv = Z - alpha[idim][nit-j-1] - beta[idim][nit-j]*beta[idim][nit-j]*rec_x;
        rec_x = 1./rec_x_inv;
      }
      ldos[idim][i] = std::imag(rec_x)*w;
    }
  }
return;
}

/*------------------------------------------------------------------------------
 * Private method to normalize the LDOS computed
 *----------------------------------------------------------------------------*/
void Green::Normalize()
{
  // normalize ldos
  double df = dw /(8.*atan(1.));

  // #pragma omp parallel for default(shared) schedule(guided) num_threads(MIN(sysdim,npmax))
  for (int idim=0; idim<sysdim; idim++){
    double odd = 0., even = 0.;
    for (int i=1; i<nw-1; i += 2) odd  += ldos[idim][i];
    for (int i=2; i<nw-1; i += 2) even += ldos[idim][i];
    double sum = ldos[idim][0] + ldos[idim][nw-1];
    sum += 4.*odd + 2.*even;
    sum = 3./(sum*df);
    for (int i=0; i<nw; i++) ldos[idim][i] *= sum;
  }

return;
}

/*------------------------------------------------------------------------------
 * Private method to write the LDOS info to file
 *----------------------------------------------------------------------------*/
void Green::writeLDOS()
{
  char str[MAXLINE], *fname;
  sprintf(str,"pldos_%d.dat",iatom);
  fname = strtok(str, " \r\n\t\f");

  FILE *fp = fopen(fname, "w"); fname = NULL;
  fprintf(fp,"#Local phonon DOS for atom %d by RSGF method\n", iatom);
  fprintf(fp,"#freq"); for (int i=0; i<sysdim; i++) fprintf(fp," %c", 'x'+i);
  fprintf(fp," total\n");

  const double rtpi = 1./(8.*atan(1.));
  double freq = wmin * rtpi;
  double df = dw * rtpi;
  for (int i=0; i<nw; i++){
    double tldos = 0.;
    fprintf(fp,"%lg", freq);
    for (int idim=0; idim<sysdim; idim++){fprintf(fp," %lg", ldos[idim][i]); tldos+= ldos[idim][i];}
    fprintf(fp," %lg\n", tldos);

    freq += df;
  }
  fclose(fp);

return;
}
