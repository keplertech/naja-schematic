#ifndef __EMSCRIPTEN__

#include "LocalSNLProvider.h"
#include "Console.h"
#include "Types.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

// naja core
#include "NLUniverse.h"
#include "NLDB.h"
#include "NLDB0.h"
#include "NLLibrary.h"
// naja SNL
#include "SNLDesign.h"
#include "SNLInstance.h"
#include "SNLInstTerm.h"
#include "SNLTerm.h"
#include "SNLBusTerm.h"
#include "SNLBusTermBit.h"
#include "SNLScalarTerm.h"
#include "SNLNetComponent.h"
#include "SNLBitNet.h"
#include "SNLBusNet.h"
#include "SNLBusNetBit.h"
#include "SNLDesignObject.h"
#include "SNLRTLInfos.h"
#include "SNLPath.h"
#include "SNLOccurrence.h"
#include "SNLEquipotential.h"
#include "SNLDesignModeling.h"
// naja formats
#include "SNLVRLConstructor.h"
#include "SNLSVConstructor.h"
#include "SNLLibertyConstructor.h"
// naja serialization
#include "SNLCapnP.h"

using json = nlohmann::json;
using namespace naja::NL;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int snlDirToInt(const SNLNetComponent::Direction& dir) {
  using E = SNLNetComponent::Direction::DirectionEnum;
  switch (static_cast<E>(dir)) {
    case E::Input:  return 0;
    case E::Output: return 1;
    case E::InOut:  return 2;
    default:        return 0;
  }
}

static std::string designName(const SNLDesign* d) {
  return d->getString();
}

// The instance's own name, "" for an anonymous one -- what protocol.py
// sends too (najaeda's getName()). Not getString(): that makes up a
// "<inst:id>"-style placeholder, which no request could name the instance
// by. The viewer shows anonymous instances by id (displayName() in Types.h)
// and addresses them by id_path.
static std::string instanceName(const SNLInstance* i) {
  return i->isUnnamed() ? std::string() : i->getName().getString();
}

// The instance a request names, root excluded: by "id_path" (instance ids,
// preferred: the only way to name an anonymous instance) or else by "path"
// (instance names). `chain` lists the instances from the top down;
// `design` is the design owning any terminal/net lookup there (the top
// design for an empty path, else the last instance's model). Mirrors
// protocol.py's resolve_request_path().
struct ResolvedInstancePath {
  bool                      ok = false;
  SNLDesign*                design = nullptr;
  std::vector<SNLInstance*> chain;
  SNLInstance* instance() const { return chain.empty() ? nullptr : chain.back(); }
};

static bool isInstanceIdJson(const json& v) {
  return v.is_number_integer() && v.get<long long>() >= 0;
}

static ResolvedInstancePath resolveRequestPath(SNLDesign* top, const json& req) {
  ResolvedInstancePath r;
  r.design = top;
  if (req.contains("id_path") && req["id_path"].is_array()) {
    for (const auto& seg : req["id_path"]) {
      if (!isInstanceIdJson(seg)) return {};
      auto* inst = r.design ? r.design->getInstance(static_cast<NLID::DesignObjectID>(seg.get<long long>()))
                            : nullptr;
      if (!inst) return {};
      r.chain.push_back(inst);
      r.design = inst->getModel();
    }
  } else if (req.contains("path") && req["path"].is_array()) {
    for (const auto& seg : req["path"]) {
      // "" never names an instance: anonymous ones need id_path.
      if (!seg.is_string() || seg.get<std::string>().empty()) return {};
      auto* inst = r.design ? r.design->getInstance(NLName(seg.get<std::string>())) : nullptr;
      if (!inst) return {};
      r.chain.push_back(inst);
      r.design = inst->getModel();
    }
  }
  r.ok = true;
  return r;
}

// "u1/<#3>/u2": the path for a human to read (the properties heading), same
// as displayPath() in Types.h and display_path() in protocol.py.
static std::string displayChain(const std::vector<SNLInstance*>& chain) {
  std::string out;
  for (size_t i = 0; i < chain.size(); ++i) {
    if (i) out += '/';
    auto name = instanceName(chain[i]);
    out += name.empty() ? "<#" + std::to_string(chain[i]->getID()) + ">" : name;
  }
  return out;
}

// Gate/cell function classification, from naja's own modeling of the cell
// (the truth table a Liberty `function` or an NLDB0 gate defines, and its
// sequential model) -- never from its name. A cell naja has no model for
// (e.g. gate-level Verilog loaded without Liberty) is Unknown and draws as
// the generic box. protocol.py's get_primitive_type() mirrors this, same as
// the rest of the wire protocol (see CLAUDE.md).
// The truth-table checks are only asked of a cell with exactly one output
// bit: on a multi-output cell (NLDB0's full adder, a multi-output Liberty
// cell) naja's getTruthTable() throws from isConst0()/isInv()/...
static size_t outputBitCount(const SNLDesign* model) {
  size_t count = 0;
  for (auto* term : model->getBitTerms()) {
    if (term->getDirection() != SNLTerm::Direction::Input) ++count;
  }
  return count;
}

static PrimitiveType getPrimitiveType(const SNLDesign* model) {
  if (!model) return PrimitiveType::Unknown;
  if (NLDB0::isAssign(model)) return PrimitiveType::Assign;
  if (SNLDesignModeling::isSequential(model)) return PrimitiveType::Dff;
  if (outputBitCount(model) != 1) return PrimitiveType::Unknown;
  if (SNLDesignModeling::isConst0(model)) return PrimitiveType::Tie0;
  if (SNLDesignModeling::isConst1(model)) return PrimitiveType::Tie1;
  if (SNLDesignModeling::isInv(model))  return PrimitiveType::Inv;
  if (SNLDesignModeling::isBuf(model))  return PrimitiveType::Buf;
  if (SNLDesignModeling::isAnd(model))  return PrimitiveType::And;
  if (SNLDesignModeling::isNand(model)) return PrimitiveType::Nand;
  if (SNLDesignModeling::isOr(model))   return PrimitiveType::Or;
  if (SNLDesignModeling::isNor(model))  return PrimitiveType::Nor;
  if (SNLDesignModeling::isXor(model))  return PrimitiveType::Xor;
  if (SNLDesignModeling::isXnor(model)) return PrimitiveType::Xnor;
  return PrimitiveType::Unknown;
}

static PrimitiveType getPrimitiveType(const SNLInstance* instance) {
  return instance ? getPrimitiveType(instance->getModel()) : PrimitiveType::Unknown;
}

