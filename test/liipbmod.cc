#include "LIIPBMod.h"
#include <apf.h>
#include <apfMDS.h>
#include <gmi_mesh.h>
#include <parma.h>
#include <PCU.h>
#include <cassert>

int main(int argc, char** argv)
{
  assert(argc == 4);
  MPI_Init(&argc,&argv);
  PCU_Comm_Init();
  if ( argc != 4 ) {
    if ( !PCU_Comm_Self() )
      printf("Usage: %s <model> <mesh> <out mesh>\n", argv[0]);
    MPI_Finalize();
    exit(EXIT_FAILURE);
  }
  gmi_register_mesh();
  apf::Mesh2* m = apf::loadMdsMesh(argv[1],argv[2]);
  Parma_PrintPtnStats(m, "initial");
  LIIPBMod liipbmod;
  liipbmod.run(m);
  Parma_PrintPtnStats(m, "final");
  m->destroyNative();
  apf::destroyMesh(m);
  PCU_Comm_Free();
  MPI_Finalize();
}
