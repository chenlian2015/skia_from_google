/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrClipMaskManager.h"
#include "GrAAConvexPathRenderer.h"
#include "GrAAHairLinePathRenderer.h"
#include "GrAARectRenderer.h"
#include "GrDrawTargetCaps.h"
#include "GrPaint.h"
#include "GrPathRenderer.h"
#include "GrRenderTarget.h"
#include "GrStencilBuffer.h"
#include "GrSWMaskHelper.h"
#include "SkRasterClip.h"
#include "SkStrokeRec.h"
#include "SkTLazy.h"
#include "effects/GrTextureDomain.h"
#include "effects/GrConvexPolyEffect.h"
#include "effects/GrRRectEffect.h"

#define GR_AA_CLIP 1
typedef SkClipStack::Element Element;

////////////////////////////////////////////////////////////////////////////////
namespace {
// set up the draw state to enable the aa clipping mask. Besides setting up the
// stage matrix this also alters the vertex layout
void setup_drawstate_aaclip(const SkIRect &devBound,
                            GrDrawState* drawState,
                            GrTexture* result) {
    SkASSERT(drawState);

    SkMatrix mat;
    // We want to use device coords to compute the texture coordinates. We set our matrix to be
    // equal to the view matrix followed by an offset to the devBound, and then a scaling matrix to
    // normalized coords. We apply this matrix to the vertex positions rather than local coords.
    mat.setIDiv(result->width(), result->height());
    mat.preTranslate(SkIntToScalar(-devBound.fLeft),
                     SkIntToScalar(-devBound.fTop));
    mat.preConcat(drawState->getViewMatrix());

    SkIRect domainTexels = SkIRect::MakeWH(devBound.width(), devBound.height());
    // This could be a long-lived effect that is cached with the alpha-mask.
    drawState->addCoverageProcessor(
        GrTextureDomainEffect::Create(result,
                                      mat,
                                      GrTextureDomain::MakeTexelDomain(result, domainTexels),
                                      GrTextureDomain::kDecal_Mode,
                                      GrTextureParams::kNone_FilterMode,
                                      kPosition_GrCoordSet))->unref();
}

bool path_needs_SW_renderer(GrContext* context,
                            const GrDrawTarget* gpu,
                            const GrDrawState* drawState,
                            const SkPath& origPath,
                            const SkStrokeRec& stroke,
                            bool doAA) {
    // the gpu alpha mask will draw the inverse paths as non-inverse to a temp buffer
    SkTCopyOnFirstWrite<SkPath> path(origPath);
    if (path->isInverseFillType()) {
        path.writable()->toggleInverseFillType();
    }
    // last (false) parameter disallows use of the SW path renderer
    GrPathRendererChain::DrawType type = doAA ?
                                         GrPathRendererChain::kColorAntiAlias_DrawType :
                                         GrPathRendererChain::kColor_DrawType;

    return NULL == context->getPathRenderer(gpu, drawState, *path, stroke, false, type);
}
}

/*
 * This method traverses the clip stack to see if the GrSoftwarePathRenderer
 * will be used on any element. If so, it returns true to indicate that the
 * entire clip should be rendered in SW and then uploaded en masse to the gpu.
 */
bool GrClipMaskManager::useSWOnlyPath(const GrDrawState* drawState,
                                      const GrReducedClip::ElementList& elements) {
    // TODO: generalize this function so that when
    // a clip gets complex enough it can just be done in SW regardless
    // of whether it would invoke the GrSoftwarePathRenderer.
    SkStrokeRec stroke(SkStrokeRec::kFill_InitStyle);

    for (GrReducedClip::ElementList::Iter iter(elements.headIter()); iter.get(); iter.next()) {
        const Element* element = iter.get();
        // rects can always be drawn directly w/o using the software path
        // Skip rrects once we're drawing them directly.
        if (Element::kRect_Type != element->getType()) {
            SkPath path;
            element->asPath(&path);
            if (path_needs_SW_renderer(this->getContext(), fClipTarget, drawState, path, stroke,
                                       element->isAA())) {
                return true;
            }
        }
    }
    return false;
}

