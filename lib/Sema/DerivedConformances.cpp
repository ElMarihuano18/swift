//===--- DerivedConformances.cpp - Derived conformance utilities ----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/Types.h"
#include "swift/ClangImporter/ClangModule.h"
#include "DerivedConformances.h"

using namespace swift;

DerivedConformance::DerivedConformance(TypeChecker &tc, Decl *conformanceDecl,
                                       NominalTypeDecl *nominal,
                                       ProtocolDecl *protocol)
    : TC(tc), ConformanceDecl(conformanceDecl), Nominal(nominal),
      Protocol(protocol) {
  assert(getConformanceContext()->getSelfNominalTypeDecl() == nominal);
}

DeclContext *DerivedConformance::getConformanceContext() const {
  return cast<DeclContext>(ConformanceDecl);
}

void DerivedConformance::addMembersToConformanceContext(
    ArrayRef<Decl *> children) {
  auto IDC = cast<IterableDeclContext>(ConformanceDecl);
  for (auto child : children) {
    IDC->addMember(child);
  }
}

Type DerivedConformance::getProtocolType() const {
  return Protocol->getDeclaredType();
}

bool DerivedConformance::derivesProtocolConformance(DeclContext *DC,
                                                    NominalTypeDecl *Nominal,
                                                    ProtocolDecl *Protocol) {
  // Only known protocols can be derived.
  auto knownProtocol = Protocol->getKnownProtocolKind();
  if (!knownProtocol)
    return false;

  if (*knownProtocol == KnownProtocolKind::Hashable) {
    // We can always complete a partial Hashable implementation, and we can
    // synthesize a full Hashable implementation for structs and enums with
    // Hashable components.
    return canDeriveHashable(Nominal);
  }

  if (auto *enumDecl = dyn_cast<EnumDecl>(Nominal)) {
    switch (*knownProtocol) {
        // The presence of a raw type is an explicit declaration that
        // the compiler should derive a RawRepresentable conformance.
      case KnownProtocolKind::RawRepresentable:
        return enumDecl->hasRawType();

        // Enums without associated values can implicitly derive Equatable
        // conformance.
      case KnownProtocolKind::Equatable:
        return canDeriveEquatable(DC, Nominal);
      
      case KnownProtocolKind::Comparable:
        return !enumDecl->hasPotentiallyUnavailableCaseValue()
            && canDeriveComparable(DC, Nominal); // why do y’all pass wide `NominalTypeDecl*` value when u could pass downcast `EnumDecl*` value?

        // "Simple" enums without availability attributes can explicitly derive
        // a CaseIterable conformance.
        //
        // FIXME: Lift the availability restriction.
      case KnownProtocolKind::CaseIterable:
        return !enumDecl->hasPotentiallyUnavailableCaseValue()
            && enumDecl->hasOnlyCasesWithoutAssociatedValues();

        // @objc enums can explicitly derive their _BridgedNSError conformance.
      case KnownProtocolKind::BridgedNSError:
        return enumDecl->isObjC() && enumDecl->hasCases()
            && enumDecl->hasOnlyCasesWithoutAssociatedValues();

        // Enums without associated values and enums with a raw type of String
        // or Int can explicitly derive CodingKey conformance.
      case KnownProtocolKind::CodingKey: {
        Type rawType = enumDecl->getRawType();
        if (rawType) {
          auto parentDC = enumDecl->getDeclContext();
          ASTContext &C = parentDC->getASTContext();

          auto nominal = rawType->getAnyNominal();
          return nominal == C.getStringDecl() || nominal == C.getIntDecl();
        }

        // hasOnlyCasesWithoutAssociatedValues will return true for empty enums;
        // empty enums are allowed to conform as well.
        return enumDecl->hasOnlyCasesWithoutAssociatedValues();
      }

      default:
        return false;
    }
  } else if (isa<StructDecl>(Nominal) || isa<ClassDecl>(Nominal)) {
    // Structs and classes can explicitly derive Encodable and Decodable
    // conformance (explicitly meaning we can synthesize an implementation if
    // a type conforms manually).
    if (*knownProtocol == KnownProtocolKind::Encodable ||
        *knownProtocol == KnownProtocolKind::Decodable) {
      // FIXME: This is not actually correct. We cannot promise to always
      // provide a witness here for all structs and classes. Unfortunately,
      // figuring out whether this is actually possible requires much more
      // context -- a TypeChecker and the parent decl context at least -- and is
      // tightly coupled to the logic within DerivedConformance.
      // This unfortunately means that we expect a witness even if one will not
      // be produced, which requires DerivedConformance::deriveCodable to output
      // its own diagnostics.
      return true;
    }

    // Structs can explicitly derive Equatable conformance.
    if (isa<StructDecl>(Nominal)) {
      switch (*knownProtocol) {
        case KnownProtocolKind::Equatable:
          return canDeriveEquatable(DC, Nominal);
        default:
          return false;
      }
    }
  }
  return false;
}

