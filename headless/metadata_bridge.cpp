#include "game_name.h"
#include "metadata_bridge.h"
#include "assets_dir.h"
#include "diagnostics.h"
#include "profiles.h"
#include "ryujinx_saves.h"
#if defined(__PROSPERO__)
#include "native_directory.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <string_view>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Eden builds stb_image for JPEG only (src/common/stb.h), the format of a game's own icon. A cover
// from a download source (a RomM server's) is a PNG as often: this file has a reader of its own
// for both, private to it (STB_IMAGE_STATIC), beside Eden's.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG 1
#define STBI_ONLY_PNG 1
#define STBI_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <stb_image.h>
#pragma GCC diagnostic pop

#include "common/fs/path_util.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/ns/language.h"
#include "core/hle/service/set/settings_types.h"
#include "core/loader/loader.h"

namespace {
constexpr std::array<const char*, 18> language_names{
    "AmericanEnglish", "BritishEnglish", "Japanese", "French", "German",
    "LatinAmericanSpanish", "Spanish", "Italian", "Dutch", "CanadianFrench",
    "Portuguese", "Russian", "Korean", "TraditionalChinese", "SimplifiedChinese",
    "BrazilianPortuguese", "Polish", "Thai",
};

FileSys::VirtualDir OpenControlRomFs(const FileSys::VirtualFile& file, bool xci) {
    std::shared_ptr<FileSys::NCA> control;
    if (xci) {
        FileSys::XCI image(file);
        if (image.GetStatus() != Loader::ResultStatus::Success) return {};
        control = image.GetNCAByType(FileSys::NCAContentType::Control);
    } else {
        FileSys::NSP package(file);
        if (package.GetStatus() != Loader::ResultStatus::Success) return {};
        control = package.GetNCA(FileSys::GetBaseTitleID(package.GetProgramTitleID()),
                                 FileSys::ContentRecordType::Control);
    }
    if (!control || control->GetStatus() != Loader::ResultStatus::Success) return {};
    const auto romfs = control->GetRomFS();
    return romfs ? FileSys::ExtractRomFS(romfs) : FileSys::VirtualDir{};
}

std::string ReadTitle(const FileSys::VirtualDir& romfs) {
    auto nacp = romfs->GetFile("control.nacp");
    if (!nacp) nacp = romfs->GetFile("Control.nacp");
    if (!nacp || nacp->GetSize() < 0x3000) return {};
    // The first of the game's names (one per language, English first) that is text: a name can be
    // empty or hold something else (game_name.h), and the next language's is then the one to show.
    std::array<char, 0x200> name{};
    for (std::size_t language = 0; language < 16; ++language) {
        if (nacp->Read(reinterpret_cast<unsigned char*>(name.data()), name.size(),
                       language * 0x300) != name.size()) return {};
        name.back() = '\0';
        if (!name.front()) continue;
        if (Eden::UsableName(name.data())) return Eden::PlainName(name.data());
        std::fprintf(stderr, "[EglantinePS5] library: name %zu of a game is not text; trying its next one\n", language);
    }
    return {};
}

// The supported-language flags of a control RomFS's NACP (bit n is NS ApplicationLanguage n); 0
// when they cannot be read.
uint32_t ReadLanguages(const FileSys::VirtualDir& romfs) {
    auto nacp = romfs->GetFile("control.nacp");
    if (!nacp) nacp = romfs->GetFile("Control.nacp");
    FileSys::RawNACP raw{};
    if (!nacp || nacp->ReadObject(&raw) != sizeof(raw)) return 0;
    return static_cast<uint32_t>(raw.supported_languages);
}

// Game file -> its language flags, kept from the Library's metadata pass over the same data.
std::map<std::string, uint32_t>& GameLanguages() {
    static std::map<std::string, uint32_t> languages;
    return languages;
}

FileSys::VirtualFile FindIcon(const FileSys::VirtualDir& romfs) {
    for (const char* language : language_names) {
        if (auto icon = romfs->GetFile(std::string{"icon_"} + language + ".dat")) return icon;
    }
    return {};
}

// A picture's bytes as a 32-bit TGA, made smaller by whole steps (box filter) until it is at most
// `largest` pixels a side. A picture of more than 4096 pixels a side is not read at all (a cover
// from a server could take a lot of memory).
bool WriteTgaBytes(const unsigned char* encoded, std::size_t size, const char* output, int largest) {
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* rgba = stbi_load_from_memory(encoded, static_cast<int>(size), &width, &height, &channels, 4);
    if (!rgba || width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        stbi_image_free(rgba);
        return false;
    }
    int step = 1;
    while (std::max(width, height) / step > largest) ++step;
    const int out_width = std::max(1, width / step);
    const int out_height = std::max(1, height / step);
    std::FILE* file = std::fopen(output, "wb");
    if (!file) {
        stbi_image_free(rgba);
        return false;
    }
    unsigned char header[18]{};
    header[2] = 2;
    header[12] = static_cast<unsigned char>(out_width);
    header[13] = static_cast<unsigned char>(out_width >> 8);
    header[14] = static_cast<unsigned char>(out_height);
    header[15] = static_cast<unsigned char>(out_height >> 8);
    header[16] = 32;
    header[17] = 0x28;
    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    std::vector<unsigned char> row(static_cast<std::size_t>(out_width) * 4);
    for (int y = 0; ok && y < out_height; ++y) {
        for (int x = 0; x < out_width; ++x) {
            unsigned sum[4]{};
            for (int dy = 0; dy < step; ++dy)
                for (int dx = 0; dx < step; ++dx) {
                    const unsigned char* source =
                        rgba + (static_cast<std::size_t>(y * step + dy) * static_cast<std::size_t>(width) +
                                static_cast<std::size_t>(x * step + dx)) * 4;
                    for (int k = 0; k < 4; ++k) sum[k] += source[k];
                }
            const unsigned count = static_cast<unsigned>(step * step);
            row[4 * x + 0] = static_cast<unsigned char>(sum[2] / count);
            row[4 * x + 1] = static_cast<unsigned char>(sum[1] / count);
            row[4 * x + 2] = static_cast<unsigned char>(sum[0] / count);
            row[4 * x + 3] = static_cast<unsigned char>(sum[3] / count);
        }
        ok = std::fwrite(row.data(), 1, row.size(), file) == row.size();
    }
    ok = std::fclose(file) == 0 && ok;
    stbi_image_free(rgba);
    return ok;
}

bool WriteTga(const FileSys::VirtualFile& icon, const char* output) {
    const auto encoded = icon->ReadAllBytes();
    return WriteTgaBytes(encoded.data(), encoded.size(), output, 4096);
}
}

