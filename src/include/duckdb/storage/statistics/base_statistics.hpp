//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/statistics/base_statistics.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {
struct SelectionVector;

class Serializer;
class Deserializer;
class FieldWriter;
class FieldReader;
class Vector;
struct UnifiedVectorFormat;

enum class StatsInfo : uint8_t {
	CAN_HAVE_NULL_VALUES = 0,
	CANNOT_HAVE_NULL_VALUES = 1,
	CAN_HAVE_VALID_VALUES = 2,
	CANNOT_HAVE_VALID_VALUES = 3,
	CAN_HAVE_NULL_AND_VALID_VALUES = 4
};

class BaseStatistics {
	friend struct NumericStats;
	friend struct StringStats;
	friend struct StructStats;
	friend struct ListStats;

public:
	DUCKDB_API ~BaseStatistics();
	// disable copy constructors
	BaseStatistics(const BaseStatistics &other) = delete;
	BaseStatistics &operator=(const BaseStatistics &) = delete;
	//! enable move constructors
	DUCKDB_API BaseStatistics(BaseStatistics &&other) noexcept;
	DUCKDB_API BaseStatistics &operator=(BaseStatistics &&) noexcept;

public:
	//! Creates a set of statistics for data that is unknown, i.e. "has_null" is true, "has_no_null" is true, etc
	//! This can be used in case nothing is known about the data - or can be used as a baseline when only a few things
	//! are known
	static unique_ptr<BaseStatistics> CreateUnknown(LogicalType type);
	//! Creates statistics for an empty database, i.e. "has_null" is false, "has_no_null" is false, etc
	//! This is used when incrementally constructing statistics by constantly adding new values
	static unique_ptr<BaseStatistics> CreateEmpty(LogicalType type);

	DUCKDB_API bool CanHaveNull() const;
	DUCKDB_API bool CanHaveNoNull() const;

	void SetDistinctCount(idx_t distinct_count);

	bool IsConstant() const;

	const LogicalType &GetType() const {
		return type;
	}

	void Set(StatsInfo info);
	void CombineValidity(BaseStatistics &left, BaseStatistics &right);
	void CopyValidity(BaseStatistics &stats);
	void CopyValidity(BaseStatistics *stats);
	inline void SetHasNull() {
		has_null = true;
	}
	inline void SetHasNoNull() {
		has_no_null = true;
	}

	void Merge(const BaseStatistics &other);

	void Copy(const BaseStatistics &other);

	unique_ptr<BaseStatistics> Copy() const;
	unique_ptr<BaseStatistics> ToUnique() const;
	BaseStatistics CopyRegular() const;
	void CopyBase(const BaseStatistics &orig);

	void Serialize(Serializer &serializer) const;
	void Serialize(FieldWriter &writer) const;

	idx_t GetDistinctCount();

	static unique_ptr<BaseStatistics> Deserialize(Deserializer &source, LogicalType type);

	//! Verify that a vector does not violate the statistics
	void Verify(Vector &vector, const SelectionVector &sel, idx_t count) const;
	void Verify(Vector &vector, idx_t count) const;

	string ToString() const;

	static unique_ptr<BaseStatistics> FromConstant(const Value &input);

private:
	BaseStatistics();
	explicit BaseStatistics(LogicalType type);

	static unique_ptr<BaseStatistics> Construct(LogicalType type);
	static void Construct(BaseStatistics &stats, LogicalType type);

	void InitializeUnknown();
	void InitializeEmpty();

private:
	//! The type of the logical segment
	LogicalType type;
	//! Whether or not the segment can contain NULL values
	bool has_null;
	//! Whether or not the segment can contain values that are not null
	bool has_no_null;
	// estimate that one may have even if distinct_stats==nullptr
	idx_t distinct_count;
	//! Numeric and String stats
	union {
		//! Numeric stats data, for numeric stats
		NumericStatsData numeric_data;
		//! String stats data, for string stats
		StringStatsData string_data;
	} stats_union;
	//! Child stats (for LIST and STRUCT)
	unique_ptr<BaseStatistics[]> child_stats;
};

} // namespace duckdb
