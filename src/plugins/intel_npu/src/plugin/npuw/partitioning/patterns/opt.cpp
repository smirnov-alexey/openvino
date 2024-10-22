// Copyright (C) 2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "opt.hpp"

#include "../../logging.hpp"
#include "../../util.hpp"
#include "openvino/op/add.hpp"
#include "openvino/op/broadcast.hpp"
#include "openvino/op/concat.hpp"
#include "openvino/op/convert.hpp"
#include "openvino/op/gather.hpp"
#include "openvino/op/matmul.hpp"
#include "openvino/op/multiply.hpp"
#include "openvino/op/reduce_sum.hpp"
#include "openvino/op/reshape.hpp"
#include "openvino/op/slice.hpp"
#include "openvino/op/split.hpp"
#include "openvino/op/subtract.hpp"
#include "openvino/op/util/op_types.hpp"
#include "openvino/pass/pattern/op/label.hpp"  // any_input
#include "openvino/pass/pattern/op/optional.hpp"
#include "openvino/pass/pattern/op/wrap_type.hpp"
#include "openvino/util/common_util.hpp"

namespace ov {
namespace npuw {
namespace patterns {
namespace opt {

void Context::permute(PPtr orig_param, const Context::Axes& order) {
    closures_to_permute[orig_param] = order;
}

void Context::to_f16(PPtr orig_param) {
    closures_to_f16.insert(orig_param);

    orig_param->set_element_type(ov::element::f16);
    orig_param->validate_and_infer_types();
}

void Context::register_parallel_matmul(O multiply, std::size_t axis, DQParMM&& mm) {
    par_dq_mms[std::make_pair(multiply, axis)].push_back(std::move(mm));
}

Context::PPtr Context::concat(ov::ParameterVector&& v, std::size_t dim) {
    // Sanity check dimensions - all dims other tham dim must match
    std::size_t sum = 0u;
    const auto& first = v.front();
    const auto first_shape = first->get_shape();
    for (auto&& p : v) {
        const auto& this_shape = p->get_shape();
        NPUW_ASSERT(first_shape.size() == this_shape.size());
        for (std::size_t d = 0; d < first_shape.size(); d++) {
            if (d != dim) {
                NPUW_ASSERT(first_shape[d] == this_shape[d]);
            } else {
                sum += this_shape[d];
            }
        }
        NPUW_ASSERT(first->get_element_type() == p->get_element_type());
    }
    auto out_shape = first_shape;
    out_shape[dim] = sum;

    auto new_param = std::make_shared<ov::op::v0::Parameter>(first->get_element_type(), out_shape);
    params_to_concat[new_param] = {std::move(v), dim};
    return new_param;
}

Context::PPtr Context::unpack(Context::PPtr w, Context::PPtr z, Context::PPtr s, ov::element::Type type) {
    // FIXME: Assume CW only
    NPUW_ASSERT(w->get_shape().size() == 2);
    NPUW_ASSERT(z->get_shape().size() == 2);
    NPUW_ASSERT(s->get_shape().size() == 2);
    auto new_param = std::make_shared<ov::op::v0::Parameter>(type, w->get_shape());
    params_to_unpack[new_param] = {w, z, s};
    return new_param;
}

Context::PPtr Context::unpack(Context::PPtr w, Context::PPtr s, ov::element::Type type) {
    const auto& w_shape = w->get_shape();
    const auto& s_shape = s->get_shape();

    Context::PPtr new_param;
    if (w_shape.size() == 3 && s_shape.size() == 3) {
        // Assume already reshaped tensor (as it does with unpack)
        ov::Shape new_shape = {w_shape[0], w_shape[1] * w_shape[2]};
        new_param = std::make_shared<ov::op::v0::Parameter>(type, new_shape);
    } else if (w_shape.size() == 2 && s_shape.size() == 2) {
        new_param = std::make_shared<ov::op::v0::Parameter>(type, w_shape);
    } else {
        NPUW_ASSERT(false && "Yet unsupported combination");
    }

    NPUW_ASSERT(new_param);
    params_to_unpack[new_param] = {w, {}, s};
    return new_param;
}

Context::PPtr Context::host_gather(Context::PPtr w, Context::PPtr ids) {
    const auto& w_shape = w->get_shape();
    const auto& ids_shape = ids->get_shape();

    NPUW_ASSERT(w_shape.size() == 2);
    NPUW_ASSERT(ids_shape.size() == 2);
    NPUW_ASSERT(ids_shape[0] == 1);

    ov::Shape new_shape = {1, ids_shape[1], w_shape[1]};
    auto new_param = std::make_shared<ov::op::v0::Parameter>(w->get_element_type(), new_shape);
    params_to_gather = Gather{new_param, w, ids};
    return new_param;
}

namespace opp = ov::pass::pattern;

// FROM:
//     ???(Act) ----------------------------------->
//     Param(W) -> to(f16) -> Multiply -> to(f32) -> MatMul
//     Param(S) ------------>
//
// TO:
//     ???(Act) -> to(f16) ->
//     Param(W) -> to(f16) -> MatMul -> Multiply -> to(f32)
//     Param(S) -> Reshape ----------->
//

DQMatMulCWi::DQMatMulCWi() {
    auto qweight = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcoeff = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcvtw = opp::wrap_type<ov::op::v0::Convert>({qweight});
    auto qmuls = opp::wrap_type<ov::op::v1::Multiply>({qcvtw, qcoeff});
    auto qcvtm = opp::wrap_type<ov::op::v0::Convert>({qmuls});
    auto qmmi = opp::any_input();
    auto qmm = opp::wrap_type<ov::op::v0::MatMul>({qmmi, qcvtm});

    // Note: Use [=] to make sure the above objects stay alive in the callback
    auto callback = [=](ov::pass::pattern::Matcher& m) {
        auto& node_to_output = m.get_pattern_value_map();

        auto matched_node_qweight = node_to_output.at(qweight).get_node_shared_ptr();
        auto matched_node_qcoeff = node_to_output.at(qcoeff).get_node_shared_ptr();
        auto matched_node_matmul = node_to_output.at(qmm).get_node_shared_ptr();

        auto matched_qweight = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qweight);
        auto matched_qcoeff = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qcoeff);
        auto matched_matmul = std::static_pointer_cast<ov::op::v0::MatMul>(matched_node_matmul);

        auto qcoeff_shape = matched_qcoeff->output(0).get_shape();

        if ((ov::element::i4 == matched_qweight->get_element_type() ||
             ov::element::i8 == matched_qweight->get_element_type()) &&
            qcoeff_shape[1] == 1 && !matched_matmul->get_transpose_a() && matched_matmul->get_transpose_b()) {
            auto matched_node_cvtw = node_to_output.at(qcvtw).get_node_shared_ptr();
            auto matched_node_cvtm = node_to_output.at(qcvtm).get_node_shared_ptr();
            auto matched_node_muls = node_to_output.at(qmuls).get_node_shared_ptr();
            auto matched_node_mmi = node_to_output.at(qmmi).get_node_shared_ptr();

            // Reconnect MatMul to read from Convert(W) directly.
            // Note: ACT is f32 so has to be converted too.
            auto new_cvt_act = std::make_shared<ov::op::v0::Convert>(matched_node_mmi, ov::element::f16);
            matched_matmul->input(0).replace_source_output(new_cvt_act);
            matched_matmul->input(1).replace_source_output(matched_node_cvtw);

            // Store MatMul's readers
            auto mm_readers = matched_matmul->output(0).get_target_inputs();

            // Introduce a Reshape to alter Scale factor's shape
            auto new_dims = std::vector<std::size_t>{qcoeff_shape[1], qcoeff_shape[0]};
            auto new_const = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{2}, new_dims);
            auto new_reshape = std::make_shared<ov::op::v1::Reshape>(matched_node_qcoeff, new_const, false);

            // Reconnect Multiply's both inputs. Drop all outputs
            matched_node_muls->input(0).replace_source_output(matched_matmul);
            matched_node_muls->input(1).replace_source_output(new_reshape);
            for (auto&& r : matched_node_muls->output(0).get_target_inputs()) {
                matched_node_muls->output(0).remove_target_input(r);
            }

            // Reconnect Convert(M) to convert the Multiply's result
            matched_node_cvtm->input(0).replace_source_output(matched_node_muls);

            // Reconnect MatMul's old readers to Convert(Multiply)
            for (auto&& r : mm_readers) {
                r.replace_source_output(matched_node_cvtm);
            }
        }

