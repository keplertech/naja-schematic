// LocalSNLProvider against the wire-protocol cases shared with protocol.py
// (tests/data/anonymous_protocol_cases.json and source_protocol_cases.json,
// also run by python/tests/test_anonymous_instances.py and
// test_source_references.py): both backends must give the same answers,
// notably for anonymous instances addressed by id_path and for source
// references.
#include "LocalSNLProvider.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include "NLUniverse.h"
#include "NLDB.h"
#include "NLLibrary.h"
#include "SNLBusNet.h"
#include "SNLBusNetBit.h"
#include "SNLBusTerm.h"
#include "SNLBusTermBit.h"
#include "SNLDesign.h"
#include "SNLInstance.h"
#include "SNLInstTerm.h"
#include "SNLRTLInfos.h"
#include "SNLScalarNet.h"
#include "SNLScalarTerm.h"
#include "SNLVRLConstructor.h"

using json = nlohmann::json;
using namespace naja::NL;

namespace {

json loadSpec(const std::string& name) {
  std::ifstream in(std::string(NAJA_SCHEMATIC_TEST_DATA) + "/" + name);
  return json::parse(in);
}

SNLTerm::Direction direction(const std::string& d) {
  if (d == "output") return SNLTerm::Direction::Output;
  if (d == "inout")  return SNLTerm::Direction::InOut;
  return SNLTerm::Direction::Input;
}

// [name, direction] for a scalar term, [name, direction, msb, lsb] for a bus.
void createTerms(SNLDesign* d, const json& terms) {
  for (const auto& t : terms) {
    if (t.size() == 4)
      SNLBusTerm::create(d, direction(t[1]), t[2].get<int>(), t[3].get<int>(),
                         NLName(t[0].get<std::string>()));
    else
      SNLScalarTerm::create(d, direction(t[1]), NLName(t[0].get<std::string>()));
  }
}

// Loads the spec'd gate-level Verilog (same as load_verilog_design() on the
// Python side): from `root` as the current directory, so recorded paths
// stay relative, Yosys src decoding on, into a DB of its own. Returns the
// top and every model below it by name.
std::map<std::string, SNLDesign*> loadVerilogDesign(NLDB* db, const json& spec,
                                                    const std::filesystem::path& root) {
  auto* lib = NLLibrary::create(db, NLName("case_work"));
  SNLVRLConstructor ctor(lib);
  ctor.config_.yosysSourceAttributes_ = true;
  auto previous = std::filesystem::current_path();
  std::filesystem::current_path(root);
  ctor.construct(std::vector<std::filesystem::path>{spec["verilog"].get<std::string>()});
  std::filesystem::current_path(previous);
  std::map<std::string, SNLDesign*> designs;
  std::vector<SNLDesign*> pending{lib->getSNLDesign(NLName(spec["top"].get<std::string>()))};
  while (!pending.empty()) {
    auto* design = pending.back();
    pending.pop_back();
    if (!designs.emplace(design->getName().getString(), design).second) continue;
    for (auto* inst : design->getInstances()) pending.push_back(inst->getModel());
  }
  return designs;
}

// Builds the spec'd netlist (same as build_design() on the Python side):
// instances in list order, so their ids are their list indices.
std::map<std::string, SNLDesign*> buildDesign(NLDB* db, const json& spec) {
  std::map<std::string, SNLDesign*> designs;
  auto* prims = NLLibrary::create(db, NLLibrary::Type::Primitives, NLName("case_prims"));
  for (const auto& p : spec["primitives"]) {
    auto* d = SNLDesign::create(prims, SNLDesign::Type::Primitive, NLName(p["name"].get<std::string>()));
    createTerms(d, p["terms"]);
    designs[p["name"]] = d;
  }
  auto* lib = NLLibrary::create(db, NLName("case_work"));
  for (const auto& s : spec["designs"]) {
    auto* d = SNLDesign::create(lib, NLName(s["name"].get<std::string>()));
    createTerms(d, s["terms"]);
    std::vector<SNLInstance*> instances;
    for (const auto& i : s["instances"]) {
      const std::string name = i[0];
      auto* inst = SNLInstance::create(d, designs.at(i[1]), name.empty() ? NLName() : NLName(name));
      EXPECT_EQ(inst->getID(), instances.size());
      instances.push_back(inst);
    }
    for (const auto& n : s["nets"]) {
      if (n.contains("msb")) {
        SNLBusNet::create(d, n["msb"].get<int>(), n["lsb"].get<int>(),
                          NLName(n["name"].get<std::string>()));
        continue;
      }
      auto* net = SNLScalarNet::create(d, NLName(n["name"].get<std::string>()));
      if (n.contains("term"))
        d->getScalarTerm(NLName(n["term"].get<std::string>()))->setNet(net);
      for (const auto& pin : n.value("pins", json::array())) {
        auto* inst = instances.at(pin[0].get<size_t>());
        inst->getInstTerm(inst->getModel()->getScalarTerm(NLName(pin[1].get<std::string>())))->setNet(net);
      }
    }
    designs[s["name"]] = d;
  }
  return designs;
}

json designRef(const SNLDesign* d) {
  return {{"db_id", d->getDB()->getID()},
          {"library_id", d->getLibrary()->getID()},
          {"design_id", d->getID()}};
}

json substitute(const json& v, const std::map<std::string, SNLDesign*>& designs) {
  const std::string prefix = "$design:";
  if (v.is_string() && v.get<std::string>().rfind(prefix, 0) == 0)
    return designRef(designs.at(v.get<std::string>().substr(prefix.size())));
  if (v.is_object()) {
    json out = json::object();
    for (auto it = v.begin(); it != v.end(); ++it) out[it.key()] = substitute(it.value(), designs);
    return out;
  }
  if (v.is_array()) {
    json out = json::array();
    for (const auto& e : v) out.push_back(substitute(e, designs));
    return out;
  }
  return v;
}

// Same rule as matches() in the Python runner: every key of `expected` in
// `actual` with that value, lists element-wise with the same length. Returns
// the first mismatch, "" if none.
std::string matches(const json& expected, const json& actual, const std::string& where) {
  if (expected.is_object()) {
    if (!actual.is_object()) return where + ": expected an object, got " + actual.dump();
    for (auto it = expected.begin(); it != expected.end(); ++it) {
      if (!actual.contains(it.key())) return where + ": missing " + it.key();
      auto err = matches(it.value(), actual[it.key()], where + "." + it.key());
      if (!err.empty()) return err;
    }
    return "";
  }
  if (expected.is_array()) {
    if (!actual.is_array() || actual.size() != expected.size())
      return where + ": expected " + expected.dump() + ", got " + actual.dump();
    for (size_t i = 0; i < expected.size(); ++i) {
      auto err = matches(expected[i], actual[i], where + "[" + std::to_string(i) + "]");
      if (!err.empty()) return err;
    }
    return "";
  }
  // nlohmann compares numbers across signed/unsigned, but not bool with int.
  if (expected != actual) return where + ": expected " + expected.dump() + ", got " + actual.dump();
  return "";
}

std::string matchesUnordered(const json& expected, const json& actual, const std::string& where) {
  if (!actual.is_array() || actual.size() != expected.size())
    return where + ": expected " + expected.dump() + ", got " + actual.dump();
  std::vector<json> left(actual.begin(), actual.end());
  for (const auto& e : expected) {
    auto hit = std::find_if(left.begin(), left.end(),
                            [&](const json& a) { return matches(e, a, "").empty(); });
    if (hit == left.end()) return where + ": nothing left matches " + e.dump();
    left.erase(hit);
  }
  return "";
}

// One case file: its design built in a DB of its own, served by a provider
// of its own (with the file's source root, if any).
struct CaseSuite {
  json                              spec;
  std::map<std::string, SNLDesign*> designs;
  LocalSNLProvider*                 provider = nullptr;
  json                              replies = json::array();

