/*
    This file is part of Mitsuba, a physically based rendering system.

    Copyright (c) 2007-2012 by Wenzel Jakob and others.

    Mitsuba is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License Version 3
    as published by the Free Software Foundation.

    Mitsuba is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <mitsuba/render/trimesh.h>
#include <mitsuba/core/random.h>
#include <mitsuba/core/plugin.h>
#include <mitsuba/core/zstream.h>
#include <mitsuba/core/timer.h>
#include <mitsuba/core/lock.h>
#include <mitsuba/core/properties.h>
#include <mitsuba/render/subsurface.h>
#include <mitsuba/render/medium.h>
#include <mitsuba/render/bsdf.h>
#include <mitsuba/render/emitter.h>
#include <boost/filesystem/fstream.hpp>

#define MTS_FILEFORMAT_HEADER     0x041C
#define MTS_FILEFORMAT_VERSION_V3 0x0003
#define MTS_FILEFORMAT_VERSION_V4 0x0004

MTS_NAMESPACE_BEGIN

TriMesh::TriMesh(const std::string &name, size_t triangleCount,
		size_t vertexCount, bool hasNormals, bool hasTexcoords,
		bool hasVertexColors, bool flipNormals, bool faceNormals)
 	: Shape(Properties()), m_triangleCount(triangleCount),
	  m_vertexCount(vertexCount), m_flipNormals(flipNormals),
	  m_faceNormals(faceNormals) {
	m_name = name;
	m_triangles = new Triangle[m_triangleCount];
	m_positions = new Point[m_vertexCount];
	m_normals = hasNormals ? new Normal[m_vertexCount] : NULL;
	m_texcoords = hasTexcoords ? new Point2[m_vertexCount] : NULL;
	m_colors = hasVertexColors ? new Color3[m_vertexCount] : NULL;
	m_tangents = NULL;
	m_surfaceArea = m_invSurfaceArea = -1;
	m_mutex = new Mutex();
}

TriMesh::TriMesh(const Properties &props)
 : Shape(props), m_triangles(NULL), m_positions(NULL),
	m_normals(NULL), m_texcoords(NULL), m_tangents(NULL),
	m_colors(NULL) {

	/* By default, any existing normals will be used for
	   rendering. If no normals are found, Mitsuba will
	   automatically generate smooth vertex normals.
	   Setting the 'faceNormals' parameter instead forces
	   the use of face normals, which will result in a faceted
	   appearance.
	*/
	m_faceNormals = props.getBoolean("faceNormals", false);

	/* Causes all normals to be flipped */
	m_flipNormals = props.getBoolean("flipNormals", false);

	m_triangles = NULL;
	m_surfaceArea = m_invSurfaceArea = -1;
	m_mutex = new Mutex();
}

TriMesh::TriMesh(Stream *stream, int index)
		: Shape(Properties()), m_triangles(NULL),
	m_positions(NULL), m_normals(NULL), m_texcoords(NULL),
	m_tangents(NULL), m_colors(NULL) {

	m_mutex = new Mutex();
	loadCompressed(stream, index);
}


/* Flags used to identify available data during serialization */
enum ETriMeshFlags {
	EHasNormals      = 0x0001,
	EHasTexcoords    = 0x0002,
	EHasTangents     = 0x0004, // unused
	EHasColors       = 0x0008,
	EFaceNormals     = 0x0010,
	ESinglePrecision = 0x1000,
	EDoublePrecision = 0x2000
};

