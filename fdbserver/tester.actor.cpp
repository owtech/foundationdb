/*
 * tester.actor.cpp
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

#include <boost/algorithm/string/predicate.hpp>
#include <cinttypes>
#include <fstream>
#include <functional>
#include <map>
#include <toml.hpp>

#include "flow/ActorCollection.h"
#include "fdbrpc/sim_validation.h"
#include "fdbrpc/simulator.h"
#include "fdbclient/ConsistencyCheckUtil.actor.h"
#include "fdbclient/ClusterInterface.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/SystemData.h"
#include "fdbserver/KnobProtectiveGroups.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/Status.h"
#include "fdbserver/QuietDatabase.h"
#include "fdbclient/MonitorLeader.h"
#include "fdbserver/CoordinationInterface.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "flow/actorcompiler.h" // This must be the last #include.

WorkloadContext::WorkloadContext() {}

WorkloadContext::WorkloadContext(const WorkloadContext& r)
  : options(r.options), clientId(r.clientId), clientCount(r.clientCount), sharedRandomNumber(r.sharedRandomNumber),
    dbInfo(r.dbInfo), rangesToCheck(r.rangesToCheck) {}

WorkloadContext::~WorkloadContext() {}

const char HEX_CHAR_LOOKUP[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

void emplaceIndex(uint8_t* data, int offset, int64_t index) {
	for (int i = 0; i < 16; i++) {
		data[(15 - i) + offset] = HEX_CHAR_LOOKUP[index & 0xf];
		index = index >> 4;
	}
}

Key doubleToTestKey(double p) {
	return StringRef(format("%016llx", *(uint64_t*)&p));
}

double testKeyToDouble(const KeyRef& p) {
	uint64_t x = 0;
	sscanf(p.toString().c_str(), "%" SCNx64, &x);
	return *(double*)&x;
}

Key doubleToTestKey(double p, const KeyRef& prefix) {
	return doubleToTestKey(p).withPrefix(prefix);
}

Key KVWorkload::getRandomKey() const {
	return getRandomKey(absentFrac);
}

Key KVWorkload::getRandomKey(double absentFrac) const {
	if (absentFrac > 0.0000001) {
		return getRandomKey(deterministicRandom()->random01() < absentFrac);
	} else {
		return getRandomKey(false);
	}
}

Key KVWorkload::getRandomKey(bool absent) const {
	return keyForIndex(deterministicRandom()->randomInt(0, nodeCount), absent);
}

Key KVWorkload::keyForIndex(uint64_t index) const {
	if (absentFrac > 0.0000001) {
		return keyForIndex(index, deterministicRandom()->random01() < absentFrac);
	} else {
		return keyForIndex(index, false);
	}
}

Key KVWorkload::keyForIndex(uint64_t index, bool absent) const {
	int adjustedKeyBytes = (absent) ? (keyBytes + 1) : keyBytes;
	Key result = makeString(adjustedKeyBytes);
	uint8_t* data = mutateString(result);
	memset(data, '.', adjustedKeyBytes);

	int idx = 0;
	if (nodePrefix > 0) {
		ASSERT(keyBytes >= 32);
		emplaceIndex(data, 0, nodePrefix);
		idx += 16;
	}
	ASSERT(keyBytes >= 16);
	double d = double(index) / nodeCount;
	emplaceIndex(data, idx, *(int64_t*)&d);

	return result;
}

double testKeyToDouble(const KeyRef& p, const KeyRef& prefix) {
	return testKeyToDouble(p.removePrefix(prefix));
}

ACTOR Future<Void> poisson(double* last, double meanInterval) {
	*last += meanInterval * -log(deterministicRandom()->random01());
	wait(delayUntil(*last));
	return Void();
}

ACTOR Future<Void> uniform(double* last, double meanInterval) {
	*last += meanInterval;
	wait(delayUntil(*last));
	return Void();
}

Value getOption(VectorRef<KeyValueRef> options, Key key, Value defaultValue) {
	for (int i = 0; i < options.size(); i++)
		if (options[i].key == key) {
			Value value = options[i].value;
			options[i].value = LiteralStringRef("");
			return value;
		}

	return defaultValue;
}

int getOption(VectorRef<KeyValueRef> options, Key key, int defaultValue) {
	for (int i = 0; i < options.size(); i++)
		if (options[i].key == key) {
			int r;
			if (sscanf(options[i].value.toString().c_str(), "%d", &r)) {
				options[i].value = LiteralStringRef("");
				return r;
			} else {
				TraceEvent(SevError, "InvalidTestOption").detail("OptionName", key);
				throw test_specification_invalid();
			}
		}

	return defaultValue;
}

uint64_t getOption(VectorRef<KeyValueRef> options, Key key, uint64_t defaultValue) {
	for (int i = 0; i < options.size(); i++)
		if (options[i].key == key) {
			uint64_t r;
			if (sscanf(options[i].value.toString().c_str(), "%" SCNd64, &r)) {
				options[i].value = LiteralStringRef("");
				return r;
			} else {
				TraceEvent(SevError, "InvalidTestOption").detail("OptionName", key);
				throw test_specification_invalid();
			}
		}

	return defaultValue;
}

int64_t getOption(VectorRef<KeyValueRef> options, Key key, int64_t defaultValue) {
	for (int i = 0; i < options.size(); i++)
		if (options[i].key == key) {
			int64_t r;
			if (sscanf(options[i].value.toString().c_str(), "%" SCNd64, &r)) {
				options[i].value = LiteralStringRef("");
				return r;
			} else {
				TraceEvent(SevError, "InvalidTestOption").detail("OptionName", key);
				throw test_specification_invalid();
			}
		}

	return defaultValue;
}

double getOption(VectorRef<KeyValueRef> options, Key key, double defaultValue) {
	for (int i = 0; i < options.size(); i++)
		if (options[i].key == key) {
			float r;
			if (sscanf(options[i].value.toString().c_str(), "%f", &r)) {
				options[i].value = LiteralStringRef("");
				return r;
			}
		}

	return defaultValue;
}

bool getOption(VectorRef<KeyValueRef> options, Key key, bool defaultValue) {
	Value p = getOption(options, key, defaultValue ? LiteralStringRef("true") : LiteralStringRef("false"));
	if (p == LiteralStringRef("true"))
		return true;
	if (p == LiteralStringRef("false"))
		return false;
	ASSERT(false);
	return false; // Assure that compiler is fine with the function
}

std::vector<std::string> getOption(VectorRef<KeyValueRef> options, Key key, std::vector<std::string> defaultValue) {
	for (int i = 0; i < options.size(); i++)
		if (options[i].key == key) {
			std::vector<std::string> v;
			int begin = 0;
			for (int c = 0; c < options[i].value.size(); c++)
				if (options[i].value[c] == ',') {
					v.push_back(options[i].value.substr(begin, c - begin).toString());
					begin = c + 1;
				}
			v.push_back(options[i].value.substr(begin).toString());
			options[i].value = LiteralStringRef("");
			return v;
		}
	return defaultValue;
}

bool hasOption(VectorRef<KeyValueRef> options, Key key) {
	for (const auto& option : options) {
		if (option.key == key) {
			return true;
		}
	}
	return false;
}

// returns unconsumed options
Standalone<VectorRef<KeyValueRef>> checkAllOptionsConsumed(VectorRef<KeyValueRef> options) {
	static StringRef nothing = LiteralStringRef("");
	Standalone<VectorRef<KeyValueRef>> unconsumed;
	for (int i = 0; i < options.size(); i++)
		if (!(options[i].value == nothing)) {
			TraceEvent(SevError, "OptionNotConsumed")
			    .detail("Key", options[i].key.toString().c_str())
			    .detail("Value", options[i].value.toString().c_str());
			unconsumed.push_back_deep(unconsumed.arena(), options[i]);
		}
	return unconsumed;
}

struct CompoundWorkload : TestWorkload {
	std::vector<Reference<TestWorkload>> workloads;

	CompoundWorkload(WorkloadContext& wcx) : TestWorkload(wcx) {}
	CompoundWorkload* add(Reference<TestWorkload>&& w) {
		workloads.push_back(std::move(w));
		return this;
	}

	std::string description() const override {
		std::string d;
		for (int w = 0; w < workloads.size(); w++)
			d += workloads[w]->description() + (w == workloads.size() - 1 ? "" : ";");
		return d;
	}
	Future<Void> setup(Database const& cx) override {
		std::vector<Future<Void>> all;
		all.reserve(workloads.size());
		for (int w = 0; w < workloads.size(); w++)
			all.push_back(workloads[w]->setup(cx));
		return waitForAll(all);
	}
	Future<Void> start(Database const& cx) override {
		std::vector<Future<Void>> all;
		all.reserve(workloads.size());
		auto wCount = std::make_shared<unsigned>(0);
		for (int i = 0; i < workloads.size(); i++) {
			std::string workloadName = workloads[i]->description();
			++(*wCount);
			TraceEvent("WorkloadRunStatus")
			    .detail("Name", workloadName)
			    .detail("Count", *wCount)
			    .detail("Phase", "Start");
			all.push_back(fmap(
			    [workloadName, wCount](Void value) {
				    --(*wCount);
				    TraceEvent("WorkloadRunStatus")
				        .detail("Name", workloadName)
				        .detail("Remaining", *wCount)
				        .detail("Phase", "End");
				    return Void();
			    },
			    workloads[i]->start(cx)));
		}
		return waitForAll(all);
	}
	Future<bool> check(Database const& cx) override {
		std::vector<Future<bool>> all;
		all.reserve(workloads.size());
		auto wCount = std::make_shared<unsigned>(0);
		for (int i = 0; i < workloads.size(); i++) {
			++(*wCount);
			std::string workloadName = workloads[i]->description();
			TraceEvent("WorkloadCheckStatus")
			    .detail("Name", workloadName)
			    .detail("Count", *wCount)
			    .detail("Phase", "Start");
			all.push_back(fmap(
			    [workloadName, wCount](bool ret) {
				    --(*wCount);
				    TraceEvent("WorkloadCheckStatus")
				        .detail("Name", workloadName)
				        .detail("Remaining", *wCount)
				        .detail("Phase", "End");
				    return true;
			    },
			    workloads[i]->check(cx)));
		}
		return allTrue(all);
	}
	void getMetrics(std::vector<PerfMetric>& m) override {
		for (int w = 0; w < workloads.size(); w++) {
			std::vector<PerfMetric> p;
			workloads[w]->getMetrics(p);
			for (int i = 0; i < p.size(); i++)
				m.push_back(p[i].withPrefix(workloads[w]->description() + "."));
		}
	}
	double getCheckTimeout() const override {
		double m = 0;
		for (int w = 0; w < workloads.size(); w++)
			m = std::max(workloads[w]->getCheckTimeout(), m);
		return m;
	}
};

Reference<TestWorkload> getWorkloadIface(WorkloadRequest work,
                                         VectorRef<KeyValueRef> options,
                                         Reference<AsyncVar<ServerDBInfo> const> dbInfo) {
	Value testName = getOption(options, LiteralStringRef("testName"), LiteralStringRef("no-test-specified"));
	WorkloadContext wcx;
	wcx.clientId = work.clientId;
	wcx.clientCount = work.clientCount;
	wcx.dbInfo = dbInfo;
	wcx.options = options;
	wcx.sharedRandomNumber = work.sharedRandomNumber;
	wcx.rangesToCheck = work.rangesToCheck;

	auto workload = IWorkloadFactory::create(testName.toString(), wcx);

	auto unconsumedOptions = checkAllOptionsConsumed(workload ? workload->options : VectorRef<KeyValueRef>());
	if (!workload || unconsumedOptions.size()) {
		TraceEvent evt(SevError, "TestCreationError");
		evt.detail("TestName", testName);
		if (!workload) {
			evt.detail("Reason", "Null workload");
			fprintf(stderr,
			        "ERROR: Workload could not be created, perhaps testName (%s) is not a valid workload\n",
			        printable(testName).c_str());
		} else {
			evt.detail("Reason", "Not all options consumed");
			fprintf(stderr, "ERROR: Workload had invalid options. The following were unrecognized:\n");
			for (int i = 0; i < unconsumedOptions.size(); i++)
				fprintf(stderr,
				        " '%s' = '%s'\n",
				        unconsumedOptions[i].key.toString().c_str(),
				        unconsumedOptions[i].value.toString().c_str());
		}
		throw test_specification_invalid();
	}
	return workload;
}

Reference<TestWorkload> getWorkloadIface(WorkloadRequest work, Reference<AsyncVar<ServerDBInfo> const> dbInfo) {
	if (work.options.size() < 1) {
		TraceEvent(SevError, "TestCreationError").detail("Reason", "No options provided");
		fprintf(stderr, "ERROR: No options were provided for workload.\n");
		throw test_specification_invalid();
	}
	if (work.options.size() == 1)
		return getWorkloadIface(work, work.options[0], dbInfo);

	WorkloadContext wcx;
	wcx.clientId = work.clientId;
	wcx.clientCount = work.clientCount;
	wcx.sharedRandomNumber = work.sharedRandomNumber;
	wcx.rangesToCheck = work.rangesToCheck;
	// FIXME: Other stuff not filled in; why isn't this constructed here and passed down to the other
	// getWorkloadIface()?
	auto compound = makeReference<CompoundWorkload>(wcx);
	for (int i = 0; i < work.options.size(); i++) {
		compound->add(getWorkloadIface(work, work.options[i], dbInfo));
	}
	return compound;
}

/**
 * Only works in simulation. This method prints all simulated processes in a human readable form to stdout. It groups
 * processes by data center, data hall, zone, and machine (in this order).
 */
