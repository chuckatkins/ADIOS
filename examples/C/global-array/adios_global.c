/* ADIOS C Example: write a global array from N processors with gwrite
 *
 * How to run: mpirun -np <N> adios_global
 * Output: adios_global.bp
 * ADIOS config file: adios_global.xml
 *
*/
#include <stdio.h>
#include <string.h>
#include "mpi.h"
#include "adios.h"
int main (int argc, char ** argv) 
{
	char        filename [256];
	int         rank, size, i;
	int         NX = 10; 
	double      t[NX];
	MPI_Comm    comm = MPI_COMM_WORLD;

	/* ADIOS variables declarations for matching gwrite_temperature.ch */
	int         adios_err;
	uint64_t    adios_groupsize, adios_totalsize;
	int64_t     adios_handle;

	MPI_Init (&argc, &argv);
	MPI_Comm_rank (comm, &rank);
	MPI_Comm_size (comm, &size);

	for (i = 0; i < NX; i++)
		t [i] = 10*rank+i;

	strcpy (filename, "adios_global.bp");

	adios_init ("adios_global.xml");

	adios_open (&adios_handle, "temperature", filename, "w");
	#include "gwrite_temperature.ch"
	adios_close (adios_handle);

        MPI_Barrier (comm);

	adios_finalize (rank);

	MPI_Finalize ();
	return 0;
}