        return true;  // root has changed
    };
    register_matcher(std::make_shared<opp::Matcher>(qmm, "OptDQMatMulCWi"), std::move(callback));
}

// 1 token case (generate)
//
// FROM:
//     ???(Act) -------------------------------------------->
//     Param(W) -> Convert(f16|f32) -> Multiply -> Reshape -> MatMul
//     Param(S) --------------------->
//
// WHERE (example):
//     Act: [ 1,  1, 4096]
//     W:   [32,128,11008]
//     S:   [32,  1,11008]
//                                         [1, 1 ,128]   x
// TO:                                     [1,11K,128]T  =
//                 [32,1,128]              [1, 1 ,11K]        [32,1,11K]
//     ???(Act)  -> Reshape > Split(/32) ->[to(f16) ->       ]}
//     Param(W*) -----------> Split(/32) ->[to(f16) -> MatMul]} Concat v
//     Param(S)  ---------------------------------------------> Multiply
//                                                              Reshape(1,a,b,c)
//                                                              ReduceSum(1)
//                                                              Reshape(a,b,c)
//                                                              to(f32)
// WHERE:
//     W* : [32,11008,128]

DQMatMulGQi::DQMatMulGQi(Context::Ref ctx) {
    auto qweight = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcoeff = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcvtw = opp::wrap_type<ov::op::v0::Convert>({qweight});
    auto qmuls = opp::wrap_type<ov::op::v1::Multiply>({qcvtw, qcoeff});
    auto qreshp = opp::wrap_type<ov::op::v1::Reshape>({qmuls, opp::any_input()});
    auto qmmi = opp::any_input();
    auto qmm = opp::wrap_type<ov::op::v0::MatMul>({qmmi, qreshp});

    // Note: Use [=] to make sure the above objects stay alive in the callback
    auto callback = [=](ov::pass::pattern::Matcher& m) {
        auto& node_to_output = m.get_pattern_value_map();

        auto matched_node_qweight = node_to_output.at(qweight).get_node_shared_ptr();
        auto matched_node_qcoeff = node_to_output.at(qcoeff).get_node_shared_ptr();
        auto matched_node_matmul = node_to_output.at(qmm).get_node_shared_ptr();
        auto matched_out_mmi = node_to_output.at(qmmi);

        auto matched_qweight = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qweight);
        auto matched_qcoeff = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qcoeff);
        auto matched_matmul = std::static_pointer_cast<ov::op::v0::MatMul>(matched_node_matmul);

        auto qweight_shape = matched_qweight->output(0).get_shape();
        auto qcoeff_shape = matched_qcoeff->output(0).get_shape();
        auto act_shape = matched_out_mmi.get_shape();
        auto out_shape = matched_node_matmul->output(0).get_shape();

        if (ov::element::i4 == matched_qweight->get_element_type() && qweight_shape.size() == 3 &&
            ov::element::f32 == matched_qcoeff->get_element_type() && qcoeff_shape.size() == 3 &&
            act_shape.size() == 3 && act_shape[1] == 1 &&  // single-token case
            qcoeff_shape[0] == qweight_shape[0] && qcoeff_shape[1] == 1 && qcoeff_shape[2] == qweight_shape[2] &&
            !matched_matmul->get_transpose_a() && !matched_matmul->get_transpose_b()) {
            // Mark W closure to transpose, and transpose the respective parameter
            ov::Shape tw_shape = {qweight_shape[0], qweight_shape[2], qweight_shape[1]};
            matched_qweight->set_partial_shape(tw_shape);
            matched_qweight->validate_and_infer_types();
            ctx.get().permute(matched_qweight, {0, 2, 1});

            // Mark S closure to be lowered fo f16
            ctx.get().to_f16(matched_qcoeff);

            // Reshape the Act to group format
            const auto NSPLIT = qweight_shape[0];
            std::vector<std::size_t> rshp_act_v = {NSPLIT, act_shape[1], act_shape[2] / NSPLIT};
            auto rshp_act_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{3}, rshp_act_v);
            auto rshp_act = std::make_shared<ov::op::v1::Reshape>(matched_out_mmi, rshp_act_c, false);

            // Split Act and W, and S tensors by NSPLIT
            auto split_axis = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 0);
            auto split_a = std::make_shared<ov::op::v1::Split>(rshp_act, split_axis, NSPLIT);
            auto split_w = std::make_shared<ov::op::v1::Split>(matched_qweight, split_axis, NSPLIT);

            // Do the CW MM for every split
            std::vector<std::shared_ptr<ov::Node>> to_concat;
            for (std::size_t i = 0; i < NSPLIT; i++) {
                auto a_f16 = std::make_shared<ov::op::v0::Convert>(split_a->output(i), ov::element::f16);
                auto w_f16 = std::make_shared<ov::op::v0::Convert>(split_w->output(i), ov::element::f16);
                auto m_f16 = std::make_shared<ov::op::v0::MatMul>(a_f16, w_f16, false, true);
                to_concat.push_back(m_f16);
            }

            // Now concat and scale the result
            auto concat = std::make_shared<ov::op::v0::Concat>(to_concat, 0);
            auto s_f16 = std::make_shared<ov::op::v1::Multiply>(concat, matched_qcoeff);

            // Now reshape to a better shape, ReduceSum, and reshape to the right size again
            std::vector<std::size_t> rshp_ccat_v = {1, NSPLIT, 1, qweight_shape[2]};
            auto rshp_ccat_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{4}, rshp_ccat_v);
            auto rshp_ccat = std::make_shared<ov::op::v1::Reshape>(s_f16, rshp_ccat_c, false);

            auto reduce_axis = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 1);
            auto reduce = std::make_shared<ov::op::v1::ReduceSum>(rshp_ccat, reduce_axis, true);

            auto rshp_out_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{3}, out_shape);
            auto rshp_out = std::make_shared<ov::op::v1::Reshape>(reduce, rshp_out_c, false);

            // Convert the result to f32 to maintain the graph contracts. FIXME should be avoided
            auto out = std::make_shared<ov::op::v0::Convert>(rshp_out, ov::element::f32);

            // Now.. Reconnect the matmul readers to the new output (reducesum)
            for (auto&& r : matched_matmul->output(0).get_target_inputs()) {
                r.replace_source_output(out);
            }
            return true;  // root has changed
        }
        return false;  // did nothing here
    };
    register_matcher(std::make_shared<opp::Matcher>(qmm, "OptDQMatMulGQi"), std::move(callback));
}

// FROM:
//     ???(Act) -------------------------------------------------------->
//     Param(W) -> Convert(f16) -> Multiply -> Reshape -> Convert(f32) -> MatMul
//     Param(S) ----------------->
//
// WHERE (example):
//     Act: [  1, 1,2048]
//     W:   [512,16, 128]
//     S:   [512,16,   1]
//                                         [1,  1,128]   x
// TO:                                     [1,512,128]T  =
//                 [16,1,128]              [1,  1,512]            [16,1,512]
//     ???(Act)  -> Reshape > Split(/16) ->[to(f16) ->         ]}
//     Param(W*) -----------> Split(/16) ->[to(f16) -> MatMul >]} Concat
//                                                                 v
//     Param(S*) ---------------------------------------------> Multiply
//                                                              Reshape(1,16,1,512)
//                                                              ReduceSum(1)
//                                                              Reshape(   1,1,512)
//                                                              to(f32)
// WHERE:
//     W* : [16,512,128]
//     S* : [16,  1,512]

