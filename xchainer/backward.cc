#include "xchainer/backward.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gsl/gsl>
#include <nonstd/optional.hpp>

#include "xchainer/array.h"
#include "xchainer/array_body.h"
#include "xchainer/array_node.h"
#include "xchainer/backprop_mode.h"
#include "xchainer/backward_context.h"
#include "xchainer/backward_fwd.h"
#include "xchainer/context.h"
#include "xchainer/device.h"
#include "xchainer/dtype.h"
#include "xchainer/error.h"
#include "xchainer/graph.h"
#include "xchainer/op_node.h"
#include "xchainer/routines/creation.h"
#include "xchainer/shape.h"

namespace xchainer {
namespace {

using internal::ArrayBody;
using internal::ArrayNode;
using internal::OpNode;

}  // namespace

namespace internal {
namespace {

void CheckGradCompatible(const Array& grad, const Shape& shape, Dtype dtype, Device& device) {
    CheckEqual(dtype, grad.dtype());
    CheckEqual(shape, grad.shape());
    CheckEqual(device, grad.device());
}

}  // namespace

void AccumulateGrad(nonstd::optional<Array>& target_grad, Array partial_grad, const Shape& shape, Dtype dtype, Device& device) {
    CheckGradCompatible(partial_grad, shape, dtype, device);
    if (target_grad.has_value()) {
        target_grad = *target_grad + partial_grad;
    } else {
        target_grad = std::move(partial_grad);
    }
}

void SetGrad(nonstd::optional<Array>& target_grad, Array grad, const Shape& shape, Dtype dtype, Device& device) {
    CheckGradCompatible(grad, shape, dtype, device);
    target_grad = std::move(grad);
}

}  // namespace internal

namespace {

struct OpNodeComparator {
    bool operator()(const std::shared_ptr<OpNode>& lhs, const std::shared_ptr<OpNode>& rhs) const { return lhs->rank() < rhs->rank(); }
};

class BackwardImpl {
public:
    BackwardImpl(const std::vector<ConstArrayRef>& outputs, const GraphId& graph_id, DoubleBackpropOption double_backprop)
        : outputs_{outputs}, graph_id_{graph_id}, double_backprop_{double_backprop} {
        for (const Array& output : outputs) {
            if (!output.IsGradRequired(graph_id)) {
                throw XchainerError{"Cannot start backprop from an array whose gradient is not required (on graph '", graph_id, "')"};
            }
            output_array_nodes_.emplace_back(internal::GetArrayBody(output)->GetArrayNode(graph_id));
        }

        // Check if backward is possible for the given graph, in this context.
        // It is not possible if a graph from an outer scope has already been backpropped.
        graph_id.context().CheckBackpropAllowed(graph_id);

        // Graphs for which gradients will be stopped.
        // These include the current graph that is being backpropped depending on the double backprop option, as well as all graphs
        // belonging to inner scopes, i.e. graphs with higher graph sub ids.
        graph_ids_to_stop_gradient_ = graph_id.context().GetInnerGraphIds(graph_id_);
        if (double_backprop_ == DoubleBackpropOption::kDisable) {
            graph_ids_to_stop_gradient_.emplace_back(graph_id_);
        }
    }

