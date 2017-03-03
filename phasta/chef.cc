#include <apf.h>
#include <apfMesh.h>
#include <gmi_mesh.h>
#include <PCU.h>
#ifdef HAVE_SIMMETRIX
#include <gmi_sim.h>
#include <SimUtil.h>
#include <SimPartitionedMesh.h>
#include <SimAdvMeshing.h>
#endif
#include <cassert>
#include <chef.h>

/** \file chef.cc
    \brief The Chef command line executable. */

namespace {
  void freeMesh(apf::Mesh* m) {
    m->destroyNative();
    apf::destroyMesh(m);
  }
}

/** @brief run the operations requested in "adapt.inp" */
int main(int argc, char** argv)
{
  MPI_Init(&argc,&argv);
  PCU_Comm_Init();
  PCU_Protect();
#ifdef HAVE_SIMMETRIX
  SimUtil_start();
  Sim_readLicenseFile(0);
  SimPartitionedMesh_start(0, 0);
  SimAdvMeshing_start();
  gmi_sim_start();
  gmi_register_sim();
#endif
  gmi_register_mesh();
  gmi_model* g = 0;
  apf::Mesh2* m = 0;
  chef::cook(g,m);
  freeMesh(m);
#ifdef HAVE_SIMMETRIX
  gmi_sim_stop();
  SimPartitionedMesh_stop();
  SimAdvMeshing_stop();
  Sim_unregisterAllKeys();
  SimUtil_stop();
#endif
  PCU_Comm_Free();
  MPI_Finalize();
}

