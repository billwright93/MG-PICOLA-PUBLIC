#include "msg.h"
#include "timer.h"

//==================================================================
//
// MG-PICOLA written by Hans Winther (ICG Portsmouth) March 2017            
//
// This file contains methods we need to read particle files
// and use this to set the IC and reconstruct the displacement-field
// from this. Currently we have implemented RAMSES, GADGET and ASCII formats.
//
// NB: GAUSSIAN, SHARPK or TOPHAT  needs to be defined
// 
// NB: We assume that the IC are such that LCDM and MG have the same IC
// at the initial redshift. Thus we use the LCDM growth-factor to bring the
// displacmentfield to redshift 0 here. In main.c after assigning particles
// (if UseCOLA == 1) we rescale the displacement-field to make it the MG
// displacmentfield again
//
// Activated if ReadParticlesFromFile == 1
//
// -----------------------------------------------------------------
// For the parameter file we need to add:
// -----------------------------------------------------------------
// NumInputParticleFiles   2                  % Number of input-files
// InputParticleFileDir    /path/output_00001 % Directory containing files
// InputParticleFilePrefix part               % Fileprefix in /path/fileprefix.X with X = 1,2,..,N. 
//                                            % Not used for RAMSES
// RamsesOutputNumber      1                  % The X in [part_0000X.out00001].
//                                            % Not used for ASCII and GADGET
// TypeInputParticleFiles  1                  % RAMSESFILE = 1 and ASCIIFILE = 2 and GADGETFILE = 3
// -----------------------------------------------------------------
//
// For ASCII files we assume the format [numpart; X1 Y1 Z1 mass; X2 Y2 Z2 mass; ... ]
// where X,Y,Z are in [0,1]
//
// -----------------------------------------------------------------
// Code needed to add to [main.c]:
// -----------------------------------------------------------------
// if(ReadParticlesFromFile)
//  ReadFilesMakeDisplacementField();
// else
//  displacement_fields();
// }
// 
// ...
// 
// -----------------------------------------------------------------
// Code needed to add to [2LPC.c]:
// -----------------------------------------------------------------
// if(ReadParticlesFromFile){
//   AssignDisplacementField(cdisp);
// else
//   ... loop containing standard IC generation ...
//
// -----------------------------------------------------------------
// Code needed to add to [read_param.c]:
// -----------------------------------------------------------------
// if(ReadParticlesFromFile){
//  strcpy(tag[nt], "InputParticleFileDir");
//  addr[nt] = InputParticleFileDir;
//  id[nt++] = STRING;
//  
//  strcpy(tag[nt], "InputParticleFilePrefix");
//  addr[nt] = InputParticleFilePrefix;
//  id[nt++] = STRING;
//  
//  strcpy(tag[nt], "TypeInputParticleFiles");
//  addr[nt] = &TypeInputParticleFiles;
//  id[nt++] = INT;
//
//  strcpy(tag[nt], "NumInputParticleFiles");
//  addr[nt] = &NumInputParticleFiles;
//  id[nt++] = INT;
//  
//  strcpy(tag[nt], "RamsesOutputNumber");
//  addr[nt] = &RamsesOutputNumber;
//  id[nt++] = INT;
//
//  strcpy(tag[nt], "ReadParticlesFromFile");
//  addr[nt] = &ReadParticlesFromFile;
//  id[nt++] = INT;
// }
// 
// -----------------------------------------------------------------
// Code needed to add to [vars.h]:
// -----------------------------------------------------------------
//  extern char InputParticleFileDir[200];
//  extern char InputParticleFilePrefix[200];
//  extern int  NumInputParticleFiles;
//  extern int  RamsesOutputNumber;
//  extern int  TypeInputParticleFiles;
//  extern int  ReadParticlesFromFile;
//  #define RAMSESFILE 1
//  #define ASCIIFILE  2
//  #define GADGETFILE 3
//
// -----------------------------------------------------------------
// Code needed to add to [vars.c]:
// -----------------------------------------------------------------
//  char InputParticleFileDir[200];
//  char InputParticleFilePrefix[200];
//  int  NumInputParticleFiles;
//  int  RamsesOutputNumber;
//  int  TypeInputParticleFiles;
//  int  ReadParticlesFromFile;
//
//==================================================================

//==================================================================
// The RAMSES header
//==================================================================
struct Ramses_Header{
  int ncpu;
  int ndim;
  int npart;
  int localseed[4];
  int nstar_tot;
  int mstar_tot[2];
  int mstar_lost[2];
  int nsink;
} ramses_header;

//==================================================================
// The GADGET header
//==================================================================
struct Gadget_Header{
  int npart[6];
  double mass[6];
  double time;
  double redshift;
  int flag_sfr;
  int flag_feedback;
  unsigned int npartTotal[6];
  int flag_cooling;
  int num_files;
  double BoxSize;
  double Omega0;
  double OmegaLambda;
  double HubbleParam;
  int flag_stellarage;
  int flag_metals;
  unsigned int npartTotalHighWord[6];
  int  flag_entropy_instead_u;
  char fill[60];
} gadget_header;

