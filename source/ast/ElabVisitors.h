//------------------------------------------------------------------------------
// ElabVisitors.h
// Internal visitors of the AST to support elaboration
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "slang/ast/ASTVisitor.h"
#include "slang/diagnostics/CompilationDiags.h"
#include "slang/diagnostics/DeclarationsDiags.h"
#include "slang/util/StackContainer.h"
#include "slang/util/TimeTrace.h"

namespace slang::ast {

using namespace syntax;

// This visitor is used to touch every node in the AST to ensure that all lazily
// evaluated members have been realized and we have recorded every diagnostic.
struct DiagnosticVisitor : public ASTVisitor<DiagnosticVisitor, false, false> {
    DiagnosticVisitor(Compilation& compilation, const size_t& numErrors, uint32_t errorLimit) :
        compilation(compilation), numErrors(numErrors), errorLimit(errorLimit) {}

    template<typename T>
    void handle(const T& symbol) {
        handleDefault(symbol);
    }

    template<typename T>
    bool handleDefault(const T& symbol) {
        if (numErrors > errorLimit || hierarchyProblem)
            return false;

        if constexpr (std::is_base_of_v<Symbol, T>) {
            auto declaredType = symbol.getDeclaredType();
            if (declaredType) {
                declaredType->getType();
                declaredType->getInitializer();
            }

            if constexpr (std::is_same_v<ParameterSymbol, T> ||
                          std::is_same_v<EnumValueSymbol, T> ||
                          std::is_same_v<SpecparamSymbol, T>) {
                symbol.getValue();
            }

            for (auto attr : compilation.getAttributes(symbol))
                attr->getValue();
        }

        if constexpr (is_detected_v<getBody_t, T>) {
            auto& body = symbol.getBody();
            if (body.bad())
                return true;

            body.visit(*this);
        }

        visitDefault(symbol);
        return true;
    }

    void handle(const ExplicitImportSymbol& symbol) {
        if (!handleDefault(symbol))
            return;
        symbol.importedSymbol();
    }

    void handle(const WildcardImportSymbol& symbol) {
        if (!handleDefault(symbol))
            return;
        symbol.getPackage();
    }

    void handle(const InterfacePortSymbol& symbol) {
        if (!handleDefault(symbol))
            return;
        symbol.getDeclaredRange();

        if (symbol.interfaceDef) {
            usedIfacePorts.emplace(symbol.interfaceDef);

            // If we this interface port specifies a modport and that
            // modport has export methods, store it in a list for later
            // processing and checking.
            if (!symbol.modport.empty()) {
                auto conn = symbol.getConnection();
                if (conn) {
                    if (conn->kind == SymbolKind::Instance)
                        conn = conn->as<InstanceSymbol>().body.find(symbol.modport);

                    if (conn && conn->kind == SymbolKind::Modport)
                        modportsWithExports.push_back({&symbol, &conn->as<ModportSymbol>()});
                }
            }
        }
    }

    void handle(const PortSymbol& symbol) {
        if (!handleDefault(symbol))
            return;
        symbol.getType();
        symbol.getInitializer();
    }

    void handle(const MultiPortSymbol& symbol) {
        if (!handleDefault(symbol))
            return;
        symbol.getType();
    }

    void handle(const ContinuousAssignSymbol& symbol) {
        if (!handleDefault(symbol))
            return;
        symbol.getAssignment();
        symbol.getDelay();
    }

    void handle(const ElabSystemTaskSymbol& symbol) {
        if (!handleDefault(symbol))
            return;
        symbol.issueDiagnostic();
    }

    void handle(const MethodPrototypeSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        if (auto sub = symbol.getSubroutine())
            handle(*sub);

        if (symbol.flags.has(MethodFlags::InterfaceExtern))
            externIfaceProtos.push_back(&symbol);
    }

    void handle(const GenericClassDefSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        // Save this for later; we need to revist all generic classes
        // once we've finished checking everything else.
        genericClasses.push_back(&symbol);
    }

    void handle(const NetType& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.getDataType();
        symbol.getResolutionFunction();
    }

    void handle(const ClassType& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.getBaseConstructorCall();
    }

    void handle(const CovergroupType& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.getCoverageEvent();
        for (auto& option : symbol.body.options)
            option.getExpression();
    }

    void handle(const CoverpointSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.getIffExpr();
        for (auto& option : symbol.options)
            option.getExpression();
    }

    void handle(const CoverCrossSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.getIffExpr();
        for (auto& option : symbol.options)
            option.getExpression();
    }

    void handle(const CoverageBinSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.getValues();
    }

    void handle(const NetSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.getDelay();
        symbol.checkInitializer();
    }

    void handle(const ConstraintBlockSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.getConstraints();
    }

    void handle(const UnknownModuleSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.getPortConnections();
    }