void printSimulatedTopology() {
	if (!g_network->isSimulated()) {
		return;
	}
	auto processes = g_simulator.getAllProcesses();
	std::sort(processes.begin(), processes.end(), [](ISimulator::ProcessInfo* lhs, ISimulator::ProcessInfo* rhs) {
		auto l = lhs->locality;
		auto r = rhs->locality;
		if (l.dcId() != r.dcId()) {
			return l.dcId() < r.dcId();
		}
		if (l.dataHallId() != r.dataHallId()) {
			return l.dataHallId() < r.dataHallId();
		}
		if (l.zoneId() != r.zoneId()) {
			return l.zoneId() < r.zoneId();
		}
		if (l.machineId() != r.zoneId()) {
			return l.machineId() < r.machineId();
		}
		return lhs->address < rhs->address;
	});
	printf("Simulated Cluster Topology:\n");
	printf("===========================\n");
	Optional<Standalone<StringRef>> dcId, dataHallId, zoneId, machineId;
	for (auto p : processes) {
		std::string indent = "";
		if (dcId != p->locality.dcId()) {
			dcId = p->locality.dcId();
			printf("%sdcId: %s\n", indent.c_str(), p->locality.describeDcId().c_str());
		}
		indent += "  ";
		if (dataHallId != p->locality.dataHallId()) {
			dataHallId = p->locality.dataHallId();
			printf("%sdataHallId: %s\n", indent.c_str(), p->locality.describeDataHall().c_str());
		}
		indent += "  ";
		if (zoneId != p->locality.zoneId()) {
			zoneId = p->locality.zoneId();
			printf("%szoneId: %s\n", indent.c_str(), p->locality.describeZone().c_str());
		}
		indent += "  ";
		if (machineId != p->locality.machineId()) {
			machineId = p->locality.machineId();
			printf("%smachineId: %s\n", indent.c_str(), p->locality.describeMachineId().c_str());
		}
		indent += "  ";
		printf("%sAddress: %s\n", indent.c_str(), p->address.toString().c_str());
		indent += "  ";
		printf("%sClass: %s\n", indent.c_str(), p->startingClass.toString().c_str());
		printf("%sName: %s\n", indent.c_str(), p->name);
	}
}

ACTOR Future<Void> databaseWarmer(Database cx) {
	loop {
		state Transaction tr(cx);
		wait(success(tr.getReadVersion()));
		wait(delay(0.25));
	}
}

// Tries indefinitely to commit a simple, self conflicting transaction
ACTOR Future<Void> pingDatabase(Database cx) {
	state Transaction tr(cx);
	loop {
		try {
			tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
			tr.setOption(FDBTransactionOptions::LOCK_AWARE);
			Optional<Value> v =
			    wait(tr.get(StringRef("/Liveness/" + deterministicRandom()->randomUniqueID().toString())));
			tr.makeSelfConflicting();
			wait(tr.commit());
			return Void();
		} catch (Error& e) {
			TraceEvent("PingingDatabaseTransactionError").error(e);
			wait(tr.onError(e));
		}
	}
}

ACTOR Future<Void> testDatabaseLiveness(Database cx,
                                        double databasePingDelay,
                                        std::string context,
                                        double startDelay = 0.0) {
	wait(delay(startDelay));
	loop {
		try {
			state double start = now();
			auto traceMsg = "PingingDatabaseLiveness_" + context;
			TraceEvent(traceMsg.c_str());
			wait(timeoutError(pingDatabase(cx), databasePingDelay));
			double pingTime = now() - start;
			ASSERT(pingTime > 0);
			TraceEvent(("PingingDatabaseLivenessDone_" + context).c_str()).detail("TimeTaken", pingTime);
			wait(delay(databasePingDelay - pingTime));
		} catch (Error& e) {
			if (e.code() != error_code_actor_cancelled)
				TraceEvent(SevError, ("PingingDatabaseLivenessError_" + context).c_str())
				    .error(e)
				    .detail("PingDelay", databasePingDelay);
			throw;
		}
	}
}

template <class T>
void sendResult(ReplyPromise<T>& reply, Optional<ErrorOr<T>> const& result) {
	auto& res = result.get();
	if (res.isError())
		reply.sendError(res.getError());
	else
		reply.send(res.get());
}

ACTOR Future<Void> runWorkloadAsync(Database cx,
                                    WorkloadInterface workIface,
                                    Reference<TestWorkload> workload,
                                    double databasePingDelay,
                                    bool isConsistencyCheckUrgent) {
	state Optional<ErrorOr<Void>> setupResult;
	state Optional<ErrorOr<Void>> startResult;
	state Optional<ErrorOr<CheckReply>> checkResult;
	state ReplyPromise<Void> setupReq;
	state ReplyPromise<Void> startReq;
	state ReplyPromise<CheckReply> checkReq;

	TraceEvent("TestBeginAsync", workIface.id())
	    .detail("Workload", workload->description())
	    .detail("DatabasePingDelay", databasePingDelay);

	state Future<Void> databaseError =
	    databasePingDelay == 0.0 ? Never() : testDatabaseLiveness(cx, databasePingDelay, "RunWorkloadAsync");

	loop choose {
		when(ReplyPromise<Void> req = waitNext(workIface.setup.getFuture())) {
			printf("Test received trigger for setup...\n");
			TraceEvent("TestSetupBeginning", workIface.id()).detail("Workload", workload->description());
			setupReq = req;
			if (!setupResult.present()) {
				try {
					wait(workload->setup(cx) || databaseError);
					TraceEvent("TestSetupComplete", workIface.id()).detail("Workload", workload->description());
					setupResult = Void();
				} catch (Error& e) {
					setupResult = operation_failed();
					TraceEvent(isConsistencyCheckUrgent ? SevWarn : SevError, "TestSetupError", workIface.id())
					    .error(e)
					    .detail("Workload", workload->description());
					if (e.code() == error_code_please_reboot || e.code() == error_code_please_reboot_delete)
						throw;
				}
			}
			sendResult(setupReq, setupResult);
			TraceEvent("TestSentResult", workIface.id()).detail("Workload", workload->description());
		}
		when(ReplyPromise<Void> req = waitNext(workIface.start.getFuture())) {
			startReq = req;
			if (!startResult.present()) {
				try {
					TraceEvent("TestStarting", workIface.id())
					    .detail("Workload", workload->description())
					    .detail("ClientCount", workload->clientCount)
					    .detail("ClientId", workload->clientId);
					wait(workload->start(cx) || databaseError);
					startResult = Void();
				} catch (Error& e) {
					startResult = operation_failed();
					if (e.code() == error_code_please_reboot || e.code() == error_code_please_reboot_delete)
						throw;
					TraceEvent(isConsistencyCheckUrgent ? SevWarn : SevError, "TestFailure", workIface.id())
					    .errorUnsuppressed(e)
					    .detail("Reason", "Error starting workload")
					    .detail("Workload", workload->description());
					// ok = false;
				}
				TraceEvent("TestComplete", workIface.id())
				    .detail("Workload", workload->description())
				    .detail("OK", !startResult.get().isError());
				printf("%s complete\n", workload->description().c_str());
			}
			sendResult(startReq, startResult);
		}
		when(ReplyPromise<CheckReply> req = waitNext(workIface.check.getFuture())) {
			checkReq = req;
			if (!checkResult.present()) {
				try {
					TraceEvent("TestChecking", workIface.id()).detail("Workload", workload->description());
					bool check = wait(timeoutError(workload->check(cx), workload->getCheckTimeout()));
					checkResult = CheckReply{ (!startResult.present() || !startResult.get().isError()) && check };
					TraceEvent("TestChecked", workIface.id())
					    .detail("Workload", workload->description())
					    .detail("Result", (!startResult.present() || !startResult.get().isError()) && check);
				} catch (Error& e) {
					checkResult = operation_failed(); // was: checkResult = false;
					if (e.code() == error_code_please_reboot || e.code() == error_code_please_reboot_delete)
						throw;
					TraceEvent(SevError, "TestFailure", workIface.id())
					    .error(e)
					    .detail("Reason", "Error checking workload")
					    .detail("Workload", workload->description());
					// ok = false;
				}
				TraceEvent("TestCheckComplete", workIface.id()).detail("Workload", workload->description());
			}

			sendResult(checkReq, checkResult);
		}
		when(ReplyPromise<std::vector<PerfMetric>> req = waitNext(workIface.metrics.getFuture())) {
			state ReplyPromise<std::vector<PerfMetric>> s_req = req;
			try {
				std::vector<PerfMetric> m;
				workload->getMetrics(m);
				TraceEvent("WorkloadSendMetrics", workIface.id()).detail("Count", m.size());
				req.send(m);
			} catch (Error& e) {
				if (e.code() == error_code_please_reboot || e.code() == error_code_please_reboot_delete)
					throw;
				TraceEvent(SevError, "WorkloadSendMetrics", workIface.id()).error(e);
				s_req.sendError(operation_failed());
			}
		}
		when(ReplyPromise<Void> r = waitNext(workIface.stop.getFuture())) {
			r.send(Void());
			break;
		}
	}

	TraceEvent("TestEndAsync", workIface.id())
	    .detail("Workload", workload->description())
	    .detail("DatabasePingDelay", databasePingDelay);

	return Void();
}

ACTOR Future<Void> testerServerWorkload(WorkloadRequest work,
                                        Reference<IClusterConnectionRecord> ccr,
                                        Reference<AsyncVar<struct ServerDBInfo> const> dbInfo,
                                        LocalityData locality,
                                        bool isConsistencyCheckUrgent) {
	state WorkloadInterface workIface;
	state bool replied = false;
	state Database cx;
	try {
		std::map<std::string, std::string> details;
		details["WorkloadTitle"] = printable(work.title);
		details["ClientId"] = format("%d", work.clientId);
		details["ClientCount"] = format("%d", work.clientCount);
		details["WorkloadTimeout"] = format("%d", work.timeout);
		startRole(Role::TESTER, workIface.id(), UID(), details);

		if (work.useDatabase) {
			cx = Database::createDatabase(ccr, -1, IsInternal::True, locality);
			cx->defaultTenant = work.defaultTenant.castTo<TenantName>();
			wait(delay(1.0));
		}

		// add test for "done" ?
		TraceEvent("WorkloadReceived", workIface.id()).detail("Title", work.title);
		auto workload = getWorkloadIface(work, dbInfo);
		if (!workload) {
			TraceEvent("TestCreationError").detail("Reason", "Workload could not be created");
			fprintf(stderr, "ERROR: The workload could not be created.\n");
			throw test_specification_invalid();
		}
		Future<Void> test =
		    runWorkloadAsync(cx, workIface, workload, work.databasePingDelay, isConsistencyCheckUrgent) ||
		    traceRole(Role::TESTER, workIface.id());
		work.reply.send(workIface);
		replied = true;

		if (work.timeout > 0) {
			test = timeoutError(test, work.timeout);
		}

		wait(test);

		endRole(Role::TESTER, workIface.id(), "Complete");
	} catch (Error& e) {
		if (!replied) {
			if (e.code() == error_code_test_specification_invalid)
				work.reply.sendError(e);
			else
				work.reply.sendError(operation_failed());
		}

		bool ok = e.code() == error_code_please_reboot || e.code() == error_code_please_reboot_delete ||
		          e.code() == error_code_actor_cancelled;
		endRole(Role::TESTER, workIface.id(), "Error", ok, e);

		if (e.code() != error_code_test_specification_invalid && e.code() != error_code_timed_out) {
			throw; // fatal errors will kill the testerServer as well
		}
	}
	return Void();
}

ACTOR Future<Void> testerServerCore(TesterInterface interf,
                                    Reference<IClusterConnectionRecord> ccr,
                                    Reference<AsyncVar<struct ServerDBInfo> const> dbInfo,
                                    LocalityData locality) {
	state PromiseStream<Future<Void>> addWorkload;
	state Future<Void> workerFatalError = actorCollection(addWorkload.getFuture());

	// Dedicated to consistencyCheckerUrgent
	// At any time, we only allow at most 1 consistency checker workload on a server
	state std::pair<int64_t, Future<Void>> consistencyCheckerUrgentTester = std::make_pair(0, Future<Void>());

	TraceEvent(SevInfo, "StartingTesterServerCore", interf.id());
	loop choose {
		when(wait(workerFatalError)) {}
		when(wait(consistencyCheckerUrgentTester.second.isValid() ? consistencyCheckerUrgentTester.second : Never())) {
			ASSERT(consistencyCheckerUrgentTester.first != 0);
			TraceEvent(SevInfo, "ConsistencyCheckUrgent_ServerWorkloadEnd", interf.id())
			    .detail("ConsistencyCheckerId", consistencyCheckerUrgentTester.first);
			consistencyCheckerUrgentTester = std::make_pair(0, Future<Void>()); // reset
		}
		when(WorkloadRequest work = waitNext(interf.recruitments.getFuture())) {
			if (work.sharedRandomNumber > SERVER_KNOBS->CONSISTENCY_CHECK_ID_MIN &&
			    work.sharedRandomNumber < SERVER_KNOBS->CONSISTENCY_CHECK_ID_MAX_PLUS_ONE) {
				// The workload is a consistency checker urgent workload
				if (work.sharedRandomNumber == consistencyCheckerUrgentTester.first) {
					TraceEvent(SevInfo, "ConsistencyCheckUrgent_ServerDuplicatedRequest", interf.id())
					    .detail("ConsistencyCheckerId", work.sharedRandomNumber)
					    .detail("ClientId", work.clientId)
					    .detail("ClientCount", work.clientCount);
				} else if (consistencyCheckerUrgentTester.second.isValid() &&
				           !consistencyCheckerUrgentTester.second.isReady()) {
					TraceEvent(SevWarnAlways, "ConsistencyCheckUrgent_ServerConflict", interf.id())
					    .detail("ExistingConsistencyCheckerId", consistencyCheckerUrgentTester.first)
					    .detail("ArrivingConsistencyCheckerId", work.sharedRandomNumber)
					    .detail("ClientId", work.clientId)
					    .detail("ClientCount", work.clientCount);
				}
				consistencyCheckerUrgentTester = std::make_pair(
				    work.sharedRandomNumber,
				    testerServerWorkload(work, ccr, dbInfo, locality, /*isConsistencyCheckUrgent=*/true));
				TraceEvent(SevInfo, "ConsistencyCheckUrgent_ServerWorkloadStart", interf.id())
				    .detail("ConsistencyCheckerId", consistencyCheckerUrgentTester.first)
				    .detail("ClientId", work.clientId)
				    .detail("ClientCount", work.clientCount);
			} else {
				addWorkload.send(testerServerWorkload(work, ccr, dbInfo, locality, /*isConsistencyCheckUrgent=*/false));
			}
		}
	}
}

