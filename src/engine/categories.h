#pragma once

#include <string>

namespace swiftdm {

// Category folder name for a file, matching the original tree
// (spec 01-product.md): Video, Audio, Compressed, Document, Application, Misc.
std::string category_folder_for(const std::string& filename);

}  // namespace swiftdm
