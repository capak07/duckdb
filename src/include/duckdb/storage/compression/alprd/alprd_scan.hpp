//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/compression/alp/alp_scan.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/compression/alprd/alprd.hpp"
#include "duckdb/storage/compression/alprd/algorithm/alprd.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/function/compression/compression.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace duckdb {

template <class T>
struct AlpRDGroupState {
public:
	using EXACT_TYPE = typename FloatingToExact<T>::type;

	void Reset() {
		index = 0;
	}

	// This is the scan of the data itself, values must have the decoded vector
	template <bool SKIP = false>
	void Scan(uint8_t *dest, idx_t count) {
		if (!SKIP) {
			memcpy(dest, (void *)(values + index), sizeof(T) * count);
		}
		index += count;
	}

	template <bool SKIP>
	void LoadValues(EXACT_TYPE *value_buffer, idx_t count) {
		if (SKIP) {
			return;
		}
		value_buffer[0] = (EXACT_TYPE)0;
		alp::AlpRDDecompression<T>::Decompress(
		    left_encoded, right_encoded, dict, value_buffer, count,
		    exceptions_count, exceptions, exceptions_positions, right_bit_width);
	}

public:
	idx_t index;
	uint8_t left_encoded[AlpRDConstants::ALP_VECTOR_SIZE * 8];
	uint8_t right_encoded[AlpRDConstants::ALP_VECTOR_SIZE * 8];
	EXACT_TYPE values[AlpRDConstants::ALP_VECTOR_SIZE];
	uint16_t exceptions[AlpRDConstants::ALP_VECTOR_SIZE];
	uint16_t exceptions_positions[AlpRDConstants::ALP_VECTOR_SIZE];
	uint16_t exceptions_count;
	uint8_t right_bit_width;
	uint16_t dict[AlpRDConstants::DICTIONARY_SIZE];
};

template <class T>
struct AlpRDScanState : public SegmentScanState {
public:
	using EXACT_TYPE = typename FloatingToExact<T>::type;

	explicit AlpRDScanState(ColumnSegment &segment) : segment(segment), count(segment.count) {
		auto &buffer_manager = BufferManager::GetBufferManager(segment.db);

		handle = buffer_manager.Pin(segment.block);
		// ScanStates never exceed the boundaries of a Segment,
		// but are not guaranteed to start at the beginning of the Block
		segment_data = handle.Ptr() + segment.GetBlockOffset();
		auto metadata_offset = Load<uint32_t>(segment_data);
		metadata_ptr = segment_data + metadata_offset;

		// Load Right Bit Width which is on the header after the pointer to the first metadata
		group_state.right_bit_width = Load<uint8_t>(segment_data + AlpRDConstants::METADATA_POINTER_SIZE);

		// Load dictionary
		memcpy(group_state.dict, (void*) (segment_data + AlpRDConstants::HEADER_SIZE), AlpRDConstants::DICTIONARY_SIZE_BYTES);

	}

	BufferHandle handle;
	data_ptr_t metadata_ptr;
	data_ptr_t segment_data;
	idx_t total_value_count = 0;
	AlpRDGroupState<T> group_state;

	ColumnSegment &segment;
	idx_t count;

	idx_t LeftInGroup() const {
		return AlpRDConstants::ALP_VECTOR_SIZE - (total_value_count % AlpRDConstants::ALP_VECTOR_SIZE);
	}

	inline bool GroupFinished() const {
		return (total_value_count % AlpRDConstants::ALP_VECTOR_SIZE) == 0;
	}

	// Scan up to a group boundary
	template <class EXACT_TYPE, bool SKIP = false>
	void ScanGroup(EXACT_TYPE *values, idx_t group_size) {
		D_ASSERT(group_size <= AlpRDConstants::ALP_VECTOR_SIZE);
		D_ASSERT(group_size <= LeftInGroup());
		if (GroupFinished() && total_value_count < count) {
			if (group_size == AlpRDConstants::ALP_VECTOR_SIZE) {
				LoadGroup<SKIP>(values);
				total_value_count += group_size;
				return;
			} else {
				// Even if SKIP is given, group size is not big enough to be able to fully skip the entire group
				LoadGroup<false>(group_state.values);
			}
		}
		group_state.template Scan<SKIP>((uint8_t *)values, group_size);

		total_value_count += group_size;
	}

	// Using the metadata, we can avoid loading any of the data if we don't care about the group at all
	void SkipGroup() {
		// Skip the offset indicating where the data starts
		metadata_ptr -= AlpRDConstants::METADATA_POINTER_SIZE;
		idx_t group_size = MinValue((idx_t)AlpRDConstants::ALP_VECTOR_SIZE, count - total_value_count);
		total_value_count += group_size;
	}