ACTOR Future<Void> clearData(Database cx) {
	state Transaction tr(cx);
	loop {
		try {
			// This transaction needs to be self-conflicting, but not conflict consistently with
			// any other transactions
			tr.clear(normalKeys);
			tr.makeSelfConflicting();
			wait(success(tr.getReadVersion())); // required since we use addReadConflictRange but not get
			wait(tr.commit());
			TraceEvent("TesterClearingDatabase").detail("AtVersion", tr.getCommittedVersion());
			break;
		} catch (Error& e) {
			TraceEvent(SevWarn, "TesterClearingDatabaseError").error(e);
			wait(tr.onError(e));
		}
	}
	return Void();
}

Future<Void> dumpDatabase(Database const& cx, std::string const& outputFilename, KeyRange const& range);

int passCount = 0;
int failCount = 0;

std::vector<PerfMetric> aggregateMetrics(std::vector<std::vector<PerfMetric>> metrics) {
	std::map<std::string, std::vector<PerfMetric>> metricMap;
	for (int i = 0; i < metrics.size(); i++) {
		std::vector<PerfMetric> workloadMetrics = metrics[i];
		TraceEvent("MetricsReturned").detail("Count", workloadMetrics.size());
		for (int m = 0; m < workloadMetrics.size(); m++) {
			printf("Metric (%d, %d): %s, %f, %s\n",
			       i,
			       m,
			       workloadMetrics[m].name().c_str(),
			       workloadMetrics[m].value(),
			       workloadMetrics[m].formatted().c_str());
			metricMap[workloadMetrics[m].name()].push_back(workloadMetrics[m]);
		}
	}
	TraceEvent("Metric")
	    .detail("Name", "Reporting Clients")
	    .detail("Value", (double)metrics.size())
	    .detail("Formatted", format("%d", metrics.size()).c_str());

	std::vector<PerfMetric> result;
	std::map<std::string, std::vector<PerfMetric>>::iterator it;
	for (it = metricMap.begin(); it != metricMap.end(); it++) {
		auto& vec = it->second;
		if (!vec.size())
			continue;
		double sum = 0;
		for (int i = 0; i < vec.size(); i++)
			sum += vec[i].value();
		if (vec[0].averaged() && vec.size())
			sum /= vec.size();
		result.emplace_back(vec[0].name(), sum, Averaged::False, vec[0].format_code());
	}
	return result;
}

void logMetrics(std::vector<PerfMetric> metrics) {
	for (int idx = 0; idx < metrics.size(); idx++)
		TraceEvent("Metric")
		    .detail("Name", metrics[idx].name())
		    .detail("Value", metrics[idx].value())
		    .detail("Formatted", format(metrics[idx].format_code().c_str(), metrics[idx].value()));
}

template <class T>
void throwIfError(const std::vector<Future<ErrorOr<T>>>& futures, std::string errorMsg) {
	for (auto& future : futures) {
		if (future.get().isError()) {
			TraceEvent(SevError, errorMsg.c_str()).error(future.get().getError());
			throw future.get().getError();
		}
	}
}

ACTOR Future<DistributedTestResults> runWorkload(Database cx,
                                                 std::vector<TesterInterface> testers,
                                                 TestSpec spec,
                                                 Optional<TenantName> defaultTenant) {
	TraceEvent("TestRunning")
	    .detail("WorkloadTitle", spec.title)
	    .detail("TesterCount", testers.size())
	    .detail("Phases", spec.phases)
	    .detail("TestTimeout", spec.timeout);

	state std::vector<Future<WorkloadInterface>> workRequests;
	state std::vector<std::vector<PerfMetric>> metricsResults;

	state int i = 0;
	state int success = 0;
	state int failure = 0;
	int64_t sharedRandom = deterministicRandom()->randomInt64(0, SERVER_KNOBS->TESTER_SHARED_RANDOM_MAX_PLUS_ONE);
	for (; i < testers.size(); i++) {
		WorkloadRequest req;
		req.title = spec.title;
		req.useDatabase = spec.useDB;
		req.timeout = spec.timeout;
		req.databasePingDelay = spec.useDB ? spec.databasePingDelay : 0.0;
		req.options = spec.options;
		req.clientId = i;
		req.clientCount = testers.size();
		req.sharedRandomNumber = sharedRandom;
		req.defaultTenant = defaultTenant.castTo<TenantNameRef>();
		req.rangesToCheck = Optional<std::vector<KeyRange>>();
		workRequests.push_back(testers[i].recruitments.getReply(req));
	}

	state std::vector<WorkloadInterface> workloads = wait(getAll(workRequests));
	state double waitForFailureTime = g_network->isSimulated() ? 24 * 60 * 60 : 60;
	if (g_network->isSimulated() && spec.simCheckRelocationDuration)
		debug_setCheckRelocationDuration(true);

	if (spec.phases & TestWorkload::SETUP) {
		state std::vector<Future<ErrorOr<Void>>> setups;
		printf("setting up test (%s)...\n", printable(spec.title).c_str());
		TraceEvent("TestSetupStart").detail("WorkloadTitle", spec.title);
		setups.reserve(workloads.size());
		for (int i = 0; i < workloads.size(); i++)
			setups.push_back(workloads[i].setup.template getReplyUnlessFailedFor<Void>(waitForFailureTime, 0));
		wait(waitForAll(setups));
		throwIfError(setups, "SetupFailedForWorkload" + printable(spec.title));
		TraceEvent("TestSetupComplete").detail("WorkloadTitle", spec.title);
	}

	if (spec.phases & TestWorkload::EXECUTION) {
		TraceEvent("TestStarting").detail("WorkloadTitle", spec.title);
		printf("running test (%s)...\n", printable(spec.title).c_str());
		state std::vector<Future<ErrorOr<Void>>> starts;
		starts.reserve(workloads.size());
		for (int i = 0; i < workloads.size(); i++)
			starts.push_back(workloads[i].start.template getReplyUnlessFailedFor<Void>(waitForFailureTime, 0));
		wait(waitForAll(starts));
		throwIfError(starts, "StartFailedForWorkload" + printable(spec.title));
		printf("%s complete\n", printable(spec.title).c_str());
		TraceEvent("TestComplete").detail("WorkloadTitle", spec.title);
	}

	if (spec.phases & TestWorkload::CHECK) {
		if (spec.useDB && (spec.phases & TestWorkload::EXECUTION)) {
			wait(delay(3.0));
		}

		state std::vector<Future<ErrorOr<CheckReply>>> checks;
		TraceEvent("CheckingResults").log();

		printf("checking test (%s)...\n", printable(spec.title).c_str());

		checks.reserve(workloads.size());
		for (int i = 0; i < workloads.size(); i++)
			checks.push_back(workloads[i].check.template getReplyUnlessFailedFor<CheckReply>(waitForFailureTime, 0));
		wait(waitForAll(checks));

		throwIfError(checks, "CheckFailedForWorkload" + printable(spec.title));

		for (int i = 0; i < checks.size(); i++) {
			if (checks[i].get().get().value)
				success++;
			else
				failure++;
		}
	}

	if (spec.phases & TestWorkload::METRICS) {
		state std::vector<Future<ErrorOr<std::vector<PerfMetric>>>> metricTasks;
		printf("fetching metrics (%s)...\n", printable(spec.title).c_str());
		TraceEvent("TestFetchingMetrics").detail("WorkloadTitle", spec.title);
		metricTasks.reserve(workloads.size());
		for (int i = 0; i < workloads.size(); i++)
			metricTasks.push_back(
			    workloads[i].metrics.template getReplyUnlessFailedFor<std::vector<PerfMetric>>(waitForFailureTime, 0));
		wait(waitForAll(metricTasks));
		throwIfError(metricTasks, "MetricFailedForWorkload" + printable(spec.title));
		for (int i = 0; i < metricTasks.size(); i++) {
			metricsResults.push_back(metricTasks[i].get().get());
		}
	}

	// Stopping the workloads is unreliable, but they have a timeout
	// FIXME: stop if one of the above phases throws an exception
	for (int i = 0; i < workloads.size(); i++)
		workloads[i].stop.send(ReplyPromise<Void>());

	return DistributedTestResults(aggregateMetrics(metricsResults), success, failure);
}

// Sets the database configuration by running the ChangeConfig workload
ACTOR Future<Void> changeConfiguration(Database cx, std::vector<TesterInterface> testers, StringRef configMode) {
	state TestSpec spec;
	Standalone<VectorRef<KeyValueRef>> options;
	spec.title = LiteralStringRef("ChangeConfig");
	options.push_back_deep(options.arena(),
	                       KeyValueRef(LiteralStringRef("testName"), LiteralStringRef("ChangeConfig")));
	options.push_back_deep(options.arena(), KeyValueRef(LiteralStringRef("configMode"), configMode));
	spec.options.push_back_deep(spec.options.arena(), options);

	DistributedTestResults testResults = wait(runWorkload(cx, testers, spec, Optional<TenantName>()));

	return Void();
}

// Runs the consistency check workload, which verifies that the database is in a consistent state
ACTOR Future<Void> checkConsistency(Database cx,
                                    std::vector<TesterInterface> testers,
                                    bool doQuiescentCheck,
                                    bool doCacheCheck,
                                    bool doTSSCheck,
                                    double quiescentWaitTimeout,
                                    double softTimeLimit,
                                    double databasePingDelay,
                                    Reference<AsyncVar<ServerDBInfo>> dbInfo) {
	state TestSpec spec;

	state double connectionFailures;
	if (g_network->isSimulated()) {
		// NOTE: the value will be reset after consistency check
		connectionFailures = g_simulator.connectionFailuresDisableDuration;
		disableConnectionFailures("ConsistencyCheck");
	}

	Standalone<VectorRef<KeyValueRef>> options;
	StringRef performQuiescent = LiteralStringRef("false");
	StringRef performCacheCheck = LiteralStringRef("false");
	StringRef performTSSCheck = LiteralStringRef("false");
	if (doQuiescentCheck) {
		performQuiescent = LiteralStringRef("true");
		spec.restorePerpetualWiggleSetting = false;
	}
	if (doCacheCheck) {
		performCacheCheck = LiteralStringRef("true");
	}
	if (doTSSCheck) {
		performTSSCheck = LiteralStringRef("true");
	}
	spec.title = LiteralStringRef("ConsistencyCheck");
	spec.databasePingDelay = databasePingDelay;
	spec.timeout = 32000;
	options.push_back_deep(options.arena(),
	                       KeyValueRef(LiteralStringRef("testName"), LiteralStringRef("ConsistencyCheck")));
	options.push_back_deep(options.arena(), KeyValueRef(LiteralStringRef("performQuiescentChecks"), performQuiescent));
	options.push_back_deep(options.arena(), KeyValueRef(LiteralStringRef("performCacheCheck"), performCacheCheck));
	options.push_back_deep(options.arena(), KeyValueRef(LiteralStringRef("performTSSCheck"), performTSSCheck));
	options.push_back_deep(options.arena(),
	                       KeyValueRef(LiteralStringRef("quiescentWaitTimeout"),
	                                   ValueRef(options.arena(), format("%f", quiescentWaitTimeout))));
	options.push_back_deep(options.arena(), KeyValueRef(LiteralStringRef("distributed"), LiteralStringRef("false")));
	options.push_back_deep(options.arena(), KeyValueRef(LiteralStringRef("shuffleShards"), LiteralStringRef("true")));
	spec.options.push_back_deep(spec.options.arena(), options);
	state double start = now();
	state bool lastRun = false;
	loop {
		TraceEvent("ConsistencyCheckWorkLoadLoopBegin");
		DistributedTestResults testResults = wait(runWorkload(cx, testers, spec, Optional<TenantName>()));
		if (testResults.ok() || lastRun) {
			if (g_network->isSimulated()) {
				g_simulator.connectionFailuresDisableDuration = connectionFailures;
			}
			TraceEvent("ConsistencyCheckWorkLoadEnd");
			return Void();
		}
		if (now() - start > softTimeLimit) {
			spec.options[0].push_back_deep(spec.options.arena(),
			                               KeyValueRef(LiteralStringRef("failureIsError"), LiteralStringRef("true")));
			lastRun = true;
		}
		TraceEvent("ConsistencyCheckWorkLoadRepairDC");
		wait(repairDeadDatacenter(cx, dbInfo, "ConsistencyCheck"));
	}
}

