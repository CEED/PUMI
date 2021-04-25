#include <PCU.h>
#include <lionPrint.h>
#include <pcu_util.h>
#include "pumi.h"
#include <apfMatrix.h>

#include <stdlib.h>
#include "maStats.h"
#include <iostream>
#include <fstream>
#include <sstream>

int main(int argc, char** argv) {
  if (argc != 5)
    fprintf(stderr, "Usage: <min> <max> <nbins> <filename>\n");
  PCU_ALWAYS_ASSERT(argc == 5);

  const double min = std::atof(argv[1]);
  const double max = std::atof(argv[2]);
  const int nbins = std::atoi(argv[3]);
  const char *filename = argv[4];
  const double bin_size = (max-min)/(nbins*1.0);

  MPI_Init(&argc, &argv);
  PCU_Comm_Init ();
  lion_set_verbosity (1);

  std::vector<double> input;
  std::string line;
  std::ifstream file;
  file.open (filename);
  while (std::getline(file, line)) { 
    input.push_back(std::stod(line));
  }
  file.close();

  int count[nbins] = {0};
  for (size_t i = 0; i < input.size(); ++i) {
    int bin = (int)std::round((input[i] - min)/bin_size);
    count[bin] += 1;
  }

  for (int i = 0; i < nbins; ++i) {
    fprintf(stderr, "%d\n", count[i]);
  }

  PCU_Comm_Free ();
  MPI_Finalize ();
}
