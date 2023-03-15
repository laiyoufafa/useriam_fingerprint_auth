/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
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
#ifndef FINGERPRINT_AUTH_SERVICE_H
#define FINGERPRINT_AUTH_SERVICE_H

#include <cstdint>
#include <mutex>

#include "nocopyable.h"
#include "system_ability.h"

namespace OHOS {
namespace UserIam {
namespace FingerprintAuth {
class FingerprintAuthService : public SystemAbility {
    DECLEAR_SYSTEM_ABILITY(FingerprintAuthService);

public:
    FingerprintAuthService();
    ~FingerprintAuthService() override = default;
    static std::shared_ptr<FingerprintAuthService> GetInstance();

    void OnStart() override;
    void OnStop() override;

private:
    void StartDriverManager();

    static std::mutex mutex_;
    static std::shared_ptr<FingerprintAuthService> instance_;
};
} // namespace FingerprintAuth
} // namespace UserIam
} // namespace OHOS

#endif // FINGERPRINT_AUTH_SERVICE_H