    void Run() {
        Context& context = graph_id_.context();

        // Push initial output array nodes
        for (size_t i = 0; i < outputs_.size(); ++i) {
            const Array& output = outputs_[i];
            const std::shared_ptr<ArrayNode>& array_node = output_array_nodes_[i];

            // Add GradRef for output array nodes
            auto emplace_result = array_node_grad_map_.emplace(array_node.get(), internal::GradRef{*array_node});

            // Set unset output gradients to the default value of one
            if (!emplace_result.first->second.get().has_value()) {
                emplace_result.first->second.get() = OnesLike(output, output.device());
            }

            PushInputOpNode(array_node);
        }

        // Backpropagation
        while (!candidate_op_nodes_.empty()) {
            std::pop_heap(candidate_op_nodes_.begin(), candidate_op_nodes_.end(), OpNodeComparator{});
            std::shared_ptr<OpNode> op_node = std::move(candidate_op_nodes_.back());
            candidate_op_nodes_.pop_back();

            // Add GradRef for input array nodes
            for (const std::shared_ptr<ArrayNode>& input_array_node : op_node->input_array_nodes()) {
                if (input_array_node != nullptr) {
                    array_node_grad_map_.emplace(input_array_node.get(), internal::GradRef{*input_array_node});
                }
            }

            // Backpropagate gradients from the output array nodes into the input array nodes.
            {
                std::vector<nonstd::optional<Array>> gxs = ComputeInputGradients(op_node);
                AccumulateInputGradients(*op_node, std::move(gxs));
            }

            // Push the input op nodes into the queue
            for (const auto& input_array_node : op_node->input_array_nodes()) {
                if (input_array_node != nullptr) {
                    PushInputOpNode(input_array_node);
                }
            }

            if (double_backprop_ == DoubleBackpropOption::kDisable) {
                op_node->Unchain();
            }

            // Erase the array node's temporarily held grad
            {
                auto range = output_array_node_keeper_.equal_range(op_node.get());
                for (auto it = range.first; it != range.second; ++it) {
                    size_t n_removed = array_node_grad_map_.erase(it->second.get());
                    (void)n_removed;  // unused
                    assert(n_removed > 0);
                }
            }
        }

        // Register this graph as backpropped.
        context.SetBackpropDone(graph_id_);
    }

private:
    // Runs backward functions to compute gradients of input array nodes.
    std::vector<nonstd::optional<Array>> ComputeInputGradients(const std::shared_ptr<OpNode>& op_node) {
        // A single op node has multiple backward functions, each of which computes the gradients of a subset of the inputs.
        // They are responsible for non-overlapping subsets of inputs.
        // This function calls these backward functions, collects the gradients computed by them and returns the collected gradients.
        assert(op_node != nullptr);

        // Output array nodes. May be nullptr if the node is gone.
        std::vector<std::shared_ptr<ArrayNode>> output_array_nodes;

        // `temp_output_grads` is a set of temporary GradRefs of this op node's output array nodes.
        // This is used for output array nodes which are either dead at the moment or alive but have not been involved in the preceding
        // backpropagation.
        // This vector is just a keeper and not used in any other way. output_grads holds the pointer to it.
        // These GradRefs are only valid in the backward functions of this op node.
        // Be careful not to cause reallocation in this vector. Otherwise the pointers would be invalidated.
        std::vector<internal::GradRef> temp_output_grads;
        temp_output_grads.reserve(op_node->output_array_nodes().size());

        std::vector<internal::GradRef*> output_grads;
        for (const std::weak_ptr<ArrayNode>& maybe_output_array_node : op_node->output_array_nodes()) {
            std::shared_ptr<ArrayNode> output_array_node = maybe_output_array_node.lock();

            // Get the pointer to the output gradient.
            if (output_array_node != nullptr) {
                // Output array node is alive.
                auto it = array_node_grad_map_.find(output_array_node.get());
                if (it != array_node_grad_map_.end()) {
                    // The grad mapping has the gradient for the array node.
                    // Keep a pointer to the gradient in the map.
                    output_grads.emplace_back(&it->second);
                } else {
                    // The grad mapping has no entry for the array node.
                    // Create a new entry in temporary gradients and keep a pointer to it.
                    temp_output_grads.emplace_back(*output_array_node);
                    output_grads.emplace_back(&temp_output_grads.back());
                }
            } else {
                // Output array node is dead.
                // Keep a pointer to the temporary gradient vector.
                temp_output_grads.emplace_back(nonstd::nullopt);
                output_grads.emplace_back(&temp_output_grads.back());
            }

            output_array_nodes.emplace_back(std::move(output_array_node));
        }

        // Call the backward functions and collects their gradients.
        std::vector<nonstd::optional<Array>> input_grads;
        input_grads.resize(op_node->input_array_node_count());

        for (const internal::OpNodeBackwardEntry& backward_entry : op_node->backward_entries()) {
            // Compute and set gradients at the appropriate indices.
            CallBackwardForSubsetOfInputGradients(op_node, backward_entry, output_array_nodes, input_grads, output_grads);
        }

        // Make a view if the input gradient whose array body is identical to one of other output or input gradients.
        // Otherwise modifying operations such as requiring grad on one gradient would be transferred to other gradients.
        // TODO(niboshi): View is needed to make new nodes. Come up with a solution to avoid extra backward insertion.
        for (auto it = input_grads.begin(); it != input_grads.end(); ++it) {
            if (it->has_value() &&
                IsGradientIdenticalToAnyOfOtherGradients(**it, output_array_nodes, gsl::make_span(&*input_grads.begin(), &*it))) {
                **it = (*it)->MakeView();
            }
        }

        // If output array nodes are not output nodes of backward, clear their gradients
        for (const std::shared_ptr<ArrayNode>& output_array_node : output_array_nodes) {
            if (output_array_node == nullptr) {
                continue;
            }
            if (std::find_if(
                        output_array_nodes_.begin(),
                        output_array_nodes_.end(),
                        [output_array_node](const std::shared_ptr<ArrayNode>& out_node) { return output_array_node == out_node; }) ==
                output_array_nodes_.end()) {
                if (output_array_node != nullptr) {
                    std::shared_ptr<ArrayBody> body = output_array_node->weak_body().lock();
                    if (body != nullptr) {
                        body->ClearGrad(output_array_node->graph_id());
                    }
                }
            }
        }

        // Erase processed OpNode from the map
        output_array_node_keeper_.erase(op_node.get());

        return input_grads;
    }

