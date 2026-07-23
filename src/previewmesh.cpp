#include "previewmesh.h"

#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

#ifdef PBRM_HAS_UFBX
#include "../third_party/ufbx/ufbx.h"
#endif

namespace {

constexpr float Pi = 3.14159265358979323846f;

PreviewVertex vertex(const QVector3D& p, const QVector3D& n, const QVector2D& uv)
{
    return {p, n, QVector4D(1, 0, 0, 1), uv, uv, QVector4D(1, 1, 1, 1)};
}

struct NifReader {
    const QByteArray& data;
    qsizetype offset = 0;
    bool valid = true;

    bool take(qsizetype count) { if (!valid || count < 0 || offset + count > data.size()) { valid=false; return false; } offset+=count; return true; }
    quint8 u8() { if (!take(1)) return 0; return quint8(data[offset-1]); }
    quint16 u16() { if (!take(2)) return 0; return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(data.constData()+offset-2)); }
    quint32 u32() { if (!take(4)) return 0; return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data.constData()+offset-4)); }
    quint64 u64() { if (!take(8)) return 0; return qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(data.constData()+offset-8)); }
    float f32() { return std::bit_cast<float>(u32()); }
    QString bytes(quint32 count) { if (count > quint32(std::numeric_limits<int>::max()) || !take(count)) return {}; return QString::fromLatin1(data.constData()+offset-count,int(count)); }
    QString sized() { return bytes(u32()); }
    QString shortString() { return bytes(u8()); }
};

float halfFloat(const char* source)
{
    const quint16 h=qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(source));
    const quint32 sign=quint32(h&0x8000u)<<16u,exponent=(h>>10u)&0x1fu,mantissa=h&0x3ffu;
    quint32 result=0;
    if(exponent==0){
        if(mantissa==0)result=sign;
        else { quint32 m=mantissa,e=113;while((m&0x400u)==0){m<<=1u;--e;}result=sign|(e<<23u)|((m&0x3ffu)<<13u); }
    }else if(exponent==31)result=sign|0x7f800000u|(mantissa<<13u);
    else result=sign|((exponent+112u)<<23u)|(mantissa<<13u);
    return std::bit_cast<float>(result);
}

float littleFloat(const char* source)
{
    const quint32 bits=qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(source));
    return std::bit_cast<float>(bits);
}

QVector3D decodeByteVector(const char* source)
{
    return QVector3D((quint8(source[0])/127.5f)-1.0f,(quint8(source[1])/127.5f)-1.0f,(quint8(source[2])/127.5f)-1.0f).normalized();
}

QVector3D gameToPreview(const QVector3D& value) { return QVector3D(value.x(),value.z(),-value.y()); }

void addQuad(PreviewMesh& mesh, const QVector3D& a, const QVector3D& b, const QVector3D& c,
    const QVector3D& d, const QVector3D& normal)
{
    const quint32 first = static_cast<quint32>(mesh.vertices.size());
    mesh.vertices << vertex(a, normal, {0, 0}) << vertex(b, normal, {1, 0})
                  << vertex(c, normal, {1, 1}) << vertex(d, normal, {0, 1});
    mesh.indices << first << first + 1 << first + 2 << first << first + 2 << first + 3;
}

QVector2D uvFor(const PreviewVertex& value, int uvSet)
{
    return uvSet == 1 ? value.uv1 : value.uv0;
}

} // namespace