DQMatMulGQ2i::DQMatMulGQ2i(Context::Ref ctx) {
    auto qweight = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcoeff = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcvtw = opp::wrap_type<ov::op::v0::Convert>({qweight});
    auto qmuls = opp::wrap_type<ov::op::v1::Multiply>({qcvtw, qcoeff});
    auto qreshp = opp::wrap_type<ov::op::v1::Reshape>({qmuls, opp::any_input()});
    auto qcvtr = opp::optional<ov::op::v0::Convert>({qreshp->output(0)});
    auto qmmi = opp::any_input();
    auto qmm = opp::wrap_type<ov::op::v0::MatMul>({qmmi, qcvtr});

    // Note: Use [=] to make sure the above objects stay alive in the callback
    auto callback = [=](ov::pass::pattern::Matcher& m) {
        auto& node_to_output = m.get_pattern_value_map();

        auto matched_node_qweight = node_to_output.at(qweight).get_node_shared_ptr();
        auto matched_node_qcoeff = node_to_output.at(qcoeff).get_node_shared_ptr();
        auto matched_node_matmul = node_to_output.at(qmm).get_node_shared_ptr();
        auto matched_out_mmi = node_to_output.at(qmmi);

        auto matched_qweight = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qweight);
        auto matched_qcoeff = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qcoeff);
        auto matched_matmul = std::static_pointer_cast<ov::op::v0::MatMul>(matched_node_matmul);

        auto qweight_shape = matched_qweight->output(0).get_shape();
        auto qcoeff_shape = matched_qcoeff->output(0).get_shape();
        auto act_shape = matched_out_mmi.get_shape();
        auto out_shape = matched_node_matmul->output(0).get_shape();

        if (ov::element::i4 == matched_qweight->get_element_type() && qweight_shape.size() == 3 &&
            ov::element::f16 == matched_qcoeff->get_element_type() && qcoeff_shape.size() == 3 &&
            act_shape.size() == 3 && act_shape[1] == 1 && qcoeff_shape[0] == qweight_shape[0] && qcoeff_shape[2] == 1 &&
            qcoeff_shape[1] == qweight_shape[1] && !matched_matmul->get_transpose_a() &&
            matched_matmul->get_transpose_b()) {
            // Mark W closure to transpose, and transpose the respective parameter
            ctx.get().permute(matched_qweight, {1, 0, 2});

            ov::Shape tw_shape = {qweight_shape[1], qweight_shape[0], qweight_shape[2]};
            matched_qweight->set_partial_shape(tw_shape);
            matched_qweight->validate_and_infer_types();

            // Also transpose S, but in a different way (see diagram above)
            ctx.get().permute(matched_qcoeff, {1, 2, 0});

            ov::Shape ts_shape = {qcoeff_shape[1], qcoeff_shape[2], qcoeff_shape[0]};
            matched_qcoeff->set_partial_shape(ts_shape);
            matched_qcoeff->validate_and_infer_types();

            // Reshape the Act to group format
            const auto NSPLIT = qweight_shape[1];
            std::vector<std::size_t> rshp_act_v = {NSPLIT, 1, act_shape[2] / NSPLIT};
            auto rshp_act_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{3}, rshp_act_v);
            auto rshp_act = std::make_shared<ov::op::v1::Reshape>(matched_out_mmi, rshp_act_c, false);

            // Split Act and W, and S tensors by NSPLIT
            auto split_axis = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 0);
            auto split_a = std::make_shared<ov::op::v1::Split>(rshp_act, split_axis, NSPLIT);
            auto split_w = std::make_shared<ov::op::v1::Split>(matched_qweight, split_axis, NSPLIT);

            std::vector<std::size_t> rshp_scale_v = {1, 1, qcoeff_shape[0]};
            auto rshp_scale_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{3}, rshp_scale_v);

            // Do the CW MM for every split
            std::vector<std::shared_ptr<ov::Node>> to_concat;
            for (std::size_t i = 0; i < NSPLIT; i++) {
                auto a_f16 = std::make_shared<ov::op::v0::Convert>(split_a->output(i), ov::element::f16);
                auto w_f16 = std::make_shared<ov::op::v0::Convert>(split_w->output(i), ov::element::f16);
                auto m_f16 = std::make_shared<ov::op::v0::MatMul>(a_f16, w_f16, false, true);
                to_concat.push_back(m_f16);
            }

            // Now concat and scale the result
            auto concat = std::make_shared<ov::op::v0::Concat>(to_concat, 0);
            auto scaled = std::make_shared<ov::op::v1::Multiply>(concat, matched_qcoeff);

            // Now reshape to a better shape, ReduceSum, and reshape to the right size again
            std::vector<std::size_t> rshp_ccat_v = {1, NSPLIT, 1, qweight_shape[0]};
            auto rshp_ccat_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{4}, rshp_ccat_v);
            auto rshp_ccat = std::make_shared<ov::op::v1::Reshape>(scaled, rshp_ccat_c, false);

            auto reduce_axis = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 1);
            // Make reduceSum not to keep axis because then it will convert to poolings in compiler.
            // Otherwise reduceSum will convert to the convolution which is less efficient than poolings.
            auto reduce = std::make_shared<ov::op::v1::ReduceSum>(rshp_ccat, reduce_axis, false);

            auto rshp_out_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{3}, out_shape);
            auto rshp_out = std::make_shared<ov::op::v1::Reshape>(reduce, rshp_out_c, false);

            // Convert the result to f32 to maintain the graph contracts if required.
            std::shared_ptr<ov::Node> out = rshp_out;
            if (matched_matmul->get_element_type() == ov::element::f32) {
                out = std::make_shared<ov::op::v0::Convert>(rshp_out, ov::element::f32);
            }

            // Now.. Reconnect the matmul readers to the new output (reducesum)
            for (auto&& r : matched_matmul->output(0).get_target_inputs()) {
                r.replace_source_output(out);
            }
            return true;  // root has changed
        }
        return false;  // did nothing here
    };
    register_matcher(std::make_shared<opp::Matcher>(qmm, "OptDQMatMulGQ2i"), std::move(callback));
}

// N token case (prompt)
//
// FROM:
//     ???(Act) -------------------------------------------->
//     Param(W) -> Convert(f16|f32) -> Multiply -> Reshape -> MatMul
//     Param(S) --------------------->
//
// WHERE (example):
//     Act: [ 1,  N, 4096]
//     W:   [32,128,11008]
//     S:   [32,  1,11008]
//                                                              [1, N ,128]   x
// TO:                                                          [1,11K,128]T  =
//                 [N,32,128]                         [1,N,128] [1, N ,11K]     [32,N,11K]
//     ???(Act)  -> Reshape > Split(/32) ->[to(f16) -> Reshape ->            ]}
//     Param(W*) -----------> Split(/32) ->[to(f16) ------------> MatMul v   ]} 32xAdd
//     Param(S)  -------------Split(/32) ->[--------------------> Multiply   ]}     v
//                                                                             to(f32)
// WHERE:
//     W* : [32,11008,128]
DQMatMulGQiP::DQMatMulGQiP(Context::Ref ctx) {
    auto qweight = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcoeff = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcvtw = opp::wrap_type<ov::op::v0::Convert>({qweight});
    auto qmuls = opp::wrap_type<ov::op::v1::Multiply>({qcvtw, qcoeff});
    auto qreshp = opp::wrap_type<ov::op::v1::Reshape>({qmuls, opp::any_input()});
    auto qmmi = opp::any_input();
    auto qmm = opp::wrap_type<ov::op::v0::MatMul>({qmmi, qreshp});

    // Note: Use [=] to make sure the above objects stay alive in the callback
    auto callback = [=](ov::pass::pattern::Matcher& m) {
        auto& node_to_output = m.get_pattern_value_map();

        auto matched_node_qweight = node_to_output.at(qweight).get_node_shared_ptr();
        auto matched_node_qcoeff = node_to_output.at(qcoeff).get_node_shared_ptr();
        auto matched_node_matmul = node_to_output.at(qmm).get_node_shared_ptr();
        auto matched_out_mmi = node_to_output.at(qmmi);

        auto matched_qweight = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qweight);
        auto matched_qcoeff = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qcoeff);
        auto matched_matmul = std::static_pointer_cast<ov::op::v0::MatMul>(matched_node_matmul);

        auto qweight_shape = matched_qweight->output(0).get_shape();
        auto qcoeff_shape = matched_qcoeff->output(0).get_shape();
        auto act_shape = matched_out_mmi.get_shape();

        if (ov::element::i4 == matched_qweight->get_element_type() && qweight_shape.size() == 3 &&
            ov::element::f32 == matched_qcoeff->get_element_type() && qcoeff_shape.size() == 3 &&
            act_shape.size() == 3 && act_shape[1] > 1 &&  // multi-token case
            qcoeff_shape[0] == qweight_shape[0] && qcoeff_shape[1] == 1 && qcoeff_shape[2] == qweight_shape[2] &&
            !matched_matmul->get_transpose_a() && !matched_matmul->get_transpose_b()) {
            // Mark W closure to transpose, and transpose the respective parameter
            ov::Shape tw_shape = {qweight_shape[0], qweight_shape[2], qweight_shape[1]};
            matched_qweight->set_partial_shape(tw_shape);
            matched_qweight->validate_and_infer_types();
            ctx.get().permute(matched_qweight, {0, 2, 1});

            // Mark S closure to be lowered fo f16
            matched_qcoeff->set_element_type(ov::element::f16);
            matched_qcoeff->validate_and_infer_types();
            ctx.get().to_f16(matched_qcoeff);

            // Reshape the Act to group format
            const auto NSPLIT = qweight_shape[0];
            std::vector<std::size_t> rshp_act_v = {act_shape[1], NSPLIT, act_shape[2] / NSPLIT};
            auto rshp_act_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{3}, rshp_act_v);
            auto rshp_act = std::make_shared<ov::op::v1::Reshape>(matched_out_mmi, rshp_act_c, false);

            // Split Act and W, and S tensors by NSPLIT
            auto split_axis_a = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 1);
            auto split_a = std::make_shared<ov::op::v1::Split>(rshp_act, split_axis_a, NSPLIT);

            auto split_axis_w = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 0);
            auto split_w = std::make_shared<ov::op::v1::Split>(matched_qweight, split_axis_w, NSPLIT);
            auto split_s = std::make_shared<ov::op::v1::Split>(matched_qcoeff, split_axis_w, NSPLIT);

            std::vector<std::size_t> r_a_v = {1, act_shape[1], act_shape[2] / NSPLIT};
            auto r_a_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{3}, r_a_v);

            // Do the CW MM for every split
            std::vector<std::shared_ptr<ov::Node>> to_concat;
            for (std::size_t i = 0; i < NSPLIT; i++) {
                auto a_f16 = std::make_shared<ov::op::v0::Convert>(split_a->output(i), ov::element::f16);
                auto r_f16 = std::make_shared<ov::op::v1::Reshape>(a_f16, r_a_c, false);
                auto w_f16 = std::make_shared<ov::op::v0::Convert>(split_w->output(i), ov::element::f16);
                auto m_f16 = std::make_shared<ov::op::v0::MatMul>(r_f16, w_f16, false, true);
                auto s_f16 = std::make_shared<ov::op::v1::Multiply>(m_f16, split_s->output(i));
                to_concat.push_back(s_f16);
            }

            // Reduce via Add
            std::vector<ov::Output<ov::Node>> reduce;
            reduce.push_back(std::make_shared<ov::op::v1::Add>(to_concat[0], to_concat[1]));
            for (std::size_t i = 1; i < NSPLIT - 1; i++) {
                reduce.push_back(std::make_shared<ov::op::v1::Add>(reduce[i - 1], to_concat[i + 1]));
            }

            // Convert the result to f32 to maintain the graph contracts. FIXME should be avoided
            auto out = std::make_shared<ov::op::v0::Convert>(reduce.back(), ov::element::f32);

            // Now.. Reconnect the matmul readers to the new output (reducesum)
            for (auto&& r : matched_matmul->output(0).get_target_inputs()) {
                r.replace_source_output(out);
            }
            return true;  // root has changed
        }
        return false;  // did nothing here
    };
    register_matcher(std::make_shared<opp::Matcher>(qmm, "OptDQMatMulGQiP"), std::move(callback));
}

