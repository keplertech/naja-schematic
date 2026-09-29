#include "DiagnosisStore.h"

#include <gtest/gtest.h>

class DiagnosisStoreTest : public ::testing::Test {
  protected:
    void TearDown() override { DiagnosisStore::clear(); }
};

TEST_F(DiagnosisStoreTest, StartsEmpty) {
  EXPECT_TRUE(DiagnosisStore::all().empty());
}

TEST_F(DiagnosisStoreTest, SetDiagnosticsReplacesPreviousSet) {
  DiagnosisItem a;
  a.message = "first";
  DiagnosisStore::setDiagnostics({a});
  EXPECT_EQ(DiagnosisStore::all().size(), 1u);

  DiagnosisItem b;
  b.message = "second";
  DiagnosisStore::setDiagnostics({b});
  ASSERT_EQ(DiagnosisStore::all().size(), 1u);
  EXPECT_EQ(DiagnosisStore::all()[0].message, "second");
}

TEST_F(DiagnosisStoreTest, ClearEmptiesTheSet) {
  DiagnosisItem a;
  DiagnosisStore::setDiagnostics({a});
  DiagnosisStore::clear();
  EXPECT_TRUE(DiagnosisStore::all().empty());
}

TEST(DiagnosisStoreColor, MapsEachSeverityToADistinctColor) {
  ImU32 info    = DiagnosisStore::colorForSeverity(DiagnosisSeverity::Info);
  ImU32 warning = DiagnosisStore::colorForSeverity(DiagnosisSeverity::Warning);
  ImU32 error   = DiagnosisStore::colorForSeverity(DiagnosisSeverity::Error);

  EXPECT_NE(info, 0u);
  EXPECT_NE(warning, 0u);
  EXPECT_NE(error, 0u);
  EXPECT_NE(info, warning);
  EXPECT_NE(warning, error);
  EXPECT_NE(info, error);
}

// The diagnosis UI (tree/schematic tinting) is currently hidden behind
// DiagnosisStore.cpp's kDiagnosisUIHidden flag without removing the
// underlying data/plumbing (see naja-schematic's CLAUDE.md). While that
// flag is true, instanceDiagnostics/netDiagnostics/instanceColor/netColor
// return empty/0 regardless of what setDiagnostics() was given -- this test
// pins that current behavior so flipping the flag back on is a deliberate,
// visible change rather than a silent regression.
TEST_F(DiagnosisStoreTest, InstanceAndNetLookupsAreSuppressedWhileUIHidden) {
  DiagnosisItem instanceItem;
  instanceItem.kind = DiagnosisKind::Instance;
  instanceItem.path = {"u1"};
  instanceItem.severity = DiagnosisSeverity::Error;

  DiagnosisItem netItem;
  netItem.kind = DiagnosisKind::Net;
  netItem.path = {"u1"};
  netItem.terminal = "Q";
  netItem.severity = DiagnosisSeverity::Warning;

  DiagnosisStore::setDiagnostics({instanceItem, netItem});

  EXPECT_TRUE(DiagnosisStore::instanceDiagnostics(InstancePath{{1, "u1"}}).empty());
  EXPECT_TRUE(DiagnosisStore::netDiagnostics(InstancePath{{1, "u1"}}, "Q").empty());
  EXPECT_EQ(DiagnosisStore::instanceColor(InstancePath{{1, "u1"}}), 0u);
  EXPECT_EQ(DiagnosisStore::netColor(InstancePath{{1, "u1"}}, "Q"), 0u);
}

// --- Matching items to instances (the lookups behind the hidden UI) ---

namespace {
DiagnosisItem instanceItem(std::vector<std::string> path,
                           std::optional<std::vector<unsigned>> idPath,
                           const std::string& message) {
  DiagnosisItem d;
  d.path    = std::move(path);
  d.idPath  = std::move(idPath);
  d.message = message;
  return d;
}

std::vector<std::string> messages(const std::vector<const DiagnosisItem*>& items) {
  std::vector<std::string> out;
  for (const auto* i : items) out.push_back(i->message);
  return out;
}
} // namespace

TEST_F(DiagnosisStoreTest, IdPathSinglesOutOneOfSeveralAnonymousSiblings) {
  DiagnosisStore::setDiagnostics({
    instanceItem({"", ""}, std::vector<unsigned>{1, 0}, "nested anon"),
    instanceItem({""}, std::vector<unsigned>{0}, "anon 0"),
  });
  const InstancePath anon0{{0, ""}}, anon1{{1, ""}};
  EXPECT_EQ(messages(DiagnosisStore::findInstanceItems(anon0)), std::vector<std::string>{"anon 0"});
  EXPECT_TRUE(DiagnosisStore::findInstanceItems(anon1).empty());
  EXPECT_EQ(messages(DiagnosisStore::findInstanceItems(InstancePath{{1, ""}, {0, ""}})),
            std::vector<std::string>{"nested anon"});
  EXPECT_TRUE(DiagnosisStore::findInstanceItems(InstancePath{{0, ""}, {0, ""}}).empty());
}

TEST_F(DiagnosisStoreTest, NamePathItemsStillMatchNamedInstances) {
  DiagnosisStore::setDiagnostics({
    instanceItem({"a/b", "u2"}, std::nullopt, "by name"),
    instanceItem({""}, std::nullopt, "ambiguous"),
  });
  EXPECT_EQ(messages(DiagnosisStore::findInstanceItems(InstancePath{{2, "a/b"}, {5, "u2"}})),
            std::vector<std::string>{"by name"});
  // "a" + "b/u2" is not "a/b" + "u2".
  EXPECT_TRUE(DiagnosisStore::findInstanceItems(InstancePath{{1, "a"}, {5, "b/u2"}}).empty());
  // A name path through an anonymous level can't pick one of its siblings:
  // it matches none of them.
  EXPECT_TRUE(DiagnosisStore::findInstanceItems(InstancePath{{0, ""}}).empty());
  EXPECT_TRUE(DiagnosisStore::findInstanceItems(InstancePath{{1, ""}}).empty());
}

TEST_F(DiagnosisStoreTest, IdPathWinsOverAMismatchingNamePath) {
  // The id path is what identifies; the names are only a label.
  DiagnosisStore::setDiagnostics({instanceItem({"u_other"}, std::vector<unsigned>{3}, "by id")});
  EXPECT_EQ(messages(DiagnosisStore::findInstanceItems(InstancePath{{3, "u3"}})),
            std::vector<std::string>{"by id"});
  EXPECT_TRUE(DiagnosisStore::findInstanceItems(InstancePath{{9, "u_other"}}).empty());
}

TEST_F(DiagnosisStoreTest, NetItemsMatchByIdPathAndTerminal) {
  DiagnosisItem net;
  net.kind     = DiagnosisKind::Net;
  net.path     = {""};
  net.idPath   = std::vector<unsigned>{1};
  net.terminal = "Q";
  DiagnosisStore::setDiagnostics({net});
  EXPECT_EQ(DiagnosisStore::findNetItems(InstancePath{{1, ""}}, "Q").size(), 1u);
  EXPECT_TRUE(DiagnosisStore::findNetItems(InstancePath{{0, ""}}, "Q").empty());
  EXPECT_TRUE(DiagnosisStore::findNetItems(InstancePath{{1, ""}}, "D").empty());
}