bool GrClipMaskManager::installClipEffects(GrDrawState* drawState,
                                           GrDrawState::AutoRestoreEffects* are,
                                           const GrReducedClip::ElementList& elements,
                                           const SkVector& clipToRTOffset,
                                           const SkRect* drawBounds) {
    SkRect boundsInClipSpace;
    if (drawBounds) {
        boundsInClipSpace = *drawBounds;
        boundsInClipSpace.offset(-clipToRTOffset.fX, -clipToRTOffset.fY);
    }

    are->set(drawState);
    GrRenderTarget* rt = drawState->getRenderTarget();
    GrReducedClip::ElementList::Iter iter(elements);
    bool failed = false;
    while (iter.get()) {
        SkRegion::Op op = iter.get()->getOp();
        bool invert;
        bool skip = false;
        switch (op) {
            case SkRegion::kReplace_Op:
                SkASSERT(iter.get() == elements.head());
                // Fallthrough, handled same as intersect.
            case SkRegion::kIntersect_Op:
                invert = false;
                if (drawBounds && iter.get()->contains(boundsInClipSpace)) {
                    skip = true;
                }
                break;
            case SkRegion::kDifference_Op:
                invert = true;
                // We don't currently have a cheap test for whether a rect is fully outside an
                // element's primitive, so don't attempt to set skip.
                break;
            default:
                failed = true;
                break;
        }
        if (failed) {
            break;
        }

        if (!skip) {
            GrPrimitiveEdgeType edgeType;
            if (GR_AA_CLIP && iter.get()->isAA()) {
                if (rt->isMultisampled()) {
                    // Coverage based AA clips don't place nicely with MSAA.
                    failed = true;
                    break;
                }
                edgeType =
                        invert ? kInverseFillAA_GrProcessorEdgeType : kFillAA_GrProcessorEdgeType;
            } else {
                edgeType =
                        invert ? kInverseFillBW_GrProcessorEdgeType : kFillBW_GrProcessorEdgeType;
            }
            SkAutoTUnref<GrFragmentProcessor> fp;
            switch (iter.get()->getType()) {
                case SkClipStack::Element::kPath_Type:
                    fp.reset(GrConvexPolyEffect::Create(edgeType, iter.get()->getPath(),
                        &clipToRTOffset));
                    break;
                case SkClipStack::Element::kRRect_Type: {
                    SkRRect rrect = iter.get()->getRRect();
                    rrect.offset(clipToRTOffset.fX, clipToRTOffset.fY);
                    fp.reset(GrRRectEffect::Create(edgeType, rrect));
                    break;
                }
                case SkClipStack::Element::kRect_Type: {
                    SkRect rect = iter.get()->getRect();
                    rect.offset(clipToRTOffset.fX, clipToRTOffset.fY);
                    fp.reset(GrConvexPolyEffect::Create(edgeType, rect));
                    break;
                }
                default:
                    break;
            }
            if (fp) {
                drawState->addCoverageProcessor(fp);
            } else {
                failed = true;
                break;
            }
        }
        iter.next();
    }

    if (failed) {
        are->set(NULL);
    }
    return !failed;
}

////////////////////////////////////////////////////////////////////////////////
// sort out what kind of clip mask needs to be created: alpha, stencil,
// scissor, or entirely software
bool GrClipMaskManager::setupClipping(GrDrawState* drawState,
                                      GrDrawState::AutoRestoreEffects* are,
                                      GrDrawState::AutoRestoreStencil* ars,
                                      ScissorState* scissorState,
                                      const GrClipData* clipDataIn,
                                      const SkRect* devBounds) {
    fCurrClipMaskType = kNone_ClipMaskType;
    if (kRespectClip_StencilClipMode == fClipMode) {
        fClipMode = kIgnoreClip_StencilClipMode;
    }

    GrReducedClip::ElementList elements(16);
    int32_t genID;
    GrReducedClip::InitialState initialState;
    SkIRect clipSpaceIBounds;
    bool requiresAA;
    GrRenderTarget* rt = drawState->getRenderTarget();

    // GrDrawTarget should have filtered this for us
    SkASSERT(rt);

    bool ignoreClip = !drawState->isClipState() || clipDataIn->fClipStack->isWideOpen();
    if (!ignoreClip) {
        SkIRect clipSpaceRTIBounds = SkIRect::MakeWH(rt->width(), rt->height());
        clipSpaceRTIBounds.offset(clipDataIn->fOrigin);
        GrReducedClip::ReduceClipStack(*clipDataIn->fClipStack,
                                       clipSpaceRTIBounds,
                                       &elements,
                                       &genID,
                                       &initialState,
                                       &clipSpaceIBounds,
                                       &requiresAA);
        if (elements.isEmpty()) {
            if (GrReducedClip::kAllIn_InitialState == initialState) {
                ignoreClip = clipSpaceIBounds == clipSpaceRTIBounds;
            } else {
                return false;
            }
        }
    }

    if (ignoreClip) {
        this->setDrawStateStencil(drawState, ars);
        return true;
    }

    // An element count of 4 was chosen because of the common pattern in Blink of:
    //   isect RR
    //   diff  RR
    //   isect convex_poly
    //   isect convex_poly
    // when drawing rounded div borders. This could probably be tuned based on a
    // configuration's relative costs of switching RTs to generate a mask vs
    // longer shaders.
    if (elements.count() <= 4) {
        SkVector clipToRTOffset = { SkIntToScalar(-clipDataIn->fOrigin.fX),
                                    SkIntToScalar(-clipDataIn->fOrigin.fY) };
        if (elements.isEmpty() ||
            (requiresAA && this->installClipEffects(drawState, are, elements, clipToRTOffset,
                                                    devBounds))) {
            SkIRect scissorSpaceIBounds(clipSpaceIBounds);
            scissorSpaceIBounds.offset(-clipDataIn->fOrigin);
            if (NULL == devBounds ||
                !SkRect::Make(scissorSpaceIBounds).contains(*devBounds)) {
                scissorState->set(scissorSpaceIBounds);
            }
            this->setDrawStateStencil(drawState, ars);
            return true;
        }
    }

#if GR_AA_CLIP
    // If MSAA is enabled we can do everything in the stencil buffer.
    if (0 == rt->numSamples() && requiresAA) {
        GrTexture* result = NULL;

        if (this->useSWOnlyPath(drawState, elements)) {
            // The clip geometry is complex enough that it will be more efficient to create it
            // entirely in software
            result = this->createSoftwareClipMask(genID,
                                                  initialState,
                                                  elements,
                                                  clipSpaceIBounds);
        } else {
            result = this->createAlphaClipMask(genID,
                                               initialState,
                                               elements,
                                               clipSpaceIBounds);
        }

        if (result) {
            // The mask's top left coord should be pinned to the rounded-out top left corner of
            // clipSpace bounds. We determine the mask's position WRT to the render target here.
            SkIRect rtSpaceMaskBounds = clipSpaceIBounds;
            rtSpaceMaskBounds.offset(-clipDataIn->fOrigin);
            setup_drawstate_aaclip(rtSpaceMaskBounds, drawState, result);
            this->setDrawStateStencil(drawState, ars);
            return true;
        }
        // if alpha clip mask creation fails fall through to the non-AA code paths
    }
#endif // GR_AA_CLIP

    // Either a hard (stencil buffer) clip was explicitly requested or an anti-aliased clip couldn't
    // be created. In either case, free up the texture in the anti-aliased mask cache.
    // TODO: this may require more investigation. Ganesh performs a lot of utility draws (e.g.,
    // clears, InOrderDrawBuffer playbacks) that hit the stencil buffer path. These may be
    // "incorrectly" clearing the AA cache.
    fAACache.reset();

    // use the stencil clip if we can't represent the clip as a rectangle.
    SkIPoint clipSpaceToStencilSpaceOffset = -clipDataIn->fOrigin;
    this->createStencilClipMask(rt,
                                genID,
                                initialState,
                                elements,
                                clipSpaceIBounds,
                                clipSpaceToStencilSpaceOffset);

    // This must occur after createStencilClipMask. That function may change the scissor. Also, it
    // only guarantees that the stencil mask is correct within the bounds it was passed, so we must
    // use both stencil and scissor test to the bounds for the final draw.
    SkIRect scissorSpaceIBounds(clipSpaceIBounds);
    scissorSpaceIBounds.offset(clipSpaceToStencilSpaceOffset);
    scissorState->set(scissorSpaceIBounds);
    this->setDrawStateStencil(drawState, ars);
    return true;
}