namespace PreviewMeshes {

PreviewMesh plane(PlaneOrientation orientation, bool twoSided)
{
    // OpenGL already renders both windings when culling is disabled, and the
    // preview shader flips back-face normals with gl_FrontFacing. Duplicating
    // this quad at the same coordinates only creates competing depth samples.
    (void)twoSided;
    PreviewMesh mesh;
    QVector3D a(-1, -1, 0), b(1, -1, 0), c(1, 1, 0), d(-1, 1, 0), n(0, 0, 1);
    if (orientation == PlaneOrientation::XZ) {
        a = {-1, 0, -1}; b = {1, 0, -1}; c = {1, 0, 1}; d = {-1, 0, 1}; n = {0, 1, 0};
    } else if (orientation == PlaneOrientation::YZ) {
        a = {0, -1, 1}; b = {0, -1, -1}; c = {0, 1, -1}; d = {0, 1, 1}; n = {1, 0, 0};
    }
    addQuad(mesh, a, b, c, d, n);
    mesh.submeshes.push_back({QStringLiteral("Plane"), 0, int(mesh.indices.size())});
    mesh.radius = std::sqrt(2.0f);
    generateTangents(mesh);
    return mesh;
}

PreviewMesh planeGrid(PlaneOrientation orientation, int subdivisions)
{
    PreviewMesh mesh;
    subdivisions = std::clamp(subdivisions, 2, 256);
    const int row = subdivisions + 1;
    mesh.vertices.reserve(row * row);
    mesh.indices.reserve(subdivisions * subdivisions * 6);
    for (int y = 0; y <= subdivisions; ++y) {
        const float v = float(y) / float(subdivisions);
        for (int x = 0; x <= subdivisions; ++x) {
            const float u = float(x) / float(subdivisions);
            QVector3D p(2.0f * u - 1.0f, 2.0f * v - 1.0f, 0.0f), n(0, 0, 1);
            if (orientation == PlaneOrientation::XZ) {
                p = QVector3D(2.0f * u - 1.0f, 0.0f, 2.0f * v - 1.0f); n = QVector3D(0, 1, 0);
            } else if (orientation == PlaneOrientation::YZ) {
                p = QVector3D(0.0f, 2.0f * v - 1.0f, 1.0f - 2.0f * u); n = QVector3D(1, 0, 0);
            }
            mesh.vertices.push_back(vertex(p, n, QVector2D(u, v)));
        }
    }
    for (int y = 0; y < subdivisions; ++y) for (int x = 0; x < subdivisions; ++x) {
        const quint32 a = quint32(y * row + x), b = a + 1, d = quint32((y + 1) * row + x), c = d + 1;
        mesh.indices << a << b << c << a << c << d;
    }
    mesh.submeshes.push_back({QStringLiteral("Displaced Plane"), 0, int(mesh.indices.size())});
    mesh.radius = std::sqrt(2.0f);
    generateTangents(mesh);
    return mesh;
}

PreviewMesh cube()
{
    PreviewMesh mesh;
    addQuad(mesh, {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1}, { 0, 0, 1});
    addQuad(mesh, { 1,-1,-1}, {-1,-1,-1}, {-1, 1,-1}, { 1, 1,-1}, { 0, 0,-1});
    addQuad(mesh, { 1,-1, 1}, { 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1}, { 1, 0, 0});
    addQuad(mesh, {-1,-1,-1}, {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1}, {-1, 0, 0});
    addQuad(mesh, {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1}, {-1, 1,-1}, { 0, 1, 0});
    addQuad(mesh, {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1}, {-1,-1, 1}, { 0,-1, 0});
    mesh.submeshes.push_back({QStringLiteral("Cube"), 0, int(mesh.indices.size())});
    mesh.radius = std::sqrt(3.0f);
    generateTangents(mesh);
    return mesh;
}

PreviewMesh sphere(int segments, int rings)
{
    PreviewMesh mesh;
    segments = std::max(segments, 8); rings = std::max(rings, 4);
    for (int y = 0; y <= rings; ++y) {
        const float v = float(y) / float(rings);
        const float phi = v * Pi;
        for (int x = 0; x <= segments; ++x) {
            const float u = float(x) / float(segments);
            const float theta = u * 2.0f * Pi;
            QVector3D n(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
            PreviewVertex out = vertex(n, n, {u, 1.0f - v});
            out.tangent = QVector4D(-std::sin(theta), 0, std::cos(theta), 1);
            mesh.vertices.push_back(out);
        }
    }
    for (int y = 0; y < rings; ++y) for (int x = 0; x < segments; ++x) {
        const quint32 a = y * (segments + 1) + x;
        const quint32 b = a + segments + 1;
        // Keep the winding consistent with the authored outward vertex normals.
        // The old inward winding was hidden while culling was disabled, but
        // exposed the back hemisphere as a dark "hole" once material-sidedness
        // began controlling GL back-face culling.
        // Alternate the quad diagonal checkerboard-style: one consistent
        // diagonal biases the normal-interpolation error directionally, which
        // glossy shading reveals as pole-centred spokes on the sphere.
        if ((x + y) & 1)
            mesh.indices << a << a + 1 << b << a + 1 << b + 1 << b;
        else
            mesh.indices << a << a + 1 << b + 1 << a << b + 1 << b;
    }
    mesh.submeshes.push_back({QStringLiteral("Sphere"), 0, int(mesh.indices.size())});
    mesh.radius = 1.0f;
    return mesh;
}

void normalizeAndCenter(PreviewMesh& mesh)
{
    if (mesh.vertices.isEmpty()) return;
    QVector3D minimum(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    QVector3D maximum(-minimum.x(), -minimum.y(), -minimum.z());
    for (const auto& v : mesh.vertices) {
        minimum.setX(std::min(minimum.x(), v.position.x())); minimum.setY(std::min(minimum.y(), v.position.y())); minimum.setZ(std::min(minimum.z(), v.position.z()));
        maximum.setX(std::max(maximum.x(), v.position.x())); maximum.setY(std::max(maximum.y(), v.position.y())); maximum.setZ(std::max(maximum.z(), v.position.z()));
    }
    const QVector3D center = (minimum + maximum) * 0.5f;
    float radius = 0.0f;
    for (const auto& v : mesh.vertices) radius = std::max(radius, (v.position - center).length());
    const float scale = radius > 1.0e-6f ? 1.0f / radius : 1.0f;
    for (auto& v : mesh.vertices) v.position = (v.position - center) * scale;
    mesh.radius = 1.0f;
    mesh.diagnostics << QStringLiteral("Centered and normalized to a unit-radius preview mesh.");
}

void generateMissingNormals(PreviewMesh& mesh)
{
    bool missing = false;
    for (const auto& v : mesh.vertices) if (v.normal.lengthSquared() < 1.0e-8f) { missing = true; break; }
    if (!missing) return;
    QVector<QVector3D> accumulated(mesh.vertices.size(), QVector3D());
    for (int i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const quint32 ia = mesh.indices[i], ib = mesh.indices[i + 1], ic = mesh.indices[i + 2];
        if (ia >= quint32(mesh.vertices.size()) || ib >= quint32(mesh.vertices.size()) || ic >= quint32(mesh.vertices.size())) continue;
        const QVector3D n = QVector3D::crossProduct(mesh.vertices[ib].position - mesh.vertices[ia].position,
            mesh.vertices[ic].position - mesh.vertices[ia].position);
        accumulated[int(ia)] += n; accumulated[int(ib)] += n; accumulated[int(ic)] += n;
    }
    for (int i = 0; i < mesh.vertices.size(); ++i) if (mesh.vertices[i].normal.lengthSquared() < 1.0e-8f)
        mesh.vertices[i].normal = accumulated[i].lengthSquared() > 1.0e-8f ? accumulated[i].normalized() : QVector3D(0, 1, 0);
    mesh.generatedNormals = true;
    mesh.diagnostics << QStringLiteral("Missing normals were generated.");
}

void generateTangents(PreviewMesh& mesh, int uvSet)
{
    if (mesh.vertices.isEmpty()) return;
    QVector<QVector3D> tangents(mesh.vertices.size(), QVector3D());
    QVector<QVector3D> bitangents(mesh.vertices.size(), QVector3D());
    for (int i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const int ia = int(mesh.indices[i]), ib = int(mesh.indices[i + 1]), ic = int(mesh.indices[i + 2]);
        if (ia < 0 || ib < 0 || ic < 0 || ia >= mesh.vertices.size() || ib >= mesh.vertices.size() || ic >= mesh.vertices.size()) continue;
        const QVector3D e1 = mesh.vertices[ib].position - mesh.vertices[ia].position;
        const QVector3D e2 = mesh.vertices[ic].position - mesh.vertices[ia].position;
        const QVector2D d1 = uvFor(mesh.vertices[ib], uvSet) - uvFor(mesh.vertices[ia], uvSet);
        const QVector2D d2 = uvFor(mesh.vertices[ic], uvSet) - uvFor(mesh.vertices[ia], uvSet);
        const float determinant = d1.x() * d2.y() - d1.y() * d2.x();
        if (std::abs(determinant) < 1.0e-8f) continue;
        const float r = 1.0f / determinant;
        const QVector3D t = (e1 * d2.y() - e2 * d1.y()) * r;
        const QVector3D b = (e2 * d1.x() - e1 * d2.x()) * r;
        tangents[ia] += t; tangents[ib] += t; tangents[ic] += t;
        bitangents[ia] += b; bitangents[ib] += b; bitangents[ic] += b;
    }
    for (int i = 0; i < mesh.vertices.size(); ++i) {
        const QVector3D n = mesh.vertices[i].normal.normalized();
        QVector3D t = tangents[i] - n * QVector3D::dotProduct(n, tangents[i]);
        if (t.lengthSquared() < 1.0e-8f) t = QVector3D::crossProduct(std::abs(n.y()) < 0.9f ? QVector3D(0,1,0) : QVector3D(1,0,0), n);
        t.normalize();
        const float handedness = QVector3D::dotProduct(QVector3D::crossProduct(n, t), bitangents[i]) < 0.0f ? -1.0f : 1.0f;
        mesh.vertices[i].tangent = QVector4D(t, handedness);
    }
    mesh.generatedTangents = true;
}

PreviewMesh loadStaticFbx(const QString& path, QString* error)
{
    if (error) error->clear();
    PreviewMesh result;
    result.sourcePath = path;
    if (QFileInfo(path).suffix().compare(QStringLiteral("fbx"), Qt::CaseInsensitive) != 0) {
        if (error) *error = QStringLiteral("Choose a static .fbx file.");
        return result;
    }
#ifndef PBRM_HAS_UFBX
    if (error) *error = QStringLiteral("This build does not include the ufbx importer.");
    return result;
#else
    ufbx_load_opts opts = {};
    opts.load_external_files = false;
    opts.ignore_embedded = true;
    ufbx_error loadError;
    const QByteArray nativePath = QFile::encodeName(path);
    ufbx_scene* scene = ufbx_load_file(nativePath.constData(), &opts, &loadError);
    if (!scene) {
        if (error) *error = QString::fromUtf8(loadError.description.data, int(loadError.description.length));
        return result;
    }
    if (scene->anim_stacks.count > 0) result.diagnostics << QStringLiteral("Animation data was ignored; preview import is static only.");
    for (size_t nodeIndex = 0; nodeIndex < scene->nodes.count; ++nodeIndex) {
        const ufbx_node* node = scene->nodes.data[nodeIndex];
        const ufbx_mesh* mesh = node->mesh;
        if (!mesh || mesh->num_faces == 0) continue;
        if (mesh->skin_deformers.count > 0) result.diagnostics << QStringLiteral("%1: skinning was ignored.").arg(QString::fromUtf8(node->name.data, int(node->name.length)));
        PreviewSubmesh submesh;
        submesh.name = QString::fromUtf8(node->name.data, int(node->name.length));
        if (submesh.name.isEmpty()) submesh.name = QStringLiteral("Mesh %1").arg(result.submeshes.size() + 1);
        submesh.firstIndex = result.indices.size();
        QVector<quint32> triangle(mesh->max_face_triangles * 3);
        for (size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex) {
            const ufbx_face face = mesh->faces.data[faceIndex];
            const size_t triangleCount = ufbx_triangulate_face(triangle.data(), size_t(triangle.size()), mesh, face);
            for (size_t corner = 0; corner < triangleCount * 3; ++corner) {
                const uint32_t index = triangle[int(corner)];
                const ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
                const ufbx_vec3 n = mesh->vertex_normal.exists ? ufbx_get_vertex_vec3(&mesh->vertex_normal, index) : ufbx_vec3{};
                const ufbx_vec2 uv0 = mesh->vertex_uv.exists ? ufbx_get_vertex_vec2(&mesh->vertex_uv, index) : ufbx_vec2{};
                const ufbx_vec2 uv1 = mesh->uv_sets.count > 1 ? ufbx_get_vertex_vec2(&mesh->uv_sets.data[1].vertex_uv, index) : uv0;
                const ufbx_vec4 color = mesh->vertex_color.exists ? ufbx_get_vertex_vec4(&mesh->vertex_color, index) : ufbx_vec4{1,1,1,1};
                const ufbx_vec3 wp = ufbx_transform_position(&node->geometry_to_world, p);
                const ufbx_vec3 wn = ufbx_transform_direction(&node->geometry_to_world, n);
                PreviewVertex v;
                v.position = QVector3D(float(wp.x), float(wp.y), float(wp.z));
                v.normal = QVector3D(float(wn.x), float(wn.y), float(wn.z)).normalized();
                v.uv0 = QVector2D(float(uv0.x), 1.0f - float(uv0.y));
                v.uv1 = QVector2D(float(uv1.x), 1.0f - float(uv1.y));
                v.color = QVector4D(float(color.x),float(color.y),float(color.z),float(color.w));
                result.indices.push_back(result.vertices.size());
                result.vertices.push_back(v);
            }
        }
        submesh.indexCount = result.indices.size() - submesh.firstIndex;
        if (submesh.indexCount > 0) result.submeshes.push_back(submesh);
        result.hasUv1 = result.hasUv1 || mesh->uv_sets.count > 1;
    }
    ufbx_free_scene(scene);
    if (!result.isValid()) {
        if (error) *error = QStringLiteral("No static triangle meshes were found in the FBX.");
        return {};
    }
    generateMissingNormals(result);
    generateTangents(result);
    normalizeAndCenter(result);
    result.diagnostics << QStringLiteral("%1 vertices, %2 triangles, %3 submeshes, UV0%4.")
        .arg(result.vertices.size()).arg(result.indices.size() / 3).arg(result.submeshes.size())
        .arg(result.hasUv1 ? QStringLiteral(" + UV1") : QString());
    return result;
#endif
}

PreviewMesh loadStaticNif(const QString& path, QString* error)
{
    if(error)error->clear();
    PreviewMesh result;result.sourcePath=path;
    if(QFileInfo(path).suffix().compare(QStringLiteral("nif"),Qt::CaseInsensitive)!=0){if(error)*error=QStringLiteral("Choose a Fallout 4 .nif file.");return result;}
    QFile file(path);if(!file.open(QIODevice::ReadOnly)){if(error)*error=file.errorString();return result;}
    const QByteArray data=file.readAll();const qsizetype newline=data.indexOf('\n');
    if(newline<0||!data.startsWith("Gamebryo File Format")){if(error)*error=QStringLiteral("Invalid NIF header.");return result;}
    NifReader reader{data,newline+1};
    const quint32 version=reader.u32();const quint8 endian=reader.u8();const quint32 userVersion=reader.u32();const quint32 blockCount=reader.u32();
    const quint32 bsVersion=reader.u32();
    if(endian!=1||version!=0x14020007u||userVersion!=12u||bsVersion<130u){
        if(error)*error=QStringLiteral("Static preview currently supports Fallout 4 little-endian NIFs (20.2.0.7, BS 130+).");
        return result;
    }
    reader.shortString();reader.shortString();reader.shortString();if(bsVersion>=130)reader.shortString();
    const quint16 typeCount=reader.u16();QVector<QString> types;types.reserve(typeCount);
    for(int i=0;i<typeCount;++i)types.push_back(reader.sized());
    QVector<quint16> typeIndices;typeIndices.reserve(int(blockCount));for(quint32 i=0;i<blockCount;++i)typeIndices.push_back(reader.u16());
    QVector<quint32> blockSizes;blockSizes.reserve(int(blockCount));for(quint32 i=0;i<blockCount;++i)blockSizes.push_back(reader.u32());
    const quint32 stringCount=reader.u32();reader.u32();QVector<QString> strings;strings.reserve(int(stringCount));for(quint32 i=0;i<stringCount;++i)strings.push_back(reader.sized());
    const quint32 groupCount=reader.u32();for(quint32 i=0;i<groupCount;++i)reader.u32();
    if(!reader.valid||blockCount>100000u){if(error)*error=QStringLiteral("Invalid or truncated Fallout 4 NIF block table.");return {};}
    QVector<qsizetype> blockOffsets;blockOffsets.reserve(int(blockCount));qsizetype blockOffset=reader.offset;
    for(quint32 i=0;i<blockCount;++i){blockOffsets.push_back(blockOffset);blockOffset+=blockSizes[int(i)];if(blockOffset>data.size()){if(error)*error=QStringLiteral("Truncated Fallout 4 NIF block data.");return {};}}

    constexpr quint64 vfUv=1ull<<1u,vfUv2=1ull<<2u,vfNormal=1ull<<3u,vfTangent=1ull<<4u,vfColor=1ull<<5u,vfSkinned=1ull<<6u,vfFullPrecision=0x400ull;
    const auto attributeOffset=[](quint64 descriptor,int attribute){return int((descriptor>>(4*attribute+2))&0x3cu);};
    for(quint32 block=0;block<blockCount;++block){
        const int typeIndex=typeIndices[int(block)];if(typeIndex<0||typeIndex>=types.size())continue;
        const QString type=types[typeIndex];
        if(type!=QStringLiteral("BSTriShape")&&type!=QStringLiteral("BSSubIndexTriShape")&&type!=QStringLiteral("BSMeshLODTriShape"))continue;
        NifReader shape{data,blockOffsets[int(block)]};
        const quint32 nameIndex=shape.u32();const quint32 extraCount=shape.u32();for(quint32 i=0;i<extraCount;++i)shape.u32();shape.u32();shape.u32();
        const QVector3D translation(shape.f32(),shape.f32(),shape.f32());float rotation[9];for(float& value:rotation)value=shape.f32();const float scale=shape.f32();
        shape.u32();for(int i=0;i<4;++i)shape.f32();shape.u32();shape.u32();shape.u32();
        const quint64 descriptor=shape.u64();const quint32 triangleCount=shape.u32();const quint16 vertexCount=shape.u16();const quint32 dataSize=shape.u32();
        const quint64 flags=descriptor>>44u;const int stride=int(descriptor&0xfull)*4;
        const qsizetype vertexStart=shape.offset,triangleStart=vertexStart+qsizetype(vertexCount)*stride;
        if(!shape.valid||stride<8||vertexCount==0||triangleCount==0||dataSize<quint32(vertexCount)*quint32(stride)
            ||triangleStart+qsizetype(triangleCount)*6>blockOffsets[int(block)]+blockSizes[int(block)])continue;
        PreviewSubmesh submesh;submesh.name=nameIndex<quint32(strings.size())?strings[int(nameIndex)]:QStringLiteral("NIF Shape %1").arg(block);
        if(submesh.name.isEmpty())submesh.name=QStringLiteral("NIF Shape %1").arg(block);
        submesh.firstIndex=result.indices.size();
        const quint32 baseVertex=quint32(result.vertices.size());
        for(int i=0;i<vertexCount;++i){
            const char* source=data.constData()+vertexStart+qsizetype(i)*stride;PreviewVertex vertex;
            QVector3D position;
            if(flags&vfFullPrecision)position=QVector3D(littleFloat(source),littleFloat(source+4),littleFloat(source+8));
            else position=QVector3D(halfFloat(source),halfFloat(source+2),halfFloat(source+4));
            QVector3D transformed(
                rotation[0]*position.x()+rotation[1]*position.y()+rotation[2]*position.z(),
                rotation[3]*position.x()+rotation[4]*position.y()+rotation[5]*position.z(),
                rotation[6]*position.x()+rotation[7]*position.y()+rotation[8]*position.z());
            vertex.position=gameToPreview(translation+transformed*scale);
            if(flags&vfUv){const int o=attributeOffset(descriptor,1);if(o+4<=stride)vertex.uv0=QVector2D(halfFloat(source+o),halfFloat(source+o+2));}
            vertex.uv1=vertex.uv0;
            if(flags&vfUv2){const int o=attributeOffset(descriptor,2);if(o+4<=stride){vertex.uv1=QVector2D(halfFloat(source+o),halfFloat(source+o+2));result.hasUv1=true;}}
            if(flags&vfNormal){const int o=attributeOffset(descriptor,3);if(o+3<=stride)vertex.normal=gameToPreview(decodeByteVector(source+o));}
            if(flags&vfTangent){const int o=attributeOffset(descriptor,4);if(o+3<=stride){const QVector3D tangent=gameToPreview(decodeByteVector(source+o));vertex.tangent=QVector4D(tangent,1);}}
            if(flags&vfColor){const int o=attributeOffset(descriptor,5);if(o+4<=stride)vertex.color=QVector4D(quint8(source[o])/255.0f,quint8(source[o+1])/255.0f,quint8(source[o+2])/255.0f,quint8(source[o+3])/255.0f);}
            result.vertices.push_back(vertex);
        }
        for(quint32 i=0;i<triangleCount*3u;++i){const qsizetype o=triangleStart+qsizetype(i)*2;const quint16 index=qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(data.constData()+o));if(index<vertexCount)result.indices.push_back(baseVertex+index);}
        submesh.indexCount=result.indices.size()-submesh.firstIndex;if(submesh.indexCount>=3)result.submeshes.push_back(submesh);
        if(flags&vfSkinned)result.diagnostics<<QStringLiteral("%1: skinning/controllers ignored; bind-pose geometry shown.").arg(submesh.name);
    }
    if(!result.isValid()){if(error)*error=QStringLiteral("No supported Fallout 4 BSTriShape geometry was found.");return {};}
    generateMissingNormals(result);generateTangents(result);normalizeAndCenter(result);
    result.diagnostics<<QStringLiteral("Fallout 4 NIF static preview: %1 vertices, %2 triangles, %3 submeshes.").arg(result.vertices.size()).arg(result.indices.size()/3).arg(result.submeshes.size());
    return result;
}

} // namespace PreviewMeshes
