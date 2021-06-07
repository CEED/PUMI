#include <pcu_util.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pcu_io.h>
#include <phIO.h>
#include <PCU.h>
#include <lionPrint.h>
#include <phiotimer.h>

#define PH_LINE 1024
#define MAGIC 362436
#define FIELD_PARAMS 3

enum {
NODES_PARAM,
VARS_PARAM,
STEP_PARAM
};

static const char* magic_name = "byteorder magic number";

void ph_write_header(FILE* f, const char* name, size_t bytes,
    int nparam, int* params)
{
  int i;
  fprintf(f,"%s : < %lu > ", name, (long)bytes);
  for (i = 0; i < nparam; ++i)
    fprintf(f, "%d ", params[i]);
  fprintf(f, "\n");
}

static void skip_leading_spaces(char** s)
{
  while (**s == ' ') ++(*s);
}

static void cut_trailing_spaces(char* s)
{
  char* e = s + strlen(s);
  for (--e; e >= s; --e)
    if (*e != ' ')
      break;
  ++e;
  *e = '\0';
}

static void parse_header(char* header, char** name, long* bytes,
    int nparam, int* params)
{
  char* saveptr = NULL;
  int i = 0;
  PCU_ALWAYS_ASSERT(header != NULL);
  header = strtok_r(header, ":", &saveptr);
  if (name) {
    *name = header;
    skip_leading_spaces(name);
    cut_trailing_spaces(*name);
  }
  strtok_r(NULL, "<", &saveptr);
  header = strtok_r(NULL, ">", &saveptr);
  if (bytes)
    sscanf(header, "%ld", bytes);
  if (params) {
    while( (header = strtok_r(NULL, " \n", &saveptr)) )
      sscanf(header, "%d", &params[i++]);
    while( i < nparam )
      params[i++] = 0;
  }
}

static int find_header(FILE* f, const char* name, char* found, char header[PH_LINE])
{
  char* hname;
  long bytes;
  char tmp[PH_LINE];
  while (fgets(header, PH_LINE, f)) {
    if ((header[0] == '#') || (header[0] == '\n'))
      continue;
    strncpy(tmp, header, PH_LINE-1);
    tmp[PH_LINE-1] = '\0';
    parse_header(tmp, &hname, &bytes, 0, NULL);
    if (!strncmp(name, hname, strlen(name))) {
      strncpy(found, hname, strlen(found));
      found[strlen(hname)] = '\0';
      return 1;
    }
    fseek(f, bytes, SEEK_CUR);
  }
  if (!PCU_Comm_Self() && strlen(name) > 0)
    lion_eprint(1,"warning: phIO could not find \"%s\"\n",name);
  return 0;
}

static void write_magic_number(FILE* f)
{
  int why = 1;
  int magic = MAGIC;
  ph_write_header(f, magic_name, sizeof(int) + 1, 1, &why);
  fwrite(&magic, sizeof(int), 1, f);
  fprintf(f,"\n");
}

static int seek_after_header(FILE* f, const char* name)
{
  char dummy[PH_LINE];
  char found[PH_LINE];
  return find_header(f, name, found, dummy);
}

static void my_fread(void* p, size_t size, size_t nmemb, FILE* f)
{
  size_t r = fread(p, size, nmemb, f);
  PCU_ALWAYS_ASSERT(r == nmemb);
}

static int read_magic_number(FILE* f)
{
  int magic;
  if (!seek_after_header(f, magic_name)) {
    if (!PCU_Comm_Self())
      lion_eprint(1,"warning: not swapping bytes\n");
    rewind(f);
    return 0;
  }
  my_fread(&magic, sizeof(int), 1, f);
  return magic != MAGIC;
}

void ph_write_preamble(FILE* f)
{
  fprintf(f, "# PHASTA Input File Version 2.0\n");
  fprintf(f, "# Byte Order Magic Number : 362436 \n");
  fprintf(f, "# Output generated by libph version: yes\n");
  write_magic_number(f);
}

void ph_write_doubles(FILE* f, const char* name, double* data,
    size_t n, int nparam, int* params)
{
  ph_write_header(f, name, n * sizeof(double) + 1, nparam, params);
  PHASTAIO_WRITETIME(fwrite(data, sizeof(double), n, f);, (n*sizeof(double)))
  fprintf(f, "\n");
}

void ph_write_ints(FILE* f, const char* name, int* data,
    size_t n, int nparam, int* params)
{
  ph_write_header(f, name, n * sizeof(int) + 1, nparam, params);
  PHASTAIO_WRITETIME(fwrite(data, sizeof(int), n, f);, (n*sizeof(int)))
  fprintf(f, "\n");
}

static void parse_params(char* header, long* bytes,
    int* nodes, int* vars, int* step)
{
  int params[FIELD_PARAMS];
  parse_header(header, NULL, bytes, FIELD_PARAMS, params);
  *nodes = params[NODES_PARAM];
  *vars = params[VARS_PARAM];
  *step = params[STEP_PARAM];
}

int ph_should_swap(FILE* f) {
  return read_magic_number(f);
}

int ph_read_field(FILE* f, const char* field, int swap,
    double** data, int* nodes, int* vars, int* step, char* hname)
{
  long bytes, n;
  char header[PH_LINE];
  int ok;
  ok = find_header(f, field, hname, header);
  if(!ok) /* not found */
    return 0;
  parse_params(header, &bytes, nodes, vars, step);
  if(!bytes) /* empty data block */
    return 1;
  PCU_ALWAYS_ASSERT(((bytes - 1) % sizeof(double)) == 0);
  n = (bytes - 1) / sizeof(double);
  PCU_ALWAYS_ASSERT((int)n == (*nodes) * (*vars));
  *data = malloc(bytes);
  PHASTAIO_READTIME(my_fread(*data, sizeof(double), n, f);, (sizeof(double)*n))
  if (swap)
    pcu_swap_doubles(*data, n);
  return 2;
}

void ph_write_field(FILE* f, const char* field, double* data,
    int nodes, int vars, int step)
{
  int params[FIELD_PARAMS];
  params[NODES_PARAM] = nodes;
  params[VARS_PARAM] = vars;
  params[STEP_PARAM] = step;
  ph_write_doubles(f, field, data, nodes * vars, FIELD_PARAMS, params);
}
