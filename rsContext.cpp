/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "rs.h"
#include "rsDevice.h"
#include "rsContext.h"
#include "rsThreadIO.h"
#include "rsMesh.h"
#include <ui/FramebufferNativeWindow.h>
#include <gui/DisplayEventReceiver.h>

#include <sys/types.h>
#include <sys/resource.h>
#include <sched.h>

#include <cutils/properties.h>

#include <sys/syscall.h>

#include <dlfcn.h>

using namespace android;
using namespace android::renderscript;

pthread_mutex_t Context::gInitMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t Context::gLibMutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef PVR_RSC
/*
 * Load an external library and lookup the rsdHalInit function symbol.
 */
bool rsLoadExternalLibrary(void *vrsc, const char *pszLibName, void **phLib)
{
    Context *rsc  = static_cast<Context *>(vrsc);
    void    *hLib = dlopen(pszLibName, RTLD_LAZY);

    if(!hLib){
        ALOGE("Failed to load external library %s (error: %s)", pszLibName, dlerror());
        return false;
    }

    RsHalInitFunc rsdHalInit = (RsHalInitFunc)dlsym(hLib,"rsdHalInit");

    if(!rsdHalInit){
        ALOGE("Failed to resolve rsdHalInit symbol from %s, error: %s", pszLibName, dlerror());
        dlclose(hLib);
        return false;
    }

    if (!rsdHalInit((RsContext)rsc,0,0)) {
        ALOGE("Hal init failed (%s)",pszLibName);
        return false;
    }

    if(phLib){
        *phLib = hLib;
    }

    ALOGD("Loaded external library \'%s\' successfully.",pszLibName);
    return true;
}
#endif

bool Context::initGLThread() {
    pthread_mutex_lock(&gInitMutex);

    if (!mHal.funcs.initGraphics(this)) {
        pthread_mutex_unlock(&gInitMutex);
        ALOGE("%p initGraphics failed", this);
        return false;
    }

    pthread_mutex_unlock(&gInitMutex);
    return true;
}

void Context::deinitEGL() {
    mHal.funcs.shutdownGraphics(this);
}

Context::PushState::PushState(Context *con) {
    mRsc = con;
    if (con->mIsGraphicsContext) {
        mFragment.set(con->getProgramFragment());
        mVertex.set(con->getProgramVertex());
        mStore.set(con->getProgramStore());
        mRaster.set(con->getProgramRaster());
        mFont.set(con->getFont());
    }
}

Context::PushState::~PushState() {
    if (mRsc->mIsGraphicsContext) {
        mRsc->setProgramFragment(mFragment.get());
        mRsc->setProgramVertex(mVertex.get());
        mRsc->setProgramStore(mStore.get());
        mRsc->setProgramRaster(mRaster.get());
        mRsc->setFont(mFont.get());
    }
}


uint32_t Context::runScript(Script *s) {
    PushState ps(this);

    uint32_t ret = s->run(this);
    return ret;
}

uint32_t Context::runRootScript() {
    timerSet(RS_TIMER_SCRIPT);
    mStateFragmentStore.mLast.clear();
    watchdog.inRoot = true;
    uint32_t ret = runScript(mRootScript.get());
    watchdog.inRoot = false;

    return ret;
}

uint64_t Context::getTime() const {
#ifndef ANDROID_RS_SERIALIZE
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_nsec + ((uint64_t)t.tv_sec * 1000 * 1000 * 1000);
#else
    return 0;
#endif //ANDROID_RS_SERIALIZE
}

void Context::timerReset() {
    for (int ct=0; ct < _RS_TIMER_TOTAL; ct++) {
        mTimers[ct] = 0;
    }
}

void Context::timerInit() {
    mTimeLast = getTime();
    mTimeFrame = mTimeLast;
    mTimeLastFrame = mTimeLast;
    mTimerActive = RS_TIMER_INTERNAL;
    mAverageFPSFrameCount = 0;
    mAverageFPSStartTime = mTimeLast;
    mAverageFPS = 0;
    timerReset();
}

