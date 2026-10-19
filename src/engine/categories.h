#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace swiftdm {

// Category folder name for a file, matching the original tree
// (spec 01-product.md): Video, Audio, Compressed, Document, Application, Misc.
std::string category_folder_for(const std::string& filename);

// Every category in display order. The confirmation dialog offers these as
// overrides of the extension-based default.
const std::vector<std::string>& category_names();

bool is_category_name(std::string_view name);

}  // namespace swiftdm