namespace {
////////////////////////////////////////////////////////////////////////////////
// set up the OpenGL blend function to perform the specified
// boolean operation for alpha clip mask creation
void setup_boolean_blendcoeffs(SkRegion::Op op, GrDrawState* drawState) {
    switch (op) {
        case SkRegion::kReplace_Op:
            drawState->setBlendFunc(kOne_GrBlendCoeff, kZero_GrBlendCoeff);
            break;
        case SkRegion::kIntersect_Op:
            drawState->setBlendFunc(kDC_GrBlendCoeff, kZero_GrBlendCoeff);
            break;
        case SkRegion::kUnion_Op:
            drawState->setBlendFunc(kOne_GrBlendCoeff, kISC_GrBlendCoeff);
            break;
        case SkRegion::kXOR_Op:
            drawState->setBlendFunc(kIDC_GrBlendCoeff, kISC_GrBlendCoeff);
            break;
        case SkRegion::kDifference_Op:
            drawState->setBlendFunc(kZero_GrBlendCoeff, kISC_GrBlendCoeff);
            break;
        case SkRegion::kReverseDifference_Op:
            drawState->setBlendFunc(kIDC_GrBlendCoeff, kZero_GrBlendCoeff);
            break;
        default:
            SkASSERT(false);
            break;
    }
}
}

////////////////////////////////////////////////////////////////////////////////
bool GrClipMaskManager::drawElement(GrDrawState* drawState,
                                    GrTexture* target,
                                    const SkClipStack::Element* element,
                                    GrPathRenderer* pr) {
    GrDrawTarget::AutoGeometryPush agp(fClipTarget);

    drawState->setRenderTarget(target->asRenderTarget());

    // TODO: Draw rrects directly here.
    switch (element->getType()) {
        case Element::kEmpty_Type:
            SkDEBUGFAIL("Should never get here with an empty element.");
            break;
        case Element::kRect_Type:
            // TODO: Do rects directly to the accumulator using a aa-rect GrProcessor that covers
            // the entire mask bounds and writes 0 outside the rect.
            if (element->isAA()) {
                this->getContext()->getAARectRenderer()->fillAARect(fClipTarget,
                                                                    drawState,
                                                                    element->getRect(),
                                                                    SkMatrix::I(),
                                                                    element->getRect());
            } else {
                fClipTarget->drawSimpleRect(drawState, element->getRect());
            }
            return true;
        default: {
            SkPath path;
            element->asPath(&path);
            path.setIsVolatile(true);
            if (path.isInverseFillType()) {
                path.toggleInverseFillType();
            }
            SkStrokeRec stroke(SkStrokeRec::kFill_InitStyle);
            if (NULL == pr) {
                GrPathRendererChain::DrawType type;
                type = element->isAA() ? GrPathRendererChain::kColorAntiAlias_DrawType :
                                         GrPathRendererChain::kColor_DrawType;
                pr = this->getContext()->getPathRenderer(fClipTarget, drawState, path, stroke,
                                                         false, type);
            }
            if (NULL == pr) {
                return false;
            }

            pr->drawPath(fClipTarget, drawState, path, stroke, element->isAA());
            break;
        }
    }
    return true;
}

