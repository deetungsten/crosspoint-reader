#include <gtest/gtest.h>

#include "KoboSyncTypes.h"

// The close-time queue and the conflict activity must derive the uploaded
// status from the same threshold, or the server flips Finished/Reading on
// every open/close cycle.
TEST(KoboProgressStatus, FinishedThresholdMatchesUploadPath) {
  EXPECT_EQ(kobo::readStatusFor(0), kobo::ReadStatus::Unread);
  EXPECT_EQ(kobo::readStatusFor(1), kobo::ReadStatus::Reading);
  EXPECT_EQ(kobo::readStatusFor(9949), kobo::ReadStatus::Reading);
  EXPECT_EQ(kobo::readStatusFor(9950), kobo::ReadStatus::Finished);
  EXPECT_EQ(kobo::readStatusFor(10000), kobo::ReadStatus::Finished);
}

// The remote and local whole-book percentages must only count as a conflict
// when they differ by more than the tolerance; anything else is rounding
// noise between layout engines and must not rewrite the server state.
TEST(KoboProgressStatus, ConflictDecisionUsesTolerance) {
  const uint16_t tol = 50;
  EXPECT_EQ(kobo::syncActionFor(4700, 4700, tol), kobo::SyncAction::AlreadySynced);
  EXPECT_EQ(kobo::syncActionFor(4749, 4700, tol), kobo::SyncAction::AlreadySynced);
  EXPECT_EQ(kobo::syncActionFor(4751, 4700, tol), kobo::SyncAction::ApplyRemote);
  EXPECT_EQ(kobo::syncActionFor(4700, 4751, tol), kobo::SyncAction::UploadLocal);
  EXPECT_EQ(kobo::syncActionFor(10000, 9800, tol), kobo::SyncAction::ApplyRemote);
  EXPECT_EQ(kobo::syncActionFor(9800, 10000, tol), kobo::SyncAction::UploadLocal);
}
