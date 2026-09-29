#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>
#include "imgui.h"

#include "Types.h"

class NetlistTreeNode;
class INetlistProvider;


class NetlistTree {
  public:
    struct TermID {
      unsigned id;
      bool isBusBit;
      int busBit;
    };
    using Path = std::vector<unsigned>;
    using NodesMap = std::map<unsigned, NetlistTreeNode*>;
    NetlistTree(INetlistProvider* provider): ws_(provider) {}
    NetlistTree(const NetlistTree&) = delete;
    NetlistTree& operator=(const NetlistTree&) = delete;

    void createRootNode(
      const std::string& name,
      const DesignRef& design_ref,
      bool hasTerms,
      bool hasPrimitives,
      bool hasInstances,
      bool hasNets,
      const std::optional<SourceLoc>& sourceLoc = std::nullopt);

    NetlistTreeNode* getNode(unsigned id) const;
    NetlistTreeNode* getRoot() const { return root_; }
    void sendLoadEquipotential(const Path& path, const TermID& termID) const;
    // Requests the full combinational fan-in cone of the term's net, back to
    // the drivers (see "trace_driver" in CLAUDE.md). For a bus, pass every bit.
    // Registers the trace in TraceStore under `label` and tags the request
    // with its trace_id. `clearView` resets the schematic first (like Show
    // Equipotential); otherwise the trace is overlaid on what's shown.
    void sendTraceDriver(const Path& path, unsigned termChildID,
                         const std::vector<int>& bits, const std::string& label,
                         bool clearView = true) const;
    INetlistProvider* getProvider() const { return ws_; }

    // Called before every tree-initiated equipotential request.
    // Use this to reset the schematic and table views.
    void setOnEquipotentialRequest(std::function<void()> cb) {
      onEquipotentialRequest_ = std::move(cb);
    }

    void render();
    void insertNodeInMap(NetlistTreeNode* node);

    // Opens the tree down to the instance at `path` ({} = root) and scrolls to it. Levels load on demand, so
    // this is asynchronous: each level's Instances/Primitives groups are
    // requested as needed and advanceReveal() continues once their replies
    // have created the child nodes. A new reveal replaces a pending one.
    void reveal(const InstancePath& path);
    // One step of a pending reveal; called by render() every frame (public
    // so tests can drive it without an ImGui frame).
    void advanceReveal();
    bool isRevealPending() const { return revealPath_.has_value(); }
    // The instance node at `path`, if its level is already loaded.
    NetlistTreeNode* findInstance(const InstancePath& path) const;
  private:
    friend class NetlistTreeNode;
    INetlistProvider*       ws_                    {nullptr};
    std::function<void()>   onEquipotentialRequest_;
    NetlistTreeNode*        root_       {nullptr};
    unsigned                nextGUIID_ {0};
    NodesMap                nodes_;
    // Pending reveal: the instance names still to walk down from the root.
    std::optional<InstancePath> revealPath_;
    // Nodes render() must open (once each) / scroll to, after a reveal.
    std::set<unsigned>      forceOpen_;
    std::optional<unsigned> scrollTo_;
    // Last SelectionStore revision render() reacted to.
    unsigned                seenSelectionRevision_ {0};
};

class NetlistTreeInstanceNode;

class NetlistTreeNode {
  friend class NetlistTree;
  public:
    using Children = std::vector<NetlistTreeNode*>;