bool GrClipMaskManager::canStencilAndDrawElement(GrDrawState* drawState,
                                                 GrTexture* target,
                                                 GrPathRenderer** pr,
                                                 const SkClipStack::Element* element) {
    drawState->setRenderTarget(target->asRenderTarget());

    if (Element::kRect_Type == element->getType()) {
        return true;
    } else {
        // We shouldn't get here with an empty clip element.
        SkASSERT(Element::kEmpty_Type != element->getType());
        SkPath path;
        element->asPath(&path);
        if (path.isInverseFillType()) {
            path.toggleInverseFillType();
        }
        SkStrokeRec stroke(SkStrokeRec::kFill_InitStyle);
        GrPathRendererChain::DrawType type = element->isAA() ?
            GrPathRendererChain::kStencilAndColorAntiAlias_DrawType :
            GrPathRendererChain::kStencilAndColor_DrawType;
        *pr = this->getContext()->getPathRenderer(fClipTarget, drawState, path, stroke, false,
                                                  type);
        return SkToBool(*pr);
    }
}

void GrClipMaskManager::mergeMask(GrDrawState* drawState,
                                  GrTexture* dstMask,
                                  GrTexture* srcMask,
                                  SkRegion::Op op,
                                  const SkIRect& dstBound,
                                  const SkIRect& srcBound) {
    SkAssertResult(drawState->setIdentityViewMatrix());

    drawState->setRenderTarget(dstMask->asRenderTarget());

    setup_boolean_blendcoeffs(op, drawState);

    SkMatrix sampleM;
    sampleM.setIDiv(srcMask->width(), srcMask->height());

    drawState->addColorProcessor(
        GrTextureDomainEffect::Create(srcMask,
                                      sampleM,
                                      GrTextureDomain::MakeTexelDomain(srcMask, srcBound),
                                      GrTextureDomain::kDecal_Mode,
                                      GrTextureParams::kNone_FilterMode))->unref();
    fClipTarget->drawSimpleRect(drawState, SkRect::Make(dstBound));
}

GrTexture* GrClipMaskManager::createTempMask(int width, int height) {
    GrSurfaceDesc desc;
    desc.fFlags = kRenderTarget_GrSurfaceFlag|kNoStencil_GrSurfaceFlag;
    desc.fWidth = width;
    desc.fHeight = height;
    desc.fConfig = kAlpha_8_GrPixelConfig;

    return this->getContext()->refScratchTexture(desc, GrContext::kApprox_ScratchTexMatch);
}

////////////////////////////////////////////////////////////////////////////////
// Return the texture currently in the cache if it exists. Otherwise, return NULL
GrTexture* GrClipMaskManager::getCachedMaskTexture(int32_t elementsGenID,
                                                   const SkIRect& clipSpaceIBounds) {
    bool cached = fAACache.canReuse(elementsGenID, clipSpaceIBounds);
    if (!cached) {
        return NULL;
    }

    return fAACache.getLastMask();
}

////////////////////////////////////////////////////////////////////////////////
// Allocate a texture in the texture cache. This function returns the texture
// allocated (or NULL on error).
GrTexture* GrClipMaskManager::allocMaskTexture(int32_t elementsGenID,
                                               const SkIRect& clipSpaceIBounds,
                                               bool willUpload) {
    // Since we are setting up the cache we should free up the
    // currently cached mask so it can be reused.
    fAACache.reset();

    GrSurfaceDesc desc;
    desc.fFlags = willUpload ? kNone_GrSurfaceFlags : kRenderTarget_GrSurfaceFlag;
    desc.fWidth = clipSpaceIBounds.width();
    desc.fHeight = clipSpaceIBounds.height();
    desc.fConfig = kRGBA_8888_GrPixelConfig;
    if (willUpload || this->getContext()->isConfigRenderable(kAlpha_8_GrPixelConfig, false)) {
        // We would always like A8 but it isn't supported on all platforms
        desc.fConfig = kAlpha_8_GrPixelConfig;
    }

    fAACache.acquireMask(elementsGenID, desc, clipSpaceIBounds);
    return fAACache.getLastMask();
}

