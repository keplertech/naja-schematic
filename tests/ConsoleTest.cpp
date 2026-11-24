// The in-app log (Console's kept lines) and how the Log tab formats them.

#include "Console.h"
#include "LogView.h"

#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct ConsoleTest : ::testing::Test {
  void SetUp() override { Console::clear(); }
  void TearDown() override {
    Console::setVerbose(true);
    Console::clear();
  }
};

}  // namespace

TEST_F(ConsoleTest, KeepsEveryLevelWithItsSource) {
  Console::Log("hello");
  Console::Warn("careful");
  Console::Error("broken");
  Console::record(LogLevel::Warning, "naja", "from the loader");
  const auto e = Console::entries();
  ASSERT_EQ(e.size(), 4u);
  EXPECT_EQ(e[0].level, LogLevel::Info);
  EXPECT_EQ(e[0].text, "hello");
  EXPECT_EQ(e[0].source, "");
  EXPECT_EQ(e[1].level, LogLevel::Warning);
  EXPECT_EQ(e[2].level, LogLevel::Error);
  EXPECT_EQ(e[3].source, "naja");
  EXPECT_EQ(Console::errorCount(), 1u);
}

// One entry per line (the view's rows are one line high), a trailing
// newline making no empty line, but an error counted once.
TEST_F(ConsoleTest, SplitsMultiLineMessages) {
  Console::Error("bad.sv:2:17: error: expected expression\n  assign y = a &;\n\n                ^\n");
  const auto e = Console::entries();
  ASSERT_EQ(e.size(), 4u);
  EXPECT_EQ(e[0].text, "bad.sv:2:17: error: expected expression");
  EXPECT_EQ(e[1].text, "  assign y = a &;");
  EXPECT_EQ(e[2].text, "");
  EXPECT_EQ(e[3].text, "                ^");
  for (const auto& entry : e) EXPECT_EQ(entry.level, LogLevel::Error);
  EXPECT_EQ(Console::errorCount(), 1u);
}

TEST_F(ConsoleTest, KeepsOnlyTheLastLines) {
  for (size_t i = 0; i < Console::kMaxEntries + 10; ++i) Console::Log(std::to_string(i));
  const auto e = Console::entries();
  ASSERT_EQ(e.size(), Console::kMaxEntries);
  EXPECT_EQ(e.front().text, "10");
  EXPECT_EQ(e.back().text, std::to_string(Console::kMaxEntries + 9));
}

TEST_F(ConsoleTest, ClearEmptiesAndBumpsTheRevision) {
  Console::Error("x");
  const uint64_t before = Console::revision();
  Console::clear();
  EXPECT_TRUE(Console::entries().empty());
  EXPECT_EQ(Console::errorCount(), 0u);
  EXPECT_NE(Console::revision(), before);
}

// The headless renderer's quiet mode: info dropped, problems kept.
TEST_F(ConsoleTest, QuietKeepsOnlyProblems) {
  Console::setVerbose(false);
  Console::Log("chatter");
  Console::record(LogLevel::Info, "naja", "more chatter");
  Console::Warn("careful");
  const auto e = Console::entries();
  ASSERT_EQ(e.size(), 1u);
  EXPECT_EQ(e[0].text, "careful");
}

// A design loads on a worker thread while the UI reads the log.
TEST_F(ConsoleTest, ThreadsLogConcurrently) {
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([] { for (int i = 0; i < 500; ++i) Console::record(LogLevel::Info, "naja", "x"); });
  for (int i = 0; i < 100; ++i) (void)Console::entries();
  for (auto& th : threads) th.join();
  EXPECT_EQ(Console::entries().size(), 2000u);
}

TEST_F(ConsoleTest, LogViewFormatsTimeSourceAndText) {
  LogEntry e;
  e.source = "naja";
  e.text = "loaded";
  e.time = 0;
  const std::string line = LogView::format(e);
  // "HH:MM:SS [naja] loaded", in local time.
  ASSERT_EQ(line.size(), std::string("00:00:00 [naja] loaded").size());
  EXPECT_EQ(line.substr(8), " [naja] loaded");
  e.source.clear();
  EXPECT_EQ(LogView::format(e).substr(8), " loaded");
}

TEST_F(ConsoleTest, UnseenErrorsCountSinceTheTabWasShown) {
  const size_t before = LogView::unseenErrors();
  Console::Error("one");
  Console::Error("two");
  EXPECT_EQ(LogView::unseenErrors(), before + 2);
  Console::clear();
  EXPECT_EQ(LogView::unseenErrors(), 0u);
}
