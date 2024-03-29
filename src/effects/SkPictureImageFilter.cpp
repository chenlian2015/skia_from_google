/*
 * Copyright 2013 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkPictureImageFilter.h"
#include "SkDevice.h"
#include "SkCanvas.h"
#include "SkReadBuffer.h"
#include "SkSurfaceProps.h"
#include "SkWriteBuffer.h"
#include "SkValidationUtils.h"

SkPictureImageFilter::SkPictureImageFilter(const SkPicture* picture, uint32_t uniqueID)
    : INHERITED(0, 0, NULL, uniqueID)
    , fPicture(SkSafeRef(picture))
    , fCropRect(picture ? picture->cullRect() : SkRect::MakeEmpty())
    , fPictureResolution(kDeviceSpace_PictureResolution) {
}

SkPictureImageFilter::SkPictureImageFilter(const SkPicture* picture, const SkRect& cropRect,
                                           uint32_t uniqueID, PictureResolution pictureResolution)
    : INHERITED(0, 0, NULL, uniqueID)
    , fPicture(SkSafeRef(picture))
    , fCropRect(cropRect)
    , fPictureResolution(pictureResolution) {
}

SkPictureImageFilter::~SkPictureImageFilter() {
    SkSafeUnref(fPicture);
}

SkFlattenable* SkPictureImageFilter::CreateProc(SkReadBuffer& buffer) {
    SkAutoTUnref<SkPicture> picture;
    SkRect cropRect;

    if (!buffer.isCrossProcess()) {
        if (buffer.readBool()) {
            picture.reset(SkPicture::CreateFromBuffer(buffer));
        }
    } else {
        buffer.validate(!buffer.readBool());
    }
    buffer.readRect(&cropRect);
    PictureResolution pictureResolution;
    if (buffer.isVersionLT(SkReadBuffer::kPictureImageFilterResolution_Version)) {
        pictureResolution = kDeviceSpace_PictureResolution;
    } else {
        pictureResolution = (PictureResolution)buffer.readInt();
    } 

    if (pictureResolution == kLocalSpace_PictureResolution) {
        return CreateForLocalSpace(picture, cropRect);
    }
    return Create(picture, cropRect);
}

void SkPictureImageFilter::flatten(SkWriteBuffer& buffer) const {
    if (!buffer.isCrossProcess()) {
        bool hasPicture = (fPicture != NULL);
        buffer.writeBool(hasPicture);
        if (hasPicture) {
            fPicture->flatten(buffer);
        }
    } else {
        buffer.writeBool(false);
    }
    buffer.writeRect(fCropRect);
    buffer.writeInt(fPictureResolution);
}

bool SkPictureImageFilter::onFilterImage(Proxy* proxy, const SkBitmap&, const Context& ctx,
                                         SkBitmap* result, SkIPoint* offset) const {
    if (!fPicture) {
        offset->fX = offset->fY = 0;
        return true;
    }

    SkRect floatBounds;
    ctx.ctm().mapRect(&floatBounds, fCropRect);
    SkIRect bounds = floatBounds.roundOut();
    if (!bounds.intersect(ctx.clipBounds())) {
        return false;
    }

    if (bounds.isEmpty()) {
        offset->fX = offset->fY = 0;
        return true;
    }

    SkAutoTUnref<SkBaseDevice> device(proxy->createDevice(bounds.width(), bounds.height()));
    if (NULL == device.get()) {
        return false;
    }

    if (kLocalSpace_PictureResolution == fPictureResolution && 
        (ctx.ctm().getType() & ~SkMatrix::kTranslate_Mask)) {
        drawPictureAtLocalResolution(proxy, device.get(), bounds, ctx);
    } else {
        drawPictureAtDeviceResolution(proxy, device.get(), bounds, ctx);
    }

    *result = device.get()->accessBitmap(false);
    offset->fX = bounds.fLeft;
    offset->fY = bounds.fTop;
    return true;
}

void SkPictureImageFilter::drawPictureAtDeviceResolution(Proxy* proxy, SkBaseDevice* device,
                                                         const SkIRect& deviceBounds,
                                                         const Context& ctx) const {
    // Pass explicit surface props, as the simplified canvas constructor discards device properties.
    // FIXME: switch back to the public constructor (and unfriend) after
    //        https://code.google.com/p/skia/issues/detail?id=3142 is fixed.
    SkCanvas canvas(device, proxy->surfaceProps(), SkCanvas::kDefault_InitFlags);

    canvas.translate(-SkIntToScalar(deviceBounds.fLeft), -SkIntToScalar(deviceBounds.fTop));
    canvas.concat(ctx.ctm());
    canvas.drawPicture(fPicture);
}

void SkPictureImageFilter::drawPictureAtLocalResolution(Proxy* proxy, SkBaseDevice* device,
                                                        const SkIRect& deviceBounds,
                                                        const Context& ctx) const {
    SkMatrix inverseCtm;
    if (!ctx.ctm().invert(&inverseCtm))
        return;
    SkRect localBounds = SkRect::Make(ctx.clipBounds());
    inverseCtm.mapRect(&localBounds);
    if (!localBounds.intersect(fCropRect))
        return;
    SkIRect localIBounds = localBounds.roundOut();
    SkAutoTUnref<SkBaseDevice> localDevice(proxy->createDevice(localIBounds.width(), localIBounds.height()));

    // Pass explicit surface props, as the simplified canvas constructor discards device properties.
    // FIXME: switch back to the public constructor (and unfriend) after
    //        https://code.google.com/p/skia/issues/detail?id=3142 is fixed.
    SkCanvas localCanvas(localDevice, proxy->surfaceProps(), SkCanvas::kDefault_InitFlags);
    localCanvas.translate(-SkIntToScalar(localIBounds.fLeft), -SkIntToScalar(localIBounds.fTop));
    localCanvas.drawPicture(fPicture);

    // Pass explicit surface props, as the simplified canvas constructor discards device properties.
    // FIXME: switch back to the public constructor (and unfriend) after
    //        https://code.google.com/p/skia/issues/detail?id=3142 is fixed.
    SkCanvas canvas(device, proxy->surfaceProps(), SkCanvas::kDefault_InitFlags);

    canvas.translate(-SkIntToScalar(deviceBounds.fLeft), -SkIntToScalar(deviceBounds.fTop));
    canvas.concat(ctx.ctm());
    SkPaint paint;
    paint.setFilterLevel(SkPaint::kLow_FilterLevel);
    canvas.drawBitmap(localDevice.get()->accessBitmap(false), SkIntToScalar(localIBounds.fLeft), SkIntToScalar(localIBounds.fTop), &paint);
    //canvas.drawPicture(fPicture);
}