  explicit CaseSuite(const std::string& file): spec(loadSpec(file)) {
    if (!NLUniverse::get()) NLUniverse::create();
    auto* db = NLDB::create(NLUniverse::get());
    const std::string data = NAJA_SCHEMATIC_TEST_DATA;
    designs = spec["design"].contains("verilog")
      ? loadVerilogDesign(db, spec["design"], data + "/" + spec["source_root"].get<std::string>())
      : buildDesign(db, spec["design"]);
    auto* top = designs.at(spec["design"]["top"]);
    db->setTopDesign(top);
    NLUniverse::get()->setTopDesign(top);
    provider = new LocalSNLProvider();
    provider->setDB(db);
    if (spec.contains("source_root"))
      provider->addSourceRoot(data + "/" + spec["source_root"].get<std::string>());
    for (const auto& m : spec.value("source_map", json::array()))
      provider->addSourceMap(m[0].get<std::string>(), data + "/" + m[1].get<std::string>());
    provider->on_message([this](const std::string& msg) { replies.push_back(json::parse(msg)); });
  }

  json ask(const json& request) {
    replies = json::array();
    provider->send(request.dump());
    return replies;
  }

  void runCases() {
    ASSERT_FALSE(spec["cases"].empty());
    for (const auto& c : spec["cases"]) {
      SCOPED_TRACE(c["name"].get<std::string>());
      json got    = ask(substitute(c["request"], designs));
      json expect = c["expect"];
      for (const auto& key : c.value("unordered", json::array())) {
        ASSERT_FALSE(got.empty());
        json expectedList = expect[0][key.get<std::string>()];
        expect[0].erase(key.get<std::string>());
        EXPECT_EQ(matchesUnordered(expectedList, got[0].value(key.get<std::string>(), json()),
                                   "replies[0]." + key.get<std::string>()), "");
      }
      EXPECT_EQ(matches(expect, got, "replies"), "");
      for (const auto& key : c.value("absent", json::array())) {
        ASSERT_FALSE(got.empty());
        EXPECT_FALSE(got[0].contains(key.get<std::string>())) << key << " in " << got[0].dump();
      }
    }
  }
};

class ProviderProtocol : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { suite_ = new CaseSuite("anonymous_protocol_cases.json"); }
  static CaseSuite* suite_;
  LocalSNLProvider* provider_ = suite_->provider;
};

