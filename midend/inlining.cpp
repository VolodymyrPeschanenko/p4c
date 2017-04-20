/*
Copyright 2013-present Barefoot Networks, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "lib/nullstream.h"
#include "frontends/p4/def_use.h"

#include "inlining.h"
#include "frontends/p4/callGraph.h"
#include "frontends/p4/methodInstance.h"
#include "frontends/common/resolveReferences/resolveReferences.h"
#include "frontends/p4/parameterSubstitution.h"
#include "frontends/p4/typeChecking/typeChecker.h"
#include "frontends/p4/moveDeclarations.h"
#include "frontends/p4/resetHeaders.h"
#include "frontends/p4/toP4/toP4.h"

namespace P4 {

namespace {

class FindLocationSets : public Inspector {
    StorageMap *storageMap;
    std::map<const IR::Expression*, const LocationSet*> loc;

    const LocationSet* get(const IR::Expression* expression) const {
        auto result = ::get(loc, expression);
        BUG_CHECK(result != nullptr, "No location set known for %1%", expression);
        return result;
    }
    void set(const IR::Expression* expression, const LocationSet* ls) {
        CHECK_NULL(expression); CHECK_NULL(ls);
        loc.emplace(expression, ls);
    }

 public:
    FindLocationSets(ReferenceMap* refMap, TypeMap* typeMap) :
            storageMap(new StorageMap(refMap, typeMap)) {}

    // default behavior
    bool preorder(const IR::Expression* expression) {
        set(expression, LocationSet::empty);
        return false;
    }

    bool preorder(const IR::Slice* expression) {
        visit(expression->e0);
        auto base = get(expression->e0);
        set(expression, base);
        return false;
    }

    bool preorder(const IR::TypeNameExpression* expression) {
        set(expression, LocationSet::empty);
        return false;
    }

    bool preorder(const IR::PathExpression* expression) {
        auto decl = storageMap->refMap->getDeclaration(expression->path, true);
        auto storage = storageMap->getStorage(decl);
        const LocationSet* result;
        if (storage != nullptr)
            result = new LocationSet(storage);
        else
            result = LocationSet::empty;
        set(expression, result);
        return false;
    }

    bool preorder(const IR::Member* expression) {
        visit(expression->expr);
        auto type = storageMap->typeMap->getType(expression, true);
        if (type->is<IR::Type_Method>())
            return false;
        auto storage = get(expression->expr);

        auto basetype = storageMap->typeMap->getType(expression->expr, true);
        if (basetype->is<IR::Type_Stack>()) {
            if (expression->member.name == IR::Type_Stack::next ||
                expression->member.name == IR::Type_Stack::last) {
                set(expression, storage);
                return false;
            }
        }

        auto fields = storage->getField(expression->member);
        set(expression, fields);
        return false;
    }

    bool preorder(const IR::ArrayIndex* expression) {
        visit(expression->left);
        visit(expression->right);
        auto storage = get(expression->left);
        if (expression->right->is<IR::Constant>()) {
            auto cst = expression->right->to<IR::Constant>();
            auto index = cst->asInt();
            auto result = storage->getIndex(index);
            set(expression, result);
        } else {
            set(expression, storage->allElements());
        }
        return false;
    }

    bool preorder(const IR::Operation_Binary* expression) {
        visit(expression->left);
        visit(expression->right);
        auto l = get(expression->left);
        auto r = get(expression->right);
        auto result = l->join(r);
        set(expression, result);
        return false;
    }

    bool preorder(const IR::Mux* expression) {
        visit(expression->e0);
        visit(expression->e1);
        visit(expression->e2);
        auto e0 = get(expression->e0);
        auto e1 = get(expression->e1);
        auto e2 = get(expression->e2);
        auto result = e0->join(e1)->join(e2);
        set(expression, result);
        return false;
    }

    bool preorder(const IR::ListExpression* expression) {
        expression->components.visit_children(*this);
        auto l = LocationSet::empty;
        for (auto c : expression->components) {
            auto cl = get(c);
            l = l->join(cl);
        }
        set(expression, l);
        return false;
    }

    bool preorder(const IR::Operation_Unary* expression) {
        visit(expression->expr);
        auto result = get(expression->expr);
        set(expression, result);
        return false;
    }

    const LocationSet* locations(const IR::Expression* expression) {
        (void)expression->apply(*this);
        auto ls = get(expression);
        if (ls != nullptr)
            return ls->canonicalize();
        return nullptr;
    }
};

// This class computes new names for inlined objects.
// An object's name is prefixed with the instance name that includes it.
// For example:
// control c() {
//   table t() { ... }  apply { t.apply() }
// }
// control d() {
//   c() cinst;
//   apply { cinst.apply(); }
// }
// After inlining we will get:
// control d() {
//   @name("cinst.t") table cinst_t() { ... }
//   apply { cinst_t.apply(); }
// }
// So the externally visible name for the table is "cinst.t"
class ComputeNewNames : public Inspector {
    cstring           prefix;
    P4::ReferenceMap* refMap;
    SymRenameMap*     renameMap;

 public:
    ComputeNewNames(cstring prefix, P4::ReferenceMap* refMap, SymRenameMap* renameMap) :
            prefix(prefix), refMap(refMap), renameMap(renameMap) {
        BUG_CHECK(!prefix.isNullOrEmpty(), "Null prefix");
        CHECK_NULL(refMap); CHECK_NULL(renameMap);
    }

    void rename(const IR::Declaration* decl) {
        BUG_CHECK(decl->is<IR::IAnnotated>(), "%1%: no annotations", decl);
        cstring name = decl->externalName();
        cstring extName;
        if (name.startsWith("."))
            // Do not change the external name of objects starting with a leading dot
            extName = name;
        else
            extName = prefix + "." + name;
        cstring baseName = extName.replace('.', '_');
        cstring newName = refMap->newName(baseName);
        renameMap->setNewName(decl, newName, extName);
    }
    void postorder(const IR::P4Table* table) override { rename(table); }
    void postorder(const IR::P4Action* action) override { rename(action); }
    void postorder(const IR::Declaration_Instance* instance) override { rename(instance); }
    void postorder(const IR::Declaration_Variable* decl) override { rename(decl); }
};

// Add a @name annotation ONLY.
static const IR::Annotations*
setNameAnnotation(cstring name, const IR::Annotations* annos) {
    if (annos == nullptr)
        annos = IR::Annotations::empty;
    return annos->addOrReplace(IR::Annotation::nameAnnotation,
                               new IR::StringLiteral(name));
}


// Perform multiple substitutions and rename global objects, such as
// tables, actions and instances.  Unfortunately these transformations
// have to be performed at the same time, because otherwise the refMap
// is invalidated.
class Substitutions : public SubstituteParameters {
    P4::ReferenceMap* refMap;  // updated
    const SymRenameMap*  renameMap;  // map with new names for global objects

 public:
    Substitutions(P4::ReferenceMap* refMap,
                  P4::ParameterSubstitution* subst,
                  P4::TypeVariableSubstitution* tvs,
                  const SymRenameMap* renameMap) :
            SubstituteParameters(refMap, subst, tvs),
            refMap(refMap), renameMap(renameMap)
    { CHECK_NULL(refMap); CHECK_NULL(renameMap); }
    const IR::Node* postorder(IR::P4Table* table) override {
        auto orig = getOriginal<IR::IDeclaration>();
        cstring newName = renameMap->getName(orig);
        cstring extName = renameMap->getExtName(orig);
        LOG1("Renaming " << dbp(orig) << " to " << newName << " (" << extName << ")");
        auto annos = setNameAnnotation(extName, table->annotations);
        auto result = new IR::P4Table(table->srcInfo, newName, annos,
                                      table->properties);
        return result;
    }
    const IR::Node* postorder(IR::P4Action* action) override {
        auto orig = getOriginal<IR::IDeclaration>();
        cstring newName = renameMap->getName(orig);
        cstring extName = renameMap->getExtName(orig);
        LOG1("Renaming " << dbp(orig) << " to " << newName << "(" << extName << ")");
        auto annos = setNameAnnotation(extName, action->annotations);
        auto result = new IR::P4Action(action->srcInfo, newName, annos,
                                       action->parameters, action->body);
        return result;
    }
    const IR::Node* postorder(IR::Declaration_Instance* instance) override {
        auto orig = getOriginal<IR::IDeclaration>();
        cstring newName = renameMap->getName(orig);
        cstring extName = renameMap->getExtName(orig);
        LOG1("Renaming " << dbp(orig) << " to " << newName << "(" << extName << ")");
        auto annos = setNameAnnotation(extName, instance->annotations);
        instance->name = newName;
        instance->annotations = annos;
        return instance;
    }
    const IR::Node* postorder(IR::Declaration_Variable* decl) override {
        auto orig = getOriginal<IR::IDeclaration>();
        cstring newName = renameMap->getName(orig);
        cstring extName = renameMap->getExtName(orig);
        LOG1("Renaming " << dbp(orig) << " to " << newName << "(" << extName << ")");
        decl->name = newName;
        return decl;
    }
    const IR::Node* postorder(IR::PathExpression* expression) override {
        LOG1("(Substitutions) visiting" << dbp(getOriginal()));
        auto decl = refMap->getDeclaration(expression->path, true);
        auto param = decl->to<IR::Parameter>();
        if (param != nullptr && subst->contains(param)) {
            // This path is the same as in SubstituteParameters
            auto value = subst->lookup(param);
            LOG1("(Substitutions) Replaced " << dbp(expression) << " for parameter "
                 << decl << " with " << dbp(value));
            return value;
        }

        cstring newName;
        if (renameMap->isRenamed(decl))
            newName = renameMap->getName(decl);
        else
            newName = expression->path->name;
        IR::ID newid = IR::ID(expression->path->srcInfo, newName);
        auto newpath = new IR::Path(newid, expression->path->absolute);
        auto result = new IR::PathExpression(newpath);
        refMap->setDeclaration(newpath, decl);
        LOG1("(Substitutions) replaced " << dbp(getOriginal()) << " with " << dbp(result));
        return result;
    }
};
}  // namespace

template <class T>
const T* PerInstanceSubstitutions::rename(ReferenceMap* refMap, const IR::Node* node) {
    Substitutions rename(refMap, &paramSubst, &tvs, &renameMap);
    auto convert = node->apply(rename);
    CHECK_NULL(convert);
    auto result = convert->to<T>();
    CHECK_NULL(result);
    return result;
}

void InlineWorkList::analyze(bool allowMultipleCalls) {
    P4::CallGraph<const IR::IContainer*> cg("Call-graph");

    for (auto m : inlineMap) {
        auto inl = m.second;
        if (inl->invocations.size() == 0) continue;
        auto it = inl->invocations.begin();
        auto first = *it;
        if (!allowMultipleCalls && inl->invocations.size() > 1) {
            ++it;
            auto second = *it;
            ::error("Multiple invocations of the same block not supported on this target: %1%, %2%",
                    first, second);
            continue;
        }
        cg.calls(inl->caller, inl->callee);
    }

    // must inline from leaves up
    std::vector<const IR::IContainer*> order;
    cg.sort(order);
    for (auto c : order) {
        // This is quadratic, but hopefully the call graph is not too large
        for (auto m : inlineMap) {
            auto inl = m.second;
            if (inl->caller == c)
                toInline.push_back(inl);
        }
    }

    std::reverse(toInline.begin(), toInline.end());
}

InlineSummary* InlineWorkList::next() {
    if (toInline.size() == 0)
        return nullptr;
    auto result = new InlineSummary();
    std::set<const IR::IContainer*> processing;
    while (!toInline.empty()) {
        auto toadd = toInline.back();
        if (processing.find(toadd->callee) != processing.end())
            break;
        toInline.pop_back();
        result->add(toadd);
        processing.emplace(toadd->caller);
    }
    return result;
}

const IR::Node* InlineDriver::preorder(IR::P4Program* program) {
    LOG1("InlineDriver");
    const IR::P4Program* prog = program;  // we need the 'const'
    toInline->analyze(true);

    while (auto todo = toInline->next()) {
        LOG1("Processing " << todo);
        inliner->prepare(toInline, todo);
        prog = prog->apply(*inliner);
        if (::errorCount() > 0)
            return prog;

#if 0
        // debugging code; we don't have an easy way to dump the program here,
        // since we are not between passes
        ToP4 top4(&std::cout, true, nullptr);
        prog->apply(top4);
#endif
    }

    prune();
    return prog;
}

/////////////////////////////////////////////////////////////////////////////////////////////

void DiscoverInlining::postorder(const IR::MethodCallStatement* statement) {
    LOG2("Visiting " << statement);
    auto mi = MethodInstance::resolve(statement, refMap, typeMap);
    if (!mi->isApply())
        return;
    auto am = mi->to<P4::ApplyMethod>();
    CHECK_NULL(am);
    if (!am->applyObject->is<IR::Type_Control>() &&
        !am->applyObject->is<IR::Type_Parser>())
        return;
    auto instantiation = am->object->to<IR::Declaration_Instance>();
    BUG_CHECK(instantiation != nullptr, "%1% expected an instance declaration", am->object);
    inlineList->addInvocation(instantiation, statement);
}

void DiscoverInlining::visit_all(const IR::Block* block) {
    for (auto it : block->constantValue) {
        if (it.second->is<IR::Block>()) {
            visit(it.second->getNode());
        }
    }
}

bool DiscoverInlining::preorder(const IR::ControlBlock* block) {
    LOG2("Visiting " << block);
    if (getContext()->node->is<IR::ParserBlock>()) {
        ::error("%1%: invocation of a control from a parser",
                block->node);
    } else if (getContext()->node->is<IR::ControlBlock>() && allowControls) {
        auto parent = getContext()->node->to<IR::ControlBlock>();
        LOG1("Will inline " << block << "@" << block->node << " into " << parent);
        auto instance = block->node->to<IR::Declaration_Instance>();
        auto callee = block->container;
        inlineList->addInstantiation(parent->container, callee, instance);
    }

    visit_all(block);
    visit(block->container->body);
    return false;
}

bool DiscoverInlining::preorder(const IR::ParserBlock* block) {
    LOG2("Visiting " << block);
    if (getContext()->node->is<IR::ControlBlock>()) {
        ::error("%1%: invocation of a parser from a control",
                block->node);
    } else if (getContext()->node->is<IR::ParserBlock>()) {
        auto parent = getContext()->node->to<IR::ParserBlock>();
        LOG1("Will inline " << block << "@" << block->node << " into " << parent);
        auto instance = block->node->to<IR::Declaration_Instance>();
        auto callee = block->container;
        inlineList->addInstantiation(parent->container, callee, instance);
    }
    visit_all(block);
    block->container->states.visit_children(*this);
    return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

Visitor::profile_t GeneralInliner::init_apply(const IR::Node* node) {
    ResolveReferences solver(refMap);
    TypeChecking typeChecker(refMap, typeMap);
    node->apply(solver);
    (void)node->apply(typeChecker);
    return AbstractInliner::init_apply(node);
}

const IR::Node* GeneralInliner::preorder(IR::P4Control* caller) {
    // prepares the code to inline
    auto orig = getOriginal<IR::P4Control>();
    if (toInline->callerToWork.find(orig) == toInline->callerToWork.end()) {
        prune();
        return caller;
    }

    workToDo = &toInline->callerToWork[orig];
    LOG1("Analyzing " << dbp(caller));
    IR::IndexedVector<IR::Declaration> locals;
    for (auto s : caller->controlLocals) {
        auto inst = s->to<IR::Declaration_Instance>();
        if (inst == nullptr ||
            workToDo->declToCallee.find(inst) == workToDo->declToCallee.end()) {
            // not a call
            locals.push_back(s);
        } else {
            auto callee = workToDo->declToCallee[inst]->to<IR::P4Control>();
            CHECK_NULL(callee);
            auto substs = new PerInstanceSubstitutions();
            workToDo->substitutions[inst] = substs;

            // Substitute constructor parameters
            substs->paramSubst.populate(callee->getConstructorParameters(), inst->arguments);
            if (inst->type->is<IR::Type_Specialized>()) {
                auto spec = inst->type->to<IR::Type_Specialized>();
                substs->tvs.setBindings(callee->getNode(),
                                        callee->getTypeParameters(), spec->arguments);
            }

            // Must rename callee local objects prefixing them with their instance name.
            cstring prefix = inst->externalName();
            ComputeNewNames cnn(prefix, refMap, &substs->renameMap);
            (void)callee->apply(cnn);  // populates substs.renameMap

            // Use temporaries for these parameters
            std::set<const IR::Parameter*> useTemporary;

            auto call = workToDo->uniqueCaller(inst);
            MethodCallDescription *mcd = nullptr;
            if (call != nullptr) {
                std::map<const IR::Parameter*, const LocationSet*> locationSets;
                FindLocationSets fls(refMap, typeMap);

                mcd = new MethodCallDescription(call->methodCall, refMap, typeMap);
                for (auto param : *mcd->substitution.getParameters()) {
                    auto arg = mcd->substitution.lookup(param);
                    auto ls = fls.locations(arg);
                    locationSets.emplace(param, ls);
                }

                for (auto param1 : *mcd->substitution.getParameters()) {
                    auto ls1 = ::get(locationSets, param1);
                    for (auto param2 : *mcd->substitution.getParameters()) {
                        if (param1 == param2) continue;
                        auto ls2 = ::get(locationSets, param2);
                        if (ls1->overlaps(ls2)) {
                            LOG2("Arg for " << dbp(param1) << " aliases with arg for "
                                 << dbp(param2) << ": using temp");
                            useTemporary.emplace(param1);
                            useTemporary.emplace(param2);
                        }
                    }
                }
            }

            // Substitute applyParameters which are not directionless
            // with fresh variable names or with the call arguments.
            for (auto param : callee->type->applyParams->parameters) {
                if (param->direction == IR::Direction::None)
                    continue;
                if (call != nullptr && (useTemporary.find(param) == useTemporary.end())) {
                    // Substitute argument directly
                    CHECK_NULL(mcd);
                    auto initializer = mcd->substitution.lookup(param);
                    LOG1("Substituting callee parameter " << dbp(param)
                         << " with " << dbp(initializer));
                    substs->paramSubst.add(param, initializer);
                } else {
                    // use a temporary variable
                    cstring newName = refMap->newName(param->name);
                    auto path = new IR::PathExpression(newName);
                    substs->paramSubst.add(param, path);
                    LOG1("Replacing " << param->name << " with " << newName);
                    auto vardecl = new IR::Declaration_Variable(newName,
                                                                param->annotations, param->type);
                    locals.push_back(vardecl);
                }
            }

            /* We will perform these substitutions twice: once here, to
               compute the names for the locals that we need to inline here,
               and once again at the call site (where we do additional
               substitutions, including the callee parameters). */
            auto clone = substs->rename<IR::P4Control>(refMap, callee);
            for (auto i : clone->controlLocals)
                locals.push_back(i);
        }
    }

    visit(caller->body);
    caller->controlLocals = locals;
    list->replace(orig, caller);
    workToDo = nullptr;
    prune();
    return caller;
}

