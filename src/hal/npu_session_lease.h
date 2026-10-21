#pragma once
// Both projects use this lease for the WHOLE native-context lifetime.
// This prevents known cross-runtime IOMMU failures; it does not enable parallel RKLLM/RKNN.
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

class NpuSessionLease {
    int fd_=-1;
    const std::string path_;
public:
    explicit NpuSessionLease(std::string path="/run/lock/rk3588-projects-npu.lock"):path_(std::move(path)){}
    NpuSessionLease(const NpuSessionLease&)=delete;
    NpuSessionLease& operator=(const NpuSessionLease&)=delete;
    ~NpuSessionLease(){if(fd_>=0)::close(fd_);}
    void acquire(bool exclusive) {
        if(fd_>=0)return;
        const int fd=::open(path_.c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0660);
        if(fd<0)throw std::runtime_error(std::string("Cannot open NPU session lock: ")+std::strerror(errno));
        struct stat st{};
        if(::fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_nlink!=1) {
            ::close(fd);throw std::runtime_error("Invalid NPU session lock file");
        }
        if(::flock(fd,(exclusive?LOCK_EX:LOCK_SH)|LOCK_NB)!=0) {
            const int error=errno;::close(fd);
            if(error==EWOULDBLOCK || error==EAGAIN)
                throw std::runtime_error("NPU occupied by an incompatible project session. Close its NPU model, or run project two with --cpu. Concurrent project-one RKNN and project-two RKLLM is not supported on the validated driver.");
            throw std::runtime_error(std::string("Cannot acquire NPU session lock: ")+std::strerror(error));
        }
        fd_=fd;
    }
};