void Context::timerFrame() {
    mTimeLastFrame = mTimeFrame;
    mTimeFrame = getTime();
    // Update average fps
    const uint64_t averageFramerateInterval = 1000 * 1000000;
    mAverageFPSFrameCount ++;
    uint64_t inverval = mTimeFrame - mAverageFPSStartTime;
    if (inverval >= averageFramerateInterval) {
        inverval = inverval / 1000000;
        mAverageFPS = (mAverageFPSFrameCount * 1000) / inverval;
        mAverageFPSFrameCount = 0;
        mAverageFPSStartTime = mTimeFrame;
    }
}

void Context::timerSet(Timers tm) {
    uint64_t last = mTimeLast;
    mTimeLast = getTime();
    mTimers[mTimerActive] += mTimeLast - last;
    mTimerActive = tm;
}

void Context::timerPrint() {
    double total = 0;
    for (int ct = 0; ct < _RS_TIMER_TOTAL; ct++) {
        total += mTimers[ct];
    }
    uint64_t frame = mTimeFrame - mTimeLastFrame;
    mTimeMSLastFrame = frame / 1000000;
    mTimeMSLastScript = mTimers[RS_TIMER_SCRIPT] / 1000000;
    mTimeMSLastSwap = mTimers[RS_TIMER_CLEAR_SWAP] / 1000000;


    if (props.mLogTimes) {
        ALOGV("RS: Frame (%i),   Script %2.1f%% (%i),  Swap %2.1f%% (%i),  Idle %2.1f%% (%lli),  Internal %2.1f%% (%lli), Avg fps: %u",
             mTimeMSLastFrame,
             100.0 * mTimers[RS_TIMER_SCRIPT] / total, mTimeMSLastScript,
             100.0 * mTimers[RS_TIMER_CLEAR_SWAP] / total, mTimeMSLastSwap,
             100.0 * mTimers[RS_TIMER_IDLE] / total, mTimers[RS_TIMER_IDLE] / 1000000,
             100.0 * mTimers[RS_TIMER_INTERNAL] / total, mTimers[RS_TIMER_INTERNAL] / 1000000,
             mAverageFPS);
    }
}

bool Context::setupCheck() {

    mFragmentStore->setup(this, &mStateFragmentStore);
    mFragment->setup(this, &mStateFragment);
    mRaster->setup(this, &mStateRaster);
    mVertex->setup(this, &mStateVertex);
    mFBOCache.setup(this);
    return true;
}

void Context::setupProgramStore() {
    mFragmentStore->setup(this, &mStateFragmentStore);
}

static uint32_t getProp(const char *str) {
    char buf[PROPERTY_VALUE_MAX];
    property_get(str, buf, "0");
    return atoi(buf);
}

void Context::displayDebugStats() {
    char buffer[128];
    sprintf(buffer, "Avg fps %u, Frame %i ms, Script %i ms", mAverageFPS, mTimeMSLastFrame, mTimeMSLastScript);
    float oldR, oldG, oldB, oldA;
    mStateFont.getFontColor(&oldR, &oldG, &oldB, &oldA);
    uint32_t bufferLen = strlen(buffer);

    ObjectBaseRef<Font> lastFont(getFont());
    setFont(NULL);
    float shadowCol = 0.1f;
    mStateFont.setFontColor(shadowCol, shadowCol, shadowCol, 1.0f);
    mStateFont.renderText(buffer, bufferLen, 5, getHeight() - 6);

    mStateFont.setFontColor(1.0f, 0.7f, 0.0f, 1.0f);
    mStateFont.renderText(buffer, bufferLen, 4, getHeight() - 7);

    setFont(lastFont.get());
    mStateFont.setFontColor(oldR, oldG, oldB, oldA);
}

