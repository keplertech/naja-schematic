#include "NetlistTree.h"

#include <imgui.h>

#include "Console.h"
#include "DiagnosisStore.h"
#include "INetlistProvider.h"
#include "ReviewStore.h"
#include "ReviewView.h"
#include "SelectionStore.h"
#include "TraceStore.h"

namespace {

// Hover help for the "(replace view)" / "(add to view)" menu entries.
void viewModeTooltip(bool clearView) {
  if (!ImGui::IsItemHovered()) return;
  ImGui::SetTooltip(clearView ? "Clears the schematic (and its traces) first"
                              : "Keeps what the schematic already shows");
}

// "Show Sources" (when the provider flagged the object) and "Show
// Properties" for the object `address` names (kind, path/id_path,
// terminal/net, bit): get_sources and get_properties take the same address.
void inspectMenuItems(INetlistProvider* provider, json address, bool hasSources) {
  if (hasSources && ImGui::MenuItem("Show Sources")) {
    address["request"] = "get_sources";
    provider->send(address.dump());
  }
  if (ImGui::MenuItem("Show Properties")) {
    address["request"] = "get_properties";
    provider->send(address.dump());
  }
}

// What a review thread on this row anchors to, and the threads already on
// it (ReviewStore, matched by ids). A net row whose host sent no net_id has
// neither. `label` names the object in the Add Comment dialog.
struct ReviewTarget {
  std::optional<json> anchor;
  std::vector<const ReviewThread*> threads;
  std::string label;
};

}  // namespace

static ReviewTarget reviewTarget(const NetlistTreeNode& node) {
  ReviewTarget target;
  const InstancePath path = node.getInstancePath();
  const std::optional<int> bit = node.isBusBit() ? std::optional<int>(node.getBusBit())
                                                 : std::nullopt;
  if (node.isInstanceNode()) {
    target.anchor = ReviewStore::instanceAnchor(path);
    target.threads = ReviewStore::instanceThreads(path);
    target.label = displayPath(path);
  } else if (node.isBitTerm() || node.isBus()) {
    target.anchor = ReviewStore::termAnchor(path, node.getChildID(), node.getTermBaseName(), bit);
    target.threads = ReviewStore::termThreads(path, node.getChildID(), bit);
    target.label = node.getLabel();
  } else if ((node.isBitNet() || node.isBusNet()) && node.getNetID()) {
    target.anchor = ReviewStore::netAnchor(path, *node.getNetID(), node.getNetBaseName(), bit);
    target.threads = ReviewStore::netThreads(path, *node.getNetID(), bit);
    target.label = node.getLabel();
  }
  if (target.anchor) {
    std::string where = displayPath(path);
    if (!node.isInstanceNode() && !where.empty()) target.label = where + "/" + target.label;
    if (target.label.empty()) target.label = "the top design";
  }
  return target;
}

void NetlistTree::createRootNode(
  const std::string& name,
  const DesignRef& design_ref,
  bool hasTerms,
  bool hasPrimitives,
  bool hasInstances,
  bool hasNets,
  bool hasSources) {
  root_ = new NetlistTreeInstanceNode(this, name, design_ref, hasTerms, hasPrimitives, hasInstances, hasNets, hasSources);
}

void NetlistTree::render() {
  // A selection made elsewhere (schematic click, host focus_instance) is
  // revealed here; one made by clicking this tree is already visible.
  if (SelectionStore::revision() != seenSelectionRevision_) {
    seenSelectionRevision_ = SelectionStore::revision();
    highlighted_.reset();
    if (SelectionStore::hasSelection() &&
        SelectionStore::origin() != SelectionStore::Origin::Tree)
      reveal(SelectionStore::selected());
  }
  advanceReveal();
  if (root_) {
    // The selected row must stand out more than the row under the mouse.
    // The default theme does the opposite (hovered at 80% opacity, selected
    // at 31%), so after a context-menu click the row under the closed menu
    // looked selected. Here the selection takes the strong accent and hover
    // a faint one, both from the current theme.
    const ImGuiStyle& style = ImGui::GetStyle();
    ImVec4 accent = style.Colors[ImGuiCol_HeaderHovered];
    ImVec4 faint  = accent;
    accent.w = 0.85f;
    faint.w  = 0.22f;
    ImGui::PushStyleColor(ImGuiCol_Header, accent);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, faint);
    root_->render();
    ImGui::PopStyleColor(2);
  } else {
    ImGui::Text("Loading Root Node...");
  }
}

