/*
 * Copyright (C) 2009-2012 The Android Open Source Project
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

#ifndef ANDROID_STRUCTURED_ALLOCATION_H
#define ANDROID_STRUCTURED_ALLOCATION_H

#include "rsType.h"

struct ANativeWindow;

// ---------------------------------------------------------------------------
namespace android {
class SurfaceTexture;

namespace renderscript {

class Program;

/*****************************************************************************
 * CAUTION
 *
 * Any layout changes for this class may require a corresponding change to be
 * made to frameworks/compile/libbcc/lib/ScriptCRT/rs_core.c, which contains
 * a partial copy of the information below.
 *
 *****************************************************************************/
class Allocation : public ObjectBase {
    // The graphics equivalent of malloc.  The allocation contains a structure of elements.

public:
    const static int MAX_LOD = 16;

    struct Hal {
        void * drv;

        struct State {
            const Type * type;

            uint32_t usageFlags;
            RsAllocationMipmapControl mipmapControl;

            // Cached fields from the Type and Element
            // to prevent pointer chasing in critical loops.
            uint32_t dimensionX;
            uint32_t dimensionY;
            uint32_t dimensionZ;
            uint32_t elementSizeBytes;
            bool hasMipmaps;
            bool hasFaces;
            bool hasReferences;
            void * unused_01;
            int32_t surfaceTextureID;
            ANativeWindow *wndSurface;
            SurfaceTexture *surfaceTexture;
            RsDataType eType;
        };
        State state;

        struct DrvState {
            mutable void * mallocPtrLOD0;
            mutable uint32_t strideLOD0;
        } drvState;
#ifdef PVR_RSC
        void * IMGPrivateData;
#endif

    };
    Hal mHal;

    static Allocation * createAllocation(Context *rsc, const Type *, uint32_t usages,
                                         RsAllocationMipmapControl mc = RS_ALLOCATION_MIPMAP_NONE,
                                         void *ptr = 0);
    virtual ~Allocation();
    void updateCache();

    const Type * getType() const {return mHal.state.type;}

    void syncAll(Context *rsc, RsAllocationUsageType src);

    void copyRange1D(Context *rsc, const Allocation *src, int32_t srcOff, int32_t destOff, int32_t len);

    void resize1D(Context *rsc, uint32_t dimX);
    void resize2D(Context *rsc, uint32_t dimX, uint32_t dimY);

    void data(Context *rsc, uint32_t xoff, uint32_t lod, uint32_t count, const void *data, size_t sizeBytes);
    void data(Context *rsc, uint32_t xoff, uint32_t yoff, uint32_t lod, RsAllocationCubemapFace face,
                 uint32_t w, uint32_t h, const void *data, size_t sizeBytes);
    void data(Context *rsc, uint32_t xoff, uint32_t yoff, uint32_t zoff, uint32_t lod, RsAllocationCubemapFace face,
                 uint32_t w, uint32_t h, uint32_t d, const void *data, size_t sizeBytes);

    void read(Context *rsc, uint32_t xoff, uint32_t lod, uint32_t count, void *data, size_t sizeBytes);
    void read(Context *rsc, uint32_t xoff, uint32_t yoff, uint32_t lod, RsAllocationCubemapFace face,
                 uint32_t w, uint32_t h, void *data, size_t sizeBytes);
    void read(Context *rsc, uint32_t xoff, uint32_t yoff, uint32_t zoff, uint32_t lod, RsAllocationCubemapFace face,
                 uint32_t w, uint32_t h, uint32_t d, void *data, size_t sizeBytes);

    void elementData(Context *rsc, uint32_t x,
                     const void *data, uint32_t elementOff, size_t sizeBytes);
    void elementData(Context *rsc, uint32_t x, uint32_t y,
                     const void *data, uint32_t elementOff, size_t sizeBytes);

    void addProgramToDirty(const Program *);
    void removeProgramToDirty(const Program *);

    virtual void dumpLOGV(const char *prefix) const;
    virtual void serialize(Context *rsc, OStream *stream) const;
    virtual RsA3DClassID getClassId() const { return RS_A3D_CLASS_ID_ALLOCATION; }
    static Allocation *createFromStream(Context *rsc, IStream *stream);

    bool getIsScript() const {
        return (mHal.state.usageFlags & RS_ALLOCATION_USAGE_SCRIPT) != 0;
    }
    bool getIsTexture() const {
        return (mHal.state.usageFlags & RS_ALLOCATION_USAGE_GRAPHICS_TEXTURE) != 0;
    }
    bool getIsRenderTarget() const {
        return (mHal.state.usageFlags & RS_ALLOCATION_USAGE_GRAPHICS_RENDER_TARGET) != 0;
    }
    bool getIsBufferObject() const {
        return (mHal.state.usageFlags & RS_ALLOCATION_USAGE_GRAPHICS_VERTEX) != 0;
    }

    void incRefs(const void *ptr, size_t ct, size_t startOff = 0) const;
    void decRefs(const void *ptr, size_t ct, size_t startOff = 0) const;
    virtual bool freeChildren();

    void sendDirty(const Context *rsc) const;
    bool getHasGraphicsMipmaps() const {
        return mHal.state.mipmapControl != RS_ALLOCATION_MIPMAP_NONE;
    }

    int32_t getSurfaceTextureID(const Context *rsc);
    void setSurfaceTexture(const Context *rsc, SurfaceTexture *st);
    void setSurface(const Context *rsc, RsNativeWindow sur);
    void ioSend(const Context *rsc);
    void ioReceive(const Context *rsc);

protected:
    Vector<const Program *> mToDirtyList;
    ObjectBaseRef<const Type> mType;
    void setType(const Type *t) {
        mType.set(t);
        mHal.state.type = t;
    }

private:
    void freeChildrenUnlocked();
    Allocation(Context *rsc, const Type *, uint32_t usages, RsAllocationMipmapControl mc, void *ptr);

    uint32_t getPackedSize() const;
    static void writePackedData(Context *rsc, const Type *type, uint8_t *dst,
                                const uint8_t *src, bool dstPadded);
    void unpackVec3Allocation(Context *rsc, const void *data, size_t dataSize);
    void packVec3Allocation(Context *rsc, OStream *stream) const;
};

}
}
#endif