int eden_write_cover_tga(const unsigned char* encoded, size_t size, const char* cover_tga_path) {
    if (!encoded || !size || size > (64u << 20) || !cover_tga_path) return 0;
    // Written beside, then moved into place: the menu may be drawing the old one.
    const std::string staged = std::string{cover_tga_path} + ".new";
    if (!WriteTgaBytes(encoded, size, staged.c_str(), 512) || std::rename(staged.c_str(), cover_tga_path) != 0) {
        (void)std::remove(staged.c_str());
        return 0;
    }
    return 1;
}

static std::vector<std::string> GetFolderGameCandidateDirs(std::string_view rom_path) {
    std::vector<std::string> dirs;
    if (rom_path.empty()) return dirs;

    std::filesystem::path p(rom_path);
    if (p.filename() == "main" || p.filename() == "main.npdm" || p.filename() == "Main.npdm") {
        p = p.parent_path();
    } else if (p.filename().string().ends_with(".dir")) {
        p = p.parent_path();
    }

    std::filesystem::path cur = p;
    for (int depth = 0; depth < 5 && !cur.empty(); ++depth) {
        std::string s = cur.string();
        if (std::find(dirs.begin(), dirs.end(), s) == dirs.end()) {
            dirs.push_back(s);
            dirs.push_back((cur / "control").string());
            dirs.push_back((cur / "Control").string());
        }
        std::string fn = cur.filename().string();
        if (fn == "games" || fn == "nx2" || fn == "roms" || fn == "mnt" || fn == "usb1" || fn == "usb0") break;
        cur = cur.parent_path();
    }

    for (const char* common : {
        "/mnt/usb0/games/covers", "/mnt/usb1/games/covers",
        "/mnt/usb0/covers", "/mnt/usb1/covers",
        "/data/eglantineps5/covers"
    }) {
        dirs.emplace_back(common);
    }
    return dirs;
}

