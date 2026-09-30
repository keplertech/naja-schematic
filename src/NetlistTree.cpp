#include "NetlistTree.h"

#include <imgui.h>

#include "Console.h"
#include "DiagnosisStore.h"
#include "INetlistProvider.h"
#include "SelectionStore.h"
#include "TraceStore.h"

namespace {

// Hover help for the "(replace view)" / "(add to view)" menu entries.
void viewModeTooltip(bool clearView) {
  if (!ImGui::IsItemHovered()) return;
  ImGui::SetTooltip(clearView ? "Clears the schematic (and its traces) first"
                              : "Keeps what the schematic already shows");
}

}  // namespace

void NetlistTree::createRootNode(
  const std::string& name,
  const DesignRef& design_ref,
  bool hasTerms,
  bool hasPrimitives,
  bool hasInstances,
  bool hasNets,
  const std::optional<SourceLoc>& sourceLoc) {
  root_ = new NetlistTreeInstanceNode(this, name, design_ref, hasTerms, hasPrimitives, hasInstances, hasNets, sourceLoc);
}

void NetlistTree::render() {
  // A selection made elsewhere (schematic click, host focus_instance) is
  // revealed here; one made by clicking this tree is already visible.
  if (SelectionStore::revision() != seenSelectionRevision_) {
    seenSelectionRevision_ = SelectionStore::revision();
    if (SelectionStore::hasSelection() &&
        SelectionStore::origin() != SelectionStore::Origin::Tree)
      reveal(SelectionStore::selected());
  }
  advanceReveal();
  if (root_) {
    root_->render();
  } else {
    ImGui::Text("Loading Root Node...");
  }
}

void NetlistTree::reveal(const InstancePath& path) {
  revealPath_ = path;
}

void NetlistTree::advanceReveal() {
  if (!revealPath_ || !root_) return;
  NetlistTreeNode* node = root_;
  std::vector<NetlistTreeNode*> toOpen;
  for (const auto& ref : *revealPath_) {
    node->expand();  // an instance node's groups are created locally
    std::vector<NetlistTreeGroupNode*> groups;
    for (auto* child : *node->children_) {
      auto* g = dynamic_cast<NetlistTreeGroupNode*>(child);
      if (g && (g->getType() == NetlistTreeGroupNode::Type::Instances ||
                g->getType() == NetlistTreeGroupNode::Type::Primitives))
        groups.push_back(g);
    }
    NetlistTreeNode* next = nullptr;
    NetlistTreeGroupNode* via = nullptr;
    for (auto* g : groups) {
      if (!g->children_) continue;
      for (auto* c : *g->children_) {
        auto* inst = dynamic_cast<NetlistTreeInstanceNode*>(c);
        if (inst && inst->getChildID() == ref.id) { next = inst; via = g; break; }
      }
      if (next) break;
    }
    if (!next) {
      // Not among the loaded levels: load the ones still missing and wait.
      bool waiting = false;
      for (auto* g : groups) {
        if (g->children_) continue;
        if (!g->hasRequestedChildren_) {
          g->hasRequestedChildren_ = true;
          g->sendLoadRequest();
        }
        waiting = true;
      }
      if (!waiting) {
        Console::Error("Cannot reveal instance: no '" + displayName(ref) + "' under " +
                       (node->getInstancePath().empty() ? node->getLabel()
                                                        : displayPath(node->getInstancePath())));
        revealPath_.reset();
      }
      return;
    }
    toOpen.push_back(node);
    toOpen.push_back(via);
    node = next;
  }
  for (auto* n : toOpen) forceOpen_.insert(n->guiID_);
  scrollTo_ = node->guiID_;
  revealPath_.reset();
}

