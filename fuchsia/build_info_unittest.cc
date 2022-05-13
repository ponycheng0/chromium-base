// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/buildinfo/cpp/fidl.h>

#include "base/fuchsia/build_info.h"
#include "base/test/gtest_util.h"

namespace base {

// Ensures that when FetchAndCacheSystemBuildInfo() has not been called in the
// process that a  DCHECK fires to alert the developer.
TEST(BuildInfoDeathTest, GetCachedBuildInfo_DcheckIfNotAlreadyFetched) {
  // Clear the cached build info to force an error condition.
  ClearCachedBuildInfoForTesting();

  EXPECT_DCHECK_DEATH_WITH(
      { GetCachedBuildInfo(); },
      "FetchAndCacheSystemBuildInfo\\(\\) has not been called in this process");

  // All test processes have BuildInfo cached before tests are run. Re-fetch and
  // cache the BuildInfo to restore that state for any tests that are
  // subsequently run in the same process as this one.
  FetchAndCacheSystemBuildInfo();
}

TEST(BuildInfoTest, GetCachedBuildInfo_CheckExpectedValues) {
  // Ensure the cached BuildInfo is in a known state.
  ClearCachedBuildInfoForTesting();
  FetchAndCacheSystemBuildInfo();

  // TODO(crbug.com/1310358): Check for specific values once Fuchsia
  // completes the requested changes to the data returned from the fake.
  EXPECT_TRUE(GetCachedBuildInfo().has_product_config());
  EXPECT_TRUE(GetCachedBuildInfo().has_board_config());
  EXPECT_TRUE(GetCachedBuildInfo().has_version());
  EXPECT_TRUE(GetCachedBuildInfo().has_latest_commit_date());
}

}  // namespace base
