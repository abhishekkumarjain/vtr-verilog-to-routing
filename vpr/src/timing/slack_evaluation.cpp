#include "slack_evaluation.h"

#include "tatum/TimingGraph.hpp"
#include "timing_util.h"
#include "vpr_error.h"
#include "atom_netlist.h"
#include "vtr_log.h"

#if defined(VPR_USE_TBB)
#    include <tbb/task_group.h>
#    include <tbb/parallel_for_each.h>
#endif

template<typename T>
void nodes_to_pins(T nodes, const AtomLookup& atom_lookup, std::vector<AtomPinId>& pins) {
    pins.reserve(nodes.size());
    for (tatum::NodeId node : nodes) {
        AtomPinId pin = atom_lookup.tnode_atom_pin(node);
        pins.push_back(pin);
    }
}

/*
 * SetupSlackCrit
 */

SetupSlackCrit::SetupSlackCrit(const AtomNetlist& netlist, const AtomLookup& netlist_lookup)
    : netlist_(netlist)
    , netlist_lookup_(netlist_lookup)
    , pin_slacks_(netlist_.pins().size(), NAN)
    , pin_criticalities_(netlist_.pins().size(), NAN) {
    //pass
}

//Returns the worst (least) slack of connections through the specified pin
float SetupSlackCrit::setup_pin_slack(AtomPinId pin) const { return pin_slacks_[pin]; }

//Returns the worst (maximum) criticality of connections through the specified pin.
//  Criticality (in [0., 1.]) represents how timing-critical something is,
//  0. is non-critical and 1. is most-critical.
float SetupSlackCrit::setup_pin_criticality(AtomPinId pin) const { return pin_criticalities_[pin]; }

SetupSlackCrit::modified_pin_range SetupSlackCrit::pins_with_modified_slack() const {
    return vtr::make_range(pins_with_modified_slacks_);
}

SetupSlackCrit::modified_pin_range SetupSlackCrit::pins_with_modified_criticality() const {
    return vtr::make_range(pins_with_modified_criticalities_);
}

void SetupSlackCrit::update_slacks_and_criticalities(const tatum::TimingGraph& timing_graph, const tatum::SetupTimingAnalyzer& analyzer) {
#if defined(VPR_USE_TBB)
    tbb::task_group g;
    g.run([&] { update_slacks(analyzer); });
    g.run([&] { update_criticalities(timing_graph, analyzer); });
    g.wait();
#else
    update_slacks(analyzer);
    update_criticalities(timing_graph, analyzer);
#endif
}

void SetupSlackCrit::update_slacks(const tatum::SetupTimingAnalyzer& analyzer) {
    //Note that this is done lazily only on the nodes modified by the analyzer
    auto nodes = analyzer.modified_nodes();

#if defined(VPR_USE_TBB)
    tbb::parallel_for_each(nodes.begin(), nodes.end(), [&, this](tatum::NodeId node) {
        this->update_pin_slack(node, analyzer);
    });
#else
    for (tatum::NodeId node : nodes) {
        update_pin_slack(node, analyzer);
    }
#endif

    //Record pins with modified slacks
    pins_with_modified_slacks_.clear();
    nodes_to_pins(nodes, netlist_lookup_, pins_with_modified_slacks_);
}

void SetupSlackCrit::update_pin_slack(const tatum::NodeId node, const tatum::SetupTimingAnalyzer& analyzer) {
    //Find the timing node associated with the pin
    AtomPinId pin = netlist_lookup_.tnode_atom_pin(node);
    VTR_ASSERT_SAFE(pin);

    //Find the worst (least) slack at this node
    auto tags = analyzer.setup_slacks(node);
    auto min_tag_iter = find_minimum_tag(tags);
    if (min_tag_iter != tags.end()) {
        pin_slacks_[pin] = min_tag_iter->time().value();
    } else {
        //No tags (e.g. driven by constant generator)
        pin_slacks_[pin] = std::numeric_limits<float>::infinity();
    }
}