NetlistTreeNode* NetlistTree::findInstance(const InstancePath& path) const {
  NetlistTreeNode* node = root_;
  for (const auto& ref : path) {
    if (!node || !node->children_) return nullptr;
    NetlistTreeNode* next = nullptr;
    for (auto* group : *node->children_) {
      if (!group->children_) continue;
      for (auto* c : *group->children_) {
        auto* inst = dynamic_cast<NetlistTreeInstanceNode*>(c);
        if (inst && inst->getChildID() == ref.id) { next = inst; break; }
      }
      if (next) break;
    }
    node = next;
  }
  return node;
}

void NetlistTree::insertNodeInMap(NetlistTreeNode* node) {
  node->guiID_ = nextGUIID_;
  nodes_[nextGUIID_++] = node;
}

NetlistTreeNode* NetlistTree::getNode(unsigned guiID) const {
  auto it = nodes_.find(guiID);
  return (it != nodes_.end()) ? it->second : nullptr;
}

std::string NetlistTreeInstanceNode::getLabel() const {
  if (isRoot()) return name_.empty() ? "<unnamed root>" : name_;
  std::string label = displayName({childID_, name_});
  if (!modelName_.empty()) label += " (" + modelName_ + ")";
  return label;
}

void NetlistTreeNode::getPath(NetlistTree::Path& path) const {
  getParent()->getPath(path);
}

InstancePath NetlistTreeNode::getInstancePath() const {
  auto* parent = getParent();
  return parent ? parent->getInstancePath() : InstancePath();
}