static bool hasVisiblePrimitiveInstances(const SNLDesign* d) {
  for (auto* inst : d->getPrimitiveInstances()) {
    auto* model = inst->getModel();
    if (!(model && NLDB0::isAssign(model))) {
      return true;
    }
  }
  return false;
}

// True if this design has any sub-instances (primitive or not) worth showing
// in a nested hierarchical schematic box — drives the client's expand glyph.
static bool hasAnySubInstances(const SNLDesign* d) {
  if (!d) return false;
  if (!d->getNonPrimitiveInstances().empty()) return true;
  return hasVisiblePrimitiveInstances(d);
}

// Anonymous scalar constant nets (1'b0/1'b1 tie-offs, e.g. an unconnected
// input naja ties off implicitly) are structural noise, not user-authored
// signals -- filtered out of both has_nets and the Nets tree listing, same
// spirit as hasVisiblePrimitiveInstances() filtering isAssign() primitives.
// Named or bus constants are left alone: a name means someone authored it,
// and a bus is shown as a whole even if every bit happens to be tied.
static bool isAnonymousConstantNet(const SNLNet* n) {
  return n->isUnnamed() and (n->isConstant0() or n->isConstant1()) and
         not dynamic_cast<const SNLBusNet*>(n);
}

static bool hasVisibleNets(const SNLDesign* d) {
  for (auto* net : d->getNets()) {
    if (!isAnonymousConstantNet(net)) return true;
  }
  return false;
}

static std::string termName(const SNLTerm* t) {
  // getName() returns the plain base name with no "[bit]" suffix — the
  // client appends that itself from the separate "bit" field, so using
  // getString() here would double the brackets for bus bit terms.
  auto s = t->getName().getString();
  // Unnamed ports have an empty name — show direction instead, matching
  // getString()'s "<term:id>" placeholder behavior.
  if (s.empty()) {
    switch (static_cast<SNLNetComponent::Direction::DirectionEnum>(t->getDirection())) {
      case SNLNetComponent::Direction::DirectionEnum::Input:  return "(in)";
      case SNLNetComponent::Direction::DirectionEnum::Output: return "(out)";
      case SNLNetComponent::Direction::DirectionEnum::InOut:  return "(inout)";
      default: return s;
    }
  }
  return s;
}

// Build a bit-term JSON node (shared by terms and occurrence entries).
static json bitTermJson(const SNLBitTerm* bt) {
  json t = {
    {"name",      termName(bt)},
    {"child_id",  static_cast<unsigned>(bt->getID())},
    {"direction", snlDirToInt(bt->getDirection())}
  };
  if (auto* btb = dynamic_cast<const SNLBusTermBit*>(bt))
    t["bit"] = static_cast<int>(btb->getBit());
  // Only sent when set: marks a flip-flop's clock pin (see drawDffInstance).
  if (SNLDesignModeling::isClock(bt)) t["clock"] = true;
  return t;
}

// RTL source location for an elaborated object, if naja has one recorded
// (SNLRTLInfos -- populated today only by the SystemVerilog/slang frontend).
// Returns JSON null when unavailable, which the client treats as "no link".
// Every pin of a design, bus terms expanded bit by bit ("data[3]", with
// "bit" set) so each gets its own schematic pin.
static json allBitTermsJson(const SNLDesign* design) {
  json terms = json::array();
  for (auto* term : design->getTerms()) {
    if (auto* bus = dynamic_cast<SNLBusTerm*>(term)) {
      int lo = std::min(static_cast<int>(bus->getLSB()),
                        static_cast<int>(bus->getMSB()));
      int hi = std::max(static_cast<int>(bus->getLSB()),
                        static_cast<int>(bus->getMSB()));
      for (int b = lo; b <= hi; ++b) {
        if (auto* bit = bus->getBit(b)) {
          json t = bitTermJson(bit);
          // Append the bit index to the name for display ("data[3]"),
          // but keep the "bit" field so the client can build requests.
          if (t.contains("bit")) {
            t["name"] = t["name"].get<std::string>() +
                        "[" + std::to_string(t["bit"].get<int>()) + "]";
          }
          terms.push_back(std::move(t));
        }
      }
    } else if (auto* st = dynamic_cast<SNLBitTerm*>(term)) {
      terms.push_back(bitTermJson(st));
    }
  }
  return terms;
}

static json sourceLocJson(const SNLDesignObject* obj) {
  if (!obj || !obj->hasRTLInfos()) return nullptr;
  auto* rtl = obj->getRTLInfos();
  if (!rtl->hasSourceLoc()) return nullptr;
  const auto& loc = *rtl->getSourceLoc();
  return json{
    {"file",       loc.file.getString()},
    {"line",       loc.line},
    {"end_line",   loc.endLine},
    {"column",     loc.column},
    {"end_column", loc.endColumn}
  };
}

// Find a design matching {dbId, libId, designId}.
static SNLDesign* findDesign(unsigned dbId, unsigned libId, unsigned designId) {
  return NLUniverse::get()->getSNLDesign(NLID::DesignReference(
      static_cast<NLID::DBID>(dbId), static_cast<NLID::LibraryID>(libId),
      static_cast<NLID::DesignID>(designId)));
}