void * Context::threadProc(void *vrsc) {
    Context *rsc = static_cast<Context *>(vrsc);
#ifndef ANDROID_RS_SERIALIZE
    rsc->mNativeThreadId = gettid();
    setpriority(PRIO_PROCESS, rsc->mNativeThreadId, ANDROID_PRIORITY_DISPLAY);
    rsc->mThreadPriority = ANDROID_PRIORITY_DISPLAY;
#endif //ANDROID_RS_SERIALIZE
    rsc->props.mLogTimes = getProp("debug.rs.profile") != 0;
    rsc->props.mLogScripts = getProp("debug.rs.script") != 0;
    rsc->props.mLogObjects = getProp("debug.rs.object") != 0;
    rsc->props.mLogShaders = getProp("debug.rs.shader") != 0;
    rsc->props.mLogShadersAttr = getProp("debug.rs.shader.attributes") != 0;
    rsc->props.mLogShadersUniforms = getProp("debug.rs.shader.uniforms") != 0;
    rsc->props.mLogVisual = getProp("debug.rs.visual") != 0;
    rsc->props.mDebugMaxThreads = getProp("debug.rs.max-threads");

    void *driverSO = NULL;

    // Provide a mechanism for dropping in a different RS driver.
#ifdef OVERRIDE_RS_DRIVER
#define XSTR(S) #S
#define STR(S) XSTR(S)
#define OVERRIDE_RS_DRIVER_STRING STR(OVERRIDE_RS_DRIVER)
    if (getProp("debug.rs.default-CPU-driver") != 0) {
        ALOGE("Skipping override driver and loading default CPU driver");
    } else {
        driverSO = dlopen(OVERRIDE_RS_DRIVER_STRING, RTLD_LAZY);
        if (driverSO == NULL) {
            ALOGE("Failed loading %s: %s", OVERRIDE_RS_DRIVER_STRING,
                  dlerror());
            // Continue to attempt loading fallback driver
        }
    }

#undef XSTR
#undef STR
#endif  // OVERRIDE_RS_DRIVER

    // Attempt to load the reference RS driver (if necessary).
    if (driverSO == NULL) {
        driverSO = dlopen("libRSDriver.so", RTLD_LAZY);
        if (driverSO == NULL) {
            rsc->setError(RS_ERROR_FATAL_DRIVER, "Failed loading RS driver");
            ALOGE("Failed loading RS driver: %s", dlerror());
            return NULL;
        }
    }

    // Need to call dlerror() to clear buffer before using it for dlsym().
    (void) dlerror();
    typedef bool (*HalSig)(Context*, uint32_t, uint32_t);
    HalSig halInit = (HalSig) dlsym(driverSO, "rsdHalInit");

    // If we can't find the C variant, we go looking for the C++ version.
    if (halInit == NULL) {
        ALOGW("Falling back to find C++ rsdHalInit: %s", dlerror());
        halInit = (HalSig) dlsym(driverSO,
                "_Z10rsdHalInitPN7android12renderscript7ContextEjj");
    }

    if (halInit == NULL) {
        rsc->setError(RS_ERROR_FATAL_DRIVER, "Failed to find rsdHalInit");
        dlclose(driverSO);
        ALOGE("Failed to find rsdHalInit: %s", dlerror());
        return NULL;
    }

    if (!(*halInit)(rsc, 0, 0)) {
        rsc->setError(RS_ERROR_FATAL_DRIVER, "Failed initializing RS Driver");
        dlclose(driverSO);
        ALOGE("Hal init failed");
        return NULL;
    }

#ifdef PVR_RSC
    ALOGD("Loading libPVRRS.so PowerVR Renderscript Driver");
    if (!rsLoadExternalLibrary(vrsc,"libPVRRS.so",&rsc->mLib))
    {
        ALOGE("Failed intializing PowerVR driver");
    }
#endif

    rsc->mHal.funcs.setPriority(rsc, rsc->mThreadPriority);

    if (rsc->mIsGraphicsContext) {
        if (!rsc->initGLThread()) {
            rsc->setError(RS_ERROR_OUT_OF_MEMORY, "Failed initializing GL");
            return NULL;
        }

        rsc->mStateRaster.init(rsc);
        rsc->setProgramRaster(NULL);
        rsc->mStateVertex.init(rsc);
        rsc->setProgramVertex(NULL);
        rsc->mStateFragment.init(rsc);
        rsc->setProgramFragment(NULL);
        rsc->mStateFragmentStore.init(rsc);
        rsc->setProgramStore(NULL);
        rsc->mStateFont.init(rsc);
        rsc->setFont(NULL);
        rsc->mStateSampler.init(rsc);
        rsc->mFBOCache.init(rsc);
    }

    rsc->mRunning = true;
    if (!rsc->mIsGraphicsContext) {
        while (!rsc->mExit) {
            rsc->mIO.playCoreCommands(rsc, -1);
        }
    } else {
#ifndef ANDROID_RS_SERIALIZE
        DisplayEventReceiver displayEvent;
        DisplayEventReceiver::Event eventBuffer[1];
#endif
        int vsyncRate = 0;
        int targetRate = 0;

        bool drawOnce = false;
        while (!rsc->mExit) {
            rsc->timerSet(RS_TIMER_IDLE);

#ifndef ANDROID_RS_SERIALIZE
            if (!rsc->mRootScript.get() || !rsc->mHasSurface || rsc->mPaused) {
                targetRate = 0;
            }

            if (vsyncRate != targetRate) {
                displayEvent.setVsyncRate(targetRate);
                vsyncRate = targetRate;
            }
            if (targetRate) {
                drawOnce |= rsc->mIO.playCoreCommands(rsc, displayEvent.getFd());
                while (displayEvent.getEvents(eventBuffer, 1) != 0) {
                    //ALOGE("vs2 time past %lld", (rsc->getTime() - eventBuffer[0].header.timestamp) / 1000000);
                }
            } else
#endif
            {
                drawOnce |= rsc->mIO.playCoreCommands(rsc, -1);
            }

            if ((rsc->mRootScript.get() != NULL) && rsc->mHasSurface &&
                (targetRate || drawOnce) && !rsc->mPaused) {

                drawOnce = false;
                targetRate = ((rsc->runRootScript() + 15) / 16);

                if (rsc->props.mLogVisual) {
                    rsc->displayDebugStats();
                }

                rsc->timerSet(RS_TIMER_CLEAR_SWAP);
                rsc->mHal.funcs.swap(rsc);
                rsc->timerFrame();
                rsc->timerSet(RS_TIMER_INTERNAL);
                rsc->timerPrint();
                rsc->timerReset();
            }
        }
    }

    ALOGV("%p RS Thread exiting", rsc);

    if (rsc->mIsGraphicsContext) {
        pthread_mutex_lock(&gInitMutex);
        rsc->deinitEGL();
        pthread_mutex_unlock(&gInitMutex);
    }

    ALOGV("%p RS Thread exited", rsc);
    return NULL;
}