TriMesh::TriMesh(Stream *stream, InstanceManager *manager)
	: Shape(stream, manager), m_tangents(NULL) {
	m_name = stream->readString();
	m_aabb = AABB(stream);

	uint32_t flags = stream->readUInt();
	m_vertexCount = stream->readSize();
	m_triangleCount = stream->readSize();

	m_positions = new Point[m_vertexCount];
	stream->readFloatArray(reinterpret_cast<Float *>(m_positions),
		m_vertexCount * sizeof(Point)/sizeof(Float));

	m_faceNormals = flags & EFaceNormals;

	if (flags & EHasNormals) {
		m_normals = new Normal[m_vertexCount];
		stream->readFloatArray(reinterpret_cast<Float *>(m_normals),
			m_vertexCount * sizeof(Normal)/sizeof(Float));
	} else {
		m_normals = NULL;
	}

	if (flags & EHasTexcoords) {
		m_texcoords = new Point2[m_vertexCount];
		stream->readFloatArray(reinterpret_cast<Float *>(m_texcoords),
			m_vertexCount * sizeof(Point2)/sizeof(Float));
	} else {
		m_texcoords = NULL;
	}

	if (flags & EHasColors) {
		m_colors = new Color3[m_vertexCount];
		stream->readFloatArray(reinterpret_cast<Float *>(m_colors),
			m_vertexCount * sizeof(Color3)/sizeof(Float));
	} else {
		m_colors = NULL;
	}

	m_triangles = new Triangle[m_triangleCount];
	stream->readUIntArray(reinterpret_cast<uint32_t *>(m_triangles),
		m_triangleCount * sizeof(Triangle)/sizeof(uint32_t));
	m_flipNormals = false;
	m_surfaceArea = m_invSurfaceArea = -1;
	m_mutex = new Mutex();
	configure();
}

static void readHelper(Stream *stream, bool fileDoublePrecision,
		Float *target, size_t count, size_t nelems) {
#if defined(SINGLE_PRECISION)
	bool hostDoublePrecision = false;
#else
	bool hostDoublePrecision = true;
#endif
	size_t size = count * nelems;
	if (fileDoublePrecision == hostDoublePrecision) {
		/* Precision matches - load directly into memory */
		stream->readFloatArray(target, size);
	} else if (fileDoublePrecision) {
		/* Double -> Single conversion */
		double *temp = new double[size];
		stream->readDoubleArray(temp, size);
		for (size_t i=0; i<size; ++i)
			target[i] = (Float) temp[i];
		delete[] temp;
	} else {
		/* Single -> Double conversion */
		float *temp = new float[size];
		stream->readSingleArray(temp, size);
		for (size_t i=0; i<size; ++i)
			target[i] = (Float) temp[i];
		delete[] temp;
	}
}


void TriMesh::loadCompressed(Stream *_stream, int index) {
	ref<Stream> stream = _stream;

	if (stream->getByteOrder() != Stream::ELittleEndian)
		Log(EError, "Tried to unserialize a shape from a stream, "
		"which was not previously set to little endian byte order!");

	short format = stream->readShort();
	if (format == 0x1C04)
		Log(EError, "Encountered a geometry file generated by an old "
			"version of Mitsuba. Please re-import the scene to update this file "
			"to the current format.");

	if (format != MTS_FILEFORMAT_HEADER)
		Log(EError, "Encountered an invalid file format!");

	short version = stream->readShort();
	if (version != MTS_FILEFORMAT_VERSION_V3 &&
	    version != MTS_FILEFORMAT_VERSION_V4)
		Log(EError, "Encountered an incompatible file version!");

	if (index != 0) {
		size_t streamSize = stream->getSize();

		/* Determine the position of the requested substream. This
		   is stored at the end of the file */
		stream->seek(streamSize - sizeof(uint32_t));
		uint32_t count = stream->readUInt();
		if (index < 0 || index > (int) count) {
			Log(EError, "Unable to unserialize mesh, "
				"shape index is out of range! (requested %i out of 0..%i)",
				index, count-1);
		}

		// Seek to the correct position
		if (version == MTS_FILEFORMAT_VERSION_V4) {
			stream->seek(stream->getSize() - sizeof(uint64_t) * (count-index) - sizeof(uint32_t));
			stream->seek(stream->readSize());
		} else {
			stream->seek(stream->getSize() - sizeof(uint32_t) * (count-index + 1));
			stream->seek(stream->readUInt());
		}

		stream->skip(sizeof(short) * 2);
	}

	stream = new ZStream(stream);
	stream->setByteOrder(Stream::ELittleEndian);

	uint32_t flags = stream->readUInt();
	if (version == MTS_FILEFORMAT_VERSION_V4)
		m_name = stream->readString();
	m_vertexCount = stream->readSize();
	m_triangleCount = stream->readSize();

	bool fileDoublePrecision = flags & EDoublePrecision;
	m_faceNormals = flags & EFaceNormals;

	if (m_positions)
		delete[] m_positions;

	m_positions = new Point[m_vertexCount];
	readHelper(stream, fileDoublePrecision,
			reinterpret_cast<Float *>(m_positions),
			m_vertexCount, sizeof(Point)/sizeof(Float));

	if (m_normals)
		delete[] m_normals;

	if (flags & EHasNormals) {
		m_normals = new Normal[m_vertexCount];
		readHelper(stream, fileDoublePrecision,
				reinterpret_cast<Float *>(m_normals),
				m_vertexCount, sizeof(Normal)/sizeof(Float));
	} else {
		m_normals = NULL;
	}

	if (m_texcoords)
		delete[] m_texcoords;

	if (flags & EHasTexcoords) {
		m_texcoords = new Point2[m_vertexCount];
		readHelper(stream, fileDoublePrecision,
				reinterpret_cast<Float *>(m_texcoords),
				m_vertexCount, sizeof(Point2)/sizeof(Float));
	} else {
		m_texcoords = NULL;
	}

	if (m_colors)
		delete[] m_colors;

	if (flags & EHasColors) {
		m_colors = new Color3[m_vertexCount];
		readHelper(stream, fileDoublePrecision,
				reinterpret_cast<Float *>(m_colors),
				m_vertexCount, sizeof(Color3)/sizeof(Float));
	} else {
		m_colors = NULL;
	}

	m_triangles = new Triangle[m_triangleCount];
	stream->readUIntArray(reinterpret_cast<uint32_t *>(m_triangles),
		m_triangleCount * sizeof(Triangle)/sizeof(uint32_t));

	m_surfaceArea = m_invSurfaceArea = -1;
	m_flipNormals = false;
}

