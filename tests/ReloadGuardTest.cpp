#include "ReloadGuard.h"

#include <gtest/gtest.h>

TEST(ReloadGuardTest, AcceptsEverythingOutsideAReload) {
  ReloadGuard guard;
  EXPECT_FALSE(guard.reloading());
  EXPECT_TRUE(guard.accept("root_response"));
  EXPECT_TRUE(guard.accept("terms_response"));
  EXPECT_TRUE(guard.accept("diagnosis_response"));
}

TEST(ReloadGuardTest, DropsRepliesForTheOldDesignUntilTheNewRoot) {
  ReloadGuard guard;
  guard.reloadStarted();
  EXPECT_TRUE(guard.reloading());
  // Replies to requests sent before the reset, and pushes the host re-sends
  // after the new root anyway.
  EXPECT_FALSE(guard.accept("terms_response"));
  EXPECT_FALSE(guard.accept("instances_response"));
  EXPECT_FALSE(guard.accept("diagnosis_response"));
  EXPECT_TRUE(guard.accept("root_response"));
  EXPECT_FALSE(guard.reloading());
  EXPECT_TRUE(guard.accept("diagnosis_response"));
}

TEST(ReloadGuardTest, OverlappingReloadsKeepOnlyTheLastRoot) {
  ReloadGuard guard;
  guard.reloadStarted();
  guard.reloadStarted();
  EXPECT_FALSE(guard.accept("root_response"));      // answers the first load_root
  EXPECT_FALSE(guard.accept("diagnosis_response")); // re-pushed after it
  EXPECT_TRUE(guard.accept("root_response"));
  EXPECT_TRUE(guard.accept("diagnosis_response"));
}

TEST(ReloadGuardTest, RootLoadedCountsAsARoot) {
  ReloadGuard guard;
  guard.reloadStarted();
  EXPECT_TRUE(guard.accept("root_loaded"));
  EXPECT_FALSE(guard.reloading());
}