////////////////////////////////////////////////////////////////////////////////
// Create a 8-bit clip mask in alpha
GrTexture* GrClipMaskManager::createAlphaClipMask(int32_t elementsGenID,
                                                  GrReducedClip::InitialState initialState,
                                                  const GrReducedClip::ElementList& elements,
                                                  const SkIRect& clipSpaceIBounds) {
    SkASSERT(kNone_ClipMaskType == fCurrClipMaskType);

    // First, check for cached texture
    GrTexture* result = this->getCachedMaskTexture(elementsGenID, clipSpaceIBounds);
    if (result) {
        fCurrClipMaskType = kAlpha_ClipMaskType;
        return result;
    }

    // There's no texture in the cache. Let's try to allocate it then.
    result = this->allocMaskTexture(elementsGenID, clipSpaceIBounds, false);
    if (NULL == result) {
        fAACache.reset();
        return NULL;
    }

    // The top-left of the mask corresponds to the top-left corner of the bounds.
    SkVector clipToMaskOffset = {
        SkIntToScalar(-clipSpaceIBounds.fLeft),
        SkIntToScalar(-clipSpaceIBounds.fTop)
    };
    // The texture may be larger than necessary, this rect represents the part of the texture
    // we populate with a rasterization of the clip.
    SkIRect maskSpaceIBounds = SkIRect::MakeWH(clipSpaceIBounds.width(), clipSpaceIBounds.height());

    // Set the matrix so that rendered clip elements are transformed to mask space from clip space.
    SkMatrix translate;
    translate.setTranslate(clipToMaskOffset);

    // The scratch texture that we are drawing into can be substantially larger than the mask. Only
    // clear the part that we care about.
    fClipTarget->clear(&maskSpaceIBounds,
                       GrReducedClip::kAllIn_InitialState == initialState ? 0xffffffff : 0x00000000,
                       true,
                       result->asRenderTarget());

    // When we use the stencil in the below loop it is important to have this clip installed.
    // The second pass that zeros the stencil buffer renders the rect maskSpaceIBounds so the first
    // pass must not set values outside of this bounds or stencil values outside the rect won't be
    // cleared.
    GrDrawTarget::AutoClipRestore acr(fClipTarget, maskSpaceIBounds);
    SkAutoTUnref<GrTexture> temp;

    // walk through each clip element and perform its set op
    for (GrReducedClip::ElementList::Iter iter = elements.headIter(); iter.get(); iter.next()) {
        const Element* element = iter.get();
        SkRegion::Op op = element->getOp();
        bool invert = element->isInverseFilled();
        if (invert || SkRegion::kIntersect_Op == op || SkRegion::kReverseDifference_Op == op) {
            GrDrawState drawState(translate);
            // We're drawing a coverage mask and want coverage to be run through the blend function.
            drawState.enableState(GrDrawState::kCoverageDrawing_StateBit |
                                  GrDrawState::kClip_StateBit);

            GrPathRenderer* pr = NULL;
            bool useTemp = !this->canStencilAndDrawElement(&drawState, result, &pr, element);
            GrTexture* dst;
            // This is the bounds of the clip element in the space of the alpha-mask. The temporary
            // mask buffer can be substantially larger than the actually clip stack element. We
            // touch the minimum number of pixels necessary and use decal mode to combine it with
            // the accumulator.
            SkIRect maskSpaceElementIBounds;

            if (useTemp) {
                if (invert) {
                    maskSpaceElementIBounds = maskSpaceIBounds;
                } else {
                    SkRect elementBounds = element->getBounds();
                    elementBounds.offset(clipToMaskOffset);
                    elementBounds.roundOut(&maskSpaceElementIBounds);
                }

                if (!temp) {
                    temp.reset(this->createTempMask(maskSpaceIBounds.fRight,
                                                    maskSpaceIBounds.fBottom));
                    if (!temp) {
                        fAACache.reset();
                        return NULL;
                    }
                }
                dst = temp;
                // clear the temp target and set blend to replace
                fClipTarget->clear(&maskSpaceElementIBounds,
                                   invert ? 0xffffffff : 0x00000000,
                                   true,
                                   dst->asRenderTarget());
                setup_boolean_blendcoeffs(SkRegion::kReplace_Op, &drawState);
            } else {
                // draw directly into the result with the stencil set to make the pixels affected
                // by the clip shape be non-zero.
                dst = result;
                GR_STATIC_CONST_SAME_STENCIL(kStencilInElement,
                                             kReplace_StencilOp,
                                             kReplace_StencilOp,
                                             kAlways_StencilFunc,
                                             0xffff,
                                             0xffff,
                                             0xffff);
                drawState.setStencil(kStencilInElement);
                setup_boolean_blendcoeffs(op, &drawState);
            }

            drawState.setAlpha(invert ? 0x00 : 0xff);

            // We have to backup the drawstate because the drawElement call may call into
            // renderers which consume it.
            GrDrawState backupDrawState(drawState);

            if (!this->drawElement(&drawState, dst, element, pr)) {
                fAACache.reset();
                return NULL;
            }

            if (useTemp) {
                // Now draw into the accumulator using the real operation and the temp buffer as a
                // texture
                this->mergeMask(&backupDrawState,
                                result,
                                temp,
                                op,
                                maskSpaceIBounds,
                                maskSpaceElementIBounds);
            } else {
                // Draw to the exterior pixels (those with a zero stencil value).
                backupDrawState.setAlpha(invert ? 0xff : 0x00);
                GR_STATIC_CONST_SAME_STENCIL(kDrawOutsideElement,
                                             kZero_StencilOp,
                                             kZero_StencilOp,
                                             kEqual_StencilFunc,
                                             0xffff,
                                             0x0000,
                                             0xffff);
                backupDrawState.setStencil(kDrawOutsideElement);
                fClipTarget->drawSimpleRect(&backupDrawState, clipSpaceIBounds);
            }
        } else {
            GrDrawState drawState(translate);
            drawState.enableState(GrDrawState::kCoverageDrawing_StateBit |
                                  GrDrawState::kClip_StateBit);

            // all the remaining ops can just be directly draw into the accumulation buffer
            drawState.setAlpha(0xff);
            setup_boolean_blendcoeffs(op, &drawState);
            this->drawElement(&drawState, result, element);
        }
    }

    fCurrClipMaskType = kAlpha_ClipMaskType;
    return result;
}