ACTOR Future<std::unordered_set<int>> runUrgentConsistencyCheckWorkload(
    Database cx,
    std::vector<TesterInterface> testers,
    TestSpec spec,
    Optional<TenantName> defaultTenant,
    int64_t consistencyCheckerId,
    std::unordered_map<int, std::vector<KeyRange>> assignment) {
	TraceEvent("ConsistencyCheckUrgent_Dispatch")
	    .detail("TesterCount", testers.size())
	    .detail("ConsistencyCheckerId", consistencyCheckerId);
	state double waitForFailureTime = g_network->isSimulated() ? 24 * 60 * 60 : 60;

	// Step 1: Get interfaces for running workloads
	state std::vector<Future<ErrorOr<WorkloadInterface>>> workRequests;
	for (int i = 0; i < testers.size(); i++) {
		WorkloadRequest req;
		req.title = spec.title;
		req.useDatabase = spec.useDB;
		req.timeout = spec.timeout;
		req.databasePingDelay = spec.useDB ? spec.databasePingDelay : 0.0;
		req.options = spec.options;
		req.clientId = i;
		req.clientCount = testers.size();
		req.sharedRandomNumber = consistencyCheckerId;
		req.defaultTenant = defaultTenant.castTo<TenantNameRef>();
		if (!SERVER_KNOBS->CONSISTENCY_CHECK_USE_PERSIST_DATA) {
			req.rangesToCheck = assignment[i];
		} else {
			req.rangesToCheck = Optional<std::vector<KeyRange>>();
		}
		workRequests.push_back(testers[i].recruitments.getReplyUnlessFailedFor(req, waitForFailureTime, 0));
		// workRequests follows the order of clientId of assignment
	}
	wait(waitForAll(workRequests));

	// Step 2: Setup workloads via the interfaces
	TraceEvent("ConsistencyCheckUrgent_SetupWorkloads")
	    .detail("TesterCount", testers.size())
	    .detail("ConsistencyCheckerId", consistencyCheckerId);
	state std::vector<int> clientIds; // record the clientId for setups/starts
	// clientIds follows the same order as setups/starts
	state std::vector<Future<ErrorOr<Void>>> setups;
	for (int i = 0; i < workRequests.size(); i++) {
		ASSERT(workRequests[i].isReady());
		if (workRequests[i].get().isError()) {
			Error e = workRequests[i].get().getError();
			TraceEvent("ConsistencyCheckUrgent_FailedToContactToClient")
			    .error(e)
			    .detail("TesterCount", testers.size())
			    .detail("TesterId", i)
			    .detail("ConsistencyCheckerId", consistencyCheckerId);
			continue; // ignore any failed tester
		} else {
			setups.push_back(
			    workRequests[i].get().get().setup.template getReplyUnlessFailedFor<Void>(waitForFailureTime, 0));
			clientIds.push_back(i); // same order as setups
		}
	}
	wait(waitForAll(setups));
	for (int i = 0; i < setups.size(); i++) {
		if (setups[i].isError()) {
			TraceEvent("ConsistencyCheckUrgent_SetupWorkloadError1")
			    .errorUnsuppressed(setups[i].getError())
			    .detail("ClientId", clientIds[i])
			    .detail("ClientCount", testers.size())
			    .detail("ConsistencyCheckerId", consistencyCheckerId);
		} else if (setups[i].get().isError()) {
			TraceEvent("ConsistencyCheckUrgent_SetupWorkloadError2")
			    .errorUnsuppressed(setups[i].get().getError())
			    .detail("ClientId", clientIds[i])
			    .detail("ClientCount", testers.size())
			    .detail("ConsistencyCheckerId", consistencyCheckerId);
		}
	}
	try {
		for (auto& setup : setups) {
			if (setup.isError()) {
				throw setup.getError();
			} else if (setup.get().isError()) {
				throw setup.get().getError();
			}
		}
	} catch (Error& e) {
		TraceEvent(SevWarn, "ConsistencyCheckUrgent_SetupWorkloadFailed").error(e);
		// Give up this round if any setup failed
		for (int i = 0; i < workRequests.size(); i++) {
			ASSERT(workRequests[i].isReady());
			if (!workRequests[i].get().isError()) {
				workRequests[i].get().get().stop.send(ReplyPromise<Void>());
			}
		}
		throw e;
	}

	// Step 3: Run workloads via the interfaces
	TraceEvent("ConsistencyCheckUrgent_RunWorkloads")
	    .detail("TesterCount", testers.size())
	    .detail("ConsistencyCheckerId", consistencyCheckerId);
	state std::unordered_set<int> completeClientIds;
	clientIds.clear();
	state std::vector<Future<ErrorOr<Void>>> starts;
	for (int i = 0; i < workRequests.size(); i++) {
		ASSERT(workRequests[i].isReady());
		if (!workRequests[i].get().isError()) {
			starts.push_back(
			    workRequests[i].get().get().start.template getReplyUnlessFailedFor<Void>(waitForFailureTime, 0));
			clientIds.push_back(i); // same order as starts
		}
	}
	wait(waitForAll(starts));
	for (int i = 0; i < starts.size(); i++) {
		if (starts[i].isError()) {
			TraceEvent("ConsistencyCheckUrgent_RunWorkloadError1")
			    .errorUnsuppressed(starts[i].getError())
			    .detail("ClientId", clientIds[i])
			    .detail("ClientCount", testers.size())
			    .detail("ConsistencyCheckerId", consistencyCheckerId);
		} else if (starts[i].get().isError()) {
			TraceEvent("ConsistencyCheckUrgent_RunWorkloadError2")
			    .errorUnsuppressed(starts[i].get().getError())
			    .detail("ClientId", clientIds[i])
			    .detail("ClientCount", testers.size())
			    .detail("ConsistencyCheckerId", consistencyCheckerId);
		} else {
			TraceEvent("ConsistencyCheckUrgent_RunWorkloadComplete")
			    .detail("ClientId", clientIds[i])
			    .detail("ClientCount", testers.size())
			    .detail("ConsistencyCheckerId", consistencyCheckerId);
			completeClientIds.insert(clientIds[i]); // Add complete clients
		}
	}

	// Step 4: Stop workloads via the interfaces
	for (int i = 0; i < workRequests.size(); i++) {
		ASSERT(workRequests[i].isReady());
		if (!workRequests[i].get().isError()) {
			TraceEvent("ConsistencyCheckUrgent_RunWorkloadStopSignal")
			    .detail("State", "Succeed")
			    .detail("ClientId", i)
			    .detail("ClientCount", testers.size())
			    .detail("ConsistencyCheckerId", consistencyCheckerId);
			workRequests[i].get().get().stop.send(ReplyPromise<Void>());
			// This signal is not reliable but acceptable
		} else {
			TraceEvent("ConsistencyCheckUrgent_RunWorkloadStopSignal")
			    .detail("State", "Not issue since the interface is failed to fetch")
			    .detail("ClientId", i)
			    .detail("ClientCount", testers.size())
			    .detail("ConsistencyCheckerId", consistencyCheckerId);
		}
	}

	TraceEvent("ConsistencyCheckUrgent_DispatchEnd")
	    .detail("TesterCount", testers.size())
	    .detail("ConsistencyCheckerId", consistencyCheckerId);

	return completeClientIds;
}

ACTOR Future<std::vector<KeyRange>> getConsistencyCheckShards(Database cx, std::vector<KeyRange> ranges) {
	// Get the scope of the input list of ranges
	state Key beginKeyToReadKeyServer;
	state Key endKeyToReadKeyServer;
	for (int i = 0; i < ranges.size(); i++) {
		if (i == 0 || ranges[i].begin < beginKeyToReadKeyServer) {
			beginKeyToReadKeyServer = ranges[i].begin;
		}
		if (i == 0 || ranges[i].end > endKeyToReadKeyServer) {
			endKeyToReadKeyServer = ranges[i].end;
		}
	}
	TraceEvent("ConsistencyCheckUrgent_GetConsistencyCheckShards")
	    .detail("RangeBegin", beginKeyToReadKeyServer)
	    .detail("RangeEnd", endKeyToReadKeyServer);
	// Read KeyServer space within the scope and add shards intersecting with the input ranges
	state std::vector<KeyRange> res;
	state Transaction tr(cx);
	loop {
		try {
			tr.setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
			tr.setOption(FDBTransactionOptions::LOCK_AWARE);
			KeyRange rangeToRead = Standalone(KeyRangeRef(beginKeyToReadKeyServer, endKeyToReadKeyServer));
			RangeResult readResult = wait(krmGetRanges(&tr,
			                                           keyServersPrefix,
			                                           rangeToRead,
			                                           SERVER_KNOBS->MOVE_KEYS_KRM_LIMIT,
			                                           SERVER_KNOBS->MOVE_KEYS_KRM_LIMIT_BYTES));
			for (int i = 0; i < readResult.size() - 1; ++i) {
				KeyRange rangeToCheck = Standalone(KeyRangeRef(readResult[i].key, readResult[i + 1].key));
				Value valueToCheck = Standalone(readResult[i].value);
				bool toAdd = false;
				for (const auto& range : ranges) {
					if (rangeToCheck.intersects(range) == true) {
						toAdd = true;
						break;
					}
				}
				if (toAdd == true) {
					res.push_back(rangeToCheck);
				}
				beginKeyToReadKeyServer = readResult[i + 1].key;
			}
			if (beginKeyToReadKeyServer >= endKeyToReadKeyServer) {
				break;
			}
		} catch (Error& e) {
			TraceEvent("ConsistencyCheckUrgent_GetConsistencyCheckShardsRetry").error(e);
			wait(tr.onError(e));
		}
	}
	return res;
}

ACTOR Future<std::vector<TesterInterface>> getTesters(Reference<AsyncVar<Optional<ClusterControllerFullInterface>>> cc,
                                                      int minTestersExpected) {
	// Recruit workers
	state int flags = GetWorkersRequest::TESTER_CLASS_ONLY | GetWorkersRequest::NON_EXCLUDED_PROCESSES_ONLY;
	state Future<Void> testerTimeout = delay(600.0); // wait 600 sec for testers to show up
	state std::vector<WorkerDetails> workers;
	loop {
		choose {
			when(std::vector<WorkerDetails> w =
			         wait(cc->get().present()
			                  ? brokenPromiseToNever(cc->get().get().getWorkers.getReply(GetWorkersRequest(flags)))
			                  : Never())) {
				if (w.size() >= minTestersExpected) {
					workers = w;
					break;
				}
				wait(delay(SERVER_KNOBS->WORKER_POLL_DELAY));
			}
			when(wait(cc->onChange())) {}
			when(wait(testerTimeout)) {
				TraceEvent(SevWarnAlways, "TesterRecruitmentTimeout").log();
				throw timed_out();
			}
		}
	}
	state std::vector<TesterInterface> ts;
	ts.reserve(workers.size());
	for (int i = 0; i < workers.size(); i++)
		ts.push_back(workers[i].interf.testerInterface);
	deterministicRandom()->randomShuffle(ts);
	return ts;
}

ACTOR Future<Void> runConsistencyCheckerUrgentInit(Database cx, int64_t consistencyCheckerId) {
	state std::vector<KeyRange> rangesToCheck;
	state int retryTimes = 0;
	loop {
		try {
			// Persist consistencyCheckerId
			// The system allows one consistency checker at a time
			// The unique ID is persisted in metadata, indicating which consistency checker takes effect
			wait(persistConsistencyCheckerId(cx, consistencyCheckerId)); // Always succeed
			if (g_network->isSimulated() && deterministicRandom()->random01() < 0.05) {
				throw operation_failed(); // Introduce random failure
			}

			// If INIT_CLEAR_METADATA_EXIT mode, the metadata is cleared at beginning
			if (CLIENT_KNOBS->CONSISTENCY_CHECK_INIT_CLEAR_METADATA ||
			    CLIENT_KNOBS->CONSISTENCY_CHECK_INIT_CLEAR_METADATA_EXIT) {
				wait(clearConsistencyCheckMetadata(cx, consistencyCheckerId));
				TraceEvent("ConsistencyCheckUrgent_MetadataClearedWhenInit")
				    .detail("ConsistencyCheckerId", consistencyCheckerId);
				return Void();
			}

			// Load ranges to check from progress metadata
			rangesToCheck.clear();
			wait(store(rangesToCheck, loadRangesToCheckFromProgressMetadata(cx, consistencyCheckerId)));
			if (g_network->isSimulated() && deterministicRandom()->random01() < 0.05) {
				throw operation_failed(); // Introduce random failure
			}

			// Prepare for the ranges to check and persist consistency checker id
			if (rangesToCheck.size() == 0) {
				// If no range to check in progress data
				// We load the range from knob
				rangesToCheck = loadRangesToCheckFromKnob();
				wait(initConsistencyCheckProgressMetadata(cx, rangesToCheck, consistencyCheckerId));
				if (g_network->isSimulated() && deterministicRandom()->random01() < 0.05) {
					throw operation_failed(); // Introduce random failure
				}
				TraceEvent e("ConsistencyCheckUrgent_Start");
				e.setMaxEventLength(-1);
				e.setMaxFieldLength(-1);
				e.detail("ConsistencyCheckerId", consistencyCheckerId);
				for (int i = 0; i < rangesToCheck.size(); i++) {
					e.detail("RangeToCheckBegin" + std::to_string(i), rangesToCheck[i].begin);
					e.detail("RangeToCheckEnd" + std::to_string(i), rangesToCheck[i].end);
				}
			} else {
				TraceEvent("ConsistencyCheckUrgent_Resume")
				    .detail("ConsistencyCheckerId", consistencyCheckerId)
				    .detail("RangesToCheckCount", rangesToCheck.size());
			}
			break;

		} catch (Error& e) {
			if (e.code() == error_code_actor_cancelled) {
				throw e;
			} else if (e.code() == error_code_key_not_found || e.code() == error_code_consistency_check_task_outdated) {
				throw e;
			} else {
				TraceEvent("ConsistencyCheckUrgent_InitWithRetriableFailure")
				    .errorUnsuppressed(e)
				    .detail("ConsistencyCheckerId", consistencyCheckerId)
				    .detail("RetryTimes", retryTimes);
				if (retryTimes > 50) {
					throw timed_out();
				}
				wait(delay(10.0));
				retryTimes++;
			}
		}
	}

	return Void();
}

ACTOR Future<std::unordered_map<int, std::vector<KeyRange>>> makeTaskAssignment(Database cx,
                                                                                int64_t consistencyCheckerId,
                                                                                std::vector<KeyRange> shardsToCheck,
                                                                                int testersCount,
                                                                                int round) {
	state std::unordered_map<int, std::vector<KeyRange>> assignment;
	if (SERVER_KNOBS->CONSISTENCY_CHECK_USE_PERSIST_DATA) {
		wait(initConsistencyCheckAssignmentMetadata(cx, consistencyCheckerId));
		if (g_network->isSimulated() && deterministicRandom()->random01() < 0.05) {
			throw operation_failed(); // Introduce random failure
		}
	}
	int batchSize = CLIENT_KNOBS->CONSISTENCY_CHECK_BATCH_SHARD_COUNT;
	int startingPoint = 0;
	if (shardsToCheck.size() > batchSize * testersCount) {
		startingPoint = deterministicRandom()->randomInt(0, shardsToCheck.size() - batchSize * testersCount);
		// We randomly pick a set of successive shards:
		// (1) We want to retry for different shards to avoid repeated failure on the same shards
		// (2) We want to check successive shards to avoid inefficiency incurred by fragments
	}
	assignment.clear();
	for (int i = startingPoint; i < shardsToCheck.size(); i++) {
		int testerIdx = (i - startingPoint) / batchSize;
		if (testerIdx > testersCount - 1) {
			break; // Have filled up all testers
		}
		assignment[testerIdx].push_back(shardsToCheck[i]);
	}
	state std::unordered_map<int, std::vector<KeyRange>>::iterator assignIt;
	for (assignIt = assignment.begin(); assignIt != assignment.end(); assignIt++) {
		TraceEvent("ConsistencyCheckUrgent_ClientAssignedTask")
		    .detail("ConsistencyCheckerId", consistencyCheckerId)
		    .detail("Round", round)
		    .detail("ClientId", assignIt->first)
		    .detail("ShardsCount", assignIt->second.size());
		if (SERVER_KNOBS->CONSISTENCY_CHECK_USE_PERSIST_DATA) {
			wait(persistConsistencyCheckAssignment(
			    cx, assignIt->first, assignIt->second, consistencyCheckerId)); // Persist assignment
			if (g_network->isSimulated() && deterministicRandom()->random01() < 0.05) {
				throw operation_failed(); // Introduce random failure
			}
		}
	}
	if (SERVER_KNOBS->CONSISTENCY_CHECK_USE_PERSIST_DATA) {
		TraceEvent e("ConsistencyCheckUrgent_PersistAssignment");
		e.setMaxEventLength(-1);
		e.setMaxFieldLength(-1);
		e.detail("ConsistencyCheckerId", consistencyCheckerId);
		e.detail("Round", round);
		e.detail("TesterCount", testersCount);
		e.detail("ShardCountTotal", shardsToCheck.size());
		for (const auto& [clientId, assignedShards] : assignment) {
			e.detail("Client" + std::to_string(clientId), assignedShards.size());
		}
	}
	return assignment;
}

