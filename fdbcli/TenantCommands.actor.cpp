/*
 * TenantCommands.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbcli/fdbcli.actor.h"

#include "fdbclient/FDBOptions.g.h"
#include "fdbclient/GenericManagementAPI.actor.h"
#include "fdbclient/IClientApi.h"
#include "fdbclient/Knobs.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "fdbclient/TenantManagement.actor.h"
#include "fdbclient/Schemas.h"

#include "flow/Arena.h"
#include "flow/FastRef.h"
#include "flow/ThreadHelper.actor.h"

#include "metacluster/Metacluster.h"

#include "flow/actorcompiler.h" // This must be the last #include.

namespace fdb_cli {

const KeyRangeRef tenantMapSpecialKeyRange("\xff\xff/management/tenant/map/"_sr, "\xff\xff/management/tenant/map0"_sr);
const KeyRangeRef tenantConfigSpecialKeyRange("\xff\xff/management/tenant/configure/"_sr,
                                              "\xff\xff/management/tenant/configure0"_sr);
const KeyRangeRef tenantRenameSpecialKeyRange("\xff\xff/management/tenant/rename/"_sr,
                                              "\xff\xff/management/tenant/rename0"_sr);

Optional<std::map<Standalone<StringRef>, Optional<Value>>>
parseTenantConfiguration(std::vector<StringRef> const& tokens, int startIndex, int endIndex, bool allowUnset) {
	std::map<Standalone<StringRef>, Optional<Value>> configParams;
	for (int tokenNum = startIndex; tokenNum < endIndex; ++tokenNum) {
		Optional<Value> value;

		StringRef token = tokens[tokenNum];
		StringRef param;
		if (allowUnset && token == "unset"_sr) {
			if (++tokenNum == tokens.size()) {
				fmt::print(stderr, "ERROR: `unset' specified without a configuration parameter.\n");
				return {};
			}
			param = tokens[tokenNum];
		} else {
			bool foundEquals = false;
			param = token.eat("=", &foundEquals);
			if (!foundEquals) {
				fmt::print(stderr,
				           "ERROR: invalid configuration string `{}'. String must specify a value using `='.\n",
				           param.toString().c_str());
				return {};
			}
			value = token;
		}

		if (configParams.count(param)) {
			fmt::print(
			    stderr, "ERROR: configuration parameter `{}' specified more than once.\n", param.toString().c_str());
			return {};
		}

		if (tokencmp(param, "tenant_group")) {
			configParams[param] = value;
		} else if (tokencmp(param, "assigned_cluster")) {
			configParams[param] = value;
		} else if (tokencmp(param, "tenant_state")) {
			if (!value.present() ||
			    value.compare(metacluster::tenantStateToString(metacluster::TenantState::READY)) != 0) {
				fmt::print(stderr,
				           "ERROR: only support setting tenant state back to `ready', but `{}' given.\n",
				           value.present() ? value.get().toString().c_str() : "null");
				return {};
			}
			configParams[param] = value;
		} else {
			fmt::print(stderr, "ERROR: unrecognized configuration parameter `{}'.\n", param.toString().c_str());
			return {};
		}
	}

	return configParams;
}

bool parseTenantListOptions(std::vector<StringRef> const& tokens,
                            int startIndex,
                            int& limit,
                            int& offset,
                            std::vector<metacluster::TenantState>& filters,
                            Optional<TenantGroupName>& tenantGroup,
                            bool& useJson) {
	for (int tokenNum = startIndex; tokenNum < tokens.size(); ++tokenNum) {
		Optional<Value> value;
		StringRef token = tokens[tokenNum];
		StringRef param;
		bool foundEquals;
		param = token.eat("=", &foundEquals);
		if (!foundEquals && !tokencmp(param, "JSON")) {
			fmt::print(stderr,
			           "ERROR: invalid option string `{}'. String must specify a value using `=' or be `JSON'.\n",
			           param.toString().c_str());
			return false;
		}
		value = token;
		if (tokencmp(param, "limit")) {
			int n = 0;
			if (sscanf(value.get().toString().c_str(), "%d%n", &limit, &n) != 1 || n != value.get().size() ||
			    limit <= 0) {
				fmt::print(stderr, "ERROR: invalid limit `{}'\n", token.toString().c_str());
				return false;
			}
		} else if (tokencmp(param, "offset")) {
			int n = 0;
			if (sscanf(value.get().toString().c_str(), "%d%n", &offset, &n) != 1 || n != value.get().size() ||
			    offset < 0) {
				fmt::print(stderr, "ERROR: invalid offset `{}'\n", token.toString().c_str());
				return false;
			}
		} else if (tokencmp(param, "state")) {
			auto filterStrings = value.get().splitAny(","_sr);
			try {
				for (auto sref : filterStrings) {
					filters.push_back(metacluster::stringToTenantState(sref.toString()));
				}
			} catch (Error& e) {
				fmt::print(stderr, "ERROR: unrecognized tenant state(s) `{}'.\n", value.get().toString());
				return false;
			}
		} else if (tokencmp(param, "tenant_group")) {
			tenantGroup = TenantGroupName(value.get().toString());
		} else if (tokencmp(param, "JSON")) {
			useJson = true;
		} else {
			fmt::print(stderr, "ERROR: unrecognized parameter `{}'.\n", param.toString().c_str());
			return false;
		}
	}
	return true;
}

Key makeConfigKey(TenantNameRef tenantName, StringRef configName) {
	return tenantConfigSpecialKeyRange.begin.withSuffix(Tuple().append(tenantName).append(configName).pack());
}

void applyConfigurationToSpecialKeys(Reference<ITransaction> tr,
                                     TenantNameRef tenantName,
                                     std::map<Standalone<StringRef>, Optional<Value>> configuration) {
	for (auto [configName, value] : configuration) {
		if (configName == "assigned_cluster"_sr) {
			fmt::print(stderr, "ERROR: assigned_cluster is only valid in metacluster configuration.\n");
			throw invalid_tenant_configuration();
		}
		if (value.present()) {
			tr->set(makeConfigKey(tenantName, configName), value.get());
		} else {
			tr->clear(makeConfigKey(tenantName, configName));
		}
	}
}

// tenant create command
ACTOR Future<bool> tenantCreateCommand(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	if (tokens.size() < 3 || tokens.size() > 6) {
		fmt::print("Usage: tenant create <NAME> [tenant_group=<TENANT_GROUP>] [assigned_cluster=<CLUSTER_NAME>] "
		           "[ignore_capacity_limit]\n\n");
		fmt::print("Creates a new tenant in the cluster with the specified name.\n");
		fmt::print("An optional group can be specified that will require this tenant\n");
		fmt::print("to be placed on the same cluster as other tenants in the same group.\n");
		fmt::print("An optional cluster name can be specified that this tenant will be placed in.\n");
		fmt::print("Optionally, `ignore_capacity_limit' can be specified together with `assigned_cluster' to allow "
		           "creation of a new tenant group on a cluster with no tenant group capacity remaining.\n");
		return false;
	}

	state Key tenantNameKey = tenantMapSpecialKeyRange.begin.withSuffix(tokens[2]);
	state Reference<ITransaction> tr = db->createTransaction();
	state bool doneExistenceCheck = false;

	state bool ignoreCapacityLimit = tokens.back() == "ignore_capacity_limit";
	int configurationEndIndex = tokens.size() - (ignoreCapacityLimit ? 1 : 0);

	state Optional<std::map<Standalone<StringRef>, Optional<Value>>> configuration =
	    parseTenantConfiguration(tokens, 3, configurationEndIndex, false);

	if (!configuration.present()) {
		return false;
	} else if (ignoreCapacityLimit && !configuration.get().contains("assigned_cluster"_sr)) {
		fmt::print(stderr, "ERROR: `ignore_capacity_limit' can only be used if `assigned_cluster' is set.\n");
		return false;
	}

	loop {
		try {
			tr->setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			state ClusterType clusterType = wait(TenantAPI::getClusterType(tr));
			if (clusterType == ClusterType::METACLUSTER_MANAGEMENT) {
				metacluster::MetaclusterTenantMapEntry tenantEntry;
				metacluster::AssignClusterAutomatically assignClusterAutomatically =
				    metacluster::AssignClusterAutomatically::True;
				for (auto const& [name, value] : configuration.get()) {
					if (name == "assigned_cluster"_sr) {
						assignClusterAutomatically = metacluster::AssignClusterAutomatically::False;
					}
					tenantEntry.configure(name, value);
				}
				tenantEntry.tenantName = tokens[2];
				wait(metacluster::createTenant(db,
				                               tenantEntry,
				                               assignClusterAutomatically,
				                               metacluster::IgnoreCapacityLimit(ignoreCapacityLimit)));
			} else {
				if (!doneExistenceCheck) {
					// Hold the reference to the standalone's memory
					state ThreadFuture<Optional<Value>> existingTenantFuture = tr->get(tenantNameKey);
					Optional<Value> existingTenant = wait(safeThreadFutureToFuture(existingTenantFuture));
					if (existingTenant.present()) {
						throw tenant_already_exists();
					}
					doneExistenceCheck = true;
				}

				tr->set(tenantNameKey, ValueRef());
				applyConfigurationToSpecialKeys(tr, tokens[2], configuration.get());
				wait(safeThreadFutureToFuture(tr->commit()));
			}

			break;
		} catch (Error& e) {
			state Error err(e);
			if (e.code() == error_code_special_keys_api_failure) {
				std::string errorMsgStr = wait(getSpecialKeysFailureErrorMessage(tr));
				fmt::print(stderr, "ERROR: {}\n", errorMsgStr.c_str());
				return false;
			}
			wait(safeThreadFutureToFuture(tr->onError(err)));
		}
	}

	fmt::print("The tenant `{}' has been created\n", printable(tokens[2]).c_str());
	return true;
}

// tenant delete command
ACTOR Future<bool> tenantDeleteCommand(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	if (tokens.size() != 3) {
		fmt::print("Usage: tenant delete <NAME>\n\n");
		fmt::print("Deletes a tenant from the cluster by name.\n");
		fmt::print("Deletion will be allowed only if the specified tenant contains no data.\n");
		return false;
	}

	state Key tenantNameKey = tenantMapSpecialKeyRange.begin.withSuffix(tokens[2]);
	state Reference<ITransaction> tr = db->createTransaction();
	state bool doneExistenceCheck = false;

	loop {
		try {
			tr->setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			state ClusterType clusterType = wait(TenantAPI::getClusterType(tr));
			if (clusterType == ClusterType::METACLUSTER_MANAGEMENT) {
				wait(metacluster::deleteTenant(db, tokens[2]));
			} else {
				if (!doneExistenceCheck) {
					// Hold the reference to the standalone's memory
					state ThreadFuture<Optional<Value>> existingTenantFuture = tr->get(tenantNameKey);
					Optional<Value> existingTenant = wait(safeThreadFutureToFuture(existingTenantFuture));
					if (!existingTenant.present()) {
						throw tenant_not_found();
					}
					doneExistenceCheck = true;
				}

				tr->clear(tenantNameKey);
				wait(safeThreadFutureToFuture(tr->commit()));
			}

			break;
		} catch (Error& e) {
			state Error err(e);
			if (e.code() == error_code_special_keys_api_failure) {
				std::string errorMsgStr = wait(getSpecialKeysFailureErrorMessage(tr));
				fmt::print(stderr, "ERROR: {}\n", errorMsgStr.c_str());
				return false;
			}
			wait(safeThreadFutureToFuture(tr->onError(err)));
		}
	}

	fmt::print("The tenant `{}' has been deleted\n", printable(tokens[2]).c_str());
	return true;
}

// tenant deleteID command
ACTOR Future<bool> tenantDeleteIdCommand(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	if (tokens.size() != 3) {
		fmt::print("Usage: tenant deleteId <ID>\n\n");
		fmt::print("Deletes a tenant from the cluster by ID.\n");
		fmt::print("Deletion will be allowed only if the specified tenant contains no data.\n");
		return false;
	}
	state Reference<ITransaction> tr = db->createTransaction();
	loop {
		try {
			tr->setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			state ClusterType clusterType = wait(TenantAPI::getClusterType(tr));
			int64_t tenantId;
			int n;
			if (clusterType != ClusterType::METACLUSTER_MANAGEMENT) {
				fmt::print(stderr, "ERROR: delete by ID should only be run on a management cluster.\n");
				return false;
			}
			if (sscanf(tokens[2].toString().c_str(), "%" PRId64 "%n", &tenantId, &n) != 1 || n != tokens[2].size() ||
			    tenantId < 0) {
				fmt::print(stderr, "ERROR: invalid ID `{}'\n", tokens[2].toString().c_str());
				return false;
			}
			wait(metacluster::deleteTenant(db, tenantId));

			break;
		} catch (Error& e) {
			state Error err(e);
			if (e.code() == error_code_special_keys_api_failure) {
				std::string errorMsgStr = wait(getSpecialKeysFailureErrorMessage(tr));
				fmt::print(stderr, "ERROR: {}\n", errorMsgStr.c_str());
				return false;
			}
			wait(safeThreadFutureToFuture(tr->onError(err)));
		}
	}

	fmt::print("The tenant with ID `{}' has been deleted\n", printable(tokens[2]).c_str());
	return true;
}

void tenantListOutputJson(std::map<TenantName, int64_t> tenants) {
	json_spirit::mArray tenantsArr;
	for (auto const& [tenantName, tenantId] : tenants) {
		json_spirit::mObject tenantObj;
		tenantObj["name"] = binaryToJson(tenantName);
		tenantObj["id"] = tenantId;
		tenantsArr.push_back(tenantObj);
	}

	json_spirit::mObject resultObj;
	resultObj["tenants"] = tenantsArr;
	resultObj["type"] = "success";

	fmt::print("{}\n", json_spirit::write_string(json_spirit::mValue(resultObj), json_spirit::pretty_print).c_str());
}

// tenant list command
ACTOR Future<bool> tenantListCommand(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	if (tokens.size() > 9) {
		fmt::print(
		    "Usage: tenant list [BEGIN] [END] "
		    "[limit=<LIMIT>|offset=<OFFSET>|state=<STATE1>,<STATE2>,...|tenant_group=<TENANT_GROUP>] [JSON] ...\n\n");
		fmt::print("Lists the tenants in a cluster.\n");
		fmt::print("Only tenants in the range BEGIN - END will be printed.\n");
		fmt::print("An optional LIMIT can be specified to limit the number of results (default 100).\n");
		fmt::print("Optionally skip over the first OFFSET results (default 0).\n");
		fmt::print("Optional comma-separated tenant state(s) can be provided to filter the list.\n");
		fmt::print("Optional tenant group can be provided to filter the list.\n");
		fmt::print("If JSON is specified, then the output will be in JSON format.\n");
		fmt::print("Specifying [offset] and [state] is only supported in a metacluster.\n");
		return false;
	}

	state StringRef beginTenant = ""_sr;
	state StringRef endTenant = "\xff\xff"_sr;
	state int limit = 100;
	state int offset = 0;
	state std::vector<metacluster::TenantState> filters;
	state Optional<TenantGroupName> tenantGroup;
	state bool useJson = false;

	if (tokens.size() >= 3) {
		beginTenant = tokens[2];
	}
	if (tokens.size() >= 4) {
		endTenant = tokens[3];
		if (endTenant <= beginTenant) {
			fmt::print(stderr, "ERROR: end must be larger than begin\n");
			return false;
		}
	}
	if (tokens.size() >= 5) {
		if (!parseTenantListOptions(tokens, 4, limit, offset, filters, tenantGroup, useJson)) {
			return false;
		}
	}

	state Key beginTenantKey = tenantMapSpecialKeyRange.begin.withSuffix(beginTenant);
	state Key endTenantKey = tenantMapSpecialKeyRange.begin.withSuffix(endTenant);
	state Reference<ITransaction> tr = db->createTransaction();

	loop {
		try {
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			state ClusterType clusterType = wait(TenantAPI::getClusterType(tr));
			state std::map<TenantName, int64_t> tenantInfo;
			// State filters only apply to calls from the management cluster
			// Tenant group filters can apply to management, data, and standalone clusters
			if (clusterType == ClusterType::METACLUSTER_MANAGEMENT) {
				std::vector<std::pair<TenantName, metacluster::MetaclusterTenantMapEntry>> tenants = wait(
				    metacluster::listTenantMetadata(db, beginTenant, endTenant, limit, offset, filters, tenantGroup));
				for (const auto& [tenantName, entry] : tenants) {
					tenantInfo[tenantName] = entry.id;
				}
			} else {
				if (tenantGroup.present()) {
					// For expediency: does not use special key space
					// TODO: add special key support
					std::vector<std::pair<TenantName, int64_t>> tenants =
					    wait(TenantAPI::listTenantGroupTenants(db, tenantGroup.get(), beginTenant, endTenant, limit));
					for (const auto& [tenantName, tenantId] : tenants) {
						tenantInfo[tenantName] = tenantId;
					}
				} else {
					// Hold the reference to the standalone's memory
					state ThreadFuture<RangeResult> kvsFuture =
					    tr->getRange(firstGreaterOrEqual(beginTenantKey), firstGreaterOrEqual(endTenantKey), limit);
					RangeResult tenants = wait(safeThreadFutureToFuture(kvsFuture));
					for (auto tenant : tenants) {
						TenantName tName = tenant.key.removePrefix(tenantMapSpecialKeyRange.begin);
						json_spirit::mValue jsonObject;
						json_spirit::read_string(tenant.value.toString(), jsonObject);
						JSONDoc jsonDoc(jsonObject);

						int64_t tId;
						jsonDoc.get("id", tId);
						tenantInfo[tName] = tId;
					}
				}
			}

			if (useJson) {
				tenantListOutputJson(tenantInfo);
			} else {
				if (tenantInfo.empty()) {
					if (tokens.size() == 2) {
						fmt::print("The cluster has no tenants\n");
					} else {
						fmt::print("The cluster has no tenants in the specified range\n");
					}
				}

				int index = 0;
				for (const auto& [tenantName, tenantId] : tenantInfo) {
					fmt::print("  {}. {}\n", ++index, printable(tenantName).c_str());
				}
			}

			return true;
		} catch (Error& e) {
			try {
				wait(safeThreadFutureToFuture(tr->onError(e)));
			} catch (Error& finalErr) {
				state std::string errorStr;
				if (finalErr.code() == error_code_special_keys_api_failure) {
					std::string str = wait(getSpecialKeysFailureErrorMessage(tr));
					errorStr = str;
				} else if (useJson) {
					errorStr = finalErr.what();
				} else {
					throw finalErr;
				}

				if (useJson) {
					json_spirit::mObject resultObj;
					resultObj["type"] = "error";
					resultObj["error"] = errorStr;
					fmt::print(
					    "{}\n",
					    json_spirit::write_string(json_spirit::mValue(resultObj), json_spirit::pretty_print).c_str());
				} else {
					fmt::print(stderr, "ERROR: {}\n", errorStr.c_str());
				}

				return false;
			}
		}
	}
}

void tenantGetCmdOutput(json_spirit::mValue jsonObject, bool useJson) {
	if (useJson) {
		json_spirit::mObject resultObj;
		resultObj["tenant"] = jsonObject;
		resultObj["type"] = "success";
		fmt::print("{}\n",
		           json_spirit::write_string(json_spirit::mValue(resultObj), json_spirit::pretty_print).c_str());
	} else {
		JSONDoc doc(jsonObject);

		int64_t id;
		std::string name;
		std::string prefix;
		std::string tenantState;
		std::string tenantLockState;
		std::string lockId;
		std::string tenantGroup;
		std::string assignedCluster;
		std::string error;

		doc.get("id", id);
		doc.get("prefix.printable", prefix);
		doc.get("lock_state", tenantLockState);

		bool hasName = doc.tryGet("name.printable", name);
		bool hasTenantState = doc.tryGet("tenant_state", tenantState);
		bool hasLockId = doc.tryGet("lock_id", lockId);
		bool hasTenantGroup = doc.tryGet("tenant_group.printable", tenantGroup);
		bool hasAssignedCluster = doc.tryGet("assigned_cluster.printable", assignedCluster);
		bool hasError = doc.tryGet("error", error);

		fmt::print("  id: {}\n", id);
		fmt::print("  prefix: {}\n", printable(prefix));

		if (hasName) {
			fmt::print("  name: {}\n", name);
		}

		if (hasTenantState) {
			fmt::print("  tenant state: {}\n", printable(tenantState));
		}

		fmt::print("  lock state: {}\n", tenantLockState);
		if (hasLockId) {
			fmt::print("  lock id: {}\n", lockId);
		}

		if (hasTenantGroup) {
			fmt::print("  tenant group: {}\n", tenantGroup);
		}
		if (hasAssignedCluster) {
			fmt::print("  assigned cluster: {}\n", printable(assignedCluster));
		}
		if (hasError) {
			fmt::print("  error: {}\n", error);
		}
	}
}

// tenant get command
ACTOR Future<bool> tenantGetCommand(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	if (tokens.size() < 3 || tokens.size() > 4 || (tokens.size() == 4 && tokens[3] != "JSON"_sr)) {
		fmt::print("Usage: tenant get <NAME> [JSON]\n\n");
		fmt::print("Prints metadata associated with the given tenant.\n");
		fmt::print("If JSON is specified, then the output will be in JSON format.\n");
		return false;
	}

	state bool useJson = tokens.size() == 4;
	state Key tenantNameKey = tenantMapSpecialKeyRange.begin.withSuffix(tokens[2]);
	state Reference<ITransaction> tr = db->createTransaction();

	loop {
		try {
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			state ClusterType clusterType = wait(TenantAPI::getClusterType(tr));
			state std::string tenantJson;
			if (clusterType == ClusterType::METACLUSTER_MANAGEMENT) {
				metacluster::MetaclusterTenantMapEntry entry = wait(metacluster::getTenantTransaction(tr, tokens[2]));
				tenantJson = entry.toJson();
			} else {
				// Hold the reference to the standalone's memory
				state ThreadFuture<Optional<Value>> tenantFuture = tr->get(tenantNameKey);
				Optional<Value> tenant = wait(safeThreadFutureToFuture(tenantFuture));
				if (!tenant.present()) {
					throw tenant_not_found();
				}
				tenantJson = tenant.get().toString();
			}
			json_spirit::mValue jsonObject;
			json_spirit::read_string(tenantJson, jsonObject);
			tenantGetCmdOutput(jsonObject, useJson);
			return true;
		} catch (Error& e) {
			try {
				wait(safeThreadFutureToFuture(tr->onError(e)));
			} catch (Error& finalErr) {
				state std::string errorStr;
				if (finalErr.code() == error_code_special_keys_api_failure) {
					std::string str = wait(getSpecialKeysFailureErrorMessage(tr));
					errorStr = str;
				} else if (useJson) {
					errorStr = finalErr.what();
				} else {
					throw finalErr;
				}

				if (useJson) {
					json_spirit::mObject resultObj;
					resultObj["type"] = "error";
					resultObj["error"] = errorStr;
					fmt::print(
					    "{}\n",
					    json_spirit::write_string(json_spirit::mValue(resultObj), json_spirit::pretty_print).c_str());
				} else {
					fmt::print(stderr, "ERROR: {}\n", errorStr.c_str());
				}

				return false;
			}
		}
	}
}

// tenant getId command
ACTOR Future<bool> tenantGetIdCommand(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	if (tokens.size() < 3 || tokens.size() > 4 || (tokens.size() == 4 && tokens[3] != "JSON"_sr)) {
		fmt::print("Usage: tenant getId <ID> [JSON]\n\n");
		fmt::print("Prints metadata associated with the given tenant ID.\n");
		fmt::print("If JSON is specified, then the output will be in JSON format.\n");
		return false;
	}

	state bool useJson = tokens.size() == 4;
	state int64_t tenantId;
	int n = 0;
	if (sscanf(tokens[2].toString().c_str(), "%" PRId64 "%n", &tenantId, &n) != 1 || n != tokens[2].size() ||
	    tenantId < 0) {
		fmt::print(stderr, "ERROR: invalid ID `{}'\n", tokens[2].toString().c_str());
		return false;
	}
	state Reference<ITransaction> tr = db->createTransaction();

	loop {
		try {
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			state ClusterType clusterType = wait(TenantAPI::getClusterType(tr));
			state std::string tenantJson;
			if (clusterType != ClusterType::METACLUSTER_MANAGEMENT) {
				TenantMapEntry entry = wait(TenantAPI::getTenantTransaction(tr, tenantId));
				tenantJson = entry.toJson();
			} else {
				metacluster::MetaclusterTenantMapEntry mEntry = wait(metacluster::getTenantTransaction(tr, tenantId));
				tenantJson = mEntry.toJson();
			}

			json_spirit::mValue jsonObject;
			json_spirit::read_string(tenantJson, jsonObject);
			tenantGetCmdOutput(jsonObject, useJson);
			return true;
		} catch (Error& e) {
			try {
				wait(safeThreadFutureToFuture(tr->onError(e)));
			} catch (Error& finalErr) {
				state std::string errorStr;
				if (finalErr.code() == error_code_special_keys_api_failure) {
					std::string str = wait(getSpecialKeysFailureErrorMessage(tr));
					errorStr = str;
				} else if (useJson) {
					errorStr = finalErr.what();
				} else {
					throw finalErr;
				}

				if (useJson) {
					json_spirit::mObject resultObj;
					resultObj["type"] = "error";
					resultObj["error"] = errorStr;
					fmt::print(
					    "{}\n",
					    json_spirit::write_string(json_spirit::mValue(resultObj), json_spirit::pretty_print).c_str());
				} else {
					fmt::print(stderr, "ERROR: {}\n", errorStr.c_str());
				}

				return false;
			}
		}
	}
}

// tenant configure command
ACTOR Future<bool> tenantConfigureCommand(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	if (tokens.size() < 4) {
		fmt::print(
		    "Usage: tenant configure <TENANT_NAME> <[unset] tenant_group[=<GROUP_NAME>]> [ignore_capacity_limit]\n\n");
		fmt::print("Updates the configuration for a tenant.\n");
		fmt::print("Use `tenant_group=<GROUP_NAME>' to change the tenant group that a\n");
		fmt::print("tenant is assigned to or `unset tenant_group' to remove a tenant from\n");
		fmt::print("its tenant group.\n");
		fmt::print("If `ignore_capacity_limit' is specified, a new tenant group can be\n");
		fmt::print("created or the tenant can be ungrouped on a cluster with no tenant group\n");
		fmt::print("capacity remaining\n");
		return false;
	}

	state bool ignoreCapacityLimit = tokens.back() == "ignore_capacity_limit";
	int configurationEndIndex = tokens.size() - (ignoreCapacityLimit ? 1 : 0);
	state Optional<std::map<Standalone<StringRef>, Optional<Value>>> configuration =
	    parseTenantConfiguration(tokens, 3, configurationEndIndex, true);

	if (!configuration.present()) {
		return false;
	}

	state Reference<ITransaction> tr = db->createTransaction();

	loop {
		try {
			tr->setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			ClusterType clusterType = wait(TenantAPI::getClusterType(tr));
			if (clusterType == ClusterType::METACLUSTER_MANAGEMENT) {
				wait(metacluster::configureTenant(
				    db, tokens[2], configuration.get(), metacluster::IgnoreCapacityLimit(ignoreCapacityLimit)));
			} else {
				applyConfigurationToSpecialKeys(tr, tokens[2], configuration.get());
				wait(safeThreadFutureToFuture(tr->commit()));
			}
			break;
		} catch (Error& e) {
			state Error err(e);
			if (e.code() == error_code_special_keys_api_failure) {
				std::string errorMsgStr = wait(getSpecialKeysFailureErrorMessage(tr));
				fmt::print(stderr, "ERROR: {}\n", errorMsgStr.c_str());
				return false;
			}
			wait(safeThreadFutureToFuture(tr->onError(err)));
		}
	}

	fmt::print("The configuration for tenant `{}' has been updated\n", printable(tokens[2]).c_str());
	return true;
}

// Helper function to extract tenant ID from json metadata string
int64_t getTenantId(Value metadata) {
	json_spirit::mValue jsonObject;
	json_spirit::read_string(metadata.toString(), jsonObject);
	JSONDoc doc(jsonObject);
	int64_t id;
	doc.get("id", id);
	return id;
}

// tenant rename command
ACTOR Future<bool> tenantRenameCommand(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	if (tokens.size() != 4) {
		fmt::print("Usage: tenant rename <OLD_NAME> <NEW_NAME>\n\n");
		fmt::print("Renames a tenant in the cluster. The old name must exist and the new\n");
		fmt::print("name must not exist in the cluster.\n");
		return false;
	}
	state Reference<ITransaction> tr = db->createTransaction();
	state Key tenantRenameKey = tenantRenameSpecialKeyRange.begin.withSuffix(tokens[2]);
	state Key tenantOldNameKey = tenantMapSpecialKeyRange.begin.withSuffix(tokens[2]);
	state Key tenantNewNameKey = tenantMapSpecialKeyRange.begin.withSuffix(tokens[3]);
	state bool firstTry = true;
	state int64_t id = -1;
	loop {
		try {
			tr->setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			state ClusterType clusterType = wait(TenantAPI::getClusterType(tr));
			if (clusterType == ClusterType::METACLUSTER_MANAGEMENT) {
				wait(metacluster::renameTenant(db, tokens[2], tokens[3]));
			} else {
				// Hold the reference to the standalone's memory
				state ThreadFuture<Optional<Value>> oldEntryFuture = tr->get(tenantOldNameKey);
				state ThreadFuture<Optional<Value>> newEntryFuture = tr->get(tenantNewNameKey);
				state Optional<Value> oldEntry = wait(safeThreadFutureToFuture(oldEntryFuture));
				state Optional<Value> newEntry = wait(safeThreadFutureToFuture(newEntryFuture));
				if (firstTry) {
					if (!oldEntry.present()) {
						throw tenant_not_found();
					}
					if (newEntry.present()) {
						throw tenant_already_exists();
					}
					// Store the id we see when first reading this key
					id = getTenantId(oldEntry.get());

					firstTry = false;
				} else {
					// If we got commit_unknown_result, the rename may have already occurred.
					if (newEntry.present()) {
						int64_t checkId = getTenantId(newEntry.get());
						if (id == checkId) {
							ASSERT(!oldEntry.present() || getTenantId(oldEntry.get()) != id);
							return true;
						}
						// If the new entry is present but does not match, then
						// the rename should fail, so we throw an error.
						throw tenant_already_exists();
					}
					if (!oldEntry.present()) {
						throw tenant_not_found();
					}
					int64_t checkId = getTenantId(oldEntry.get());
					// If the id has changed since we made our first attempt,
					// then it's possible we've already moved the tenant. Don't move it again.
					if (id != checkId) {
						throw tenant_not_found();
					}
				}
				tr->set(tenantRenameKey, tokens[3]);
				wait(safeThreadFutureToFuture(tr->commit()));
			}
			break;
		} catch (Error& e) {
			state Error err(e);
			if (e.code() == error_code_special_keys_api_failure) {
				std::string errorMsgStr = wait(getSpecialKeysFailureErrorMessage(tr));
				fmt::print(stderr, "ERROR: {}\n", errorMsgStr.c_str());
				return false;
			}
			wait(safeThreadFutureToFuture(tr->onError(err)));
		}
	}

	fmt::print(
	    "The tenant `{}' has been renamed to `{}'\n", printable(tokens[2]).c_str(), printable(tokens[3]).c_str());
	return true;
}

ACTOR Future<bool> tenantLockCommand(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	state UID uid;
	state Reference<ITransaction> tr;
	state StringRef name;
	state Key nameKey;
	state TenantAPI::TenantLockState desiredLockState;
	state int uidIdx;
	if (tokens[1] == "lock"_sr && (tokens.size() < 3 || tokens.size() > 5)) {
		fmt::print("Usage: tenant lock <NAME> [w|rw] [UID]\n\n");
		fmt::print("Locks a tenant for read-write or read-only with a given UID.\n");
		fmt::print("By default a read-write lock is created.\n");
		fmt::print("If no UID is passed, fdbcli will generate one.\n");
		fmt::print("UID has to be a 16-byte number represented in hex.\n");
		return false;
	} else if (tokens[1] == "unlock"_sr && tokens.size() != 4) {
		fmt::print("Usage: tenant unlock <NAME> <UID>\n\n");
		return false;
	}
	name = tokens[2];
	nameKey = tenantMapSpecialKeyRange.begin.withSuffix(name);
	if (tokens[1] == "unlock"_sr) {
		uidIdx = 3;
		desiredLockState = TenantAPI::TenantLockState::UNLOCKED;
	} else {
		uidIdx = 4;
		if (tokens.size() > 3) {
			if (tokens[3] == "w"_sr) {
				desiredLockState = TenantAPI::TenantLockState::READ_ONLY;
			} else if (tokens[3] == "rw"_sr) {
				desiredLockState = TenantAPI::TenantLockState::LOCKED;
			} else {
				fmt::print(stderr, "ERROR: Invalid lock type `{}'\n", tokens[3]);
				return false;
			}
		} else {
			desiredLockState = TenantAPI::TenantLockState::LOCKED;
		}
	}
	if (tokens.size() > uidIdx) {
		try {
			auto uidStr = tokens[uidIdx].toString();
			if (uidStr.size() < 32) {
				// UID::fromString expects the string to be exactly 32 characters long, but the uid might be shorter
				// if the most significant byte[s] are 0. So we need to pad
				uidStr.insert(0, 32 - uidStr.size(), '0');
			}
			uid = UID::fromStringThrowsOnFailure(uidStr);
		} catch (Error& e) {
			ASSERT(e.code() == error_code_operation_failed);
			fmt::print(stderr, "ERROR: Couldn't not parse `{}' as a valid UID", tokens[uidIdx].toString());
			return false;
		}
	} else {
		ASSERT(desiredLockState != TenantAPI::TenantLockState::UNLOCKED);
		uid = deterministicRandom()->randomUniqueID();
	}
	tr = db->createTransaction();
	loop {
		try {
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			ClusterType clusterType = wait(TenantAPI::getClusterType(tr));
			if (clusterType == ClusterType::METACLUSTER_MANAGEMENT) {
				wait(metacluster::changeTenantLockState(db, name, desiredLockState, uid));
			} else {
				tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				state ThreadFuture<Optional<Value>> tenantFuture = tr->get(nameKey);
				Optional<Value> entry = wait(safeThreadFutureToFuture(tenantFuture));
				if (!entry.present()) {
					fmt::print(stderr, "ERROR: Tenant `{}' does not exist\n", name);
					return false;
				}
				auto tenantId = getTenantId(entry.get());
				wait(TenantAPI::changeLockState(tr.getPtr(), tenantId, desiredLockState, uid));
				wait(safeThreadFutureToFuture(tr->commit()));
			}
			if (desiredLockState != TenantAPI::TenantLockState::UNLOCKED) {
				fmt::print("Locked tenant `{}' with UID `{}'\n", name.toString(), uid.toString());
			} else {
				fmt::print("Unlocked tenant `{}'\n", name.toString());
			}
			return true;
		} catch (Error& e) {
			if (e.code() == error_code_tenant_locked) {
				if (desiredLockState == TenantAPI::TenantLockState::UNLOCKED) {
					fmt::print(stderr, "ERROR: Wrong lock UID\n");
				} else {
					fmt::print(stderr, "ERROR: Tenant locked with a different UID\n");
				}
				return false;
			}
			wait(safeThreadFutureToFuture(tr->onError(e)));
		}
	}
}

// tenant command
Future<bool> tenantCommand(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	if (tokens.size() == 1) {
		printUsage(tokens[0]);
		return true;
	} else if (tokencmp(tokens[1], "create")) {
		return tenantCreateCommand(db, tokens);
	} else if (tokencmp(tokens[1], "delete")) {
		return tenantDeleteCommand(db, tokens);
	} else if (tokencmp(tokens[1], "deleteId")) {
		return tenantDeleteIdCommand(db, tokens);
	} else if (tokencmp(tokens[1], "list")) {
		return tenantListCommand(db, tokens);
	} else if (tokencmp(tokens[1], "get")) {
		return tenantGetCommand(db, tokens);
	} else if (tokencmp(tokens[1], "getId")) {
		return tenantGetIdCommand(db, tokens);
	} else if (tokencmp(tokens[1], "configure")) {
		return tenantConfigureCommand(db, tokens);
	} else if (tokencmp(tokens[1], "rename")) {
		return tenantRenameCommand(db, tokens);
	} else if (tokencmp(tokens[1], "lock")) {
		return tenantLockCommand(db, tokens);
	} else if (tokencmp(tokens[1], "unlock")) {
		return tenantLockCommand(db, tokens);
	} else {
		printUsage(tokens[0]);
		return true;
	}
}

Future<bool> tenantCommandForwarder(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	ASSERT(!tokens.empty() && (tokens[0].endsWith("tenant"_sr) || tokens[0].endsWith("tenants"_sr)));
	std::vector<StringRef> forwardedTokens = { "tenant"_sr,
		                                       tokens[0].endsWith("tenant"_sr) ? tokens[0].removeSuffix("tenant"_sr)
		                                                                       : tokens[0].removeSuffix("tenants"_sr) };
	for (int i = 1; i < tokens.size(); ++i) {
		forwardedTokens.push_back(tokens[i]);
	}

	return tenantCommand(db, forwardedTokens);
}

void tenantGenerator(const char* text,
                     const char* line,
                     std::vector<std::string>& lc,
                     std::vector<StringRef> const& tokens) {
	if (tokens.size() == 1) {
		const char* opts[] = { "create",    "delete", "deleteId", "list",   "get",
			                   "configure", "rename", "lock",     "unlock", nullptr };
		arrayGenerator(text, line, opts, lc);
	} else if (tokens.size() >= 3 && tokencmp(tokens[1], "create")) {
		const char* opts[] = { "tenant_group=", "assigned_cluster=", "ignore_capacity_limit", nullptr };
		arrayGenerator(text, line, opts, lc);
	} else if (tokens.size() == 3 && tokencmp(tokens[1], "get")) {
		const char* opts[] = { "JSON", nullptr };
		arrayGenerator(text, line, opts, lc);
	} else if (tokens.size() == 3 && tokencmp(tokens[1], "getId")) {
		const char* opts[] = { "JSON", nullptr };
		arrayGenerator(text, line, opts, lc);
	} else if (tokens.size() >= 4 && tokencmp(tokens[1], "list")) {
		const char* opts[] = { "limit=", "offset=", "state=", "tenant_group=", "JSON", nullptr };
		arrayGenerator(text, line, opts, lc);
	} else if (tokencmp(tokens[1], "configure")) {
		if (tokens.size() == 3) {
			const char* opts[] = { "tenant_group=", "unset", nullptr };
			arrayGenerator(text, line, opts, lc);
		} else if (tokens.size() == 4 && tokencmp(tokens[3], "unset")) {
			const char* opts[] = { "tenant_group=", nullptr };
			arrayGenerator(text, line, opts, lc);
		} else if (tokens.size() == 4 + tokencmp(tokens[3], "unset")) {
			const char* opts[] = { "ignore_capacity_limit", nullptr };
			arrayGenerator(text, line, opts, lc);
		}
	} else if (tokencmp(tokens[1], "lock")) {
		if (tokens.size() == 3) {
			const char* opts[] = { "w", "rw", nullptr };
			arrayGenerator(text, line, opts, lc);
		}
	}
}

std::vector<const char*> tenantHintGenerator(std::vector<StringRef> const& tokens, bool inArgument) {
	if (tokens.size() == 1) {
		return { "<create|delete|deleteId|list|get|getId|configure|rename>", "[ARGS]" };
	} else if (tokencmp(tokens[1], "create") && tokens.size() < 5) {
		static std::vector<const char*> opts = {
			"<NAME>", "[tenant_group=<TENANT_GROUP>]", "[assigned_cluster=<CLUSTER_NAME>]", "[ignore_capacity_limit]"
		};
		return std::vector<const char*>(opts.begin() + tokens.size() - 2, opts.end());
	} else if (tokencmp(tokens[1], "delete") && tokens.size() < 3) {
		static std::vector<const char*> opts = { "<NAME>" };
		return std::vector<const char*>(opts.begin() + tokens.size() - 2, opts.end());
	} else if (tokencmp(tokens[1], "deleteId") && tokens.size() < 3) {
		static std::vector<const char*> opts = { "<ID>" };
		return std::vector<const char*>(opts.begin() + tokens.size() - 2, opts.end());
	} else if (tokencmp(tokens[1], "list") && tokens.size() < 7) {
		static std::vector<const char*> opts = { "[BEGIN]",
			                                     "[END]",
			                                     "[limit=LIMIT]",
			                                     "[offset=OFFSET]",
			                                     "[state=<STATE1>,<STATE2>,...]",
			                                     "[tenant_group=TENANT_GROUP]",
			                                     "[JSON]" };
		return std::vector<const char*>(opts.begin() + tokens.size() - 2, opts.end());
	} else if (tokencmp(tokens[1], "get") && tokens.size() < 4) {
		static std::vector<const char*> opts = { "<NAME>", "[JSON]" };
		return std::vector<const char*>(opts.begin() + tokens.size() - 2, opts.end());
	} else if (tokencmp(tokens[1], "getId") && tokens.size() < 4) {
		static std::vector<const char*> opts = { "<ID>", "[JSON]" };
		return std::vector<const char*>(opts.begin() + tokens.size() - 2, opts.end());
	} else if (tokencmp(tokens[1], "configure")) {
		if (tokens.size() < 4) {
			static std::vector<const char*> opts = { "<TENANT_NAME>",
				                                     "<[unset] tenant_group[=<GROUP_NAME>]>",
				                                     "[ignore_capacity_limit]" };
			return std::vector<const char*>(opts.begin() + tokens.size() - 2, opts.end());
		} else if ("unset"_sr.startsWith(tokens[3]) && tokens[3].size() <= 5) {
			if (tokens.size() < 6) {
				static std::vector<const char*> opts = { "<tenant_group[=<GROUP_NAME>]>", "[ignore_capacity_limit]" };
				return std::vector<const char*>(opts.begin() + tokens.size() - 4, opts.end());
			}
		} else if (tokens.size() == 4) {
			static std::vector<const char*> opts = { "[ignore_capacity_limit]" };
			return opts;
		}
		return {};
	} else if (tokencmp(tokens[1], "rename") && tokens.size() < 4) {
		static std::vector<const char*> opts = { "<OLD_NAME>", "<NEW_NAME>" };
		return std::vector<const char*>(opts.begin() + tokens.size() - 2, opts.end());
	} else if (tokencmp(tokens[1], "lock") && tokens.size() < 5) {
		static std::vector<const char*> opts = { "<NAME>", "[w|rw]", "[UID]" };
		return std::vector<const char*>(opts.begin() + tokens.size() - 2, opts.end());
	} else if (tokencmp(tokens[1], "unlock") && tokens.size() < 4) {
		static std::vector<const char*> opts = { "<NAME>", "<UID>" };
		return std::vector<const char*>(opts.begin() + tokens.size() - 2, opts.end());
	} else {
		return {};
	}
}

CommandFactory tenantRegisterFactory(
    "tenant",
    CommandHelp("tenant <create|delete|list|get|getId|configure|rename|lock|unlock> [ARGS]",
                "view and manage tenants in a cluster or metacluster",
                "`create' and `delete' add and remove tenants from the cluster.\n"
                "`list' prints a list of tenants in the cluster.\n"
                "`get' prints the metadata for a particular tenant.\n"
                "`configure' modifies the configuration for a tenant.\n"
                "`rename' changes the name of a tenant.\n"
                "`lock` locks a tenant.\n"
                "`unlock` unlocks a tenant.\n"),
    &tenantGenerator,
    &tenantHintGenerator);

// Generate hidden commands for the old versions of the tenant commands
CommandFactory createTenantFactory("createtenant");
CommandFactory deleteTenantFactory("deletetenant");
CommandFactory listTenantsFactory("listtenants");
CommandFactory getTenantFactory("gettenant");
CommandFactory configureTenantFactory("configuretenant");
CommandFactory renameTenantFactory("renametenant");

} // namespace fdb_cli