    // Calls a single backward function that computes a subset of the gradients and returns the result.
    void CallBackwardForSubsetOfInputGradients(
            const std::shared_ptr<internal::OpNode>& op_node,
            const internal::OpNodeBackwardEntry& backward_entry,
            std::vector<std::shared_ptr<ArrayNode>>& output_array_nodes,
            std::vector<nonstd::optional<Array>>& input_grads,
            std::vector<internal::GradRef*>& output_grads) {
        // `computed_input_grads` holds the storage of gradients of all the inputs of the op node.
        // The given backward entry will compute and store a subset of those gradients.
        // The backward entry may compute and store the gradients of other inputs as well, which will be ignored.

        std::vector<Array> computed_input_grads(input_grads.size());

        // Call backward.
        BackwardContext bctx{op_node, backward_entry, output_array_nodes, output_grads, computed_input_grads, graph_id_, double_backprop_};
        {
            NoBackpropModeScope scope{graph_ids_to_stop_gradient_};
            backward_entry.backward_func()(bctx);
        }

        for (size_t i_input_grad : backward_entry.input_array_node_indices()) {
            if (!op_node->HasInputArrayNode(i_input_grad)) {
                // Input grad is not required
                continue;
            }

            Array& computed_input_grad = gsl::at(computed_input_grads, i_input_grad);
            if (internal::GetArrayBody(computed_input_grad) == nullptr) {
                // Input grad is not set by backward function
                continue;
            }

            // Set grads at the appropriate index in the vector containing all the input grads of the op node.
            {
                const std::shared_ptr<ArrayNode>& input_array_node = gsl::at(op_node->input_array_nodes(), i_input_grad);
                assert(input_array_node != nullptr);
                nonstd::optional<Array>& input_grad = input_grads[i_input_grad];

                internal::SetGrad(
                        input_grad, computed_input_grad, input_array_node->shape(), input_array_node->dtype(), input_array_node->device());
            }
        }
    }

    // Returns whether the specified input gradient is identical to any of the other input gradients or output gradients.
    bool IsGradientIdenticalToAnyOfOtherGradients(
            const Array& input_grad,
            const std::vector<std::shared_ptr<ArrayNode>>& output_array_nodes,
            gsl::span<nonstd::optional<Array>> other_input_grads) {
        // TODO(niboshi): Check node identity instead of body identity.
        return std::any_of(
                       output_array_nodes.begin(),
                       output_array_nodes.end(),
                       [&input_grad, this](const std::shared_ptr<ArrayNode>& output_array_node) {
                           if (output_array_node == nullptr) {
                               return false;
                           }
                           std::shared_ptr<ArrayBody> body = output_array_node->weak_body().lock();
                           if (body == nullptr) {
                               return false;
                           }
                           const nonstd::optional<Array>* output_grad = body->GetGrad(graph_id_);
                           return output_grad != nullptr && output_grad->has_value() &&
                                  internal::GetArrayBody(input_grad) == internal::GetArrayBody(**output_grad);
                       }) ||
               std::any_of(
                       other_input_grads.begin(), other_input_grads.end(), [&input_grad](const nonstd::optional<Array>& other_input_grad) {
                           return other_input_grad.has_value() &&
                                  internal::GetArrayBody(*other_input_grad) == internal::GetArrayBody(input_grad);
                       });
    }

