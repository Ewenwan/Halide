#include <algorithm>
#include <sstream>

#include "CodeGen_Internal.h"
#include "CodeGen_Vulkan_Dev.h"
#include "Deinterleave.h"
#include "Debug.h"
#include "IROperator.h"

#include "spirv/spirv.h"

// Temporary:
#include <fstream>

namespace Halide {
namespace Internal {

void CodeGen_Vulkan_Dev::SPIRVEmitter::add_instruction(std::vector<uint32_t> &region, uint32_t opcode,
                                                       std::initializer_list<uint32_t> words) {
    region.push_back(((1 + words.size()) << 16) | opcode);
    region.insert(region.end(), words.begin(), words.end());
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::add_instruction(uint32_t opcode, std::initializer_list<uint32_t> words) {
    spir_v_kernels.push_back(((1 + words.size()) << 16) | opcode);
    spir_v_kernels.insert(spir_v_kernels.end(), words.begin(), words.end());
}
void CodeGen_Vulkan_Dev::SPIRVEmitter::add_instruction(std::vector<uint32_t> &region, uint32_t opcode,
                                                        std::vector<uint32_t> words) {
  region.push_back(((1 + words.size()) << 16) | opcode);
  region.insert(region.end(), words.begin(), words.end());
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::add_instruction(uint32_t opcode, std::vector<uint32_t> words) {
  spir_v_kernels.push_back(((1 + words.size()) << 16) | opcode);
  spir_v_kernels.insert(spir_v_kernels.end(), words.begin(), words.end());
}

uint32_t CodeGen_Vulkan_Dev::SPIRVEmitter::emit_constant(const Type &t, const void *data) {
    std::string key(t.bytes() + 4, ' ');
    key[0] = t.code();
    key[1] = t.bits();
    key[2] = t.lanes() & 0xff;
    key[3] = (t.lanes() >> 8) & 0xff;
    const char *data_char = (const char *)data;
    for (int i = 0; i < t.bytes(); i++) {
        key[i + 4] = data_char[i];
    }

    auto item = constant_map.find(key);
    if  (item == constant_map.end()) {
        uint32_t type_id = map_type(t);
        uint32_t extra_words = (t.bytes() + 3) / 4;
        uint32_t constant_id = next_id++;
        spir_v_kernels.push_back(((3 + extra_words) << 16) | SpvOpConstant);
        spir_v_kernels.push_back(type_id);
        spir_v_kernels.push_back(constant_id);

        const uint8_t *data_temp = (const uint8_t *)data;
        size_t bytes_copied = 0;
        for (uint32_t i = 0; i < extra_words; i++) {
            uint32_t word;
            size_t to_copy = std::min(t.bytes() - bytes_copied, (size_t)4);
            memcpy(&word, data_temp, to_copy);
            bytes_copied += to_copy;
            spir_v_types.push_back(word);
            data_temp++;
        }
        return constant_id;
    } else {
        return item->second;
    }
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::scalarize(Expr e) {
    internal_assert(e.type().is_vector()) << "CodeGen_Vulkan_Dev::SPIRVEmitter::scalarize must be called with an expression of vector type.\n";
    uint32_t type_id = map_type(e.type());

    uint32_t result_id = next_id++;
    add_instruction(SpvOpConstantNull, { type_id, result_id } );

    for (int i = 0; i < e.type().lanes(); i++) {
        extract_lane(e, i).accept(this);
        uint32_t composite_vec = next_id++;
        add_instruction(SpvOpVectorInsertDynamic, { type_id, composite_vec, (uint32_t)i, result_id, id });
        result_id = composite_vec;
    }
    id = result_id;
}

uint32_t CodeGen_Vulkan_Dev::SPIRVEmitter::map_type(const Type &t) {
    auto item = type_map.find(t);
    if  (item == type_map.end()) {
        // TODO, handle arrays, pointers, halide_buffer_t
        uint32_t type_id = 0;
        if (t.lanes() != 1) {
            uint32_t base_id = map_type(t.with_lanes(1));
            type_id = next_id++;
            add_instruction(spir_v_types, SpvOpTypeVector, { type_id, base_id, (uint32_t)t.lanes() });
        } else {
            if (t.is_float()) {
                type_id = next_id++;
                add_instruction(spir_v_types, SpvOpTypeFloat, { type_id, (uint32_t)t.bits() });
            } else if (t.is_bool()) {
                type_id = next_id++;
                add_instruction(spir_v_types, SpvOpTypeBool, { type_id });
            } else if (t.is_int_or_uint()) {
                type_id = next_id++;
                // Integer types always have the signedness bit set to
                // 0 because setting it to 1 is apparently not
                // supported in Kernel capability.
                add_instruction(spir_v_types, SpvOpTypeInt, { type_id, (uint32_t)t.bits(), 0 });
            } else {
                internal_error << "Unsupported type in Vulkan backend " << t << "\n";
            }
        }
        type_map[t] = type_id;
        return type_id;
    } else {
        return item->second;
    }
}

uint32_t CodeGen_Vulkan_Dev::SPIRVEmitter::map_type_to_pair(const Type &t) {
    uint32_t &ref = pair_type_map[t];

    if  (ref == 0) {
        uint32_t base_type = map_type(t);

        uint32_t type_id = next_id++;

        add_instruction(spir_v_types, SpvOpTypeStruct, { type_id, base_type, base_type });
        ref = type_id;
    }
    return ref;
}

uint32_t CodeGen_Vulkan_Dev::SPIRVEmitter::map_pointer_type_local(const Type &type) {
    uint32_t &ref = pointer_type_map_local[type];

    if (ref == 0) {
        uint32_t base_type_id = map_type(type);
        ref = next_id++;
        add_instruction(spir_v_types, SpvOpTypePointer, { ref, SpvStorageClassFunction, base_type_id });
        pointer_type_map_local[type] = ref;
    }
    return ref;
}

// TODO: Probably should unify all the pointer types into a single map
uint32_t CodeGen_Vulkan_Dev::SPIRVEmitter::map_pointer_type_input(const Type &type) {
    uint32_t &ref = pointer_type_map_input[type];

    if (ref == 0) {
      uint32_t base_type_id = map_type(type);
      ref = next_id++;
      add_instruction(spir_v_types, SpvOpTypePointer, { ref, SpvStorageClassInput, base_type_id });
      pointer_type_map_input[type] = ref;
    }
    return ref;
}
void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Variable *var) {
    id = symbol_table.get(var->name);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const IntImm *imm) {
    id = emit_constant(imm->type, &imm->value);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const UIntImm *imm) {
    id = emit_constant(imm->type, &imm->value);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const StringImm *imm) {
    uint32_t extra_words = (imm->value.size() + 1 + 3) / 4;
    id = next_id++;
    spir_v_kernels.push_back(((2 + extra_words) << 16) | SpvOpString);
    spir_v_kernels.push_back(id);

    const char *data_temp = (const char *)imm->value.c_str();
    size_t bytes_copied = 0;
    for (uint32_t i = 0; i < extra_words; i++) {
        uint32_t word;
        size_t to_copy = std::min(imm->value.size() + 1 - bytes_copied, (size_t)4);
        memcpy(&word, data_temp, to_copy);
        bytes_copied += to_copy;
        spir_v_kernels.push_back(word);
        data_temp += 4;
    }
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const FloatImm *imm) {
    id = emit_constant(imm->type, &imm->value);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Cast *op) {
    uint32_t opcode = 0;
    if (op->value.type().is_float()) {
        if (op->type.is_float()) {
            opcode = SpvOpFConvert;
        } else if (op->type.is_uint()) {
            opcode = SpvOpConvertFToU;
        } else if (op->type.is_int()) {
            opcode = SpvOpConvertFToS;
        } else {
            internal_error << "Vulkan cast unhandled case " << op->value.type() << " to " << op->type << "\n";
        }
    } else if (op->value.type().is_uint()) {
        if (op->type.is_float()) {
            opcode = SpvOpConvertUToF;
        } else if (op->type.is_uint()) {
            opcode = SpvOpUConvert;
        } else if (op->type.is_int()) {
            opcode = SpvOpSatConvertUToS;
        } else {
            internal_error << "Vulkan cast unhandled case " << op->value.type() << " to " << op->type << "\n";
        }
    } else if (op->value.type().is_int()) {
        if (op->type.is_float()) {
            opcode = SpvOpConvertSToF;
        } else if (op->type.is_uint()) {
            opcode = SpvOpSatConvertSToU;
        } else if (op->type.is_int()) {
            opcode = SpvOpSConvert;
        } else {
            internal_error << "Vulkan cast unhandled case " << op->value.type() << " to " << op->type << "\n";
        }
    } else {
        internal_error << "Vulkan cast unhandled case " << op->value.type() << " to " << op->type << "\n";
    }

    uint32_t type_id = map_type(op->type);
    op->value.accept(this);
    uint32_t src_id = id;
    id = next_id++;
    add_instruction(opcode, { type_id, id, src_id });
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Add *op) {
    visit_binop(op->type, op->a, op->b, op->type.is_float() ? SpvOpFAdd : SpvOpIAdd);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Sub *op) {
    visit_binop(op->type, op->a, op->b, op->type.is_float() ? SpvOpFSub : SpvOpISub);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Mul *op) {
    visit_binop(op->type, op->a, op->b, op->type.is_float() ? SpvOpFMul : SpvOpIMul);
}

// TODO: Wait for PR which makes this an intrinsic and then generate directly.
Expr CodeGen_Vulkan_Dev::SPIRVEmitter::mulhi_shr(Expr a, Expr b, int shr) {
    internal_error << "Unimplemented.\n";
    return Expr();
#if 0
    uint32_t type_id = map_type(a.type());

    a.accept(this);
    uint32_t a_id = id;
    a.accept(this);
    uint32_t b_id = id;

    uint32_t pair_type_id = map_type_to_pair(a.type());

    // Double width multiply
    uint32_t product_pair = next_id++;
    spir_v_kernels.push_back((5 << 16) | (a.type().is_uint()) ? SpvOpUMulExtended : SpvOpSMulExtended);
    spir_v_kernels.push_back(pair_type_id);
    spir_v_kernels.push_back(a_id);
    spir_v_kernels.push_back(b_id);

    // Extract the halves of the double width result and
    // assemble them into the shifted signle width one.
    uint32_t low_item_id = next_id++;
    spir_v_kernels.push_back((5 << 16) | SpvOpCompositeExtract);
    spir_v_kernels.push_back(type_id);
    spir_v_kernels.push_back(low_item_id);
    spir_v_kernels.push_back(product_pair);
    spir_v_kernels.push_back(0);
    
    uint32_t high_item_id = next_id++;
    spir_v_kernels.push_back((5 << 16) | SpvOpCompositeExtract);
    spir_v_kernels.push_back(type_id);
    spir_v_kernels.push_back(high_item_id);
    spir_v_kernels.push_back(product_pair);
    spir_v_kernels.push_back(1);

    // TODO: This part is incorrect. We just need the high part
    // shifted down by shr. The low part is not used.
    uint32_t low_part_id = next_id++;
    spir_v_kernels.push_back((5 << 16) | SpvOpShiftRightLogical);
    spir_v_kernels.push_back(type_id);
    spir_v_kernels.push_back(low_part_id);
    spir_v_kernels.push_back(low_item_id);
    spir_v_kernels.push_back(shr);
    
    uint32_t high_part_id = next_id++;
    spir_v_kernels.push_back((5 << 16) | SpvOpShiftLeftLogical);
    spir_v_kernels.push_back(type_id);
    spir_v_kernels.push_back(high_part_id);
    spir_v_kernels.push_back(high_item_id);
    spir_v_kernels.push_back(a.type().bits() - shr);

    uint32_t result_id = next_id++;
    spir_v_kernels.push_back((4 << 16) | SpvOpBitwiseOr);
    spir_v_kernels.push_back(type_id);
    spir_v_kernels.push_back(result_id);
    spir_v_kernels.push_back(low_part_id);
    spir_v_kernels.push_back(high_part_id);

    return result_id;
#endif
}

Expr CodeGen_Vulkan_Dev::SPIRVEmitter::sorted_avg(Expr a, Expr b) {
    // b > a, so the following works without widening:
    // a + (b - a)/2
    return a + (b - a)/2;
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Div *op) {
    user_assert(!is_zero(op->b)) << "Division by constant zero in expression: " << Expr(op) << "\n";

    if (op->type.is_float()) {
        visit_binop(op->type, op->a, op->b, SpvOpFDiv);
    } else {
        // TODO: Wait for division refactor PR to make be able to use LLVM constant divide logic
        Expr e = lower_euclidean_div(op->a, op->b);
        e.accept(this);
    }
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Mod *op) {
    if (op->type.is_float()) {
        // Takes sign of result from op->b
        visit_binop(op->type, op->a, op->b, SpvOpFMod);
    } else {
        // TODO: Wait for division refactor PR to make be able to use LLVM constant divide logic
        Expr e = lower_euclidean_mod(op->a, op->b);
        e.accept(this);
    }
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Max *op) {
    std::string a_name = unique_name('a');
    std::string b_name = unique_name('b');
    Expr a = Variable::make(op->a.type(), a_name);
    Expr b = Variable::make(op->b.type(), b_name);
    Expr temp = Let::make(a_name, op->a,
                          Let::make(b_name, op->b, select(a > b, a, b)));
    temp.accept(this);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Min *op) {
    std::string a_name = unique_name('a');
    std::string b_name = unique_name('b');
    Expr a = Variable::make(op->a.type(), a_name);
    Expr b = Variable::make(op->b.type(), b_name);
    Expr temp = Let::make(a_name, op->a,
                          Let::make(b_name, op->b, select(a < b, a, b)));
    temp.accept(this);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const EQ *op) {
    visit_binop(op->type, op->a, op->b, op->type.is_float() ? SpvOpFOrdEqual : SpvOpIEqual);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const NE *op) {
    visit_binop(op->type, op->a, op->b, op->type.is_float() ? SpvOpFOrdNotEqual : SpvOpINotEqual);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const LT *op) {
    uint32_t opcode = 0;
    if (op->a.type().is_float()) {
        opcode = SpvOpFOrdLessThan;
    } else if (op->a.type().is_int()) {
        opcode = SpvOpSLessThan;
    } else if (op->a.type().is_uint()) {
        opcode = SpvOpULessThan;
    } else {
        internal_error << "CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const LT *op): unhandled type: " << op->a.type() << "\n";
    }
    visit_binop(op->type, op->a, op->b, opcode);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const LE *op) {
    uint32_t opcode = 0;
    if (op->a.type().is_float()) {
        opcode = SpvOpFOrdLessThanEqual;
    } else if (op->a.type().is_int()) {
        opcode = SpvOpSLessThanEqual;
    } else if (op->a.type().is_uint()) {
        opcode = SpvOpULessThanEqual;
    } else {
        internal_error << "CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const LE *op): unhandled type: " << op->a.type() << "\n";
    }
    visit_binop(op->type, op->a, op->b, opcode);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const GT *op) {
    uint32_t opcode = 0;
    if (op->a.type().is_float()) {
        opcode = SpvOpFOrdGreaterThan;
    } else if (op->a.type().is_int()) {
        opcode = SpvOpSGreaterThan;
    } else if (op->a.type().is_uint()) {
        opcode = SpvOpUGreaterThan;
    } else {
        internal_error << "CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const GT *op): unhandled type: " << op->a.type() << "\n";
    }
    visit_binop(op->type, op->a, op->b, opcode);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const GE *op) {
    uint32_t opcode = 0;
    if (op->a.type().is_float()) {
        opcode = SpvOpFOrdGreaterThanEqual;
    } else if (op->a.type().is_int()) {
        opcode = SpvOpSGreaterThanEqual;
    } else if (op->a.type().is_uint()) {
        opcode = SpvOpUGreaterThanEqual;
    } else {
        internal_error << "CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const GE *op): unhandled type: " << op->a.type() << "\n";
    }
    visit_binop(op->type, op->a, op->b, opcode);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const And *op) {
    visit_binop(op->type, op->a, op->b, SpvOpLogicalAnd);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Or *op) {
    visit_binop(op->type, op->a, op->b, SpvOpLogicalOr);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Not *op) {
    uint32_t type_id = map_type(op->type);
    op->a.accept(this);
    uint32_t a_id = id;
    id = next_id++;
    add_instruction(SpvOpLogicalNot, { type_id, id, a_id });
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Call *op) {
    if (op->is_intrinsic(Call::gpu_thread_barrier)) {
    } else if (op->is_intrinsic(Call::bitwise_and)) {
        internal_assert(op->args.size() == 2);
        visit_binop(op->type, op->args[0], op->args[1], SpvOpBitwiseAnd);
    } else if (op->is_intrinsic(Call::bitwise_xor)) {
        internal_assert(op->args.size() == 2);
        visit_binop(op->type, op->args[0], op->args[1], SpvOpBitwiseXor);
    } else if (op->is_intrinsic(Call::bitwise_or)) {
        internal_assert(op->args.size() == 2);
        visit_binop(op->type, op->args[0], op->args[1], SpvOpBitwiseOr);
    } else if (op->is_intrinsic(Call::bitwise_not)) {
        internal_assert(op->args.size() == 1);
        uint32_t type_id = map_type(op->type);
        op->args[0]->accept(this);
        uint32_t arg_id = id;
        id = next_id++;
        add_instruction(SpvOpNot, { type_id, id, arg_id });
    } else if (op->is_intrinsic(Call::reinterpret)) {
    } else if (op->is_intrinsic(Call::if_then_else)) {
        if (op->type.is_vector()) {
            scalarize(op);
        } else {
            internal_assert(op->args.size() == 3);
            auto phi_inputs = emit_if_then_else(op->args[0], op->args[1], op->args[2]);
            // Generate Phi node if used as an expression.

            uint32_t type_id = map_type(op->type);
            id = next_id++;
            spir_v_kernels.push_back((7 << 16) | SpvOpPhi);
            spir_v_kernels.push_back(type_id);
            spir_v_kernels.push_back(id);
            spir_v_kernels.insert(spir_v_kernels.end(), phi_inputs.ids, phi_inputs.ids + 4);
      }
    } else if (op->is_intrinsic("div_round_to_zero")) {
        internal_assert(op->args.size() == 2);
        uint32_t opcode = 0;
        if (op->type.is_int()) {
            opcode = SpvOpSDiv;
        } else if (op->type.is_uint()) {
            opcode = SpvOpUDiv;
        } else {
            internal_error << "div_round_to_zero of non-integer type.\n";
        }
        visit_binop(op->type, op->args[0], op->args[1], opcode);
    } else if (op->is_intrinsic("mod_round_to_zero")) {
        internal_assert(op->args.size() == 2);
        uint32_t opcode = 0;
        if (op->type.is_int()) {
            opcode = SpvOpSMod;
        } else if (op->type.is_uint()) {
            opcode = SpvOpUMod;
        } else {
            internal_error << "mod_round_to_zero of non-integer type.\n";
        }
        visit_binop(op->type, op->args[0], op->args[1], opcode);
    }
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Select *op) {
    uint32_t type_id = map_type(op->type);
    op->condition.accept(this);
    uint32_t cond_id = id;
    op->true_value.accept(this);
    uint32_t true_id = id;
    op->false_value.accept(this);
    uint32_t false_id = id;
    id = next_id++;
    add_instruction(SpvOpSelect, { type_id, id, cond_id, true_id, false_id });
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Load *op) {
#if 0
    uint32_t result_type = map_type(op->type);
    uint32_t pointer_id = id;
    id = next_id++;
    add_instruction(SpvOpLoad, { result_type, id, pointer_id });
#endif
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Store *op) {
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Let *let) {
    let->value.accept(this);
    ScopedBinding<uint32_t> binding(symbol_table, let->name, id);
    let->body.accept(this);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const LetStmt *let) {
    let->value.accept(this);
    ScopedBinding<uint32_t> binding(symbol_table, let->name, id);
    let->body.accept(this);
    // TODO: Figure out undef here?
    id = 0xffffffff;
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const AssertStmt *) {
    // TODO: Fill this in.
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const ProducerConsumer *) {
    // I believe these nodes are solely for annotation purposes.
#if 0
    string name;
    if (op->is_producer) {
        name = std::string("produce ") + op->name;
    } else {
        name = std::string("consume ") + op->name;
    }
    BasicBlock *produce = BasicBlock::Create(*context, name, function);
    builder->CreateBr(produce);
    builder->SetInsertPoint(produce);
    codegen(op->body);
#endif
}

namespace {
std::pair<std::string, uint32_t> simt_intrinsic(const std::string &name) {
    if (ends_with(name, ".__thread_id_x")) {
      return {"LocalInvocationId", 0};
    } else if (ends_with(name, ".__thread_id_y")) {
      return {"LocalInvocationId", 1};
    } else if (ends_with(name, ".__thread_id_z")) {
      return {"LocalInvocationId", 2};
    } else if (ends_with(name, ".__block_id_x")) {
      return {"WorkgroupId", 0};
    } else if (ends_with(name, ".__block_id_y")) {
      return {"WorkgroupId", 1};
    } else if (ends_with(name, ".__block_id_z")) {
      return {"WorkgroupId", 2};
    } else if (ends_with(name, "id_w")) {
      user_error << "Vulkan only supports <=3 dimensions for gpu blocks";
    }
    internal_error << "simt_intrinsic called on bad variable name: " << name << "\n";
    return {"", -1};
}
} // anonymous namespace

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const For *op) {

    if (is_gpu_var(op->name)) {
        internal_assert((op->for_type == ForType::GPUBlock) ||
                        (op->for_type == ForType::GPUThread))
            << "kernel loops must be either gpu block or gpu thread\n";
        // This should always be true at this point in codegen
        internal_assert(is_zero(op->min));

        auto intrinsic = simt_intrinsic(op->name);

        // The builtins are pointers to vec3
        uint32_t intrinsic_type_id = map_pointer_type_input(Type(Type::UInt, 32, 3));

        // If the symbol table does not contain mappings for the SIMT intrinsic,
        // we need to add them first
        // TODO: could be done at module init time, and could use ScopedBinding
        if (!symbol_table.contains(intrinsic.first)) {
            uint32_t intrinsic_id = next_id++;
            add_instruction(spir_v_types, SpvOpVariable, {intrinsic_type_id, intrinsic_id, SpvStorageClassInput});
            symbol_table.push(intrinsic.first, intrinsic_id);

            // Annotate that this is the specific builtin
            auto built_in_kind = starts_with(intrinsic.first, "Workgroup") ? SpvBuiltInWorkgroupId : SpvBuiltInLocalInvocationId;
            add_instruction(spir_v_annotations, SpvOpDecorate, {intrinsic_id, SpvDecorationBuiltIn, built_in_kind});
        }

        uint32_t intrinsic_id = symbol_table.get(intrinsic.first);
        uint32_t gpu_var_id = next_id++;
        add_instruction(SpvOpCompositeExtract, {map_type(UInt(32)), gpu_var_id, intrinsic_id, intrinsic.second});
        {
            ScopedBinding<uint32_t> binding(symbol_table, op->name, gpu_var_id);
            //std::cerr << "Loop body is " << op->body;
            //if (!op->body.as<For>()) return;
            op->body.accept(this);
        }

    } else {

        internal_assert(op->for_type == ForType::Serial) << "CodeGen_Vulkan_Dev::SPIRVEmitter::visit unhandled For type: " << op->for_type << "\n";

        // TODO: Loop vars are alway int32_t right?
        uint32_t index_type_id = map_type(Int(32));
        uint32_t index_var_type_id = map_pointer_type_local(Int(32));

        op->min.accept(this);
        uint32_t min_id = id;
        op->extent.accept(this);
        uint32_t extent_id = id;

        // Compute max.
        uint32_t max_id = next_id++;
        add_instruction(SpvOpIAdd, { index_type_id, max_id, min_id, extent_id });

        // Declare loop var
        uint32_t loop_var_id = next_id++;
        add_instruction(SpvOpVariable, { index_var_type_id, loop_var_id, min_id });

        uint32_t header_label_id = next_id++;
        uint32_t loop_top_label_id = next_id++;
        uint32_t body_label_id = next_id++;
        uint32_t continue_label_id = next_id++;
        uint32_t merge_label_id = next_id++;
        add_instruction(SpvOpLabel, { header_label_id });
        add_instruction(SpvOpLoopMerge, { merge_label_id, continue_label_id, SpvLoopControlMaskNone });
        add_instruction(SpvOpBranch, { loop_top_label_id });
        add_instruction(SpvOpLabel, { loop_top_label_id });

        // loop test.
        uint32_t cur_index_id = next_id++;
        add_instruction(SpvOpLoad, { index_type_id, cur_index_id, loop_var_id });

        uint32_t loop_test_id = next_id++;
        add_instruction(SpvOpSLessThanEqual, { loop_test_id, cur_index_id, max_id });
        add_instruction(SpvOpBranchConditional, { loop_test_id, body_label_id, merge_label_id });

        add_instruction(SpvOpLabel, { body_label_id });

        {
            ScopedBinding<uint32_t> binding(symbol_table, op->name, cur_index_id);

            op->body.accept(this);
        }

        add_instruction(SpvOpBranch, { continue_label_id });
        add_instruction(SpvOpLabel, { continue_label_id });

        // Loop var update?
        uint32_t next_index_id = next_id++;
        int32_t one = 1;
        uint32_t constant_one_id = emit_constant(Int(32), &one);
        add_instruction(SpvOpIAdd, { index_type_id, next_index_id, cur_index_id, constant_one_id});
        add_instruction(SpvOpStore, { index_type_id, next_index_id, loop_var_id });
        add_instruction(SpvOpBranch, { header_label_id });
        add_instruction(SpvOpLabel, { merge_label_id });
    }
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Ramp *op) {
    // TODO: Is there a way to do this that doesn't require duplicating lane values?
    uint32_t base_type_id = map_type(op->base.type());
    uint32_t type_id = map_type(op->type);
    op->base.accept(this);
    uint32_t base_id = id;
    op->stride.accept(this);
    uint32_t stride_id = id;
    uint32_t add_opcode = op->base.type().is_float() ? SpvOpFAdd : SpvOpIAdd;
    // Generate adds to make the elements of the ramp.
    uint32_t prev_id = base_id;
    uint32_t first_id = next_id;
    for (int i = 1; i < op->lanes; i++) {
        uint32_t this_id = next_id++;
        add_instruction(add_opcode, { base_type_id, this_id, prev_id, stride_id });
        prev_id = this_id;
    }
    
    id = next_id++;
    spir_v_kernels.push_back(((op->lanes + 3) << 16) | SpvOpCompositeConstruct);
    spir_v_kernels.push_back(type_id);
    spir_v_kernels.push_back(id);
    spir_v_kernels.push_back(base_id);
    for (int i = 1; i < op->lanes; i++) {
        spir_v_kernels.push_back(first_id++);
    }
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Broadcast *op) {
    // TODO: Is there a way to do this that doesn't require duplicating lane values?
    uint32_t type_id = map_type(op->type);
    op->value.accept(this);
    uint32_t value_id = id;
    id = next_id++;
    spir_v_kernels.push_back(((op->lanes + 3) << 16) | SpvOpCompositeConstruct);
    spir_v_kernels.push_back(type_id);
    spir_v_kernels.push_back(id);
    spir_v_kernels.insert(spir_v_kernels.end(), op->lanes, value_id);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Provide *) {
    internal_error << "CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Provide *): Provide encountered during codegen\n";
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Allocate *) {
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Free *) {
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Realize *) {
    internal_error << "CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Realize *): Realize encountered during codegen\n";
}

template <typename StmtOrExpr>
CodeGen_Vulkan_Dev::SPIRVEmitter::PhiNodeInputs
CodeGen_Vulkan_Dev::SPIRVEmitter::emit_if_then_else(Expr condition,
                                                    StmtOrExpr then_case, StmtOrExpr else_case) {
    condition.accept(this);
    uint32_t cond_id = id;
    uint32_t then_label_id = next_id++;
    uint32_t else_label_id = next_id++;
    uint32_t merge_label_id = next_id++;

    add_instruction(SpvOpBranchConditional, { cond_id, then_label_id, else_label_id });
    add_instruction(SpvOpLabel, { then_label_id });

    then_case.accept(this);
    uint32_t then_id = id;

    add_instruction(SpvOpBranch, { merge_label_id });
    add_instruction(SpvOpLabel, { else_label_id });

    else_case.accept(this);
    uint32_t else_id = id;

    add_instruction(SpvOpLabel, { merge_label_id });

    return {{ then_id, then_label_id, else_id, else_label_id }};
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const IfThenElse *op) {
    emit_if_then_else(op->condition, op->then_case, op->else_case);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Evaluate *op) {
    op->value.accept(this);
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Shuffle *op) {
    internal_assert(op->vectors.size() == 2) << "CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Shuffle *op): SPIR-V codegen currently only supports shuffles of vector pairs.\n";
    uint32_t type_id = map_type(op->type);
    op->vectors[0].accept(this);
    uint32_t vector0_id = id;
    op->vectors[1].accept(this);
    uint32_t vector1_id = id;

    id = next_id++;
    spir_v_kernels.push_back(((5 + op->indices.size()) << 16) | SpvOpPhi);
    spir_v_kernels.push_back(type_id);
    spir_v_kernels.push_back(id);
    spir_v_kernels.push_back(vector0_id);
    spir_v_kernels.push_back(vector1_id);
    spir_v_kernels.insert(spir_v_kernels.end(), op->indices.begin(), op->indices.end());
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Prefetch *) {
    internal_error << "CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Prefetch *): Prefetch encountered during codegen\n";
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Fork *) {
    internal_error << "void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Fork *) not supported yet.";
}

void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Acquire *) {
    internal_error << "void CodeGen_Vulkan_Dev::SPIRVEmitter::visit(const Acquire *) not supported yet.";
}

// TODO: fast math decorations.
void CodeGen_Vulkan_Dev::SPIRVEmitter::visit_binop(Type t, Expr a, Expr b, int32_t opcode) {
    uint32_t type_id = map_type(t);
    a.accept(this);
    uint32_t a_id = id;
    b.accept(this);
    uint32_t b_id = id;
    id = next_id++;
    add_instruction(opcode, { type_id, id, a_id, b_id });
}


void CodeGen_Vulkan_Dev::SPIRVEmitter::add_kernel(Stmt s,
                                                  const std::string &name,
                                                  const std::vector<DeviceArgument> &args) {
    debug(2) << "Adding Vulkan kernel " << name << "\n";

    // Add function definition
    // TODO: can we use one of the function control annotations?

    // Declare the function type.  TODO: should this be unique?
    // First, gather all the types of the parameters
    uint32_t function_type_id = next_id++;
    std::vector<uint32_t> param_type_ids;
    param_type_ids.push_back(function_type_id);
    // return type
    param_type_ids.push_back(void_id);

    for (auto arg: args) {
        auto type_id = arg.is_buffer ? map_pointer_type_input(arg.type) : map_type(arg.type);
        param_type_ids.push_back(type_id);
    }
    add_instruction(spir_v_types, SpvOpTypeFunction, param_type_ids);

    // Add definition and parameters
    uint32_t function_id = next_id++;
    add_instruction(SpvOpFunction, {void_id, function_id, SpvFunctionControlMaskNone, function_type_id});
    for (size_t i = 0; i < args.size(); i++) {
        auto param_id = next_id++;
        // The first two entries in param_type_ids are the function type id and return type
        add_instruction(SpvOpFunctionParameter, {param_type_ids[2+i], param_id});
        symbol_table.push(args[i].name, param_id);
    }

    s.accept(this);

    // Insert function end delimiter
    add_instruction(SpvOpFunctionEnd, {});

    // Pop scope
    for (auto arg: args) {
        symbol_table.pop(arg.name);
    }
}

CodeGen_Vulkan_Dev::CodeGen_Vulkan_Dev(Target t) {
}

void CodeGen_Vulkan_Dev::init_module() {
    debug(2) << "Vulkan device codegen init_module\n";

    // Header.
    emitter.spir_v_header.push_back(SpvMagicNumber);
    emitter.spir_v_header.push_back(SpvVersion);
    emitter.spir_v_header.push_back(SpvSourceLanguageUnknown);
    emitter.spir_v_header.push_back(0); // Bound placeholder
    emitter.spir_v_header.push_back(0); // Reserved for schema.

    // the unique void type
    emitter.next_id++; // 0 is not a valid id
    emitter.void_id = emitter.next_id++;
    emitter.add_instruction(emitter.spir_v_types, SpvOpTypeVoid, {emitter.void_id});

    // Capabilities
    emitter.add_instruction(emitter.spir_v_header, SpvOpCapability, {SpvCapabilityAddresses});
    emitter.add_instruction(emitter.spir_v_header, SpvOpCapability, {SpvCapabilityKernel});

    // Memory model
    // TODO: 32-bit or 64-bit?
    // TODO: Which memory model?
    emitter.add_instruction(emitter.spir_v_header, SpvOpMemoryModel,
                            {SpvAddressingModelPhysical32, SpvMemoryModelOpenCL});


    // OpCapability instructions
    //    Enumerate type maps and add subwidth integer types if used
    // OpExtensions instructions
    // OpExtImport instructions
    // One OpMemoryModelInstruction
    // OpEntryPoint instructions -- tricky as we don't know them until the kernels are added. May need to insert as we go.
    // OpExecutionMode or OpExecutionModeId -- are these also added at add_kernel time?
    // debug -- empty?
    // annotation -- empty?
    //     I believe alignment info for load/store/etc. is done with annotations.
    // OpType instructions. Contained in spir_v_types member.
    // Function declarations. Are there any?
    // Function bodies -- one per add_kernel
}

void CodeGen_Vulkan_Dev::add_kernel(Stmt stmt,
                                    const std::string &name,
                                    const std::vector<DeviceArgument> &args) {
    current_kernel_name = name;
    emitter.add_kernel(stmt, name, args);
    dump();
}

std::vector<char> CodeGen_Vulkan_Dev::compile_to_src() {
    #ifdef WITH_VULKAN

    emitter.spir_v_header[3] = emitter.next_id;

    std::vector<char> final_module;
    size_t total_size = (emitter.spir_v_header.size() + emitter.spir_v_entrypoints.size() + emitter.spir_v_annotations.size() + emitter.spir_v_types.size() + emitter.spir_v_kernels.size()) * sizeof(uint32_t);
    final_module.reserve(total_size);
    final_module.insert(final_module.end(), (const char *)emitter.spir_v_header.data(), (const char *)(emitter.spir_v_header.data() + emitter.spir_v_header.size()));
    final_module.insert(final_module.end(), (const char *)emitter.spir_v_entrypoints.data(), (const char *)(emitter.spir_v_entrypoints.data() + emitter.spir_v_entrypoints.size()));
    final_module.insert(final_module.end(), (const char *)emitter.spir_v_annotations.data(), (const char *)(emitter.spir_v_annotations.data() + emitter.spir_v_annotations.size()));
    final_module.insert(final_module.end(), (const char *)emitter.spir_v_types.data(), (const char *)(emitter.spir_v_types.data() + emitter.spir_v_types.size()));
    final_module.insert(final_module.end(), (const char *)emitter.spir_v_kernels.data(), (const char *)(emitter.spir_v_kernels.data() + emitter.spir_v_kernels.size()));
    assert(final_module.size() == total_size);
    return final_module;

    #endif
}

std::string CodeGen_Vulkan_Dev::get_current_kernel_name() {
    return current_kernel_name;
}

std::string CodeGen_Vulkan_Dev::print_gpu_name(const std::string &name) {
    return name;
}

void CodeGen_Vulkan_Dev::dump() {
    // TODO: Figure out what goes here.
    // For now: dump to file so source can be consumed by validator
    auto module = compile_to_src();
    std::ofstream f("out.spv", std::ios::out | std::ios::binary);
    f.write((char*)(module.data()), module.size());
    f.close();
}

}  // namespace Internal
}  // namespace Halide