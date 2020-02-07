#include <PCU.h>
#include "pcu_util.h"
#include "apfConvert.h"
#include "apfMesh2.h"
#include "apf.h"
#include "apfNumbering.h"
#include <map>

namespace apf {

typedef int Gid;

static void constructVerts(
    Mesh2* m, const Gid* conn, int nelem, int etype,
    GlobalToVert& result)
{
  ModelEntity* interior = m->findModelEntity(m->getDimension(), 0);
  int end = nelem * apf::Mesh::adjacentCount[etype][0];
  for (int i = 0; i < end; ++i)
    if ( ! result.count(conn[i]))
      result[conn[i]] = m->createVert_(interior);
}

static void constructElements(
    Mesh2* m, const Gid* conn, int nelem, int etype,
    GlobalToVert& globalToVert)
{
  ModelEntity* interior = m->findModelEntity(m->getDimension(), 0);
  int nev = apf::Mesh::adjacentCount[etype][0];
  for (int i = 0; i < nelem; ++i) {
    Downward verts;
    int offset = i * nev;
    for (int j = 0; j < nev; ++j)
      verts[j] = globalToVert[conn[j + offset]];
    buildElement(m, interior, etype, verts);
  }
}

static Gid getMax(const GlobalToVert& globalToVert)
{
  Gid max = -1;
  APF_CONST_ITERATE(GlobalToVert, globalToVert, it)
    max = std::max(max, it->first);
  return PCU_Max_Int(max); // this is type-dependent
}


/* algorithm courtesy of Sebastian Rettenberger:
   use brokers/routers for the vertex global ids.
   Although we have used this trick before (see mpas/apfMPAS.cc),
   I didn't think to use it here, so credit is given. */
static void constructResidence(Mesh2* m, GlobalToVert& globalToVert)
{
  Gid max = getMax(globalToVert);
  Gid total = max + 1;
  int peers = PCU_Comm_Peers();
  int quotient = total / peers;
  int remainder = total % peers;
  int mySize = quotient;
  int self = PCU_Comm_Self();
  if (self == (peers - 1))
    mySize += remainder;
  typedef std::vector< std::vector<int> > TmpParts;
  TmpParts tmpParts(mySize);
  /* if we have a vertex, send its global id to the
     broker for that global id */
  PCU_Comm_Begin();
  APF_ITERATE(GlobalToVert, globalToVert, it) {
    int gid = it->first;
    int to = std::min(peers - 1, gid / quotient);
    PCU_COMM_PACK(to, gid);
  }
  PCU_Comm_Send();
  int myOffset = self * quotient;
  /* brokers store all the part ids that sent messages
     for each global id */
  while (PCU_Comm_Receive()) {
    int gid;
    PCU_COMM_UNPACK(gid);
    int from = PCU_Comm_Sender();
    tmpParts.at(gid - myOffset).push_back(from);
  }
  /* for each global id, send all associated part ids
     to all associated parts */
  PCU_Comm_Begin();
  for (int i = 0; i < mySize; ++i) {
    std::vector<int>& parts = tmpParts[i];
    for (size_t j = 0; j < parts.size(); ++j) {
      int to = parts[j];
      int gid = i + myOffset;
      int nparts = parts.size();
      PCU_COMM_PACK(to, gid);
      PCU_COMM_PACK(to, nparts);
      for (size_t k = 0; k < parts.size(); ++k)
        PCU_COMM_PACK(to, parts[k]);
    }
  }
  PCU_Comm_Send();
  /* receiving a global id and associated parts,
     lookup the vertex and classify it on the partition
     model entity for that set of parts */
  while (PCU_Comm_Receive()) {
    int gid;
    PCU_COMM_UNPACK(gid);
    int nparts;
    PCU_COMM_UNPACK(nparts);
    Parts residence;
    for (int i = 0; i < nparts; ++i) {
      int part;
      PCU_COMM_UNPACK(part);
      residence.insert(part);
    }
    MeshEntity* vert = globalToVert[gid];
    m->setResidence(vert, residence);
  }
}

/* given correct residence from the above algorithm,
   negotiate remote copies by exchanging (gid,pointer)
   pairs with parts in the residence of the vertex */
static void constructRemotes(Mesh2* m, GlobalToVert& globalToVert)
{
  int self = PCU_Comm_Self();
  PCU_Comm_Begin();
  APF_ITERATE(GlobalToVert, globalToVert, it) {
    int gid = it->first;
    MeshEntity* vert = it->second;
    Parts residence;
    m->getResidence(vert, residence);
    APF_ITERATE(Parts, residence, rit)
      if (*rit != self) {
        PCU_COMM_PACK(*rit, gid);
        PCU_COMM_PACK(*rit, vert);
      }
  }
  PCU_Comm_Send();
  while (PCU_Comm_Receive()) {
    int gid;
    PCU_COMM_UNPACK(gid);
    MeshEntity* remote;
    PCU_COMM_UNPACK(remote);
    int from = PCU_Comm_Sender();
    MeshEntity* vert = globalToVert[gid];
    m->addRemote(vert, from, remote);
  }
}

void construct(Mesh2* m, const int* conn, int nelem, int etype,
    GlobalToVert& globalToVert)
{
  constructVerts(m, conn, nelem, etype, globalToVert);
  constructElements(m, conn, nelem, etype, globalToVert);
  constructResidence(m, globalToVert);
  constructRemotes(m, globalToVert);
  stitchMesh(m);
  m->acceptChanges();
}

void setCoords(Mesh2* m, const double* coords, int nverts,
    GlobalToVert& globalToVert)
{
  Gid max = getMax(globalToVert);
  Gid total = max + 1;
  int peers = PCU_Comm_Peers();
  int quotient = total / peers;
  int remainder = total % peers;
  int mySize = quotient;
  int self = PCU_Comm_Self();
  if (self == (peers - 1))
    mySize += remainder;
  int myOffset = self * quotient;

  /* Force each peer to have exactly mySize verts.
     This means we might need to send and recv some coords */
  double* c = new double[mySize*3];

  int start = PCU_Exscan_Int(nverts);

  PCU_Comm_Begin();
  int to = std::min(peers - 1, start / quotient);
  int n = std::min((to+1)*quotient-start, nverts);
  while (nverts > 0) {
    PCU_COMM_PACK(to, start);
    PCU_COMM_PACK(to, n);
    PCU_Comm_Pack(to, coords, n*3*sizeof(double));

    nverts -= n;
    start += n;
    coords += n*3;
    to = std::min(peers - 1, to + 1);
    n = std::min(quotient, nverts);
  }
  PCU_Comm_Send();
  while (PCU_Comm_Receive()) {
    PCU_COMM_UNPACK(start);
    PCU_COMM_UNPACK(n);
    PCU_Comm_Unpack(&c[(start - myOffset) * 3], n*3*sizeof(double));
  }

  /* Tell all the owners of the coords what we need */
  typedef std::vector< std::vector<int> > TmpParts;
  TmpParts tmpParts(mySize);
  PCU_Comm_Begin();
  APF_CONST_ITERATE(GlobalToVert, globalToVert, it) {
    int gid = it->first;
    int to = std::min(peers - 1, gid / quotient);
    PCU_COMM_PACK(to, gid);
  }
  PCU_Comm_Send();
  while (PCU_Comm_Receive()) {
    int gid;
    PCU_COMM_UNPACK(gid);
    int from = PCU_Comm_Sender();
    tmpParts.at(gid - myOffset).push_back(from);
  }

  /* Send the coords to everybody who want them */
  PCU_Comm_Begin();
  for (int i = 0; i < mySize; ++i) {
    std::vector<int>& parts = tmpParts[i];
    for (size_t j = 0; j < parts.size(); ++j) {
      int to = parts[j];
      int gid = i + myOffset;
      PCU_COMM_PACK(to, gid);
      PCU_Comm_Pack(to, &c[i*3], 3*sizeof(double));
    }
  }
  PCU_Comm_Send();
  while (PCU_Comm_Receive()) {
    int gid;
    PCU_COMM_UNPACK(gid);
    double v[3];
    PCU_Comm_Unpack(v, sizeof(v));
    Vector3 vv(v);
    m->setPoint(globalToVert[gid], 0, vv);
  }

  delete [] c;
}

void setMatches(Mesh2* m, const int* matches, int nverts,
    GlobalToVert& globalToVert)
{
  PCU_Debug_Open();
  Gid max = getMax(globalToVert);
  Gid total = max + 1;
  int peers = PCU_Comm_Peers();
  int quotient = total / peers;
  int remainder = total % peers;
  int mySize = quotient;
  int self = PCU_Comm_Self();
  if (self == (peers - 1))
    mySize += remainder;
  int myOffset = self * quotient;

  /* Force each peer to have exactly mySize verts.
     This means we might need to send and recv some matches */
  int* c = new int[mySize];
  PCU_Debug_Print("%d mysize %d\n", self, mySize);

  int start = PCU_Exscan_Int(nverts);

  PCU_Comm_Begin();
  int to = std::min(peers - 1, start / quotient);
  int n = std::min((to+1)*quotient-start, nverts);
  while (nverts > 0) {
    PCU_COMM_PACK(to, start);
    PCU_COMM_PACK(to, n);
    PCU_Comm_Pack(to, matches, n*sizeof(int));
    PCU_Debug_Print("%d sending start %d n %d to %d\n",
        self, start, n, to);

    nverts -= n;
    start += n;
    matches += n;
    to = std::min(peers - 1, to + 1);
    n = std::min(quotient, nverts);
  }
  PCU_Comm_Send();
  while (PCU_Comm_Receive()) {
    PCU_COMM_UNPACK(start);
    PCU_COMM_UNPACK(n);
    PCU_Comm_Unpack(&c[(start - myOffset)], n*sizeof(int));
    PCU_Debug_Print("%d reciving start %d n %d from %d\n",
        self, start, n, PCU_Comm_Sender());
  }

  for (int i = 0; i < mySize; ++i) {
    int match = c[i];
    if( match != -1 )
      PCU_Debug_Print("%d found match %d at gid %d\n",
          self, match, i+myOffset);
  }

  /* Tell all the owners of the matches what we need */
  typedef std::vector< std::vector<int> > TmpParts;
  TmpParts tmpParts(mySize);
  PCU_Comm_Begin();
  APF_CONST_ITERATE(GlobalToVert, globalToVert, it) {
    int gid = it->first;
    int to = std::min(peers - 1, gid / quotient);
    PCU_COMM_PACK(to, gid);
    PCU_Debug_Print("%d requesting matches of gid %d isShared %d isOwned %d from %d\n",
        self, gid, m->isShared(it->second), m->isOwned(it->second), to);
  }
  PCU_Comm_Send();
  while (PCU_Comm_Receive()) {
    int gid;
    PCU_COMM_UNPACK(gid);
    int from = PCU_Comm_Sender();
    tmpParts.at(gid - myOffset).push_back(from);
  }
 
  MeshTag* matchGidTag = m->createIntTag("matchGids", 1);
  /* Send the matches to everybody who wants them */
  PCU_Comm_Begin();
  for (int i = 0; i < mySize; ++i) {
    std::vector<int>& parts = tmpParts[i];
    for (size_t j = 0; j < parts.size(); ++j) {
      int to = parts[j];
      int gid = i + myOffset;
      int matchGid = c[i];
      PCU_COMM_PACK(to, gid);
      PCU_COMM_PACK(to, matchGid);
      if( matchGid != -1 ) {
        PCU_Debug_Print("%d packing i %d gid %d matchGid %d to %d\n",
            self, i, gid, matchGid, to);
      }
    }
  }
  PCU_Comm_Send();
  while (PCU_Comm_Receive()) {
    int gid;
    PCU_COMM_UNPACK(gid);
    int match;
    PCU_COMM_UNPACK(match);
    PCU_ALWAYS_ASSERT(gid != match);
    PCU_ALWAYS_ASSERT(globalToVert.count(gid));
    m->setIntTag(globalToVert[gid], matchGidTag, &match);
    if( match != -1 ) {
      PCU_Debug_Print("%d attaching match %d to gid %d\n",
          self, match, gid);
    }
  }

  /* Use the 1D partitioning of global ids to distribute the 
   * entity pointers and their owning ranks. Process 0 will hold
   * the entity pointers and owners for mesh vertex gid [0..quotient),
   * process 1 holds gids [quotient..2*quotient), ...
   */
  PCU_Comm_Begin();
  APF_CONST_ITERATE(GlobalToVert, globalToVert, it) {
    MeshEntity* e = it->second;
    int gid = it->first;
    int to = std::min(peers - 1, gid / quotient);
    PCU_COMM_PACK(to, gid);
    PCU_COMM_PACK(to, e);
    PCU_Debug_Print("%d packing pointer to %d gid %d vert %p\n",
        self, to, gid, (void*)e);
  }
  PCU_Comm_Send();
  typedef std::pair< int, apf::MeshEntity* > EntOwnerPtrs;
  typedef std::map< int, std::vector< EntOwnerPtrs > > GidPtrs;
  GidPtrs gidPtrs;
  while (PCU_Comm_Receive()) {
    int gid;
    PCU_COMM_UNPACK(gid);
    MeshEntity* vert;
    PCU_COMM_UNPACK(vert);
    int owner = PCU_Comm_Sender();
    gidPtrs[gid-myOffset].push_back(EntOwnerPtrs(owner,vert));
    PCU_Debug_Print("%d unpacking pointer from %d gid %d vert %p\n",
        self, owner, gid, (void*)vert);
  }

  /* Tell the brokers of the matches we need */
  typedef std::pair<int,int> MatchingPair;
  typedef std::map< MatchingPair, std::vector<int> > MatchMap;
  MatchMap matchParts;
  PCU_Comm_Begin();
  APF_CONST_ITERATE(GlobalToVert, globalToVert, it) { //loop over local verts
    int gid = it->first;
    int matchGid;
    m->getIntTag(it->second, matchGidTag, &matchGid); //get the matched ent gid
    if( matchGid != -1 ) {  // marker for an unmatched vertex
      int to = std::min(peers - 1, matchGid / quotient); // broker
      PCU_COMM_PACK(to, gid); // send the local vert gid
      PCU_COMM_PACK(to, matchGid); // and the match gid needed
      PCU_Debug_Print("%d packing req ptr to %d gid %d matchGid %d\n",
          self, to, gid, matchGid);
    }
  }
  PCU_Comm_Send();
  while (PCU_Comm_Receive()) {
    int gid;
    PCU_COMM_UNPACK(gid); // request from entity gid
    int matchGid;
    PCU_COMM_UNPACK(matchGid); // requesting matched entity gid 
    MatchingPair mp(gid,matchGid);
    int from = PCU_Comm_Sender();
    matchParts[mp].push_back(from); // store a list of the proceses that need the pair (entity gid, match gid)
    PCU_Debug_Print("%d unpacking ptr req from %d gid %d matchGid %d\n",
        self, from, gid, matchGid);
  }

  /* Send the match pointer and owner process to everybody
   * who wants them */
  PCU_Comm_Begin();
  APF_ITERATE(MatchMap,matchParts,it) {
    MatchingPair mp = it->first;
    int gid = mp.first;
    int matchGid = mp.second;
    std::vector<int> parts = it->second;
    for(size_t i=0; i<parts.size(); i++) {
      const int to = parts[i];
      PCU_COMM_PACK(to, gid);
      PCU_COMM_PACK(to, matchGid);
      size_t numMatches = gidPtrs[matchGid-myOffset].size();
      PCU_COMM_PACK(to, numMatches);
      for( size_t i=0; i<gidPtrs[matchGid-myOffset].size(); i++) {
        EntOwnerPtrs eop = gidPtrs[matchGid-myOffset][i];
        int owner = eop.first;
        PCU_COMM_PACK(to, owner);
        apf::MeshEntity* ent = eop.second;
        PCU_COMM_PACK(to, ent);
        PCU_Debug_Print("%d packing match ptr to %d gid %d matchGid %d vert %p owner %d\n",
            self, to, gid, matchGid, (void*)ent, owner);
      }
    }
  }
  PCU_Comm_Send();
  while (PCU_Comm_Receive()) {
    int gid;
    PCU_COMM_UNPACK(gid);
    int matchGid;
    PCU_COMM_UNPACK(matchGid);
    size_t numMatches;
    PCU_COMM_UNPACK(numMatches);
    for(size_t i=0; i<numMatches; i++) {
      int owner;
      PCU_COMM_UNPACK(owner);
      MeshEntity* match;
      PCU_COMM_UNPACK(match);
      PCU_Debug_Print("%d unpacked match ptr from %d gid %d matchGid %d matchPtr %p owner %d\n",
          self, PCU_Comm_Sender(), gid, matchGid, (void*)match, owner);
      PCU_ALWAYS_ASSERT(globalToVert.count(gid));
      MeshEntity* partner = globalToVert[gid];
      if(match == partner && owner == self) {
        PCU_Debug_Print("%d match == partner owner == self match %p partner %p owner %d\n",
            self, (void*)match, (void*)partner, owner);
      }
      PCU_ALWAYS_ASSERT(! (match == partner && owner == self) );
      m->addMatch(partner, owner, match);
    }
  }

  APF_CONST_ITERATE(GlobalToVert, globalToVert, it) { //loop over local verts
    apf::MeshEntity* left = it->second;
    int matchGid;
    m->getIntTag(left, matchGidTag, &matchGid); //get the matched ent gid
    if( matchGid != -1 ) {  // a matched vtx
      apf::Copies copies;
      m->getRemotes(left,copies);
      APF_ITERATE(apf::Copies, copies, cp) {
        int rightPart = cp->first;
        apf::MeshEntity* right = cp->second;
        m->addMatch(left, rightPart, right);
        PCU_Debug_Print("%d add remote copy match ptr to %d gid %d\n",
          self, rightPart, it->first);
      }
    }
  }

  delete [] c;

  apf::removeTagFromDimension(m, matchGidTag, 0);
  m->destroyTag(matchGidTag);
}

void destruct(Mesh2* m, int*& conn, int& nelem, int &etype)
{
  int dim = m->getDimension();
  nelem = m->count(dim);
  conn = 0;
  GlobalNumbering* global = makeGlobal(numberOwnedNodes(m, "apf_destruct"));
  synchronize(global);
  MeshIterator* it = m->begin(dim);
  MeshEntity* e;
  int i = 0;
  while ((e = m->iterate(it))) {
    etype = m->getType(e);
    Downward verts;
    int nverts = m->getDownward(e, 0, verts);
    if (!conn)
      conn = new Gid[nelem * nverts];
    for (int j = 0; j < nverts; ++j)
      conn[i++] = getNumber(global, Node(verts[j], 0));
  }
  m->end(it);
  destroyGlobalNumbering(global);
}

void extractCoords(Mesh2* m, double*& coords, int& nverts)
{
  nverts = countOwned(m, 0);
  coords = new double[nverts * 3];

  MeshIterator* it = m->begin(0);
  int i = 0;
  while (MeshEntity* v = m->iterate(it)) {
    if (m->isOwned(v)) {
      Vector3 p;
      m->getPoint(v, 0, p);
      p.toArray(&coords[i*3]);
      i++;
    }
  }
  m->end(it);
}

}
