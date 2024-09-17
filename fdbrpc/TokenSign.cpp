/*
 * TokenSign.cpp
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
#include "flow/network.h"
#include "flow/serialize.h"
#include "flow/Arena.h"
#include "flow/AutoCPointer.h"
#include "flow/Error.h"
#include "flow/IRandom.h"
#include "flow/MkCert.h"
#include "flow/Platform.h"
#include "flow/ScopeExit.h"
#include "flow/Trace.h"
#include "flow/UnitTest.h"
#include <fmt/format.h>
#include <cmath>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <utility>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/error/en.h>
#include "fdbrpc/Base64Encode.h"
#include "fdbrpc/Base64Decode.h"

namespace {

// test-only constants for generating random tenant ID and key names
constexpr int MinIssuerNameLen = 16;
constexpr int MaxIssuerNameLenPlus1 = 25;
constexpr authz::TenantId MinTenantId = 1;
constexpr authz::TenantId MaxTenantIdPlus1 = 0xffffffffll;
constexpr int MinKeyNameLen = 10;
constexpr int MaxKeyNameLenPlus1 = 21;

StringRef genRandomAlphanumStringRef(Arena& arena, IRandom& rng, int minLen, int maxLenPlusOne) {
	const auto len = rng.randomInt(minLen, maxLenPlusOne);
	auto strRaw = new (arena) uint8_t[len];
	for (auto i = 0; i < len; i++)
		strRaw[i] = (uint8_t)rng.randomAlphaNumeric();
	return StringRef(strRaw, len);
}

Optional<StringRef> checkVerifyAlgorithm(PKeyAlgorithm algo, PublicKey key) {
	if (algo != key.algorithm()) {
		return "Token algorithm does not match key's"_sr;
	} else {
		return {};
	}
}

bool checkSignAlgorithm(PKeyAlgorithm algo, PrivateKey key) {
	if (algo != key.algorithm()) {
		TraceEvent(SevWarnAlways, "TokenSignAlgoMismatch")
		    .suppressFor(10)
		    .detail("Expected", pkeyAlgorithmName(algo))
		    .detail("PublicKeyAlgorithm", key.algorithmName());
		return false;
	} else {
		return true;
	}
}

Optional<StringRef> convertEs256P1363ToDer(Arena& arena, StringRef p1363) {
	const int SIGLEN = p1363.size();
	const int HALF_SIGLEN = SIGLEN / 2;
	auto r = AutoCPointer(BN_bin2bn(p1363.begin(), HALF_SIGLEN, nullptr), &::BN_free);
	auto s = AutoCPointer(BN_bin2bn(p1363.begin() + HALF_SIGLEN, HALF_SIGLEN, nullptr), &::BN_free);
	if (!r || !s)
		return {};
	auto sig = AutoCPointer(::ECDSA_SIG_new(), &ECDSA_SIG_free);
	if (!sig)
		return {};
	::ECDSA_SIG_set0(sig, r.release(), s.release());
	auto const derLen = ::i2d_ECDSA_SIG(sig, nullptr);
	if (derLen < 0)
		return {};
	auto buf = new (arena) uint8_t[derLen];
	auto bufPtr = buf;
	::i2d_ECDSA_SIG(sig, &bufPtr);
	return StringRef(buf, derLen);
}

Optional<StringRef> convertEs256DerToP1363(Arena& arena, StringRef der) {
	uint8_t const* derPtr = der.begin();
	auto sig = AutoCPointer(::d2i_ECDSA_SIG(nullptr, &derPtr, der.size()), &::ECDSA_SIG_free);
	if (!sig) {
		return {};
	}
	// ES256-specific constant. Adapt as needed
	constexpr const int SIGLEN = 64;
	constexpr const int HALF_SIGLEN = SIGLEN / 2;
	auto buf = new (arena) uint8_t[SIGLEN];
	::memset(buf, 0, SIGLEN);
	auto bufr = buf;
	auto bufs = bufr + HALF_SIGLEN;
	auto r = std::add_pointer_t<BIGNUM const>();
	auto s = std::add_pointer_t<BIGNUM const>();
	ECDSA_SIG_get0(sig, &r, &s);
	auto const lenr = BN_num_bytes(r);
	auto const lens = BN_num_bytes(s);
	if (lenr > HALF_SIGLEN || lens > HALF_SIGLEN)
		return {};
	BN_bn2bin(r, bufr + (HALF_SIGLEN - lenr));
	BN_bn2bin(s, bufs + (HALF_SIGLEN - lens));
	return StringRef(buf, SIGLEN);
}

} // namespace

namespace authz {

using MessageDigestMethod = const EVP_MD*;

std::pair<PKeyAlgorithm, MessageDigestMethod> getMethod(Algorithm alg) {
	if (alg == Algorithm::RS256) {
		return { PKeyAlgorithm::RSA, ::EVP_sha256() };
	} else if (alg == Algorithm::ES256) {
		return { PKeyAlgorithm::EC, ::EVP_sha256() };
	} else {
		return { PKeyAlgorithm::UNSUPPORTED, nullptr };
	}
}

std::string_view getAlgorithmName(Algorithm alg) {
	if (alg == Algorithm::RS256)
		return { "RS256" };
	else if (alg == Algorithm::ES256)
		return { "ES256" };
	else
		UNREACHABLE();
}

} // namespace authz

namespace authz::jwt {

template <class FieldType, size_t NameLen>
void appendField(fmt::memory_buffer& b, char const (&name)[NameLen], Optional<FieldType> const& field) {
	if (!field.present())
		return;
	auto const& f = field.get();
	auto bi = std::back_inserter(b);
	if constexpr (std::is_same_v<FieldType, VectorRef<StringRef>>) {
		fmt::format_to(bi, " {}=[", name);
		for (auto i = 0; i < f.size(); i++) {
			if (i)
				fmt::format_to(bi, ",");
			fmt::format_to(bi, fmt::runtime(f[i].toStringView()));
		}
		fmt::format_to(bi, "]");
	} else if constexpr (std::is_same_v<FieldType, VectorRef<TenantId>>) {
		fmt::format_to(bi, " {}=[", name);
		for (auto i = 0; i < f.size(); i++) {
			if (i)
				fmt::format_to(bi, ",");
			fmt::format_to(bi, "{:#x}", f[i]);
		}
		fmt::format_to(bi, "]");
	} else if constexpr (std::is_same_v<FieldType, StringRef>) {
		fmt::format_to(bi, " {}={}", name, f.toStringView());
	} else {
		fmt::format_to(bi, " {}={}", name, f);
	}
}

StringRef toStringRef(Arena& arena, const TokenRef& tokenSpec) {
	auto buf = fmt::memory_buffer();
	fmt::format_to(std::back_inserter(buf),
	               "alg={} kid={}",
	               getAlgorithmName(tokenSpec.algorithm),
	               tokenSpec.keyId.toStringView());
	appendField(buf, "iss", tokenSpec.issuer);
	appendField(buf, "sub", tokenSpec.subject);
	appendField(buf, "aud", tokenSpec.audience);
	appendField(buf, "iat", tokenSpec.issuedAtUnixTime);
	appendField(buf, "exp", tokenSpec.expiresAtUnixTime);
	appendField(buf, "nbf", tokenSpec.notBeforeUnixTime);
	appendField(buf, "jti", tokenSpec.tokenId);
	appendField(buf, "tenants", tokenSpec.tenants);
	auto str = new (arena) uint8_t[buf.size()];
	memcpy(str, buf.data(), buf.size());
	return StringRef(str, buf.size());
}

template <class FieldType, class Writer, bool AllowSingletonArrayAsString = false>
void putField(Optional<FieldType> const& field,
              Writer& wr,
              const char* fieldName,
              std::bool_constant<AllowSingletonArrayAsString> _ = std::bool_constant<false>{}) {
	if (!field.present())
		return;
	wr.Key(fieldName);
	auto const& value = field.get();
	static_assert(std::is_same_v<StringRef, FieldType> || std::is_same_v<FieldType, uint64_t> ||
	              std::is_same_v<FieldType, VectorRef<StringRef>> || std::is_same_v<FieldType, VectorRef<TenantId>>);
	if constexpr (std::is_same_v<StringRef, FieldType>) {
		wr.String(reinterpret_cast<const char*>(value.begin()), value.size());
	} else if constexpr (std::is_same_v<FieldType, uint64_t>) {
		wr.Uint64(value);
	} else if constexpr (std::is_same_v<FieldType, VectorRef<TenantId>>) {
		// "tenants" array = array of base64-encoded tenant key prefix
		// key prefix = bytestring representation of big-endian tenant ID (int64_t)
		Arena arena;
		wr.StartArray();
		for (auto elem : value) {
			auto const bigEndianId = bigEndian64(elem);
			auto encodedElem =
			    base64::encode(arena, StringRef(reinterpret_cast<const uint8_t*>(&bigEndianId), sizeof(bigEndianId)));
			wr.String(reinterpret_cast<const char*>(encodedElem.begin()), encodedElem.size());
		}
		wr.EndArray();
	} else {
		// VectorRef<StringRef> case
		if constexpr (AllowSingletonArrayAsString) {
			if (value.size() == 1 && deterministicRandom()->coinflip()) {
				// randomly make the field string, not array, to test parsing behavior
				wr.String(reinterpret_cast<const char*>(value[0].begin()), value[0].size());
				return;
			}
		}
		wr.StartArray();
		for (auto elem : value) {
			wr.String(reinterpret_cast<const char*>(elem.begin()), elem.size());
		}
		wr.EndArray();
	}
}

StringRef makeSignInput(Arena& arena, const TokenRef& tokenSpec) {
	using Buffer = rapidjson::StringBuffer;
	using Writer = rapidjson::Writer<Buffer>;
	auto headerBuffer = Buffer();
	auto payloadBuffer = Buffer();
	auto header = Writer(headerBuffer);
	auto payload = Writer(payloadBuffer);
	header.StartObject();
	header.Key("typ");
	header.String("JWT");
	auto algo = getAlgorithmName(tokenSpec.algorithm);
	header.Key("alg");
	header.String(algo.data(), algo.size());
	auto kid = tokenSpec.keyId.toStringView();
	header.Key("kid");
	header.String(kid.data(), kid.size());
	header.EndObject();
	payload.StartObject();
	putField(tokenSpec.issuer, payload, "iss");
	putField(tokenSpec.subject, payload, "sub");
	putField(tokenSpec.audience, payload, "aud", std::bool_constant<true>{});
	putField(tokenSpec.issuedAtUnixTime, payload, "iat");
	putField(tokenSpec.expiresAtUnixTime, payload, "exp");
	putField(tokenSpec.notBeforeUnixTime, payload, "nbf");
	putField(tokenSpec.tokenId, payload, "jti");
	putField(tokenSpec.tenants, payload, "tenants");
	payload.EndObject();
	auto const headerPartLen = base64::url::encodedLength(headerBuffer.GetSize());
	auto const payloadPartLen = base64::url::encodedLength(payloadBuffer.GetSize());
	auto const totalLen = headerPartLen + 1 + payloadPartLen;
	auto out = new (arena) uint8_t[totalLen];
	auto cur = out;
	cur += base64::url::encode(reinterpret_cast<const uint8_t*>(headerBuffer.GetString()), headerBuffer.GetSize(), cur);
	ASSERT_EQ(cur - out, headerPartLen);
	*cur++ = '.';
	cur +=
	    base64::url::encode(reinterpret_cast<const uint8_t*>(payloadBuffer.GetString()), payloadBuffer.GetSize(), cur);
	ASSERT_EQ(cur - out, totalLen);
	return StringRef(out, totalLen);
}

StringRef signToken(Arena& arena, StringRef signInput, Algorithm algorithm, PrivateKey privateKey) {
	auto tmpArena = Arena();
	auto [signAlgo, digest] = getMethod(algorithm);
	if (!checkSignAlgorithm(signAlgo, privateKey)) {
		throw digital_signature_ops_error();
	}
	auto plainSig = privateKey.sign(tmpArena, signInput, *digest);
	if (algorithm == Algorithm::ES256) {
		// Need to convert ASN.1/DER signature to IEEE-P1363
		auto convertedSig = convertEs256DerToP1363(tmpArena, plainSig);
		if (!convertedSig.present()) {
			auto tmpArena = Arena();
			TraceEvent(SevWarn, "TokenSigConversionFailure").log();
			throw digital_signature_ops_error();
		}
		plainSig = convertedSig.get();
	}
	auto const sigPartLen = base64::url::encodedLength(plainSig.size());
	auto const totalLen = signInput.size() + 1 + sigPartLen;
	auto out = new (arena) uint8_t[totalLen];
	auto cur = out;
	::memcpy(cur, signInput.begin(), signInput.size());
	cur += signInput.size();
	*cur++ = '.';
	cur += base64::url::encode(plainSig.begin(), plainSig.size(), cur);
	ASSERT_EQ(cur - out, totalLen);
	return StringRef(out, totalLen);
}

StringRef signToken(Arena& arena, const TokenRef& tokenSpec, PrivateKey privateKey) {
	auto tmpArena = Arena();
	auto signInput = makeSignInput(tmpArena, tokenSpec);
	return signToken(arena, signInput, tokenSpec.algorithm, privateKey);
}

Optional<StringRef> parseHeaderPart(Arena& arena, TokenRef& token, StringRef b64urlHeader) {
	auto tmpArena = Arena();
	auto optHeader = base64::url::decode(tmpArena, b64urlHeader);
	if (!optHeader.present())
		return "Failed to decode base64 header"_sr;
	auto header = optHeader.get();
	auto d = rapidjson::Document();
	d.Parse(reinterpret_cast<const char*>(header.begin()), header.size());
	if (d.HasParseError()) {
		return "Failed to parse header as JSON"_sr;
	}
	if (!d.IsObject())
		return "Header is not a JSON object"_sr;
	auto typItr = d.FindMember("typ");
	if (typItr == d.MemberEnd() || !typItr->value.IsString())
		return "No 'typ' field"_sr;
	auto algItr = d.FindMember("alg");
	if (algItr == d.MemberEnd() || !algItr->value.IsString())
		return "No 'alg' field"_sr;
	auto kidItr = d.FindMember("kid");
	if (kidItr == d.MemberEnd() || !kidItr->value.IsString())
		return "No 'kid' field"_sr;
	auto const& typ = typItr->value;
	auto const& alg = algItr->value;
	auto const& kid = kidItr->value;
	auto typValue = StringRef(reinterpret_cast<const uint8_t*>(typ.GetString()), typ.GetStringLength());
	if (typValue != "JWT"_sr)
		return "'typ' is not 'JWT'"_sr;
	auto algValue = StringRef(reinterpret_cast<const uint8_t*>(alg.GetString()), alg.GetStringLength());
	auto algType = algorithmFromString(algValue.toStringView());
	if (algType == Algorithm::UNKNOWN)
		return "Unsupported algorithm"_sr;
	token.algorithm = algType;
	token.keyId = StringRef(arena, reinterpret_cast<const uint8_t*>(kid.GetString()), kid.GetStringLength());
	return {};
}

template <class FieldType, bool AllowStringAsSingletonArray = false>
Optional<StringRef> parseField(Arena& arena,
                               Optional<FieldType>& out,
                               const rapidjson::Document& d,
                               const char* fieldName,
                               std::bool_constant<AllowStringAsSingletonArray> _ = std::bool_constant<false>{}) {
	auto fieldItr = d.FindMember(fieldName);
	if (fieldItr == d.MemberEnd())
		return {};
	auto const& field = fieldItr->value;
	static_assert(std::is_same_v<StringRef, FieldType> || std::is_same_v<FieldType, uint64_t> ||
	              std::is_same_v<FieldType, VectorRef<StringRef>> || std::is_same_v<FieldType, VectorRef<TenantId>>);
	if constexpr (std::is_same_v<FieldType, StringRef>) {
		if (!field.IsString()) {
			return StringRef(arena, fmt::format("'{}' is not a string", fieldName));
		}
		out = StringRef(arena, reinterpret_cast<const uint8_t*>(field.GetString()), field.GetStringLength());
	} else if constexpr (std::is_same_v<FieldType, uint64_t>) {
		if (!field.IsNumber()) {
			return StringRef(arena, fmt::format("'{}' is not a number", fieldName));
		}
		auto const number = field.GetDouble();
		if (number < 0) {
			return StringRef(arena, fmt::format("negative '{}' value is not allowed", fieldName));
		}
		out = static_cast<uint64_t>(number);
	} else if constexpr (std::is_same_v<FieldType, VectorRef<StringRef>>) {
		if constexpr (AllowStringAsSingletonArray) {
			if (field.IsString()) {
				auto vector = new (arena) StringRef[1];
				vector[0] =
				    StringRef(arena, reinterpret_cast<const uint8_t*>(field.GetString()), field.GetStringLength());
				out = VectorRef<StringRef>(vector, 1);
				CODE_PROBE(true, "Interpret authorization token's claim value string as a singleton array");
				return {};
			} else if (!field.IsArray()) {
				return StringRef(arena, fmt::format("'{}' is not an array or a string", fieldName));
			}
		} else if (!field.IsArray()) {
			return StringRef(arena, fmt::format("'{}' is not an array", fieldName));
		}
		if (field.Size() > 0) {
			auto vector = new (arena) StringRef[field.Size()];
			for (auto i = 0; i < field.Size(); i++) {
				if (!field[i].IsString()) {
					return StringRef(arena, fmt::format("{}th element of '{}' is not a string", i + 1, fieldName));
				}
				vector[i] = StringRef(
				    arena, reinterpret_cast<const uint8_t*>(field[i].GetString()), field[i].GetStringLength());
			}
			out = VectorRef<StringRef>(vector, field.Size());
		} else {
			out = VectorRef<StringRef>();
		}
	} else {
		// tenant ids case: convert array of base64-encoded length-8 bytestring containing big-endian int64_t to
		// local-endian int64_t
		if (!field.IsArray()) {
			return StringRef(arena, fmt::format("'{}' is not an array", fieldName));
		}
		if (field.Size() > 0) {
			auto vector = new (arena) TenantId[field.Size()];
			for (auto i = 0; i < field.Size(); i++) {
				if (!field[i].IsString()) {
					return StringRef(arena, fmt::format("{}th element of '{}' is not a string", i + 1, fieldName));
				}
				Optional<StringRef> decodedString = base64::decode(
				    arena,
				    StringRef(reinterpret_cast<const uint8_t*>(field[i].GetString()), field[i].GetStringLength()));
				if (decodedString.present()) {
					auto const tenantPrefix = decodedString.get();
					if (tenantPrefix.size() != sizeof(TenantId)) {
						CODE_PROBE(true, "Tenant prefix has an invalid length");
						return StringRef(arena,
						                 fmt::format("{}th element of '{}' has an invalid bytewise length of {}",
						                             i + 1,
						                             fieldName,
						                             tenantPrefix.size()));
					}
					TenantId tenantId = *reinterpret_cast<const TenantId*>(tenantPrefix.begin());
					vector[i] = fromBigEndian64(tenantId);
				} else {
					CODE_PROBE(true, "Tenant field has failed to be parsed");
					return StringRef(arena,
					                 fmt::format("Failed to base64-decode {}th element of '{}'", i + 1, fieldName));
				}
			}
			out = VectorRef<TenantId>(vector, field.Size());
		} else {
			out = VectorRef<TenantId>();
		}
	}
	return {};
}

Optional<StringRef> parsePayloadPart(Arena& arena, TokenRef& token, StringRef b64urlPayload) {
	auto tmpArena = Arena();
	auto optPayload = base64::url::decode(tmpArena, b64urlPayload);
	if (!optPayload.present())
		return "Failed to base64-decode payload part"_sr;
	auto payload = optPayload.get();
	auto d = rapidjson::Document();
	d.Parse(reinterpret_cast<const char*>(payload.begin()), payload.size());
	if (d.HasParseError()) {
		return "Token payload part is not valid JSON"_sr;
	}
	if (!d.IsObject())
		return "Token payload is not a JSON object"_sr;
	Optional<StringRef> err;
	if ((err = parseField(arena, token.issuer, d, "iss")).present())
		return err;
	if ((err = parseField(arena, token.subject, d, "sub")).present())
		return err;
	if ((err = parseField(arena, token.audience, d, "aud", std::bool_constant<true>{})).present())
		return err;
	if ((err = parseField(arena, token.tokenId, d, "jti")).present())
		return err;
	if ((err = parseField(arena, token.issuedAtUnixTime, d, "iat")).present())
		return err;
	if ((err = parseField(arena, token.expiresAtUnixTime, d, "exp")).present())
		return err;
	if ((err = parseField(arena, token.notBeforeUnixTime, d, "nbf")).present())
		return err;
	if ((err = parseField(arena, token.tenants, d, "tenants")).present())
		return err;
	return {};
}

Optional<StringRef> parseSignaturePart(Arena& arena, TokenRef& token, StringRef b64urlSignature) {
	auto optSig = base64::url::decode(arena, b64urlSignature);
	if (!optSig.present())
		return "Failed to base64url-decode signature part"_sr;
	token.signature = optSig.get();
	return {};
}

Optional<StringRef> parseToken(Arena& arena,
                               StringRef signedTokenIn,
                               TokenRef& parsedTokenOut,
                               StringRef& signInputOut) {
	signInputOut = StringRef();
	auto fullToken = signedTokenIn;
	auto b64urlHeader = signedTokenIn.eat("."_sr);
	auto b64urlPayload = signedTokenIn.eat("."_sr);
	auto b64urlSignature = signedTokenIn;
	if (b64urlHeader.empty() || b64urlPayload.empty() || b64urlSignature.empty())
		return "Token does not follow header.payload.signature structure"_sr;
	signInputOut = fullToken.substr(0, b64urlHeader.size() + 1 + b64urlPayload.size());
	auto err = Optional<StringRef>();
	if ((err = parseHeaderPart(arena, parsedTokenOut, b64urlHeader)).present())
		return err;
	if ((err = parsePayloadPart(arena, parsedTokenOut, b64urlPayload)).present())
		return err;
	if ((err = parseSignaturePart(arena, parsedTokenOut, b64urlSignature)).present())
		return err;
	return err;
}

std::pair<bool, Optional<StringRef>> verifyToken(StringRef signInput,
                                                 const TokenRef& parsedToken,
                                                 PublicKey publicKey) {
	Arena tmpArena;
	Optional<StringRef> err;
	auto [verifyAlgo, digest] = getMethod(parsedToken.algorithm);
	if ((err = checkVerifyAlgorithm(verifyAlgo, publicKey)).present())
		return { false, err };
	auto sig = parsedToken.signature;
	if (parsedToken.algorithm == Algorithm::ES256) {
		// Need to convert IEEE-P1363 signature to ASN.1/DER
		auto convertedSig = convertEs256P1363ToDer(tmpArena, sig);
		if (!convertedSig.present() || convertedSig.get().empty()) {
			err = "Failed to convert signature for verification"_sr;
			return { false, err };
		}
		sig = convertedSig.get();
	}
	return { publicKey.verify(signInput, sig, *digest), err };
}

std::pair<bool, Optional<StringRef>> verifyToken(StringRef signedToken, PublicKey publicKey) {
	auto arena = Arena();
	auto fullToken = signedToken;
	auto b64urlHeader = signedToken.eat("."_sr);
	auto b64urlPayload = signedToken.eat("."_sr);
	auto b64urlSignature = signedToken;
	auto err = Optional<StringRef>();
	if (b64urlHeader.empty() || b64urlPayload.empty() || b64urlSignature.empty()) {
		err = "Token does not follow header.payload.signature structure"_sr;
		return { false, err };
	}
	auto signInput = fullToken.substr(0, b64urlHeader.size() + 1 + b64urlPayload.size());
	auto parsedToken = TokenRef();
	if ((err = parseHeaderPart(arena, parsedToken, b64urlHeader)).present())
		return { false, err };
	auto optSig = base64::url::decode(arena, b64urlSignature);
	if (!optSig.present()) {
		err = "Failed to base64url-decode signature part"_sr;
		return { false, err };
	}
	parsedToken.signature = optSig.get();
	return verifyToken(signInput, parsedToken, publicKey);
}

TokenRef makeRandomTokenSpec(Arena& arena, IRandom& rng, Algorithm alg) {
	auto ret = TokenRef{};
	ret.algorithm = alg;
	ret.keyId = genRandomAlphanumStringRef(arena, rng, MinKeyNameLen, MaxKeyNameLenPlus1);
	ret.issuer = genRandomAlphanumStringRef(arena, rng, MinIssuerNameLen, MaxIssuerNameLenPlus1);
	ret.subject = genRandomAlphanumStringRef(arena, rng, MinIssuerNameLen, MaxIssuerNameLenPlus1);
	ret.tokenId = genRandomAlphanumStringRef(arena, rng, 16, 31);
	auto numAudience = rng.randomInt(1, 5);
	auto aud = new (arena) StringRef[numAudience];
	for (auto i = 0; i < numAudience; i++)
		aud[i] = genRandomAlphanumStringRef(arena, rng, MinIssuerNameLen, MaxIssuerNameLenPlus1);
	ret.audience = VectorRef<StringRef>(aud, numAudience);
	ret.issuedAtUnixTime = g_network->timer();
	ret.notBeforeUnixTime = ret.issuedAtUnixTime.get();
	ret.expiresAtUnixTime = ret.issuedAtUnixTime.get() + rng.randomInt(360, 1080 + 1);
	auto numTenants = rng.randomInt(1, 3);
	auto tenants = new (arena) TenantId[numTenants];
	for (auto i = 0; i < numTenants; i++)
		tenants[i] = rng.randomInt64(MinTenantId, MaxTenantIdPlus1);
	ret.tenants = VectorRef<TenantId>(tenants, numTenants);
	return ret;
}

} // namespace authz::jwt

void forceLinkTokenSignTests() {}

TEST_CASE("/fdbrpc/TokenSign/JWT") {
	const auto numIters = 100;
	for (auto i = 0; i < numIters; i++) {
		auto arena = Arena();
		auto privateKey = mkcert::makeEcP256();
		auto publicKey = privateKey.toPublic();
		auto& rng = *deterministicRandom();
		auto tokenSpec = authz::jwt::makeRandomTokenSpec(arena, rng, authz::Algorithm::ES256);
		auto signedToken = authz::jwt::signToken(arena, tokenSpec, privateKey);
		auto verifyOk = false;
		auto verifyErr = Optional<StringRef>();
		std::tie(verifyOk, verifyErr) = authz::jwt::verifyToken(signedToken, publicKey);
		ASSERT(!verifyErr.present());
		ASSERT(verifyOk);
		auto signaturePart = signedToken;
		signaturePart.eat("."_sr);
		signaturePart.eat("."_sr);
		{
			auto tmpArena = Arena();
			auto parsedToken = authz::jwt::TokenRef{};
			auto signInput = StringRef();
			auto parseError = parseToken(tmpArena, signedToken, parsedToken, signInput);
			ASSERT(!parseError.present());
			ASSERT_EQ(tokenSpec.algorithm, parsedToken.algorithm);
			ASSERT_EQ(tokenSpec.issuer, parsedToken.issuer);
			ASSERT_EQ(tokenSpec.subject, parsedToken.subject);
			ASSERT_EQ(tokenSpec.tokenId, parsedToken.tokenId);
			ASSERT_EQ(tokenSpec.audience, parsedToken.audience);
			ASSERT_EQ(tokenSpec.keyId, parsedToken.keyId);
			ASSERT_EQ(tokenSpec.issuedAtUnixTime.get(), parsedToken.issuedAtUnixTime.get());
			ASSERT_EQ(tokenSpec.expiresAtUnixTime.get(), parsedToken.expiresAtUnixTime.get());
			ASSERT_EQ(tokenSpec.notBeforeUnixTime.get(), parsedToken.notBeforeUnixTime.get());
			ASSERT_EQ(tokenSpec.tenants, parsedToken.tenants);
			auto optSig = base64::url::decode(tmpArena, signaturePart);
			ASSERT(optSig.present());
			ASSERT_EQ(optSig.get(), parsedToken.signature);
			std::tie(verifyOk, verifyErr) = authz::jwt::verifyToken(signInput, parsedToken, publicKey);
			ASSERT(!verifyErr.present());
			ASSERT(verifyOk);
		}
		// try tampering with signed token by adding one more tenant
		tokenSpec.tenants.get().push_back(arena, rng.randomInt64(MinTenantId, MaxTenantIdPlus1));
		auto tamperedTokenPart = makeSignInput(arena, tokenSpec);
		auto tamperedTokenString = fmt::format("{}.{}", tamperedTokenPart.toString(), signaturePart.toString());
		std::tie(verifyOk, verifyErr) = authz::jwt::verifyToken(StringRef(tamperedTokenString), publicKey);
		ASSERT(!verifyErr.present());
		ASSERT(!verifyOk);
	}
	printf("%d runs OK\n", numIters);
	return Void();
}

TEST_CASE("/fdbrpc/TokenSign/JWT/ToStringRef") {
	auto t = authz::jwt::TokenRef();
	t.algorithm = authz::Algorithm::ES256;
	t.issuer = "issuer"_sr;
	t.subject = "subject"_sr;
	StringRef aud[3]{ "aud1"_sr, "aud2"_sr, "aud3"_sr };
	t.audience = VectorRef<StringRef>(aud, 3);
	t.issuedAtUnixTime = 123ul;
	t.expiresAtUnixTime = 456ul;
	t.notBeforeUnixTime = 789ul;
	t.keyId = "keyId"_sr;
	t.tokenId = "tokenId"_sr;
	authz::TenantId tenants[2]{ 0x1ll, 0xabcdefabcdefll };
	t.tenants = VectorRef<authz::TenantId>(tenants, 2);
	auto arena = Arena();
	auto tokenStr = toStringRef(arena, t);
	auto tokenStrExpected =
	    "alg=ES256 kid=keyId iss=issuer sub=subject aud=[aud1,aud2,aud3] iat=123 exp=456 nbf=789 jti=tokenId tenants=[0x1,0xabcdefabcdef]"_sr;
	if (tokenStr != tokenStrExpected) {
		fmt::print("Expected: {}\nGot     : {}\n", tokenStrExpected.toStringView(), tokenStr.toStringView());
		ASSERT(false);
	} else {
		fmt::print("TEST OK\n");
	}
	return Void();
}

TEST_CASE("/fdbrpc/TokenSign/bench") {
	auto keyTypes = std::array<StringRef, 1>{ "EC"_sr };
	for (auto kty : keyTypes) {
		constexpr auto repeat = 5;
		constexpr auto numSamples = 10000;
		fmt::print("=== {} keys case\n", kty.toString());
		auto key = kty == "EC"_sr ? mkcert::makeEcP256() : mkcert::makeRsa4096Bit();
		auto pubKey = key.toPublic();
		auto& rng = *deterministicRandom();
		auto arena = Arena();
		auto jwtSpecs = new (arena) authz::jwt::TokenRef[numSamples];
		auto jwts = new (arena) StringRef[numSamples];
		for (auto i = 0; i < numSamples; i++) {
			jwtSpecs[i] = authz::jwt::makeRandomTokenSpec(
			    arena, rng, kty == "EC"_sr ? authz::Algorithm::ES256 : authz::Algorithm::RS256);
		}
		{
			auto const jwtSignBegin = timer_monotonic();
			for (auto i = 0; i < numSamples; i++) {
				jwts[i] = authz::jwt::signToken(arena, jwtSpecs[i], key);
			}
			auto const jwtSignEnd = timer_monotonic();
			fmt::print("JWT Sign   :         {:.2f} OPS\n", numSamples / (jwtSignEnd - jwtSignBegin));
		}
		{
			auto const jwtVerifyBegin = timer_monotonic();
			for (auto rep = 0; rep < repeat; rep++) {
				for (auto i = 0; i < numSamples; i++) {
					auto [verifyOk, errorMsg] = authz::jwt::verifyToken(jwts[i], pubKey);
					ASSERT(!errorMsg.present());
					ASSERT(verifyOk);
				}
			}
			auto const jwtVerifyEnd = timer_monotonic();
			fmt::print("JWT Verify :         {:.2f} OPS\n", repeat * numSamples / (jwtVerifyEnd - jwtVerifyBegin));
		}
	}
	return Void();
}
