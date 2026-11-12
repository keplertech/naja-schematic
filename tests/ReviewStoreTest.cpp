#include "DiagnosisStore.h"
#include "ReviewStore.h"

#include <gtest/gtest.h>

namespace {

// What python/naja_schematic/review.py pushes (docs/review_format.md): an
// instance thread, a term thread on bus bit d[1], a net thread on an
// anonymous net, a resolved one, and one written on another netlist.
const char* kResponse = R"({
  "response": "review_response", "author": "Jane", "writable": true,
  "snapshot": "aaaa", "views": [],
  "threads": [
    {"id": "t1", "title": "", "author": "Jane", "source": "human", "created": "2026-10-31T14:25:30.123Z",
     "snapshot": "aaaa", "snapshot_stored": true, "view": null,
     "anchor": {"kind": "instance", "id_path": [0, 0], "path": ["u_sub", "u_and"]},
     "resolution": {"state": "exact", "id_path": [0, 0], "path": ["u_sub", "u_and"]},
     "status": "open", "severity": "error", "labels": ["lut"],
     "comments": [{"id": "e1", "author": "Jane", "date": "2026-10-31T14:25:30.123Z", "text": "LUT looks wrong\nsee INIT"},
                  {"id": "e2", "author": "John", "date": "2026-10-31T14:26:00.000Z", "text": "Agreed"}]},
    {"id": "t2", "author": "kepler-formal", "source": "kepler-formal", "snapshot": "aaaa",
     "anchor": {"kind": "term", "id_path": [], "path": [], "term_id": 2, "terminal": "d", "bit": 1},
     "resolution": {"state": "exact", "id_path": [], "path": []},
     "status": "open", "severity": "warning", "comments": []},
    {"id": "t3", "author": "Jane", "snapshot": "aaaa",
     "anchor": {"kind": "net", "id_path": [0], "path": ["u_sub"], "net_id": 5, "net": ""},
     "resolution": {"state": "exact", "id_path": [0], "path": ["u_sub"]},
     "status": "resolved", "severity": "error", "comments": []},
    {"id": "t4", "author": "Jane", "snapshot": "bbbb", "snapshot_stored": false,
     "anchor": {"kind": "instance", "id_path": [0, 0], "path": ["u_sub", "u_and"]},
     "resolution": {"state": "other_snapshot"},
     "status": "open", "severity": "error", "comments": []}
  ]})";

InstancePath uAnd() { return {{0, "u_sub"}, {0, "u_and"}}; }

class Review : public ::testing::Test {
 protected:
  void SetUp() override { ReviewStore::setResponse(json::parse(kResponse)); }
  void TearDown() override { ReviewStore::clear(); }
};

TEST_F(Review, ParsesTheHostsResponse) {
  EXPECT_TRUE(ReviewStore::hasReview());
  EXPECT_TRUE(ReviewStore::writable());
  EXPECT_EQ(ReviewStore::author(), "Jane");
  const auto& threads = ReviewStore::threads();
  ASSERT_EQ(threads.size(), 4u);
  const auto& t1 = threads[0];
  EXPECT_EQ(t1.state, ReviewState::Exact);
  EXPECT_EQ(t1.severity, DiagnosisSeverity::Error);
  EXPECT_EQ(t1.labels, std::vector<std::string>{"lut"});
  ASSERT_EQ(t1.comments.size(), 2u);
  EXPECT_EQ(t1.comments[1].author, "John");
  ASSERT_TRUE(t1.resolvedPath);
  EXPECT_EQ(*t1.resolvedPath, uAnd());
  EXPECT_EQ(threads[1].anchor.termId, 2u);
  EXPECT_EQ(threads[1].anchor.bit, 1);
  EXPECT_EQ(threads[3].state, ReviewState::OtherSnapshot);
  EXPECT_FALSE(threads[3].resolvedPath);
}

TEST_F(Review, MatchesObjectsByIdsOnlyAndOnlyExactThreads) {
  // t4 has the same ids but was written on another netlist: never matched.
  auto onAnd = ReviewStore::instanceThreads(uAnd());
  ASSERT_EQ(onAnd.size(), 1u);
  EXPECT_EQ(onAnd[0]->id, "t1");
  // Names don't matter, ids do.
  EXPECT_EQ(ReviewStore::instanceThreads({{0, "renamed"}, {0, "x"}}).size(), 1u);
  EXPECT_TRUE(ReviewStore::instanceThreads({{0, "u_sub"}, {1, "u_and"}}).empty());

  EXPECT_EQ(ReviewStore::termThreads({}, 2, 1).size(), 1u);
  EXPECT_TRUE(ReviewStore::termThreads({}, 2, 0).empty());
  EXPECT_EQ(ReviewStore::termThreads({}, 2).size(), 1u);  // the bus row: any bit
  EXPECT_TRUE(ReviewStore::termThreads({}, 3).empty());

  EXPECT_EQ(ReviewStore::netThreads({{0, "u_sub"}}, 5).size(), 1u);
  EXPECT_TRUE(ReviewStore::netThreads({}, 5).empty());
}

