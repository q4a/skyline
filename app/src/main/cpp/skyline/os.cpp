// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "nce.h"
#include "nce/guest.h"
#include "kernel/types/KProcess.h"
#include "vfs/os_backing.h"
#include "loader/nro.h"
#include "loader/nso.h"
#include "loader/nca.h"
#include "loader/nsp.h"
#include "loader/xci.h"
#include "os.h"

namespace skyline::kernel {
    OS::OS(
#ifdef __ANDROID__ // FIX_LINUX jvm
        std::shared_ptr<JvmManager> &jvmManager,
#endif
        std::shared_ptr<Settings> &settings,
        std::string publicAppFilesPath,
        std::string privateAppFilesPath,
        std::string nativeLibraryPath,
        std::string deviceTimeZone,
        std::shared_ptr<vfs::FileSystem> assetFileSystem)
        : nativeLibraryPath(std::move(nativeLibraryPath)),
          publicAppFilesPath(std::move(publicAppFilesPath)),
          privateAppFilesPath(std::move(privateAppFilesPath)),
      #ifdef __ANDROID__ // FIX_LINUX jvm
          state(this, jvmManager, settings),
      #endif
          deviceTimeZone(std::move(deviceTimeZone)),
          assetFileSystem(std::move(assetFileSystem)),
          serviceManager(state) {}

    void OS::Execute(int romFd, loader::RomFormat romType) {
        auto romFile{std::make_shared<vfs::OsBacking>(romFd)};
        auto keyStore{std::make_shared<crypto::KeyStore>(privateAppFilesPath + "keys/")};

        state.loader = [&]() -> std::shared_ptr<loader::Loader> {
            switch (romType) {
                case loader::RomFormat::NRO:
                    return std::make_shared<loader::NroLoader>(std::move(romFile));
                case loader::RomFormat::NSO:
                    return std::make_shared<loader::NsoLoader>(std::move(romFile));
                case loader::RomFormat::NCA:
                    return std::make_shared<loader::NcaLoader>(std::move(romFile), std::move(keyStore));
                case loader::RomFormat::NSP:
                    return std::make_shared<loader::NspLoader>(romFile, keyStore);
                case loader::RomFormat::XCI:
                    return std::make_shared<loader::XciLoader>(romFile, keyStore);
                default:
                    throw exception("Unsupported ROM extension.");
            }
        }();

        auto &process{state.process};
        process = std::make_shared<kernel::type::KProcess>(state);
        auto entry{state.loader->LoadProcessData(process, state)};
        process->InitializeHeapTls();
        auto thread{process->CreateThread(entry)};
        if (thread) {
            Logger::Debug("Starting main HOS thread");
            thread->Start(true);
            process->Kill(true, true, true);
        }
    }
}
