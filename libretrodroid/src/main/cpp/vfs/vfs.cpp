/*
 *     Copyright (C) 2021  Filippo Scognamiglio
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "vfs.h"

#include <unistd.h>
#include <sys/stat.h>
#include <strings.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <optional>
#include <sstream>

#include "vfs/vfs_implementation.h"
#include "../log.h"
#include "../utils/utils.h"

namespace libretrodroid {

namespace {

std::string normalizePath(const char* raw) {
    if (raw == nullptr) return "";
    std::string input(raw);
    bool absolute = !input.empty() && input[0] == '/';
    std::vector<std::string> parts;
    std::stringstream ss(input);
    std::string segment;
    while (std::getline(ss, segment, '/')) {
        if (segment.empty() || segment == ".") continue;
        if (segment == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!absolute) {
                parts.push_back(segment);
            }
            continue;
        }
        parts.push_back(segment);
    }
    std::string result = absolute ? "/" : "";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += '/';
        result += parts[i];
    }
    return result;
}

} // namespace

const char *VFS::path(struct retro_vfs_file_handle* stream) {
    LOGV("VFS Calling path");
    auto& vfs = VFS::getInstance();
    std::lock_guard<std::mutex> lock(vfs.handlesMutex);
    auto it = vfs.virtualHandles.find(stream);
    if (it != vfs.virtualHandles.end()) {
        return it->second.path.c_str();
    }
    return retro_vfs_file_get_path_impl(stream);
}

struct retro_vfs_file_handle* VFS::open(const char* path, unsigned int mode, unsigned int hints) {
    LOGV("VFS Calling open: %s %i", path, mode);
    auto result = VFS::getInstance().virtualOpen(path, mode, hints);
    if (result != nullptr) {
        return result;
    }

    return retro_vfs_file_open_impl(path, mode, hints);
}

int VFS::close(struct retro_vfs_file_handle *stream) {
    LOGV("VFS Calling close");
    auto& vfs = VFS::getInstance();
    bool isVirtual = false;
    {
        std::lock_guard<std::mutex> lock(vfs.handlesMutex);
        isVirtual = vfs.virtualHandles.erase(stream) > 0;
    }
    if (isVirtual) {
        delete stream;
        return 0;
    }
    return retro_vfs_file_close_impl(stream);
}

int64_t VFS::size(struct retro_vfs_file_handle *stream) {
    LOGV("VFS Calling size");
    auto& vfs = VFS::getInstance();
    {
        std::lock_guard<std::mutex> lock(vfs.handlesMutex);
        auto it = vfs.virtualHandles.find(stream);
        if (it != vfs.virtualHandles.end()) {
            return it->second.size;
        }
    }
    return retro_vfs_file_size_impl(stream);
}

int64_t VFS::tell(struct retro_vfs_file_handle *stream) {
    LOGV("VFS Calling tell");
    auto& vfs = VFS::getInstance();
    {
        std::lock_guard<std::mutex> lock(vfs.handlesMutex);
        auto it = vfs.virtualHandles.find(stream);
        if (it != vfs.virtualHandles.end()) {
            return it->second.pos;
        }
    }
    return retro_vfs_file_tell_impl(stream);
}

int64_t VFS::seek(struct retro_vfs_file_handle *stream, int64_t offset, int seek_position) {
    LOGV("VFS Calling seek");
    auto& vfs = VFS::getInstance();
    {
        std::lock_guard<std::mutex> lock(vfs.handlesMutex);
        auto it = vfs.virtualHandles.find(stream);
        if (it != vfs.virtualHandles.end()) {
            int64_t newPos = 0;
            switch (seek_position) {
                case RETRO_VFS_SEEK_POSITION_START:
                    newPos = offset;
                    break;
                case RETRO_VFS_SEEK_POSITION_CURRENT:
                    newPos = it->second.pos + offset;
                    break;
                case RETRO_VFS_SEEK_POSITION_END:
                    newPos = it->second.size + offset;
                    break;
                default:
                    return -1;
            }
            if (newPos < 0) return -1;
            it->second.pos = newPos;
            return newPos;
        }
    }
    return retro_vfs_file_seek_impl(stream, offset, seek_position);
}

int64_t VFS::read(struct retro_vfs_file_handle *stream, void *s, uint64_t len) {
    LOGV("VFS Calling read");
    auto& vfs = VFS::getInstance();
    int fd = -1;
    int64_t pos = 0;
    int64_t size = 0;
    bool isVirtual = false;
    {
        std::lock_guard<std::mutex> lock(vfs.handlesMutex);
        auto it = vfs.virtualHandles.find(stream);
        if (it != vfs.virtualHandles.end()) {
            isVirtual = true;
            fd = it->second.fd;
            pos = it->second.pos;
            size = it->second.size;
        }
    }
    if (!isVirtual) {
        return retro_vfs_file_read_impl(stream, s, len);
    }

    uint64_t total = 0;
    while (total < len && pos < size) {
        size_t remaining = static_cast<size_t>(std::min<uint64_t>(len - total, static_cast<uint64_t>(size - pos)));
        ssize_t n = ::pread64(fd, static_cast<char*>(s) + total, remaining, pos);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        total += static_cast<uint64_t>(n);
        pos += n;
    }

    {
        std::lock_guard<std::mutex> lock(vfs.handlesMutex);
        auto it = vfs.virtualHandles.find(stream);
        if (it != vfs.virtualHandles.end()) {
            it->second.pos = pos;
        }
    }
    return static_cast<int64_t>(total);
}

int64_t VFS::write(struct retro_vfs_file_handle *stream, const void *s, uint64_t len) {
    LOGV("VFS Calling write");
    auto& vfs = VFS::getInstance();
    {
        std::lock_guard<std::mutex> lock(vfs.handlesMutex);
        if (vfs.virtualHandles.find(stream) != vfs.virtualHandles.end()) {
            return -1;
        }
    }
    return retro_vfs_file_write_impl(stream, s, len);
}

int VFS::flush(struct retro_vfs_file_handle *stream) {
    LOGV("VFS Calling flush");
    auto& vfs = VFS::getInstance();
    {
        std::lock_guard<std::mutex> lock(vfs.handlesMutex);
        if (vfs.virtualHandles.find(stream) != vfs.virtualHandles.end()) {
            return 0;
        }
    }
    return retro_vfs_file_flush_impl(stream);
}

int VFS::remove(const char *path) {
    LOGV("VFS Calling remove");
    return retro_vfs_file_remove_impl(path);
}

int VFS::rename(const char *old_path, const char *new_path) {
    LOGV("VFS Calling rename");
    return retro_vfs_file_rename_impl(old_path, new_path);
}

int64_t VFS::truncate(struct retro_vfs_file_handle* stream, int64_t length) {
    LOGV("VFS Calling truncate");
    auto& vfs = VFS::getInstance();
    {
        std::lock_guard<std::mutex> lock(vfs.handlesMutex);
        if (vfs.virtualHandles.find(stream) != vfs.virtualHandles.end()) {
            return -1;
        }
    }
    return retro_vfs_file_truncate_impl(stream, length);
}

// A virtual file must also answer stat as the file it stands for: cores check a path exists, and
// read its size, before opening it. Everything else is the real filesystem.
int VFS::stat(const char *path, int32_t *size) {
    int64_t size64 = 0;
    int flags = stat64(path, &size64);
    if (size != nullptr) {
        *size = size64 > INT32_MAX ? INT32_MAX : static_cast<int32_t>(size64);
    }
    return flags;
}

int VFS::stat64(const char *path, int64_t *size) {
    LOGV("VFS Calling stat: %s", path);
    VFSFile* virtualFile = VFS::getInstance().findVirtualFile(path);
    struct ::stat info {};
    // fstat, not lseek, for a virtual file: its position is shared with every stream open on it.
    int result = virtualFile != nullptr ? ::fstat(virtualFile->getFD(), &info) : ::stat(path, &info);
    if (result != 0) {
        return 0;
    }
    if (size != nullptr) {
        *size = info.st_size;
    }
    int flags = RETRO_VFS_STAT_IS_VALID;
    if (S_ISDIR(info.st_mode)) flags |= RETRO_VFS_STAT_IS_DIRECTORY;
    if (S_ISCHR(info.st_mode)) flags |= RETRO_VFS_STAT_IS_CHARACTER_SPECIAL;
    return flags;
}

int VFS::mkdir(const char *dir) {
    LOGV("VFS Calling mkdir: %s", dir);
    return retro_vfs_mkdir_impl(dir);
}

struct retro_vfs_dir_handle* VFS::opendir(const char *dir, bool include_hidden) {
    LOGV("VFS Calling opendir: %s", dir);
    return retro_vfs_opendir_impl(dir, include_hidden);
}

bool VFS::readdir(struct retro_vfs_dir_handle *dirstream) {
    return retro_vfs_readdir_impl(dirstream);
}

const char* VFS::direntGetName(struct retro_vfs_dir_handle *dirstream) {
    return retro_vfs_dirent_get_name_impl(dirstream);
}

bool VFS::direntIsDir(struct retro_vfs_dir_handle *dirstream) {
    return retro_vfs_dirent_is_dir_impl(dirstream);
}

int VFS::closedir(struct retro_vfs_dir_handle *dirstream) {
    return retro_vfs_closedir_impl(dirstream);
}

retro_vfs_interface * VFS::getInterface() {
    // The v4 table starts with the v3 one, so a core asking for any version reads a prefix of it.
    static retro_vfs_interface_v4 iface { retro_vfs_interface {
        /* Introduced in VFS API v1 */
        &VFS::path,
        &VFS::open,
        &VFS::close,
        &VFS::size,
        &VFS::tell,
        &VFS::seek,
        &VFS::read,
        &VFS::write,
        &VFS::flush,
        &VFS::remove,
        &VFS::rename,

        /* Introduced in VFS API v2 */
        &VFS::truncate,

        /* Introduced in VFS API v3 */
        &VFS::stat,
        &VFS::mkdir,
        &VFS::opendir,
        &VFS::readdir,
        &VFS::direntGetName,
        &VFS::direntIsDir,
        &VFS::closedir
    }, &VFS::stat64 };
    return &iface.v3;
}

