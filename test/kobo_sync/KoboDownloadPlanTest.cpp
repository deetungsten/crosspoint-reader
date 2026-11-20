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

TEST(KoboDownloadPlan, ContentSuffixKeepsExtension) {
  EXPECT_EQ(kobo::withContentSuffix("/Books/Author - Title.epub", "12345678-aaaa-bbbb-cccc-ddddeeeeffff"),
            "/Books/Author - Title [12345678].epub");
}

TEST(KoboDownloadPlan, ContentSuffixWithoutExtensionIsAppended) {
  EXPECT_EQ(kobo::withContentSuffix("/Books/Title", "12345678-aaaa"), "/Books/Title [12345678]");
}

TEST(KoboDownloadPlan, ContentSuffixUsesFirstEightContentIdChars) {
  EXPECT_EQ(kobo::withContentSuffix("a.b.epub", "short"), "a.b [short].epub");
}
