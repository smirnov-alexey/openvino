// Copyright (C) 2018-2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//
#include <iterator>
#include <string>

#include "intel_gpu/runtime/error_handler.hpp"
#include "intel_gpu/runtime/memory.hpp"
#include "json_object.h"
#include "openvino/core/partial_shape.hpp"
#include "openvino/core/validation_util.hpp"
#include "primitive_type_base.h"
#include "reshape_inst.h"
#include "reshape_shape_inference.hpp"
#include "squeeze_shape_inference.hpp"
#include "unsqueeze_shape_inference.hpp"

namespace cldnn {
GPU_DEFINE_PRIMITIVE_TYPE_ID(reshape)

padding propagate_padding(const layout& in_layout, const ov::PartialShape& out_shape, reshape::reshape_mode mode, const ov::ITensorAccessor& ta) {
    if (mode == reshape::reshape_mode::base)
        return padding();

    auto in_pad = in_layout.data_padding;
    if (in_pad.get_dynamic_pad_dims() == tensor(0)) {
        return padding();
    }

    std::vector<int64_t> axes;
    if (auto t = ta(1)) {
        axes = ov::get_tensor_data_as<int64_t, std::vector<int64_t>>(t);
    } else {
        OPENVINO_THROW("[GPU] Can't propagate padding for reshape op as axes data is not available");
    }

    auto rank = in_layout.get_partial_shape().size();

    auto default_format = format::get_default_format(rank);

    auto pad_lower = in_pad.lower_size().sizes(default_format);
    auto pad_upper = in_pad.upper_size().sizes(default_format);
    auto pad_mask = in_pad.get_dynamic_pad_dims().sizes(default_format);

    std::vector<int32_t> update_pad_lower;
    std::vector<int32_t> update_pad_upper;
    std::vector<int32_t> update_pad_mask;

    if (mode == reshape::reshape_mode::unsqueeze) {
        update_pad_lower = pad_lower;
        update_pad_upper = pad_upper;
        update_pad_mask = pad_mask;

        std::unordered_set<int64_t> tmp(axes.begin(), axes.end());
        std::vector<int64_t> unique_axes;
        const auto expanded_rank = rank + tmp.size();
        std::transform(axes.begin(), axes.end(), std::back_inserter(unique_axes), [=](int64_t axis) {
            return ov::util::normalize(axis, expanded_rank);
        });

        // Normalize then remove repeated axes after normalization.
        for (const auto& axis : axes) {
            if (static_cast<size_t>(axis) <= out_shape.size()) {
                pad_lower.insert(std::next(std::begin(pad_lower), axis), 0);
                pad_upper.insert(std::next(std::begin(pad_upper), axis), 0);
                pad_mask.insert(std::next(std::begin(pad_mask), axis), 0);
            } else {
                pad_lower.push_back(0);
                pad_upper.push_back(0);
                pad_mask.push_back(0);
            }
        }
    } else {
        std::unordered_set<int64_t> unique_axes;
        std::transform(axes.begin(), axes.end(), std::inserter(unique_axes, unique_axes.end()), [=](int64_t axis) {
            return ov::util::normalize(axis, rank);
        });

        for (size_t i = 0; i < pad_lower.size(); i++) {
            auto rm_iter = unique_axes.find(i);
            if (rm_iter == unique_axes.end()) {
                update_pad_lower.push_back(pad_lower[i]);
                update_pad_upper.push_back(pad_upper[i]);
                update_pad_mask.push_back(pad_mask[i]);
            } else {
                // If we have a non-squeezable case (pad along removed axis), then out padding is reset
                // and kernel must be executed
                auto rm_axis = *rm_iter;
                if (pad_lower[rm_axis] != 0 || pad_upper[rm_axis] != 0 || pad_mask[rm_axis] != 0 )
                    return padding();
            }
        }
    }

    auto convert_pad = [](const std::vector<int32_t> pad) {
        return tensor(format::get_default_format(pad.size()), pad, 0);
    };

    return padding(convert_pad(update_pad_lower).sizes(),
                   convert_pad(update_pad_upper).sizes(),
                   0.0f,
                   convert_pad(update_pad_mask));
}

layout reshape_inst::calc_output_layout(reshape_node const& node, kernel_impl_params const& impl_param) {
    assert(static_cast<bool>(impl_param.desc->output_data_types[0]) == false &&
           "Output data type forcing is not supported for reshape_node!");
    auto input_layout = impl_param.get_non_padded_input_layout();
    auto desc = impl_param.typed_desc<reshape>();
    if (desc->output_shape.count() == 0) {
        if (desc->output_partial_shape.size() != 0) {
            format out_fmt = format::adjust_to_rank(input_layout.format, desc->output_partial_shape.rank().get_length());
            return layout{desc->output_partial_shape, input_layout.data_type, out_fmt};
        } else {
            OPENVINO_ASSERT("[GPU] Output shape is not provided");
        }
    }

    auto sizes = desc->output_shape.sizes();
    auto input_sizes = input_layout.get_tensor().sizes();
    size_t need_recalc = 0;
    uint32_t shape_count = 1;

    for (size_t i = 0; i < sizes.size(); i++) {
        if (sizes[i] == -1) {
            if (need_recalc) {
                CLDNN_ERROR_MESSAGE(desc->id, "Only one dimension of the new shape can be -1");
            }
            need_recalc = i;
            continue;
        }
        if (sizes[i] == 0) {
            sizes[i] = input_sizes[i];
        }
        shape_count *= sizes[i];
    }
    if (need_recalc)
        sizes[need_recalc] = static_cast<int>(input_layout.count()) / shape_count;

    return layout{input_layout.data_type, input_layout.format, tensor(sizes)};
}

template<typename ShapeType>
std::vector<layout> reshape_inst::calc_output_layouts(reshape_node const& node, const kernel_impl_params& impl_param) {
    assert(static_cast<bool>(impl_param.typed_desc<reshape>()->output_data_types[0]) == false &&
           "Output data type forcing is not supported for reshape_node!");
    auto prim = impl_param.typed_desc<reshape>();
    auto input_layout = impl_param.get_input_layout(0);

    auto& memory_deps = impl_param.memory_deps;
    // On program build stage for the cases with pattern being stored in a runtime tensor
    // we return output_partial_shape taken from the original model intead of something like PartialShape::dynamic(rank)
    // as ngraph may refine output shape using interval arithmetic
    if ((memory_deps.empty() && prim->output_pattern.empty()) || input_layout.is_dynamic()) {
        if (prim->output_shape.count() != 0) {
            return { layout{input_layout.data_type, input_layout.format, prim->output_shape} };
        } else {
            auto fm = format::adjust_to_rank(input_layout.format, prim->output_partial_shape.size());
            return { layout{prim->output_partial_shape, input_layout.data_type, fm} };
        }
    }

    ShapeType pattern_shape = impl_param.input_layouts.size() == 2 ? impl_param.get_input_layout(1).get<ShapeType>()
                                                                   : ShapeType(ov::Shape{ prim->output_pattern.size() });
    // Since reshape does not support 0D tensor(scalar) for shape input
    // the case propagated to 0D tensor should be handled manually with 1D tensor
    if (pattern_shape.size() == 0) {
        pattern_shape = ShapeType{1};
    }
    std::vector<ShapeType> output_shapes = {ShapeType()};
    std::vector<ShapeType> input_shapes = {
        input_layout.get<ShapeType>(),
        pattern_shape,
    };

    std::unordered_map<size_t, ov::Tensor> const_data;
    const auto ta = ov::make_tensor_accessor(const_data);
    padding out_pad = padding();

    auto run_shape_infer = [&](reshape::reshape_mode mode) {
         switch (mode) {
            case reshape::reshape_mode::base: {
                ov::op::v1::Reshape op;
                op.set_special_zero(prim->special_zero);
                op.set_friendly_name(prim->id.c_str());
                output_shapes = ov::op::v1::shape_infer(&op, input_shapes, ta);
                // If the reshape is base mode, it is currently not set as can_be_optimized at prepare_buffer_fusing.
                // So we can just run the reshape kernel
                // TODO: allow propagatable reshapes
                out_pad = padding();
                break;
            }
            case reshape::reshape_mode::squeeze: {
                ov::op::v0::Squeeze op;
                op.set_friendly_name(prim->id.c_str());
                output_shapes = shape_infer(&op, input_shapes, ta);
                out_pad = propagate_padding(input_layout, output_shapes[0], prim->mode, ta);
                break;
            }
            case reshape::reshape_mode::unsqueeze: {
                ov::op::v0::Unsqueeze op;
                op.set_friendly_name(prim->id.c_str());
                output_shapes = shape_infer(&op, input_shapes, ta);
                out_pad = propagate_padding(input_layout, output_shapes[0], prim->mode, ta);
                break;
            }
            default:
                OPENVINO_THROW("Unsupported reshape mode");
        }
    };

    if (memory_deps.count(1) > 0) {
        auto pattern_mem = memory_deps.at(1);

        cldnn::mem_lock<uint8_t, mem_lock_type::read> pattern_lock(pattern_mem, impl_param.get_stream());

        auto pattern_ptr = pattern_lock.data();
        auto pattern_tensor = make_tensor(pattern_mem->get_layout(), pattern_ptr);

        const_data.emplace(1, pattern_tensor);
        run_shape_infer(prim->mode);
    } else {
        auto pattern_data = prim->output_pattern;
        auto pattern_tensor = make_tensor({pattern_shape, data_types::i64, format::bfyx}, static_cast<void*>(pattern_data.data()));

        const_data.emplace(1, pattern_tensor);
        run_shape_infer(prim->mode);
    }

    auto output_format = input_layout.format;
    if (node.get_preferred_output_fmt() != format::any) {
        output_format = node.get_preferred_output_fmt();
    }

    auto new_out_pad = out_pad;
    if (new_out_pad == padding())
        new_out_pad = impl_param.get_output_layout(0).data_padding;

    return { layout {output_shapes[0], input_layout.data_type, format::adjust_to_rank(output_format, output_shapes[0].size()), new_out_pad} };
}

template std::vector<layout> reshape_inst::calc_output_layouts<ov::PartialShape>(reshape_node const& node, const kernel_impl_params& impl_param);

std::string reshape_inst::to_string(reshape_node const& node) {
    auto desc = node.get_primitive();
    auto node_info = node.desc_to_json();
    auto& input = node.input();

    std::stringstream primitive_description;

    json_composite reshape_info;
    reshape_info.add("input id", input.id());
    reshape_info.add("output shape", desc->output_shape);
    reshape_info.add("output pshape", desc->output_partial_shape);
    reshape_info.add("output pattern", desc->output_pattern);
    reshape_info.add("special zero", desc->special_zero);

    node_info->add("reshape info", reshape_info);
    node_info->dump(primitive_description);

    return primitive_description.str();
}

reshape_inst::typed_primitive_inst(network& network, reshape_node const& node) :
        parent(network, node, (!node.can_be_optimized() && node.get_output_layout().is_static()) ? true : false) {
    auto input_layout = node.get_input_layout();
    auto output_layout = node.get_output_layout();
    CLDNN_ERROR_DATA_TYPES_MISMATCH(node.id(),
                                    "Input layout data typr",
                                    input_layout.data_type,
                                    "output layout data type",
                                    output_layout.data_type,
                                    "");
    if (output_layout.is_static() && input_layout.is_static())
        CLDNN_ERROR_NOT_EQUAL(node.id(),
                              "Output layout count",
                              output_layout.count(),
                              "input layout count",
                              input_layout.count(),
                              "Output layout of reshape primitive changes size of input buffer");

    // if reshape operated in-place, postpone creation of the output until network run,
    // then create new memory object as the reinterpreted output of the previous primitive
    if (input_layout.is_static() && output_layout.is_static()) {
        if (!node.can_be_optimized()) {
            _outputs = allocate_outputs();
            _mem_allocated = true;
        } else {
            update_output_memory();
        }
    } else {
        if (_exec_deps.size() > 0 && input_memory_ptr())
            update_output_memory();
    }
}

void reshape_inst::on_execute() {
    update_output_memory();
}

void reshape_inst::update_output_memory() {
    if (!can_be_optimized())
        return;

    if (_outputs[0] && _network.get_engine().is_the_same_buffer(output_memory(), input_memory()) &&
        output_memory().get_layout() == _impl_params->get_output_layout())
        return;

    build_deps();  // reshape need deps
    if (node->get_program().is_new_shape_infer() && input_memory_ptr() == nullptr)
        return;
    OPENVINO_ASSERT(input_memory_ptr() != nullptr, "[GPU] Failed to reuse input in ", id(), " primitive: input memory was not allocated");
    _outputs = {_network.get_engine().reinterpret_buffer(input_memory(), _impl_params->get_output_layout())};
}

}  // namespace cldnn
