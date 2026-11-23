#include "Types.h"

#include <set>
#include <tuple>

#include "Console.h"

SourceRef parseSourceRef(const json& j) {
  SourceRef ref;
  ref.role = j.value("role", std::string(""));
  if (j.contains("index") && j["index"].is_number_unsigned())
    ref.index = j["index"].get<unsigned>();
  ref.file           = j.value("file", std::string(""));
  // Missing coordinates stay 0 (unknown): an end is never made up.
  ref.line           = j.value("line", 0u);
  ref.column         = j.value("column", 0u);
  ref.endLine        = j.value("end_line", 0u);
  ref.endColumn      = j.value("end_column", 0u);
  ref.provider       = j.value("provider", std::string(""));
  ref.language       = j.value("language", std::string(""));
  ref.representation = j.value("representation", std::string(""));
  return ref;
}

std::string sourceRefLocation(const SourceRef& ref) {
  std::string out = ref.file.empty() ? "<unknown file>" : ref.file;
  if (ref.line) {
    out += ":" + std::to_string(ref.line);
    if (ref.column) out += ":" + std::to_string(ref.column);
  }
  return out;
}

Direction intToDirection(int dirInt) {
  switch (dirInt) {
    case 0:
      return Direction::Input;
    case 1:
      return Direction::Output;
    case 2:
      return Direction::Inout;
    default:
      Console::Error("Invalid direction: " + std::to_string(dirInt) + ", defaulting to Input");
      return Direction::Input;
  }
}

void from_json(const json& j, DesignRef& d) {
  j.at("db_id").get_to(d.db_id);
  j.at("library_id").get_to(d.library_id);
  j.at("design_id").get_to(d.design_id);
}

void from_json(const json& j, InstanceResponseJson& r) {
  if (j.contains("name") && !j["name"].is_null())
    r.name = j["name"].get<std::string>();
  if (j.contains("model_name") && !j["model_name"].is_null())
    r.model_name = j["model_name"].get<std::string>();
  r.primitive_type = primitiveTypeFromString(j.value("primitive_type", std::string("unknown")));

  r.child_id = j.value("child_id", 0u);
  j.at("design_ref").get_to(r.design_ref);
  j.at("has_primitives").get_to(r.has_primitives);
  j.at("has_instances").get_to(r.has_instances);
  j.at("has_terms").get_to(r.has_terms);
  j.at("has_nets").get_to(r.has_nets);
  r.has_sources = j.value("has_sources", false);
}

void from_json(const json& j, InstancesResponseJson& instances) {
  j.at("gui_id").get_to(instances.gui_id);

  if (j.contains("children") && j["children"].is_array()) {
    for (const auto& child : j["children"]) {
      instances.children.push_back(child.get<InstanceResponseJson>());
    }
  }  
}

void from_json(const json& j, TermsResponseJson& t) {
  j.at("gui_id").get_to(t.gui_id);

  if (j.contains("children") && j["children"].is_array()) {
    for (const auto& child : j["children"]) {
      TermResponseJson term;

      if (child.contains("name") && !child["name"].is_null()) {
        term.name = child["name"].get<std::string>();
      }
      child.at("child_id").get_to(term.child_id);

      if (child.contains("direction") && !child["direction"].is_null()) {
        term.direction = intToDirection(child["direction"].get<int>());
      }

      if (child.contains("msb") && !child["msb"].is_null()) {
        term.msb = child["msb"].get<int>();
      } else {
        term.msb = std::nullopt;
      }

      if (child.contains("lsb") && !child["lsb"].is_null()) {
        term.lsb = child["lsb"].get<int>();
      } else {
        term.lsb = std::nullopt;
      }
      term.has_sources = child.value("has_sources", false);

      t.children.push_back(std::move(term));
    }
  }
}

