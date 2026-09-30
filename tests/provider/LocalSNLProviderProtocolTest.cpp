// LocalSNLProvider against the wire-protocol cases shared with protocol.py
// (tests/data/anonymous_protocol_cases.json, also run by
// python/tests/test_anonymous_instances.py): both backends must give the
// same answers, notably for anonymous instances addressed by id_path.
#include "LocalSNLProvider.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include "NLUniverse.h"
#include "NLDB.h"
#include "NLLibrary.h"
#include "SNLDesign.h"
#include "SNLInstance.h"
#include "SNLInstTerm.h"
#include "SNLScalarNet.h"
#include "SNLScalarTerm.h"

using json = nlohmann::json;
using namespace naja::NL;

namespace {

json loadSpec() {
  std::ifstream in(NAJA_SCHEMATIC_PROTOCOL_CASES);
  return json::parse(in);
}

SNLTerm::Direction direction(const std::string& d) {
  if (d == "output") return SNLTerm::Direction::Output;
  if (d == "inout")  return SNLTerm::Direction::InOut;
  return SNLTerm::Direction::Input;
}

// Builds the spec'd netlist (same as build_design() on the Python side):
// instances in list order, so their ids are their list indices.
std::map<std::string, SNLDesign*> buildDesign(NLDB* db, const json& spec) {
  std::map<std::string, SNLDesign*> designs;
  auto* prims = NLLibrary::create(db, NLLibrary::Type::Primitives, NLName("anon_prims"));
  for (const auto& p : spec["primitives"]) {
    auto* d = SNLDesign::create(prims, SNLDesign::Type::Primitive, NLName(p["name"].get<std::string>()));
    for (const auto& t : p["terms"])
      SNLScalarTerm::create(d, direction(t[1]), NLName(t[0].get<std::string>()));
    designs[p["name"]] = d;
  }
  auto* lib = NLLibrary::create(db, NLName("anon_work"));
  for (const auto& s : spec["designs"]) {
    auto* d = SNLDesign::create(lib, NLName(s["name"].get<std::string>()));
    for (const auto& t : s["terms"])
      SNLScalarTerm::create(d, direction(t[1]), NLName(t[0].get<std::string>()));
    std::vector<SNLInstance*> instances;
    for (const auto& i : s["instances"]) {
      const std::string name = i[0];
      auto* inst = SNLInstance::create(d, designs.at(i[1]), name.empty() ? NLName() : NLName(name));
      EXPECT_EQ(inst->getID(), instances.size());
      instances.push_back(inst);
    }
    for (const auto& n : s["nets"]) {
      auto* net = SNLScalarNet::create(d, NLName(n["name"].get<std::string>()));
      d->getScalarTerm(NLName(n["term"].get<std::string>()))->setNet(net);
      for (const auto& pin : n["pins"]) {
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

class ProviderProtocol : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    spec_ = new json(loadSpec());
    if (!NLUniverse::get()) NLUniverse::create();
    auto* db = NLDB::create(NLUniverse::get());
    designs_ = new std::map<std::string, SNLDesign*>(buildDesign(db, (*spec_)["design"]));
    auto* top = designs_->at((*spec_)["design"]["top"]);
    db->setTopDesign(top);
    NLUniverse::get()->setTopDesign(top);
    provider_ = new LocalSNLProvider();
    provider_->setDB(db);
    provider_->on_message([](const std::string& msg) { replies_.push_back(json::parse(msg)); });
  }

  json ask(const json& request) {
    replies_ = json::array();
    provider_->send(request.dump());
    return replies_;
  }

  static json*                                spec_;
  static std::map<std::string, SNLDesign*>*   designs_;
  static LocalSNLProvider*                    provider_;
  static json                                 replies_;
};

json*                              ProviderProtocol::spec_     = nullptr;
std::map<std::string, SNLDesign*>* ProviderProtocol::designs_  = nullptr;
LocalSNLProvider*                  ProviderProtocol::provider_ = nullptr;
json                               ProviderProtocol::replies_  = json::array();

} // namespace

TEST_F(ProviderProtocol, AnswersEverySharedCaseLikeProtocolPy) {
  ASSERT_FALSE((*spec_)["cases"].empty());
  for (const auto& c : (*spec_)["cases"]) {
    SCOPED_TRACE(c["name"].get<std::string>());
    json replies = ask(substitute(c["request"], *designs_));
    json expect  = c["expect"];
    for (const auto& key : c.value("unordered", json::array())) {
      ASSERT_FALSE(replies.empty());
      json expectedList = expect[0][key.get<std::string>()];
      expect[0].erase(key.get<std::string>());
      EXPECT_EQ(matchesUnordered(expectedList, replies[0].value(key.get<std::string>(), json()),
                                 "replies[0]." + key.get<std::string>()), "");
    }
    EXPECT_EQ(matches(expect, replies, "replies"), "");
    for (const auto& key : c.value("absent", json::array())) {
      ASSERT_FALSE(replies.empty());
      EXPECT_FALSE(replies[0].contains(key.get<std::string>())) << key << " in " << replies[0].dump();
    }
  }
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
