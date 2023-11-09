#include "duckdb/catalog/catalog_set.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

void CatalogEntryMap::AddEntry(unique_ptr<CatalogEntry> entry) {
	auto name = entry->name;

	if (entries.find(name) != entries.end()) {
		throw InternalException("Entry with name \"%s\" already exists", name);
	}
	entries.insert(make_pair(name, std::move(entry)));
}

void CatalogEntryMap::UpdateEntry(unique_ptr<CatalogEntry> catalog_entry) {
	auto name = catalog_entry->name;

	auto entry = entries.find(name);
	if (entry == entries.end()) {
		throw InternalException("Entry with name \"%s\" does not exist", name);
	}

	auto existing = std::move(entry->second);
	entry->second = std::move(catalog_entry);
	entry->second->SetChild(std::move(existing));
}

case_insensitive_map_t<unique_ptr<CatalogEntry>> &CatalogEntryMap::Entries() {
	return entries;
}

void CatalogEntryMap::DropEntry(CatalogEntry &entry) {
	auto &name = entry.name;
	auto chain = GetEntry(name);
	if (!chain) {
		throw InternalException("Attempting to drop entry with name \"%s\" but no chain with that name exists", name);
	}
	auto child = entry.TakeChild();
	if (!entry.Parent()) {
		// This is the top of the chain
		D_ASSERT(chain.get() == &entry);
		auto it = entries.find(name);
		D_ASSERT(it != entries.end());

		// Remove the entry
		it->second.reset();
		if (child) {
			// Replace it with its child
			it->second = std::move(child);
		} else {
			entries.erase(it);
		}
	} else {
		// Just replace the entry with its child
		auto parent = entry.Parent();
		parent->SetChild(std::move(child));
	}
}

optional_ptr<CatalogEntry> CatalogEntryMap::GetEntry(const string &name) {
	auto entry = entries.find(name);
	if (entry == entries.end()) {
		return nullptr;
	}
	return entry->second.get();
}

CatalogSet::CatalogSet(Catalog &catalog_p, unique_ptr<DefaultGenerator> defaults)
    : catalog(catalog_p.Cast<DuckCatalog>()), defaults(std::move(defaults)) {
	D_ASSERT(catalog_p.IsDuckCatalog());
}
CatalogSet::~CatalogSet() {
}

bool IsDependencyEntry(CatalogEntry &entry) {
	return entry.type == CatalogType::DEPENDENCY_ENTRY || entry.type == CatalogType::DEPENDENCY_SET;
}

bool CatalogSet::CreateEntry(CatalogTransaction transaction, const string &name, unique_ptr<CatalogEntry> value,
                             const DependencyList &dependencies) {
	if (value->internal && !catalog.IsSystemCatalog() && name != DEFAULT_SCHEMA) {
		throw InternalException("Attempting to create internal entry \"%s\" in non-system catalog - internal entries "
		                        "can only be created in the system catalog",
		                        name);
	}
	if (!value->internal) {
		if (!value->temporary && catalog.IsSystemCatalog() && !IsDependencyEntry(*value)) {
			throw InternalException(
			    "Attempting to create non-internal entry \"%s\" in system catalog - the system catalog "
			    "can only contain internal entries",
			    name);
		}
		if (value->temporary && !catalog.IsTemporaryCatalog()) {
			throw InternalException("Attempting to create temporary entry \"%s\" in non-temporary catalog", name);
		}
		if (!value->temporary && catalog.IsTemporaryCatalog() && name != DEFAULT_SCHEMA) {
			throw InvalidInputException("Cannot create non-temporary entry \"%s\" in temporary catalog", name);
		}
	}

	// create a new entry and replace the currently stored one
	// set the timestamp to the timestamp of the current transaction
	// and point it at the dummy node
	value->timestamp = transaction.transaction_id;
	value->set = this;
	// now add the dependency set of this object to the dependency manager
	catalog.GetDependencyManager().AddObject(transaction, *value, dependencies);

	// lock the catalog for writing
	lock_guard<mutex> write_lock(catalog.GetWriteLock());
	// lock this catalog set to disallow reading
	unique_lock<mutex> read_lock(catalog_lock);

	// first check if the entry exists in the unordered set
	auto entry_value = map.GetEntry(name);
	if (!entry_value) {
		// if it does not: entry has never been created

		// check if there is a default entry
		auto entry = CreateDefaultEntry(transaction, name, read_lock);
		if (entry) {
			return false;
		}

		// first create a dummy deleted entry for this entry
		// so transactions started before the commit of this transaction don't
		// see it yet
		auto dummy_node = make_uniq<InCatalogEntry>(CatalogType::INVALID, value->ParentCatalog(), name);
		dummy_node->timestamp = 0;
		dummy_node->deleted = true;
		dummy_node->set = this;

		map.AddEntry(std::move(dummy_node));
	} else {
		auto &current = *entry_value;
		// if it does, we have to check version numbers
		if (HasConflict(transaction, current.timestamp)) {
			// current version has been written to by a currently active
			// transaction
			throw TransactionException("Catalog write-write conflict on create with \"%s\"", current.name);
		}
		// there is a current version that has been committed
		// if it has not been deleted there is a conflict
		if (!current.deleted) {
			return false;
		}
	}

	auto value_ptr = value.get();
	map.UpdateEntry(std::move(value));
	// push the old entry in the undo buffer for this transaction
	if (transaction.transaction) {
		auto &dtransaction = transaction.transaction->Cast<DuckTransaction>();
		dtransaction.PushCatalogEntry(value_ptr->Child());
	}
	return true;
}