uint64_t eden_game_title_id(const char* rom_path) {
    if (!rom_path) return 0;
    Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, Eden::AssetsPath("keys"));
    const std::string_view named{rom_path};
    const bool is_folder = named.ends_with("/exefs/main") || named.ends_with(".dir") ||
                           (!named.ends_with(".nsp") && !named.ends_with(".xci"));
    if (is_folder) {
        const auto dirs = GetFolderGameCandidateDirs(rom_path);
        // 1. Try reading main.npdm with full file buffer
        for (const auto& dir : dirs) {
            for (const char* npdm_name : {"/main.npdm", "/Main.npdm", "/exefs/main.npdm", "/base/exefs/main.npdm"}) {
                const std::string npdm = dir + npdm_name;
                if (std::FILE* file = std::fopen(npdm.c_str(), "rb")) {
                    std::fseek(file, 0, SEEK_END);
                    long file_size = std::ftell(file);
                    std::fseek(file, 0, SEEK_SET);
                    if (file_size >= 0x80 && file_size <= (1 << 20)) {
                        std::vector<unsigned char> data(static_cast<size_t>(file_size));
                        if (std::fread(data.data(), 1, data.size(), file) == data.size() &&
                            std::memcmp(data.data(), "META", 4) == 0) {
                            std::uint32_t aci = 0;
                            std::memcpy(&aci, data.data() + 0x70, sizeof(aci));
                            if (aci + 0x18 <= data.size() && std::memcmp(data.data() + aci, "ACI0", 4) == 0) {
                                std::uint64_t title = 0;
                                std::memcpy(&title, data.data() + aci + 0x10, sizeof(title));
                                std::fclose(file);
                                if (title != 0) return title;
                            }
                        }
                    }
                    std::fclose(file);
                }
            }
        }
        // 2. Try reading control.nacp
        for (const auto& dir : dirs) {
            for (const char* name : {"/control.nacp", "/Control.nacp", "/control/control.nacp"}) {
                const std::string nacp_path = dir + name;
                if (std::FILE* file = std::fopen(nacp_path.c_str(), "rb")) {
                    FileSys::RawNACP raw{};
                    if (std::fread(&raw, 1, sizeof(raw), file) == sizeof(raw)) {
                        std::fclose(file);
                        if (raw.save_data_owner_id != 0) return raw.save_data_owner_id;
                        if (raw.presence_group_id != 0) return raw.presence_group_id;
                        if (raw.dlc_base_title_id != 0) return raw.dlc_base_title_id;
                    } else {
                        std::fclose(file);
                    }
                }
            }
        }
        // 3. Fallback: check if directory name has 16-hex or decimal title ID
        for (const auto& dir : dirs) {
            std::string folder_name = std::filesystem::path(dir).filename().string();
            if (folder_name != "exefs" && folder_name != "base" && folder_name != "roms" &&
                folder_name != "games" && folder_name != "nx2" && folder_name != "control" &&
                folder_name != "covers" && !folder_name.empty()) {
                char* end = nullptr;
                uint64_t parsed = std::strtoull(folder_name.c_str(), &end, 16);
                if (end != folder_name.c_str() && *end == '\0' && (parsed >> 32) != 0) {
                    return parsed;
                }
                parsed = std::strtoull(folder_name.c_str(), &end, 10);
                if (end != folder_name.c_str() && *end == '\0' && parsed != 0) {
                    return parsed;
                }
            }
        }
        // If still not found, fallback to synthetic non-zero title ID based on path hash
        return 0x0100000000000000ull | (static_cast<uint64_t>(std::hash<std::string>{}(rom_path)) & 0xFFFFFFFFFFFFull);
    }
    FileSys::RealVfsFilesystem vfs;
    const auto file = vfs.OpenFile(rom_path, FileSys::OpenMode::Read);
    if (!file) return 0;
    std::string path = rom_path;
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (path.ends_with(".xci")) {
        FileSys::XCI image(file);
        return image.GetStatus() == Loader::ResultStatus::Success ? image.GetProgramTitleID() : 0;
    }
    if (path.ends_with(".nsp")) {
        FileSys::NSP package(file);
        return package.GetStatus() == Loader::ResultStatus::Success ? package.GetProgramTitleID() : 0;
    }
    return 0;
}