    void AccumulateInputGradients(const OpNode& op_node, std::vector<nonstd::optional<Array>> gxs) {
        gsl::span<const std::shared_ptr<ArrayNode>> input_array_nodes = op_node.input_array_nodes();
        assert(input_array_nodes.size() == gxs.size());
        for (size_t i = 0; i < input_array_nodes.size(); ++i) {
            nonstd::optional<Array>& gx = gxs[i];
            if (gx.has_value()) {
                assert(input_array_nodes[i] != nullptr);
                const ArrayNode& input_array_node = *input_array_nodes[i];
                // Retrieve the pointer to the input gradient.
                internal::GradRef& input_grad = array_node_grad_map_.at(input_array_nodes[i].get());
                internal::AccumulateGrad(
                        input_grad.get(), std::move(*gx), input_array_node.shape(), input_array_node.dtype(), input_array_node.device());
            }
        }
    }

    void PushInputOpNode(const std::shared_ptr<ArrayNode>& array_node) {
        // When double backprop is enabled, array_node releases the pointer to the input node here. After this operation, array_node will
        // look like a leaf node of the graph. Note that this move does not invalidates the array_node object itself; it is guaranteed
        // by the standard that shared_ptr becomes null after move-assigned to another.
        std::shared_ptr<OpNode> creator_op_node =
                double_backprop_ == DoubleBackpropOption::kEnable ? array_node->creator_op_node() : array_node->move_creator_op_node();

        if (creator_op_node) {
            auto range = output_array_node_keeper_.equal_range(creator_op_node.get());
            if (std::none_of(range.first, range.second, [&array_node](const auto& pair) { return pair.second == array_node; })) {
                // First appearance of the combination of op node and input node.
                bool is_first_visit = range.first == range.second;
                output_array_node_keeper_.emplace(creator_op_node.get(), array_node);  // Iterators are invalidated here.
                if (is_first_visit) {
                    // First appearance of this op node. Push it to the queue.
                    candidate_op_nodes_.push_back(std::move(creator_op_node));
                    std::push_heap(candidate_op_nodes_.begin(), candidate_op_nodes_.end(), OpNodeComparator{});
                }
            }
        }
    }

    // Op nodes to be visited. This is a max heap ordered by the rank of each op node (see OpNodeComparator).
    std::vector<std::shared_ptr<OpNode>> candidate_op_nodes_;

    // This mapping is used to keep output array nodes alive (referenced from op nodes as weak pointers).
    std::unordered_multimap<const OpNode*, std::shared_ptr<ArrayNode>> output_array_node_keeper_;

    // Mapping from array nodes to the corresponding gradients. Gradients may be genuine gradients held by array bodies or temporary
    // gradients which are only valid during backward computation at most.
    std::unordered_map<ArrayNode*, internal::GradRef> array_node_grad_map_;

    // Arguments to Backward().
    // Be careful that references require the referred objects alive (it should be guaranteed by Backward()).
    const std::vector<ConstArrayRef>& outputs_;
    std::vector<std::reference_wrapper<const std::shared_ptr<ArrayNode>>> output_array_nodes_;
    const GraphId& graph_id_;
    DoubleBackpropOption double_backprop_;

    std::vector<GraphId> graph_ids_to_stop_gradient_;
};

}  // namespace

void Backward(const Array& output, const nonstd::optional<GraphId>& graph_id, DoubleBackpropOption double_backprop) {
    GraphId actual_graph_id = internal::GetArrayGraphId(output, graph_id);
    std::vector<ConstArrayRef> outputs{output};  // Do not inline it; we need to guarantee that the vector is alive until Run() finishes.
    BackwardImpl{outputs, actual_graph_id, double_backprop}.Run();
}

void Backward(const std::vector<ConstArrayRef>& outputs, const nonstd::optional<GraphId>& graph_id, DoubleBackpropOption double_backprop) {
    if (outputs.empty()) {
        return;
    }
    GraphId actual_graph_id = internal::GetArrayGraphId(outputs.front().get(), graph_id);
    BackwardImpl{outputs, actual_graph_id, double_backprop}.Run();
}

}  // namespace xchainer
