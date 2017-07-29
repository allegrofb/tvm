/*!
 *  Copyright (c) 2017 by Contributors
 *  Lower TVM related buildin intrinsics such as packed call.
 * \file lower_tvm_buildin.cc
 */
#include <tvm/ir.h>
#include <tvm/ir_mutator.h>
#include <tvm/ir_pass.h>
#include <tvm/target_info.h>
#include <unordered_set>
#include "./ir_util.h"
#include "../arithmetic/compute_expr.h"
#include "../runtime/thread_storage_scope.h"
namespace tvm {
namespace ir {

using runtime::StorageScope;

inline Expr ConstInt32(size_t index) {
  CHECK_LE(index, std::numeric_limits<int>::max());
  return make_const(Int(32), static_cast<int>(index));
}

inline Expr StackAlloca(std::string type, size_t num) {
  Array<Expr> args = {StringImm::make(type), ConstInt32(num)};
  return Call::make(Handle(), intrinsic::tvm_stack_alloca, args, Call::Intrinsic);
}

// Calculate the statistics of packed function.
// These information are needed during codegen.
class BuiltinLower : public IRMutator {
 public:
  Stmt Build(Stmt stmt) {
    stack_shape_ = Var("stack_shape", Handle());
    stack_array_ = Var("stack_array", Handle());
    stack_value_ = Var("stack_value", Handle());
    stack_tcode_ = Var("stack_tcode", Handle());
    stmt = this->Mutate(stmt);
    if (max_shape_stack_ != 0) {
      stmt = LetStmt::make(
          stack_shape_, StackAlloca("shape", max_shape_stack_), stmt);
    }
    if (max_array_stack_ != 0) {
      stmt = LetStmt::make(
          stack_array_, StackAlloca("array", max_array_stack_), stmt);
    }
    if (max_arg_stack_ != 0) {
      stmt = LetStmt::make(
          stack_value_, StackAlloca("arg_value", max_arg_stack_), stmt);
      stmt = LetStmt::make(
          stack_tcode_, StackAlloca("arg_tcode", max_arg_stack_), stmt);
    }
    return stmt;
  }

  Stmt Mutate(Stmt stmt) final {
    stmt = IRMutator::Mutate(stmt);
    CHECK_EQ(run_shape_stack_, 0);
    CHECK_EQ(run_array_stack_, 0);
    CHECK_EQ(run_arg_stack_, 0);
    while (prep_seq_.size() != 0) {
      stmt = Block::make(prep_seq_.back(), stmt);
      prep_seq_.pop_back();
    }
    return stmt;
  }

  Stmt Mutate_(const Allocate* op, const Stmt& s) {
    // Lower allocate to device allocate when needed.
    Stmt stmt = IRMutator::Mutate_(op, s);
    op = stmt.as<Allocate>();
    // For special memory, remove allocate.
    auto it = storage_info_.find(op->buffer_var.get());
    if (it != storage_info_.end() && it->second.scope.tag.length() != 0) {
      ++it->second.alloc_count;
      CHECK_LE(it->second.alloc_count, 1)
          << "Double allocation of " << it->second.scope.to_string();
      return op->body;
    }
    // Get constant allocation bound.
    int64_t dev_type;
    int64_t nbytes = GetVectorBytes(op->type);
    if (device_type_.defined()) {
      if (arith::GetConst(device_type_, &dev_type)) {
        if (dev_type == kCPU) {
          int32_t constant_size = op->constant_allocation_size();
          if (constant_size > 0 && constant_size * nbytes < runtime::kMaxStackAlloca) {
            return stmt;
          }
        }
      }
    }
    Expr total_bytes = make_const(op->extents[0].type(), nbytes);
    for (size_t i = 0; i < op->extents.size(); ++i) {
      total_bytes = total_bytes * op->extents[i];
    }
    CHECK(device_type_.defined()) << "Unknown device type in current IR";
    CHECK(device_id_.defined()) << "Unknown device id in current IR";
    Stmt throw_last_error = Evaluate::make(Call::make(Int(32),
                                           intrinsic::tvm_throw_last_error, {},
                                           Call::Intrinsic));

    Stmt body = Block::make(
        IfThenElse::make(Call::make(Bool(1),
                                    intrinsic::tvm_handle_is_null,
                                    {op->buffer_var}, Call::PureIntrinsic),
                         throw_last_error),
        op->body);
    Stmt alloca = LetStmt::make(op->buffer_var,
                                Call::make(op->buffer_var.type(),
                                           "TVMBackendAllocWorkspace",
                                           {cast(Int(32), device_type_),
                                                 cast(Int(32), device_id_),
                                                 cast(UInt(64), total_bytes)},
                                           Call::Extern),
                                body);

    Expr free_op = Call::make(Int(32),
                              "TVMBackendFreeWorkspace",
                              {cast(Int(32), device_type_),
                                    cast(Int(32), device_id_),
                                    op->buffer_var},
                              Call::Extern);
    Stmt free_stmt = IfThenElse::make(free_op != make_zero(Int(32)), throw_last_error);
    body = Block::make(alloca, free_stmt);
    body = AttrStmt::make(
        op->buffer_var, attr::storage_alignment,
        make_const(Int(32), runtime::kTempAllocaAlignment),
        body);
    return body;
  }

