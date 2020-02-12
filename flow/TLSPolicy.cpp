/*
 * TLSPolicy.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2020 Apple Inc. and the FoundationDB project authors
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

#include <algorithm>
#include <cstring>
#include <exception>
#include <map>
#include <openssl/objects.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/x509_vfy.h>
#include <stdint.h>
#include <string>
#include <utility>

#include "flow/FastRef.h"
#include "flow/Trace.h"
#include "flow/TLSPolicy.h"

// To force typeinfo to only be emitted once.
TLSPolicy::~TLSPolicy() {}

static int hexValue(char c) {
	static char const digits[] = "0123456789ABCDEF";

	if (c >= 'a' && c <= 'f')
		c -= ('a' - 'A');

	int value = std::find(digits, digits + 16, c) - digits;
	if (value >= 16) {
		throw std::runtime_error("hexValue");
	}
	return value;
}

// Does not handle "raw" form (e.g. #28C4D1), only escaped text
static std::string de4514(std::string const& input, int start, int& out_end) {
	std::string output;

	if(input[start] == '#' || input[start] == ' ') {
		out_end = start;
		return output;
	}

	int space_count = 0;

	for(int p = start; p < input.size();) {
		switch(input[p]) {
		case '\\': // Handle escaped sequence

			// Backslash escaping nothing!
			if(p == input.size() - 1) {
				out_end = p;
				goto FIN;
			}

			switch(input[p+1]) {
			case ' ':
			case '"':
			case '#':
			case '+':
			case ',':
			case ';':
			case '<':
			case '=':
			case '>':
			case '|':
			case '\\':
				output += input[p+1];
				p += 2;
				space_count = 0;
				continue;

			default:
				// Backslash escaping pair of hex digits requires two characters
				if(p == input.size() - 2) {
					out_end = p;
					goto FIN;
				}

				try {
					output += hexValue(input[p+1]) * 16 + hexValue(input[p+2]);
					p += 3;
					space_count = 0;
					continue;
				} catch( ... ) {
					out_end = p;
					goto FIN;
				}
			}

		case '"':
		case '+':
		case ',':
		case ';':
		case '<':
		case '>':
		case 0:
			// All of these must have been escaped
			out_end = p;
			goto FIN;

		default:
			// Character is what it is
			output += input[p];
			if(input[p] == ' ')
				space_count++;
			else
				space_count = 0;
			p++;
		}
	}

	out_end = input.size();

 FIN:
	out_end -= space_count;
	output.resize(output.size() - space_count);

	return output;
}

static std::pair<std::string, std::string> splitPair(std::string const& input, char c) {
	int p = input.find_first_of(c);
	if(p == input.npos) {
		throw std::runtime_error("splitPair");
	}
	return std::make_pair(input.substr(0, p), input.substr(p+1, input.size()));
}

static NID abbrevToNID(std::string const& sn) {
	NID nid = NID_undef;

	if (sn == "C" || sn == "CN" || sn == "L" || sn == "ST" || sn == "O" || sn == "OU" || sn == "UID" || sn == "DC" || sn == "subjectAltName")
		nid = OBJ_sn2nid(sn.c_str());
	if (nid == NID_undef)
		throw std::runtime_error("abbrevToNID");

	return nid;
}

static X509Location locationForNID(NID nid) {
	const char* name = OBJ_nid2ln(nid);
	if (name == NULL) {
		throw std::runtime_error("locationForNID");
	}
	if (strncmp(name, "X509v3", 6) == 0) {
		return X509Location::EXTENSION;
	} else {
		// It probably isn't true that all other NIDs live in the NAME, but it is for now...
		return X509Location::NAME;
	}
}

bool TLSPolicy::set_verify_peers(std::vector<std::string> verify_peers) {
	for (int i = 0; i < verify_peers.size(); i++) {
		try {
			std::string& verifyString = verify_peers[i];
			int start = 0;
			while(start < verifyString.size()) {
				int split = verifyString.find('|', start);
				if(split == std::string::npos) {
					break;
				}
				if(split == start || verifyString[split-1] != '\\') {
					rules.emplace_back(verifyString.substr(start,split-start));
					start = split+1;
				}
			}
			rules.emplace_back(verifyString.substr(start));
		} catch ( const std::runtime_error& e ) {
			rules.clear();
			std::string& verifyString = verify_peers[i];
			TraceEvent(SevError, "FDBLibTLSVerifyPeersParseError").detail("Config", verifyString);
			return false;
		}
	}
	return true;
}

TLSPolicy::Rule::Rule(std::string input) {
	int s = 0;

	while (s < input.size()) {
		int eq = input.find('=', s);

		if (eq == input.npos)
			throw std::runtime_error("parse_verify");

		MatchType mt = MatchType::EXACT;
		if (input[eq-1] == '>') mt = MatchType::PREFIX;
		if (input[eq-1] == '<') mt = MatchType::SUFFIX;
		std::string term = input.substr(s, eq - s - (mt == MatchType::EXACT ? 0 : 1));

		if (term.find("Check.") == 0) {
			if (eq + 2 > input.size())
				throw std::runtime_error("parse_verify");
			if (eq + 2 != input.size() && input[eq + 2] != ',')
				throw std::runtime_error("parse_verify");
			if (mt != MatchType::EXACT)
				throw std::runtime_error("parse_verify: cannot prefix match Check");

			bool* flag;

			if (term == "Check.Valid")
				flag = &verify_cert;
			else if (term == "Check.Unexpired")
				flag = &verify_time;
			else
				throw std::runtime_error("parse_verify");

			if (input[eq + 1] == '0')
				*flag = false;
			else if (input[eq + 1] == '1')
				*flag = true;
			else
				throw std::runtime_error("parse_verify");

			s = eq + 3;
		} else {
			std::map< int, Criteria >* criteria = &subject_criteria;

			if (term.find('.') != term.npos) {
				auto scoped = splitPair(term, '.');

				if (scoped.first == "S" || scoped.first == "Subject")
					criteria = &subject_criteria;
				else if (scoped.first == "I" || scoped.first == "Issuer")
					criteria = &issuer_criteria;
				else if (scoped.first == "R" || scoped.first == "Root")
					criteria = &root_criteria;
				else
					throw std::runtime_error("parse_verify");

				term = scoped.second;
			}

			int remain;
			auto unesc = de4514(input, eq + 1, remain);

			if (remain == eq + 1)
				throw std::runtime_error("parse_verify");

			NID termNID = abbrevToNID(term);
			const X509Location loc = locationForNID(termNID);
			criteria->insert(std::make_pair(termNID, Criteria(unesc, mt, loc)));

			if (remain != input.size() && input[remain] != ',')
				throw std::runtime_error("parse_verify");

			s = remain + 1;
		}
	}
}

bool match_criteria_entry(const std::string& criteria, ASN1_STRING* entry, MatchType mt) {
	bool rc = false;
	ASN1_STRING* asn_criteria = NULL;
	unsigned char* criteria_utf8 = NULL;
	int criteria_utf8_len = 0;
	unsigned char* entry_utf8 = NULL;
	int entry_utf8_len = 0;

	if ((asn_criteria = ASN1_IA5STRING_new()) == NULL)
		goto err;
	if (ASN1_STRING_set(asn_criteria, criteria.c_str(), criteria.size()) != 1)
		goto err;
	if ((criteria_utf8_len = ASN1_STRING_to_UTF8(&criteria_utf8, asn_criteria)) < 1)
		goto err;
	if ((entry_utf8_len = ASN1_STRING_to_UTF8(&entry_utf8, entry)) < 1)
		goto err;
	if (mt == MatchType::EXACT) {
		if (criteria_utf8_len == entry_utf8_len &&
		    memcmp(criteria_utf8, entry_utf8, criteria_utf8_len) == 0)
			rc = true;
	} else if (mt == MatchType::PREFIX) {
		if (criteria_utf8_len <= entry_utf8_len &&
		    memcmp(criteria_utf8, entry_utf8, criteria_utf8_len) == 0)
			rc = true;
	} else if (mt == MatchType::SUFFIX) {
		if (criteria_utf8_len <= entry_utf8_len &&
		    memcmp(criteria_utf8, entry_utf8 + (entry_utf8_len - criteria_utf8_len), criteria_utf8_len) == 0)
			rc = true;
	}

	err:
	ASN1_STRING_free(asn_criteria);
	free(criteria_utf8);
	free(entry_utf8);
	return rc;
}

bool match_name_criteria(X509_NAME *name, NID nid, const std::string& criteria, MatchType mt) {
	X509_NAME_ENTRY *name_entry;
	int idx;

	// If name does not exist, or has multiple of this RDN, refuse to proceed.
	if ((idx = X509_NAME_get_index_by_NID(name, nid, -1)) < 0)
		return false;
	if (X509_NAME_get_index_by_NID(name, nid, idx) != -1)
		return false;
	if ((name_entry = X509_NAME_get_entry(name, idx)) == NULL)
		return false;

	return match_criteria_entry(criteria, X509_NAME_ENTRY_get_data(name_entry), mt);
}

bool match_extension_criteria(X509 *cert, NID nid, const std::string& value, MatchType mt) {
	if (nid != NID_subject_alt_name && nid != NID_issuer_alt_name) {
		// I have no idea how other extensions work.
		return false;
	}
	auto pos = value.find(':');
	if (pos == value.npos) {
		return false;
	}
	std::string value_gen = value.substr(0, pos);
	std::string value_val = value.substr(pos+1, value.npos);
	STACK_OF(GENERAL_NAME)* sans = reinterpret_cast<STACK_OF(GENERAL_NAME)*>(X509_get_ext_d2i(cert, nid, NULL, NULL));
	if (sans == NULL) {
		return false;
	}
	int num_sans = sk_GENERAL_NAME_num( sans );
	bool rc = false;
	for( int i = 0; i < num_sans && !rc; ++i ) {
		GENERAL_NAME* altname = sk_GENERAL_NAME_value( sans, i );
		std::string matchable;
		switch (altname->type) {
		case GEN_OTHERNAME:
			break;
		case GEN_EMAIL:
			if (value_gen == "EMAIL" &&
			    match_criteria_entry( value_val, altname->d.rfc822Name, mt)) {
				rc = true;
				break;
			}
		case GEN_DNS:
			if (value_gen == "DNS" &&
			    match_criteria_entry( value_val, altname->d.dNSName, mt )) {
				rc = true;
				break;
			}
		case GEN_X400:
		case GEN_DIRNAME:
		case GEN_EDIPARTY:
			break;
		case GEN_URI:
			if (value_gen == "URI" &&
			    match_criteria_entry( value_val, altname->d.uniformResourceIdentifier, mt )) {
				rc = true;
				break;
			}
		case GEN_IPADD:
			if (value_gen == "IP" &&
			    match_criteria_entry( value_val, altname->d.iPAddress, mt )) {
				rc = true;
				break;
			}
		case GEN_RID:
			break;
		}
	}
	sk_GENERAL_NAME_pop_free(sans, GENERAL_NAME_free);
	return rc;
}

bool match_criteria(X509* cert, X509_NAME* subject, NID nid, const std::string& criteria, MatchType mt, X509Location loc) {
	switch(loc) {
	case X509Location::NAME: {
		return match_name_criteria(subject, nid, criteria, mt);
	}
	case X509Location::EXTENSION: {
		return match_extension_criteria(cert, nid, criteria, mt);
	}
	}
	// Should never be reachable.
	return false;
}

std::tuple<bool,std::string> check_verify(const TLSPolicy::Rule* verify, X509_STORE_CTX* store_ctx, bool is_client) {
	X509_NAME *subject, *issuer;
	bool rc = false;
	X509* cert = NULL;
	// if returning false, give a reason string
	std::string reason = "";

	// If certificate verification is disabled, there's nothing more to do.
	if (!verify->verify_cert)
		return std::make_tuple(true, reason);

	//X509_STORE_CTX_trusted_stack(store_ctx, policy->roots);
	X509_STORE_CTX_set_default(store_ctx, is_client ? "ssl_server" : "ssl_client");
	if (!verify->verify_time)
		X509_VERIFY_PARAM_set_flags(X509_STORE_CTX_get0_param(store_ctx), X509_V_FLAG_NO_CHECK_TIME);
	if (X509_verify_cert(store_ctx) <= 0) {
		const char *errstr = X509_verify_cert_error_string(X509_STORE_CTX_get_error(store_ctx));
		reason = "Verify cert error: " + std::string(errstr);
		goto err;
	}

	// Check subject criteria.
	cert = sk_X509_value(X509_STORE_CTX_get0_chain(store_ctx), 0);
	if ((subject = X509_get_subject_name(cert)) == NULL) {
		reason = "Cert subject error";
		goto err;
	}
	for (auto &pair: verify->subject_criteria) {
		if (!match_criteria(cert, subject, pair.first, pair.second.criteria, pair.second.match_type, pair.second.location)) {
			reason = "Cert subject match failure";
			goto err;
		}
	}

	// Check issuer criteria.
	if ((issuer = X509_get_issuer_name(cert)) == NULL) {
		reason = "Cert issuer error";
		goto err;
	}
	for (auto &pair: verify->issuer_criteria) {
		if (!match_criteria(cert, issuer, pair.first, pair.second.criteria, pair.second.match_type, pair.second.location)) {
			reason = "Cert issuer match failure";
			goto err;
		}
	}

	// Check root criteria - this is the subject of the final certificate in the stack.
	cert = sk_X509_value(X509_STORE_CTX_get0_chain(store_ctx), sk_X509_num(X509_STORE_CTX_get0_chain(store_ctx)) - 1);
	if ((subject = X509_get_subject_name(cert)) == NULL) {
		reason = "Root subject error";
		goto err;
	}
	for (auto &pair: verify->root_criteria) {
		if (!match_criteria(cert, subject, pair.first, pair.second.criteria, pair.second.match_type, pair.second.location)) {
			reason = "Root subject match failure";
			goto err;
		}
	}

	// If we got this far, everything checked out...
	rc = true;

 err:
	return std::make_tuple(rc, reason);
}

bool TLSPolicy::verify_peer(X509_STORE_CTX* store_ctx) {
	bool rc = false;
	std::set<std::string> verify_failure_reasons;
	bool verify_success;
	std::string verify_failure_reason;

	// Any matching rule is sufficient.
	for (auto &verify_rule: rules) {
		std::tie(verify_success, verify_failure_reason) = check_verify(&verify_rule, store_ctx, is_client);
		if (verify_success) {
			rc = true;
			break;
		} else {
			if (verify_failure_reason.length() > 0)
				verify_failure_reasons.insert(verify_failure_reason);
		}
	}

	if (!rc) {
		// log the various failure reasons
		for (std::string reason : verify_failure_reasons) {
			TraceEvent("TLSPolicyFailure").suppressFor(1.0).detail("Reason", reason);
		}
	}
	return rc;
}
