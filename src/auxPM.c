//==========================================================================//
//  Copyright (c) 2013       Cullan Howlett & Marc Manera,                  //
//                           Institute of Cosmology and Gravitation,        //
//                           University of Portsmouth.                      //
//                                                                          //
//  MG-PICOLA written by Hans Winther (ICG Portsmouth) March 2017           //
//                                                                          //
//  This file is part of PICOLA.                                            //
//                                                                          //
//  PICOLA is free software: you can redistribute it and/or modify          //
//  it under the terms of the GNU General Public License as published by    //
//  the Free Software Foundation, either version 3 of the License, or       //
//  (at your option) any later version.                                     //
//                                                                          //
//  PICOLA is distributed in the hope that it will be useful,               //
//  but WITHOUT ANY WARRANTY; without even the implied warranty of          //
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           //
//  GNU General Public License for more details.                            //
//                                                                          //
//  You should have received a copy of the GNU General Public License       //
//  along with PICOLA.  If not, see <http://www.gnu.org/licenses/>.         //
//==========================================================================//

//=========================================================================================//
//This file contains some additional routines (parallel and serial) needed for any PM code //
//=========================================================================================//

#include "vars.h"
#include "proto.h"
#include "msg.h"
#include "timer.h"
#include "mg.h"            

//=================================================================
// A master routine called from main.c to calculate the acceleration
//=================================================================
void GetDisplacements(void) {

  //=================================================================
  // First we check whether all the particles are on the correct processor after the last time step/
  // original 2LPT displacement and move them if not
  //=================================================================
  if (ThisTask == 0) printf("Moving particles across task boundaries...\n");
  MoveParticles();

#ifdef MEMORY_MODE
  density = malloc(2*Total_size*sizeof(float_kind));
  P3D     = (complex_kind *) density;
  plan    = my_fftw_mpi_plan_dft_r2c_3d(Nmesh, Nmesh, Nmesh, density, P3D, MPI_COMM_WORLD, FFTW_ESTIMATE);
  if(modified_gravity_active) AllocateMGArrays();
#endif

  //=================================================================
  // Then we do the Cloud-in-Cell assignment to get the density grid and FFT it.  
  //=================================================================
  if (ThisTask == 0) printf("Calculating density using Cloud-in-Cell...\n");
  PtoMesh();

  if(modified_gravity_active) ComputeFifthForce();

#ifdef MEMORY_MODE
  N11  = malloc( 2 * Total_size * sizeof(float_kind));
  N12  = malloc( 2 * Total_size * sizeof(float_kind));
  N13  = malloc( 2 * Total_size * sizeof(float_kind));
  FN11 = (complex_kind*) N11;
  FN12 = (complex_kind*) N12;
  FN13 = (complex_kind*) N13;
  p11  = my_fftw_mpi_plan_dft_c2r_3d(Nmesh, Nmesh, Nmesh, FN11, N11, MPI_COMM_WORLD, FFTW_ESTIMATE);
  p12  = my_fftw_mpi_plan_dft_c2r_3d(Nmesh, Nmesh, Nmesh, FN12, N12, MPI_COMM_WORLD, FFTW_ESTIMATE);
  p13  = my_fftw_mpi_plan_dft_c2r_3d(Nmesh, Nmesh, Nmesh, FN13, N13, MPI_COMM_WORLD, FFTW_ESTIMATE);
#endif

  //=================================================================
  // This returns N11,N12,N13 which hold the components of
  // the vector (grad grad^{-2} density) on a grid.
  //=================================================================
  if (ThisTask == 0) printf("Calculating forces...\n");
  Forces();

#ifdef MEMORY_MODE
  free(density);
  my_fftw_destroy_plan(plan);
  if(modified_gravity_active) FreeMGArrays();
  for (int j = 0; j < 3; j++) Disp[j] = malloc(NumPart * sizeof(float));
#else
  for (int j = 0; j < 3; j++) Disp[j] = malloc(NumPart * sizeof(float_kind));
#endif

  //=================================================================
  // Now find the accelerations at the particle positions using 3-linear interpolation. 
  //=================================================================
  if (ThisTask == 0) printf("Calculating accelerations...\n");
  MtoParticles();

#ifdef MEMORY_MODE
  free(N11);
  free(N12);
  free(N13);  
  my_fftw_destroy_plan(p11);
  my_fftw_destroy_plan(p12);
  my_fftw_destroy_plan(p13);
#endif
}

