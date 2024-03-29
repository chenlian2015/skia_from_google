/*
 * Copyright 2014 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SKPBench.h"
#include "SkCommandLineFlags.h"
#include "SkMultiPictureDraw.h"
#include "SkSurface.h"

DEFINE_int32(benchTile, 256, "Tile dimension used for SKP playback.");

SKPBench::SKPBench(const char* name, const SkPicture* pic, const SkIRect& clip, SkScalar scale,
                   bool useMultiPictureDraw)
    : fPic(SkRef(pic))
    , fClip(clip)
    , fScale(scale)
    , fName(name)
    , fUseMultiPictureDraw(useMultiPictureDraw) {
    fUniqueName.printf("%s_%.2g", name, scale);  // Scale makes this unqiue for skiaperf.com traces.
    if (useMultiPictureDraw) {
        fUniqueName.append("_mpd");
    }
}

SKPBench::~SKPBench() {
    for (int i = 0; i < fSurfaces.count(); ++i) {
        fSurfaces[i]->unref();
    }
}

const char* SKPBench::onGetName() {
    return fName.c_str();
}

const char* SKPBench::onGetUniqueName() {
    return fUniqueName.c_str();
}

void SKPBench::onPerCanvasPreDraw(SkCanvas* canvas) {
    if (!fUseMultiPictureDraw) {
        return;
    }

    SkIRect bounds;
    SkAssertResult(canvas->getClipDeviceBounds(&bounds));

    int xTiles = SkScalarCeilToInt(bounds.width()  / SkIntToScalar(FLAGS_benchTile));
    int yTiles = SkScalarCeilToInt(bounds.height() / SkIntToScalar(FLAGS_benchTile));

    fSurfaces.setReserve(xTiles * yTiles);
    fTileRects.setReserve(xTiles * yTiles);

    SkImageInfo ii = canvas->imageInfo().makeWH(FLAGS_benchTile, FLAGS_benchTile);

    for (int y = bounds.fTop; y < bounds.fBottom; y += FLAGS_benchTile) {
        for (int x = bounds.fLeft; x < bounds.fRight; x += FLAGS_benchTile) {
            const SkIRect tileRect = SkIRect::MakeXYWH(x, y, FLAGS_benchTile, FLAGS_benchTile);
            *fTileRects.append() = tileRect;
            *fSurfaces.push() = canvas->newSurface(ii);

            // Never want the contents of a tile to include stuff the parent
            // canvas clips out
            SkRect clip = SkRect::Make(bounds);
            clip.offset(-SkIntToScalar(tileRect.fLeft), -SkIntToScalar(tileRect.fTop));
            fSurfaces.top()->getCanvas()->clipRect(clip);

            fSurfaces.top()->getCanvas()->setMatrix(canvas->getTotalMatrix());
            fSurfaces.top()->getCanvas()->scale(fScale, fScale);
        }
    }
}

void SKPBench::onPerCanvasPostDraw(SkCanvas* canvas) {
    if (!fUseMultiPictureDraw) {
        return;
    }

    // Draw the last set of tiles into the master canvas in case we're
    // saving the images
    for (int i = 0; i < fTileRects.count(); ++i) {
        SkAutoTUnref<SkImage> image(fSurfaces[i]->newImageSnapshot());
        canvas->drawImage(image,
                          SkIntToScalar(fTileRects[i].fLeft), SkIntToScalar(fTileRects[i].fTop));
        SkSafeSetNull(fSurfaces[i]);
    }

    fSurfaces.rewind();
    fTileRects.rewind();
}

bool SKPBench::isSuitableFor(Backend backend) {
    return backend != kNonRendering_Backend;
}

SkIPoint SKPBench::onGetSize() {
    return SkIPoint::Make(fClip.width(), fClip.height());
}

void SKPBench::onDraw(const int loops, SkCanvas* canvas) {
    if (fUseMultiPictureDraw) {
        for (int i = 0; i < loops; i++) {
            SkMultiPictureDraw mpd;

            for (int i = 0; i < fTileRects.count(); ++i) {
                SkMatrix trans;
                trans.setTranslate(-fTileRects[i].fLeft/fScale,
                                   -fTileRects[i].fTop/fScale);
                mpd.add(fSurfaces[i]->getCanvas(), fPic, &trans);
            }

            mpd.draw();

            for (int i = 0; i < fTileRects.count(); ++i) {
                fSurfaces[i]->getCanvas()->flush();
            }
        }
    } else {
        SkIRect bounds;
        SkAssertResult(canvas->getClipDeviceBounds(&bounds));

        SkAutoCanvasRestore overall(canvas, true/*save now*/);
        canvas->scale(fScale, fScale);

        for (int i = 0; i < loops; i++) {
            for (int y = bounds.fTop; y < bounds.fBottom; y += FLAGS_benchTile) {
                for (int x = bounds.fLeft; x < bounds.fRight; x += FLAGS_benchTile) {
                    SkAutoCanvasRestore perTile(canvas, true/*save now*/);
                    canvas->clipRect(SkRect::MakeXYWH(x/fScale, y/fScale,
                                                      FLAGS_benchTile/fScale,
                                                      FLAGS_benchTile/fScale));
                    fPic->playback(canvas);
                }
            }

            canvas->flush();
        }
    }
}