////////////////////////////////////////////////////////////////////////////////
// Create a 1-bit clip mask in the stencil buffer. 'devClipBounds' are in device
// (as opposed to canvas) coordinates
bool GrClipMaskManager::createStencilClipMask(GrRenderTarget* rt,
                                              int32_t elementsGenID,
                                              GrReducedClip::InitialState initialState,
                                              const GrReducedClip::ElementList& elements,
                                              const SkIRect& clipSpaceIBounds,
                                              const SkIPoint& clipSpaceToStencilOffset) {
    SkASSERT(kNone_ClipMaskType == fCurrClipMaskType);
    SkASSERT(rt);

    // TODO: dynamically attach a SB when needed.
    GrStencilBuffer* stencilBuffer = rt->getStencilBuffer();
    if (NULL == stencilBuffer) {
        return false;
    }

    if (stencilBuffer->mustRenderClip(elementsGenID, clipSpaceIBounds, clipSpaceToStencilOffset)) {
        stencilBuffer->setLastClip(elementsGenID, clipSpaceIBounds, clipSpaceToStencilOffset);
        // Set the matrix so that rendered clip elements are transformed from clip to stencil space.
        SkVector translate = {
            SkIntToScalar(clipSpaceToStencilOffset.fX),
            SkIntToScalar(clipSpaceToStencilOffset.fY)
        };
        SkMatrix matrix;
        matrix.setTranslate(translate);

        // We set the current clip to the bounds so that our recursive draws are scissored to them.
        SkIRect stencilSpaceIBounds(clipSpaceIBounds);
        stencilSpaceIBounds.offset(clipSpaceToStencilOffset);
        GrDrawTarget::AutoClipRestore acr(fClipTarget, stencilSpaceIBounds);

        int clipBit = stencilBuffer->bits();
        SkASSERT((clipBit <= 16) && "Ganesh only handles 16b or smaller stencil buffers");
        clipBit = (1 << (clipBit-1));

        fClipTarget->clearStencilClip(stencilSpaceIBounds,
                                      GrReducedClip::kAllIn_InitialState == initialState,
                                      rt);

        // walk through each clip element and perform its set op
        // with the existing clip.
        for (GrReducedClip::ElementList::Iter iter(elements.headIter()); iter.get(); iter.next()) {
            const Element* element = iter.get();

            GrDrawState drawState(matrix);
            drawState.setRenderTarget(rt);
            drawState.enableState(GrDrawState::kClip_StateBit);
            drawState.enableState(GrDrawState::kNoColorWrites_StateBit);

            // if the target is MSAA then we want MSAA enabled when the clip is soft
            if (rt->isMultisampled()) {
                drawState.setState(GrDrawState::kHWAntialias_StateBit, element->isAA());
            }

            bool fillInverted = false;
            // enabled at bottom of loop
            fClipMode = kIgnoreClip_StencilClipMode;

            // This will be used to determine whether the clip shape can be rendered into the
            // stencil with arbitrary stencil settings.
            GrPathRenderer::StencilSupport stencilSupport;

            SkStrokeRec stroke(SkStrokeRec::kFill_InitStyle);
            SkRegion::Op op = element->getOp();

            GrPathRenderer* pr = NULL;
            SkPath clipPath;
            if (Element::kRect_Type == element->getType()) {
                stencilSupport = GrPathRenderer::kNoRestriction_StencilSupport;
                fillInverted = false;
            } else {
                element->asPath(&clipPath);
                fillInverted = clipPath.isInverseFillType();
                if (fillInverted) {
                    clipPath.toggleInverseFillType();
                }
                pr = this->getContext()->getPathRenderer(fClipTarget,
                                                         &drawState,
                                                         clipPath,
                                                         stroke,
                                                         false,
                                                         GrPathRendererChain::kStencilOnly_DrawType,
                                                         &stencilSupport);
                if (NULL == pr) {
                    return false;
                }
            }

            int passes;
            GrStencilSettings stencilSettings[GrStencilSettings::kMaxStencilClipPasses];

            bool canRenderDirectToStencil =
                GrPathRenderer::kNoRestriction_StencilSupport == stencilSupport;
            bool canDrawDirectToClip; // Given the renderer, the element,
                                      // fill rule, and set operation can
                                      // we render the element directly to
                                      // stencil bit used for clipping.
            canDrawDirectToClip = GrStencilSettings::GetClipPasses(op,
                                                                   canRenderDirectToStencil,
                                                                   clipBit,
                                                                   fillInverted,
                                                                   &passes,
                                                                   stencilSettings);

            // draw the element to the client stencil bits if necessary
            if (!canDrawDirectToClip) {
                GR_STATIC_CONST_SAME_STENCIL(gDrawToStencil,
                                             kIncClamp_StencilOp,
                                             kIncClamp_StencilOp,
                                             kAlways_StencilFunc,
                                             0xffff,
                                             0x0000,
                                             0xffff);
                if (Element::kRect_Type == element->getType()) {
                    *drawState.stencil() = gDrawToStencil;
                    fClipTarget->drawSimpleRect(&drawState, element->getRect());
                } else {
                    if (!clipPath.isEmpty()) {
                        GrDrawTarget::AutoGeometryPush agp(fClipTarget);
                        if (canRenderDirectToStencil) {
                            *drawState.stencil() = gDrawToStencil;
                            pr->drawPath(fClipTarget, &drawState, clipPath, stroke, false);
                        } else {
                            pr->stencilPath(fClipTarget, &drawState, clipPath, stroke);
                        }
                    }
                }
            }

            // now we modify the clip bit by rendering either the clip
            // element directly or a bounding rect of the entire clip.
            fClipMode = kModifyClip_StencilClipMode;
            for (int p = 0; p < passes; ++p) {
                GrDrawState drawStateCopy(drawState);
                *drawStateCopy.stencil() = stencilSettings[p];

                if (canDrawDirectToClip) {
                    if (Element::kRect_Type == element->getType()) {
                        fClipTarget->drawSimpleRect(&drawStateCopy, element->getRect());
                    } else {
                        GrDrawTarget::AutoGeometryPush agp(fClipTarget);
                        pr->drawPath(fClipTarget, &drawStateCopy, clipPath, stroke, false);
                    }
                } else {
                    // The view matrix is setup to do clip space -> stencil space translation, so
                    // draw rect in clip space.
                    fClipTarget->drawSimpleRect(&drawStateCopy, SkRect::Make(clipSpaceIBounds));
                }
            }
        }
    }
    // set this last because recursive draws may overwrite it back to kNone.
    SkASSERT(kNone_ClipMaskType == fCurrClipMaskType);
    fCurrClipMaskType = kStencil_ClipMaskType;
    fClipMode = kRespectClip_StencilClipMode;
    return true;
}

