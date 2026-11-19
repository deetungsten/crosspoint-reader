#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "KoboFeedParser.h"
#include "KoboSyncTime.h"

namespace {

struct Collected {
  std::vector<kobo::FeedEntry> entries;
};

void collect(void* ctx, const kobo::FeedEntry& entry) { static_cast<Collected*>(ctx)->entries.push_back(entry); }

std::string readFixture() {
  std::ifstream file(KOBO_FIXTURE_PATH, std::ios::binary);
  EXPECT_TRUE(file.good()) << "cannot open " << KOBO_FIXTURE_PATH;
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::vector<kobo::FeedEntry> parse(const std::string& json, const size_t chunk = 0) {
  Collected collected;
  KoboFeedParser parser(collect, &collected);
  if (chunk == 0) {
    parser.feed(json.data(), json.size());
  } else {
    for (size_t offset = 0; offset < json.size(); offset += chunk) {
      const size_t len = std::min(chunk, json.size() - offset);
      parser.feed(json.data() + offset, len);
    }
  }
  EXPECT_FALSE(parser.hasError());
  EXPECT_TRUE(parser.finished());
  return collected.entries;
}

const kobo::FeedEntry* findEntry(const std::vector<kobo::FeedEntry>& entries, const char* contentId,
                                 const kobo::EntryKind kind) {
  for (const auto& entry : entries) {
    if (entry.kind == kind && strcmp(entry.contentId, contentId) == 0) return &entry;
  }
  return nullptr;
}

TEST(KoboFeedParserTest, ParsesCalibreWebFeed) {
  const std::string json = readFixture();
  ASSERT_FALSE(json.empty());
  const auto entries = parse(json);

  // One book, one changed reading state, and two tag items.
  ASSERT_EQ(entries.size(), 4u);

  const auto* book = findEntry(entries, "11111111-1111-4111-8111-111111111111", kobo::EntryKind::Book);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->kind, kobo::EntryKind::Book);
  EXPECT_TRUE(book->isNew);
  EXPECT_STREQ(book->title, "Notes on the Analytical Engine");
  EXPECT_STREQ(book->author, "Ada Lovelace, Grace Hopper");
  EXPECT_STREQ(book->series, "Computing");
  EXPECT_STREQ(book->downloadUrl, "http://192.168.1.50:8083/kobo/token/download/1/epub");
  EXPECT_STREQ(book->downloadFormat, "EPUB3");
  EXPECT_EQ(book->downloadSize, 2345u);
  EXPECT_EQ(book->progressBp, 4250);
  EXPECT_EQ(book->readStatus, kobo::ReadStatus::Reading);
  EXPECT_EQ(book->readingStateLastModified, 1704277230u);
  EXPECT_STREQ(book->locationValue, "chapter3");
  EXPECT_STREQ(book->locationType, "KoboToc");

  const auto* state = findEntry(entries, "22222222-2222-4222-8222-222222222222", kobo::EntryKind::ReadingState);
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->kind, kobo::EntryKind::ReadingState);
  EXPECT_EQ(state->progressBp, 10000);
  EXPECT_EQ(state->readStatus, kobo::ReadStatus::Finished);

  const auto* firstTag = findEntry(entries, "11111111-1111-4111-8111-111111111111", kobo::EntryKind::Tag);
  ASSERT_NE(firstTag, nullptr);
  EXPECT_STREQ(firstTag->tagName, "Favourites");
  EXPECT_EQ(firstTag->kind, kobo::EntryKind::Tag);
  EXPECT_STREQ(firstTag->tagId, "33333333-3333-4333-8333-333333333333");
}