	template <bool SKIP = false>
	void LoadGroup(EXACT_TYPE *value_buffer) {
		group_state.Reset();

		// Load the offset indicating where a groups data starts
		metadata_ptr -= AlpRDConstants::METADATA_POINTER_SIZE;
		auto data_byte_offset = Load<uint32_t>(metadata_ptr);
		D_ASSERT(data_byte_offset < Storage::BLOCK_SIZE);

		idx_t group_size = MinValue((idx_t)AlpRDConstants::ALP_VECTOR_SIZE, (count - total_value_count));

		data_ptr_t group_ptr = segment_data + data_byte_offset;
		group_state.exceptions_count = Load<uint16_t>(group_ptr);
		group_ptr += AlpRDConstants::EXCEPTIONS_COUNT_SIZE;

		D_ASSERT(group_state.exceptions_count <= group_size);

		auto left_bp_size = BitpackingPrimitives::GetRequiredSize(group_size, AlpRDConstants::DICTIONARY_BW);
		auto right_bp_size = BitpackingPrimitives::GetRequiredSize(group_size, group_state.right_bit_width);

		memcpy(group_state.left_encoded, (void*) group_ptr, left_bp_size);
		group_ptr += left_bp_size;

		memcpy(group_state.right_encoded, (void*) group_ptr, right_bp_size);
		group_ptr += right_bp_size;

		if (group_state.exceptions_count > 0){
			memcpy(group_state.exceptions, (void*) group_ptr, AlpRDConstants::EXCEPTION_SIZE * group_state.exceptions_count);
			group_ptr += AlpRDConstants::EXCEPTION_SIZE * group_state.exceptions_count;
			memcpy(group_state.exceptions_positions, (void*) group_ptr, AlpRDConstants::EXCEPTION_POSITION_SIZE * group_state.exceptions_count);
		}

		// Read all the values to the specified 'value_buffer'
		group_state.template LoadValues<SKIP>(value_buffer, group_size);
	}

public:
	//! Skip the next 'skip_count' values, we don't store the values
	void Skip(ColumnSegment &col_segment, idx_t skip_count) {

		if (total_value_count != 0 && !GroupFinished()) {
			// Finish skipping the current group
			idx_t to_skip = LeftInGroup();
			skip_count -= to_skip;
			ScanGroup<EXACT_TYPE, true>(nullptr, to_skip);
		}
		// Figure out how many entire groups we can skip
		// For these groups, we don't even need to process the metadata or values
		idx_t groups_to_skip = skip_count / AlpRDConstants::ALP_VECTOR_SIZE;
		for (idx_t i = 0; i < groups_to_skip; i++) {
			SkipGroup();
		}
		skip_count -= AlpRDConstants::ALP_VECTOR_SIZE * groups_to_skip;
		if (skip_count == 0) {
			return;
		}
		// For the last group that this skip (partially) touches, we do need to
		// load the metadata and values into the group_state because
		// we don't know exactly how many they are
		ScanGroup<EXACT_TYPE, true>(nullptr, skip_count);
	}
};

template <class T>
unique_ptr<SegmentScanState> AlpRDInitScan(ColumnSegment &segment) {
	auto result = make_uniq_base<SegmentScanState, AlpRDScanState<T>>(segment);
	return result;
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//
template <class T>
void AlpRDScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                      idx_t result_offset) {
	using EXACT_TYPE = typename FloatingToExact<T>::type;
	auto &scan_state = (AlpRDScanState<T> &)*state.scan_state;

	// Get the pointer to the result values
	auto current_result_ptr = FlatVector::GetData<EXACT_TYPE>(result);
	result.SetVectorType(VectorType::FLAT_VECTOR);
	current_result_ptr += result_offset;

	idx_t scanned = 0;
	while (scanned < scan_count) {
		const auto remaining = scan_count - scanned;
		const idx_t to_scan = MinValue(remaining, scan_state.LeftInGroup());

		scan_state.template ScanGroup<EXACT_TYPE>(current_result_ptr + scanned, to_scan);
		scanned += to_scan;
	}
}

template <class T>
void AlpRDSkip(ColumnSegment &segment, ColumnScanState &state, idx_t skip_count) {
	auto &scan_state = (AlpRDScanState<T> &)*state.scan_state;
	scan_state.Skip(segment, skip_count);
}

template <class T>
void AlpRDScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result) {
	AlpRDScanPartial<T>(segment, state, scan_count, result, 0);
}

} // namespace duckdb
