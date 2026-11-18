#include <gtest/gtest.h>

#include "KoboSyncToken.h"

namespace {

constexpr double BOOKS_CREATED = 1700000000.0;
constexpr double BOOKS_MODIFIED = 1700000100.5;
constexpr double ARCHIVE_MODIFIED = 1700000200.0;
constexpr double READING_STATE_MODIFIED = 1700000300.25;
constexpr double TAGS_MODIFIED = 1700000400.0;

TEST(KoboSyncTokenTest, BuildAndParseRoundTrip) {
  KoboSyncToken token;
  token.rawKoboStoreToken = "store-token";
  token.booksLastCreated = BOOKS_CREATED;
  token.booksLastModified = BOOKS_MODIFIED;
  token.archiveLastModified = ARCHIVE_MODIFIED;
  token.readingStateLastModified = READING_STATE_MODIFIED;
  token.tagsLastModified = TAGS_MODIFIED;

  const std::string encoded = token.build();
  ASSERT_FALSE(encoded.empty());
  EXPECT_EQ(encoded.find('.'), std::string::npos);

  const KoboSyncToken parsed = KoboSyncToken::fromHeader(encoded);
  ASSERT_TRUE(parsed.decoded);
  EXPECT_TRUE(parsed.valid());
  EXPECT_EQ(parsed.encoded, encoded);
  EXPECT_EQ(parsed.rawKoboStoreToken, "store-token");
  EXPECT_DOUBLE_EQ(parsed.booksLastCreated, BOOKS_CREATED);
  EXPECT_DOUBLE_EQ(parsed.booksLastModified, BOOKS_MODIFIED);
  EXPECT_DOUBLE_EQ(parsed.archiveLastModified, ARCHIVE_MODIFIED);
  EXPECT_DOUBLE_EQ(parsed.readingStateLastModified, READING_STATE_MODIFIED);
  EXPECT_DOUBLE_EQ(parsed.tagsLastModified, TAGS_MODIFIED);
}

TEST(KoboSyncTokenTest, EmptyAndMalformedTokensAreInvalid) {
  EXPECT_TRUE(KoboSyncToken::fromHeader("").empty());
  EXPECT_FALSE(KoboSyncToken::fromHeader("not base64!").valid());
  EXPECT_FALSE(KoboSyncToken::fromHeader("e30=").valid());  // {}
  EXPECT_FALSE(KoboSyncToken::fromHeader(std::string(3000, 'A')).valid());
}

TEST(KoboSyncTokenTest, OfficialStoreTokenIsReplayedVerbatim) {
  const std::string storeToken = "Zm9v.YmFy";
  const KoboSyncToken token = KoboSyncToken::fromHeader(storeToken);
  EXPECT_FALSE(token.decoded);
  EXPECT_TRUE(token.valid());
  EXPECT_EQ(token.encoded, storeToken);
}

TEST(KoboSyncTokenTest, ParsesCalibreWebTokenWithFloatTimestamps) {
  // {"version": "1-1-0", "data": {...}} with fractional epochs as produced by
  // SyncToken.to_epoch_timestamp().
  const std::string json =
      "{\"version\": \"1-1-0\", \"data\": {\"raw_kobo_store_token\": \"\", "
      "\"books_last_modified\": 1700000100.5, \"books_last_created\": 1700000000, "
      "\"archive_last_modified\": 0, \"reading_state_last_modified\": 1700000300, "
      "\"tags_last_modified\": 1700000400}}";
  const KoboSyncToken parsed = KoboSyncToken::fromHeader(kobo_base64::encode(json));
  ASSERT_TRUE(parsed.decoded);
  EXPECT_DOUBLE_EQ(parsed.booksLastModified, 1700000100.5);
  EXPECT_DOUBLE_EQ(parsed.booksLastCreated, 1700000000);
  EXPECT_DOUBLE_EQ(parsed.readingStateLastModified, 1700000300);
}

TEST(KoboSyncTokenTest, RejectsTokenWithoutRequiredFields) {
  const std::string json = "{\"version\": \"1-1-0\", \"data\": {\"books_last_modified\": 1}}";
  const KoboSyncToken parsed = KoboSyncToken::fromHeader(kobo_base64::encode(json));
  EXPECT_FALSE(parsed.decoded);
  EXPECT_FALSE(parsed.valid());
}

}  // namespace