void from_json(const json& j, NetsResponseJson& n) {
  j.at("gui_id").get_to(n.gui_id);

  if (j.contains("children") && j["children"].is_array()) {
    for (const auto& child : j["children"]) {
      NetResponseJson net;

      if (child.contains("name") && !child["name"].is_null()) {
        net.name = child["name"].get<std::string>();
      }
      if (child.contains("net_id") && child["net_id"].is_number_unsigned()) {
        net.net_id = child["net_id"].get<unsigned>();
      }

      if (child.contains("msb") && !child["msb"].is_null()) {
        net.msb = child["msb"].get<int>();
      } else {
        net.msb = std::nullopt;
      }

      if (child.contains("lsb") && !child["lsb"].is_null()) {
        net.lsb = child["lsb"].get<int>();
      } else {
        net.lsb = std::nullopt;
      }
      net.has_sources = child.value("has_sources", false);

      n.children.push_back(std::move(net));
    }
  }
}

// An equipotential pin's optional "net"/"net_id" (see BitTerm::net).
static void readPinNet(const json& j, BitTerm& term) {
  if (j.contains("net") && j["net"].is_string()) term.net = j["net"].get<std::string>();
  if (j.contains("net_id") && j["net_id"].is_number_integer() && j["net_id"].get<long long>() >= 0)
    term.netId = j["net_id"].get<unsigned>();
}

// [name, child_id, model] levels, as occurrence paths carry them.
static bool readLevels(const json& j, InstancePath& path, std::vector<std::string>& models) {
  if (!j.is_array()) return false;
  for (const auto& level : j) {
    if (!level.is_array() || level.size() < 2 || !level[0].is_string() ||
        !level[1].is_number_integer() || level[1].get<long long>() < 0) return false;
    path.push_back({ level[1].get<unsigned>(), level[0].get<std::string>() });
    models.push_back(level.size() > 2 && level[2].is_string() ? level[2].get<std::string>() : std::string());
  }
  return true;
}

// An equipotential's "graph" (NetGraph), or nothing when it doesn't fit the
// body it came with: an index out of range or a malformed entry.
static std::optional<NetGraph> readNetGraph(const json& j, const Equipotential& e) {
  if (!j.contains("nodes") || !j["nodes"].is_array() || !j.contains("edges") || !j["edges"].is_array())
    return std::nullopt;
  auto indices = [](const json& v, size_t size, std::vector<size_t>& out) {
    if (!v.is_array()) return false;
    for (const auto& i : v) {
      if (!i.is_number_integer() || i.get<long long>() < 0 || i.get<size_t>() >= size) return false;
      out.push_back(i.get<size_t>());
    }
    return true;
  };
  NetGraph g;
  for (const auto& nj : j["nodes"]) {
    NetGraphNode n;
    std::vector<std::string> models;
    if (!nj.is_object() || !readLevels(nj.value("path", json::array()), n.path, models) ||
        !indices(nj.value("occurrences", json::array()), e.occurrences.size(), n.occurrences) ||
        !indices(nj.value("terms", json::array()), e.terms.size(), n.terms))
      return std::nullopt;
    if (nj.contains("net") && nj["net"].is_string()) n.net = nj["net"].get<std::string>();
    g.nodes.push_back(std::move(n));
  }
  for (const auto& ej : j["edges"]) {
    if (!ej.is_object()) return std::nullopt;
    NetGraphEdge edge;
    const json from = ej.value("from", json()), to = ej.value("to", json());
    if (!from.is_number_integer() || !to.is_number_integer() || from.get<long long>() < 0 ||
        to.get<long long>() < 0 || from.get<size_t>() >= g.nodes.size() || to.get<size_t>() >= g.nodes.size())
      return std::nullopt;
    edge.from = from.get<size_t>();
    edge.to   = to.get<size_t>();
    edge.port = ej.value("kind", std::string()) == "port";
    if (edge.port) {
      const json pin = ej.value("pin", json());
      if (!pin.is_object() || !readLevels(pin.value("path", json()), edge.path, edge.pathModels) ||
          edge.path.empty() || !pin.contains("child_id") || !pin["child_id"].is_number_integer())
        return std::nullopt;
      edge.pin.name      = pin.value("name", std::string());
      edge.pin.child_id  = pin["child_id"].get<unsigned>();
      edge.pin.direction = intToDirection(pin.value("direction", 2));
      if (pin.contains("bit") && pin["bit"].is_number_integer()) edge.pin.bit = pin["bit"].get<int>();
    }
    g.edges.push_back(std::move(edge));
  }
  return g;
}

