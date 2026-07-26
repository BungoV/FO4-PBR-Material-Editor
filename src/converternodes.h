#pragma once

#include "bgsmreader.h"
#include "converter.h"
#include "nodegraph.h"

// The converter's node library: every stage of the FO4/FO76 -> PBR pipeline
// (docs/FO4-Conversion.md) exposed as a node type over the domain-agnostic
// core in nodegraph.h. The node implementations call the same
// convertercore/normalderive/geometryao code the orchestrator uses, so a graph
// and a batch run produce the same images.
namespace ConverterNodes {

// Opaque payload tag for Bgsm::Material values travelling between nodes.
extern const char* const kMaterialTag;

void registerAll(NodeGraph::Registry& registry);

// A registry with the whole library already registered — the graph runner and
// the workspace share one.
const NodeGraph::Registry& sharedRegistry();

// Runs a graph at full resolution for one material and harvests its
// output.texture nodes by role, ready to hand to Converter::JobOptions so the
// orchestrator writes them exactly as it writes a batch conversion.
Converter::SuppliedImages runGraph(const NodeGraph::Graph& graph, const QString& materialPath,
    const QString& textureRoot, const QString& referenceMesh, const QString& correctionMaskPath = QString(),
    QStringList* errors = nullptr);

// Largest authored texture dimension across the material's slots. This is the
// bar every map in the material is measured against: anything below it is
// flagged on the canvas as under-resolution.
int materialMaxResolution(const Bgsm::Material& material, const QString& textureRoot);

// Fallout 76 authors _r under two conventions and the DDS format is what tells
// them apart, so the same stored number means two different reflectances:
//
//   BC1 sRGB (36% of a 900-material survey) — dielectric floor 0.22, metals to
//     0.70 at the median map's p99.
//   BC4 linear (64%, and 84% of those are a 4x4 flat holding one constant) —
//     dielectric 0.035, textured maps reaching 0.208 at p99.
//
// This maps a linear BC4 map onto the BC1 scale: the sRGB transfer, then a
// stretch that lands 0.035 on the 0.22 dielectric floor and 0.208 on the 0.70
// metal top. Both conventions then span the same range, so ONE pair of Levels
// thresholds gives a full-spectrum metal mask on either — without the graph
// having to branch on compression, which is the whole point.
//
// A no-op on an already-sRGB map. Exposed because it is a rule about the game's
// data rather than an implementation detail, and because it is worth testing.
QImage normaliseReflectanceToSrgbScale(const QImage& image, bool sourceIsSrgb);

// Loads a .pbrgraph; returns an empty graph and sets error when unreadable.
NodeGraph::Graph loadGraphFile(const QString& path, QString* error = nullptr);
bool saveGraphFile(const NodeGraph::Graph& graph, const QString& path, QString* error = nullptr);

// Default graphs reproducing the current hardcoded pipeline. These are what a
// fresh conversion starts from; users rewire copies of them.
NodeGraph::Graph defaultFo4Graph();
NodeGraph::Graph defaultFo76Graph();
NodeGraph::Graph defaultGraphFor(const Bgsm::Material& material);

} // namespace ConverterNodes
