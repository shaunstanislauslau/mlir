//===- AsmPrinter.cpp - MLIR Assembly Printer Implementation --------------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// This file implements the MLIR AsmPrinter class, which is used to implement
// the various print() methods on the core IR objects.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/CFGFunction.h"
#include "mlir/IR/MLFunction.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/OperationSet.h"
#include "mlir/IR/Statements.h"
#include "mlir/IR/Types.h"
#include "mlir/Support/STLExtras.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
using namespace mlir;

void Identifier::print(raw_ostream &os) const { os << str(); }

void Identifier::dump() const { print(llvm::errs()); }

//===----------------------------------------------------------------------===//
// ModuleState
//===----------------------------------------------------------------------===//

namespace {
class ModuleState {
public:
  /// This is the operation set for the current context if it is knowable (a
  /// context could be determined), otherwise this is null.
  OperationSet *const operationSet;

  explicit ModuleState(MLIRContext *context)
      : operationSet(context ? &OperationSet::get(context) : nullptr) {}

  // Initializes module state, populating affine map state.
  void initialize(const Module *module);

  int getAffineMapId(const AffineMap *affineMap) const {
    auto it = affineMapIds.find(affineMap);
    if (it == affineMapIds.end()) {
      return -1;
    }
    return it->second;
  }

  const DenseMap<const AffineMap *, int> &getAffineMapIds() const {
    return affineMapIds;
  }

private:
  void recordAffineMapReference(const AffineMap *affineMap) {
    if (affineMapIds.count(affineMap) == 0) {
      affineMapIds[affineMap] = nextAffineMapId++;
    }
  }

  // Visit functions.
  void visitFunction(const Function *fn);
  void visitExtFunction(const ExtFunction *fn);
  void visitCFGFunction(const CFGFunction *fn);
  void visitMLFunction(const MLFunction *fn);
  void visitType(const Type *type);
  void visitAttribute(const Attribute *attr);
  void visitOperation(const Operation *op);

  DenseMap<const AffineMap *, int> affineMapIds;
  int nextAffineMapId = 0;
};
} // end anonymous namespace

// TODO Support visiting other types/instructions when implemented.
void ModuleState::visitType(const Type *type) {
  if (type->getKind() == Type::Kind::Function) {
    // Visit input and result types for functions.
    auto *funcType = cast<FunctionType>(type);
    for (auto *input : funcType->getInputs()) {
      visitType(input);
    }
    for (auto *result : funcType->getResults()) {
      visitType(result);
    }
  } else if (type->getKind() == Type::Kind::MemRef) {
    // Visit affine maps in memref type.
    auto *memref = cast<MemRefType>(type);
    for (AffineMap *map : memref->getAffineMaps()) {
      recordAffineMapReference(map);
    }
  }
}

void ModuleState::visitAttribute(const Attribute *attr) {
  if (isa<AffineMapAttr>(attr)) {
    recordAffineMapReference(cast<AffineMapAttr>(attr)->getValue());
  } else if (isa<ArrayAttr>(attr)) {
    for (auto elt : cast<ArrayAttr>(attr)->getValue()) {
      visitAttribute(elt);
    }
  }
}

void ModuleState::visitOperation(const Operation *op) {
  for (auto elt : op->getAttrs()) {
    visitAttribute(elt.second);
  }
}

void ModuleState::visitExtFunction(const ExtFunction *fn) {
  visitType(fn->getType());
}

void ModuleState::visitCFGFunction(const CFGFunction *fn) {
  visitType(fn->getType());
  for (auto &block : *fn) {
    for (auto &op : block.getOperations()) {
      visitOperation(&op);
    }
  }
}

void ModuleState::visitMLFunction(const MLFunction *fn) {
  visitType(fn->getType());
  // TODO Visit function body statements (and attributes if required).
}

void ModuleState::visitFunction(const Function *fn) {
  switch (fn->getKind()) {
  case Function::Kind::ExtFunc:
    return visitExtFunction(cast<ExtFunction>(fn));
  case Function::Kind::CFGFunc:
    return visitCFGFunction(cast<CFGFunction>(fn));
  case Function::Kind::MLFunc:
    return visitMLFunction(cast<MLFunction>(fn));
  }
}

// Initializes module state, populating affine map state.
void ModuleState::initialize(const Module *module) {
  for (auto fn : module->functionList) {
    visitFunction(fn);
  }
}

