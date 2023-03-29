/****************************************************************************** 

  Copyright 2011 Scientific Computation Research Center, 
      Rensselaer Polytechnic Institute. All rights reserved.
  
  This work is open source software, licensed under the terms of the
  BSD license as described in the LICENSE file in the top-level directory.

*******************************************************************************/
#ifndef PCU_MPI_H
#define PCU_MPI_H

#include "pcu_buffer.h"
#include <mpi.h>

typedef struct
{
  pcu_buffer buffer;
  MPI_Request request;
  int peer;
} pcu_message;

void pcu_make_message(pcu_message* m);
void pcu_free_message(pcu_message* m);

typedef struct
{
  MPI_Comm original_comm;
  MPI_Comm user_comm;
  MPI_Comm coll_comm;
  int rank;
  int size;
} pcu_mpi_t;

int pcu_mpi_size(pcu_mpi_t*);
int pcu_mpi_rank(pcu_mpi_t*);
void pcu_mpi_send(pcu_mpi_t*, pcu_message* m, MPI_Comm comm);
bool pcu_mpi_done(pcu_mpi_t*, pcu_message* m);
bool pcu_mpi_receive(pcu_mpi_t*, pcu_message* m, MPI_Comm comm);
void pcu_mpi_init(MPI_Comm comm, pcu_mpi_t* mpi);
void pcu_mpi_finalize(pcu_mpi_t* mpi);

#endif