void NetlistTreeNode::render() {
  expand();
  NetlistTree* tree = getTree();
  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
  if (isLeaf()) {
    flags = flags | ImGuiTreeNodeFlags_Leaf;
  }
  if (isInstanceNode() && SelectionStore::isSelected(getInstancePath())) {
    flags = flags | ImGuiTreeNodeFlags_Selected;
  }
  if (tree->forceOpen_.erase(guiID_)) {
    ImGui::SetNextItemOpen(true);
  }
  if (getColor() != 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, getColor());
  }
  auto isOpen = ImGui::TreeNodeEx((void*)(intptr_t)guiID_, flags, "%s", getLabel().c_str());
  if (getColor() != 0) {
    ImGui::PopStyleColor();
  }
  if (tree->scrollTo_ == guiID_) {
    ImGui::SetScrollHereY(0.5f);
    tree->scrollTo_.reset();
  }
  // Clicking an instance's label (not its open/close arrow) selects it.
  if (isInstanceNode() && ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
    SelectionStore::select(getInstancePath(), SelectionStore::Origin::Tree);
  }
  if (ImGui::IsItemHovered()) {
    auto diagnostics = getDiagnostics();
    if (!diagnostics.empty()) {
      ImGui::BeginTooltip();
      for (const auto* d : diagnostics) {
        ImGui::TextColored(ImColor(DiagnosisStore::colorForSeverity(d->severity)).Value,
                           "[%s] %s", toString(d->severity), d->message.c_str());
        if (!d->source.empty()) ImGui::TextDisabled("source: %s", d->source.c_str());
      }
      ImGui::EndTooltip();
    }
  }
  if (isBitTerm()) {
    if (ImGui::BeginPopupContextItem()) {
      for (bool clearView : {true, false}) {
        bool clicked = ImGui::MenuItem(clearView ? "Show Equipotential (replace view)"
                                                 : "Show Equipotential (add to view)");
        viewModeTooltip(clearView);
        if (!clicked) continue;
        NetlistTree::Path path;
        getPath(path);
        getTree()->sendLoadEquipotential(
          path,
          NetlistTree::TermID{getChildID(), isBusBit(), getBusBit()}, clearView);
      }
      for (bool clearView : {true, false}) {
        bool clicked = ImGui::MenuItem(clearView ? "Trace to Driver (replace view)"
                                                 : "Trace to Driver (add to view)");
        viewModeTooltip(clearView);
        if (!clicked) continue;
        NetlistTree::Path path;
        getPath(path);
        std::string label = getTermBaseName();
        if (isBusBit()) label += "[" + std::to_string(getBusBit()) + "]";
        getTree()->sendTraceDriver(
          path, getChildID(),
          isBusBit() ? std::vector<int>{getBusBit()} : std::vector<int>{},
          traceLabel(getInstancePath(), label), clearView);
      }
      if (ImGui::MenuItem("Show Properties")) {
        json req;
        req["request"]  = "get_properties";
        req["kind"]     = "term";
        writePath(req, getInstancePath());
        req["terminal"] = getTermBaseName();
        if (isBusBit()) req["bit"] = getBusBit();
        getTree()->getProvider()->send(req.dump());
      }
      ImGui::EndPopup();
    }
  } else if (isBus()) {
    if (ImGui::BeginPopupContextItem()) {
      // No bus-level Show Equipotential or Trace to Driver for now: both
      // work one bit at a time, from the bit rows.
      if (ImGui::MenuItem("Show Properties")) {
        json req;
        req["request"]  = "get_properties";
        req["kind"]     = "term";
        writePath(req, getInstancePath());
        req["terminal"] = getTermBaseName();
        getTree()->getProvider()->send(req.dump());
      }
      ImGui::EndPopup();
    }
  } else if (isBitNet() || isBusNet()) {
    if (ImGui::BeginPopupContextItem()) {
      if (ImGui::MenuItem("Show Properties")) {
        json req;
        req["request"] = "get_properties";
        req["kind"]    = "net";
        writePath(req, getInstancePath());
        req["net"]     = getNetBaseName();
        if (isBusBit()) req["bit"] = getBusBit();
        getTree()->getProvider()->send(req.dump());
      }
      ImGui::EndPopup();
    }
  } else if (isInstanceNode()) {
    if (ImGui::BeginPopupContextItem()) {
      if (auto loc = getSourceLoc()) {
        if (ImGui::MenuItem("Show RTL Source")) {
          json req;
          req["request"] = "load_source";
          req["file"]    = loc->file;
          req["line"]    = loc->line;
          getTree()->getProvider()->send(req.dump());
        }
      }
      if (ImGui::MenuItem("Show Properties")) {
        json req;
        req["request"] = "get_properties";
        req["kind"]    = "instance";
        writePath(req, getInstancePath());
        getTree()->getProvider()->send(req.dump());
      }
      ImGui::EndPopup();
    }
  }
  if (isOpen and not isLeaf()) {
    if (children_ != nullptr) {
      for (auto child : *children_) {
        child->render();
      }
    } else {
      auto flags = ImGuiTreeNodeFlags_Leaf;
      ImGui::TreeNodeEx("Loading...", flags);
      if (not hasRequestedChildren_) {
        hasRequestedChildren_ = true;
        sendLoadRequest();
      }
      ImGui::TreePop();
    }
  }
  if (isOpen) {
    ImGui::TreePop();
  }
}

NetlistTreeInstanceNode::NetlistTreeInstanceNode(
    NetlistTree* tree,
    const std::string& name,
    const DesignRef& designRef,
    bool hasTerms,
    bool hasPrimitives,
    bool hasInstances,
    bool hasNets,
    const std::optional<SourceLoc>& sourceLoc):
    NetlistTreeNode(tree),
    isRoot_(true), name_(name),
    childID_(0),designRef_(designRef),
    hasTerms_(hasTerms), hasPrimitives_(hasPrimitives), hasInstances_(hasInstances),
    hasNets_(hasNets),
    sourceLoc_(sourceLoc)
{}

NetlistTreeInstanceNode::NetlistTreeInstanceNode(
    NetlistTreeNode* parent,
    const std::string& name,
    const std::string& modelName,
    unsigned childID,
    const DesignRef& designRef,
    bool hasTerms,
    bool hasPrimitives,
    bool hasInstances,
    bool hasNets,
    const std::optional<SourceLoc>& sourceLoc):
    NetlistTreeNode(parent),
    isRoot_(false), name_(name), modelName_(modelName),
    childID_(childID), designRef_(designRef),
    hasTerms_(hasTerms), hasPrimitives_(hasPrimitives), hasInstances_(hasInstances),
    hasNets_(hasNets),
    sourceLoc_(sourceLoc)
{}

