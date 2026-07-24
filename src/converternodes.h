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
    const QString& textureRoot, const QString& referenceMesh, QStringList* errors = nullptr);

// Loads a .pbrgraph; returns an empty graph and sets error when unreadable.
NodeGraph::Graph loadGraphFile(const QString& path, QString* error = nullptr);
bool saveGraphFile(const NodeGraph::Graph& graph, const QString& path, QString* error = nullptr);

// Default graphs reproducing the current hardcoded pipeline. These are what a
// fresh conversion starts from; users rewire copies of them.
NodeGraph::Graph defaultFo4Graph();
NodeGraph::Graph defaultFo76Graph();
NodeGraph::Graph defaultGraphFor(const Bgsm::Material& material);

} // namespace ConverterNodes
