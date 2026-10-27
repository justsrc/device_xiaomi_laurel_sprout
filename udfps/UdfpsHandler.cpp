/*
 * Copyright (C) 2022 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UdfpsHandler.laurel_sprout"

#include "UdfpsHandler.h"

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <android-base/logging.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

#define COMMAND_NIT 10
#define PARAM_NIT_FOD 1
#define PARAM_NIT_NONE 0

/*
 * Goodix vendor acquired codes (reported as AcquiredInfo::VENDOR):
 *   20 - UI ready: HBM/green spot up, sensor capture armed (after extCmd(NIT, 1))
 *   21 - waiting for finger (authentication/enrollment session armed)
 *   22 - finger down
 *   23 - finger up
 */
#define GF_ACQUIRED_UI_READY 20
#define GF_ACQUIRED_WAITING_FINGER 21
#define GF_ACQUIRED_FINGER_DOWN 22
#define GF_ACQUIRED_FINGER_UP 23

using ::aidl::android::hardware::biometrics::fingerprint::AcquiredInfo;

static const char* kFodUiPaths[] = {
        "/sys/devices/platform/soc/soc:qcom,dsi-display-primary/fod_ui",
        "/sys/devices/platform/soc/soc:qcom,dsi-display/fod_ui",
};

static const char* kFodStatusPath = "/sys/class/touch/tp_dev/fod_status";

static bool readBool(int fd) {
    char buf[8] = {0};

    if (lseek(fd, 0, SEEK_SET) < 0) {
        LOG(ERROR) << "failed to seek fd: " << strerror(errno);
        return false;
    }

    ssize_t rc = read(fd, buf, sizeof(buf) - 1);
    if (rc < 1) {
        LOG(ERROR) << "failed to read bool from fd: " << strerror(errno);
        return false;
    }

    return buf[0] != '0';
}

class LaurelSproutUdfpsHandler : public UdfpsHandler {
  public:
    LaurelSproutUdfpsHandler() = default;
    ~LaurelSproutUdfpsHandler() override;

    void init(fingerprint_device_t* device) override;
    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) override;
    void onFingerUp() override;
    void onAcquired(int32_t result, int32_t vendorCode) override;
    void onAuthenticationSucceeded() override;
    void onAuthenticationFailed() override;
    void cancel() override;

  private:
    void workerLoop();
    void requestUpdate();
    void applyDesiredState();
    void finishAuthentication();

    fingerprint_device_t* mDevice = nullptr;

    /*
     * Desired state: written by any thread, applied only by the worker thread.
     * The touch controller driver blocks ~300 ms in the kernel for every write
     * to fod_status, so all sysfs/extCmd work must stay off the hot paths (the
     * HAL notify/binder threads and the poll loop).
     */
    std::mutex mLock;
    bool mFodUi = false;        /* kernel fod_ui: FOD dim layer visible */
    bool mSessionArmed = false; /* vendor 21/22: a session wants FOD mode */
    bool mFingerDown = false;   /* finger on the sensor */
    bool mStop = false;

    /* Last values actually applied, to skip redundant writes. */
    bool mAppliedFodStatus = false;
    bool mAppliedNit = false;

    int mFodUiFd = -1;
    int mFodStatusFd = -1;
    int mWakeFd = -1; /* eventfd used to wake the worker without a poll timeout */
    std::thread mWorker;
};

void LaurelSproutUdfpsHandler::init(fingerprint_device_t* device) {
    mDevice = device;

    int fd = -1;
    for (auto& path : kFodUiPaths) {
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            break;
        }
    }

    if (fd < 0) {
        LOG(ERROR) << "failed to open fod_ui: " << strerror(errno);
        return;
    }
    mFodUiFd = fd;

    mFodStatusFd = open(kFodStatusPath, O_RDWR);
    if (mFodStatusFd < 0) {
        LOG(ERROR) << "failed to open " << kFodStatusPath << ": " << strerror(errno);
        /* Keep going: NIT handling still works, fod_status writes are skipped. */
    }

    mWakeFd = eventfd(0, EFD_CLOEXEC);
    if (mWakeFd < 0) {
        LOG(ERROR) << "failed to create eventfd: " << strerror(errno);
        close(mFodUiFd);
        mFodUiFd = -1;
        if (mFodStatusFd >= 0) {
            close(mFodStatusFd);
            mFodStatusFd = -1;
        }
        return;
    }

    mWorker = std::thread(&LaurelSproutUdfpsHandler::workerLoop, this);
}

LaurelSproutUdfpsHandler::~LaurelSproutUdfpsHandler() {
    {
        std::lock_guard lock(mLock);
        mStop = true;
    }

    if (mWakeFd >= 0) {
        uint64_t one = 1;
        (void)write(mWakeFd, &one, sizeof(one));
    }

    if (mWorker.joinable()) {
        mWorker.join();
    }

    if (mFodUiFd >= 0) {
        close(mFodUiFd);
    }
    if (mFodStatusFd >= 0) {
        close(mFodStatusFd);
    }
    if (mWakeFd >= 0) {
        close(mWakeFd);
    }
}