static void dumpDB() {
  std::function<void(NLLibrary*, int)> dumpLib = [&](NLLibrary* lib, int indent) {
    std::string pad(indent * 2, ' ');
    Console::Log(pad + "lib id=" + std::to_string(lib->getID()) +
                 " name=" + lib->getString());
    for (auto* d : lib->getSNLDesigns())
      Console::Log(pad + "  design id=" + std::to_string(d->getID()) +
                   " name=" + d->getString() +
                   " terms=" + std::to_string(d->getTerms().size()));
    for (auto* sub : lib->getLibraries()) dumpLib(sub, indent + 1);
  };
  for (auto* db : NLUniverse::get()->getDBs()) {
    Console::Log("DB id=" + std::to_string(db->getID()));
    for (auto* lib : db->getLibraries()) dumpLib(lib, 1);
  }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

LocalSNLProvider::LocalSNLProvider() {
  if (!NLUniverse::get()) NLUniverse::create();
}

LocalSNLProvider::~LocalSNLProvider() {
  // SNL does not expose a public API to remove individual DBs;
  // the universe and its DBs are released when the process exits.
}

void LocalSNLProvider::on_open(std::function<void()> cb)                    { openCb_  = std::move(cb); }
void LocalSNLProvider::on_message(std::function<void(const std::string&)> cb){ msgCb_   = std::move(cb); }
void LocalSNLProvider::on_close(std::function<void()> cb)                   { closeCb_ = std::move(cb); }
void LocalSNLProvider::on_error(std::function<void(const std::string&)> cb)  { errCb_   = std::move(cb); }

void LocalSNLProvider::start() {
  Console::Log("LocalSNLProvider: starting in standalone mode");
  if (openCb_) openCb_();
}

void LocalSNLProvider::send(const std::string& msg) { handleRequest(msg); }

// ---------------------------------------------------------------------------
// DB management
// ---------------------------------------------------------------------------

NLDB* LocalSNLProvider::freshDB() {
  db_ = nullptr; // old DB remains in universe; SNL has no public single-DB removal API
  return NLDB::create(NLUniverse::get());
}

void LocalSNLProvider::findAndSetTop() {
  // Collect designs referenced by non-primitive instances
  std::set<SNLDesign*> instantiated;
  for (auto* lib : db_->getLibraries()) {
    for (auto* design : lib->getSNLDesigns()) {
      if (design->isPrimitive()) continue;
      for (auto* inst : design->getNonPrimitiveInstances())
        instantiated.insert(inst->getModel());
    }
  }

  // First non-primitive design not instantiated by anything else is the top
  for (auto* lib : db_->getLibraries()) {
    if (lib->isPrimitives()) continue;
    for (auto* design : lib->getSNLDesigns()) {
      if (design->isPrimitive()) continue;
      if (!instantiated.count(design)) {
        db_->setTopDesign(design);
        Console::Log("Top design: " + designName(design));
        return;
      }
    }
  }
  Console::Error("LocalSNLProvider: could not determine top design");
}

bool LocalSNLProvider::setTopByName(const std::string& name) {
  for (auto* lib : db_->getLibraries()) {
    if (lib->isPrimitives()) continue;
    for (auto* design : lib->getSNLDesigns()) {
      if (design->isPrimitive()) continue;
      if (designName(design) == name) {
        db_->setTopDesign(design);
        Console::Log("Top design (explicit): " + name);
        return true;
      }
    }
  }
  Console::Error("LocalSNLProvider: requested top module not found: " + name);
  return false;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

void LocalSNLProvider::loadSNL(const std::string& path) {
  Console::Log("LocalSNLProvider: loading SNL from " + path);
  try {
    db_ = nullptr; // old DB remains in universe (no public removal API)
    db_ = SNLCapnP::load(std::filesystem::path(path));
    if (!db_->getTopDesign())
      findAndSetTop();
    Console::Log("LocalSNLProvider: SNL loaded");
  } catch (const std::exception& e) {
    Console::Error("loadSNL failed: " + std::string(e.what()));
    db_ = nullptr;
  }
}

void LocalSNLProvider::loadVerilog(
    const std::vector<std::string>& verilogFiles,
    const std::vector<std::string>& libertyFiles)
{
  Console::Log("LocalSNLProvider: loading Verilog (" +
               std::to_string(verilogFiles.size()) + " files, " +
               std::to_string(libertyFiles.size()) + " liberty)");
  try {
    db_ = freshDB();

    if (!libertyFiles.empty()) {
      auto* primLib = NLLibrary::create(db_, NLLibrary::Type::Primitives, NLName("primitives"));
      SNLLibertyConstructor libCtor(primLib);
      std::vector<std::filesystem::path> paths(libertyFiles.begin(), libertyFiles.end());
      libCtor.construct(paths);
    }

    auto* userLib = NLLibrary::create(db_, NLName("work"));
    SNLVRLConstructor::Config cfg;
    cfg.blackboxUnknownModules_ = !libertyFiles.empty();
    SNLVRLConstructor vrlCtor(userLib);
    vrlCtor.config_ = cfg;
    std::vector<std::filesystem::path> paths(verilogFiles.begin(), verilogFiles.end());
    vrlCtor.construct(paths);

    findAndSetTop();
    Console::Log("LocalSNLProvider: Verilog loaded");
  } catch (const std::exception& e) {
    Console::Error("loadVerilog failed: " + std::string(e.what()));
    db_ = nullptr;
  }
}

// Heuristic: does this path look like a manifest/command file (as opposed to
// an .sv/.v source) rather than a real SystemVerilog source file? Beyond the
// standard "-f"/"-flist" extensions, EDA flows commonly name these files
// after the "-F" convention with no fixed extension (e.g. "Flist.cva6",
// "sources.flist", "filelist.txt"), so we also match on the "flist" substring
// appearing anywhere in the filename.
static bool looksLikeFlist(const std::filesystem::path& p) {
  auto ext = p.extension().string();
  if (ext == ".f" || ext == ".flist") return true;
  std::string name = p.filename().string();
  std::transform(name.begin(), name.end(), name.begin(),
                  [](unsigned char c) { return std::tolower(c); });
  return name.find("flist") != std::string::npos;
}

void LocalSNLProvider::loadSystemVerilog(const std::vector<std::string>& sources,
                                         const std::string& topModule) {
  Console::Log("LocalSNLProvider: loading SystemVerilog (" +
               std::to_string(sources.size()) + " source entries)");
  try {
    db_ = freshDB();

    // Pass .sv/.v files through directly; hand manifest/command files
    // ("-f"/"-flist"/"Flist.*"-style paths) to slang's own "-f" command-file
    // reader, which natively supports nested "-F" includes, "+incdir+",
    // comments, and "${VAR}" environment variable expansion -- all of which
    // real-world manifests (e.g. CVA6's Flist.cva6) rely on.
    std::vector<std::filesystem::path> paths;
    // "--top" is forwarded to slang's own driver just like "-f" above (both
    // are raw argv tokens to slang, not filesystem paths); this forces the
    // requested module as elaboration root instead of leaving slang -- and
    // findAndSetTop()'s post-hoc heuristic -- to guess it from whatever ends
    // up uninstantiated, which picks the wrong design on multi-root flists.
    if (!topModule.empty()) {
      Console::Log("Forcing SystemVerilog top module: " + topModule);
      paths.emplace_back("--top");
      paths.emplace_back(topModule);
    }
    for (const auto& s : sources) {
      std::filesystem::path p(s);
      if (looksLikeFlist(p)) {
        Console::Log("Loading as flist (-f): " + p.string());
        paths.emplace_back("-f");
      }
      paths.push_back(p);
    }

    auto* userLib = NLLibrary::create(db_, NLName("work"));
    SNLSVConstructor svCtor(userLib);
    svCtor.construct(paths);

    if (topModule.empty() || !setTopByName(topModule)) {
      findAndSetTop();
    }
    Console::Log("LocalSNLProvider: SystemVerilog loaded (" +
                 std::to_string(paths.size()) + " files)");
  } catch (const std::exception& e) {
    Console::Error("loadSystemVerilog failed: " + std::string(e.what()));
    db_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Request dispatch
// ---------------------------------------------------------------------------

void LocalSNLProvider::handleRequest(const std::string& jsonRequest) {
  if (!msgCb_) return;

  json req;
  try { req = json::parse(jsonRequest); }
  catch (...) {
    Console::Error("LocalSNLProvider: bad JSON: " + jsonRequest);
    return;
  }

  std::string request = req.value("request", "");

  if (request == "load_root") {
    msgCb_(buildRootResponse());
  } else if (request == "load_instances" || request == "load_primitives") {
    unsigned guiId = req.value("gui_id", 0u);
    unsigned dbId = 0, libId = 0, designId = 0;
    if (req.contains("design_ref")) {
      dbId     = req["design_ref"].value("db_id",      0u);
      libId    = req["design_ref"].value("library_id", 0u);
      designId = req["design_ref"].value("design_id",  0u);
    }
    msgCb_(buildInstancesResponse(guiId, dbId, libId, designId,
                                  request == "load_primitives"));
  } else if (request == "load_terms") {
    unsigned guiId = req.value("gui_id", 0u);
    unsigned dbId = 0, libId = 0, designId = 0;
    if (req.contains("design_ref")) {
      dbId     = req["design_ref"].value("db_id",      0u);
      libId    = req["design_ref"].value("library_id", 0u);
      designId = req["design_ref"].value("design_id",  0u);
    }
    msgCb_(buildTermsResponse(guiId, dbId, libId, designId));
  } else if (request == "load_nets") {
    unsigned guiId = req.value("gui_id", 0u);
    unsigned dbId = 0, libId = 0, designId = 0;
    if (req.contains("design_ref")) {
      dbId     = req["design_ref"].value("db_id",      0u);
      libId    = req["design_ref"].value("library_id", 0u);
      designId = req["design_ref"].value("design_id",  0u);
    }
    msgCb_(buildNetsResponse(guiId, dbId, libId, designId));
  } else if (request == "load_equipotential") {
    msgCb_(buildEquipotentialResponse(req));
  } else if (request == "trace_driver") {
    msgCb_(buildTraceDriverResponse(req));
  } else if (request == "expand_instance_terms") {
    msgCb_(buildExpandInstanceTermsResponse(req));
  } else if (request == "load_instance_internals") {
    msgCb_(buildInstanceInternalsResponse(req));
  } else if (request == "load_source") {
    msgCb_(buildSourceResponse(req));
  } else if (request == "get_properties") {
    msgCb_(buildPropertiesResponse(req));
  } else if (request == "resolve_instance") {
    msgCb_(buildResolveInstanceResponse(req));
  } else if (request == "instance_selected") {
    // A notification (the user's selection), for hosts that script the
    // viewer -- nothing to answer in the standalone app.
  } else {
    Console::Error("LocalSNLProvider: unknown request: " + request);
  }
}

// ---------------------------------------------------------------------------
// Response builders
// ---------------------------------------------------------------------------

std::string LocalSNLProvider::buildRootResponse() const {
  json root;
  auto* top = db_ ? db_->getTopDesign() : nullptr;
  if (!top) {
    root = {
      {"name", db_ ? "(failed to find top design)" : "(no netlist — use File > Open)"},
      {"design_ref", {{"db_id", 0}, {"library_id", 0}, {"design_id", 0}}},
      {"has_terms", false}, {"has_primitives", false}, {"has_instances", false},
      {"has_nets", false}
    };
  } else {
    root = {
      {"name", designName(top)},
      {"design_ref", {
        {"db_id",      static_cast<unsigned>(db_->getID())},
        {"library_id", static_cast<unsigned>(top->getLibrary()->getID())},
        {"design_id",  static_cast<unsigned>(top->getID())}
      }},
      {"has_terms",      !top->getTerms().empty()},
      {"has_primitives", hasVisiblePrimitiveInstances(top)},
      {"has_instances",  !top->getNonPrimitiveInstances().empty()},
      {"has_nets",       hasVisibleNets(top)}
    };
  }
  return json{{"response", "root_response"}, {"root", root}}.dump();
}

std::string LocalSNLProvider::buildInstancesResponse(
    unsigned guiId, unsigned dbId, unsigned libId, unsigned designId, bool primitives) const
{
  json children = json::array();

  auto* design = findDesign(dbId, libId, designId);
  if (!design) {
    Console::Error("buildInstancesResponse: design not found db=" + std::to_string(dbId) +
                   " lib=" + std::to_string(libId) + " design=" + std::to_string(designId));
    dumpDB();
  }

  if (design) {
    auto instances = primitives
      ? design->getPrimitiveInstances()
      : design->getNonPrimitiveInstances();

    for (auto* inst : instances) {
      auto* model = inst->getModel();
      if (model && NLDB0::isAssign(model)) {
        continue;
      }
      children.push_back({
        {"name",           instanceName(inst)},
        {"model_name",     model ? designName(model) : ""},
        {"primitive_type", toString(getPrimitiveType(model))},
        {"child_id",       static_cast<unsigned>(inst->getID())},
        {"design_ref", {
          {"db_id",      model ? static_cast<unsigned>(model->getDB()->getID()) : 0u},
          {"library_id", model ? static_cast<unsigned>(model->getLibrary()->getID()) : 0u},
          {"design_id",  model ? static_cast<unsigned>(model->getID()) : 0u}
        }},
        {"has_terms",      model && !model->getTerms().empty()},
        {"has_primitives", model && hasVisiblePrimitiveInstances(model)},
        {"has_instances",  model && !model->getNonPrimitiveInstances().empty()},
        {"has_nets",       model && hasVisibleNets(model)},
        {"source_loc",     sourceLocJson(inst)}
      });
    }
  }

  return json{
    {"response", primitives ? "primitives_response" : "instances_response"},
    {"gui_id",   guiId},
    {"children", children}
  }.dump();
}

std::string LocalSNLProvider::buildTermsResponse(
    unsigned guiId, unsigned dbId, unsigned libId, unsigned designId) const
{
  json children = json::array();

  auto* design = findDesign(dbId, libId, designId);
  if (!design) {
    Console::Error("buildTermsResponse: design not found db=" + std::to_string(dbId) +
                   " lib=" + std::to_string(libId) + " design=" + std::to_string(designId));
    dumpDB();
  }

  if (design) {
    for (auto* term : design->getTerms()) {
      if (auto* bt = dynamic_cast<SNLBusTerm*>(term)) {
        // Bus term: emit as a single entry with msb/lsb. Use the plain
        // base name (no "[msb:lsb]" suffix) -- the client appends the
        // range itself from msb/lsb, so getString() here would double it.
        json t = {
          {"name",      bt->getName().getString()},
          {"child_id",  static_cast<unsigned>(bt->getID())},
          {"direction", snlDirToInt(bt->getDirection())},
          {"msb",       static_cast<int>(bt->getMSB())},
          {"lsb",       static_cast<int>(bt->getLSB())}
        };
        children.push_back(std::move(t));
      } else if (auto* st = dynamic_cast<SNLBitTerm*>(term)) {
        children.push_back(bitTermJson(st));
      }
    }
  }

  return json{
    {"response", "terms_response"},
    {"gui_id",   guiId},
    {"children", children}
  }.dump();
}

std::string LocalSNLProvider::buildNetsResponse(
    unsigned guiId, unsigned dbId, unsigned libId, unsigned designId) const
{
  json children = json::array();

  auto* design = findDesign(dbId, libId, designId);
  if (!design) {
    Console::Error("buildNetsResponse: design not found db=" + std::to_string(dbId) +
                   " lib=" + std::to_string(libId) + " design=" + std::to_string(designId));
    dumpDB();
  }

  if (design) {
    for (auto* net : design->getNets()) {
      if (isAnonymousConstantNet(net)) continue;
      if (auto* bn = dynamic_cast<SNLBusNet*>(net)) {
        // Bus net: emit as a single entry with msb/lsb, same convention as
        // buildTermsResponse's bus terms -- the client appends "[msb:lsb]"
        // itself and expands individual bits lazily.
        json n = {
          {"name", bn->getName().getString()},
          {"msb",  static_cast<int>(bn->getMSB())},
          {"lsb",  static_cast<int>(bn->getLSB())}
        };
        children.push_back(std::move(n));
      } else if (auto* bit = dynamic_cast<SNLBitNet*>(net)) {
        children.push_back({{"name", bit->getName().getString()}});
      }
    }
  }

  return json{
    {"response", "nets_response"},
    {"gui_id",   guiId},
    {"children", children}
  }.dump();
}

// Wire-format body of an equipotential (no "response" key): its top-level
// terms plus every leaf inst-term occurrence on the net.
// With `sinks` set, only the net's drivers and those listed receivers (top
// terms as SNLOccurrence(term), inst terms as (path, instTerm)) are emitted
// -- a driver trace shows the path it followed, not every reader on the net.
static json equipotentialJson(const SNLEquipotential& equi,
                              const std::set<SNLOccurrence>* sinks = nullptr) {
  json terms = json::array();
  for (auto* bt : equi.getTermsSet()) {
    // A top-level output is a receiver of the net; an input/inout drives it.
    if (sinks && bt->getDirection() == SNLTerm::Direction::Output &&
        !sinks->count(SNLOccurrence(bt))) continue;
    terms.push_back(bitTermJson(bt));
  }

  json occs = json::array();
  for (const auto& occ : equi.getInstTermOccurrencesSet()) {
    auto* it = occ.getInstTerm();
    if (sinks && it->getDirection() == SNLTerm::Direction::Input &&
        !sinks->count(occ)) continue;
    auto* bt = it->getBitTerm();
    // Each path entry is [name, child_id, model_name]: the model name lets
    // the schematic label the hierarchical module boxes it draws around a
    // driver trace (see EquipotentialView's hierarchy grouping).
    json pathArr = json::array();
    for (auto* inst : occ.getPath().getInstances())
      pathArr.push_back(json::array({instanceName(inst),
                                     static_cast<unsigned>(inst->getID()),
                                     designName(inst->getModel())}));
    // The occurrence path is to the parent design; append the instance itself
    auto* theInst = it->getInstance();
    pathArr.push_back(json::array({instanceName(theInst),
                                   static_cast<unsigned>(theInst->getID()),
                                   designName(theInst->getModel())}));
    auto entry = bitTermJson(bt);
    entry["path"] = std::move(pathArr);
    if (auto* model = theInst->getModel()) {
      entry["design_ref"] = {
        {"db_id",      static_cast<unsigned>(model->getDB()->getID())},
        {"library_id", static_cast<unsigned>(model->getLibrary()->getID())},
        {"design_id",  static_cast<unsigned>(model->getID())}
      };
    }
    entry["primitive_type"] = toString(getPrimitiveType(theInst));
    entry["has_instances"] = hasAnySubInstances(theInst->getModel());
    // Lets the view tell whether every pin of this instance is already on
    // screen (solid box) or only a subset (dashed, double-click to expand).
    if (auto* model = theInst->getModel())
      entry["bit_term_count"] = model->getBitTerms().size();
    entry["source_loc"]    = sourceLocJson(theInst);
    occs.push_back(std::move(entry));
  }
  return json{{"terms", std::move(terms)}, {"occurrences", std::move(occs)}};
}

// Resolves a load_equipotential-style request (path of instance child IDs,
// term_id, optional bit) to the net-component occurrence an equipotential is
// computed from. Returns an invalid occurrence when it can't be resolved.
// `bitOverride`, when set, takes precedence over req["bit"] (lets a bus
// request fan out over several bits of the same term).
static SNLOccurrence resolveStartOccurrence(SNLDesign* topDesign,
                                            const json& req,
                                            std::optional<int> bitOverride) {
  SNLPath::PathIDDescriptor pathIDs;
  if (req.contains("path") && req["path"].is_array())
    for (auto& pid : req["path"])
      pathIDs.push_back(static_cast<NLID::DesignObjectID>(pid.get<unsigned>()));

  unsigned termId = req.value("term_id", 0u);
  std::optional<int> bit = bitOverride;
  if (!bit && req.contains("bit")) bit = req["bit"].get<int>();

  auto resolveBitTerm = [&](SNLDesign* design) -> SNLBitTerm* {
    auto* term = design->getTerm(static_cast<NLID::DesignObjectID>(termId));
    if (!term) return nullptr;
    if (!bit) return dynamic_cast<SNLBitTerm*>(term);
    auto* bus = dynamic_cast<SNLBusTerm*>(term);
    return bus ? bus->getBit(static_cast<NLID::Bit>(*bit)) : nullptr;
  };

  if (pathIDs.empty()) {
    // Top-level term
    auto* bitTerm = resolveBitTerm(topDesign);
    return bitTerm ? SNLOccurrence(bitTerm) : SNLOccurrence();
  }
  // Instance term — occurrence of the tail instance's inst term
  SNLPath snlPath(topDesign, pathIDs);
  auto* model = snlPath.getModel();
  if (!model) return SNLOccurrence();
  auto* bitTerm = resolveBitTerm(model);
  if (!bitTerm) return SNLOccurrence();
  auto* instTerm = snlPath.getTailInstance()->getInstTerm(bitTerm);
  return instTerm ? SNLOccurrence(snlPath.getHeadPath(), instTerm) : SNLOccurrence();
}

bool LocalSNLProvider::hasDesign() const { return db_ && db_->getTopDesign(); }

std::optional<json> LocalSNLProvider::termRequest(const json& spec, std::string& error) const {
  auto* topDesign = db_ ? db_->getTopDesign() : nullptr;
  if (!topDesign) { error = "no design loaded"; return std::nullopt; }
  const std::string terminal = spec.value("terminal", "");
  if (terminal.empty()) { error = "no \"terminal\" given"; return std::nullopt; }
  auto resolved = resolveRequestPath(topDesign, spec);
  if (!resolved.ok || !resolved.design) { error = "no such instance"; return std::nullopt; }
  auto* term = resolved.design->getTerm(NLName(terminal));
  if (!term) { error = "no terminal '" + terminal + "'"; return std::nullopt; }
  json req;
  req["path"] = json::array();
  for (auto* inst : resolved.chain) req["path"].push_back(inst->getID());
  req["term_id"] = term->getID();
  if (spec.contains("bit")) {
    auto* bus = dynamic_cast<SNLBusTerm*>(term);
    if (!bus || !spec["bit"].is_number_integer() ||
        !bus->getBit(static_cast<NLID::Bit>(spec["bit"].get<int>()))) {
      error = "no bit " + spec["bit"].dump() + " on '" + terminal + "'";
      return std::nullopt;
    }
    req["bit"] = spec["bit"];
  } else if (dynamic_cast<SNLBusTerm*>(term)) {
    error = "'" + terminal + "' is a bus: give a \"bit\"";
    return std::nullopt;
  }
  return req;
}

std::string LocalSNLProvider::buildEquipotentialResponse(const json& req) const {
  // The viewer's trace_id (a tree "Show Equipotential") is echoed back, so
  // it knows which trace the net is for (see TraceStore).
  auto reply = [&req](json response) {
    response["response"] = "equipotential_response";
    if (req.contains("trace_id")) response["trace_id"] = req["trace_id"];
    return response.dump();
  };
  const json empty = {{"terms", json::array()}, {"occurrences", json::array()}};

  auto* topDesign = db_ ? db_->getTopDesign() : nullptr;
  if (!topDesign) return reply(empty);

  try {
    auto start = resolveStartOccurrence(topDesign, req, std::nullopt);
    if (!start.isValid()) return reply(empty);
    SNLEquipotential equi(start, SNLEquipotential::Mode::TraverseAssigns);
    return reply(equipotentialJson(equi));
  } catch (const std::exception& e) {
    Console::Error("buildEquipotentialResponse: " + std::string(e.what()));
    return reply(empty);
  }
}

// Upper bound on nets returned by one trace_driver request: every net is a
// schematic wire plus its instance boxes, so an unbounded cone through a big
// design would bury the view (and the frame time) rather than help.
static constexpr size_t kMaxTraceNets = 500;

std::string LocalSNLProvider::buildTraceDriverResponse(const json& req) const {
  // The viewer's trace_id is echoed back, so it knows which trace the nets
  // are for (see TraceStore).
  auto reply = [&req](json equis, bool truncated) {
    json r = {{"response", "trace_driver_response"},
              {"equipotentials", std::move(equis)},
              {"truncated", truncated}};
    if (req.contains("trace_id")) r["trace_id"] = req["trace_id"];
    return r.dump();
  };
  const std::string empty = reply(json::array(), false);

  auto* topDesign = db_ ? db_->getTopDesign() : nullptr;
  if (!topDesign) return empty;

  try {
    // One start per requested bus bit, or the single term/bit otherwise.
    std::vector<SNLOccurrence> starts;
    if (req.contains("bits") && req["bits"].is_array()) {
      for (auto& b : req["bits"])
        starts.push_back(resolveStartOccurrence(topDesign, req, b.get<int>()));
    } else {
      starts.push_back(resolveStartOccurrence(topDesign, req, std::nullopt));
    }

    // Breadth-first from the start nets toward the drivers, so each net in the
    // result shares an instance with an earlier one (the layout relies on
    // that to chain nets left-to-right).
    // `sinks` = the receiver pins the trace entered this net through (the
    // start pin, or the input pin of the cell being crossed); a net reached
    // through several of them accumulates all of them.
    struct ConeNet {
      SNLEquipotential          equi;
      std::set<SNLOccurrence>   sinks;
    };
    std::vector<ConeNet> cone;
    std::map<SNLEquipotential, size_t> indexOf;
    auto enqueue = [&](const SNLOccurrence& sink) {
      SNLEquipotential equi(sink, SNLEquipotential::Mode::TraverseAssigns);
      auto [it, inserted] = indexOf.emplace(equi, cone.size());
      if (inserted) cone.push_back({std::move(equi), {sink}});
      else          cone[it->second].sinks.insert(sink);
    };
    for (const auto& start : starts)
      if (start.isValid()) enqueue(start);

    bool truncated = false;
    for (size_t i = 0; i < cone.size(); ++i) {
      // Copy: enqueue() below may reallocate `cone`.
      const auto instTermOccs = cone[i].equi.getInstTermOccurrencesSet();
      for (const auto& occ : instTermOccs) {
        auto* driverTerm = occ.getInstTerm();
        if (driverTerm->getDirection() != SNLTerm::Direction::Output) continue;
        // The cone ends at sequential cells and at cells with no timing
        // model (blackboxes): no combinational arc to cross.
        auto* model = driverTerm->getInstance()->getModel();
        if (SNLDesignModeling::isSequential(model) ||
            !SNLDesignModeling::hasModeling(model)) continue;
        for (auto* input : SNLDesignModeling::getCombinatorialInputs(driverTerm)) {
          SNLOccurrence sink(occ.getPath(), input);
          // Already-known nets don't grow the cone, so only cap new ones.
          if (cone.size() >= kMaxTraceNets &&
              !indexOf.count(SNLEquipotential(sink, SNLEquipotential::Mode::TraverseAssigns))) {
            truncated = true; break;
          }
          enqueue(sink);
        }
        if (truncated) break;
      }
      if (truncated) break;
    }
    if (truncated)
      Console::Log("trace_driver: cone truncated at " +
                    std::to_string(kMaxTraceNets) + " nets");

    json equis = json::array();
    for (const auto& net : cone) equis.push_back(equipotentialJson(net.equi, &net.sinks));
    return reply(std::move(equis), truncated);
  } catch (const std::exception& e) {
    Console::Error("buildTraceDriverResponse: " + std::string(e.what()));
    return empty;
  }
}

std::string LocalSNLProvider::buildExpandInstanceTermsResponse(const json& req) const {
  // The requesting box's instance path (names and ids), echoed back as is.
  json instancePath   = req.value("instance_path", json::array());
  json instanceIdPath = req.value("instance_id_path", json::array());
  unsigned dbId = 0, libId = 0, designId = 0;
  if (req.contains("design_ref")) {
    dbId     = req["design_ref"].value("db_id",      0u);
    libId    = req["design_ref"].value("library_id", 0u);
    designId = req["design_ref"].value("design_id",  0u);
  }

  auto* design = findDesign(dbId, libId, designId);
  return json{
    {"response", "expanded_instance_terms"},
    {"instance_path", instancePath},
    {"instance_id_path", instanceIdPath},
    {"terms",    design ? allBitTermsJson(design) : json::array()}
  }.dump();
}

// Everything the viewer needs to show one instance given only its path --
// "id_path" (preferred) or "path" (names), see resolveRequestPath(): each
// path level's [name, child_id, model_name], and the instance's model and
// full pin list, to reveal it in the tree and draw it alone in the
// schematic. The reply's "path"/"id_path" are the resolved instance's
// (names and ids) when found, the request's as given otherwise. Mirrors
// protocol.py's _handle_resolve_instance.
std::string LocalSNLProvider::buildResolveInstanceResponse(const json& req) const {
  json reply = {{"response", "instance_resolved"},
                {"path", req.contains("path") && req["path"].is_array() ? req["path"] : json::array()},
                {"found", false}};
  if (req.contains("id_path") && req["id_path"].is_array()) reply["id_path"] = req["id_path"];
  auto* topDesign = db_ ? db_->getTopDesign() : nullptr;
  if (!topDesign) return reply.dump();

  auto resolved = resolveRequestPath(topDesign, req);
  if (!resolved.ok) {
    Console::Error("resolve_instance: could not resolve instance path");
    return reply.dump();
  }
  json names = json::array(), ids = json::array(), levels = json::array();
  for (auto* inst : resolved.chain) {
    names.push_back(instanceName(inst));
    ids.push_back(static_cast<unsigned>(inst->getID()));
    levels.push_back(json::array({instanceName(inst),
                                  static_cast<unsigned>(inst->getID()),
                                  designName(inst->getModel())}));
  }
  reply["path"]    = std::move(names);
  reply["id_path"] = std::move(ids);
  reply["found"]   = true;
  auto* instance = resolved.instance();
  if (!instance) return reply.dump();  // the top design itself

  SNLDesign* design = resolved.design;
  reply["instance"] = {
    {"path",       std::move(levels)},
    {"design_ref", {
      {"db_id",      static_cast<unsigned>(design->getDB()->getID())},
      {"library_id", static_cast<unsigned>(design->getLibrary()->getID())},
      {"design_id",  static_cast<unsigned>(design->getID())}
    }},
    {"primitive_type", toString(getPrimitiveType(design))},
    {"has_instances", hasAnySubInstances(design)},
    {"source_loc",    sourceLocJson(instance)},
    {"terms",         allBitTermsJson(design)}
  };
  return reply.dump();
}

// Resolve an instance's own model (the request's design_ref, exactly as the
// client already has it from InstTermOccurrence::designRef / the merged
// instance's occurrence data) and report what's inside it for a nested
// hierarchical schematic box: one level of sub-instances, plus the nets
// wiring them together (and, for a net that also reaches one of the model's
// own boundary ports, that pass-through connection too).
std::string LocalSNLProvider::buildInstanceInternalsResponse(const json& req) const {
  // The requesting box's instance path (names and ids), echoed back as is.
  json instancePath   = req.value("instance_path", json::array());
  json instanceIdPath = req.value("instance_id_path", json::array());
  unsigned dbId = 0, libId = 0, designId = 0;
  if (req.contains("design_ref")) {
    dbId     = req["design_ref"].value("db_id",      0u);
    libId    = req["design_ref"].value("library_id", 0u);
    designId = req["design_ref"].value("design_id",  0u);
  }

  json children = json::array();
  json nets     = json::array();

  auto* model = findDesign(dbId, libId, designId);
  if (model) {
    // Children: everything one level down, primitive or not — the nested
    // box shows the model's actual contents, not split by tree group.
    for (auto* inst : model->getNonPrimitiveInstances()) {
      auto* sub = inst->getModel();
      children.push_back({
        {"name",           instanceName(inst)},
        {"model_name",     sub ? designName(sub) : ""},
        {"primitive_type", toString(getPrimitiveType(sub))},
        {"child_id",       static_cast<unsigned>(inst->getID())},
        {"design_ref", {
          {"db_id",      sub ? static_cast<unsigned>(sub->getDB()->getID()) : 0u},
          {"library_id", sub ? static_cast<unsigned>(sub->getLibrary()->getID()) : 0u},
          {"design_id",  sub ? static_cast<unsigned>(sub->getID()) : 0u}
        }},
        {"has_terms",      sub && !sub->getTerms().empty()},
        {"has_primitives", sub && hasVisiblePrimitiveInstances(sub)},
        {"has_instances",  sub && !sub->getNonPrimitiveInstances().empty()},
        {"source_loc",     sourceLocJson(inst)}
      });
    }
    for (auto* inst : model->getPrimitiveInstances()) {
      auto* sub = inst->getModel();
      if (sub && NLDB0::isAssign(sub)) continue;
      children.push_back({
        {"name",           instanceName(inst)},
        {"model_name",     sub ? designName(sub) : ""},
        {"primitive_type", toString(getPrimitiveType(sub))},
        {"child_id",       static_cast<unsigned>(inst->getID())},
        {"design_ref", {
          {"db_id",      sub ? static_cast<unsigned>(sub->getDB()->getID()) : 0u},
          {"library_id", sub ? static_cast<unsigned>(sub->getLibrary()->getID()) : 0u},
          {"design_id",  sub ? static_cast<unsigned>(sub->getID()) : 0u}
        }},
        {"has_terms",      sub && !sub->getTerms().empty()},
        {"has_primitives", sub && hasVisiblePrimitiveInstances(sub)},
        {"has_instances",  sub && !sub->getNonPrimitiveInstances().empty()},
        {"source_loc",     sourceLocJson(inst)}
      });
    }

    // Internal nets: split each net's components into sub-instance pins
    // (SNLInstTerm) vs the model's own boundary ports (SNLBitTerm) so the
    // client can tell a child-to-child wire from a pass-through to this
    // instance's own external port. Nets with fewer than two live
    // endpoints don't need drawing.
    auto emitBitNet = [&](SNLBitNet* bn, const std::string& name, std::optional<int> bit) {
      json pins = json::array();
      for (auto* comp : bn->getComponents()) {
        if (auto* it = dynamic_cast<SNLInstTerm*>(comp)) {
          auto pin = bitTermJson(it->getBitTerm());
          pin["inst_id"] = static_cast<unsigned>(it->getInstance()->getID());
          pins.push_back(std::move(pin));
        } else if (auto* bt = dynamic_cast<SNLBitTerm*>(comp)) {
          pins.push_back(bitTermJson(bt));
        }
      }
      if (pins.size() < 2) return;
      json n = {{"name", name}, {"pins", std::move(pins)}};
      if (bit.has_value()) n["bit"] = *bit;
      nets.push_back(std::move(n));
    };

    for (auto* net : model->getNets()) {
      if (auto* bus = dynamic_cast<SNLBusNet*>(net)) {
        int lo = std::min(static_cast<int>(bus->getLSB()), static_cast<int>(bus->getMSB()));
        int hi = std::max(static_cast<int>(bus->getLSB()), static_cast<int>(bus->getMSB()));
        for (int b = lo; b <= hi; ++b) {
          if (auto* bit = bus->getBit(b)) emitBitNet(bit, bus->getString(), b);
        }
      } else if (auto* bn = dynamic_cast<SNLBitNet*>(net)) {
        emitBitNet(bn, bn->getString(), std::nullopt);
      }
    }
  }

  return json{
    {"response",  "instance_internals_response"},
    {"instance_path", instancePath},
    {"instance_id_path", instanceIdPath},
    {"children",  children},
    {"nets",      nets}
  }.dump();
}

// Fetches the raw text of an RTL source file named by a source_loc (see
// sourceLocJson() above), so the client can display it without needing
// filesystem access of its own -- the WASM/browser build has none, so this
// goes through the same provider abstraction as everything else rather than
// having native mode read the file directly.
std::string LocalSNLProvider::buildSourceResponse(const json& req) const {
  std::string file = req.value("file", std::string(""));
  int line = req.value("line", 0);

  std::ifstream ifs(file);
  bool found = static_cast<bool>(ifs);
  std::string text;
  if (found) {
    std::ostringstream ss;
    ss << ifs.rdbuf();
    text = ss.str();
  }

  return json{
    {"response", "source_response"},
    {"file",     file},
    {"line",     line},
    {"found",    found},
    {"text",     text}
  }.dump();
}

// General name/value property inspector for whatever object the UI asks
// about (an instance, a term/pin or a net). The instance (or the one
// containing the term/net) is named the same way DiagnosisItem names
// things -- "id_path" (instance ids, preferred) or "path" (instance names),
// root excluded -- see resolveRequestPath() and CLAUDE.md's "Path matching
// convention". Unknown/unresolvable objects yield an empty properties list
// rather than an error, matching "no properties" semantics.
std::string LocalSNLProvider::buildPropertiesResponse(const json& req) const {
  json properties = json::array();
  std::string subject;

  auto emit = [&](const std::string& name, const std::string& value) {
    properties.push_back({{"name", name}, {"value", value}});
  };

  auto* topDesign = db_ ? db_->getTopDesign() : nullptr;
  if (topDesign) {
    std::string kind = req.value("kind", std::string("instance"));
    auto resolved = resolveRequestPath(topDesign, req);
    SNLDesign*   design   = resolved.design;
    SNLInstance* instance = resolved.instance();
    const std::string at  = displayChain(resolved.chain);

    if (!resolved.ok) {
      Console::Error("get_properties: could not resolve instance path");
    } else if (kind == "term") {
      std::string terminal = req.value("terminal", std::string(""));
      subject = at.empty() ? terminal : at + "/" + terminal;
      if (design && !terminal.empty()) {
        if (auto* term = design->getTerm(NLName(terminal))) {
          if (req.contains("bit") && !req["bit"].is_null()) {
            auto* bus = dynamic_cast<SNLBusTerm*>(term);
            auto* bit = bus ? bus->getBit(static_cast<NLID::Bit>(req["bit"].get<int>())) : nullptr;
            if (bit) {
              emit("Name",      termName(bit));
              emit("Direction", toString(Direction(snlDirToInt(bit->getDirection()))));
              emit("Bit",       std::to_string(bit->getBit()));
            }
          } else {
            emit("Name",      termName(term));
            emit("Direction", toString(Direction(snlDirToInt(term->getDirection()))));
            if (auto* bus = dynamic_cast<SNLBusTerm*>(term)) {
              emit("MSB", std::to_string(bus->getMSB()));
              emit("LSB", std::to_string(bus->getLSB()));
            }
          }
        }
      }
    } else if (kind == "net") {
      std::string netName = req.value("net", std::string(""));
      subject = at.empty() ? netName : at + "/" + netName;
      if (design && !netName.empty()) {
        if (auto* net = design->getNet(NLName(netName))) {
          if (req.contains("bit") && !req["bit"].is_null()) {
            auto* bus = dynamic_cast<SNLBusNet*>(net);
            auto* bit = bus ? bus->getBit(static_cast<NLID::Bit>(req["bit"].get<int>())) : nullptr;
            if (bit) {
              emit("Name", bit->getName().getString());
              emit("Bit",  std::to_string(bit->getBit()));
            }
          } else {
            emit("Name", net->getName().getString());
            if (auto* bus = dynamic_cast<SNLBusNet*>(net)) {
              emit("MSB", std::to_string(bus->getMSB()));
              emit("LSB", std::to_string(bus->getLSB()));
            }
          }
        }
      }
    } else { // "instance"
      if (!instance) {
        subject = designName(topDesign);
        emit("Name", subject);
        emit("Type", "Top Design");
      } else {
        subject = displayChain({instance});
        auto* model = instance->getModel();
        emit("Name",  instanceName(instance));  // "" when anonymous
        emit("ID",    std::to_string(instance->getID()));
        emit("Model", model ? designName(model) : "");
        emit("Type",  model && model->isPrimitive() ? "Primitive" : "Hierarchical");
      }
    }
  }

  return json{
    {"response",   "properties_response"},
    {"subject",    subject},
    {"properties", properties}
  }.dump();
}

#endif // __EMSCRIPTEN__
