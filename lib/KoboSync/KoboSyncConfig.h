#pragma once

// The Kobo (calibre-web) sync feature is opt-in per build target. Every
// translation unit that pulls in network or UI code is wrapped in this flag so
// boards that do not enable it pay neither flash nor RAM for the feature.
#ifndef CROSSPOINT_KOBO_SYNC
#define CROSSPOINT_KOBO_SYNC 0
#endif