int eden_extract_game_metadata(const char* rom_path, const char* keys_dir,
                               const char* cover_tga_path, char* title,
                               size_t title_capacity) {
    if (!rom_path || !keys_dir || !cover_tga_path || !title || title_capacity == 0) return 0;
    title[0] = '\0';
    Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, keys_dir);

    const std::string_view named{rom_path};
    const bool is_folder = named.ends_with("/exefs/main") || named.ends_with(".dir") ||
                           (!named.ends_with(".nsp") && !named.ends_with(".xci"));

    // Folder game extraction
    if (is_folder) {
        int result = 0;
        const auto dirs = GetFolderGameCandidateDirs(rom_path);

        // 1. Try control.nacp
        for (const auto& dir : dirs) {
            for (const char* name : {"/control.nacp", "/Control.nacp", "/control/control.nacp"}) {
                const std::string nacp_path = dir + name;
                if (std::FILE* file = std::fopen(nacp_path.c_str(), "rb")) {
                    FileSys::RawNACP raw{};
                    if (std::fread(&raw, 1, sizeof(raw), file) == sizeof(raw)) {
                        std::fclose(file);
                        GameLanguages()[rom_path] = static_cast<uint32_t>(raw.supported_languages);
                        for (std::size_t lang = 0; lang < 16; ++lang) {
                            const auto& entry = raw.language_entries.language_entries[lang];
                            std::string candidate_title(entry.application_name.data(),
                                                        strnlen(entry.application_name.data(), sizeof(entry.application_name)));
                            while (!candidate_title.empty() && (candidate_title.back() == '\0' || candidate_title.back() == ' '))
                                candidate_title.pop_back();
                            if (!candidate_title.empty() && Eden::UsableName(candidate_title.c_str())) {
                                const std::string plain = Eden::PlainName(candidate_title.c_str());
                                std::snprintf(title, title_capacity, "%s", plain.c_str());
                                result |= EDEN_METADATA_TITLE;
                                break;
                            }
                        }
                    } else {
                        std::fclose(file);
                    }
                    if (result & EDEN_METADATA_TITLE) break;
                }
            }
            if (result & EDEN_METADATA_TITLE) break;
        }

        const uint64_t title_id = eden_game_title_id(rom_path);
        char title_id_hex_upper[32]{};
        char title_id_hex_lower[32]{};
        if (title_id != 0) {
            std::snprintf(title_id_hex_upper, sizeof(title_id_hex_upper), "%016llX", static_cast<unsigned long long>(title_id));
            std::snprintf(title_id_hex_lower, sizeof(title_id_hex_lower), "%016llx", static_cast<unsigned long long>(title_id));
        }

        // 2. Cover extraction
        for (const auto& dir : dirs) {
            // Check icon dat files
            for (const char* lang : language_names) {
                const std::string icon_path = dir + "/icon_" + lang + ".dat";
                if (std::FILE* f = std::fopen(icon_path.c_str(), "rb")) {
                    std::fseek(f, 0, SEEK_END);
                    long len = std::ftell(f);
                    std::fseek(f, 0, SEEK_SET);
                    if (len > 0 && len < (16 << 20)) {
                        std::vector<unsigned char> bytes(static_cast<size_t>(len));
                        if (std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size()) {
                            std::fclose(f);
                            if (WriteTgaBytes(bytes.data(), bytes.size(), cover_tga_path, 4096)) {
                                result |= EDEN_METADATA_COVER;
                                break;
                            }
                        } else std::fclose(f);
                    } else std::fclose(f);
                }
            }
            if (result & EDEN_METADATA_COVER) break;

            std::vector<std::string> image_candidates = {
                "/cover.png", "/cover.jpg", "/cover.jpeg", "/cover.bmp",
                "/Cover.png", "/Cover.jpg", "/Cover.jpeg",
                "/icon.png", "/icon.jpg", "/icon.jpeg", "/icon.bmp",
                "/Icon.png", "/Icon.jpg", "/Icon.jpeg"
            };
            if (title_id != 0) {
                image_candidates.push_back(std::string("/") + title_id_hex_upper + ".png");
                image_candidates.push_back(std::string("/") + title_id_hex_upper + ".jpg");
                image_candidates.push_back(std::string("/") + title_id_hex_upper + ".jpeg");
                image_candidates.push_back(std::string("/") + title_id_hex_lower + ".png");
                image_candidates.push_back(std::string("/") + title_id_hex_lower + ".jpg");
                image_candidates.push_back(std::string("/") + title_id_hex_lower + ".jpeg");
            }
            for (const auto& img_name : image_candidates) {
                const std::string img_path = dir + img_name;
                if (std::FILE* f = std::fopen(img_path.c_str(), "rb")) {
                    std::fseek(f, 0, SEEK_END);
                    long len = std::ftell(f);
                    std::fseek(f, 0, SEEK_SET);
                    if (len > 0 && len < (16 << 20)) {
                        std::vector<unsigned char> bytes(static_cast<size_t>(len));
                        if (std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size()) {
                            std::fclose(f);
                            if (WriteTgaBytes(bytes.data(), bytes.size(), cover_tga_path, 4096)) {
                                result |= EDEN_METADATA_COVER;
                                break;
                            }
                        } else std::fclose(f);
                    } else std::fclose(f);
                }
            }
            if (result & EDEN_METADATA_COVER) break;
        }

        // 3. Known Switch 2 Title ID to friendly game name
        if (!(result & EDEN_METADATA_TITLE) || title[0] == '\0') {
            static const std::unordered_map<uint64_t, const char*> kKnownTitles = {
                {0x040074a01bf12000ull, "Donkey Kong Bananza"},
                {0x0400380020056000ull, "Splatoon Raiders"},
                {0x0400c3f00006e000ull, "Mario Kart World"},
            };
            if (const auto it = kKnownTitles.find(title_id); it != kKnownTitles.end()) {
                std::snprintf(title, title_capacity, "%s", it->second);
                result |= EDEN_METADATA_TITLE;
            }
        }

        // If title still empty, fallback to folder name
        if (!(result & EDEN_METADATA_TITLE) && title[0] == '\0') {
            for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
                std::string folder_name = std::filesystem::path(*it).filename().string();
                if (folder_name != "exefs" && folder_name != "base" && folder_name != "roms" &&
                    folder_name != "games" && folder_name != "nx2" && folder_name != "control" &&
                    folder_name != "covers" && !folder_name.empty()) {
                    std::snprintf(title, title_capacity, "%s", folder_name.c_str());
                    result |= EDEN_METADATA_TITLE;
                    break;
                }
            }
        }
        return result;
    }

    FileSys::RealVfsFilesystem vfs;
    const auto file = vfs.OpenFile(rom_path, FileSys::OpenMode::Read);
    if (!file) return 0;
    std::string path = rom_path;
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool xci = path.size() >= 4 && path.substr(path.size() - 4) == ".xci";
    const auto romfs = OpenControlRomFs(file, xci);
    if (!romfs) return 0;
    GameLanguages()[rom_path] = ReadLanguages(romfs);

    int result = 0;
    const auto extracted_title = ReadTitle(romfs);
    if (!extracted_title.empty()) {
        std::snprintf(title, title_capacity, "%s", extracted_title.c_str());
        result |= EDEN_METADATA_TITLE;
    }
    if (const auto icon = FindIcon(romfs); icon && WriteTga(icon, cover_tga_path))
        result |= EDEN_METADATA_COVER;
    return result;
}