// mapping of clip-respecting stencil funcs to normal stencil funcs
// mapping depends on whether stencil-clipping is in effect.
static const GrStencilFunc
    gSpecialToBasicStencilFunc[2][kClipStencilFuncCount] = {
    {// Stencil-Clipping is DISABLED,  we are effectively always inside the clip
        // In the Clip Funcs
        kAlways_StencilFunc,          // kAlwaysIfInClip_StencilFunc
        kEqual_StencilFunc,           // kEqualIfInClip_StencilFunc
        kLess_StencilFunc,            // kLessIfInClip_StencilFunc
        kLEqual_StencilFunc,          // kLEqualIfInClip_StencilFunc
        // Special in the clip func that forces user's ref to be 0.
        kNotEqual_StencilFunc,        // kNonZeroIfInClip_StencilFunc
                                      // make ref 0 and do normal nequal.
    },
    {// Stencil-Clipping is ENABLED
        // In the Clip Funcs
        kEqual_StencilFunc,           // kAlwaysIfInClip_StencilFunc
                                      // eq stencil clip bit, mask
                                      // out user bits.

        kEqual_StencilFunc,           // kEqualIfInClip_StencilFunc
                                      // add stencil bit to mask and ref

        kLess_StencilFunc,            // kLessIfInClip_StencilFunc
        kLEqual_StencilFunc,          // kLEqualIfInClip_StencilFunc
                                      // for both of these we can add
                                      // the clip bit to the mask and
                                      // ref and compare as normal
        // Special in the clip func that forces user's ref to be 0.
        kLess_StencilFunc,            // kNonZeroIfInClip_StencilFunc
                                      // make ref have only the clip bit set
                                      // and make comparison be less
                                      // 10..0 < 1..user_bits..
    }
};

namespace {
// Sets the settings to clip against the stencil buffer clip while ignoring the
// client bits.
const GrStencilSettings& basic_apply_stencil_clip_settings() {
    // stencil settings to use when clip is in stencil
    GR_STATIC_CONST_SAME_STENCIL_STRUCT(gSettings,
        kKeep_StencilOp,
        kKeep_StencilOp,
        kAlwaysIfInClip_StencilFunc,
        0x0000,
        0x0000,
        0x0000);
    return *GR_CONST_STENCIL_SETTINGS_PTR_FROM_STRUCT_PTR(&gSettings);
}
}

void GrClipMaskManager::setDrawStateStencil(GrDrawState* drawState,
                                            GrDrawState::AutoRestoreStencil* ars) {
    // We make two copies of the StencilSettings here (except in the early
    // exit scenario. One copy from draw state to the stack var. Then another
    // from the stack var to the gpu. We could make this class hold a ptr to
    // GrGpu's fStencilSettings and eliminate the stack copy here.

    // use stencil for clipping if clipping is enabled and the clip
    // has been written into the stencil.
    GrStencilSettings settings;

    // The GrGpu client may not be using the stencil buffer but we may need to
    // enable it in order to respect a stencil clip.
    if (drawState->getStencil().isDisabled()) {
        if (GrClipMaskManager::kRespectClip_StencilClipMode == fClipMode) {
            settings = basic_apply_stencil_clip_settings();
        } else {
            return;
        }
    } else {
        settings = drawState->getStencil();
    }

    // TODO: dynamically attach a stencil buffer
    int stencilBits = 0;
    GrStencilBuffer* stencilBuffer = drawState->getRenderTarget()->getStencilBuffer();
    if (stencilBuffer) {
        stencilBits = stencilBuffer->bits();
    }

    SkASSERT(fClipTarget->caps()->stencilWrapOpsSupport() || !settings.usesWrapOp());
    SkASSERT(fClipTarget->caps()->twoSidedStencilSupport() || !settings.isTwoSided());
    this->adjustStencilParams(&settings, fClipMode, stencilBits);
    ars->set(drawState);
    drawState->setStencil(settings);
}