const IR::Node* GeneralInliner::preorder(IR::MethodCallStatement* statement) {
    if (workToDo == nullptr)
        return statement;
    auto orig = getOriginal<IR::MethodCallStatement>();
    if (workToDo->callToInstance.find(orig) == workToDo->callToInstance.end())
        return statement;
    LOG1("Inlining invocation " << dbp(orig));
    auto decl = workToDo->callToInstance[orig];
    CHECK_NULL(decl);

    auto called = workToDo->declToCallee[decl];
    if (!called->is<IR::P4Control>())
        // Parsers are inlined in the ParserState processor
        return statement;

    auto callee = called->to<IR::P4Control>();
    IR::IndexedVector<IR::StatOrDecl> body;
    // clone the substitution: it may be reused for multiple invocations
    auto substs = new PerInstanceSubstitutions(*workToDo->substitutions[decl]);

    MethodCallDescription mcd(statement->methodCall, refMap, typeMap);
    for (auto param : *mcd.substitution.getParameters()) {
        LOG1("Looking for " << param->name);
        auto initializer = substs->paramSubst.lookup(param);
        auto arg = mcd.substitution.lookup(param);
        if ((param->direction == IR::Direction::In || param->direction == IR::Direction::InOut) &&
            initializer != arg) {
            auto stat = new IR::AssignmentStatement(initializer, arg);
            body.push_back(stat);
        } else if (param->direction == IR::Direction::Out) {
            auto paramType = typeMap->getType(param, true);
            // This is important, since this variable may be used many times.
            DoResetHeaders::generateResets(typeMap, paramType, initializer, &body);
        }
    }

    // inline actual body
    callee = substs->rename<IR::P4Control>(refMap, callee);
    body.append(callee->body->components);

    // Copy values of out and inout parameters
    for (auto param : *mcd.substitution.getParameters()) {
        if (param->direction == IR::Direction::InOut || param->direction == IR::Direction::Out) {
            auto left = mcd.substitution.lookup(param);
            auto initializer = substs->paramSubst.lookupByName(param->name);
            if (initializer != left) {
                auto copyout = new IR::AssignmentStatement(left, initializer->clone());
                body.push_back(copyout);
            }
        }
    }

    auto annotations = callee->type->annotations->where(
        [](const IR::Annotation* a) { return a->name != IR::Annotation::nameAnnotation; });
    auto result = new IR::BlockStatement(statement->srcInfo, annotations, body);
    LOG1("Replacing " << dbp(orig) << " with " << dbp(result));
    prune();
    return result;
}

