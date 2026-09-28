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

  EXPECT_TRUE(DiagnosisStore::instanceDiagnostics(InstancePath{"u1"}).empty());
  EXPECT_TRUE(DiagnosisStore::netDiagnostics(InstancePath{"u1"}, "Q").empty());
  EXPECT_EQ(DiagnosisStore::instanceColor(InstancePath{"u1"}), 0u);
  EXPECT_EQ(DiagnosisStore::netColor(InstancePath{"u1"}, "Q"), 0u);
}
