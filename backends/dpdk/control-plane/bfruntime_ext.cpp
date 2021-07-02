#include "bfruntime_ext.h"

namespace DPDK {

namespace BFRT {

struct BfRtSchemaGenerator::ActionSelector {
    std::string name;
    std::string get_mem_name;
    P4Id id;
    P4Id get_mem_id;
    int64_t max_group_size;
    int64_t num_groups;  // aka size of selector
    std::vector<P4Id> tableIds;
    Util::JsonArray* annotations;

    static boost::optional<ActionSelector>
    from(const p4configv1::P4Info& p4info, const p4configv1::ActionProfile& actionProfile) {
        const auto& pre = actionProfile.preamble();
        if (!actionProfile.with_selector())
            return boost::none;
        auto selectorId = makeBfRtId(pre.id(), ::dpdk::P4Ids::ACTION_SELECTOR);
        auto selectorGetMemId = makeBfRtId(pre.id(), ::dpdk::P4Ids::ACTION_SELECTOR_GET_MEMBER);
        auto tableIds = collectTableIds(
            p4info, actionProfile.table_ids().begin(), actionProfile.table_ids().end());
        return ActionSelector{pre.name(), pre.name() + "_get_member",
                 selectorId, selectorGetMemId, actionProfile.max_group_size(),
                 actionProfile.size(), tableIds, transformAnnotations(pre)};
    }

