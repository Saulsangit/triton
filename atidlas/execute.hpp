#ifndef VIENNACL_DEVICE_SPECIFIC_EXECUTE_HPP
#define VIENNACL_DEVICE_SPECIFIC_EXECUTE_HPP

#include <cstring>
#include <vector>
#include <typeinfo>

#include "viennacl/tools/tools.hpp"
#include "viennacl/tools/timer.hpp"
#include "viennacl/scheduler/forwards.h"

#include "atidlas/forwards.h"
#include "atidlas/templates/template_base.hpp"
#include "atidlas/tree_parsing.hpp"
#include "atidlas/execution_handler.hpp"

namespace atidlas
{

inline void execute(template_base const & T, statements_container const & statements, viennacl::ocl::context & ctx = viennacl::ocl::current_context(), bool force_compilation = false)
{
  //Generate program name
  std::string program_name = tree_parsing::statements_representation(statements, BIND_TO_HANDLE);
  execution_handler handler(program_name, ctx, ctx.current_device(), force_compilation);
  handler.add(program_name, T, statements);
  handler.execute(program_name, statements);
}

}

#endif