TriMesh::~TriMesh() {
	if (m_positions)
		delete[] m_positions;
	if (m_normals)
		delete[] m_normals;
	if (m_texcoords)
		delete[] m_texcoords;
	if (m_tangents)
		delete[] m_tangents;
	if (m_colors)
		delete[] m_colors;
	if (m_triangles)
		delete[] m_triangles;
}

std::string TriMesh::getName() const {
	return m_name;
}

AABB TriMesh::getAABB() const {
	return m_aabb;
}

Float TriMesh::pdfPosition(const PositionSamplingRecord &pRec) const {
	return m_invSurfaceArea;
}

void TriMesh::configure() {
	Shape::configure();

	if (!m_aabb.isValid()) {
		/* Most shape objects should compute the AABB while
		   loading the geometry -- but let's be on the safe side */
		for (size_t i=0; i<m_vertexCount; i++)
			m_aabb.expandBy(m_positions[i]);
	}

	/* Potentially compute/recompute/flip normals, as specified by the user */
	computeNormals();

	/* Compute proper position partials with respect to the UV paramerization when:
	    1. An anisotropic BRDF is attached to the shape
		2. The material explicitly requests tangents so that it can do texture filtering
	*/
	if (hasBSDF() &&
		((m_bsdf->getType() & BSDF::EAnisotropic) || m_bsdf->usesRayDifferentials()))
		computeUVTangents();

	/* For manifold exploration: always compute UV tangents when a glossy material
	   is involved. TODO: find a way to avoid this expense (compute on demand?) */
	if (hasBSDF() && (m_bsdf->getType() & BSDF::EGlossy))
		computeUVTangents();
}

void TriMesh::prepareSamplingTable() {
	if (m_triangleCount == 0) {
		Log(EError, "Encountered an empty triangle mesh!");
		return;
	}

	LockGuard guard(m_mutex);
	if (m_surfaceArea < 0) {
		/* Generate a PDF for sampling wrt. area */
		m_areaDistr.reserve(m_triangleCount);
		for (size_t i=0; i<m_triangleCount; i++)
			m_areaDistr.append(m_triangles[i].surfaceArea(m_positions));
		m_surfaceArea = m_areaDistr.normalize();
		m_invSurfaceArea = 1.0f / m_surfaceArea;
	}
}

Float TriMesh::getSurfaceArea() const {
	if (EXPECT_NOT_TAKEN(m_surfaceArea < 0))
		const_cast<TriMesh *>(this)->prepareSamplingTable();

	return m_surfaceArea;
}