CaseSuite* ProviderProtocol::suite_ = nullptr;

class SourceProtocol : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { suite_ = new CaseSuite("source_protocol_cases.json"); }
  static CaseSuite* suite_;
};

CaseSuite* SourceProtocol::suite_ = nullptr;

} // namespace

TEST_F(ProviderProtocol, AnswersEverySharedCaseLikeProtocolPy) {
  suite_->runCases();
}

TEST_F(SourceProtocol, AnswersEverySharedCaseLikeProtocolPy) {
  suite_->runCases();
}

// What a frontend predating typed references leaves: only the unclassified
// legacy location, listed as such (protocol.py: test_legacy_location_...).
// On the anonymous-instance design, which has no source information.
TEST_F(ProviderProtocol, ListsTheLegacyLocationWhenNoTypedReference) {
  auto* mid = suite_->designs.at("mid");
  auto* net = mid->getNet(NLName("o"));
  SNLRTLInfos::create(net)->setSourceLoc({NLName("a.sv"), 3, 4, 5, 0});
  json reply = suite_->ask({{"request", "get_sources"}, {"kind", "net"},
                            {"id_path", {0}}, {"net", "o"}})[0];
  json nets = suite_->ask({{"request", "load_nets"}, {"gui_id", 0},
                           {"design_ref", designRef(mid)}})[0];
  net->getRTLInfos()->destroy();
  EXPECT_EQ(reply["sources"], json::parse(R"([{"role": "legacy", "file": "a.sv",
    "line": 3, "column": 5, "end_line": 4, "end_column": 0,
    "provider": "", "language": "", "representation": ""}])"));
  EXPECT_FALSE(nets["children"][0].contains("has_sources"));  // net i
  EXPECT_EQ(nets["children"][1]["has_sources"], true);         // net o
}