    void handle(const PrimitiveInstanceSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.getPortConnections();
        symbol.getDelay();
    }

    void handle(const ClockingBlockSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.getEvent();
        symbol.getDefaultInputSkew();
        symbol.getDefaultOutputSkew();
    }

    void handle(const InstanceSymbol& symbol) {
        if (numErrors > errorLimit || hierarchyProblem)
            return;

        TimeTraceScope timeScope("AST Instance", [&] {
            std::string buffer;
            symbol.getHierarchicalPath(buffer);
            return buffer;
        });

        instanceCount[&symbol.getDefinition()]++;

        for (auto attr : compilation.getAttributes(symbol))
            attr->getValue();

        for (auto conn : symbol.getPortConnections()) {
            conn->getExpression();
            conn->checkSimulatedNetTypes();
            for (auto attr : compilation.getAttributes(*conn))
                attr->getValue();
        };

        // Detect infinite recursion, which happens if we see this exact
        // instance body somewhere higher up in the stack.
        if (!activeInstanceBodies.emplace(&symbol.body).second) {
            symbol.getParentScope()->addDiag(diag::InfinitelyRecursiveHierarchy, symbol.location)
                << symbol.name;
            hierarchyProblem = true;
            return;
        }

        // In order to avoid "effectively infinite" recursions, where parameter values
        // are changing but the numbers are so huge that we would run for almost forever,
        // check the depth and bail out after a certain configurable point.
        auto guard = ScopeGuard([this, &symbol] { activeInstanceBodies.erase(&symbol.body); });
        if (activeInstanceBodies.size() > compilation.getOptions().maxInstanceDepth) {
            auto& diag = symbol.getParentScope()->addDiag(diag::MaxInstanceDepthExceeded,
                                                          symbol.location);
            diag << symbol.getDefinition().getKindString();
            diag << compilation.getOptions().maxInstanceDepth;
            hierarchyProblem = true;
            return;
        }

        visit(symbol.body);
    }

    void handle(const GenerateBlockSymbol& symbol) {
        if (!symbol.isInstantiated)
            return;
        handleDefault(symbol);
    }

    void handle(const SubroutineSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        if (symbol.flags.has(MethodFlags::DPIImport))
            dpiImports.push_back(&symbol);
    }

    void handle(const DefParamSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.getTarget();
        symbol.getValue();
    }

    void handle(const SequenceSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.makeDefaultInstance();
    }

    void handle(const PropertySymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.makeDefaultInstance();
    }

    void handle(const LetDeclSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.makeDefaultInstance();
    }

    void handle(const RandSeqProductionSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.getRules();
    }

    void handle(const TimingPathSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.checkDuplicatePaths(timingPathMap);
    }

    void handle(const PulseStyleSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.checkPreviouslyUsed(timingPathMap);
    }

    void handle(const SystemTimingCheckSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.getArguments();
    }

    void handle(const SpecparamSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.getPathSource();
    }

    void finalize() {
        // Once everything has been visited, go back over and check things that might
        // have been influenced by visiting later symbols. Unfortunately visiting
        // a specialization can trigger more specializations to be made for the
        // same or other generic classs, so we need to be careful here when iterating.
        SmallSet<const Type*, 8> visitedSpecs;
        SmallVector<const Type*> toVisit;
        bool didSomething;
        do {
            didSomething = false;
            for (auto symbol : genericClasses) {
                for (auto& spec : symbol->specializations()) {
                    if (visitedSpecs.emplace(&spec).second)
                        toVisit.push_back(&spec);
                }

                for (auto spec : toVisit) {
                    spec->visit(*this);
                    didSomething = true;
                }

                toVisit.clear();
            }
        } while (didSomething);

        // Go back over and find generic classes that were never instantiated
        // and force an empty one to make sure we collect all diagnostics that
        // don't depend on parameter values.
        for (auto symbol : genericClasses) {
            if (symbol->numSpecializations() == 0)
                symbol->getInvalidSpecialization().visit(*this);
        }
    }

    Compilation& compilation;
    const size_t& numErrors;
    uint32_t errorLimit;
    bool hierarchyProblem = false;
    flat_hash_map<const Definition*, size_t> instanceCount;
    flat_hash_set<const InstanceBodySymbol*> activeInstanceBodies;
    flat_hash_set<const Definition*> usedIfacePorts;
    SmallVector<const GenericClassDefSymbol*> genericClasses;
    SmallVector<const SubroutineSymbol*> dpiImports;
    SmallVector<const MethodPrototypeSymbol*> externIfaceProtos;
    SmallVector<std::pair<const InterfacePortSymbol*, const ModportSymbol*>> modportsWithExports;
    TimingPathMap timingPathMap;
};

// This visitor is for finding all bind directives in the hierarchy.
struct BindVisitor : public ASTVisitor<BindVisitor, false, false> {
    BindVisitor(const flat_hash_set<const BindDirectiveSyntax*>& foundDirectives, size_t expected) :
        foundDirectives(foundDirectives), expected(expected) {}