void TriMesh::samplePosition(PositionSamplingRecord &pRec,
		const Point2 &_sample) const {
	if (EXPECT_NOT_TAKEN(m_surfaceArea < 0))
		const_cast<TriMesh *>(this)->prepareSamplingTable();

	Point2 sample(_sample);
	size_t index = m_areaDistr.sampleReuse(sample.y);
	pRec.p = m_triangles[index].sample(m_positions, m_normals, pRec.n, sample);
	pRec.pdf = m_invSurfaceArea;
	pRec.measure = EArea;
}

struct Vertex {
	Point p;
	Point2 uv;
	Color3 col;
	inline Vertex() : p(0.0f), uv(0.0f), col(0.0f) { }
};

/// For using vertices as keys in an associative structure
struct vertex_key_order : public
	std::binary_function<Vertex, Vertex, bool> {
	static int compare(const Vertex &v1, const Vertex &v2) {
		if (v1.p.x < v2.p.x) return -1;
		else if (v1.p.x > v2.p.x) return 1;
		if (v1.p.y < v2.p.y) return -1;
		else if (v1.p.y > v2.p.y) return 1;
		if (v1.p.z < v2.p.z) return -1;
		else if (v1.p.z > v2.p.z) return 1;
		if (v1.uv.x < v2.uv.x) return -1;
		else if (v1.uv.x > v2.uv.x) return 1;
		if (v1.uv.y < v2.uv.y) return -1;
		else if (v1.uv.y > v2.uv.y) return 1;
		for (int i=0; i<SPECTRUM_SAMPLES; ++i) {
			if (v1.col[i] < v2.col[i]) return -1;
			else if (v1.col[i] > v2.col[i]) return 1;
		}
		return 0;
	}

	bool operator()(const Vertex &v1, const Vertex &v2) const {
		return compare(v1, v2) < 0;
	}
};

/// Used in \ref TriMesh::rebuildTopology()
struct TopoData {
	size_t idx;   /// Triangle index
	bool clustered; /// Has the tri-vert. pair been assigned to a cluster?
	inline TopoData() { }
	inline TopoData(size_t idx, bool clustered)
		: idx(idx), clustered(clustered) { }
};