uint32_t eden_game_supported_languages(const char* rom_path, const char* keys_dir) {
    if (!rom_path || !keys_dir) return 0;
    if (const auto known = GameLanguages().find(rom_path); known != GameLanguages().end()) return known->second;
    if (const std::string_view named{rom_path}; named.ends_with("/exefs/main")) {
        const auto dirs = GetFolderGameCandidateDirs(rom_path);
        for (const auto& dir : dirs) {
            for (const char* name : {"/control.nacp", "/Control.nacp"}) {
                const std::string nacp_path = dir + name;
                if (std::FILE* file = std::fopen(nacp_path.c_str(), "rb")) {
                    FileSys::RawNACP raw{};
                    if (std::fread(&raw, 1, sizeof(raw), file) == sizeof(raw)) {
                        std::fclose(file);
                        return GameLanguages()[rom_path] = static_cast<uint32_t>(raw.supported_languages);
                    }
                    std::fclose(file);
                }
            }
        }
        return 0;
    }
    try {
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, keys_dir);
        FileSys::RealVfsFilesystem vfs;
        const auto file = vfs.OpenFile(rom_path, FileSys::OpenMode::Read);
        if (!file) return 0;
        std::string path = rom_path;
        std::transform(path.begin(), path.end(), path.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto romfs = OpenControlRomFs(file, path.ends_with(".xci"));
        if (!romfs) return 0;
        return GameLanguages()[rom_path] = ReadLanguages(romfs);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[EglantinePS5] languages: %s\n", error.what());
        return 0;
    }
}

// The base package of a NUP, or the newest update's when there is no base.
const FileSys::NupContainer::PackageInfo* NupProgramPackage(const FileSys::NupContainer& nup) {
    if (nup.GetStatus() != Loader::ResultStatus::Success) return nullptr;
    auto index = nup.FindBasePackage();
    if (!index) index = nup.FindBestUpdatePackage();
    return index ? &nup.GetPackages()[*index] : nullptr;
}

namespace {
struct AddOns {
    std::string update;
    std::set<uint64_t> dlc;
    uint32_t languages = 0;  // the update's own language flags (its control data replaces the game's)
};
// Base title ID -> its update and DLC files, from the last eden_scan_addons.
std::map<uint64_t, AddOns>& ScannedAddOns() {
    static std::map<uint64_t, AddOns> scanned;
    return scanned;
}
}