void NetlistTreeInstanceNode::getPath(NetlistTree::Path& path) const {
  if (isRoot()) {
    path = NetlistTree::Path();
    return;
  } else {
    getParent()->getPath(path);
  }
  path.push_back(childID_);
}

InstancePath NetlistTreeInstanceNode::getInstancePath() const {
  if (isRoot()) return {};
  InstancePath path = getParent() ? getParent()->getInstancePath() : InstancePath();
  path.push_back({childID_, name_});
  return path;
}

ImU32 NetlistTreeInstanceNode::getColor() const {
  if (isRoot()) return 0;
  return DiagnosisStore::instanceColor(getInstancePath());
}

std::vector<const DiagnosisItem*> NetlistTreeInstanceNode::getDiagnostics() const {
  if (isRoot()) return {};
  return DiagnosisStore::instanceDiagnostics(getInstancePath());
}

NetlistTreeGroupNode::NetlistTreeGroupNode(
  NetlistTreeNode* parent,
  Type type): NetlistTreeNode(parent), type_(type) {
}

std::string NetlistTreeGroupNode::getLabel() const {
  switch (type_) {
    case Type::Terms:
      return "Terms";
    case Type::Nets:
      return "Nets";
    case Type::Primitives:
      return "Primitives";
    case Type::Instances:
      return "Instances";
    default:
      return "Unknown Group";
  }
}

DesignRef NetlistTreeNode::getDesignRef() const {
  return getParent()->getDesignRef();
}

NetlistTreeInstanceNode* NetlistTreeNode::getInstanceNode() const {
  return getParent()->getInstanceNode();
}

void NetlistTreeGroupNode::sendLoadRequest() const {
  Console::Log("Sending load request for group: " + getLabel());
  std::string request;
  switch (type_) {
    case Type::Terms:
      request = R"({"request":"load_terms",)";
      break;
    case Type::Nets:
      request = R"({"request":"load_nets",)";
      break;
    case Type::Primitives:
      request = R"({"request":"load_primitives",)";
      break;
    case Type::Instances:
      request = R"({"request":"load_instances",)";
      break;
    default:
      Console::Error("Unknown group type for load request");
      return;
  }
  request += R"("gui_id":)" + std::to_string(guiID_) + ",";
  auto designRef = getDesignRef();
  request += R"("design_ref":{"db_id":)" +
        std::to_string(designRef.db_id) +
        R"(,"library_id":)" +
        std::to_string(designRef.library_id) +
        R"(,"design_id":)" +
        std::to_string(designRef.design_id) +
        R"(}})";
  Console::Log("Sending load request: " + request);
  getTree()->getProvider()->send(request);
}

NetlistTreeTermNode::NetlistTreeTermNode(
  NetlistTreeNode* parent,
  const std::string& name,
  unsigned childID,
  Direction direction,
  std::optional<int> msb,
  std::optional<int> lsb):
  NetlistTreeNode(parent), name_(name), childID_(childID), direction_(direction), msb_(msb), lsb_(lsb) {
  if (not msb_.has_value() or not lsb_.has_value()) { //scalar
    children_ = new Children();
  }
}

bool NetlistTreeTermNode::isTopTerm() const {
  return getInstanceNode()->isRoot();
}

size_t NetlistTreeTermNode::getWidth() const {
  return std::abs(msb_.value_or(0) - lsb_.value_or(0)) + 1;
}

std::vector<int> NetlistTreeTermNode::busBits() const {
  std::vector<int> bits;
  if (msb_.has_value() and lsb_.has_value()) {
    int msb = msb_.value();
    int lsb = lsb_.value();
    auto width = getWidth();
    bits.reserve(width);
    for (size_t i=0; i<width; i++) {
      bits.push_back((msb>lsb)?msb-int(i):msb+int(i));
    }
  }
  return bits;
}