void TriMesh::rebuildTopology(Float maxAngle) {
	typedef std::multimap<Vertex, TopoData, vertex_key_order> MMap;
	typedef std::pair<Vertex, TopoData> MPair;
	const Float dpThresh = std::cos(degToRad(maxAngle));

	if (m_normals) {
		delete[] m_normals;
		m_normals = NULL;
	}

	if (m_tangents) {
		delete[] m_tangents;
		m_tangents = NULL;
	}

	Log(EInfo, "Rebuilding the topology of \"%s\" (" SIZE_T_FMT
			" triangles, " SIZE_T_FMT " vertices, max. angle = %f)",
			m_name.c_str(), m_triangleCount, m_vertexCount, maxAngle);
	ref<Timer> timer = new Timer();

	MMap vertexToFace;
	std::vector<Point> newPositions;
	std::vector<Point2> newTexcoords;
	std::vector<Color3> newColors;
	std::vector<Normal> faceNormals(m_triangleCount);
	Triangle *newTriangles = new Triangle[m_triangleCount];

	newPositions.reserve(m_vertexCount);
	if (m_texcoords != NULL)
		newTexcoords.reserve(m_vertexCount);
	if (m_colors != NULL)
		newColors.reserve(m_vertexCount);

	/* Create an associative list and precompute a few things */
	for (size_t i=0; i<m_triangleCount; ++i) {
		const Triangle &tri = m_triangles[i];
		Vertex v;
		for (int j=0; j<3; ++j) {
			v.p = m_positions[tri.idx[j]];
			if (m_texcoords)
				v.uv = m_texcoords[tri.idx[j]];
			if (m_colors)
				v.col = m_colors[tri.idx[j]];
			vertexToFace.insert(MPair(v, TopoData(i, false)));
		}
		Point v0 = m_positions[tri.idx[0]];
		Point v1 = m_positions[tri.idx[1]];
		Point v2 = m_positions[tri.idx[2]];
		faceNormals[i] = Normal(normalize(cross(v1 - v0, v2 - v0)));
		for (int j=0; j<3; ++j)
			newTriangles[i].idx[j] = 0xFFFFFFFFU;
	}

	/* Under the reasonable assumption that the vertex degree is
	   bounded by a constant, the following runs in O(n) */
	for (MMap::iterator it = vertexToFace.begin(); it != vertexToFace.end();) {
		MMap::iterator start = vertexToFace.lower_bound(it->first);
		MMap::iterator end = vertexToFace.upper_bound(it->first);

		/* Perform a greedy clustering of normals */
		for (MMap::iterator it2 = start; it2 != end; it2++) {
			const Vertex &v = it2->first;
			const TopoData &t1 = it2->second;
			Normal n1(faceNormals[t1.idx]);
			if (t1.clustered)
				continue;

			uint32_t vertexIdx = (uint32_t) newPositions.size();
			newPositions.push_back(v.p);
			if (m_texcoords)
				newTexcoords.push_back(v.uv);
			if (m_colors)
				newColors.push_back(v.col);

			for (MMap::iterator it3 = it2; it3 != end; ++it3) {
				TopoData &t2 = it3->second;
				if (t2.clustered)
					continue;
				Normal n2(faceNormals[t2.idx]);

				if (n1 == n2 || dot(n1, n2) > dpThresh) {
					const Triangle &tri = m_triangles[t2.idx];
					Triangle &newTri = newTriangles[t2.idx];
					for (int i=0; i<3; ++i) {
						if (m_positions[tri.idx[i]] == v.p)
							newTri.idx[i] = vertexIdx;
					}
					t2.clustered = true;
				}
			}
		}

		it = end;
	}

	for (size_t i=0; i<m_triangleCount; ++i)
		for (int j=0; j<3; ++j)
			Assert(newTriangles[i].idx[j] != 0xFFFFFFFFU);

	delete[] m_triangles;
	m_triangles = newTriangles;

	delete[] m_positions;
	m_positions = new Point[newPositions.size()];
	memcpy(m_positions, &newPositions[0], sizeof(Point) * newPositions.size());

	if (m_texcoords) {
		delete[] m_texcoords;
		m_texcoords = new Point2[newTexcoords.size()];
		memcpy(m_texcoords, &newTexcoords[0], sizeof(Point2) * newTexcoords.size());
	}

	if (m_colors) {
		delete[] m_colors;
		m_colors = new Color3[newColors.size()];
		memcpy(m_colors, &newColors[0], sizeof(Color3) * newColors.size());
	}

	m_vertexCount = newPositions.size();

	Log(EInfo, "Done after %i ms (mesh now has " SIZE_T_FMT " vertices)",
			timer->getMilliseconds(), m_vertexCount);

	configure();
}

void TriMesh::computeNormals() {
	int invalidNormals = 0;
	if (m_faceNormals) {
		if (m_normals) {
			delete[] m_normals;
			m_normals = NULL;
		}

		if (m_flipNormals) {
			/* Change the winding order */
			for (size_t i=0; i<m_triangleCount; ++i) {
				Triangle &t = m_triangles[i];
				std::swap(t.idx[0], t.idx[1]);
			}
		}
	} else {
		if (m_normals) {
			if (m_flipNormals) {
				for (size_t i=0; i<m_vertexCount; i++)
					m_normals[i] *= -1;
			} else {
				/* Do nothing */
			}
		} else {
			m_normals = new Normal[m_vertexCount];
			memset(m_normals, 0, sizeof(Normal)*m_vertexCount);

			/* Well-behaved vertex normal computation based on
			   "Computing Vertex Normals from Polygonal Facets"
			   by Grit Thuermer and Charles A. Wuethrich,
			   JGT 1998, Vol 3 */
			for (size_t i=0; i<m_triangleCount; i++) {
				const Triangle &tri = m_triangles[i];
				Normal n(0.0f);
				for (int i=0; i<3; ++i) {
					const Point &v0 = m_positions[tri.idx[i]];
					const Point &v1 = m_positions[tri.idx[(i+1)%3]];
					const Point &v2 = m_positions[tri.idx[(i+2)%3]];
					Vector sideA(v1-v0), sideB(v2-v0);
					if (i==0) {
						n = cross(sideA, sideB);
						Float length = n.length();
						if (length == 0)
							break;
						n /= length;
					}
					Float angle = unitAngle(normalize(sideA), normalize(sideB));
					m_normals[tri.idx[i]] += n * angle;
				}
			}

			for (size_t i=0; i<m_vertexCount; i++) {
				Normal &n = m_normals[i];
				Float length = n.length();
				if (m_flipNormals)
					length *= -1;
				if (length != 0) {
					n /= length;
				} else {
					/* Choose some bogus value */
					invalidNormals++;
					n = Normal(1, 0, 0);
				}
			}
		}
	}

	m_flipNormals = false;

	if (invalidNormals > 0)
		Log(EWarn, "\"%s\": Unable to generate %i vertex normals",
			m_name.c_str(), invalidNormals);
}

