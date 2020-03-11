#include <iostream>
#include <fstream>
#include <sstream>
#include <numeric>
#include "EigenPeetz.h"
#include "EigLab.h"

using namespace std;

// BLAS routines
extern "C"{
double ddot_(const int *N, const double *x, const int *incx, const double *y, const int *incy);
void daxpy_(const int *N, const double *a, const double *x, const int *incx,
    const double *y, const int *incy);
}

//TODO: Investigate the whether eigensolver in Eigen is sufficient or if solvers
// in BLAS/LAPACK are necessary for performance (for subspace problem)

/********************************************************************
 * Initialize Variables and Petsc logging functionality (called
 * when program starts, similar to PetscInitialize)
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode EigenPeetz::Initialize()
{
  PetscErrorCode ierr = 0;
  ierr = PetscLogEventRegister("EIG_Compute", 0, &EIG_Compute); CHKERRQ(ierr);
  ierr = PetscLogEventRegister("EIG_Initialize", 0, &EIG_Initialize); CHKERRQ(ierr);
  ierr = PetscLogEventRegister("EIG_Prep", 0, &EIG_Prep); CHKERRQ(ierr);
  ierr = PetscLogEventRegister("EIG_Comp_Init", 0, &EIG_Comp_Init); CHKERRQ(ierr);
  ierr = PetscLogEventRegister("EIG_Hierachy", 0, &EIG_Hierarchy); CHKERRQ(ierr);
  ierr = PetscLogEventRegister("EIG_Setup_Coarse", 0, &EIG_Setup_Coarse); CHKERRQ(ierr);
  ierr = PetscLogEventRegister("EIG_Comp_Coarse", 0, &EIG_Comp_Coarse); CHKERRQ(ierr);
  ierr = PetscLogEventRegister("EIG_Convergence", 0, &EIG_Convergence); CHKERRQ(ierr);
  ierr = PetscLogEventRegister("EIG_Expand", 0, &EIG_Expand); CHKERRQ(ierr);
  ierr = PetscLogEventRegister("EIG_Update", 0, &EIG_Update); CHKERRQ(ierr);
  ierr = PetscLogEventRegister("EIG_MGSetup", 0, &EIG_MGSetup); CHKERRQ(ierr);
  ierr = PetscLogEventRegister("EIG_Precondition", 0, &EIG_Precondition); CHKERRQ(ierr);
  ierr = PetscLogEventRegister("EIG_Jacobi", 0, &EIG_Jacobi); CHKERRQ(ierr);

  PetscInt mg_levels = 30;
  EIG_ApplyOP  = new PetscLogEvent[mg_levels-1];
  EIG_ApplyOP1 = new PetscLogEvent[mg_levels-1];
  EIG_ApplyOP2 = new PetscLogEvent[mg_levels-1];
  EIG_ApplyOP3 = new PetscLogEvent[mg_levels-1];
  EIG_ApplyOP4 = new PetscLogEvent[mg_levels-1];
  for (int i = 0; i < mg_levels-1; i++) {
    char event_name[30];
    sprintf(event_name, "EIG_ApplyOP_%i", i+1);
    ierr = PetscLogEventRegister(event_name, 0, EIG_ApplyOP+i); CHKERRQ(ierr);
    sprintf(event_name, "EIG_ApplyOP1_%i", i+1);
    ierr = PetscLogEventRegister(event_name, 0, EIG_ApplyOP1+i); CHKERRQ(ierr);
    sprintf(event_name, "EIG_ApplyOP2_%i", i+1);
    ierr = PetscLogEventRegister(event_name, 0, EIG_ApplyOP2+i); CHKERRQ(ierr);
    sprintf(event_name, "EIG_ApplyOP3_%i", i+1);
    ierr = PetscLogEventRegister(event_name, 0, EIG_ApplyOP3+i); CHKERRQ(ierr);
    sprintf(event_name, "EIG_ApplyOP4_%i", i+1);
    ierr = PetscLogEventRegister(event_name, 0, EIG_ApplyOP4+i); CHKERRQ(ierr);
  }
  return ierr;
}

/********************************************************************
 * Terminate the Petsc logging functionality (called) when program ends
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode EigenPeetz::Finalize()
{
  PetscErrorCode ierr = 0;

  delete[] EIG_ApplyOP;
  delete[] EIG_ApplyOP1;
  delete[] EIG_ApplyOP2;
  delete[] EIG_ApplyOP3;
  delete[] EIG_ApplyOP4;

  return ierr;
}

/********************************************************************
 * The main constructor
 * 
 *******************************************************************/