ACTOR Future<Void> runConsistencyCheckerUrgentCore(Reference<AsyncVar<Optional<ClusterControllerFullInterface>>> cc,
                                                   Database cx,
                                                   Optional<std::vector<TesterInterface>> testers,
                                                   int minTestersExpected,
                                                   TestSpec testSpec,
                                                   Optional<TenantName> defaultTenant,
                                                   Reference<AsyncVar<ServerDBInfo>> dbInfo) {
	state int64_t consistencyCheckerId = deterministicRandom()->randomInt64(
	    SERVER_KNOBS->CONSISTENCY_CHECK_ID_MIN, SERVER_KNOBS->CONSISTENCY_CHECK_ID_MAX_PLUS_ONE);
	state std::vector<KeyRange> rangesToCheck; // get from progress metadata
	state std::vector<KeyRange> shardsToCheck; // get from keyServer metadata
	state Optional<double> whenFailedToGetTesterStart;
	state KeyRangeMap<bool> globalProgressMap; // used to keep track of progress when persisting metadata is not allowed
	state std::unordered_map<int, std::vector<KeyRange>> assignment; // used to keep track of assignment of tasks
	state std::vector<TesterInterface> ts; // used to store testers interface

	// Initialization
	if (SERVER_KNOBS->CONSISTENCY_CHECK_USE_PERSIST_DATA) {
		// In case when persisting metadata is allowed, enforce consistencyCheckerId and prepare for metadata
		try {
			wait(runConsistencyCheckerUrgentInit(cx, consistencyCheckerId));
		} catch (Error& e) {
			if (e.code() == error_code_key_not_found || e.code() == error_code_consistency_check_task_outdated ||
			    e.code() == error_code_timed_out) {
				TraceEvent("ConsistencyCheckUrgent_Exit")
				    .errorUnsuppressed(e)
				    .detail("Reason", "FailureWhenInit")
				    .detail("ConsistencyCheckerId", consistencyCheckerId);
				return Void();
			} else {
				throw e;
			}
		}
		// Immediately exit after the clear for INIT_CLEAR_METADATA_EXIT mode
		if (CLIENT_KNOBS->CONSISTENCY_CHECK_INIT_CLEAR_METADATA_EXIT) {
			TraceEvent("ConsistencyCheckUrgent_Exit")
			    .detail("Reason", "SuccessClearMetadataWhenInit")
			    .detail("ConsistencyCheckerId", consistencyCheckerId);
			return Void();
		}
		// At this point, consistencyCheckerId has the ownership except that another consistency checker overwrites the
		// id metadata
	} else {
		// In case when persisting metadata is not allowed, prepare for globalProgressMap
		// globalProgressMap is used to keep track of the global progress of checking
		globalProgressMap.insert(allKeys, true);
		rangesToCheck = loadRangesToCheckFromKnob();
		for (const auto& rangeToCheck : rangesToCheck) {
			// Mark rangesToCheck as incomplete
			// Those ranges will be checked
			globalProgressMap.insert(rangeToCheck, false);
		}
		globalProgressMap.coalesce(allKeys);
	}

	// Main loop
	state int retryTimes = 0;
	state int round = 0;
	loop {
		try {
			// Step 1: Load ranges to check, if nothing to run, exit
			TraceEvent("ConsistencyCheckUrgent_RoundBegin")
			    .detail("ConsistencyCheckerId", consistencyCheckerId)
			    .detail("RetryTimes", retryTimes)
			    .detail("TesterCount", ts.size())
			    .detail("Round", round);

			rangesToCheck.clear();
			if (SERVER_KNOBS->CONSISTENCY_CHECK_USE_PERSIST_DATA) {
				wait(store(rangesToCheck, loadRangesToCheckFromProgressMetadata(cx, consistencyCheckerId)));
				if (g_network->isSimulated() && deterministicRandom()->random01() < 0.05) {
					throw operation_failed(); // Introduce random failure
				}
			} else {
				for (auto& range : globalProgressMap.ranges()) {
					if (!range.value()) { // range that is not finished
						rangesToCheck.push_back(range.range());
					}
				}
			}
			if (rangesToCheck.size() == 0) {
				TraceEvent("ConsistencyCheckUrgent_Complete")
				    .detail("ConsistencyCheckerId", consistencyCheckerId)
				    .detail("RetryTimes", retryTimes)
				    .detail("Round", round);
				if (SERVER_KNOBS->CONSISTENCY_CHECK_USE_PERSIST_DATA) {
					wait(clearConsistencyCheckMetadata(cx, consistencyCheckerId));
					if (g_network->isSimulated() && deterministicRandom()->random01() < 0.05) {
						throw operation_failed(); // Introduce random failure
					}
				}
				TraceEvent("ConsistencyCheckUrgent_Exit")
				    .detail("Reason", "Complete")
				    .detail("ConsistencyCheckerId", consistencyCheckerId)
				    .detail("RetryTimes", retryTimes)
				    .detail("Round", round);
				return Void();
			}

			// Step 2: Get testers
			ts.clear();
			if (!testers.present()) {
				try {
					wait(store(ts, getTesters(cc, minTestersExpected)));
					whenFailedToGetTesterStart.reset();
				} catch (Error& e) {
					if (e.code() == error_code_timed_out) {
						if (!whenFailedToGetTesterStart.present()) {
							whenFailedToGetTesterStart = now();
						} else if (now() - whenFailedToGetTesterStart.get() > 3600 * 24) { // 1 day
							TraceEvent(SevError, "TesterRecruitmentTimeout").log();
						}
					}
					throw e;
				}
				if (g_network->isSimulated() && deterministicRandom()->random01() < 0.05) {
					throw operation_failed(); // Introduce random failure
				}
			} else {
				ts = testers.get();
			}
			TraceEvent("ConsistencyCheckUrgent_GoTTesters")
			    .detail("ConsistencyCheckerId", consistencyCheckerId)
			    .detail("Round", round)
			    .detail("RetryTimes", retryTimes)
			    .detail("TesterCount", ts.size());

			// Step 3: Load shards to check from keyserver space
			// Shard is the unit for the task assignment
			shardsToCheck.clear();
			wait(store(shardsToCheck, getConsistencyCheckShards(cx, rangesToCheck)));
			TraceEvent("ConsistencyCheckUrgent_GotShardsToCheck")
			    .detail("ConsistencyCheckerId", consistencyCheckerId)
			    .detail("Round", round)
			    .detail("RetryTimes", retryTimes)
			    .detail("TesterCount", ts.size())
			    .detail("ShardCount", shardsToCheck.size());

			// Step 4: Assign tasks to clientId
			assignment.clear();
			wait(store(assignment, makeTaskAssignment(cx, consistencyCheckerId, shardsToCheck, ts.size(), round)));

			// Step 5: Run checking on testers
			std::unordered_set<int> completeClients = wait(
			    runUrgentConsistencyCheckWorkload(cx, ts, testSpec, defaultTenant, consistencyCheckerId, assignment));
			if (g_network->isSimulated() && deterministicRandom()->random01() < 0.05) {
				throw operation_failed(); // Introduce random failure
			}
			if (!SERVER_KNOBS->CONSISTENCY_CHECK_USE_PERSIST_DATA) {
				// In case when persisting metadata is not allowed, we use
				// the complete client to decide which ranges are completed
				for (const auto& clientId : completeClients) {
					for (const auto& range : assignment[clientId]) {
						globalProgressMap.insert(range, true); // Mark the ranges as complete
					}
				}
			}
			TraceEvent("ConsistencyCheckUrgent_RoundEnd")
			    .detail("ConsistencyCheckerId", consistencyCheckerId)
			    .detail("RetryTimes", retryTimes)
			    .detail("SucceedTesterCount", completeClients.size())
			    .detail("SucceedTesters", describe(completeClients))
			    .detail("TesterCount", ts.size())
			    .detail("Round", round);
			round++;

		} catch (Error& e) {
			if (e.code() == error_code_actor_cancelled) {
				throw e;
			} else if (e.code() == error_code_key_not_found || e.code() == error_code_consistency_check_task_outdated) {
				TraceEvent("ConsistencyCheckUrgent_Exit") // Happens only when persisting data is allowed
				    .errorUnsuppressed(e)
				    .detail("Reason", "ConsistencyCheckerOutdated")
				    .detail("ConsistencyCheckerId", consistencyCheckerId)
				    .detail("RetryTimes", retryTimes)
				    .detail("Round", round);
				return Void(); // Exit
			} else {
				TraceEvent("ConsistencyCheckUrgent_CoreWithRetriableFailure")
				    .errorUnsuppressed(e)
				    .detail("ConsistencyCheckerId", consistencyCheckerId)
				    .detail("RetryTimes", retryTimes)
				    .detail("Round", round);
				wait(delay(10.0));
				retryTimes++;
			}
		}

		wait(delay(10.0)); // Backoff 10 seconds for the next round

		// Decide and enforce the consistencyCheckerId for the next round
		consistencyCheckerId = deterministicRandom()->randomInt64(SERVER_KNOBS->CONSISTENCY_CHECK_ID_MIN,
		                                                          SERVER_KNOBS->CONSISTENCY_CHECK_ID_MAX_PLUS_ONE);
		if (SERVER_KNOBS->CONSISTENCY_CHECK_USE_PERSIST_DATA) {
			state int retryTimesForUpdatingCheckerId = 0;
			loop {
				try {
					wait(persistConsistencyCheckerId(cx, consistencyCheckerId));
					if (g_network->isSimulated() && deterministicRandom()->random01() < 0.05) {
						throw operation_failed(); // Introduce random failure
					}
					break; // Continue to the next round
				} catch (Error& e) {
					if (e.code() == error_code_actor_cancelled) {
						throw e;
					}
					if (retryTimesForUpdatingCheckerId > 50) {
						TraceEvent("ConsistencyCheckUrgent_Exit")
						    .errorUnsuppressed(e)
						    .detail("Reason", "PersistConsistencyCheckerIdFailed")
						    .detail("ConsistencyCheckerId", consistencyCheckerId)
						    .detail("Round", round);
						return Void(); // Exit
					}
					wait(delay(1.0));
					retryTimesForUpdatingCheckerId++;
				}
			}
		}
	}
}

ACTOR Future<Void> checkConsistencyUrgentSim(Database cx,
                                             std::vector<TesterInterface> testers,
                                             double softTimeLimit,
                                             double databasePingDelay,
                                             Reference<AsyncVar<ServerDBInfo>> dbInfo) {
	state TestSpec spec;
	Standalone<VectorRef<KeyValueRef>> options;
	spec.title = LiteralStringRef("ConsistencyCheck");
	spec.databasePingDelay = databasePingDelay;
	spec.timeout = 32000;
	spec.phases = TestWorkload::SETUP | TestWorkload::EXECUTION;
	options.push_back_deep(options.arena(),
	                       KeyValueRef(LiteralStringRef("testName"), LiteralStringRef("ConsistencyCheck")));
	options.push_back_deep(options.arena(),
	                       KeyValueRef(LiteralStringRef("performQuiescentChecks"), LiteralStringRef("false")));
	options.push_back_deep(options.arena(),
	                       KeyValueRef(LiteralStringRef("performCacheCheck"), LiteralStringRef("false")));
	options.push_back_deep(options.arena(),
	                       KeyValueRef(LiteralStringRef("performTSSCheck"), LiteralStringRef("false")));
	options.push_back_deep(options.arena(), KeyValueRef(LiteralStringRef("distributed"), LiteralStringRef("true")));
	spec.options.push_back_deep(spec.options.arena(), options);
	loop {
		TraceEvent("ConsistencyCheckUrgent_SimBegin");
		try {
			wait(runConsistencyCheckerUrgentCore(Reference<AsyncVar<Optional<ClusterControllerFullInterface>>>(),
			                                     cx,
			                                     testers,
			                                     1,
			                                     spec,
			                                     Optional<TenantName>(),
			                                     dbInfo));
			break;
		} catch (Error& e) {
			if (e.code() == error_code_actor_cancelled) {
				throw e;
			}
			if (e.code() == error_code_operation_failed) {
				continue;
			}
			TraceEvent("ConsistencyCheckUrgent_RepairDC");
			wait(repairDeadDatacenter(cx, dbInfo, "ConsistencyCheckUrgent"));
		}
	}
	return Void();
}