void TriMesh::computeUVTangents() {
	int degenerate = 0;
	if (!m_texcoords) {
		bool anisotropic = hasBSDF() && m_bsdf->getType() & BSDF::EAnisotropic;
		if (anisotropic)
			Log(EError, "\"%s\": computeUVTangents(): texture coordinates "
				"are required to generate tangent vectors. If you want to render with an anisotropic "
				"material, please make sure that all associated shapes have valid texture coordinates.",
				getName().c_str());
		return;
	}

	if (m_tangents)
		return;

	m_tangents = new TangentSpace[m_triangleCount];
	memset(m_tangents, 0, sizeof(TangentSpace)*m_triangleCount);

	for (size_t i=0; i<m_triangleCount; i++) {
		uint32_t idx0 = m_triangles[i].idx[0],
				 idx1 = m_triangles[i].idx[1],
				 idx2 = m_triangles[i].idx[2];

		const Point
			  &v0 = m_positions[idx0],
			  &v1 = m_positions[idx1],
			  &v2 = m_positions[idx2];

		const Point2
			&uv0 = m_texcoords[idx0],
			&uv1 = m_texcoords[idx1],
			&uv2 = m_texcoords[idx2];

		Vector dP1 = v1 - v0, dP2 = v2 - v0;
		Vector2 dUV1 = uv1 - uv0, dUV2 = uv2 - uv0;
		Normal n = Normal(cross(dP1, dP2));
		Float length = n.length();
		if (length == 0) {
			++degenerate;
			continue;
		}

		Float determinant = dUV1.x * dUV2.y - dUV1.y * dUV2.x;
		if (determinant == 0) {
			/* The user-specified parameterization is degenerate. Pick
			   arbitrary tangents that are perpendicular to the geometric normal */
			coordinateSystem(n/length, m_tangents[i].dpdu, m_tangents[i].dpdv);
		} else {
			Float invDet = 1.0f / determinant;
			m_tangents[i].dpdu = ( dUV2.y * dP1 - dUV1.y * dP2) * invDet;
			m_tangents[i].dpdv = (-dUV2.x * dP1 + dUV1.x * dP2) * invDet;
		}
	}

	if (degenerate > 0)
		Log(EWarn, "\"%s\": computeTangentSpace(): Mesh contains %i "
			"degenerate triangles!", getName().c_str(), degenerate);
}

