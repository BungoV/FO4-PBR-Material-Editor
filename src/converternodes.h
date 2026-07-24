#pragma once

#include "bgsmreader.h"
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

// Default graphs reproducing the current hardcoded pipeline. These are what a
// fresh conversion starts from; users rewire copies of them.
NodeGraph::Graph defaultFo4Graph();
NodeGraph::Graph defaultFo76Graph();
NodeGraph::Graph defaultGraphFor(const Bgsm::Material& material);

} // namespace ConverterNodes
