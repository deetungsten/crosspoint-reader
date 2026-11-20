#include <gtest/gtest.h>

#include "KoboPreSleepSync.h"

TEST(KoboPreSleepSync, RequiresTimeoutConfiguredAndConnected) {
  EXPECT_TRUE(kobo::shouldAttemptPreSleepSync(true, true, true));
  EXPECT_FALSE(kobo::shouldAttemptPreSleepSync(false, true, true));
  EXPECT_FALSE(kobo::shouldAttemptPreSleepSync(true, false, true));
  EXPECT_FALSE(kobo::shouldAttemptPreSleepSync(true, true, false));
  EXPECT_FALSE(kobo::shouldAttemptPreSleepSync(false, false, false));
}

TEST(KoboPreSleepSync, ProbeBudgetIsWrapSafe) {
  EXPECT_FALSE(kobo::probeBudgetExpired(100, 109, 10));
  EXPECT_TRUE(kobo::probeBudgetExpired(100, 110, 10));
  EXPECT_TRUE(kobo::probeBudgetExpired(0xFFFFFFF0u, 0x00000005u, 10));
  EXPECT_FALSE(kobo::probeBudgetExpired(0xFFFFFFF0u, 0x00000005u, 30));
}

TEST(KoboPreSleepSync, RemainingBudgetNeverUnderflows) {
  EXPECT_EQ(kobo::remainingProbeBudgetMs(100, 100, 10000), 10000u);
  EXPECT_EQ(kobo::remainingProbeBudgetMs(100, 6000, 10000), 4100u);
  EXPECT_EQ(kobo::remainingProbeBudgetMs(100, 10100, 10000), 0u);
  EXPECT_EQ(kobo::remainingProbeBudgetMs(0xFFFFFFF0u, 0x00000005u, 10), 0u);
}

TEST(KoboPreSleepSync, ParsesHttpAndHttpsDefaults) {
  const auto https = kobo::parseEndpointAddress("https://books.example.com");
  ASSERT_TRUE(https.valid);
  EXPECT_EQ(https.host, "books.example.com");
  EXPECT_EQ(https.port, 443);
  EXPECT_TRUE(https.secure);

  const auto http = kobo::parseEndpointAddress("http://192.168.1.5:8083/kobo/token");
  ASSERT_TRUE(http.valid);
  EXPECT_EQ(http.host, "192.168.1.5");
  EXPECT_EQ(http.port, 8083);
  EXPECT_FALSE(http.secure);
}

TEST(KoboPreSleepSync, ParsesTrailingPathUserinfoAndIpv6) {
  const auto path = kobo::parseEndpointAddress("https://example.com/some/path?x=1");
  ASSERT_TRUE(path.valid);
  EXPECT_EQ(path.host, "example.com");
  EXPECT_EQ(path.port, 443);

  const auto user = kobo::parseEndpointAddress("http://user:pass@example.com:80/");
  ASSERT_TRUE(user.valid);
  EXPECT_EQ(user.host, "example.com");
  EXPECT_EQ(user.port, 80);

  const auto ipv6 = kobo::parseEndpointAddress("http://[::1]:9000/x");
  ASSERT_TRUE(ipv6.valid);
  EXPECT_EQ(ipv6.host, "::1");
  EXPECT_EQ(ipv6.port, 9000);
}

TEST(KoboPreSleepSync, RejectsMalformedEndpoints) {
  EXPECT_FALSE(kobo::parseEndpointAddress("").valid);
  EXPECT_FALSE(kobo::parseEndpointAddress("books.example.com").valid);
  EXPECT_FALSE(kobo::parseEndpointAddress("ftp://books.example.com").valid);
  EXPECT_FALSE(kobo::parseEndpointAddress("http://").valid);
  EXPECT_FALSE(kobo::parseEndpointAddress("http://:8080").valid);
  EXPECT_FALSE(kobo::parseEndpointAddress("http://host:0").valid);
  EXPECT_FALSE(kobo::parseEndpointAddress("http://host:65536").valid);
  EXPECT_FALSE(kobo::parseEndpointAddress("http://host:abc").valid);
  EXPECT_FALSE(kobo::parseEndpointAddress("http://::1:8080").valid);
  EXPECT_FALSE(kobo::parseEndpointAddress("http://[::1").valid);
}