    void handle(const RootSymbol& symbol) { visitDefault(symbol); }

    void handle(const CompilationUnitSymbol& symbol) {
        if (foundDirectives.size() == expected)
            return;
        visitDefault(symbol);
    }

    void handle(const InstanceSymbol& symbol) {
        if (foundDirectives.size() == expected)
            return;

        if (!visitedInstances.emplace(&symbol.body).second) {
            errored = true;
            return;
        }

        visitDefault(symbol.body);
    }

    void handle(const GenerateBlockSymbol& symbol) {
        if (foundDirectives.size() == expected || !symbol.isInstantiated)
            return;
        visitDefault(symbol);
    }

    void handle(const GenerateBlockArraySymbol& symbol) {
        if (foundDirectives.size() == expected)
            return;

        auto members = symbol.members();
        if (members.begin() == members.end())
            return;

        visit(*members.begin());
    }

    template<typename T>
    void handle(const T&) {}

    const flat_hash_set<const BindDirectiveSyntax*>& foundDirectives;
    flat_hash_set<const InstanceBodySymbol*> visitedInstances;
    size_t expected;
    bool errored = false;
};

// This visitor is for finding all defparam directives in the hierarchy.
// We're given a target generate "level" to reach, where the level is a measure
// of how deep the design is in terms of nested generate blocks. Once we reach
// the target level we don't go any deeper, except for the following case:
//
// If we find a potentially infinitely recursive module (because it instantiates
// itself directly or indirectly) we will continue traversing deeper to see if we
// hit the limit for max depth, which lets us bail out of defparam evaluation completely.
// Since defparams are disallowed from modifying parameters above them across generate
// blocks, an infinitely recursive module instantiation can't be stopped by a deeper
// defparam evaluation.
struct DefParamVisitor : public ASTVisitor<DefParamVisitor, false, false> {
    DefParamVisitor(size_t maxInstanceDepth, size_t generateLevel) :
        maxInstanceDepth(maxInstanceDepth), generateLevel(generateLevel) {}

    void handle(const RootSymbol& symbol) { visitDefault(symbol); }
    void handle(const CompilationUnitSymbol& symbol) { visitDefault(symbol); }

    void handle(const DefParamSymbol& symbol) {
        if (generateDepth <= generateLevel)
            found.push_back(&symbol);
    }

    void handle(const InstanceSymbol& symbol) {
        if (hierarchyProblem)
            return;

        // If we hit max depth we have a problem -- setting the hierarchyProblem
        // member will cause other functions to early out so that we completely
        // this visitation as quickly as possible.
        if (instanceDepth > maxInstanceDepth) {
            hierarchyProblem = &symbol;
            return;
        }

        bool inserted = false;
        const bool wasInRecursive = inRecursiveInstance;
        if (!inRecursiveInstance) {
            // If the instance's definition is already in the active set,
            // we potentially have an infinitely recursive instantiation and
            // need to go all the way to the maximum depth to find out.
            inserted = activeInstances.emplace(&symbol.getDefinition()).second;
            if (!inserted)
                inRecursiveInstance = true;
        }

        // If we're past our target depth because we're searching for a potentially
        // infinitely recursive instantiation, don't count the block.
        if (generateDepth <= generateLevel)
            numBlocksSeen++;

        instanceDepth++;
        visitDefault(symbol.body);
        instanceDepth--;

        inRecursiveInstance = wasInRecursive;
        if (inserted)
            activeInstances.erase(&symbol.getDefinition());
    }

    void handle(const GenerateBlockSymbol& symbol) {
        if (!symbol.isInstantiated || hierarchyProblem)
            return;

        if (generateDepth >= generateLevel && !inRecursiveInstance)
            return;

        // We don't count the case where we are *at* the target level
        // because we're about to descend into the generate block,
        // so it counts as deeper level.
        if (generateDepth < generateLevel)
            numBlocksSeen++;

        generateDepth++;
        visitDefault(symbol);
        generateDepth--;
    }

    void handle(const GenerateBlockArraySymbol& symbol) {
        for (auto& member : symbol.members()) {
            if (hierarchyProblem)
                return;
            visit(member);
        }
    }

    template<typename T>
    void handle(const T&) {}

    SmallVector<const DefParamSymbol*> found;
    flat_hash_set<const Definition*> activeInstances;
    size_t instanceDepth = 0;
    size_t maxInstanceDepth = 0;
    size_t generateLevel = 0;
    size_t numBlocksSeen = 0;
    size_t generateDepth = 0;
    bool inRecursiveInstance = false;
    const InstanceSymbol* hierarchyProblem = nullptr;
};

} // namespace slang::ast