// N token case (prompt)
//
// FROM:
//     ???(Act) ------------------------------------------------------->
//     Param(W) -> Convert(f16|f32) -> Multiply -> Reshape -> Convert -> MatMul
//     Param(S) --------------------->
//
// WHERE (example):
//     Act: [    1, N,4096]
//     W:   [11008,32, 128]
//     S:   [11008,32,   1]
//                                                             [1, N ,128]   x
// TO:                                                         [1,11K,128]T  =
//                 [N,32,128]                        [1,N,128] [1, N ,11K]     [32,N,11K]
//     ???(Act)  -> Reshape > Split(/32) ->[to(f16) - Reshape ->            ]}
//     Param(W*) -----------> Split(/32) ->[to(f16) -----------> MatMul v   ]} 32xAdd
//     Param(S*) -----------> Split(/32) ->[-------------------> Multiply   ]}     v
//                                                                             to(f32)
// WHERE:
//     W* : [32,11008,  128]
//     S* : [32,    1,11008]
DQMatMulGQ2iP::DQMatMulGQ2iP(Context::Ref ctx) {
    auto qweight = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcoeff = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcvtw = opp::wrap_type<ov::op::v0::Convert>({qweight});
    auto qmuls = opp::wrap_type<ov::op::v1::Multiply>({qcvtw, qcoeff});
    auto qreshp = opp::wrap_type<ov::op::v1::Reshape>({qmuls, opp::any_input()});
    auto qcvtm = opp::optional<ov::op::v0::Convert>({qreshp->output(0)});
    auto qmmi = opp::any_input();
    auto qmm = opp::wrap_type<ov::op::v0::MatMul>({qmmi, qcvtm});

    // Note: Use [=] to make sure the above objects stay alive in the callback
    auto callback = [=](ov::pass::pattern::Matcher& m) {
        auto& node_to_output = m.get_pattern_value_map();

        auto matched_node_qweight = node_to_output.at(qweight).get_node_shared_ptr();
        auto matched_node_qcoeff = node_to_output.at(qcoeff).get_node_shared_ptr();
        auto matched_node_matmul = node_to_output.at(qmm).get_node_shared_ptr();
        auto matched_out_mmi = node_to_output.at(qmmi);

        auto matched_qweight = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qweight);
        auto matched_qcoeff = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qcoeff);
        auto matched_matmul = std::static_pointer_cast<ov::op::v0::MatMul>(matched_node_matmul);

        auto qweight_shape = matched_qweight->output(0).get_shape();
        auto qcoeff_shape = matched_qcoeff->output(0).get_shape();
        auto act_shape = matched_out_mmi.get_shape();

        if (ov::element::i4 == matched_qweight->get_element_type() && qweight_shape.size() == 3 &&
            ov::element::f16 == matched_qcoeff->get_element_type() && qcoeff_shape.size() == 3 &&
            act_shape.size() == 3 && act_shape[1] > 1 &&  // multi-token case
            qcoeff_shape[0] == qweight_shape[0] && qcoeff_shape[1] == qweight_shape[1] && qcoeff_shape[2] == 1 &&
            !matched_matmul->get_transpose_a() && matched_matmul->get_transpose_b()) {
            // Mark W closure to transpose, and transpose the respective parameter
            ov::Shape tw_shape = {qweight_shape[1], qweight_shape[0], qweight_shape[2]};
            matched_qweight->set_partial_shape(tw_shape);
            matched_qweight->validate_and_infer_types();
            ctx.get().permute(matched_qweight, {1, 0, 2});

            // Also transpose S, but in a different way (see diagram above)
            ctx.get().permute(matched_qcoeff, {1, 2, 0});

            ov::Shape ts_shape = {qcoeff_shape[1], qcoeff_shape[2], qcoeff_shape[0]};
            matched_qcoeff->set_partial_shape(ts_shape);
            matched_qcoeff->validate_and_infer_types();

            // Reshape the Act to group format
            const auto NSPLIT = qweight_shape[1];
            std::vector<std::size_t> rshp_act_v = {act_shape[1], NSPLIT, act_shape[2] / NSPLIT};
            auto rshp_act_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{3}, rshp_act_v);
            auto rshp_act = std::make_shared<ov::op::v1::Reshape>(matched_out_mmi, rshp_act_c, false);

            // Split Act and W, and S tensors by NSPLIT
            auto split_axis_a = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 1);
            auto split_a = std::make_shared<ov::op::v1::Split>(rshp_act, split_axis_a, NSPLIT);

            auto split_axis_w = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 0);
            auto split_w = std::make_shared<ov::op::v1::Split>(matched_qweight, split_axis_w, NSPLIT);
            auto split_s = std::make_shared<ov::op::v1::Split>(matched_qcoeff, split_axis_w, NSPLIT);

            std::vector<std::size_t> r_a_v = {1, act_shape[1], act_shape[2] / NSPLIT};
            auto r_a_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{3}, r_a_v);

            // Do the CW MM for every split
            std::vector<std::shared_ptr<ov::Node>> to_concat;
            for (std::size_t i = 0; i < NSPLIT; i++) {
                auto a_f16 = std::make_shared<ov::op::v0::Convert>(split_a->output(i), ov::element::f16);
                auto r_f16 = std::make_shared<ov::op::v1::Reshape>(a_f16, r_a_c, false);
                auto w_f16 = std::make_shared<ov::op::v0::Convert>(split_w->output(i), ov::element::f16);
                auto m_f16 = std::make_shared<ov::op::v0::MatMul>(r_f16, w_f16, false, true);
                auto s_f16 = std::make_shared<ov::op::v1::Multiply>(m_f16, split_s->output(i));
                to_concat.push_back(s_f16);
            }

            // Reduce via Add
            std::vector<ov::Output<ov::Node>> reduce;
            reduce.push_back(std::make_shared<ov::op::v1::Add>(to_concat[0], to_concat[1]));
            for (std::size_t i = 1; i < NSPLIT - 1; i++) {
                reduce.push_back(std::make_shared<ov::op::v1::Add>(reduce[i - 1], to_concat[i + 1]));
            }

            ov::Output<ov::Node> out = reduce.back();
            if (matched_matmul->output(0).get_element_type() == ov::element::f32) {
                // Convert the result to f32 to maintain the graph contracts, if needed
                out = std::make_shared<ov::op::v0::Convert>(out, ov::element::f32);
            }

            // Now.. Reconnect the matmul readers to the new output (reducesum)
            for (auto&& r : matched_matmul->output(0).get_target_inputs()) {
                r.replace_source_output(out);
            }
            return true;  // root has changed
        }
        return false;  // did nothing here
    };
    register_matcher(std::make_shared<opp::Matcher>(qmm, "OptDQMatMulGQ2iP"), std::move(callback));
}

