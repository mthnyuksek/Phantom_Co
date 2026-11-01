#pragma once
#include <cstddef>

namespace pc2 {

// Files embedded at build time (see Cmake/embed.cmake).
// group: "ui" (Web_Ui) or "vault" (Vault_Template). path uses '/'.
struct EmbeddedFile {
    const char* group;
    const char* path;
    const unsigned char* data;
    size_t size;
};

extern const EmbeddedFile kEmbedded[];
extern const size_t kEmbeddedCount;

}  // namespace pc2
