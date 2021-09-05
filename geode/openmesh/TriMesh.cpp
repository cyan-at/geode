#include <geode/config.h>
#ifdef GEODE_OPENMESH
#include <geode/openmesh/TriMesh.h>
#include <geode/openmesh/color_cast.h>
#include <geode/array/Field.h>
#include <geode/array/view.h>
#include <geode/utility/prioritize.h>
#include <geode/python/Class.h>
#include <geode/math/isfinite.h>
#include <geode/math/copysign.h>
#include <geode/structure/UnionFind.h>
#include <geode/geometry/SimplexTree.h>
#include <geode/geometry/ParticleTree.h>
#include <geode/geometry/RayIntersection.h>
#include <geode/utility/path.h>
#include <geode/vector/Rotation.h>
#include <geode/utility/convert_case.h>
#include <geode/utility/stl.h>
#include <geode/openmesh/triangulator.h>
#include <geode/vector/Frame.h>
#include <geode/vector/convert.h>
#include <geode/array/NdArray.h>
#include <iostream>
#include <queue>
namespace geode {

using std::map;
using std::cout;
using std::cerr;
using std::endl;
using std::pair;
using std::make_pair;
typedef Vector<real,2> TV2;

GEODE_DEFINE_TYPE(TriMesh)

#ifdef GEODE_PYTHON
namespace {
// for vector conversions
template<> struct NumpyIsScalar<OpenMesh::BaseHandle>:public mpl::true_{};
template<> struct NumpyIsScalar<OpenMesh::VertexHandle>:public mpl::true_{};
template<> struct NumpyIsScalar<OpenMesh::EdgeHandle>:public mpl::true_{};
template<> struct NumpyIsScalar<OpenMesh::HalfedgeHandle>:public mpl::true_{};
template<> struct NumpyIsScalar<OpenMesh::FaceHandle>:public mpl::true_{};

template<> struct NumpyScalar<OpenMesh::BaseHandle>{enum{value=NPY_INT};};
template<> struct NumpyScalar<OpenMesh::VertexHandle>{enum{value=NPY_INT};};
template<> struct NumpyScalar<OpenMesh::EdgeHandle>{enum{value=NPY_INT};};
template<> struct NumpyScalar<OpenMesh::HalfedgeHandle>{enum{value=NPY_INT};};
template<> struct NumpyScalar<OpenMesh::FaceHandle>{enum{value=NPY_INT};};
}
#endif

GEODE_DEFINE_VECTOR_CONVERSIONS(GEODE_CORE_EXPORT,2,VertexHandle)
GEODE_DEFINE_VECTOR_CONVERSIONS(GEODE_CORE_EXPORT,3,VertexHandle)
GEODE_DEFINE_VECTOR_CONVERSIONS(GEODE_CORE_EXPORT,2,FaceHandle)
GEODE_DEFINE_VECTOR_CONVERSIONS(GEODE_CORE_EXPORT,3,FaceHandle)
GEODE_DEFINE_VECTOR_CONVERSIONS(GEODE_CORE_EXPORT,3,HalfedgeHandle)

TriMesh::TriMesh() {}

TriMesh::TriMesh(const TriMesh& m)
  : OTriMesh(m) {}

TriMesh::TriMesh(RawArray<const Vector<int,3> > tris, RawArray<const Vector<real,3> > X) {
  if (tris.size())
    GEODE_ASSERT(scalar_view(tris).max()<X.size());
  for (const Vector<real,3>& x : X)
    add_vertex(x);
  for (const Vector<int,3>& tri : tris)
    GEODE_ASSERT(add_face(VertexHandle(tri.x),VertexHandle(tri.y),VertexHandle(tri.z)).is_valid());
}

TriMesh::TriMesh(Tuple<Ref<TriangleSoup>,Array<Vector<real,3>>> const &in) {
  RawArray<const Vector<int,3> > tris = in.x->elements;
  RawArray<const Vector<real,3> > X = in.y;
  if (tris.size())
    GEODE_ASSERT(scalar_view(tris).max()<X.size());
  for (const Vector<real,3>& x : X)
    add_vertex(x);
  for (const Vector<int,3>& tri : tris)
    GEODE_ASSERT(add_face(VertexHandle(tri.x),VertexHandle(tri.y),VertexHandle(tri.z)).is_valid());
}

TriMesh::~TriMesh() {
}

TriMesh &TriMesh::operator=(OTriMesh const &m) {
  OTriMesh::operator=(m);
  return *this;
}

Ref<TriMesh> TriMesh::copy() const {
  Ref<TriMesh> m = new_<TriMesh>(*this);
  return m;
}

// load from file/stream
void TriMesh::read(string const &filename) {
  OMSilencer silencer;
  if (!OpenMesh::IO::read_mesh(*this,filename))
    throw IOError(format("TriMesh::read: failed to read mesh '%s'",filename));
}

void TriMesh::read(istream &is, string const &ext) {
  OMSilencer silencer;
  OpenMesh::IO::Options opt;
  if (!OpenMesh::IO::read_mesh(*this,is,ext,opt))
    throw IOError(format("TriMesh::read: failed to read mesh from stream, type '%s'",ext));
}

namespace {
// For some reason, OpenMesh::IO::write_mesh turns on log output upon completion.  Fight this.
struct Disabler {
  bool enabled;
  Disabler() : enabled(omlog().is_enabled()) {}
  ~Disabler() {
    if (!enabled)
      omlog().disable();
  }
};
}

static OpenMesh::IO::Options write_options(const TriMesh& mesh, const string& ext, bool normals=false) {
  OpenMesh::IO::Options options = OpenMesh::IO::Options::Default;
  if (ext==".ply" || ext==".stl")
    options = options | OpenMesh::IO::Options::Binary;
  if (mesh.has_vertex_colors() && mesh.n_vertices())
    options.set(OpenMesh::IO::Options::VertexColor);
  if (normals && mesh.has_vertex_normals())
    options.set(OpenMesh::IO::Options::VertexNormal);
  if (ext!=".stl" && mesh.has_face_colors() && mesh.n_faces())
    options.set(OpenMesh::IO::Options::FaceColor);
  return options;
}

static void write_helper(const TriMesh& mesh, const string& filename, bool normals) {
  //OMSilencer silencer;
  //Disabler disabler;
  const auto ext = to_lower(path::extension(filename));
  if (!OpenMesh::IO::write_mesh(mesh, filename, write_options(mesh,ext,normals)))
    throw IOError(format("TriMesh::write: failed to write mesh to file '%s'", filename));
}

void TriMesh::write(string const &filename) const {
  write_helper(*this,filename,false);
}

void TriMesh::write_with_normals(string const &filename) const {
  write_helper(*this,filename,true);
}

void TriMesh::write(std::ostream &os, string const &extension) const {
  OMSilencer silencer;
  Disabler disabler;
  const auto ext = to_lower(extension);
  if (!OpenMesh::IO::write_mesh(*this, os, ext, write_options(*this,ext)))
    throw IOError("TriMesh::write: failed to write mesh to stream");
}

void TriMesh::add_vertices(RawArray<const TV> X) {
  for (const TV& x : X)
    GEODE_ASSERT(add_vertex(x).is_valid());
}

// add a bunch of faces
void TriMesh::add_faces(RawArray<const Vector<int,3> > faces) {
  GEODE_ASSERT(!faces.size() || scalar_view(faces).max()<(int)n_vertices());
  for (const Vector<int,3>& face : faces)
    add_face(VertexHandle(face.x),VertexHandle(face.y),VertexHandle(face.z));
}

void TriMesh::add_mesh(const TriMesh& mesh) {
  // Copy vertices
  const int n = n_vertices();
  add_vertices(mesh.X());

  // Copy per vertex texture coordinates if present in both meshes
  if (has_vertex_texcoords2D() && mesh.has_vertex_texcoords2D())
    for (const auto v : mesh.vertex_handles())
      set_texcoord2D(vertex_handle(v.idx()+n),mesh.texcoord2D(v));

  // Copy vertex colors if present in both meshes
  if (has_vertex_colors() && mesh.has_vertex_colors())
    for (const auto v : mesh.vertex_handles())
      set_color(vertex_handle(v.idx()+n),mesh.color(v));

  // Copy faces
  add_faces((mesh.elements()+n).copy());

  // Copy halfedge texture coordinates if present in both meshes
  if (has_halfedge_texcoords2D() && mesh.has_halfedge_texcoords2D())
    for (const auto e : mesh.halfedge_handles())
      set_texcoord2D(halfedge_handle(VertexHandle(mesh.from_vertex_handle(e).idx()+n),
                                     VertexHandle(mesh.  to_vertex_handle(e).idx()+n)),mesh.texcoord2D(e));
}

Box<Vector<real,3> > TriMesh::bounding_box() const {
  return geode::bounding_box(X());
}

Box<Vector<real,3> > TriMesh::bounding_box(vector<FaceHandle> const &faces) const {
  Box<Vector<real,3> > box;
  for (FaceHandle fh : faces) {
    Vector<VertexHandle, 3> vh = vertex_handles(fh);
    box.enlarge(point(vh.x));
    box.enlarge(point(vh.y));
    box.enlarge(point(vh.z));
  }
  return box;
}

Vector<real,3> TriMesh::centroid() const {
  Vector<real,3> c;
  double s = 0;
  for (TriMesh::ConstVertexIter vi = vertices_sbegin(); vi != vertices_end(); ++vi) {
    double w = 0;
    for (TriMesh::ConstVertexFaceIter vf = cvf_iter(vi); vf; ++vf) {
      w += triangle(vf.handle()).area();
    }
    w /= 3.;
    c += w * point(vi.handle());
    s += w;
  }

  return c/s;
}

Vector<real,3> TriMesh::centroid(FaceHandle fh) const {
  return triangle(fh).center();
}

real TriMesh::mean_edge_length() const {
  real result = 0;
  int count = 0;

  for (TriMesh::ConstEdgeIter it = edges_sbegin(); it != edges_end(); ++it) {
    count++;
    result += calc_edge_length(it.handle());
  }

  return result / count;
}

real TriMesh::area(FaceHandle fh) const {
  return calc_sector_area(halfedge_handle(fh));
}

Triangle<Vector<real, 3> > TriMesh::triangle(FaceHandle fh) const {
  Vector<VertexHandle, 3> vh = vertex_handles(fh);
  return Triangle<Vector<real, 3> >(point(vh[0]), point(vh[1]), point(vh[2]));
}

Segment<Vector<real, 3> > TriMesh::segment(EdgeHandle eh) const {
  Vector<VertexHandle, 2> vh = vertex_handles(eh);
  return Segment<Vector<real, 3> >(point(vh[0]), point(vh[1]));
}

Segment<Vector<real, 3> > TriMesh::segment(HalfedgeHandle heh) const {
  return Segment<Vector<real, 3> >(point(from_vertex_handle(heh)), point(to_vertex_handle(heh)));
}

real TriMesh::cotan_weight(EdgeHandle eh) const {

  auto he0 = halfedge_handle(eh, 0);
  auto he1 = halfedge_handle(eh, 1);
  auto v0 = to_vertex_handle(he0);
  auto v1 = to_vertex_handle(he1);
  auto p0 = point(v0);
  auto p1 = point(v1);

  real weight = 0.;

  auto p2 = point(opposite_vh(he0));
  auto d0 = (p0 - p2).normalized();
  auto d1 = (p1 - p2).normalized();
  weight += 1.0 / tan(acos(std::max(-1., std::min(1., dot(d0,d1)))));

  p2 = point(opposite_vh(he1));
  d0 = (p0 - p2).normalized();
  d1 = (p1 - p2).normalized();
  weight += 1.0 / tan(acos(std::max(-1., std::min(1., dot(d0,d1)))));

  return weight;
}

Vector<VertexHandle, 2> TriMesh::vertex_handles(HalfedgeHandle heh) const {
  return Vector<VertexHandle,2>(from_vertex_handle(heh),to_vertex_handle(heh));
}

Vector<VertexHandle, 2> TriMesh::vertex_handles(EdgeHandle eh) const {
  return vertex_handles(halfedge_handle(eh,0));
}

Vector<VertexHandle, 3> TriMesh::vertex_handles(FaceHandle fh) const {
  HalfedgeHandle h0 = halfedge_handle(fh),
                 h1 = next_halfedge_handle(h0);
  return Vector<VertexHandle, 3>(to_vertex_handle(h0),
                                 to_vertex_handle(h1),
                                 from_vertex_handle(h0));
}

Vector<EdgeHandle,3> TriMesh::edge_handles(FaceHandle fh) const {
  Vector<HalfedgeHandle, 3> he = halfedge_handles(fh);
  return vec(edge_handle(he.x), edge_handle(he.y), edge_handle(he.z));
}

Vector<HalfedgeHandle,2> TriMesh::halfedge_handles(EdgeHandle eh) const {
  return vec(halfedge_handle(eh, 0), halfedge_handle(eh, 1));
}

Vector<HalfedgeHandle,3> TriMesh::halfedge_handles(FaceHandle fh) const {
  HalfedgeHandle h0 = halfedge_handle(fh),
                 h1 = next_halfedge_handle(h0),
                 h2 = next_halfedge_handle(h1);
  return vec(h0,h1,h2);
}

Vector<FaceHandle, 2> TriMesh::face_handles(EdgeHandle eh) const {
  return vec(face_handle(halfedge_handle(eh, 0)), face_handle(halfedge_handle(eh, 1)));
}

Vector<FaceHandle,3> TriMesh::face_handles(FaceHandle fh) const {
  Vector<FaceHandle,3> v;
  int i = 0;
  for (CFFIter it = cff_iter(fh); it; ++it) {
    v[i] = it.handle();
    i++;
  }
  return v;
}


FaceHandle TriMesh::valid_face_handle(EdgeHandle eh) const {
  Vector<FaceHandle, 2> fh = face_handles(eh);

  if (fh.x.is_valid())
    return fh.x;
  else
    return fh.y;
}

bool TriMesh::quad_convex(EdgeHandle eh) const {

  if (is_boundary(eh)) {
    return false;
  }

  HalfedgeHandle heh1 = halfedge_handle(eh, 0);
  HalfedgeHandle heh2 = halfedge_handle(eh, 1);
  HalfedgeHandle heh3 = prev_halfedge_handle(heh1);
  HalfedgeHandle heh4 = prev_halfedge_handle(heh2);

  return calc_sector_angle(heh1) + calc_sector_angle(heh4) < M_PI &&
    calc_sector_angle(heh2) + calc_sector_angle(heh3) < M_PI;
}

vector<VertexHandle> TriMesh::vertex_one_ring(VertexHandle vh) const {
  vector<VertexHandle> v;
  for (ConstVertexVertexIter it = cvv_iter(vh); it; ++it) {
    v.push_back(it.handle());
  }
  return v;
}

vector<HalfedgeHandle> TriMesh::outgoing_halfedges(VertexHandle vh) const {
  auto vr = vertex_one_ring(vh);
  vector<HalfedgeHandle> h;
  for( auto v : vr)
    h.push_back(halfedge_handle(vh,v));
  return h;
}

vector<HalfedgeHandle> TriMesh::incoming_halfedges(VertexHandle vh) const {
  auto vr = vertex_one_ring(vh);
  vector<HalfedgeHandle> h;
  for( auto v : vr)
    h.push_back(halfedge_handle(v,vh));
  return h;
}


vector<FaceHandle> TriMesh::incident_faces(VertexHandle vh) const {
  vector<FaceHandle> fh;

  for (TriMesh::ConstVertexFaceIter vf = cvf_iter(vh); vf; ++vf) {
    fh.push_back(vf.handle());
  }

  return fh;
}

EdgeHandle TriMesh::common_edge(FaceHandle fh, FaceHandle fh2) const {
  for (ConstFaceHalfedgeIter e = cfh_iter(fh); e; ++e)
    if (opposite_face_handle(e) == fh2)
      return edge_handle(e.handle());
  return TriMesh::InvalidEdgeHandle;
}

HalfedgeHandle TriMesh::halfedge_handle(VertexHandle v1, VertexHandle v2) const {
  if (valence(v1) < 2) {
    return TriMesh::InvalidHalfedgeHandle;
  }

  for (TriMesh::ConstVertexOHalfedgeIter it = cvoh_iter(v1); it; ++it)
    if (to_vertex_handle(it) == v2)
      return it.handle();
  return TriMesh::InvalidHalfedgeHandle;
}

HalfedgeHandle TriMesh::halfedge_handle(FaceHandle fh, VertexHandle vh) const {
  for (TriMesh::ConstFaceHalfedgeIter fi = cfh_iter(fh); fi; ++fi) {
    if (to_vertex_handle(fi.handle()) == vh) {
      return fi.handle();
    }
  }
  return TriMesh::InvalidHalfedgeHandle;
}

EdgeHandle TriMesh::edge_handle(VertexHandle v1, VertexHandle v2) const {
  if (valence(v1) < 2) {
    return TriMesh::InvalidEdgeHandle;
  }

  for (TriMesh::ConstVertexOHalfedgeIter it = cvoh_iter(v1); it; ++it)
    if (to_vertex_handle(it) == v2)
      return edge_handle(it.handle());
  return TriMesh::InvalidEdgeHandle;
}

TriMesh::Normal TriMesh::normal(EdgeHandle eh) const {
  FaceHandle fh1 = face_handle(halfedge_handle(eh, 0));
  FaceHandle fh2 = face_handle(halfedge_handle(eh, 1));

  assert(fh1.is_valid() || fh2.is_valid());

  if (!fh1.is_valid()) {
    return normal(fh2);
  } else if (!fh2.is_valid()) {
    return normal(fh1);
  }

  Normal n1 = normal(fh1);
  Normal n2 = normal(fh2);

  Normal n = n1 + n2;

  if (n.length() < 1e-10) {
    // get a normal from the face and edge
    Point p1 = point(from_vertex_handle(halfedge_handle(eh, 0)));
    Point p2 = point(to_vertex_handle(halfedge_handle(eh, 1)));
    n = cross(n1, p2-p1).normalized();
    return -n;
  } else {
    return n.normalized();
  }
}

TriMesh::Point TriMesh::point(FaceHandle fh, Vector<real,3> const &bary) const {
  Vector<VertexHandle, 3> vh = vertex_handles(fh);
  TriMesh::Point p(0., 0., 0.);
  for (int i = 0; i < 3; ++i) {
    p += bary[i] * point(vh[i]);
  }
  return p;
}

TriMesh::Normal TriMesh::smooth_normal(FaceHandle fh,
                                       Vector<real, 3> const &bary) const {
  Vector<VertexHandle, 3> vh = vertex_handles(fh);

  assert(has_vertex_normals());

  TriMesh::Normal n(0., 0., 0.);
  for (int i = 0; i < 3; ++i) {
    n += bary[i] * normal(vh[i]);
  }

  return n;
}

unordered_map<VertexHandle, VertexHandle, Hasher> TriMesh::garbage_collection_with_map() {
  OpenMesh::VPropHandleT<VertexHandle> oldid;
  add_property(oldid);

  for (auto vh : vertex_handles()) {
    property(oldid, vh) = vh;
  }

  garbage_collection();

  unordered_map<VertexHandle, VertexHandle, Hasher> old_to_new;

  for (auto vh : vertex_handles()) {
    old_to_new[property(oldid,vh)] = vh;
  }

  remove_property(oldid);

  return old_to_new;
}

int TriMesh::remove_infinite_vertices() {
  int removed = 0;
  for (auto v : vertex_handles()) {
    if (!isfinite(point(v))) {
      removed++;
      delete_vertex(v);
    }
  }
  garbage_collection();
  return removed;
}

vector<FaceHandle> TriMesh::triangle_fan(vector<VertexHandle> const &ring, VertexHandle center, bool closed) {
  // make a triangle fan, possibly closed, around a given node
  vector<FaceHandle> fh;

  int end = (int)ring.size()-!closed;

  for (int j = 0; j < end; ++j) {
    // this fails if the mesh is not manifold, or if the point order is not appropriate
    TriMesh::FaceHandle face = add_face(center, ring[j], ring[(j+1) % ring.size()]);

    if (face == TriMesh::InvalidFaceHandle) {
      cerr << "WARNING: tried inserting a non-manifold face: " << center << ", " << ring[j] << ", " << ring[(j+1) % ring.size()] << endl;
    }

    fh.push_back(face);
  }

  return fh;
}

vector<FaceHandle> TriMesh::select_faces(function<bool(FaceHandle)> pr) const {
  vector<FaceHandle> result;
  for (FaceHandle fh : face_handles()) {
    if (pr(fh))
      result.push_back(fh);
  }
  return result;
}


Ref<TriMesh> TriMesh::extract_faces(vector<FaceHandle> const &faces,
                                    unordered_map<VertexHandle, VertexHandle, Hasher> &id2id) const {
  Ref<TriMesh> out = new_<TriMesh>();

  id2id.clear();
  for (vector<FaceHandle>::const_iterator it = faces.begin(); it != faces.end(); ++it) {
    vector<VertexHandle> verts;
    for (ConstFaceVertexIter fv = cfv_iter(*it); fv; ++fv) {
      if (!id2id.count(fv.handle()))
        id2id[fv.handle()] = out->add_vertex(point(fv.handle()));

      verts.push_back(id2id[fv.handle()]);
    }
    out->add_face(verts);
  }

  return out;
}

Ref<TriMesh> TriMesh::extract_faces(vector<FaceHandle> const &faces) const {
  unordered_map<VertexHandle, VertexHandle, Hasher> id2id;
  return extract_faces(faces, id2id);
}

Ref<TriMesh> TriMesh::inverse_extract_faces(vector<FaceHandle> const &faces,
                                            unordered_map<VertexHandle, VertexHandle, Hasher> &id2id) const {
  Ref<TriMesh> out = new_<TriMesh>();

  id2id.clear();
  for (ConstFaceIter it = faces_sbegin(); it != faces_end(); ++it) {
    if(!contains(faces,it.handle())){
      vector<VertexHandle> verts;
      for (ConstFaceVertexIter fv = cfv_iter(it.handle()); fv; ++fv) {
        if (!id2id.count(fv.handle())) {
          id2id[fv.handle()] = out->add_vertex(point(fv.handle()));
        }

      verts.push_back(id2id[fv.handle()]);
      }
      out->add_face(verts);
    }
  }

  return out;
}

Ref<TriMesh> TriMesh::inverse_extract_faces(vector<FaceHandle> const &faces) const {
  unordered_map<VertexHandle, VertexHandle, Hasher> id2id;
  return inverse_extract_faces(faces, id2id);
}

Nested<TV2> TriMesh::silhouette(const Rotation<TV>& rotation) const {
  GEODE_ASSERT(has_face_normals());
  GEODE_ASSERT(!has_boundary());
  const TV up = rotation.z_axis();

  // Classify silhouette halfedges: those that are above, but whose opposites are below
  Array<HalfedgeHandle> unused;
  Hashtable<HalfedgeHandle> unused_set;
  for (auto e : edge_handles()) {
    const auto h0 = halfedge_handle(e,0),
               h1 = halfedge_handle(e,1);
    const bool above0 = dot(normal(face_handle(h0)),up)>=0,
               above1 = dot(normal(face_handle(h1)),up)>=0;
    if (above0 != above1) {
      const auto h = above0?h0:h1;
      unused.append(h);
      unused_set.set(h);
    }
  }

  // Walk loops until we have no edges left
  Array<int> offsets;
  offsets.append(0);
  Array<TV2> flat;
  while (unused.size()) {
    // Grab one and walk as far as possible
    const auto start = unused.pop();
    if (!unused_set.erase(start))
      continue;
    auto edge = start;
    for (;;) {
      auto next = next_halfedge_handle(edge);
      while (!unused_set.erase(next) && next!=start) {
        next = next_halfedge_handle(opposite_halfedge_handle(next));
        assert(edge_handle(next)!=edge_handle(edge)); // Implies something nonmanifold is happening
      }
      flat.append(rotation.inverse_times(point(from_vertex_handle(next))).xy());
      if (next==start)
        break;
      edge = next;
    }
    offsets.append(flat.size());
  }
  return Nested<TV2>(offsets,flat);
}

unordered_set<HalfedgeHandle, Hasher> TriMesh::boundary_of(vector<FaceHandle> const &faces) const {

  unordered_set<EdgeHandle, Hasher> edgeboundary;
  unordered_set<HalfedgeHandle, Hasher> boundary;
  for (FaceHandle f : faces) {
    if (!f.is_valid())
      continue;

    for (ConstFaceHalfedgeIter he = cfh_iter(f); he; ++he) {
      if (edgeboundary.count(edge_handle(he.handle()))) {
        boundary.erase(opposite_halfedge_handle(he.handle()));
        edgeboundary.erase(edge_handle(he.handle()));
      } else {
        boundary.insert(he.handle());
        edgeboundary.insert(edge_handle(he.handle()));
      }
    }
  }

  return boundary;
}

unordered_map<VertexHandle, double, Hasher> TriMesh::geodesic_distance(VertexHandle source,
                                                                     VertexHandle sink) const {
  return geodesic_distance(source, vector<VertexHandle>(1, sink));
}

unordered_map<VertexHandle, double, Hasher> TriMesh::geodesic_distance(VertexHandle source,
                                                                     vector<VertexHandle> const &sinks) const {
return geodesic_distance(vector<VertexHandle>(1,source), sinks);
}

// compute the approximate geodesic distance from one point to a set of points, and store
// all values computed on the way (can be used to re-trace the approximate shortest paths)
unordered_map<VertexHandle, double, Hasher> TriMesh::geodesic_distance(vector<VertexHandle> const &sources,
                                                                     vector<VertexHandle> const &sinks) const {

  // initialize distance map and unassigned set
  unordered_map<VertexHandle, double, Hasher> dist;
  for(auto& vh : sources)
    dist[vh] = 0.;

  unordered_set<VertexHandle, Hasher> unassigned(sinks.begin(), sinks.end());
  for(auto& vh : sources)
    unassigned.erase(vh);

  //cout << "computing geodesic distance from " << source << " to " << sinks.size() << " sinks, " << unassigned.size() << " distances unassigned." << endl;

  if (unassigned.empty())
    return dist;

  std::priority_queue<Prioritize<VertexHandle>, vector<Prioritize<VertexHandle> >, std::greater<Prioritize<VertexHandle> > > queue;
  for(auto& vh : sources)
    queue.push(prioritize(vh, 0.));

  while (!queue.empty()) {

    VertexHandle current = queue.top().a;
    double d = queue.top().p;
    queue.pop();

    // assign distance if not set yet
    if (unassigned.count(current)) {
      dist[current] = d;
      unassigned.erase(current);

      // nothing else to do? leave.
      if (unassigned.empty()) {
        break;
      }
    }

    for (TriMesh::ConstVertexVertexIter vv = cvv_iter(current); vv; ++vv) {

      double l = (point(vv.handle()) - point(current)).magnitude();
      double newdist = d + l;
      assert(isfinite(newdist));

      double olddist = inf;
      if (dist.count(vv.handle())) {
        olddist = dist[vv.handle()];
      }

      if (newdist < olddist) {
        dist[vv.handle()] = newdist;
        queue.push(prioritize(vv.handle(), newdist));
      }
    }
  }

  return dist;
}

// compute and return the approximate shortest path from one point to another
vector<VertexHandle> TriMesh::vertex_shortest_path(VertexHandle source, VertexHandle sink) const {
  unordered_map<VertexHandle, double, Hasher> dist = geodesic_distance(sink, source);

  // walk backward from the sink to the source and record the shortest path
  vector<VertexHandle> path(1, source);

  while (path.back() != sink) {
    // find neighboring vertex which minimizes the distance
    double mind = inf;
    VertexHandle next = InvalidVertexHandle;
    for (TriMesh::ConstVertexVertexIter vv = cvv_iter(path.back()); vv; ++vv) {
      if (dist.count(vv.handle()) && dist[vv.handle()] < mind) {
        next = vv.handle();
        mind = dist[vv.handle()];
      }
    }

    assert(next.is_valid());
    path.push_back(next);
  }

  return path;
}

TriMesh::FaceHandle TriMesh::local_closest_face(TriMesh::Point const &p, FaceHandle start) const {

  //cout << "computing closest face for p = " << p << ", starting at " << start << endl;

  unordered_set<FaceHandle, Hasher> checked;

  FaceHandle current = start;
  real mind = triangle(current).distance(p);
  checked.insert(start);

  //cout << "  start at d = " << mind << endl;

  while (true) {
    unordered_set<FaceHandle, Hasher> next;
    for (ConstFaceVertexIter fv = cfv_iter(current); fv; ++fv) {
      for (ConstVertexFaceIter vf = cvf_iter(fv.handle()); vf; ++vf) {
        if (!checked.count(vf.handle())) {
          next.insert(vf.handle());
        }
      }
    }

    //cout << "  " << next.size() << " neighboring faces." << endl;

    FaceHandle nextf = InvalidFaceHandle;
    for (unordered_set<FaceHandle, Hasher>::iterator it = next.begin(); it != next.end(); ++it) {
      real d = triangle(*it).distance(p);
      checked.insert(*it);
      if (d < mind) {
        mind = d;
        nextf = *it;
      }
    }

    //cout << "  next face " << nextf << " distance " << mind << endl;

    if (nextf.is_valid()) {
      current = nextf;
    } else {
      // no closer triangle found in the neighborhood
      return current;
    }
  }
}

TriMesh::FaceHandle TriMesh::local_closest_face(TriMesh::Point const &p, VertexHandle start) const {
  // choose an arbitrary face incident to this vertex to start the search from
  FaceHandle fh = face_handle(halfedge_handle(start));
  if (!fh.is_valid()) {
    fh = opposite_face_handle(halfedge_handle(start));
  }
  assert(fh.is_valid());
  return local_closest_face(p, fh);
}


#if 0
// cuts the mesh along the plane, but only walks locally, starting from the given face, which
// is supposed to intersect the plane (false is returned and nothing is done if it doesn't)
bool TriMesh::cut_local(Plane<real> const &plane, double epsilon) {

  // absolute distance from the plane needed to get off of it
  double delta = epsilon * mean_edge_length();

  struct Classify {
    Plane<real> const &p;
    real delta;
    Classify(Plane const &p, real delta): p(p), delta(delta) {}

    int operator(Point const &p) {
      double d = plane.phi(point(vi));
      if (fabs(d) < delta) {
        property(vtype, vi.handle()) = 0;
      } else if (d < 0) {
        property(vtype, vi.handle()) = -1;
      } else {
        property(vtype, vi.handle()) = 1;
      }
    }
  } classify(plane, delta);

}
#endif

// compute edge-connected components around a non-manifold (boundary) vertex
vector<vector<FaceHandle>> TriMesh::surface_components(VertexHandle vh, unordered_set<EdgeHandle,Hasher> exclude_edges) const {
  auto incident = incident_faces(vh);

  // map face handles to indices in incident
  unordered_map<FaceHandle, int, Hasher> fmap;
  for (auto it = incident.begin(); it != incident.end(); ++it) {
    fmap[*it] = (int)(it-incident.begin());
  }

  UnionFind union_find((int)incident.size());
  for (ConstVertexEdgeIter e = cve_iter(vh); e; ++e) {
    if (is_boundary(e) || exclude_edges.count(e))
      continue;
    auto faces = face_handles(e.handle());
    assert(fmap.count(faces.x));
    assert(fmap.count(faces.y));
    union_find.merge(fmap[faces.x], fmap[faces.y]);
  }

  // spit out connected components
  unordered_map<int, vector<FaceHandle>> components;
  for (auto f : incident) {
    components[union_find.find(fmap[f])].push_back(f);
  }

  vector<vector<FaceHandle>> result;

  for (auto c : components) {
    result.push_back(c.second);
  }

  return result;
}

// split a (boundary) vertex in as many vertices as there are edge-connected surface components
vector<VertexHandle> TriMesh::split_nonmanifold_vertex(VertexHandle vh, unordered_set<EdgeHandle,Hasher> exclude_edges) {
  auto components = surface_components(vh, exclude_edges);

  vector<VertexHandle> verts(1,vh);
  TV dupe = point(vh);
  for (int i = 1; i < (int) components.size(); ++i) {
    VertexHandle v = add_vertex(dupe);

    for (auto f : components[i]) {
      auto vhs = vertex_handles(f);
      delete_face(f,false);
      // replace vh with v in vhs
      if (vhs.x == vh) vhs.x = v;
      if (vhs.y == vh) vhs.y = v;
      if (vhs.z == vh) vhs.z = v;

      add_face(vhs.x, vhs.y, vhs.z);

      GEODE_ASSERT(!status(vhs.x).deleted() && vhs.x.is_valid() && isfinite(point(vhs.x)) );
      GEODE_ASSERT(!status(vhs.y).deleted() && vhs.y.is_valid()  && isfinite(point(vhs.y)) );
      GEODE_ASSERT(!status(vhs.z).deleted() && vhs.z.is_valid()  && isfinite(point(vhs.z)) );
    }

    verts.push_back(v);
  }

  return verts;
}

// split an edge in two if the incident faces are only connected through this edge
// returns the newly created edge. Both end points have to be boundary vertices
// for this to happen.
vector<EdgeHandle> TriMesh::separate_edge(EdgeHandle eh) {

  // nothing to split if already a boundary edge
  if (is_boundary(eh)) {
    return make_vector(EdgeHandle());
  }
  // we split the edge by splitting each end vertex
  auto vhs = vertex_handles(eh);
  unordered_set<EdgeHandle, Hasher> edgeset;
  edgeset.insert(eh);
  auto vs0 = split_nonmanifold_vertex(vhs.x, edgeset);
  auto vs1 = split_nonmanifold_vertex(vhs.y, edgeset);

  // find all vertex pairs that have edges between them and return the edges
  vector<EdgeHandle> result;
  for (auto v0 : vs0) {
    for (auto v1 : vs1) {
      auto e = edge_handle(v0,v1);
      if (e.is_valid())
        result.push_back(e);
    }
  }

  return result;
}

// split the mesh along a string of edges. If the edges form loops, this
// results in two holes per loop. All non-loop connected components create
// a single hole. Returns all vertices that were split, and all vertices they
// were split into.
GEODE_CORE_EXPORT vector<VertexHandle> TriMesh::separate_edges(vector<EdgeHandle> ehs) {
  unordered_set<EdgeHandle, Hasher> edgeset(ehs.begin(), ehs.end());

  unordered_set<VertexHandle, Hasher> edgeverts;
  for (auto e : ehs) {
    auto vhs = vertex_handles(e);
    edgeverts.insert(vhs.x);
    edgeverts.insert(vhs.y);
  }

  vector<VertexHandle> allverts;
  for (auto v : edgeverts) {
    extend(allverts, split_nonmanifold_vertex(v, edgeset));
  }
  return allverts;
}


void TriMesh::cut_and_mirror(Plane<real> const &plane, bool mirror, double epsilon, double area_hack) {
  OpenMesh::VPropHandleT<int> vtype;
  add_property(vtype);

  // absolute distance from the plane needed to get off of it
  double delta = epsilon * mean_edge_length();

  // classify all vertices: on the plane, front, back
  for (TriMesh::VertexIter vi = vertices_sbegin(); vi != vertices_end(); ++vi) {
    double d = plane.phi(point(vi));

    if (abs(d) < delta) {
      property(vtype, vi) = 0;
    } else if (d < 0) {
      property(vtype, vi) = -1;
    } else {
      property(vtype, vi) = 1;
    }
  }

  // TODO: Remove this horror
  if (area_hack) {
    // Compute connected components of vertices
    UnionFind union_find(n_vertices());
    for (ConstEdgeIter e = edges_sbegin(); e != edges_end(); ++e) {
      VertexHandle v0 = from_vertex_handle(halfedge_handle(e,0)),
                   v1 = from_vertex_handle(halfedge_handle(e,1));
      if ((property(vtype,v0)<0) == (property(vtype,v1)<0))
        union_find.merge(v0.idx(),v1.idx());
    }
    // Compute adjacency map between regions
    Hashtable<int,Hashtable<int> > neighbors;
    for (ConstEdgeIter e = edges_sbegin(); e != edges_end(); ++e) {
      VertexHandle v0 = from_vertex_handle(halfedge_handle(e,0)),
                   v1 = from_vertex_handle(halfedge_handle(e,1));
      if ((property(vtype,v0)<0) != (property(vtype,v1)<0)) {
        int r0 = union_find.find(v0.idx()),
            r1 = union_find.find(v1.idx());
        if (r0 != r1) {
          neighbors[r0].set(r1);
          neighbors[r1].set(r0);
        }
      }
    }
    // Measure approximate surface area of each region
    Hashtable<int,T> area;
    for (ConstFaceIter f = faces_sbegin(); f != faces_end(); ++f) {
      Vector<VertexHandle,3> v = vertex_handles(f);
      Vector<int,3> r;
      for (int i=0;i<3;i++)
        r[i] = union_find.find(v[i].idx());
      if (r.elements_equal())
        area[r.x] += this->area(f);
    }
    // Flip the signs of any small regions with at least one neighbor
    for (ConstVertexIter v = vertices_sbegin(); v != vertices_end(); ++v) {
      int r = union_find.find(v.handle().idx());
      if (area[r]<area_hack && neighbors[r].size())
        property(vtype,v) = -property(vtype,v);
    }
  }

  for (TriMesh::VertexIter vi = vertices_sbegin(); vi != vertices_end(); ++vi) {

    if (property(vtype, vi.handle()) == 0) {

      // reclassify some vertices:
      bool positive_neighbors = false;
      bool negative_neighbors = false;

      for (TriMesh::VertexVertexIter vv = vv_iter(vi); vv; ++vv) {
        int type = property(vtype, vv.handle());
        if (type == 1) {
          positive_neighbors = true;
        } else if (type == -1) {
          negative_neighbors = true;
        }
      }

      // if a vertex is on the plane but have no negative neighbors but has positive neighbors
      // and if it is strictly above the plane, it should be a positive vertex
      if (positive_neighbors && !negative_neighbors && plane.phi(point(vi)) > 0) {
        property(vtype, vi.handle()) = 1;
        continue;
      }

      // project vertices on the plane onto the plane
      point(vi.handle()) = plane.surface(point(vi.handle()));
    }

  }

  // find all edges that cross the plane front to back and make a new vertex at the intersection
  unordered_map<EdgeHandle, VertexHandle, Hasher> splitter;
  for (TriMesh::EdgeIter ei = edges_sbegin(); ei != edges_end(); ++ei) {
    if (property(vtype, to_vertex_handle(halfedge_handle(ei.handle(), 0))) *
        property(vtype, to_vertex_handle(halfedge_handle(ei.handle(), 1))) < 0) {
      EdgeHandle eh = ei.handle();
      Segment<Vector<real, 3> > edge = segment(eh);
      VertexHandle split = splitter[eh] = add_vertex(plane.segment_intersection(edge));
      GEODE_ASSERT(property(vtype, split) == 0);
    }
  }

  // split each crossing edge with a new vertex on the plane
  for (unordered_map<EdgeHandle, VertexHandle, Hasher>::iterator it = splitter.begin(); it != splitter.end(); ++it) {
    split(it->first, it->second);
  }

  // delete all faces with no front vertices
  for (TriMesh::FaceIter fi = faces_sbegin(); fi != faces_end(); ++fi) {
    bool front = false;
    GEODE_DEBUG_ONLY(bool back = false);
    for (TriMesh::FaceVertexIter fv = fv_iter(fi.handle()); fv; ++fv) {
      int type = property(vtype, fv.handle());
      if (type == 1) {
        front = true;
      } else if (type == -1) {
        GEODE_DEBUG_ONLY(back = true);
      }
    }

    assert(!front || !back);

    if (!front)
      delete_face(fi);
  }

  // make sure delete_face got rid of all negative vertices
  for (TriMesh::VertexIter vi = vertices_sbegin(); vi != vertices_end(); ++vi) {
    assert(property(vtype, vi) != -1);
  }

  if (mirror) {
    unordered_map<VertexHandle, VertexHandle, Hasher> mirrored;

    // duplicate all front vertices with mirrored position
    for (TriMesh::VertexIter vi = vertices_sbegin(); vi != vertices_end(); ++vi) {
      int type = property(vtype, vi);
      if (type == 1) {
        mirrored[vi.handle()] = add_vertex(plane.mirror(point(vi.handle())));
        property(vtype, mirrored[vi.handle()]) = -1;
      } else if (type == 0) {
        mirrored[vi.handle()] = vi.handle();
      } else {
        // we reached past the last used vertex, quit
        break;
      }
    }

    // for all faces that still exist, create a new face with mirrored vertices
    vector<Vector<VertexHandle, 3> > new_faces;
    for (TriMesh::FaceIter fi = faces_sbegin(); fi != faces_end(); ++fi) {
      Vector<VertexHandle, 3> newverts, oldverts = vertex_handles(fi.handle());

      //cout << "duplicating face " << oldverts << ", types " << property(vtype, oldverts[0]) << " " << property(vtype, oldverts[1]) << " " << property(vtype, oldverts[2]) << endl;

      for (int i = 2; i > -1; --i) {
        assert(mirrored.count(oldverts[i]));
        newverts[2-i] = mirrored[oldverts[i]];
      }

      new_faces.push_back(newverts);
    }

    for (vector<Vector<VertexHandle, 3> >::iterator it = new_faces.begin(); it != new_faces.end(); ++it) {
      Vector<VertexHandle, 3> &newverts = *it;
      GEODE_DEBUG_ONLY(FaceHandle fh =) add_face(newverts[0], newverts[1], newverts[2]);
      assert(fh.is_valid());
    }
  }

  remove_property(vtype);
}

void TriMesh::cut(Plane<real> const &plane, T epsilon, T area_hack) {
  cut_and_mirror(plane, false, epsilon, area_hack);
}

void TriMesh::mirror(Plane<real> const &plane, T epsilon) {
  cut_and_mirror(plane, true, epsilon, 0);
}

bool TriMesh::has_boundary() const {
  // check vertices, not halfedges, which is faster
  for (ConstVertexIter it = vertices_sbegin(); it != vertices_end(); ++it) {
    if (is_boundary(it))
      return true;
  }
  return false;
}


bool TriMesh::is_boundary_vertex(const VertexHandle& vh) const {
  return is_boundary(vh);
}


// find boundary loops
vector<vector<TriMesh::HalfedgeHandle> > TriMesh::boundary_loops() const {
  unordered_set<HalfedgeHandle, Hasher> done;
  vector<vector<HalfedgeHandle> >  loops;
  for (ConstHalfedgeIter it = halfedges_begin(); it != halfedges_end(); ++it) {
    if(status(edge_handle(it)).deleted() || status(it).deleted()) continue;
    if (is_boundary(it) && !done.count(it.handle())) {
      loops.push_back(boundary_loop(it.handle()));
      done.insert(loops.back().begin(), loops.back().end());
    }
  }

  return loops;
}

// find the boundary loop starting at seed (empty if seed is not on the boundary)
vector<TriMesh::HalfedgeHandle> TriMesh::boundary_loop(HalfedgeHandle const &seed) const {
  if (!is_boundary(seed))
    return vector<HalfedgeHandle>();

  vector<HalfedgeHandle> loop;
  loop.push_back(seed);

  do {
    loop.push_back(next_halfedge_handle(loop.back()));
  } while (loop.front() != loop.back());

  loop.pop_back();

  return loop;
}

// fill the hole enclosed by the given halfedges, retain the new faces only if the surface area is smaller than max_area
// returns whether filled or not
vector<FaceHandle> TriMesh::fill_hole(vector<HalfedgeHandle> const &loop, double max_area) {

  // get vertices
  vector<VertexHandle> verts;
  for (vector<HalfedgeHandle>::const_iterator it = loop.begin(); it != loop.end(); ++it) {
    assert(is_boundary(*it));
    verts.push_back(to_vertex_handle(*it));
  }

  ShortEdgePriority ep(*this);
  vector<FaceHandle> faces;
  triangulate_face(*this, verts, faces, ep);

  bool keep = true;
  double A = area(faces);
  if (faces.size() < loop.size() - 2) {
    keep = false;
  } else {
    keep = A < max_area;
  }

  if (!keep) {
    for (vector<TriMesh::FaceHandle>::iterator it = faces.begin(); it != faces.end(); ++it) {
      delete_face(*it);
    }
    return vector<FaceHandle>();
  } else {
    return faces;
  }
}

// fill all holes with maximum area given
int TriMesh::fill_holes(double max_area) {
  vector<vector<HalfedgeHandle> > loops = boundary_loops();

  int nfilled = 0;
  for (auto loop : loops) {
    if (!fill_hole(loop, max_area).empty())
      nfilled++;
  }

  garbage_collection();

  return nfilled;
}

Array<Vector<int,3> > TriMesh::elements() const {
  Array<Vector<int,3> > tris;
  tris.preallocate(n_faces());
  for (TriMesh::ConstFaceIter f = faces_begin(); f != faces_end(); ++f) {
    Vector<VertexHandle,3> v = vertex_handles(f);
    tris.append(vec(v.x.idx(),v.y.idx(),v.z.idx()));
  }
  return tris;
}

Array<Vector<int,2> > TriMesh::segments() const {
  Array<Vector<int,2> > segs;
  segs.preallocate(n_edges());
  for (auto eh : edge_handles()) {
    Vector<VertexHandle, 2> vh = vertex_handles(eh);
    segs.append(Vector<int,2>(vh[0].idx(),vh[1].idx()));
  }
  return segs;
}

Array<Vector<real,3> > TriMesh::verts() const {
  Array<Vector<real,3> > vs;
  vs.preallocate(n_vertices());
  for (auto vh : vertex_handles()) {
    vs.append(point(vh));
  }
  return vs;
}

Array<int> TriMesh::vert_ids() const {
  Array<int> vs;
  vs.preallocate(n_vertices());
  for (auto vh : vertex_handles()) {
    vs.append(vh.idx());
  }
  return vs;
}


RawArray<const Vector<real,3> > TriMesh::X() const {
  return RawArray<const Vector<real,3> >(n_vertices(),points());
}

RawArray<Vector<real,3> > TriMesh::X() {
  return RawArray<Vector<real,3> >(n_vertices(),const_cast_(points()));
}

Array<Vector<real,3> > TriMesh::X_python() const {
  return X().copy();
}

void TriMesh::set_X_python(RawArray<const Vector<real,3>> new_X) {
  GEODE_ASSERT(new_X.size()==(int)n_vertices());
  X() = new_X;
}

void TriMesh::set_vertex_normals(RawArray<const Vector<real,3>> normals) {
  request_vertex_normals();
  GEODE_ASSERT(normals.size()==(int)n_vertices());
  for (auto v : vertex_handles())
    set_normal(v,normals[v.idx()]);
}

void TriMesh::set_vertex_colors(RawArray<const Vector<real,3>> colors) {
  request_vertex_colors();
  GEODE_ASSERT(colors.size()==(int)n_vertices());
  for (auto v : vertex_handles())
    set_color(v,to_byte_color(colors[v.idx()]));
}

Field<Vector<Vector<real,2>,3>,FaceHandle> TriMesh::face_texcoords() const {
  GEODE_ASSERT(has_halfedge_texcoords2D());
  Field<Vector<Vector<real,2>,3>,FaceHandle> texcoords(n_faces(),uninit);
  for (const auto f : face_handles()) {
    const auto h = halfedge_handles(f);
    texcoords[f] = Vector<Vector<real,2>,3>(texcoord2D(h.x),
                                            texcoord2D(h.y),
                                            texcoord2D(h.z));
  }
  return texcoords;
}

void TriMesh::set_face_texcoords(RawField<const Vector<Vector<real,2>,3>,FaceHandle> texcoords) {
  GEODE_ASSERT(texcoords.size()==(int)n_faces());
  request_halfedge_texcoords2D();
  scalar_view(prop(halfedge_texcoords2D_pph()).flat).fill(inf);
  for (const auto f : face_handles()) {
    const auto h = halfedge_handles(f);
    const auto& tc = texcoords[f];
    set_texcoord2D(h.x,tc.x);
    set_texcoord2D(h.y,tc.y);
    set_texcoord2D(h.z,tc.z);
  }
}

void TriMesh::add_box(TV min, TV max) {
  vector<VertexHandle> vh;
  vh.push_back(add_vertex(min));
  vh.push_back(add_vertex(vec(min.x, min.y, max.z)));
  vh.push_back(add_vertex(vec(min.x, max.y, min.z)));
  vh.push_back(add_vertex(vec(min.x, max.y, max.z)));

  vh.push_back(add_vertex(vec(max.x, min.y, min.z)));
  vh.push_back(add_vertex(vec(max.x, min.y, max.z)));
  vh.push_back(add_vertex(vec(max.x, max.y, min.z)));
  vh.push_back(add_vertex(max));

  add_face(vh[0], vh[1], vh[2]);
  add_face(vh[2], vh[1], vh[3]);

  add_face(vh[1], vh[0], vh[5]);
  add_face(vh[5], vh[0], vh[4]);

  add_face(vh[3], vh[1], vh[7]);
  add_face(vh[7], vh[1], vh[5]);

  add_face(vh[0], vh[2], vh[4]);
  add_face(vh[4], vh[2], vh[6]);

  add_face(vh[2], vh[3], vh[6]);
  add_face(vh[6], vh[3], vh[7]);

  add_face(vh[5], vh[6], vh[7]);
  add_face(vh[6], vh[5], vh[4]);
}

void TriMesh::add_sphere(TV c, real r, int divisions) {
  Point b0(r, 0, 0);
  Point b1(0, r, 0);
  Point b2(0, 0, r);

  // end cap 1
  VertexHandle c0 = add_vertex(c - b0);

  vector<VertexHandle> vh;
  for (int i = 0; i < divisions; ++i) {
    real t = -(divisions/2.-1.)/(divisions/2.);
    Point q = b0 * t;
    Point b = b1 * cos(i * M_PI * 2 / divisions) + b2 * sin(i * M_PI * 2 / divisions);
    vh.push_back(add_vertex(c + q + sqrt(1-t*t) * b));
  }

  for (int i = divisions-1, j = 0; j < divisions; i = j++) {
    add_face(c0, vh[j], vh[i]);
  }

  // segments
  for (int k = 0; k < divisions-2; ++k) {
    real t = (k - (divisions/2.-2.))/(divisions/2.);
    Point q = b0 * t;

    vector<VertexHandle> vh_next;
    for (int i = 0; i < divisions; ++i) {
      Point b = b1 * cos(i * M_PI * 2 / divisions) + b2 * sin(i * M_PI * 2 / divisions);
      vh_next.push_back(add_vertex(c + q + sqrt(1-t*t) * b));
    }

    for (int i = divisions-1, j = 0; j < divisions; i = j++) {
      add_face(vh[i], vh[j], vh_next[j]);
      add_face(vh[i], vh_next[j], vh_next[i]);
    }

    vh = vh_next;
  }

  // end cap 2
  VertexHandle c1 = add_vertex(c + b0);

  for (int i = divisions-1, j = 0; j < divisions; i = j++) {
    add_face(c1, vh[i], vh[j]);
  }
}

void TriMesh::add_cylinder(TV p0, TV p1, real r1, real r2, int divisions, bool caps) {
  // find orthogonal vectors
  Point d = (p1-p0).normalized();
  Point b1 = d.orthogonal_vector().normalized();
  Point b2 = cross(d, b1).normalized();

  vector<VertexHandle> vh1, vh2;
  for (int i = 0; i < divisions; ++i) {
    Point b = b1 * cos(i * M_PI * 2 / divisions) + b2 * sin(i * M_PI * 2 / divisions);
    vh1.push_back(add_vertex(p0 + r1 * b));
    vh2.push_back(add_vertex(p1 + r2 * b));
  }

  for (int i = divisions-1, j = 0; j < divisions; i = j++) {
    add_face(vh1[i], vh1[j], vh2[j]);
    add_face(vh1[i], vh2[j], vh2[i]);
  }

  // make caps (if requested)
  if (caps) {
    VertexHandle c1 = add_vertex(p0);
    VertexHandle c2 = add_vertex(p1);
    for (int i = divisions-1, j = 0; j < divisions; i = j++) {
      add_face(c1, vh1[j], vh1[i]);
      add_face(c2, vh2[i], vh2[j]);
    }
  }
}

void TriMesh::scale(real s, const Vector<real, 3>& center) {
  Vector<real,3> base = (1-s)*center;
  for (TriMesh::VertexIter v = vertices_begin(); v != vertices_end(); ++v)
    set_point(v,base+s*point(v));
}

void TriMesh::scale(TV s, const Vector<real, 3>& center) {
  Vector<real,3> base = (1-s)*center;
  for (TriMesh::VertexIter v = vertices_begin(); v != vertices_end(); ++v)
    set_point(v,base+s*point(v));
  if (has_face_normals())
    update_face_normals();
  if (has_vertex_normals())
    update_vertex_normals();
}

void TriMesh::translate(Vector<real, 3> const& t) {
  for (TriMesh::VertexIter v = vertices_begin(); v != vertices_end(); ++v)
    set_point(v, t + point(v));
}

void TriMesh::rotate(Rotation<Vector<real, 3> > const &R, Vector<real,3> const &center) {
  for (TriMesh::VertexIter v = vertices_begin(); v != vertices_end(); ++v)
    set_point(v, center + R * (point(v) - center));
  if (has_face_normals())
    update_face_normals();
  if (has_vertex_normals())
    update_vertex_normals();
}

void TriMesh::transform(Frame<Vector<real, 3> > const &F) {
  for (TriMesh::VertexIter v = vertices_begin(); v != vertices_end(); ++v) {
    set_point(v, F * point(v));
  }
}

void TriMesh::transform(Matrix<real, 4> const &M) {
  for (TriMesh::VertexIter v = vertices_begin(); v != vertices_end(); ++v) {
    set_point(v, M.homogeneous_times(point(v)));
  }
}

void TriMesh::invert_component(std::vector<FaceHandle> component) {
  // this will fail horribly if the given faces are not a complete component
  // (ie if they have neighbors that are not in the vector)

  // get all half-edges and vertices in this component
  vector<HalfedgeHandle> halfedges;
  unordered_set<VertexHandle, Hasher> vertices;

  for (FaceHandle f : component) {
    for (ConstFaceHalfedgeIter fh = cfh_iter(f); fh; ++fh) {
      halfedges.push_back(fh.handle());
      vertices.insert(to_vertex_handle(fh.handle()));
    }
  }

  // For each halfedge, collect next halfedge and source vertex
  unordered_map<HalfedgeHandle,pair<HalfedgeHandle,VertexHandle>, Hasher> infos;
  for (HalfedgeHandle e : halfedges)
    infos[e] = make_pair(next_halfedge_handle(e), from_vertex_handle(e));

  // Set each halfedge to point in the opposite direction
  for (HalfedgeHandle e : halfedges) {
    pair<HalfedgeHandle,VertexHandle> const &info = infos.find(e)->second;
    set_next_halfedge_handle(info.first, e);
    set_vertex_handle(e, info.second);
  }

  // Update vertex to edge pointers
  for (VertexHandle v : vertices)
    set_halfedge_handle(v, next_halfedge_handle(halfedge_handle(v)));
}

void TriMesh::invert() {
  // For each halfedge, collect next halfedge and source vertex
  unordered_map<TriMesh::HalfedgeHandle,pair<TriMesh::HalfedgeHandle,TriMesh::VertexHandle>,Hasher> infos;
  for (TriMesh::ConstHalfedgeIter e = halfedges_sbegin(); e != halfedges_end(); ++e)
    infos[e] = make_pair(next_halfedge_handle(e), from_vertex_handle(e));

  // Set each halfedge to point in the opposite direction
  for (TriMesh::ConstHalfedgeIter e = halfedges_sbegin(); e != halfedges_end(); ++e) {
    const pair<TriMesh::HalfedgeHandle,TriMesh::VertexHandle>& info = infos.find(e)->second;
    set_next_halfedge_handle(info.first, e);
    set_vertex_handle(e, info.second);
  }

  // Update vertex to edge pointers
  for (TriMesh::ConstVertexIter v = vertices_sbegin(); v != vertices_end(); ++v)
    // choosing the next halfedge will ensure that the halfedge is pointing away from v
    // and the next halfedge will be in the same face as v's original halfedge_handle,
    // in particular, an invalid face if v is on the boundary.
    if (halfedge_handle(v).is_valid())
      set_halfedge_handle(v, next_halfedge_handle(halfedge_handle(v)));
}

real TriMesh::volume() const {
  // If S is the surface and I is the interior, Stokes theorem gives
  //     V = int_I dV = 1/3 int_I (div x) dV = 1/3 int_S x . dA
  //       = 1/3 sum_t c_t . A_t
  //       = 1/18 sum_t (a + b + c) . (b - a) x (c - a)
  //       = 1/18 sum_t det (a+b+c, b-a, c-a)
  //       = 1/18 sum_t det (3a, b-a, c-a)
  //       = 1/6 sum_t det (a,b,c)
  // where a,b,c are the vertices of each triangle.

  real sum=0;

  for (TriMesh::ConstFaceIter f = faces_sbegin(); f != faces_end(); ++f) {
    Triangle<Vector<real, 3> > t = triangle(f);
    sum+=det(t.x0,t.x1,t.x2);
  }
  return real(1./6)*sum;
}

real TriMesh::volume(RawArray<const FaceHandle> faces) const {
  real sum=0;

  for (TriMesh::FaceHandle f : faces) {
    Triangle<Vector<real, 3> > t = triangle(f);
    sum += det(t.x0,t.x1,t.x2);
  }
  return real(1./6)*sum;
}


real TriMesh::area() const {
  // TODO: Do the division by two once at the end
  T sum = 0;
  for (ConstFaceIter f = faces_sbegin(); f != faces_end(); ++f)
    sum += area(f);
  return sum;
}

real TriMesh::area(RawArray<const FaceHandle> faces) const {
  // TODO: Do the division by two once at the end
  T sum = 0;
  for (FaceHandle f : faces)
    sum += area(f);
  return sum;
}


real TriMesh::dihedral_angle(EdgeHandle e) const {
  return dihedral_angle(halfedge_handle(e,0));
}

real TriMesh::dihedral_angle(HalfedgeHandle e) const {
  Triangle<TV> t0 = triangle(face_handle(e)),
               t1 = triangle(face_handle(opposite_halfedge_handle(e)));
  TV d = t1.center() - t0.center();
  double abs_theta = acos(min(1.,max(-1.,dot(t0.n,t1.n))));
  return copysign(abs_theta,dot(t1.n-t0.n,d));
}

real TriMesh::cos_dihedral_angle(HalfedgeHandle e) const {
  const TV x1 = point(from_vertex_handle(e)),
           x2 = point(to_vertex_handle(e)),
           x0 = point(to_vertex_handle(next_halfedge_handle(e))),
           x3 = point(to_vertex_handle(next_halfedge_handle(opposite_halfedge_handle(e))));
  const TV n0 = cross(x2-x1,x0-x1),
           n1 = cross(x3-x1,x0-x1);
  return dot(n0,n1)/sqrt(sqr_magnitude(n0)*sqr_magnitude(n1));
}

real TriMesh::cos_sector_angle(HalfedgeHandle e) const {
  const auto v0 = from_vertex_handle(e), v1 = to_vertex_handle(e), v2 = to_vertex_handle(next_halfedge_handle(e));
  const TV x1 = point(v1),
           e0 = point(v0)-x1,
           e1 = point(v2)-x1;
  return dot(e0,e1)/sqrt(sqr_magnitude(e0)*sqr_magnitude(e1));
}

// delete a set of faces
void TriMesh::delete_faces(std::vector<FaceHandle> const &fh) {
  for (FaceHandle f : fh) {
    if (f.is_valid())
      delete_face(f);
  }
}

Tuple<int,Array<int> > TriMesh::component_vertex_map() const {
  // Find components
  UnionFind union_find(n_vertices());
  for (ConstEdgeIter e=edges_sbegin();e!=edges_end();++e) {
    auto h = halfedge_handle(e,0);
    union_find.merge(from_vertex_handle(h).idx(),to_vertex_handle(h).idx());
  }

  // Label components
  Hashtable<int,int> labels;
  for (int i=0;i<union_find.size();i++)
    if (union_find.is_root(i))
      labels.set(i,labels.size());

  //map vertex to mesh/label id
  Array<int> map(n_vertices(),uninit);
  for (int i=0;i<union_find.size();i++)
    map[i] = labels.get(union_find.find(i));

  return tuple(labels.size(),map);
}

Array<int> TriMesh::component_face_map() const {
  Tuple<int,Array<int> > vmap = component_vertex_map();
  Array<int> map(n_faces(),uninit);
  int i =0;
  for (ConstFaceIter f=faces_begin();f!=faces_end();++f) {
    auto v = vertex_handles(f);
    map[i] = vmap.y[v[0].idx()];
    i++;
  }
  return map;
}

vector<Ref<TriMesh> > TriMesh::component_meshes() const {
  Tuple<int,Array<int> > vmap = component_vertex_map();
  vector<Ref<TriMesh> > meshes;
  for(int i=0;i<vmap.x;i++)
    meshes.push_back(new_<TriMesh>());

  // Add vertices
  Array<VertexHandle> map(n_vertices(),uninit);
  for (int i=0;i<vmap.y.size();i++)
    map[i] = meshes[vmap.y[i]]->add_vertex(point(VertexHandle(i)));
  // Add faces
  for (ConstFaceIter f=faces_begin();f!=faces_end();++f) {
    auto v = vertex_handles(f);
    Vector<VertexHandle,3> mv(vec(map[v[0].idx()],map[v[1].idx()],map[v[2].idx()]));
    GEODE_ASSERT(meshes[vmap.y[v[0].idx()]]->add_face(mv[0],mv[1],mv[2]).is_valid());
  }
  return meshes;
}

vector<Ref<TriMesh> > TriMesh::nested_components() const{
  // get face->component map
  Array<int> fmap = component_face_map();

  // get volumes
  vector<Ref<TriMesh> > meshes = component_meshes();

  vector<real> volumes;
  for (const Ref<TriMesh> mesh : meshes){
    mesh->request_face_normals();
    mesh->request_vertex_normals();
    mesh->update_normals();
    volumes.push_back(mesh->volume()); // could be zero if things haven't been repaired or whatnot, need to check later
  }

  Ref<TriangleSoup> m = new_<TriangleSoup>(elements());
  Ref<SimplexTree<TV,2> > tree = new_<SimplexTree<TV,2> >(*m,X().copy(),1);
  real thicken = 1e-6*bounding_box().sizes().magnitude();

  map<int,vector<int> > nested;

  for(int id = 0; id < (int)meshes.size();id++){
    const Ref<TriMesh> m = meshes[id];
    // only emit from holes (volume <0)
    real cur_volume = volumes[id];
    if(cur_volume > 0) continue;
    RayIntersection<TV> r(m->triangle(FaceHandle(0)).center(),m->normal(FaceHandle(0))*-1);
    auto ints = tree->intersections(r,thicken);
    vector<RayIntersection<TV> > results(ints.begin(),ints.end());
    vector<pair<real,int> > ts;
    vector<int> hits(meshes.size(),0); //potentially overkill
    //aggregate hits
    for (const RayIntersection<TV>& r : results){
      int hit_id = fmap[r.aggregate_id];
      // ignore self
      if(id!=hit_id){
        // catch all positive-volume intersections (since we emit negative only)
        if(volumes[hit_id] > 0){
          ts.push_back(make_pair(r.t_max,hit_id));
          hits[hit_id]++;
        }
      }
    }

    sort(ts.begin(),ts.end()); // find smallest t
    int closest = -1;
    for (pair<real,int>& p : ts){
      //check that we have an odd number of hits, if so return as soon as possible
      if (hits[p.second]&1){
        closest = p.second;
        break;
      }
    }

    if(closest!=-1) {
      if(!nested.count(closest)) nested.insert(make_pair(closest,vector<int>()));
      nested[closest].push_back(id);
    }
  }

  vector<Ref<TriMesh> > output;
  vector<int> redundant;
  for(const pair<int,vector<int> >& p : nested){
    Ref<TriMesh> m = meshes[p.first];
    for(const int& hole : p.second){
      m->add_mesh(*meshes[hole]);
      redundant.push_back(hole);
    }
    output.push_back(m);
    redundant.push_back(p.first);
  }

  for(int i=0;i<(int)meshes.size();i++)
    if(!contains(redundant,i)) output.push_back(meshes[i]);

  return output;
}

Ref<TriMesh> TriMesh::largest_connected_component() const {
  auto components = component_meshes();

  double amax = components[0]->area();
  Ref<TriMesh> best = components[0];
  for (int i = 1; i < (int)components.size(); ++i) {
    double a = components[i]->area();
    if (a > amax) {
      amax = a;
      best = components[i];
    }
  }
  return best;
}

Ref<SimplexTree<Vector<real,3>,2>> TriMesh::face_tree() const {
  return new_<SimplexTree<TV,2>>(*new_<TriangleSoup>(elements()),X().copy(),4);
}

Ref<SimplexTree<Vector<real,3>,1>> TriMesh::edge_tree() const {
  return new_<SimplexTree<TV,1>>(*new_<SegmentSoup>(segments()),X().copy(),4);
}

Ref<ParticleTree<Vector<real,3>>> TriMesh::point_tree() const {
  return new_<ParticleTree<Vector<real,3>>>(X().copy(),4);
}

OMSilencer::OMSilencer(bool log, bool err, bool out)
  : log_enabled(::omlog().is_enabled())
  , err_enabled(::omerr().is_enabled())
  , out_enabled(::omout().is_enabled()) {
  if (log)
    ::omlog().disable();
  if (out)
    ::omout().disable();
  if (err)
    ::omerr().disable();
}

OMSilencer::~OMSilencer() {
  if (log_enabled)
    ::omlog().enable();
  if (err_enabled)
    ::omerr().enable();
  if (out_enabled)
    ::omout().enable();
}

Ref<TriMesh> merge(vector<Ref<const TriMesh>> meshes) {
  Ref<TriMesh> m = new_<TriMesh>(*meshes.back());
  meshes.pop_back();
  for (Ref<const TriMesh> mesh : meshes)
    m->add_mesh(*mesh);
  return m;
}

//TODO: refactor these three
Vector<real,4> TriMesh::face_color(const FaceHandle& fh){
  auto vv = this->color(fh);
  return Vector<real,4>(vv[0],vv[1],vv[2],vv[3])/255.;
}

Vector<real,4> TriMesh::vertex_color(const VertexHandle& vh){
  auto vv = this->color(vh);
  return Vector<real,4>(vv[0],vv[1],vv[2],vv[3])/255.;
}

Vector<real,4> TriMesh::edge_color(const EdgeHandle& eh){
  auto vv = this->color(eh);
  return Vector<real,4>(vv[0],vv[1],vv[2],vv[3])/255.;
}

}