TEST(KoboFeedParserTest, NormalizesTypographicPunctuationInCatalogFields) {
  const std::string json = R"([{"NewEntitlement":{"BookEntitlement":{"Id":"11111111-1111-4111-8111-111111111111"},"BookMetadata":{"Title":"Death\u2019s End \u2018x\u2019 \u201A \u201B \u201Cq\u201D \u201E\u201F \u2039y\u203A \u2013 \u2014 \u2015 \u2026\u00A020\u00B0","Contributors":["O\u2019Malley"],"Series":{"Name":"A\u2013B"}}}},{"NewTag":{"Tag":{"Id":"33333333-3333-4333-8333-333333333333","Name":"F\u2019avourites","Items":[{"RevisionId":"11111111-1111-4111-8111-111111111111"}]}}}])";
  const auto entries = parse(json);

  const auto* book = findEntry(entries, "11111111-1111-4111-8111-111111111111", kobo::EntryKind::Book);
  ASSERT_NE(book, nullptr);
  EXPECT_STREQ(book->title, "Death's End 'x' ' ' \"q\" \"\" 'y' - - - ... 20\xC2\xB0");
  EXPECT_STREQ(book->author, "O'Malley");
  EXPECT_STREQ(book->series, "A-B");

  const auto* tag = findEntry(entries, "11111111-1111-4111-8111-111111111111", kobo::EntryKind::Tag);
  ASSERT_NE(tag, nullptr);
  EXPECT_STREQ(tag->tagName, "F'avourites");
}

TEST(KoboFeedParserTest, NormalizesCp1252PunctuationBytes) {
  std::string json =
      "[{\"NewEntitlement\":{\"BookEntitlement\":{\"Id\":\"11111111-1111-4111-8111-111111111111\"},"
      "\"BookMetadata\":{\"Title\":\"Death";
  json += '\x92';
  json += "s \x93quoted\x94 \x96";
  json += "dash\x97 \x85";
  json += "end\"}}}]";
  const auto entries = parse(json);

  const auto* book = findEntry(entries, "11111111-1111-4111-8111-111111111111", kobo::EntryKind::Book);
  ASSERT_NE(book, nullptr);
  EXPECT_STREQ(book->title, "Death's \"quoted\" -dash- ...end");
}

TEST(KoboFeedParserTest, NormalizesExtendedCp1252QuotesAndNbsp) {
  std::string json =
      "[{\"NewEntitlement\":{\"BookEntitlement\":{\"Id\":\"11111111-1111-4111-8111-111111111111\"},"
      "\"BookMetadata\":{\"Title\":\"";
  json += '\x84';
  json += "Die";
  json += '\x93';
  json += " ";
  json += '\x82';
  json += "single";
  json += '\x92';
  json += " ";
  json += '\x8B';
  json += "angle";
  json += '\x9B';
  json += " ";
  json += '\xA0';
  json += "nbsp\"}}}]";
  const auto entries = parse(json);

  const auto* book = findEntry(entries, "11111111-1111-4111-8111-111111111111", kobo::EntryKind::Book);
  ASSERT_NE(book, nullptr);
  EXPECT_STREQ(book->title, "\"Die\" 'single' 'angle'  nbsp");
}

TEST(KoboFeedParserTest, ChunkBoundariesDoNotChangeResult) {
  const std::string json = readFixture();
  const auto whole = parse(json);
  const auto bytes = parse(json, 1);
  const auto odd = parse(json, 17);

  ASSERT_EQ(whole.size(), bytes.size());
  ASSERT_EQ(whole.size(), odd.size());
  for (size_t i = 0; i < whole.size(); i++) {
    EXPECT_STREQ(whole[i].contentId, bytes[i].contentId);
    EXPECT_STREQ(whole[i].contentId, odd[i].contentId);
    EXPECT_STREQ(whole[i].title, bytes[i].title);
    EXPECT_STREQ(whole[i].title, odd[i].title);
    EXPECT_EQ(whole[i].progressBp, bytes[i].progressBp);
    EXPECT_EQ(whole[i].progressBp, odd[i].progressBp);
  }
}

TEST(KoboFeedParserTest, EmptyArrayFinishesCleanly) {
  const auto entries = parse("[]");
  EXPECT_TRUE(entries.empty());
}

TEST(KoboFeedParserTest, MalformedJsonReportsError) {
  Collected collected;
  KoboFeedParser parser(collect, &collected);
  const std::string broken = "[tru]";
  parser.feed(broken.data(), broken.size());
  EXPECT_TRUE(parser.hasError());
}

TEST(KoboFeedParserTest, IsoTimestampParsesUtc) {
  EXPECT_EQ(kobo::parseIso8601Utc("1970-01-01T00:00:01Z"), 1u);
  EXPECT_EQ(kobo::parseIso8601Utc("2024-01-03T10:20:30Z"), 1704277230u);
  EXPECT_EQ(kobo::parseIso8601Utc("not a timestamp"), 0u);
  EXPECT_EQ(kobo::parseIso8601Utc("1969-12-31T23:59:59Z"), 0u);
}

}  // namespace
