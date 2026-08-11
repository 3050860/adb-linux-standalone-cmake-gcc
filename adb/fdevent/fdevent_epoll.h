#pragma once

#if defined(__linux__)

#include "sysdeps.h"

#include <sys/epoll.h>

#include <deque>
#include <list>
#include <mutex>
#include <unordered_map>

#include <android-base/thread_annotations.h>

#include "adb_unique_fd.h"
#include "fdevent.h"

struct fdevent_context_epoll final : public fdevent_context {
    fdevent_context_epoll();
    virtual ~fdevent_context_epoll();

    virtual void Register(fdevent* fde) final;
    virtual void Unregister(fdevent* fde) final;

    virtual void Set(fdevent* fde, unsigned events) final;

    virtual void Loop() final;
    size_t InstalledCount() final;

  protected:
    virtual void Interrupt() final;

  private:
    unique_fd epoll_fd_;
    unique_fd interrupt_fd_;
    fdevent* interrupt_fde_ = nullptr;
};

#endif  // defined(__linux__)