//===----------------------------------------------------------------------===//
// ModulePrinter
//===----------------------------------------------------------------------===//

namespace {
class ModulePrinter {
public:
  ModulePrinter(raw_ostream &os, ModuleState &state) : os(os), state(state) {}
  explicit ModulePrinter(const ModulePrinter &printer)
      : os(printer.os), state(printer.state) {}

  template <typename Container, typename UnaryFunctor>
  inline void interleaveComma(const Container &c, UnaryFunctor each_fn) const {
    interleave(c.begin(), c.end(), each_fn, [&]() { os << ", "; });
  }

  void print(const Module *module);
  void print(const Attribute *attr) const;
  void print(const Type *type) const;
  void print(const Function *fn);
  void print(const ExtFunction *fn);
  void print(const CFGFunction *fn);
  void print(const MLFunction *fn);

  void print(const AffineMap *map);
  void print(const AffineExpr *expr) const;

protected:
  raw_ostream &os;
  ModuleState &state;

  void printFunctionSignature(const Function *fn);
  void printAffineMapId(int affineMapId) const;
  void printAffineMapReference(const AffineMap *affineMap) const;

  void print(const AffineBinaryOpExpr *expr) const;
};
} // end anonymous namespace

// Prints function with initialized module state.
void ModulePrinter::print(const Function *fn) {
  switch (fn->getKind()) {
  case Function::Kind::ExtFunc:
    return print(cast<ExtFunction>(fn));
  case Function::Kind::CFGFunc:
    return print(cast<CFGFunction>(fn));
  case Function::Kind::MLFunc:
    return print(cast<MLFunction>(fn));
  }
}

// Prints affine map identifier.
void ModulePrinter::printAffineMapId(int affineMapId) const {
  os << "#map" << affineMapId;
}

void ModulePrinter::printAffineMapReference(const AffineMap *affineMap) const {
  int mapId = state.getAffineMapId(affineMap);
  if (mapId >= 0) {
    // Map will be printed at top of module so print reference to its id.
    printAffineMapId(mapId);
  } else {
    // Map not in module state so print inline.
    affineMap->print(os);
  }
}

void ModulePrinter::print(const Module *module) {
  for (const auto &mapAndId : state.getAffineMapIds()) {
    printAffineMapId(mapAndId.second);
    os << " = ";
    mapAndId.first->print(os);
    os << '\n';
  }
  for (auto *fn : module->functionList)
    print(fn);
}

void ModulePrinter::print(const Attribute *attr) const {
  switch (attr->getKind()) {
  case Attribute::Kind::Bool:
    os << (cast<BoolAttr>(attr)->getValue() ? "true" : "false");
    break;
  case Attribute::Kind::Integer:
    os << cast<IntegerAttr>(attr)->getValue();
    break;
  case Attribute::Kind::Float:
    // FIXME: this isn't precise, we should print with a hex format.
    os << cast<FloatAttr>(attr)->getValue();
    break;
  case Attribute::Kind::String:
    // FIXME: should escape the string.
    os << '"' << cast<StringAttr>(attr)->getValue() << '"';
    break;
  case Attribute::Kind::Array: {
    auto elts = cast<ArrayAttr>(attr)->getValue();
    os << '[';
    interleaveComma(elts, [&](Attribute *attr) { print(attr); });
    os << ']';
    break;
  }
  case Attribute::Kind::AffineMap:
    printAffineMapReference(cast<AffineMapAttr>(attr)->getValue());
    break;
  }
}