ACTOR Future<bool> runTest(Database cx,
                           std::vector<TesterInterface> testers,
                           TestSpec spec,
                           Reference<AsyncVar<ServerDBInfo>> dbInfo,
                           Optional<TenantName> defaultTenant) {
	state DistributedTestResults testResults;
	state double savedDisableDuration = 0;

	try {
		Future<DistributedTestResults> fTestResults = runWorkload(cx, testers, spec, defaultTenant);
		if (g_network->isSimulated() && spec.simConnectionFailuresDisableDuration > 0) {
			savedDisableDuration = g_simulator.connectionFailuresDisableDuration;
			g_simulator.connectionFailuresDisableDuration = spec.simConnectionFailuresDisableDuration;
		}
		if (spec.timeout > 0) {
			fTestResults = timeoutError(fTestResults, spec.timeout);
		}
		DistributedTestResults _testResults = wait(fTestResults);
		testResults = _testResults;
		logMetrics(testResults.metrics);
		if (g_network->isSimulated() && savedDisableDuration > 0) {
			g_simulator.connectionFailuresDisableDuration = savedDisableDuration;
		}
	} catch (Error& e) {
		if (e.code() == error_code_timed_out) {
			TraceEvent(SevError, "TestFailure")
			    .error(e)
			    .detail("Reason", "Test timed out")
			    .detail("Timeout", spec.timeout);
			fprintf(stderr, "ERROR: Test timed out after %d seconds.\n", spec.timeout);
			testResults.failures = testers.size();
			testResults.successes = 0;
		} else {
			TraceEvent(SevWarnAlways, "TestFailure").error(e).detail("Reason", e.what());
			throw;
		}
	}

	state bool ok = testResults.ok();

	if (spec.useDB) {
		if (spec.dumpAfterTest) {
			try {
				wait(timeoutError(dumpDatabase(cx, "dump after " + printable(spec.title) + ".html", allKeys), 30.0));
			} catch (Error& e) {
				TraceEvent(SevError, "TestFailure").error(e).detail("Reason", "Unable to dump database");
				ok = false;
			}

			wait(delay(1.0));
		}

		// Run the consistency check workload
		if (spec.runConsistencyCheck) {
			try {
				// Urgent consistency check
				wait(timeoutError(checkConsistencyUrgentSim(cx, testers, 18000, spec.databasePingDelay, dbInfo),
				                  20000.0));
				// Normal consistency check
				bool quiescent = g_network->isSimulated() ? !BUGGIFY : spec.waitForQuiescenceEnd;
				wait(timeoutError(checkConsistency(cx,
				                                   testers,
				                                   quiescent,
				                                   spec.runConsistencyCheckOnCache,
				                                   spec.runConsistencyCheckOnTSS,
				                                   10000.0,
				                                   18000,
				                                   spec.databasePingDelay,
				                                   dbInfo),
				                  20000.0));
			} catch (Error& e) {
				TraceEvent(SevError, "TestFailure").error(e).detail("Reason", "Unable to perform consistency check");
				ok = false;
			}
		}
	}

	TraceEvent(ok ? SevInfo : SevWarnAlways, "TestResults").detail("Workload", spec.title).detail("Passed", (int)ok);
	//.detail("Metrics", metricSummary);

	if (ok) {
		passCount++;
	} else {
		failCount++;
	}

	printf("%d test clients passed; %d test clients failed\n", testResults.successes, testResults.failures);

	if (spec.useDB && spec.clearAfterTest) {
		try {
			TraceEvent("TesterClearingDatabase").log();
			wait(timeoutError(clearData(cx), 1000.0));
		} catch (Error& e) {
			TraceEvent(SevError, "ErrorClearingDatabaseAfterTest").error(e);
			throw; // If we didn't do this, we don't want any later tests to run on this DB
		}

		wait(delay(1.0));
	}

	return ok;
}

std::map<std::string, std::function<void(const std::string&)>> testSpecGlobalKeys = {
	// These are read by SimulatedCluster and used before testers exist.  Thus, they must
	// be recognized and accepted, but there's no point in placing them into a testSpec.
	{ "extraDB", [](const std::string& value) { TraceEvent("TestParserTest").detail("ParsedExtraDB", ""); } },
	{ "configureLocked",
	  [](const std::string& value) { TraceEvent("TestParserTest").detail("ParsedConfigureLocked", ""); } },
	{ "minimumReplication",
	  [](const std::string& value) { TraceEvent("TestParserTest").detail("ParsedMinimumReplication", ""); } },
	{ "minimumRegions",
	  [](const std::string& value) { TraceEvent("TestParserTest").detail("ParsedMinimumRegions", ""); } },
	{ "logAntiQuorum",
	  [](const std::string& value) { TraceEvent("TestParserTest").detail("ParsedLogAntiQuorum", ""); } },
	{ "buggify", [](const std::string& value) { TraceEvent("TestParserTest").detail("ParsedBuggify", ""); } },
	// The test harness handles NewSeverity events specially.
	{ "StderrSeverity", [](const std::string& value) { TraceEvent("StderrSeverity").detail("NewSeverity", value); } },
	{ "ClientInfoLogging",
	  [](const std::string& value) {
	      if (value == "false") {
		      setNetworkOption(FDBNetworkOptions::DISABLE_CLIENT_STATISTICS_LOGGING);
	      }
	      // else { } It is enable by default for tester
	      TraceEvent("TestParserTest").detail("ClientInfoLogging", value);
	  } },
	{ "startIncompatibleProcess",
	  [](const std::string& value) { TraceEvent("TestParserTest").detail("ParsedStartIncompatibleProcess", value); } },
	{ "storageEngineExcludeTypes",
	  [](const std::string& value) { TraceEvent("TestParserTest").detail("ParsedStorageEngineExcludeTypes", ""); } },
	{ "maxTLogVersion",
	  [](const std::string& value) { TraceEvent("TestParserTest").detail("ParsedMaxTLogVersion", ""); } },
	{ "disableTss", [](const std::string& value) { TraceEvent("TestParserTest").detail("ParsedDisableTSS", ""); } },
	{ "disableHostname",
	  [](const std::string& value) { TraceEvent("TestParserTest").detail("ParsedDisableHostname", ""); } },
	{ "disableRemoteKVS", [](const std::string& value) { TraceEvent("TestParserTest").detail("ParsedRemoteKVS", ""); } }
};

std::map<std::string, std::function<void(const std::string& value, TestSpec* spec)>> testSpecTestKeys = {
	{ "testTitle",
	  [](const std::string& value, TestSpec* spec) {
	      spec->title = value;
	      TraceEvent("TestParserTest").detail("ParsedTest", spec->title);
	  } },
	{ "timeout",
	  [](const std::string& value, TestSpec* spec) {
	      sscanf(value.c_str(), "%d", &(spec->timeout));
	      ASSERT(spec->timeout > 0);
	      TraceEvent("TestParserTest").detail("ParsedTimeout", spec->timeout);
	  } },
	{ "databasePingDelay",
	  [](const std::string& value, TestSpec* spec) {
	      double databasePingDelay;
	      sscanf(value.c_str(), "%lf", &databasePingDelay);
	      ASSERT(databasePingDelay >= 0);
	      if (!spec->useDB && databasePingDelay > 0) {
		      TraceEvent(SevError, "TestParserError")
		          .detail("Reason", "Cannot have non-zero ping delay on test that does not use database")
		          .detail("PingDelay", databasePingDelay)
		          .detail("UseDB", spec->useDB);
		      ASSERT(false);
	      }
	      spec->databasePingDelay = databasePingDelay;
	      TraceEvent("TestParserTest").detail("ParsedPingDelay", spec->databasePingDelay);
	  } },
	{ "runSetup",
	  [](const std::string& value, TestSpec* spec) {
	      spec->phases = TestWorkload::EXECUTION | TestWorkload::CHECK | TestWorkload::METRICS;
	      if (value == "true")
		      spec->phases |= TestWorkload::SETUP;
	      TraceEvent("TestParserTest").detail("ParsedSetupFlag", (spec->phases & TestWorkload::SETUP) != 0);
	  } },
	{ "dumpAfterTest",
	  [](const std::string& value, TestSpec* spec) {
	      spec->dumpAfterTest = (value == "true");
	      TraceEvent("TestParserTest").detail("ParsedDumpAfter", spec->dumpAfterTest);
	  } },
	{ "clearAfterTest",
	  [](const std::string& value, TestSpec* spec) {
	      spec->clearAfterTest = (value == "true");
	      TraceEvent("TestParserTest").detail("ParsedClearAfter", spec->clearAfterTest);
	  } },
	{ "useDB",
	  [](const std::string& value, TestSpec* spec) {
	      spec->useDB = (value == "true");
	      TraceEvent("TestParserTest").detail("ParsedUseDB", spec->useDB);
	      if (!spec->useDB)
		      spec->databasePingDelay = 0.0;
	  } },
	{ "startDelay",
	  [](const std::string& value, TestSpec* spec) {
	      sscanf(value.c_str(), "%lf", &spec->startDelay);
	      TraceEvent("TestParserTest").detail("ParsedStartDelay", spec->startDelay);
	  } },
	{ "runConsistencyCheck",
	  [](const std::string& value, TestSpec* spec) {
	      spec->runConsistencyCheck = (value == "true");
	      TraceEvent("TestParserTest").detail("ParsedRunConsistencyCheck", spec->runConsistencyCheck);
	  } },
	{ "runConsistencyCheckOnCache",
	  [](const std::string& value, TestSpec* spec) {
	      spec->runConsistencyCheckOnCache = (value == "true");
	      TraceEvent("TestParserTest").detail("ParsedRunConsistencyCheckOnCache", spec->runConsistencyCheckOnCache);
	  } },
	{ "runConsistencyCheckOnTSS",
	  [](const std::string& value, TestSpec* spec) {
	      spec->runConsistencyCheckOnTSS = (value == "true");
	      TraceEvent("TestParserTest").detail("ParsedRunConsistencyCheckOnTSS", spec->runConsistencyCheckOnTSS);
	  } },
	{ "waitForQuiescence",
	  [](const std::string& value, TestSpec* spec) {
	      bool toWait = value == "true";
	      spec->waitForQuiescenceBegin = toWait;
	      spec->waitForQuiescenceEnd = toWait;
	      TraceEvent("TestParserTest").detail("ParsedWaitForQuiescence", toWait);
	  } },
	{ "waitForQuiescenceBegin",
	  [](const std::string& value, TestSpec* spec) {
	      bool toWait = value == "true";
	      spec->waitForQuiescenceBegin = toWait;
	      TraceEvent("TestParserTest").detail("ParsedWaitForQuiescenceBegin", toWait);
	  } },
	{ "waitForQuiescenceEnd",
	  [](const std::string& value, TestSpec* spec) {
	      bool toWait = value == "true";
	      spec->waitForQuiescenceEnd = toWait;
	      TraceEvent("TestParserTest").detail("ParsedWaitForQuiescenceEnd", toWait);
	  } },
	{ "simCheckRelocationDuration",
	  [](const std::string& value, TestSpec* spec) {
	      spec->simCheckRelocationDuration = (value == "true");
	      TraceEvent("TestParserTest").detail("ParsedSimCheckRelocationDuration", spec->simCheckRelocationDuration);
	  } },
	{ "connectionFailuresDisableDuration",
	  [](const std::string& value, TestSpec* spec) {
	      double connectionFailuresDisableDuration;
	      sscanf(value.c_str(), "%lf", &connectionFailuresDisableDuration);
	      ASSERT(connectionFailuresDisableDuration >= 0);
	      spec->simConnectionFailuresDisableDuration = connectionFailuresDisableDuration;
	      TraceEvent("TestParserTest")
	          .detail("ParsedSimConnectionFailuresDisableDuration", spec->simConnectionFailuresDisableDuration);
	  } },
	{ "simBackupAgents",
	  [](const std::string& value, TestSpec* spec) {
	      if (value == "BackupToFile" || value == "BackupToFileAndDB")
		      spec->simBackupAgents = ISimulator::BackupAgentType::BackupToFile;
	      else
		      spec->simBackupAgents = ISimulator::BackupAgentType::NoBackupAgents;
	      TraceEvent("TestParserTest").detail("ParsedSimBackupAgents", spec->simBackupAgents);

	      if (value == "BackupToDB" || value == "BackupToFileAndDB")
		      spec->simDrAgents = ISimulator::BackupAgentType::BackupToDB;
	      else
		      spec->simDrAgents = ISimulator::BackupAgentType::NoBackupAgents;
	      TraceEvent("TestParserTest").detail("ParsedSimDrAgents", spec->simDrAgents);
	  } },
	{ "checkOnly",
	  [](const std::string& value, TestSpec* spec) {
	      if (value == "true")
		      spec->phases = TestWorkload::CHECK;
	  } },
	{ "restorePerpetualWiggleSetting",
	  [](const std::string& value, TestSpec* spec) {
	      if (value == "false")
		      spec->restorePerpetualWiggleSetting = false;
	  } },
};

std::vector<TestSpec> readTests(std::ifstream& ifs) {
	TestSpec spec;
	std::vector<TestSpec> result;
	Standalone<VectorRef<KeyValueRef>> workloadOptions;
	std::string cline;
	bool beforeFirstTest = true;
	bool parsingWorkloads = false;

	while (ifs.good()) {
		getline(ifs, cline);
		std::string line = removeWhitespace(cline);
		if (!line.size() || line.find(';') == 0)
			continue;

		size_t found = line.find('=');
		if (found == std::string::npos)
			// hmmm, not good
			continue;
		std::string attrib = removeWhitespace(line.substr(0, found));
		std::string value = removeWhitespace(line.substr(found + 1));

		if (attrib == "testTitle") {
			beforeFirstTest = false;
			parsingWorkloads = false;
			if (workloadOptions.size()) {
				spec.options.push_back_deep(spec.options.arena(), workloadOptions);
				workloadOptions = Standalone<VectorRef<KeyValueRef>>();
			}
			if (spec.options.size() && spec.title.size()) {
				result.push_back(spec);
				spec = TestSpec();
			}

			testSpecTestKeys[attrib](value, &spec);
		} else if (testSpecTestKeys.find(attrib) != testSpecTestKeys.end()) {
			if (parsingWorkloads)
				TraceEvent(SevError, "TestSpecTestParamInWorkload").detail("Attrib", attrib).detail("Value", value);
			testSpecTestKeys[attrib](value, &spec);
		} else if (testSpecGlobalKeys.find(attrib) != testSpecGlobalKeys.end()) {
			if (!beforeFirstTest)
				TraceEvent(SevError, "TestSpecGlobalParamInTest").detail("Attrib", attrib).detail("Value", value);
			testSpecGlobalKeys[attrib](value);
		} else {
			if (attrib == "testName") {
				parsingWorkloads = true;
				if (workloadOptions.size()) {
					TraceEvent("TestParserFlush").detail("Reason", "new (compound) test");
					spec.options.push_back_deep(spec.options.arena(), workloadOptions);
					workloadOptions = Standalone<VectorRef<KeyValueRef>>();
				}
			}

			workloadOptions.push_back_deep(workloadOptions.arena(), KeyValueRef(StringRef(attrib), StringRef(value)));
			TraceEvent("TestParserOption").detail("ParsedKey", attrib).detail("ParsedValue", value);
		}
	}
	if (workloadOptions.size())
		spec.options.push_back_deep(spec.options.arena(), workloadOptions);
	if (spec.options.size() && spec.title.size()) {
		result.push_back(spec);
	}

	return result;
}