void VFS::initialize(std::vector<VFSFile> files) {
    std::lock_guard<std::mutex> lock(handlesMutex);
    for (auto& [handle, _] : virtualHandles) {
        delete handle;
    }
    virtualHandles.clear();
    this->virtualFiles = std::move(files);
}

void VFS::deinitialize() {
    std::lock_guard<std::mutex> lock(handlesMutex);
    for (auto& [handle, _] : virtualHandles) {
        delete handle;
    }
    virtualHandles.clear();
    virtualFiles.clear();
}

struct retro_vfs_file_handle* VFS::virtualOpen(const char *path, unsigned int mode, unsigned int hints) {
    LOGV("VFS Calling open: %s %i", path, mode);

    if ((mode & RETRO_VFS_FILE_ACCESS_WRITE) != 0) {
        return nullptr;
    }

    VFSFile* virtualFile = findVirtualFile(path);

    if (virtualFile == nullptr) {
        return nullptr;
    }

    struct ::stat info {};
    if (::fstat(virtualFile->getFD(), &info) != 0) {
        return nullptr;
    }

    LOGD("VFS Performing virtual file open: %s", virtualFile->getFileName().data());

    auto stream = new retro_vfs_file_handle{};
    {
        std::lock_guard<std::mutex> lock(handlesMutex);
        virtualHandles.emplace(stream, VirtualHandle {
            virtualFile->getFD(),
            0,
            static_cast<int64_t>(info.st_size),
            virtualFile->getFileName(),
        });
    }

