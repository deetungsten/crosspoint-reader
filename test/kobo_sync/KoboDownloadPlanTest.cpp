#include <gtest/gtest.h>

#include "KoboDownloadPlan.h"

TEST(KoboDownloadPlan, RemovedBookIsSkipped) {
  EXPECT_FALSE(kobo::needsDownload(true, false, false));
  EXPECT_FALSE(kobo::needsDownload(true, true, true));
}

TEST(KoboDownloadPlan, DownloadedBookWithFileIsSkipped) {
  EXPECT_FALSE(kobo::needsDownload(false, true, true));
}

TEST(KoboDownloadPlan, DownloadedBookWithMissingFileIsFetchedAgain) {
  EXPECT_TRUE(kobo::needsDownload(false, true, false));
}

TEST(KoboDownloadPlan, BookWithoutLocalPathIsFetched) {
  EXPECT_TRUE(kobo::needsDownload(false, false, false));
  EXPECT_TRUE(kobo::needsDownload(false, false, true));
}