EigenPeetz::EigenPeetz()
{
  n = 0;
  nev_req = 6; nev_conv = 0;
  tau = LM;
  tau_num = 0;
  eps = 1e-6;
  maxit = 500;
  verbose = 0;
  file_opened = 0;
  PetscOptionsGetInt(NULL, NULL, "-EigenPeetz_Verbose", &verbose, NULL);
  PetscOptionsGetInt(NULL, NULL, "-EigenPeetz_nev", &this->nev_req, NULL);
  PetscOptionsGetScalar(NULL, NULL, "-EigenPeetz_tol", &this->eps, NULL);
  PetscOptionsGetInt(NULL, NULL, "-EigenPeetz_maxit", &this->maxit, NULL);
}

/********************************************************************
 * Main Destructor
 * 
 *******************************************************************/
EigenPeetz::~EigenPeetz()
{
  for (unsigned int ii = 0; ii < A.size(); ii++)
    MatDestroy(A.data()+ii);
  for (unsigned int ii = 0; ii < B.size(); ii++)
    MatDestroy(B.data()+ii);
  VecDestroyVecs(nev_conv, &phi);
  PetscErrorCode ierr = Close_File(); CHKERRV(ierr);
}

/********************************************************************
 * Set how much information the subroutines print
 * 
 * @param verbose: The higher the number, the more is printed
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode EigenPeetz::Set_Verbose(PetscInt verbose)
{
  this->verbose = verbose;
  PetscErrorCode ierr = PetscOptionsGetInt(NULL, NULL, "-EigenPeetz_Verbose", &this->verbose, NULL);
  CHKERRQ(ierr);
  return 0;
}

/********************************************************************
 * Set a file that is already being used to print the output to
 * 
 * @param output: The file handle
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode EigenPeetz::Set_File(FILE *output)
{
  PetscErrorCode ierr = 0;
  ierr = Close_File(); CHKERRQ(ierr);
  this->output = output;
  file_opened = false;
  return ierr;
}

/********************************************************************
 * Create a file to print the output to
 * 
 * @param filename: The name of the file
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode EigenPeetz::Open_File(const char filename[])
{
  PetscErrorCode ierr = 0;
  ierr = Close_File(); CHKERRQ(ierr);
  ierr = PetscFOpen(comm, filename, "w", &output); CHKERRQ(ierr);
  file_opened = true;
  return ierr;
}

/********************************************************************
 * Set the operators that define the eigen system
 * 
 * @param A: The first matrix
 * @param B: The (optional) second matrix
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode EigenPeetz::Set_Operators(Mat A, Mat B)
{
  PetscErrorCode ierr = 0;
  if (this->verbose >= 3)
    ierr = PetscFPrintf(comm, output, "Setting operators\n"); CHKERRQ(ierr);

  if (this->A.size() > 0) {
    ierr = MatDestroy(this->A.data()); CHKERRQ(ierr);
    this->A[0] = A;
    ierr = PetscObjectReference((PetscObject)A); CHKERRQ(ierr);
  }
  else {
    this->A.push_back(A);
    ierr = PetscObjectReference((PetscObject)A); CHKERRQ(ierr);
  }
  if (this->B.size() > 0) {
    ierr = MatDestroy(this->B.data()); CHKERRQ(ierr);
    this->B[0] = B;
    ierr = PetscObjectReference((PetscObject)B); CHKERRQ(ierr);
  }
  else {
    this->B.push_back(B);
    ierr = PetscObjectReference((PetscObject)B); CHKERRQ(ierr);
  }

  return 0;
}

/********************************************************************
 * Sets target eigenvalues and number to find
 * 
 * @param tau: The target type (e.g. 'LM') or a target value
 * @param nev: The number of eigenmodes to calculate
 * @param ntype: The type of eigenvalues (e.g. 6 total, or 6 unique)
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode EigenPeetz::Set_Target(Tau tau, PetscInt nev, Nev_Type ntype)
{
  PetscErrorCode ierr = 0;

  if (this->verbose >= 3) {
    ierr = PetscFPrintf(comm, output, "Setting target eigenvalues\n"); CHKERRQ(ierr);
  }
  this->tau = tau;
  this->nev_req = nev;
  this->nev_type = ntype;
  this->Qsize = nev_req + (this->nev_type == TOTAL_NEV ? 0 : 6);
  ierr = PetscOptionsGetInt(NULL, NULL, "-EigenPeetz_nev", &this->nev_req, NULL);

  return ierr;
}
PetscErrorCode EigenPeetz::Set_Target(PetscScalar tau, PetscInt nev, Nev_Type ntype)
{ 
  PetscErrorCode ierr = 0;

  if (this->verbose >= 3) {
    ierr = PetscFPrintf(comm, output, "Setting target eigenvalues\n"); CHKERRQ(ierr);
  }
  this->tau = NUMERIC;
  this->tau_num = tau;
  this->nev_req = nev;
  this->nev_type = ntype;
  this->Qsize = nev_req + (this->nev_type == TOTAL_NEV ? 0 : 6);
  ierr = PetscOptionsGetInt(NULL, NULL, "-EigenPeetz_nev", &this->nev_req, NULL);
      CHKERRQ(ierr);

  return ierr;
}

/********************************************************************
 * Sets convergence tolerance
 * 
 * @param tol: Convergence tolerance
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode EigenPeetz::Set_Tol(PetscScalar tol)
{
  this->eps = tol;
  PetscErrorCode ierr = PetscOptionsGetScalar(NULL, NULL, "-EigenPeetz_tol",
                                             &this->eps, NULL);
      CHKERRQ(ierr);
  return ierr;
}

/********************************************************************
 * Sets maximum iterations
 * 
 * @param maxit: Maximum number of iterations
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode EigenPeetz::Set_MaxIt(PetscInt maxit) 
{
  this->maxit = maxit;
  PetscErrorCode ierr = PetscOptionsGetInt(NULL, NULL, "-EigenPeetz_maxit",
                                           &this->maxit, NULL);
      CHKERRQ(ierr);
  return ierr;
}

/********************************************************************
 * Check if all eigenvalues have been found
 * 
 * @return done: A flag indicating completion
 * 
 *******************************************************************/