void NetlistTreeTermNode::expand() {
  if (children_ == nullptr) {
    children_ = new Children();
    for (int bit : busBits()) {
      children_->push_back(new NetlistTreeBusTermBitNode(
        this,
        bit
      ));
    }
  }
}

std::string NetlistTreeTermNode::getLabel() const {
  std::string name;
  if (not name_.empty()) {
    name = name_;
  } else {
    name = "<unnamed>";
  }
  if (not isBitTerm()) {
    name += "[" + std::to_string(msb_.value()) + ":" + std::to_string(lsb_.value()) + "]";
  }
  return name;
}

ImU32 NetlistTreeTermNode::getColor() const {
  switch (direction_) {
    case Direction::Input:
      if (isTopTerm()) {
        return IM_COL32(255, 0, 0, 255); // Red
      } else {
        return IM_COL32(0, 255, 0, 255); // Green
      }
    case Direction::Output:
      if (isTopTerm()) {
        return IM_COL32(0, 255, 0, 255); // Green
      } else {
        return IM_COL32(255, 0, 0, 255); // Red
      }
    case Direction::Inout:
      return IM_COL32(255, 255, 0, 255); // Yellow
    default:
      return 0;
  }
}

NetlistTreeBusTermBitNode::NetlistTreeBusTermBitNode(
  NetlistTreeTermNode* parent,
  int bit
): NetlistTreeNode(parent), bit_(bit)
{}

std::string NetlistTreeBusTermBitNode::getLabel() const {
  return std::to_string(bit_);
}

NetlistTreeNetNode::NetlistTreeNetNode(
  NetlistTreeNode* parent,
  const std::string& name,
  std::optional<int> msb,
  std::optional<int> lsb):
  NetlistTreeNode(parent), name_(name), msb_(msb), lsb_(lsb) {
  if (not msb_.has_value() or not lsb_.has_value()) { //scalar
    children_ = new Children();
  }
}

size_t NetlistTreeNetNode::getWidth() const {
  return std::abs(msb_.value_or(0) - lsb_.value_or(0)) + 1;
}

std::vector<int> NetlistTreeNetNode::busBits() const {
  std::vector<int> bits;
  if (msb_.has_value() and lsb_.has_value()) {
    int msb = msb_.value();
    int lsb = lsb_.value();
    auto width = getWidth();
    bits.reserve(width);
    for (size_t i=0; i<width; i++) {
      bits.push_back((msb>lsb)?msb-int(i):msb+int(i));
    }
  }
  return bits;
}

void NetlistTreeNetNode::expand() {
  if (children_ == nullptr) {
    children_ = new Children();
    for (int bit : busBits()) {
      children_->push_back(new NetlistTreeBusNetBitNode(
        this,
        bit
      ));
    }
  }
}

std::string NetlistTreeNetNode::getLabel() const {
  std::string name;
  if (not name_.empty()) {
    name = name_;
  } else {
    name = "<unnamed>";
  }
  if (not isBitNet()) {
    name += "[" + std::to_string(msb_.value()) + ":" + std::to_string(lsb_.value()) + "]";
  }
  return name;
}

NetlistTreeBusNetBitNode::NetlistTreeBusNetBitNode(
  NetlistTreeNetNode* parent,
  int bit
): NetlistTreeNode(parent), bit_(bit)
{}

std::string NetlistTreeBusNetBitNode::getLabel() const {
  return std::to_string(bit_);
}

void NetlistTreeNode::createChildren() {
  children_ = new Children();
}