void from_json(const json& j, Equipotential& e) {
  if (j.contains("terms") && j["terms"].is_array()) {
    for (const auto& termJson : j["terms"]) {
      BitTerm term;
      if (termJson.contains("name") && !termJson["name"].is_null()) {
        term.name = termJson["name"].get<std::string>();
      }
      // The port's id: what a pin click, a trace or a review anchor on its
      // flag sends back.
      if (termJson.contains("child_id") && !termJson["child_id"].is_null()) {
        term.child_id = termJson["child_id"].get<unsigned>();
      }
      if (termJson.contains("direction") && !termJson["direction"].is_null()) {
        term.direction = intToDirection(termJson["direction"].get<int>());
      }
      if (termJson.contains("bit") && !termJson["bit"].is_null()) {
        term.bit = termJson["bit"].get<int>();
      } else {
        term.bit = std::nullopt;
      }
      readPinNet(termJson, term);

      e.terms.push_back(std::move(term));
    }

    for (const auto& occJson : j["occurrences"]) {
      InstTermOccurrence occurrence;

      if (occJson.contains("path") && occJson["path"].is_array()) {
        for (const auto& pathElem : occJson["path"]) {
          //pathElem is a table of 2 elements: {name:..., child_id:...}
          if (!pathElem.is_array()) {
            continue;
          }
          const auto& name     = pathElem[0].get<std::string>();
          const auto& child_id = pathElem[1].get<unsigned>();
          occurrence.path.push_back({child_id, name});
          occurrence.pathIds.push_back(child_id);
          // Optional third element: the instance's model name.
          occurrence.pathModels.push_back(
              pathElem.size() > 2 && pathElem[2].is_string() ? pathElem[2].get<std::string>()
                                                             : std::string());
        }
      }

      BitTerm term;
      if (occJson.contains("name") && !occJson["name"].is_null()) {
        term.name = occJson["name"].get<std::string>();
      }
      if (occJson.contains("child_id") && !occJson["child_id"].is_null()) {
        term.child_id = occJson["child_id"].get<unsigned>();
      } else if (occJson.contains("term_id") && !occJson["term_id"].is_null()) {
        // naja-schematic <= 0.2.x Python hosts named it "term_id".
        term.child_id = occJson["term_id"].get<unsigned>();
      }
      if (occJson.contains("direction") && !occJson["direction"].is_null()) {
        term.direction = intToDirection(occJson["direction"].get<int>());
      }
      term.clock = occJson.value("clock", false);
      if (occJson.contains("role") && occJson["role"].is_string())
        term.role = occJson["role"].get<std::string>();
      if (occJson.contains("bit") && !occJson["bit"].is_null()) {
        term.bit = occJson["bit"].get<int>();
      } else {
        term.bit = std::nullopt;
      }

      readPinNet(occJson, term);

      occurrence.term = std::move(term);

      if (occJson.contains("design_ref") && occJson["design_ref"].is_object())
        occurrence.designRef = occJson["design_ref"].get<DesignRef>();

      occurrence.primitiveType = primitiveTypeFromString(occJson.value("primitive_type", std::string("unknown")));
      occurrence.has_instances = occJson.value("has_instances", false);
      if (occJson.contains("bit_term_count") && occJson["bit_term_count"].is_number_unsigned())
        occurrence.bit_term_count = occJson["bit_term_count"].get<size_t>();
      occurrence.has_sources   = occJson.value("has_sources", false);

      e.occurrences.push_back(std::move(occurrence));
    }
  }
  if (j.contains("graph") && j["graph"].is_object()) e.graph = readNetGraph(j["graph"], e);
}