void Context::destroyWorkerThreadResources() {
    //ALOGV("destroyWorkerThreadResources 1");
    ObjectBase::zeroAllUserRef(this);
    if (mIsGraphicsContext) {
         mRaster.clear();
         mFragment.clear();
         mVertex.clear();
         mFragmentStore.clear();
         mFont.clear();
         mRootScript.clear();
         mStateRaster.deinit(this);
         mStateVertex.deinit(this);
         mStateFragment.deinit(this);
         mStateFragmentStore.deinit(this);
         mStateFont.deinit(this);
         mStateSampler.deinit(this);
         mFBOCache.deinit(this);
    }
    ObjectBase::freeAllChildren(this);
    mExit = true;
    //ALOGV("destroyWorkerThreadResources 2");
}

void Context::printWatchdogInfo(void *ctx) {
    Context *rsc = (Context *)ctx;
    if (rsc->watchdog.command && rsc->watchdog.file) {
        ALOGE("RS watchdog timeout: %i  %s  line %i %s", rsc->watchdog.inRoot,
             rsc->watchdog.command, rsc->watchdog.line, rsc->watchdog.file);
    } else {
        ALOGE("RS watchdog timeout: %i", rsc->watchdog.inRoot);
    }
}


void Context::setPriority(int32_t p) {
    // Note: If we put this in the proper "background" policy
    // the wallpapers can become completly unresponsive at times.
    // This is probably not what we want for something the user is actively
    // looking at.
    mThreadPriority = p;
    setpriority(PRIO_PROCESS, mNativeThreadId, p);
    mHal.funcs.setPriority(this, mThreadPriority);
}

Context::Context() {
    mDev = NULL;
    mRunning = false;
    mExit = false;
    mPaused = false;
    mObjHead = NULL;
    mError = RS_ERROR_NONE;
    mTargetSdkVersion = 14;
    mDPI = 96;
    mIsContextLite = false;
#ifdef PVR_RSC
    mLib = NULL;
#endif
    memset(&watchdog, 0, sizeof(watchdog));
}