bool CatalogSet::CreateEntry(ClientContext &context, const string &name, unique_ptr<CatalogEntry> value,
                             const DependencyList &dependencies) {
	return CreateEntry(catalog.GetCatalogTransaction(context), name, std::move(value), dependencies);
}

optional_ptr<CatalogEntry> CatalogSet::GetEntryInternal(CatalogTransaction transaction, CatalogEntry &catalog_entry) {
	// if it does: we have to retrieve the entry and to check version numbers
	if (HasConflict(transaction, catalog_entry.timestamp)) {
		// current version has been written to by a currently active
		// transaction
		throw TransactionException("Catalog write-write conflict on alter with \"%s\"", catalog_entry.name);
	}
	// there is a current version that has been committed by this transaction
	if (catalog_entry.deleted) {
		// if the entry was already deleted, it now does not exist anymore
		// so we return that we could not find it
		return nullptr;
	}
	return &catalog_entry;
}

optional_ptr<CatalogEntry> CatalogSet::GetEntryInternal(CatalogTransaction transaction, const string &name) {
	auto entry_value = map.GetEntry(name);
	if (!entry_value) {
		// the entry does not exist, check if we can create a default entry
		return nullptr;
	}
	return GetEntryInternal(transaction, *entry_value);
}

bool CatalogSet::AlterOwnership(CatalogTransaction transaction, ChangeOwnershipInfo &info) {
	// lock the catalog for writing
	unique_lock<mutex> write_lock(catalog.GetWriteLock());

	auto entry = GetEntryInternal(transaction, info.name);
	if (!entry) {
		return false;
	}

	auto &owner_entry = catalog.GetEntry(transaction.GetContext(), info.owner_schema, info.owner_name);
	write_lock.unlock();
	catalog.GetDependencyManager().AddOwnership(transaction, owner_entry, *entry);
	return true;
}

