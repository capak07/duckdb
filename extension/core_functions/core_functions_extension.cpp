#define DUCKDB_EXTENSION_MAIN
#include "core_functions_extension.hpp"
#include "function_list.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

void CoreFunctionsExtension::Load(DuckDB &ddb) {
	auto &db = *ddb.instance;
	auto &catalog = Catalog::GetSystemCatalog(db);
	CatalogTransaction transaction(db, 1, 0);
	auto functions = StaticFunctionDefinition::GetFunctionList();
	for(idx_t i = 0; functions[i].name; i++) {
		auto &function = functions[i];
		if (function.get_function) {
			auto scalar_function = function.get_function();
			scalar_function.name = function.name;
			CreateScalarFunctionInfo info(scalar_function);
			info.internal = true;
			catalog.CreateFunction(transaction, info);
		} else {
			throw InternalException("Do not know how to register function of this type");
		}
	}
}

std::string CoreFunctionsExtension::Name() {
	return "core_functions";
}

} // namespace duckdb

extern "C" {

DUCKDB_EXTENSION_API void core_functions_init(duckdb::DatabaseInstance &db) {
	duckdb::DuckDB db_wrapper(db);
	db_wrapper.LoadExtension<duckdb::CoreFunctionsExtension>();
}

DUCKDB_EXTENSION_API const char *core_functions_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