  Stmt Mutate_(const AttrStmt* op, const Stmt &s) final {
    if (op->attr_key == attr::device_context_id) {
      CHECK(!device_id_.defined());
      device_id_ = op->value;
      return Mutate(op->body);
    } else if (op->attr_key == attr::device_context_type) {
      CHECK(!device_type_.defined());
      device_type_ = op->value;
      return Mutate(op->body);
    } else if (op->attr_key == attr::storage_scope) {
      const Variable* buf = op->node.as<Variable>();
      StorageScope scope = StorageScope::make(op->value.as<StringImm>()->value);
      StorageEntry e;
      e.scope = scope;
      if (scope.tag.length() != 0) {
        e.info = GetMemoryInfo(op->value.as<StringImm>()->value);
        CHECK(e.info.defined()) << "Cannot find memory info of " << scope.to_string();
      }
      storage_info_[buf] = e;
      return IRMutator::Mutate_(op, s);
    } else {
      return IRMutator::Mutate_(op, s);
    }
  }
  Expr Mutate_(const Call* op, const Expr &e) final {
    if (op->is_intrinsic(intrinsic::tvm_access_ptr)) {
      return MakeAccessPtr(op, e);
    } else if (op->is_intrinsic(intrinsic::tvm_call_packed)) {
      return MakeCallPacked(op, e);
    } else if (op->is_intrinsic(intrinsic::tvm_stack_make_shape)) {
      return MakeShape(op, e);
    } else if (op->is_intrinsic(intrinsic::tvm_stack_make_array)) {
      return MakeArray(op, e);
    } else {
      return IRMutator::Mutate_(op, e);
    }
  }

  Expr Convert(Type t, Expr e) {
    if (e.type() != t) {
      return Cast::make(t, e);
    } else {
      return e;
    }
  }
  // call shape
  Expr MakeShape(const Call* op, const Expr& e) {
    size_t stack_begin = run_shape_stack_;
    run_shape_stack_ += op->args.size();
    Expr expr = IRMutator::Mutate_(op, e);
    op = expr.as<Call>();
    for (size_t i = 0; i < op->args.size(); ++i) {
      prep_seq_.emplace_back(
          Store::make(stack_shape_, Convert(Int(64), op->args[i]),
                      ConstInt32(stack_begin +i), const_true(1)));
    }
    return AddressOffset(stack_shape_, Int(64), stack_begin);
  }
  // make array
  Expr MakeArray(const Call* op, const Expr& e) {
    size_t idx = run_array_stack_;
    run_array_stack_ += 1;
    Expr expr = IRMutator::Mutate_(op, e);
    op = expr.as<Call>();
    prep_seq_.emplace_back(
        TVMStructSet(stack_array_, idx, intrinsic::kArrData, op->args[0]));
    prep_seq_.emplace_back(
        TVMStructSet(stack_array_, idx, intrinsic::kArrShape, op->args[1]));
    Expr strides = op->args[2];
    if (!strides.defined() || is_zero(strides)) {
      strides = make_zero(Handle());
    }
    prep_seq_.emplace_back(
        TVMStructSet(stack_array_, idx, intrinsic::kArrStrides, strides));
    prep_seq_.emplace_back(
        TVMStructSet(stack_array_, idx, intrinsic::kArrNDim, op->args[3]));
    Type dtype = op->args[4].type();
    prep_seq_.emplace_back(
        TVMStructSet(stack_array_, idx, intrinsic::kArrTypeCode,
                     make_const(UInt(8), static_cast<int>(dtype.code()))));
    prep_seq_.emplace_back(
        TVMStructSet(stack_array_, idx, intrinsic::kArrTypeBits,
                     make_const(UInt(8), dtype.bits())));
    prep_seq_.emplace_back(
        TVMStructSet(stack_array_, idx, intrinsic::kArrTypeLanes,
                     make_const(UInt(16), dtype.lanes())));
    // set byte offset
    int data_bytes = GetVectorBytes(dtype);
    Expr byte_offset = op->args[5];
    if (!is_zero(byte_offset)) {
      byte_offset = byte_offset * make_const(byte_offset.type(), data_bytes);
    }
    prep_seq_.emplace_back(
        TVMStructSet(stack_array_, idx, intrinsic::kArrByteOffset,
                     Convert(UInt(64), byte_offset)));
    CHECK(device_type_.defined()) << "Unknown device type in current IR";
    CHECK(device_id_.defined()) << "Unknown device id in current IR";
    prep_seq_.emplace_back(
        TVMStructSet(stack_array_, idx, intrinsic::kArrDeviceId,
                     Convert(Int(32), device_id_)));
    prep_seq_.emplace_back(
        TVMStructSet(stack_array_, idx, intrinsic::kArrDeviceType,
                     Convert(Int(32), device_type_)));
    return TVMStructGet(Handle(), stack_array_, idx, intrinsic::kArrAddr);
  }
  // call packled.
  Expr MakeCallPacked(const Call* op, const Expr& e) {
    size_t restore_shape_stack = run_shape_stack_;
    size_t restore_array_stack = run_array_stack_;
    size_t arg_stack_begin = run_arg_stack_;
    run_arg_stack_ += op->args.size();
    // Specially handle the buffer packed intrinsic
    Expr expr = IRMutator::Mutate_(op, e);
    op = expr.as<Call>();
    for (size_t i = 1; i < op->args.size(); ++i) {
      Expr stack_index = ConstInt32(arg_stack_begin + i - 1);
      Expr arg = op->args[i];
      Type t = arg.type();
      Type api_type = APIType(t);
      if (t != api_type) {
        arg = Cast::make(api_type, arg);
      }
      prep_seq_.emplace_back(TVMStructSet(
          stack_value_, static_cast<int>(arg_stack_begin + i - 1),
          intrinsic::kTVMValueContent, arg));
      int arg_tcode = api_type.code();
      if (IsArrayHandle(arg)) arg_tcode = kArrayHandle;
      prep_seq_.emplace_back(
          Store::make(stack_tcode_,
                      ConstInt32(arg_tcode),
                      stack_index, const_true(1)));
    }
    // UPDATE stack value
    max_arg_stack_ = std::max(run_arg_stack_, max_arg_stack_);
    max_shape_stack_ = std::max(run_shape_stack_, max_shape_stack_);
    max_array_stack_ = std::max(run_array_stack_, max_array_stack_);
    run_shape_stack_ = restore_shape_stack;
    run_array_stack_ = restore_array_stack;
    run_arg_stack_ = arg_stack_begin;
    Array<Expr> packed_args = {
      op->args[0],
      stack_value_,
      stack_tcode_,
      ConstInt32(arg_stack_begin),
      ConstInt32(arg_stack_begin + op->args.size() - 1)
    };
    return Call::make(
        Int(32), intrinsic::tvm_call_packed_lowered,
        packed_args, Call::Intrinsic);
  }
  // tvm_access_ptr
  Expr MakeAccessPtr(const Call* op, const Expr& e) {
    // Specially handle the buffer packed intrinsic
    Expr expr = IRMutator::Mutate_(op, e);
    op = expr.as<Call>();
    CHECK_EQ(op->args.size(), 5U);
    Type dtype = op->args[0].type();
    const Variable* buffer = op->args[1].as<Variable>();
    Expr offset = op->args[2];
    auto it = storage_info_.find(buffer);
    if (it != storage_info_.end() && it->second.scope.tag.length() != 0) {
      return MakeTaggedAccessPtr(
          op->type, dtype, offset,
          it->second.info.defined() ? it->second.info->unit_bits : 8);
    }
    CHECK(op->type.is_handle());
    // Change to address_of
    return AddressOffset(Var(op->args[1].node_), dtype, offset);
  }

