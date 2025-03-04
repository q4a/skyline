// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2018-2020 fincs (https://github.com/devkitPro/deko3d)

#pragma once

#include <gpu/interconnect/blit_context.h>
#include "engine.h"

namespace skyline::soc::gm20b {
    struct ChannelContext;
}

namespace skyline::soc::gm20b::engine::fermi2d {
    /**
     * @brief The Fermi 2D engine handles perfoming blit and resolve operations
     */
    class Fermi2D : public MacroEngineBase {
      private:
        host1x::SyncpointSet &syncpoints;
        gpu::interconnect::BlitContext context;
        ChannelContext &channelCtx;

        /**
         * @brief Calls the appropriate function corresponding to a certain method with the supplied argument
         */
        void HandleMethod(u32 method, u32 argument);

      public:
        static constexpr u32 RegisterCount{0xE00}; //!< The number of Fermi 2D registers

        /**
         * @url https://github.com/devkitPro/deko3d/blob/master/source/maxwell/engine_2d.def
         */
        #pragma pack(push, 1)
        union Registers {
            std::array<u32, RegisterCount> raw;

            template<size_t Offset, typename Type>
            using Register = util::OffsetMember<Offset, Type, u32>;

            struct PixelsFromMemory {
                enum class BlockShapeV : u8 {
                    Auto = 0,
                    Shape8x8 = 1,
                    Shape16x4 = 2
                };

                enum class SampleModeOrigin : u8 {
                    Center = 0,
                    Corner = 1
                };

                enum class SampleModeFilter : u8 {
                    Point = 0,
                    Bilinear = 1
                };

                BlockShapeV blockShape : 3;
                u32 _pad0_ : 29;
                u16 corralSize : 10;
                u32 _pad1_ : 22;
                bool safeOverlap : 1;
                u32 _pad2_ : 31;
                struct {
                    SampleModeOrigin origin : 1;
                    u8 _pad0_ : 3;
                    SampleModeFilter filter : 1;
                    u32 _pad1_ : 27;
                } sampleMode;

                u32 _pad3_[8];

                i32 dstX0;
                i32 dstY0;
                i32 dstWidth;
                i32 dstHeight;
                i64 duDx;
                i64 dvDy;
                i64 srcX0;
                union {
                    i64 srcY0;
                    struct {
                        u32 _pad4_;
                        u32 trigger;
                    };
                };
            };

            Register<0x80, type::Surface> dst;
            Register<0x8C, type::Surface> src;
            Register<0x220, PixelsFromMemory> pixelsFromMemory;
        };
        static_assert(sizeof(Registers) == (RegisterCount * sizeof(u32)));
        #pragma pack(pop)

        Registers registers{};

        Fermi2D(const DeviceState &state, ChannelContext &channelCtx, MacroState &macroState, gpu::interconnect::CommandExecutor &executor);

        void CallMethodFromMacro(u32 method, u32 argument) override;

        u32 ReadMethodFromMacro(u32 method) override;

        void CallMethod(u32 method, u32 argument);
    };
}