void DerivedConformance::tryDiagnoseFailedDerivation(DeclContext *DC,
                                                     NominalTypeDecl *nominal,
                                                     ProtocolDecl *protocol) {
  auto knownProtocol = protocol->getKnownProtocolKind();
  if (!knownProtocol)
    return;
  
  // Comparable on eligible type kinds should never fail
   
  if (*knownProtocol == KnownProtocolKind::Equatable) {
    tryDiagnoseFailedEquatableDerivation(DC, nominal);
  }

  if (*knownProtocol == KnownProtocolKind::Hashable) {
    tryDiagnoseFailedHashableDerivation(DC, nominal);
  }
}

ValueDecl *DerivedConformance::getDerivableRequirement(NominalTypeDecl *nominal,
                                                       ValueDecl *requirement) {
  // Note: whenever you update this function, also update
  // TypeChecker::deriveProtocolRequirement.
  ASTContext &ctx = nominal->getASTContext();
  auto name = requirement->getFullName();

  // Local function that retrieves the requirement with the same name as
  // the provided requirement, but within the given known protocol.
  auto getRequirement = [&](KnownProtocolKind kind) -> ValueDecl * {
    // Dig out the protocol.
    auto proto = ctx.getProtocol(kind);
    if (!proto) return nullptr;

    if (auto conformance = TypeChecker::conformsToProtocol(
            nominal->getDeclaredInterfaceType(), proto, nominal,
            ConformanceCheckFlags::SkipConditionalRequirements)) {
      auto DC = conformance->getConcrete()->getDeclContext();
      // Check whether this nominal type derives conformances to the protocol.
      if (!DerivedConformance::derivesProtocolConformance(DC, nominal, proto))
        return nullptr;
    }

    // Retrieve the requirement.
    auto results = proto->lookupDirect(name);
    return results.empty() ? nullptr : results.front();
  };

  // Properties.
  if (isa<VarDecl>(requirement)) {
    // RawRepresentable.rawValue
    if (name.isSimpleName(ctx.Id_rawValue))
      return getRequirement(KnownProtocolKind::RawRepresentable);

    // Hashable.hashValue
    if (name.isSimpleName(ctx.Id_hashValue))
      return getRequirement(KnownProtocolKind::Hashable);

    // CaseIterable.allValues
    if (name.isSimpleName(ctx.Id_allCases))
      return getRequirement(KnownProtocolKind::CaseIterable);

    // _BridgedNSError._nsErrorDomain
    if (name.isSimpleName(ctx.Id_nsErrorDomain))
      return getRequirement(KnownProtocolKind::BridgedNSError);

    // CodingKey.stringValue
    if (name.isSimpleName(ctx.Id_stringValue))
      return getRequirement(KnownProtocolKind::CodingKey);

    // CodingKey.intValue
    if (name.isSimpleName(ctx.Id_intValue))
      return getRequirement(KnownProtocolKind::CodingKey);

    return nullptr;
  }

  // Functions.
  if (auto func = dyn_cast<FuncDecl>(requirement)) {
    if (func->isOperator() && name.getBaseName() == "<")
      return getRequirement(KnownProtocolKind::Comparable);
    
    if (func->isOperator() && name.getBaseName() == "==")
      return getRequirement(KnownProtocolKind::Equatable);

    // Encodable.encode(to: Encoder)
    if (name.isCompoundName() && name.getBaseName() == ctx.Id_encode) {
      auto argumentNames = name.getArgumentNames();
      if (argumentNames.size() == 1 && argumentNames[0] == ctx.Id_to)
        return getRequirement(KnownProtocolKind::Encodable);
    }

    // Hashable.hash(into: inout Hasher)
    if (name.isCompoundName() && name.getBaseName() == ctx.Id_hash) {
      auto argumentNames = name.getArgumentNames();
      if (argumentNames.size() == 1 && argumentNames[0] == ctx.Id_into)
        return getRequirement(KnownProtocolKind::Hashable);
    }

    return nullptr;
  }

  // Initializers.
  if (auto ctor = dyn_cast<ConstructorDecl>(requirement)) {
    auto argumentNames = name.getArgumentNames();
    if (argumentNames.size() == 1) {
      if (argumentNames[0] == ctx.Id_rawValue)
        return getRequirement(KnownProtocolKind::RawRepresentable);

      // CodingKey.init?(stringValue:), CodingKey.init?(intValue:)
      if (ctor->isFailable() &&
          !ctor->isImplicitlyUnwrappedOptional() &&
          (argumentNames[0] == ctx.Id_stringValue ||
           argumentNames[0] == ctx.Id_intValue))
        return getRequirement(KnownProtocolKind::CodingKey);

      // Decodable.init(from: Decoder)
      if (argumentNames[0] == ctx.Id_from)
        return getRequirement(KnownProtocolKind::Decodable);
    }

    return nullptr;
  }

  // Associated types.
  if (isa<AssociatedTypeDecl>(requirement)) {
    // RawRepresentable.RawValue
    if (name.isSimpleName(ctx.Id_RawValue))
      return getRequirement(KnownProtocolKind::RawRepresentable);

    // CaseIterable.AllCases
    if (name.isSimpleName(ctx.Id_AllCases))
      return getRequirement(KnownProtocolKind::CaseIterable);

    return nullptr;
  }

  return nullptr;
}