void ModulePrinter::print(const Type *type) const {
  switch (type->getKind()) {
  case Type::Kind::AffineInt:
    os << "affineint";
    return;
  case Type::Kind::BF16:
    os << "bf16";
    return;
  case Type::Kind::F16:
    os << "f16";
    return;
  case Type::Kind::F32:
    os << "f32";
    return;
  case Type::Kind::F64:
    os << "f64";
    return;

  case Type::Kind::Integer: {
    auto *integer = cast<IntegerType>(type);
    os << 'i' << integer->getWidth();
    return;
  }
  case Type::Kind::Function: {
    auto *func = cast<FunctionType>(type);
    os << '(';
    interleaveComma(func->getInputs(), [&](Type *type) { os << *type; });
    os << ") -> ";
    auto results = func->getResults();
    if (results.size() == 1)
      os << *results[0];
    else {
      os << '(';
      interleaveComma(results, [&](Type *type) { os << *type; });
      os << ')';
    }
    return;
  }
  case Type::Kind::Vector: {
    auto *v = cast<VectorType>(type);
    os << "vector<";
    for (auto dim : v->getShape())
      os << dim << 'x';
    os << *v->getElementType() << '>';
    return;
  }
  case Type::Kind::RankedTensor: {
    auto *v = cast<RankedTensorType>(type);
    os << "tensor<";
    for (auto dim : v->getShape()) {
      if (dim < 0)
        os << '?';
      else
        os << dim;
      os << 'x';
    }
    os << *v->getElementType() << '>';
    return;
  }
  case Type::Kind::UnrankedTensor: {
    auto *v = cast<UnrankedTensorType>(type);
    os << "tensor<??" << *v->getElementType() << '>';
    return;
  }
  case Type::Kind::MemRef: {
    auto *v = cast<MemRefType>(type);
    os << "memref<";
    for (auto dim : v->getShape()) {
      if (dim < 0)
        os << '?';
      else
        os << dim;
      os << 'x';
    }
    os << *v->getElementType();
    for (auto map : v->getAffineMaps()) {
      os << ", ";
      printAffineMapReference(map);
    }
    os << ", " << v->getMemorySpace();
    os << '>';
    return;
  }
  }
}

//===----------------------------------------------------------------------===//
// Affine expressions and maps
//===----------------------------------------------------------------------===//

void ModulePrinter::print(const AffineExpr *expr) const {
  switch (expr->getKind()) {
  case AffineExpr::Kind::SymbolId:
    os << 's' << cast<AffineSymbolExpr>(expr)->getPosition();
    return;
  case AffineExpr::Kind::DimId:
    os << 'd' << cast<AffineDimExpr>(expr)->getPosition();
    return;
  case AffineExpr::Kind::Constant:
    os << cast<AffineConstantExpr>(expr)->getValue();
    return;
  case AffineExpr::Kind::Add:
  case AffineExpr::Kind::Mul:
  case AffineExpr::Kind::FloorDiv:
  case AffineExpr::Kind::CeilDiv:
  case AffineExpr::Kind::Mod:
    return print(cast<AffineBinaryOpExpr>(expr));
  }
}

void ModulePrinter::print(const AffineBinaryOpExpr *expr) const {
  if (expr->getKind() != AffineExpr::Kind::Add) {
    os << '(';
    print(expr->getLHS());
    switch (expr->getKind()) {
    case AffineExpr::Kind::Mul:
      os << " * ";
      break;
    case AffineExpr::Kind::FloorDiv:
      os << " floordiv ";
      break;
    case AffineExpr::Kind::CeilDiv:
      os << " ceildiv ";
      break;
    case AffineExpr::Kind::Mod:
      os << " mod ";
      break;
    default:
      llvm_unreachable("unexpected affine binary op expression");
    }

    print(expr->getRHS());
    os << ')';
    return;
  }

  // Print out special "pretty" forms for add.
  os << '(';
  print(expr->getLHS());

  // Pretty print addition to a product that has a negative operand as a
  // subtraction.
  if (auto *rhs = dyn_cast<AffineBinaryOpExpr>(expr->getRHS())) {
    if (rhs->getKind() == AffineExpr::Kind::Mul) {
      if (auto *rrhs = dyn_cast<AffineConstantExpr>(rhs->getRHS())) {
        if (rrhs->getValue() < 0) {
          os << " - (";
          print(rhs->getLHS());
          os << " * " << -rrhs->getValue() << "))";
          return;
        }
      }
    }
  }

  // Pretty print addition to a negative number as a subtraction.
  if (auto *rhs = dyn_cast<AffineConstantExpr>(expr->getRHS())) {
    if (rhs->getValue() < 0) {
      os << " - " << -rhs->getValue() << ")";
      return;
    }
  }

  os << " + ";
  print(expr->getRHS());
  os << ')';
}

void ModulePrinter::print(const AffineMap *map) {
  // Dimension identifiers.
  os << '(';
  for (int i = 0; i < (int)map->getNumDims() - 1; i++)
    os << "d" << i << ", ";
  if (map->getNumDims() >= 1)
    os << "d" << map->getNumDims() - 1;
  os << ")";

  // Symbolic identifiers.
  if (map->getNumSymbols() >= 1) {
    os << " [";
    for (int i = 0; i < (int)map->getNumSymbols() - 1; i++)
      os << "s" << i << ", ";
    if (map->getNumSymbols() >= 1)
      os << "s" << map->getNumSymbols() - 1;
    os << "]";
  }

  // AffineMap should have at least one result.
  assert(!map->getResults().empty());
  // Result affine expressions.
  os << " -> (";
  interleaveComma(map->getResults(), [&](AffineExpr *expr) { print(expr); });
  os << ")";

  if (!map->isBounded()) {
    return;
  }

  // Print range sizes for bounded affine maps.
  os << " size (";
  interleaveComma(map->getRangeSizes(), [&](AffineExpr *expr) { print(expr); });
  os << ")";
}