    static boost::optional<ActionSelector>
    fromDPDK(const p4configv1::P4Info& p4info, const p4configv1::ExternInstance& externInstance) {
        const auto& pre = externInstance.preamble();
        ::dpdk::ActionSelector actionSelector;
        if (!externInstance.info().UnpackTo(&actionSelector)) {
            ::error("Extern instance %1% does not pack an ActionSelector object", pre.name());
            return boost::none;
        }
        auto selectorId = makeBfRtId(pre.id(), ::dpdk::P4Ids::ACTION_SELECTOR);
        auto selectorGetMemId = makeBfRtId(pre.id(), ::dpdk::P4Ids::ACTION_SELECTOR_GET_MEMBER);
        auto tableIds = collectTableIds(
            p4info, actionSelector.table_ids().begin(), actionSelector.table_ids().end());
        return ActionSelector{pre.name(), pre.name() + "_get_member",
            selectorId, selectorGetMemId, actionSelector.max_group_size(),
            actionSelector.num_groups(), tableIds, transformAnnotations(pre)};
    };
};

void
BfRtSchemaGenerator::addMatchActionData(const p4configv1::Table& table,
        Util::JsonObject* tableJson, Util::JsonArray* dataJson,
        P4Id maxActionParamId) const {
    cstring tableType = tableJson->get("table_type")->to<Util::JsonValue>()->getString();
    if (tableType == "MatchAction_Direct") {
        tableJson->emplace(
            "action_specs", makeActionSpecs(table, &maxActionParamId));
    } else if (tableType == "MatchAction_Indirect") {
        auto* f = makeCommonDataField(
            BF_RT_DATA_ACTION_MEMBER_ID, "$ACTION_MEMBER_ID",
            makeTypeInt("uint32"), false /* repeated */);
        addSingleton(dataJson, f, true /* mandatory */, false /* read-only */);
    } else if (tableType == "MatchAction_Indirect_Selector") {
        // action member id and selector group id are mutually-exclusive, so
        // we use a "oneof" here.
        auto* choicesDataJson = new Util::JsonArray();
        choicesDataJson->append(makeCommonDataField(
            BF_RT_DATA_ACTION_MEMBER_ID, "$ACTION_MEMBER_ID",
            makeTypeInt("uint32"), false /* repeated */));
        choicesDataJson->append(makeCommonDataField(
            BF_RT_DATA_SELECTOR_GROUP_ID, "$SELECTOR_GROUP_ID",
            makeTypeInt("uint32"), false /* repeated */));
        addOneOf(dataJson, choicesDataJson, true /* mandatory */, false /* read-only */);
    } else {
        BUG("Invalid table type '%1%'", tableType);
    }
}

void
BfRtSchemaGenerator::addActionSelectorGetMemberCommon(Util::JsonArray* tablesJson,
                                        const ActionSelector& actionSelector) const {
        auto* tableJson = initTableJson(actionSelector.get_mem_name,
                actionSelector.get_mem_id, "SelectorGetMember", 1 /* size */,
                actionSelector.annotations);

        auto* keyJson = new Util::JsonArray();
        addKeyField(keyJson, BF_RT_DATA_SELECTOR_GROUP_ID, "$SELECTOR_GROUP_ID",
                            true /* mandatory */, "Exact", makeTypeInt("uint64"));
        addKeyField(keyJson, BF_RT_DATA_HASH_VALUE, "hash_value",
                            true /* mandatory */, "Exact", makeTypeInt("uint64"));
        tableJson->emplace("key", keyJson);

        auto* dataJson = new Util::JsonArray();
        {
            auto* f = makeCommonDataField(BF_RT_DATA_ACTION_MEMBER_ID, "$ACTION_MEMBER_ID",
            makeTypeInt("uint64"), false /* repeated */);
            addSingleton(dataJson, f, false /* mandatory */, false /* read-only */);
        }
        tableJson->emplace("data", dataJson);

        tableJson->emplace("supported_operations", new Util::JsonArray());
        tableJson->emplace("attributes", new Util::JsonArray());
        addToDependsOn(tableJson, actionSelector.id);

        tablesJson->append(tableJson);
}

void
BfRtSchemaGenerator::addActionSelectorCommon(Util::JsonArray* tablesJson,
                const ActionSelector& actionSelector) const {
    // TODO(antonin): formalize ID allocation for selector tables
    // repeat same annotations as for action table
    // the maximum number of groups is the table size for the selector table
    auto* tableJson = initTableJson(
        actionSelector.name, actionSelector.id, "Selector",
        actionSelector.num_groups, actionSelector.annotations);

    auto* keyJson = new Util::JsonArray();
    addKeyField(keyJson, BF_RT_DATA_SELECTOR_GROUP_ID, "$SELECTOR_GROUP_ID",
            true /* mandatory */, "Exact", makeTypeInt("uint32"));
    tableJson->emplace("key", keyJson);

    auto* dataJson = new Util::JsonArray();
    {
        auto* f = makeCommonDataField(
                BF_RT_DATA_ACTION_MEMBER_ID, "$ACTION_MEMBER_ID",
                makeTypeInt("uint32"), true /* repeated */);
        addSingleton(dataJson, f, false /* mandatory */, false /* read-only */);
    }
    {
        auto* f = makeCommonDataField(
                BF_RT_DATA_ACTION_MEMBER_STATUS, "$ACTION_MEMBER_STATUS",
                makeTypeBool(), true /* repeated */);
        addSingleton(dataJson, f, false /* mandatory */, false /* read-only */);
    }
    {
        auto* f = makeCommonDataField(
                BF_RT_DATA_MAX_GROUP_SIZE, "$MAX_GROUP_SIZE",
                makeTypeInt("uint32", actionSelector.max_group_size), false /* repeated */);
        addSingleton(dataJson, f, false /* mandatory */, false /* read-only */);
    }
    tableJson->emplace("data", dataJson);

    tableJson->emplace("supported_operations", new Util::JsonArray());
    tableJson->emplace("attributes", new Util::JsonArray());

    tablesJson->append(tableJson);
}

bool
BfRtSchemaGenerator::addActionProfIds(const p4configv1::Table& table,
        Util::JsonObject* tableJson) const {
    auto implementationId = table.implementation_id();
    auto actProfId = static_cast<P4Id>(0);
    auto actSelectorId = static_cast<P4Id>(0);
    if (implementationId > 0) {
        auto hasSelector = actProfHasSelector(implementationId);
        if (hasSelector == boost::none) {
            ::error("Invalid implementation id in p4info: %1%", implementationId);
            return false;
        }
        cstring tableType = *hasSelector ?
            "MatchAction_Indirect_Selector" : "MatchAction_Indirect";
        tableJson->erase("table_type");
        tableJson->emplace("table_type", tableType);
        actProfId = ActionProf::makeActProfId(implementationId);
        if (*hasSelector)
            actSelectorId = makeActSelectorId(implementationId);
    }

    if (actProfId > 0) addToDependsOn(tableJson, actProfId);
    if (actSelectorId > 0) addToDependsOn(tableJson, actSelectorId);
    return true;
}

void
BfRtSchemaGenerator::addActionProfs(Util::JsonArray* tablesJson) const {
    for (const auto& actionProf : p4info.action_profiles()) {
        auto actionProfInstance = ActionProf::from(p4info, actionProf);
        if (actionProfInstance == boost::none) continue;
        addActionProfCommon(tablesJson, *actionProfInstance);

        auto actionSelectorInstance = ActionSelector::from(p4info, actionProf);
        if (actionSelectorInstance == boost::none) continue;
        addActionSelectorCommon(tablesJson, *actionSelectorInstance);
    }
}

boost::optional<bool>
BfRtSchemaGenerator::actProfHasSelector(P4Id actProfId) const {
    if (isOfType(actProfId, p4configv1::P4Ids::ACTION_PROFILE)) {
        auto* actionProf = Standard::findActionProf(p4info, actProfId);
        if (actionProf == nullptr) return boost::none;
        return actionProf->with_selector();
    } else if (isOfType(actProfId, ::dpdk::P4Ids::ACTION_SELECTOR)) {
        return true;
    }
    return boost::none;
}

const Util::JsonObject*
BfRtSchemaGenerator::genSchema() const {
    auto* json = new Util::JsonObject();

    json->emplace("schema_version", cstring("1.0.0"));

    auto* tablesJson = new Util::JsonArray();
    json->emplace("tables", tablesJson);

    addMatchTables(tablesJson);
    addActionProfs(tablesJson);
    addCounters(tablesJson);
    addMeters(tablesJson);
    // TODO(antonin): handle "standard" (v1model / PSA) registers

    auto* learnFiltersJson = new Util::JsonArray();
    json->emplace("learn_filters", learnFiltersJson);
    addLearnFilters(learnFiltersJson);

    addDPDKExterns(tablesJson, learnFiltersJson);
    return json;
}

void
BfRtSchemaGenerator::addDPDKExterns(Util::JsonArray* tablesJson,
                                      Util::JsonArray* learnFiltersJson) const {
    for (const auto& externType : p4info.externs()) {
        auto externTypeId = static_cast<::dpdk::P4Ids::Prefix>(externType.extern_type_id());
        if (externTypeId == ::dpdk::P4Ids::ACTION_SELECTOR) {
            for (const auto& externInstance : externType.instances()) {
                auto actionSelector =
                    ActionSelector::fromDPDK(p4info, externInstance);
                if (actionSelector != boost::none) {
                    addActionSelectorCommon(tablesJson, *actionSelector);
                    addActionSelectorGetMemberCommon(tablesJson, *actionSelector);
                }
            }
        }
    }
}


static Util::JsonObject* findJsonTable(Util::JsonArray* tablesJson, cstring tblName) {
    for (auto *t : *tablesJson) {
        auto *tblObj = t->to<Util::JsonObject>();
        auto tName = tblObj->get("name")->to<Util::JsonValue>()->getString();
        if (tName == tblName) {
            return tblObj;
        }
    }
    return nullptr;
}

}  // namespace BFRT

}  // namespace DPDK 