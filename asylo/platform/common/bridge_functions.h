/*
 *
 * Copyright 2018 Asylo authors
 *
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
 *
 */

#ifndef ASYLO_PLATFORM_COMMON_BRIDGE_FUNCTIONS_H_
#define ASYLO_PLATFORM_COMMON_BRIDGE_FUNCTIONS_H_

#include <csignal>
#include <cstdint>

#include "asylo/platform/common/bridge_types.h"

namespace asylo {

// Converts |bridge_siginfo| to a runtime siginfo_t. Returns nullptr if
// unsuccessful.
siginfo_t *FromBridgeSigInfo(const struct bridge_siginfo_t *bridge_siginfo,
                             siginfo_t *siginfo);

// Converts |siginfo| to a bridge siginfo_t. Returns nullptr if unsuccessful.
struct bridge_siginfo_t *ToBridgeSigInfo(
    const siginfo_t *siginfo, struct bridge_siginfo_t *bridge_siginfo);

}  // namespace asylo

#endif  // ASYLO_PLATFORM_COMMON_BRIDGE_FUNCTIONS_H_
