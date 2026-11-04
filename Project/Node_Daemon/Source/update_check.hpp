#pragma once
#include "util.hpp"

namespace pc2 {

// "Is there a newer Phantom Co.?" Asks GitHub for the latest release of PC2_RELEASES_REPO in the background,
// at most once every 12 hours (or right away with `force`). Nothing is downloaded or installed: the page only
// shows a link to the release.
// Returns {current, checked (bool), latest, url, newer (bool), error}; `latest` stays empty until a check worked.
json update_info(bool force = false);

// "1.2.3" or "v1.2.3" > "1.2.0"? Missing parts count as 0, anything after the numbers ("-beta") is ignored.
bool version_newer(const std::string& a, const std::string& b);

}  // namespace pc2
