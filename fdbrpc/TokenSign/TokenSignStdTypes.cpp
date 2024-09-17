/*
 * TokenSignStlTypes.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2024 Apple Inc. and the FoundationDB project authors
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

#include "fdbrpc/TokenSign.h"
#include "fdbrpc/TokenSignStdTypes.h"
#include "flow/PKey.h"
#include "flow/MkCert.h"
#include <stdexcept>

namespace {

// converts std::optional<STANDARD_TYPE(S)> to Optional<FLOW_TYPE(T)>
template <class T, class S>
void convertAndAssign(Arena& arena, Optional<T>& to, const std::optional<S>& from) {
	if (!from.has_value()) {
		to.reset();
		return;
	}
	if constexpr (std::is_same_v<S, std::vector<std::string>>) {
		static_assert(std::is_same_v<T, VectorRef<StringRef>>,
		              "Source type std::vector<std::string> must convert to VectorRef<StringRef>");
		const auto& value = from.value();
		if (value.empty()) {
			to = VectorRef<StringRef>();
		} else {
			// no need to deep copy string because we have the underlying memory for the duration of token signing.
			auto buf = new (arena) StringRef[value.size()];
			for (auto i = 0u; i < value.size(); i++) {
				buf[i] = StringRef(value[i]);
			}
			to = VectorRef<StringRef>(buf, value.size());
		}
	} else if constexpr (std::is_same_v<S, std::vector<int64_t>>) {
		static_assert(std::is_same_v<T, VectorRef<int64_t>>,
		              "Source type std::vector<int64_t> must convert to VectorRef<int64_t>");
		const auto& value = from.value();
		if (value.empty()) {
			to = VectorRef<int64_t>();
		} else {
			auto buf = new (arena) int64_t[value.size()];
			for (auto i = 0; i < value.size(); i++)
				buf[i] = value[i];
			to = VectorRef<int64_t>(buf, value.size());
		}
	} else if constexpr (std::is_same_v<S, std::string>) {
		static_assert(std::is_same_v<T, StringRef>, "Source type std::string must convert to StringRef");
		const auto& value = from.value();
		// no need to deep copy string because we have the underlying memory for the duration of token signing.
		to = StringRef(value);
	} else {
		static_assert(std::is_same_v<S, T>,
		              "Source types that aren't std::vector<std::string>, std::vector<int64_t>, or std::string must "
		              "have the same destination type");
		static_assert(std::is_trivially_copy_assignable_v<S>,
		              "Source types that aren't std::vector<std::string> or std::string must not use heap memory");
		to = from.value();
	}
}

} // anonymous namespace

namespace authz::jwt::stdtypes {

std::string makeEcP256PrivateKeyPem() {
	try {
		PrivateKey pk = mkcert::makeEcP256();
		auto tmpArena = Arena();
		auto pem = pk.writePem(tmpArena);
		return pem.toString();
	} catch (Error& e) {
		throw std::runtime_error(e.name());
	}
}

std::string signToken(const TokenSpec& tokenSpec, const std::string& privateKeyPem) {
	try {
		auto arena = Arena();
		auto privateKey = PrivateKey(PemEncoded{}, StringRef(privateKeyPem));
		// translate TokenSpec (uses STL types) to TokenRef (uses Flow types)
		auto t = authz::jwt::TokenRef{};
		t.algorithm = tokenSpec.algorithm;
		t.keyId = StringRef(tokenSpec.keyId);
		convertAndAssign(arena, t.issuer, tokenSpec.issuer);
		convertAndAssign(arena, t.subject, tokenSpec.subject);
		convertAndAssign(arena, t.audience, tokenSpec.audience);
		convertAndAssign(arena, t.issuedAtUnixTime, tokenSpec.issuedAtUnixTime);
		convertAndAssign(arena, t.expiresAtUnixTime, tokenSpec.expiresAtUnixTime);
		convertAndAssign(arena, t.notBeforeUnixTime, tokenSpec.notBeforeUnixTime);
		convertAndAssign(arena, t.tokenId, tokenSpec.tokenId);
		convertAndAssign(arena, t.tenants, tokenSpec.tenants);
		return signToken(arena, t, privateKey).toString();
	} catch (Error& e) {
		if (e.code() == error_code_pkey_decode_error) {
			// bad PEM
			throw std::invalid_argument(e.name());
		} else {
			throw std::runtime_error(e.name());
		}
	}
}

} // namespace authz::jwt::stdtypes