//===----------------------------------------------------------------------===//
// Function printing
//===----------------------------------------------------------------------===//

void ModulePrinter::printFunctionSignature(const Function *fn) {
  auto type = fn->getType();

  os << "@" << fn->getName() << '(';
  interleaveComma(type->getInputs(), [&](Type *eltType) { print(eltType); });
  os << ')';

  switch (type->getResults().size()) {
  case 0:
    break;
  case 1:
    os << " -> ";
    print(type->getResults()[0]);
    break;
  default:
    os << " -> (";
    interleaveComma(type->getResults(), [&](Type *eltType) { print(eltType); });
    os << ')';
    break;
  }
}

void ModulePrinter::print(const ExtFunction *fn) {
  os << "extfunc ";
  printFunctionSignature(fn);
  os << '\n';
}

namespace {

// FunctionState contains common functionality for printing
// CFG and ML functions.
class FunctionState : public ModulePrinter {
public:
  FunctionState(const ModulePrinter &other) : ModulePrinter(other) {}

  void printOperation(const Operation *op);

protected:
  void numberValueID(const SSAValue *value) {
    assert(!valueIDs.count(value) && "Value numbered multiple times");
    valueIDs[value] = nextValueID++;
  }

  void printValueID(const SSAValue *value,
                    bool dontPrintResultNo = false) const {
    int resultNo = -1;
    auto lookupValue = value;

    // If this is a reference to the result of a multi-result instruction, print
    // out the # identifier and make sure to map our lookup to the first result
    // of the instruction.
    if (auto *result = dyn_cast<InstResult>(value)) {
      if (result->getOwner()->getNumResults() != 1) {
        resultNo = result->getResultNumber();
        lookupValue = result->getOwner()->getResult(0);
      }
    }

    auto it = valueIDs.find(lookupValue);
    if (it == valueIDs.end()) {
      os << "<<INVALID SSA VALUE>>";
      return;
    }

    os << '%' << it->getSecond();
    if (resultNo != -1 && !dontPrintResultNo)
      os << '#' << resultNo;
  }

private:
  /// This is the value ID for each SSA value in the current function.
  DenseMap<const SSAValue *, unsigned> valueIDs;
  unsigned nextValueID = 0;
};
} // end anonymous namespace

void FunctionState::printOperation(const Operation *op) {
  os << "  ";

  if (op->getNumResults()) {
    printValueID(op->getResult(0), /*dontPrintResultNo*/ true);
    os << " = ";
  }

  // Check to see if this is a known operation.  If so, use the registered
  // custom printer hook.
  if (auto opInfo = state.operationSet->lookup(op->getName().str())) {
    opInfo->printAssembly(op, os);
    return;
  }

  // Otherwise use the standard verbose printing approach.

  // TODO: escape name if necessary.
  os << "\"" << op->getName().str() << "\"(";

  interleaveComma(op->getOperands(),
                  [&](const SSAValue *value) { printValueID(value); });

  os << ')';
  auto attrs = op->getAttrs();
  if (!attrs.empty()) {
    os << '{';
    interleaveComma(attrs, [&](NamedAttribute attr) {
      os << attr.first << ": ";
      print(attr.second);
    });
    os << '}';
  }

  // Print the type signature of the operation.
  os << " : (";
  interleaveComma(op->getOperands(),
                  [&](const SSAValue *value) { print(value->getType()); });
  os << ") -> ";

  if (op->getNumResults() == 1) {
    print(op->getResult(0)->getType());
  } else {
    os << '(';
    interleaveComma(op->getResults(),
                    [&](const SSAValue *result) { print(result->getType()); });
    os << ')';
  }
}

//===----------------------------------------------------------------------===//
// CFG Function printing
//===----------------------------------------------------------------------===//

namespace {
class CFGFunctionPrinter : public FunctionState {
public:
  CFGFunctionPrinter(const CFGFunction *function, const ModulePrinter &other);