void eden_scan_addons(const char* updates_dir, const char* keys_dir) {
    auto& scanned = ScannedAddOns();
    scanned.clear();
    if (!updates_dir || !keys_dir) return;
    try {
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, keys_dir);
        FileSys::RealVfsFilesystem vfs;
        auto directory = vfs.OpenDirectory(updates_dir, FileSys::OpenMode::Read);
        if (!directory) return;
        const FileSys::ExternalContentProvider provider({std::move(directory)});
        for (const auto& entry : provider.ListEntriesFilter(FileSys::TitleType::Update, std::nullopt, std::nullopt)) {
            auto& addons = scanned[FileSys::GetBaseTitleID(entry.title_id)];
            if (!addons.update.empty()) continue;
            // Newest first; the display version comes from the update's own control data.
            if (const auto versions = provider.ListUpdateVersions(entry.title_id); !versions.empty()) {
                addons.update = versions.front().version_string.empty()
                    ? "v" + std::to_string(versions.front().version) : versions.front().version_string;
            } else if (const auto version = provider.GetEntryVersion(entry.title_id)) {
                addons.update = "v" + std::to_string(*version);
            }
            if (const auto control = provider.GetEntry(entry.title_id, FileSys::ContentRecordType::Control);
                control && control->GetStatus() == Loader::ResultStatus::Success)
                if (const auto romfs = control->GetRomFS())
                    if (const auto files = FileSys::ExtractRomFS(romfs)) addons.languages = ReadLanguages(files);
        }
        for (const auto& entry : provider.ListEntriesFilter(FileSys::TitleType::AOC, std::nullopt, std::nullopt))
            scanned[FileSys::GetBaseTitleID(entry.title_id)].dlc.insert(entry.title_id);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[EglantinePS5] updates: %s\n", error.what());
        scanned.clear();
    }
}

void eden_note_nup_addons(const char* rom_path) {
    if (!rom_path) return;
    try {
        FileSys::RealVfsFilesystem vfs;
        const auto file = vfs.OpenFile(rom_path, FileSys::OpenMode::Read);
        if (!file || !FileSys::NupContainer::IsNupFile(file)) return;
        const FileSys::NupContainer nup(file);
        const auto* program = NupProgramPackage(nup);
        if (!program) return;
        auto& addons = ScannedAddOns()[FileSys::GetBaseTitleID(program->title_id)];
        if (const auto update = nup.FindBestUpdatePackage()) {
            const auto& package = nup.GetPackages()[*update];
            addons.update = package.display_version.empty() ? "v" + std::to_string(package.version)
                                                            : package.display_version;
        }
        for (const auto& package : nup.GetPackages())
            if (package.kind == FileSys::NupContainer::PackageKind::Addon) addons.dlc.insert(package.title_id);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[EglantinePS5] nup: %s\n", error.what());
    }
}

namespace {
// Whether an NSP or XCI holds content of the game: its update (base | 0x800) or one of its DLC
// (base + 0x1000 + n) have the game's base title ID.
bool HoldsContentOf(const FileSys::VirtualFile& file, uint64_t base) {
    std::string name = file->GetName();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::shared_ptr<FileSys::NSP> package;
    if (name.ends_with(".nsp")) {
        package = std::make_shared<FileSys::NSP>(file);
    } else if (name.ends_with(".xci")) {
        FileSys::XCI image(file);
        if (image.GetStatus() == Loader::ResultStatus::Success) package = image.GetSecurePartitionNSP();
    }
    if (!package || package->GetStatus() != Loader::ResultStatus::Success) return false;
    for (const auto& [title_id, contents] : package->GetNCAs())
        if (FileSys::GetBaseTitleID(title_id) == base) return true;
    return false;
}

int FindAddOnFiles(const FileSys::VirtualDir& directory, uint64_t base, eden_found_file found, void* user, int depth) {
    if (!directory || depth > 8) return 0;
    int count = 0;
    for (const auto& file : directory->GetFiles())
        if (HoldsContentOf(file, base)) {
            found(user, file->GetFullPath().c_str());
            ++count;
        }
    for (const auto& folder : directory->GetSubdirectories()) count += FindAddOnFiles(folder, base, found, user, depth + 1);
    return count;
}
} // namespace

int eden_game_addon_files(uint64_t title_id, const char* updates_dir, const char* keys_dir, eden_found_file found,
                          void* user) {
    if (!title_id || !updates_dir || !keys_dir || !found) return 0;
    try {
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, keys_dir);
        FileSys::RealVfsFilesystem vfs;
        return FindAddOnFiles(vfs.OpenDirectory(updates_dir, FileSys::OpenMode::Read),
                              FileSys::GetBaseTitleID(title_id), found, user, 0);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[EglantinePS5] updates: %s\n", error.what());
        return 0;
    }
}