    NetlistTreeNode* getParent() const;
    NetlistTree* getTree() const;
    void createTermNode(
      const std::string& name,
      unsigned childID,
      Direction direction,
      std::optional<int> msb,
      std::optional<int> lsb);
    void createNetNode(
      const std::string& name,
      std::optional<int> msb,
      std::optional<int> lsb);
    void createInstanceNode(
      const std::string& name,
      const std::string& modelName,
      unsigned charID,
      const DesignRef& design_ref,
      bool hasTerms,
      bool hasPrimitives,
      bool hasInstances,
      bool hasNets,
      const std::optional<SourceLoc>& sourceLoc = std::nullopt);
    void createChildren();
    bool hasChildren() const { return children_ != nullptr; }
    virtual NetlistTreeInstanceNode* getInstanceNode() const;
    virtual DesignRef getDesignRef() const;
    virtual void expand() {}
    virtual std::string getLabel() const = 0;
    virtual bool isRoot() const { return false; }
    virtual void sendLoadRequest() const {}
    virtual ImU32 getColor() const { return 0; }
    virtual unsigned getChildID() const { return 0; }
    virtual bool isBusBit() const {
      return false;
    }
    virtual bool isLeaf() const {
      return false;
    }
    virtual bool isBitTerm() const {
      return false;
    }
    // Net-node counterparts of isBitTerm()/isBus() above -- kept as separate
    // virtuals (rather than reusing isBitTerm()/isBus()) so render()'s
    // context-menu dispatch can tell a net leaf/bus from a term one: nets
    // have no direction and offer no "Show Equipotential" action.
    virtual bool isBitNet() const {
      return false;
    }
    virtual bool isBusNet() const {
      return false;
    }
    // True for a node backed by a real design object suitable for "Show
    // Properties" (an instance, including the root) -- false for group
    // nodes ("Terms"/"Primitives"/"Instances"), which aren't objects.
    virtual bool isInstanceNode() const {
      return false;
    }
    // Base term name (no "[bit]" suffix), for term/bus-bit nodes only --
    // used to build a get_properties request. Empty for non-term nodes.
    virtual std::string getTermBaseName() const {
      return std::string();
    }
    // Base net name (no "[bit]" suffix), for net/bus-net-bit nodes only --
    // used to build a get_properties request. Empty for non-net nodes.
    virtual std::string getNetBaseName() const {
      return std::string();
    }
    // True for a whole-bus term node (msb/lsb both set) — offers a
    // "Show Bus Equipotential" action instead of the single-bit one.
    virtual bool isBus() const {
      return false;
    }
    // Bit indices from msb to lsb for a bus node (isBus() == true); empty
    // otherwise. Same order expand() builds NetlistTreeBusTermBitNode
    // children in.
    virtual std::vector<int> busBits() const {
      return {};
    }
    virtual int getBusBit() const {
      return 0;
    }
    virtual void getPath(NetlistTree::Path& path) const;
    // Instance-name path, root excluded ({} at/above root). Matches
    // DiagnosisItem::path and InstanceShape::path.
    virtual InstancePath getInstancePath() const;
    // Diagnosis items attached to this node's path, if any (only
    // NetlistTreeInstanceNode currently reports these).
    virtual std::vector<const DiagnosisItem*> getDiagnostics() const { return {}; }
    // RTL source location, if naja recorded one for this node (only
    // NetlistTreeInstanceNode currently reports these; populated today only
    // for SystemVerilog-loaded designs). Drives the "Show RTL Source"
    // context-menu entry.
    virtual std::optional<SourceLoc> getSourceLoc() const { return std::nullopt; }
  protected:
    NetlistTreeNode(NetlistTree* tree);
    NetlistTreeNode(NetlistTreeNode* parent);
    unsigned    guiID_                {0};
    Children*   children_             {nullptr};
  private:
    void render();

    void*       parent_               {nullptr};
    bool        hasRequestedChildren_ {false};
};

class NetlistTreeInstanceNode : public NetlistTreeNode {
  public:
    NetlistTreeInstanceNode(
      NetlistTree* tree,
      const std::string& name,
      const DesignRef& design_ref,
      bool hasTerms,
      bool hasPrimitives,
      bool hasInstances,
      bool hasNets,
      const std::optional<SourceLoc>& sourceLoc = std::nullopt
    );
    NetlistTreeInstanceNode(
      NetlistTreeNode* parent,
      const std::string& name,
      const std::string& modelName,
      unsigned childID,
      const DesignRef& design_ref,
      bool hasTerms,
      bool hasPrimitives,
      bool hasInstances,
      bool hasNets,
      const std::optional<SourceLoc>& sourceLoc = std::nullopt
    );

    virtual void expand() override;
    virtual bool isRoot() const override { return isRoot_; }
    virtual DesignRef getDesignRef() const override { return designRef_; }
    virtual std::string getLabel() const override;
    virtual void getPath(NetlistTree::Path& path) const override;
    virtual InstancePath getInstancePath() const override;
    virtual unsigned getChildID() const override { return childID_; }
    virtual bool isLeaf() const override {
      return !(hasTerms_ || hasPrimitives_ || hasInstances_ || hasNets_);
    }
    virtual NetlistTreeInstanceNode* getInstanceNode() const override {
      return const_cast<NetlistTreeInstanceNode*>(this);
    }
    virtual ImU32 getColor() const override;
    virtual std::vector<const DiagnosisItem*> getDiagnostics() const override;
    virtual std::optional<SourceLoc> getSourceLoc() const override { return sourceLoc_; }
    virtual bool isInstanceNode() const override { return true; }
    const std::string& getName() const { return name_; }
  private:
    bool        isRoot_         {false};
    std::string name_           {};
    std::string modelName_      {};
    unsigned    childID_        {0};
    DesignRef   designRef_      {};
    bool        hasTerms_       {false};
    bool        hasPrimitives_  {false};
    bool        hasInstances_   {false};
    bool        hasNets_        {false};
    std::optional<SourceLoc> sourceLoc_ {};
};