//==================================================================
// Check a real-grid by computing some quantities like max/min/avg/rms
//==================================================================
void check_realgrid(float_kind *grid, char *desc){
  double mingrid = 1e100, maxgrid = -1e100;
  double avggrid = 0.0,   rmsgrid = 0.0;
  for(int IX = 0; IX < Local_nx; IX++){
    for(int IY = 0; IY < Nmesh; IY++){
      for(int IZ = 0; IZ < Nmesh; IZ++){
        double curgrid = grid[(IX*Nmesh+IY)*2*(Nmesh/2+1)+IZ];
        avggrid += curgrid;
        rmsgrid += curgrid*curgrid;
        if(curgrid > maxgrid) maxgrid = curgrid;
        if(curgrid < mingrid) mingrid = curgrid;
      }
    }
  }

  // Allreduce over all CPUs
  ierr = MPI_Allreduce(&mingrid,   &mingrid, 1, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
  ierr = MPI_Allreduce(&maxgrid,   &maxgrid, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
  ierr = MPI_Allreduce(&avggrid,   &avggrid, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  ierr = MPI_Allreduce(&rmsgrid,   &rmsgrid, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  
  avggrid /= (double) (Nmesh*Nmesh*Nmesh);
  rmsgrid = sqrt(rmsgrid / (double) (Nmesh*Nmesh*Nmesh));
  
  if(ThisTask == 0) {
    printf("Check grid [%s]  Min: [%e]  Max: [%e]  Avg: [%e]  Rms: [%e]\n", desc, mingrid, maxgrid, avggrid, rmsgrid);
  }
}

//==================================================================
// Check a complex grid by computing some quantities like min/max
//==================================================================
void check_complexgrid(complex_kind *grid, char *desc){
  double mingridre = 1e100, maxgridre = -1e100;
  double mingridim = 1e100, maxgridim = -1e100;
  for (int i = 0; i < Local_nx; i++) {
    for (int j = 0; j < (unsigned int)(Nmesh/2+1); j++) {
      for (int k = 0; k < (unsigned int)(Nmesh/2+1); k++) {
        unsigned int ind = (i*Nmesh + j)*(Nmesh/2+1) + k;
        if(grid[ind][0] < mingridre) mingridre = grid[ind][0];
        if(grid[ind][0] > maxgridre) maxgridre = grid[ind][0];
        if(grid[ind][1] < mingridim) mingridim = grid[ind][0];
        if(grid[ind][1] > maxgridim) maxgridim = grid[ind][0];
      }
    }
  }

  // Allreduce over all CPUs
  ierr = MPI_Allreduce(&mingridre, &mingridre, 1, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
  ierr = MPI_Allreduce(&maxgridre, &maxgridre, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
  ierr = MPI_Allreduce(&mingridim, &mingridim, 1, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
  ierr = MPI_Allreduce(&maxgridim, &maxgridim, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);

  if(ThisTask == 0) {
    printf("Check grid [%s]  Min_Re: [%e]  Max_Re: [%e]  Min_Im: [%e]  Max_Im: [%e]\n", desc, mingridre, maxgridre, mingridim, maxgridim);
  }
}

//==================================================================
// This routine reads a single particle file and bins the particles to the [density] grid
// Assuming we have allocated the [density] grid and initialized it to [-1] at the start
// npart_loc is the number of particles in the buffer and the format of the buffer is
// [x1 x2 .. y1 y2 .. z1 z2 ..]
// Returns how many particles in the current slice there were in this file
//==================================================================
int ProcessParticlesSingleFile(char *buffer, int npart_loc) {
  unsigned int i;
  unsigned int IX,IY,IZ;
  int IXneigh,IYneigh,IZneigh;
  double X,Y,Z;
  double TX,TY,TZ;
  double DX,DY,DZ;
  double scaleBox=(double)Nmesh;
  double WPAR=pow((double)Nmesh/(double)Nsample,3);
  int npart_processed = 0;
  float *pos_flt = (float *) buffer;
  double *pos = (double *) buffer;

  // Loop over all particles and CIC add to grid
  for(i = 0; i < npart_loc; i++) {

    // Fetch X in [0,1]
    if(TypeInputParticleFiles == 3){
      // Gadget
      X = (double) pos_flt[3*i];
    } else {
      // Ramses and ascii
      X = pos[i]; 
    }
    
    // We must only process particles that are in the slice belonging to this CPU
    int IXX = (int)(X * scaleBox) - (int)(Local_x_start);
    if( IXX >= Local_nx || IXX < 0 )
      continue;
   
    // Fetch Y and Z
    if(TypeInputParticleFiles == 3){
      // Gadget
      Y = (double) pos_flt[3*i + 1];
      Z = (double) pos_flt[3*i + 2];
    } else {
      // Ramses and ascii
      Y = pos[i + 1*npart_loc];
      Z = pos[i + 2*npart_loc];
    }

    // Increase counter
    npart_processed++;

    // Scale particles such that they are in [0, Nmesh]
    X *= scaleBox;
    Y *= scaleBox;
    Z *= scaleBox;

    IX=(unsigned int)X;
    IY=(unsigned int)Y;
    IZ=(unsigned int)Z;
    DX=X-(double)IX;
    DY=Y-(double)IY;
    DZ=Z-(double)IZ;
    TX=1.0-DX;
    TY=1.0-DY;
    TZ=1.0-DZ;

    DY *= WPAR;
    TY *= WPAR;

    IX -= Local_x_start;
    if(IY >= (unsigned int)Nmesh) IY=0;
    if(IZ >= (unsigned int)Nmesh) IZ=0;

    IXneigh=IX+1;
    IYneigh=IY+1;
    IZneigh=IZ+1;
    if(IYneigh >= (unsigned int)Nmesh) IYneigh=0;
    if(IZneigh >= (unsigned int)Nmesh) IZneigh=0;

    density[(IX      * Nmesh+IY     ) * 2*(Nmesh/2 + 1) + IZ     ] += TX * TY * TZ;
    density[(IX      * Nmesh+IY     ) * 2*(Nmesh/2 + 1) + IZneigh] += TX * TY * DZ;
    density[(IX      * Nmesh+IYneigh) * 2*(Nmesh/2 + 1) + IZ     ] += TX * DY * TZ;
    density[(IX      * Nmesh+IYneigh) * 2*(Nmesh/2 + 1) + IZneigh] += TX * DY * DZ;
    density[(IXneigh * Nmesh+IY     ) * 2*(Nmesh/2 + 1) + IZ     ] += DX * TY * TZ;
    density[(IXneigh * Nmesh+IY     ) * 2*(Nmesh/2 + 1) + IZneigh] += DX * TY * DZ;
    density[(IXneigh * Nmesh+IYneigh) * 2*(Nmesh/2 + 1) + IZ     ] += DX * DY * TZ;
    density[(IXneigh * Nmesh+IYneigh) * 2*(Nmesh/2 + 1) + IZneigh] += DX * DY * DZ;
  }

  return npart_processed;
}

//==================================================================
// Binary read methods. Read a single int
//==================================================================
int read_int(FILE* fp){
  int tmp, skip, status;
  fread(&skip, sizeof(int), 1, fp);
  status = fread(&tmp, sizeof(int), 1, fp);
  if(status != 1){
    printf("Error in read_int Task [%i] : %i != %i\n", ThisTask, status, 1);
    fflush(stdout);
    MPI_Abort(MPI_COMM_WORLD,1);
    exit(1);
  }
  fread(&skip, sizeof(int), 1, fp);
  return tmp;
}

//==================================================================
// Binary read methods. Read an array of ints
//==================================================================
void read_int_vec(FILE* fp, int *buffer, int n){
  int skip, status;
  fread(&skip, sizeof(int), 1, fp);
  status = fread(buffer, sizeof(int), n, fp);
  if(status != n){
    printf("Error in read_int_vec Task [%i] : %i != %i\n", ThisTask, status, n);
    fflush(stdout);
    MPI_Abort(MPI_COMM_WORLD,1);
    exit(1);
  }
  fread(&skip, sizeof(int), 1, fp);
}

//==================================================================
// Binary read methods. Read an array of doubles
//==================================================================
void read_double_vec(FILE* fp, double *buffer, int n){
  int skip, status;
  fread(&skip, sizeof(int), 1, fp);
  status = fread(buffer, sizeof(double), n, fp);
  if(status != n){
    printf("Error in read_double_vec Task [%i] : %i != %i\n", ThisTask, status, n);
    fflush(stdout);
    MPI_Abort(MPI_COMM_WORLD,1);
    exit(1);
  }
  fread(&skip, sizeof(int), 1, fp);
}

//==================================================================
// Binary read methods. Read an array of floats
//==================================================================
void read_float_vec(FILE* fp, float *buffer, int n){
  int skip, status;
  fread(&skip, sizeof(int), 1, fp);
  status = fread(buffer, sizeof(float), n, fp);
  if(status != n){
    printf("Error in read_float_vec Task [%i] : %i != %i\n", ThisTask, status, n);
    fflush(stdout);
    MPI_Abort(MPI_COMM_WORLD,1);
    exit(1);
  }
  fread(&skip, sizeof(int), 1, fp);
}

//==================================================================
// Read the header of RAMSES files and store in header struct
//==================================================================
void read_ramses_header(FILE *fp){
  ramses_header.ncpu   = read_int(fp);
  ramses_header.ndim   = read_int(fp);
  ramses_header.npart  = read_int(fp);
  read_int_vec(fp, ramses_header.localseed, 4);
  ramses_header.nstar_tot = read_int(fp);
  read_int_vec(fp, ramses_header.mstar_tot, 2);
  read_int_vec(fp, ramses_header.mstar_lost, 2);
  ramses_header.nsink  = read_int(fp);
}

//==================================================================
// Read the header of ASCII files and store in header struct
//==================================================================
void read_gadget_header(FILE *fp){
  int tmp, status;
  fread(&tmp, sizeof(int), 1, fp);
  status = fread(&gadget_header, sizeof(gadget_header), 1, fp);
  if(status != 1){
    printf("Error in read_gadget_header Task [%i] : %i != %i\n", ThisTask, status, 1);
    fflush(stdout);
    MPI_Abort(MPI_COMM_WORLD,1);
    exit(1);
  }
  fread(&tmp, sizeof(int), 1, fp);
}

//==================================================================
// Read the header of all RAMSES particles files and
// determine the maximum particle number in them
// which we use to allocate the read buffer
//==================================================================
int find_maxpart_ramsesfiles(char *outputdir, int outnumber, int nfiles){
  int maxpart = 0;
  for(int i = 1; i <= nfiles; i++){
    FILE *fp;
    char filename[200];
    sprintf(filename, "%s/part_%05i.out%05i", outputdir, outnumber, i);
    if( (fp = fopen(filename,"r")) == NULL){
      printf("Error: cannot open file [%s]\n", filename);
      MPI_Abort(MPI_COMM_WORLD, 1);
      exit(1);
    }

    // Read header
    read_ramses_header(fp);
    int npart_loc = ramses_header.npart;

    if(npart_loc > maxpart) maxpart = npart_loc;

    fclose(fp);
  }
  return maxpart;
}

//==================================================================
// Read the first line of all ascii particles files and
// determine the maximum particle number in them
// which we use to allocate the read buffer
//==================================================================
int find_maxpart_asciifiles(char *outputdir, char *fileprefix, int nfiles){
  int maxpart = 0;
  for(int i = 1; i <= nfiles; i++){
    FILE *fp;
    char filename[200];
    sprintf(filename, "%s/%s.%i", outputdir, fileprefix, i);
    if( (fp = fopen(filename,"r")) == NULL){
      printf("Error: cannot open file [%s]\n", filename);
      MPI_Abort(MPI_COMM_WORLD, 1);
      exit(1);
    }

    // Read header
    int npart_loc;
    fscanf(fp, "%i\n", &npart_loc);

    if(npart_loc > maxpart) maxpart = npart_loc;

    fclose(fp);
  }
  return maxpart;
}

//==================================================================
// Read the header of all gadget particles files and
// determine the maximum particle number in them
// which we use to allocate the read buffer
//==================================================================
int find_maxpart_gadgetfiles(char *outputdir, char *fileprefix, int nfiles){
  int maxpart = 0;
  for(int i = 0; i < nfiles; i++){
    FILE *fp;
    char filename[200];
    sprintf(filename, "%s/%s.%i", outputdir, fileprefix, i);
    if( (fp = fopen(filename,"r")) == NULL){
      printf("Error: cannot open file [%s]\n", filename);
      MPI_Abort(MPI_COMM_WORLD, 1);
      exit(1);
    }

    // Read header
    read_gadget_header(fp);
    int npart_loc = gadget_header.npart[1];

    if(npart_loc > maxpart) maxpart = npart_loc;

    fclose(fp);
  }
  return maxpart;
}

//==================================================================
// Reads a single RAMSES file and stores the particle positions in [buffer]
// Updates [npart_read] with how many particles it was in this file [npart_loc] and returns this
//==================================================================
int read_ramses_file(char *filedir, int outnumber, int filenum, char *buffer, int *npart_read){
  FILE *fp;
  char filename[200];
  double *buffer_dbl = (double *) buffer;

  // Make filename and open in
  sprintf(filename, "%s/part_%05i.out%05i", filedir, outnumber, filenum);
  if( (fp = fopen(filename,"r")) == NULL){
    printf("Error: cannot open file [%s]\n", filename);
    MPI_Abort(MPI_COMM_WORLD, 1);
    exit(1);
  }

  // Read header
  read_ramses_header(fp);
  int npart_loc = ramses_header.npart;

  // Read particle positions (Buffer Format: [x1 x2 ... xn y1 y2 ... yn z1 z2 ... zn] )
  read_double_vec(fp, &buffer_dbl[0*npart_loc], npart_loc);
  read_double_vec(fp, &buffer_dbl[1*npart_loc], npart_loc);
  read_double_vec(fp, &buffer_dbl[2*npart_loc], npart_loc);

  // Make sure all positions are in [0,1]
  for(int i = 0; i < 3*npart_loc; i++)
    if(buffer_dbl[i] >= 1.0) buffer_dbl[i] -= 1.0;

  if(ThisTask == 0) {
    printf("# Reading RAMSES file: %s\n", filename);
    printf("First particle X: [%f]  Y: [%f]  Z: [%f]\n", buffer_dbl[0], buffer_dbl[0+npart_loc], buffer_dbl[0+2*npart_loc]);
  }

  fclose(fp);

  // Update how many particles we have read so far
  *npart_read += npart_loc;
  return npart_loc;
}

//==================================================================
// Assuming ascii-files in format /filedir/fileprefix.X where X = 1, 2, ..., n
// and that ascii-files have format [numpart; X1 Y1 Z1 mass; X2 Y2 Z2 mass; ...]
// with positions in [0,1]. Mass not used.
//==================================================================
int read_ascii_file(char *filedir, char *fileprefix, int filenum, char *buffer, int *npart_read){
  FILE *fp;
  char filename[200];
  double tmp;
  double *buffer_dbl = (double *) buffer;

  // Make filename and open
  sprintf(filename, "%s/%s.%i", filedir, fileprefix, filenum);
  if( (fp = fopen(filename,"r")) == NULL){
    printf("Error: cannot open file [%s]\n", filename);
    MPI_Abort(MPI_COMM_WORLD, 1);
    exit(1);
  }

  // Read header
  int npart_loc;
  fscanf(fp, "%i", &npart_loc);

  // Read particle positions (Buffer Format: [x1 x2 ... xn y1 y2 ... yn z1 z2 ... zn] )
  for(int i = 0; i < npart_loc; i++)
    fscanf(fp, "%lf %lf %lf %lf\n", &buffer_dbl[i + 0*npart_loc], &buffer_dbl[i + 1*npart_loc], &buffer_dbl[i + 2*npart_loc], &tmp);
  
  // Make sure all positions are in [0,1]
  for(int i = 0; i < 3*npart_loc; i++)
    if(buffer_dbl[i] >= 1.0) buffer_dbl[i] -= 1.0;

  if(ThisTask == 0) {
    printf("# Reading ascii file: %s  Npart: %i\n", filename, npart_loc);
    printf("First particle X: [%f]  Y: [%f]  Z: [%f]\n", buffer_dbl[0], buffer_dbl[0+npart_loc], buffer_dbl[0+2*npart_loc]);
  }

  fclose(fp);

  // Update how many particles we have read so far
  *npart_read += npart_loc;
  return npart_loc;
}

//==================================================================
// Reads a single GADGET1 file and stores the particle positions in [buffer]
// Updates [npart_read] with how many particles it was in this file [npart_loc] and returns this
//==================================================================
int read_gadget_file(char *filedir, char *fileprefix, int filenum, char *buffer, int *npart_read){
  FILE *fp;
  char filename[200];
  float *buffer_flt = (float *) buffer;
  
  // Make filename and open
  sprintf(filename, "%s/%s.%i", filedir, fileprefix, filenum );
  if( (fp = fopen(filename,"r")) == NULL){
    printf("Error: cannot open file [%s]\n", filename);
    MPI_Abort(MPI_COMM_WORLD, 1);
    exit(1);
  }

  // Read header
  read_gadget_header(fp);
  int npart_loc = gadget_header.npart[1];

  // Read positions (Buffer format: [x1 y1 z1 x2 y2 z2 ...])
  read_float_vec(fp, buffer_flt, 3*npart_loc);

  // Normalize positions to [0,1]
  double normfac = 1.0 / gadget_header.BoxSize;
  for(int i = 0; i < 3*npart_loc; i++){
    buffer_flt[i] *= normfac;
    if(buffer_flt[i] >= 1.0) buffer_flt[i] -= 1.0;
  }

  if(ThisTask == 0) {
    printf("# Reading gadget file: %s  Npart: %i\n", filename, npart_loc);
    printf("First particle X: [%f]  Y: [%f]  Z: [%f]\n", buffer_flt[0], buffer_flt[0+npart_loc], buffer_flt[0+2*npart_loc]);
  }
  fclose(fp);

  // Update how many particles we have read so far
  *npart_read += npart_loc;
  return npart_loc;
}

//==================================================================
// We read particle files and use it to generate the displacement-field
// corresponding to that particle distribution as an alternative to generate
// the initial conditons directly. This is useful if we want to run sims
// in PICOLA using the same IC as some external simulation
// Assumes the density-field from particles is the 1LPT density field,
// which in reality we should have something like q = q_0 + DPsi_1(q) + DPsi_2(q)
// NB: This has to be run after Total_size and Nmesh has been computed
//==================================================================
void ReadFilesMakeDisplacementField(void){
  timer_start(_ReadParticlesFromFile);
  
  if(ThisTask == 0) {
    printf("\n==============================================\n");
    printf("Reading particles from external file\n");
    printf("==============================================\n\n");
  }

  // Compute size of buffer needed
  int maxpart = 0;
  if(TypeInputParticleFiles == RAMSESFILE){
    maxpart = find_maxpart_ramsesfiles(InputParticleFileDir, RamsesOutputNumber, NumInputParticleFiles);
  
    if(ThisTask == 0) {
      printf("RAMSES Filedir: [%s] Nfiles: [%i] OutputNumber: [%i] Maxpart_files: [%i]\n", 
          InputParticleFileDir, NumInputParticleFiles, RamsesOutputNumber, maxpart);
    }
  } else if(TypeInputParticleFiles == ASCIIFILE){
    maxpart = find_maxpart_asciifiles(InputParticleFileDir, InputParticleFilePrefix, NumInputParticleFiles);
    
    if(ThisTask == 0) {
      printf("ASCII Filedir: [%s] Fileprefix: [%s] Nfiles: [%i] Maxpart_files: [%i]\n", 
          InputParticleFileDir, InputParticleFilePrefix, NumInputParticleFiles, maxpart);
    }
  } else if(TypeInputParticleFiles == GADGETFILE) { 
    maxpart = find_maxpart_gadgetfiles(InputParticleFileDir, InputParticleFilePrefix, NumInputParticleFiles);
    
    if(ThisTask == 0) {
      printf("GADGET Filedir: [%s] Fileprefix: [%s] Nfiles: [%i] Maxpart_files: [%i]\n", 
          InputParticleFileDir, InputParticleFilePrefix, NumInputParticleFiles, maxpart);
    }
  } else {
    printf("Error: unknown file-format [%i]\n", TypeInputParticleFiles);
    MPI_Abort(MPI_COMM_WORLD, 1);
    exit(1);
  }

  // Allocate read buffer
  char *buffer   = malloc(3*sizeof(double)*maxpart);
  float *pos_flt = (float *)  buffer;
  double *pos    = (double *) buffer;
  
  // Initialize density array and FFT plan
  density = malloc(2*Total_size*sizeof(float_kind));
  P3D  = (complex_kind *) density;
  for(int i = 0; i < 2*Total_size; i++) density[i] = -1.0;
  plan = my_fftw_mpi_plan_dft_r2c_3d(Nmesh, Nmesh, Nmesh, density, P3D, MPI_COMM_WORLD, FFTW_ESTIMATE);

  if(ThisTask == 0) {
    printf("\n=================================\n");
    printf("Starting read particle files\n");
    printf("=================================\n");
  }

  //====================================
  // For testing. Output particles to ascii file
  // FILE *fp;
  // if(ThisTask == 0){
  //   if( (fp = fopen("output_particles.1","w")) == NULL){
  //     printf("Error: cannot open file [%s]\n", "output_particles.1");
  //     MPI_Abort(MPI_COMM_WORLD, 1);
  //     exit(1);
  //   }
  //   fprintf(fp,"%i\n", NumPart);
  // }
  //====================================

  // Loop over particle files and read particles
  int npart_read = 0, NumPart_local = 0;
  double maxxyz = -1e100, minxyz = 1e100;

  for(int filenum = 1; filenum <= NumInputParticleFiles; filenum++){
    int npart_file = 0;

    // Read the files
    if(TypeInputParticleFiles == RAMSESFILE){
      npart_file = read_ramses_file(InputParticleFileDir, RamsesOutputNumber, filenum, buffer, &npart_read);
    } else if(TypeInputParticleFiles == ASCIIFILE){
      npart_file = read_ascii_file(InputParticleFileDir, InputParticleFilePrefix, filenum, buffer, &npart_read);
    } else if(TypeInputParticleFiles == GADGETFILE){
      npart_file = read_gadget_file(InputParticleFileDir, InputParticleFilePrefix, filenum - 1, buffer, &npart_read);
    }

    if(ThisTask == 0) {
      printf("Read so far: %i  Part in current file %i\n", npart_read, npart_file); 
    }

    //====================================
    // Output the particles to a ascii file
    // if(ThisTask == 0){
    //   for(int i = 0; i < npart_file; i++){
    //     if(TypeInputParticleFiles == 3)
    //       fprintf(fp, "%e %e %e 1.0\n", pos_flt[i], pos_flt[i + npart_file], pos_flt[i + 2*npart_file]);
    //     else
    //       fprintf(fp, "%e %e %e 1.0\n", pos[i], pos[i + npart_file], pos[i + 2*npart_file]);
    //   }
    // }
    //====================================

    // Process particles [note assuming density array has been init to -1 before running this]
    NumPart_local += ProcessParticlesSingleFile(buffer, npart_file);

    // Compute min / max
    for(int j = 0; j < 3*npart_file; j++){
      if(TypeInputParticleFiles == 3){
        if(pos_flt[j] > maxxyz) maxxyz = (double) pos_flt[j];
        if(pos_flt[j] < minxyz) minxyz = (double) pos_flt[j];
      } else {
        if(pos[j] > maxxyz) maxxyz = pos[j];
        if(pos[j] < minxyz) minxyz = pos[j];
      }
    }
  }
 
  //====================================
  // Output the particles to a ascii file
  // if(ThisTask == 0) fclose(fp);
  //====================================

  if(ThisTask == 0) {
    printf("Particles in particle files has Min_xyz: [%e]  Max_xyz: [%e]\n", minxyz, maxxyz);
  }

  // Clean up
  free(buffer);

  // Copy across the extra slice from the task on the left and add it to the leftmost slice
  // of the task on the right. Skip over tasks without any slices.
  float_kind * temp_density = (float_kind *)calloc(2*alloc_slice,sizeof(float_kind));
  ierr = MPI_Sendrecv(&(density[2*last_slice]),2*alloc_slice*sizeof(float_kind),MPI_BYTE,RightTask,0,
      &(temp_density[0]),2*alloc_slice*sizeof(float_kind),MPI_BYTE,LeftTask,0,MPI_COMM_WORLD,&status);
  if (NumPart_local != 0) {
    for (int i = 0; i < 2*alloc_slice; i++) density[i] += (temp_density[i]+1.0);
  }
  free(temp_density);

  // Check density field
  check_realgrid(density, "density-field");

  if(ThisTask == 0) printf("Fourier transforming density field...\n");

  // FFT the density field
  my_fftw_execute(plan);

  // Account for FFTW normalization and use LCDM growth-factor to bring the density-field to redshift 0
  double normfac = 1.0/(double)(Nmesh*Nmesh*Nmesh);
  normfac *= growth_DLCDM(1.0) / growth_DLCDM(1.0/(1.0+Init_Redshift));
  for(int i = 0; i < 2*Total_size; i++) density[i] *= normfac;

  // Compute some quantities (min / max / avg of density)
  check_complexgrid(P3D, "density-field-k");
  timer_stop(_ReadParticlesFromFile);

  // We now have delta(k, z = 0) in P3D and are is ready to compute the displacement-field
  // The method below calls AssignDisplacementField()
  if(ThisTask == 0) printf("Done precomputing delta(k) from particles, now compute displacement-fields\n\n");
  displacement_fields();

  // Clean up
  free(density);
  my_fftw_destroy_plan(plan);
}

//==================================================================
// Assign the density-field from the already computed delta(k)
// Since IC are assumed to be for LCDM this is the LCDM displacment-
// field. For scale-dependent growth we therefore rescale it
// We also deconvolve the window-function
//==================================================================
void AssignDisplacementField(complex_kind *(cdisp[3])){
  
  // If we want to rescale sigma8 as given in file
  // Assumes the sigam8 in file is the acctual one in the part. dist.
  double sigma8_mg_over_sigma8_lcdm = mg_sigma8_enhancement(1.0);
  
  for (int i = 0; i < Local_nx; i++) {
    int iglobal = i + Local_x_start;
    for (int j = 0 ; j < (unsigned int)(Nmesh/2+1); j++) {
      int kmin = 0;
      if ((iglobal == 0) && (j == 0)) {
        kmin = 1;
        for(int axes = 0; axes < 3; axes++){
          cdisp[axes][0][0] = 0.0;
          cdisp[axes][0][1] = 0.0;
        }
      }
      for (int k = kmin; k < (unsigned int) (Nmesh/2+1); k++) {
        unsigned int coord = (i*Nmesh+j)*(Nmesh/2+1)+k;

        // Compute k-vector and its norm
        double kvec[3], d[3] = {iglobal > Nmesh/2 ? iglobal-Nmesh : iglobal, j, k};
        double kmag2 = 0.0;
        for(int axes = 0; axes < 3; axes++){
          kvec[axes] = d[axes] * 2 * PI / Box;
          kmag2 += kvec[axes] * kvec[axes];
        }

        // Deconvolve window function (CIC)
        double grid_corr = 1.0;
        for(int axes = 0; axes < 3; axes++){
          if (d[axes] != 0) grid_corr *= sin((PI*d[axes])/(double)Nmesh)/((PI*d[axes])/(double)Nmesh);
        }
        grid_corr = pow(1.0 / grid_corr, 2.0);
        
        // Since the displacement field we stored is for LCDM we rescale it to get it for MG
        double kmag = sqrt(kmag2);
        double rescale_fac  = sqrt(mg_pofk_ratio(kmag, 1.0));

        // Rescale sigma8
        if( ! input_sigma8_is_for_lcdm )
          rescale_fac /= sigma8_mg_over_sigma8_lcdm;

        // Assign the displacementfield from delta(k) we have computed
        for(int axes = 0; axes < 3; axes++){
          cdisp[axes][coord][0] = -kvec[axes] / kmag2 * P3D[coord][1] * grid_corr * rescale_fac;
          cdisp[axes][coord][1] =  kvec[axes] / kmag2 * P3D[coord][0] * grid_corr * rescale_fac;
        }

        // Assign the mirror along the y axis
        if ((j != (unsigned int)(Nmesh/2)) && (j != 0)) {
          coord = (i*Nmesh + (Nmesh-j))*(Nmesh/2+1)+k;
          
          // Mirror ky-vector component
          kvec[1] = -d[1] * 2 * PI / Box;

          // Assign the displacementfield from delta(k) we have computed
          for(int axes = 0; axes < 3; axes++){
            cdisp[axes][coord][0] = -kvec[axes] / kmag2 * P3D[coord][1] * grid_corr * rescale_fac;
            cdisp[axes][coord][1] =  kvec[axes] / kmag2 * P3D[coord][0] * grid_corr * rescale_fac;
          }
        }
      }
    }
  }
}

void readICFromFile_assign_particles(){
  double A = 1.0/(1.0 + Init_Redshift);
  double Di_lcdm = growth_DLCDM(A);
  double Dv_lcdm = growth_dDLCDMdy(A);

#ifndef SCALEDEPENDENT
  // The IC we read in are for LCDM so make sure we rescale the initial displacement field
  // given to the particles so that this becomes correct. For scale-dependent growth this is
  // not needed here as we don't store D and D2 but recompute them at every step
  double rescale_1lpt = growth_DLCDM(A) / growth_D(A);
  double rescale_2lpt = growth_D2LCDM(A) / growth_D2(A);
#endif

  // ===========================================================================================
  // Generate the initial particle positions and velocities
  // If UseCOLA = 0 (non-COLA), then velocity is ds/dy, which is simply the 2LPT IC.
  // Else set vel = 0 if we subtract LPT. This is the same as the action of the operator L_- from TZE, as initial velocities are in 2LPT.
  // ===========================================================================================

  // Assign positions, velocities and displacement
  for(int i = 0; i < Local_np; i++) {
    for (int j = 0; j < Nsample; j++) {
      for (int k = 0; k < Nsample; k++) {
        unsigned int coord = (i * Nsample + j) * Nsample + k;

#ifdef PARTICLE_ID          
        P[coord].ID = ((unsigned long long)((i + Local_p_start) * Nsample + j)) * (unsigned long long)Nsample + (unsigned long long)k;
#endif

        // Assign displacementfields to particles
        for (int m = 0; m < 3; m++) {
#ifndef SCALEDEPENDENT 
          P[coord].D[m]  = ZA[m][coord];
          P[coord].D2[m] = LPT[m][coord];
#endif
          if (UseCOLA == 0) {

            //==============================================================================================
            // When reading particles from file we assume the IC are as in LCDM so we use this growth-factor 
            // to get the right normalization here. If changing this one probably also need to change the normfac in readICfromfile.h
            //==============================================================================================
            P[coord].Vel[m] = ZA[m][coord] * Dv_lcdm;

          } else {

            P[coord].Vel[m] = 0.0;

          }
        }

        //==============================================================================================
        // NB: We don't add the 2LPT contribution here as this is already included in the particle distribution read from file
        //==============================================================================================
        P[coord].Pos[0] = periodic_wrap( (i + Local_p_start)*(Box/(double)Nsample) + ZA[0][coord] * Di_lcdm );
        P[coord].Pos[1] = periodic_wrap( (j                )*(Box/(double)Nsample) + ZA[1][coord] * Di_lcdm );
        P[coord].Pos[2] = periodic_wrap( (k                )*(Box/(double)Nsample) + ZA[2][coord] * Di_lcdm );

        //==============================================================================================
        // So far the displacement-field corresponds to LCDM at redshift 0 so rescale it to the correct value
        //==============================================================================================
#ifndef SCALEDEPENDENT 
        if(UseCOLA != 0){
          for (int m = 0; m < 3; m++) {
            P[coord].D[m]  *= rescale_1lpt;
            P[coord].D2[m] *= rescale_2lpt;
          }
        }
#endif
      }
    }
  }
}

//====================================================
// Write a GADGET header
//====================================================

#ifdef GADGET_STYLE
void write_gadget_header(FILE *fp, double A){
  double Z = 1.0/A - 1.0;
  int dummy;      

  // Gadget header stuff
  for(int k = 0; k < 6; k++) {
    header.npart[k]      = 0;
    header.npartTotal[k] = 0;
    header.mass[k]       = 0;
  }
  header.npart[1]      = NumPart;
  header.npartTotal[1] = TotNumPart;
  header.npartTotal[2] = (TotNumPart >> 32);
  header.mass[1]       = (3.0 * Omega * Hubble * Hubble * Box * Box * Box) / (8.0 * PI * G * TotNumPart);
  header.time          = A;
  header.redshift      = Z;

  header.flag_sfr        = 0;
  header.flag_feedback   = 0;
  header.flag_cooling    = 0;
  header.flag_stellarage = 0;
  header.flag_metals     = 0;
  header.flag_stellarage = 0;
  header.flag_metals     = 0;
  header.hashtabsize     = 0;

  header.num_files = NTaskWithN;

  header.BoxSize      = Box;
  header.Omega0       = Omega;
  header.OmegaLambda  = 1.0 - Omega;
  header.HubbleParam  = HubbleParam;

  dummy = sizeof(header);
  my_fwrite(&dummy,  sizeof(dummy),  1, fp);
  my_fwrite(&header, sizeof(header), 1, fp);
  my_fwrite(&dummy,  sizeof(dummy),  1, fp);
}
#endif