DeclRefExpr *
DerivedConformance::createSelfDeclRef(AbstractFunctionDecl *fn) {
  ASTContext &C = fn->getASTContext();

  auto selfDecl = fn->getImplicitSelfDecl();
  return new (C) DeclRefExpr(selfDecl, DeclNameLoc(), /*implicit*/true);
}

AccessorDecl *DerivedConformance::
addGetterToReadOnlyDerivedProperty(VarDecl *property,
                                   Type propertyContextType) {
  auto getter =
    declareDerivedPropertyGetter(property, propertyContextType);

  property->setImplInfo(StorageImplInfo::getImmutableComputed());
  property->setAccessors(SourceLoc(), {getter}, SourceLoc());

  return getter;
}

AccessorDecl *
DerivedConformance::declareDerivedPropertyGetter(VarDecl *property,
                                                 Type propertyContextType) {
  bool isStatic = property->isStatic();

  auto &C = property->getASTContext();
  auto parentDC = property->getDeclContext();
  ParameterList *params = ParameterList::createEmpty(C);

  Type propertyInterfaceType = property->getInterfaceType();
  
  auto getterDecl = AccessorDecl::create(C,
    /*FuncLoc=*/SourceLoc(), /*AccessorKeywordLoc=*/SourceLoc(),
    AccessorKind::Get, property,
    /*StaticLoc=*/SourceLoc(), StaticSpellingKind::None,
    /*Throws=*/false, /*ThrowsLoc=*/SourceLoc(),
    /*GenericParams=*/nullptr, params,
    TypeLoc::withoutLoc(propertyInterfaceType), parentDC);
  getterDecl->setImplicit();
  getterDecl->setStatic(isStatic);
  getterDecl->setIsTransparent(false);

  // Compute the interface type of the getter.
  if (auto env = parentDC->getGenericEnvironmentOfContext())
    getterDecl->setGenericEnvironment(env);
  getterDecl->computeType();

  getterDecl->copyFormalAccessFrom(property);
  getterDecl->setValidationToChecked();

  C.addSynthesizedDecl(getterDecl);

  return getterDecl;
}