void NetlistTreeInstanceNode::expand() {
  if (children_ != nullptr) {
    return;
  }
  children_ = new Children();
  if (hasTerms_) {
    children_->push_back(new NetlistTreeGroupNode(this, NetlistTreeGroupNode::Type::Terms));
  }
  if (hasNets_) {
    children_->push_back(new NetlistTreeGroupNode(this, NetlistTreeGroupNode::Type::Nets));
  }
  if (hasPrimitives_) {
    children_->push_back(new NetlistTreeGroupNode(this, NetlistTreeGroupNode::Type::Primitives));
  }
  if (hasInstances_) {
    children_->push_back(new NetlistTreeGroupNode(this, NetlistTreeGroupNode::Type::Instances));
  }
}

void NetlistTreeNode::createTermNode(
  const std::string& name,
  unsigned childID,
  Direction direction,
  std::optional<int> msb,
  std::optional<int> lsb) {
  //this should be a group node of type Terms
  children_->push_back(new NetlistTreeTermNode(this, name, childID, direction, msb, lsb));
}

void NetlistTreeNode::createNetNode(
  const std::string& name,
  std::optional<int> msb,
  std::optional<int> lsb) {
  //this should be a group node of type Nets
  children_->push_back(new NetlistTreeNetNode(this, name, msb, lsb));
}

void NetlistTreeNode::createInstanceNode(
  const std::string& name,
  const std::string& modelName,
  unsigned childID,
  const DesignRef& design_ref,
  bool hasTerms,
  bool hasPrimitives,
  bool hasInstances,
  bool hasNets,
  const std::optional<SourceLoc>& sourceLoc) {
  auto node = new NetlistTreeInstanceNode(
    this, name, modelName,
    childID,
    design_ref,
    hasTerms, hasPrimitives, hasInstances, hasNets, sourceLoc);
  children_->push_back(node);
}

NetlistTreeNode::NetlistTreeNode(NetlistTree* tree): parent_(tree) {
  tree->insertNodeInMap(this);
}

NetlistTreeNode::NetlistTreeNode(NetlistTreeNode* parent): parent_(parent) {
  getTree()->insertNodeInMap(this);
}

NetlistTreeNode* NetlistTreeNode::getParent() const {
  if (isRoot()) {
    return nullptr;
  }
  return static_cast<NetlistTreeNode*>(parent_);
}

NetlistTree* NetlistTreeNode::getTree() const {
  if (isRoot()) {
    return static_cast<NetlistTree*>(parent_);
  }
  return getParent()->getTree();
}

void NetlistTree::sendLoadEquipotential(const NetlistTree::Path& path, const TermID& termID,
                                        bool clearView) const {
  std::string request = R"({"request":"load_equipotential",)";
  request += R"("path":[)";
  for (size_t i = 0; i < path.size(); ++i) {
    request += std::to_string(path[i]);
    if (i < path.size() - 1) {
      request += ",";
    }
  }
  request += R"(],)";
  request += R"("term_id":)" + std::to_string(termID.id);
  if (termID.isBusBit) {
    request += R"(,"bit":)" + std::to_string(termID.busBit);
  }
  request += R"(})";
  Console::Log("Sending load equipotential request: " + request);
  if (clearView && onEquipotentialRequest_) onEquipotentialRequest_();
  ws_->send(request);
}

void NetlistTree::sendTraceDriver(const NetlistTree::Path& path, unsigned termChildID,
                                  const std::vector<int>& bits, const std::string& label,
                                  bool clearView) const {
  // Clear first: clearing the view also forgets its traces.
  if (clearView && onEquipotentialRequest_) onEquipotentialRequest_();
  json req;
  req["request"]  = "trace_driver";
  req["path"]     = path;
  req["term_id"]  = termChildID;
  // One bit: same shape as load_equipotential. Several: a "bits" list.
  if (bits.size() == 1)      req["bit"]  = bits.front();
  else if (bits.size() > 1)  req["bits"] = bits;
  req["trace_id"] = TraceStore::begin(label);
  Console::Log("Sending trace driver request: " + req.dump());
  ws_->send(req.dump());
}