  const CFGFunction *getFunction() const { return function; }

  void print();
  void print(const BasicBlock *block);

  void print(const Instruction *inst);
  void print(const OperationInst *inst);
  void print(const ReturnInst *inst);
  void print(const BranchInst *inst);

  unsigned getBBID(const BasicBlock *block) {
    auto it = basicBlockIDs.find(block);
    assert(it != basicBlockIDs.end() && "Block not in this function?");
    return it->second;
  }

private:
  const CFGFunction *function;
  DenseMap<const BasicBlock *, unsigned> basicBlockIDs;

  void numberValuesInBlock(const BasicBlock *block);
};
} // end anonymous namespace

CFGFunctionPrinter::CFGFunctionPrinter(const CFGFunction *function,
                                       const ModulePrinter &other)
    : FunctionState(other), function(function) {
  // Each basic block gets a unique ID per function.
  unsigned blockID = 0;
  for (auto &block : *function) {
    basicBlockIDs[&block] = blockID++;
    numberValuesInBlock(&block);
  }
}

/// Number all of the SSA values in the specified basic block.
void CFGFunctionPrinter::numberValuesInBlock(const BasicBlock *block) {
  for (auto *arg : block->getArguments()) {
    numberValueID(arg);
  }
  for (auto &op : *block) {
    // We number instruction that have results, and we only number the first
    // result.
    if (op.getNumResults() != 0)
      numberValueID(op.getResult(0));
  }

  // Terminators do not define values.
}

void CFGFunctionPrinter::print() {
  os << "cfgfunc ";
  printFunctionSignature(getFunction());
  os << " {\n";

  for (auto &block : *function)
    print(&block);
  os << "}\n\n";
}

void CFGFunctionPrinter::print(const BasicBlock *block) {
  os << "bb" << getBBID(block);

  if (!block->args_empty()) {
    os << '(';
    interleaveComma(block->getArguments(), [&](const BBArgument *arg) {
      printValueID(arg);
      os << ": ";
      ModulePrinter::print(arg->getType());
    });
    os << ')';
  }
  os << ":\n";

  for (auto &inst : block->getOperations()) {
    print(&inst);
    os << '\n';
  }

  print(block->getTerminator());
  os << '\n';
}

void CFGFunctionPrinter::print(const Instruction *inst) {
  switch (inst->getKind()) {
  case Instruction::Kind::Operation:
    return print(cast<OperationInst>(inst));
  case TerminatorInst::Kind::Branch:
    return print(cast<BranchInst>(inst));
  case TerminatorInst::Kind::Return:
    return print(cast<ReturnInst>(inst));
  }
}

void CFGFunctionPrinter::print(const OperationInst *inst) {
  printOperation(inst);
}

void CFGFunctionPrinter::print(const BranchInst *inst) {
  os << "  br bb" << getBBID(inst->getDest());

  if (inst->getNumOperands() != 0) {
    os << '(';
    // TODO: Use getOperands() when we have it.
    interleaveComma(inst->getInstOperands(), [&](const InstOperand &operand) {
      printValueID(operand.get());
    });
    os << ") : ";
    interleaveComma(inst->getInstOperands(), [&](const InstOperand &operand) {
      ModulePrinter::print(operand.get()->getType());
    });
  }
}

void CFGFunctionPrinter::print(const ReturnInst *inst) {
  os << "  return";

  if (inst->getNumOperands() != 0)
    os << ' ';

  interleaveComma(inst->getOperands(), [&](const CFGValue *operand) {
    printValueID(operand);
    os << " : ";
    ModulePrinter::print(operand->getType());
  });
}

void ModulePrinter::print(const CFGFunction *fn) {
  CFGFunctionPrinter(fn, *this).print();
}

//===----------------------------------------------------------------------===//
// ML Function printing
//===----------------------------------------------------------------------===//

namespace {
class MLFunctionPrinter : public FunctionState {
public:
  MLFunctionPrinter(const MLFunction *function, const ModulePrinter &other);

  const MLFunction *getFunction() const { return function; }

  // Prints ML function
  void print();

  // Methods to print ML function statements
  void print(const Statement *stmt);
  void print(const OperationStmt *stmt);
  void print(const ForStmt *stmt);
  void print(const IfStmt *stmt);
  void print(const StmtBlock *block);

  // Number of spaces used for indenting nested statements
  const static unsigned indentWidth = 2;

private:
  const MLFunction *function;
  int numSpaces;
};
} // end anonymous namespace