namespace {
class ComputeNewStateNames : public Inspector {
    ReferenceMap* refMap;
    cstring prefix;
    cstring acceptName;
    std::map<cstring, cstring> *stateRenameMap;
 public:
    ComputeNewStateNames(ReferenceMap* refMap, cstring prefix, cstring acceptName,
                         std::map<cstring, cstring> *stateRenameMap) :
            refMap(refMap), prefix(prefix), acceptName(acceptName), stateRenameMap(stateRenameMap)
    { CHECK_NULL(refMap); CHECK_NULL(stateRenameMap); }
    bool preorder(const IR::ParserState* state) override {
        cstring newName;
        if (state->name.name == IR::ParserState::accept) {
            newName = acceptName;
        } else {
            cstring base = prefix + "_" + state->name.name;
            newName = refMap->newName(base);
        }
        stateRenameMap->emplace(state->name.name, newName);
        return false;  // prune
    }
};

// Renames the states in a parser for inlining.  We cannot rely on the
// ReferenceMap for identifying states - it is currently inconsistent,
// so we rely on the fact that state names only appear in very
// specific places.
class RenameStates : public Transform {
    std::map<cstring, cstring> *stateRenameMap;

 public:
    explicit RenameStates(std::map<cstring, cstring> *stateRenameMap) :
            stateRenameMap(stateRenameMap)
    { CHECK_NULL(stateRenameMap); }
    const IR::Node* preorder(IR::Path* path) override {
        // This is certainly a state name, by the way we organized the visitors
        cstring newName = ::get(stateRenameMap, path->name);
        path->name = IR::ID(path->name.srcInfo, newName);
        return path;
    }
    const IR::Node* preorder(IR::SelectExpression* expression) override {
        expression->selectCases.parallel_visit_children(*this);
        prune();
        return expression;
    }
    const IR::Node* preorder(IR::SelectCase* selCase) override {
        visit(selCase->state);
        prune();
        return selCase;
    }
    const IR::Node* preorder(IR::ParserState* state) override {
        if (state->name.name == IR::ParserState::accept ||
            state->name.name == IR::ParserState::reject) {
            prune();
            return state;
        }
        cstring newName = ::get(stateRenameMap, state->name.name);
        state->name = IR::ID(state->name.srcInfo, newName);
        if (state->selectExpression != nullptr)
            visit(state->selectExpression);
        prune();
        return state;
    }
    const IR::Node* preorder(IR::P4Parser* parser) override {
        parser->states.visit_children(*this);
        prune();
        return parser;
    }
};
}  // namespace