void GrClipMaskManager::adjustStencilParams(GrStencilSettings* settings,
                                            StencilClipMode mode,
                                            int stencilBitCnt) {
    SkASSERT(stencilBitCnt > 0);

    if (kModifyClip_StencilClipMode == mode) {
        // We assume that this clip manager itself is drawing to the GrGpu and
        // has already setup the correct values.
        return;
    }

    unsigned int clipBit = (1 << (stencilBitCnt - 1));
    unsigned int userBits = clipBit - 1;

    GrStencilSettings::Face face = GrStencilSettings::kFront_Face;
    bool twoSided = fClipTarget->caps()->twoSidedStencilSupport();

    bool finished = false;
    while (!finished) {
        GrStencilFunc func = settings->func(face);
        uint16_t writeMask = settings->writeMask(face);
        uint16_t funcMask = settings->funcMask(face);
        uint16_t funcRef = settings->funcRef(face);

        SkASSERT((unsigned) func < kStencilFuncCount);

        writeMask &= userBits;

        if (func >= kBasicStencilFuncCount) {
            int respectClip = kRespectClip_StencilClipMode == mode;
            if (respectClip) {
                // The GrGpu class should have checked this
                SkASSERT(this->isClipInStencil());
                switch (func) {
                    case kAlwaysIfInClip_StencilFunc:
                        funcMask = clipBit;
                        funcRef = clipBit;
                        break;
                    case kEqualIfInClip_StencilFunc:
                    case kLessIfInClip_StencilFunc:
                    case kLEqualIfInClip_StencilFunc:
                        funcMask = (funcMask & userBits) | clipBit;
                        funcRef  = (funcRef  & userBits) | clipBit;
                        break;
                    case kNonZeroIfInClip_StencilFunc:
                        funcMask = (funcMask & userBits) | clipBit;
                        funcRef = clipBit;
                        break;
                    default:
                        SkFAIL("Unknown stencil func");
                }
            } else {
                funcMask &= userBits;
                funcRef &= userBits;
            }
            const GrStencilFunc* table =
                gSpecialToBasicStencilFunc[respectClip];
            func = table[func - kBasicStencilFuncCount];
            SkASSERT(func >= 0 && func < kBasicStencilFuncCount);
        } else {
            funcMask &= userBits;
            funcRef &= userBits;
        }

        settings->setFunc(face, func);
        settings->setWriteMask(face, writeMask);
        settings->setFuncMask(face, funcMask);
        settings->setFuncRef(face, funcRef);

        if (GrStencilSettings::kFront_Face == face) {
            face = GrStencilSettings::kBack_Face;
            finished = !twoSided;
        } else {
            finished = true;
        }
    }
    if (!twoSided) {
        settings->copyFrontSettingsToBack();
    }
}

////////////////////////////////////////////////////////////////////////////////
GrTexture* GrClipMaskManager::createSoftwareClipMask(int32_t elementsGenID,
                                                     GrReducedClip::InitialState initialState,
                                                     const GrReducedClip::ElementList& elements,
                                                     const SkIRect& clipSpaceIBounds) {
    SkASSERT(kNone_ClipMaskType == fCurrClipMaskType);

    GrTexture* result = this->getCachedMaskTexture(elementsGenID, clipSpaceIBounds);
    if (result) {
        return result;
    }

    // The mask texture may be larger than necessary. We round out the clip space bounds and pin
    // the top left corner of the resulting rect to the top left of the texture.
    SkIRect maskSpaceIBounds = SkIRect::MakeWH(clipSpaceIBounds.width(), clipSpaceIBounds.height());

    GrSWMaskHelper helper(this->getContext());

    SkMatrix matrix;
    matrix.setTranslate(SkIntToScalar(-clipSpaceIBounds.fLeft),
                        SkIntToScalar(-clipSpaceIBounds.fTop));

    helper.init(maskSpaceIBounds, &matrix, false);
    helper.clear(GrReducedClip::kAllIn_InitialState == initialState ? 0xFF : 0x00);
    SkStrokeRec stroke(SkStrokeRec::kFill_InitStyle);

    for (GrReducedClip::ElementList::Iter iter(elements.headIter()) ; iter.get(); iter.next()) {
        const Element* element = iter.get();
        SkRegion::Op op = element->getOp();

        if (SkRegion::kIntersect_Op == op || SkRegion::kReverseDifference_Op == op) {
            // Intersect and reverse difference require modifying pixels outside of the geometry
            // that is being "drawn". In both cases we erase all the pixels outside of the geometry
            // but leave the pixels inside the geometry alone. For reverse difference we invert all
            // the pixels before clearing the ones outside the geometry.
            if (SkRegion::kReverseDifference_Op == op) {
                SkRect temp = SkRect::Make(clipSpaceIBounds);
                // invert the entire scene
                helper.draw(temp, SkRegion::kXOR_Op, false, 0xFF);
            }
            SkPath clipPath;
            element->asPath(&clipPath);
            clipPath.toggleInverseFillType();
            helper.draw(clipPath, stroke, SkRegion::kReplace_Op, element->isAA(), 0x00);
            continue;
        }

        // The other ops (union, xor, diff) only affect pixels inside
        // the geometry so they can just be drawn normally
        if (Element::kRect_Type == element->getType()) {
            helper.draw(element->getRect(), op, element->isAA(), 0xFF);
        } else {
            SkPath path;
            element->asPath(&path);
            helper.draw(path, stroke, op, element->isAA(), 0xFF);
        }
    }

    // Allocate clip mask texture
    result = this->allocMaskTexture(elementsGenID, clipSpaceIBounds, true);
    if (NULL == result) {
        fAACache.reset();
        return NULL;
    }
    helper.toTexture(result);

    fCurrClipMaskType = kAlpha_ClipMaskType;
    return result;
}

////////////////////////////////////////////////////////////////////////////////
void GrClipMaskManager::purgeResources() {
    fAACache.purgeResources();
}

void GrClipMaskManager::setClipTarget(GrClipTarget* clipTarget) {
    fClipTarget = clipTarget;
    fAACache.setContext(clipTarget->getContext());
}

void GrClipMaskManager::adjustPathStencilParams(const GrStencilBuffer* stencilBuffer,
                                                GrStencilSettings* settings) {
    // TODO: dynamically attach a stencil buffer
    if (stencilBuffer) {
        int stencilBits = stencilBuffer->bits();
        this->adjustStencilParams(settings, fClipMode, stencilBits);
    }
}
