/*
 * Stats.h
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

#ifndef FDBRPC_STATS_H
#define FDBRPC_STATS_H
#include "flow/Error.h"
#include "flow/IRandom.h"
#include "flow/Knobs.h"
#include "flow/OTELMetrics.h"
#include "flow/serialize.h"
#include <string>
#include <type_traits>
#include "flow/swift_support.h"
#pragma once

// Yet another performance statistics interface
/*

struct MyCounters {
CounterCollection cc;
Counter foo, bar, baz;
MyCounters() : foo("foo", cc), bar("bar", cc), baz("baz", cc) {}
};

*/

#include <cstdint>
#include <cstddef>
#include "flow/flow.h"
#include "flow/TDMetric.actor.h"
#include "fdbrpc/DDSketch.h"

struct ICounter : public IMetric {
	// All counters have a name and value
	ICounter() : IMetric(knobToMetricModel(FLOW_KNOBS->METRICS_DATA_MODEL)) {}
	virtual std::string const& getName() const = 0;
	virtual int64_t getValue() const = 0;

	// Counters may also have rate and roughness
	virtual bool hasRate() const = 0;
	virtual double getRate() const = 0;
	virtual bool hasRoughness() const = 0;
	virtual double getRoughness() const = 0;

	virtual void resetInterval() = 0;

	virtual void remove() {}
	virtual bool suppressTrace() const { return false; }
};

template <>
struct Traceable<ICounter*> : std::true_type {
	static std::string toString(ICounter const* counter) {
		if (counter->hasRate() && counter->hasRoughness()) {
			return format("%g %g %lld", counter->getRate(), counter->getRoughness(), (long long)counter->getValue());
		} else {
			return format("%lld", (long long)counter->getValue());
		}
	}
};

class CounterCollection {
	friend class CounterCollectionImpl;

	std::string name;
	std::string id;
	std::vector<struct ICounter*> counters, countersToRemove;

	double logTime;

public:
	CounterCollection(std::string const& name, std::string const& id = std::string())
	  : name(name), id(id), logTime(0) {}
	~CounterCollection() {
		for (auto c : countersToRemove)
			c->remove();
	}

	void addCounter(ICounter* counter) { counters.push_back(counter); }

	// Call remove method on this counter in ~CounterCollection
	void markForRemoval(ICounter* counter) { countersToRemove.push_back(counter); }

	std::string const& getName() const { return name; }

	std::string const& getId() const { return id; }

	void logToTraceEvent(TraceEvent& te);

	Future<Void> traceCounters(
	    std::string const& traceEventName,
	    UID traceEventID,
	    double interval,
	    std::string const& trackLatestName = std::string(),
	    std::function<void(TraceEvent&)> const& decorator = [](auto& te) {});
};

struct Counter final : public ICounter, NonCopyable {
public:
	typedef int64_t Value;

	Counter(std::string const& name, CounterCollection& collection, bool skipTraceOnSilentInterval = false);

	void operator+=(Value delta);
	void operator++() { *this += 1; }
	void clear();
	void resetInterval() override;

	std::string const& getName() const override { return name; }

	Value getIntervalDelta() const { return interval_delta; }
	Value getValue() const override { return interval_start_value + interval_delta; }

	// dValue / dt
	double getRate() const override;

	// Measures the clumpiness or dispersion of the counter.
	// Computed as a normalized variance of the time between each incrementation of the value.
	// A delta of N is treated as N distinct increments, with N-1 increments having time span 0.
	// Normalization is performed by dividing each time sample by the mean time before taking variance.
	//
	// roughness = Variance(t/mean(T)) for time interval samples t in T
	//
	// A uniformly periodic counter will have roughness of 0
	// A uniformly periodic counter that increases in clumps of N will have roughness of N-1
	// A counter with exponentially distributed incrementations will have roughness of 1
	double getRoughness() const override;

	bool hasRate() const override { return true; }
	bool hasRoughness() const override { return true; }

	bool suppressTrace() const override { return skip_trace_on_silent_interval && getIntervalDelta() == 0; }

private:
	std::string name;
	double interval_start, last_event, interval_sq_time, roughness_interval_start;
	Value interval_delta, interval_start_value;
	Int64MetricHandle metric;
	bool skip_trace_on_silent_interval;
};

template <>
struct Traceable<Counter> : std::true_type {
	static std::string toString(Counter const& counter) {
		return Traceable<ICounter*>::toString((ICounter const*)&counter);
	}
};

template <class F>
struct SpecialCounter final : ICounter, FastAllocated<SpecialCounter<F>>, NonCopyable {
	SpecialCounter(CounterCollection& collection, std::string const& name, F&& f) : name(name), f(f) {
		collection.addCounter(this);
		collection.markForRemoval(this);
	}
	void remove() override { delete this; }

	std::string const& getName() const override { return name; }
	int64_t getValue() const override {
		auto result = f();
		// Disallow conversion from floating point to int64_t, since this has
		// been a source of confusion - e.g. a percentage represented as a
		// fraction between 0 and 1 is not meaningful after conversion to
		// int64_t.
		static_assert(!std::is_floating_point_v<decltype(result)>);
		return result;
	}

	void resetInterval() override {}

	bool hasRate() const override { return false; }
	double getRate() const override { throw internal_error(); }
	bool hasRoughness() const override { return false; }
	double getRoughness() const override { throw internal_error(); }

	std::string name;
	F f;
};
template <class F>
static void specialCounter(CounterCollection& collection, std::string const& name, F&& f) {
	new SpecialCounter<F>(collection, name, std::move(f));
}

FDB_BOOLEAN_PARAM(Filtered);

class LatencyBands {
	std::map<double, std::unique_ptr<Counter>> bands;
	std::unique_ptr<Counter> filteredCount;
	std::function<void(TraceEvent&)> decorator;

	std::string name;
	UID id;
	double loggingInterval;

	std::unique_ptr<CounterCollection> cc;
	Future<Void> logger;

	void insertBand(double value);

public:
	LatencyBands(
	    std::string const& name,
	    UID id,
	    double loggingInterval,
	    std::function<void(TraceEvent&)> const& decorator = [](auto&) {});

	LatencyBands(LatencyBands&&) = default;
	LatencyBands& operator=(LatencyBands&&) = default;

	void addThreshold(double value);
	void addMeasurement(double measurement, int count = 1, Filtered = Filtered::False);
	void clearBands();
	~LatencyBands();
};

class LatencySample : public IMetric {
public:
	LatencySample(std::string name,
	              UID id,
	              double loggingInterval,
	              double accuracy,
	              bool skipTraceOnSilentInterval = false);
	void addMeasurement(double measurement);

private:
	std::string name;
	UID id;
	// These UIDs below are needed to emit the tail latencies as gauges
	//
	// If an OTEL aggregator is able to directly accept and process histograms
	// the tail latency gauges won't necessarily be needed anymore since they can be
	// calculated directly from the emitted buckets. To support users who have an aggregator
	// who cannot accept histograms, the tails latencies are still directly emitted.
	UID p50id;
	UID p90id;
	UID p95id;
	UID p99id;
	UID p999id;
	double sampleEmit;

	DDSketch<double> sketch;
	Future<Void> logger;
	bool skipTraceOnSilentInterval;

	Reference<EventCacheHolder> latencySampleEventHolder;

	void logSample();
};

#endif