const IR::Node* GeneralInliner::preorder(IR::ParserState* state) {
    LOG1("Visiting state " << dbp(state));
    auto states = new IR::IndexedVector<IR::ParserState>();
    IR::IndexedVector<IR::StatOrDecl> current;

    // Scan the statements to find a parser call instruction
    auto srcInfo = state->srcInfo;
    auto annotations = state->annotations;
    IR::ID name = state->name;
    for (auto e : state->components) {
        if (!e->is<IR::MethodCallStatement>()) {
            current.push_back(e);
            continue;
        }
        auto call = e->to<IR::MethodCallStatement>();
        if (workToDo->callToInstance.find(call) == workToDo->callToInstance.end()) {
            current.push_back(e);
            continue;
        }

        LOG1("Inlining invocation " << dbp(call));
        auto decl = workToDo->callToInstance[call];
        CHECK_NULL(decl);

        auto called = workToDo->declToCallee[decl];
        auto callee = called->to<IR::P4Parser>();
        // clone the substitution: it may be reused for multiple invocations
        auto substs = new PerInstanceSubstitutions(*workToDo->substitutions[decl]);

        // Evaluate in and inout parameters in order.
        auto it = call->methodCall->arguments->begin();
        for (auto param : callee->type->applyParams->parameters) {
            auto initializer = *it;
            LOG1("Looking for " << param->name);
            if (param->direction == IR::Direction::In || param->direction == IR::Direction::InOut) {
                auto expr = substs->paramSubst.lookupByName(param->name);
                auto stat = new IR::AssignmentStatement(expr, initializer);
                current.push_back(stat);
            } else if (param->direction == IR::Direction::Out) {
                auto expr = substs->paramSubst.lookupByName(param->name);
                auto paramType = typeMap->getType(param, true);
                // This is important, since this variable may be used many times.
                DoResetHeaders::generateResets(typeMap, paramType, expr, &current);
            } else if (param->direction == IR::Direction::None) {
                substs->paramSubst.add(param, initializer);
            }
            ++it;
        }

        callee = substs->rename<IR::P4Parser>(refMap, callee);

        cstring nextState = refMap->newName(state->name);
        std::map<cstring, cstring> renameMap;
        ComputeNewStateNames cnn(refMap, callee->name.name, nextState, &renameMap);
        (void)callee->apply(cnn);
        RenameStates rs(&renameMap);
        auto renamed = callee->apply(rs);
        cstring newStartName = ::get(renameMap, IR::ParserState::start);
        auto transition = new IR::PathExpression(IR::ID(newStartName, nullptr));
        auto newState = new IR::ParserState(srcInfo, name, annotations, current, transition);
        states->push_back(newState);
        for (auto s : renamed->to<IR::P4Parser>()->states) {
            if (s->name == IR::ParserState::accept ||
                s->name == IR::ParserState::reject)
                continue;
            states->push_back(s);
        }

        // Prepare next state
        annotations = IR::Annotations::empty;
        name = IR::ID(nextState, nullptr);
        current.clear();

        // Copy back out and inout parameters
        it = call->methodCall->arguments->begin();
        for (auto param : callee->type->applyParams->parameters) {
            auto left = *it;
            if (param->direction == IR::Direction::InOut ||
                param->direction == IR::Direction::Out) {
                auto expr = substs->paramSubst.lookupByName(param->name);
                auto copyout = new IR::AssignmentStatement(left, expr->clone());
                current.push_back(copyout);
            }
            ++it;
        }
    }

    if (!states->empty()) {
        // Create final state
        auto newState = new IR::ParserState(name, annotations,
                                            current, state->selectExpression);
        states->push_back(newState);
        LOG1("Replacing with " << states->size() << " states");
        prune();
        return states;
    }
    prune();
    return state;
}