// A big file (a gate-level netlist) only sends the lines around the reference.
TEST_F(SourceProtocol, SendsAWindowOfALargeFile) {
  auto dir = std::filesystem::temp_directory_path() / "naja_schematic_big_source";
  std::filesystem::create_directories(dir);
  {
    std::ofstream out(dir / "big.v");
    for (int n = 1; n <= 200000; ++n) out << "line " << n << " ..........................\n";
  }
  auto* top = suite_->designs.at("top");
  auto* infos = top->getRTLInfos();
  auto saved = *infos->getSourceDeclaration();
  SNLSourceReference ref = saved;
  ref.range.file = NLName((dir / "big.v").string());
  ref.range.line = 100000;
  ref.range.endLine = 100002;
  infos->setSourceDeclaration(ref);
  json reply = suite_->ask({{"request", "load_source"}, {"kind", "instance"},
                            {"id_path", json::array()}, {"role", "declaration"}})[0];
  infos->setSourceDeclaration(saved);
  std::filesystem::remove_all(dir);
  EXPECT_EQ(reply["found"], true);
  EXPECT_EQ(reply["stale"], true);  // written after the design was loaded
  EXPECT_EQ(reply["truncated"], true);
  EXPECT_EQ(reply["first_line"], 99500);
  std::string text = reply["text"];
  EXPECT_EQ(text.rfind("line 99500 ", 0), 0u);
  EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 100502 - 99500 + 1);
}

// termRequest(): a terminal named by a script (headless --trace /
// --equipotential) turned into the ids a load_equipotential request takes.
TEST_F(ProviderProtocol, TermRequestResolvesTerminalsByName) {
  std::string error;
  auto anon = provider_->termRequest(json{{"id_path", {1, 0}}, {"terminal", "A"}}, error);
  ASSERT_TRUE(anon) << error;
  EXPECT_EQ((*anon)["path"], json({1, 0}));
  EXPECT_EQ((*anon)["term_id"], 0);
  EXPECT_FALSE(anon->contains("bit"));

  auto named = provider_->termRequest(json{{"path", {"a/b", "u_leaf"}}, {"terminal", "Y"}}, error);
  ASSERT_TRUE(named) << error;
  EXPECT_EQ((*named)["path"], json({2, 2}));
  EXPECT_EQ((*named)["term_id"], 1);

  auto port = provider_->termRequest(json{{"id_path", json::array()}, {"terminal", "x"}}, error);
  ASSERT_TRUE(port) << error;
  EXPECT_EQ((*port)["path"], json::array());
  EXPECT_EQ((*port)["term_id"], 0);
}

TEST_F(ProviderProtocol, TermRequestRejectsWhatDoesNotResolve) {
  std::string error;
  EXPECT_FALSE(provider_->termRequest(json{{"id_path", {3}}, {"terminal", "nope"}}, error));
  EXPECT_NE(error.find("nope"), std::string::npos);
  EXPECT_FALSE(provider_->termRequest(json{{"path", {""}}, {"terminal", "i"}}, error));  // anonymous by name
  EXPECT_FALSE(provider_->termRequest(json{{"id_path", {9}}, {"terminal", "i"}}, error));
  EXPECT_FALSE(provider_->termRequest(json{{"id_path", {3}}, {"terminal", "i"}, {"bit", 0}}, error));  // scalar
  EXPECT_FALSE(provider_->termRequest(json{{"id_path", {3}}}, error));  // no terminal
}

// A source map rewrites a prefix only at a path boundary, and a map that
// leads nowhere falls through to the next (protocol.py: test_source_maps).
TEST_F(SourceProtocol, SourceMapsMatchAtPathBoundaries) {
  const std::string sources = std::string(NAJA_SCHEMATIC_TEST_DATA) + "/sources";
  auto ask = [&](const std::vector<std::pair<std::string, std::string>>& maps) {
    LocalSNLProvider provider;
    provider.setDB(suite_->designs.at("top")->getDB());
    for (const auto& [from, to] : maps) provider.addSourceMap(from, to);
    json reply;
    provider.on_message([&](const std::string& msg) { reply = json::parse(msg); });
    provider.send(json{{"request", "load_source"}, {"kind", "instance"}, {"id_path", {0, 2}},
                       {"role", "origin"}, {"index", 0}}.dump());
    return reply["found"].get<bool>();
  };
  EXPECT_TRUE(ask({{"/synth/machine", sources}}));
  EXPECT_TRUE(ask({{"/synth/machine/", sources + "/"}}));
  EXPECT_FALSE(ask({{"/synth/mach", sources}}));                      // not a boundary
  EXPECT_TRUE(ask({{"/synth", "/nowhere"}, {"/synth/machine", sources}}));  // falls through
  EXPECT_FALSE(ask({}));
}
