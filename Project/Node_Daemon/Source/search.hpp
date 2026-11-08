#pragma once
// Global search: chats (also tasks' and scripts' conversations), tasks, project notes and the errors of runs. A plain scan of
// the Vault files, so nothing has to be kept up to date. Case and Turkish letters are folded (i = ı = İ, s = ş...).
#include <functional>

#include "store.hpp"

namespace pc2 {

// can_see: may this person read a conversation with this meta? {results: [{type, title, before, match, after, link, ts}], truncated}
json search_vault(Store& store, const std::string& query, size_t limit, const std::function<bool(const json& meta)>& can_see);

}  // namespace pc2