void LaurelSproutUdfpsHandler::workerLoop() {
    struct pollfd pollFds[] = {
            {.fd = mFodUiFd, .events = POLLERR | POLLPRI, .revents = 0},
            {.fd = mWakeFd, .events = POLLIN, .revents = 0},
    };

    /* Apply the current fod_ui state before waiting: if this service was
     * (re)started while the FOD circle is already shown, poll() would not wake
     * until the next fod_ui transition and the hole would stay dark. */
    {
        std::lock_guard lock(mLock);
        mFodUi = readBool(mFodUiFd);
        LOG(INFO) << "worker started, initial fod_ui: " << (mFodUi ? "1" : "0");
    }
    applyDesiredState();

    while (true) {
        pollFds[0].revents = 0;
        pollFds[1].revents = 0;

        int rc = poll(pollFds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG(ERROR) << "failed to poll fod_ui/eventfd: " << strerror(errno);
            break;
        }

        {
            std::lock_guard lock(mLock);
            if (mStop) {
                break;
            }
        }

        if (pollFds[0].revents & (POLLERR | POLLPRI)) {
            bool value = readBool(mFodUiFd);
            std::lock_guard lock(mLock);
            if (value != mFodUi) {
                mFodUi = value;
                LOG(INFO) << "fod_ui -> " << (value ? "1" : "0");
            }
        }

        if (pollFds[1].revents & POLLIN) {
            uint64_t ignored = 0;
            (void)read(mWakeFd, &ignored, sizeof(ignored));
        }

        applyDesiredState();
    }
}

void LaurelSproutUdfpsHandler::requestUpdate() {
    if (mWakeFd < 0) {
        return;
    }

    uint64_t one = 1;
    if (write(mWakeFd, &one, sizeof(one)) < 0 && errno != EAGAIN) {
        LOG(ERROR) << "failed to write eventfd: " << strerror(errno);
    }
}

void LaurelSproutUdfpsHandler::applyDesiredState() {
    bool fodUi;
    bool sessionArmed;
    bool fingerDown;
    {
        std::lock_guard lock(mLock);
        if (mStop) {
            return;
        }
        fodUi = mFodUi;
        sessionArmed = mSessionArmed;
        fingerDown = mFingerDown;
    }

    /*
     * The touch controller is put into FOD mode as soon as the session is
     * armed or the circle is visible. NIT needs the panel HBM/green spot,
     * which only exists when the dim layer is up (fod_ui) or the finger is
     * already on the sensor.
     */
    bool fodStatus = fodUi || sessionArmed || fingerDown;
    bool nit = fodUi || fingerDown;

    if (nit != mAppliedNit) {
        if (mDevice && mDevice->extCmd) {
            int rc = mDevice->extCmd(mDevice, COMMAND_NIT,
                                     nit ? PARAM_NIT_FOD : PARAM_NIT_NONE);
            if (rc == 0) {
                mAppliedNit = nit;
                LOG(INFO) << "NIT " << (nit ? "on" : "off");
            } else {
                LOG(ERROR) << "failed to set NIT " << (nit ? "on" : "off") << ": " << rc;
            }
        }
    }

    if (mFodStatusFd >= 0 && fodStatus != mAppliedFodStatus) {
        if (write(mFodStatusFd, fodStatus ? "1" : "0", 1) == 1) {
            mAppliedFodStatus = fodStatus;
            LOG(INFO) << "fod_status -> " << (fodStatus ? "1" : "0");
        } else {
            LOG(ERROR) << "failed to write fod_status: " << strerror(errno);
        }
    }
}

void LaurelSproutUdfpsHandler::onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/,
                                            float /*major*/) {
    /* SystemUI hands us the touch over the FOD area even when the kernel
     * fod_ui path is unreliable (e.g. BiometricPrompt dialogs). Arm here. */
    {
        std::lock_guard lock(mLock);
        mSessionArmed = true;
        mFingerDown = true;
    }
    requestUpdate();
}

void LaurelSproutUdfpsHandler::onFingerUp() {
    {
        std::lock_guard lock(mLock);
        mFingerDown = false;
        mSessionArmed = false;
    }
    requestUpdate();
}

void LaurelSproutUdfpsHandler::onAcquired(int32_t result, int32_t vendorCode) {
    if (static_cast<AcquiredInfo>(result) != AcquiredInfo::VENDOR) {
        return;
    }

    switch (vendorCode) {
        case GF_ACQUIRED_WAITING_FINGER:
            /* Session armed, waiting for the finger: put the touch controller
             * into FOD mode right away so the hole press is detected even when
             * no FOD dim layer is presented (in-app dialogs/popups). */
            {
                std::lock_guard lock(mLock);
                mSessionArmed = true;
            }
            break;
        case GF_ACQUIRED_UI_READY:
            /* Sensor capture unblocked by our extCmd(NIT, 1); nothing to do. */
            return;
        case GF_ACQUIRED_FINGER_DOWN: {
            std::lock_guard lock(mLock);
            mSessionArmed = true;
            mFingerDown = true;
        } break;
        case GF_ACQUIRED_FINGER_UP: {
            std::lock_guard lock(mLock);
            mFingerDown = false;
            mSessionArmed = false;
        } break;
        default:
            return;
    }

    requestUpdate();
}

void LaurelSproutUdfpsHandler::onAuthenticationSucceeded() {
    finishAuthentication();
}

void LaurelSproutUdfpsHandler::onAuthenticationFailed() {
    finishAuthentication();
}

void LaurelSproutUdfpsHandler::cancel() {
    finishAuthentication();
}

void LaurelSproutUdfpsHandler::finishAuthentication() {
    {
        std::lock_guard lock(mLock);
        mSessionArmed = false;
        mFingerDown = false;
    }
    requestUpdate();
}

static UdfpsHandler* create() {
    return new LaurelSproutUdfpsHandler();
}

static void destroy(UdfpsHandler* handler) {
    delete handler;
}

extern "C" UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
        .create = create,
        .destroy = destroy,
};