////////////////////////////////////////////////////////////////////////////////
// Parallel matmuls
// Identifies this pattern
//
// Multiply -----------------------------------> MatMul
// Param(W) -> to(f32) -> Multiply -> Reshape ->
// Param(S) ------------>

DQParMMGQ::DQParMMGQ(Context::Ref ctx) {
    auto qweight = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcoeff = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcvtw = opp::wrap_type<ov::op::v0::Convert>({qweight});
    auto qmuls = opp::wrap_type<ov::op::v1::Multiply>({qcvtw, qcoeff});
    auto qreshp = opp::wrap_type<ov::op::v1::Reshape>({qmuls, opp::any_input()});
    auto qmmi = opp::wrap_type<ov::op::v1::Multiply>({opp::any_input(), opp::any_input()});
    auto qcvtr = opp::optional<ov::op::v0::Convert>({qreshp->output(0)});
    auto qmm = opp::wrap_type<ov::op::v0::MatMul>({qmmi, qcvtr});

    // Note: Use [=] to make sure the above objects stay alive in the callback
    auto callback = [=](ov::pass::pattern::Matcher& m) {
        auto& node_to_output = m.get_pattern_value_map();
        auto w_param =
            std::static_pointer_cast<ov::op::v0::Parameter>(node_to_output.at(qweight).get_node_shared_ptr());
        auto s_param = std::static_pointer_cast<ov::op::v0::Parameter>(node_to_output.at(qcoeff).get_node_shared_ptr());
        auto matmul = std::static_pointer_cast<ov::op::v0::MatMul>(node_to_output.at(qmm).get_node_shared_ptr());

        auto qmmi_shape = node_to_output.at(qmm).get_shape();

        if (qmmi_shape.size() != 3 || qmmi_shape[0] != 1) {
            // Not handling such cases
            return false;
        }

        if (!matmul->get_transpose_a() && !matmul->get_transpose_b()) {
            ctx.get().register_parallel_matmul(node_to_output.at(qmmi), 2, Context::DQParMM{w_param, s_param, matmul});
        } else if (!matmul->get_transpose_a() && matmul->get_transpose_b()) {
            ctx.get().register_parallel_matmul(node_to_output.at(qmmi), 0, Context::DQParMM{w_param, s_param, matmul});
        }
        return false;  // no change here
    };
    register_matcher(std::make_shared<opp::Matcher>(qmm, "OptDQParMMGQ"), std::move(callback));
}

void mergeParallelMatMuls(const std::shared_ptr<ov::Model>& m, Context& ctx) {
    for (auto&& mul_to_mms : ctx.par_dq_mms) {
        auto& parallel_matmuls = mul_to_mms.second;
        if (parallel_matmuls.size() < 2) {
            continue;
        }
        ov::Output<ov::Node> orig_multiply;

        std::size_t axis_to_concat = -1;
        std::tie(orig_multiply, axis_to_concat) = mul_to_mms.first;

        const ov::Shape orig_act_shape = orig_multiply.get_shape();

        if (!util::is_set(axis_to_concat, ctx.pmm_dims)) {
            LOG_VERB("Parallel MatMuls found, but fusion over dim " << axis_to_concat << " is not enabled");
            continue;
        }

        const auto& first_w = parallel_matmuls[0].w;
        const auto& first_s = parallel_matmuls[0].s;
        ov::ParameterVector old_w, old_s;
        bool all_ok = true;
        for (auto&& dqmm : parallel_matmuls) {
            if (first_w->get_shape().size() != dqmm.w->get_shape().size() ||
                first_s->get_shape().size() != dqmm.s->get_shape().size() ||
                dqmm.w->get_shape().size() != dqmm.s->get_shape().size()) {
                all_ok = false;
                break;
            }
            for (std::size_t d = 0u; d < first_w->get_shape().size(); d++) {
                if (d != axis_to_concat && (first_w->get_shape()[d] != dqmm.w->get_shape()[d] ||
                                            first_s->get_shape()[d] != dqmm.s->get_shape()[d])) {
                    all_ok = false;
                    break;
                }
            }
            old_w.push_back(dqmm.w);
            old_s.push_back(dqmm.s);
        }
        if (!all_ok) {
            continue;
        }
        auto new_w = ctx.concat(std::move(old_w), axis_to_concat);
        auto new_s = ctx.concat(std::move(old_s), axis_to_concat);
        auto new_cvt = std::make_shared<ov::op::v0::Convert>(new_w, new_s->get_element_type());

        std::shared_ptr<ov::Node> new_mul = std::make_shared<ov::op::v1::Multiply>(new_cvt, new_s);
        if ((new_s->get_element_type() == ov::element::f16) && (orig_multiply.get_element_type() == ov::element::f32)) {
            new_mul = std::make_shared<ov::op::v0::Convert>(new_mul, ov::element::f32);
        }
        auto new_w_shape = new_w->get_shape();

        using S = std::vector<std::size_t>;
        S new_rshp_v;
        if (axis_to_concat == 2) {
            new_rshp_v = S{new_w_shape[0] * new_w_shape[1], new_w_shape[2]};
        } else if (axis_to_concat == 0) {
            new_rshp_v = S{new_w_shape[0], new_w_shape[1] * new_w_shape[2]};
        } else {
            NPUW_ASSERT(false);
        }
        auto new_rshp_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{2}, new_rshp_v);
        auto new_rshp = std::make_shared<ov::op::v1::Reshape>(new_mul, new_rshp_c, false);

        // Transpose input_b if concat was done by 0th axis (meaning the original MM's input_b were also transposed)
        auto new_mm = std::make_shared<ov::op::v0::MatMul>(orig_multiply, new_rshp, false, (axis_to_concat == 0));

        // Create new slices & reconnect matmuls
        // FIXME: use zip
        std::size_t offset = 0u;
        for (std::size_t i = 0u; i < parallel_matmuls.size(); i++) {
            auto this_orig_wshape = parallel_matmuls[i].w->get_shape();
            auto this_slice_start =
                std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{3}, S{0, 0, offset});
            auto this_slice_end = std::make_shared<ov::op::v0::Constant>(
                ov::element::i32,
                ov::Shape{3},
                S{1, orig_act_shape[1], offset + this_orig_wshape[axis_to_concat]});
            auto this_slice_step = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{3}, S{1, 1, 1});
            auto this_slice =
                std::make_shared<ov::op::v8::Slice>(new_mm, this_slice_start, this_slice_end, this_slice_step);

            // redirect the original matmul's readers to the slice
            for (auto&& r : parallel_matmuls[i].mm->output(0).get_target_inputs()) {
                r.replace_source_output(this_slice);
            }
            offset += this_orig_wshape[axis_to_concat];
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Head/tail (Gather + Vocab)

// Identify a Gather+DQ Asym CW MatMul pattern, lift Gather up
// Note: this pattern is applied on the full model before any partitioning
DQLiftGatherAsymCW::DQLiftGatherAsymCW() {
    auto qweight = opp::wrap_type<ov::op::v0::Constant>();
    auto qzerop = opp::wrap_type<ov::op::v0::Constant>();
    auto qcoeff = opp::wrap_type<ov::op::v0::Constant>();
    auto qcvtw = opp::wrap_type<ov::op::v0::Convert>({qweight});
    auto qcvtz = opp::wrap_type<ov::op::v0::Convert>({qzerop});
    auto qsubz = opp::wrap_type<ov::op::v1::Subtract>({qcvtw, qcvtz});
    auto qmuls = opp::wrap_type<ov::op::v1::Multiply>({qsubz, qcoeff});
    auto qcvtm = opp::wrap_type<ov::op::v0::Convert>({qmuls});

    auto pids = opp::wrap_type<ov::op::v0::Parameter>();
    auto cvtids = opp::wrap_type<ov::op::v0::Convert>({pids});
    auto gather = opp::wrap_type<ov::op::v8::Gather>({qcvtm, cvtids, opp::any_input()});

    // Note: Use [=] to make sure the above objects stay alive in the callback
    auto callback = [=](ov::pass::pattern::Matcher& m) {
        auto& node_to_output = m.get_pattern_value_map();

        // Create new gathers on W, Z, and S respectively
        auto matched_out_w = node_to_output.at(qweight);
        auto matched_out_z = node_to_output.at(qzerop);
        auto matched_out_s = node_to_output.at(qcoeff);
        auto matched_out_ids = node_to_output.at(cvtids);
        const auto& matched_out_gather = node_to_output.at(gather);

        // Replicate the compute part
        auto gather_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 0);
        auto new_g_w = std::make_shared<ov::op::v8::Gather>(matched_out_w, matched_out_ids, gather_c);
        auto new_g_z = std::make_shared<ov::op::v8::Gather>(matched_out_z, matched_out_ids, gather_c);
        auto new_g_s = std::make_shared<ov::op::v8::Gather>(matched_out_s, matched_out_ids, gather_c);

        auto new_cvt_w = std::make_shared<ov::op::v0::Convert>(new_g_w, ov::element::f16);
        auto new_cvt_z = std::make_shared<ov::op::v0::Convert>(new_g_z, ov::element::f16);
        auto new_sub = std::make_shared<ov::op::v1::Subtract>(new_cvt_w, new_cvt_z);
        auto new_mul = std::make_shared<ov::op::v1::Multiply>(new_sub, new_g_s);
        auto new_out = std::make_shared<ov::op::v0::Convert>(new_mul, ov::element::f32);

        // Reconnect old gather readers to the new Multiply
        for (auto&& r : matched_out_gather.get_target_inputs()) {
            r.replace_source_output(new_out);
        }
        return true;  // root was changed
    };
    register_matcher(std::make_shared<opp::Matcher>(gather, "DQGatherAsymCW"), std::move(callback));
}