// Reduce template bloat
/* See header.
namespace OpenMesh {
template class PropertyT<int>;
template class PropertyT<geode::OVec<geode::real,2>>;
template class PropertyT<geode::OVec<geode::real,3>>;
template class PropertyT<geode::OVec<unsigned char,4>>;
template class PropertyT<VectorT<double,3>>;
template class PolyMeshT<AttribKernelT<FinalMeshItemsT<geode::MeshTraits,true>,TriConnectivity>>;
}
*/

#include <geode/python/function.h>

using namespace geode;

void wrap_trimesh() {
  typedef TriMesh Self;

  // need to specify exact type for overloaded functions
  typedef Box<Vector<real,3> > (TriMesh::*box_Method)() const;
  typedef TriMesh::FaceHandle (TriMesh::*fh_Method_vh_vh_vh)(TriMesh::VertexHandle, TriMesh::VertexHandle, TriMesh::VertexHandle);

  typedef Vector<TriMesh::VertexHandle, 3> (TriMesh::*Vvh3_Method_fh)(TriMesh::FaceHandle ) const;
  typedef Vector<TriMesh::VertexHandle, 2> (TriMesh::*Vvh2_Method_eh)(TriMesh::EdgeHandle ) const;

  typedef Vector<TriMesh::HalfedgeHandle, 3> (TriMesh::*Vhe3_Method_fh)(TriMesh::FaceHandle ) const;

  typedef void (TriMesh::*v_Method_str)(string const &);
  typedef void (TriMesh::*v_CMethod_str)(string const &) const;

  typedef void (TriMesh::*v_Method_r_vec3)(real, const Vector<real, 3>&);
  typedef void (TriMesh::*v_Method_vec3_vec3)(Vector<real, 3>, const Vector<real, 3>&);

  typedef Ref<TriMesh> (TriMesh::*Mesh_CMethod_vfh)(vector<FaceHandle> const &faces) const;

  typedef Segment<Vector<real, 3> > Segment3;

  Class<Self>("TriMesh")
    .GEODE_INIT()
    .GEODE_METHOD(copy)
    .GEODE_METHOD(add_vertex)
    .GEODE_OVERLOADED_METHOD(fh_Method_vh_vh_vh, add_face)
    .GEODE_METHOD(add_vertices)
    .GEODE_METHOD(add_faces)
    .GEODE_METHOD(add_mesh)
    .GEODE_METHOD(n_vertices)
    .GEODE_METHOD(n_faces)
    .GEODE_METHOD(n_edges)
    .GEODE_METHOD(n_halfedges)
    .GEODE_METHOD(remove_infinite_vertices)
    .GEODE_OVERLOADED_METHOD(v_Method_str, read)
    .GEODE_OVERLOADED_METHOD(v_CMethod_str, write)
    .GEODE_METHOD(write_with_normals)
    .GEODE_OVERLOADED_METHOD(box_Method, bounding_box)
    .GEODE_METHOD(mean_edge_length)
    .GEODE_OVERLOADED_METHOD_2(Vvh3_Method_fh, "face_vertex_handles", vertex_handles)
    .GEODE_OVERLOADED_METHOD_2(Vvh2_Method_eh, "edge_vertex_handles", vertex_handles)
    .GEODE_OVERLOADED_METHOD_2(Vhe3_Method_fh, "face_halfedge_handles", halfedge_handles)
    .GEODE_METHOD(incident_faces)
    .GEODE_METHOD(vertex_one_ring)
    .GEODE_METHOD(outgoing_halfedges)
    .GEODE_METHOD(incoming_halfedges)
    .GEODE_METHOD(smooth_normal)
    .GEODE_METHOD(add_cylinder)
    .GEODE_METHOD(add_sphere)
    .GEODE_METHOD(add_box)
    .GEODE_METHOD(vertex_shortest_path)
    .GEODE_METHOD(elements)
    .GEODE_METHOD(segments)
    .GEODE_METHOD(verts)
    .GEODE_METHOD(vert_ids)
    .GEODE_METHOD(invert)
    .GEODE_METHOD(to_vertex_handle)
    .GEODE_METHOD(from_vertex_handle)
    .GEODE_METHOD(select_faces)
    .GEODE_OVERLOADED_METHOD(Mesh_CMethod_vfh, extract_faces)
    .GEODE_METHOD_2("X",X_python)
    .GEODE_METHOD_2("set_X",set_X_python)
    .GEODE_METHOD(set_vertex_normals)
    .GEODE_METHOD(set_vertex_colors)
    .GEODE_METHOD(face_texcoords)
    .GEODE_METHOD(set_face_texcoords)
    .GEODE_METHOD(has_vertex_texcoords2D)
    .GEODE_METHOD(has_halfedge_texcoords2D)
    .GEODE_METHOD(component_meshes)
    .GEODE_METHOD(largest_connected_component)
    .GEODE_METHOD(request_vertex_normals)
    .GEODE_METHOD(request_face_normals)
    .GEODE_METHOD(update_face_normals)
    .GEODE_METHOD(update_vertex_normals)
    .GEODE_METHOD(update_normals)
    .GEODE_METHOD(request_face_colors)
    .GEODE_METHOD(request_vertex_colors)
    .GEODE_METHOD(has_face_normals)
    .GEODE_METHOD(has_vertex_normals)
    .GEODE_METHOD(has_vertex_colors)
    .GEODE_METHOD(has_face_colors)
    .GEODE_METHOD(has_edge_colors)
    .GEODE_METHOD(face_color)
    .GEODE_METHOD(vertex_color)
    .GEODE_METHOD(edge_color)
    .GEODE_METHOD_2("request_face_texcoords",request_halfedge_texcoords2D)
    .GEODE_METHOD(request_halfedge_texcoords2D)
    .GEODE_OVERLOADED_METHOD_2(const Self::TexCoord2D& (Self::*)(VertexHandle)const, "vertex_texcoord2D", texcoord2D)
    .GEODE_OVERLOADED_METHOD_2(const Self::TexCoord2D& (Self::*)(HalfedgeHandle)const, "halfedge_texcoord2D", texcoord2D)
    .GEODE_OVERLOADED_METHOD(real(Self::*)()const,volume)
    .GEODE_OVERLOADED_METHOD(real(Self::*)()const,area)
    .GEODE_OVERLOADED_METHOD_2(v_Method_r_vec3, "scale", scale)
    .GEODE_OVERLOADED_METHOD_2(v_Method_vec3_vec3, "scale_anisotropic", scale)
    .GEODE_OVERLOADED_METHOD(void(Self::*)(Matrix<real,4>const&),transform)
    .GEODE_METHOD(translate)
    .GEODE_METHOD(boundary_loops)
    .GEODE_METHOD(boundary_loop)
    .GEODE_METHOD(face_tree)
    .GEODE_METHOD(edge_tree)
    .GEODE_METHOD(point_tree)
    .GEODE_METHOD(garbage_collection_with_map)
    .GEODE_OVERLOADED_METHOD(HalfedgeHandle (Self::*)(VertexHandle,VertexHandle)const, halfedge_handle)
    .GEODE_OVERLOADED_METHOD(OTriMesh::Point const &(Self::*)(VertexHandle)const, point)
    .GEODE_OVERLOADED_METHOD(Segment3 (Self::*)(HalfedgeHandle)const, segment)
    .GEODE_OVERLOADED_METHOD(bool (Self::*)(HalfedgeHandle)const, is_boundary)
    .GEODE_OVERLOADED_METHOD(bool (Self::*)(const VertexHandle&)const, is_boundary_vertex)
    .GEODE_OVERLOADED_METHOD(HalfedgeHandle (Self::*)(HalfedgeHandle)const, opposite_halfedge_handle)
    .GEODE_OVERLOADED_METHOD(HalfedgeHandle (Self::*)(HalfedgeHandle)const, next_halfedge_handle)
    .GEODE_OVERLOADED_METHOD(HalfedgeHandle (Self::*)(HalfedgeHandle)const, prev_halfedge_handle)
    .GEODE_OVERLOADED_METHOD_2(OTriMesh::Point (Self::*)(FaceHandle,Vector<real,3>const&)const, "interpolated_point", point)
    .GEODE_OVERLOADED_METHOD(Self::Normal (Self::*)(FaceHandle)const, normal)
    .GEODE_OVERLOADED_METHOD_2(Self::Normal (Self::*)(VertexHandle)const, "vertex_normal",normal)
    .GEODE_OVERLOADED_METHOD(Self::TV(Self::*)()const, centroid)
    .GEODE_OVERLOADED_METHOD_2(Self::TV(Self::*)(FaceHandle)const, "face_centroid", centroid)
    ;
}

#endif // GEODE_OPENMESH
