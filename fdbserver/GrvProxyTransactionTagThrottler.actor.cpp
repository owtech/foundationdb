/*
 * GrvProxyTransactionTagThrottler.actor.cpp
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

#include "fdbserver/GrvProxyTransactionTagThrottler.h"
#include "flow/UnitTest.h"
#include "flow/actorcompiler.h" // must be last include

uint64_t GrvProxyTransactionTagThrottler::DelayedRequest::lastSequenceNumber = 0;

void GrvProxyTransactionTagThrottler::DelayedRequest::updateProxyTagThrottledDuration() {
	req.proxyTagThrottledDuration = now() - startTime;
}

void GrvProxyTransactionTagThrottler::TagQueue::setRate(double rate) {
	if (rateInfo.present()) {
		rateInfo.get().setRate(rate);
	} else {
		rateInfo = GrvTransactionRateInfo(rate);
	}
}

void GrvProxyTransactionTagThrottler::updateRates(TransactionTagMap<double> const& newRates) {
	for (const auto& [tag, rate] : newRates) {
		auto it = queues.find(tag);
		if (it == queues.end()) {
			queues[tag] = TagQueue(rate);
		} else {
			it->second.setRate(rate);
		}
	}

	// Clean up tags that did not appear in newRates
	for (auto& [tag, queue] : queues) {
		if (newRates.find(tag) == newRates.end()) {
			queue.rateInfo.reset();
		}
	}

	// TODO: Use std::erase_if in C++20
	for (auto it = queues.begin(); it != queues.end();) {
		const auto& [tag, queue] = *it;
		if (queue.requests.empty() && !queue.rateInfo.present()) {
			it = queues.erase(it);
		} else {
			++it;
		}
	}
}

void GrvProxyTransactionTagThrottler::addRequest(GetReadVersionRequest const& req) {
	ASSERT(req.isTagged());
	auto const& tag = req.tags.begin()->first;
	if (req.tags.size() > 1) {
		// The GrvProxyTransactionTagThrottler assumes that each GetReadVersionRequest
		// has at most one tag. If a transaction uses multiple tags and
		// SERVER_KNOBS->ENFORCE_TAG_THROTTLING_ON_PROXIES is enabled, there may be
		// unexpected behaviour, because only one tag is used for throttling.
		TraceEvent(SevWarnAlways, "GrvProxyTransactionTagThrottler_MultipleTags")
		    .detail("NumTags", req.tags.size())
		    .detail("UsingTag", printable(tag));
	}
	queues[tag].requests.emplace_back(req);
}

void GrvProxyTransactionTagThrottler::releaseTransactions(double elapsed,
                                                          SpannedDeque<GetReadVersionRequest>& outBatchPriority,
                                                          SpannedDeque<GetReadVersionRequest>& outDefaultPriority) {
	struct TagInfo {
		// Store pointers here to avoid frequent std::unordered_map lookups
		TagQueue* queue;
		uint32_t* numReleased;
		// Sequence number of the first queued request
		int64_t nextSeqNo;
		bool operator<(TagInfo const& rhs) const { return nextSeqNo < rhs.nextSeqNo; }
		explicit TagInfo(TagQueue& queue, uint32_t& numReleased) : queue(&queue), numReleased(&numReleased) {
			ASSERT(!this->queue->requests.empty());
			nextSeqNo = this->queue->requests.front().sequenceNumber;
		}
	};

	// Track transactions released for each tag
	std::vector<std::pair<TransactionTag, uint32_t>> transactionsReleased;
	transactionsReleased.reserve(queues.size());

	std::priority_queue<TagInfo> pq;
	for (auto& [tag, queue] : queues) {
		if (queue.rateInfo.present()) {
			queue.rateInfo.get().startReleaseWindow();
		}
		if (!queue.requests.empty()) {
			auto& [_, count] = transactionsReleased.emplace_back(tag, 0);
			pq.emplace(queue, count);
		}
	}

	while (!pq.empty()) {
		auto info = pq.top();
		pq.pop();
		// Used to determine when it is time to start processing another tag
		auto const nextQueueSeqNo = pq.empty() ? std::numeric_limits<int64_t>::max() : pq.top().nextSeqNo;

		while (!info.queue->requests.empty()) {
			auto& delayedReq = info.queue->requests.front();
			auto count = delayedReq.req.tags.begin()->second;
			ASSERT_EQ(info.nextSeqNo, delayedReq.sequenceNumber);
			if (info.queue->rateInfo.present() && !info.queue->rateInfo.get().canStart(*(info.numReleased), count)) {
				// Cannot release any more transaction from this tag (don't push the info back into pq)
				CODE_PROBE(true, "GrvProxyTransactionTagThrottler::releaseTransactions : Throttling transaction");
				break;
			} else {
				if (info.nextSeqNo < nextQueueSeqNo) {
					// Releasing transaction
					*(info.numReleased) += count;
					delayedReq.updateProxyTagThrottledDuration();
					if (delayedReq.req.priority == TransactionPriority::BATCH) {
						outBatchPriority.push_back(delayedReq.req);
					} else if (delayedReq.req.priority == TransactionPriority::DEFAULT) {
						outDefaultPriority.push_back(delayedReq.req);
					} else {
						// Immediate priority transactions should bypass the GrvProxyTransactionTagThrottler
						ASSERT(false);
					}
					info.queue->requests.pop_front();
					if (!info.queue->requests.empty()) {
						info.nextSeqNo = info.queue->requests.front().sequenceNumber;
					}
				} else {
					CODE_PROBE(
					    true, "GrvProxyTransactionTagThrottler::releaseTransactions : Switching tags to preserve FIFO");
					pq.push(info);
					break;
				}
			}
		}
	}

	// End release windows for queues with valid rateInfo
	{
		TransactionTagMap<uint32_t> transactionsReleasedMap;
		for (const auto& [tag, count] : transactionsReleased) {
			transactionsReleasedMap[tag] = count;
		}
		for (auto& [tag, queue] : queues) {
			if (queue.rateInfo.present()) {
				queue.rateInfo.get().endReleaseWindow(transactionsReleasedMap[tag], false, elapsed);
			}
		}
	}
}

uint32_t GrvProxyTransactionTagThrottler::size() {
	return queues.size();
}

ACTOR static Future<Void> mockClient(GrvProxyTransactionTagThrottler* throttler,
                                     TransactionPriority priority,
                                     TagSet tagSet,
                                     int batchSize,
                                     double desiredRate,
                                     TransactionTagMap<uint32_t>* counters) {
	state Future<Void> timer;
	state TransactionTagMap<uint32_t> tags;
	for (const auto& tag : tagSet) {
		tags[tag] = batchSize;
	}
	loop {
		timer = delayJittered(static_cast<double>(batchSize) / desiredRate);
		GetReadVersionRequest req;
		req.tags = tags;
		req.priority = priority;
		throttler->addRequest(req);
		wait(success(req.reply.getFuture()) && timer);
		for (auto& [tag, _] : tags) {
			(*counters)[tag] += batchSize;
		}
	}
}

ACTOR static Future<Void> mockServer(GrvProxyTransactionTagThrottler* throttler) {
	state SpannedDeque<GetReadVersionRequest> outBatchPriority("TestGrvProxyTransactionTagThrottler_Batch"_loc);
	state SpannedDeque<GetReadVersionRequest> outDefaultPriority("TestGrvProxyTransactionTagThrottler_Default"_loc);
	loop {
		state double elapsed = (0.009 + 0.002 * deterministicRandom()->random01());
		wait(delay(elapsed));
		throttler->releaseTransactions(elapsed, outBatchPriority, outDefaultPriority);
		while (!outBatchPriority.empty()) {
			outBatchPriority.front().reply.send(GetReadVersionReply{});
			outBatchPriority.pop_front();
		}
		while (!outDefaultPriority.empty()) {
			outDefaultPriority.front().reply.send(GetReadVersionReply{});
			outDefaultPriority.pop_front();
		}
	}
}

static TransactionTag getRandomTag() {
	TransactionTag result;
	auto arr = new (result.arena()) uint8_t[32];
	for (int i = 0; i < 32; ++i) {
		arr[i] = (uint8_t)deterministicRandom()->randomInt(0, 256);
	}
	result.contents() = TransactionTagRef(arr, 32);
	return result;
}

static bool isNear(double desired, int64_t actual) {
	return std::abs(desired - actual) * 10 < desired;
}

// Rate limit set at 10, but client attempts 20 transactions per second.
// Client should be throttled to only 10 transactions per second.
TEST_CASE("/GrvProxyTransactionTagThrottler/Simple") {
	state GrvProxyTransactionTagThrottler throttler;
	state TagSet tagSet;
	state TransactionTagMap<uint32_t> counters;
	{
		TransactionTagMap<double> rates;
		rates["sampleTag"_sr] = 10.0;
		throttler.updateRates(rates);
	}
	tagSet.addTag("sampleTag"_sr);

	state Future<Void> client = mockClient(&throttler, TransactionPriority::DEFAULT, tagSet, 1, 20.0, &counters);
	state Future<Void> server = mockServer(&throttler);
	wait(timeout(client && server, 60.0, Void()));
	TraceEvent("TagQuotaTest_Simple").detail("Counter", counters["sampleTag"_sr]);
	ASSERT(isNear(counters["sampleTag"_sr], 60.0 * 10.0));
	return Void();
}

// Clients share the available 30 transaction/second budget
TEST_CASE("/GrvProxyTransactionTagThrottler/MultiClient") {
	state GrvProxyTransactionTagThrottler throttler;
	state TagSet tagSet;
	state TransactionTagMap<uint32_t> counters;
	{
		TransactionTagMap<double> rates;
		rates["sampleTag"_sr] = 30.0;
		throttler.updateRates(rates);
	}
	tagSet.addTag("sampleTag"_sr);

	state std::vector<Future<Void>> clients;
	clients.reserve(10);
	for (int i = 0; i < 10; ++i) {
		clients.push_back(mockClient(&throttler, TransactionPriority::DEFAULT, tagSet, 1, 10.0, &counters));
	}

	state Future<Void> server = mockServer(&throttler);
	wait(timeout(waitForAll(clients) && server, 60.0, Void()));
	TraceEvent("TagQuotaTest_MultiClient").detail("Counter", counters["sampleTag"_sr]);
	ASSERT(isNear(counters["sampleTag"_sr], 60.0 * 30.0));
	return Void();
}

// Test processing GetReadVersionRequests that batch several transactions
TEST_CASE("/GrvProxyTransactionTagThrottler/Batch") {
	state GrvProxyTransactionTagThrottler throttler;
	state TagSet tagSet;
	state TransactionTagMap<uint32_t> counters;
	{
		TransactionTagMap<double> rates;
		rates["sampleTag"_sr] = 10.0;
		throttler.updateRates(rates);
	}
	tagSet.addTag("sampleTag"_sr);

	state Future<Void> client = mockClient(&throttler, TransactionPriority::DEFAULT, tagSet, 5, 20.0, &counters);
	state Future<Void> server = mockServer(&throttler);
	wait(timeout(client && server, 60.0, Void()));

	TraceEvent("TagQuotaTest_Batch").detail("Counter", counters["sampleTag"_sr]);
	ASSERT(isNear(counters["sampleTag"_sr], 60.0 * 10.0));
	return Void();
}

// Tests cleanup of tags that are no longer throttled.
TEST_CASE("/GrvProxyTransactionTagThrottler/Cleanup1") {
	GrvProxyTransactionTagThrottler throttler;
	for (int i = 0; i < 1000; ++i) {
		auto const tag = getRandomTag();
		TransactionTagMap<double> rates;
		rates[tag] = 10.0;
		throttler.updateRates(rates);
		ASSERT_EQ(throttler.size(), 1);
	}
	return Void();
}

// Tests cleanup of tags once queues have been emptied
TEST_CASE("/GrvProxyTransactionTagThrottler/Cleanup2") {
	GrvProxyTransactionTagThrottler throttler;
	{
		GetReadVersionRequest req;
		req.tags["sampleTag"_sr] = 1;
		req.priority = TransactionPriority::DEFAULT;
		throttler.addRequest(req);
	}
	ASSERT_EQ(throttler.size(), 1);
	throttler.updateRates(TransactionTagMap<double>{});
	ASSERT_EQ(throttler.size(), 1);
	{
		SpannedDeque<GetReadVersionRequest> outBatchPriority("TestGrvProxyTransactionTagThrottler_Batch"_loc);
		SpannedDeque<GetReadVersionRequest> outDefaultPriority("TestGrvProxyTransactionTagThrottler_Default"_loc);
		throttler.releaseTransactions(0.1, outBatchPriority, outDefaultPriority);
	}
	// Calling updates cleans up the queues in throttler
	throttler.updateRates(TransactionTagMap<double>{});
	ASSERT_EQ(throttler.size(), 0);
	return Void();
}