bool CatalogSet::AlterEntry(CatalogTransaction transaction, const string &name, AlterInfo &alter_info) {
	// lock the catalog for writing
	unique_lock<mutex> write_lock(catalog.GetWriteLock());
	// lock this catalog set to disallow reading
	unique_lock<mutex> read_lock(catalog_lock);

	// first check if the entry exists in the unordered set
	auto entry = GetEntryInternal(transaction, name);
	if (!entry) {
		return false;
	}
	if (!alter_info.allow_internal && entry->internal) {
		throw CatalogException("Cannot alter entry \"%s\" because it is an internal system entry", entry->name);
	}

	// create a new entry and replace the currently stored one
	// set the timestamp to the timestamp of the current transaction
	// and point it to the updated table node
	string original_name = entry->name;
	if (!transaction.context) {
		throw InternalException("Cannot AlterEntry without client context");
	}
	auto &context = *transaction.context;
	auto value = entry->AlterEntry(context, alter_info);
	if (!value) {
		// alter failed, but did not result in an error
		return true;
	}

	auto new_entry = value.get();
	value->timestamp = transaction.transaction_id;
	value->set = this;

	const bool name_changed = !StringUtil::CIEquals(value->name, original_name);
	if (name_changed) {
		auto entry_value = map.GetEntry(value->name);
		if (entry_value) {
			auto &original_entry = GetEntryForTransaction(transaction, *entry_value);
			if (!original_entry.deleted) {
				entry->UndoAlter(context, alter_info);
				string rename_err_msg =
				    "Could not rename \"%s\" to \"%s\": another entry with this name already exists!";
				throw CatalogException(rename_err_msg, original_name, value->name);
			}
		}
		DropEntryInternal(transaction, original_name, false, CatalogType::RENAMED_ENTRY);
		static const DependencyList EMPTY_DEPENDENCIES;
		read_lock.unlock();
		write_lock.unlock();

		// Create a dummy renamed entry for this entry
		// so in the cleanup/commit/rollback state we can identify that this was a rename
		auto renamed_node = make_uniq<InCatalogEntry>(CatalogType::RENAMED_ENTRY, value->ParentCatalog(), value->name);
		renamed_node->timestamp = transaction.transaction_id;
		renamed_node->deleted = false;
		renamed_node->set = this;

		// The 'renamed node' + 'value' get put into a different catalog entry chain
		CreateEntry(transaction, value->name, std::move(renamed_node), EMPTY_DEPENDENCIES);
		write_lock.lock();
		read_lock.lock();

		auto lookup = GetEntryInternal(transaction, value->name);
		D_ASSERT(lookup);
		map.UpdateEntry(std::move(value));
	} else {
		map.UpdateEntry(std::move(value));
	}

	// push the old entry in the undo buffer for this transaction
	if (transaction.transaction) {
		// serialize the AlterInfo into a temporary buffer
		MemoryStream stream;
		BinarySerializer serializer(stream);
		serializer.Begin();
		serializer.WriteProperty(100, "column_name", alter_info.GetColumnName());
		serializer.WriteProperty(101, "alter_info", &alter_info);
		serializer.End();

		auto &dtransaction = transaction.transaction->Cast<DuckTransaction>();
		dtransaction.PushCatalogEntry(new_entry->Child(), stream.GetData(), stream.GetPosition());
	}

	// Check the dependency manager to verify that there are no conflicting dependencies with this alter
	// Note that we do this AFTER the new entry has been entirely set up in the catalog set
	// that is because in case the alter fails because of a dependency conflict, we need to be able to cleanly roll back
	// to the old entry.
	read_lock.unlock();
	write_lock.unlock();
	catalog.GetDependencyManager().AlterObject(transaction, *entry, *new_entry);

	return true;
}

bool CatalogSet::DropDependencies(CatalogTransaction transaction, const string &name, bool cascade,
                                  bool allow_drop_internal) {
	auto entry = GetEntry(transaction, name);
	if (!entry) {
		return false;
	}
	if (entry->internal && !allow_drop_internal) {
		throw CatalogException("Cannot drop entry \"%s\" because it is an internal system entry", entry->name);
	}
	// check any dependencies of this object
	D_ASSERT(entry->ParentCatalog().IsDuckCatalog());
	auto &duck_catalog = entry->ParentCatalog().Cast<DuckCatalog>();
	duck_catalog.GetDependencyManager().DropObject(transaction, *entry, cascade);
	return true;
}

bool CatalogSet::DropEntryInternal(CatalogTransaction transaction, const string &name, bool allow_drop_internal,
                                   CatalogType tombstone_type) {
	// lock the catalog for writing
	// we can only delete an entry that exists
	auto entry = GetEntryInternal(transaction, name);
	if (!entry) {
		return false;
	}
	if (entry->internal && !allow_drop_internal) {
		throw CatalogException("Cannot drop entry \"%s\" because it is an internal system entry", entry->name);
	}

	// create a new tombstone entry and replace the currently stored one
	// set the timestamp to the timestamp of the current transaction
	// and point it at the tombstone node
	auto value = make_uniq<InCatalogEntry>(tombstone_type, entry->ParentCatalog(), entry->name);
	value->timestamp = transaction.transaction_id;
	value->set = this;
	value->deleted = true;
	auto value_ptr = value.get();
	map.UpdateEntry(std::move(value));

	// push the old entry in the undo buffer for this transaction
	if (transaction.transaction) {
		auto &dtransaction = transaction.transaction->Cast<DuckTransaction>();
		dtransaction.PushCatalogEntry(value_ptr->Child());
	}
	return true;
}

