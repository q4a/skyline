// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "settings.h"
#include "jvm.h"

namespace skyline {
    /**
     * @brief Handles settings on the android platform
     * @note Lifetime of this class must not exceed the one of the JNIEnv contained inside ktSettings
     */
    class AndroidSettings final : public Settings {
      private:
        KtSettings ktSettings;

      public:
        /**
         * @note Will construct the underlying KtSettings object in-place
         */
        AndroidSettings(JNIEnv *env, jobject settingsInstance) : ktSettings(env, settingsInstance) {
            Update();
        }

        /**
         * @note Will take ownership of the passed KtSettings object
         */
        AndroidSettings(KtSettings &&ktSettings) : ktSettings(std::move(ktSettings)) {
            Update();
        }

        void Update() override {
            isDocked = ktSettings.GetBool("isDocked");
            usernameValue = std::move(ktSettings.GetString("usernameValue"));
            systemLanguage = ktSettings.GetInt<skyline::language::SystemLanguage>("systemLanguage");
            forceTripleBuffering = ktSettings.GetBool("forceTripleBuffering");
            disableFrameThrottling = ktSettings.GetBool("disableFrameThrottling");
        };
    };
}
