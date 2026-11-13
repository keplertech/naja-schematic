// The viewer's message handling, shared by every entry point: provider
// setup and the dispatch of every reply/push (setupProvider()), the design
// reload, the fonts. No SDL, GL or ImGui backend: the headless renderer
// (naja-schematic-render, HeadlessView) links this without them; the
// windowed frame loop is in AppLogic.cpp.
#include "AppLogic.h"

#include <algorithm>
#include <iostream>
#include <sstream>

#include <imgui.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "INetlistProvider.h"
#include "NetlistTree.h"
#include "GUIData.h"
#include "Types.h"
#include "Console.h"
#include "EquipotentialView.h"
#include "DiagnosisStore.h"
#include "StyleStore.h"
#include "SourceStore.h"
#include "SourceView.h"
#include "PropertiesStore.h"
#include "SelectionStore.h"
#include "TraceStore.h"
#include "ShowNetStore.h"
#include "ReviewStore.h"
#include "CousineFont.h"
#include "DroidSansFont.h"

#ifndef __EMSCRIPTEN__
#include <fstream>
#endif

// Base UI font size in pixels. ImGui's built-in fallback (Proggy Clean, ~13px
// bitmap font) reads as tiny/blurry on most displays; DroidSans at this size
// is crisp and legible for both chrome (menus, panels) and schematic labels,
// which scale off of it (see SchematicView's labelFontSize()).
static constexpr float kBaseFontSizePx = 17.0f;

void setupFonts() {
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->AddFontFromMemoryCompressedTTF(
      DroidSans_compressed_data, DroidSans_compressed_size, kBaseFontSizePx);
  // Monospace, for code in the Source tab; DroidSans above stays the default.
  SourceView::setFont(io.Fonts->AddFontFromMemoryCompressedTTF(
      Cousine_compressed_data, Cousine_compressed_size, kBaseFontSizePx - 1.0f));
}

// ---------------------------------------------------------------------------
// Provider setup — identical message dispatch for both WASM and native modes.
// ---------------------------------------------------------------------------

// Wire the equipotential-reset callback onto whichever NetlistTree is current.
// Called both at startup and after every design reload.
static void attachTreeCallbacks(AppState& state) {
  state.guiData->netlist_->setOnEquipotentialRequest([&state]() {
    ShowNetStore::forget();
    state.guiData->clearEquipotentials();
    EquipotentialView::resetLayout();
  });
}


void reloadNetlist(AppState& state) {
  // Cleared now, not with clearNets()' next-frame clear: a focus_instance
  // re-pushed after the new root can be drawn before that frame, and the
  // deferred clear would wipe it.
  ShowNetStore::forget();
  state.guiData->clearEquipotentials();
  EquipotentialView::resetLayout();
  delete state.guiData->netlist_;
  state.guiData->netlist_ = new NetlistTree(state.provider);
  attachTreeCallbacks(state);
  state.provider->send(R"({"request":"load_root"})");
}