  Expr MakeTaggedAccessPtr(Type ptr_type, Type dtype,
                           Expr offset, int unit_bits) {
    int dtype_bits = dtype.bits() * dtype.lanes();
    CHECK_EQ(unit_bits % dtype_bits, 0);
    return Convert(ptr_type,
                   ir::Simplify(offset / make_const(offset.type(), unit_bits / dtype_bits)));
  }

 private:
  bool IsArrayHandle(const Expr& arg) {
    // specially set array handle.
    if (const Call* buf = arg.as<Call>()) {
      if (buf->is_intrinsic(intrinsic::tvm_struct_get) &&
          buf->args[2].as<IntImm>()->value == intrinsic::kArrAddr) {
        return true;
      }
    }
    return false;
  }

  // The prepration sequence to be emitted.
  std::vector<Stmt> prep_seq_;
  Expr device_type_;
  Expr device_id_;
  // Var handle for each stack.
  Var stack_shape_;
  Var stack_array_;
  Var stack_tcode_;
  Var stack_value_;
  // The running statistics
  uint64_t run_shape_stack_{0};
  uint64_t run_array_stack_{0};
  uint64_t run_arg_stack_{0};
  // statistics of stacks
  uint64_t max_shape_stack_{0};
  uint64_t max_array_stack_{0};
  uint64_t max_arg_stack_{0};
  // The storage entry.
  struct StorageEntry {
    // Whether it is tagged memory.
    StorageScope scope;
    // The memory info if any.
    MemoryInfo info;
    // Allocation counter
    int alloc_count{0};
  };
  // The storage scope of each buffer
  std::unordered_map<const Variable*, StorageEntry> storage_info_;
};

LoweredFunc LowerTVMBuiltin(LoweredFunc f) {
  auto n = std::make_shared<LoweredFuncNode>(*f.operator->());
  n->body = BuiltinLower().Build(n->body);
  return LoweredFunc(n);
}

}  // namespace ir
}  // namespace tvm