std::pair<VarDecl *, PatternBindingDecl *>
DerivedConformance::declareDerivedProperty(Identifier name,
                                           Type propertyInterfaceType,
                                           Type propertyContextType,
                                           bool isStatic, bool isFinal) {
  auto &C = TC.Context;
  auto parentDC = getConformanceContext();

  VarDecl *propDecl = new (C) VarDecl(/*IsStatic*/isStatic, VarDecl::Introducer::Var,
                                      /*IsCaptureList*/false, SourceLoc(), name,
                                      parentDC);
  propDecl->setImplicit();
  propDecl->copyFormalAccessFrom(Nominal, /*sourceIsParentContext*/ true);
  propDecl->setInterfaceType(propertyInterfaceType);
  propDecl->setValidationToChecked();

  Pattern *propPat = new (C) NamedPattern(propDecl, /*implicit*/ true);
  propPat->setType(propertyContextType);

  propPat = TypedPattern::createImplicit(C, propPat, propertyContextType);
  propPat->setType(propertyContextType);

  auto *pbDecl = PatternBindingDecl::createImplicit(
      C, StaticSpellingKind::None, propPat, /*InitExpr*/ nullptr, parentDC);
  return {propDecl, pbDecl};
}

bool DerivedConformance::checkAndDiagnoseDisallowedContext(
    ValueDecl *synthesizing) const {
  // In general, conformances can't be synthesized in extensions across files;
  // but we have to allow it as a special case for Equatable and Hashable on
  // enums with no associated values to preserve source compatibility.
  bool allowCrossfileExtensions = false;
  if (Protocol->isSpecificProtocol(KnownProtocolKind::Equatable) ||
      Protocol->isSpecificProtocol(KnownProtocolKind::Hashable)) {
    auto ED = dyn_cast<EnumDecl>(Nominal);
    allowCrossfileExtensions = ED && ED->hasOnlyCasesWithoutAssociatedValues();
  }

  if (!allowCrossfileExtensions &&
      Nominal->getModuleScopeContext() !=
          getConformanceContext()->getModuleScopeContext()) {
    TC.diagnose(ConformanceDecl->getLoc(),
                diag::cannot_synthesize_in_crossfile_extension,
                getProtocolType());
    TC.diagnose(Nominal->getLoc(), diag::kind_declared_here,
                DescriptiveDeclKind::Type);
    return true;
  }

  // A non-final class can't have an protocol-witnesss initializer in an
  // extension.
  if (auto CD = dyn_cast<ClassDecl>(Nominal)) {
    if (!CD->isFinal() && isa<ConstructorDecl>(synthesizing) &&
        isa<ExtensionDecl>(ConformanceDecl)) {
      TC.diagnose(ConformanceDecl->getLoc(),
                  diag::cannot_synthesize_init_in_extension_of_nonfinal,
                  getProtocolType(), synthesizing->getFullName());
      return true;
    }
  }

  return false;
}

/// Build a type-checked integer literal.
static IntegerLiteralExpr *buildIntegerLiteral(ASTContext &C, unsigned index) {
  Type intType = C.getIntDecl()->getDeclaredType();

  auto literal = IntegerLiteralExpr::createFromUnsigned(C, index);
  literal->setType(intType);
  literal->setBuiltinInitializer(C.getIntBuiltinInitDecl(C.getIntDecl()));

  return literal;
}

