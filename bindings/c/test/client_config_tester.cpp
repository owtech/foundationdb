/*
 * client_config_tester.cpp
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

/*
 * A utility for testing FDB client with different configuration options
 *
 * It performs following steps:
 * 1. Initialize FDB client with the specified options
 * 2. Create a database
 * 3. Perform a simple transaction
 * 4. Check if these steps succeed of fail with the expected error
 * 5. Print client status if requested
 */

#include "fmt/core.h"
#include "test/fdb_api.hpp"
#include "SimpleOpt/SimpleOpt.h"
#include <thread>
#include <string_view>
#include <unordered_map>
#include "fdbclient/FDBOptions.g.h"

#if (defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__))
#include <unistd.h>
#elif defined(_WIN32)
#include <process.h>
#else
#error Unsupported platform
#endif

using namespace std::string_view_literals;

namespace {

enum TesterOptionId {
	OPT_HELP,
	OPT_CONNFILE,
	OPT_EXTERNAL_CLIENT_LIBRARY,
	OPT_EXTERNAL_CLIENT_DIRECTORY,
	OPT_API_VERSION,
	OPT_TRANSACTION_TIMEOUT,
	OPT_TRACE,
	OPT_TRACE_DIR,
	OPT_TMP_DIR,
	OPT_EXPECTED_ERROR,
	OPT_PRINT_STATUS,
	OPT_NETWORK_OPTION
};

const int MIN_TESTABLE_API_VERSION = 400;

CSimpleOpt::SOption TesterOptionDefs[] = //
    { { OPT_HELP, "-h", SO_NONE },
	  { OPT_HELP, "--help", SO_NONE },
	  { OPT_CONNFILE, "-C", SO_REQ_SEP },
	  { OPT_CONNFILE, "--cluster-file", SO_REQ_SEP },
	  { OPT_EXTERNAL_CLIENT_LIBRARY, "--external-client-library", SO_REQ_SEP },
	  { OPT_EXTERNAL_CLIENT_DIRECTORY, "--external-client-dir", SO_REQ_SEP },
	  { OPT_API_VERSION, "--api-version", SO_REQ_SEP },
	  { OPT_TRANSACTION_TIMEOUT, "--transaction-timeout", SO_REQ_SEP },
	  { OPT_TRACE, "--log", SO_NONE },
	  { OPT_TRACE_DIR, "--log-dir", SO_REQ_SEP },
	  { OPT_TMP_DIR, "--tmp-dir", SO_REQ_SEP },
	  { OPT_EXPECTED_ERROR, "--expected-error", SO_REQ_SEP },
	  { OPT_PRINT_STATUS, "--print-status", SO_NONE },
	  { OPT_NETWORK_OPTION, "--network-option-", SO_REQ_SEP },
	  SO_END_OF_OPTIONS };

class TesterOptions {
public:
	// FDB API version, using the latest version by default
	int apiVersion = FDB_API_VERSION;
	std::string clusterFile;
	std::string externalClientLibrary;
	std::string externalClientDir;
	bool disableLocalClient = false;
	bool disableClientBypass = false;
	int transactionTimeout = 0;
	bool trace = false;
	std::string traceDir;
	std::string tmpDir;
	bool ignoreExternalClientFailures = false;
	bool failIncompatibleClient = false;
	fdb::Error::CodeType expectedError = 0;
	bool printStatus = false;
	std::vector<std::pair<std::string, std::string>> networkOptions;
};

namespace {
TesterOptions options;
}

void printProgramUsage(const char* execName) {
	printf("usage: %s [OPTIONS]\n"
	       "\n",
	       execName);
	printf("  -C, --cluster-file FILE\n"
	       "                 The path of a file containing the connection string for the\n"
	       "                 FoundationDB cluster. The default is `fdb.cluster'\n"
	       "  --local-client-library FILE\n"
	       "                 Path to the local client library.\n"
	       "  --external-client-library FILE\n"
	       "                 Path to the external client library.\n"
	       "  --external-client-dir DIR\n"
	       "                 Directory containing external client libraries.\n"
	       "  --api-version VERSION\n"
	       "                 Required FDB API version (default %d).\n"
	       "  --transaction-timeout MILLISECONDS\n"
	       "                 The timeout for the test transactions in milliseconds (default: 0 - no timeout)\n"
	       "  --log          Enables trace file logging for the CLI session.\n"
	       "  --log-dir PATH Specifes the output directory for trace files. If\n"
	       "                 unspecified, defaults to the current directory. Has\n"
	       "                 no effect unless --log is specified.\n"
	       "  --tmp-dir DIR\n"
	       "                 Directory for temporary files of the client.\n"
	       "  --expected-error ERR\n"
	       "                 FDB error code the test expected to fail with (default: 0).\n"
	       "  --print-status\n"
	       "                 Print database client status.\n"
	       "  --network-option-OPTIONNAME OPTIONVALUE\n"
	       "                 Changes a network option. OPTIONAME should be lowercase.\n"
	       "  -h, --help     Display this help and exit.\n",
	       FDB_API_VERSION);
}

bool processIntOption(const std::string& optionName, const std::string& value, int minValue, int maxValue, int& res) {
	char* endptr;
	res = strtol(value.c_str(), &endptr, 10);
	if (*endptr != '\0') {
		fmt::print(stderr, "Invalid value {} for {}", value, optionName);
		return false;
	}
	if (res < minValue || res > maxValue) {
		fmt::print(
		    stderr, "Value for {} must be between {} and {}. Input value {}", optionName, minValue, maxValue, res);
		return false;
	}
	return true;
}

// Extracts the key for command line arguments that are specified with a prefix (e.g. --knob-).
// This function converts any hyphens in the extracted key to underscores.
bool extractPrefixedArgument(std::string prefix, const std::string& arg, std::string& res) {
	if (arg.size() <= prefix.size() || arg.find(prefix) != 0 ||
	    (arg[prefix.size()] != '-' && arg[prefix.size()] != '_')) {
		return false;
	}

	res = arg.substr(prefix.size() + 1);
	std::transform(res.begin(), res.end(), res.begin(), [](int c) { return c == '-' ? '_' : c; });
	return true;
}

bool processArg(const CSimpleOpt& args) {
	switch (args.OptionId()) {
	case OPT_CONNFILE:
		options.clusterFile = args.OptionArg();
		break;
	case OPT_EXTERNAL_CLIENT_LIBRARY:
		options.externalClientLibrary = args.OptionArg();
		break;
	case OPT_EXTERNAL_CLIENT_DIRECTORY:
		options.externalClientDir = args.OptionArg();
		break;
	case OPT_API_VERSION:
		if (!processIntOption(
		        args.OptionText(), args.OptionArg(), MIN_TESTABLE_API_VERSION, FDB_API_VERSION, options.apiVersion)) {
			return false;
		}
		break;
	case OPT_TRANSACTION_TIMEOUT:
		if (!processIntOption(args.OptionText(), args.OptionArg(), 0, 1000000, options.transactionTimeout)) {
			return false;
		}
		break;
	case OPT_TRACE:
		options.trace = true;
		break;
	case OPT_TRACE_DIR:
		options.traceDir = args.OptionArg();
		break;
	case OPT_TMP_DIR:
		options.tmpDir = args.OptionArg();
		break;
	case OPT_EXPECTED_ERROR:
		if (!processIntOption(args.OptionText(), args.OptionArg(), 0, 10000, options.expectedError)) {
			return false;
		}
		break;
	case OPT_PRINT_STATUS:
		options.printStatus = true;
		break;

	case OPT_NETWORK_OPTION: {
		std::string optionName;
		if (!extractPrefixedArgument("--network-option", args.OptionSyntax(), optionName)) {
			fmt::print(stderr, "ERROR: unable to parse network option '{}'\n", args.OptionSyntax());
			return false;
		}
		options.networkOptions.emplace_back(optionName, args.OptionArg());
		break;
	}
	}
	return true;
}

bool parseArgs(int argc, char** argv) {
	// declare our options parser, pass in the arguments from main
	// as well as our array of valid options.
	CSimpleOpt args(argc, argv, TesterOptionDefs);

	// while there are arguments left to process
	while (args.Next()) {
		if (args.LastError() == SO_SUCCESS) {
			if (args.OptionId() == OPT_HELP) {
				printProgramUsage(argv[0]);
				return false;
			}
			if (!processArg(args)) {
				return false;
			}
		} else {
			fmt::print(stderr, "ERROR: Invalid argument: {}\n", args.OptionText());
			printProgramUsage(argv[0]);
			return false;
		}
	}
	return true;
}

void exitImmediately(int exitCode) {
#ifdef _WIN32
	TerminateProcess(GetCurrentProcess(), exitCode);
#else
	_exit(exitCode);
#endif
}

void checkErrorCodeAndExit(fdb::Error::CodeType e) {
	if (e == options.expectedError) {
		exitImmediately(0);
	}
	fmt::print(stderr, "Expected Error: {}, but got {}\n", options.expectedError, e);
	exitImmediately(1);
}

void fdb_check(fdb::Error e, std::string_view msg) {
	if (e.code()) {
		fmt::print(stderr, "{}, Error: {}({})\n", msg, e.code(), e.what());
		checkErrorCodeAndExit(e.code());
	}
}

std::string stringToUpper(const std::string& str) {
	std::string outStr(str);
	std::transform(outStr.begin(), outStr.end(), outStr.begin(), [](char c) { return std::toupper(c); });
	return outStr;
}

void applyNetworkOptions() {
	if (!options.tmpDir.empty() && options.apiVersion >= FDB_API_VERSION_CLIENT_TMP_DIR) {
		fdb::network::setOption(FDBNetworkOption::FDB_NET_OPTION_CLIENT_TMP_DIR, options.tmpDir);
	}
	if (!options.externalClientLibrary.empty()) {
		fdb::network::setOption(FDBNetworkOption::FDB_NET_OPTION_EXTERNAL_CLIENT_LIBRARY,
		                        options.externalClientLibrary);
	}
	if (!options.externalClientDir.empty()) {
		fdb::network::setOption(FDBNetworkOption::FDB_NET_OPTION_EXTERNAL_CLIENT_DIRECTORY, options.externalClientDir);
	}
	if (options.trace) {
		fdb::network::setOption(FDBNetworkOption::FDB_NET_OPTION_TRACE_ENABLE, options.traceDir);
	}

	std::unordered_map<std::string, FDBNetworkOption> networkOptionsByName;
	for (auto const& [optionCode, optionInfo] : FDBNetworkOptions::optionInfo) {
		networkOptionsByName[optionInfo.name] = static_cast<FDBNetworkOption>(optionCode);
	}

	for (auto const& [optionName, optionVal] : options.networkOptions) {
		auto iter = networkOptionsByName.find(stringToUpper(optionName));
		if (iter == networkOptionsByName.end()) {
			fmt::print(stderr, "Unknown network option {}\n", optionName);
		}
		fdb::network::setOption(iter->second, optionVal);
	}
}

void printDatabaseStatus(fdb::Database db) {
	if (options.printStatus) {
		auto statusFuture = db.getClientStatus();
		fdb_check(statusFuture.blockUntilReady(), "Wait on getClientStatus failed");
		fmt::print("{}\n", fdb::toCharsRef(statusFuture.get()));
		fflush(stdout);
	}
}

void testTransaction() {
	fdb::Database db(options.clusterFile);
	fdb::Transaction tx = db.createTransaction();
	while (true) {
		// Set a time out to avoid long delays when testing invalid configurations
		try {
			tx.setOption(FDB_TR_OPTION_TIMEOUT, options.transactionTimeout);
			auto getFuture = tx.get(fdb::toBytesRef("key2"sv), true);
			fdb_check(getFuture.blockUntilReady(), "Wait on get failed");
			if (getFuture.error()) {
				throw getFuture.error();
			}
			tx.set(fdb::toBytesRef("key1"sv), fdb::toBytesRef("val1"sv));
			auto commitFuture = tx.commit();
			fdb_check(commitFuture.blockUntilReady(), "Wait on commit failed");
			if (commitFuture.error()) {
				throw commitFuture.error();
			}
			break;
		} catch (fdb::Error err) {
			if (err.code() == error_code_timed_out) {
				fmt::print(stderr, "Transaction timed out\n");
				printDatabaseStatus(db);
				exitImmediately(1);
			}
			auto onErrorFuture = tx.onError(err);
			fdb_check(onErrorFuture.blockUntilReady(), "Wait on onError failed");
			auto onErrResult = onErrorFuture.error();
			if (onErrResult) {
				fmt::print(stderr,
				           "Transaction failed with a non-retriable error: {}({})\n",
				           onErrResult.code(),
				           onErrResult.what());
				printDatabaseStatus(db);
				checkErrorCodeAndExit(onErrResult.code());
			}
		}
	}
	printDatabaseStatus(db);
}

} // namespace

int main(int argc, char** argv) {
	int retCode = 0;
	try {
		if (!parseArgs(argc, argv)) {
			return 1;
		}

		fdb::selectApiVersion(options.apiVersion);
		applyNetworkOptions();
		fdb_check(fdb::network::setupNothrow(), "Setup network failed");

		std::thread network_thread{ [] { fdb_check(fdb::network::run(), "FDB network thread failed"); } };

		// Try creating a database and executing a transaction
		testTransaction();

		fdb_check(fdb::network::stop(), "Stop network failed");
		network_thread.join();
	} catch (const fdb::Error& err) {
		fmt::print(stderr, "FDB Error: {}\n", err.what());
		retCode = err.code();
	} catch (const std::runtime_error& err) {
		fmt::print(stderr, "runtime error caught: {}\n", err.what());
		retCode = 1;
	}
	checkErrorCodeAndExit(retCode);
}