void setupProvider(AppState& state) {
  state.guiData->netlist_ = new NetlistTree(state.provider);
  EquipotentialView::setProvider(state.provider);
  attachTreeCallbacks(state);

  // Every selection change (tree click, schematic click, host focus) shows
  // the instance's properties and is reported to the host, which scripts
  // on it (the notebook widget's Schematic.selected).
  SelectionStore::setListener([&state](const InstancePath& path) {
    json note;
    note["request"] = "instance_selected";
    writePath(note, path);
    state.provider->send(note.dump());
    json props;
    props["request"] = "get_properties";
    props["kind"]    = "instance";
    writePath(props, path);
    state.provider->send(props.dump());
  });

  state.provider->on_open([&state]() {
    state.connected = true;
    Console::Log("Connected to netlist provider");
    state.provider->send(R"({"request":"load_root"})");
  });

  state.provider->on_message([&state](const std::string& msg) {
    Console::Log("Message received: " + msg);
    std::string clean = msg;
    auto nullPos = clean.find('\0');
    if (nullPos != std::string::npos) clean.resize(nullPos);
    while (!clean.empty() &&
           (clean.back() == '\n' || clean.back() == '\r' ||
            clean.back() == ' '  || clean.back() == '\t')) {
      clean.pop_back();
    }

    json j;
    try {
      j = json::parse(clean);
    } catch (const std::exception& e) {
      Console::Error("Failed to parse JSON: " + std::string(e.what()));
      return;
    }

    std::string resp = j.value("response", "");
    if (resp.empty()) {
      Console::Error("Missing response field.");
      return;
    }

    if (resp == "design_changed") {
      // Host push: the design behind the provider was replaced (or edited
      // in place), so every node and id the viewer holds may be stale.
      // GenerationProvider has already switched to the new generation, so
      // replies for the old one no longer get here, and the load_root
      // below goes out stamped with the new one.
      Console::Log("Design changed by the host: reloading");
      reloadNetlist(state);
      return;
    }

    if (resp == "root_response" || resp == "root_loaded") {
      if (state.guiData->netlist_->getRoot()) {
        // A design_changed that overtook the very first load_root: both
        // that load_root and the reload's are answered for the new design.
        Console::Log("Ignoring a second root for the same design");
        return;
      }
      Console::Log("Root node data received");
      DiagnosisStore::clear();  // stale diagnoses reference the old design
      StyleStore::clear();      // so do styles: the host pushes them again
      PropertiesStore::clear(); // stale properties reference the old design
      SourceStore::clear();     // so do source references
      SelectionStore::clear();  // so does a stale selection
      ReviewStore::clear();     // the host pushes the review again for this design
      state.serverInfo = j.value("server", json::object());
      const auto& root = j["root"];
      if (root.contains("has_terms") || root.contains("has_primitives") || root.contains("has_instances")) {
        InstanceResponseJson data = root.get<InstanceResponseJson>();
        state.guiData->netlist_->createRootNode(
          data.name, data.design_ref,
          data.has_terms, data.has_primitives, data.has_instances, data.has_nets,
          data.has_sources);
      } else {
        DesignRef designRef{};
        if (root.contains("design_ref")) {
          designRef = root["design_ref"].get<DesignRef>();
        }
        state.guiData->netlist_->createRootNode(
          root.value("name", std::string("<unnamed root>")),
          designRef, false, false, root.value("has_children", false), false);
      }
    } else if (resp == "instances_response" || resp == "primitives_response" || resp == "children_loaded") {
      unsigned gui_id = 0;
      std::vector<InstanceResponseJson> children;
      if (resp == "children_loaded") {
        gui_id = j.value("node_gui_id", 0);
        const auto& rawChildren = j["children"];
        if (rawChildren.is_array()) {
          for (const auto& child : rawChildren) {
            InstanceResponseJson item;
            item.name       = child.value("name", "");
            item.model_name = child.value("model_name", "");
            item.child_id   = child.value("instance_id", 0);
            if (child.contains("design_ref"))
              item.design_ref = child["design_ref"].get<DesignRef>();
            bool hasChildren   = child.value("has_children", false);
            item.has_terms     = false;
            item.has_primitives = false;
            item.has_instances  = hasChildren;
            item.has_nets       = false;
            children.push_back(std::move(item));
          }
        }
      } else {
        InstancesResponseJson data = j.get<InstancesResponseJson>();
        gui_id   = data.gui_id;
        children = std::move(data.children);
      }
      auto parent = state.guiData->netlist_->getNode(gui_id);
      if (!parent) {
        Console::Error("Cannot find node: " + std::to_string(gui_id));
        return;
      }
      if (parent->hasChildren()) {
        Console::Error("internal error: node already has children");
        return;
      }
      parent->createChildren();
      for (auto& instance : children) {
        parent->createInstanceNode(
          instance.name, instance.model_name, instance.child_id, instance.design_ref,
          instance.has_terms, instance.has_primitives, instance.has_instances, instance.has_nets,
          instance.has_sources);
      }
    } else if (resp == "terms_response") {
      TermsResponseJson data = j.get<TermsResponseJson>();
      auto parent = state.guiData->netlist_->getNode(data.gui_id);
      if (!parent) {
        Console::Error("Cannot find node: " + std::to_string(data.gui_id));
        return;
      }
      if (parent->hasChildren()) {
        Console::Error("internal error: node already has children");
        return;
      }
      parent->createChildren();
      for (auto& term : data.children) {
        parent->createTermNode(term.name, term.child_id,
                               Direction(term.direction), term.msb, term.lsb, term.has_sources);
      }
    } else if (resp == "nets_response") {
      NetsResponseJson data = j.get<NetsResponseJson>();
      auto parent = state.guiData->netlist_->getNode(data.gui_id);
      if (!parent) {
        Console::Error("Cannot find node: " + std::to_string(data.gui_id));
        return;
      }
      if (parent->hasChildren()) {
        Console::Error("internal error: node already has children");
        return;
      }
      parent->createChildren();
      for (auto& net : data.children) {
        parent->createNetNode(net.name, net.msb, net.lsb, net.has_sources, net.net_id);
      }
    } else if (resp == "equipotential_response") {
      // A trace_id (echoed from a tree "Show Equipotential" or a host's
      // show_net) makes the net a one-net trace; none (a schematic pin
      // click) adds it uncolored.
      int traceId = ShowNetStore::takeReply(j.value("trace_id", 0));
      if (traceId < 0) return;  // a host net the view was cleared of meanwhile
      if (traceId != 0 && !TraceStore::find(traceId)) traceId = 0;  // removed meanwhile
      if (traceId != 0 && j.contains("found") && j["found"].is_boolean() && !j["found"]) {
        // Named a pin the design doesn't have (e.g. a stale host push).
        const std::string& label = TraceStore::find(traceId)->label;
        Console::Error(label.substr(0, label.rfind(" (net)")) + ": no such pin in the design");
      }
      auto* eq = new Equipotential(j.get<Equipotential>());
      if (EquipotentialView::noteUnconnectedReply(j, *eq)) {
        // Nothing else on the pin's net: it's drawn with a no-connect cross.
        const Trace* t = traceId ? TraceStore::find(traceId) : nullptr;
        Console::Log((t ? t->label.substr(0, t->label.rfind(" (net)")) : std::string("Pin")) +
                     ": not connected");
      }
      if (traceId != 0 && eq->terms.empty() && eq->occurrences.empty()) {
        // Nothing to show (unresolved term): no empty row in the Traces tab.
        TraceStore::remove(traceId);
        traceId = 0;
      }
      if (state.guiData->addEquipotential(eq, traceId)) {
        Console::Log("Equipotential data received");
        state.tableEquipotentialCount = 1;
      } else {
        Console::Log("Equipotential already shown");
      }
    } else if (resp == "trace_driver_response") {
      // Nets arrive breadth-first from the traced net toward the drivers, so
      // adding them in order lets the layout chain each one off an instance
      // that's already placed.
      // Nets already on screen (e.g. the one the trace started from, when
      // it's launched from a schematic pin) are skipped rather than drawn
      // twice, but still recorded as on this trace.
      // A provider that doesn't echo trace_id still gets a trace of its own.
      // A host's show_net with "trace": true is handled like its one-net
      // traces (see ShowNetStore::takeReply()).
      int traceId = ShowNetStore::takeReply(j.value("trace_id", 0));
      if (traceId < 0) return;  // a host trace the view was cleared of meanwhile
      if (!TraceStore::find(traceId)) traceId = TraceStore::begin("Trace");
      size_t n = 0, added = 0;
      if (j.contains("equipotentials") && j["equipotentials"].is_array()) {
        for (const auto& e : j["equipotentials"]) {
          ++n;
          if (state.guiData->addEquipotential(new Equipotential(e.get<Equipotential>()), traceId)) ++added;
        }
      }
      state.tableEquipotentialCount = std::max<size_t>(added, 1);
      Console::Log("Driver trace received: " + std::to_string(n) + " net(s), " +
                   std::to_string(added) + " new" +
                   (j.value("truncated", false) ? " (truncated)" : ""));
    } else if (resp == "expanded_instance_terms") {
      auto path = readPath(j, "instance_path", "instance_id_path");
      if (!path) {
        Console::Error("expanded_instance_terms without a valid instance_id_path");
        return;
      }
      std::vector<EquipotentialView::ExpandedPort> ports;
      if (j.contains("terms") && j["terms"].is_array()) {
        for (const auto& t : j["terms"]) {
          EquipotentialView::ExpandedPort ep;
          ep.childId = t.value("child_id", 0u);
          ep.name    = displayPinLabel(t.value("name", std::string("")), ep.childId);
          if (t.contains("bit") && !t["bit"].is_null())
            ep.bit = t["bit"].get<int>();
          ep.clock = t.value("clock", false);
          int dirInt   = t.value("direction", 0);
          ep.direction = dirInt == 1 ? Direction::Output
                       : dirInt == 2 ? Direction::Inout
                                     : Direction::Input;
          ports.push_back(std::move(ep));
        }
      }
      EquipotentialView::applyInstanceExpansion(*path, ports);
    } else if (resp == "instance_internals_response") {
      auto path = readPath(j, "instance_path", "instance_id_path");
      if (!path) {
        Console::Error("instance_internals_response without a valid instance_id_path");
        return;
      }
      EquipotentialView::InstanceInternals data;
      if (j.contains("children") && j["children"].is_array()) {
        for (const auto& c : j["children"]) {
          EquipotentialView::InstanceChild ic;
          ic.name    = c.value("name", std::string(""));
          ic.childId = c.value("child_id", 0u);
          if (c.contains("design_ref"))
            ic.designRef = c["design_ref"].get<DesignRef>();
          ic.primitiveType = primitiveTypeFromString(c.value("primitive_type", std::string("unknown")));
          ic.hasInstances = c.value("has_instances", false) || c.value("has_primitives", false);
          ic.hasSources   = c.value("has_sources", false);
          data.children.push_back(std::move(ic));
        }
      }
      if (j.contains("nets") && j["nets"].is_array()) {
        for (const auto& n : j["nets"]) {
          EquipotentialView::InternalNet in;
          in.name = n.value("name", std::string(""));
          if (n.contains("bit") && !n["bit"].is_null()) in.bit = n["bit"].get<int>();
          if (n.contains("pins") && n["pins"].is_array()) {
            for (const auto& p : n["pins"]) {
              EquipotentialView::InternalPin ip;
              ip.name    = displayTermName(p.value("name", std::string("")),
                                           p.value("child_id", 0u));
              int dirInt = p.value("direction", 0);
              ip.direction = dirInt == 1 ? Direction::Output
                           : dirInt == 2 ? Direction::Inout
                                         : Direction::Input;
              if (p.contains("bit") && !p["bit"].is_null()) ip.bit = p["bit"].get<int>();
              if (p.contains("inst_id") && !p["inst_id"].is_null())
                ip.instChildId = p["inst_id"].get<unsigned>();
              in.pins.push_back(std::move(ip));
            }
          }
          data.nets.push_back(std::move(in));
        }
      }
      EquipotentialView::applyInstanceInternals(*path, data);
    } else if (resp == "sources_response") {
      // The source references of the object a "Show Sources" asked about;
      // the first one's file is opened right away.
      std::vector<SourceRef> refs;
      for (const auto& entry : j.value("sources", json::array()))
        refs.push_back(parseSourceRef(entry));
      json address = json::object();
      for (const char* key : {"kind", "id_path", "path", "terminal", "net", "bit"})
        if (j.contains(key)) address[key] = j[key];
      const bool any = !refs.empty();
      SourceStore::setReferences(j.value("subject", std::string("")), address, std::move(refs));
      state.focusSourceTab  = true;
      state.growSourcePanel = true;
      if (any && state.provider) state.provider->send(SourceStore::loadRequest(0).dump());
    } else if (resp == "source_response") {
      SourceRef ref = parseSourceRef(j);
      if (j.value("found", false)) {
        SourceStore::setSource(ref, j.value("text", std::string("")),
                               j.value("first_line", std::uint64_t(1)), j.value("truncated", false),
                               j.value("path", std::string("")), j.value("stale", false));
        Console::Log("Loaded source: " + ref.file);
      } else {
        SourceStore::setMissing(ref);
        Console::Error("Failed to load source: " + (ref.file.empty() ? "no such reference" : ref.file));
      }
      state.focusSourceTab = true;
    } else if (resp == "properties_response") {
      PropertiesResponseJson data = j.get<PropertiesResponseJson>();
      std::string subject = j.value("subject", std::string(""));
      Console::Log("Properties received: " + std::to_string(data.properties.size()) + " item(s)");
      PropertiesStore::setProperties(subject, std::move(data.properties));
      state.focusPropertiesTab = !state.reviewTabShown;
    } else if (resp == "diagnosis_response") {
      std::vector<DiagnosisItem> items;
      if (j.contains("items") && j["items"].is_array()) {
        for (const auto& raw : j["items"]) {
          items.push_back(raw.get<DiagnosisItem>());
        }
      }
      Console::Log("Diagnosis received: " + std::to_string(items.size()) + " item(s)");
      DiagnosisStore::setDiagnostics(std::move(items));
      state.focusDiagnosisTab = true;
    } else if (resp == "style_response") {
      // Host push (style()): colors, widths, labels on nets, instances, pins.
      std::vector<std::string> errors;
      auto items = StyleStore::parse(j, &errors);
      for (const auto& e : errors) Console::Error(e);
      Console::Log("Styles received: " + std::to_string(items.size()) + " item(s)");
      StyleStore::setStyles(std::move(items));
    } else if (resp == "review_response") {
      ReviewStore::setResponse(j);
    } else if (resp == "export_svg") {
      state.svgExportRequested = true;
    } else if (resp == "show_net") {
      // Host push (show_net()): the net on one pin, as a one-net trace --
      // a tree "Show Equipotential" the host asked for (see ShowNetStore).
      auto req = ShowNetStore::request(j, [&state]() {
        state.guiData->clearEquipotentials();
        EquipotentialView::resetLayout();
      });
      if (req) state.provider->send(req->dump());
    } else if (resp == "focus_instance") {
      // Host push (notebook show_instance()): resolve the path, then
      // instance_resolved below reveals and draws it. The host names the
      // instance by id_path (preferred: it reaches anonymous instances)
      // and/or by names; both are passed on for the provider to pick.
      // The host nets shown so far go with the view it clears -- unless
      // the push has "add": true (the headless renderer drawing several
      // instances): the instance's box is then added to what's shown, as
      // a review "go to" adds one, and the selection isn't moved.
      const bool add = j.value("add", false);
      json req;
      req["request"] = "resolve_instance";
      for (const char* key : {"path", "id_path"})
        if (j.contains(key) && j[key].is_array()) req[key] = j[key];
      if (!req.contains("path") && !req.contains("id_path")) req["path"] = json::array();
      if (add) {
        if (auto path = readPath(req)) EquipotentialView::addOnResolve(*path);
      } else {
        ShowNetStore::forget();
      }
      state.provider->send(req.dump());
    } else if (resp == "instance_resolved") {
      auto path = j.value("found", false) ? readPath(j) : std::nullopt;
      if (!path) {
        std::string what = j.contains("id_path") ? j["id_path"].dump() : "";
        if (j.contains("path") && j["path"].is_array()) {
          std::string names;
          for (const auto& seg : j["path"])
            if (seg.is_string()) names += (names.empty() ? "" : "/") + seg.get<std::string>();
          what = what.empty() ? names : names + " " + what;
        }
        Console::Error("No instance '" + what + "' in the design");
        return;
      }
      // Start a fresh schematic from the instance alone, all pins open --
      // or, asked for by a review "go to", add it to what's shown.
      const bool add = EquipotentialView::takeAddOnResolve(*path);
      if (auto start = EquipotentialView::startInstanceFromResolved(j)) {
        if (!add) {
          state.guiData->clearEquipotentials();
          EquipotentialView::resetLayout();
        }
        EquipotentialView::showInstance(*start);
      }
      if (!add) SelectionStore::select(*path, SelectionStore::Origin::Host);
    } else if (resp == "error") {
      std::cerr << "Backend error: " << j["message"] << std::endl;
      // A refused review write: shown in the Review tab.
      const std::string request = j.value("request", std::string());
      if (request.rfind("review_", 0) == 0) {
        const std::string message = j.value("message", std::string("refused"));
        Console::Error(request + ": " + message);
        ReviewStore::setError(message);
      }
    }
  });

  state.provider->on_error([](const std::string& err) {
    Console::Error("Provider error: " + err);
  });

  state.provider->on_close([]() {
    Console::Error("Provider connection closed");
  });

  state.provider->start();
}