bool EigenPeetz::Done()
{
  if ((nev_conv == 1) && (nev_type != TOTAL_NEV))
    return false;
  ArrayXPS lambda_sort = lambda.segment(0,nev_conv);
  MatrixXPS empty(0,0);
  Sorteig(empty, lambda_sort);
  switch (nev_type) {
    case TOTAL_NEV:
      return (nev_conv == nev_req);
    case UNIQUE_NEV:
      return (((1.0 - lambda_sort.segment(1,nev_conv-1).array() /
                      lambda_sort.segment(0,nev_conv-1).array()).abs() 
              > 1e-5).cast<int>().sum() == nev_req);
    case UNIQUE_LAST_NEV:
      return ((nev_conv > nev_req) && (abs(1.0 - 
               lambda_sort(nev_conv-2) / lambda_sort(nev_conv-1)) > 1e-5));
  }
  return false;
}

/********************************************************************
 * Sort the Ritz modes
 * 
 * @param W: Ritz vectors
 * @param S: Ritz values
 * 
 * @return ind: Sorting order
 * 
 *******************************************************************/
Eigen::ArrayXi EigenPeetz::Sorteig(MatrixXPS &W, ArrayXPS &S)
{
  MatrixXPS Wcopy = W;
  ArrayXPS Scopy = S;
  Eigen::ArrayXi ind;
  switch (tau) {
    case NUMERIC:
      S = S-tau_num;
      ind = EigLab::gensort(S);
      break;
    case LM:
      S = S.cwiseAbs();
      ind = EigLab::gensort(S).reverse();
      break;
    case SM:
      S = S.cwiseAbs();
      ind = EigLab::gensort(S);
      break;
    case LR: case LA:
      ind = EigLab::gensort(S).reverse();
      break;
    case SR: case SA:
      ind = EigLab::gensort(S);
      break;
  }

  for (int ii = 0; ii < ind.size(); ii++) {
    S(ii) = Scopy(ind(ii));
    if (W.size() > 0)
      W.col(ii) = Wcopy.col(ind(ii));
  }

  return ind;
}

