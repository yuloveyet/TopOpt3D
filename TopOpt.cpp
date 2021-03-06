#include <mpi.h> // Has to precede Petsc includes to use MPI::BOOL
#include "TopOpt.h"
#include <fstream>
#include <climits>

using namespace std;

/********************************************************************
 * Initialization done by each constructor
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode TopOpt::Initialize()
{
  PetscErrorCode ierr = 0;

  smoother = "chebyshev";
  verbose = 1;
  folder = "";
  print_every = INT_MAX;
  last_print = 0;
  interpolation = SIMP;
  KUF_reason = KSP_CONVERGED_ITERATING;
  minGeoHybrid = 2;
  ierr = PetscOptionsGetInt(NULL, NULL, "-hybrid_min_geo_levels",
                            &minGeoHybrid, NULL);

  ierr = PrepLog(); CHKERRQ(ierr);
  MPI_Set();
  char fname[200] = "Output.txt";
  ierr = PetscOptionsGetString(NULL, NULL, "-output", fname, 200, NULL); CHKERRQ(ierr);
  ierr = PetscFOpen(comm, "Output.txt", "w", &output); CHKERRQ(ierr);

  return ierr;
}

/********************************************************************
 * Set up the loggers
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode TopOpt::PrepLog()
{
  PetscErrorCode ierr = 0;
  ierr = PetscLogEventRegister("Optimization Update", 0, &UpdateEvent); CHKERRQ(ierr);
  ierr = PetscLogEventRegister("Functions", 0, &funcEvent); CHKERRQ(ierr);
  ierr = PetscLogEventRegister("FE Analysis", 0, &FEEvent); CHKERRQ(ierr);
  return ierr;
}

/********************************************************************
 * Clear out the data structures
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode TopOpt::Clear()
{ 
  PetscErrorCode ierr = 0;
  delete[] B; delete[] G; delete[] GT; delete[] W;
  ierr = VecDestroy(&F); CHKERRQ(ierr);
  ierr = VecDestroy(&U); CHKERRQ(ierr);
  //ierr = MatDestroy(&spK); CHKERRQ(ierr);
  ierr = VecDestroy(&spKVec); CHKERRQ(ierr);
  ierr = MatDestroy(&K); CHKERRQ(ierr);
  ierr = VecDestroy(&MLump); CHKERRQ(ierr);
  ierr = KSPDestroy(&KUF); CHKERRQ(ierr);
  //ierr = KSPDestroy(&dynamicKSP); CHKERRQ(ierr);
  //ierr = KSPDestroy(&bucklingKSP); CHKERRQ(ierr);
  ierr = MatDestroy(&P); CHKERRQ(ierr);
  ierr = MatDestroy(&R); CHKERRQ(ierr);
  ierr = VecDestroy(&REdge); CHKERRQ(ierr);
  ierr = VecDestroy(&V); CHKERRQ(ierr);
  ierr = VecDestroy(&dVdrho); CHKERRQ(ierr);
  ierr = VecDestroy(&E); CHKERRQ(ierr);
  ierr = VecDestroy(&dEdz); CHKERRQ(ierr);
  ierr = VecDestroy(&Es); CHKERRQ(ierr);
  ierr = VecDestroy(&dEsdz); CHKERRQ(ierr);
  ierr = VecDestroy(&x); CHKERRQ(ierr);
  ierr = VecDestroy(&y); CHKERRQ(ierr);
  ierr = VecDestroy(&rho); CHKERRQ(ierr);
  ierr = VecDestroy(&rhoq); CHKERRQ(ierr);
  for (unsigned int i = 0; i < function_list.size(); i++)
    delete function_list[i];
  ierr = PetscFClose(comm, output); CHKERRQ(ierr);
  return ierr;
}

/********************************************************************
 * Print out the mesh information
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode TopOpt::MeshOut()
{
  PetscErrorCode ierr = 0;
  ofstream file;
  if (this->myid == 0) {
    file.open("Element_Distribution.bin", ios::binary);
    file.write((char*)this->elmdist.data(), this->elmdist.size()*sizeof(PetscInt));
    file.close();
    file.open("Node_Distribution.bin", ios::binary);
    file.write((char*)this->nddist.data(), this->nddist.size()*sizeof(PetscInt));
    file.close();
  }

  // Getting distribution of loads, supports, springs, and masses
  int loaddist = this->loadNode.rows(), suppdist = this->suppNode.rows();
  int eigsuppdist = this->eigenSuppNode.rows();
  int springdist = this->springNode.rows(), massdist = this->massNode.rows();
  MPI_Request loadreq, suppreq, eigsuppreq, springreq, massreq;
  ierr = MPI_Iscan(MPI_IN_PLACE, &loaddist, 1, MPI_INT, MPI_SUM, this->comm,
            &loadreq); CHKERRQ(ierr);
  ierr = MPI_Iscan(MPI_IN_PLACE, &suppdist, 1, MPI_INT, MPI_SUM, this->comm,
            &suppreq); CHKERRQ(ierr);
  ierr = MPI_Iscan(MPI_IN_PLACE, &eigsuppdist, 1, MPI_INT, MPI_SUM, this->comm,
            &eigsuppreq); CHKERRQ(ierr);
  ierr = MPI_Iscan(MPI_IN_PLACE, &springdist, 1, MPI_INT, MPI_SUM, this->comm,
            &springreq); CHKERRQ(ierr);
  ierr = MPI_Iscan(MPI_IN_PLACE, &massdist, 1, MPI_INT, MPI_SUM, this->comm,
            &massreq); CHKERRQ(ierr);

  MPI_File fh;
  int myid = this->myid, nprocs = this->nprocs;
  Eigen::Array<PetscInt, -1, -1, Eigen::RowMajor> global_int;
  Eigen::Array<double, -1, -1, Eigen::RowMajor> global_float;
  // Writing element array
  ierr = MPI_File_open(this->comm, "elements.bin", MPI_MODE_CREATE |
             MPI_MODE_WRONLY | MPI_MODE_DELETE_ON_CLOSE, MPI_INFO_NULL, &fh);
         CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);
  ierr = MPI_File_open(this->comm, "elements.bin", MPI_MODE_CREATE |
                       MPI_MODE_WRONLY, MPI_INFO_NULL, &fh); CHKERRQ(ierr);
  ierr = MPI_File_seek(fh, this->elmdist(myid) * this->element.cols() *
                       sizeof(PetscInt), MPI_SEEK_SET); CHKERRQ(ierr);
  global_int.resize(this->nLocElem, this->element.cols());
  for (int el = 0; el < this->nLocElem; el++) {
    for (int nd = 0; nd < this->element.cols(); nd++)
      global_int(el,nd) = this->gNode(this->element(el,nd)); 
  }
  ierr = MPI_File_write_all(fh, global_int.data(), global_int.size(),
                            MPI_PETSCINT, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);

  // Writing node array
  ierr = MPI_File_open(this->comm, "nodes.bin", MPI_MODE_CREATE |
             MPI_MODE_WRONLY | MPI_MODE_DELETE_ON_CLOSE, MPI_INFO_NULL, &fh);
         CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);
  ierr = MPI_File_open(this->comm, "nodes.bin", MPI_MODE_CREATE |
                       MPI_MODE_WRONLY, MPI_INFO_NULL, &fh); CHKERRQ(ierr);
  ierr = MPI_File_seek(fh, this->nddist(myid) * this->node.cols() *
                       sizeof(double), MPI_SEEK_SET); CHKERRQ(ierr);
  ierr = MPI_File_write_all(fh, this->node.data(), this->nLocNode *
             this->node.cols(), MPI_DOUBLE, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);

  // Writing load node array
  ierr = MPI_File_open(this->comm, "loadNodes.bin", MPI_MODE_CREATE |
             MPI_MODE_WRONLY | MPI_MODE_DELETE_ON_CLOSE, MPI_INFO_NULL, &fh);
         CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);
  ierr = MPI_File_open(this->comm, "loadNodes.bin", MPI_MODE_CREATE |
                       MPI_MODE_WRONLY, MPI_INFO_NULL, &fh); CHKERRQ(ierr);
  ierr = MPI_Wait(&loadreq, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  loaddist -= this->loadNode.rows();
  ierr = MPI_File_seek(fh, loaddist*sizeof(PetscInt), MPI_SEEK_SET); CHKERRQ(ierr);
  global_int.resize(this->loadNode.rows(), 1);
  for (int i = 0; i < this->loadNode.size(); i++)
    global_int(i, 0) = this->gNode(this->loadNode(i));
  ierr = MPI_File_write_all(fh, global_int.data(), global_int.size(),
                            MPI_PETSCINT, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);

  // Writing loads array
  ierr = MPI_File_open(this->comm, "loads.bin", MPI_MODE_CREATE |
             MPI_MODE_WRONLY | MPI_MODE_DELETE_ON_CLOSE, MPI_INFO_NULL, &fh);
         CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);
  ierr = MPI_File_open(this->comm, "loads.bin", MPI_MODE_CREATE |
                       MPI_MODE_WRONLY, MPI_INFO_NULL, &fh); CHKERRQ(ierr);
  ierr = MPI_File_seek(fh, loaddist * this->loads.cols() * sizeof(double),
                       MPI_SEEK_SET); CHKERRQ(ierr);
  ierr = MPI_File_write_all(fh, this->loads.data(), this->loads.size(),
                            MPI_DOUBLE, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);

  // Writing support node array
  ierr = MPI_File_open(this->comm, "supportNodes.bin", MPI_MODE_CREATE |
             MPI_MODE_WRONLY | MPI_MODE_DELETE_ON_CLOSE, MPI_INFO_NULL, &fh);
         CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);
  ierr = MPI_File_open(this->comm, "supportNodes.bin", MPI_MODE_CREATE |
                       MPI_MODE_WRONLY, MPI_INFO_NULL, &fh); CHKERRQ(ierr);
  ierr = MPI_Wait(&suppreq, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  suppdist -= this->suppNode.rows();
  ierr = MPI_File_seek(fh, suppdist*sizeof(PetscInt), MPI_SEEK_SET); CHKERRQ(ierr);
  global_int.resize(this->suppNode.rows(), 1);
  for (int i = 0; i < this->suppNode.size(); i++)
    global_int(i, 0) = this->gNode(this->suppNode(i)); CHKERRQ(ierr);
  ierr = MPI_File_write_all(fh, global_int.data(), global_int.size(),
                            MPI_PETSCINT, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);

  // Writing supports array
  ierr = MPI_File_open(this->comm, "supports.bin", MPI_MODE_CREATE |
             MPI_MODE_WRONLY | MPI_MODE_DELETE_ON_CLOSE, MPI_INFO_NULL, &fh);
         CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);
  ierr = MPI_File_open(this->comm, "supports.bin", MPI_MODE_CREATE |
                       MPI_MODE_WRONLY, MPI_INFO_NULL, &fh); CHKERRQ(ierr);
  ierr = MPI_File_seek(fh, suppdist * this->supports.cols() * sizeof(bool),
                       MPI_SEEK_SET); CHKERRQ(ierr);
  ierr = MPI_File_write_all(fh, this->supports.data(), this->supports.size(),
                            MPI::BOOL, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);

  // Writing eigen analysis support node array
  ierr = MPI_File_open(this->comm, "eigenSupportNodes.bin", MPI_MODE_CREATE |
             MPI_MODE_WRONLY | MPI_MODE_DELETE_ON_CLOSE, MPI_INFO_NULL, &fh);
         CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);
  ierr = MPI_File_open(this->comm, "eigenSupportNodes.bin", MPI_MODE_CREATE |
                       MPI_MODE_WRONLY, MPI_INFO_NULL, &fh); CHKERRQ(ierr);
  ierr = MPI_Wait(&eigsuppreq, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  eigsuppdist -= this->eigenSuppNode.rows();
  ierr = MPI_File_seek(fh, eigsuppdist*sizeof(PetscInt), MPI_SEEK_SET); CHKERRQ(ierr);
  global_int.resize(this->eigenSuppNode.rows(), 1);
  for (int i = 0; i < this->eigenSuppNode.size(); i++)
    global_int(i, 0) = this->gNode(this->eigenSuppNode(i)); CHKERRQ(ierr);
  ierr = MPI_File_write_all(fh, global_int.data(), global_int.size(),
                            MPI_PETSCINT, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);

  // Writing eigen analysis supports array
  ierr = MPI_File_open(this->comm, "eigenSupports.bin", MPI_MODE_CREATE |
             MPI_MODE_WRONLY | MPI_MODE_DELETE_ON_CLOSE, MPI_INFO_NULL, &fh);
         CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);
  ierr = MPI_File_open(this->comm, "eigenSupports.bin", MPI_MODE_CREATE |
                       MPI_MODE_WRONLY, MPI_INFO_NULL, &fh); CHKERRQ(ierr);
  ierr = MPI_File_seek(fh, eigsuppdist * this->eigenSupports.cols() * sizeof(bool),
                       MPI_SEEK_SET); CHKERRQ(ierr);
  ierr = MPI_File_write_all(fh, this->eigenSupports.data(), this->eigenSupports.size(),
                            MPI::BOOL, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);

  // Writing spring node array
  ierr = MPI_File_open(this->comm, "springNodes.bin", MPI_MODE_CREATE |
             MPI_MODE_WRONLY | MPI_MODE_DELETE_ON_CLOSE, MPI_INFO_NULL, &fh);
         CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);
  ierr = MPI_File_open(this->comm, "springNodes.bin", MPI_MODE_CREATE |
                       MPI_MODE_WRONLY, MPI_INFO_NULL, &fh); CHKERRQ(ierr);
  ierr = MPI_Wait(&springreq, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  springdist -= this->springNode.rows();
  ierr = MPI_File_seek(fh, springdist*sizeof(PetscInt), MPI_SEEK_SET); CHKERRQ(ierr);
  global_int.resize(this->springNode.rows(), 1);
  for (int i = 0; i < this->springNode.size(); i++)
    global_int(i, 0) = this->gNode(this->springNode(i));
  ierr = MPI_File_write_all(fh, global_int.data(), global_int.size(),
                            MPI_PETSCINT, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);

  // Writing springs array
  ierr = MPI_File_open(this->comm, "springs.bin", MPI_MODE_CREATE |
             MPI_MODE_WRONLY | MPI_MODE_DELETE_ON_CLOSE, MPI_INFO_NULL, &fh);
         CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);
  ierr = MPI_File_open(this->comm, "springs.bin", MPI_MODE_CREATE |
                       MPI_MODE_WRONLY, MPI_INFO_NULL, &fh); CHKERRQ(ierr);
  ierr = MPI_File_seek(fh, springdist * this->springs.cols() * sizeof(double),
                       MPI_SEEK_SET); CHKERRQ(ierr);
  ierr = MPI_File_write_all(fh, this->springs.data(), this->springs.size(),
                            MPI_DOUBLE, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);

  // Writing mass node array
  ierr = MPI_File_open(this->comm, "massNodes.bin", MPI_MODE_CREATE |
             MPI_MODE_WRONLY | MPI_MODE_DELETE_ON_CLOSE, MPI_INFO_NULL, &fh);
         CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);
  ierr = MPI_File_open(this->comm, "massNodes.bin", MPI_MODE_CREATE |
                       MPI_MODE_WRONLY, MPI_INFO_NULL, &fh); CHKERRQ(ierr);
  ierr = MPI_Wait(&massreq, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  massdist -= this->massNode.rows();
  ierr = MPI_File_seek(fh, massdist*sizeof(PetscInt), MPI_SEEK_SET); CHKERRQ(ierr);
  global_int.resize(this->massNode.rows(), 1);
  for (int i = 0; i < this->massNode.size(); i++)
    global_int(i, 0) = this->gNode(this->massNode(i));
  ierr = MPI_File_write_all(fh, global_int.data(), global_int.size(),
                            MPI_PETSCINT, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);

  // Writing masses array
  ierr = MPI_File_open(this->comm, "masses.bin", MPI_MODE_CREATE |
             MPI_MODE_WRONLY | MPI_MODE_DELETE_ON_CLOSE, MPI_INFO_NULL, &fh);
         CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);
  ierr = MPI_File_open(this->comm, "masses.bin", MPI_MODE_CREATE |
                       MPI_MODE_WRONLY, MPI_INFO_NULL, &fh); CHKERRQ(ierr);
  ierr = MPI_File_seek(fh, massdist*this->masses.cols()*sizeof(double),
                       MPI_SEEK_SET); CHKERRQ(ierr);
  ierr = MPI_File_write_all(fh, this->masses.data(), this->masses.size(),
                            MPI_DOUBLE, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);

  // Writing filter
  PetscViewer view;
  ierr = PetscViewerBinaryOpen(this->comm, "Filter.bin", FILE_MODE_WRITE, &view); CHKERRQ(ierr);
  ierr = MatView(this->P, view); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(&view); CHKERRQ(ierr);

  // Writing max length scale filter
  ierr = PetscViewerBinaryOpen(this->comm, "Max_Filter.bin",
                               FILE_MODE_WRITE, &view); CHKERRQ(ierr);
  ierr = MatView(this->R, view); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(&view); CHKERRQ(ierr);
  ierr = PetscViewerBinaryOpen(this->comm, "Void_Edge_Volume.bin",
                               FILE_MODE_WRITE, &view); CHKERRQ(ierr);
  ierr = VecView(this->REdge, view); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(&view); CHKERRQ(ierr);

  // Writing active element list
  ierr = MPI_File_open(this->comm, "active.bin", MPI_MODE_CREATE |
             MPI_MODE_WRONLY | MPI_MODE_DELETE_ON_CLOSE, MPI_INFO_NULL, &fh);
         CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);
  ierr = MPI_File_open(this->comm, "active.bin", MPI_MODE_CREATE |
                       MPI_MODE_WRONLY, MPI_INFO_NULL, &fh); CHKERRQ(ierr);
  ierr = MPI_File_seek(fh, this->elmdist(myid) * sizeof(bool),
                       MPI_SEEK_SET); CHKERRQ(ierr);
  ierr = MPI_File_write_all(fh, this->active.data(), this->nLocElem,
                            MPI::BOOL, MPI_STATUS_IGNORE); CHKERRQ(ierr);
  ierr = MPI_File_close(&fh); CHKERRQ(ierr);

  // Writing projecting matrices
  ArrayXPI lcol(this->nprocs);
  for (unsigned int i = 0; i < this->PR.size(); i++) {
    stringstream level; level << i;
    string filename = "P" + level.str() + ".bin";
    ierr = PetscViewerBinaryOpen(this->comm, filename.c_str(), FILE_MODE_WRITE, &view); CHKERRQ(ierr);
    ierr = MatView(this->PR[i], view); CHKERRQ(ierr);
    ierr = PetscViewerDestroy(&view); CHKERRQ(ierr);
    ierr = MatGetLocalSize(this->PR[i], NULL, lcol.data()+this->myid); CHKERRQ(ierr);
    MPI_Allgather(MPI_IN_PLACE, 1, MPI_PETSCINT, lcol.data(), 1, MPI_PETSCINT, this->comm);
    if (this->myid == 0) {
      filename += ".split";
      file.open(filename.c_str(), ios::binary);
      file.write((char*)lcol.data(), lcol.size()*sizeof(PetscInt));
      file.close();
    }
  }
  
  return 0;
}

/********************************************************************
 * Print out result of a single step
 * 
 * @param f: Objective value
 * @param cons: Constraint values
 * @param it: Iteration number
 * @param nactive: Number of active elements (not currently used)
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode TopOpt::StepOut(const double &f, const Eigen::VectorXd &cons,
                               int it, long nactive)
{
  PetscErrorCode ierr = 0;

  // Print out values at every step if desired
  if ((print_every - ++last_print) == 0) {
    char name_suffix[30];
    sprintf(name_suffix, "_pen%1.4g_it%i", penal, it);
    ierr = PrintVals(name_suffix); CHKERRQ(ierr);
    last_print = 0;
  }
  // So we print at iteration 10, 20, 30, etc. instead of 9, 19, 29...
  last_print *= (it > 0);

  // Print out total objective and constraint values
  ierr = PetscFPrintf(comm, output, "Iteration number: %u\n", it); CHKERRQ(ierr);
  ierr = PetscFPrintf(comm, output, "Objective: %1.6g\n", f); CHKERRQ(ierr);
  ierr = PetscFPrintf(comm, output, "Constraints:\n"); CHKERRQ(ierr);
  for (short i = 0; i < cons.size(); i++) {
    ierr = PetscFPrintf(comm, output, "\t%1.12g\t", cons(i)); CHKERRQ(ierr);
  }

  // Print out value of each called function
  ierr = PetscFPrintf(comm, output, "\nAll function values:\n"); CHKERRQ(ierr);
  for (unsigned int i = 0; i < function_list.size(); i++) {
    ierr = PetscFPrintf(comm, output, "\t%12s: %1.8g\n",
                        Function_Base::name[function_list[i]->func_type],
                        function_list[i]->Get_Value()); CHKERRQ(ierr);
  }
  ierr = PetscFPrintf(comm, output, "\n"); CHKERRQ(ierr);

  return ierr;
}

/********************************************************************
 * Print out result of a penalization increment
 * 
 * @param it: Iteration number
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode TopOpt::ResultOut(int it)
{
  PetscErrorCode ierr = 0;

  // Output a ratio of stiffness to volume
  PetscScalar Esum, Vsum;
  ierr = VecSum(this->E, &Esum); CHKERRQ(ierr);
  ierr = VecSum(this->V, &Vsum); CHKERRQ(ierr);
  ierr = PetscFPrintf(this->comm, this->output, "********************************"
         "****************\nAfter %4i iterations with a penalty of %1.4g the\n"
         "ratio of stiffness sum to volume sum is %1.4g\n" "*********************"
         "***************************\n\n", it, this->penal, Esum/Vsum);
         CHKERRQ(ierr);

  char name_suffix[30];
  sprintf(name_suffix, "_pen%1.4g", this->penal);
  ierr = PrintVals(name_suffix); CHKERRQ(ierr);
  last_print = 0;

  return ierr;
}

/********************************************************************
 * The actual printing of the optimization state
 * 
 * @param name_suffix: Characters to append to the output file names
 * 
 * @return ierr: PetscErrorCode
 * 
 *******************************************************************/