/// Create AST statements which convert from an enum to an Int with a switch.
/// \p stmts The generated statements are appended to this vector.
/// \p parentDC Either an extension or the enum itself.
/// \p enumDecl The enum declaration.
/// \p enumVarDecl The enum input variable.
/// \p funcDecl The parent function.
/// \p indexName The name of the output variable.
/// \return A DeclRefExpr of the output variable (of type Int).
DeclRefExpr *swift::convertEnumToIndex(SmallVectorImpl<ASTNode> &stmts,
                                DeclContext *parentDC,
                                EnumDecl *enumDecl,
                                VarDecl *enumVarDecl,
                                AbstractFunctionDecl *funcDecl,
                                const char *indexName) {
  ASTContext &C = enumDecl->getASTContext();
  Type enumType = enumVarDecl->getType();
  Type intType = C.getIntDecl()->getDeclaredType();

  auto indexVar = new (C) VarDecl(/*IsStatic*/false, VarDecl::Introducer::Var,
                                  /*IsCaptureList*/false, SourceLoc(),
                                  C.getIdentifier(indexName),
                                  funcDecl);
  indexVar->setInterfaceType(intType);
  indexVar->setImplicit();

  // generate: var indexVar
  Pattern *indexPat = new (C) NamedPattern(indexVar, /*implicit*/ true);
  indexPat->setType(intType);
  indexPat = TypedPattern::createImplicit(C, indexPat, intType);
  indexPat->setType(intType);
  auto *indexBind = PatternBindingDecl::createImplicit(
      C, StaticSpellingKind::None, indexPat, /*InitExpr*/ nullptr, funcDecl);

  unsigned index = 0;
  SmallVector<ASTNode, 4> cases;
  for (auto elt : enumDecl->getAllElements()) {
    // generate: case .<Case>:
    auto pat = new (C) EnumElementPattern(TypeLoc::withoutLoc(enumType),
                                          SourceLoc(), SourceLoc(),
                                          Identifier(), elt, nullptr);
    pat->setImplicit();
    pat->setType(enumType);

    auto labelItem = CaseLabelItem(pat);

    // generate: indexVar = <index>
    auto indexExpr = buildIntegerLiteral(C, index++);

    auto indexRef = new (C) DeclRefExpr(indexVar, DeclNameLoc(),
                                        /*implicit*/true,
                                        AccessSemantics::Ordinary,
                                        LValueType::get(intType));
    auto assignExpr = new (C) AssignExpr(indexRef, SourceLoc(),
                                         indexExpr, /*implicit*/ true);
    assignExpr->setType(TupleType::getEmpty(C));
    auto body = BraceStmt::create(C, SourceLoc(), ASTNode(assignExpr),
                                  SourceLoc());
    cases.push_back(CaseStmt::create(C, SourceLoc(), labelItem, SourceLoc(),
                                     SourceLoc(), body,
                                     /*case body vardecls*/ None));
  }

  // generate: switch enumVar { }
  auto enumRef = new (C) DeclRefExpr(enumVarDecl, DeclNameLoc(),
                                     /*implicit*/true,
                                     AccessSemantics::Ordinary,
                                     enumVarDecl->getType());
  auto switchStmt = SwitchStmt::create(LabeledStmtInfo(), SourceLoc(), enumRef,
                                       SourceLoc(), cases, SourceLoc(), C);

  stmts.push_back(indexBind);
  stmts.push_back(switchStmt);

  return new (C) DeclRefExpr(indexVar, DeclNameLoc(), /*implicit*/ true,
                             AccessSemantics::Ordinary, intType);
}

/// Returns the ParamDecl for each associated value of the given enum whose type
/// does not conform to a protocol
/// \p theEnum The enum whose elements and associated values should be checked.
/// \p protocol The protocol being requested.
/// \return The ParamDecl of each associated value whose type does not conform.
SmallVector<ParamDecl *, 3>
swift::associatedValuesNotConformingToProtocol(DeclContext *DC, EnumDecl *theEnum,
                                        ProtocolDecl *protocol) {
  auto lazyResolver = DC->getASTContext().getLazyResolver();
  SmallVector<ParamDecl *, 3> nonconformingAssociatedValues;
  for (auto elt : theEnum->getAllElements()) {
    if (!elt->hasInterfaceType())
      lazyResolver->resolveDeclSignature(elt);

    auto PL = elt->getParameterList();
    if (!PL)
      continue;

    for (auto param : *PL) {
      auto type = param->getInterfaceType();
      if (!TypeChecker::conformsToProtocol(DC->mapTypeIntoContext(type),
                                           protocol, DC, None)) {
        nonconformingAssociatedValues.push_back(param);
      }
    }
  }
  return nonconformingAssociatedValues;
}