class NetlistTreeGroupNode : public NetlistTreeNode {
  public:
    enum class Type {
      Terms,
      Nets,
      Primitives,
      Instances
    };
    NetlistTreeGroupNode(NetlistTreeNode* parent, Type type);

    Type getType() const { return type_; }
    virtual std::string getLabel() const override;
    virtual void sendLoadRequest() const override;
    virtual bool isLeaf() const override {
      return false;
    }
  private:
    Type type_;
};

class NetlistTreeTermNode : public NetlistTreeNode {
  public:
    NetlistTreeTermNode(NetlistTreeNode* parent,
                        const std::string& name,
                        unsigned childID,
                        Direction direction,
                        std::optional<int> msb,
                        std::optional<int> lsb);

    virtual std::string getLabel() const override;
    virtual ImU32 getColor() const override;
    virtual void expand() override;
    virtual bool isLeaf() const override {
      return isBitTerm();
    }
    virtual bool isBitTerm() const override {
      return !(msb_.has_value() && lsb_.has_value());
    }
    virtual bool isBus() const override {
      return msb_.has_value() && lsb_.has_value();
    }
    virtual unsigned getChildID() const override { return childID_; }
    virtual std::string getTermBaseName() const override { return name_; }
    bool isTopTerm() const;
    size_t getWidth() const;
    virtual std::vector<int> busBits() const override;
  private:
    std::string         name_;
    unsigned            childID_;
    Direction           direction_;
    std::optional<int>  msb_;
    std::optional<int>  lsb_;
};

class NetlistTreeBusTermBitNode : public NetlistTreeNode {
  public:
    NetlistTreeBusTermBitNode(
      NetlistTreeTermNode* parent,
      int bit
    );
    virtual bool isBitTerm() const override { return true; }
    virtual std::string getLabel() const override;
    virtual bool isLeaf() const override {
      return true;
    }
    virtual bool isBusBit() const override {
      return true;
    }
    virtual int getBusBit() const override{
      return bit_;
    }
    virtual unsigned getChildID() const override {
      return getParent()->getChildID();
    }
    virtual std::string getTermBaseName() const override {
      return getParent()->getTermBaseName();
    }
    bool isTopTerm() const {
      return getInstanceNode()->isRoot();
    }
  private:
    int bit_;
};

class NetlistTreeNetNode : public NetlistTreeNode {
  public:
    NetlistTreeNetNode(NetlistTreeNode* parent,
                       const std::string& name,
                       std::optional<int> msb,
                       std::optional<int> lsb);

    virtual std::string getLabel() const override;
    virtual void expand() override;
    virtual bool isLeaf() const override {
      return isBitNet();
    }
    virtual bool isBitNet() const override {
      return !(msb_.has_value() && lsb_.has_value());
    }
    virtual bool isBusNet() const override {
      return msb_.has_value() && lsb_.has_value();
    }
    virtual std::string getNetBaseName() const override { return name_; }
    size_t getWidth() const;
    virtual std::vector<int> busBits() const override;
  private:
    std::string         name_;
    std::optional<int>  msb_;
    std::optional<int>  lsb_;
};

class NetlistTreeBusNetBitNode : public NetlistTreeNode {
  public:
    NetlistTreeBusNetBitNode(
      NetlistTreeNetNode* parent,
      int bit
    );
    virtual bool isBitNet() const override { return true; }
    virtual std::string getLabel() const override;
    virtual bool isLeaf() const override {
      return true;
    }
    virtual bool isBusBit() const override {
      return true;
    }
    virtual int getBusBit() const override {
      return bit_;
    }
    virtual std::string getNetBaseName() const override {
      return getParent()->getNetBaseName();
    }
  private:
    int bit_;
};