// Identify a Gather+DQ Sym CW MatMul pattern, lift Gather up
// Note: this pattern is applied on the full model before any partitioning
DQLiftGatherSymCW::DQLiftGatherSymCW() {
    auto qweight = opp::wrap_type<ov::op::v0::Constant>();
    auto qcoeff = opp::wrap_type<ov::op::v0::Constant>();
    auto qcvtw = opp::wrap_type<ov::op::v0::Convert>({qweight});
    auto qmuls = opp::wrap_type<ov::op::v1::Multiply>({qcvtw, qcoeff});
    auto qcvtm = opp::wrap_type<ov::op::v0::Convert>({qmuls});

    auto pids = opp::wrap_type<ov::op::v0::Parameter>();
    auto cvtids = opp::wrap_type<ov::op::v0::Convert>({pids});
    auto gather = opp::wrap_type<ov::op::v8::Gather>({qcvtm, cvtids, opp::any_input()});

    // Note: Use [=] to make sure the above objects stay alive in the callback
    auto callback = [=](ov::pass::pattern::Matcher& m) {
        auto& node_to_output = m.get_pattern_value_map();

        auto matched_out_w = node_to_output.at(qweight);
        auto matched_out_s = node_to_output.at(qcoeff);
        auto matched_out_ids = node_to_output.at(cvtids);
        const auto& matched_out_gather = node_to_output.at(gather);

        // Create new gathers on W and S, connect respectively
        auto new_cvt_w = std::make_shared<ov::op::v0::Convert>(matched_out_w, ov::element::f16);
        auto gather_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 0);
        auto new_g_w = std::make_shared<ov::op::v8::Gather>(new_cvt_w, matched_out_ids, gather_c);
        auto new_g_s = std::make_shared<ov::op::v8::Gather>(matched_out_s, matched_out_ids, gather_c);
        auto new_mul = std::make_shared<ov::op::v1::Multiply>(new_g_w, new_g_s);
        auto new_out = std::make_shared<ov::op::v0::Convert>(new_mul, ov::element::f32);

        // Reconnect old gather readers to the new Multiply
        for (auto&& r : matched_out_gather.get_target_inputs()) {
            r.replace_source_output(new_out);
        }
        return true;  // root was changed
    };
    register_matcher(std::make_shared<opp::Matcher>(gather, "DQGatherSymCW"), std::move(callback));
}

// Identify a Gather+DQ Sym GQ MatMul pattern, lift Gather up
// Note(1): this pattern is applied on the full model before any partitioning
// Note(2): here's a difference, the new lifted Gathers stay behind Convert(W) & Convert(S)
DQLiftGatherSymGQ::DQLiftGatherSymGQ() {
    auto qweight = opp::wrap_type<ov::op::v0::Constant>();
    auto qcoeff = opp::wrap_type<ov::op::v0::Constant>();
    auto qcvtw = opp::wrap_type<ov::op::v0::Convert>({qweight});
    auto qmuls = opp::wrap_type<ov::op::v1::Multiply>({qcvtw, qcoeff});
    auto qreshp = opp::wrap_type<ov::op::v1::Reshape>({qmuls, opp::any_input()});
    auto qcvtm = opp::wrap_type<ov::op::v0::Convert>({qreshp});

    auto pids = opp::wrap_type<ov::op::v0::Parameter>();
    auto cvtids = opp::wrap_type<ov::op::v0::Convert>({pids});
    auto gather = opp::wrap_type<ov::op::v8::Gather>({qcvtm, cvtids, opp::any_input()});

    // Note: Use [=] to make sure the above objects stay alive in the callback
    auto callback = [=](ov::pass::pattern::Matcher& m) {
        auto& node_to_output = m.get_pattern_value_map();

        // Create new gathers on W and S respectively
        auto matched_out_w = node_to_output.at(qweight);
        auto matched_out_s = node_to_output.at(qcoeff);
        auto matched_out_ids = node_to_output.at(cvtids);
        const auto& matched_out_gather = node_to_output.at(gather);

        auto matched_gather_shape = matched_out_gather.get_shape();

        // Replicate the compute part
        auto new_cvt_w = std::make_shared<ov::op::v0::Convert>(matched_out_w, ov::element::f16);

        auto gather_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 0);
        auto new_g_w = std::make_shared<ov::op::v8::Gather>(new_cvt_w, matched_out_ids, gather_c);
        auto new_g_s = std::make_shared<ov::op::v8::Gather>(matched_out_s, matched_out_ids, gather_c);

        auto new_mul = std::make_shared<ov::op::v1::Multiply>(new_g_w, new_g_s);

        auto new_rshp_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32,
                                                                 ov::Shape{matched_gather_shape.size()},
                                                                 matched_gather_shape);
        auto new_reshape = std::make_shared<ov::op::v1::Reshape>(new_mul, new_rshp_c, false);

        auto new_out = std::make_shared<ov::op::v0::Convert>(new_reshape, ov::element::f32);

        // Reconnect old gather readers to the new Multiply
        for (auto&& r : matched_out_gather.get_target_inputs()) {
            r.replace_source_output(new_out);
        }
        return true;  // root was changed
    };
    register_matcher(std::make_shared<opp::Matcher>(gather, "DQGatherSymGQ"), std::move(callback));
}

// This is a companion to DQLiftGatherAsymCW step. This pass runs if
// the respective block (mainly, a head) was turned a function
// (e.g. with FUNCALL_FOR_ALL) As in this case the DQDictMatMulCWu
// compile-time converts asymmetric MM to fp16, do the same thing here
DQUnpackDictGatherCWu::DQUnpackDictGatherCWu(Context::Ref ctx) {
    auto pids = opp::wrap_type<ov::op::v0::Parameter>();
    auto cvtids = opp::wrap_type<ov::op::v0::Convert>({pids});

    auto qweight = opp::wrap_type<ov::op::v0::Parameter>();
    auto qzerop = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcoeff = opp::wrap_type<ov::op::v0::Parameter>();
    auto qgthrw = opp::wrap_type<ov::op::v8::Gather>({qweight, cvtids, opp::any_input()});
    auto qgthrz = opp::wrap_type<ov::op::v8::Gather>({qzerop, cvtids, opp::any_input()});
    auto qgthrs = opp::wrap_type<ov::op::v8::Gather>({qcoeff, cvtids, opp::any_input()});

    auto qcvtw = opp::wrap_type<ov::op::v0::Convert>({qgthrw});
    auto qcvtz = opp::wrap_type<ov::op::v0::Convert>({qgthrz});
    auto qsubz = opp::wrap_type<ov::op::v1::Subtract>({qcvtw, qcvtz});
    auto qmuls = opp::wrap_type<ov::op::v1::Multiply>({qsubz, qgthrs});
    auto qcvtm = opp::wrap_type<ov::op::v0::Convert>({qmuls});

    // Note: Use [=] to make sure the above objects stay alive in the callback
    auto callback = [=](ov::pass::pattern::Matcher& m) {
        auto& node_to_output = m.get_pattern_value_map();

        auto matched_node_qweight = node_to_output.at(qweight).get_node_shared_ptr();
        auto matched_node_qzerop = node_to_output.at(qzerop).get_node_shared_ptr();
        auto matched_node_qcoeff = node_to_output.at(qcoeff).get_node_shared_ptr();
        auto matched_out_ids = node_to_output.at(cvtids);
        auto matched_node_cvt = node_to_output.at(qcvtm).get_node_shared_ptr();

        auto matched_qweight = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qweight);
        auto matched_qzerop = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qzerop);
        auto matched_qcoeff = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qcoeff);

        // Strip down the DQ subgraph, replace the original Q-ed closure tensor with unpacked fp16
        auto new_wi = ctx.get().unpack(matched_qweight, matched_qzerop, matched_qcoeff, ov::element::f16);
        auto gather_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 0);
        auto new_g = std::make_shared<ov::op::v8::Gather>(new_wi, matched_out_ids, gather_c);

        matched_node_cvt->input(0).replace_source_output(new_g);

        return true;  // root has changed
    };
    register_matcher(std::make_shared<opp::Matcher>(qcvtm, "DQDictGatherCWu"), std::move(callback));
}