void NetlistTree::reveal(const InstancePath& path, std::optional<RevealLeaf> leaf) {
  revealPath_ = path;
  revealLeaf_ = leaf;
  highlighted_.reset();
  seenSelectionRevision_ = SelectionStore::revision();
}

NetlistTreeNode* NetlistTree::highlightedNode() const {
  return highlighted_ ? getNode(*highlighted_) : nullptr;
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
        revealLeaf_.reset();
      }
      return;
    }
    toOpen.push_back(node);
    toOpen.push_back(via);
    node = next;
  }
  NetlistTreeNode* target = node;
  if (revealLeaf_) {
    // On into the instance's Terms or Nets group, to the row.
    const bool isTerm = revealLeaf_->kind == RevealLeaf::Kind::Term;
    const auto type = isTerm ? NetlistTreeGroupNode::Type::Terms : NetlistTreeGroupNode::Type::Nets;
    node->expand();
    NetlistTreeGroupNode* group = nullptr;
    for (auto* child : *node->children_) {
      auto* g = dynamic_cast<NetlistTreeGroupNode*>(child);
      if (g && g->getType() == type) { group = g; break; }
    }
    if (group && !group->children_) {
      if (!group->hasRequestedChildren_) {
        group->hasRequestedChildren_ = true;
        group->sendLoadRequest();
      }
      return;  // wait for its rows
    }
    NetlistTreeNode* row = nullptr;
    if (group) {
      for (auto* c : *group->children_) {
        const bool match = isTerm ? (c->isBitTerm() || c->isBus()) && c->getChildID() == revealLeaf_->id
                                  : c->getNetID() == revealLeaf_->id;
        if (match) { row = c; break; }
      }
    }
    if (!row) {
      Console::Error(std::string("Cannot reveal ") + (isTerm ? "term" : "net") + " #" +
                     std::to_string(revealLeaf_->id) + " in " +
                     (node->getInstancePath().empty() ? node->getLabel()
                                                      : displayPath(node->getInstancePath())));
    } else {
      toOpen.push_back(node);
      toOpen.push_back(group);
      target = row;
      if (revealLeaf_->bit && (row->isBus() || row->isBusNet())) {
        row->expand();
        for (auto* b : *row->children_) {
          if (b->isBusBit() && b->getBusBit() == *revealLeaf_->bit) {
            toOpen.push_back(row);
            target = b;
            break;
          }
        }
      }
      highlighted_ = target->guiID_;
    }
  }
  for (auto* n : toOpen) forceOpen_.insert(n->guiID_);
  scrollTo_ = target->guiID_;
  revealPath_.reset();
  revealLeaf_.reset();
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
  if ((isInstanceNode() && SelectionStore::isSelected(getInstancePath())) ||
      tree->highlighted_ == guiID_) {
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
  const ReviewTarget review = reviewTarget(*this);
  if (ImGui::IsItemHovered()) {
    auto diagnostics = getDiagnostics();
    if (!diagnostics.empty() || !review.threads.empty()) {
      ImGui::BeginTooltip();
      for (const auto* d : diagnostics) {
        ImGui::TextColored(ImColor(DiagnosisStore::colorForSeverity(d->severity)).Value,
                           "[%s] %s", toString(d->severity), d->message.c_str());
        if (!d->source.empty()) ImGui::TextDisabled("source: %s", d->source.c_str());
      }
      ReviewView::threadsTooltip(review.threads);
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
        std::string label = getTermBaseName();
        if (isBusBit()) label += "[" + std::to_string(getBusBit()) + "]";
        getTree()->sendLoadEquipotential(
          path,
          NetlistTree::TermID{getChildID(), isBusBit(), getBusBit()},
          traceLabel(getInstancePath(), label) + " (net)", clearView);
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
      json req;
      req["kind"]     = "term";
      writePath(req, getInstancePath());
      req["terminal"] = getTermBaseName();
      if (isBusBit()) req["bit"] = getBusBit();
      inspectMenuItems(getTree()->getProvider(), req, hasSources());
      if (review.anchor) ReviewView::commentMenuItem(*review.anchor, review.label);
      ImGui::EndPopup();
    }
  } else if (isBus()) {
    if (ImGui::BeginPopupContextItem()) {
      // No bus-level Show Equipotential or Trace to Driver for now: both
      // work one bit at a time, from the bit rows.
      json req;
      req["kind"]     = "term";
      writePath(req, getInstancePath());
      req["terminal"] = getTermBaseName();
      inspectMenuItems(getTree()->getProvider(), req, hasSources());
      if (review.anchor) ReviewView::commentMenuItem(*review.anchor, review.label);
      ImGui::EndPopup();
    }
  } else if (isBitNet() || isBusNet()) {
    if (ImGui::BeginPopupContextItem()) {
      json req;
      req["kind"] = "net";
      writePath(req, getInstancePath());
      req["net"]  = getNetBaseName();
      if (isBusBit()) req["bit"] = getBusBit();
      inspectMenuItems(getTree()->getProvider(), req, hasSources());
      if (review.anchor) ReviewView::commentMenuItem(*review.anchor, review.label);
      ImGui::EndPopup();
    }
  } else if (isInstanceNode()) {
    if (ImGui::BeginPopupContextItem()) {
      // The top design has no box of its own to draw.
      if (!isRoot()) {
        bool clicked = ImGui::MenuItem("Show in Schematic (replace view)");
        viewModeTooltip(true);
        if (clicked) getTree()->sendShowInstance(getInstancePath());
      }
      json req;
      req["kind"] = "instance";
      writePath(req, getInstancePath());
      inspectMenuItems(getTree()->getProvider(), req, hasSources());
      if (review.anchor) ReviewView::commentMenuItem(*review.anchor, review.label);
      ImGui::EndPopup();
    }
  }
  // Open review threads on this object: their count, in the worst
  // severity's color (the popups and tooltip above are other windows, so
  // this still follows the row's label).
  if (const size_t open = ReviewStore::openCount(review.threads)) {
    ImGui::SameLine();
    ImGui::TextColored(ImColor(ReviewStore::openColor(review.threads)).Value, "[%zu]", open);
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
    bool hasSources):
    NetlistTreeNode(tree),
    isRoot_(true), name_(name),
    childID_(0),designRef_(designRef),
    hasTerms_(hasTerms), hasPrimitives_(hasPrimitives), hasInstances_(hasInstances),
    hasNets_(hasNets),
    hasSources_(hasSources)
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
    bool hasSources):
    NetlistTreeNode(parent),
    isRoot_(false), name_(name), modelName_(modelName),
    childID_(childID), designRef_(designRef),
    hasTerms_(hasTerms), hasPrimitives_(hasPrimitives), hasInstances_(hasInstances),
    hasNets_(hasNets),
    hasSources_(hasSources)
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
  std::optional<int> lsb,
  bool hasSources):
  NetlistTreeNode(parent), name_(name), childID_(childID), direction_(direction), msb_(msb), lsb_(lsb),
  hasSources_(hasSources) {
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
  std::optional<int> lsb,
  bool hasSources,
  std::optional<unsigned> netID):
  NetlistTreeNode(parent), name_(name), msb_(msb), lsb_(lsb), hasSources_(hasSources),
  netID_(netID) {
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
  std::optional<int> lsb,
  bool hasSources) {
  //this should be a group node of type Terms
  children_->push_back(new NetlistTreeTermNode(this, name, childID, direction, msb, lsb, hasSources));
}

