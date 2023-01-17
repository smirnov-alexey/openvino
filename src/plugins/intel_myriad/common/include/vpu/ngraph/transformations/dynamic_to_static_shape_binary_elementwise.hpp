// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include "ngraph/node.hpp"

#include <memory>

namespace vpu {

void dynamicToStaticShapeBinaryEltwise(std::shared_ptr<ngraph::Node> eltwise);

}  // namespace vpu
