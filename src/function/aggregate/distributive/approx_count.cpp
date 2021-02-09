#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "hyperloglog.hpp"

namespace duckdb {
using namespace hll;
struct approx_distinct_count_state_t {
	HyperLogLog *log;
};

struct ApproxCountDistinctFunctionBase {
	template <class STATE>
	static void Initialize(STATE *state) {
		state->log = nullptr;
	}

	template <class STATE, class OP>
	static void Combine(STATE &source, STATE *target) {
		if (!source.log) {
			return;
		}
		if (!target->log) {
			target->log = source.log;
			source.log = nullptr;
			return;
		}
		target->log->merge(*source.log);
	}

	template <class T, class STATE>
	static void Finalize(Vector &result, FunctionData *, STATE *state, T *target, nullmask_t &nullmask, idx_t idx) {
		if (state->log) {
			target[idx] = state->log->estimate();
		} else {
			target[idx] = 0;
		}
	}

	static bool IgnoreNull() {
		return true;
	}
	template <class STATE>
	static void Destroy(STATE *state) {
		if (state->log) {
			delete state->log;
		}
	}
};

struct ApproxCountDistinctFunction : ApproxCountDistinctFunctionBase {
	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, nullmask_t &nullmask, idx_t idx) {
		if (!state->log) {
			state->log = new HyperLogLog(16);
		}
		if (nullmask[idx]) {
			return;
		}
		INPUT_TYPE value = input[idx];
		auto value_bytes = static_cast<char *>(static_cast<void *>(&value));
		state->log->add(value_bytes, sizeof(value));
	}
	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, nullmask_t &nullmask,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, bind_data, input, nullmask, 0);
		}
	}
};

struct ApproxCountDistinctFunctionString : ApproxCountDistinctFunctionBase {
	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, nullmask_t &nullmask, idx_t idx) {
		if (!state->log) {
			state->log = new HyperLogLog(16);
		}
		if (nullmask[idx]) {
			return;
		}
		string value = input[idx].GetString();
		auto val_c_str = value.c_str();
		state->log->add(val_c_str, value.size());
	}
	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, nullmask_t &nullmask,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, bind_data, input, nullmask, 0);
		}
	}
};

AggregateFunction GetApproxCountDistinctFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::UINT16:
		return AggregateFunction::UnaryAggregateDestructor<approx_distinct_count_state_t, uint16_t, int64_t,
		                                                   ApproxCountDistinctFunction>(LogicalType::UTINYINT,
		                                                                                LogicalType::BIGINT);
	case PhysicalType::UINT32:
		return AggregateFunction::UnaryAggregateDestructor<approx_distinct_count_state_t, uint32_t, int64_t,
		                                                   ApproxCountDistinctFunction>(LogicalType::UINTEGER,
		                                                                                LogicalType::BIGINT);
	case PhysicalType::UINT64:
		return AggregateFunction::UnaryAggregateDestructor<approx_distinct_count_state_t, uint64_t, int64_t,
		                                                   ApproxCountDistinctFunction>(LogicalType::UBIGINT,
		                                                                                LogicalType::BIGINT);
	case PhysicalType::INT16:
		return AggregateFunction::UnaryAggregateDestructor<approx_distinct_count_state_t, int16_t, int64_t,
		                                                   ApproxCountDistinctFunction>(LogicalType::TINYINT,
		                                                                                LogicalType::BIGINT);
	case PhysicalType::INT32:
		return AggregateFunction::UnaryAggregateDestructor<approx_distinct_count_state_t, int32_t, int64_t,
		                                                   ApproxCountDistinctFunction>(LogicalType::INTEGER,
		                                                                                LogicalType::BIGINT);
	case PhysicalType::INT64:
		return AggregateFunction::UnaryAggregateDestructor<approx_distinct_count_state_t, int64_t, int64_t,
		                                                   ApproxCountDistinctFunction>(LogicalType::BIGINT,
		                                                                                LogicalType::BIGINT);
	case PhysicalType::FLOAT:
		return AggregateFunction::UnaryAggregateDestructor<approx_distinct_count_state_t, float, int64_t,
		                                                   ApproxCountDistinctFunction>(LogicalType::FLOAT,
		                                                                                LogicalType::BIGINT);
	case PhysicalType::DOUBLE:
		return AggregateFunction::UnaryAggregateDestructor<approx_distinct_count_state_t, double, int64_t,
		                                                   ApproxCountDistinctFunction>(LogicalType::DOUBLE,
		                                                                                LogicalType::BIGINT);
	case PhysicalType::VARCHAR:
		return AggregateFunction::UnaryAggregateDestructor<approx_distinct_count_state_t, string_t, int64_t,
		                                                   ApproxCountDistinctFunctionString>(LogicalType::VARCHAR,
		                                                                                      LogicalType::BIGINT);

	default:
		throw NotImplementedException("Unimplemented approximate_count aggregate");
	}
}

void ApproxCountDistinctFun::RegisterFunction(BuiltinFunctions &set) {
	AggregateFunctionSet approx_count("approx_count_distinct");
	approx_count.AddFunction(GetApproxCountDistinctFunction(PhysicalType::UINT16));
	approx_count.AddFunction(GetApproxCountDistinctFunction(PhysicalType::UINT32));
	approx_count.AddFunction(GetApproxCountDistinctFunction(PhysicalType::UINT64));
	approx_count.AddFunction(GetApproxCountDistinctFunction(PhysicalType::FLOAT));
	approx_count.AddFunction(GetApproxCountDistinctFunction(PhysicalType::INT16));
	approx_count.AddFunction(GetApproxCountDistinctFunction(PhysicalType::INT32));
	approx_count.AddFunction(GetApproxCountDistinctFunction(PhysicalType::INT64));
	approx_count.AddFunction(GetApproxCountDistinctFunction(PhysicalType::DOUBLE));
	approx_count.AddFunction(GetApproxCountDistinctFunction(PhysicalType::VARCHAR));
	approx_count.AddFunction(AggregateFunction::UnaryAggregateDestructor<approx_distinct_count_state_t, int64_t,
	                                                                     int64_t, ApproxCountDistinctFunction>(
	    LogicalType::TIMESTAMP, LogicalType::BIGINT));
	set.AddFunction(approx_count);
}

} // namespace duckdb