// This is a follow-up to DQLiftGatherSymGQ step, which happens if the respective
// block (mainly, a head) was turned a function (e.g. with FUNCALL_FOR_ALL)
DQUnpackDictGatherGQi::DQUnpackDictGatherGQi(Context::Ref ctx) {
    auto pids = opp::wrap_type<ov::op::v0::Parameter>();
    auto cvtids = opp::wrap_type<ov::op::v0::Convert>({pids});

    auto qweight = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcoeff = opp::wrap_type<ov::op::v0::Parameter>();
    auto qgthrw = opp::wrap_type<ov::op::v8::Gather>({qweight, cvtids, opp::any_input()});
    auto qgthrs = opp::wrap_type<ov::op::v8::Gather>({qcoeff, cvtids, opp::any_input()});

    auto qcvtw = opp::wrap_type<ov::op::v0::Convert>({qgthrw});
    auto qmuls = opp::wrap_type<ov::op::v1::Multiply>({qcvtw, qgthrs});
    auto qrshp = opp::wrap_type<ov::op::v1::Reshape>({qmuls, opp::any_input()});
    auto qcvtm = opp::wrap_type<ov::op::v0::Convert>({qrshp});

    // Note: Use [=] to make sure the above objects stay alive in the callback
    auto callback = [=](ov::pass::pattern::Matcher& m) {
        auto& node_to_output = m.get_pattern_value_map();

        auto matched_node_qweight = node_to_output.at(qweight).get_node_shared_ptr();
        auto matched_node_qcoeff = node_to_output.at(qcoeff).get_node_shared_ptr();
        auto matched_out_ids = node_to_output.at(cvtids);
        auto matched_node_cvt = node_to_output.at(qcvtm).get_node_shared_ptr();

        auto matched_qweight = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qweight);
        auto matched_qcoeff = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qcoeff);

        // Strip down the DQ subgraph, replace the original Q-ed closure tensor with unpacked fp16
        auto new_wi = ctx.get().unpack(matched_qweight, matched_qcoeff, ov::element::f16);

        auto gather_c = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 0);
        auto new_g = std::make_shared<ov::op::v8::Gather>(new_wi, matched_out_ids, gather_c);
        matched_node_cvt->input(0).replace_source_output(new_g);

        return true;  // root has changed
    };
    register_matcher(std::make_shared<opp::Matcher>(qcvtm, "DQDictGatherCWu"), std::move(callback));
}

// Identify the case* where the FP16/32 vocab tensor is gathered with
// input_ids and the embedding size is high. In this case, substitute
// gather with a host-side op. Lower vocab tensor to f16.
// * - This case normally happens as a result of other
// * - DictGather-related transformations
HostGather::HostGather(Context::Ref ctx) {
    auto pids = opp::wrap_type<ov::op::v0::Parameter>();
    auto cvtids = opp::wrap_type<ov::op::v0::Convert>({pids});

    auto qweight = opp::wrap_type<ov::op::v0::Parameter>();
    auto qgthrw = opp::wrap_type<ov::op::v8::Gather>({qweight, cvtids, opp::any_input()});

    auto callback = [=](ov::pass::pattern::Matcher& m) {
        auto& node_to_output = m.get_pattern_value_map();
        auto out_shape = node_to_output.at(qgthrw).get_shape();
        auto& matched_out_qweight = node_to_output.at(qweight);
        auto qweight_type = matched_out_qweight.get_element_type();

        const auto& matched_out_gather = node_to_output.at(qgthrw);

        auto sole_reader = [](ov::Output<ov::Node> out) {
            const auto readers = out.get_target_inputs();
            NPUW_ASSERT(readers.size() >= 1);
            return readers.begin()->get_node();
        };

        if (out_shape.back() >= 2048 && (qweight_type == ov::element::f16 || qweight_type == ov::element::f32) &&
            (matched_out_gather.get_target_inputs().size() > 1 ||
             ov::is_type<ov::op::v0::Convert>(sole_reader(matched_out_gather)))) {
            auto matched_node_qweight = node_to_output.at(qweight).get_node_shared_ptr();
            auto matched_node_ids = node_to_output.at(pids).get_node_shared_ptr();
            const auto& matched_out_gthr = node_to_output.at(qgthrw);
            auto matched_qweight = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qweight);
            auto matched_ids = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_ids);

            if (qweight_type == ov::element::f32) {
                ctx.get().to_f16(matched_qweight);
            }
            auto new_param = ctx.get().host_gather(matched_qweight, matched_ids);
            std::shared_ptr<ov::Node> new_cvt;
            if (qweight_type == ov::element::f16) {
                new_cvt = new_param;
            } else {
                new_cvt = std::make_shared<ov::op::v0::Convert>(new_param, ov::element::f32);
            }
            NPUW_ASSERT(new_cvt);
            for (auto&& r : matched_out_gthr.get_target_inputs()) {
                r.replace_source_output(new_cvt);
            }
            return true;  // Root has changed
        }
        return false;  // Root hasn't changed (yet)
    };
    register_matcher(std::make_shared<opp::Matcher>(qgthrw, "HostGather"), std::move(callback));
}

// Identify the case* where the gather is applied on a compressed
// (symmetric) vocab tensor. Both CW and GQ paths are supported.
//
// FIXME: This may be inefficient: 4x-es the memory consumption
// due to i4-to-fp16 conversion.
HostGatherDQ::HostGatherDQ(Context::Ref ctx) {
    auto pids = opp::wrap_type<ov::op::v0::Parameter>();
    auto cvtids = opp::wrap_type<ov::op::v0::Convert>({pids});

    auto qweight = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcvtw = opp::wrap_type<ov::op::v0::Convert>({qweight});
    auto qcoeff = opp::wrap_type<ov::op::v0::Parameter>();

    auto qgthrw = opp::wrap_type<ov::op::v8::Gather>({qcvtw, cvtids, opp::any_input()});
    auto qgthrc = opp::wrap_type<ov::op::v8::Gather>({qcoeff, cvtids, opp::any_input()});
    auto qmul = opp::wrap_type<ov::op::v1::Multiply>({qgthrw, qgthrc});

    auto callback = [=](ov::pass::pattern::Matcher& m) {
        auto& node_to_output = m.get_pattern_value_map();
        const auto& matched_out_mul = node_to_output.at(qmul);
        auto out_shape = matched_out_mul.get_shape();

        if (out_shape.size() != 3 && out_shape.size() != 4) {
            return false;
        }

        // shape=3 == CW model, 1 x N x Hs
        // shape=4 == GQ model, 1 x G x(N/G) x Hs
        // were Hs = hidden size, G is # of groups, N is the prompt size.
        auto out_len = out_shape.size() == 3 ? out_shape[2] : out_shape[2] * out_shape[3];

        const auto& matched_out_qweight = node_to_output.at(qweight);
        auto qweight_type = matched_out_qweight.get_element_type();

        if (out_len >= 2048 && qweight_type == ov::element::i4) {
            auto matched_node_qweight = node_to_output.at(qweight).get_node_shared_ptr();
            auto matched_node_qcoeff = node_to_output.at(qcoeff).get_node_shared_ptr();
            auto matched_node_ids = node_to_output.at(pids).get_node_shared_ptr();

            auto matched_qweight = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qweight);
            auto matched_qcoeff = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qcoeff);
            auto matched_ids = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_ids);

            auto fp16vocab = ctx.get().unpack(matched_qweight, matched_qcoeff, ov::element::f16);
            auto new_param = ctx.get().host_gather(fp16vocab, matched_ids);
            for (auto&& r : matched_out_mul.get_target_inputs()) {
                r.replace_source_output(new_param);
            }
            return true;  // Root has changed
        }
        return false;  // Root hasn't changed (yet)
    };
    register_matcher(std::make_shared<opp::Matcher>(qmul, "HostGatherDQ"), std::move(callback));
}

// FROM:
//     Param(W) -> to(f16) ->
//     Param(Z) -> to(f16) -> Subtract
//     Param(S) ---------------------> Multiply -> to(f32) -> MatMul -> Result
//     ???(Act) -------------------------------------------->
//
// TO:
//     Param(W) ------------>
//     ???(Act) -> to(f16) -> MatMul -> to(f32) -> Result