    return stream;
}

int VFS::duplicateVirtualFD(const char* path) {
    VFSFile* virtualFile = findVirtualFile(path);
    return virtualFile != nullptr ? dup(virtualFile->getFD()) : -1;
}

VFSFile* VFS::findVirtualFile(const char *path) {
    std::string target = normalizePath(path);
    if (target.empty()) return nullptr;

    for (auto& virtualFile : virtualFiles) {
        if (target == normalizePath(virtualFile.getFileName().data())) {
            return &virtualFile;
        }
    }

    // Fall back to case-insensitive basename matching within the same directory (handles .cue/.gdi
    // sheets whose track names differ in case from the library file).
    auto slash = target.rfind('/');
    if (slash == std::string::npos) return nullptr;
    std::string targetDir = target.substr(0, slash);
    std::string targetBase = target.substr(slash + 1);

    for (auto& virtualFile : virtualFiles) {
        std::string cand = normalizePath(virtualFile.getFileName().data());
        auto cSlash = cand.rfind('/');
        if (cSlash == std::string::npos) continue;
        if (cand.substr(0, cSlash) == targetDir &&
            ::strcasecmp(cand.substr(cSlash + 1).c_str(), targetBase.c_str()) == 0) {
            return &virtualFile;
        }
    }
    return nullptr;
}

} // namespace libretrodroid