template <typename T>
std::string toml_to_string(const T& value) {
	// TOML formatting converts numbers to strings exactly how they're in the file
	// and thus, is equivalent to testspec.  However, strings are quoted, so we
	// must remove the quotes.
	if (value.type() == toml::value_t::string) {
		const std::string& formatted = toml::format(value);
		return formatted.substr(1, formatted.size() - 2);
	} else {
		return toml::format(value);
	}
}

struct TestSet {
	KnobKeyValuePairs overrideKnobs;
	std::vector<TestSpec> testSpecs;
};

namespace {

// In the current TOML scope, look for "knobs" field. If exists, translate all
// key value pairs into KnobKeyValuePairs
KnobKeyValuePairs getOverriddenKnobKeyValues(const toml::value& context) {
	KnobKeyValuePairs result;

	try {
		const toml::array& overrideKnobs = toml::find(context, "knobs").as_array();
		for (const toml::value& knob : overrideKnobs) {
			for (const auto& [key, value_] : knob.as_table()) {
				const std::string& value = toml_to_string(value_);
				ParsedKnobValue parsedValue = CLIENT_KNOBS->parseKnobValue(key, value);
				if (std::get_if<NoKnobFound>(&parsedValue)) {
					parsedValue = SERVER_KNOBS->parseKnobValue(key, value);
				}
				if (std::get_if<NoKnobFound>(&parsedValue)) {
					TraceEvent(SevError, "TestSpecUnrecognizedKnob")
					    .detail("KnobName", key)
					    .detail("OverrideValue", value);
					continue;
				}
				result.set(key, parsedValue);
			}
		}
	} catch (const std::out_of_range&) {
		// No knobs field in this scope, this is not an error
	}

	return result;
}

} // namespace

TestSet readTOMLTests_(std::string fileName) {
	Standalone<VectorRef<KeyValueRef>> workloadOptions;
	TestSet result;

	const toml::value& conf = toml::parse(fileName);

	// Parse the global knob changes
	result.overrideKnobs = getOverriddenKnobKeyValues(conf);

	// Then parse each test
	const toml::array& tests = toml::find(conf, "test").as_array();
	for (const toml::value& test : tests) {
		TestSpec spec;

		// First handle all test-level settings
		for (const auto& [k, v] : test.as_table()) {
			if (k == "workload" || k == "knobs") {
				continue;
			}
			if (testSpecTestKeys.find(k) != testSpecTestKeys.end()) {
				testSpecTestKeys[k](toml_to_string(v), &spec);
			} else {
				TraceEvent(SevError, "TestSpecUnrecognizedTestParam")
				    .detail("Attrib", k)
				    .detail("Value", toml_to_string(v));
			}
		}

		// And then copy the workload attributes to spec.options
		const toml::array& workloads = toml::find(test, "workload").as_array();
		for (const toml::value& workload : workloads) {
			workloadOptions = Standalone<VectorRef<KeyValueRef>>();
			TraceEvent("TestParserFlush").detail("Reason", "new (compound) test");
			for (const auto& [attrib, v] : workload.as_table()) {
				const std::string& value = toml_to_string(v);
				workloadOptions.push_back_deep(workloadOptions.arena(),
				                               KeyValueRef(StringRef(attrib), StringRef(value)));
				TraceEvent("TestParserOption").detail("ParsedKey", attrib).detail("ParsedValue", value);
			}
			spec.options.push_back_deep(spec.options.arena(), workloadOptions);
		}

		// And then copy the knob attributes to spec.overrideKnobs
		spec.overrideKnobs = getOverriddenKnobKeyValues(test);

		result.testSpecs.push_back(spec);
	}

	return result;
}

// A hack to catch and log std::exception, because TOML11 has very useful
// error messages, but the actor framework can't handle std::exception.
TestSet readTOMLTests(std::string fileName) {
	try {
		return readTOMLTests_(fileName);
	} catch (std::exception& e) {
		std::cerr << e.what() << std::endl;
		TraceEvent("TOMLParseError").detail("Error", printable(e.what()));
		// TODO: replace with toml_parse_error();
		throw unknown_error();
	}
}

ACTOR Future<Void> monitorServerDBInfo(Reference<AsyncVar<Optional<ClusterControllerFullInterface>>> ccInterface,
                                       LocalityData locality,
                                       Reference<AsyncVar<ServerDBInfo>> dbInfo) {
	// Initially most of the serverDBInfo is not known, but we know our locality right away
	ServerDBInfo localInfo;
	localInfo.myLocality = locality;
	dbInfo->set(localInfo);

	loop {
		GetServerDBInfoRequest req;
		req.knownServerInfoID = dbInfo->get().id;

		choose {
			when(ServerDBInfo _localInfo =
			         wait(ccInterface->get().present()
			                  ? brokenPromiseToNever(ccInterface->get().get().getServerDBInfo.getReply(req))
			                  : Never())) {
				ServerDBInfo localInfo = _localInfo;
				TraceEvent("GotServerDBInfoChange")
				    .detail("ChangeID", localInfo.id)
				    .detail("MasterID", localInfo.master.id())
				    .detail("RatekeeperID", localInfo.ratekeeper.present() ? localInfo.ratekeeper.get().id() : UID())
				    .detail("DataDistributorID",
				            localInfo.distributor.present() ? localInfo.distributor.get().id() : UID());

				localInfo.myLocality = locality;
				dbInfo->set(localInfo);
			}
			when(wait(ccInterface->onChange())) {
				if (ccInterface->get().present())
					TraceEvent("GotCCInterfaceChange")
					    .detail("CCID", ccInterface->get().get().id())
					    .detail("CCMachine", ccInterface->get().get().getWorkers.getEndpoint().getPrimaryAddress());
			}
		}
	}
}

// Disables connection failures after the given time seconds
ACTOR Future<Void> disableConnectionFailuresAfter(double seconds, std::string context) {
	if (g_network->isSimulated()) {
		TraceEvent(SevWarnAlways, ("ScheduleDisableConnectionFailures_" + context).c_str())
		    .detail("At", now() + seconds);
		wait(delay(seconds));
		disableConnectionFailures(context);
	}
	return Void();
}

/**
 * \brief Test orchestrator: sends test specification to testers in the right order and collects the results.
 *
 * There are multiple actors in this file with similar names (runTest, runTests) and slightly different signatures.
 *
 * This is the actual orchestrator. It reads the test specifications (from tests), prepares the cluster (by running the
 * configure command given in startingConfiguration) and then runs the workload.
 *
 * \param cc The cluster controller interface
 * \param ci Same as cc.clientInterface
 * \param testers The interfaces of the testers that should run the actual workloads
 * \param tests The test specifications to run
 * \param startingConfiguration If non-empty, the orchestrator will attempt to set this configuration before starting
 * the tests.
 * \param locality client locality (it seems this is unused?)
 *
 * \returns A future which will be set after all tests finished.
 */
ACTOR Future<Void> runTests(Reference<AsyncVar<Optional<struct ClusterControllerFullInterface>>> cc,
                            Reference<AsyncVar<Optional<struct ClusterInterface>>> ci,
                            std::vector<TesterInterface> testers,
                            std::vector<TestSpec> tests,
                            StringRef startingConfiguration,
                            LocalityData locality,
                            Optional<TenantName> defaultTenant) {
	state Database cx;
	state Reference<AsyncVar<ServerDBInfo>> dbInfo(new AsyncVar<ServerDBInfo>);
	state Future<Void> ccMonitor = monitorServerDBInfo(cc, LocalityData(), dbInfo); // FIXME: locality

	state bool useDB = false;
	state bool waitForQuiescenceBegin = false;
	state bool waitForQuiescenceEnd = false;
	state bool restorePerpetualWiggleSetting = false;
	state bool perpetualWiggleEnabled = false;
	state double startDelay = 0.0;
	state double databasePingDelay = 1e9;
	state ISimulator::BackupAgentType simBackupAgents = ISimulator::BackupAgentType::NoBackupAgents;
	state ISimulator::BackupAgentType simDrAgents = ISimulator::BackupAgentType::NoBackupAgents;
	state bool enableDD = false;
	if (tests.empty())
		useDB = true;
	for (auto iter = tests.begin(); iter != tests.end(); ++iter) {
		if (iter->useDB)
			useDB = true;
		if (iter->waitForQuiescenceBegin)
			waitForQuiescenceBegin = true;
		if (iter->waitForQuiescenceEnd)
			waitForQuiescenceEnd = true;
		if (iter->restorePerpetualWiggleSetting)
			restorePerpetualWiggleSetting = true;
		startDelay = std::max(startDelay, iter->startDelay);
		databasePingDelay = std::min(databasePingDelay, iter->databasePingDelay);
		if (iter->simBackupAgents != ISimulator::BackupAgentType::NoBackupAgents)
			simBackupAgents = iter->simBackupAgents;

		if (iter->simDrAgents != ISimulator::BackupAgentType::NoBackupAgents) {
			simDrAgents = iter->simDrAgents;
		}
		enableDD = enableDD || getOption(iter->options[0], LiteralStringRef("enableDD"), false);
	}

	if (g_network->isSimulated()) {
		g_simulator.backupAgents = simBackupAgents;
		g_simulator.drAgents = simDrAgents;
	}

	// turn off the database ping functionality if the suite of tests are not going to be using the database
	if (!useDB)
		databasePingDelay = 0.0;

	if (useDB) {
		cx = openDBOnServer(dbInfo);
		cx->defaultTenant = defaultTenant;
	}

	disableConnectionFailures("Tester");

	// Change the configuration (and/or create the database) if necessary
	printf("startingConfiguration:%s start\n", startingConfiguration.toString().c_str());
	printSimulatedTopology();
	if (useDB && startingConfiguration != StringRef()) {
		try {
			wait(timeoutError(changeConfiguration(cx, testers, startingConfiguration), 2000.0));
			if (g_network->isSimulated() && enableDD) {
				wait(success(setDDMode(cx, 1)));
			}
		} catch (Error& e) {
			TraceEvent(SevError, "TestFailure").error(e).detail("Reason", "Unable to set starting configuration");
		}
		if (restorePerpetualWiggleSetting) {
			std::string_view confView(reinterpret_cast<const char*>(startingConfiguration.begin()),
			                          startingConfiguration.size());
			const std::string setting = "perpetual_storage_wiggle:=";
			auto pos = confView.find(setting);
			if (pos != confView.npos && confView.at(pos + setting.size()) == '1') {
				perpetualWiggleEnabled = true;
			}
		}
	}

	if (useDB && defaultTenant.present()) {
		TraceEvent("CreatingDefaultTenant").detail("Tenant", defaultTenant.get());
		wait(ManagementAPI::createTenant(cx.getReference(), defaultTenant.get()));
	}

	if (useDB && waitForQuiescenceBegin) {
		TraceEvent("TesterStartingPreTestChecks")
		    .detail("DatabasePingDelay", databasePingDelay)
		    .detail("StartDelay", startDelay);
		try {
			wait(quietDatabase(cx, dbInfo, "Start") ||
			     (databasePingDelay == 0.0
			          ? Never()
			          : testDatabaseLiveness(cx, databasePingDelay, "QuietDatabaseStart", startDelay)));
		} catch (Error& e) {
			TraceEvent("QuietDatabaseStartExternalError").error(e);
			throw;
		}

		if (perpetualWiggleEnabled) { // restore the enabled perpetual storage wiggle setting
			printf("Set perpetual_storage_wiggle=1 ...\n");
			wait(setPerpetualStorageWiggle(cx, true, LockAware::True));
			printf("Set perpetual_storage_wiggle=1 Done.\n");
		}
	}

	enableConnectionFailures("Tester");
	state Future<Void> disabler = disableConnectionFailuresAfter(FLOW_KNOBS->SIM_SPEEDUP_AFTER_SECONDS, "Tester");
	state Future<Void> repairDataCenter;
	if (useDB) {
		Future<Void> reconfigure = reconfigureAfter(cx, FLOW_KNOBS->SIM_SPEEDUP_AFTER_SECONDS, dbInfo, "Tester");
		repairDataCenter = reconfigure;
	}

	TraceEvent("TestsExpectedToPass").detail("Count", tests.size());
	state int idx = 0;
	state std::unique_ptr<KnobProtectiveGroup> knobProtectiveGroup;
	for (; idx < tests.size(); idx++) {
		printf("Run test:%s start\n", tests[idx].title.toString().c_str());
		knobProtectiveGroup = std::make_unique<KnobProtectiveGroup>(tests[idx].overrideKnobs);
		wait(success(runTest(cx, testers, tests[idx], dbInfo, defaultTenant)));
		knobProtectiveGroup.reset(nullptr);
		printf("Run test:%s Done.\n", tests[idx].title.toString().c_str());
		// do we handle a failure here?
	}

	printf("\n%d tests passed; %d tests failed.\n", passCount, failCount);

	// If the database was deleted during the workload we need to recreate the database
	if (tests.empty() || useDB) {
		if (waitForQuiescenceEnd) {
			printf("Waiting for DD to end...\n");
			try {
				wait(quietDatabase(cx, dbInfo, "End", 0, 2e6, 2e6) ||
				     (databasePingDelay == 0.0 ? Never()
				                               : testDatabaseLiveness(cx, databasePingDelay, "QuietDatabaseEnd")));
			} catch (Error& e) {
				TraceEvent("QuietDatabaseEndExternalError").error(e);
				throw;
			}
		}
	}
	printf("\n");

	return Void();
}

/**
 * \brief Proxy function that waits until enough testers are available and then calls into the orchestrator.
 *
 * There are multiple actors in this file with similar names (runTest, runTests) and slightly different signatures.
 *
 * This actor wraps the actual orchestrator (also called runTests). But before calling that actor, it waits for enough
 * testers to come up.
 *
 * \param cc The cluster controller interface
 * \param ci Same as cc.clientInterface
 * \param tests The test specifications to run
 * \param minTestersExpected The number of testers to expect. This actor will block until it can find this many testers.
 * \param startingConfiguration If non-empty, the orchestrator will attempt to set this configuration before starting
 * the tests.
 * \param locality client locality (it seems this is unused?)
 *
 * \returns A future which will be set after all tests finished.
 */
