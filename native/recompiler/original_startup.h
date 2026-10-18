#ifndef HMAWL_ORIGINAL_STARTUP_H
#define HMAWL_ORIGINAL_STARTUP_H

#include "dol_image.h"
#include "native_dispatch.h"
#include "rvz_image.h"

#include <filesystem>
#include <string>

enum class OriginalStartupStop { Dependency, Budget, UnexpectedReturn };

struct OriginalStartupResult {
    OriginalStartupStop stop;
    uint32_t pc;
    uint32_t lr;
    uint32_t stack;
    uint32_t argument3;
    uint32_t argument4;
    uint32_t argument5;
    std::string reason;
    bool osInitReturned = false;
    bool userInitReturned = false;
    bool gameMainEntered = false;
    bool memoryInitialized = false;
    bool screenInitEntered = false;
    bool gxInitEntered = false;
    bool gxInitReturned = false;
    bool viInitEntered = false;
    bool viInitReturned = false;
    bool viConfigured = false;
    bool viRetraceReturned = false;
    bool screenInitReturned = false;
    bool arInitReturned = false;
    bool arqInitReturned = false;
    bool aiInitReturned = false;
    bool dspInitReturned = false;
    bool dspTaskAdded = false;
    bool audioTaskInitReturned = false;
    bool pcmDmaStartReturned = false;
    bool gameInitializationReturned = false;
    bool gameLoopEntered = false;
    bool firstLoopPreparationReturned = false;
    bool sceneModuleLinked = false;
    bool scenePrologEntered = false;
    bool sceneInitializationEntered = false;
    bool scenePrologReturned = false;
    bool systemTaskReturned = false;
    uint32_t taskPassesReturned = 0;
    bool deferredUpdatesReturned = false;
    bool deferredUpdateQueueCleared = false;
    bool screenPreparationReturned = false;
    bool renderQueuePrepared = false;
    bool renderPassReturned = false;
    bool renderQueueCleared = false;
    bool frameDrawSyncReturned = false;
    bool postRenderCallbackReturned = false;
    bool frameRetraceWaitReturned = false;
    bool frameBufferExchangeReturned = false;
    bool frameTimingReturned = false;
    bool controllerUpdateReturned = false;
    uint32_t gameLoopIterationsCompleted = 0;
    uint32_t sceneConstructorsReturned = 0;
    uint32_t constructorsReturned = 0;
    uint32_t startupObjectsReturned = 0;
    uint32_t resourceReadNotifications = 0;
    uint32_t clzDecodesStarted = 0;
    uint32_t clzDecodesCompleted = 0;
    uint32_t clzSource = 0;
    uint32_t clzDestination = 0;
    uint32_t clzExpectedBytes = 0;
    uint32_t clzProducedBytes = 0;
    uint64_t clzDecoderBlockEntries = 0;
};

DolImage LoadOriginalUsDol(const std::filesystem::path& path);
PpcState CreateOriginalStartupState(const DolImage& image);
void InstallOriginalStartupDisc(PpcState& state, RvzImage& disc);
void RegisterOriginalStartup(NativeDispatcher& dispatcher, const PpcState& state);
OriginalStartupResult RunOriginalStartup(NativeDispatcher& dispatcher, PpcState& state,
                                        size_t blockBudget = 1000000,
                                        size_t maxCallDepth = 128,
                                        const std::function<void(uint32_t)>& onBlock = {});
std::string DescribeOriginalStartupStop(const OriginalStartupResult& result);

#endif