/********************************************************************
 * Iterative classical M-orthogonal Gram-Schmidt 
 * 
 * @param Q: Vectors to orthogonalize against
 * @param M: Matrix defining the inner product
 * @param u: Vector to orthogonalize
 * @param r: Norm of u after orthogonalization
 * @param k: Number of vectors in Q
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode EigenPeetz::Icgsm(Vec *Q, Mat M, Vec u, PetscScalar &r, PetscInt k)
{
  PetscErrorCode ierr = 0;
  double alpha = 0.5;
  int itmax = 3, it = 1;
  Vec um;
  ierr = VecDuplicate(u, &um); CHKERRQ(ierr);
  ierr = MatMult(M, u, um); CHKERRQ(ierr);
  PetscScalar r0;
  ierr = VecDot(u, um, &r0); CHKERRQ(ierr);
  r0 = sqrt(r0);
  while (true) {
    ierr = VecMDot(um, k, Q, TempScal.data()); CHKERRQ(ierr);
    TempScal *= -1;
    ierr = VecMAXPY(u, k, TempScal.data(), Q); CHKERRQ(ierr);
    //ierr = GS(Q, um, u, k); CHKERRQ(ierr);
    ierr = MatMult(M, u, um); CHKERRQ(ierr);
    ierr = VecDot(u, um, &r); CHKERRQ(ierr);
    r = sqrt(r);
    if (r > alpha*r0 || it > itmax)
      break;
    it++; r0 = r;
  }
  if (r <= alpha*r0) {
    SETERRQ(comm, PETSC_ERR_FP, "Breakdown in ICGSM routine");
  }

  ierr = VecDestroy(&um); CHKERRQ(ierr);

  return 0;
}

/********************************************************************
 * Modified M-orthogonal Gram-Schmidt 
 * 
 * @param Q: Vectors to orthogonalize against
 * @param BQ: Vectors multiplied by matrix defining the inner product
 * @param u: Vector to orthogonalize
 * @param k: Number of vectors in Q
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode EigenPeetz::Mgsm(Vec* Q, Vec* BQ, Vec u, PetscInt k)
{
  PetscErrorCode ierr = 0;

  for (int ii = 0; ii < k; ii++) {
    ierr = VecDot(u, BQ[ii], TempScal.data()); CHKERRQ(ierr);
    ierr = VecAXPY(u, -TempScal(0), Q[ii]); CHKERRQ(ierr);
  }

  return 0;
}

/********************************************************************
 * Standard M-orthogonal Gram-Schmidt orthogonalization step
 * 
 * @param Q: Vectors to orthogonalize against
 * @param Mu: Matrix defining the inner product times u
 * @param u: Vector to orthogonalize
 * @param k: Number of vectors in Q
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode EigenPeetz::GS(Vec* Q, Vec Mu, Vec u, PetscInt k)
{
  // So far this function only hurts performance and is not yet usable
  PetscErrorCode ierr = 0;

  PetscScalar *p_Mu, *p_u, **p_Q;
  int one = 1, bn;
  ierr = VecGetLocalSize(u, &bn); CHKERRQ(ierr);
  MPI_Request *request = new MPI_Request[k];
  int *flag = new int[k];
  int allflag = 0;

  // Serial Dot products on each process, share partial result with Iallreduce
  ierr = VecGetArray(Mu, &p_Mu); CHKERRQ(ierr);
  ierr = VecGetArrays(Q, k, &p_Q); CHKERRQ(ierr);
  for (int ii = 0; ii < k; ii++) {
    TempScal(ii) = -ddot_(&bn, p_Mu, &one, p_Q[ii], &one);
    //MPI_Iallreduce(MPI_IN_PLACE, TempScal.data()+ii, 1, MPI_DOUBLE, MPI_SUM,
    //               comm, request+ii);
  }
  ierr = VecRestoreArray(Mu, &p_Mu); CHKERRQ(ierr);

  // Remove projections of each vector once allReduce finishes
  ierr = VecGetArray(u, &p_u); CHKERRQ(ierr);
  fill(flag, flag+k, 0);
  while (allflag < k) {
    for (int ii = 0; ii < k; ii++) {
      if (flag[ii])
        continue;
      MPI_Test(request+ii, flag+ii, MPI_STATUS_IGNORE);
      if (flag[ii]) {
        daxpy_(&bn, TempScal.data()+ii, p_Q[ii], &one, p_u, &one);
      }
    }
    allflag = accumulate(flag, flag+k, 0);
  }

  ierr = VecRestoreArrays(Q, k, &p_Q); CHKERRQ(ierr);
  ierr = VecRestoreArray(u, &p_u); CHKERRQ(ierr);
  delete[] request;
  delete[] flag;

  return 0;
}