PetscErrorCode TopOpt::PrintVals(char *name_suffix)
{
  PetscErrorCode ierr = 0;
  char filename[30];
  PetscViewer output;

  sprintf(filename, "U%s.bin", name_suffix);
  ierr = PetscViewerBinaryOpen(this->comm, filename,
      FILE_MODE_WRITE, &output); CHKERRQ(ierr);
  ierr = VecView(this->U, output); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(&output); CHKERRQ(ierr);

  sprintf(filename, "x%s.bin", name_suffix);
  ierr = PetscViewerBinaryOpen(this->comm, filename,
      FILE_MODE_WRITE, &output); CHKERRQ(ierr);
  ierr = VecView(this->x, output); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(&output); CHKERRQ(ierr);

  sprintf(filename, "V%s.bin", name_suffix);
  ierr = PetscViewerBinaryOpen(this->comm, filename,
      FILE_MODE_WRITE, &output); CHKERRQ(ierr);
  ierr = VecView(this->V, output); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(&output); CHKERRQ(ierr);

  sprintf(filename, "Es%s.bin", name_suffix);
  ierr = PetscViewerBinaryOpen(this->comm, filename,
      FILE_MODE_WRITE, &output); CHKERRQ(ierr);
  ierr = VecView(this->Es, output); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(&output); CHKERRQ(ierr);

  sprintf(filename, "E%s.bin", name_suffix);
  ierr = PetscViewerBinaryOpen(this->comm, filename,
      FILE_MODE_WRITE, &output); CHKERRQ(ierr);
  ierr = VecView(this->E, output); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(&output); CHKERRQ(ierr);

  for (int i = 0; i < this->bucklingShape.cols(); i++) {
    sprintf(filename,"phiB%s_mode%i.bin", name_suffix, i);
    Vec phi;
    ierr = VecCreateMPIWithArray(this->comm, 1, this->numDims*this->nLocNode,
        this->numDims*this->nNode, this->bucklingShape.data() +
        this->bucklingShape.rows()*i, &phi); CHKERRQ(ierr);
    ierr = PetscViewerBinaryOpen(this->comm, filename,
        FILE_MODE_WRITE, &output); CHKERRQ(ierr);
    ierr = VecView(phi, output); CHKERRQ(ierr);
    ierr = PetscViewerDestroy(&output); CHKERRQ(ierr);
    ierr = VecDestroy(&phi); CHKERRQ(ierr);
  }

  for (int i = 0; i < this->dynamicShape.cols(); i++) {
    sprintf(filename,"phiD_%s_mode%i.bin", name_suffix, i);
    Vec phi;
    ierr = VecCreateMPIWithArray(this->comm, 1, this->numDims*this->nLocNode,
        this->numDims*this->nNode, this->dynamicShape.data() +
        this->dynamicShape.rows()*i, &phi); CHKERRQ(ierr);
    ierr = PetscViewerBinaryOpen(this->comm, filename,
        FILE_MODE_WRITE, &output); CHKERRQ(ierr);
    ierr = VecView(phi, output); CHKERRQ(ierr);
    ierr = PetscViewerDestroy(&output); CHKERRQ(ierr);
    ierr = VecDestroy(&phi); CHKERRQ(ierr);
  }

  return ierr;
}
