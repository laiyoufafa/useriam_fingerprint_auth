/*
 * Copyright (c) 2023 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sensor_illumination_task.h"

#include "common_event_manager.h"
#include "common_event_subscriber.h"
#include "common_event_support.h"
#include "display_power_mgr_client.h"
#include "iservice_registry.h"
#include "pipeline/rs_render_thread.h"
#include "system_ability_definition.h"
#include "system_ability_status_change_stub.h"
#include "transaction/rs_transaction.h"
#include "transaction/rs_interfaces.h"
#include "ui/rs_surface_extractor.h"

#include "iam_check.h"
#include "iam_logger.h"
#include "iam_ptr.h"

#include "sa_command_manager.h"
#include "screen_state_monitor.h"

#define LOG_LABEL UserIam::Common::LABEL_FINGERPRINT_AUTH_SA

namespace OHOS {
namespace UserIam {
namespace FingerprintAuth {
using namespace EventFwk;
using namespace AAFwk;
using namespace UserAuth;
using namespace Rosen;

namespace {
constexpr uint32_t INVALID_BRIGHTNESS = -1;
constexpr uint32_t MAX_DISPLAY_TIME = 1000; // ms
constexpr uint32_t BRIGHTNESS_INDEX = 0;
constexpr uint32_t ALPHA_INDEX = 1;
constexpr uint32_t THOUSAND = 1000l; // center X and Y in per thousand
constexpr int32_t INVALID_DISPLAY_ID = -1;
constexpr float MAX_ZORDER = 100000.0f;
constexpr uint8_t BRIGHTNESS_AND_ALPHA[][2] = { { 4, 234 }, { 6, 229 }, { 8, 219 }, { 10, 220 }, { 12, 216 },
    { 14, 211 }, { 16, 208 }, { 20, 205 }, { 24, 187 }, { 28, 176 }, { 30, 170 }, { 34, 163 }, { 40, 159 }, { 46, 142 },
    { 50, 140 }, { 56, 140 }, { 64, 125 }, { 74, 121 }, { 84, 111 }, { 94, 101 }, { 104, 92 }, { 114, 81 }, { 124, 81 },
    { 134, 69 }, { 144, 68 }, { 154, 58 }, { 164, 56 }, { 174, 46 }, { 184, 44 }, { 194, 35 }, { 204, 34 }, { 214, 30 },
    { 224, 22 }, { 234, 23 }, { 244, 18 }, { 248, 0 }, { 255, 0 } };
ResultCode GetBackgroundAlpha(int32_t currScreenBrightness, uint32_t &outAlpha)
{
    for (uint32_t i = 0; i < sizeof(BRIGHTNESS_AND_ALPHA) / sizeof(BRIGHTNESS_AND_ALPHA[0]); i++) {
        if (currScreenBrightness > BRIGHTNESS_AND_ALPHA[i][BRIGHTNESS_INDEX]) {
            continue;
        } else if (currScreenBrightness == BRIGHTNESS_AND_ALPHA[i][BRIGHTNESS_INDEX] || i == 0) {
            outAlpha = BRIGHTNESS_AND_ALPHA[i][ALPHA_INDEX];
        } else {
            // linear interpolation
            float gapBrightnessDiff =
                BRIGHTNESS_AND_ALPHA[i][BRIGHTNESS_INDEX] - BRIGHTNESS_AND_ALPHA[i - 1][BRIGHTNESS_INDEX];
            float gapAlphaDiff = BRIGHTNESS_AND_ALPHA[i][ALPHA_INDEX] - BRIGHTNESS_AND_ALPHA[i - 1][ALPHA_INDEX];
            float slope = gapAlphaDiff / gapBrightnessDiff;
            float currBrightnessDiff = currScreenBrightness - BRIGHTNESS_AND_ALPHA[i - 1][BRIGHTNESS_INDEX];
            float currAlphaDiff = slope * currBrightnessDiff;
            outAlpha = BRIGHTNESS_AND_ALPHA[i - 1][ALPHA_INDEX] + static_cast<uint32_t>(currAlphaDiff);
        }
        IAM_LOGI("brightness %{public}d to alpha %{public}u", currScreenBrightness, outAlpha);
        return ResultCode::SUCCESS;
    }
    IAM_LOGE("brightness mismatch");
    return ResultCode::GENERAL_ERROR;
}

SkColor ConvertToSkColor(uint32_t color)
{
    uint8_t *colorBytes = static_cast<uint8_t *>(static_cast<void *>(&color));
    constexpr uint32_t RIndex = 0;
    constexpr uint32_t GIndex = 1;
    constexpr uint32_t BIndex = 2;
    constexpr uint32_t AIndex = 3;
    return SkColorSetARGB(colorBytes[AIndex], colorBytes[RIndex], colorBytes[GIndex], colorBytes[BIndex]);
}

struct CanvasParamStruct {
    uint32_t centerXInPx;
    uint32_t centerYInPy;
    uint32_t radius;
    uint32_t color;
};
using CanvasParam = CanvasParamStruct;

ResultCode DrawCanvas(std::shared_ptr<RSPaintFilterCanvas> canvas, const CanvasParam &param)
{
    IF_FALSE_LOGE_AND_RETURN_VAL(canvas != nullptr, ResultCode::GENERAL_ERROR);

    uint32_t brightness = DisplayPowerMgr::DisplayPowerMgrClient::GetInstance().GetDeviceBrightness();
    IF_FALSE_LOGE_AND_RETURN_VAL(brightness != INVALID_BRIGHTNESS, ResultCode::GENERAL_ERROR);
    IAM_LOGI("get device brightness %{public}u", brightness);
    uint32_t alpha;
    ResultCode result = GetBackgroundAlpha(brightness, alpha);
    IF_FALSE_LOGE_AND_RETURN_VAL(result == ResultCode::SUCCESS, ResultCode::GENERAL_ERROR);
    canvas->clear(SkColorSetARGB(alpha, 0x00, 0x00, 0x00));

    SkPaint paint;
    paint.setStyle(SkPaint::kFill_Style);
    paint.setAntiAlias(true);
    paint.setColor(ConvertToSkColor(param.color));

    canvas->drawCircle(SkPoint::Make(param.centerXInPx, param.centerYInPy), param.radius, paint);
    return ResultCode::SUCCESS;
}

bool convertThousandthToPx(uint32_t relativeLength, uint32_t fullLength, uint32_t &pxLength)
{
    uint64_t pxLengthUint64 = static_cast<uint64_t>(relativeLength) * static_cast<uint64_t>(fullLength) / THOUSAND;
    if (pxLengthUint64 > std::numeric_limits<uint32_t>::max()) {
        IAM_LOGE("pxLengthUint64 is more than UINT32_MAX");
        return false;
    }
    pxLength = static_cast<uint32_t>(pxLengthUint64);
    return true;
}
} // namespace

SensorIlluminationTask::SensorIlluminationTask() : timer_("sensor_illumination_timer")
{
    ScreenStateMonitor::GetInstance().Subscribe();
    timer_.Setup();
    currTimerId_ = 0;
    defaultDisplayId_ = INVALID_DISPLAY_ID;
    defaultScreenId_ = Rosen::INVALID_SCREEN_ID;
}

SensorIlluminationTask::~SensorIlluminationTask()
{
    ScreenStateMonitor::GetInstance().Unsubscribe();
    timer_.Unregister(currTimerId_);
    timer_.Shutdown();
    if (destructCallback_ != nullptr) {
        destructCallback_();
    }
}

ResultCode SensorIlluminationTask::EnableSensorIllumination(uint32_t centerX, uint32_t centerY, uint32_t radius,
    uint32_t color)
{
    std::lock_guard<std::recursive_mutex> lock(recursiveMutex_);
    IAM_LOGI("start");

    RSSurfaceNodeConfig config = { .SurfaceNodeName = "FingerprintSenor" };

    auto surfaceNode = RSSurfaceNode::Create(config, RSSurfaceNodeType::SELF_DRAWING_WINDOW_NODE);
    IF_FALSE_LOGE_AND_RETURN_VAL(surfaceNode != nullptr, ResultCode::GENERAL_ERROR);

    auto defaultDisplay = DisplayManager::GetInstance().GetDefaultDisplay();
    IF_FALSE_LOGE_AND_RETURN_VAL(defaultDisplay != nullptr, ResultCode::GENERAL_ERROR);

    surfaceNode->SetBounds(0, 0, defaultDisplay->GetWidth(), defaultDisplay->GetHeight());

    surfaceNode->SetFingerprint(true);

    std::shared_ptr<RSSurface> rsSurface = RSSurfaceExtractor::ExtractRSSurface(surfaceNode);
    IF_FALSE_LOGE_AND_RETURN_VAL(rsSurface != nullptr, ResultCode::GENERAL_ERROR);

    auto renderContext = std::shared_ptr<RenderContext>(RenderContextFactory::GetInstance().CreateNewEngine());
    IF_FALSE_LOGE_AND_RETURN_VAL(renderContext != nullptr, ResultCode::GENERAL_ERROR);
    renderContext->InitializeEglContext();
    rsSurface->SetRenderContext(renderContext.get());
    auto surfaceFrame = rsSurface->RequestFrame(defaultDisplay->GetWidth(), defaultDisplay->GetHeight());
    IF_FALSE_LOGE_AND_RETURN_VAL(surfaceFrame != nullptr, ResultCode::GENERAL_ERROR);
    auto skSurface = surfaceFrame->GetSurface();
    IF_FALSE_LOGE_AND_RETURN_VAL(skSurface != nullptr, ResultCode::GENERAL_ERROR);

    uint32_t centerXInPx = 0;
    bool convertRetX = convertThousandthToPx(centerX, defaultDisplay->GetWidth(), centerXInPx);
    IF_FALSE_LOGE_AND_RETURN_VAL(convertRetX == true, ResultCode::GENERAL_ERROR);
    uint32_t centerYInPx = 0;
    bool convertRetY = convertThousandthToPx(centerY, defaultDisplay->GetHeight(), centerYInPx);
    IF_FALSE_LOGE_AND_RETURN_VAL(convertRetY == true, ResultCode::GENERAL_ERROR);

    auto canvas = Common::MakeShared<RSPaintFilterCanvas>(skSurface.get());
    IF_FALSE_LOGE_AND_RETURN_VAL(canvas != nullptr, ResultCode::GENERAL_ERROR);
    auto drawCanvasResult = DrawCanvas(canvas, CanvasParam { centerXInPx, centerYInPx, radius, color });
    IF_FALSE_LOGE_AND_RETURN_VAL(drawCanvasResult == ResultCode::SUCCESS, ResultCode::GENERAL_ERROR);

    rsSurface->FlushFrame(surfaceFrame);

    auto transactionPolicy = RSTransactionProxy::GetInstance();
    transactionPolicy->FlushImplicitTransaction();

    defaultScreenId_ = Rosen::RSInterfaces::GetInstance().GetDefaultScreenId();
    defaultDisplayId_ = defaultDisplay->GetId();
    currRsSurface_ = surfaceNode;

    IAM_LOGI("success");
    return ResultCode::SUCCESS;
}

ResultCode SensorIlluminationTask::DisableSensorIllumination()
{
    std::lock_guard<std::recursive_mutex> lock(recursiveMutex_);
    IAM_LOGI("start");

    TurnOffSensorIllumination();
    currRsSurface_ = nullptr;

    ScreenStateMonitor::GetInstance().Unsubscribe();
    IAM_LOGI("success");
    return ResultCode::SUCCESS;
}

ResultCode SensorIlluminationTask::TurnOnSensorIllumination()
{
    std::lock_guard<std::recursive_mutex> lock(recursiveMutex_);
    IAM_LOGI("start");

    IF_FALSE_LOGE_AND_RETURN_VAL(currRsSurface_ != nullptr, ResultCode::GENERAL_ERROR);

    if (ScreenStateMonitor::GetInstance().GetScreenOn() == false) {
        IAM_LOGI("screen is not on, no need turn on display");
        return ResultCode::SUCCESS;
    }

    if (isIlluminationOn_) {
        IAM_LOGI("illumination already on");
        return ResultCode::SUCCESS;
    }

    timer_.Unregister(currTimerId_);
    currTimerId_ = timer_.Register(
        [weak_self = weak_from_this()]() {
            auto self = weak_self.lock();
            if (self == nullptr) {
                IAM_LOGE("object is released");
                return;
            }
            self->OnDisplayTimeOut();
        },
        MAX_DISPLAY_TIME, true);

    IF_FALSE_LOGE_AND_RETURN_VAL(defaultScreenId_ != Rosen::INVALID_SCREEN_ID, ResultCode::GENERAL_ERROR);
    currRsSurface_->SetPositionZ(MAX_ZORDER);
    currRsSurface_->AttachToDisplay(defaultScreenId_);
    OHOS::Rosen::RSTransaction::FlushImplicitTransaction();

    isIlluminationOn_ = true;

    IAM_LOGI("success");
    return ResultCode::SUCCESS;
}

ResultCode SensorIlluminationTask::TurnOffSensorIllumination()
{
    std::lock_guard<std::recursive_mutex> lock(recursiveMutex_);
    IAM_LOGI("start");

    IF_FALSE_LOGE_AND_RETURN_VAL(currRsSurface_ != nullptr, ResultCode::GENERAL_ERROR);

    if (!isIlluminationOn_) {
        IAM_LOGI("illumination already off");
        return ResultCode::SUCCESS;
    }

    timer_.Unregister(currTimerId_);

    IF_FALSE_LOGE_AND_RETURN_VAL(defaultScreenId_ != Rosen::INVALID_SCREEN_ID, ResultCode::GENERAL_ERROR);
    currRsSurface_->DetachToDisplay(defaultScreenId_);
    OHOS::Rosen::RSTransaction::FlushImplicitTransaction();

    isIlluminationOn_ = false;

    IAM_LOGI("success");
    return ResultCode::SUCCESS;
}

void SensorIlluminationTask::OnDisplayTimeOut()
{
    IAM_LOGI("start");
    TurnOffSensorIllumination();
}

void SensorIlluminationTask::RegisterDestructCallback(DestructCallback destructCallback)
{
    destructCallback_ = destructCallback;
}

std::shared_ptr<ISensorIlluminationTask> GetSensorIlluminationTask()
{
    return Common::MakeShared<SensorIlluminationTask>();
}
} // namespace FingerprintAuth
} // namespace UserIam
} // namespace OHOS