MLFunctionPrinter::MLFunctionPrinter(const MLFunction *function,
                                     const ModulePrinter &other)
    : FunctionState(other), function(function), numSpaces(0) {}

void MLFunctionPrinter::print() {
  os << "mlfunc ";
  // FIXME: should print argument names rather than just signature
  printFunctionSignature(function);
  os << " {\n";
  print(function);
  os << "  return\n";
  os << "}\n\n";
}

void MLFunctionPrinter::print(const StmtBlock *block) {
  numSpaces += indentWidth;
  for (auto &stmt : block->getStatements()) {
    print(&stmt);
    os << "\n";
  }
  numSpaces -= indentWidth;
}

void MLFunctionPrinter::print(const Statement *stmt) {
  switch (stmt->getKind()) {
  case Statement::Kind::Operation:
    return print(cast<OperationStmt>(stmt));
  case Statement::Kind::For:
    return print(cast<ForStmt>(stmt));
  case Statement::Kind::If:
    return print(cast<IfStmt>(stmt));
  }
}

void MLFunctionPrinter::print(const OperationStmt *stmt) {
  printOperation(stmt);
}

void MLFunctionPrinter::print(const ForStmt *stmt) {
  os.indent(numSpaces) << "for x = " << *stmt->getLowerBound();
  os << " to " << *stmt->getUpperBound();
  if (stmt->getStep()->getValue() != 1)
    os << " step " << *stmt->getStep();

  os << " {\n";
  print(static_cast<const StmtBlock *>(stmt));
  os.indent(numSpaces) << "}";
}

void MLFunctionPrinter::print(const IfStmt *stmt) {
  os.indent(numSpaces) << "if () {\n";
  print(stmt->getThenClause());
  os.indent(numSpaces) << "}";
  if (stmt->hasElseClause()) {
    os << " else {\n";
    print(stmt->getElseClause());
    os.indent(numSpaces) << "}";
  }
}

void ModulePrinter::print(const MLFunction *fn) {
  MLFunctionPrinter(fn, *this).print();
}

//===----------------------------------------------------------------------===//
// print and dump methods
//===----------------------------------------------------------------------===//

void Attribute::print(raw_ostream &os) const {
  ModuleState state(/*no context is known*/ nullptr);
  ModulePrinter(os, state).print(this);
}

void Attribute::dump() const { print(llvm::errs()); }

void Type::print(raw_ostream &os) const {
  ModuleState state(getContext());
  ModulePrinter(os, state).print(this);
}

void Type::dump() const { print(llvm::errs()); }

void AffineMap::dump() const {
  print(llvm::errs());
  llvm::errs() << "\n";
}

void AffineExpr::dump() const {
  print(llvm::errs());
  llvm::errs() << "\n";
}

void AffineExpr::print(raw_ostream &os) const {
  ModuleState state(/*no context is known*/ nullptr);
  ModulePrinter(os, state).print(this);
}

void AffineMap::print(raw_ostream &os) const {
  ModuleState state(/*no context is known*/ nullptr);
  ModulePrinter(os, state).print(this);
}

void Instruction::print(raw_ostream &os) const {
  ModuleState state(getFunction()->getContext());
  ModulePrinter modulePrinter(os, state);
  CFGFunctionPrinter(getFunction(), modulePrinter).print(this);
}

void Instruction::dump() const {
  print(llvm::errs());
  llvm::errs() << "\n";
}

void BasicBlock::print(raw_ostream &os) const {
  ModuleState state(getFunction()->getContext());
  ModulePrinter modulePrinter(os, state);
  CFGFunctionPrinter(getFunction(), modulePrinter).print(this);
}

void BasicBlock::dump() const { print(llvm::errs()); }

void Statement::print(raw_ostream &os) const {
  ModuleState state(getFunction()->getContext());
  ModulePrinter modulePrinter(os, state);
  MLFunctionPrinter(getFunction(), modulePrinter).print(this);
}

void Statement::dump() const { print(llvm::errs()); }

void Function::print(raw_ostream &os) const {
  ModuleState state(getContext());
  ModulePrinter(os, state).print(this);
}

void Function::dump() const { print(llvm::errs()); }

void Module::print(raw_ostream &os) const {
  ModuleState state(getContext());
  state.initialize(this);
  ModulePrinter(os, state).print(this);
}

void Module::dump() const { print(llvm::errs()); }