// An instance ID on the wire: a non-negative integer (nlohmann keeps
// integers built in C++ from an `int` signed, so is_number_unsigned() alone
// would reject them).
static bool isInstanceId(const json& v) {
  return v.is_number_integer() && v.get<long long>() >= 0;
}

static DiagnosisKind diagnosisKindFromString(const std::string& s) {
  if (s == "net") return DiagnosisKind::Net;
  return DiagnosisKind::Instance;
}

static DiagnosisSeverity diagnosisSeverityFromString(const std::string& s) {
  if (s == "error")   return DiagnosisSeverity::Error;
  if (s == "warning") return DiagnosisSeverity::Warning;
  return DiagnosisSeverity::Info;
}

void from_json(const json& j, DiagnosisItem& d) {
  d.kind = diagnosisKindFromString(j.value("kind", std::string("instance")));

  d.path.clear();
  if (j.contains("path") && j["path"].is_array()) {
    for (const auto& seg : j["path"]) {
      if (seg.is_string()) d.path.push_back(seg.get<std::string>());
    }
  }
  d.idPath.reset();
  if (j.contains("id_path") && j["id_path"].is_array()) {
    std::vector<unsigned> ids;
    for (const auto& seg : j["id_path"]) {
      if (!isInstanceId(seg)) { ids.clear(); break; }
      ids.push_back(seg.get<unsigned>());
    }
    // A malformed id_path is ignored (the item falls back to `path`)
    // rather than silently truncated to a different instance.
    if (ids.size() == j["id_path"].size()) d.idPath = std::move(ids);
  }

  d.terminal = j.value("terminal", std::string(""));
  d.severity = diagnosisSeverityFromString(j.value("severity", std::string("info")));
  d.message  = j.value("message", std::string(""));
  d.source   = j.value("source", std::string(""));
}

void from_json(const json& j, PropertyItem& p) {
  p.name  = j.value("name", std::string(""));
  p.value = j.value("value", std::string(""));
}

void from_json(const json& j, PropertiesResponseJson& r) {
  r.properties.clear();
  if (j.contains("properties") && j["properties"].is_array()) {
    for (const auto& raw : j["properties"]) {
      r.properties.push_back(raw.get<PropertyItem>());
    }
  }
}

// One endpoint of an equipotential, by name: (false, {}, "<term>[bit]") for
// a top-level term, (true, instance path, "<pin>[bit]") for an instance pin.
using EndpointKey = std::tuple<bool, InstancePath, std::string>;

static std::set<EndpointKey> endpointKeys(const Equipotential& e) {
  std::set<EndpointKey> keys;
  for (const auto& t : e.terms) keys.insert({false, {}, t.getString()});
  for (const auto& o : e.occurrences) keys.insert({true, o.path, o.term.getString()});
  return keys;
}

bool equipotentialCovers(const Equipotential& shown, const Equipotential& candidate) {
  const auto have = endpointKeys(shown);
  for (const auto& k : endpointKeys(candidate))
    if (!have.count(k)) return false;
  return true;
}

void writePath(json& j, const InstancePath& path, const char* namesKey, const char* idsKey) {
  j[namesKey] = pathNames(path);
  j[idsKey]   = pathIds(path);
}

std::optional<InstancePath> readPath(const json& j, const char* namesKey, const char* idsKey) {
  if (!j.contains(idsKey) || !j[idsKey].is_array()) return std::nullopt;
  const json& ids = j[idsKey];
  const json* names = j.contains(namesKey) && j[namesKey].is_array() ? &j[namesKey] : nullptr;
  if (names && names->size() != ids.size()) return std::nullopt;
  InstancePath path;
  for (size_t i = 0; i < ids.size(); ++i) {
    if (!isInstanceId(ids[i])) return std::nullopt;
    InstanceRef ref;
    ref.id = ids[i].get<unsigned>();
    if (names && (*names)[i].is_string()) ref.name = (*names)[i].get<std::string>();
    path.push_back(std::move(ref));
  }
  return path;
}