void NetlistTreeNode::createNetNode(
  const std::string& name,
  std::optional<int> msb,
  std::optional<int> lsb,
  bool hasSources,
  std::optional<unsigned> netID) {
  //this should be a group node of type Nets
  children_->push_back(new NetlistTreeNetNode(this, name, msb, lsb, hasSources, netID));
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
  bool hasSources) {
  auto node = new NetlistTreeInstanceNode(
    this, name, modelName,
    childID,
    design_ref,
    hasTerms, hasPrimitives, hasInstances, hasNets, hasSources);
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
                                        const std::string& label, bool clearView) const {
  // Clear first: clearing the view also forgets its traces.
  if (clearView && onEquipotentialRequest_) onEquipotentialRequest_();
  json req;
  req["request"] = "load_equipotential";
  req["path"]    = path;
  req["term_id"] = termID.id;
  if (termID.isBusBit) req["bit"] = termID.busBit;
  if (!label.empty()) req["trace_id"] = TraceStore::begin(label);
  Console::Log("Sending load equipotential request: " + req.dump());
  ws_->send(req.dump());
}

void NetlistTree::sendShowInstance(const InstancePath& path) const {
  json req;
  req["request"] = "resolve_instance";
  writePath(req, path);
  ws_->send(req.dump());
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
