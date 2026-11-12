// Headless ImGui test of the Add Comment dialog (same harness as
// EquipotentialViewTest): what it sends to the host.
#include "ReviewStore.h"
#include "ReviewView.h"
#include "SelectionStore.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include "FakeNetlistProvider.h"

namespace {

class CommentDialog : public ::testing::Test {
 protected:
  void SetUp() override {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1200.f, 800.f);
    io.DeltaTime   = 1.f / 60.f;
    io.IniFilename = nullptr;
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
  }
  void TearDown() override {
    ReviewStore::clear();
    ImGui::DestroyContext();
  }
  void frame(int n = 1) {
    for (int i = 0; i < n; ++i) {
      ImGui::NewFrame();
      ReviewView::renderComposer(&provider);
      ImGui::Render();
    }
  }
  void click(ImVec2 at) {
    ImGui::GetIO().AddMousePosEvent(at.x, at.y);
    frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
  }
  FakeNetlistProvider provider;
};

TEST_F(CommentDialog, SendsTheThreadAnchoredByIds) {
  const json anchor = ReviewStore::termAnchor({{0, "u_sub"}}, 2, "a", std::nullopt);
  ReviewView::openComposer(anchor, "u_sub/a");
  frame(2);
  ASSERT_TRUE(ReviewView::commentButtonForTesting());
  // The text box has the keyboard focus when the dialog opens.
  ImGui::GetIO().AddInputCharactersUTF8("glitch here");
  frame();
  click(*ReviewView::commentButtonForTesting());

  ASSERT_EQ(provider.sent.size(), 1u);
  json req = json::parse(provider.sent[0]);
  EXPECT_EQ(req["request"], "review_add_thread");
  EXPECT_EQ(req["anchor"], anchor);
  EXPECT_EQ(req["text"], "glitch here");
  EXPECT_EQ(req["severity"], "info");
  frame();
  EXPECT_FALSE(ReviewView::commentButtonForTesting());  // closed
}

TEST_F(CommentDialog, SendsNothingWithoutText) {
  ReviewView::openComposer(ReviewStore::instanceAnchor({}), "the top design");
  frame(2);
  ASSERT_TRUE(ReviewView::commentButtonForTesting());
  click(*ReviewView::commentButtonForTesting());  // disabled
  EXPECT_TRUE(provider.sent.empty());
}

TEST(ReviewGoTo, AnExactThreadSelectsItsInstanceAndAsksToRevealItsObject) {
  ReviewStore::setResponse(json::parse(R"({"response": "review_response", "threads": [
    {"id": "t1", "anchor": {"kind": "term", "id_path": [3], "path": ["u3"], "term_id": 5, "bit": 2},
     "resolution": {"state": "exact", "id_path": [3], "path": ["u3_now"]}, "comments": []},
    {"id": "t2", "anchor": {"kind": "term", "id_path": [3], "term_id": 5},
     "resolution": {"state": "other_snapshot"}, "comments": []}]})"));
  SelectionStore::clear();
  const auto& threads = ReviewStore::threads();

  ReviewView::goTo(threads[1]);  // on another netlist: nowhere to go
  EXPECT_FALSE(ReviewView::takeGoTo());
  EXPECT_FALSE(SelectionStore::hasSelection());

  ReviewView::goTo(threads[0]);
  EXPECT_TRUE(SelectionStore::isSelected({{3, "u3_now"}}));
  auto go = ReviewView::takeGoTo();
  ASSERT_TRUE(go);
  EXPECT_EQ(go->path, (InstancePath{{3, "u3_now"}}));
  EXPECT_EQ(go->anchor.kind, ReviewAnchorKind::Term);
  EXPECT_EQ(go->anchor.termId, 5u);
  EXPECT_EQ(go->anchor.bit, 2);
  EXPECT_FALSE(ReviewView::takeGoTo());  // once

  SelectionStore::clear();
  ReviewStore::clear();
}

}  // namespace