void TriMesh::getNormalDerivative(const Intersection &its,
		Vector &dndu, Vector &dndv, bool shadingFrame) const {
	if (!shadingFrame || !m_normals) {
		dndu = dndv = Vector(0.0f);
	} else {
		Assert(its.primIndex < m_triangleCount);

		const Triangle &tri = m_triangles[its.primIndex];

		uint32_t idx0 = tri.idx[0],
				 idx1 = tri.idx[1],
				 idx2 = tri.idx[2];

		const Point
			&p0 = m_positions[idx0],
			&p1 = m_positions[idx1],
			&p2 = m_positions[idx2];

		/* Recompute the barycentric coordinates, since 'its.uv' may have been
		   overwritten with coordinates of the texture "parameterization". */
 		Vector rel = its.p - p0, du = p1 - p0, dv = p2 - p0;

		Float b1  = dot(du, rel), b2 = dot(dv, rel), /* Normal equations */
			  a11 = dot(du, du), a12 = dot(du, dv),
			  a22 = dot(dv, dv),
			  det = a11 * a22 - a12 * a12;

		if (det == 0) {
			dndu = dndv = Vector(0.0f);
			return;
		}

		Float invDet = 1.0f / det,
		      u = ( a22 * b1 - a12 * b2) * invDet,
		      v = (-a12 * b1 + a11 * b2) * invDet,
		      w = 1 - u - v;

		const Normal
			&n0 = m_normals[idx0],
			&n1 = m_normals[idx1],
			&n2 = m_normals[idx2];

		/* Now compute the derivative of "normalize(u*n1 + v*n2 + (1-u-v)*n0)"
		   with respect to [u, v] in the local triangle parameterization.

		   Since d/du [f(u)/|f(u)|] = [d/du f(u)]/|f(u)|
		     - f(u)/|f(u)|^3 <f(u), d/du f(u)>, this results in
		*/

		Normal N(u * n1 + v * n2 + w * n0);
		Float il = 1.0f / N.length(); N *= il;

		dndu = (n1 - n0) * il; dndu -= N * dot(N, dndu);
		dndv = (n2 - n0) * il; dndv -= N * dot(N, dndv);

		if (m_tangents) {
			/* Compute derivatives with respect to a specified texture
			   UV parameterization.  */
			const Point2
				&uv0 = m_texcoords[idx0],
				&uv1 = m_texcoords[idx1],
				&uv2 = m_texcoords[idx2];

			Vector2 duv1 = uv1 - uv0, duv2 = uv2 - uv0;

			det = duv1.x * duv2.y - duv1.y * duv2.x;

			if (det == 0) {
				dndu = dndv = Vector(0.0f);
				return;
			}

			invDet = 1.0f / det;
			Vector dndu_ = ( duv2.y * dndu - duv1.y * dndv) * invDet;
			Vector dndv_ = (-duv2.x * dndu + duv1.x * dndv) * invDet;
			dndu = dndu_; dndv = dndv_;
		}
	}
}

ref<TriMesh> TriMesh::createTriMesh() {
	return this;
}

void TriMesh::serialize(Stream *stream, InstanceManager *manager) const {
	Shape::serialize(stream, manager);
	uint32_t flags = 0;
	if (m_normals)
		flags |= EHasNormals;
	if (m_texcoords)
		flags |= EHasTexcoords;
	if (m_colors)
		flags |= EHasColors;
	if (m_faceNormals)
		flags |= EFaceNormals;
	stream->writeString(m_name);
	m_aabb.serialize(stream);
	stream->writeUInt(flags);
	stream->writeSize(m_vertexCount);
	stream->writeSize(m_triangleCount);

	stream->writeFloatArray(reinterpret_cast<Float *>(m_positions),
		m_vertexCount * sizeof(Point)/sizeof(Float));
	if (m_normals)
		stream->writeFloatArray(reinterpret_cast<Float *>(m_normals),
			m_vertexCount * sizeof(Normal)/sizeof(Float));
	if (m_texcoords)
		stream->writeFloatArray(reinterpret_cast<Float *>(m_texcoords),
			m_vertexCount * sizeof(Point2)/sizeof(Float));
	if (m_colors)
		stream->writeFloatArray(reinterpret_cast<Float *>(m_colors),
			m_vertexCount * sizeof(Color3)/sizeof(Float));
	stream->writeUIntArray(reinterpret_cast<uint32_t *>(m_triangles),
		m_triangleCount * sizeof(Triangle)/sizeof(uint32_t));
}

void TriMesh::writeOBJ(const fs::path &path) const {
	fs::ofstream os(path);
	os << "o " << m_name << endl;
	for (size_t i=0; i<m_vertexCount; ++i) {
		os << "v "
			<< m_positions[i].x << " "
			<< m_positions[i].y << " "
			<< m_positions[i].z << endl;
	}

	if (m_texcoords) {
		for (size_t i=0; i<m_vertexCount; ++i) {
			os << "vt "
				<< m_texcoords[i].x << " "
				<< m_texcoords[i].y << endl;
		}
	}

	if (m_normals) {
		for (size_t i=0; i<m_vertexCount; ++i) {
			os << "vn "
				<< m_normals[i].x << " "
				<< m_normals[i].y << " "
				<< m_normals[i].z << endl;
		}
	}

	for (size_t i=0; i<m_triangleCount; ++i) {
		int i0 = m_triangles[i].idx[0] + 1,
			i1 = m_triangles[i].idx[1] + 1,
			i2 = m_triangles[i].idx[2] + 1;
		if (m_normals && m_texcoords) {
			os << "f " << i0 << "/" << i0 << "/" << i0 << " "
			   <<  i1 << "/" << i1 << "/" << i1 << " "
			   <<  i2 << "/" << i2 << "/" << i2 << endl;
		} else if (m_normals) {
			os << "f " << i0 << "//" << i0 << " "
			   <<  i1 << "//" << i1 << " "
			   <<  i2 << "//" << i2 << endl;
		} else {
			os << "f " << i0 << " " << i1 << " " << i2 << endl;
		}
	}

	os.close();
}

