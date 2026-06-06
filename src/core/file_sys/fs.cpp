// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <tsl/robin_set.h>
#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/emulator_settings.h"
#include "core/file_sys/devices/logger.h"
#include "core/file_sys/devices/nop_device.h"
#include "core/file_sys/fs.h"

namespace Core::FileSys {

bool MntPoints::ignore_game_patches = false;

std::string RemoveTrailingSlashes(const std::string& path) {
    // Remove trailing slashes to make comparisons simpler.
    std::string path_sanitized = path;
    while (path_sanitized.ends_with("/")) {
        path_sanitized.pop_back();
    }
    return path_sanitized;
}

MntPoints::GuestPathInfo MntPoints::ResolveGuestPath(std::string_view guest_path) {
    // Evil games like Turok2 pass double slashes e.g /app0//game.kpf
    std::string normalized_path(guest_path);
    size_t pos = normalized_path.find("//");
    while (pos != std::string::npos) {
        normalized_path.replace(pos, 2, "/");
        pos = normalized_path.find("//", pos + 1);
    }

    std::filesystem::path relative_path;

    if (const auto mount = GetMount(normalized_path)) {
        if (normalized_path.size() > mount->mount.size()) {
            // Remove device (e.g /app0) from path to retrieve relative path.
            relative_path = std::string_view{normalized_path}.substr(mount->mount.size() + 1);
        }
    }

    return {
        .normalized_path = std::move(normalized_path),
        .relative_path = std::move(relative_path),
    };
}

void MntPoints::Mount(const std::filesystem::path& host_folder, const std::string& guest_folder,
                      bool read_only) {
    std::scoped_lock lock{m_mutex};
    const auto guest_folder_sanitized = RemoveTrailingSlashes(guest_folder);
    m_mnt_pairs.emplace_back(host_folder, guest_folder_sanitized, read_only);
}

void MntPoints::Unmount(const std::filesystem::path& host_folder, const std::string& guest_folder) {
    std::scoped_lock lock{m_mutex};
    const auto guest_folder_sanitized = RemoveTrailingSlashes(guest_folder);
    auto it = std::remove_if(m_mnt_pairs.begin(), m_mnt_pairs.end(), [&](const MntPair& pair) {
        return pair.mount == guest_folder_sanitized;
    });
    m_mnt_pairs.erase(it, m_mnt_pairs.end());
}

void MntPoints::UnmountAll() {
    std::scoped_lock lock{m_mutex};
    m_mnt_pairs.clear();
}

std::filesystem::path MntPoints::GetHostPath(std::string_view path, bool* is_read_only,
                                             HostPathType path_type) {

    const auto guest_info = ResolveGuestPath(path);
    const auto& corrected_path = guest_info.normalized_path;
    const auto& rel_path = guest_info.relative_path;

    if (path.length() > 255)
        return "";

    const std::optional<MntPair> mount = GetMount(corrected_path);
    if (!mount) {
        return "";
    }

    if (is_read_only) {
        *is_read_only = mount->read_only;
    }

    const auto corrected_path_sanitized = RemoveTrailingSlashes(corrected_path);
    std::filesystem::path host_path = mount->host_path;

    // Update folder is either mount + "-UPDATE" or mount + "-patch"
    std::filesystem::path patch_path = mount->host_path;
    patch_path += "-UPDATE";
    if (!std::filesystem::exists(patch_path)) {
        patch_path = mount->host_path;
        patch_path += "-patch";
    }

    // Mods folder can only be at mount + "-mods"
    std::filesystem::path mods_path = mount->host_path;
    mods_path += "-mods";

    // If we're just retrieving the mount, return the correct mount path.
    if (corrected_path_sanitized == mount->mount) {
        if (path_type == HostPathType::Mod) {
            if (EmulatorSettings.IsModsEnabled()) {
                return mods_path;
            }
            return std::filesystem::path();
        } else if (path_type == HostPathType::Patch) {
            return patch_path;
        } else {
            return host_path;
        }
    }

    host_path /= rel_path;
    patch_path /= rel_path;

    if (path_type == HostPathType::Mod) {
        return mods_path;
    } else if (path_type == HostPathType::Patch) {
        return patch_path;
    }

    const auto& ordered_mods = GetOrderedModRoots(mods_path);

    std::vector<std::filesystem::path> active_mod_paths;
    active_mod_paths.reserve(ordered_mods.size());

    for (const auto& mod : ordered_mods) {
        active_mod_paths.emplace_back(mod / rel_path);
    }

    if ((corrected_path.starts_with("/app0") || corrected_path.starts_with("/hostapp")) &&
        path_type != HostPathType::Base && EmulatorSettings.IsModsEnabled() &&
        std::filesystem::exists(mods_path)) {
        for (const auto& mod_path : active_mod_paths) {
            if (std::filesystem::exists(mod_path)) {
                return mod_path;
            }
        }
    }

    if ((corrected_path.starts_with("/app0") || corrected_path.starts_with("/hostapp")) &&
        path_type != HostPathType::Base && !ignore_game_patches &&
        std::filesystem::exists(patch_path)) {
        return patch_path;
    }

    if (!NeedsCaseInsensitiveSearch) {
        return host_path;
    }

    const auto search = [&](const auto host_path) {
        // If the path does not exist attempt to verify this.
        // Retrieve parent path until we find one that exists.
        std::scoped_lock lk{m_mutex};
        path_parts.clear();
        auto current_path = host_path;
        while (!current_path.empty() && !std::filesystem::exists(current_path)) {
            // We have probably cached this if it's a folder.
            if (auto it = path_cache.find(current_path); it != path_cache.end()) {
                current_path = it->second;
                break;
            }
            path_parts.emplace_back(current_path.filename());
            current_path = current_path.parent_path();
        }
        if (!current_path.empty()) {
            // We have found an anchor. Traverse parts we recoded and see if they
            // exist in filesystem but in different case.
            auto guest_path = current_path;
            while (!path_parts.empty()) {
                const auto part = path_parts.back();
                const auto add_match = [&](const auto& host_part) {
                    current_path /= host_part;
                    guest_path /= part;
                    path_cache[guest_path] = current_path;
                    path_parts.pop_back();
                };
                // Can happen when the mismatch is in upper folder.
                if (std::filesystem::exists(current_path / part)) {
                    add_match(part);
                    continue;
                }
                const auto part_low = Common::ToLower(part.string());
                bool found_match = false;
                for (const auto& path : std::filesystem::directory_iterator(current_path)) {
                    const auto candidate = path.path().filename();
                    const auto filename = Common::ToLower(candidate.string());
                    // Check if a filename matches in case insensitive manner.
                    if (filename != part_low) {
                        continue;
                    }
                    // We found a match, record the actual path in the cache.
                    add_match(candidate);
                    found_match = true;
                    break;
                }
                if (!found_match) {
                    return std::optional<std::filesystem::path>({});
                }
            }
        }
        return std::optional<std::filesystem::path>(current_path);
    };

    if ((corrected_path.starts_with("/app0") || corrected_path.starts_with("/hostapp")) &&
        path_type != HostPathType::Base) {
        for (const auto& mod_path : active_mod_paths) {
            if (const auto path = search(mod_path)) {
                return *path;
            }
        }
    }

    if (path_type != HostPathType::Base && !ignore_game_patches) {
        if (const auto path = search(patch_path)) {
            return *path;
        }
    }
    if (const auto path = search(host_path)) {
        return *path;
    }

    // Opening the guest path will surely fail but at least gives
    // a better error message than the empty path.
    return host_path;
}

// TODO: Does not handle mount points inside mount points.
void MntPoints::IterateDirectory(std::string_view guest_directory,
                                 const IterateDirectoryCallback& callback) {
    const auto base_path = GetHostPath(guest_directory, nullptr, HostPathType::Base);

    // Forces path types so as not to resolve to base path
    const auto patch_path = GetHostPath(guest_directory, nullptr, HostPathType::Patch);
    const auto mods_path = GetHostPath(guest_directory, nullptr, HostPathType::Mod);

    const auto guest_info = ResolveGuestPath(guest_directory);
    const auto& rel_path = guest_info.relative_path;

    const auto& ordered_mods = GetOrderedModRoots(mods_path);

    auto get_mod_entry_path =
        [&](const std::filesystem::path& filename) -> std::optional<std::filesystem::path> {
        for (const auto& mod_root : ordered_mods) {
            const auto mod_file = mod_root / rel_path / filename;
            if (std::filesystem::exists(mod_file)) {
                return mod_file;
            }
        }
        return std::nullopt;
    };

    // Prepend entries for . and .., as both are treated as files on PS4.
    callback(base_path / ".", false);
    callback(base_path / "..", false);

    // Pass 1: Any files that existed in the base directory, using mod/patch directory if needed.
    if (std::filesystem::exists(base_path)) {
        for (const auto& entry : std::filesystem::directory_iterator(base_path)) {
            const auto mod_entry_path = get_mod_entry_path(entry.path().filename());
            const auto patch_entry_path = patch_path / entry.path().filename();

            if (mod_entry_path) {
                callback(*mod_entry_path, !std::filesystem::is_directory(*mod_entry_path));
                continue;
            } else if (std::filesystem::exists(patch_entry_path)) {
                callback(patch_entry_path, !std::filesystem::is_directory(patch_entry_path));
                continue;
            }
            callback(entry.path(), !entry.is_directory());
        }
    }

    // Pass 2: Any files that exist only in the patch directory.
    if (std::filesystem::exists(patch_path)) {
        for (const auto& entry : std::filesystem::directory_iterator(patch_path)) {
            const auto base_entry_path = base_path / entry.path().filename();
            if (!std::filesystem::exists(base_entry_path)) {
                const auto mod_entry_path = get_mod_entry_path(entry.path().filename());
                if (mod_entry_path) {
                    callback(*mod_entry_path, !std::filesystem::is_directory(*mod_entry_path));
                    continue;
                }
                callback(entry.path(), !entry.is_directory());
            }
        }
    }

    // Pass 3: Any files that exist only in the mod directory (confirmed this can be valid)
    for (const auto& mod_root : ordered_mods) {
        const auto current_mod_dir = mod_root / rel_path;
        if (std::filesystem::exists(current_mod_dir)) {
            for (const auto& entry : std::filesystem::directory_iterator(current_mod_dir)) {
                const auto filename = entry.path().filename();
                const auto base_entry_path = base_path / filename;
                const auto patch_entry_path = patch_path / filename;

                if (!std::filesystem::exists(base_entry_path) &&
                    !std::filesystem::exists(patch_entry_path)) {
                    const auto mod_entry_path = get_mod_entry_path(filename);
                    if (mod_entry_path == entry.path()) {
                        callback(entry.path(), !entry.is_directory());
                    }
                }
            }
        }
    }
}

void CheckModConfig(const std::filesystem::path& mods_path) {
    if (!std::filesystem::exists(mods_path)) {
        return;
    }

    const auto json_path = mods_path / "mods.json";
    nlohmann::json config = nlohmann::json::array();

    if (std::filesystem::exists(json_path)) {
        std::ifstream file(json_path);
        try {
            file >> config;
        } catch (const nlohmann::json::exception& e) {
            LOG_WARNING(Core_Filesystem, "Failed to parse mods JSON: {}", e.what());
            return;
        }
    }

    // map known mods to avoid duplicates
    tsl::robin_set<std::string> known_mods;
    for (const auto& mod : config) {
        if (mod.contains("name") && mod["name"].is_string()) {
            known_mods.insert(mod["name"].get<std::string>());
        }
    }

    bool changed = false;

    // check directory for new mods that aren't in json yet
    for (const auto& entry : std::filesystem::directory_iterator(mods_path)) {
        if (!entry.is_directory()) {
            continue;
        }

        const auto name = entry.path().filename().string();
        if (!known_mods.contains(name)) {
            config.push_back({
                {"name", name}, {"enabled", true} // enabled by default
            });
            known_mods.insert(name);
            changed = true;
        }
    }

    if (changed || !std::filesystem::exists(json_path)) {
        std::ofstream file(json_path, std::ios::trunc);
        file << config.dump(4);
    }
}

const std::vector<std::filesystem::path>& MntPoints::GetOrderedModRoots(
    const std::filesystem::path& mods_path) {

    std::scoped_lock lock{m_mutex};

    // get the order only once
    if (m_mods_initialized) {
        return m_active_mod_paths;
    }
    m_mods_initialized = true;

    if (!EmulatorSettings.IsModsEnabled() || !std::filesystem::exists(mods_path)) {
        return m_active_mod_paths;
    }

    CheckModConfig(mods_path);
    const auto json_path = mods_path / "mods.json";

    std::ifstream file(json_path);
    nlohmann::json config;
    try {
        file >> config;
    } catch (const nlohmann::json::exception& e) {
        LOG_WARNING(Core_Filesystem, "Failed to parse mods JSON: {}", e.what());
        return m_active_mod_paths;
    }

    if (config.is_array()) {
        for (const auto& mod : config) {
            // only load the mod if it's enabled
            if (mod.value("enabled", false) && mod.contains("name") && mod["name"].is_string()) {
                auto mod_path = mods_path / mod["name"].get<std::string>();

                if (std::filesystem::is_directory(mod_path)) {
                    m_active_mod_paths.push_back(std::move(mod_path));
                }
            }
        }
    }

    return m_active_mod_paths;
}

int HandleTable::CreateHandle() {
    std::scoped_lock lock{m_mutex};

    auto* file = new File{};
    file->is_opened = false;

    int existingFilesNum = m_files.size();

    for (int index = 0; index < existingFilesNum; index++) {
        if (m_files.at(index) == nullptr) {
            m_files[index] = file;
            return index;
        }
    }

    m_files.push_back(file);
    return m_files.size() - 1;
}

void HandleTable::DeleteHandle(int d) {
    std::scoped_lock lock{m_mutex};
    delete m_files.at(d);
    m_files[d] = nullptr;
}

File* HandleTable::GetFile(int d) {
    std::scoped_lock lock{m_mutex};
    if (d < 0 || d >= m_files.size()) {
        return nullptr;
    }
    return m_files.at(d);
}

File* HandleTable::GetSocket(int d) {
    std::scoped_lock lock{m_mutex};
    if (d < 0 || d >= m_files.size()) {
        return nullptr;
    }
    auto file = m_files.at(d);
    if (!file) {
        return nullptr;
    }
    if (file->type != Core::FileSys::FileType::Socket) {
        return nullptr;
    }
    return file;
}

File* HandleTable::GetEpoll(int d) {
    std::scoped_lock lock{m_mutex};
    if (d < 0 || d >= m_files.size()) {
        return nullptr;
    }
    auto file = m_files.at(d);
    if (file->type != Core::FileSys::FileType::Epoll) {
        return nullptr;
    }
    return file;
}

File* HandleTable::GetResolver(int d) {
    std::scoped_lock lock{m_mutex};
    if (d < 0 || d >= m_files.size()) {
        return nullptr;
    }
    auto file = m_files.at(d);
    if (file->type != Core::FileSys::FileType::Resolver) {
        return nullptr;
    }
    return file;
}

File* HandleTable::GetFile(const std::filesystem::path& host_name) {
    std::scoped_lock lock{m_mutex};
    for (auto* file : m_files) {
        if (file != nullptr && file->m_host_name == host_name) {
            return file;
        }
    }
    return nullptr;
}

void HandleTable::CreateStdHandles() {
    auto setup = [this](const char* path, auto* device) {
        int fd = CreateHandle();
        auto* file = GetFile(fd);
        file->is_opened = true;
        file->type = FileType::Device;
        file->m_guest_name = path;
        file->device =
            std::shared_ptr<Devices::BaseDevice>{reinterpret_cast<Devices::BaseDevice*>(device)};
    };
    // order matters
    setup("/dev/stdin", new Devices::Logger("stdin", false));   // stdin
    setup("/dev/stdout", new Devices::Logger("stdout", false)); // stdout
    setup("/dev/stderr", new Devices::Logger("stderr", true));  // stderr
}

int HandleTable::GetFileDescriptor(File* file) {
    std::scoped_lock lock{m_mutex};
    auto it = std::find(m_files.begin(), m_files.end(), file);

    if (it != m_files.end()) {
        return std::distance(m_files.begin(), it);
    }
    return 0;
}

} // namespace Core::FileSys