//==============================================================================================
// A routine to check whether all the particles are on the correct processor and move them if not.
//==============================================================================================
void MoveParticles(void) {
  timer_start(_MoveParticles);

  //==============================================================================================
  // Note that there are some subtleties in this routine that deal with the fact in some instances there
  // may be no particles on the last N tasks depending on how the work is partioned, hence we need to 
  // skip over these tasks and copy to the correct ones. We include subtleties that deal with the fact that
  // a task may have no particles by skipping over them from the other tasks perspective and
  // setting any sendrecv commands on these tasks to null
  //==============================================================================================

  int X;
  int j;
  int neighbour, neighbour_left, neighbour_right, neighbour_count = 0;
  int send_count_max = (int)(ceil(Local_np*Nsample*Nsample*(Buffer-1.0)));
  int send_count_left = 0, send_count_right = 0;
  int recv_count_left = 0, recv_count_right = 0;
  int procdiff_left, procdiff_right, procdiffmax = 1, procdiffmaxglob = 1;
  unsigned int i;
  double scaleBox=(double)Nmesh/Box;

  //==============================================================================================
  // We assume that at least one send is needed and calculate the true number of sends needed in the first iteration.
  // (Yes, i know we shouldn't really modify the iteration counter inside the loop but it creates a good algorithm both here and
  // when we assign the particles to be copied) 
  //==============================================================================================
  for (j = 1; j <= procdiffmaxglob; j++) {

    //==============================================================================================
    // Allocate memory to hold the particles to be transfered. We assume a maximum of Local_np*Nsample*Nsample*(buffer-1.0).
    //==============================================================================================
    struct part_data * P_send_left  = (struct part_data *)malloc(send_count_max*sizeof(struct part_data));
    struct part_data * P_send_right = (struct part_data *)malloc(send_count_max*sizeof(struct part_data));

    //==============================================================================================
    // The main purpose here is to calculate how many sendrecvs we need to perform (i.e., the maximum number 
    // of tasks a particle has moved across). However, we also assume that at least one send is needed 
    // and so set up the particles to be transferred to the neighbouring tasks
    //==============================================================================================
    send_count_left = 0; send_count_right = 0;
    recv_count_left = 0; recv_count_right = 0;
    if (j <= procdiffmax) {
      for (i = 0; i < NumPart; i++) {
        X = (int)(P[i].Pos[0]*scaleBox);
        procdiff_left = 0; procdiff_right = 0;
        if (Slab_to_task[X] != ThisTask) {
          neighbour = ThisTask;
          do {
            procdiff_left++;
            neighbour--;
            if (neighbour < 0) neighbour += NTask;
            if (Local_np_table[neighbour] == 0) procdiff_left--;
          } while(Slab_to_task[X] != neighbour);
          neighbour = ThisTask;
          do {
            procdiff_right++;
            neighbour++;
            if (neighbour >= NTask) neighbour -= NTask;
            if (Local_np_table[neighbour] == 0) procdiff_right--;
          } while(Slab_to_task[X] != neighbour);
          if ((procdiff_left != 0) || (procdiff_right != 0)) {
            if (procdiff_left <= procdiff_right) {
              if (j == 1) {
                if (procdiff_left > procdiffmax) procdiffmax = procdiff_left;
              }
              if (procdiff_left == j) {
                P_send_left[send_count_left] = P[i];
                P[i] = P[NumPart-1];
                i--; NumPart--;
                send_count_left++;
                if (send_count_left >= send_count_max) {
                  printf("\nERROR: Number of particles to be sent left on task %d is greater than send_count_max\n", ThisTask);
                  printf("       You must increase the size of the buffer region.\n\n");
                 FatalError((char *)"auxPM.c", 219);
                }
              }
            } else {
              if (j == 1) {
                if (procdiff_right > procdiffmax) procdiffmax = procdiff_right;
              }
              if (procdiff_right == j) {
                P_send_right[send_count_right] = P[i];
                P[i] = P[NumPart-1];
                i--; NumPart--;
                send_count_right++;
                if (send_count_right >= send_count_max) {
                  printf("\nERROR: Number of particles to be sent right on task %d is greater than send_count_max\n", ThisTask);
                  printf("       You must increase the size of the buffer region.\n\n");
                  FatalError((char *)"auxPM.c", 234);
                }
              }
            }
          }
        }
      }
    } 

    //==============================================================================================
    // If we have to send to non-adjoining tasks then we have to recompute the neighbour's task number. For adjoining tasks 
    // we have already got these in the variables LeftTask and RightTask which are also used elsewhere
    //==============================================================================================
    if (j == 1) {
      neighbour_left = LeftTask;
      neighbour_right = RightTask;      
      ierr = MPI_Allreduce(&procdiffmax, &procdiffmaxglob, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
      if (ThisTask == 0) printf("Need to transfer particles %d times...\n", procdiffmaxglob);
    } else {
      if (Local_np == 0) {
        neighbour_left = MPI_PROC_NULL;
        neighbour_right = MPI_PROC_NULL;
      } else {

        neighbour_count = 0;
        neighbour_left = ThisTask;
        do {
          neighbour_left--;
          neighbour_count++;
          if(neighbour_left < 0) neighbour_left += NTask;
          if(Local_np_table[neighbour_left] == 0) neighbour_count--;
        } while(neighbour_count != j);

        neighbour_count = 0;
        neighbour_right = ThisTask;
        do {
          neighbour_right++;
          neighbour_count++;
          if(neighbour_right >= NTask) neighbour_right -= NTask;
          if(Local_np_table[neighbour_right] == 0) neighbour_count--;
        } while(neighbour_count != j);
      }
    }

    ierr = MPI_Sendrecv(&send_count_left, 1,MPI_INT,neighbour_left, 0,&recv_count_right,1,MPI_INT,neighbour_right,0,MPI_COMM_WORLD,&status);
    ierr = MPI_Sendrecv(&send_count_right,1,MPI_INT,neighbour_right,0,&recv_count_left, 1,MPI_INT,neighbour_left, 0,MPI_COMM_WORLD,&status);

    if (NumPart+recv_count_left+recv_count_right > Local_np*Nsample*Nsample*Buffer) {
      printf("\nERROR: Number of particles to be recieved on task %d is greater than available space\n", ThisTask);
      printf("       You must increase the size of the buffer region.\n\n");
      FatalError((char *)"auxPM.c", 282);
    }

    //==============================================================================================
    // Copy across the new particles and store them at the end (of the memory). Then modify NumPart to include them.
    //==============================================================================================
    ierr = MPI_Sendrecv(&(P_send_left[0]),send_count_left*sizeof(struct part_data),MPI_BYTE,neighbour_left,0,
        &(P[NumPart]),recv_count_right*sizeof(struct part_data),MPI_BYTE,neighbour_right,0,MPI_COMM_WORLD,&status);
    ierr = MPI_Sendrecv(&(P_send_right[0]),send_count_right*sizeof(struct part_data),MPI_BYTE,neighbour_right,0,
        &(P[NumPart+recv_count_right]),recv_count_left*sizeof(struct part_data),MPI_BYTE,neighbour_left,0,MPI_COMM_WORLD,&status);

    NumPart += (recv_count_left+recv_count_right);

    free(P_send_left);
    free(P_send_right);
  }
  
  timer_stop(_MoveParticles);
  return;  
}

//==============================
// Does Cloud-in-Cell assignment.
//==============================
void PtoMesh(void) {
  timer_start(_PtoMesh);
  unsigned int i;
  unsigned int IX, IY, IZ;
  unsigned int IXneigh, IYneigh, IZneigh;
  double X, Y, Z;
  double TX, TY, TZ;
  double DX, DY, DZ;
  double scaleBox = (double)Nmesh/Box;
  double WPAR = pow((double)Nmesh / (double)Nsample,3);

  // Initialize density to -1
  for(i = 0; i < 2 * Total_size; i++) 
    density[i] = -1.0;
  
  for(i = 0; i < NumPart; i++) {

    // Scale positions to be in [0, Nmesh]
    X = P[i].Pos[0] * scaleBox;
    Y = P[i].Pos[1] * scaleBox;
    Z = P[i].Pos[2] * scaleBox;

    // Grid-index for cell containing particle
    IX = (unsigned int)X;
    IY = (unsigned int)Y;
    IZ = (unsigned int)Z;

    // Coordinate distances to center of cell
    DX = X-(double)IX;
    DY = Y-(double)IY;
    DZ = Z-(double)IZ;
   
    // CIC weights
    TX = 1.0 - DX;
    TY = 1.0 - DY;
    TZ = 1.0 - DZ;
    DY *= WPAR;
    TY *= WPAR;

    // Periodic BC
    IX -= Local_x_start;
    if(IY >= (unsigned int)Nmesh) IY = 0;
    if(IZ >= (unsigned int)Nmesh) IZ = 0;

    // Neighbor gridindex
    // No check for x as we have an additional slice on the right
    IXneigh = IX + 1;
    IYneigh = IY + 1;
    IZneigh = IZ + 1;
    if(IYneigh >= (unsigned int)Nmesh) IYneigh = 0;
    if(IZneigh >= (unsigned int)Nmesh) IZneigh = 0;

    //====================================================================================
    // Assign density to the 8 cells containing the particle cloud
    //====================================================================================
    density[(IX*Nmesh+IY)*2*(Nmesh/2+1)+IZ]                += TX*TY*TZ;
    density[(IX*Nmesh+IY)*2*(Nmesh/2+1)+IZneigh]           += TX*TY*DZ;
    density[(IX*Nmesh+IYneigh)*2*(Nmesh/2+1)+IZ]           += TX*DY*TZ;
    density[(IX*Nmesh+IYneigh)*2*(Nmesh/2+1)+IZneigh]      += TX*DY*DZ;
    density[(IXneigh*Nmesh+IY)*2*(Nmesh/2+1)+IZ]           += DX*TY*TZ;
    density[(IXneigh*Nmesh+IY)*2*(Nmesh/2+1)+IZneigh]      += DX*TY*DZ;
    density[(IXneigh*Nmesh+IYneigh)*2*(Nmesh/2+1)+IZ]      += DX*DY*TZ;
    density[(IXneigh*Nmesh+IYneigh)*2*(Nmesh/2+1)+IZneigh] += DX*DY*DZ;
  }

  //====================================================================================
  // Copy across the extra slice from the task on the left and add it to the leftmost slice
  // of the task on the right. Skip over tasks without any slices.
  //====================================================================================
  
  float_kind * temp_density = (float_kind *)calloc(2*alloc_slice,sizeof(float_kind));
  ierr = MPI_Sendrecv(&(density[2*last_slice]),2*alloc_slice*sizeof(float_kind),MPI_BYTE,RightTask,0,
      &(temp_density[0]),2*alloc_slice*sizeof(float_kind),MPI_BYTE,LeftTask,0,MPI_COMM_WORLD,&status);
  if (NumPart != 0) {
    for (i = 0; i < 2 * alloc_slice; i++) density[i] += (temp_density[i] + 1.0);
  }
  free(temp_density);

  //====================================================================================
  // If modified gravity is active We take a copy of the density array 
  // needed to compute the fifth-force
  //====================================================================================
  if(modified_gravity_active) CopyDensityArray();

  // FFT the density field
  my_fftw_execute(plan);

  // For testing. Compute P(k) every time-step
  // As currently written this is P(k) in the co-moving frame
  // compute_power_spectrum(P3D);

  timer_stop(_PtoMesh);
  return;
}

//===========================================
// Calculate the force grids from the density.
//===========================================
void Forces(void) {
  timer_start(_Forces);
  int dd[3];
  double RK, KK, grid_corr;
  double Scale = 2. * M_PI / Box;
  complex_kind dens;

  //==========================================================
  // We need global values for i as opposed to local values
  // Same goes for anything that relies on i (such as RK). 
  //==========================================================
  for (unsigned int i = 0; i < Local_nx; i++) {
    int iglobal = i + Local_x_start;
    for (unsigned int j = 0; j < (unsigned int)(Nmesh/2+1); j++) {
      int kmin = 0;
      if ((iglobal == 0) && (j == 0)) {
        // Set the k = (0,0,0) mode
        FN11[0][0] = 0.0; FN11[0][1] = 0.0;
        FN12[0][0] = 0.0; FN12[0][1] = 0.0;
        FN13[0][0] = 0.0; FN13[0][1] = 0.0;
        kmin = 1;
      }
      for (unsigned int k = kmin; k < (unsigned int)(Nmesh/2+1); k++) {

        //==========================================================
        // Compute k_vec = (kx,ky,kz) and RK = |k_vec|^2
        //==========================================================
        unsigned int ind = (i*Nmesh + j)*(Nmesh/2+1) + k;
        dd[0] = iglobal > Nmesh/2 ? iglobal-Nmesh : iglobal;
        dd[1] = j;
        dd[2] = k;
        RK    = dd[0]*dd[0] + dd[1]*dd[1] + dd[2]*dd[2];
        KK    = -1.0/RK;

        //==========================================================
        // Deconvolve the CIC window function twice (once for density, once for force interpolation)
        // and add gaussian smoothing if requested
        //==========================================================
        grid_corr = 1.0;
        for(int axes = 0; axes < 3; axes++)
          if(dd[axes] != 0) grid_corr *= sin( (PI*dd[axes]) / (double)Nmesh )/( (PI*dd[axes]) / (double)Nmesh);
        grid_corr = pow(1.0 / grid_corr, 4.0);
        grid_corr = 1.0;

        //==========================================================
        // Newtonian potential
        //==========================================================
        dens[0] = (     P3D[ind][0]*KK*grid_corr)/pow((double)Nmesh,3);
        dens[1] = (-1.0*P3D[ind][1]*KK*grid_corr)/pow((double)Nmesh,3);

        //==========================================================
        // Add fifth-force potential
        //==========================================================
        if(modified_gravity_active){
          dens[0] += (     P3D_mgarray_two[ind][0]*KK*grid_corr)/pow((double)Nmesh,3);
          dens[1] += (-1.0*P3D_mgarray_two[ind][1]*KK*grid_corr)/pow((double)Nmesh,3);
        }

        //==========================================================
        // dens now holds the total potential so we can solve for the force. 
        //==========================================================
        FN11[ind][0] = dens[1] * dd[0] / Scale;
        FN11[ind][1] = dens[0] * dd[0] / Scale;
        FN12[ind][0] = dens[1] * dd[1] / Scale;
        FN12[ind][1] = dens[0] * dd[1] / Scale;
        FN13[ind][0] = dens[1] * dd[2] / Scale;
        FN13[ind][1] = dens[0] * dd[2] / Scale;

        //==========================================================
        // Do the mirror force along the y axis
        //==========================================================
        if ((j != (unsigned int)(Nmesh/2)) && (j != 0)) {
          int ind = (i*Nmesh + (Nmesh-j))*(Nmesh/2+1) + k;
          dd[1] = -j;

          //==========================================================
          // Newtonian potential
          //==========================================================
          dens[0] = (     P3D[ind][0]*KK*grid_corr)/pow((double)Nmesh,3) ;
          dens[1] = (-1.0*P3D[ind][1]*KK*grid_corr)/pow((double)Nmesh,3) ;

          //==========================================================
          // Add fifth-force potential
          //==========================================================
          if(modified_gravity_active){
            dens[0] += (     P3D_mgarray_two[ind][0]*KK*grid_corr)/pow((double)Nmesh,3);
            dens[1] += (-1.0*P3D_mgarray_two[ind][1]*KK*grid_corr)/pow((double)Nmesh,3);
          }

          //==========================================================
          // dens now holds the total potential so we can solve for the force. 
          //==========================================================
          FN11[ind][0] = dens[1] * dd[0] / Scale;
          FN11[ind][1] = dens[0] * dd[0] / Scale;
          FN12[ind][0] = dens[1] * dd[1] / Scale;
          FN12[ind][1] = dens[0] * dd[1] / Scale;
          FN13[ind][0] = dens[1] * dd[2] / Scale;
          FN13[ind][1] = dens[0] * dd[2] / Scale;
        }
      }
    }
  }

  // Perform FFTs
  my_fftw_execute(p11);
  my_fftw_execute(p12);
  my_fftw_execute(p13);

  //============================================================================
  // Copy across the extra slice from the process on the right and save it at the 
  // end of the force array. Skip over tasks without any slices.
  //============================================================================
  ierr = MPI_Sendrecv(&(N11[0]), 2*alloc_slice*sizeof(float_kind), MPI_BYTE, LeftTask, 0,
                      &(N11[2*last_slice]), 2*alloc_slice*sizeof(float_kind),MPI_BYTE,RightTask, 0, MPI_COMM_WORLD, &status);
  ierr = MPI_Sendrecv(&(N12[0]), 2*alloc_slice*sizeof(float_kind), MPI_BYTE, LeftTask, 0,
                      &(N12[2*last_slice]), 2*alloc_slice*sizeof(float_kind),MPI_BYTE,RightTask, 0, MPI_COMM_WORLD, &status);
  ierr = MPI_Sendrecv(&(N13[0]), 2*alloc_slice*sizeof(float_kind), MPI_BYTE, LeftTask, 0,
                      &(N13[2*last_slice]), 2*alloc_slice*sizeof(float_kind),MPI_BYTE,RightTask, 0, MPI_COMM_WORLD, &status);

  timer_stop(_Forces);
  return;
}

//===========================
// Does 3-linear interpolation
//===========================
void MtoParticles(void) {
  timer_start(_MtoParticles);
  unsigned int i;
  unsigned int IX,IY,IZ;
  unsigned int IXneigh,IYneigh,IZneigh;
  double X,Y,Z;
  double TX,TY,TZ;
  double DX,DY,DZ;
  double scaleBox = (double)Nmesh/Box;
  double WPAR = 1;

  for(int axes = 0; axes < 3; axes++)
    sumDxyz[axes] = 0;

  for(i = 0; i < NumPart; i++) {

    X = P[i].Pos[0] * scaleBox;
    Y = P[i].Pos[1] * scaleBox;
    Z = P[i].Pos[2] * scaleBox;

    IX = (unsigned int) X;
    IY = (unsigned int) Y;
    IZ = (unsigned int) Z;
   
    DX = X - (double) IX;
    DY = Y - (double) IY;
    DZ = Z - (double) IZ;
    
    TX = 1.0 - DX;
    TY = 1.0 - DY;
    TZ = 1.0 - DZ;

    DY *= WPAR;
    TY *= WPAR;

    IX -= Local_x_start;
    if(IY >= (unsigned int)Nmesh) IY = 0;
    if(IZ >= (unsigned int)Nmesh) IZ = 0;

    IXneigh = IX + 1;
    IYneigh = IY + 1;
    IZneigh = IZ + 1;
    if(IYneigh >= (unsigned int)Nmesh) IYneigh = 0;
    if(IZneigh >= (unsigned int)Nmesh) IZneigh = 0;

    Disp[0][i] = N11[(IX*Nmesh+IY)*2*(Nmesh/2+1)+IZ]               *TX*TY*TZ +
                 N11[(IX*Nmesh+IY)*2*(Nmesh/2+1)+IZneigh]          *TX*TY*DZ +
                 N11[(IX*Nmesh+IYneigh)*2*(Nmesh/2+1)+IZ]          *TX*DY*TZ +
                 N11[(IX*Nmesh+IYneigh)*2*(Nmesh/2+1)+IZneigh]     *TX*DY*DZ +
                 N11[(IXneigh*Nmesh+IY)*2*(Nmesh/2+1)+IZ]          *DX*TY*TZ +
                 N11[(IXneigh*Nmesh+IY)*2*(Nmesh/2+1)+IZneigh]     *DX*TY*DZ +
                 N11[(IXneigh*Nmesh+IYneigh)*2*(Nmesh/2+1)+IZ]     *DX*DY*TZ +
                 N11[(IXneigh*Nmesh+IYneigh)*2*(Nmesh/2+1)+IZneigh]*DX*DY*DZ;

    Disp[1][i] = N12[(IX*Nmesh+IY)*2*(Nmesh/2+1)+IZ]               *TX*TY*TZ +
                 N12[(IX*Nmesh+IY)*2*(Nmesh/2+1)+IZneigh]          *TX*TY*DZ +
                 N12[(IX*Nmesh+IYneigh)*2*(Nmesh/2+1)+IZ]          *TX*DY*TZ +
                 N12[(IX*Nmesh+IYneigh)*2*(Nmesh/2+1)+IZneigh]     *TX*DY*DZ +
                 N12[(IXneigh*Nmesh+IY)*2*(Nmesh/2+1)+IZ]          *DX*TY*TZ +
                 N12[(IXneigh*Nmesh+IY)*2*(Nmesh/2+1)+IZneigh]     *DX*TY*DZ +
                 N12[(IXneigh*Nmesh+IYneigh)*2*(Nmesh/2+1)+IZ]     *DX*DY*TZ +
                 N12[(IXneigh*Nmesh+IYneigh)*2*(Nmesh/2+1)+IZneigh]*DX*DY*DZ;

    Disp[2][i] = N13[(IX*Nmesh+IY)*2*(Nmesh/2+1)+IZ]               *TX*TY*TZ +
                 N13[(IX*Nmesh+IY)*2*(Nmesh/2+1)+IZneigh]          *TX*TY*DZ +
                 N13[(IX*Nmesh+IYneigh)*2*(Nmesh/2+1)+IZ]          *TX*DY*TZ +
                 N13[(IX*Nmesh+IYneigh)*2*(Nmesh/2+1)+IZneigh]     *TX*DY*DZ +
                 N13[(IXneigh*Nmesh+IY)*2*(Nmesh/2+1)+IZ]          *DX*TY*TZ +
                 N13[(IXneigh*Nmesh+IY)*2*(Nmesh/2+1)+IZneigh]     *DX*TY*DZ +
                 N13[(IXneigh*Nmesh+IYneigh)*2*(Nmesh/2+1)+IZ]     *DX*DY*TZ +
                 N13[(IXneigh*Nmesh+IYneigh)*2*(Nmesh/2+1)+IZneigh]*DX*DY*DZ;

    for(int axes = 0; axes < 3; axes++)
      sumDxyz[axes] += Disp[axes][i];
  }

  // Make sumDx, sumDy and sumDz global averages
  for(int axes = 0; axes < 3; axes++){
    ierr = MPI_Allreduce(MPI_IN_PLACE, &(sumDxyz[axes]), 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    sumDxyz[axes] /= (double) TotNumPart;
  }

  timer_stop(_MtoParticles);
  return;      
}

//===============================
// Wrap the particles periodically
//===============================
#if (MEMORY_MODE || SINGLE_PRECISION)
float periodic_wrap(float x){
  while(x >= (float)Box) x -= (float)Box;
  while(x < 0) x += (float)Box;
  if (x == (float)Box) x = 0.0;
  return x;
}
#else
double periodic_wrap(double x){
  while(x >= Box) x -= Box;
  while(x < 0) x += Box;
  if (x == Box) x = 0.0;
  return x;
}
#endif

//===============
// Error message
//===============
void FatalError(char* filename, int linenum) {
  printf("Fatal Error at line %d in file %s\n", linenum, filename);
  fflush(stdout);
  free(OutputList);
  MPI_Abort(MPI_COMM_WORLD, 1);
  exit(1);
}

//===========================================================================
// This catches I/O errors occuring for fwrite(). In this case we better stop.
//===========================================================================
size_t my_fwrite(void *ptr, size_t size, size_t nmemb, FILE * stream) {
  size_t nwritten;
  if((nwritten = fwrite(ptr, size, nmemb, stream)) != nmemb) {
    printf("\nERROR: I/O error (fwrite) on task=%d has occured.\n\n", ThisTask);
    fflush(stdout);
    FatalError((char *)"auxPM.c", 621);
  }
  return nwritten;
}

//===========================================================================
// For testing compute P(k) = <|density(k)|^2>
// As currently written this is P(k) in the co-moving frame
//===========================================================================
void compute_power_spectrum(complex_kind *P3D){
  int nbins = Nmesh;
  double *pofk_bin     = malloc(sizeof(double) * nbins);
  double *pofk_bin_all = malloc(sizeof(double) * nbins);
  double *n_bin        = malloc(sizeof(double) * nbins);
  double *n_bin_all    = malloc(sizeof(double) * nbins);

  // FFT normalization factor for |density(k)|^2
  double fac = 1.0/pow((double) Nmesh, 6);

  for(int i = 0; i < nbins; i++)
    pofk_bin[i] = pofk_bin_all[i] = n_bin[i] = n_bin_all[i] = 0.0;

  for (int i = 0; i < Local_nx; i++) {
    int iglobal = i + Local_x_start;
    for (int j = 0 ; j < (unsigned int)(Nmesh/2+1); j++) {
      for (int k = 0; k < (unsigned int) (Nmesh/2+1); k++) {
        unsigned int coord = (i*Nmesh+j)*(Nmesh/2+1)+k;

        // Compute k-vector and its norm
        double d[3] = {iglobal > Nmesh/2 ? iglobal-Nmesh : iglobal, j, k};
        double kmag2_int = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        double kmag_int = sqrt(kmag2_int);

        // Deconvolve window function (CIC)
        double grid_corr = 1.0;
        for(int axes = 0; axes < 3; axes++){
          if (d[axes] != 0) grid_corr *= sin((PI*d[axes])/(double)Nmesh)/((PI*d[axes])/(double)Nmesh);
        }
        grid_corr = pow(1.0 / grid_corr, 4.0) * fac;

        // Add to bins
        double pofk = (P3D[coord][0] * P3D[coord][0] + P3D[coord][1] * P3D[coord][1]) * grid_corr;
        int nk = (int) (kmag_int + 0.5); 
        if( nk  < nbins && nk > 0 ){
          pofk_bin[nk] += pofk;
          n_bin[nk]    += 1.0;
        }

        // Add the mirror along the y-axis
        if ((j != (unsigned int)(Nmesh/2)) && (j != 0)) {
          coord = (i*Nmesh + (Nmesh-j))*(Nmesh/2+1)+k;
         
          // Add to bins
          if( nk  < nbins && nk > 0 ){
            pofk = (P3D[coord][0] * P3D[coord][0] + P3D[coord][1] * P3D[coord][1]) * grid_corr;
            pofk_bin[nk] += pofk;
            n_bin[nk]    += 1.0;
          }
        }
      }
    }
  }

  // Communicate
  MPI_Allreduce(pofk_bin, pofk_bin_all, nbins, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(n_bin,    n_bin_all,    nbins, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

  if(ThisTask == 0){
    printf("Output P(k) at a = %f \n", aexp_global);
    for(int i = 1; i < nbins; i++){
      if(n_bin_all[i] > 0){
        pofk_bin[i] = pofk_bin_all[i] / n_bin_all[i] - 1.0/pow((double) Nsample, 3);
      }
      printf("%8.3f   %8.3f\n", i * 2 * M_PI / Box, pofk_bin[i] * pow( Box, 3 ) );
    }
  }

  // Free memory
  free(pofk_bin);
  free(pofk_bin_all);
  free(n_bin);
  free(n_bin_all);
}