void SetupSlackCrit::update_criticalities(const tatum::TimingGraph& timing_graph, const tatum::SetupTimingAnalyzer& analyzer) {
    //Record the maximum required time, and wost slack per domain pair
    std::map<DomainPair, float> max_req;
    std::map<DomainPair, float> worst_slack;
    for (tatum::NodeId node : timing_graph.logical_outputs()) {
        for (auto& tag : analyzer.setup_tags(node, tatum::TagType::DATA_REQUIRED)) {
            auto domain_pair = DomainPair(tag.launch_clock_domain(), tag.capture_clock_domain());

            float req = tag.time().value();
            if (!max_req.count(domain_pair) || max_req[domain_pair] < req) {
                max_req[domain_pair] = req;
            }
        }

        for (auto& tag : analyzer.setup_slacks(node)) {
            auto domain_pair = DomainPair(tag.launch_clock_domain(), tag.capture_clock_domain());

            float slack = tag.time().value();

            VTR_ASSERT_SAFE_MSG(!std::isnan(slack), "Slack should not be nan");
            VTR_ASSERT_SAFE_MSG(std::isfinite(slack), "Slack should not be infinite");

            if (!worst_slack.count(domain_pair) || slack < worst_slack[domain_pair]) {
                worst_slack[domain_pair] = slack;
            }
        }
    }

    if (max_req == prev_max_req_ && worst_slack == prev_worst_slack_) {
        //Max required times and worst slacks unchanged, incrementally update
        //the criticalities of each pin
        //
        // Note that this is done lazily only on the nodes modified by the analyzer
        auto nodes = analyzer.modified_nodes();

#if defined(VPR_USE_TBB)
        tbb::parallel_for_each(nodes.begin(), nodes.end(), [&, this](tatum::NodeId node) {
            AtomPinId pin = netlist_lookup_.tnode_atom_pin(node);
            VTR_ASSERT_SAFE(pin);
            this->pin_criticalities_[pin] = this->calc_pin_criticality(node, analyzer, max_req, worst_slack);
        });
#else
        for (tatum::NodeId node : nodes) {
            AtomPinId pin = netlist_lookup_.tnode_atom_pin(node);
            VTR_ASSERT_SAFE(pin);
            pin_criticalities_[pin] = calc_pin_criticality(node, analyzer, max_req, worst_slack);
        }
#endif

        //Record pins with modified criticalities
        pins_with_modified_criticalities_.clear();
        nodes_to_pins(nodes, netlist_lookup_, pins_with_modified_criticalities_);
    } else {
        //Max required and/or worst slacks changed, fully recalculate criticalities
        //
        //  TODO: consider if incremental criticality update is feasible based only 
        //        on changed domain pairs....
        auto nodes = timing_graph.nodes();

#if defined(VPR_USE_TBB)
        tbb::parallel_for_each(nodes.begin(), nodes.end(), [&, this](tatum::NodeId node) {
            AtomPinId pin = netlist_lookup_.tnode_atom_pin(node);
            VTR_ASSERT_SAFE(pin);
            this->pin_criticalities_[pin] = this->calc_pin_criticality(node, analyzer, max_req, worst_slack);
        });
#else
        for (tatum::NodeId node : nodes) {
            AtomPinId pin = netlist_lookup_.tnode_atom_pin(node);
            VTR_ASSERT_SAFE(pin);
            pin_criticalities_[pin] = calc_pin_criticality(node, analyzer, max_req, worst_slack);
        }
#endif

        //Record pins with modified criticalities
        pins_with_modified_criticalities_.clear();
        nodes_to_pins(nodes, netlist_lookup_, pins_with_modified_criticalities_);
    }
    prev_max_req_ = max_req;
    prev_worst_slack_ = worst_slack;
}

float SetupSlackCrit::calc_pin_criticality(const tatum::NodeId node,
                                           const tatum::SetupTimingAnalyzer& analyzer,
                                           const std::map<DomainPair, float>& max_req,
                                           const std::map<DomainPair, float>& worst_slack) {
    //Calculate maximum criticality over all domains
    return calc_relaxed_criticality(max_req, worst_slack, analyzer.setup_slacks(node));
}
/*
 * HoldSlackCrit
 */

HoldSlackCrit::HoldSlackCrit(const AtomNetlist& netlist, const AtomLookup& netlist_lookup)
    : netlist_(netlist)
    , netlist_lookup_(netlist_lookup)
    , pin_slacks_(netlist_.pins().size(), NAN)
    , pin_criticalities_(netlist_.pins().size(), NAN) {
    //pass
}