ACTOR Future<Void> runTests(Reference<AsyncVar<Optional<struct ClusterControllerFullInterface>>> cc,
                            Reference<AsyncVar<Optional<struct ClusterInterface>>> ci,
                            std::vector<TestSpec> tests,
                            test_location_t at,
                            int minTestersExpected,
                            StringRef startingConfiguration,
                            LocalityData locality,
                            Optional<TenantName> defaultTenant) {
	state int flags = (at == TEST_ON_SERVERS ? 0 : GetWorkersRequest::TESTER_CLASS_ONLY) |
	                  GetWorkersRequest::NON_EXCLUDED_PROCESSES_ONLY;
	TraceEvent("RunTests").detail("TestOnServers", at == TEST_ON_SERVERS);
	state Future<Void> testerTimeout = delay(600.0); // wait 600 sec for testers to show up
	state std::vector<WorkerDetails> workers;

	loop {
		choose {
			when(std::vector<WorkerDetails> w =
			         wait(cc->get().present()
			                  ? brokenPromiseToNever(cc->get().get().getWorkers.getReply(GetWorkersRequest(flags)))
			                  : Never())) {
				if (w.size() >= minTestersExpected) {
					workers = w;
					break;
				}
				wait(delay(SERVER_KNOBS->WORKER_POLL_DELAY));
			}
			when(wait(cc->onChange())) {}
			when(wait(testerTimeout)) {
				TraceEvent(SevError, "TesterRecruitmentTimeout").log();
				throw timed_out();
			}
		}
	}

	std::vector<TesterInterface> ts;
	ts.reserve(workers.size());
	for (int i = 0; i < workers.size(); i++)
		ts.push_back(workers[i].interf.testerInterface);

	wait(runTests(cc, ci, ts, tests, startingConfiguration, locality, defaultTenant));
	return Void();
}

ACTOR Future<Void> runConsistencyCheckerUrgentHolder(Reference<AsyncVar<Optional<ClusterControllerFullInterface>>> cc,
                                                     Database cx,
                                                     Optional<std::vector<TesterInterface>> testers,
                                                     int minTestersExpected,
                                                     TestSpec testSpec,
                                                     Optional<TenantName> defaultTenant,
                                                     Reference<AsyncVar<ServerDBInfo>> dbInf) {
	loop {
		wait(runConsistencyCheckerUrgentCore(cc, cx, testers, minTestersExpected, testSpec, defaultTenant, dbInf));
		wait(delay(CLIENT_KNOBS->CONSISTENCY_CHECK_URGENT_NEXT_WAIT_TIME));
	}
}

/**
 * \brief Set up testing environment and run the given tests on a cluster.
 *
 * There are multiple actors in this file with similar names (runTest, runTests) and slightly different signatures.
 *
 * This actor is usually the first entry point into the test environment. It itself doesn't implement too much
 * functionality. Its main purpose is to generate the test specification from passed arguments and then call into the
 * correct actor which will orchestrate the actual test.
 *
 * \param connRecord A cluster connection record. Not all tests require a functional cluster but all tests require
 * a cluster record.
 * \param whatToRun TEST_TYPE_FROM_FILE to read the test description from a passed toml file or
 * TEST_TYPE_CONSISTENCY_CHECK to generate a test spec for consistency checking
 * \param at TEST_HERE: this process will act as a test client and execute the given workload. TEST_ON_SERVERS: Run a
 * test client on every worker in the cluster. TEST_ON_TESTERS: Run a test client on all servers with class Test
 * \param minTestersExpected In at is not TEST_HERE, this will instruct the orchestrator until it can find at least
 * minTestersExpected test-clients. This is usually passed through from a command line argument. In simulation, the
 * simulator will pass the number of testers that it started.
 * \param fileName The path to the toml-file containing the test description. Is ignored if whatToRun !=
 * TEST_TYPE_FROM_FILE
 * \param startingConfiguration Can be used to configure a cluster before running the test. If this is an empty string,
 * it will be ignored, otherwise it will be passed to changeConfiguration.
 * \param locality The client locality to be used. This is only used if at == TEST_HERE
 *
 * \returns A future which will be set after all tests finished.
 */
ACTOR Future<Void> runTests(Reference<IClusterConnectionRecord> connRecord,
                            test_type_t whatToRun,
                            test_location_t at,
                            int minTestersExpected,
                            std::string fileName,
                            StringRef startingConfiguration,
                            LocalityData locality,
                            UnitTestParameters testOptions,
                            Optional<TenantName> defaultTenant) {
	state TestSet testSet;
	state std::unique_ptr<KnobProtectiveGroup> knobProtectiveGroup(nullptr);
	auto cc = makeReference<AsyncVar<Optional<ClusterControllerFullInterface>>>();
	auto ci = makeReference<AsyncVar<Optional<ClusterInterface>>>();
	std::vector<Future<Void>> actors;
	if (connRecord) {
		actors.push_back(reportErrors(monitorLeader(connRecord, cc), "MonitorLeader"));
		actors.push_back(reportErrors(extractClusterInterface(cc, ci), "ExtractClusterInterface"));
	}

	if (whatToRun == TEST_TYPE_CONSISTENCY_CHECK_URGENT) {
		// consistencyCheckerId must be not 0, indicating this is in urgent mode of consistency checker
		TestSpec spec;
		Standalone<VectorRef<KeyValueRef>> options;
		spec.title = LiteralStringRef("ConsistencyCheck");
		spec.databasePingDelay = 0;
		spec.timeout = 0;
		spec.waitForQuiescenceBegin = false;
		spec.waitForQuiescenceEnd = false;
		spec.phases = TestWorkload::SETUP | TestWorkload::EXECUTION;
		std::string rateLimitMax = format("%d", CLIENT_KNOBS->CONSISTENCY_CHECK_RATE_LIMIT_MAX);
		options.push_back_deep(options.arena(),
		                       KeyValueRef(LiteralStringRef("testName"), LiteralStringRef("ConsistencyCheck")));
		options.push_back_deep(options.arena(),
		                       KeyValueRef(LiteralStringRef("performQuiescentChecks"), LiteralStringRef("false")));
		options.push_back_deep(
		    options.arena(),
		    KeyValueRef(LiteralStringRef("distributed"),
		                LiteralStringRef("false"))); // The distribution mechanism does not rely on this flag
		options.push_back_deep(options.arena(),
		                       KeyValueRef(LiteralStringRef("failureIsError"), LiteralStringRef("true")));
		options.push_back_deep(options.arena(), KeyValueRef(LiteralStringRef("indefinite"), LiteralStringRef("false")));
		options.push_back_deep(options.arena(), KeyValueRef(LiteralStringRef("rateLimitMax"), StringRef(rateLimitMax)));
		options.push_back_deep(options.arena(),
		                       KeyValueRef(LiteralStringRef("shuffleShards"), LiteralStringRef("false")));
		spec.options.push_back_deep(spec.options.arena(), options);
		testSet.testSpecs.push_back(spec);
	} else if (whatToRun == TEST_TYPE_CONSISTENCY_CHECK) {
		TestSpec spec;
		Standalone<VectorRef<KeyValueRef>> options;
		spec.title = LiteralStringRef("ConsistencyCheck");
		spec.databasePingDelay = 0;
		spec.timeout = 0;
		spec.waitForQuiescenceBegin = false;
		spec.waitForQuiescenceEnd = false;
		std::string rateLimitMax = format("%d", CLIENT_KNOBS->CONSISTENCY_CHECK_RATE_LIMIT_MAX);
		options.push_back_deep(options.arena(),
		                       KeyValueRef(LiteralStringRef("testName"), LiteralStringRef("ConsistencyCheck")));
		options.push_back_deep(options.arena(),
		                       KeyValueRef(LiteralStringRef("performQuiescentChecks"), LiteralStringRef("false")));
		options.push_back_deep(options.arena(),
		                       KeyValueRef(LiteralStringRef("distributed"), LiteralStringRef("false")));
		options.push_back_deep(options.arena(),
		                       KeyValueRef(LiteralStringRef("failureIsError"), LiteralStringRef("true")));
		options.push_back_deep(options.arena(), KeyValueRef(LiteralStringRef("indefinite"), LiteralStringRef("true")));
		options.push_back_deep(options.arena(), KeyValueRef(LiteralStringRef("rateLimitMax"), StringRef(rateLimitMax)));
		options.push_back_deep(options.arena(),
		                       KeyValueRef(LiteralStringRef("shuffleShards"), LiteralStringRef("true")));
		spec.options.push_back_deep(spec.options.arena(), options);
		testSet.testSpecs.push_back(spec);
	} else if (whatToRun == TEST_TYPE_UNIT_TESTS) {
		TestSpec spec;
		Standalone<VectorRef<KeyValueRef>> options;
		spec.title = LiteralStringRef("UnitTests");
		spec.startDelay = 0;
		spec.useDB = false;
		spec.timeout = 0;
		options.push_back_deep(options.arena(),
		                       KeyValueRef(LiteralStringRef("testName"), LiteralStringRef("UnitTests")));
		options.push_back_deep(options.arena(), KeyValueRef(LiteralStringRef("testsMatching"), fileName));
		// Add unit test options as test spec options
		for (auto& kv : testOptions.params) {
			options.push_back_deep(options.arena(), KeyValueRef(kv.first, kv.second));
		}
		spec.options.push_back_deep(spec.options.arena(), options);
		testSet.testSpecs.push_back(spec);
	} else {
		std::ifstream ifs;
		ifs.open(fileName.c_str(), std::ifstream::in);
		if (!ifs.good()) {
			TraceEvent(SevError, "TestHarnessFail")
			    .detail("Reason", "file open failed")
			    .detail("File", fileName.c_str());
			fprintf(stderr, "ERROR: Could not open file `%s'\n", fileName.c_str());
			return Void();
		}
		enableClientInfoLogging(); // Enable Client Info logging by default for tester
		if (boost::algorithm::ends_with(fileName, ".txt")) {
			testSet.testSpecs = readTests(ifs);
		} else if (boost::algorithm::ends_with(fileName, ".toml")) {
			// TOML is weird about opening the file as binary on windows, so we
			// just let TOML re-open the file instead of using ifs.
			testSet = readTOMLTests(fileName);
		} else {
			TraceEvent(SevError, "TestHarnessFail")
			    .detail("Reason", "unknown tests specification extension")
			    .detail("File", fileName.c_str());
			return Void();
		}
		ifs.close();
	}

	knobProtectiveGroup = std::make_unique<KnobProtectiveGroup>(testSet.overrideKnobs);
	Future<Void> tests;
	if (whatToRun == TEST_TYPE_CONSISTENCY_CHECK_URGENT) {
		state Database cx;
		state Reference<AsyncVar<ServerDBInfo>> dbInfo(new AsyncVar<ServerDBInfo>);
		state Future<Void> ccMonitor = monitorServerDBInfo(cc, LocalityData(), dbInfo); // FIXME: locality
		cx = openDBOnServer(dbInfo);
		cx->defaultTenant = defaultTenant;
		tests = reportErrors(runConsistencyCheckerUrgentHolder(cc,
		                                                       cx,
		                                                       Optional<std::vector<TesterInterface>>(),
		                                                       minTestersExpected,
		                                                       testSet.testSpecs[0],
		                                                       defaultTenant,
		                                                       dbInfo),
		                     "runConsistencyCheckerUrgentCore");
	} else if (at == TEST_HERE) {
		auto db = makeReference<AsyncVar<ServerDBInfo>>();
		std::vector<TesterInterface> iTesters(1);
		actors.push_back(
		    reportErrors(monitorServerDBInfo(cc, LocalityData(), db), "MonitorServerDBInfo")); // FIXME: Locality
		actors.push_back(reportErrors(testerServerCore(iTesters[0], connRecord, db, locality), "TesterServerCore"));
		tests = runTests(cc, ci, iTesters, testSet.testSpecs, startingConfiguration, locality, defaultTenant);
	} else {
		tests = reportErrors(
		    runTests(cc, ci, testSet.testSpecs, at, minTestersExpected, startingConfiguration, locality, defaultTenant),
		    "RunTests");
	}

	choose {
		when(wait(tests)) {
			return Void();
		}
		when(wait(quorum(actors, 1))) {
			ASSERT(false);
			throw internal_error();
		}
	}
}

namespace {
ACTOR Future<Void> testExpectedErrorImpl(Future<Void> test,
                                         const char* testDescr,
                                         Optional<Error> expectedError,
                                         Optional<bool*> successFlag,
                                         std::map<std::string, std::string> details,
                                         Optional<Error> throwOnError,
                                         UID id) {
	state Error actualError;
	try {
		wait(test);
	} catch (Error& e) {
		if (e.code() == error_code_actor_cancelled) {
			throw e;
		}
		actualError = e;
		// The test failed as expected
		if (!expectedError.present() || actualError.code() == expectedError.get().code()) {
			return Void();
		}
	}

	// The test has failed
	if (successFlag.present()) {
		*(successFlag.get()) = false;
	}
	TraceEvent evt(SevError, "TestErrorFailed", id);
	evt.detail("TestDescription", testDescr);
	if (expectedError.present()) {
		evt.detail("ExpectedError", expectedError.get().name());
		evt.detail("ExpectedErrorCode", expectedError.get().code());
	}
	if (actualError.isValid()) {
		evt.detail("ActualError", actualError.name());
		evt.detail("ActualErrorCode", actualError.code());
	} else {
		evt.detail("Reason", "Unexpected success");
	}

	// Make sure that no duplicate details were provided
	ASSERT(details.count("TestDescription") == 0);
	ASSERT(details.count("ExpectedError") == 0);
	ASSERT(details.count("ExpectedErrorCode") == 0);
	ASSERT(details.count("ActualError") == 0);
	ASSERT(details.count("ActualErrorCode") == 0);
	ASSERT(details.count("Reason") == 0);

	for (auto& p : details) {
		evt.detail(p.first.c_str(), p.second);
	}
	if (throwOnError.present()) {
		throw throwOnError.get();
	}
	return Void();
}
} // namespace

Future<Void> testExpectedError(Future<Void> test,
                               const char* testDescr,
                               Optional<Error> expectedError,
                               Optional<bool*> successFlag,
                               std::map<std::string, std::string> details,
                               Optional<Error> throwOnError,
                               UID id) {
	return testExpectedErrorImpl(test, testDescr, expectedError, successFlag, details, throwOnError, id);
}
