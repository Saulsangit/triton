#include <algorithm>
#include "triton/ir/module.h"
#include "triton/ir/function.h"
#include "triton/codegen/transform/peephole.h"

namespace triton {
namespace codegen{
namespace transform{


ir::value* rewrite_trans_phi_impl(ir::value *value, ir::builder &builder,
                                 const std::vector<int>& perm) {
  if(auto phi = dynamic_cast<ir::phi_node*>(value)) {
    // transpose operands
    std::vector<ir::value*> incs;
    for(unsigned n = 0; n < phi->get_num_incoming(); n++)
      incs.push_back(rewrite_trans_phi_impl(phi->get_incoming_value(n), builder, perm));
    // create phi for transposed values
    builder.set_insert_point(phi);
    ir::phi_node* result = builder.create_phi(incs[0]->get_type(), incs.size());
    for(unsigned n = 0; n < phi->get_num_incoming(); n++)
      result->add_incoming(incs[n], phi->get_incoming_block(n));
    return result;
  }
  else if(auto i = dynamic_cast<ir::instruction*>(value)){
    ir::basic_block* block = i->get_parent();
    auto it = std::find(block->begin(), block->end(), i);
    it++;
    builder.set_insert_point(it);
    ir::instruction *trans = (ir::instruction*)builder.create_trans(i, perm);
    trans->set_operand(0, i);
    return trans;
  }
  return nullptr;
}

bool peephole::rewrite_trans_phi(ir::instruction* value, ir::builder& builder) {
  auto trans = dynamic_cast<ir::trans_inst*>(value);
  if(!trans)
    return false;
  auto users = trans->get_users();
  auto ops = trans->ops();
  if(users.size() > 1 || ops.size() > 1)
    return false;
  ir::value* op = *ops.begin();
  // trans(phi) -> phi(trans(), trans()...)
  auto* phi = dynamic_cast<ir::phi_node*>(op);
  if(!phi)
    return false;
  ir::value* new_phi = rewrite_trans_phi_impl(phi, builder, trans->get_perm());
  if(!new_phi)
    return false;
  trans->replace_all_uses_with(new_phi);

  return true;
}

bool peephole::rewrite_dot(ir::instruction *value, ir::builder& builder){
  // dot(a, b, 0) + c -> dot(a, b, c)
  auto add = dynamic_cast<ir::binary_operator*>(value);
  if(add && add->get_op() == ir::binary_op_t::FAdd) {
    ir::value *lhs = add->get_operand(0);
    ir::value *rhs = add->get_operand(1);
    ir::dot_inst *lhs_dot = dynamic_cast<ir::dot_inst*>(lhs);
    ir::dot_inst *rhs_dot = dynamic_cast<ir::dot_inst*>(rhs);
    if(!lhs_dot && !rhs_dot)
      return false;
    ir::dot_inst *dot = lhs_dot ? lhs_dot : rhs_dot;
    ir::value *other = (dot == lhs) ? rhs : lhs;
    ir::value *acc = dot->get_operand(2);
    ir::splat_inst *splat = dynamic_cast<ir::splat_inst*>(acc);
    ir::constant_fp *_0 = nullptr;
    if(splat)
      _0 = dynamic_cast<ir::constant_fp*>(splat->get_operand(0));
    if(!(_0 && _0->get_value() == 0.0))
      return false;
    ir::value *a = dot->get_operand(0);
    ir::value *b = dot->get_operand(1);
    builder.set_insert_point(add);
    ir::value * new_dot = builder.insert(ir::dot_inst::create_nn(a, b, other, dot->get_name()));
    add->replace_all_uses_with(new_dot);
    return true;
  }
}

bool peephole::rewrite_unit_red(ir::instruction *value, ir::builder& builder){
  auto x = dynamic_cast<ir::reduce_inst*>(value);
  if(!x)
    return false;
  ir::value *arg = x->get_operand(0);
  auto shapes = arg->get_type()->get_tile_shapes();
  if(shapes[x->get_axis()] == 1){
    builder.set_insert_point(x);
    ir::value* new_red = builder.create_reshape(arg, x->get_type()->get_tile_shapes());
    x->replace_all_uses_with(new_red);
    return true;
  }
  return false;
}

bool peephole::rewrite_mult(ir::instruction *value, ir::builder& builder) {
    auto binop = dynamic_cast<ir::binary_operator*>(value);
    if(binop && binop->get_op() == ir::binary_op_t::Mul) {
      ir::value *lhs = binop->get_operand(0);
      ir::value *rhs = binop->get_operand(1);
      ir::constant_int *_1_lhs = nullptr;
      if(ir::splat_inst *splat = dynamic_cast<ir::splat_inst*>(lhs))
        _1_lhs = dynamic_cast<ir::constant_int*>(splat->get_operand(0));
      ir::constant_int *_1_rhs = nullptr;
      if(ir::splat_inst *splat = dynamic_cast<ir::splat_inst*>(rhs))
        _1_rhs = dynamic_cast<ir::constant_int*>(splat->get_operand(0));
      if(_1_lhs){
        binop->replace_all_uses_with(rhs);
        return true;
      }
      else if(_1_rhs){
        binop->replace_all_uses_with(lhs);
        return true;
      }
    }
    return false;
}


bool peephole::rewrite_gep_ptr_min_off_plus_off(ir::instruction *value, ir::builder& builder) {
  auto x = dynamic_cast<ir::getelementptr_inst*>(value);
  if(!x)
    return false;
  auto y = dynamic_cast<ir::getelementptr_inst*>(x->get_pointer_operand());
  if(!y)
    return false;
  auto idx = *y->idx_begin();
  auto z = dynamic_cast<ir::binary_operator*>(idx);
  if(!z)
    return false;
  bool is_sub = z->get_op() == ir::binary_op_t::Sub;
  auto *lhs = dynamic_cast<ir::constant_int*>(z->get_operand(0));
  bool is_lhs_0 = lhs && (lhs->get_value()==0);
  bool is_rhs_eq_x_rhs = z->get_operand(1) == *x->idx_begin();
  if(is_sub && is_lhs_0 && is_rhs_eq_x_rhs){
    x->replace_all_uses_with(y->get_pointer_operand());
    return true;
  }
  return false;
}


void peephole::run(ir::module &mod) {
  ir::builder &builder = mod.get_builder();
  // keep track of whether any modification was made
  std::set<ir::value*> seen;
  size_t n_seen;

  // rewrite dots first
  do{
    n_seen = seen.size();
    for(ir::function *fn: mod.get_function_list())
    for(ir::basic_block *block: fn->blocks())
    for(ir::instruction* i: block->get_inst_list()){
      if(seen.find(i) != seen.end())
        continue;
      bool was_modified = rewrite_dot(i, builder);
      if(was_modified){
        seen.insert(i);
      }
    }
  }while(seen.size() != n_seen);

  // rewrite other ops
  seen.clear();
  do{
    n_seen = seen.size();
    for(ir::function *fn: mod.get_function_list())
    for(ir::basic_block *block: fn->blocks())
    for(ir::instruction* i: block->get_inst_list()){
      if(seen.find(i) != seen.end())
        continue;
      bool was_modified = false;
      was_modified = was_modified || rewrite_mult(i, builder);
      was_modified = was_modified || rewrite_trans_phi(i, builder);
      was_modified = was_modified || rewrite_unit_red(i, builder);
      was_modified = was_modified || rewrite_gep_ptr_min_off_plus_off(i, builder);
      if(was_modified)
        seen.insert(i);
    }
  }while(seen.size() != n_seen);
}

}
}
}