Context * Context::createContext(Device *dev, const RsSurfaceConfig *sc) {
    Context * rsc = new Context();

    if (!rsc->initContext(dev, sc)) {
        delete rsc;
        return NULL;
    }
    return rsc;
}

Context * Context::createContextLite() {
    Context * rsc = new Context();
    rsc->mIsContextLite = true;
    return rsc;
}

bool Context::initContext(Device *dev, const RsSurfaceConfig *sc) {
    pthread_mutex_lock(&gInitMutex);

    mIO.init();
    mIO.setTimeoutCallback(printWatchdogInfo, this, 2e9);

    dev->addContext(this);
    mDev = dev;
    if (sc) {
        mUserSurfaceConfig = *sc;
    } else {
        memset(&mUserSurfaceConfig, 0, sizeof(mUserSurfaceConfig));
    }

    mIsGraphicsContext = sc != NULL;

    int status;
    pthread_attr_t threadAttr;

    pthread_mutex_unlock(&gInitMutex);

    // Global init done at this point.

    status = pthread_attr_init(&threadAttr);
    if (status) {
        ALOGE("Failed to init thread attribute.");
        return false;
    }

    mHasSurface = false;

    timerInit();
    timerSet(RS_TIMER_INTERNAL);

    status = pthread_create(&mThreadId, &threadAttr, threadProc, this);
    if (status) {
        ALOGE("Failed to start rs context thread.");
        return false;
    }
    while (!mRunning && (mError == RS_ERROR_NONE)) {
        usleep(100);
    }

    if (mError != RS_ERROR_NONE) {
        ALOGE("Errors during thread init");
        return false;
    }

    pthread_attr_destroy(&threadAttr);
    return true;
}

Context::~Context() {
    ALOGV("%p Context::~Context", this);

    if (!mIsContextLite) {
        mPaused = false;
        void *res;

        mIO.shutdown();
        int status = pthread_join(mThreadId, &res);
        rsAssert(mExit);

        if (mHal.funcs.shutdownDriver) {
            mHal.funcs.shutdownDriver(this);
        }

        // Global structure cleanup.
        pthread_mutex_lock(&gInitMutex);

#ifdef PVR_RSC
        if (mLib) {
            dlclose(mLib);
            mLib = NULL;
        }
#endif
        if (mDev) {
            mDev->removeContext(this);
            mDev = NULL;
        }
        pthread_mutex_unlock(&gInitMutex);
    }
    ALOGV("%p Context::~Context done", this);
}

void Context::setSurface(uint32_t w, uint32_t h, RsNativeWindow sur) {
    rsAssert(mIsGraphicsContext);
    mHal.funcs.setSurface(this, w, h, sur);

    mHasSurface = sur != NULL;
    mWidth = w;
    mHeight = h;

    if (mWidth && mHeight) {
        mStateVertex.updateSize(this);
        mFBOCache.updateSize();
    }
}

uint32_t Context::getCurrentSurfaceWidth() const {
    for (uint32_t i = 0; i < mFBOCache.mHal.state.colorTargetsCount; i ++) {
        if (mFBOCache.mHal.state.colorTargets[i] != NULL) {
            return mFBOCache.mHal.state.colorTargets[i]->getType()->getDimX();
        }
    }
    if (mFBOCache.mHal.state.depthTarget != NULL) {
        return mFBOCache.mHal.state.depthTarget->getType()->getDimX();
    }
    return mWidth;
}

uint32_t Context::getCurrentSurfaceHeight() const {
    for (uint32_t i = 0; i < mFBOCache.mHal.state.colorTargetsCount; i ++) {
        if (mFBOCache.mHal.state.colorTargets[i] != NULL) {
            return mFBOCache.mHal.state.colorTargets[i]->getType()->getDimY();
        }
    }
    if (mFBOCache.mHal.state.depthTarget != NULL) {
        return mFBOCache.mHal.state.depthTarget->getType()->getDimY();
    }
    return mHeight;
}

void Context::pause() {
    rsAssert(mIsGraphicsContext);
    mPaused = true;
}

void Context::resume() {
    rsAssert(mIsGraphicsContext);
    mPaused = false;
}

