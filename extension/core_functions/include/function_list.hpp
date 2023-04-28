//===----------------------------------------------------------------------===//
//                         DuckDB
//
// function_list.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

namespace duckdb {

typedef ScalarFunction (*get_scalar_function_t)();
typedef ScalarFunctionSet (*get_scalar_function_set_t)();

struct StaticFunctionDefinition {
	const char *name;
	const char *parameters;
	const char *description;
	const char *example;
	get_scalar_function_t get_function;
	get_scalar_function_set_t get_function_set;

	static StaticFunctionDefinition *GetFunctionList();
};

} // namespace duckdb