bool CatalogSet::DropEntry(CatalogTransaction transaction, const string &name, bool cascade, bool allow_drop_internal) {
	if (!DropDependencies(transaction, name, cascade, allow_drop_internal)) {
		return false;
	}
	lock_guard<mutex> write_lock(catalog.GetWriteLock());
	lock_guard<mutex> read_lock(catalog_lock);
	return DropEntryInternal(transaction, name, allow_drop_internal);
}

bool CatalogSet::DropEntry(ClientContext &context, const string &name, bool cascade, bool allow_drop_internal) {
	return DropEntry(catalog.GetCatalogTransaction(context), name, cascade, allow_drop_internal);
}

DuckCatalog &CatalogSet::GetCatalog() {
	return catalog;
}

void CatalogSet::CleanupEntry(CatalogEntry &catalog_entry) {
	// destroy the backed up entry: it is no longer required
	lock_guard<mutex> write_lock(catalog.GetWriteLock());
	lock_guard<mutex> lock(catalog_lock);
	auto &parent = *catalog_entry.Parent();
	map.DropEntry(catalog_entry);
	if (parent.deleted && !parent.HasChild() && !parent.HasParent()) {
		// The entry's parent is a tombstone and the entry had no child
		// clean up the mapping and the tombstone entry as well
		auto entry = map.GetEntry(parent.name);
		D_ASSERT(entry.get() == &parent);
		map.DropEntry(parent);
	}
}

bool CatalogSet::HasConflict(CatalogTransaction transaction, transaction_t timestamp) {
	return (timestamp >= TRANSACTION_ID_START && timestamp != transaction.transaction_id) ||
	       (timestamp < TRANSACTION_ID_START && timestamp > transaction.start_time);
}

bool CatalogSet::UseTimestamp(CatalogTransaction transaction, transaction_t timestamp) {
	if (timestamp == transaction.transaction_id) {
		// we created this version
		return true;
	}
	if (timestamp < transaction.start_time) {
		// this version was commited before we started the transaction
		return true;
	}
	return false;
}

CatalogEntry &CatalogSet::GetEntryForTransaction(CatalogTransaction transaction, CatalogEntry &current) {
	reference<CatalogEntry> entry(current);
	while (entry.get().HasChild()) {
		if (UseTimestamp(transaction, entry.get().timestamp)) {
			break;
		}
		entry = entry.get().Child();
	}
	return entry.get();
}

CatalogEntry &CatalogSet::GetCommittedEntry(CatalogEntry &current) {
	reference<CatalogEntry> entry(current);
	while (entry.get().HasChild()) {
		if (entry.get().timestamp < TRANSACTION_ID_START) {
			// this entry is committed: use it
			break;
		}
		entry = entry.get().Child();
	}
	return entry.get();
}

SimilarCatalogEntry CatalogSet::SimilarEntry(CatalogTransaction transaction, const string &name) {
	unique_lock<mutex> lock(catalog_lock);
	CreateDefaultEntries(transaction, lock);

	SimilarCatalogEntry result;
	for (auto &kv : map.Entries()) {
		auto ldist = StringUtil::SimilarityScore(kv.first, name);
		if (ldist < result.distance) {
			result.distance = ldist;
			result.name = kv.first;
		}
	}
	return result;
}

optional_ptr<CatalogEntry> CatalogSet::CreateEntryInternal(CatalogTransaction transaction,
                                                           unique_ptr<CatalogEntry> entry) {
	auto existing_entry = map.GetEntry(entry->name);

	if (existing_entry) {
		return nullptr;
	}
	auto catalog_entry = entry.get();

	entry->set = this;
	entry->timestamp = 0;
	map.AddEntry(std::move(entry));
	return catalog_entry;
}

optional_ptr<CatalogEntry> CatalogSet::CreateDefaultEntry(CatalogTransaction transaction, const string &name,
                                                          unique_lock<mutex> &lock) {
	// no entry found with this name, check for defaults
	if (!defaults || defaults->created_all_entries) {
		// no defaults either: return null
		return nullptr;
	}
	// this catalog set has a default map defined
	// check if there is a default entry that we can create with this name
	if (!transaction.context) {
		// no context - cannot create default entry
		return nullptr;
	}
	lock.unlock();
	auto entry = defaults->CreateDefaultEntry(*transaction.context, name);

	lock.lock();
	if (!entry) {
		// no default entry
		return nullptr;
	}
	// there is a default entry! create it
	auto result = CreateEntryInternal(transaction, std::move(entry));
	if (result) {
		return result;
	}
	// we found a default entry, but failed
	// this means somebody else created the entry first
	// just retry?
	lock.unlock();
	return GetEntry(transaction, name);
}