void Context::setRootScript(Script *s) {
    rsAssert(mIsGraphicsContext);
    mRootScript.set(s);
}

void Context::setProgramStore(ProgramStore *pfs) {
    rsAssert(mIsGraphicsContext);
    if (pfs == NULL) {
        mFragmentStore.set(mStateFragmentStore.mDefault);
    } else {
        mFragmentStore.set(pfs);
    }
}

void Context::setProgramFragment(ProgramFragment *pf) {
    rsAssert(mIsGraphicsContext);
    if (pf == NULL) {
        mFragment.set(mStateFragment.mDefault);
    } else {
        mFragment.set(pf);
    }
}

void Context::setProgramRaster(ProgramRaster *pr) {
    rsAssert(mIsGraphicsContext);
    if (pr == NULL) {
        mRaster.set(mStateRaster.mDefault);
    } else {
        mRaster.set(pr);
    }
}

void Context::setProgramVertex(ProgramVertex *pv) {
    rsAssert(mIsGraphicsContext);
    if (pv == NULL) {
        mVertex.set(mStateVertex.mDefault);
    } else {
        mVertex.set(pv);
    }
}

void Context::setFont(Font *f) {
    rsAssert(mIsGraphicsContext);
    if (f == NULL) {
        mFont.set(mStateFont.mDefault);
    } else {
        mFont.set(f);
    }
}

void Context::assignName(ObjectBase *obj, const char *name, uint32_t len) {
    rsAssert(!obj->getName());
    obj->setName(name, len);
    mNames.add(obj);
}

void Context::removeName(ObjectBase *obj) {
    for (size_t ct=0; ct < mNames.size(); ct++) {
        if (obj == mNames[ct]) {
            mNames.removeAt(ct);
            return;
        }
    }
}

RsMessageToClientType Context::peekMessageToClient(size_t *receiveLen, uint32_t *subID) {
    return (RsMessageToClientType)mIO.getClientHeader(receiveLen, subID);
}

RsMessageToClientType Context::getMessageToClient(void *data, size_t *receiveLen, uint32_t *subID, size_t bufferLen) {
    return (RsMessageToClientType)mIO.getClientPayload(data, receiveLen, subID, bufferLen);
}

bool Context::sendMessageToClient(const void *data, RsMessageToClientType cmdID,
                                  uint32_t subID, size_t len, bool waitForSpace) const {

    return mIO.sendToClient(cmdID, subID, data, len, waitForSpace);
}

void Context::initToClient() {
    while (!mRunning) {
        usleep(100);
    }
}

void Context::deinitToClient() {
    mIO.clientShutdown();
}

void Context::setError(RsError e, const char *msg) const {
    mError = e;
    sendMessageToClient(msg, RS_MESSAGE_TO_CLIENT_ERROR, e, strlen(msg) + 1, true);
}


void Context::dumpDebug() const {
    ALOGE("RS Context debug %p", this);
    ALOGE("RS Context debug");

    ALOGE(" RS width %i, height %i", mWidth, mHeight);
    ALOGE(" RS running %i, exit %i, paused %i", mRunning, mExit, mPaused);
    ALOGE(" RS pThreadID %li, nativeThreadID %i", (long int)mThreadId, mNativeThreadId);
}

///////////////////////////////////////////////////////////////////////////////////////////
//

