#pragma once

#include "FixSession.h"

#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace civ6fix {

// Window messages are process-global input. Keep object ownership here and use the
// window message only as a pointer-free wake-up signal.
class SessionStatusMailbox {
public:
    void Push(SessionStatus status) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(std::move(status));
    }

    std::optional<SessionStatus> TryPop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.empty()) {
            return std::nullopt;
        }
        SessionStatus status = std::move(pending_.front());
        pending_.pop_front();
        return status;
    }

private:
    std::mutex mutex_;
    std::deque<SessionStatus> pending_;
};

}  // namespace civ6fix