int eden_game_language(const char* rom_path, const char* keys_dir, uint64_t title_id, int chosen) {
    const auto& codes = Service::Set::available_language_codes;
    if (chosen < 0 || chosen >= static_cast<int>(codes.size())) return chosen;
    uint32_t supported = 0;
    const auto& scanned = ScannedAddOns();
    if (const auto found = scanned.find(FileSys::GetBaseTitleID(title_id));
        title_id && found != scanned.end() && !found->second.update.empty())
        supported = found->second.languages;
    if (!supported) supported = eden_game_supported_languages(rom_path, keys_dir);
    namespace NS = Service::NS;
    const auto application = NS::ConvertToApplicationLanguage(codes[static_cast<std::size_t>(chosen)]);
    const auto* priorities = application ? NS::GetApplicationLanguagePriorityList(*application) : nullptr;
    if (!supported || !priorities) return chosen;
    for (const auto language : *priorities) {
        if ((supported & NS::GetSupportedLanguageFlag(language)) == 0) continue;
        const auto match = NS::ConvertToLanguageCode(language);
        for (std::size_t i = 0; match && i < codes.size(); ++i)
            if (codes[i] == *match) return static_cast<int>(i);
    }
    return chosen;
}

int eden_game_addons(uint64_t title_id, char* update_version, size_t capacity, unsigned* dlc_count) {
    if (update_version && capacity) update_version[0] = '\0';
    if (dlc_count) *dlc_count = 0;
    const auto& scanned = ScannedAddOns();
    const auto found = scanned.find(FileSys::GetBaseTitleID(title_id));
    if (!title_id || found == scanned.end()) return 0;
    if (update_version && capacity) std::snprintf(update_version, capacity, "%s", found->second.update.c_str());
    if (dlc_count) *dlc_count = static_cast<unsigned>(found->second.dlc.size());
    return !found->second.update.empty() || !found->second.dlc.empty();
}

namespace {
// The save folder name of the profile games use (profiles.h), as Eden's save paths spell it.
// Without a profile file (no game has run yet) the one existing user save folder is used.
std::string EdenUserFolder(const std::filesystem::path& saves, std::string& error) {
    // The chosen profile's (profiles.h): a save goes in and comes out for whoever is playing.
    if (const auto profiles = Eden::Profiles::Read(); !profiles.empty())
        return profiles[static_cast<std::size_t>(Eden::Profiles::CurrentIndex())].Key();
    std::error_code list_error;
    std::vector<std::string> users;
    for (const auto& entry : Eden::RyujinxSaves::ListFolder(saves, list_error)) {
        const std::string name = entry.path().filename().string();
        if (name.size() == 32 && name != std::string(32, '0')) users.push_back(name);
    }
    if (users.size() == 1) return users.front();
    error = "start any game once first, so EglantinePS5 creates its user";
    return {};
}
} // namespace

namespace {
struct SaveSource {
    int kind = EDEN_SAVE_NONE;
    std::vector<Eden::RyujinxSaves::Save> saves;
};
// A save folder copied by hand comes before a Ryujinx data folder: it names the game itself.
SaveSource FindSaveSource(uint64_t title_id) {
    std::string error;
    SaveSource source;
    source.saves = Eden::RyujinxSaves::FindFolderSaves(Eden::AssetsPath("save-import"), title_id, error);
    if (!source.saves.empty()) {
        source.kind = EDEN_SAVE_FOLDER;
        return source;
    }
    source.saves = Eden::RyujinxSaves::FindSaves(Eden::AssetsPath("ryujinx"), title_id, error);
    if (!source.saves.empty()) source.kind = EDEN_SAVE_RYUJINX;
    return source;
}
std::string TitleName(uint64_t title_id) {
    char title[17];
    std::snprintf(title, sizeof(title), "%016llX", static_cast<unsigned long long>(title_id));
    return title;
}
std::string TimeStamp() {
    char stamp[32] = "now";
    const std::time_t now = std::time(nullptr);
    if (const std::tm* local = std::localtime(&now)) std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", local);
    return stamp;
}
void PutPath(char* path, size_t capacity, const std::string& value) {
    if (path && capacity) std::snprintf(path, capacity, "%s", value.c_str());
}
} // namespace

int eden_save_import_source(uint64_t title_id) {
    return title_id ? FindSaveSource(title_id).kind : EDEN_SAVE_NONE;
}