namespace android {
namespace renderscript {

void rsi_ContextFinish(Context *rsc) {
}

void rsi_ContextBindRootScript(Context *rsc, RsScript vs) {
    Script *s = static_cast<Script *>(vs);
    rsc->setRootScript(s);
}

void rsi_ContextBindSampler(Context *rsc, uint32_t slot, RsSampler vs) {
    Sampler *s = static_cast<Sampler *>(vs);

    if (slot > RS_MAX_SAMPLER_SLOT) {
        ALOGE("Invalid sampler slot");
        return;
    }

    s->bindToContext(&rsc->mStateSampler, slot);
}

void rsi_ContextBindProgramStore(Context *rsc, RsProgramStore vpfs) {
    ProgramStore *pfs = static_cast<ProgramStore *>(vpfs);
    rsc->setProgramStore(pfs);
}

void rsi_ContextBindProgramFragment(Context *rsc, RsProgramFragment vpf) {
    ProgramFragment *pf = static_cast<ProgramFragment *>(vpf);
    rsc->setProgramFragment(pf);
}

void rsi_ContextBindProgramRaster(Context *rsc, RsProgramRaster vpr) {
    ProgramRaster *pr = static_cast<ProgramRaster *>(vpr);
    rsc->setProgramRaster(pr);
}

void rsi_ContextBindProgramVertex(Context *rsc, RsProgramVertex vpv) {
    ProgramVertex *pv = static_cast<ProgramVertex *>(vpv);
    rsc->setProgramVertex(pv);
}

void rsi_ContextBindFont(Context *rsc, RsFont vfont) {
    Font *font = static_cast<Font *>(vfont);
    rsc->setFont(font);
}

void rsi_AssignName(Context *rsc, RsObjectBase obj, const char *name, size_t name_length) {
    ObjectBase *ob = static_cast<ObjectBase *>(obj);
    rsc->assignName(ob, name, name_length);
}

void rsi_ObjDestroy(Context *rsc, void *optr) {
    ObjectBase *ob = static_cast<ObjectBase *>(optr);
    rsc->removeName(ob);
    ob->decUserRef();
}

void rsi_ContextPause(Context *rsc) {
    rsc->pause();
}

void rsi_ContextResume(Context *rsc) {
    rsc->resume();
}

void rsi_ContextSetSurface(Context *rsc, uint32_t w, uint32_t h, RsNativeWindow sur) {
    rsc->setSurface(w, h, sur);
}

void rsi_ContextSetPriority(Context *rsc, int32_t p) {
    rsc->setPriority(p);
}

void rsi_ContextDump(Context *rsc, int32_t bits) {
    ObjectBase::dumpAll(rsc);
}

void rsi_ContextDestroyWorker(Context *rsc) {
    rsc->destroyWorkerThreadResources();
}

void rsi_ContextDestroy(Context *rsc) {
    ALOGV("%p rsContextDestroy", rsc);
    rsContextDestroyWorker(rsc);
    delete rsc;
    ALOGV("%p rsContextDestroy done", rsc);
}


RsMessageToClientType rsi_ContextPeekMessage(Context *rsc,
                                           size_t * receiveLen, size_t receiveLen_length,
                                           uint32_t * subID, size_t subID_length) {
    return rsc->peekMessageToClient(receiveLen, subID);
}

RsMessageToClientType rsi_ContextGetMessage(Context *rsc, void * data, size_t data_length,
                                          size_t * receiveLen, size_t receiveLen_length,
                                          uint32_t * subID, size_t subID_length) {
    rsAssert(subID_length == sizeof(uint32_t));
    rsAssert(receiveLen_length == sizeof(size_t));
    return rsc->getMessageToClient(data, receiveLen, subID, data_length);
}

void rsi_ContextInitToClient(Context *rsc) {
    rsc->initToClient();
}

void rsi_ContextDeinitToClient(Context *rsc) {
    rsc->deinitToClient();
}

}
}

RsContext rsContextCreate(RsDevice vdev, uint32_t version,
                          uint32_t sdkVersion) {
    ALOGV("rsContextCreate dev=%p", vdev);
    Device * dev = static_cast<Device *>(vdev);
    Context *rsc = Context::createContext(dev, NULL);
    if (rsc) {
        rsc->setTargetSdkVersion(sdkVersion);
    }
    return rsc;
}

RsContext rsContextCreateGL(RsDevice vdev, uint32_t version,
                            uint32_t sdkVersion, RsSurfaceConfig sc,
                            uint32_t dpi) {
    ALOGV("rsContextCreateGL dev=%p", vdev);
    Device * dev = static_cast<Device *>(vdev);
    Context *rsc = Context::createContext(dev, &sc);
    if (rsc) {
        rsc->setTargetSdkVersion(sdkVersion);
        rsc->setDPI(dpi);
    }
    ALOGV("%p rsContextCreateGL ret", rsc);
    return rsc;
}

// Only to be called at a3d load time, before object is visible to user
// not thread safe
void rsaGetName(RsContext con, void * obj, const char **name) {
    ObjectBase *ob = static_cast<ObjectBase *>(obj);
    (*name) = ob->getName();
}