#ifndef __EMSCRIPTEN__
// ---------------------------------------------------------------------------
// Native diagnosis-file loading — shared by the menu action and by main()'s
// --diagnosis flag. See AppLogic.h for the calling contract.
// ---------------------------------------------------------------------------

bool loadDiagnosisFile(const std::string& path) {
  std::ifstream ifs(path);
  if (!ifs) {
    Console::Error("Failed to open diagnosis file: " + path);
    return false;
  }
  try {
    json j; ifs >> j;
    const auto& arr = j.contains("items") ? j["items"] : j;
    std::vector<DiagnosisItem> items;
    if (arr.is_array()) {
      for (const auto& raw : arr) items.push_back(raw.get<DiagnosisItem>());
    }
    Console::Log("Loaded " + std::to_string(items.size()) + " diagnosis item(s) from " + path);
    DiagnosisStore::setDiagnostics(std::move(items));
    return true;
  } catch (const std::exception& e) {
    Console::Error("Failed to parse diagnosis JSON: " + std::string(e.what()));
    return false;
  }
}

bool loadStyleFile(const std::string& path) {
  std::ifstream ifs(path);
  if (!ifs) {
    Console::Error("Failed to open style file: " + path);
    return false;
  }
  try {
    json j; ifs >> j;
    std::vector<std::string> errors;
    auto items = StyleStore::parse(j, &errors);
    for (const auto& e : errors) Console::Error(path + ": " + e);
    Console::Log("Loaded " + std::to_string(items.size()) + " style item(s) from " + path);
    StyleStore::setStyles(std::move(items));
    return errors.empty();
  } catch (const std::exception& e) {
    Console::Error("Failed to parse style JSON: " + std::string(e.what()));
    return false;
  }
}
#endif // __EMSCRIPTEN__