int eden_save_import(uint64_t title_id, char* path, size_t capacity) {
    namespace fs = std::filesystem;
    PutPath(path, capacity, "");
    const SaveSource source = title_id ? FindSaveSource(title_id) : SaveSource{};
    if (source.saves.empty()) return EDEN_SAVE_NOTHING;
    std::string error;
    const fs::path root = fs::path{Eden::UserDir()} / "nand" / "user" / "save" / "0000000000000000";
    const std::string user = EdenUserFolder(root, error);
    if (user.empty()) {
        Eden::Report("save import", error.c_str());
        return EDEN_SAVE_NO_USER;
    }
    const std::string title = TitleName(title_id);
    const fs::path backups = fs::path{Eden::kDataDir} / "backup" / "save-import";
    bool replaced = false;
    if (!Eden::RyujinxSaves::Import(source.saves, root / user / title, root / std::string(32, '0') / title, backups,
                                    title + "-" + TimeStamp(), error, &replaced)) {
        Eden::Report("save import", error.c_str());
        return EDEN_SAVE_FAILED;
    }
    if (replaced) PutPath(path, capacity, backups.string());
    Eden::Report("save import", (title + (source.kind == EDEN_SAVE_FOLDER ? " from save-import" : " from ryujinx")).c_str());
    return EDEN_SAVE_DONE;
}

int eden_save_export(uint64_t title_id, char* path, size_t capacity) {
    namespace fs = std::filesystem;
    PutPath(path, capacity, "");
    if (!title_id) return EDEN_SAVE_NOTHING;
    std::string error;
    const fs::path root = fs::path{Eden::UserDir()} / "nand" / "user" / "save" / "0000000000000000";
    const std::string user = EdenUserFolder(root, error);
    if (user.empty()) return EDEN_SAVE_NOTHING;
    const std::string title = TitleName(title_id);
    const fs::path target = fs::path{Eden::AssetsPath("save-export")} / (title + "-" + TimeStamp());
    if (!Eden::RyujinxSaves::Export(root / user / title, root / std::string(32, '0') / title, target, error)) {
        Eden::Report("save export", error.c_str());
        return error == "no save yet" ? EDEN_SAVE_NOTHING : EDEN_SAVE_FAILED;
    }
    PutPath(path, capacity, target.string());
    Eden::Report("save export", target.string().c_str());
    return EDEN_SAVE_DONE;
}

const char* eden_startup_error() {
    static const std::string error = []() -> std::string {
        try {
            const std::string keys = Eden::AssetsPath("keys");
            const std::string firmware = Eden::AssetsPath("firmware");
            Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, keys);
            FileSys::RealVfsFilesystem vfs;
            const auto prod = vfs.OpenFile(std::string{keys} + "/prod.keys", FileSys::OpenMode::Read);
            if (prod && prod->GetSize()) {
                auto& manager = Core::Crypto::KeyManager::Instance();
                if (!manager.HasKey(Core::Crypto::S256KeyType::Header))
                    return "prod.keys could not supply an NCA header key. Replace it with a valid key dump.";
            }
            std::error_code ec;
#if defined(__PROSPERO__)
            const auto entries = Eden::ReadNativeDirectory(firmware, ec);
#else
            std::vector<std::filesystem::directory_entry> entries;
            for (std::filesystem::directory_iterator it{firmware, ec}, end; !ec && it != end; it.increment(ec))
                entries.push_back(*it);
#endif
            if (!ec) {
                for (const auto& entry : entries) {
                    if (entry.path().extension() != ".nca") continue;
                    const auto file = vfs.OpenFile(entry.path().string(), FileSys::OpenMode::Read);
                    if (!file) return "A firmware NCA cannot be read. Reinstall the firmware dump.";
                    FileSys::NCA nca(file);
                    if (nca.GetStatus() != Loader::ResultStatus::Success)
                        return "Firmware NCA validation failed (code " +
                            std::to_string(static_cast<unsigned>(nca.GetStatus())) +
                            "). Check that firmware and prod.keys are compatible.";
                    if (nca.GetTitleId() == 0x0100000000000809ULL && nca.GetRomFS()) {
                        const auto romfs = FileSys::ExtractRomFS(nca.GetRomFS());
                        const auto romfs_file = romfs ? romfs->GetFile("file") : FileSys::VirtualFile{};
                        (void)romfs_file;
                    }
                }
            }
            return {};
        } catch (...) {
            return "Setup validation failed. Check that firmware and key files are readable and valid.";
        }
    }();
    return error.c_str();
}