//Returns the worst (least) slack of connections through the specified pin
float HoldSlackCrit::hold_pin_slack(AtomPinId pin) const { return pin_slacks_[pin]; }

//Returns the worst (maximum) criticality of connections through the specified pin.
//  Criticality (in [0., 1.]) represents how timing-critical something is,
//  0. is non-critical and 1. is most-critical.
float HoldSlackCrit::hold_pin_criticality(AtomPinId pin) const { return pin_criticalities_[pin]; }

void HoldSlackCrit::update_slacks_and_criticalities(const tatum::TimingGraph& timing_graph, const tatum::HoldTimingAnalyzer& analyzer) {
#if defined(VPR_USE_TBB)
    tbb::task_group g;
    g.run([&] { update_slacks(analyzer); });
    g.run([&] { update_criticalities(timing_graph, analyzer); });
    g.wait();
#else
    update_slacks(analyzer);
    update_criticalities(timing_graph, analyzer);
#endif
}

void HoldSlackCrit::update_slacks(const tatum::HoldTimingAnalyzer& analyzer) {
    for (AtomPinId pin : netlist_.pins()) {
        update_pin_slack(pin, analyzer);
    }
}

void HoldSlackCrit::update_pin_slack(const AtomPinId pin, const tatum::HoldTimingAnalyzer& analyzer) {
    //Find the timing node associated with the pin
    tatum::NodeId node = netlist_lookup_.atom_pin_tnode(pin);
    VTR_ASSERT(node);

    //Find the worst (least) slack at this node
    auto tags = analyzer.hold_slacks(node);
    auto min_tag_iter = find_minimum_tag(tags);
    if (min_tag_iter != tags.end()) {
        pin_slacks_[pin] = min_tag_iter->time().value();
    } else {
        //No tags (e.g. driven by constant generator)
        pin_slacks_[pin] = std::numeric_limits<float>::infinity();
    }
}

void HoldSlackCrit::update_criticalities(const tatum::TimingGraph& timing_graph, const tatum::HoldTimingAnalyzer& analyzer) {
    //TODO: this calculates a simple shifted and scaled criticality, not clear if this is the
    //right approach (e.g. should we use a more intellegent method like the one used by setup slack?)
    float worst_slack = std::numeric_limits<float>::infinity();
    float best_slack = -std::numeric_limits<float>::infinity();
    for (tatum::NodeId node : timing_graph.nodes()) {
        for (auto& tag : analyzer.hold_slacks(node)) {
            float slack = tag.time().value();
            worst_slack = std::min(worst_slack, slack);
            best_slack = std::max(best_slack, slack);
        }
    }

    //Calculate the transformation from slack to criticality,
    //we scale and shift the result so the worst_slack takes on criticalty 1.0
    //while the best slack takes on criticality 0.0
    float scale = 1. / std::abs(best_slack - worst_slack);
    float shift = -worst_slack;

    //Update the criticalities of each pin
    auto pins = netlist_.pins();
#if defined(VPR_USE_TBB)
    tbb::parallel_for_each(pins.begin(), pins.end(), [&, this](auto pin) {
        this->pin_criticalities_[pin] = this->calc_pin_criticality(pin, analyzer, scale, shift);
    });
#else
    for (auto pin : pins) {
        pin_criticalities_[pin] = calc_pin_criticality(pin, analyzer, scale, shift);
    }
#endif
}

float HoldSlackCrit::calc_pin_criticality(AtomPinId pin,
                                          const tatum::HoldTimingAnalyzer& analyzer,
                                          const float scale,
                                          const float shift) {
    tatum::NodeId node = netlist_lookup_.atom_pin_tnode(pin);
    VTR_ASSERT(node);

    float criticality = 0.;

    for (auto tag : analyzer.hold_slacks(node)) {
        float slack = tag.time().value();

        float tag_criticality = 1. - scale * (slack + shift);

        criticality = std::max(criticality, tag_criticality);
    }

    VTR_ASSERT(criticality >= 0.);
    VTR_ASSERT(criticality <= 1.);

    return criticality;
}