DQUnpackDictMatMulCWu::DQUnpackDictMatMulCWu(Context::Ref ctx) {
    auto qweight = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcoeff = opp::wrap_type<ov::op::v0::Parameter>();
    auto qzerop = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcvtw = opp::wrap_type<ov::op::v0::Convert>({qweight});
    auto qcvtz = opp::wrap_type<ov::op::v0::Convert>({qzerop});
    auto qsub = opp::wrap_type<ov::op::v1::Subtract>({qcvtw, qcvtz});
    auto qmuls = opp::wrap_type<ov::op::v1::Multiply>({qsub, qcoeff});
    auto qcvtm = opp::wrap_type<ov::op::v0::Convert>({qmuls});
    auto qmmi = opp::any_input();
    auto qmm = opp::wrap_type<ov::op::v0::MatMul>({qmmi, qcvtm});
    auto qres = opp::wrap_type<ov::op::v0::Result>({qmm});

    // Note: Use [=] to make sure the above objects stay alive in the callback
    auto callback = [=](ov::pass::pattern::Matcher& m) {
        auto& node_to_output = m.get_pattern_value_map();

        auto matched_node_qweight = node_to_output.at(qweight).get_node_shared_ptr();
        auto matched_node_qzerop = node_to_output.at(qzerop).get_node_shared_ptr();
        auto matched_node_cvtw = node_to_output.at(qcvtw).get_node_shared_ptr();
        auto matched_node_cvtz = node_to_output.at(qcvtz).get_node_shared_ptr();
        auto matched_node_qcoeff = node_to_output.at(qcoeff).get_node_shared_ptr();
        auto matched_node_matmul = node_to_output.at(qmm).get_node_shared_ptr();
        auto matched_mmi = node_to_output.at(qmmi);
        auto matched_node_res = node_to_output.at(qres).get_node_shared_ptr();

        auto matched_qweight = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qweight);
        auto matched_qzerop = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qzerop);
        auto matched_qcoeff = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qcoeff);
        auto matched_matmul = std::static_pointer_cast<ov::op::v0::MatMul>(matched_node_matmul);
        auto matched_result = std::static_pointer_cast<ov::op::v0::Result>(matched_node_res);

        auto qcoeff_shape = matched_qcoeff->output(0).get_shape();

        if (ov::element::u8 == matched_qweight->get_element_type() && qcoeff_shape[1] == 1 &&
            !matched_matmul->get_transpose_a() && matched_matmul->get_transpose_b()) {
            auto new_cvt_a = std::make_shared<ov::op::v0::Convert>(matched_mmi, ov::element::f16);

            auto new_wi = ctx.get().unpack(matched_qweight, matched_qzerop, matched_qcoeff, ov::element::f16);
            auto new_mm = std::make_shared<ov::op::v0::MatMul>(new_cvt_a, new_wi, false, true);
            auto new_out = std::make_shared<ov::op::v0::Convert>(new_mm, ov::element::f32);

            matched_result->input(0).replace_source_output(new_out);
        }
        return false;  // root has changed (yet)
    };
    register_matcher(std::make_shared<opp::Matcher>(qres, "OptDQDictMatMulCWu"), std::move(callback));
}

// FROM:
//     Param(W) -> to(f16) ->
//     Param(S) ------------> Multiply -> Reshape -> to(f32) -> MatMul -> Result
//     ???(Act) ---------------------------------------------->
//
// TO:
//     Param(W) ------------>
//     ???(Act) -> to(f16) -> MatMul -> to(f32) -> Result
// NB: This pass only worsens the performance so is disabled
DQUnpackDictMatMulGQi::DQUnpackDictMatMulGQi(Context::Ref ctx) {
    auto qweight = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcoeff = opp::wrap_type<ov::op::v0::Parameter>();
    auto qcvtw = opp::wrap_type<ov::op::v0::Convert>({qweight});
    auto qmuls = opp::wrap_type<ov::op::v1::Multiply>({qcvtw, qcoeff});
    auto qreshp = opp::wrap_type<ov::op::v1::Reshape>({qmuls, opp::any_input()});
    auto qcvtm = opp::wrap_type<ov::op::v0::Convert>({qreshp});
    auto qcvtr = opp::wrap_type<ov::op::v0::Convert>({qreshp});
    auto qmmi = opp::any_input();
    auto qmm = opp::wrap_type<ov::op::v0::MatMul>({qmmi, qcvtr});
    auto qres = opp::wrap_type<ov::op::v0::Result>({qmm});

    // Note: Use [=] to make sure the above objects stay alive in the callback
    auto callback = [=](ov::pass::pattern::Matcher& m) {
        auto& node_to_output = m.get_pattern_value_map();

        auto matched_node_qweight = node_to_output.at(qweight).get_node_shared_ptr();
        auto matched_node_cvtw = node_to_output.at(qcvtw).get_node_shared_ptr();
        auto matched_node_qcoeff = node_to_output.at(qcoeff).get_node_shared_ptr();
        auto matched_node_matmul = node_to_output.at(qmm).get_node_shared_ptr();
        auto matched_mmi = node_to_output.at(qmmi);
        auto matched_node_res = node_to_output.at(qres).get_node_shared_ptr();

        auto matched_qweight = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qweight);
        auto matched_qcoeff = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_qcoeff);
        auto matched_matmul = std::static_pointer_cast<ov::op::v0::MatMul>(matched_node_matmul);
        auto matched_result = std::static_pointer_cast<ov::op::v0::Result>(matched_node_res);

        const auto& qcoeff_shape = matched_qcoeff->output(0).get_shape();

        if (ov::element::i4 == matched_qweight->get_element_type() && qcoeff_shape.size() == 3) {
            auto new_cvt_a = std::make_shared<ov::op::v0::Convert>(matched_mmi, ov::element::f16);

            auto new_wi = ctx.get().unpack(matched_qweight, matched_qcoeff, ov::element::f16);
            auto new_mm = std::make_shared<ov::op::v0::MatMul>(new_cvt_a,
                                                               new_wi,
                                                               matched_matmul->get_transpose_a(),
                                                               matched_matmul->get_transpose_b());
            auto new_out = std::make_shared<ov::op::v0::Convert>(new_mm, ov::element::f32);

            matched_result->input(0).replace_source_output(new_out);
        }
        return false;  // root has changed (yet)
    };
    register_matcher(std::make_shared<opp::Matcher>(qres, "OptDQDictMatMulGQi"), std::move(callback));
}

// FROM:
//     Param(W):f32 ->
//     ???(Act) -----> MatMul -> Result
//
// TO:
//     Param(W):f16 -------->
//     ???(Act) -> to(f16) -> MatMul -> to(f32) -> Result
// NB: This pass only worsens the performance so is disabled
CompressDictMatMulf32::CompressDictMatMulf32(Context::Ref ctx) {
    auto weight = opp::wrap_type<ov::op::v0::Parameter>();
    auto mmi = opp::any_input();
    auto mm = opp::wrap_type<ov::op::v0::MatMul>({mmi, weight});
    auto res = opp::wrap_type<ov::op::v0::Result>({mm});

    // Note: Use [=] to make sure the above objects stay alive in the callback
    auto callback = [=](ov::pass::pattern::Matcher& m) {
        auto& node_to_output = m.get_pattern_value_map();

        auto matched_node_weight = node_to_output.at(weight).get_node_shared_ptr();
        auto matched_node_matmul = node_to_output.at(mm).get_node_shared_ptr();
        auto matched_mmi = node_to_output.at(mmi);
        auto matched_node_res = node_to_output.at(res).get_node_shared_ptr();

        auto matched_weight = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_weight);
        auto matched_matmul = std::static_pointer_cast<ov::op::v0::MatMul>(matched_node_matmul);
        auto matched_result = std::static_pointer_cast<ov::op::v0::Result>(matched_node_res);

        if (ov::element::f32 == matched_weight->get_element_type()) {
            auto new_cvt_a = std::make_shared<ov::op::v0::Convert>(matched_mmi, ov::element::f16);

            ctx.get().to_f16(matched_weight);
            auto new_mm = std::make_shared<ov::op::v0::MatMul>(new_cvt_a,
                                                               matched_weight,
                                                               matched_matmul->get_transpose_a(),
                                                               matched_matmul->get_transpose_b());
            auto new_out = std::make_shared<ov::op::v0::Convert>(new_mm, ov::element::f32);

            matched_result->input(0).replace_source_output(new_out);
        }
        return false;  // root has changed (yet)
    };
    register_matcher(std::make_shared<opp::Matcher>(res, "OptCompressDictMatMulf32"), std::move(callback));
}

}  // namespace opt
}  // namespace patterns
}  // namespace npuw
}  // namespace ov
