// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#ifdef __ANDROID__ // FIX_LINUX jni touch
#include <jni.h>
#endif
#include "shared_mem.h"

namespace skyline::input {
    /*
     * @brief A description of a point being touched on the screen
     * @note All members are jint as it's treated as a jint array in Kotlin
     * @note This structure corresponds to TouchScreenStateData, see that for details
     */
    struct TouchScreenPoint {
#ifdef __ANDROID__ // FIX_LINUX jni touch
        jint attribute;
        jint id;
        jint x;
        jint y;
        jint minor;
        jint major;
        jint angle;
#else
        int32_t attribute;
        int32_t id;
        int32_t x;
        int32_t y;
        int32_t minor;
        int32_t major;
        int32_t angle;
#endif
    };

    /**
     * @brief This class is used to manage the shared memory responsible for touch-screen data
     */
    class TouchManager {
      private:
        const DeviceState &state;
        bool activated{};
        TouchScreenSection &section;

      public:
        /**
         * @param hid A pointer to HID Shared Memory on the host
         */
        TouchManager(const DeviceState &state, input::HidSharedMemory *hid);

        void Activate();

        void SetState(span<TouchScreenPoint> touchPoints);
    };
}