TEST_F(Review, APinShowsThreadsOnItsBitOrOnTheWholeTerm) {
  // t2 is on d[1]: shown on that bit's pin and on the merged bus pin.
  EXPECT_EQ(ReviewStore::pinThreads({}, 2, 1).size(), 1u);
  EXPECT_TRUE(ReviewStore::pinThreads({}, 2, 0).empty());
  EXPECT_EQ(ReviewStore::pinThreads({}, 2, std::nullopt).size(), 1u);
  EXPECT_TRUE(ReviewStore::pinThreads({0}, 2, 1).empty());

  // A thread on the whole bus is on every bit's pin.
  json response = json::parse(kResponse);
  response["threads"][1]["anchor"].erase("bit");
  ReviewStore::setResponse(response);
  EXPECT_EQ(ReviewStore::pinThreads({}, 2, 0).size(), 1u);
  EXPECT_EQ(ReviewStore::pinThreads({}, 2, 1).size(), 1u);
}

TEST_F(Review, BadgesCountOpenThreadsInTheWorstSeverityColor) {
  auto net = ReviewStore::netThreads({{0, "u_sub"}}, 5);
  EXPECT_EQ(ReviewStore::openCount(net), 0u);  // resolved
  EXPECT_EQ(ReviewStore::openColor(net), 0u);
  auto bus = ReviewStore::termThreads({}, 2);
  EXPECT_EQ(ReviewStore::openCount(bus), 1u);
  EXPECT_EQ(ReviewStore::openColor(bus), DiagnosisStore::colorForSeverity(DiagnosisSeverity::Warning));
}

TEST_F(Review, LabelsNameTheObjectForPeople) {
  const auto& threads = ReviewStore::threads();
  EXPECT_EQ(anchorLabel(threads[0].anchor), "u_sub/u_and");
  EXPECT_EQ(anchorLabel(threads[1].anchor), "d[1]");
  EXPECT_EQ(anchorLabel(threads[2].anchor), "u_sub/<net #5>");
}

TEST_F(Review, BuildsRequestsWithIdsAndLabels) {
  json anchor = ReviewStore::termAnchor({{3, "u1"}, {7, ""}}, 4, "Q", 2);
  EXPECT_EQ(anchor, json::parse(R"({"kind": "term", "id_path": [3, 7], "path": ["u1", ""],
                                    "term_id": 4, "terminal": "Q", "bit": 2})"));
  EXPECT_EQ(ReviewStore::netAnchor({}, 9, "", std::nullopt),
            json::parse(R"({"kind": "net", "id_path": [], "path": [], "net_id": 9, "net": ""})"));
  json req = ReviewStore::addThreadRequest(ReviewStore::instanceAnchor(uAnd()), "look",
                                           DiagnosisSeverity::Warning, "");
  EXPECT_EQ(req["request"], "review_add_thread");
  EXPECT_EQ(req["severity"], "warning");
  EXPECT_FALSE(req.contains("title"));
  EXPECT_EQ(req["anchor"]["id_path"], json::parse("[0, 0]"));
  EXPECT_EQ(ReviewStore::addEventRequest("t1", "", "resolved"),
            json::parse(R"({"request": "review_add_event", "thread": "t1", "status": "resolved"})"));
}

TEST_F(Review, ClearedForAFreshDesignAndReadOnlyWithoutAuthor) {
  ReviewStore::clear();
  EXPECT_FALSE(ReviewStore::hasReview());
  EXPECT_TRUE(ReviewStore::threads().empty());
  ReviewStore::setResponse(json::parse(R"({"response": "review_response", "writable": false,
                                           "threads": [{"id": "x", "anchor": 3}]})"));
  EXPECT_TRUE(ReviewStore::hasReview());
  EXPECT_FALSE(ReviewStore::writable());
  ASSERT_EQ(ReviewStore::threads().size(), 1u);  // malformed parts are defaulted
  EXPECT_EQ(ReviewStore::threads()[0].state, ReviewState::Unresolved);
}

}  // namespace