optional_ptr<CatalogEntry> CatalogSet::GetEntry(CatalogTransaction transaction, const string &name) {
	unique_lock<mutex> lock(catalog_lock);
	auto entry_value = map.GetEntry(name);
	if (entry_value) {
		// we found an entry for this name
		// check the version numbers

		auto &catalog_entry = *entry_value;
		auto &current = GetEntryForTransaction(transaction, catalog_entry);
		if (current.deleted) {
			return nullptr;
		}
		// if (!UseTimestamp(transaction, entry_value->GetTimestamp())) {
		//	return nullptr;
		//}
		D_ASSERT(StringUtil::CIEquals(name, current.name));
		return &current;
	}
	return CreateDefaultEntry(transaction, name, lock);
}

optional_ptr<CatalogEntry> CatalogSet::GetEntry(ClientContext &context, const string &name) {
	return GetEntry(catalog.GetCatalogTransaction(context), name);
}

void CatalogSet::UpdateTimestamp(CatalogEntry &entry, transaction_t timestamp) {
	entry.timestamp = timestamp;
}

void CatalogSet::Undo(CatalogEntry &entry) {
	lock_guard<mutex> write_lock(catalog.GetWriteLock());
	lock_guard<mutex> lock(catalog_lock);

	// entry has to be restored
	// and entry->parent has to be removed ("rolled back")

	// i.e. we have to place (entry) as (entry->parent) again
	auto &to_be_removed_node = *entry.Parent();

	D_ASSERT(StringUtil::CIEquals(entry.name, to_be_removed_node.name));
	if (!to_be_removed_node.HasParent()) {
		to_be_removed_node.Child().SetAsRoot();
	}
	map.DropEntry(to_be_removed_node);

	if (entry.type == CatalogType::INVALID) {
		// This was the root of the entry chain
		map.DropEntry(entry);
	}
	// we mark the catalog as being modified, since this action can lead to e.g. tables being dropped
	catalog.ModifyCatalog();
}

void CatalogSet::CreateDefaultEntries(CatalogTransaction transaction, unique_lock<mutex> &lock) {
	if (!defaults || defaults->created_all_entries || !transaction.context) {
		return;
	}
	// this catalog set has a default set defined:
	auto default_entries = defaults->GetDefaultEntries();
	for (auto &default_entry : default_entries) {
		auto entry_value = map.GetEntry(default_entry);
		if (!entry_value) {
			// we unlock during the CreateEntry, since it might reference other catalog sets...
			// specifically for views this can happen since the view will be bound
			lock.unlock();
			auto entry = defaults->CreateDefaultEntry(*transaction.context, default_entry);
			if (!entry) {
				throw InternalException("Failed to create default entry for %s", default_entry);
			}

			lock.lock();
			CreateEntryInternal(transaction, std::move(entry));
		}
	}
	defaults->created_all_entries = true;
}

void CatalogSet::Scan(CatalogTransaction transaction, const std::function<void(CatalogEntry &)> &callback) {
	// lock the catalog set
	unique_lock<mutex> lock(catalog_lock);
	CreateDefaultEntries(transaction, lock);

	for (auto &kv : map.Entries()) {
		auto &entry = *kv.second;
		auto &entry_for_transaction = GetEntryForTransaction(transaction, entry);
		if (!entry_for_transaction.deleted) {
			callback(entry_for_transaction);
		}
	}
}

void CatalogSet::Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback) {
	Scan(catalog.GetCatalogTransaction(context), callback);
}

void CatalogSet::Scan(const std::function<void(CatalogEntry &)> &callback) {
	// lock the catalog set
	lock_guard<mutex> lock(catalog_lock);
	for (auto &kv : map.Entries()) {
		auto &entry = *kv.second;
		auto &commited_entry = GetCommittedEntry(entry);
		if (!commited_entry.deleted) {
			callback(commited_entry);
		}
	}
}

void CatalogSet::Verify(Catalog &catalog_p) {
	D_ASSERT(&catalog_p == &catalog);
	vector<reference<CatalogEntry>> entries;
	Scan([&](CatalogEntry &entry) { entries.push_back(entry); });
	for (auto &entry : entries) {
		entry.get().Verify(catalog_p);
	}
}

} // namespace duckdb