const IR::Node* GeneralInliner::preorder(IR::P4Parser* caller) {
    // prepares the code to inline
    auto orig = getOriginal<IR::P4Parser>();
    if (toInline->callerToWork.find(orig) == toInline->callerToWork.end()) {
        prune();
        return caller;
    }

    workToDo = &toInline->callerToWork[orig];
    LOG1("Analyzing " << dbp(caller));
    IR::IndexedVector<IR::Declaration> locals;
    for (auto s : caller->parserLocals) {
        auto inst = s->to<IR::Declaration_Instance>();
        if (inst == nullptr ||
            workToDo->declToCallee.find(inst) == workToDo->declToCallee.end()) {
            // not a call
            locals.push_back(s);
        } else {
            auto callee = workToDo->declToCallee[inst]->to<IR::P4Parser>();
            CHECK_NULL(callee);
            auto substs = new PerInstanceSubstitutions();
            workToDo->substitutions[inst] = substs;

            // Substitute constructor parameters
            substs->paramSubst.populate(callee->getConstructorParameters(), inst->arguments);
            if (inst->type->is<IR::Type_Specialized>()) {
                auto spec = inst->type->to<IR::Type_Specialized>();
                substs->tvs.setBindings(callee->getNode(),
                                        callee->getTypeParameters(), spec->arguments);
            }

            // Must rename callee local objects prefixing them with their instance name.
            cstring prefix = inst->externalName();
            ComputeNewNames cnn(prefix, refMap, &substs->renameMap);
            (void)callee->apply(cnn);  // populates substs.renameMap

            // Substitute applyParameters which are not directionless
            // with fresh variable names.
            for (auto param : callee->type->applyParams->parameters) {
                if (param->direction == IR::Direction::None)
                    continue;
                cstring newName = refMap->newName(param->name);
                auto path = new IR::PathExpression(newName);
                substs->paramSubst.add(param, path);
                LOG1("Replacing " << param->name << " with " << newName);
                auto vardecl = new IR::Declaration_Variable(newName,
                                                            param->annotations, param->type);
                locals.push_back(vardecl);
            }

            /* We will perform these substitutions twice: once here, to
               compute the names for the locals that we need to inline here,
               and once again at the call site (where we do additional
               substitutions, including the callee parameters). */
            auto clone = substs->rename<IR::P4Parser>(refMap, callee);
            for (auto i : clone->parserLocals)
                locals.push_back(i);
        }
    }

    caller->states.visit_children(*this);
    caller->parserLocals = locals;
    list->replace(orig, caller);
    workToDo = nullptr;
    prune();
    return caller;
}

}  // namespace P4