void TriMesh::serialize(Stream *_stream) const {
	ref<Stream> stream = _stream;

	if (stream->getByteOrder() != Stream::ELittleEndian)
		Log(EError, "Tried to unserialize a shape from a stream, "
			"which was not previously set to little endian byte order!");

	stream->writeShort(MTS_FILEFORMAT_HEADER);
	stream->writeShort(MTS_FILEFORMAT_VERSION_V4);
	stream = new ZStream(stream);

#if defined(SINGLE_PRECISION)
	uint32_t flags = ESinglePrecision;
#else
	uint32_t flags = EDoublePrecision;
#endif

	if (m_normals)
		flags |= EHasNormals;
	if (m_texcoords)
		flags |= EHasTexcoords;
	if (m_colors)
		flags |= EHasColors;
	if (m_faceNormals)
		flags |= EFaceNormals;

	stream->writeUInt(flags);
	stream->writeString(m_name);
	stream->writeSize(m_vertexCount);
	stream->writeSize(m_triangleCount);

	stream->writeFloatArray(reinterpret_cast<Float *>(m_positions),
		m_vertexCount * sizeof(Point)/sizeof(Float));
	if (m_normals)
		stream->writeFloatArray(reinterpret_cast<Float *>(m_normals),
			m_vertexCount * sizeof(Normal)/sizeof(Float));
	if (m_texcoords)
		stream->writeFloatArray(reinterpret_cast<Float *>(m_texcoords),
			m_vertexCount * sizeof(Point2)/sizeof(Float));
	if (m_colors)
		stream->writeFloatArray(reinterpret_cast<Float *>(m_colors),
			m_vertexCount * sizeof(Color3)/sizeof(Float));
	stream->writeUIntArray(reinterpret_cast<uint32_t *>(m_triangles),
		m_triangleCount * sizeof(Triangle)/sizeof(uint32_t));
}

size_t TriMesh::getPrimitiveCount() const {
	return m_triangleCount;
}

size_t TriMesh::getEffectivePrimitiveCount() const {
	return m_triangleCount;
}

std::string TriMesh::toString() const {
	std::ostringstream oss;
	oss << getClass()->getName() << "[" << endl
		<< "  name = \"" << m_name<< "\"," << endl
		<< "  triangleCount = " << m_triangleCount << "," << endl
		<< "  vertexCount = " << m_vertexCount << "," << endl
		<< "  faceNormals = " << (m_faceNormals ? "true" : "false") << "," << endl
		<< "  hasNormals = " << (m_normals ? "true" : "false") << "," << endl
		<< "  hasTexcoords = " << (m_texcoords ? "true" : "false") << "," << endl
		<< "  hasTangents = " << (m_tangents ? "true" : "false") << "," << endl
		<< "  hasColors = " << (m_colors ? "true" : "false") << "," << endl
		<< "  surfaceArea = " << m_surfaceArea << "," << endl
		<< "  aabb = " << m_aabb.toString() << "," << endl
		<< "  bsdf = " << indent(m_bsdf.toString()) << "," << endl;
	if (isMediumTransition())
		oss << "  interiorMedium = " << indent(m_interiorMedium.toString()) << "," << endl
			<< "  exteriorMedium = " << indent(m_exteriorMedium.toString()) << "," << endl;
	oss << "  subsurface = " << indent(m_subsurface.toString()) << "," << endl
		<< "  emitter = " << indent(m_emitter.toString()) << endl
		<< "]";
	return oss.str();
}

MTS_IMPLEMENT_CLASS_S(TriMesh, false, Shape)
MTS_NAMESPACE_END