/// Returns true if, for every element of the given enum, it either has no
/// associated values or all of them conform to a protocol.
/// \p theEnum The enum whose elements and associated values should be checked.
/// \p protocol The protocol being requested.
/// \return True if all associated values of all elements of the enum conform.
bool swift::allAssociatedValuesConformToProtocol(DeclContext *DC,
                                                 EnumDecl *theEnum,
                                                 ProtocolDecl *protocol) {
  return associatedValuesNotConformingToProtocol(DC, theEnum, protocol).empty();
}

/// Returns the pattern used to match and bind the associated values (if any) of
/// an enum case.
/// \p enumElementDecl The enum element to match.
/// \p varPrefix The prefix character for variable names (e.g., a0, a1, ...).
/// \p varContext The context into which payload variables should be declared.
/// \p boundVars The array to which the pattern's variables will be appended.
Pattern*
swift::enumElementPayloadSubpattern(EnumElementDecl *enumElementDecl,
                             char varPrefix, DeclContext *varContext,
                             SmallVectorImpl<VarDecl*> &boundVars) {
  auto parentDC = enumElementDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  // No arguments, so no subpattern to match.
  if (!enumElementDecl->hasAssociatedValues())
    return nullptr;

  auto argumentType = enumElementDecl->getArgumentInterfaceType();
  if (auto tupleType = argumentType->getAs<TupleType>()) {
    // Either multiple (labeled or unlabeled) arguments, or one labeled
    // argument. Return a tuple pattern that matches the enum element in arity,
    // types, and labels. For example:
    // case a(x: Int) => (x: let a0)
    // case b(Int, String) => (let a0, let a1)
    SmallVector<TuplePatternElt, 3> elementPatterns;
    int index = 0;
    for (auto tupleElement : tupleType->getElements()) {
      auto payloadVar = indexedVarDecl(varPrefix, index++,
                                       tupleElement.getType(), varContext);
      boundVars.push_back(payloadVar);

      auto namedPattern = new (C) NamedPattern(payloadVar);
      namedPattern->setImplicit();
      auto letPattern = new (C) VarPattern(SourceLoc(), /*isLet*/ true,
                                           namedPattern);
      elementPatterns.push_back(TuplePatternElt(tupleElement.getName(),
                                                SourceLoc(), letPattern));
    }

    auto pat = TuplePattern::create(C, SourceLoc(), elementPatterns,
                                    SourceLoc());
    pat->setImplicit();
    return pat;
  }

  // Otherwise, a one-argument unlabeled payload. Return a paren pattern whose
  // underlying type is the same as the payload. For example:
  // case a(Int) => (let a0)
  auto underlyingType = argumentType->getWithoutParens();
  auto payloadVar = indexedVarDecl(varPrefix, 0, underlyingType, varContext);
  boundVars.push_back(payloadVar);

  auto namedPattern = new (C) NamedPattern(payloadVar);
  namedPattern->setImplicit();
  auto letPattern = new (C) VarPattern(SourceLoc(), /*isLet*/ true,
                                       namedPattern);
  auto pat = new (C) ParenPattern(SourceLoc(), letPattern, SourceLoc());
  pat->setImplicit();
  return pat;
}


/// Creates a named variable based on a prefix character and a numeric index.
/// \p prefixChar The prefix character for the variable's name.
/// \p index The numeric index to append to the variable's name.
/// \p type The type of the variable.
/// \p varContext The context of the variable.
/// \return A VarDecl named with the prefix and number.
VarDecl *swift::indexedVarDecl(char prefixChar, int index, Type type,
                               DeclContext *varContext) {
  ASTContext &C = varContext->getASTContext();

  llvm::SmallString<8> indexVal;
  indexVal.append(1, prefixChar);
  APInt(32, index).toString(indexVal, 10, /*signed*/ false);
  auto indexStr = C.AllocateCopy(indexVal);
  auto indexStrRef = StringRef(indexStr.data(), indexStr.size());

  auto varDecl = new (C) VarDecl(/*IsStatic*/false, VarDecl::Introducer::Let,
                                 /*IsCaptureList*/true, SourceLoc(),
                                 C.getIdentifier(indexStrRef),
                                 varContext);
  varDecl->setType(type);
  varDecl->setHasNonPatternBindingInit(true);
  return varDecl;
}
