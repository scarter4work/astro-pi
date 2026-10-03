#include "nukex/core/cube.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nukex {

namespace {
std::size_t record_bytes(std::size_t stride, int w, int h) {
    return stride * static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
}
} // namespace

Cube::Cube(int w, int h, const ChannelConfig& config)
    : width(w)
    , height(h)
    , channel_config(config)
    , n_frames_loaded(0)
    , allocated_channels_(config.n_channels)
    , stride_(voxel_record_size(config.n_channels))
    , bytes_(record_bytes(stride_, w, h))
    // Default-init, not value-init: the bytes are about to be constructed over
    // and zeroing them first would be a second full pass over what can be tens
    // of gigabytes. std::make_unique would value-initialize, so `new` is used
    // directly here.
    , storage_(new std::byte[bytes_])
    , fd_(-1)
{
    construct_all();
}

Cube::Cube(int w, int h, const ChannelConfig& config, const std::string& backing_dir)
    : width(w)
    , height(h)
    , channel_config(config)
    , n_frames_loaded(0)
    , allocated_channels_(config.n_channels)
    , stride_(voxel_record_size(config.n_channels))
    , bytes_(record_bytes(stride_, w, h))
    , storage_(nullptr)
    , fd_(-1)
{
#if defined(_WIN32)
    (void)backing_dir;
    throw std::runtime_error("Cube: file backing is not implemented on Windows");
#else
    std::string path = backing_dir + "/nukex_cube_XXXXXX";
    std::vector<char> tmpl(path.begin(), path.end());
    tmpl.push_back('\0');
    fd_ = ::mkstemp(tmpl.data());
    if (fd_ < 0)
        throw std::runtime_error("Cube: cannot create a backing file in " + backing_dir);
    // Unlink now: the mapping keeps the inode alive, and a crash -- or a
    // PixInsight force-exit -- leaves no multi-gigabyte file behind.
    ::unlink(tmpl.data());
    if (::ftruncate(fd_, static_cast<off_t>(bytes_)) != 0) {
        ::close(fd_); fd_ = -1;
        throw std::runtime_error("Cube: cannot size the backing file (" +
                                 std::to_string(bytes_) + " bytes) in " + backing_dir);
    }
    void* m = ::mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (m == MAP_FAILED) {
        ::close(fd_); fd_ = -1;
        throw std::runtime_error("Cube: mmap of the backing file failed");
    }
    storage_ = static_cast<std::byte*>(m);
    construct_all();
#endif
}

void Cube::construct_all() {
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t i = 0; i < n; i++)
        construct_voxel(storage_ + i * stride_, channel_config.n_channels);
}

void Cube::release() noexcept {
    if (storage_) {
#if !defined(_WIN32)
        if (fd_ >= 0) {
            ::munmap(storage_, bytes_);
            ::close(fd_);
            fd_ = -1;
            storage_ = nullptr;
            return;
        }
#endif
        delete[] storage_;
        storage_ = nullptr;
    }
}

Cube::~Cube() { release(); }

Cube::Cube(Cube&& other) noexcept
    : width(other.width)
    , height(other.height)
    , channel_config(std::move(other.channel_config))
    , n_frames_loaded(other.n_frames_loaded)
    , allocated_channels_(other.allocated_channels_)
    , stride_(other.stride_)
    , bytes_(other.bytes_)
    , storage_(other.storage_)
    , fd_(other.fd_)
{
    other.storage_ = nullptr;
    other.fd_ = -1;
    other.bytes_ = 0;
    other.width = other.height = 0;
}

Cube& Cube::operator=(Cube&& other) noexcept {
    if (this != &other) {
        release();
        width = other.width; height = other.height;
        channel_config = std::move(other.channel_config);
        n_frames_loaded = other.n_frames_loaded;
        allocated_channels_ = other.allocated_channels_;
        stride_ = other.stride_; bytes_ = other.bytes_;
        storage_ = other.storage_; fd_ = other.fd_;
        other.storage_ = nullptr; other.fd_ = -1; other.bytes_ = 0;
        other.width = other.height = 0;
    }
    return *this;
}

} // namespace nukex
